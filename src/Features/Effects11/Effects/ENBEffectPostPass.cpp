#include "ENBEffectPostPass.h"

#include "../EffectManager.h"
#include "../TextureManager.h"

void ENBEffectPostPass::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto textureSDRTemp = textureManager.GetCommonTexture("TextureSDRTemp");
	auto textureSDRTemp2 = textureManager.GetCommonTexture("TextureSDRTemp2");
	auto scratchIt = effectTextureCache.find("TextureScratch");

	if (!textureSDRTemp || !textureSDRTemp2 || scratchIt == effectTextureCache.end() || !scratchIt->second.texture) {
		return;
	}

	D3D11_TEXTURE2D_DESC current{}, scratch{};
	textureSDRTemp->texture->GetDesc(&current);
	scratchIt->second.texture->GetDesc(&scratch);
	if (current.Width != scratch.Width || current.Height != scratch.Height || current.Format != scratch.Format)
		CreateEffectTextures();
	auto& textureScratch = scratchIt->second;
	auto [executed, inOutput, inTemp] = ExecuteTechniqueSequence(GetSelectedTechnique(), EffectManager::GetSingleton().GetEyeCroppedSRV(*textureSDRTemp), *textureSDRTemp2, textureScratch);

	if (executed && inOutput) {
		textureManager.SwapTextures("TextureSDRTemp", "TextureSDRTemp2");
	} else if (executed && inTemp) {
		auto& manager = EffectManager::GetSingleton();
		manager.CopyTexture(manager.GetEyeCroppedSRV(textureScratch), textureSDRTemp->rtv.get(), false, manager.currentEyeIndex);
	}
}

void ENBEffectPostPass::UpdateEffectVariables()
{
	auto* textureSDRTemp = GetCachedCommonTexture("TextureSDRTemp");
	SetShaderResourceVariable("TextureOriginal", textureSDRTemp ? EffectManager::GetSingleton().GetEyeCroppedSRV(*textureSDRTemp) : nullptr);
}

void ENBEffectPostPass::CreateEffectTextures()
{
	auto* textureSDRTemp = TextureManager::GetSingleton().GetCommonTexture("TextureSDRTemp");
	if (!textureSDRTemp || !textureSDRTemp->texture)
		return;

	D3D11_TEXTURE2D_DESC desc;
	textureSDRTemp->texture->GetDesc(&desc);
	effectTextureCache["TextureScratch"] = CreateTexture(desc.Width, desc.Height, desc.Format, "ENBEffectPostPass::TextureScratch");
}
