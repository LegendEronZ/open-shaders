#include "CSEditor.h"
#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.cs_editor."

#include "State.h"
#include "Util.h"
#include "Utils/UI.h"

#include "CSEditor/EditorWindow.h"
#include <cstring>
#include <filesystem>

namespace
{
	constexpr const char* kJsonExtension = ".json";
}

void CSEditor::DataLoaded()
{
	s_dataAvailable = true;
}

bool CSEditor::HasWidgetJsonFiles()
{
	if (s_checkedWidgetJsonFiles)
		return s_hasWidgetJsonFiles;

	const auto communityShaderPath = Util::PathHelpers::GetCommunityShaderPath();
	for (const auto folderName : Widget::kSaveFolderNames) {
		const auto widgetSettingsPath = communityShaderPath / std::filesystem::path(folderName);
		std::error_code ec;
		const bool isDirectory = std::filesystem::is_directory(widgetSettingsPath, ec);
		if (ec) {
			// A missing folder is the normal case (the user simply has no saved
			// widgets for this category), so don't treat it as a warning.
			if (ec != std::errc::no_such_file_or_directory)
				logger::warn("[CSEditor] Failed to inspect widget settings path '{}': {}", widgetSettingsPath.string(), ec.message());
			continue;
		}
		if (!isDirectory)
			continue;

		for (std::filesystem::directory_iterator it(widgetSettingsPath, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code entryEc;
			const bool isRegularFile = it->is_regular_file(entryEc);
			if (entryEc) {
				logger::warn("[CSEditor] Failed to inspect widget settings file '{}': {}", it->path().string(), entryEc.message());
				continue;
			}
			if (isRegularFile && _stricmp(it->path().extension().string().c_str(), kJsonExtension) == 0) {
				logger::info("[CSEditor] Detected widget settings in '{}'", widgetSettingsPath.string());
				s_hasWidgetJsonFiles = true;
				s_checkedWidgetJsonFiles = true;
				return true;
			}
		}
		if (ec) {
			logger::warn("[CSEditor] Failed to scan widget settings path '{}': {}", widgetSettingsPath.string(), ec.message());
			continue;
		}
	}

	s_checkedWidgetJsonFiles = true;
	return false;
}

bool CSEditor::ShouldPreloadEditorResources()
{
	return s_dataAvailable && !s_resourcesInitialized && EditorWindow::CanBeOpen() && HasWidgetJsonFiles();
}

void CSEditor::EnsureDataLoaded()
{
	if (!s_dataAvailable)
		return;

	if (!s_resourcesInitialized) {
		EditorWindow::GetSingleton()->SetupResources();
		s_resourcesInitialized = true;
	}
}

void CSEditor::OpenEditorWindow()
{
	if (!EditorWindow::CanBeOpen())
		return;

	EnsureDataLoaded();
	EditorWindow::GetSingleton()->open = true;
}

void CSEditor::ToggleEditorWindow()
{
	auto* editorWindow = EditorWindow::GetSingleton();
	if (!editorWindow)
		return;

	if (!editorWindow->open && !EditorWindow::CanBeOpen())
		return;
	if (!editorWindow->open)
		EnsureDataLoaded();
	editorWindow->open = !editorWindow->open;
}

int8_t LerpInt8_t(const int8_t oldValue, const int8_t newVal, const float lerpValue)
{
	int lerpedValue = (int)std::lerp(oldValue, newVal, lerpValue);
	return (int8_t)std::clamp(lerpedValue, -128, 127);
}

uint8_t LerpUint8_t(const uint8_t oldValue, const uint8_t newVal, const float lerpValue)
{
	int lerpedValue = (int)std::lerp(oldValue, newVal, lerpValue);
	return (uint8_t)std::clamp(lerpedValue, 0, 255);
}

void LerpColor(const RE::TESWeather::Data::Color3& oldColor, RE::TESWeather::Data::Color3& newColor, const float changePct)
{
	newColor.red = LerpInt8_t(oldColor.red, newColor.red, changePct);
	newColor.green = LerpInt8_t(oldColor.green, newColor.green, changePct);
	newColor.blue = LerpInt8_t(oldColor.blue, newColor.blue, changePct);
}

void LerpColor(const RE::Color& oldColor, RE::Color& newColor, const float changePct)
{
	newColor.red = LerpUint8_t(oldColor.red, newColor.red, changePct);
	newColor.green = LerpUint8_t(oldColor.green, newColor.green, changePct);
	newColor.blue = LerpUint8_t(oldColor.blue, newColor.blue, changePct);
}

void LerpDirectional(RE::BGSDirectionalAmbientLightingColors::Directional& oldColor, RE::BGSDirectionalAmbientLightingColors::Directional& newColor, const float changePct)
{
	LerpColor(oldColor.x.max, newColor.x.max, changePct);
	LerpColor(oldColor.x.min, newColor.x.min, changePct);
	LerpColor(oldColor.y.max, newColor.y.max, changePct);
	LerpColor(oldColor.y.min, newColor.y.min, changePct);
	LerpColor(oldColor.z.max, newColor.z.max, changePct);
	LerpColor(oldColor.z.min, newColor.z.min, changePct);
}

void CSEditor::DrawLauncherButton()
{
	const bool canOpen = loaded && EditorWindow::CanBeOpen();
	ImGui::BeginDisabled(!canOpen);
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
	if (ImGui::Button(T(TKEY("open_editor"), "Open OS Editor"), { -1, 0 }))
		OpenEditorWindow();
	ImGui::PopStyleVar();
	ImGui::EndDisabled();

	if (!canOpen) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			if (!loaded) {
				ImGui::Text("%s", T(TKEY("open_editor_not_loaded_tooltip"), "OS Editor is not loaded."));
			} else if (globals::state->isLoadingMenuOpen) {
				ImGui::Text("%s", T(TKEY("open_editor_loading_tooltip"), "OS Editor cannot be opened during a loading screen."));
			} else {
				ImGui::Text("%s", T(TKEY("open_editor_enter_world_tooltip"), "Enter the game world to open OS Editor."));
			}
		}
	}
}

void CSEditor::Prepass()
{
	if (ShouldPreloadEditorResources()) {
		EnsureDataLoaded();
	}
	UpdateWeatherLockAndTime();
}

void CSEditor::UpdateWeatherLockAndTime()
{
	auto editorWindow = EditorWindow::GetSingleton();
	if (editorWindow->IsWeatherLocked()) {
		auto lockedWeather = editorWindow->GetLockedWeather();
		auto sky = globals::game::sky;
		if (sky && lockedWeather && sky->currentWeather != lockedWeather) {
			sky->ForceWeather(lockedWeather, false);
		}
	}

	editorWindow->UpdateTimeState();
}

void CSEditor::LerpWeather(RE::TESWeather* oldWeather, RE::TESWeather* newWeather, float currentWeatherPct)
{
	if (!oldWeather || !newWeather) {
		// Avoid dereferencing null pointers; nothing to lerp.
		return;
	}

	//// Precipitation
	newWeather->data.precipitationBeginFadeIn = LerpUint8_t(oldWeather->data.precipitationBeginFadeIn, newWeather->data.precipitationBeginFadeIn, currentWeatherPct);
	newWeather->data.precipitationEndFadeOut = LerpUint8_t(oldWeather->data.precipitationEndFadeOut, newWeather->data.precipitationEndFadeOut, currentWeatherPct);

	//// Sun
	newWeather->data.sunDamage = LerpUint8_t(oldWeather->data.sunDamage, newWeather->data.sunDamage, currentWeatherPct);
	newWeather->data.sunGlare = LerpUint8_t(oldWeather->data.sunGlare, newWeather->data.sunGlare, currentWeatherPct);

	//// Lightning
	newWeather->data.thunderLightningBeginFadeIn = LerpUint8_t(oldWeather->data.thunderLightningBeginFadeIn, newWeather->data.thunderLightningBeginFadeIn, currentWeatherPct);
	newWeather->data.thunderLightningEndFadeOut = LerpUint8_t(oldWeather->data.thunderLightningEndFadeOut, newWeather->data.thunderLightningEndFadeOut, currentWeatherPct);
	newWeather->data.thunderLightningFrequency = (int8_t)LerpUint8_t((uint8_t)oldWeather->data.thunderLightningFrequency, (uint8_t)newWeather->data.thunderLightningFrequency, currentWeatherPct);
	LerpColor(oldWeather->data.lightningColor, newWeather->data.lightningColor, currentWeatherPct);

	//// Trans delta
	newWeather->data.transDelta = LerpUint8_t(oldWeather->data.transDelta, newWeather->data.transDelta, currentWeatherPct);

	//// Visual Effects
	newWeather->data.visualEffectBegin = LerpUint8_t(oldWeather->data.visualEffectBegin, newWeather->data.visualEffectBegin, currentWeatherPct);
	newWeather->data.visualEffectEnd = LerpUint8_t(oldWeather->data.visualEffectEnd, newWeather->data.visualEffectEnd, currentWeatherPct);

	//// Wind
	newWeather->data.windDirection = LerpUint8_t(oldWeather->data.windDirection, newWeather->data.windDirection, currentWeatherPct);
	newWeather->data.windDirectionRange = LerpUint8_t(oldWeather->data.windDirectionRange, newWeather->data.windDirectionRange, currentWeatherPct);
	newWeather->data.windSpeed = LerpUint8_t(oldWeather->data.windSpeed, newWeather->data.windSpeed, currentWeatherPct);

	//// Fog
	newWeather->fogData.dayFar = std::lerp(oldWeather->fogData.dayFar, newWeather->fogData.dayFar, currentWeatherPct);
	newWeather->fogData.dayMax = std::lerp(oldWeather->fogData.dayMax, newWeather->fogData.dayMax, currentWeatherPct);
	newWeather->fogData.dayNear = std::lerp(oldWeather->fogData.dayNear, newWeather->fogData.dayNear, currentWeatherPct);
	newWeather->fogData.dayPower = std::lerp(oldWeather->fogData.dayPower, newWeather->fogData.dayPower, currentWeatherPct);

	newWeather->fogData.nightFar = std::lerp(oldWeather->fogData.nightFar, newWeather->fogData.nightFar, currentWeatherPct);
	newWeather->fogData.nightMax = std::lerp(oldWeather->fogData.nightMax, newWeather->fogData.nightMax, currentWeatherPct);
	newWeather->fogData.nightNear = std::lerp(oldWeather->fogData.nightNear, newWeather->fogData.nightNear, currentWeatherPct);
	newWeather->fogData.nightPower = std::lerp(oldWeather->fogData.nightPower, newWeather->fogData.nightPower, currentWeatherPct);

	//// Weather colors
	for (size_t i = 0; i < RE::TESWeather::ColorTypes::kTotal; i++) {
		for (size_t j = 0; j < RE::TESWeather::ColorTime::kTotal; j++) {
			LerpColor(oldWeather->colorData[i][j], newWeather->colorData[i][j], currentWeatherPct);
		}
	}

	//// DALC
	for (size_t i = 0; i < RE::TESWeather::ColorTime::kTotal; i++) {
		auto& newDALC = newWeather->directionalAmbientLightingColors[i];
		auto& oldDALC = oldWeather->directionalAmbientLightingColors[i];

		LerpColor(oldDALC.specular, newDALC.specular, currentWeatherPct);
		newWeather->directionalAmbientLightingColors[i].fresnelPower = std::lerp(oldDALC.fresnelPower, newDALC.fresnelPower, currentWeatherPct);
		LerpDirectional(oldDALC.directional, newDALC.directional, currentWeatherPct);
	}

	//// Clouds
	for (size_t i = 0; i < RE::TESWeather::kTotalLayers; i++) {
		for (size_t j = 0; j < RE::TESWeather::ColorTime::kTotal; j++) {
			LerpColor(oldWeather->cloudColorData[i][j], newWeather->cloudColorData[i][j], currentWeatherPct);
			newWeather->cloudAlpha[i][j] = std::lerp(oldWeather->cloudAlpha[i][j], newWeather->cloudAlpha[i][j], currentWeatherPct);
		}

		newWeather->cloudLayerSpeedY[i] = LerpInt8_t(oldWeather->cloudLayerSpeedY[i], newWeather->cloudLayerSpeedY[i], currentWeatherPct);
		newWeather->cloudLayerSpeedX[i] = LerpInt8_t(oldWeather->cloudLayerSpeedX[i], newWeather->cloudLayerSpeedX[i], currentWeatherPct);
	}
}

#undef I18N_KEY_PREFIX
