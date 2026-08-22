#pragma once

#include <d3d11.h>
#include <nvapi.h>
#include <nvapi_lite_d3dext.h>
#include <winrt/base.h>

namespace VRFeatures
{
	// Must stay in sync with FoveatedRender::FoveationProfile. centerOffsets are a
	// signed delta from each eye's natural (0.5, 0.5) center, not an absolute position.
	struct FoveationProfile
	{
		bool available = false;
		float coverageScale = 1.0f;
		float centerHorizontalScale = 1.0f;
		float2 centerOffsets[2] = {};
	};

	/** @brief Manages NVIDIA Variable Rate Shading via NVAPI for VR foveated rendering. */
	class VRVariableRateShading
	{
	public:
		static VRVariableRateShading* GetSingleton()
		{
			static VRVariableRateShading instance;
			return &instance;
		}

		/** @brief Initializes NVAPI and checks hardware VRS support; safe to call every frame (no-op after the first call). */
		bool Initialize();
		/** @brief Applies the coarse shading-rate pattern, sized to the current DLSS/FSR internal render resolution. */
		void ApplyForRenderTarget(ID3D11DeviceContext* a_context);
		/** @brief Turns VRS off (all tiles full rate) without releasing GPU resources. */
		void Disable(ID3D11DeviceContext* a_context);
		/** @brief Releases the shading-rate texture/view. */
		void Cleanup();
		/** @brief Enables/disables VRS, initializing NVAPI on first enable and releasing resources on disable. */
		void SetEnabled(bool a_enabled);
		bool IsEnabled() const { return enabled; }
		bool IsAvailable() const { return nvapiAvailable; }

		/** @brief Replaces the active foveation profile used to derive VRS ring radii and eye centers. */
		void SetFoveationProfile(const FoveationProfile& a_profile);

		/** @brief Forces 1x1 shading rate for the current draw only; does not change IsEnabled(), so a caller can force full rate for one draw (e.g. grass) while VRS stays enabled overall. */
		void ForceFullRate(ID3D11DeviceContext* a_context);

		struct RegionInfo
		{
			bool usingDlssFoveation = false;  // false: full-eye fallback (no DLSS foveation active)
			float coverageScale = 1.0f;
			float centerHorizontalScale = 1.0f;
			float outerWidthFraction = 1.0f;   // full-quality ellipse width, as a fraction of eye width
			float outerHeightFraction = 1.0f;  // full-quality ellipse height, as a fraction of eye height
		};
		/** @brief Snapshot of the region currently in effect, for the settings UI. */
		RegionInfo GetRegionInfo() const;

	private:
		VRVariableRateShading() = default;
		~VRVariableRateShading() = default;
		VRVariableRateShading(const VRVariableRateShading&) = delete;
		VRVariableRateShading(VRVariableRateShading&&) = delete;
		VRVariableRateShading& operator=(const VRVariableRateShading&) = delete;
		VRVariableRateShading& operator=(VRVariableRateShading&&) = delete;

		static constexpr float kInnerRadiusFactor = 0.6f;
		static constexpr float kMiddleRadiusFactor = 0.8f;
		static constexpr uint32_t kVrsTileSize = 16;  // fixed by the NVAPI VRS hardware spec

		void CreateShadingRateResource(uint32_t width, uint32_t height);
		void UpdateShadingRatePattern();
		// Fills the viewport shading-rate table (index 0 always full rate) and submits it.
		void SubmitShadingRateTable(ID3D11DeviceContext* a_context, bool a_enable,
			NV_PIXEL_SHADING_RATE a_rate1, NV_PIXEL_SHADING_RATE a_rate2, NV_PIXEL_SHADING_RATE a_rate3);

		struct EffectiveCoverage
		{
			float coverageScale;
			float centerHorizontalScale;
		};
		// foveationProfile.available ? its values : full coverage (no reduction).
		EffectiveCoverage GetEffectiveCoverage() const;

		bool enabled = false;
		bool initialized = false;
		bool nvapiAvailable = false;
		FoveationProfile foveationProfile{};
		winrt::com_ptr<ID3D11NvShadingRateResourceView> shadingRateView;
		winrt::com_ptr<ID3D11Texture2D> srrTexture;
		uint32_t currentWidth = 0;
		uint32_t currentHeight = 0;
	};
}
