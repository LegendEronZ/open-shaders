#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class PresetLocationKind : uint8_t
{
	GameRoot,      // <gameroot>\enbseries.ini
	DataRoot,      // <gameroot>\Data\enbseries.ini
	DataSubfolder  // <gameroot>\Data\<mod name>\enbseries.ini
};

/** @brief One of Effects11's optional .fx-backed effects: an enbseries.ini [EFFECT]
 *  flag and whether the file it would load actually exists in this preset. */
struct PresetEffectStatus
{
	std::string flagName;
	bool enabled = false;
	bool fileExists = false;
};

/** @brief A pointer/reference into this is invalidated by the next Rescan(). */
struct PresetLocation
{
	PresetLocationKind kind;
	std::filesystem::path root;  // dir containing enbseries.ini and enbseries folder
	std::string label;           // display label for the picker
	bool valid = false;          // exists(root / "enbseries.ini") && is_directory(root / "enbseries")

	std::string headerComment;                     // enbeffect.fx's leading // comment block, reproduced verbatim; empty if none found
	std::vector<PresetEffectStatus> effectStatus;  // ini flag vs actual file, for the picker tooltip

	PresetLocation(PresetLocationKind a_kind, std::filesystem::path a_root, std::string a_label, bool a_valid) :
		kind(a_kind), root(std::move(a_root)), label(std::move(a_label)), valid(a_valid) {}
};

class PresetManager
{
public:
	static PresetManager& GetSingleton();

	/** @brief Re-scans game root, Data root, and Data's immediate child folders for
	 *  enbseries.ini + enbseries\ pairs. Call at Feature Initialize() and from an
	 *  explicit UI Rescan action; do not call every frame. */
	void Rescan();

	const std::vector<PresetLocation>& GetDiscoveredLocations() const;

	/** @brief root must be the .root of one of GetDiscoveredLocations()'s entries
	 *  (or empty, meaning no active location). Only changes cached state; does NOT
	 *  reload settings or reapply effects -- the caller does that (see MenuManager). */
	void SetActiveLocation(const std::filesystem::path& root);
	/** @brief Currently active location, or nullptr if none is active. */
	const PresetLocation* GetActiveLocation() const;

	/** @brief Converts a discovered location's (absolute) root to a path relative to
	 *  the game root, safe to persist -- an absolute path would break if the game
	 *  folder ever moves (Steam library migration, syncing configs across machines). */
	std::string ToRelativeKey(const std::filesystem::path& a_root) const;
	/** @brief Reverse of ToRelativeKey(): the discovered location whose root matches
	 *  a_relativeKey, or nullptr. */
	const PresetLocation* FindByRelativeKey(const std::string& a_relativeKey) const;

	// Unchanged signatures, still the only thing ~20 other call sites (WeatherManager,
	// Effect, SettingManager, Effects11.cpp) need to know about.
	std::filesystem::path GetENBSeriesPath() const;
	std::filesystem::path GetENBSeriesIniPath() const;

private:
	std::vector<PresetLocation> discoveredLocations;
	std::filesystem::path activeRoot;  // empty = none active
};
