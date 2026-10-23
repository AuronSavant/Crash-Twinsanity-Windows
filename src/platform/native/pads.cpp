// The controllers: the machine's gamepad (SDL3) and keyboard, presented to the game as a DualShock 2 on port 0 the way the PS2's
// libpad does (analog mode, two actuators, pressure sensitive mode), reporting what the PS2's report has: the button word
// (pressed 0), the sticks (0x80 the middle, up 0) and the buttons' pressures (0-255). See native/NATIVE.md for the mapping
#include "native.h"
#include "coop.h"
#include "fixes.h"
#include "platform/pads.h"
#include "ui/bindings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
using namespace Platform::Pads;

// The report's button word, pressed 1 before it's inverted (the PS2's bits: high byte left, down, right, up, start, R3, L3,
// select; low byte square, cross, circle, triangle, R1, L1, R2, L2)
enum Button : u32
{
    ButtonL2 = 0x1,
    ButtonR2 = 0x2,
    ButtonL1 = 0x4,
    ButtonR1 = 0x8,
    ButtonTriangle = 0x10,
    ButtonCircle = 0x20,
    ButtonCross = 0x40,
    ButtonSquare = 0x80,
    ButtonSelect = 0x100,
    ButtonL3 = 0x200,
    ButtonR3 = 0x400,
    ButtonStart = 0x800,
    ButtonUp = 0x1000,
    ButtonRight = 0x2000,
    ButtonDown = 0x4000,
    ButtonLeft = 0x8000,
};

// A DualShock 2 in pressure sensitive mode: type 7 (analog), 9 halfwords of data after the header
constexpr u8 ModeAnalog = 0x73;
constexpr u8 ModePressure = 0x79;
constexpr s32 PressureReportBytes = 20;
constexpr s32 AnalogReportBytes = 8;
constexpr u8 StickMiddle = 0x80;
constexpr u8 Pressed = 0xFF;

bool g_Initialised = false;
bool g_Open = false;
bool g_PressureMode = false;
SDL_Gamepad* g_Gamepad = nullptr;

// What each button's pressure byte is (the PS2's ReportByte order)
struct PressureOf
{
    Button button;
    ReportByte byte;
};
constexpr PressureOf Pressures[] = {
    {ButtonRight, PressureRight}, {ButtonLeft, PressureLeft},     {ButtonUp, PressureUp},       {ButtonDown, PressureDown},
    {ButtonTriangle, PressureTriangle}, {ButtonCircle, PressureCircle}, {ButtonCross, PressureCross},
    {ButtonSquare, PressureSquare}, {ButtonL1, PressureL1},       {ButtonR1, PressureR1},       {ButtonL2, PressureL2},
    {ButtonR2, PressureR2},
};

void StartSdl()
{
    if (!g_Initialised)
    {
        g_Initialised = true;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
        {
            Native::Log("pads: SDL's gamepads didn't start: %s", SDL_GetError());
        }
    }
}

// Co-op's pad port 1 (coop.h): opened, its gamepad, and what it read last
bool g_SecondOpen = false;
SDL_Gamepad* g_SecondGamepad = nullptr;
NativeUi::BoundPad g_LastSecondPlayer = {0, 0, 0, NativeUi::BoundPad::StickMiddle, NativeUi::BoundPad::StickMiddle,
                                         NativeUi::BoundPad::StickMiddle, NativeUi::BoundPad::StickMiddle};

// A stick's byte with another's push added (both from the middle), kept within the stick's range
u8 AddStick(u8 own, u8 other)
{
    s32 sum = static_cast<s32>(own) + static_cast<s32>(other) - NativeUi::BoundPad::StickMiddle;
    return static_cast<u8>(std::clamp(sum, 0, 255));
}

// Player 2's last read put together with player 1's: the sticks' pushes added, either's buttons, the harder trigger
void MergeSecondPlayer(NativeUi::BoundPad* pad)
{
    const NativeUi::BoundPad& other = g_LastSecondPlayer;
    pad->buttons |= other.buttons;
    pad->l2Pressure = std::max(pad->l2Pressure, other.l2Pressure);
    pad->r2Pressure = std::max(pad->r2Pressure, other.r2Pressure);
    pad->leftX = AddStick(pad->leftX, other.leftX);
    pad->leftY = AddStick(pad->leftY, other.leftY);
    pad->rightX = AddStick(pad->rightX, other.rightX);
    pad->rightY = AddStick(pad->rightY, other.rightY);
}

// A gamepad kept until it's unplugged, else the first plugged in that the other player hasn't got
void FindGamepad(SDL_Gamepad** gamepad, SDL_Gamepad* other, const char* who)
{
    if (*gamepad != nullptr && SDL_GamepadConnected(*gamepad))
    {
        return;
    }

    if (*gamepad != nullptr)
    {
        SDL_CloseGamepad(*gamepad);
        *gamepad = nullptr;
    }

    SDL_JoystickID taken = other != nullptr ? SDL_GetGamepadID(other) : 0;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; i++)
    {
        if (ids[i] == taken)
        {
            continue;
        }

        *gamepad = SDL_OpenGamepad(ids[i]);
        if (*gamepad != nullptr)
        {
            Native::Log("pads: %s%s", who, SDL_GetGamepadName(*gamepad));
        }

        break;
    }

    SDL_free(ids);
}

// Player 1's gamepad: the first (the second's player 2's, unless player 2 has the keyboard)
void FindGamepad()
{
    FindGamepad(&g_Gamepad, g_SecondGamepad, NativeCoop::Enabled() ? "player 1: " : "");
}

// Whether player 2 plays on the keyboard (co-op's --coop-keyboard): then player 1 has the gamepad alone
bool KeyboardForPlayerTwo()
{
    return NativeCoop::Enabled() && NativeCoop::KeyboardSecond();
}

// This frame's keys (pressed now or since the last read), read with player 1's pad: player 2's keyboard reads them after it
bool g_FrameKeys[SDL_SCANCODE_COUNT];
}

s32 Platform::Pads::Initialise()
{
    StartSdl();
    return 1;
}

s32 Platform::Pads::Open(s32 port, s32 slot, void*)
{
    StartSdl();
    if (port == 1 && slot == 0 && NativeCoop::Enabled())
    {
        g_SecondOpen = true;
        return 1;
    }

    if (port != 0 || slot != 0)
    {
        return 0;
    }

    g_Open = true;
    return 1;
}

s32 Platform::Pads::Close(s32 port, s32 slot)
{
    if (port == 0 && slot == 0)
    {
        g_Open = false;
        g_PressureMode = false;
    }
    else if (port == 1 && slot == 0)
    {
        g_SecondOpen = false;
    }

    return 1;
}

// Co-op's port 1 is always there (a DualShock 2 at rest while player 2 has no gamepad), so the game never stops for it
Platform::Pads::State Platform::Pads::GetState(s32 port, s32 slot)
{
    if (port == 1 && slot == 0)
    {
        return g_SecondOpen ? StateStable : StateDisconnected;
    }

    return port == 0 && slot == 0 && g_Open ? StateStable : StateDisconnected;
}

Platform::Pads::RequestState Platform::Pads::GetRequestState(s32, s32)
{
    return RequestComplete;
}

// A DualShock 2's modes: digital (4) and analog (7), in analog mode
s32 Platform::Pads::InfoMode(s32 port, s32 slot, ModeInfo info, s32 index)
{
    if (GetState(port, slot) != StateStable)
    {
        return 0;
    }

    switch (info)
    {
    case InfoCurrentId:
    case InfoCurrentExtendedId:
        return ModeIdAnalog;
    case InfoCurrentIndex:
        return 1;
    case InfoModeTable:
        return index == ModeTableLength ? 2 : index == 0 ? ModeIdDigital : index == 1 ? ModeIdAnalog : 0;
    }

    return 0;
}

s32 Platform::Pads::SetMainMode(s32, s32, MainMode, ModeLock)
{
    return 1;
}

// Two actuators, as a DualShock 2 has: the small motor (on or off) and the large one (0-255)
s32 Platform::Pads::InfoActuator(s32 port, s32 slot, s32 actuator, s32)
{
    if (GetState(port, slot) != StateStable)
    {
        return 0;
    }

    return actuator == CountActuators ? 2 : 1;
}

s32 Platform::Pads::SetActuatorAlign(s32, s32, const u8*)
{
    return 1;
}

s32 Platform::Pads::SetActuatorDirect(s32 port, s32 slot, const u8 values[6])
{
    if (port == 1 && slot == 0 && g_SecondOpen)
    {
        NativeCoop::RumbleSecondPad(values[0], values[1]);
        return 1;
    }

    if (port != 0 || slot != 0)
    {
        return 0;
    }

    if (g_Gamepad != nullptr)
    {
        // The small motor on or off, the large one by its strength; the game sets them again every frame it wants them
        u16 small = values[0] != 0 ? 0xFFFF : 0;
        u16 large = static_cast<u16>(values[1] * 0x101);
        SDL_RumbleGamepad(g_Gamepad, large, small, 100);
    }

    return 1;
}

s32 Platform::Pads::InfoPressureMode(s32 port, s32 slot)
{
    return GetState(port, slot) == StateStable ? 1 : 0;
}

s32 Platform::Pads::EnterPressureMode(s32 port, s32 slot)
{
    if (GetState(port, slot) != StateStable)
    {
        return 0;
    }

    g_PressureMode = true;
    return 1;
}

namespace
{
// A report (Read's) of what's pressed: the digital buttons' pressures full (the triggers have their own when a gamepad gives them)
s32 WriteReport(const NativeUi::BoundPad& bound, bool pressureMode, u8* data)
{
    u32 buttons = bound.buttons;
    u8 pressure[PressureReportBytes] = {};
    pressure[PressureL2] = bound.l2Pressure;
    pressure[PressureR2] = bound.r2Pressure;
    for (const PressureOf& of : Pressures)
    {
        if ((buttons & of.button) != 0 && pressure[of.byte] == 0)
        {
            pressure[of.byte] = Pressed;
        }
    }

    std::memset(data, 0, PressureReportBytes);
    data[0] = 0;
    data[ReportMode] = pressureMode ? ModePressure : ModeAnalog;
    data[ReportButtonsHigh] = static_cast<u8>(~buttons >> 8);
    data[ReportButtonsLow] = static_cast<u8>(~buttons);
    data[StickRightX] = bound.rightX;
    data[StickRightY] = bound.rightY;
    data[StickLeftX] = bound.leftX;
    data[StickLeftY] = bound.leftY;
    if (pressureMode)
    {
        for (u32 byte = PressureRight; byte <= PressureR2; byte++)
        {
            data[byte] = pressure[byte];
        }
    }

    return pressureMode ? PressureReportBytes : AnalogReportBytes;
}

// Keys pressed since the last read: a press shorter than a frame (the native frames can be slow) still counts for one
bool g_Latched[SDL_SCANCODE_COUNT];
bool g_Watching = false;
bool g_QuitAsked = false;

bool WatchPadEvents(void*, SDL_Event* event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode > SDL_SCANCODE_UNKNOWN &&
        event->key.scancode < SDL_SCANCODE_COUNT)
    {
        g_Latched[event->key.scancode] = true;
    }
    else if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
    {
        // The window's close button, the Dock's or the menu's quit, Cmd+Q
        g_QuitAsked = true;
    }

    return true;
}
}

s32 Platform::Pads::Read(s32 port, s32 slot, u8* data)
{
    if (!g_Watching)
    {
        g_Watching = true;
        SDL_AddEventWatch(WatchPadEvents, nullptr);
    }

    if (port == 1 && slot == 0 && g_SecondOpen)
    {
        return NativeCoop::ReadSecondPad(data, g_PressureMode);
    }

    if (port != 0 || slot != 0 || !g_Open)
    {
        return 0;
    }

    SDL_PumpEvents();
    if (g_QuitAsked)
    {
        // Quitting: the saves and settings are written as they're made, so nothing is left to keep
        // Straight out: SDL's shutdown under the sound thread still feeding its stream crashed on the way out
        Native::Log("quit");
        std::_Exit(0);
    }

    Native::SnapshotFrame();
    Native::QuickStartFrame();
    NativeFixes::Frame();
    // The level sweep's frame here too: GameController::Update (its other caller) doesn't run while some cutscenes play, the pads'
    // reads do (their skip polls them)
    NativeUi::SweepFrame(true);
    FindGamepad();
    // The keyboard's keys and the gamepad's buttons by the controls' bindings (src/platform/native/ui/bindings.cpp: the defaults
    // are NATIVE.md's), the sticks the gamepad's (the keys bound to the left stick's directions over it)
    NativeUi::BoundPad bound;
    static bool keys[SDL_SCANCODE_COUNT];
    const bool* state = SDL_GetKeyboardState(nullptr);
    for (int key = 0; key < SDL_SCANCODE_COUNT; key++)
    {
        keys[key] = state[key] || g_Latched[key];
        g_Latched[key] = false;
    }

    // Co-op: F6 brings player 2 back beside player 1 (Select on player 2's pad does too)
    static bool regroupHeld = false;
    if (keys[SDL_SCANCODE_F6] && !regroupHeld)
    {
        NativeCoop::RequestRegroup();
    }

    regroupHeld = keys[SDL_SCANCODE_F6];
    // Player 2 on the keyboard: player 1 reads the gamepad alone, the keys kept for player 2's read
    std::copy(keys, keys + SDL_SCANCODE_COUNT, g_FrameKeys);
    if (KeyboardForPlayerTwo())
    {
        // No keys: the keyboard and the mouse's buttons are player 2's (the mouse's left button spun both Crashes)
        NativeUi::ReadBindings(nullptr, g_Gamepad, &bound);
    }
    else
    {
        NativeUi::ReadBindings(keys, g_Gamepad, &bound);
    }
    // Co-op: player 2's last read added in while both steer one thing (the Rollerbrawl: coop.cpp)
    if (NativeCoop::SharesPad())
    {
        MergeSecondPlayer(&bound);
    }

    return WriteReport(bound, g_PressureMode, data);
}

namespace
{
// Co-op's scripted player 2 ($TWIN_COOP_P2_INPUT, for tests): "frames:leftX:leftY:buttons,..." (the sticks' bytes 0-255, 128 the
// middle; the buttons the report's bits, hex, Button's), played in a loop
struct ScriptStep
{
    u32 frames;
    u8 leftX;
    u8 leftY;
    u32 buttons;
};

std::vector<ScriptStep> ReadScript()
{
    std::vector<ScriptStep> steps;
    const char* text = std::getenv("TWIN_COOP_P2_INPUT");
    while (text != nullptr && *text != '\0')
    {
        unsigned frames = 0, leftX = StickMiddle, leftY = StickMiddle, buttons = 0;
        if (std::sscanf(text, "%u:%u:%u:%x", &frames, &leftX, &leftY, &buttons) >= 1 && frames > 0)
        {
            steps.push_back({frames, static_cast<u8>(std::min(leftX, 255u)), static_cast<u8>(std::min(leftY, 255u)), buttons});
        }

        text = std::strchr(text, ',');
        text = text != nullptr ? text + 1 : nullptr;
    }

    return steps;
}

const std::vector<ScriptStep>& Script()
{
    static const std::vector<ScriptStep> steps = ReadScript();
    return steps;
}

// The scripted step of this read
const ScriptStep& ScriptNow()
{
    static u32 frame = 0;
    static size_t step = 0;
    const std::vector<ScriptStep>& steps = Script();
    if (frame >= steps[step].frames)
    {
        frame = 0;
        step = (step + 1) % steps.size();
    }

    frame++;
    return steps[step];
}

u8 StickByte(Sint16 value)
{
    return static_cast<u8>((static_cast<s32>(value) + 32768) >> 8);
}

u8 TriggerByte(Sint16 value)
{
    return static_cast<u8>(std::clamp(static_cast<s32>(value), 0, 32767) * 255 / 32767);
}

// Player 2's gamepad as a DualShock 2: the face buttons by place (south cross, east circle, west square, north triangle), the
// shoulders, triggers, sticks and their clicks, back select
void ReadSecondGamepad(SDL_Gamepad* gamepad, NativeUi::BoundPad* pad)
{
    struct ButtonOf
    {
        SDL_GamepadButton from;
        Button to;
    };
    static constexpr ButtonOf Buttons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, ButtonCross},        {SDL_GAMEPAD_BUTTON_EAST, ButtonCircle},
        {SDL_GAMEPAD_BUTTON_WEST, ButtonSquare},        {SDL_GAMEPAD_BUTTON_NORTH, ButtonTriangle},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ButtonL1},   {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ButtonR1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, ButtonL3},      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, ButtonR3},
        {SDL_GAMEPAD_BUTTON_START, ButtonStart},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, ButtonUp},         {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ButtonDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ButtonLeft},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ButtonRight},
    };
    constexpr u8 TriggerDown = 0x40;
    for (const ButtonOf& of : Buttons)
    {
        if (SDL_GetGamepadButton(gamepad, of.from))
        {
            pad->buttons |= of.to;
        }
    }

    pad->l2Pressure = TriggerByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    pad->r2Pressure = TriggerByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    pad->buttons |= (pad->l2Pressure >= TriggerDown ? ButtonL2 : 0) | (pad->r2Pressure >= TriggerDown ? ButtonR2 : 0);
    pad->leftX = StickByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
    pad->leftY = StickByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    pad->rightX = StickByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
    pad->rightY = StickByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    NativeUi::Squared(&pad->leftX, &pad->leftY);
    NativeUi::Squared(&pad->rightX, &pad->rightY);

    // Back (Select) is player 2's regroup: brought back beside player 1 when it's pressed
    static bool backHeld = false;
    bool back = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK);
    if (back && !backHeld)
    {
        NativeCoop::RequestRegroup();
    }

    backHeld = back;
}
}

bool NativeCoop::SecondPlayerPresent()
{
    return g_SecondOpen && (!Script().empty() || g_SecondGamepad != nullptr || KeyboardForPlayerTwo());
}

// Read after port 0 (GamePadController::Update's order), whose read pumped SDL's events
s32 NativeCoop::ReadSecondPad(u8* data, bool pressureMode)
{
    NativeUi::BoundPad bound = {};
    bound.leftX = bound.leftY = bound.rightX = bound.rightY = NativeUi::BoundPad::StickMiddle;
    if (!Script().empty())
    {
        const ScriptStep& step = ScriptNow();
        bound.leftX = step.leftX;
        bound.leftY = step.leftY;
        bound.buttons = step.buttons & ~ButtonSelect;
        // Select is the regroup, as Back on a gamepad
        static bool selectHeld = false;
        bool select = (step.buttons & ButtonSelect) != 0;
        if (select && !selectHeld)
        {
            NativeCoop::RequestRegroup();
        }

        selectHeld = select;
    }
    else if (KeyboardForPlayerTwo())
    {
        // The keyboard by the controls' keyboard bindings (the keys player 1's read took this frame), no gamepad
        NativeUi::ReadBindings(g_FrameKeys, nullptr, &bound);
        // Its Select is the regroup, as Back on a gamepad
        static bool keySelectHeld = false;
        bool select = (bound.buttons & ButtonSelect) != 0;
        if (select && !keySelectHeld)
        {
            NativeCoop::RequestRegroup();
        }

        keySelectHeld = select;
        bound.buttons &= ~ButtonSelect;
    }
    else
    {
        FindGamepad(&g_SecondGamepad, g_Gamepad, "player 2: ");
        if (g_SecondGamepad != nullptr)
        {
            ReadSecondGamepad(g_SecondGamepad, &bound);
        }
    }

    g_LastSecondPlayer = bound;
    return WriteReport(bound, pressureMode, data);
}

void NativeCoop::RumbleSecondPad(u8 small, u8 large)
{
    if (g_SecondGamepad != nullptr)
    {
        SDL_RumbleGamepad(g_SecondGamepad, static_cast<u16>(large * 0x101), small != 0 ? 0xFFFF : 0, 100);
    }
}
