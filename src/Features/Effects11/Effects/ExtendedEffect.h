#pragma once

#include "../UITree.h"
#include "Effect.h"

#include <span>

#ifdef ENABLE_ENB_EXTENDER

class ExtendedEffect : public Effect
{
public:
	void LoadWeatherData();
	void ApplyWeatherBlending(float blendFactor, uint32_t currentWeatherID, uint32_t lastWeatherID);
	void SyncWeatherDataFromUI(uint32_t weatherID);
	void ApplyTimeOfDayInterpolation();

	void Unload() override;
	bool IsTechniqueEnabled(TechniqueInfo& info) override;

	// Rendering
	void RenderImGui() override;
	static void RenderMergedUI(std::span<Effect*> effects, UITree::FilterMode filter = UITree::FilterMode::All);

private:
	using WeatherValues = std::unordered_map<std::string, std::string>;
	std::unordered_map<uint32_t, WeatherValues> weatherData;

	std::unordered_map<std::string, int> bindingCache;

	enum class TimePeriod : uint8_t
	{
		Dawn,
		Sunrise,
		Day,
		Sunset,
		Dusk,
		Night,
		Interior,
		Unknown
	};

	struct TimeOfDayEntry
	{
		size_t index;
		TimePeriod period;
	};

	struct TimeOfDayGroup
	{
		ID3DX11EffectVariable* baseVariable = nullptr;
		UIVariableType type = UIVariableType::Float;
		bool exteriorWeatherOnly = false;
		std::vector<TimeOfDayEntry> entries;
	};

	// Which variables share a base name, and which period each carries, is fixed once a
	// preset's UI variables are parsed -- only the weights move per frame.
	std::vector<TimeOfDayGroup> timeOfDayGroups;
	bool timeOfDayGroupsBuilt = false;

	// Weather blending recomputes byte-identical values unless one of these moves.
	float lastWeatherBlendFactor = -1.0f;
	uint32_t lastCurrentWeatherID = UINT32_MAX;
	uint32_t lastLastWeatherID = UINT32_MAX;
	bool weatherValuesDirty = true;

	int ResolveTechniqueBinding(const std::string& variableName);
	void BuildTimeOfDayGroups();
	static TimePeriod ParseTimePeriod(const std::string& period);
	static float GetPeriodWeight(TimePeriod period);
};

using EffectBase = ExtendedEffect;

#else

using EffectBase = Effect;

#endif
