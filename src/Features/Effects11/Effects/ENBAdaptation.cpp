#include "ENBAdaptation.h"

#include "../SettingManager.h"
#include "../TextureManager.h"
#include "Globals.h"

namespace
{
	constexpr float kNeutralExposure[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
}

void ENBAdaptation::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto* currentSRV = textureManager.GetDownsampleTextureBlurry();
	if (!currentSRV) {
		return;
	}

	SetShaderResourceVariable("TextureCurrent", currentSRV);

	if (!textureCurrent.texture)
		return;

	auto* downsampleTechnique = effect->GetTechniqueByName("Downsample");
	if (!downsampleTechnique || !downsampleTechnique->IsValid())
		return;
	ExecuteTechnique("Downsample", textureCurrent);

	SetShaderResourceVariable("TextureCurrent", textureCurrent.srv.get());

	auto* texturePrevious = adaptationTextures[historyIndex];
	auto* textureAdaptation = adaptationTextures[historyIndex ^ 1];
	if (!texturePrevious || !textureAdaptation)
		return;
	SetShaderResourceVariable("TexturePrevious", texturePrevious->srv.get());
	auto* drawTechnique = effect->GetTechniqueByName("Draw");
	if (!drawTechnique || !drawTechnique->IsValid())
		return;
	ExecuteTechnique("Draw", *textureAdaptation);
	historyIndex ^= 1;
	historyValid = true;
}

ID3D11ShaderResourceView* ENBAdaptation::GetHistorySRV() const
{
	return IsCompiled() && adaptationTextures[historyIndex] ? adaptationTextures[historyIndex]->srv.get() : nullptr;
}

void ENBAdaptation::UpdateEffectVariables()
{
	auto& settingManager = SettingManager::GetSingleton();

	if (!idsCached) {
		idForceMinMax = settingManager.GetSettingID("ForceMinMaxValues", "ADAPTATION");
		idAdaptTime = settingManager.GetSettingID("AdaptationTime", "ADAPTATION");
		idAdaptMin = settingManager.GetSettingID("AdaptationMin", "ADAPTATION");
		idAdaptMax = settingManager.GetSettingID("AdaptationMax", "ADAPTATION");
		idAdaptSens = settingManager.GetSettingID("AdaptationSensitivity", "ADAPTATION");
		idsCached = true;
	}

	auto forceMinMaxValues = settingManager.GetValue<bool>(idForceMinMax);

	float adaptationTime = settingManager.GetValue<float>(idAdaptTime);
	float deltaTime = (globals::game::deltaTime) ? (*globals::game::deltaTime) : 0.0f;

	float4 adaptationParameters{};
	adaptationParameters.x = !forceMinMaxValues ? 0.0f : settingManager.GetValue<float>(idAdaptMin);
	adaptationParameters.y = !forceMinMaxValues ? 65535.0f : settingManager.GetValue<float>(idAdaptMax);
	adaptationParameters.z = settingManager.GetValue<float>(idAdaptSens);
	adaptationParameters.w = historyValid ? std::clamp((adaptationTime > 0.0f) ? (deltaTime / adaptationTime) : 1.0f, 0.0f, 1.0f) : 1.0f;

	SetVectorVariable("AdaptationParameters", &adaptationParameters, sizeof(adaptationParameters));
}

void ENBAdaptation::CreateEffectTextures()
{
	auto& textureManager = TextureManager::GetSingleton();
	adaptationTextures[0] = textureManager.GetCommonTexture("TextureAdaptation");
	adaptationTextures[1] = textureManager.GetCommonTexture("TextureAdaptationSwap");
	for (const auto* texture : adaptationTextures)
		globals::d3d::context->ClearRenderTargetView(texture->rtv.get(), kNeutralExposure);
	historyIndex = 0;
	historyValid = false;
	textureCurrent = CreateTexture(16, 16, DXGI_FORMAT_R32_FLOAT, "ENBAdaptation::TextureCurrent");
}
