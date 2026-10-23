# Two-player co-op

Crash Twinsanity plays two characters at once in most of its levels: the one you play (Crash) and a second one the game moves by
itself (Cortex, later Nina). Co-op gives the second character to a second player: player 2's gamepad drives Cortex with the same
controls the game uses when you play him yourself.

## Playing

    CrashTwinsanity.exe --coop                # player 1: keyboard and the first gamepad, player 2: the second gamepad
    CrashTwinsanity.exe --coop-keyboard       # player 1: the keyboard only, player 2: the first gamepad

(`$TWIN_COOP=1` and `$TWIN_COOP_KEYBOARD=1` do the same.) Player 2's buttons are the DualShock 2's by place: bottom face button
cross (jump), right circle, left square, top triangle, the shoulders L1/R1, the triggers L2/R2.

Online with a friend: run it with `--coop` on the host's PC and use **Steam Remote Play Together** (add `CrashTwinsanity.exe` as a
non-Steam game) or Parsec. The friend's controller is the host's second gamepad. The friend doesn't need the game.

## How it works (src/platform/native/coop.cpp)

- Pad port 1 exists (src/platform/native/pads.cpp): the second gamepad, read as a DualShock 2. The game made only pad 0 (main.cpp's
  `PadCount`); with co-op on it makes both, and the game controller's `secondPad` (left in the retail code, read by the scripts'
  `PadOf(2)` condition) is pad 1.
- Each frame, `NativeCoop::Frame` gives the second character's controls node (`ControlsNode::pad`) pad 1 while player 1 is in
  control, the same way `GameController::SwitchCharacter` gives the played character pad 0. Without a pad the character follows
  player 1 by the game's own scripts.
- The pad is taken back (the game's scripts move the character again) whenever: player 1's control is taken away (cutscenes,
  scripted moves), the game drives the second character (`motionDriven`), either is on a vehicle or a passenger, or the second
  character changes (a level, a switch). No player 2 gamepad: the game plays as it always did.
- The camera follows player 1 as before.

## Player 2 from the start: the twin

Where the level has no second character (the beach, Jungle Bungle, ...), player 2 gets a twin of the character player 1 plays
(Cortex when his object is loaded and Crash is played): made beside player 1 once player 1 has played a moment in a chunk, from
player 1's instance's properties (`CreateInstanceFrom`; the progress's handle of the character emptied while it's made, as the
game makes a playable character once). `$TWIN_COOP_SPAWN=0` turns it off.

- **Pickups:** the twin draws in and collects wumpa fruit as the player does (characterframe.cpp's `TouchHits`).
- **Scripts:** while a script runs for something the twin set off (its runner's originator, behaviours.cpp's
  `BehaviourRunner::Update`), the twin is "the player": a creature bounced on launches whoever bounced on it. Such a script
  doesn't take player 1's control away (commandsgame.cpp's `DisablePlayerControlCommand`).
- **Damage:** the twin is hurt as player 1 is. A hit that would kill it puts it back beside player 1 instead (a playable
  character's death restarts the level); player 1 plays on. Its scripts can't take player 1's control away.
- **Regroup:** Select (Back) on player 2's pad, or F6, brings player 2 beside player 1 (into player 1's chunk too). After every
  restart player 2 is brought back the same way, or made again.
- **Enemies:** while a creature's script runs and the twin is nearer it than player 1, the twin is its player: enemies go for
  the nearer of the two.
- **Teammates:** the two don't hurt each other (characterstate.cpp's `CharacterAgent::Contact`).
- **Restarts and new chunks:** the twin is made again beside player 1.

## Testing aids

- **Save history** (`src/platform/native/savehistory.cpp`): every save the game writes (autosaves at checkpoints included) is
  kept, the whole memory card copied into `%APPDATA%\Crash Twinsanity\save history\NNN date time area`. `Play\Load
  Autosave.bat` lists them oldest first, backs up the current card into `save history\_backups` and puts the chosen one back
  (then LOAD GAME on the main menu).

## Testing

`python native/tools/coop_test.py` starts a cavern tunnel (Cortex following Crash) headless twice (player 2 scripted by `$TWIN_COOP_P2_INPUT`, then
no player 2) and checks player 2 took Cortex, moved him where the game's own Cortex didn't go, and nothing crashed.
`$TWIN_COOP_LOG=1` logs the characters, their places and who controls the second every 2 s.

`$TWIN_COOP_P2_INPUT`: `frames:leftX:leftY:buttons,...` played in a loop (sticks 0-255, 128 the middle; buttons hex, the
report's bits: 0x40 cross, 0x20 circle, 0x80 square, 0x10 triangle).

## Known limits (prototype)

- One screen and one camera, on player 1. Player 2 can walk off screen.
- Sections where the game ties the two together, carries one or puts them on a vehicle stay the game's.
- Tested by bots (headless, scripted player 2) on the beach and 96 level chunks, and by hand on the beach by the owner (pickups,
  bouncing). Story scripts and cutscenes were written for one Crash: expect odd moments elsewhere.
