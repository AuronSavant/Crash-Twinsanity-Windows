#pragma once

// Two players on one machine (coop.cpp, native/COOP.md): the second character of the pairing (Cortex with Crash, Nina, ...) read
// from pad port 1 (the second gamepad) instead of following the first by itself. Off unless asked for (--coop or $TWIN_COOP=1)
#include "common.h"

struct InstanceContext;

namespace NativeCoop
{
// Whether co-op is on: --coop, else $TWIN_COOP (1 on, 0 off)
bool Enabled();
void SetEnabled(bool on);
// --coop-keyboard: player 1 on the keyboard alone, player 2 on the first gamepad (else player 1 has the keyboard and the first
// gamepad, player 2 the second gamepad)
bool KeyboardFirst();
void SetKeyboardFirst(bool on);

// Pad port 1 (pads.cpp): whether a second player is there (a gamepad of theirs, or $TWIN_COOP_P2_INPUT's scripted input), and its
// report as Platform::Pads::Read's
bool SecondPlayerPresent();
s32 ReadSecondPad(u8* data, bool pressureMode);
void RumbleSecondPad(u8 small, u8 large);

// A hit that would kill an instance (CharacterAgent::Contact): when it's player 2's twin, it comes back at the latest checkpoint
// instead (a playable character's death would restart the level) and the hit is dropped (true)
bool SpareTwin(InstanceContext* instance);
// Whether two instances are player 1's character and player 2's twin (either way round): their hits on each other are dropped
bool Teammates(InstanceContext* one, InstanceContext* other);

// Whether an instance is player 2's twin
bool IsTwin(const InstanceContext* instance);

// While a script runs for something player 2's twin did (its runner's originator, BehaviourRunner::Update), the twin is "the
// player" (g_PlayerInstance and the player's character and part): a creature bounced on launches whoever bounced on it
class TwinAsPlayer
{
public:
    explicit TwinAsPlayer(const void* originator);
    ~TwinAsPlayer();
    TwinAsPlayer(const TwinAsPlayer&) = delete;
    TwinAsPlayer& operator=(const TwinAsPlayer&) = delete;

private:
    bool swapped_ = false;
    void* saved_[5] = {};
};

// Once a frame (GameController::Update): the second character given pad port 1 while the first is played with a pad, given back
// to the game when the first isn't (cutscenes, the game's own control taken away), on a vehicle or carried
void Frame();
}
