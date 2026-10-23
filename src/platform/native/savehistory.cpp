// The saves' history (savehistory.h): each save the game writes (an autosave at a checkpoint, a save from the pause menu) copied,
// with the whole memory card, into Native::SettingsFolder()/"save history"/"NNN YYYY-MM-DD HH-MM-SS area", NNN counting up so
// the folders list in the order they were made. Play\Load Autosave.bat lists them and puts one back as the memory card
#include "savehistory.h"

#include "native.h"

#include "game/chunkdata.h"
#include "game/instances.h"
#include "game/player.h"
#include "game/reference.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
using Clock = std::chrono::steady_clock;

// The card's copied this long after its last write (a save writes its files one after the other, the last a few seconds after
// the first: two seconds caught one half written)
constexpr auto SettleTime = std::chrono::seconds(6);

bool g_Due = false;
Clock::time_point g_LastWrite;

// The area the player is in, for the folder's name: the chunk's path past "levels\", its separators made dashes ("earth-hub-beach")
std::string AreaName()
{
    if (g_PlayerInstance == nullptr || g_PlayerInstance->object == nullptr)
    {
        return "unknown";
    }

    auto* player = static_cast<InstanceContext*>(g_PlayerInstance->object);
    ChunkData* chunk = InstanceLive(player) ? player->chunk : nullptr;
    const char* name = chunk != nullptr ? chunk->name.string : nullptr;
    if (name == nullptr)
    {
        return "unknown";
    }

    std::string area = name;
    constexpr const char Levels[] = "levels\\";
    if (area.compare(0, sizeof(Levels) - 1, Levels) == 0)
    {
        area = area.substr(sizeof(Levels) - 1);
    }

    for (char& c : area)
    {
        if (c == '\\' || c == '/' || c == ':' || c == ' ')
        {
            c = '-';
        }
    }

    return area;
}

// The next number: one past the highest of the folders there
int NextNumber(const fs::path& history)
{
    int highest = 0;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(history, error))
    {
        int number = 0;
        if (entry.is_directory(error) && std::sscanf(entry.path().filename().string().c_str(), "%d", &number) == 1 &&
            number > highest)
        {
            highest = number;
        }
    }

    return highest + 1;
}

void CopyCard()
{
    fs::path settings = Native::SettingsFolder();
    fs::path card = settings / "memorycard";
    fs::path history = settings / "save history";
    std::error_code error;
    // A file still empty: the save isn't finished, kept at the next write instead
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(card, error))
    {
        if (entry.is_regular_file(error) && entry.file_size(error) == 0)
        {
            Native::Log("save history: %s is still empty, the save isn't kept yet", entry.path().filename().string().c_str());
            return;
        }
    }

    fs::create_directories(history, error);
    std::time_t now = std::time(nullptr);
    std::tm local{};
    Native::LocalTime(now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
    char number[16];
    std::snprintf(number, sizeof(number), "%03d", NextNumber(history));
    fs::path target = history / (std::string(number) + " " + stamp + " " + AreaName());
    fs::copy(card, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    if (error)
    {
        Native::Log("save history: the memory card wasn't copied: %s", error.message().c_str());
        return;
    }

    Native::Log("save history: kept as \"%s\"", target.filename().string().c_str());
}
}

void NativeSaveHistory::Written()
{
    g_Due = true;
    g_LastWrite = Clock::now();
}

void NativeSaveHistory::Frame()
{
    if (g_Due && Clock::now() - g_LastWrite >= SettleTime)
    {
        g_Due = false;
        CopyCard();
    }
}
