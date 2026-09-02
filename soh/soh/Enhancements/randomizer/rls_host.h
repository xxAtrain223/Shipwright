#pragma once

#include "soh/Enhancements/randomizer/location_access.h"
#include "soh/Enhancements/randomizer/entrance.h"

inline bool has(const RandomizerGet item) {
    return logic->HasItem(item);
}

inline bool can_use(const RandomizerGet item) {
    return logic->CanUse(item);
}

inline bool flag(const LogicVal key) {
    return logic->Get(key);
}

inline uint8_t setting(const RandomizerSettingKey key) {
    return ctx->GetOption(key).Get();
}

inline bool trick(const RandomizerTrick key) {
    return (bool)ctx->GetTrickOption(key);
}

inline bool can_plant_bean(const RandomizerRegion reg, const RandomizerGet bean) {
    return CanPlantBean(reg, bean);
}

inline bool is_child() {
    return logic->IsChild;
}

inline bool is_adult() {
    return logic->IsAdult;
}

inline bool at_day() {
    return logic->AtDay;
}

inline bool at_night() {
    return logic->AtNight;
}

inline Rando::DungeonKey GetDungeonKey(SceneID scene) {
    switch (scene) {
        case SCENE_DEKU_TREE:
            return Rando::DEKU_TREE;
        case SCENE_DODONGOS_CAVERN:
            return Rando::DODONGOS_CAVERN;
        case SCENE_JABU_JABU:
            return Rando::JABU_JABUS_BELLY;
        case SCENE_FOREST_TEMPLE:
            return Rando::FOREST_TEMPLE;
        case SCENE_FIRE_TEMPLE:
            return Rando::FIRE_TEMPLE;
        case SCENE_WATER_TEMPLE:
            return Rando::WATER_TEMPLE;
        case SCENE_SPIRIT_TEMPLE:
            return Rando::SPIRIT_TEMPLE;
        case SCENE_SHADOW_TEMPLE:
            return Rando::SHADOW_TEMPLE;
        case SCENE_BOTTOM_OF_THE_WELL:
            return Rando::BOTTOM_OF_THE_WELL;
        case SCENE_ICE_CAVERN:
            return Rando::ICE_CAVERN;
        case SCENE_GERUDO_TRAINING_GROUND:
            return Rando::GERUDO_TRAINING_GROUND;
        case SCENE_INSIDE_GANONS_CASTLE:
            return Rando::GANONS_CASTLE;
    }
}

inline bool is_vanilla() {
    auto scene = areaTable[logic->CurrentRegionKey].scene;
    auto dungeonKey = GetDungeonKey(scene);
    return ctx->GetDungeon(dungeonKey)->IsVanilla();
}

inline bool is_mq() {
    auto scene = areaTable[logic->CurrentRegionKey].scene;
    auto dungeonKey = GetDungeonKey(scene);
    return ctx->GetDungeon(dungeonKey)->IsMQ();
}

inline int required_triforce_pieces() {
    return ctx->GetOption(RSK_TRIFORCE_HUNT_PIECES_REQUIRED).Get() + 1;
}

inline int collected_triforce_pieces() {
    return logic->GetSaveContext()->ship.quest.data.randomizer.triforcePiecesCollected;
}

inline int bottle_count() {
    return logic->BottleCount();
}

inline int check_price(const RandomizerCheck check = RC_UNKNOWN_CHECK) {
    return GetCheckPrice(check);
}

inline int effective_health() {
    return logic->EffectiveHealth();
}

inline bool small_keys(SceneID scene, int requiredAmount) {
    return logic->SmallKeys(static_cast<int16_t>(scene), requiredAmount);
}

inline int ocarina_buttons() {
    return logic->OcarinaButtons();
}

inline bool any_age(ConditionFn condition) {
    return AnyAgeTime(condition);
}