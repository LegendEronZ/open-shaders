#pragma once

#include "ExtendedEffect.h"

class ENBDepthOfField : public EffectBase
{
public:
	/** @brief Returns the preset depth-of-field shader filename. */
	virtual std::string GetName() const override { return "enbdepthoffield.fx"; }

	/** @brief Reloads the effect and invalidates both eyes' focus histories. */
	virtual bool Apply() override;
	/** @brief Updates aperture and focus, then renders depth of field for the current eye. */
	virtual void Execute() override;
	/** @brief Binds adaptation and frame-time-dependent focus parameters. */
	virtual void UpdateEffectVariables() override;

	/** @brief Returns the current eye's aperture only when updated this frame. */
	ID3D11ShaderResourceView* GetApertureSRV() const;

protected:
	void CreateEffectTextures() override;

private:
	uint32_t idApertureTime = 0xFFFFFFFF;
	uint32_t idFocusingTime = 0xFFFFFFFF;
	uint32_t idEnableAdaptation = 0xFFFFFFFF;
	bool idsCached = false;
	std::array<bool, 2> historyValid{};
	std::array<uint32_t, 2> historyIndex{};
	std::string fallbackTechnique;

	std::array<ID3D11ShaderResourceView*, 2> apertureSRV{};
	std::array<uint32_t, 2> apertureFrame{ UINT32_MAX, UINT32_MAX };
};
