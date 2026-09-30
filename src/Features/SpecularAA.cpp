#include "SpecularAA.h"

#include "../I18n/I18n.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.specular_aa."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SpecularAA::Settings,
	Enable,
	EnableOnGrass,
	Variance,
	MaxAddedRoughness)

void SpecularAA::RestoreDefaultSettings()
{
	settings = {};
}

void SpecularAA::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SpecularAA::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SpecularAA::DrawSettings()
{
	Util::CheckboxFlag(T(TKEY("enable"), "Enable Specular AA"), settings.Enable);
	ImGui::BeginDisabled(!settings.Enable);
	Util::CheckboxFlag(T(TKEY("enable_on_grass"), "Enable on GGX Grass"), settings.EnableOnGrass);
	ImGui::SliderFloat(T(TKEY("variance"), "Filter Strength"), &settings.Variance, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("max_added_roughness"), "Max Added Roughness"), &settings.MaxAddedRoughness, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();
}
