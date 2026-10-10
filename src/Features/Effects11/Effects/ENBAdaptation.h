#pragma once

#include "ExtendedEffect.h"

class ENBAdaptation : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbadaptation.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	/** @brief Returns the last completed exposure, shared by both eyes, or null when adaptation is unavailable. */
	ID3D11ShaderResourceView* GetHistorySRV() const;

	TextureManager::Texture textureCurrent;

protected:
	void CreateEffectTextures() override;

private:
	TextureManager::Texture* adaptationTextures[2] = {};
	uint32_t historyIndex = 0;
	bool historyValid = false;

	uint32_t idForceMinMax = 0xFFFFFFFF;
	uint32_t idAdaptTime = 0xFFFFFFFF;
	uint32_t idAdaptMin = 0xFFFFFFFF;
	uint32_t idAdaptMax = 0xFFFFFFFF;
	uint32_t idAdaptSens = 0xFFFFFFFF;
	bool idsCached = false;
};
