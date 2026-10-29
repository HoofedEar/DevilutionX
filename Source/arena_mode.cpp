/**
 * @file arena_mode.cpp
 *
 * Implementation of the single player Arena game mode: a permadeath run that starts
 * with a fresh hero and fights increasingly difficult waves of monsters.
 *
 * Every arena hosts a fixed number of waves drawn from a small pool of monster types.
 * Once they are cleared the arena's exit opens and leads to the next arena, which
 * draws its monsters from a deeper dungeon level.
 */
#include "arena_mode.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "control/control.hpp"
#include "effects.h"
#include "engine/random.hpp"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "interfac.h"
#include "inv_iterators.hpp"
#include "items.h"
#include "levels/gendung.h"
#include "levels/setmaps.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "objects.h"
#include "plrmsg.h"
#include "tables/monstdat.h"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/status_macros.hpp"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"

namespace devilution {

namespace {

/** Number of waves fought in an arena before its exit opens. */
constexpr int WavesPerArena = 3;
/** Seconds between entering an arena or clearing a wave and the next wave starting. */
constexpr int WaveCountdownSeconds = 5;
/** Number of monster types an arena draws its waves from. */
constexpr size_t MaxArenaMonsterTypes = 4;
/** Sprite budget for an arena's monster types, matching the limit used for regular dungeon levels. */
constexpr int MaxArenaMonsterImages = 4000;
constexpr int MaxWaveSize = 40;
/** Monsters spawn at least this many tiles away from the player, if the arena has room for it. */
constexpr int MinSpawnDistance = 7;

/** The arenas a run cycles through. */
constexpr _setlevels ArenaRotation[] = { SL_ARENA_CHURCH, SL_ARENA_HELL, SL_ARENA_CIRCLE_OF_LIFE };

enum class ArenaPhase : uint8_t {
	/** Counting down to the next wave. */
	Countdown,
	/** A wave is alive. */
	Fighting,
	/** All waves of the arena are cleared and its exit is open. */
	Cleared,
};

struct ArenaRun {
	HeroClass heroClass = HeroClass::Warrior;
	char heroName[PlayerNameLength] = {};
	/** Number of arenas cleared so far, drives the monster difficulty. */
	int arenaIndex = 0;
	/** The current wave, counted across the whole run. */
	int wave = 0;
	/** Number of waves started in the current arena. */
	int wavesInArena = 0;
	ArenaPhase phase = ArenaPhase::Countdown;
	int countdownTicks = 0;
	/** Indices into LevelMonsterTypes of the monster types the current arena's waves are made of. */
	std::vector<size_t> monsterTypes;
	/** Prevents taking the exit by accident when standing on it as it opens. */
	bool exitArmed = false;
};

ArenaRun Run;

int TicksPerSecond()
{
	return std::max<int>(sgGameInitInfo.nTickRate, 1);
}

_setlevels CurrentArenaLevel()
{
	// The shareware data lacks the hell tileset the other arenas are built from.
	if (gbIsSpawn)
		return SL_ARENA_CHURCH;
	return ArenaRotation[Run.arenaIndex % std::size(ArenaRotation)];
}

int MaxArenaDepth()
{
	return gbIsHellfire ? 24 : 16;
}

/** @brief The dungeon level the current arena's monsters are taken from. */
int ArenaDepth()
{
	return std::min(1 + (Run.arenaIndex * 2), MaxArenaDepth());
}

/** @brief Number of arenas past the deepest dungeon level, which make up for it by strengthening their monsters. */
int ArenasBeyondMaxDepth()
{
	return std::max(0, Run.arenaIndex - ((MaxArenaDepth() - 1) / 2));
}

int WavesCleared()
{
	return Run.phase == ArenaPhase::Fighting ? Run.wave - 1 : Run.wave;
}

WorldTilePosition ArenaExitPosition()
{
	// LoadArenaMap keeps the exit trigger's position around while the trigger itself is disabled.
	return trigs[0].position;
}

bool IsArenaMonsterCandidate(const MonsterData &monsterData, int depth)
{
	if (monsterData.availability == MonsterAvailability::Never)
		return false;

	if (gbIsSpawn && monsterData.availability == MonsterAvailability::Retail)
		return false;

	return depth >= monsterData.minDunLvl && depth <= monsterData.maxDunLvl;
}

std::vector<_monster_id> GetArenaMonsterCandidates()
{
	// The shareware version lacks most monsters, so fall back to shallower levels if a depth has none.
	for (int depth = ArenaDepth(); depth >= 1; depth--) {
		std::vector<_monster_id> candidates;
		for (size_t i = 0; i < MonstersData.size(); i++) {
			const auto type = static_cast<_monster_id>(i);
			if (type != MT_GOLEM && IsArenaMonsterCandidate(MonstersData[i], depth))
				candidates.push_back(type);
		}
		if (!candidates.empty())
			return candidates;
	}
	return {};
}

bool IsWaveMonster(const Monster &monster)
{
	return monster.type().type != MT_GOLEM && !monster.isInvalid && !monster.hasNoLife() && monster.mode != MonsterMode::Death;
}

int CountWaveMonsters()
{
	int count = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		if (IsWaveMonster(Monsters[ActiveMonsters[i]]))
			count++;
	}
	return count;
}

/** @brief Keeps wave monsters tracking the player, even when the player is out of their sight. */
void HuntPlayer()
{
	const WorldTilePosition target = MyPlayer->position.future;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsWaveMonster(monster) || (monster.flags & MFLAG_TARGETS_MONSTER) != 0)
			continue;
		monster.activeForTicks = UINT8_MAX;
		monster.position.last = target;
	}
}

bool IsTileFreeForSpawn(Point position)
{
	return IsTileWalkable(position)
	    && dMonster[position.x][position.y] == 0
	    && dPlayer[position.x][position.y] == 0
	    && !IsObjectAtPosition(position)
	    && position != ArenaExitPosition();
}

/** @brief Finds free tiles the player can be reached from, preferring ones that keep some distance to the player. */
std::vector<Point> FindSpawnPositions(Point origin)
{
	constexpr Displacement Neighbors[] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

	std::vector<bool> visited(static_cast<size_t>(MAXDUNX) * MAXDUNY, false);
	const auto visit = [&visited](Point position) {
		const size_t index = (static_cast<size_t>(position.x) * MAXDUNY) + position.y;
		const bool wasVisited = visited[index];
		visited[index] = true;
		return !wasVisited;
	};

	std::vector<Point> queue { origin };
	visit(origin);
	std::vector<Point> farPositions;
	std::vector<Point> nearPositions;
	for (size_t head = 0; head < queue.size(); head++) {
		const Point position = queue[head];
		if (IsTileFreeForSpawn(position)) {
			if (position.WalkingDistance(origin) >= MinSpawnDistance)
				farPositions.push_back(position);
			else
				nearPositions.push_back(position);
		}
		for (const Displacement offset : Neighbors) {
			const Point next = position + offset;
			if (InDungeonBounds(next) && visit(next) && IsTileWalkable(next, /*ignoreDoors=*/true))
				queue.push_back(next);
		}
	}

	return farPositions.empty() ? nearPositions : farPositions;
}

int WaveSize()
{
	return std::min(4 + (2 * Run.wavesInArena) + (2 * Run.arenaIndex), MaxWaveSize);
}

void ApplyArenaScaling(Monster &monster)
{
	const int extraArenas = ArenasBeyondMaxDepth();
	if (extraArenas == 0)
		return;

	const int hitPointsPercent = std::min(100 + (50 * extraArenas), 1000);
	monster.maxHitPoints = monster.maxHitPoints * hitPointsPercent / 100;
	monster.hitPoints = monster.maxHitPoints;

	const int damagePercent = std::min(100 + (20 * extraArenas), 400);
	const auto scaleDamage = [damagePercent](uint8_t damage) {
		return static_cast<uint8_t>(std::min(damage * damagePercent / 100, 255));
	};
	monster.minDamage = scaleDamage(monster.minDamage);
	monster.maxDamage = scaleDamage(monster.maxDamage);
	monster.minDamageSpecial = scaleDamage(monster.minDamageSpecial);
	monster.maxDamageSpecial = scaleDamage(monster.maxDamageSpecial);
}

void SpawnWave()
{
	if (Run.monsterTypes.empty())
		return;

	std::vector<Point> positions = FindSpawnPositions(MyPlayer->position.tile);
	const int waveSize = WaveSize();
	for (int i = 0; i < waveSize && !positions.empty(); i++) {
		const size_t pick = static_cast<size_t>(GenerateRnd(static_cast<int32_t>(positions.size())));
		const Point position = positions[pick];
		positions[pick] = positions.back();
		positions.pop_back();

		const size_t typeIndex = Run.monsterTypes[static_cast<size_t>(GenerateRnd(static_cast<int32_t>(Run.monsterTypes.size())))];
		Monster *monster = AddMonster(position, static_cast<Direction>(GenerateRnd(8)), typeIndex, true);
		if (monster == nullptr)
			break;

		ApplyArenaScaling(*monster);
		// Let wave monsters path around obstacles to reach the player.
		monster->flags |= MFLAG_SEARCH;
	}
	HuntPlayer();
}

void StartCountdown()
{
	Run.phase = ArenaPhase::Countdown;
	Run.countdownTicks = WaveCountdownSeconds * TicksPerSecond();
}

void StartNextWave()
{
	Run.wave++;
	Run.wavesInArena++;
	Run.phase = ArenaPhase::Fighting;
	SpawnWave();
	EventPlrMsg(FormatRuntime(_("Wave {:d} has begun!"), Run.wave), UiFlags::ColorRed);
}

void IdentifyArenaItems()
{
	// There is no town to identify items in, so clearing a wave reveals the loot.
	for (uint8_t i = 0; i < ActiveItemCount; i++) {
		Items[ActiveItems[i]]._iIdentified = true;
	}

	Player &myPlayer = *MyPlayer;
	for (Item &item : PlayerItemsRange(myPlayer)) {
		item._iIdentified = true;
	}
	CalcPlrInv(myPlayer, true);
}

void SpawnWaveReward()
{
	const Point playerPosition = MyPlayer->position.tile;
	for (const _item_indexes reward : { IDI_HEAL, IDI_MANA }) {
		SpawnRewardItem(reward, GetSuperItemLoc(playerPosition), /*sendmsg=*/false);
	}
}

void FinishWave()
{
	IdentifyArenaItems();
	SpawnWaveReward();

	if (Run.wavesInArena < WavesPerArena) {
		EventPlrMsg(FormatRuntime(_("Wave {:d} cleared!"), Run.wave));
		StartCountdown();
		return;
	}

	Run.phase = ArenaPhase::Cleared;
	Run.exitArmed = MyPlayer->position.tile != ArenaExitPosition();
	PlaySFX(SfxID::QuestDone);
	EventPlrMsg(_("The arena is clear! Take the exit to enter the next arena."));
}

/** @brief Rewards the hero for clearing an arena, as there is no town to heal and repair in. */
void RestoreArenaHero(Player &player)
{
	for (Item &item : EquippedPlayerItemsRange(player)) {
		item._iDurability = item._iMaxDur;
		item._iCharges = item._iMaxCharges;
	}
	CalcPlrInv(player, true);
	player.RestoreFullLife();
	player.RestoreFullMana();
}

void EnterNextArena()
{
	Player &myPlayer = *MyPlayer;

	Run.arenaIndex++;
	Run.wavesInArena = 0;
	StartCountdown();
	RestoreArenaHero(myPlayer);

	const _setlevels nextArena = CurrentArenaLevel();
	setlvltype = GetArenaLevelType(nextArena);
	StartNewLvl(myPlayer, WM_DIABSETLVL, nextArena);
}

void CheckArenaExit()
{
	const Player &myPlayer = *MyPlayer;
	if (myPlayer.position.tile != ArenaExitPosition()) {
		Run.exitArmed = true;
		return;
	}

	if (Run.exitArmed && myPlayer._pmode == PM_STAND)
		EnterNextArena();
}

} // namespace

bool ArenaUiSetHeroInfos(bool (* /*uiAddHeroInfo*/)(_uiheroinfo *))
{
	return true;
}

bool ArenaUiCreateHero(_uiheroinfo *heroInfo)
{
	Run.heroClass = heroInfo->heroclass;
	CopyUtf8(Run.heroName, heroInfo->name, sizeof(Run.heroName));
	return true;
}

void CreateArenaPlayer(Player &player)
{
	CreatePlayer(player, Run.heroClass);
	CopyUtf8(player._pName, Run.heroName, PlayerNameLength);
	CalcPlrInv(player, false);
}

void InitArenaRun()
{
	Run.arenaIndex = 0;
	Run.wave = 0;
	Run.wavesInArena = 0;
	Run.exitArmed = false;
	StartCountdown();

	const _setlevels arena = CurrentArenaLevel();
	setlevel = true;
	setlvlnum = arena;
	setlvltype = GetArenaLevelType(arena);
	leveltype = setlvltype;
	currlevel = static_cast<uint8_t>(arena);
	MyPlayer->setLevel(arena);
}

std::expected<void, std::string> AddArenaMonsterTypes()
{
	Run.monsterTypes.clear();

	std::vector<_monster_id> candidates = GetArenaMonsterCandidates();
	int images = 0;
	while (!candidates.empty() && Run.monsterTypes.size() < MaxArenaMonsterTypes && LevelMonsterTypeCount < MaxLvlMTypes) {
		const size_t pick = static_cast<size_t>(GenerateRnd(static_cast<int32_t>(candidates.size())));
		const _monster_id type = candidates[pick];
		candidates[pick] = candidates.back();
		candidates.pop_back();

		const int image = MonstersData[type].image;
		if (images + image > MaxArenaMonsterImages)
			continue;
		images += image;

		ASSIGN_OR_RETURN(const size_t typeIndex, AddMonsterType(type, PLACE_SCATTER));
		Run.monsterTypes.push_back(typeIndex);
	}

	if (Run.monsterTypes.empty())
		return std::unexpected<std::string>("No monsters are available for the arena");

	return {};
}

void ProcessArena()
{
	if (!gbIsArenaMode || !setlevel || !IsArenaLevel(setlvlnum))
		return;

	const Player &myPlayer = *MyPlayer;
	if (MyPlayerIsDead || myPlayer.hasNoLife() || myPlayer._pLvlChanging || myPlayer._pmode == PM_NEWLVL)
		return;

	switch (Run.phase) {
	case ArenaPhase::Countdown:
		if (--Run.countdownTicks <= 0)
			StartNextWave();
		break;
	case ArenaPhase::Fighting:
		if (CountWaveMonsters() == 0)
			FinishWave();
		else
			HuntPlayer();
		break;
	case ArenaPhase::Cleared:
		CheckArenaExit();
		break;
	}
}

bool IsArenaExitOpen()
{
	return gbIsArenaMode && Run.phase == ArenaPhase::Cleared;
}

void DrawArenaHud(const Surface &out)
{
	if (!gbIsArenaMode || !setlevel || !IsArenaLevel(setlvlnum))
		return;

	if (MyPlayerIsDead) {
		// Shown below the "You have died" text.
		const TextRenderOptions options {
			.flags = UiFlags::FontSize30 | UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::VerticalCenter,
			.spacing = 2
		};
		DrawString(out, FormatRuntime(_("Waves survived: {:d}"), WavesCleared()), Point { 0, gnScreenHeight / 2 }, options);
		return;
	}

	std::string title;
	std::string status;
	switch (Run.phase) {
	case ArenaPhase::Countdown: {
		const int seconds = (Run.countdownTicks + TicksPerSecond() - 1) / TicksPerSecond();
		title = FormatRuntime(_("Wave {:d}"), Run.wave + 1);
		status = FormatRuntime(_("Begins in {:d}"), std::max(seconds, 0));
	} break;
	case ArenaPhase::Fighting:
		title = FormatRuntime(_("Wave {:d}"), Run.wave);
		status = FormatRuntime(_("Enemies left: {:d}"), CountWaveMonsters());
		break;
	case ArenaPhase::Cleared:
		title = _("Arena cleared");
		status = _("Take the exit to continue");
		break;
	}

	int right = out.w() - 8;
	if (IsRightPanelOpen())
		right = std::min(right, GetRightPanel().position.x - 8);

	Point position { 8, 8 };
	const int width = right - position.x;
	DrawString(out, title, Rectangle { position, Size { width, 0 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorGold | UiFlags::AlignRight });
	position.y += 28;
	DrawString(out, status, Rectangle { position, Size { width, 0 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::AlignRight });
	position.y += 16;
	DrawString(out, _(QuestLevelNames[setlvlnum]), Rectangle { position, Size { width, 0 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhitegold | UiFlags::AlignRight });
}

} // namespace devilution
