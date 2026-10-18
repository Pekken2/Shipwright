// Roguelike Descent
// -----------------
// A room-by-room roguelike mode for Ship of Harkinian.
//
// How a run works:
//  * "Start Run" (Enhancements > Extra Modes) snapshots the loaded save, then strips Link down to an adult with the
//    Master Sword, Hylian Shield and a few hearts.
//  * Every floor, Link is dropped straight into a random room of a random dungeon. Doors are sealed.
//  * Kill every enemy in the room (the room's own plus extra waves that scale with depth) to descend.
//  * Rooms heal you a little; every few floors you get a random item.
//  * Dying ends the run. The snapshot is restored and Link goes back to where the run started.
//  * Saving (manual and autosave) is blocked while a run is active, so the real save file is never touched.
//
// The room drop uses the engine's own void-out respawn system: while the target dungeon scene is being set up (after
// its scene commands run, before its first room loads) we pick a room from the scene's transition actor (door) list,
// compute a standing spot just inside that door, and point the "void out" respawn data at it. The game then loads
// that room and spawns Link there, in a single load.

#include "RoguelikeDescent.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include "soh/SohGui/ImGuiUtils.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_ROGUELIKE(name) CVAR_ENHANCEMENT("Roguelike." name)

#define ROGUELIKE_START_HEARTS CVarGetInteger(CVAR_ROGUELIKE("StartHearts"), 3)
#define ROGUELIKE_ROOM_TIME_LIMIT CVarGetInteger(CVAR_ROGUELIKE("RoomTimeLimit"), 120)
#define ROGUELIKE_REWARD_EVERY CVarGetInteger(CVAR_ROGUELIKE("RewardEvery"), 2)
#define ROGUELIKE_AUTO_RESTART CVarGetInteger(CVAR_ROGUELIKE("AutoRestart"), 0)
#define ROGUELIKE_BEST_FLOOR_CVAR CVAR_ROGUELIKE("BestFloor")

// The game logic runs at 20 updates per second.
static constexpr int FRAMES_PER_SECOND = 20;
static constexpr int WAVE_SPAWN_DELAY = 25;  // frames after landing before the first wave appears
static constexpr int CLEAR_CHECK_DELAY = 45; // frames after landing before a room can count as cleared
static constexpr int DESCEND_DELAY = 3 * FRAMES_PER_SECOND;
static constexpr int RUN_OVER_DELAY = 3 * FRAMES_PER_SECOND;

// ---------------------------------------------------------------------------------------------------------------------
// Data tables
// ---------------------------------------------------------------------------------------------------------------------

typedef struct {
    s16 sceneNum;
    s32 entrance;
    const char* name;
    u8 tunic; // EQUIP_VALUE_TUNIC_*
} RoguelikeDungeon;

// Ordered roughly from easiest to hardest. Deeper floors unlock more of the list.
static const RoguelikeDungeon sDungeons[] = {
    { SCENE_DEKU_TREE, ENTR_DEKU_TREE_ENTRANCE, "Inside the Deku Tree", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_DODONGOS_CAVERN, ENTR_DODONGOS_CAVERN_ENTRANCE, "Dodongo's Cavern", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_JABU_JABU, ENTR_JABU_JABU_ENTRANCE, "Inside Jabu-Jabu's Belly", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_BOTTOM_OF_THE_WELL, ENTR_BOTTOM_OF_THE_WELL_ENTRANCE, "Bottom of the Well", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_FOREST_TEMPLE, ENTR_FOREST_TEMPLE_ENTRANCE, "Forest Temple", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_ICE_CAVERN, ENTR_ICE_CAVERN_ENTRANCE, "Ice Cavern", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_FIRE_TEMPLE, ENTR_FIRE_TEMPLE_ENTRANCE, "Fire Temple", EQUIP_VALUE_TUNIC_GORON },
    { SCENE_WATER_TEMPLE, ENTR_WATER_TEMPLE_ENTRANCE, "Water Temple", EQUIP_VALUE_TUNIC_ZORA },
    { SCENE_SHADOW_TEMPLE, ENTR_SHADOW_TEMPLE_ENTRANCE, "Shadow Temple", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_SPIRIT_TEMPLE, ENTR_SPIRIT_TEMPLE_ENTRANCE, "Spirit Temple", EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_GERUDO_TRAINING_GROUND, ENTR_GERUDO_TRAINING_GROUND_ENTRANCE, "Gerudo Training Ground",
      EQUIP_VALUE_TUNIC_KOKIRI },
    { SCENE_INSIDE_GANONS_CASTLE, ENTR_INSIDE_GANONS_CASTLE_ENTRANCE, "Inside Ganon's Castle",
      EQUIP_VALUE_TUNIC_KOKIRI },
};

typedef struct {
    s16 actorId;
    s16 params;
    bool flying;
} RoguelikeEnemy;

// Enemy pools by tier. All of these can be beaten with just a sword and are known to work when spawned outside of
// their usual rooms (they're used by the Enemy Randomizer too).
static const std::vector<RoguelikeEnemy> sEnemyTiers[] = {
    // Tier 1
    {
        { ACTOR_EN_FIREFLY, 2, true },  // Keese
        { ACTOR_EN_TITE, -1, false },   // Red Tektite
        { ACTOR_EN_ST, 0, false },      // Skulltula
        { ACTOR_EN_SKB, 1, false },     // Stalchild (small)
        { ACTOR_EN_DODOJR, 0, false },  // Baby Dodongo
        { ACTOR_EN_DEKUBABA, 1, false } // Deku Baba (large)
    },
    // Tier 2
    {
        { ACTOR_EN_FIREFLY, 1, true },   // Fire Keese
        { ACTOR_EN_TITE, -2, false },    // Blue Tektite
        { ACTOR_EN_WF, 0, false },       // Wolfos
        { ACTOR_EN_ST, 1, false },       // Big Skulltula
        { ACTOR_EN_DODONGO, -1, false }, // Dodongo
        { ACTOR_EN_ZF, -1, false },      // Lizalfos
        { ACTOR_EN_RR, 0, false },       // Like-Like
    },
    // Tier 3
    {
        { ACTOR_EN_FIREFLY, 4, true },   // Ice Keese
        { ACTOR_EN_TEST, 2, false },     // Stalfos
        { ACTOR_EN_ZF, -2, false },      // Dinolfos
        { ACTOR_EN_WF, 1, false },       // White Wolfos
        { ACTOR_EN_RD, 1, false },       // Redead
        { ACTOR_EN_RD, 32766, false },   // Gibdo
        { ACTOR_EN_FLOORMAS, 0, false }, // Floormaster
    },
    // Tier 4
    {
        { ACTOR_EN_IK, 3, false },    // Iron Knuckle (white)
        { ACTOR_EN_GELDB, 0, false }, // Gerudo Thief
        { ACTOR_EN_TEST, 2, false },  // Stalfos
        { ACTOR_EN_ZF, -2, false },   // Dinolfos
        { ACTOR_EN_IK, 2, false },    // Iron Knuckle (black)
    },
};
static constexpr int NUM_ENEMY_TIERS = ARRAY_COUNT(sEnemyTiers);

typedef enum {
    REWARD_HEART_CONTAINER,
    REWARD_BOW,
    REWARD_HOOKSHOT,
    REWARD_LONGSHOT,
    REWARD_BOMB_BAG,
    REWARD_HAMMER,
    REWARD_MAGIC,
    REWARD_DOUBLE_MAGIC,
    REWARD_DINS_FIRE,
    REWARD_NAYRUS_LOVE,
    REWARD_FIRE_ARROWS,
    REWARD_ICE_ARROWS,
    REWARD_LIGHT_ARROWS,
    REWARD_MIRROR_SHIELD,
    REWARD_BIGGORON_SWORD,
    REWARD_HOVER_BOOTS,
    REWARD_DOUBLE_DEFENSE,
    REWARD_FAIRY_BOTTLE,
    REWARD_MAX,
} RoguelikeReward;

// ---------------------------------------------------------------------------------------------------------------------
// Run state
// ---------------------------------------------------------------------------------------------------------------------

typedef enum {
    ROOM_STATE_ENTERING, // just landed, waiting to spawn the first wave
    ROOM_STATE_FIGHTING, // enemies are alive
    ROOM_STATE_CLEARED,  // counting down to the next floor
    ROOM_STATE_RUN_OVER, // Link died, counting down to the end of the run
} RoomState;

typedef struct {
    s32 entranceIndex;
    s8 roomIndex;
    Vec3f pos;
    s16 yaw;
} ReturnPoint;

static struct {
    bool active;
    int floor;
    int lastDungeon;
    s16 lastScene;
    s8 lastRoom;
    RoomState roomState;
    int roomFrames;
    int stateFrames;
    int wavesLeft;
} sRun;

static SaveContext sSnapshot;
static ReturnPoint sReturnPoint;

// Requests coming from the menu (which runs outside of the game update) are handled on the next player update.
static bool sStartRequested = false;
static bool sAbandonRequested = false;

// Set while we are steering a scene load so the respawn data points at our chosen room.
static bool sPendingRoomPick = false;
static s16 sPendingScene = -1;
// Set while a load we started with the respawn system is in progress, so Link doesn't take void-out damage.
static bool sSuppressVoidDamage = false;

static std::mt19937 sRng{ std::random_device{}() };

static int RandInt(int minInclusive, int maxInclusive) {
    std::uniform_int_distribution<int> dist(minInclusive, maxInclusive);
    return dist(sRng);
}

static float RandFloat(float min, float max) {
    std::uniform_real_distribution<float> dist(min, max);
    return dist(sRng);
}

static void Notify(const std::string& message, const std::string& suffix = "", const char* icon = nullptr) {
    Notification::Emit({
        .itemIcon = icon,
        .message = message,
        .suffix = suffix,
        .remainingTime = 6.0f,
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Room picking
// ---------------------------------------------------------------------------------------------------------------------

typedef struct {
    s8 room;
    Vec3f pos;
    s16 yaw;
} RoomSpot;

static bool IsUsableTransitionActor(const TransitionActorEntry* entry) {
    s16 id = entry->id < 0 ? -entry->id : entry->id;
    id &= 0x1FFF;

    switch (id) {
        case ACTOR_EN_DOOR:
        case ACTOR_DOOR_SHUTTER:
        case ACTOR_DOOR_KILLER:
            return true;
        case ACTOR_EN_HOLL: {
            // Only the horizontal (wall) room-switch planes. The vertical ones are for drops between floors.
            s32 type = (entry->params >> 6) & 7;
            return type == 0 || type == 3 || type == 4 || type == 6;
        }
        default:
            return false;
    }
}

// Collects a standing spot just inside every room that a door or room-switch plane leads into.
// Side 0 of a transition actor is the room on its local -Z side, side 1 is the room on its local +Z side
// (see EnDoor / EnHoll / Player door handling).
static std::vector<RoomSpot> CollectRoomSpots(PlayState* play) {
    std::vector<RoomSpot> spots;

    for (s32 i = 0; i < play->transiActorCtx.numActors; i++) {
        const TransitionActorEntry* entry = &play->transiActorCtx.list[i];
        if (!IsUsableTransitionActor(entry) || entry->sides[0].room == entry->sides[1].room) {
            continue;
        }

        f32 fwdX = Math_SinS(entry->rotY);
        f32 fwdZ = Math_CosS(entry->rotY);

        for (s32 side = 0; side < 2; side++) {
            s8 room = entry->sides[side].room;
            if (room < 0 || room >= play->numRooms) {
                continue;
            }

            const f32 sign = (side == 0) ? -1.0f : 1.0f;
            static const f32 sDistances[] = { 70.0f, 110.0f, 50.0f };

            for (f32 dist : sDistances) {
                Vec3f probe;
                probe.x = entry->pos.x + fwdX * dist * sign;
                probe.y = entry->pos.y + 60.0f;
                probe.z = entry->pos.z + fwdZ * dist * sign;

                CollisionPoly* floorPoly = NULL;
                f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &floorPoly, &probe);
                if (floorPoly == NULL || floorY <= BGCHECK_Y_MIN + 1.0f || fabsf(floorY - entry->pos.y) > 60.0f) {
                    continue;
                }

                RoomSpot spot;
                spot.room = room;
                spot.pos = { probe.x, floorY, probe.z };
                // Face away from the door, into the room.
                spot.yaw = (side == 0) ? (s16)(entry->rotY + 0x8000) : entry->rotY;
                spots.push_back(spot);
                break;
            }
        }
    }

    return spots;
}

// Runs right after the scene's commands are processed, before its first room is loaded.
static void PickRoomForPendingScene(int16_t sceneNum) {
    if (!sPendingRoomPick || gPlayState == NULL) {
        return;
    }
    sPendingRoomPick = false;

    if (sceneNum != sPendingScene) {
        return;
    }

    std::vector<RoomSpot> spots = CollectRoomSpots(gPlayState);
    if (spots.empty()) {
        // No usable doors. Link simply starts at the dungeon entrance.
        return;
    }

    // Pick a room uniformly (rooms with many doors shouldn't be favoured), avoiding an immediate repeat.
    std::vector<s8> rooms;
    for (const RoomSpot& spot : spots) {
        if (std::find(rooms.begin(), rooms.end(), spot.room) == rooms.end()) {
            rooms.push_back(spot.room);
        }
    }
    if (rooms.size() > 1 && sceneNum == sRun.lastScene) {
        rooms.erase(std::remove(rooms.begin(), rooms.end(), sRun.lastRoom), rooms.end());
    }
    s8 room = rooms[RandInt(0, (int)rooms.size() - 1)];

    std::vector<RoomSpot> roomSpots;
    for (const RoomSpot& spot : spots) {
        if (spot.room == room) {
            roomSpots.push_back(spot);
        }
    }
    const RoomSpot& spot = roomSpots[RandInt(0, (int)roomSpots.size() - 1)];

    RespawnData* respawn = &gSaveContext.respawn[RESPAWN_MODE_DOWN];
    respawn->entranceIndex = gSaveContext.entranceIndex;
    respawn->roomIndex = spot.room;
    respawn->pos = spot.pos;
    respawn->yaw = spot.yaw;
    respawn->playerParams = 0xDFF; // PLAYER_START_MODE_IDLE, no forced camera
    respawn->tempSwchFlags = 0;
    respawn->tempCollectFlags = 0;
    gSaveContext.respawnFlag = 1;
    sSuppressVoidDamage = true;

    sRun.lastScene = sceneNum;
    sRun.lastRoom = spot.room;
}

// ---------------------------------------------------------------------------------------------------------------------
// Inventory helpers
// ---------------------------------------------------------------------------------------------------------------------

static void SetEquipValue(EquipmentType type, u16 value) {
    gSaveContext.equips.equipment &= ~gEquipMasks[type];
    gSaveContext.equips.equipment |= value << gEquipShifts[type];
}

static void GiveOwnedEquip(EquipmentType type, u16 invIndex) {
    gSaveContext.inventory.equipment |= OWNED_EQUIP_FLAG(type, invIndex);
}

static bool HasItem(u8 item) {
    return INV_CONTENT(item) == item;
}

static bool HasBottledFairy() {
    for (s32 slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4; slot++) {
        if (gSaveContext.inventory.items[slot] == ITEM_FAIRY) {
            return true;
        }
    }
    return false;
}

// Puts a newly found item on the first free C button so it can be used right away.
static void AssignToFreeCButton(PlayState* play, u8 item) {
    // Upgrading the Hookshot to the Longshot: swap it in place.
    for (s32 button = 1; button <= 3; button++) {
        if (item == ITEM_LONGSHOT && gSaveContext.equips.buttonItems[button] == ITEM_HOOKSHOT) {
            gSaveContext.equips.buttonItems[button] = ITEM_LONGSHOT;
            Interface_LoadItemIcon1(play, button);
            return;
        }
    }
    for (s32 button = 1; button <= 3; button++) {
        if (gSaveContext.equips.buttonItems[button] == item) {
            return;
        }
    }
    for (s32 button = 1; button <= 3; button++) {
        if (gSaveContext.equips.buttonItems[button] == ITEM_NONE) {
            gSaveContext.equips.buttonItems[button] = item;
            gSaveContext.equips.cButtonSlots[button - 1] = SLOT(item);
            Interface_LoadItemIcon1(play, button);
            return;
        }
    }
}

static void GiveMagic(PlayState* play, bool doubleMagic) {
    gSaveContext.isMagicAcquired = true;
    if (doubleMagic) {
        gSaveContext.isDoubleMagicAcquired = true;
        gSaveContext.magicFillTarget = MAGIC_DOUBLE_METER;
        gSaveContext.magicLevel = 0;
    } else {
        gSaveContext.magicFillTarget = MAGIC_NORMAL_METER;
    }
    Magic_Fill(play);
}

static bool IsRewardAvailable(RoguelikeReward reward) {
    const bool hasMagic = gSaveContext.isMagicAcquired;
    const bool hasBow = HasItem(ITEM_BOW);

    switch (reward) {
        case REWARD_HEART_CONTAINER:
            return gSaveContext.healthCapacity < 20 * 16;
        case REWARD_BOW:
            return !hasBow;
        case REWARD_HOOKSHOT:
            return INV_CONTENT(ITEM_HOOKSHOT) == ITEM_NONE;
        case REWARD_LONGSHOT:
            return INV_CONTENT(ITEM_HOOKSHOT) == ITEM_HOOKSHOT;
        case REWARD_BOMB_BAG:
            return !HasItem(ITEM_BOMB);
        case REWARD_HAMMER:
            return !HasItem(ITEM_HAMMER);
        case REWARD_MAGIC:
            return !hasMagic;
        case REWARD_DOUBLE_MAGIC:
            return hasMagic && !gSaveContext.isDoubleMagicAcquired;
        case REWARD_DINS_FIRE:
            return hasMagic && !HasItem(ITEM_DINS_FIRE);
        case REWARD_NAYRUS_LOVE:
            return hasMagic && !HasItem(ITEM_NAYRUS_LOVE);
        case REWARD_FIRE_ARROWS:
            return hasMagic && hasBow && !HasItem(ITEM_ARROW_FIRE);
        case REWARD_ICE_ARROWS:
            return hasMagic && hasBow && !HasItem(ITEM_ARROW_ICE);
        case REWARD_LIGHT_ARROWS:
            return hasMagic && hasBow && !HasItem(ITEM_ARROW_LIGHT);
        case REWARD_MIRROR_SHIELD:
            return !CHECK_OWNED_EQUIP(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_MIRROR);
        case REWARD_BIGGORON_SWORD:
            return !CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BIGGORON);
        case REWARD_HOVER_BOOTS:
            return !CHECK_OWNED_EQUIP(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_HOVER);
        case REWARD_DOUBLE_DEFENSE:
            return !gSaveContext.isDoubleDefenseAcquired;
        case REWARD_FAIRY_BOTTLE:
            return gSaveContext.inventory.items[SLOT_BOTTLE_4] == ITEM_NONE;
        default:
            return false;
    }
}

static void GiveReward(PlayState* play, RoguelikeReward reward) {
    Player* player = GET_PLAYER(play);
    const char* name = "";
    u32 icon = ITEM_NONE;

    switch (reward) {
        case REWARD_HEART_CONTAINER:
            Item_Give(play, ITEM_HEART_CONTAINER);
            name = "Heart Container";
            icon = ITEM_HEART_CONTAINER;
            break;
        case REWARD_BOW:
            Item_Give(play, ITEM_BOW);
            AssignToFreeCButton(play, ITEM_BOW);
            name = "Fairy Bow";
            icon = ITEM_BOW;
            break;
        case REWARD_HOOKSHOT:
            Item_Give(play, ITEM_HOOKSHOT);
            AssignToFreeCButton(play, ITEM_HOOKSHOT);
            name = "Hookshot";
            icon = ITEM_HOOKSHOT;
            break;
        case REWARD_LONGSHOT:
            Item_Give(play, ITEM_LONGSHOT);
            AssignToFreeCButton(play, ITEM_LONGSHOT);
            name = "Longshot";
            icon = ITEM_LONGSHOT;
            break;
        case REWARD_BOMB_BAG:
            Item_Give(play, ITEM_BOMB_BAG_20);
            AssignToFreeCButton(play, ITEM_BOMB);
            name = "Bomb Bag";
            icon = ITEM_BOMB;
            break;
        case REWARD_HAMMER:
            Item_Give(play, ITEM_HAMMER);
            AssignToFreeCButton(play, ITEM_HAMMER);
            name = "Megaton Hammer";
            icon = ITEM_HAMMER;
            break;
        case REWARD_MAGIC:
            GiveMagic(play, false);
            name = "Magic Meter";
            icon = ITEM_MAGIC_SMALL;
            break;
        case REWARD_DOUBLE_MAGIC:
            GiveMagic(play, true);
            name = "Double Magic";
            icon = ITEM_MAGIC_LARGE;
            break;
        case REWARD_DINS_FIRE:
            Item_Give(play, ITEM_DINS_FIRE);
            AssignToFreeCButton(play, ITEM_DINS_FIRE);
            name = "Din's Fire";
            icon = ITEM_DINS_FIRE;
            break;
        case REWARD_NAYRUS_LOVE:
            Item_Give(play, ITEM_NAYRUS_LOVE);
            AssignToFreeCButton(play, ITEM_NAYRUS_LOVE);
            name = "Nayru's Love";
            icon = ITEM_NAYRUS_LOVE;
            break;
        case REWARD_FIRE_ARROWS:
            Item_Give(play, ITEM_ARROW_FIRE);
            AssignToFreeCButton(play, ITEM_ARROW_FIRE);
            name = "Fire Arrows";
            icon = ITEM_ARROW_FIRE;
            break;
        case REWARD_ICE_ARROWS:
            Item_Give(play, ITEM_ARROW_ICE);
            AssignToFreeCButton(play, ITEM_ARROW_ICE);
            name = "Ice Arrows";
            icon = ITEM_ARROW_ICE;
            break;
        case REWARD_LIGHT_ARROWS:
            Item_Give(play, ITEM_ARROW_LIGHT);
            AssignToFreeCButton(play, ITEM_ARROW_LIGHT);
            name = "Light Arrows";
            icon = ITEM_ARROW_LIGHT;
            break;
        case REWARD_MIRROR_SHIELD:
            Item_Give(play, ITEM_SHIELD_MIRROR);
            SetEquipValue(EQUIP_TYPE_SHIELD, EQUIP_VALUE_SHIELD_MIRROR);
            Player_SetEquipmentData(play, player);
            name = "Mirror Shield";
            icon = ITEM_SHIELD_MIRROR;
            break;
        case REWARD_BIGGORON_SWORD:
            Item_Give(play, ITEM_SWORD_BGS);
            gSaveContext.bgsFlag = true;
            gSaveContext.swordHealth = 8;
            gSaveContext.equips.buttonItems[0] = ITEM_SWORD_BGS;
            SetEquipValue(EQUIP_TYPE_SWORD, EQUIP_VALUE_SWORD_BIGGORON);
            Interface_LoadItemIcon1(play, 0);
            Player_SetEquipmentData(play, player);
            name = "Biggoron's Sword";
            icon = ITEM_SWORD_BGS;
            break;
        case REWARD_HOVER_BOOTS:
            Item_Give(play, ITEM_BOOTS_HOVER);
            name = "Hover Boots (equip from the pause menu)";
            icon = ITEM_BOOTS_HOVER;
            break;
        case REWARD_DOUBLE_DEFENSE:
            gSaveContext.isDoubleDefenseAcquired = true;
            gSaveContext.inventory.defenseHearts = 20;
            gSaveContext.healthAccumulator = 20 * 16;
            name = "Double Defense";
            icon = ITEM_DOUBLE_DEFENSE;
            break;
        case REWARD_FAIRY_BOTTLE:
            Item_Give(play, ITEM_BOTTLE);
            Item_Give(play, ITEM_FAIRY);
            name = "Bottled Fairy (saves you from one death)";
            icon = ITEM_FAIRY;
            break;
        default:
            return;
    }

    Audio_PlayFanfare(NA_BGM_ITEM_GET | 0x900);
    Notify("Reward: ", name, GetTextureForItemId(icon));
}

static void GiveRandomReward(PlayState* play) {
    std::vector<RoguelikeReward> pool;
    for (int reward = 0; reward < REWARD_MAX; reward++) {
        if (IsRewardAvailable((RoguelikeReward)reward)) {
            pool.push_back((RoguelikeReward)reward);
        }
    }
    if (pool.empty()) {
        // Everything collected already: a full heal instead.
        gSaveContext.healthAccumulator = gSaveContext.healthCapacity;
        Notify("Reward: ", "Full heal");
        return;
    }
    GiveReward(play, pool[RandInt(0, (int)pool.size() - 1)]);
}

// Small heal and ammo top-up after every cleared room.
static void GiveRoomClearRefill(PlayState* play) {
    Health_ChangeBy(play, 16);
    if (HasItem(ITEM_BOW)) {
        Item_Give(play, ITEM_ARROWS_MEDIUM);
    }
    if (HasItem(ITEM_BOMB)) {
        Item_Give(play, ITEM_BOMBS_5);
    }
    if (gSaveContext.isMagicAcquired) {
        Magic_Fill(play);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Enemy waves
// ---------------------------------------------------------------------------------------------------------------------

static int WavesForFloor(int floor) {
    if (floor >= 12) {
        return 3;
    }
    if (floor >= 5) {
        return 2;
    }
    return 1;
}

static int EnemiesPerWave(int floor) {
    return std::min(2 + floor / 3, 6);
}

static int MaxTierForFloor(int floor) {
    return std::min(1 + (floor - 1) / 4, NUM_ENEMY_TIERS);
}

// Finds a spot on the same floor as Link with no wall in between, so the enemy is reachable and in this room.
static bool FindEnemySpawnSpot(PlayState* play, Vec3f* outPos) {
    Player* player = GET_PLAYER(play);
    Vec3f origin = player->actor.world.pos;
    origin.y += 30.0f;

    for (int attempt = 0; attempt < 16; attempt++) {
        f32 angle = RandFloat(0.0f, 6.2831853f);
        f32 radius = RandFloat(150.0f, 380.0f);

        Vec3f target = { origin.x + sinf(angle) * radius, origin.y, origin.z + cosf(angle) * radius };
        Vec3f hit;
        CollisionPoly* wallPoly = NULL;
        s32 bgId;
        if (BgCheck_EntityLineTest1(&play->colCtx, &origin, &target, &hit, &wallPoly, true, false, false, true,
                                    &bgId)) {
            continue; // a wall is in the way
        }

        Vec3f probe = target;
        probe.y += 50.0f;
        CollisionPoly* floorPoly = NULL;
        f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &floorPoly, &probe);
        if (floorPoly == NULL || floorY <= BGCHECK_Y_MIN + 1.0f || fabsf(floorY - player->actor.world.pos.y) > 100.0f) {
            continue; // no floor, or a pit / ledge
        }

        *outPos = { target.x, floorY, target.z };
        return true;
    }
    return false;
}

static void SpawnWave(PlayState* play) {
    Player* player = GET_PLAYER(play);
    int maxTier = MaxTierForFloor(sRun.floor);
    int count = EnemiesPerWave(sRun.floor);

    for (int i = 0; i < count; i++) {
        int tier = RandInt(std::max(1, maxTier - 1), maxTier) - 1;
        const std::vector<RoguelikeEnemy>& pool = sEnemyTiers[tier];
        const RoguelikeEnemy& enemy = pool[RandInt(0, (int)pool.size() - 1)];

        Vec3f pos;
        if (!FindEnemySpawnSpot(play, &pos)) {
            continue;
        }
        if (enemy.flying) {
            pos.y += 60.0f;
        }

        s16 yaw = Math_Vec3f_Yaw(&pos, &player->actor.world.pos);
        Actor_Spawn(&play->actorCtx, play, enemy.actorId, pos.x, pos.y, pos.z, 0, yaw, 0, enemy.params);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Run flow
// ---------------------------------------------------------------------------------------------------------------------

static void TriggerTransition(PlayState* play, s32 entrance) {
    gSaveContext.cutsceneIndex = 0;
    gSaveContext.nextCutsceneIndex = 0xFFEF;
    play->nextEntranceIndex = entrance;
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_FADE_BLACK;
}

// Picks the next floor's dungeon and sets everything up for the room drop. Returns the entrance to load.
static s32 PrepareNextFloor() {
    sRun.floor++;

    int unlocked = std::min((int)ARRAY_COUNT(sDungeons), 4 + ((sRun.floor - 1) / 3) * 2);
    int dungeon = RandInt(0, unlocked - 1);
    const RoguelikeDungeon& d = sDungeons[dungeon];

    // Wear the right tunic for the heat / water.
    GiveOwnedEquip(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_GORON);
    GiveOwnedEquip(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_ZORA);
    SetEquipValue(EQUIP_TYPE_TUNIC, d.tunic);

    sPendingRoomPick = true;
    sPendingScene = d.sceneNum;
    sRun.lastDungeon = dungeon;

    Notify("Floor " + std::to_string(sRun.floor) + ": ", d.name);
    return d.entrance;
}

static void GoToNextFloor(PlayState* play) {
    TriggerTransition(play, PrepareNextFloor());
}

static void ApplyStartingLoadout() {
    gSaveContext.linkAge = LINK_AGE_ADULT;

    for (s32 i = 0; i < ARRAY_COUNT(gSaveContext.inventory.items); i++) {
        gSaveContext.inventory.items[i] = ITEM_NONE;
    }
    for (s32 i = 0; i < ARRAY_COUNT(gSaveContext.inventory.ammo); i++) {
        gSaveContext.inventory.ammo[i] = 0;
    }
    gSaveContext.inventory.upgrades = 0;
    gSaveContext.inventory.equipment = 0;
    gSaveContext.inventory.defenseHearts = 0;

    GiveOwnedEquip(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_MASTER);
    GiveOwnedEquip(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_HYLIAN);
    GiveOwnedEquip(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_KOKIRI);
    GiveOwnedEquip(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_KOKIRI);

    for (s32 i = 0; i < ARRAY_COUNT(gSaveContext.equips.buttonItems); i++) {
        gSaveContext.equips.buttonItems[i] = ITEM_NONE;
    }
    for (s32 i = 0; i < ARRAY_COUNT(gSaveContext.equips.cButtonSlots); i++) {
        gSaveContext.equips.cButtonSlots[i] = SLOT_NONE;
    }
    gSaveContext.equips.buttonItems[0] = ITEM_SWORD_MASTER;
    gSaveContext.equips.equipment = 0;
    SetEquipValue(EQUIP_TYPE_SWORD, EQUIP_VALUE_SWORD_MASTER);
    SetEquipValue(EQUIP_TYPE_SHIELD, EQUIP_VALUE_SHIELD_HYLIAN);
    SetEquipValue(EQUIP_TYPE_TUNIC, EQUIP_VALUE_TUNIC_KOKIRI);
    SetEquipValue(EQUIP_TYPE_BOOTS, EQUIP_VALUE_BOOTS_KOKIRI);
    gSaveContext.adultEquips = gSaveContext.equips;

    s16 hearts = (s16)std::clamp(ROGUELIKE_START_HEARTS, 1, 20);
    gSaveContext.healthCapacity = hearts * 16;
    gSaveContext.health = hearts * 16;
    gSaveContext.healthAccumulator = 0;

    gSaveContext.isMagicAcquired = false;
    gSaveContext.isDoubleMagicAcquired = false;
    gSaveContext.magicLevel = 0;
    gSaveContext.magic = 0;
    gSaveContext.magicCapacity = 0;
    gSaveContext.magicFillTarget = 0;
    gSaveContext.magicTarget = 0;
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    gSaveContext.isDoubleDefenseAcquired = false;
    gSaveContext.bgsFlag = false;
    gSaveContext.rupees = 0;
    gSaveContext.nayrusLoveTimer = 0;
}

static void ResetRunState() {
    sRun.active = true;
    sRun.floor = 0;
    sRun.lastDungeon = 0;
    sRun.lastScene = -1;
    sRun.lastRoom = -1;
    sRun.roomState = ROOM_STATE_ENTERING;
    sRun.roomFrames = 0;
    sRun.stateFrames = 0;
    sRun.wavesLeft = 0;
}

static void StartRun(PlayState* play) {
    Player* player = GET_PLAYER(play);

    // Remember everything about the real save and where Link is standing.
    if (!sRun.active) {
        memcpy(&sSnapshot, &gSaveContext, sizeof(SaveContext));
        sReturnPoint.entranceIndex = gSaveContext.entranceIndex;
        sReturnPoint.roomIndex = play->roomCtx.curRoom.num;
        sReturnPoint.pos = player->actor.world.pos;
        sReturnPoint.yaw = player->actor.shape.rot.y;
    }

    ResetRunState();
    ApplyStartingLoadout();
    Notify("Roguelike run started. ", "Clear each room to descend!");
    GoToNextFloor(play);
}

// Roguelike Descent files (picked on the File Select quest screen) start a run straight from the file load:
// no intro cutscene, and the first load goes directly into the first room.
static void StartRunFromFileLoad() {
    ResetRunState();
    ApplyStartingLoadout();
    // The file select screen has already set up the magic meter for the file's saved state; clear that out too.
    gSaveContext.magicFillTarget = 0;
    gSaveContext.magicCapacity = 0;

    gSaveContext.entranceIndex = PrepareNextFloor();
    gSaveContext.cutsceneIndex = 0;
    gSaveContext.nextCutsceneIndex = 0xFFEF;
    gSaveContext.respawnFlag = 0;
    gSaveContext.dayTime = 0x8000; // noon
    gSaveContext.nightFlag = 0;
}

static void SaveBestFloor(int floor) {
    if (floor > CVarGetInteger(ROGUELIKE_BEST_FLOOR_CVAR, 0)) {
        CVarSetInteger(ROGUELIKE_BEST_FLOOR_CVAR, floor);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
}

// Puts the real save back and returns Link to where the run started.
static void EndRun(PlayState* play) {
    // The scene we're leaving writes its flags back into the save when it unloads. Point them at the snapshot's
    // values so the run can't leave any trace in the real save's dungeon flags.
    const SavedSceneFlags* saved = &sSnapshot.sceneFlags[play->sceneNum];
    play->actorCtx.flags.chest = saved->chest;
    play->actorCtx.flags.swch = saved->swch;
    play->actorCtx.flags.clear = saved->clear;
    play->actorCtx.flags.collect = saved->collect;

    // Restore the persistent part of the save (everything before fileNum), plus the live magic meter state.
    memcpy(&gSaveContext, &sSnapshot, offsetof(SaveContext, fileNum));
    gSaveContext.magicCapacity = sSnapshot.magicCapacity;
    gSaveContext.magicFillTarget = sSnapshot.magicFillTarget;
    gSaveContext.magicTarget = sSnapshot.magicTarget;
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    gSaveContext.nayrusLoveTimer = 0;
    gSaveContext.healthAccumulator = 0;

    RespawnData* respawn = &gSaveContext.respawn[RESPAWN_MODE_DOWN];
    respawn->entranceIndex = sReturnPoint.entranceIndex;
    respawn->roomIndex = sReturnPoint.roomIndex;
    respawn->pos = sReturnPoint.pos;
    respawn->yaw = sReturnPoint.yaw;
    respawn->playerParams = 0xDFF;
    respawn->tempSwchFlags = 0;
    respawn->tempCollectFlags = 0;
    gSaveContext.respawnFlag = 1;
    sSuppressVoidDamage = true;

    sRun.active = false;
    sPendingRoomPick = false;
    TriggerTransition(play, sReturnPoint.entranceIndex);
}

static void OnRunOver(PlayState* play) {
    int reached = sRun.floor;
    int previousBest = CVarGetInteger(ROGUELIKE_BEST_FLOOR_CVAR, 0);
    SaveBestFloor(reached);

    std::string result = "Reached floor " + std::to_string(reached);
    if (reached > previousBest) {
        result += " (new best!)";
    } else {
        result += " (best: " + std::to_string(previousBest) + ")";
    }
    Notify("Run over. ", result);

    // Roguelike Descent files have no "real" save to go back to, so they always roll straight into a new run.
    if (IS_ROGUELIKE || ROGUELIKE_AUTO_RESTART) {
        StartRun(play);
    } else {
        EndRun(play);
    }
}

static void EnterRoomState(RoomState state) {
    sRun.roomState = state;
    sRun.stateFrames = 0;
}

static void OnRoomCleared(PlayState* play, bool timedOut) {
    EnterRoomState(ROOM_STATE_CLEARED);
    SaveBestFloor(sRun.floor);

    if (timedOut) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        Notify("Out of time. ", "The floor gives way...");
        return;
    }

    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    GiveRoomClearRefill(play);

    int every = std::max(1, ROGUELIKE_REWARD_EVERY);
    if (sRun.floor % every == 0) {
        GiveRandomReward(play);
    } else {
        Notify("Floor " + std::to_string(sRun.floor) + " cleared!");
    }
}

static void OnPlayerUpdateRoguelike() {
    PlayState* play = gPlayState;
    if (play == NULL) {
        return;
    }
    if (sStartRequested) {
        sStartRequested = false;
        if (play->transitionTrigger == TRANS_TRIGGER_OFF) {
            StartRun(play);
        }
        return;
    }

    if (!sRun.active) {
        sAbandonRequested = false;
        return;
    }

    if (sAbandonRequested) {
        sAbandonRequested = false;
        if (play->transitionTrigger == TRANS_TRIGGER_OFF) {
            SaveBestFloor(sRun.floor);
            Notify("Run abandoned on floor ", std::to_string(sRun.floor));
            if (IS_ROGUELIKE) {
                StartRun(play); // nothing to return to on a Roguelike Descent file: start over
            } else {
                EndRun(play);
            }
        }
        return;
    }

    if (play->transitionTrigger != TRANS_TRIGGER_OFF || play->transitionMode != TRANS_MODE_OFF) {
        return;
    }

    sRun.roomFrames++;
    sRun.stateFrames++;

    switch (sRun.roomState) {
        case ROOM_STATE_ENTERING:
            if (sRun.stateFrames >= WAVE_SPAWN_DELAY) {
                SpawnWave(play);
                sRun.wavesLeft--;
                EnterRoomState(ROOM_STATE_FIGHTING);
            }
            break;

        case ROOM_STATE_FIGHTING: {
            if (sRun.roomFrames < CLEAR_CHECK_DELAY) {
                break;
            }
            if (play->actorCtx.actorLists[ACTORCAT_ENEMY].length == 0) {
                if (sRun.wavesLeft > 0) {
                    sRun.wavesLeft--;
                    Sfx_PlaySfxCentered(NA_SE_SY_WARNING_COUNT_N);
                    SpawnWave(play);
                } else {
                    OnRoomCleared(play, false);
                }
                break;
            }
            int limit = ROGUELIKE_ROOM_TIME_LIMIT;
            if (limit > 0 && sRun.roomFrames >= limit * FRAMES_PER_SECOND) {
                OnRoomCleared(play, true);
            }
            break;
        }

        case ROOM_STATE_CLEARED:
            if (sRun.stateFrames >= DESCEND_DELAY) {
                GoToNextFloor(play);
            }
            break;

        case ROOM_STATE_RUN_OVER:
            // Keep Link standing until the run wraps up.
            if (gSaveContext.health <= 0) {
                gSaveContext.health = 1;
            }
            if (sRun.stateFrames >= RUN_OVER_DELAY) {
                OnRunOver(play);
            }
            break;
    }
}

static void OnSceneInitRoguelike(int16_t sceneNum) {
    if (!sRun.active) {
        return;
    }
    // New room (or a reload of the same one after falling into a pit). Either way, the fight starts over.
    sRun.roomFrames = 0;
    sRun.wavesLeft = WavesForFloor(sRun.floor);
    if (sRun.roomState != ROOM_STATE_RUN_OVER) {
        EnterRoomState(ROOM_STATE_ENTERING);
    }
}

static void OnPlayerHealthChangeRoguelike(int16_t amount) {
    if (!sRun.active || sRun.roomState == ROOM_STATE_RUN_OVER || gSaveContext.health > 0) {
        return;
    }
    // A bottled fairy gets to do its job.
    if (HasBottledFairy()) {
        return;
    }
    // Permadeath: instead of the game over screen, end the run.
    gSaveContext.health = 1;
    Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
    EnterRoomState(ROOM_STATE_RUN_OVER);
}

static void OnLoadGameRoguelike(int32_t fileNum) {
    // Loading a file always comes from disk, which the run never wrote to.
    sRun.active = false;
    sPendingRoomPick = false;
    sStartRequested = false;
    sAbandonRequested = false;

    if (IS_ROGUELIKE) {
        StartRunFromFileLoad();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Public API (used by the menu and by Autosave)
// ---------------------------------------------------------------------------------------------------------------------

extern "C" bool Roguelike_IsRunActive() {
    return sRun.active;
}

void Roguelike_RequestStartRun() {
    if (GameInteractor::IsSaveLoaded(false) && !sRun.active) {
        sStartRequested = true;
    }
}

void Roguelike_RequestAbandonRun() {
    if (sRun.active) {
        sAbandonRequested = true;
    }
}

int Roguelike_GetCurrentFloor() {
    return sRun.active ? sRun.floor : 0;
}

int Roguelike_GetBestFloor() {
    return CVarGetInteger(ROGUELIKE_BEST_FLOOR_CVAR, 0);
}

static void RegisterRoguelikeDescent() {
    COND_HOOK(OnPlayerUpdate, true, OnPlayerUpdateRoguelike);
    COND_HOOK(OnSceneInit, true, OnSceneInitRoguelike);
    COND_HOOK(AfterSceneCommands, true, PickRoomForPendingScene);
    COND_HOOK(OnPlayerHealthChange, true, OnPlayerHealthChangeRoguelike);
    COND_HOOK(OnLoadGame, true, OnLoadGameRoguelike);

    // No saving during a run: the real save file stays untouched.
    COND_VB_SHOULD(VB_BE_ABLE_TO_SAVE, true, {
        if (sRun.active) {
            *should = false;
        }
    });
    // Doors are sealed: each room is an arena.
    COND_VB_SHOULD(VB_BE_ABLE_TO_OPEN_DOORS, true, {
        if (sRun.active) {
            *should = false;
        }
    });
    // Our room drops use the void-out respawn path, but shouldn't cost a heart.
    COND_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, true, {
        if (sSuppressVoidDamage) {
            *should = false;
            sSuppressVoidDamage = false; // only for the one load we started
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterRoguelikeDescent, {});
