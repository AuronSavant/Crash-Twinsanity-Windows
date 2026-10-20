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
#include "game/characters.h"
#include "game/chunkdata.h"
#include "game/chunkloading.h"
#include "game/gamecontroller.h"
#include "game/instancefactory.h"
#include "game/instances.h"
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

#include <cstdlib>
#include <cstring>

namespace
{
// -1: not asked for on the command line ($TWIN_COOP decides)
int g_Enabled = -1;
bool g_KeyboardFirst = false;

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

// Player 2's character: the progress's second (its pairings that play two: tied, the Rollerbrawl, ...), else another playable
// character the level made beside the one played (Cortex following Crash: the progress names no second then), the first of them
InstanceContext* SecondCharacter(const GameProgress& progress, InstanceContext* first, u32* which)
{
    *which = progress.play.second;
    if (progress.play.second != GameProgress::NoCharacter)
    {
        return progress.Instance(progress.play.second);
    }

    // (CharacterNone's slot is the level's other characters that aren't playable)
    for (u32 character = 0; character < GameProgress::Characters; character++)
    {
        InstanceContext* instance = character != CharacterNone ? progress.Instance(character) : nullptr;
        if (instance != nullptr && instance != first && InstanceLive(instance) && ControlsOf(instance) != nullptr)
        {
            *which = character;
            return instance;
        }
    }

    return nullptr;
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
// The frame (g_Frames) the twin last died, and how long after it player 1's control is watched (10 s)
u32 g_TwinDiedFrame = 0;
bool g_TwinDied = false;
constexpr u32 WatchAfterDeathFrames = 600;

InstanceContext* Partner()
{
    auto* partner = g_Partner != nullptr ? static_cast<InstanceContext*>(g_Partner->object) : nullptr;
    return partner != nullptr && InstanceLive(partner) ? partner : nullptr;
}
u32 g_ControlledFrames = 0;
constexpr u32 SpawnAfterFrames = 90;
constexpr f32 SpawnBeside = 1.5f;

u16 PartnerObject(u32 partner)
{
    GameResources* resources = g_InstanceFactory != nullptr ? g_InstanceFactory->resources : nullptr;
    ResourceTable* objects = resources != nullptr ? resources->objects : nullptr;
    if (objects == nullptr || objects->items == nullptr)
    {
        return NoObjectId;
    }

    for (u32 id = 0; id < objects->bits.capacity; id++)
    {
        auto* object = static_cast<GameObject*>(objects->items[id]);
        if (object == nullptr || object->header.type != GameObject::TypeCharacter || object->properties == nullptr ||
            object->properties->intCount == 0)
        {
            continue;
        }

        if (object->properties->IntAt(CharacterKindProperty) == static_cast<s32>(partner))
        {
            Native::Log("coop: character %u's object is 0x%X (%s)", partner, id,
                        object->name.string != nullptr ? object->name.string : "?");
            return static_cast<u16>(id);
        }
    }

    return NoObjectId;
}

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
        firstControls->bits.motionDriven != 0 || firstAgent->vehicle != nullptr)
    {
        return;
    }

    g_SpawnTriedChunk = first->chunk;
    GameProgress& progress = controller->progress;
    u32 played = progress.play.character;
    u32 partner = played == CharacterCrash ? CharacterCortex : CharacterCrash;
    if (progress.Instance(partner) != nullptr)
    {
        return;
    }

    u16 object = PartnerObject(partner);
    // Not loaded here (Cortex on the beach): a twin of the one played, whose object is
    bool twin = object == NoObjectId;
    if (twin)
    {
        object = firstAgent->objectId;
        partner = played;
        Native::Log("coop: character %u isn't loaded here, player 2 gets a twin of character %u", partner, played);
    }

    ChunkEntry* chunk = ChunkOfIndex(controller->chunkManager, firstAgent->chunkIndex);
    if (chunk == nullptr)
    {
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
        constexpr u32 PlayerFlags = ReferencedObjectFlags::Visible | ReferencedObjectFlags::CollisionActive |
                                    ReferencedObjectFlags::ShadowActive | ReferencedObjectFlags::ReceivesTriggerSignals;
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

// After the twin died (its death's scripts were written for the one player: they take control away and wait for the restart that
// no longer comes), player 1's control given back while the game is in plain play
void KeepPlayerOneInControl(GameController* controller, InstanceContext* first)
{
    if (!g_TwinDied || g_Frames - g_TwinDiedFrame > WatchAfterDeathFrames)
    {
        g_TwinDied = false;
        return;
    }

    if (first == nullptr || !InstanceLive(first) || controller->State() != GameController::StatePlaying)
    {
        return;
    }

    ControlsNode* controls = ControlsOf(first);
    CharacterAgent* agent = CharacterAgentOf(first);
    if (controls == nullptr || agent == nullptr || agent->vehicle != nullptr || agent->state.dead != 0)
    {
        return;
    }

    bool taken = controls->pad != controller->pad || controls->bits.motionDriven != 0 ||
                 agent->buttons.locked.value == CharacterButtons::LockAll;
    if (!taken)
    {
        return;
    }

    controls->pad = controller->pad;
    controls->bits.motionDriven = 0;
    if (agent->buttons.locked.value == CharacterButtons::LockAll)
    {
        agent->buttons.locked.value = 0;
    }

    Native::Log("coop: player 1's control given back after player 2's twin died");
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

bool NativeCoop::KeyboardFirst()
{
    return g_KeyboardFirst || std::getenv("TWIN_COOP_KEYBOARD") != nullptr;
}

void NativeCoop::SetKeyboardFirst(bool on)
{
    g_KeyboardFirst = on;
}

bool NativeCoop::IsTwin(const InstanceContext* instance)
{
    return instance != nullptr && instance == Partner() && g_PartnerIsTwin;
}

NativeCoop::TwinAsPlayer::TwinAsPlayer(const void* originator)
{
    InstanceContext* twin = Partner();
    if (originator == nullptr || twin == nullptr || originator != twin || !g_PartnerIsTwin)
    {
        return;
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

    // The latest checkpoint set (the respawn one, else the saved one, else the start's): the twin comes back where player 1 would
    // (a checkpoint in a chunk that isn't loaded lets the twin go; it's made again beside player 1)
    const Checkpoint* checkpoint = nullptr;
    for (u32 slot : {GameProgress::CheckpointRespawn, GameProgress::CheckpointSaved, GameProgress::CheckpointStart})
    {
        Checkpoint* candidate = progress.checkpoints[slot];
        if (candidate != nullptr && candidate->bits.set != 0)
        {
            checkpoint = candidate;
            break;
        }
    }

    if (CharacterAgent* agent = CharacterAgentOf(instance))
    {
        agent->velocity = {0.0f, 0.0f, 0.0f, 1.0f};
    }

    g_TwinDied = true;
    g_TwinDiedFrame = g_Frames;
    if (checkpoint != nullptr)
    {
        InstancePlacement placement;
        CopyInstancePlacement(&placement, &checkpoint->character);
        placement.position.x += SpawnBeside;
        placement.Apply(instance);
        QueueObject(instance);
        Native::Log("coop: player 2's twin died: back at the checkpoint (%.1f, %.1f, %.1f)", placement.position.x,
                    placement.position.y, placement.position.z);
        return true;
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

    Native::Log("coop: player 2's twin died, no checkpoint: back beside player 1");
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
    KeepPlayerOneInControl(controller, first);
    u32 which = GameProgress::NoCharacter;
    InstanceContext* second = SecondCharacter(progress, first, &which);
    // The partner made for player 2 (a twin isn't in the progress's characters)
    if (second == nullptr && Partner() != nullptr && Partner() != first)
    {
        second = Partner();
        which = progress.play.character;
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
}
