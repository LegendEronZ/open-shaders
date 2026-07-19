#pragma once

#include "Feature.h"
#include "I18n/I18n.h"

struct CSEditor : Feature
{
public:
	static CSEditor* GetSingleton()
	{
		static CSEditor singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "CS Editor"; }
	virtual std::string GetDisplayName() override { return T("feature.cs_editor.name", "OS Editor"); }
	virtual inline std::string GetShortName() override { return "CSEditor"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CS_EDITOR"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.cs_editor.description", "Development tool for inspecting, editing, and previewing renderer-facing data in-game."),
			{ T("feature.cs_editor.key_feature_1", "Provides weather editing functionality"),
				T("feature.cs_editor.key_feature_2", "Includes dynamic saving and loading of vanilla post processing and weather settings."),
				T("feature.cs_editor.key_feature_3", "Real-time editing and previewing of effects") } };
	}

	virtual void DataLoaded() override;
	virtual void Prepass() override;

	void DrawLauncherButton();

	static void OpenEditorWindow();
	static void ToggleEditorWindow();
	static void UpdateWeatherLockAndTime();

	void LerpWeather(RE::TESWeather*, RE::TESWeather*, float);

private:
	static inline bool s_dataAvailable = false;
	static inline bool s_resourcesInitialized = false;
	static inline bool s_checkedWidgetJsonFiles = false;
	static inline bool s_hasWidgetJsonFiles = false;

	static bool HasWidgetJsonFiles();
	static bool ShouldPreloadEditorResources();
	static void EnsureDataLoaded();
};
