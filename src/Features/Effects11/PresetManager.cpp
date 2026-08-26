#include "PresetManager.h"

#include <Windows.h>
#include <fstream>

namespace
{
	struct KnownEffect
	{
		const char* flagName;
		const char* fileName;
	};

	// The optional ENB effects Effects11 actually loads (see EffectManager.h); enbeffect.fx
	// is unconditional and has no matching ini flag, so it's excluded here.
	constexpr KnownEffect kKnownEffects[] = {
		{ "EnableBloom", "enbbloom.fx" },
		{ "EnableLens", "enblens.fx" },
		{ "EnableAdaptation", "enbadaptation.fx" },
		{ "EnablePostPassShader", "enbeffectpostpass.fx" },
	};

	bool ReadIniBool(const std::string& a_iniPath, const char* a_key)
	{
		char buffer[16];
		GetPrivateProfileStringA("EFFECT", a_key, "false", buffer, sizeof(buffer), a_iniPath.c_str());
		return _stricmp(buffer, "true") == 0;
	}

	// Best-effort scrape of a preset-author credit line from enbeffect.fx: real ENB
	// presets commonly keep the SDK sample's unmodified "Author: Boris Vorontsov" line,
	// so that alone is misleading -- instead take the first comment line after a
	// "CREDITS" separator, which is where preset authors conventionally sign their work
	// (e.g. "// Post process file for Rudy ENB SE edited by Rudy102"). No standardized
	// field exists, so an empty result just means the convention wasn't followed.
	std::string ScrapeAuthorHint(const std::filesystem::path& a_enbeffectPath)
	{
		std::ifstream file(a_enbeffectPath);
		if (!file)
			return {};

		std::string line;
		bool sawCreditsMarker = false;
		for (int i = 0; i < 30 && std::getline(file, line); ++i) {
			if (!sawCreditsMarker) {
				if (line.find("CREDITS") != std::string::npos)
					sawCreditsMarker = true;
				continue;
			}

			auto start = line.find_first_not_of("/ \t");
			if (start == std::string::npos)
				continue;

			auto end = line.find_last_not_of(" \t\r");
			return line.substr(start, end - start + 1);
		}
		return {};
	}

	void PopulateSummary(PresetLocation& a_location)
	{
		const auto enbseriesDir = a_location.root / "enbseries";
		const auto iniPathStr = (a_location.root / "enbseries.ini").string();

		for (const auto& known : kKnownEffects) {
			a_location.effectStatus.push_back({ known.flagName,
				ReadIniBool(iniPathStr, known.flagName),
				std::filesystem::exists(enbseriesDir / known.fileName) });
		}

		a_location.authorHint = ScrapeAuthorHint(enbseriesDir / "enbeffect.fx");
	}
}

PresetManager& PresetManager::GetSingleton()
{
	static PresetManager instance;
	return instance;
}

void PresetManager::Rescan()
{
	discoveredLocations.clear();

	const auto dataRoot = std::filesystem::absolute(std::filesystem::path("Data"));
	if (std::filesystem::exists(dataRoot / "enbseries.ini") && std::filesystem::is_directory(dataRoot / "enbseries")) {
		discoveredLocations.push_back({ PresetLocationKind::DataRoot, dataRoot, "Data", true });
	}

	const auto gameRoot = std::filesystem::absolute(std::filesystem::path("."));
	if (std::filesystem::exists(gameRoot / "enbseries.ini") && std::filesystem::is_directory(gameRoot / "enbseries")) {
		discoveredLocations.push_back({ PresetLocationKind::GameRoot, gameRoot, "Game root", true });
	}

	std::error_code ec;
	if (std::filesystem::is_directory(dataRoot, ec)) {
		for (const auto& child : std::filesystem::directory_iterator(dataRoot, ec)) {
			if (!child.is_directory(ec))
				continue;

			const auto childRoot = std::filesystem::absolute(child.path());
			if (std::filesystem::exists(childRoot / "enbseries.ini") && std::filesystem::is_directory(childRoot / "enbseries")) {
				discoveredLocations.push_back({ PresetLocationKind::DataSubfolder,
					childRoot,
					"Data\\" + childRoot.filename().string(),
					true });
			}
		}
	}

	// Cheap (one ini read + a handful of stat() calls per location): safe to do inline
	// here since Rescan() only runs at Initialize() or an explicit UI/devbench action,
	// never per-frame.
	for (auto& location : discoveredLocations)
		PopulateSummary(location);
}

const std::vector<PresetLocation>& PresetManager::GetDiscoveredLocations() const
{
	return discoveredLocations;
}

void PresetManager::SetActiveLocation(const std::filesystem::path& root)
{
	activeRoot = root;
}

const PresetLocation* PresetManager::GetActiveLocation() const
{
	for (const auto& loc : discoveredLocations) {
		if (loc.root == activeRoot)
			return &loc;
	}
	return nullptr;
}

std::filesystem::path PresetManager::GetENBSeriesPath() const
{
	if (activeRoot.empty())
		return {};
	return activeRoot / "enbseries";
}

std::filesystem::path PresetManager::GetENBSeriesIniPath() const
{
	if (activeRoot.empty())
		return {};
	return activeRoot / "enbseries.ini";
}
