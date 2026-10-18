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
#include <unordered_map>
#include <string>
#include <vector>

#include "soh/Enhancements/cosmetics/authenticGfxPatches.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorMaximumHealth.h"
#include "soh/Notification/Notification.h"
#include "soh/SohGui/ImGuiUtils.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Box/z_en_box.h"
extern PlayState* gPlayState;
}

#define CVAR_ROGUELIKE(name) CVAR_ENHANCEMENT("Roguelike." name)

#define ROGUELIKE_START_HEARTS CVarGetInteger(CVAR_ROGUELIKE("StartHearts"), 3)
#define ROGUELIKE_ROOM_TIME_LIMIT CVarGetInteger(CVAR_ROGUELIKE("RoomTimeLimit"), 120)
#define ROGUELIKE_REWARD_EVERY CVarGetInteger(CVAR_ROGUELIKE("RewardEvery"), 2)
#define ROGUELIKE_AUTO_RESTART CVarGetInteger(CVAR_ROGUELIKE("AutoRestart"), 0)
#define ROGUELIKE_CHEST_CHANCE CVarGetInteger(CVAR_ROGUELIKE("ChestChance"), 35)
#define ROGUELIKE_ELITES CVarGetInteger(CVAR_ROGUELIKE("Elites"), 1)
#define ROGUELIKE_BOSS_FLOORS CVarGetInteger(CVAR_ROGUELIKE("BossFloors"), 1)
#define ROGUELIKE_BOSS_EVERY CVarGetInteger(CVAR_ROGUELIKE("BossEvery"), 5)
#define ROGUELIKE_MODIFIERS CVarGetInteger(CVAR_ROGUELIKE("Modifiers"), 1)
#define ROGUELIKE_BEST_FLOOR_CVAR CVAR_ROGUELIKE("BestFloor")

// The game logic runs at 20 updates per second.
static constexpr int FRAMES_PER_SECOND = 20;
static constexpr int WAVE_SPAWN_DELAY = 25;  // frames after landing before the first wave appears
static constexpr int CLEAR_CHECK_DELAY = 45; // frames after landing before a room can count as cleared
static constexpr int DESCEND_DELAY = 3 * FRAMES_PER_SECOND;
static constexpr int RUN_OVER_DELAY = 3 * FRAMES_PER_SECOND;
static constexpr int CHEST_WAIT_LIMIT = 60 * FRAMES_PER_SECOND; // descend anyway if a chest is ignored this long
static constexpr s32 CHEST_NONE = -1;

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
    ROOM_STATE_BOSS,     // boss floor: waiting for the boss to fall
} RoomState;

typedef enum {
    FLOORMOD_NONE,
    FLOORMOD_MIRROR,       // the world is mirrored
    FLOORMOD_GIANT,        // enemies are huge and tougher, 1.5x XP
    FLOORMOD_GLASS_CANNON, // everyone deals double damage
    FLOORMOD_SWARM,        // twice the enemies
    FLOORMOD_ELITE,        // every enemy is an elite
    FLOORMOD_HASTY,        // short room timer, double XP
    FLOORMOD_MAX,
} FloorModifier;

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
    s32 chestFlag; // treasure flag of the reward chest waiting to be opened, or CHEST_NONE
    int level;     // Link's level this run
    int xp;        // XP towards the next level
    bool bossFloor;
    bool bossDefeated;
    int bossWaitFrames;     // frames since the boss fell, so its death scene can play out
    FloorModifier modifier; // this floor's twist, if any
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
// Reward chests
// ---------------------------------------------------------------------------------------------------------------------

// Major chest items, reusing the reward availability rules. Rewards with no vanilla get-item (magic meter, double
// defense) are left to the floor rewards.
static GetItemID RewardToGetItem(RoguelikeReward reward) {
    switch (reward) {
        case REWARD_HEART_CONTAINER:
            return GI_HEART_CONTAINER_2;
        case REWARD_BOW:
            return GI_BOW;
        case REWARD_HOOKSHOT:
            return GI_HOOKSHOT;
        case REWARD_LONGSHOT:
            return GI_LONGSHOT;
        case REWARD_BOMB_BAG:
            return GI_BOMB_BAG_20;
        case REWARD_HAMMER:
            return GI_HAMMER;
        case REWARD_DINS_FIRE:
            return GI_DINS_FIRE;
        case REWARD_NAYRUS_LOVE:
            return GI_NAYRUS_LOVE;
        case REWARD_FIRE_ARROWS:
            return GI_ARROW_FIRE;
        case REWARD_ICE_ARROWS:
            return GI_ARROW_ICE;
        case REWARD_LIGHT_ARROWS:
            return GI_ARROW_LIGHT;
        case REWARD_MIRROR_SHIELD:
            return GI_SHIELD_MIRROR;
        case REWARD_BIGGORON_SWORD:
            return GI_SWORD_BGS;
        case REWARD_HOVER_BOOTS:
            return GI_BOOTS_HOVER;
        default:
            return GI_NONE;
    }
}

// Picks the chest's contents. Returns true for a "major" item (shown in a big chest).
static GetItemID PickChestItem(bool* isMajor) {
    std::vector<GetItemID> majors;
    for (int reward = 0; reward < REWARD_MAX; reward++) {
        GetItemID gi = RewardToGetItem((RoguelikeReward)reward);
        if (gi != GI_NONE && IsRewardAvailable((RoguelikeReward)reward)) {
            majors.push_back(gi);
        }
    }

    if (!majors.empty() && RandInt(1, 100) <= 40) {
        *isMajor = true;
        return majors[RandInt(0, (int)majors.size() - 1)];
    }

    std::vector<GetItemID> minors = { GI_HEART_PIECE, GI_HEART_PIECE };
    if (HasItem(ITEM_BOMB)) {
        minors.push_back(GI_BOMBS_10);
    }
    if (HasItem(ITEM_BOW)) {
        minors.push_back(GI_ARROWS_LARGE);
    }
    *isMajor = false;
    return minors[RandInt(0, (int)minors.size() - 1)];
}

// Finds a clear patch of floor on Link's level, in the given distance band, with no wall in between.
static bool FindFloorSpotNearPlayer(PlayState* play, f32 minRadius, f32 maxRadius, Vec3f* outPos) {
    Player* player = GET_PLAYER(play);
    Vec3f origin = player->actor.world.pos;
    origin.y += 30.0f;

    for (int attempt = 0; attempt < 16; attempt++) {
        f32 angle = RandFloat(0.0f, 6.2831853f);
        f32 radius = RandFloat(minRadius, maxRadius);

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

// Maybe drops a treasure chest after a room is cleared. Returns true if one appeared.
static s32 FindFreeTreasureFlag(PlayState* play);

static bool TrySpawnRewardChest(PlayState* play) {
    // Randomizer files map chests to seed locations, so chests are kept out of runs started on them.
    if (IS_RANDO || RandInt(1, 100) > ROGUELIKE_CHEST_CHANCE) {
        return false;
    }

    // Use a treasure flag that isn't already set in this dungeon, so the chest spawns closed.
    s32 flag = FindFreeTreasureFlag(play);
    if (flag == CHEST_NONE) {
        return false;
    }

    Vec3f pos;
    if (!FindFloorSpotNearPlayer(play, 100.0f, 220.0f, &pos)) {
        return false;
    }

    bool isMajor = false;
    GetItemID item = PickChestItem(&isMajor);
    Player* player = GET_PLAYER(play);
    s16 yaw = Math_Vec3f_Yaw(&pos, &player->actor.world.pos); // open side faces Link

    Actor* chest = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOX, pos.x, pos.y, pos.z, 0, yaw, 0,
                               (s16)ENBOX_PARAMS(isMajor ? ENBOX_TYPE_BIG_DEFAULT : ENBOX_TYPE_SMALL, item, flag));
    if (chest == NULL) {
        return false;
    }

    sRun.chestFlag = flag;
    Sfx_PlaySfxCentered(NA_SE_SY_TRE_BOX_APPEAR);
    return true;
}

// Items picked up from chests get the same convenience treatment as floor rewards.
static void OnItemReceiveRoguelike(GetItemEntry itemEntry) {
    PlayState* play = gPlayState;
    if (!sRun.active || play == NULL || itemEntry.modIndex != MOD_NONE) {
        return;
    }
    Player* player = GET_PLAYER(play);

    switch (itemEntry.itemId) {
        case ITEM_DINS_FIRE:
        case ITEM_NAYRUS_LOVE:
        case ITEM_ARROW_FIRE:
        case ITEM_ARROW_ICE:
        case ITEM_ARROW_LIGHT:
            // Magic items are useless without a meter, so the first one brings it along.
            if (!gSaveContext.isMagicAcquired) {
                GiveMagic(play, false);
                Notify("Magic meter unlocked!");
            }
            break;
        default:
            break;
    }

    switch (itemEntry.itemId) {
        case ITEM_BOW:
        case ITEM_HOOKSHOT:
        case ITEM_LONGSHOT:
        case ITEM_HAMMER:
        case ITEM_DINS_FIRE:
        case ITEM_NAYRUS_LOVE:
        case ITEM_ARROW_FIRE:
        case ITEM_ARROW_ICE:
        case ITEM_ARROW_LIGHT:
            AssignToFreeCButton(play, (u8)itemEntry.itemId);
            break;
        case ITEM_BOMB_BAG_20:
            AssignToFreeCButton(play, ITEM_BOMB);
            break;
        case ITEM_SHIELD_MIRROR:
            SetEquipValue(EQUIP_TYPE_SHIELD, EQUIP_VALUE_SHIELD_MIRROR);
            Player_SetEquipmentData(play, player);
            break;
        case ITEM_SWORD_BGS:
            gSaveContext.bgsFlag = true;
            gSaveContext.swordHealth = 8;
            gSaveContext.equips.buttonItems[0] = ITEM_SWORD_BGS;
            SetEquipValue(EQUIP_TYPE_SWORD, EQUIP_VALUE_SWORD_BIGGORON);
            Interface_LoadItemIcon1(play, 0);
            Player_SetEquipmentData(play, player);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pick 1 of 3: reward floors put down a row of big chests; opening one makes the others vanish.
// ---------------------------------------------------------------------------------------------------------------------

static constexpr int MAX_CHOICE_CHESTS = 3;

typedef struct {
    Actor* actor;
    s32 flag;
} ChoiceChest;

static ChoiceChest sChoiceChests[MAX_CHOICE_CHESTS];
static int sChoiceCount = 0;

// Chest rewards can be offered a bit more freely than direct rewards: spells and magic arrows bring a magic meter
// with them if Link doesn't have one yet (see OnItemReceiveRoguelike).
static bool IsChestRewardAvailable(RoguelikeReward reward) {
    switch (reward) {
        case REWARD_DINS_FIRE:
            return !HasItem(ITEM_DINS_FIRE);
        case REWARD_NAYRUS_LOVE:
            return !HasItem(ITEM_NAYRUS_LOVE);
        case REWARD_FIRE_ARROWS:
            return HasItem(ITEM_BOW) && !HasItem(ITEM_ARROW_FIRE);
        case REWARD_ICE_ARROWS:
            return HasItem(ITEM_BOW) && !HasItem(ITEM_ARROW_ICE);
        case REWARD_LIGHT_ARROWS:
            return HasItem(ITEM_BOW) && !HasItem(ITEM_ARROW_LIGHT);
        default:
            return IsRewardAvailable(reward);
    }
}

// A treasure flag that is unset in this scene and not already used by one of our chests.
static s32 FindFreeTreasureFlag(PlayState* play) {
    for (s32 f = 31; f >= 0; f--) {
        bool taken = Flags_GetTreasure(play, f) || f == sRun.chestFlag;
        for (int i = 0; i < sChoiceCount; i++) {
            taken = taken || sChoiceChests[i].flag == f;
        }
        if (!taken) {
            return f;
        }
    }
    return CHEST_NONE;
}

// Is there floor at about Link's height at `target`, with no wall between Link and it?
static bool IsReachableFloorSpot(PlayState* play, Vec3f target, Vec3f* outPos) {
    Player* player = GET_PLAYER(play);
    Vec3f origin = player->actor.world.pos;
    origin.y += 30.0f;
    target.y = origin.y;

    Vec3f hit;
    CollisionPoly* wallPoly = NULL;
    s32 bgId;
    if (BgCheck_EntityLineTest1(&play->colCtx, &origin, &target, &hit, &wallPoly, true, false, false, true, &bgId)) {
        return false;
    }
    Vec3f probe = target;
    probe.y += 50.0f;
    CollisionPoly* floorPoly = NULL;
    f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &floorPoly, &probe);
    if (floorPoly == NULL || floorY <= BGCHECK_Y_MIN + 1.0f || fabsf(floorY - player->actor.world.pos.y) > 60.0f) {
        return false;
    }
    *outPos = { target.x, floorY, target.z };
    return true;
}

static Actor* SpawnChest(PlayState* play, Vec3f pos, GetItemID item, bool big, s32 flag) {
    Player* player = GET_PLAYER(play);
    s16 yaw = Math_Vec3f_Yaw(&pos, &player->actor.world.pos); // open side faces Link
    return Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOX, pos.x, pos.y, pos.z, 0, yaw, 0,
                       (s16)ENBOX_PARAMS(big ? ENBOX_TYPE_BIG_DEFAULT : ENBOX_TYPE_SMALL, item, flag));
}

// Puts down up to three big chests with different major items. Returns false if there aren't at least two
// different items to choose from, or nowhere to put them (the caller then hands out a direct reward instead).
static bool SpawnRewardChoice(PlayState* play) {
    if (IS_RANDO) {
        return false;
    }

    std::vector<GetItemID> items;
    for (int reward = 0; reward < REWARD_MAX; reward++) {
        GetItemID gi = RewardToGetItem((RoguelikeReward)reward);
        if (gi != GI_NONE && IsChestRewardAvailable((RoguelikeReward)reward)) {
            items.push_back(gi);
        }
    }
    std::shuffle(items.begin(), items.end(), sRng);
    int count = std::min((int)items.size(), MAX_CHOICE_CHESTS);
    if (count < 2) {
        return false;
    }

    // Lay them out in a row in front of Link.
    Player* player = GET_PLAYER(play);
    f32 fwdX = Math_SinS(player->actor.shape.rot.y);
    f32 fwdZ = Math_CosS(player->actor.shape.rot.y);
    Vec3f center = { player->actor.world.pos.x + fwdX * 150.0f, player->actor.world.pos.y,
                     player->actor.world.pos.z + fwdZ * 150.0f };
    static const f32 sOffsets3[] = { -120.0f, 0.0f, 120.0f };
    static const f32 sOffsets2[] = { -75.0f, 75.0f };

    std::vector<Vec3f> placed;
    sChoiceCount = 0;
    for (int i = 0; i < count; i++) {
        f32 offset = (count == 3) ? sOffsets3[i] : sOffsets2[i];
        Vec3f target = { center.x + fwdZ * offset, center.y, center.z - fwdX * offset };
        Vec3f pos;
        bool found = IsReachableFloorSpot(play, target, &pos);

        // Fall back to any clear spot that isn't on top of another chest.
        for (int attempt = 0; !found && attempt < 10; attempt++) {
            if (FindFloorSpotNearPlayer(play, 110.0f, 260.0f, &pos)) {
                found = true;
                for (const Vec3f& other : placed) {
                    if (Math_Vec3f_DistXZ(&pos, (Vec3f*)&other) < 100.0f) {
                        found = false;
                    }
                }
            }
        }
        if (!found) {
            continue;
        }

        s32 flag = FindFreeTreasureFlag(play);
        if (flag == CHEST_NONE) {
            break;
        }
        Actor* chest = SpawnChest(play, pos, items[i], true, flag);
        if (chest != NULL) {
            sChoiceChests[sChoiceCount++] = { chest, flag };
            placed.push_back(pos);
        }
    }

    if (sChoiceCount < 2) {
        // Not enough room for a real choice; take back what was placed and let the caller give a direct reward.
        for (int i = 0; i < sChoiceCount; i++) {
            Actor_Kill(sChoiceChests[i].actor);
        }
        sChoiceCount = 0;
        return false;
    }

    Sfx_PlaySfxCentered(NA_SE_SY_TRE_BOX_APPEAR);
    return true;
}

// Makes every choice chest except `keep` vanish in a puff of smoke.
static void RemoveChoiceChests(PlayState* play, Actor* keep) {
    for (int i = 0; i < sChoiceCount; i++) {
        Actor* chest = sChoiceChests[i].actor;
        if (chest == NULL || chest == keep) {
            continue;
        }
        Vec3f pos = chest->world.pos;
        pos.y += 20.0f;
        Vec3f zero = { 0.0f, 0.0f, 0.0f };
        EffectSsDeadDb_Spawn(play, &pos, &zero, &zero, 120, 0, 255, 255, 255, 255, 0, 0, 255, 1, 9, true);
        Actor_Kill(chest);
        sChoiceChests[i].actor = NULL;
    }
}

// Returns true while the choice is still waiting to be made.
static bool UpdateRewardChoice(PlayState* play) {
    if (sChoiceCount == 0) {
        return false;
    }
    for (int i = 0; i < sChoiceCount; i++) {
        if (Flags_GetTreasure(play, sChoiceChests[i].flag)) {
            RemoveChoiceChests(play, sChoiceChests[i].actor);
            play->actorCtx.flags.chest &= ~(1u << sChoiceChests[i].flag); // free it for later
            sChoiceCount = 0;
            sRun.stateFrames = 0; // let the item fanfare finish before descending
            return false;
        }
    }
    if (sRun.stateFrames >= CHEST_WAIT_LIMIT) {
        RemoveChoiceChests(play, NULL);
        sChoiceCount = 0;
        Notify("The chests sink away...");
        sRun.stateFrames = DESCEND_DELAY;
        return false;
    }
    return true;
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
    int count = std::min(2 + floor / 3, 6);
    if (sRun.modifier == FLOORMOD_SWARM) {
        count = std::min(count * 2, 10);
    }
    return count;
}

static int MaxTierForFloor(int floor) {
    return std::min(1 + (floor - 1) / 4, NUM_ENEMY_TIERS);
}

// Enemies appear on Link's floor with no wall in between, so they are reachable and in this room.
static bool FindEnemySpawnSpot(PlayState* play, Vec3f* outPos) {
    return FindFloorSpotNearPlayer(play, 150.0f, 380.0f, outPos);
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
// Leveling: Link levels up from kills; enemies get levels from the floor depth, and some are elites with an element.
// ---------------------------------------------------------------------------------------------------------------------

typedef enum {
    AFFIX_NONE,
    AFFIX_FIRE,   // sets Link on fire
    AFFIX_ICE,    // freezes Link
    AFFIX_POISON, // drains hearts over a few seconds
    AFFIX_SHOCK,  // electrocutes Link
    AFFIX_MAX,
} EnemyAffix;

static const char* sAffixNames[AFFIX_MAX] = { "", "Fire", "Ice", "Poison", "Shock" };
static const Color_RGBA8 sAffixPrim[AFFIX_MAX] = {
    { 0, 0, 0, 0 }, { 255, 220, 120, 255 }, { 220, 255, 255, 255 }, { 210, 255, 140, 255 }, { 255, 255, 200, 255 },
};
static const Color_RGBA8 sAffixEnv[AFFIX_MAX] = {
    { 0, 0, 0, 0 }, { 255, 60, 0, 255 }, { 60, 150, 255, 255 }, { 60, 200, 30, 255 }, { 255, 230, 0, 255 },
};

typedef struct {
    int level;
    EnemyAffix affix;
    u8 prevHealth;
} EnemyInfo;

static std::unordered_map<Actor*, EnemyInfo> sEnemies;
static std::vector<EnemyAffix> sNewElites; // elites that appeared this frame, announced together

static EnemyAffix sPendingAffix = AFFIX_NONE; // element to apply to Link once the hit that caused it settles
static int sPendingAffixDelay = 0;
static int sAffixCooldown = 0;
static int sPoisonTicks = 0;
static int sPoisonTimer = 0;
static bool sApplyingOwnDamage = false; // set while we change health ourselves, so it isn't scaled again

static constexpr int AFFIX_COOLDOWN = 3 * FRAMES_PER_SECOND;
static constexpr int POISON_TICKS = 5;
static constexpr int POISON_INTERVAL = FRAMES_PER_SECOND * 3 / 2;
static constexpr int MAX_LEVEL = 30;

static int XpForNextLevel(int level) {
    return 40 * level;
}
// Extra damage Link deals, in percent.
static int PlayerDamageBonusPct(int level) {
    return 8 * (level - 1);
}
// Damage Link shrugs off, in percent.
static int PlayerDefensePct(int level) {
    return std::min(3 * (level - 1), 40);
}
// How much harder enemies hit on deeper floors.
static f32 EnemyDamageMultiplier(int floor) {
    return std::min(1.0f + 0.06f * (floor - 1), 2.5f);
}

static void ResetLeveling() {
    sRun.level = 1;
    sRun.xp = 0;
    sEnemies.clear();
    sNewElites.clear();
    sPendingAffix = AFFIX_NONE;
    sPendingAffixDelay = 0;
    sAffixCooldown = 0;
    sPoisonTicks = 0;
    sPoisonTimer = 0;
}

static void ApplyOwnHealthChange(PlayState* play, s16 amount) {
    sApplyingOwnDamage = true;
    Health_ChangeBy(play, amount);
    sApplyingOwnDamage = false;
}

static void GainXp(PlayState* play, int xp) {
    if (sRun.level >= MAX_LEVEL) {
        return;
    }
    sRun.xp += xp;
    while (sRun.level < MAX_LEVEL && sRun.xp >= XpForNextLevel(sRun.level)) {
        sRun.xp -= XpForNextLevel(sRun.level);
        sRun.level++;

        std::string perks = "+" + std::to_string(PlayerDamageBonusPct(sRun.level)) + "% damage, " +
                            std::to_string(PlayerDefensePct(sRun.level)) + "% defense";
        if (sRun.level % 3 == 0 && gSaveContext.healthCapacity < 20 * 16) {
            gSaveContext.healthCapacity += 16;
            perks += ", +1 heart";
        }
        ApplyOwnHealthChange(play, 2 * 16);
        Audio_PlayFanfare(NA_BGM_SMALL_ITEM_GET);
        Notify("Level up! Level " + std::to_string(sRun.level) + ": ", perks);
    }
}

// Gives a newly seen enemy its level (more health) and maybe makes it an elite.
static void SetUpEnemy(Actor* actor) {
    EnemyInfo info;
    info.level = std::max(1, sRun.floor + RandInt(-1, 1));
    info.affix = AFFIX_NONE;

    int eliteChance = std::min(8 + sRun.floor * 4, 60);
    if (sRun.modifier == FLOORMOD_ELITE) {
        eliteChance = 100;
    }
    if (sRun.bossFloor) {
        eliteChance = 0; // boss fights are hard enough
    }
    if (ROGUELIKE_ELITES && RandInt(1, 100) <= eliteChance) {
        info.affix = (EnemyAffix)RandInt(AFFIX_FIRE, AFFIX_MAX - 1);
        info.level += 2;
        sNewElites.push_back(info.affix);
    }

    f32 healthScale = 1.0f + 0.10f * (info.level - 1);
    if (info.affix != AFFIX_NONE) {
        healthScale *= 1.5f;
    }
    // Giants: bigger and tougher. A few enemies only work at their normal size (same list as Enemy Sizes).
    bool canGrow = actor->id != ACTOR_EN_BROB && actor->id != ACTOR_EN_DHA && actor->id != ACTOR_EN_DH;
    if (sRun.modifier == FLOORMOD_GIANT && canGrow) {
        Actor_SetScale(actor, actor->scale.z * 1.6f);
        healthScale *= 1.3f;
    }
    int health = (int)lroundf(actor->colChkInfo.health * healthScale);
    actor->colChkInfo.health = (u8)std::clamp(health, 1, 255);
    SetActorMaximumHealth(actor, actor->colChkInfo.health); // keeps enemy health bars right
    info.prevHealth = actor->colChkInfo.health;

    sEnemies[actor] = info;
}

static void DrawAffixParticles(PlayState* play, Actor* actor, EnemyAffix affix) {
    if ((play->gameplayFrames % 4) != 0) {
        return;
    }
    Vec3f pos = actor->focus.pos;
    pos.x += RandFloat(-20.0f, 20.0f);
    pos.y += RandFloat(-10.0f, 20.0f);
    pos.z += RandFloat(-20.0f, 20.0f);
    Vec3f velocity = { 0.0f, (affix == AFFIX_ICE) ? -0.5f : 1.5f, 0.0f };
    Vec3f accel = { 0.0f, 0.0f, 0.0f };
    Color_RGBA8 prim = sAffixPrim[affix];
    Color_RGBA8 env = sAffixEnv[affix];
    EffectSsKiraKira_SpawnDispersed(play, &pos, &velocity, &accel, &prim, &env, 1500, 12);
}

static void OnActorUpdateRoguelike(void* refActor) {
    PlayState* play = gPlayState;
    Actor* actor = (Actor*)refActor;
    if (!sRun.active || play == NULL || actor->category != ACTORCAT_ENEMY) {
        return;
    }

    auto it = sEnemies.find(actor);
    if (it == sEnemies.end()) {
        if (actor->colChkInfo.health == 0) {
            return; // invulnerable or not set up yet
        }
        SetUpEnemy(actor);
        return;
    }
    EnemyInfo& info = it->second;

    // Link's level bonus: when this enemy loses health, take a bit more. Never finishes it off by itself, so the
    // enemy's own death handling always runs from a real hit.
    u8 health = actor->colChkInfo.health;
    if (health < info.prevHealth && health > 0) {
        int dealt = info.prevHealth - health;
        int bonusPct = PlayerDamageBonusPct(sRun.level) + (sRun.modifier == FLOORMOD_GLASS_CANNON ? 100 : 0);
        int extra = (dealt * bonusPct + 50) / 100;
        if (extra > 0) {
            actor->colChkInfo.health = (u8)std::max(1, (int)health - extra);
        }
    }
    info.prevHealth = actor->colChkInfo.health;

    if (info.affix != AFFIX_NONE) {
        DrawAffixParticles(play, actor, info.affix);
    }
}

static void OnActorDestroyRoguelike(void* refActor) {
    sEnemies.erase((Actor*)refActor);
    for (int i = 0; i < sChoiceCount; i++) {
        if (sChoiceChests[i].actor == refActor) {
            sChoiceChests[i].actor = NULL;
        }
    }
}

static void OnEnemyDefeatRoguelike(void* refActor) {
    PlayState* play = gPlayState;
    if (!sRun.active || play == NULL) {
        return;
    }
    // Some enemies report their defeat more than once; only the first counts, since the entry is erased here.
    auto it = sEnemies.find((Actor*)refActor);
    if (it == sEnemies.end()) {
        return;
    }
    int level = it->second.level;
    bool elite = it->second.affix != AFFIX_NONE;
    sEnemies.erase(it);

    int xp = 10 + 5 * level;
    if (elite) {
        xp *= 2;
    }
    if (sRun.modifier == FLOORMOD_HASTY) {
        xp *= 2;
    } else if (sRun.modifier == FLOORMOD_GIANT) {
        xp = xp * 3 / 2;
    }
    GainXp(play, xp);
}

// The elite closest to Link, if one is close enough to have been the one that hit him.
static EnemyAffix FindAttackingAffix(PlayState* play) {
    Player* player = GET_PLAYER(play);
    EnemyAffix best = AFFIX_NONE;
    f32 bestDist = 200.0f;
    for (const auto& [actor, info] : sEnemies) {
        if (info.affix == AFFIX_NONE || actor->update == NULL) {
            continue;
        }
        f32 dist = Actor_WorldDistXYZToActor(actor, &player->actor);
        if (dist < bestDist) {
            bestDist = dist;
            best = info.affix;
        }
    }
    return best;
}

// Called for every health change while a run is active. Scales damage by floor depth and Link's defense, and
// queues the element of an elite that just hit Link.
static void ScaleDamageTaken(PlayState* play, int16_t amount) {
    if (amount >= 0 || sApplyingOwnDamage) {
        return;
    }
    int taken = -amount;
    f32 scaled = taken * EnemyDamageMultiplier(sRun.floor) * (1.0f - PlayerDefensePct(sRun.level) / 100.0f);
    if (sRun.modifier == FLOORMOD_GLASS_CANNON) {
        scaled *= 2.0f;
    }
    int finalDamage = std::max(1, (int)lroundf(scaled));
    int delta = finalDamage - taken; // positive: extra damage, negative: damage refunded
    gSaveContext.health = (s16)std::clamp(gSaveContext.health - delta, -1, (int)gSaveContext.healthCapacity);

    if (sAffixCooldown == 0 && sPendingAffix == AFFIX_NONE) {
        EnemyAffix affix = FindAttackingAffix(play);
        if (affix != AFFIX_NONE) {
            sPendingAffix = affix;
            sPendingAffixDelay = 8; // let the hit's own knockback play out first
        }
    }
}

static void UpdateLeveling(PlayState* play) {
    if (!sNewElites.empty()) {
        std::string names;
        for (size_t i = 0; i < sNewElites.size(); i++) {
            names += (i > 0 ? ", " : "") + std::string(sAffixNames[sNewElites[i]]);
        }
        Notify(sNewElites.size() == 1 ? "Elite enemy: " : "Elite enemies: ", names);
        sNewElites.clear();
    }

    if (sAffixCooldown > 0) {
        sAffixCooldown--;
    }

    Player* player = GET_PLAYER(play);
    if (sPendingAffix != AFFIX_NONE && --sPendingAffixDelay <= 0 && gSaveContext.health > 0 &&
        sRun.roomState != ROOM_STATE_RUN_OVER) {
        switch (sPendingAffix) {
            case AFFIX_FIRE:
                GameInteractor::RawAction::BurnPlayer();
                break;
            case AFFIX_ICE:
                GameInteractor::RawAction::FreezePlayer();
                break;
            case AFFIX_SHOCK:
                GameInteractor::RawAction::ElectrocutePlayer();
                break;
            case AFFIX_POISON:
                sPoisonTicks = POISON_TICKS;
                sPoisonTimer = POISON_INTERVAL;
                Notify("Poisoned!");
                break;
            default:
                break;
        }
        sPendingAffix = AFFIX_NONE;
        sAffixCooldown = AFFIX_COOLDOWN;
    }

    if (sPoisonTicks > 0 && --sPoisonTimer <= 0) {
        sPoisonTicks--;
        sPoisonTimer = POISON_INTERVAL;
        Vec3f pos = player->actor.world.pos;
        pos.y += 40.0f;
        Vec3f velocity = { 0.0f, 1.0f, 0.0f };
        Vec3f accel = { 0.0f, 0.0f, 0.0f };
        Color_RGBA8 prim = sAffixPrim[AFFIX_POISON];
        Color_RGBA8 env = sAffixEnv[AFFIX_POISON];
        EffectSsKiraKira_SpawnDispersed(play, &pos, &velocity, &accel, &prim, &env, 2500, 15);
        Player_PlaySfx(&player->actor, NA_SE_VO_LI_DAMAGE_S);
        ApplyOwnHealthChange(play, -4); // a quarter heart per tick
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Boss floors and floor modifiers
// ---------------------------------------------------------------------------------------------------------------------

typedef struct {
    s16 sceneNum;
    s32 entrance;
    const char* name;
    u8 tunic;
    bool (*canFight)(); // adult Link needs certain gear to be able to win
} RoguelikeBoss;

static bool BossNeedsNothing() {
    return true;
}
static bool BossNeedsBow() {
    return HasItem(ITEM_BOW);
}
static bool BossNeedsBombs() {
    return HasItem(ITEM_BOMB);
}
static bool BossNeedsHookshot() {
    return INV_CONTENT(ITEM_HOOKSHOT) != ITEM_NONE;
}
static bool BossNeedsMirrorShield() {
    return CHECK_OWNED_EQUIP(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_MIRROR);
}

static const RoguelikeBoss sBosses[] = {
    { SCENE_FIRE_TEMPLE_BOSS, ENTR_FIRE_TEMPLE_BOSS_ENTRANCE, "Volvagia", EQUIP_VALUE_TUNIC_GORON, BossNeedsNothing },
    { SCENE_DEKU_TREE_BOSS, ENTR_DEKU_TREE_BOSS_ENTRANCE, "Queen Gohma", EQUIP_VALUE_TUNIC_KOKIRI, BossNeedsBow },
    { SCENE_DODONGOS_CAVERN_BOSS, ENTR_DODONGOS_CAVERN_BOSS_ENTRANCE, "King Dodongo", EQUIP_VALUE_TUNIC_GORON,
      BossNeedsBombs },
    { SCENE_FOREST_TEMPLE_BOSS, ENTR_FOREST_TEMPLE_BOSS_ENTRANCE, "Phantom Ganon", EQUIP_VALUE_TUNIC_KOKIRI,
      BossNeedsBow },
    { SCENE_WATER_TEMPLE_BOSS, ENTR_WATER_TEMPLE_BOSS_ENTRANCE, "Morpha", EQUIP_VALUE_TUNIC_KOKIRI, BossNeedsHookshot },
    { SCENE_SHADOW_TEMPLE_BOSS, ENTR_SHADOW_TEMPLE_BOSS_ENTRANCE, "Bongo Bongo", EQUIP_VALUE_TUNIC_KOKIRI,
      BossNeedsBow },
    { SCENE_SPIRIT_TEMPLE_BOSS, ENTR_SPIRIT_TEMPLE_BOSS_2, "Twinrova", EQUIP_VALUE_TUNIC_KOKIRI,
      BossNeedsMirrorShield },
};

static int sLastBoss = -1;

static bool IsBossFloor(int floor) {
    return ROGUELIKE_BOSS_FLOORS && floor > 0 && floor % std::max(2, ROGUELIKE_BOSS_EVERY) == 0;
}

static const char* sModifierNames[FLOORMOD_MAX] = {
    "", "Mirror World", "Giants", "Glass Cannon", "Swarm", "Elite Floor", "Hasty",
};
static const char* sModifierDescriptions[FLOORMOD_MAX] = {
    "",
    "The world is flipped.",
    "Enemies are huge and tougher. 1.5x XP.",
    "You and the enemies deal double damage.",
    "Twice as many enemies.",
    "Every enemy is an elite.",
    "45 seconds to clear the room. Double XP.",
};

static FloorModifier RollFloorModifier(int floor) {
    if (!ROGUELIKE_MODIFIERS || floor < 2) {
        return FLOORMOD_NONE;
    }
    int chance = std::min(15 + floor * 3, 50);
    if (RandInt(1, 100) > chance) {
        return FLOORMOD_NONE;
    }
    return (FloorModifier)RandInt(FLOORMOD_NONE + 1, FLOORMOD_MAX - 1);
}

// Mirror World borrows SoH's own Mirrored World switch for one floor, unless the player already has that mode on.
static bool sMirrorApplied = false;
static void SetRunMirror(bool on) {
    if (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorldMode"), 0) != 0 || on == sMirrorApplied) {
        return;
    }
    sMirrorApplied = on;
    if (on) {
        CVarSetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 1);
    } else {
        CVarClear(CVAR_ENHANCEMENT("MirroredWorld"));
    }
    ApplyMirrorWorldGfxPatches();
}

// Sets up a boss floor. Returns the boss room entrance.
static s32 PrepareBossFloor() {
    std::vector<int> eligible;
    for (int i = 0; i < (int)ARRAY_COUNT(sBosses); i++) {
        if (sBosses[i].canFight()) {
            eligible.push_back(i);
        }
    }
    if (eligible.size() > 1) {
        eligible.erase(std::remove(eligible.begin(), eligible.end(), sLastBoss), eligible.end());
    }
    int index = eligible[RandInt(0, (int)eligible.size() - 1)];
    const RoguelikeBoss& boss = sBosses[index];
    sLastBoss = index;

    // Make sure the boss is there even if it was beaten before (in the real save or earlier this run).
    memset(&gSaveContext.sceneFlags[boss.sceneNum], 0, sizeof(SavedSceneFlags));

    SetEquipValue(EQUIP_TYPE_TUNIC, boss.tunic);
    sPendingRoomPick = false;
    sRun.bossFloor = true;
    sRun.bossDefeated = false;
    sRun.bossWaitFrames = 0;
    sRun.modifier = FLOORMOD_NONE;

    Notify("Floor " + std::to_string(sRun.floor) + ": BOSS - ", boss.name);
    return boss.entrance;
}

static void OnBossDefeatRoguelike(void* refActor) {
    if (sRun.active && sRun.bossFloor && !sRun.bossDefeated) {
        sRun.bossDefeated = true;
        sRun.bossWaitFrames = 0;
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

    // Wear the right tunic for the heat / water.
    GiveOwnedEquip(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_GORON);
    GiveOwnedEquip(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_ZORA);

    if (IsBossFloor(sRun.floor)) {
        return PrepareBossFloor();
    }
    sRun.bossFloor = false;
    sRun.modifier = RollFloorModifier(sRun.floor);

    int unlocked = std::min((int)ARRAY_COUNT(sDungeons), 4 + ((sRun.floor - 1) / 3) * 2);
    int dungeon = RandInt(0, unlocked - 1);
    const RoguelikeDungeon& d = sDungeons[dungeon];

    SetEquipValue(EQUIP_TYPE_TUNIC, d.tunic);

    sPendingRoomPick = true;
    sPendingScene = d.sceneNum;
    sRun.lastDungeon = dungeon;

    Notify("Floor " + std::to_string(sRun.floor) + ": ", d.name);
    if (sRun.modifier != FLOORMOD_NONE) {
        Notify(std::string(sModifierNames[sRun.modifier]) + "! ", sModifierDescriptions[sRun.modifier]);
    }
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

    // Skip the boss intro cutscenes (same flags Boss Rush uses).
    gSaveContext.eventChkInf[7] |= 0xFF;
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
    sRun.chestFlag = CHEST_NONE;
    sRun.bossFloor = false;
    sRun.bossDefeated = false;
    sRun.bossWaitFrames = 0;
    sRun.modifier = FLOORMOD_NONE;
    sLastBoss = -1;
    sChoiceCount = 0;
    ResetLeveling();
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
    sRun.chestFlag = CHEST_NONE;

    if (timedOut) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        Notify("Out of time. ", "The floor gives way...");
        return;
    }

    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    GiveRoomClearRefill(play);

    int every = std::max(1, ROGUELIKE_REWARD_EVERY);
    if (sRun.floor % every == 0) {
        if (SpawnRewardChoice(play)) {
            Notify("Choose your reward! ", "Open one chest - the others will vanish.");
        } else {
            GiveRandomReward(play);
        }
    } else {
        Notify("Floor " + std::to_string(sRun.floor) + " cleared!");
    }

    if (sChoiceCount == 0 && TrySpawnRewardChest(play)) {
        Notify("A treasure chest appeared! ", "Open it to descend.");
    }
}

// The boss is down and its death scene is over: big rewards, then the usual pick of three.
static void OnBossCleared(PlayState* play) {
    EnterRoomState(ROOM_STATE_CLEARED);
    SaveBestFloor(sRun.floor);
    sRun.chestFlag = CHEST_NONE;

    Notify("Boss defeated! ", "Floor " + std::to_string(sRun.floor) + " cleared.");
    GiveRoomClearRefill(play);
    if (gSaveContext.healthCapacity < 20 * 16) {
        Item_Give(play, ITEM_HEART_CONTAINER);
        Notify("Reward: ", "Heart Container", GetTextureForItemId(ITEM_HEART_CONTAINER));
    }
    gSaveContext.healthAccumulator = gSaveContext.healthCapacity; // full heal

    // A bonus that chests can't hold.
    std::vector<RoguelikeReward> bonus;
    for (RoguelikeReward reward : { REWARD_MAGIC, REWARD_DOUBLE_MAGIC, REWARD_DOUBLE_DEFENSE }) {
        if (IsRewardAvailable(reward)) {
            bonus.push_back(reward);
        }
    }
    if (!bonus.empty()) {
        GiveReward(play, bonus[RandInt(0, (int)bonus.size() - 1)]);
    }

    GainXp(play, 60 + 15 * sRun.floor);

    if (SpawnRewardChoice(play)) {
        Notify("Choose your reward! ", "Open one chest - the others will vanish.");
    } else {
        GiveRandomReward(play);
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
    UpdateLeveling(play);

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
            if (sRun.modifier == FLOORMOD_HASTY) {
                limit = 45;
            }
            if (limit > 0 && sRun.roomFrames >= limit * FRAMES_PER_SECOND) {
                OnRoomCleared(play, true);
            }
            break;
        }

        case ROOM_STATE_BOSS:
            if (sRun.bossDefeated) {
                sRun.bossWaitFrames++;
                if (sRun.bossWaitFrames >= 2 * FRAMES_PER_SECOND && play->msgCtx.msgMode == MSGMODE_NONE &&
                    !Player_InCsMode(play)) {
                    OnBossCleared(play);
                }
            }
            break;

        case ROOM_STATE_CLEARED:
            if (UpdateRewardChoice(play)) {
                break;
            }
            if (sRun.chestFlag != CHEST_NONE) {
                if (Flags_GetTreasure(play, sRun.chestFlag)) {
                    // Opened. Free the flag again so it can't hide a real chest later, then wait for the item
                    // fanfare to finish before descending.
                    play->actorCtx.flags.chest &= ~(1u << sRun.chestFlag);
                    sRun.chestFlag = CHEST_NONE;
                    sRun.stateFrames = 0;
                } else if (sRun.stateFrames >= CHEST_WAIT_LIMIT) {
                    Notify("The chest sinks away...");
                    sRun.chestFlag = CHEST_NONE;
                    sRun.stateFrames = DESCEND_DELAY;
                } else {
                    break;
                }
            }
            if (sRun.stateFrames >= DESCEND_DELAY && play->msgCtx.msgMode == MSGMODE_NONE && !Player_InCsMode(play)) {
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
    SetRunMirror(sRun.active && sRun.modifier == FLOORMOD_MIRROR);
    if (!sRun.active) {
        return;
    }
    // New room (or a reload of the same one after falling into a pit). Either way, the fight starts over.
    sRun.roomFrames = 0;
    sRun.wavesLeft = WavesForFloor(sRun.floor);
    sEnemies.clear();
    sNewElites.clear();
    sPendingAffix = AFFIX_NONE;
    sChoiceCount = 0;
    if (sRun.roomState != ROOM_STATE_RUN_OVER) {
        if (sRun.bossFloor) {
            sRun.wavesLeft = 0;
            EnterRoomState(ROOM_STATE_BOSS);
        } else {
            EnterRoomState(ROOM_STATE_ENTERING);
        }
    }
}

static void OnPlayerHealthChangeRoguelike(int16_t amount) {
    if (!sRun.active || sRun.roomState == ROOM_STATE_RUN_OVER) {
        return;
    }
    if (gPlayState != NULL) {
        ScaleDamageTaken(gPlayState, amount);
    }
    if (gSaveContext.health > 0) {
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
    SetRunMirror(false);

    if (IS_ROGUELIKE) {
        StartRunFromFileLoad();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Public API (used by the menu and by Autosave)
// ---------------------------------------------------------------------------------------------------------------------

// Shows Link's level/XP and the floor along the bottom of the pause screen while a run is active.
extern "C" void Roguelike_DrawPauseInfo(PlayState* play) {
    // Only on the main pause screen (not while it opens/closes or on the save prompt).
    if (!sRun.active || play == NULL || play->pauseCtx.state != 6 || play->pauseCtx.debugState != 0) {
        return;
    }

    std::string levelText = "Level " + std::to_string(sRun.level);
    if (sRun.level < MAX_LEVEL) {
        levelText += "  XP " + std::to_string(sRun.xp) + "/" + std::to_string(XpForNextLevel(sRun.level));
    } else {
        levelText += "  (max)";
    }
    std::string floorText = "Floor " + std::to_string(sRun.floor) + "  Best " +
                            std::to_string(std::max(sRun.floor, CVarGetInteger(ROGUELIKE_BEST_FLOOR_CVAR, 0)));

    u8 alpha = (u8)std::min<u16>(play->pauseCtx.alpha, 255);

    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_39Opa(play->state.gfxCtx);
    gDPSetAlphaDither(POLY_OPA_DISP++, G_AD_DISABLE);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_SHADE);
    gDPSetCombineLERP(POLY_OPA_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                      PRIMITIVE, 0);
    CLOSE_DISPS(play->state.gfxCtx);

    if (sRun.bossFloor || sRun.modifier != FLOORMOD_NONE) {
        std::string twist = sRun.bossFloor ? std::string("Boss floor") : std::string(sModifierNames[sRun.modifier]);
        Interface_DrawTextLine(play->state.gfxCtx, (char*)twist.c_str(), 190, 210, 150, 210, 255, alpha, 0.65f, 1);
    }

    // Left: level (gold). Right: floor (white).
    Interface_DrawTextLine(play->state.gfxCtx, (char*)levelText.c_str(), 40, 222, 255, 220, 100, alpha, 0.75f, 1);
    Interface_DrawTextLine(play->state.gfxCtx, (char*)floorText.c_str(), 190, 222, 255, 255, 255, alpha, 0.75f, 1);
}

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

int Roguelike_GetLevel() {
    return sRun.active ? sRun.level : 0;
}

int Roguelike_GetXp() {
    return sRun.xp;
}

int Roguelike_GetXpForNextLevel() {
    return XpForNextLevel(std::max(1, sRun.level));
}

static void RegisterRoguelikeDescent() {
    COND_HOOK(OnPlayerUpdate, true, OnPlayerUpdateRoguelike);
    COND_HOOK(OnSceneInit, true, OnSceneInitRoguelike);
    COND_HOOK(AfterSceneCommands, true, PickRoomForPendingScene);
    COND_HOOK(OnPlayerHealthChange, true, OnPlayerHealthChangeRoguelike);
    COND_HOOK(OnLoadGame, true, OnLoadGameRoguelike);
    COND_HOOK(OnItemReceive, true, OnItemReceiveRoguelike);
    COND_HOOK(OnActorUpdate, true, OnActorUpdateRoguelike);
    COND_HOOK(OnActorDestroy, true, OnActorDestroyRoguelike);
    COND_HOOK(OnEnemyDefeat, true, OnEnemyDefeatRoguelike);
    COND_HOOK(OnBossDefeat, true, OnBossDefeatRoguelike);

    // Boss floors: no blue warp out and no heart container drop; the run hands out its own rewards.
    COND_VB_SHOULD(VB_SPAWN_BLUE_WARP, true, {
        if (sRun.active) {
            *should = false;
        }
    });
    COND_VB_SHOULD(VB_PLAY_BLUE_WARP_CS, true, {
        if (sRun.active) {
            *should = false;
        }
    });
    COND_VB_SHOULD(VB_SPAWN_HEART_CONTAINER, true, {
        if (sRun.active) {
            *should = false;
        }
    });

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
