#pragma once

#include "Buffer.h"

struct SpecularAA : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	// Metadata
	virtual inline std::string GetName() override { return "Specular AA"; }
	virtual std::string GetDisplayName() override { return T("feature.specular_aa.name", "Specular AA"); }
	virtual inline std::string GetShortName() override { return "SpecularAA"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.specular_aa.description", "Reduces shimmering of small, sharp highlights on bumpy and curved surfaces."),
			{ T("feature.specular_aa.key_feature_1", "Widens roughness by the normal variation within each pixel"),
				T("feature.specular_aa.key_feature_2", "Works on vanilla, GGX and PBR specular"),
				T("feature.specular_aa.key_feature_3", "Steadier highlights under TAA and upscalers") }
		};
	}

	// Functionality
	virtual bool inline IsCore() const override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "SPECULAR_AA"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type a_type) override
	{
		return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Grass;
	}
	virtual bool SupportsVR() override { return true; }

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	struct alignas(16) Settings
	{
		uint Enable = false;
		uint EnableOnGrass = true;
		float Variance = 0.15f;
		float MaxAddedRoughness = 0.67f;
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);

	Settings settings;
};
