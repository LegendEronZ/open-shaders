#include "ENBEffectPostPass.h"

#include "../EffectManager.h"
#include "../TextureManager.h"

void ENBEffectPostPass::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto textureSDRTemp = textureManager.GetCommonTexture("TextureSDRTemp");
	auto textureSDRTemp2 = textureManager.GetCommonTexture("TextureSDRTemp2");

	if (!textureSDRTemp || !textureSDRTemp2) {
		return;
	}

	// TextureSDRTemp spans the full packed SBS buffer; the first pass here (unlike
	// mid-sequence passes) doesn't go through ExecuteTechniqueSequence's own
	// GetEyeCroppedSRV, so crop it explicitly or this eye's pass samples both eyes.
	auto* inputSRV = EffectManager::GetSingleton().GetEyeCroppedSRV(*textureSDRTemp);
	auto [executed, inOutput] = ExecuteTechniqueSequence(GetSelectedTechnique(), inputSRV, *textureSDRTemp2, *textureSDRTemp);

	if (executed && inOutput) {
		textureManager.SwapTextures("TextureSDRTemp", "TextureSDRTemp2");
	}
}

void ENBEffectPostPass::UpdateEffectVariables()
{
	auto* textureSDRTemp = GetCachedCommonTexture("TextureSDRTemp");
	SetShaderResourceVariable("TextureOriginal", textureSDRTemp ? EffectManager::GetSingleton().GetEyeCroppedSRV(*textureSDRTemp) : nullptr);
}