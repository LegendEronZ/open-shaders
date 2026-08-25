#include "PresetManager.h"

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
