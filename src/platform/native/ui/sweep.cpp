// The level sweep (testing, native/LEVEL_SWEEP.md): $TWIN_SWEEP_LEVEL=Levels\Earth\Hub\Beach starts the game in that chunk the way
// the game's own command-line start does (the travellers' tales logo's branch: the progress's start chunk, a saved game's entry,
// the save controller taking the progress, the level loaded), then plays it with scripted input for $TWIN_SWEEP_SECONDS (30) of
// play and exits 0. The states it went through are logged ("sweep:"); it exits 3 when play hasn't started in $TWIN_SWEEP_TIMEOUT
// (180) seconds. The input: the left stick turned round a little each cycle, cross and square now and then, any held button
// skipping the cutscenes ("hold any button to skip": cross is held 1 s each cycle)
#include "ui/bindings.h"

#include "native.h"

#include "game/chunkdata.h"
#include "game/gamecontroller.h"
#include "game/instances.h"
#include "game/place.h"
#include "game/player.h"
#include "game/reference.h"
#include "game/string.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace NativeUi
{
namespace
{
using Clock = std::chrono::steady_clock;

bool g_Read = false;
std::string g_Level;
f32 g_Seconds = 30.0f;
f32 g_Timeout = 180.0f;
bool g_Warped = false;
u32 g_LastState = ~0u;
Clock::time_point g_Start;
Clock::time_point g_PlayStart;
bool g_Playing = false;
f32 g_Played = 0.0f;
Clock::time_point g_LastFrame;
u32 g_Frames = 0;

void ReadSettings()
{
    if (g_Read)
    {
        return;
    }

    g_Read = true;
    const char* level = std::getenv("TWIN_SWEEP_LEVEL");
    g_Level = level != nullptr ? level : "";
    if (const char* seconds = std::getenv("TWIN_SWEEP_SECONDS"))
    {
        g_Seconds = static_cast<f32>(std::atof(seconds));
    }

    if (const char* timeout = std::getenv("TWIN_SWEEP_TIMEOUT"))
    {
        g_Timeout = static_cast<f32>(std::atof(timeout));
    }

    g_Start = Clock::now();
    g_LastFrame = g_Start;
}

// $TWIN_HOP=cryscave,escape: while playing, the player taken through its chunk's link to each of those chunks in turn (the tests'
// way into chunks only entered from a neighbour), placed as the game's WarpToChunkLinkTowardsPlayer places an instance: half a
// unit in front of the link's load wall, its middle, where the link takes it. The player's chunk changes are logged
void HopFrame()
{
    static const char* hops = std::getenv("TWIN_HOP");
    static std::vector<std::string> targets;
    static size_t next = 0;
    static std::string lastChunk;
    static Clock::time_point lastHop;
    if (hops == nullptr || G_GameController == nullptr || G_GameController->State() != GameController::StatePlaying ||
        g_PlayerInstance == nullptr || g_PlayerInstance->object == nullptr)
    {
        return;
    }

    if (targets.empty())
    {
        std::string list = hops;
        for (size_t start = 0; start <= list.size();)
        {
            size_t comma = list.find(',', start);
            comma = comma == std::string::npos ? list.size() : comma;
            if (comma > start)
            {
                targets.push_back(list.substr(start, comma - start));
            }
            start = comma + 1;
        }
    }

    auto* player = static_cast<InstanceContext*>(g_PlayerInstance->object);
    ChunkData* chunk = player->chunk;
    if (chunk == nullptr || chunk->name.string == nullptr)
    {
        return;
    }

    std::string name = chunk->name.string;
    if (name != lastChunk)
    {
        Native::Log("hop: the player in %s", name.c_str());
        for (ChunkLinkData* link = chunk->links; link != nullptr; link = link->next)
        {
            const char* linked = link->linkedData != nullptr && link->linkedData->name.string != nullptr
                                     ? link->linkedData->name.string
                                     : "?";
            Native::Log("hop:   linked to %s%s%s", linked, link->loadWall != nullptr ? "" : " (no wall)",
                        link->flags.linkedRm2Loaded != 0 ? "" : " (not loaded)");
        }

        lastChunk = name;
    }

    if (next < targets.size() && name.size() >= targets[next].size() &&
        name.compare(name.size() - targets[next].size(), std::string::npos, targets[next]) == 0)
    {
        next++;
        return;
    }

    Clock::time_point now = Clock::now();
    if (next >= targets.size() || now - lastHop < std::chrono::seconds(2))
    {
        return;
    }

    for (ChunkLinkData* link = chunk->links; link != nullptr; link = link->next)
    {
        ChunkData* linked = link->linkedData;
        if (linked == nullptr || linked->name.string == nullptr || link->loadWall == nullptr || link->flags.linkedRm2Loaded == 0)
        {
            continue;
        }

        std::string linkedName = linked->name.string;
        if (linkedName.size() < targets[next].size() ||
            linkedName.compare(linkedName.size() - targets[next].size(), std::string::npos, targets[next]) != 0)
        {
            continue;
        }

        const LoadWall* wall = link->loadWall;
        Vector4 position = {0.0f, 0.0f, 0.0f, 1.0f};
        for (const Vector4& corner : wall->corners)
        {
            position.x += corner.x * 0.25f;
            position.y += corner.y * 0.25f;
            position.z += corner.z * 0.25f;
        }

        position.x += wall->plane.x * 0.5f;
        position.y += wall->plane.y * 0.5f;
        position.z += wall->plane.z * 0.5f;
        ObjectPlace* place = player->place;
        place->SyncPosition();
        Native::Log("hop: the player from (%.1f, %.1f, %.1f) in %s to (%.1f, %.1f, %.1f), the link to %s", place->position.x,
                    place->position.y, place->position.z, name.c_str(), position.x, position.y, position.z, linkedName.c_str());
        if (place->MoveTo(&position))
        {
            QueueObject(player);
        }

        lastHop = now;
        return;
    }

    // Not linked yet (a link whose chunk isn't loaded has no name): the player taken near the wall of each such link in turn,
    // three units in front of it (out of its reach), where the chunk loads
    static u32 approach = 0;
    u32 index = 0;
    u32 count = 0;
    for (ChunkLinkData* link = chunk->links; link != nullptr; link = link->next)
    {
        count += link->loadWall != nullptr && (link->linkedData == nullptr || link->linkedData->name.string == nullptr) ? 1 : 0;
    }

    for (ChunkLinkData* link = chunk->links; link != nullptr && count != 0; link = link->next)
    {
        if (link->loadWall == nullptr || (link->linkedData != nullptr && link->linkedData->name.string != nullptr) ||
            index++ != approach % count)
        {
            continue;
        }

        const LoadWall* wall = link->loadWall;
        Vector4 position = {0.0f, 0.0f, 0.0f, 1.0f};
        for (const Vector4& corner : wall->corners)
        {
            position.x += corner.x * 0.25f;
            position.y += corner.y * 0.25f;
            position.z += corner.z * 0.25f;
        }

        position.x += wall->plane.x * 3.0f;
        position.y += wall->plane.y * 3.0f;
        position.z += wall->plane.z * 3.0f;
        Native::Log("hop: the player near an unloaded link of %s, at (%.1f, %.1f, %.1f)", name.c_str(), position.x, position.y,
                    position.z);
        ObjectPlace* place = player->place;
        place->SyncPosition();
        if (place->MoveTo(&position))
        {
            QueueObject(player);
        }

        approach++;
        lastHop = now;
        return;
    }
}

// The player put at a position (the tests' tools), as the game's warps put an instance
void PutPlayer(InstanceContext* player, const Vector4& position)
{
    ObjectPlace* place = player->place;
    place->SyncPosition();
    if (place->MoveTo(&position))
    {
        QueueObject(player);
    }
}

// $TWIN_TRIGGER_PLAN=list or a list of steps (5,die,5): the message triggers of the player's chunk listed once play starts in
// it (by position: their index, place and box), then a step every 8 s: a number puts the player in that trigger, "die" drops
// it 200 units below where it is (it falls to its death). The game's states are the sweep's log
void TriggerPlanFrame()
{
    static const char* plan = std::getenv("TWIN_TRIGGER_PLAN");
    static std::vector<std::string> steps;
    static size_t next = 0;
    static std::string listedChunk;
    static std::vector<InstanceContext*> triggers;
    static Clock::time_point lastStep = Clock::now();
    if (plan == nullptr || G_GameController == nullptr || G_GameController->State() != GameController::StatePlaying ||
        g_PlayerInstance == nullptr || g_PlayerInstance->object == nullptr)
    {
        return;
    }

    auto* player = static_cast<InstanceContext*>(g_PlayerInstance->object);
    ChunkData* chunk = player->chunk;
    if (chunk == nullptr || chunk->name.string == nullptr)
    {
        return;
    }

    // The plan's deaths never end the game
    G_GameController->progress.counts.lives = 50;
    if (steps.empty())
    {
        std::string list = plan;
        for (size_t start = 0; start <= list.size();)
        {
            size_t comma = list.find(',', start);
            comma = comma == std::string::npos ? list.size() : comma;
            if (comma > start)
            {
                steps.push_back(list.substr(start, comma - start));
            }
            start = comma + 1;
        }
    }

    if (listedChunk != chunk->name.string)
    {
        listedChunk = chunk->name.string;
        struct Found
        {
            ChunkData* chunk;
            std::vector<InstanceContext*>* triggers;
        } found = {chunk, &triggers};
        triggers.clear();
        ForEachLiveInstance(
            [](InstanceContext* instance, void* user)
            {
                auto* f = static_cast<Found*>(user);
                if (instance->chunk == f->chunk && instance->place != nullptr &&
                    GetGameNode(&instance->nodes, NodeMessageTrigger) != nullptr)
                {
                    f->triggers->push_back(instance);
                }
            },
            &found);
        for (InstanceContext* trigger : triggers)
        {
            trigger->place->SyncPosition();
        }

        std::sort(triggers.begin(), triggers.end(),
                  [](const InstanceContext* a, const InstanceContext* b)
                  {
                      const Vector4& p = a->place->position;
                      const Vector4& q = b->place->position;
                      return p.x != q.x ? p.x < q.x : p.z != q.z ? p.z < q.z : p.y < q.y;
                  });
        player->place->SyncPosition();
        Native::Log("triggers: %zu in %s, the player at (%.1f, %.1f, %.1f)", triggers.size(), listedChunk.c_str(),
                    player->place->position.x, player->place->position.y, player->place->position.z);
        for (size_t i = 0; i < triggers.size(); i++)
        {
            const Vector4& p = triggers[i]->place->position;
            const Box& box = triggers[i]->collision.box;
            Native::Log("triggers: %zu at (%.1f, %.1f, %.1f), box (%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f)", i, p.x, p.y, p.z, box.min.x,
                        box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
        }
    }

    Clock::time_point now = Clock::now();
    if (next >= steps.size() || now - lastStep < std::chrono::seconds(8))
    {
        return;
    }

    lastStep = now;
    const std::string& step = steps[next++];
    player->place->SyncPosition();
    Vector4 position = player->place->position;
    if (step == "die")
    {
        position.y -= 200.0f;
        Native::Log("triggers: step %s: the player dropped from (%.1f, %.1f, %.1f)", step.c_str(), player->place->position.x,
                    player->place->position.y, player->place->position.z);
        PutPlayer(player, position);
        return;
    }

    size_t index = static_cast<size_t>(std::atoi(step.c_str()));
    if (step == "list" || index >= triggers.size())
    {
        return;
    }

    triggers[index]->place->SyncPosition();
    position = triggers[index]->place->position;
    position.w = 1.0f;
    Native::Log("triggers: step %s: the player into trigger %zu at (%.1f, %.1f, %.1f)", step.c_str(), index, position.x, position.y,
                position.z);
    PutPlayer(player, position);
}

[[noreturn]] void Finish(int status, const char* why)
{
    Native::Log("sweep: %s: %s (%.1f s played, %u frames)", g_Level.c_str(), why, g_Played, g_Frames);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(status);
}
}

bool SweepActive()
{
    ReadSettings();
    return !g_Level.empty();
}

void SweepFrame(bool fromPads)
{
    // $TWIN_START_CHUNK=Levels\Earth\Hub\bossarea: the game started in that chunk as the sweep starts it, then played by hand
    static const char* startChunk = std::getenv("TWIN_START_CHUNK");
    static bool started = false;
    if (startChunk != nullptr && !started && G_GameController != nullptr)
    {
        GameController* controller = G_GameController;
        u32 state = controller->State();
        if (state == GameController::StateVivendiLogo || state == GameController::StateTravellersTalesLogo)
        {
            started = true;
            StringAssign(&controller->progress.startChunk, startChunk);
            controller->states.entry = EntrySaved;
            controller->saveController.Take(&controller->progress, controller->chunkManager, nullptr);
            controller->SetNextState(GameController::StateLoadingLevel);
            Native::Log("start: loading %s", startChunk);
        }
    }

    if (!SweepActive())
    {
        return;
    }

    GameController* controller = G_GameController;
    if (controller == nullptr)
    {
        return;
    }

    if (!fromPads)
    {
        HopFrame();
        TriggerPlanFrame();
        // $TWIN_TEST_HANG=SECONDS: the game's thread stopped that long once play starts (the hang watch's test)
        static const char* hang = std::getenv("TWIN_TEST_HANG");
        static bool hung = false;
        if (hang != nullptr && !hung && controller->State() == GameController::StatePlaying)
        {
            hung = true;
            std::this_thread::sleep_for(std::chrono::seconds(std::atoi(hang)));
        }
    }

    // Called from the game controller's update and the pads' read (pads.cpp), both every frame while the game plays: the time
    // is what passed since either's last call, the frames the game controller's
    Clock::time_point now = Clock::now();
    f32 delta = std::chrono::duration<f32>(now - g_LastFrame).count();
    g_LastFrame = now;
    u32 state = controller->State();
    static f32 lastTick = 0.0f;
    f32 sinceStart = std::chrono::duration<f32>(now - g_Start).count();
    if (std::getenv("TWIN_SWEEP_TRACE") != nullptr && sinceStart - lastTick > 5.0f)
    {
        lastTick = sinceStart;
        Native::Log("sweep: tick at %.1f s: state %u, next %u, played %.1f s, %u frames", sinceStart, state,
                    controller->NextState(), g_Played, g_Frames);
    }
    if (state != g_LastState)
    {
        Native::Log("sweep: state %u at %.1f s", state, std::chrono::duration<f32>(now - g_Start).count());
        g_LastState = state;
    }

    // At the first logo: the level, as the command line's start chunk
    if (!g_Warped && (state == GameController::StateVivendiLogo || state == GameController::StateTravellersTalesLogo))
    {
        g_Warped = true;
        StringAssign(&controller->progress.startChunk, g_Level.c_str());
        controller->states.entry = EntrySaved;
        controller->saveController.Take(&controller->progress, controller->chunkManager, nullptr);
        controller->SetNextState(GameController::StateLoadingLevel);
        Native::Log("sweep: loading %s", g_Level.c_str());
        return;
    }

    // A chunk with no character's instance in it (a hub's part or a tunnel, only ever entered from a neighbour, whose links bring
    // the characters): play would start with none (FollowCameraRig::Restart through a null vtable)
    if (controller->NextState() == GameController::StateStartingPlay)
    {
        bool any = false;
        for (u32 character = 0; character < GameProgress::Characters; character++)
        {
            any = any || controller->progress.Instance(character) != nullptr;
        }

        if (!any)
        {
            Finish(4, "no character in the chunk");
        }
    }

    // Play counted while the game runs (playing, watching a cutscene or a movie)
    bool running = state == GameController::StatePlaying || state == GameController::StateWatching ||
                   state == GameController::StateMovie;
    if (running)
    {
        if (!g_Playing)
        {
            g_Playing = true;
            g_PlayStart = now;
        }

        if (!fromPads)
        {
            g_Frames++;
        }

        g_Played += std::min(delta, 0.25f);
        controller->hudDelay = 0;
        // $TWIN_SWEEP_PICTURES=FIRST: every picture OLEG reads from a file from the FIRSTth (the levels' titles, then the screens'
        // tiles but the gallery's named two), one each 20 frames from 3 s in, read at once
        static const char* pictures = std::getenv("TWIN_SWEEP_PICTURES");
        static u32 nextPicture = pictures != nullptr ? static_cast<u32>(std::atoi(pictures)) : 0;
        if (pictures && !fromPads && g_Played > 3.0f && g_Frames % 20 == 0)
        {
            const u32 titles = OLEG::NoLevelTitle;
            u32 picture = nextPicture < titles ? OLEG::PictureLevelTitle : OLEG::PictureTiles;
            u32 wanted = nextPicture < titles ? nextPicture : nextPicture - titles;
            if (picture == OLEG::PictureTiles && (wanted == OLEG::TilesNamed || wanted == OLEG::TilesNamed + 1))
            {
                wanted = OLEG::TilesCredits;
                nextPicture = titles + OLEG::TilesCredits;
            }

            if (picture == OLEG::PictureTiles && wanted >= OLEG::TilesNone)
            {
                Finish(0, "pictures read");
            }

            Native::Log("sweep: picture %u %u", picture, wanted);
            controller->oleg.pictures[picture].wanted = wanted;
            controller->oleg.LoadPicture(picture, 1);
            nextPicture++;
        }

        if (g_Played >= g_Seconds)
        {
            Finish(0, "ran");
        }
    }
    else if (state == GameController::StateGameOver)
    {
        // The scripted input lost every life (the game's own game over screen, waiting for a choice): played as far as it goes
        Finish(0, "game over");
    }
    else if (state == GameController::StatePaused)
    {
        // Out of a pause the scripted start may have made
        controller->SetNextState(GameController::StatePlaying);
    }

    if (!g_Playing && std::chrono::duration<f32>(now - g_Start).count() > g_Timeout)
    {
        char why[64];
        std::snprintf(why, sizeof(why), "never played (state %u)", state);
        Finish(3, why);
    }
}

void SweepInput(BoundPad* pad)
{
    // $TWIN_AUTOPLAY: the same input in any play (a saved game loaded by $TWIN_SAVE_TEST=load, say), from when play starts
    static const bool autoplay = std::getenv("TWIN_AUTOPLAY") != nullptr;
    f32 t = g_Played;
    if (autoplay && !SweepActive())
    {
        GameController* controller = G_GameController;
        if (controller == nullptr || controller->State() != GameController::StatePlaying)
        {
            return;
        }

        static const Clock::time_point began = Clock::now();
        t = std::chrono::duration<f32>(Clock::now() - began).count();
    }
    else if (!SweepActive() || !g_Playing)
    {
        return;
    }

    // $TWIN_SWEEP_IDLE: no scripted input (the cutscenes play out, nothing is skipped)
    static const bool idle = std::getenv("TWIN_SWEEP_IDLE") != nullptr;
    if (idle)
    {
        return;
    }

    // A 4 s cycle: the stick pushed in a direction that turns 100 degrees a cycle, cross held for the first second (a jump, and
    // the cutscenes' skip), square tapped at 2.5 s
    s32 cycle = static_cast<s32>(t / 4.0f);
    f32 inCycle = t - static_cast<f32>(cycle) * 4.0f;
    f32 angle = static_cast<f32>(cycle) * 1.745f;
    pad->leftX = static_cast<u8>(128.0f + std::sin(angle) * 127.0f);
    pad->leftY = static_cast<u8>(128.0f - std::cos(angle) * 127.0f);
    if (inCycle < 1.0f)
    {
        pad->buttons |= PadBitCross;
    }

    if (inCycle > 2.5f && inCycle < 2.7f)
    {
        pad->buttons |= PadBitSquare;
    }
}
}
