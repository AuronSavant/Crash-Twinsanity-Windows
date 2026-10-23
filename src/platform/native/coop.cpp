// Two players on one machine (native/COOP.md). The game already plays two characters at once in most of its levels: the one
// played (GameProgress::play.character, its controls node reading the main pad) and the second (play.second: Cortex with Crash,
// Nina, ...), whose controls node has no pad, so it follows the first by itself. With co-op on, the second character's controls
// node is given pad port 1 while the first is played with its pad: the same controls the game gives a character it plays, read
// from the second gamepad. Whenever the game takes control away (cutscenes, a vehicle, the characters tied or carried) the pad is
// taken back, so the game's own scripts drive the second character as they always did
#include "coop.h"

#include "native.h"

#include "game/agentparts.h"
#include "game/agents.h"
#include "game/behaviours.h"
#include "game/characters.h"
#include "game/chunkdata.h"
#include "game/chunkloading.h"
#include "game/gamecontroller.h"
#include "game/instancefactory.h"
#include "game/instances.h"
#include "game/lights.h"
#include "game/objectnode.h"
#include "game/objects.h"
#include "game/place.h"
#include "game/pads.h"
#include "game/player.h"
#include "game/progress.h"
#include "platform/pads.h"
#include "game/properties.h"
#include "game/reference.h"
#include "game/resources.h"
#include "game/vehicles.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace
{
// -1: not asked for on the command line ($TWIN_COOP decides)
int g_Enabled = -1;
bool g_KeyboardSecond = false;

// The second character the pad was given to (none: the game's own)
InstanceContext* g_Controlled = nullptr;
u32 g_Frames = 0;

// $TWIN_COOP_SPAWN=0: no partner made where the level has none
bool SpawnPartners()
{
    static const char* asked = std::getenv("TWIN_COOP_SPAWN");
    return asked == nullptr || std::strcmp(asked, "0") != 0;
}

bool Logging()
{
    static const bool asked = std::getenv("TWIN_COOP_LOG") != nullptr;
    return asked;
}

Vector4 PositionOf(const InstanceContext* instance)
{
    const ObjectPlace* place = instance->place;
    if (place == nullptr)
    {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    if ((place->bits.value & PlaceBits::MatrixMoved) != 0)
    {
        return {place->matrix.m[3][0], place->matrix.m[3][1], place->matrix.m[3][2], 1.0f};
    }

    return place->position;
}

ControlsNode* ControlsOf(InstanceContext* instance)
{
    return static_cast<ControlsNode*>(GetGameNode(&instance->nodes, NodeControls));
}

// Player 2's character from the level: the progress's second, the one the game itself pairs with player 1's (tied, the
// Rollerbrawl, the Humiliskate, ...). Any other playable character the level made is the story's (the boss area's Cortex is the
// boss, the one the aliens take is about to go) and is left to it: then player 2 plays a twin of player 1's character
InstanceContext* SecondCharacter(const GameProgress& progress, InstanceContext*, u32* which)
{
    *which = progress.play.second;
    return progress.play.second != GameProgress::NoCharacter ? progress.Instance(progress.play.second) : nullptr;
}

// The pad given back: the second character stands still until the game moves it again (as DisableCharacter leaves it)
void Release(InstanceContext* instance, GamePad* secondPad, const char* why)
{
    if (instance != nullptr && InstanceLive(instance))
    {
        ControlsNode* controls = ControlsOf(instance);
        if (controls != nullptr && controls->pad == secondPad)
        {
            controls->pad = nullptr;
            if (CharacterAgent* agent = CharacterAgentOf(instance))
            {
                agent->moveInput = {0.0f, 0.0f, 0.0f, 1.0f};
            }
        }
    }

    if (g_Controlled != nullptr)
    {
        Native::Log("coop: player 2 lets go (%s)", why);
    }

    g_Controlled = nullptr;
}

// Why the second character can't be player 2's now (none: it can)
const char* Unplayable(GameController* controller, InstanceContext* first, InstanceContext* second)
{
    if (first == nullptr || second == nullptr || first == second || !InstanceLive(first) || !InstanceLive(second))
    {
        return "no second character";
    }

    ControlsNode* firstControls = ControlsOf(first);
    ControlsNode* controls = ControlsOf(second);
    if (firstControls == nullptr || controls == nullptr)
    {
        return "no controls";
    }

    // The game took player 1's control away (a cutscene, a scripted move): player 2's too
    if (firstControls->pad != controller->pad || firstControls->bits.motionDriven != 0)
    {
        return "player 1 isn't in control";
    }

    // The game moves the second character itself (a scripted walk), or someone else's pad reads it
    if (controls->bits.motionDriven != 0)
    {
        return "the game moves it";
    }

    if (controls->pad != nullptr && controls->pad != controller->secondPad)
    {
        return "another pad has it";
    }

    // On a vehicle (its controls the vehicle's) or a passenger, or tied to the other character (the leader moves both)
    CharacterAgent* agent = CharacterAgentOf(second);
    CharacterAgent* firstAgent = CharacterAgentOf(first);
    if (agent == nullptr || agent->vehicle != nullptr || controls->replacement != nullptr)
    {
        return "on a vehicle";
    }

    if (firstAgent != nullptr && firstAgent->vehicle != nullptr)
    {
        return "player 1 is on a vehicle";
    }

    CharacterMoveBits moveBits = static_cast<CharacterPart*>(agent->part)->moveBits;
    if (moveBits.linkedFirst != 0 || moveBits.linkedSecond != 0)
    {
        return "tied together";
    }

    return nullptr;
}

// The partner made for player 2 where the level has none: Cortex beside Crash, else Crash beside whoever is played. Only when the
// partner's object is in the game's resources (its model, animations and scripts loaded with the level or the game's common
// ones): the game makes a playable character once, so none is made while the partner is anywhere else
ChunkData* g_SpawnTriedChunk = nullptr;
// The partner made (none when it's gone with its chunk), and whether it's a twin of player 1's character
Reference* g_Partner = nullptr;
bool g_PartnerIsTwin = false;
// Player 2 asked to be brought back beside player 1 (their Select, or F6), and a restart seen (the twin put back after it)
bool g_RegroupAsked = false;
bool g_RestartSeen = false;
// Player 2's pad added into player 1's (the Rollerbrawl)
bool g_SharesPad = false;
// While a script runs with the twin as the player (TwinAsPlayer), the twin
InstanceContext* g_ScriptPlayer = nullptr;

InstanceContext* Partner()
{
    auto* partner = g_Partner != nullptr ? static_cast<InstanceContext*>(g_Partner->object) : nullptr;
    return partner != nullptr && InstanceLive(partner) ? partner : nullptr;
}
u32 g_ControlledFrames = 0;
constexpr u32 SpawnAfterFrames = 90;
// The flags a twin takes from player 1's instance: shown, solid, shadowed and reached by triggers
constexpr u32 PlayerFlags = ReferencedObjectFlags::Visible | ReferencedObjectFlags::CollisionActive |
                            ReferencedObjectFlags::ShadowActive | ReferencedObjectFlags::ReceivesTriggerSignals;
constexpr f32 SpawnBeside = 1.5f;

void SpawnPartner(GameController* controller, InstanceContext* first)
{
    // Once player 1 has played a moment in a chunk, once a chunk
    if (first == nullptr || !InstanceLive(first) || first->chunk == g_SpawnTriedChunk || ++g_ControlledFrames < SpawnAfterFrames)
    {
        return;
    }

    ControlsNode* firstControls = ControlsOf(first);
    CharacterAgent* firstAgent = CharacterAgentOf(first);
    if (firstControls == nullptr || firstAgent == nullptr || firstControls->pad != controller->pad ||
        firstControls->bits.motionDriven != 0 || firstAgent->vehicle != nullptr ||
        controller->State() != GameController::StatePlaying)
    {
        return;
    }

    g_SpawnTriedChunk = first->chunk;
    GameProgress& progress = controller->progress;
    // Always a twin of the one played (a second Crash): a Cortex made here would be the story's Cortex (the boss's, the one the
    // aliens take)
    u32 played = progress.play.character;
    u32 partner = played;
    u16 object = firstAgent->objectId;
    constexpr bool twin = true;
    Native::Log("coop: player 2 gets a twin of character %u", played);

    // The chunk player 1 is in now, loaded (the agent's chunk index is the chunk it started the level in: once player 1 had
    // gone on into another and that one was let go, the twin's camera rig was made in a chunk with no scenery, a crash)
    ChunkEntry* chunk = ChunkOfInstance(controller->chunkManager, first);
    if (chunk == nullptr || chunk->data == nullptr || chunk->data->chunk != first->chunk)
    {
        Native::Log("coop: player 1's chunk isn't ready, the twin's made later");
        g_SpawnTriedChunk = nullptr;
        g_ControlledFrames = 0;
        return;
    }

    Vector4 position = PositionOf(first);
    position.x += SpawnBeside;
    position.y += 0.2f;
    s32 angles[3] = {0, 0, 0};
    InstanceFactory* factory = g_InstanceFactory;
    factory->SetInstanceProperties();
    factory->ClearUnused1();
    factory->SetGivesIds();
    factory->ClearGivesFlagSlots();
    factory->creationFlags = 0;
    // The game makes a playable character once: its handle emptied while the twin's made (the factory's stand-in then keeps the
    // new instance as the character's, with controls and follow nodes of its own), then given back
    Reference* kept = nullptr;
    if (twin)
    {
        kept = progress.characters[played];
        progress.characters[played] = nullptr;
    }

    // A twin takes player 1's instance's properties (its layout's: its state, its near distance), a partner its object's
    InstanceContext* made = twin ? CreateInstanceFrom(factory, chunk, first, object, &position, angles, 0)
                                 : CreateInstance(factory, chunk, object, &position, angles);
    if (made != nullptr)
    {
        AssignReference(&g_Partner, made);
        g_PartnerIsTwin = twin;
        Native::Log("coop: partner made: flags 0x%X, its chunk %s player 1's", made->flags.value,
                    made->chunk == first->chunk ? "is" : "isn't");
        // Woken and queued as the game does a character it moves into play
        if (made->flags.asleep != 0)
        {
            CallVirtual<void>(made, made->vtable, ReferencedObject::WakeSlot);
        }

        // Shown, solid, shadowed and reached by triggers as player 1 is (its object's state has it hidden: the layouts' instances
        // of the characters give theirs)
        made->flags.value = (made->flags.value & ~PlayerFlags) | (first->flags.value & PlayerFlags);
        // Its nodes updated whatever the distance (its object gives none: the layouts' instances of the characters do, and the
        // spawn commands give theirs the same)
        for (GameNode* node : made->nodes.nodes)
        {
            if (node != nullptr)
            {
                node->nearDistance = GameNode::AnyNearDistance;
            }
        }
        QueueObject(made);
        Native::Log("coop: partner woken: flags 0x%X (player 1's 0x%X), clock %u (player 1's %u), seen %u (player 1's %u)",
                    made->flags.value, first->flags.value, made->clockIndex, first->clockIndex, made->seen, first->seen);
    }

    if (twin)
    {
        if (progress.characters[played] != nullptr)
        {
            RemoveReference(&progress.characters[played]);
        }

        progress.characters[played] = kept;
    }
    Native::Log("coop: made character %u for player 2 at (%.1f, %.1f, %.1f): %s", partner, position.x, position.y, position.z,
                made != nullptr ? "ok" : "failed");
}

// Player 2's character brought back beside player 1 (Select on player 2's pad or F6, and after every restart): put in player 1's
// chunk, woken, shown and solid as player 1, alive with player 1's hit points and nothing locked. With none (a twin gone with a
// restart or a chunk), one is made beside player 1 at once
void Regroup(GameController* controller, InstanceContext* first, const char* why)
{
    if (first == nullptr || !InstanceLive(first))
    {
        return;
    }

    InstanceContext* twin = Partner();
    // The character player 2 plays (the level's Cortex), else its twin
    InstanceContext* second = g_Controlled != nullptr && InstanceLive(g_Controlled) && g_Controlled != twin ? g_Controlled : twin;
    if (second == nullptr || second == first)
    {
        if (NativeCoop::SecondPlayerPresent() && SpawnPartners())
        {
            g_SpawnTriedChunk = nullptr;
            // After the usual moment of player 1's control: made at once after a restart, the area wasn't ready (its camera
            // rig's made in the chunk's scenery: a crash at the first boss's restart)
            g_ControlledFrames = 0;
            Native::Log("coop: regroup (%s): no character for player 2, one is made", why);
        }

        return;
    }

    CharacterAgent* agent = CharacterAgentOf(second);
    CharacterAgent* firstAgent = CharacterAgentOf(first);
    if (agent == nullptr || firstAgent == nullptr || agent->vehicle != nullptr)
    {
        return;
    }

    if (second->chunk != first->chunk && first->chunk != nullptr)
    {
        MoveToChunk(first->chunk, second);
    }

    if (second->flags.asleep != 0)
    {
        CallVirtual<void>(second, second->vtable, ReferencedObject::WakeSlot);
    }

    if (second == twin)
    {
        second->flags.value = (second->flags.value & ~PlayerFlags) | (first->flags.value & PlayerFlags);
        static_cast<CharacterPart*>(agent->part)->flags.hitPoints = static_cast<CharacterPart*>(firstAgent->part)->flags.hitPoints;
        agent->state.dead = 0;
        agent->buttons.locked.value = 0;
        if (ControlsNode* controls = ControlsOf(second))
        {
            controls->bits.motionDriven = 0;
        }
    }

    agent->velocity = {0.0f, 0.0f, 0.0f, 1.0f};
    Vector4 position = PositionOf(first);
    position.x += SpawnBeside;
    position.y += 0.5f;
    ObjectPlace* place = second->place;
    place->SyncPosition();
    place->MoveTo(&position);
    QueueObject(second);
    Native::Log("coop: regroup (%s): player 2 put beside player 1 at (%.1f, %.1f, %.1f)", why, position.x, position.y, position.z);
}

void LogFrame(GameController* controller, InstanceContext* first, InstanceContext* second, const char* why)
{
    if (!Logging() || g_Frames % 120 != 0)
    {
        return;
    }

    const GameProgress& progress = controller->progress;
    Vector4 a = first != nullptr ? PositionOf(first) : Vector4{};
    Vector4 b = second != nullptr ? PositionOf(second) : Vector4{};
    ControlsNode* controls = second != nullptr ? ControlsOf(second) : nullptr;
    // The characters whose instances are made (a bit each), and each one's controls: p(ad 0), 2 (pad 1), n(one), m(otion driven)
    char present[GameProgress::Characters + 1] = {};
    for (u32 character = 0; character < GameProgress::Characters; character++)
    {
        InstanceContext* instance = progress.Instance(character);
        ControlsNode* its = instance != nullptr && InstanceLive(instance) ? ControlsOf(instance) : nullptr;
        present[character] = its == nullptr                         ? '.'
                             : its->bits.motionDriven != 0          ? 'm'
                             : its->pad == controller->pad          ? 'p'
                             : its->pad == controller->secondPad    ? '2'
                             : its->pad == nullptr                  ? 'n'
                                                                    : '?';
    }

    Native::Log("coop: made %s, second's flags 0x%X", present, second != nullptr ? second->flags.value : 0);
    if (CharacterAgent* agent = second != nullptr ? CharacterAgentOf(second) : nullptr)
    {
        GamePad* pad = controller->secondPad;
        Native::Log("coop: second's move (%.2f, %.2f, %.2f) walk %s, pad 2 buttons 0x%X stick (%u, %u)", agent->moveInput.x,
                    agent->moveInput.y, agent->moveInput.z, agent->walk != nullptr ? "yes" : "no", pad->now,
                    pad->info != nullptr ? pad->info->report[Platform::Pads::StickLeftX] : 0,
                    pad->info != nullptr ? pad->info->report[Platform::Pads::StickLeftY] : 0);
        auto* node = static_cast<ObjectNode*>(GetGameNode(&second->nodes, NodeObject));
        Native::Log("coop: second's frozen %u collision %u scripted %u", static_cast<CharacterPart*>(agent->part)->bits.frozen,
                    second->flags.collisionActive, node != nullptr ? node->flags.moves : 9u);
        CharacterAgent* firstAgent = first != nullptr ? CharacterAgentOf(first) : nullptr;
        Native::Log("coop: second's locks 0x%X velocity (%.2f, %.2f, %.2f) state 0x%X; first's locks 0x%X velocity (%.2f, %.2f, "
                    "%.2f) state 0x%X",
                    agent->buttons.locked.value, agent->velocity.x, agent->velocity.y, agent->velocity.z, agent->state.value,
                    firstAgent != nullptr ? firstAgent->buttons.locked.value : 0, firstAgent != nullptr ? firstAgent->velocity.x : 0,
                    firstAgent != nullptr ? firstAgent->velocity.y : 0, firstAgent != nullptr ? firstAgent->velocity.z : 0,
                    firstAgent != nullptr ? firstAgent->state.value : 0);
    }
    Native::Log("coop: frame %u characters %u+%u pairing %u p1 (%.1f, %.1f, %.1f) p2 (%.1f, %.1f, %.1f) pad %s motion %u "
                "replaced %u present %u: %s",
                g_Frames, progress.play.character, progress.play.second, progress.play.pairing, a.x, a.y, a.z, b.x, b.y, b.z,
                controls == nullptr                    ? "-"
                : controls->pad == nullptr             ? "none"
                : controls->pad == controller->secondPad ? "p2"
                                                       : "p1",
                controls != nullptr ? controls->bits.motionDriven : 0, controls != nullptr && controls->replacement != nullptr,
                NativeCoop::SecondPlayerPresent(), why != nullptr ? why : "player 2 plays");
}
}

bool NativeCoop::Enabled()
{
    if (g_Enabled < 0)
    {
        const char* environment = std::getenv("TWIN_COOP");
        g_Enabled = environment != nullptr && std::strcmp(environment, "0") != 0 ? 1 : 0;
    }

    return g_Enabled != 0;
}

void NativeCoop::SetEnabled(bool on)
{
    g_Enabled = on ? 1 : 0;
}

bool NativeCoop::KeyboardSecond()
{
    return g_KeyboardSecond || std::getenv("TWIN_COOP_KEYBOARD") != nullptr;
}

void NativeCoop::SetKeyboardSecond(bool on)
{
    g_KeyboardSecond = on;
}

bool NativeCoop::SharesPad()
{
    return g_SharesPad;
}

bool NativeCoop::IsTwin(const InstanceContext* instance)
{
    return instance != nullptr && instance == Partner() && g_PartnerIsTwin;
}

void NativeCoop::TintTwinLights(ChunkLights* lights)
{
    // Red and green down, blue up: the twin's a blue-tinged Crash in any light
    constexpr f32 Red = 0.55f;
    constexpr f32 Green = 0.75f;
    constexpr f32 Blue = 1.6f;
    auto tint = [](Vector4* colour) {
        colour->x *= Red;
        colour->y *= Green;
        colour->z *= Blue;
    };

    for (Vector4& colour : lights->colours)
    {
        tint(&colour);
    }

    tint(&lights->ambient);
}

namespace
{
// Whether a creature (an enemy) is nearer the twin than player 1: in the twin's chunk without player 1, else by distance
bool NearerTheTwin(InstanceContext* creature, InstanceContext* twin)
{
    GameController* controller = G_GameController;
    if (creature == nullptr || creature == twin || !InstanceLive(creature) ||
        GetGameNode(&creature->nodes, NodeCreature) == nullptr || controller == nullptr || creature->chunk != twin->chunk)
    {
        return false;
    }

    const GameProgress& progress = controller->progress;
    InstanceContext* first = progress.Instance(progress.play.character);
    if (first == nullptr || !InstanceLive(first) || first->chunk != creature->chunk)
    {
        return true;
    }

    Vector4 at = PositionOf(creature);
    Vector4 a = PositionOf(first);
    Vector4 b = PositionOf(twin);
    f32 toFirst = (at.x - a.x) * (at.x - a.x) + (at.y - a.y) * (at.y - a.y) + (at.z - a.z) * (at.z - a.z);
    f32 toTwin = (at.x - b.x) * (at.x - b.x) + (at.y - b.y) * (at.y - b.y) + (at.z - b.z) * (at.z - b.z);
    return toTwin < toFirst;
}
}

InstanceContext* NativeCoop::ScriptPlayer()
{
    return g_ScriptPlayer;
}

void NativeCoop::RequestRegroup()
{
    if (Enabled())
    {
        g_RegroupAsked = true;
    }
}

NativeCoop::TwinAsPlayer::TwinAsPlayer(const void* originator, InstanceContext* owner)
{
    InstanceContext* twin = Partner();
    if (twin == nullptr || !g_PartnerIsTwin || twin->flags.asleep != 0)
    {
        return;
    }

    // A boss fight (the boss's health bar) is the game's own: its scripts see player 1 alone (the Mecha-Bandicoot's escape
    // platform never came while the boss took the twin for the player)
    GameController* controller = G_GameController;
    if (controller == nullptr || controller->progress.play.mode == PlayHealth)
    {
        return;
    }

    // What the twin set off, and the enemies nearer the twin (they chase and attack the player their scripts see)
    if (originator != twin)
    {
        if (!NearerTheTwin(owner, twin))
        {
            return;
        }

        static u32 enemyRuns = 0;
        if (Logging() && enemyRuns++ % 600 == 0)
        {
            Native::Log("coop: an enemy's script takes player 2 for the player (%u so far)", enemyRuns);
        }
    }

    CharacterAgent* agent = CharacterAgentOf(twin);
    if (agent == nullptr || g_Partner == nullptr)
    {
        return;
    }

    swapped_ = true;
    saved_[0] = g_PlayerInstance;
    saved_[1] = g_PlayerCharacter;
    saved_[2] = g_PlayerCharacter2;
    saved_[3] = g_PlayerPart;
    saved_[4] = g_PlayerPart2;
    g_PlayerInstance = g_Partner;
    g_PlayerCharacter = static_cast<PlayerCharacter*>(agent);
    g_PlayerCharacter2 = static_cast<PlayerCharacter*>(agent);
    g_PlayerPart = static_cast<CharacterPart*>(agent->part);
    g_PlayerPart2 = static_cast<CharacterPart*>(agent->part);
    saved_[5] = g_ScriptPlayer;
    g_ScriptPlayer = twin;
}

NativeCoop::TwinAsPlayer::~TwinAsPlayer()
{
    if (!swapped_)
    {
        return;
    }

    g_PlayerInstance = static_cast<Reference*>(saved_[0]);
    g_PlayerCharacter = static_cast<PlayerCharacter*>(saved_[1]);
    g_PlayerCharacter2 = static_cast<PlayerCharacter*>(saved_[2]);
    g_PlayerPart = static_cast<CharacterPart*>(saved_[3]);
    g_PlayerPart2 = static_cast<CharacterPart*>(saved_[4]);
    g_ScriptPlayer = static_cast<InstanceContext*>(saved_[5]);
}

namespace
{
// The throw trace: on until then (from Cortex's thrown events)
using TraceClock = std::chrono::steady_clock;
TraceClock::time_point g_TraceUntil{};
constexpr auto TraceTime = std::chrono::seconds(15);

bool Tracing()
{
    return TraceClock::now() < g_TraceUntil;
}

InstanceContext* CortexInstance()
{
    GameController* controller = G_GameController;
    return controller != nullptr ? controller->progress.Instance(CharacterCortex) : nullptr;
}

// Who an instance is, for the trace: the players' characters by name, anything else by its object's name
const char* WhoIs(const InstanceContext* instance)
{
    if (instance == nullptr)
    {
        return "-";
    }

    if (instance == CortexInstance())
    {
        return "Cortex";
    }

    if (instance == Partner())
    {
        return "player 2's twin";
    }

    GameController* controller = G_GameController;
    if (controller != nullptr && instance == controller->progress.Instance(controller->progress.play.character))
    {
        return "player 1";
    }

    auto* node = static_cast<AgentNode*>(GetGameNode(&const_cast<InstanceContext*>(instance)->nodes, NodeCharacter));
    AgentNode* agentNode = node != nullptr ? node : AgentNodeOf(const_cast<InstanceContext*>(instance));
    GameObject* object = agentNode != nullptr && agentNode->agent != nullptr ? agentNode->agent->object : nullptr;
    return object != nullptr && object->name.string != nullptr ? object->name.string : "?";
}
}

void NativeCoop::TraceEvent(Agent* agent, u32 event, const void* originator)
{
    if (!Enabled() || agent == nullptr)
    {
        return;
    }

    InstanceContext* cortex = CortexInstance();
    bool isCortex = cortex != nullptr && agent->instance == cortex;
    if (isCortex && (event == EventThrownFromSpin || event == EventThrownFromJump))
    {
        g_TraceUntil = TraceClock::now() + TraceTime;
        Native::Log("trace: Cortex thrown (event %u): his scripts and what he sets off logged for 15 s", event);
    }

    if (Tracing() && (isCortex || (cortex != nullptr && originator == cortex)))
    {
        Native::Log("trace: event %u on %s (from %s)", event, WhoIs(agent->instance),
                    WhoIs(static_cast<const InstanceContext*>(originator)));
    }
}

void NativeCoop::TraceCommand(BehaviourRunner* runner, u32 commandId)
{
    if (!Tracing() || runner == nullptr)
    {
        return;
    }

    InstanceContext* cortex = CortexInstance();
    InstanceContext* owner = runner->agentNode != nullptr ? runner->agentNode->owner : nullptr;
    if (cortex != nullptr && (owner == cortex || runner->originator == cortex))
    {
        Native::Log("trace: %s runs command %u (from %s)", WhoIs(owner), commandId,
                    WhoIs(static_cast<const InstanceContext*>(runner->originator)));
    }
}

bool NativeCoop::Teammates(InstanceContext* one, InstanceContext* other)
{
    InstanceContext* twin = Partner();
    GameController* controller = G_GameController;
    if (twin == nullptr || one == nullptr || other == nullptr || controller == nullptr)
    {
        return false;
    }

    const GameProgress& progress = controller->progress;
    InstanceContext* first = progress.Instance(progress.play.character);
    return (one == twin && other == first) || (one == first && other == twin);
}

bool NativeCoop::SpareTwin(InstanceContext* instance)
{
    GameController* controller = G_GameController;
    if (instance == nullptr || instance != Partner() || controller == nullptr)
    {
        return false;
    }

    GameProgress& progress = controller->progress;
    InstanceContext* first = progress.Instance(progress.play.character);
    if (first == nullptr || first == instance || !InstanceLive(first))
    {
        return false;
    }

    // Back beside player 1, alive: the twin never dies (moving it to a checkpoint's placement crashed the game: a placement in a
    // chunk that isn't loaded lets the instance go). Player 2 can also come back any time with Select (Regroup)
    if (CharacterAgent* agent = CharacterAgentOf(instance))
    {
        agent->velocity = {0.0f, 0.0f, 0.0f, 1.0f};
    }

    Vector4 position = PositionOf(first);
    position.x += SpawnBeside;
    position.y += 0.5f;
    ObjectPlace* place = instance->place;
    place->SyncPosition();
    if (place->MoveTo(&position))
    {
        QueueObject(instance);
    }

    Native::Log("coop: player 2's twin would have died: back beside player 1");
    return true;
}

void NativeCoop::Frame()
{
    if (!Enabled())
    {
        return;
    }

    GameController* controller = G_GameController;
    if (controller == nullptr || controller->secondPad == nullptr)
    {
        return;
    }

    g_Frames++;
    GameProgress& progress = controller->progress;
    InstanceContext* first = progress.Instance(progress.play.character);
    // Both steer the Rollerbrawl (Crash and Cortex rolling as they fight): player 1 drives it, player 2's pad added in (pads.cpp)
    CharacterAgent* firstAgent = first != nullptr && InstanceLive(first) ? CharacterAgentOf(first) : nullptr;
    bool sharing = firstAgent != nullptr && firstAgent->vehicle != nullptr &&
                   firstAgent->vehicle->Kind() == Vehicle::KindRollerbrawl && SecondPlayerPresent();
    if (sharing != g_SharesPad)
    {
        Native::Log(sharing ? "coop: both players steer the Rollerbrawl" : "coop: the Rollerbrawl's over");
    }

    g_SharesPad = sharing;
    // A restart (player 1's death, a checkpoint restarted): the twin put back beside player 1 once play is on again. Only a
    // restart: the end of a cutscene starts play again too (EndCutscene's StateStartingPlay), and moving the twin then breaks
    // the scripts that follow it
    u32 state = controller->State();
    if (state == GameController::StateRestarting)
    {
        g_RestartSeen = true;
    }
    else if (state == GameController::StatePlaying && (g_RestartSeen || g_RegroupAsked))
    {
        Regroup(controller, first, g_RestartSeen ? "after a restart" : "player 2 asked");
        g_RestartSeen = false;
        g_RegroupAsked = false;
    }
    u32 which = GameProgress::NoCharacter;
    InstanceContext* second = SecondCharacter(progress, first, &which);
    InstanceContext* twin = Partner();
    if (twin == first)
    {
        twin = nullptr;
    }

    // The twin waits where it is while player 2 plays the paired character (it's never put to sleep: an instance put to sleep
    // while the area loaded hung the game entering the first boss's area), and is player 2's again, beside player 1, after
    bool backToTwin = second == nullptr && twin != nullptr && g_Controlled != nullptr && g_Controlled != twin;
    if (second == nullptr && twin != nullptr)
    {
        second = twin;
        which = progress.play.character;
        if (backToTwin && state == GameController::StatePlaying)
        {
            // The paired character let go first: the regroup moves the twin, not it
            Release(g_Controlled, controller->secondPad, "back to player 2's twin");
            Regroup(controller, first, "back to player 2's twin");
        }
    }

    // The second character changed (a switch, a new level): the one before let go
    if (g_Controlled != nullptr && g_Controlled != second)
    {
        Release(g_Controlled, controller->secondPad, "the second character changed");
        // Gone (a restart, a level left): made again where player 1 plays next
        g_SpawnTriedChunk = nullptr;
    }

    if (second == nullptr && SecondPlayerPresent() && SpawnPartners())
    {
        SpawnPartner(controller, first);
    }
    else
    {
        g_ControlledFrames = 0;
    }

    const char* why = SecondPlayerPresent() ? Unplayable(controller, first, second) : "no player 2";
    LogFrame(controller, first, second, why);
    if (why != nullptr)
    {
        if (g_Controlled != nullptr)
        {
            Release(g_Controlled, controller->secondPad, why);
        }

        return;
    }

    ControlsNode* controls = ControlsOf(second);
    if (controls->pad != controller->secondPad)
    {
        controls->pad = controller->secondPad;
        g_Controlled = second;
        Native::Log("coop: player 2 plays character %u", which);
    }

    // Player 2's Cortex shoots with square (the spin's button): the gun's input kept unlocked (his companion scripts lock it) and
    // its ammo topped up
    CharacterAgent* agent = CharacterAgentOf(second);
    if (second != Partner() && agent != nullptr && agent->gun != nullptr &&
        agent->properties->GetInt(CharacterKindProperty) == CharacterCortex)
    {
        constexpr u32 LowAmmo = 5;
        constexpr u32 ToppedUpAmmo = 15;
        agent->buttons.locked.square = 0;
        if (agent->gun->bits.ammo < LowAmmo)
        {
            agent->gun->bits.ammo = ToppedUpAmmo;
        }
    }
}
