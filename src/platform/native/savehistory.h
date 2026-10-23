#pragma once

// The saves' history (savehistory.cpp, native/COOP.md "Testing"): every save the game writes kept, the whole memory card copied
// into the settings folder's "save history" as "NNN date time area" (in the order they were made), for Play\Load Autosave.bat to
// put back. A debugging aid: start from any part of the game reached before
namespace NativeSaveHistory
{
// A memory card file written (memorycard.cpp): the card copied once the game has stopped writing for a moment
void Written();
// Once a frame (GameController::Update): the copy made when it's due
void Frame();
}
