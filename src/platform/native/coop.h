#pragma once

// Two players on one machine (coop.cpp, native/COOP.md): the second character of the pairing (Cortex with Crash, Nina, ...) read
// from pad port 1 (the second gamepad) instead of following the first by itself. Off unless asked for (--coop or $TWIN_COOP=1)
#include "common.h"

class Agent;
struct BehaviourRunner;
struct ChunkLights;
struct InstanceContext;

namespace NativeCoop
{
// Whether co-op is on: --coop, else $TWIN_COOP (1 on, 0 off)
bool Enabled();
void SetEnabled(bool on);
// --coop-keyboard: player 1 on the first gamepad, player 2 on the keyboard (by the controls' keyboard bindings). Else player 1 has
// the keyboard and the first gamepad, player 2 the second gamepad
bool KeyboardSecond();
void SetKeyboardSecond(bool on);

// Whether player 2's pad is added into player 1's (both steering the Rollerbrawl), set once a frame by Frame
bool SharesPad();

// Pad port 1 (pads.cpp): whether a second player is there (a gamepad of theirs, or $TWIN_COOP_P2_INPUT's scripted input), and its
// report as Platform::Pads::Read's
bool SecondPlayerPresent();
s32 ReadSecondPad(u8* data, bool pressureMode);
void RumbleSecondPad(u8 small, u8 large);

// A hit that would kill an instance (CharacterAgent::Contact): when it's player 2's twin, it's put back beside player 1 instead
// (a playable character's death would restart the level) and the hit is dropped (true)
bool SpareTwin(InstanceContext* instance);
// Whether two instances are player 1's character and player 2's twin (either way round): their hits on each other are dropped
bool Teammates(InstanceContext* one, InstanceContext* other);

// Whether an instance is player 2's twin
bool IsTwin(const InstanceContext* instance);
// The lights gathered for the twin's model (ModelNode::Draw) tinted blue, so the two Crashes can be told apart
void TintTwinLights(ChunkLights* lights);

// While a script runs for something player 2's twin did (its runner's originator, BehaviourRunner::Update), or an enemy's (a
// creature's) script runs and the twin is nearer it than player 1, the twin is "the player" (g_PlayerInstance and the player's
// character and part): a creature bounced on launches whoever bounced on it, enemies go for the nearer of the two
class TwinAsPlayer
{
public:
    TwinAsPlayer(const void* originator, InstanceContext* owner);
    ~TwinAsPlayer();
    TwinAsPlayer(const TwinAsPlayer&) = delete;
    TwinAsPlayer& operator=(const TwinAsPlayer&) = delete;

private:
    bool swapped_ = false;
    void* saved_[6] = {};
};
// The twin while a TwinAsPlayer is on (else none): the commands that take the played character for the player take it instead
InstanceContext* ScriptPlayer();

// Player 2 asked to be brought back beside player 1 (Select on their pad, or F6)
void RequestRegroup();

// The throw trace (a look at what Cortex's scripts do once Crash throws him, to give player 2 those moves): an event run on an
// agent (RunAgentEvent), and a body's commands about to run (RunBody). Logged for 15 s from Cortex's thrown events, for Cortex's
// own scripts and the ones he set off
void TraceEvent(Agent* agent, u32 event, const void* originator);
void TraceCommand(BehaviourRunner* runner, u32 commandId);

// Once a frame (GameController::Update): the second character given pad port 1 while the first is played with a pad, given back
// to the game when the first isn't (cutscenes, the game's own control taken away), on a vehicle or carried
void Frame();
}
