#pragma once

class MenuManager
{
public:
	static MenuManager& GetSingleton();
	/** @brief Selects and reloads a discovered preset, preserving the selection across launches. */
	bool RenderPresetSelector();
};
