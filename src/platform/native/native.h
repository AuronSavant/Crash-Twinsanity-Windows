#pragma once

// What the native platform layer's parts share: its settings, its log and the game's disc image
#include "common.h"

#include <cstdio>
#include <ctime>
#include <string>

namespace Native
{
// The settings: from the command line (--iso PATH), else $TWINSANITY_ISO, else local.json's "disc_image" in the working folder
struct Settings
{
    const char* discImage;
    // Print each platform call the native build doesn't do yet (stubs), once
    bool logStubs;
    // No window (--headless): the frames are drawn and kept to be read back, nothing shows (tests)
    bool headless;
};
const Settings& GetSettings();
// Takes the native build's own arguments out of argv (what's left goes to the game's Main)
void ParseArguments(int& argc, char** argv);
// Where the app keeps its settings and saves, and the disc image asked for with the system's file dialog (remembered there)
std::string SettingsFolder();
std::string PickDiscImage();
// --snapshots DIR: the shown picture saved as a PNG every 2 seconds (snapshots.cpp), checked once a frame
void SetSnapshotFolder(const char* folder);
void SnapshotFrame();
// --quick: straight to playing the beach (quickstart.cpp), checked once a frame
void SetQuickStart(bool quick);
void QuickStartFrame();

// The local time of a moment (localtime_r, localtime_s on Windows)
void LocalTime(std::time_t seconds, std::tm* local);
// An environment variable set (value) or removed (nullptr): setenv and unsetenv, _putenv_s on Windows
void SetEnvironment(const char* name, const char* value);

// The log (stderr): the platform's messages, prefixed
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void Fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));

// The movies' decoder (movie/movie.cpp): FFmpeg's version and its libraries' licences, logged once at start-up
void LogMovieLibraries();

// The disc: an ISO 9660 image read in place (the PS2's paths: "CRASH6\CRASH.BD", any case, / or \, with or without ";1")
namespace Disc
{
struct Entry
{
    u64 offset;
    u32 size;
};
// Opens the image (once); false when it can't be read or isn't one
bool Open(const char* path);
// A file's place in the image, false when there's none
bool Find(const char* path, Entry* entry);
// Reads from the image: how many bytes were read
u32 Read(u64 offset, void* destination, u32 size);
// Mods (disc.cpp): a file of the archive (CRASH6\CRASH.BD's: "Levels\Earth\Hub\beach.rm2") in the settings folder's mods
// folder at its path: its start (from the archive's) and size, past the image's end; whether an image offset is a mod's (read
// whatever the file it was opened as says its size is)
bool FindArchiveMod(const char* path, u32* start, u32* size);
bool IsMod(u64 offset);
}

// The sound processor's memory (the SPU2's 2 MB), where sound banks are streamed to: the audio side plays from it
constexpr u32 SpuMemorySize = 0x200000;
extern u8 g_SpuMemory[SpuMemorySize];
}
