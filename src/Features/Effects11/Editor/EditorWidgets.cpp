#include "EditorWidgets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>

#include "I18n/I18n.h"
#include "Utils/Format.h"
#include "Utils/UI.h"
#include <unordered_map>

namespace Effects11UI
{
	namespace
	{
		// Large ranges use drag fields to avoid moving hundreds of units per slider pixel.
		constexpr float kSliderMaxRange = 10.0f;
		constexpr int kSliderMaxIntRange = 100;
		constexpr float kLabelColumnWeight = 0.44f;
		constexpr float kValueColumnWeight = 0.56f;

		struct ClipboardState
		{
			enum class Kind
			{
				None,
				Float,
				Color
			} kind = Kind::None;
			float values[Clipboard::kColorComponents] = {};
		} clipboard;
	}

	std::string PrettifyName(std::string_view a_key)
	{
		static const std::unordered_map<std::string_view, const char* (*)()> names = {
			{ "AdaptationMax", [] { return T("feature.effects11.setting.AdaptationMax", "Adaptation Max"); } },
			{ "AdaptationMin", [] { return T("feature.effects11.setting.AdaptationMin", "Adaptation Min"); } },
			{ "AdaptationSensitivity", [] { return T("feature.effects11.setting.AdaptationSensitivity", "Adaptation Sensitivity"); } },
			{ "AdaptationTime", [] { return T("feature.effects11.setting.AdaptationTime", "Adaptation Time"); } },
			{ "AirGlowIntensity", [] { return T("feature.effects11.setting.AirGlowIntensity", "Air Glow Intensity"); } },
			{ "AirGlowRange", [] { return T("feature.effects11.setting.AirGlowRange", "Air Glow Range"); } },
			{ "AmbientInfluence", [] { return T("feature.effects11.setting.AmbientInfluence", "Ambient Influence"); } },
			{ "AmbientLightingDesaturation", [] { return T("feature.effects11.setting.AmbientLightingDesaturation", "Ambient Lighting Desaturation"); } },
			{ "AmbientLightingIntensity", [] { return T("feature.effects11.setting.AmbientLightingIntensity", "Ambient Lighting Intensity"); } },
			{ "Amount", [] { return T("feature.effects11.setting.Amount", "Amount"); } },
			{ "ApertureTime", [] { return T("feature.effects11.setting.ApertureTime", "Aperture Time"); } },
			{ "AtmosphereThickness", [] { return T("feature.effects11.setting.AtmosphereThickness", "Atmosphere Thickness"); } },
			{ "AuroraBorealisCurve", [] { return T("feature.effects11.setting.AuroraBorealisCurve", "Aurora Borealis Curve"); } },
			{ "AuroraBorealisIntensity", [] { return T("feature.effects11.setting.AuroraBorealisIntensity", "Aurora Borealis Intensity"); } },
			{ "Brightness", [] { return T("feature.effects11.setting.Brightness", "Brightness"); } },
			{ "CalculateCloudsEdgeFromScattering", [] { return T("feature.effects11.setting.CalculateCloudsEdgeFromScattering", "Calculate Clouds Edge From Scattering"); } },
			{ "CloudsColorFilter", [] { return T("feature.effects11.setting.CloudsColorFilter", "Clouds Color Filter"); } },
			{ "CloudsCurve", [] { return T("feature.effects11.setting.CloudsCurve", "Clouds Curve"); } },
			{ "CloudsDesaturation", [] { return T("feature.effects11.setting.CloudsDesaturation", "Clouds Desaturation"); } },
			{ "CloudsEdgeClamp", [] { return T("feature.effects11.setting.CloudsEdgeClamp", "Clouds Edge Clamp"); } },
			{ "CloudsEdgeFadeRange", [] { return T("feature.effects11.setting.CloudsEdgeFadeRange", "Clouds Edge Fade Range"); } },
			{ "CloudsEdgeIntensity", [] { return T("feature.effects11.setting.CloudsEdgeIntensity", "Clouds Edge Intensity"); } },
			{ "CloudsEdgeMoonMultiplier", [] { return T("feature.effects11.setting.CloudsEdgeMoonMultiplier", "Clouds Edge Moon Multiplier"); } },
			{ "CloudsIntensity", [] { return T("feature.effects11.setting.CloudsIntensity", "Clouds Intensity"); } },
			{ "CloudsLightingDensity", [] { return T("feature.effects11.setting.CloudsLightingDensity", "Clouds Lighting Density"); } },
			{ "CloudsLightingDesaturation", [] { return T("feature.effects11.setting.CloudsLightingDesaturation", "Clouds Lighting Desaturation"); } },
			{ "CloudsLightingForwardScattering", [] { return T("feature.effects11.setting.CloudsLightingForwardScattering", "Clouds Lighting Forward Scattering"); } },
			{ "CloudsLightingMoonIntensity", [] { return T("feature.effects11.setting.CloudsLightingMoonIntensity", "Clouds Lighting Moon Intensity"); } },
			{ "CloudsLightingSunMinIntensity", [] { return T("feature.effects11.setting.CloudsLightingSunMinIntensity", "Clouds Lighting Sun Min Intensity"); } },
			{ "CloudsLightingSunMultiplier", [] { return T("feature.effects11.setting.CloudsLightingSunMultiplier", "Clouds Lighting Sun Multiplier"); } },
			{ "CloudsOpacity", [] { return T("feature.effects11.setting.CloudsOpacity", "Clouds Opacity"); } },
			{ "CloudsVertexAlphaBoost", [] { return T("feature.effects11.setting.CloudsVertexAlphaBoost", "Clouds Vertex Alpha Boost"); } },
			{ "ColorFilter", [] { return T("feature.effects11.setting.ColorFilter", "Color Filter"); } },
			{ "ColorFromSun", [] { return T("feature.effects11.setting.ColorFromSun", "Color From Sun"); } },
			{ "ColorPow", [] { return T("feature.effects11.setting.ColorPow", "Color Pow"); } },
			{ "Curve", [] { return T("feature.effects11.setting.Curve", "Curve"); } },
			{ "DawnDuration", [] { return T("feature.effects11.setting.DawnDuration", "Dawn Duration"); } },
			{ "DayTime", [] { return T("feature.effects11.setting.DayTime", "Day Time"); } },
			{ "Density", [] { return T("feature.effects11.setting.Density", "Density"); } },
			{ "Desaturation", [] { return T("feature.effects11.setting.Desaturation", "Desaturation"); } },
			{ "DirectLightingColorFilter", [] { return T("feature.effects11.setting.DirectLightingColorFilter", "Direct Lighting Color Filter"); } },
			{ "DirectLightingColorFilterAmount", [] { return T("feature.effects11.setting.DirectLightingColorFilterAmount", "Direct Lighting Color Filter Amount"); } },
			{ "DirectLightingCurve", [] { return T("feature.effects11.setting.DirectLightingCurve", "Direct Lighting Curve"); } },
			{ "DirectLightingDesaturation", [] { return T("feature.effects11.setting.DirectLightingDesaturation", "Direct Lighting Desaturation"); } },
			{ "DirectLightingIntensity", [] { return T("feature.effects11.setting.DirectLightingIntensity", "Direct Lighting Intensity"); } },
			{ "DisableWrongSkyMath", [] { return T("feature.effects11.setting.DisableWrongSkyMath", "Disable Wrong Sky Math"); } },
			{ "DuskDuration", [] { return T("feature.effects11.setting.DuskDuration", "Dusk Duration"); } },
			{ "DustDarkening", [] { return T("feature.effects11.setting.DustDarkening", "Dust Darkening"); } },
			{ "DustDensity", [] { return T("feature.effects11.setting.DustDensity", "Dust Density"); } },
			{ "DustVolume", [] { return T("feature.effects11.setting.DustVolume", "Dust Volume"); } },
			{ "EdgeSoftness", [] { return T("feature.effects11.setting.EdgeSoftness", "Edge Softness"); } },
			{ "Enable", [] { return T("feature.effects11.setting.Enable", "Enable"); } },
			{ "EnableAdaptation", [] { return T("feature.effects11.setting.EnableAdaptation", "Enable Adaptation"); } },
			{ "EnableAnimatedStars", [] { return T("feature.effects11.setting.EnableAnimatedStars", "Enable Animated Stars"); } },
			{ "EnableBloom", [] { return T("feature.effects11.setting.EnableBloom", "Enable Bloom"); } },
			{ "EnableCloudShadows", [] { return T("feature.effects11.setting.EnableCloudShadows", "Enable Cloud Shadows"); } },
			{ "EnableCloudsLightingFromMoon", [] { return T("feature.effects11.setting.EnableCloudsLightingFromMoon", "Enable Clouds Lighting From Moon"); } },
			{ "EnableCloudsScattering", [] { return T("feature.effects11.setting.EnableCloudsScattering", "Enable Clouds Scattering"); } },
			{ "EnableDepthOfField", [] { return T("feature.effects11.setting.EnableDepthOfField", "Enable Depth Of Field"); } },
			{ "EnableImageBasedLighting", [] { return T("feature.effects11.setting.EnableImageBasedLighting", "Enable Image Based Lighting"); } },
			{ "EnableLens", [] { return T("feature.effects11.setting.EnableLens", "Enable Lens"); } },
			{ "EnableLighting", [] { return T("feature.effects11.setting.EnableLighting", "Enable Lighting"); } },
			{ "EnableLocationWeather", [] { return T("feature.effects11.setting.EnableLocationWeather", "Enable Location Weather"); } },
			{ "EnableMoonRays", [] { return T("feature.effects11.setting.EnableMoonRays", "Enable Moon Rays"); } },
			{ "EnableMultipleWeathers", [] { return T("feature.effects11.setting.EnableMultipleWeathers", "Enable Multiple Weathers"); } },
			{ "EnablePostPassShader", [] { return T("feature.effects11.setting.EnablePostPassShader", "Enable Post Pass Shader"); } },
			{ "EnableProceduralSun", [] { return T("feature.effects11.setting.EnableProceduralSun", "Enable Procedural Sun"); } },
			{ "EnableSunRays", [] { return T("feature.effects11.setting.EnableSunRays", "Enable Sun Rays"); } },
			{ "EnableVolumetricRays", [] { return T("feature.effects11.setting.EnableVolumetricRays", "Enable Volumetric Rays"); } },
			{ "EnableWater", [] { return T("feature.effects11.setting.EnableWater", "Enable Water"); } },
			{ "FocusingTime", [] { return T("feature.effects11.setting.FocusingTime", "Focusing Time"); } },
			{ "FogAmountMultiplier", [] { return T("feature.effects11.setting.FogAmountMultiplier", "Fog Amount Multiplier"); } },
			{ "FogColorCurve", [] { return T("feature.effects11.setting.FogColorCurve", "Fog Color Curve"); } },
			{ "FogColorFilter", [] { return T("feature.effects11.setting.FogColorFilter", "Fog Color Filter"); } },
			{ "FogColorFilterAmount", [] { return T("feature.effects11.setting.FogColorFilterAmount", "Fog Color Filter Amount"); } },
			{ "FogColorMultiplier", [] { return T("feature.effects11.setting.FogColorMultiplier", "Fog Color Multiplier"); } },
			{ "FogCurveMultiplier", [] { return T("feature.effects11.setting.FogCurveMultiplier", "Fog Curve Multiplier"); } },
			{ "ForceMinMaxValues", [] { return T("feature.effects11.setting.ForceMinMaxValues", "Force Min Max Values"); } },
			{ "FresnelMax", [] { return T("feature.effects11.setting.FresnelMax", "Fresnel Max"); } },
			{ "FresnelMin", [] { return T("feature.effects11.setting.FresnelMin", "Fresnel Min"); } },
			{ "FresnelMultiplier", [] { return T("feature.effects11.setting.FresnelMultiplier", "Fresnel Multiplier"); } },
			{ "GammaCurve", [] { return T("feature.effects11.setting.GammaCurve", "Gamma Curve"); } },
			{ "GlowCurve", [] { return T("feature.effects11.setting.GlowCurve", "Glow Curve"); } },
			{ "GlowIntensity", [] { return T("feature.effects11.setting.GlowIntensity", "Glow Intensity"); } },
			{ "GradientDesaturation", [] { return T("feature.effects11.setting.GradientDesaturation", "Gradient Desaturation"); } },
			{ "GradientHorizonColorFilter", [] { return T("feature.effects11.setting.GradientHorizonColorFilter", "Gradient Horizon Color Filter"); } },
			{ "GradientHorizonCurve", [] { return T("feature.effects11.setting.GradientHorizonCurve", "Gradient Horizon Curve"); } },
			{ "GradientHorizonIntensity", [] { return T("feature.effects11.setting.GradientHorizonIntensity", "Gradient Horizon Intensity"); } },
			{ "GradientIntensity", [] { return T("feature.effects11.setting.GradientIntensity", "Gradient Intensity"); } },
			{ "GradientMiddleColorFilter", [] { return T("feature.effects11.setting.GradientMiddleColorFilter", "Gradient Middle Color Filter"); } },
			{ "GradientMiddleCurve", [] { return T("feature.effects11.setting.GradientMiddleCurve", "Gradient Middle Curve"); } },
			{ "GradientMiddleIntensity", [] { return T("feature.effects11.setting.GradientMiddleIntensity", "Gradient Middle Intensity"); } },
			{ "GradientTopColorFilter", [] { return T("feature.effects11.setting.GradientTopColorFilter", "Gradient Top Color Filter"); } },
			{ "GradientTopCurve", [] { return T("feature.effects11.setting.GradientTopCurve", "Gradient Top Curve"); } },
			{ "GradientTopIntensity", [] { return T("feature.effects11.setting.GradientTopIntensity", "Gradient Top Intensity"); } },
			{ "HorizonRange", [] { return T("feature.effects11.setting.HorizonRange", "Horizon Range"); } },
			{ "Intensity", [] { return T("feature.effects11.setting.Intensity", "Intensity"); } },
			{ "LightingInfluence", [] { return T("feature.effects11.setting.LightingInfluence", "Lighting Influence"); } },
			{ "MoonColorFilter", [] { return T("feature.effects11.setting.MoonColorFilter", "Moon Color Filter"); } },
			{ "MoonCurve", [] { return T("feature.effects11.setting.MoonCurve", "Moon Curve"); } },
			{ "MoonDesaturation", [] { return T("feature.effects11.setting.MoonDesaturation", "Moon Desaturation"); } },
			{ "MoonGlowAmount", [] { return T("feature.effects11.setting.MoonGlowAmount", "Moon Glow Amount"); } },
			{ "MoonGlowRange", [] { return T("feature.effects11.setting.MoonGlowRange", "Moon Glow Range"); } },
			{ "MoonIntensity", [] { return T("feature.effects11.setting.MoonIntensity", "Moon Intensity"); } },
			{ "MoonRaysMultiplier", [] { return T("feature.effects11.setting.MoonRaysMultiplier", "Moon Rays Multiplier"); } },
			{ "MotionStretch", [] { return T("feature.effects11.setting.MotionStretch", "Motion Stretch"); } },
			{ "MotionTransparency", [] { return T("feature.effects11.setting.MotionTransparency", "Motion Transparency"); } },
			{ "Muddiness", [] { return T("feature.effects11.setting.Muddiness", "Muddiness"); } },
			{ "MultiplicativeAmount", [] { return T("feature.effects11.setting.MultiplicativeAmount", "Multiplicative Amount"); } },
			{ "NightTime", [] { return T("feature.effects11.setting.NightTime", "Night Time"); } },
			{ "Opacity", [] { return T("feature.effects11.setting.Opacity", "Opacity"); } },
			{ "PointLightingCurve", [] { return T("feature.effects11.setting.PointLightingCurve", "Point Lighting Curve"); } },
			{ "PointLightingDesaturation", [] { return T("feature.effects11.setting.PointLightingDesaturation", "Point Lighting Desaturation"); } },
			{ "PointLightingInfluence", [] { return T("feature.effects11.setting.PointLightingInfluence", "Point Lighting Influence"); } },
			{ "PointLightingIntensity", [] { return T("feature.effects11.setting.PointLightingIntensity", "Point Lighting Intensity"); } },
			{ "ProceduralGradientWeightCurve", [] { return T("feature.effects11.setting.ProceduralGradientWeightCurve", "Procedural Gradient Weight Curve"); } },
			{ "Quality", [] { return T("feature.effects11.setting.Quality", "Quality"); } },
			{ "RangeFactor", [] { return T("feature.effects11.setting.RangeFactor", "Range Factor"); } },
			{ "ReflectionAmount", [] { return T("feature.effects11.setting.ReflectionAmount", "Reflection Amount"); } },
			{ "ScatteringColor", [] { return T("feature.effects11.setting.ScatteringColor", "Scattering Color"); } },
			{ "ShadowAmount", [] { return T("feature.effects11.setting.ShadowAmount", "Shadow Amount"); } },
			{ "Size", [] { return T("feature.effects11.setting.Size", "Size"); } },
			{ "SkyColorAmount", [] { return T("feature.effects11.setting.SkyColorAmount", "Sky Color Amount"); } },
			{ "StarsAnimationDensity", [] { return T("feature.effects11.setting.StarsAnimationDensity", "Stars Animation Density"); } },
			{ "StarsAnimationIntensity", [] { return T("feature.effects11.setting.StarsAnimationIntensity", "Stars Animation Intensity"); } },
			{ "StarsAnimationTime", [] { return T("feature.effects11.setting.StarsAnimationTime", "Stars Animation Time"); } },
			{ "StarsCurve", [] { return T("feature.effects11.setting.StarsCurve", "Stars Curve"); } },
			{ "StarsIntensity", [] { return T("feature.effects11.setting.StarsIntensity", "Stars Intensity"); } },
			{ "SunColorFilter", [] { return T("feature.effects11.setting.SunColorFilter", "Sun Color Filter"); } },
			{ "SunDesaturation", [] { return T("feature.effects11.setting.SunDesaturation", "Sun Desaturation"); } },
			{ "SunGlowIntensity", [] { return T("feature.effects11.setting.SunGlowIntensity", "Sun Glow Intensity"); } },
			{ "SunGlowRange", [] { return T("feature.effects11.setting.SunGlowRange", "Sun Glow Range"); } },
			{ "SunIntensity", [] { return T("feature.effects11.setting.SunIntensity", "Sun Intensity"); } },
			{ "SunLightingMultiplier", [] { return T("feature.effects11.setting.SunLightingMultiplier", "Sun Lighting Multiplier"); } },
			{ "SunRaysMultiplier", [] { return T("feature.effects11.setting.SunRaysMultiplier", "Sun Rays Multiplier"); } },
			{ "SunSpecularMultiplier", [] { return T("feature.effects11.setting.SunSpecularMultiplier", "Sun Specular Multiplier"); } },
			{ "SunriseTime", [] { return T("feature.effects11.setting.SunriseTime", "Sunrise Time"); } },
			{ "SunsetTime", [] { return T("feature.effects11.setting.SunsetTime", "Sunset Time"); } },
			{ "Type", [] { return T("feature.effects11.setting.Type", "Type"); } },
			{ "UseEffect", [] { return T("feature.effects11.setting.UseEffect", "Use Effect"); } },
			{ "UseLinearMath", [] { return T("feature.effects11.setting.UseLinearMath", "Use Linear Math"); } },
			{ "UseOriginalPostProcessing", [] { return T("feature.effects11.setting.UseOriginalPostProcessing", "Use Original Post Processing"); } },
			{ "UseProceduralGradientWeights", [] { return T("feature.effects11.setting.UseProceduralGradientWeights", "Use Procedural Gradient Weights"); } },
			{ "WavesAmplitude", [] { return T("feature.effects11.setting.WavesAmplitude", "Waves Amplitude"); } },
		};
		if (const auto it = names.find(a_key); it != names.end())
			return it->second();
		return Util::PrettifyIdentifier(a_key);
	}

	bool ContainsNoCase(std::string_view a_text, std::string_view a_filter)
	{
		return Util::StringMatchesSearch(a_text, a_filter);
	}

	int DecimalsForStep(float a_step)
	{
		if (!(a_step > 0.0f))
			return 3;
		const int decimals = static_cast<int>(std::ceil(-std::log10(a_step) - 1e-4f));
		return std::clamp(decimals, 0, 4);
	}

	std::string MakeFormat(int a_decimals, std::string_view a_prefix)
	{
		std::string format;
		// '%' in a (translated) prefix would be read as a conversion
		for (char c : a_prefix) {
			if (c == '%')
				format += '%';
			format += c;
		}
		if (!a_prefix.empty())
			format += "  ";
		format += "%." + std::to_string(std::clamp(a_decimals, 0, 6)) + "f";
		return format;
	}

	bool BeginPropertyTable(const char* a_id)
	{
		constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
		if (!ImGui::BeginTable(a_id, 2, flags))
			return false;
		ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, kLabelColumnWeight);
		ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, kValueColumnWeight);
		return true;
	}

	void EndPropertyTable()
	{
		ImGui::EndTable();
	}

	bool PropertyLabel(const char* a_label, bool a_dimmed, const LabelBadge& a_badge)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		if (a_dimmed)
			Util::TextUnformattedDisabled(a_label);
		else
			ImGui::TextUnformatted(a_label);
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		if (a_badge.text)
			Badge(a_badge.text, a_badge.color, a_badge.tooltip);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-FLT_MIN);
		return hovered;
	}

	void PropertyContinuation()
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-FLT_MIN);
	}

	bool FloatValue(const char* a_id, float* a_value, float a_min, float a_max, float a_step, const char* a_format)
	{
		constexpr ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
		const float range = a_max - a_min;
		if (range > 0.0f && range <= kSliderMaxRange)
			return ImGui::SliderFloat(a_id, a_value, a_min, a_max, a_format, flags);
		const float speed = a_step > 0.0f ? a_step : (std::max)(range / 1000.0f, 0.001f);
		return ImGui::DragFloat(a_id, a_value, speed, a_min, a_max, a_format, flags);
	}

	bool FloatNValue(const char* a_id, float* a_values, int a_components, float a_min, float a_max, const char* a_format)
	{
		constexpr ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
		const int components = std::clamp(a_components, 2, 4);
		const float range = a_max - a_min;
		if (range > 0.0f && range <= kSliderMaxRange)
			return ImGui::SliderScalarN(a_id, ImGuiDataType_Float, a_values, components, &a_min, &a_max, a_format, flags);
		const float speed = (std::max)(range / 1000.0f, 0.001f);
		return ImGui::DragScalarN(a_id, ImGuiDataType_Float, a_values, components, speed, &a_min, &a_max, a_format, flags);
	}

	bool IntValue(const char* a_id, int* a_value, int a_min, int a_max)
	{
		constexpr ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
		if (a_max > a_min && static_cast<int64_t>(a_max) - a_min <= kSliderMaxIntRange)
			return ImGui::SliderInt(a_id, a_value, a_min, a_max, "%d", flags);
		return ImGui::DragInt(a_id, a_value, 1.0f, a_min, a_max, "%d", flags);
	}

	bool ColorValue(const char* a_id, float* a_color, int a_components, bool a_hdr)
	{
		// NoOptions leaves right-click to the parameter's own context menu
		ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoOptions;
		if (a_hdr)
			flags |= ImGuiColorEditFlags_HDR;
		if (a_components >= 4)
			return ImGui::ColorEdit4(a_id, a_color, flags | ImGuiColorEditFlags_AlphaBar);
		return ImGui::ColorEdit3(a_id, a_color, flags);
	}

	void Badge(const char* a_text, const ImVec4& a_color, const char* a_tooltip)
	{
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextColored(a_color, "%s", a_text);
		if (a_tooltip)
			Util::AddTooltip(a_tooltip, ImGuiHoveredFlags_None);
	}

	void HeaderTags(std::initializer_list<std::pair<const char*, ImVec4>> a_tags, float a_rightInset)
	{
		const ImVec2 itemMin = ImGui::GetItemRectMin();
		const ImVec2 itemMax = ImGui::GetItemRectMax();
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float y = itemMin.y + (itemMax.y - itemMin.y - ImGui::GetTextLineHeight()) * 0.5f;
		float x = itemMax.x - ImGui::GetStyle().FramePadding.x - a_rightInset;

		auto* drawList = ImGui::GetWindowDrawList();
		for (auto it = std::rbegin(a_tags); it != std::rend(a_tags); ++it) {
			const auto& [text, color] = *it;
			if (!text || !*text)
				continue;
			x -= ImGui::CalcTextSize(text).x;
			drawList->AddText(ImVec2(x, y), ImGui::GetColorU32(color), text);
			x -= spacing;
		}
	}

	namespace Clipboard
	{
		void SetFloat(float a_value)
		{
			clipboard.kind = ClipboardState::Kind::Float;
			clipboard.values[0] = a_value;
			ImGui::SetClipboardText(std::format("{:.4f}", a_value).c_str());
		}

		void SetColor(const float* a_rgb)
		{
			clipboard.kind = ClipboardState::Kind::Color;
			std::copy_n(a_rgb, kColorComponents, clipboard.values);
			ImGui::SetClipboardText(std::format("{:.4f}, {:.4f}, {:.4f}", a_rgb[0], a_rgb[1], a_rgb[2]).c_str());
		}

		bool HasFloat() { return clipboard.kind == ClipboardState::Kind::Float; }
		bool HasColor() { return clipboard.kind == ClipboardState::Kind::Color; }
		float GetFloat() { return clipboard.values[0]; }
		void GetColor(float* a_rgb) { std::copy_n(clipboard.values, kColorComponents, a_rgb); }
	}
}
