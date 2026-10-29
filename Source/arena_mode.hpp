/**
 * @file arena_mode.hpp
 *
 * Interface of the single player Arena game mode: a permadeath run that starts
 * with a fresh hero and fights increasingly difficult waves of monsters.
 */
#pragma once

#include <expected>
#include <string>

#include "DiabloUI/diabloui.h"
#include "engine/surface.hpp"
#include "player.h"

namespace devilution {

/** @brief Hero dialog callback that lists no heroes, so every arena run starts by creating a new one. */
bool ArenaUiSetHeroInfos(bool (*uiAddHeroInfo)(_uiheroinfo *));

/** @brief Hero dialog callback that keeps the new hero in memory instead of writing a save file. */
bool ArenaUiCreateHero(_uiheroinfo *heroInfo);

/** @brief Builds the local player from the hero chosen in the arena hero dialog. */
void CreateArenaPlayer(Player &player);

/** @brief Resets the run state and moves the local player into the first arena. */
void InitArenaRun();

/** @brief Registers the monster types that the current arena's waves are drawn from. */
std::expected<void, std::string> AddArenaMonsterTypes();

/** @brief Advances the wave logic, called once per game tick. */
void ProcessArena();

/** @brief Whether all waves of the current arena are cleared and the exit leads to the next arena. */
bool IsArenaExitOpen();

void DrawArenaHud(const Surface &out);

} // namespace devilution
