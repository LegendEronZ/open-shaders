#include "VRVariableRateShading.h"

#include "Features/FoveatedCommon.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"

#include <vector>

namespace VRFeatures
{
	bool VRVariableRateShading::Initialize()
	{
		if (initialized) {
			return nvapiAvailable;
		}

		NvAPI_Status status = NvAPI_Initialize();
		if (status != NVAPI_OK) {
			nvapiAvailable = false;
			initialized = true;
			logger::error("VRVariableRateShading: NvAPI_Initialize failed ({})", static_cast<int>(status));
			return false;
		}

		NV_D3D1x_GRAPHICS_CAPS caps{};
		status = NvAPI_D3D1x_GetGraphicsCapabilities(globals::d3d::device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
		if (status != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
			nvapiAvailable = false;
			initialized = true;
			logger::info("VRVariableRateShading: Variable Rate Shading not supported ({})", static_cast<int>(status));
			return false;
		}

		nvapiAvailable = true;
		initialized = true;
		logger::info("VRVariableRateShading: NVAPI VRS initialized successfully");
		return true;
	}

	void VRVariableRateShading::CreateShadingRateResource(uint32_t width, uint32_t height)
	{
		if (!nvapiAvailable) {
			return;
		}

		if (width == currentWidth && height == currentHeight && shadingRateView) {
			return;
		}

		const uint32_t tileWidth = (width + kVrsTileSize - 1) / kVrsTileSize;
		const uint32_t tileHeight = (height + kVrsTileSize - 1) / kVrsTileSize;

		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = tileWidth;
		texDesc.Height = tileHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R8_UINT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		srrTexture = nullptr;
		HRESULT hr = globals::d3d::device->CreateTexture2D(&texDesc, nullptr, srrTexture.put());
		if (FAILED(hr)) {
			logger::error("VRVariableRateShading: Failed to create SRR texture ({}x{}), hr={:#010x}", tileWidth, tileHeight, static_cast<unsigned long>(hr));
			return;
		}

		NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC desc{};
		desc.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
		desc.Format = DXGI_FORMAT_R8_UINT;
		desc.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = 0;

		shadingRateView = nullptr;
		NvAPI_Status status = NvAPI_D3D11_CreateShadingRateResourceView(globals::d3d::device, srrTexture.get(), &desc, shadingRateView.put());
		if (status != NVAPI_OK) {
			logger::error("VRVariableRateShading: NvAPI_D3D11_CreateShadingRateResourceView failed ({})", static_cast<int>(status));
			shadingRateView = nullptr;
			return;
		}

		currentWidth = width;
		currentHeight = height;
		UpdateShadingRatePattern();
	}

	VRVariableRateShading::EffectiveCoverage VRVariableRateShading::GetEffectiveCoverage() const
	{
		if (!foveationProfile.available) {
			return { 1.0f, 1.0f };
		}
		return { foveationProfile.coverageScale, foveationProfile.centerHorizontalScale };
	}

	void VRVariableRateShading::UpdateShadingRatePattern()
	{
		CS_GPU_PASS("VRVariableRateShading::UpdatePattern");

		if (!shadingRateView || !srrTexture) {
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		srrTexture->GetDesc(&desc);

		const uint32_t width = desc.Width;
		const uint32_t height = desc.Height;
		const float halfWidth = width * 0.5f;
		const uint32_t rowPitch = width * sizeof(uint8_t);

		std::vector<uint8_t> buffer(static_cast<size_t>(width) * height);

		float2 leftCenter;
		float2 rightCenter;
		if (foveationProfile.available) {
			leftCenter = { (0.5f + foveationProfile.centerOffsets[0].x) * halfWidth, (0.5f + foveationProfile.centerOffsets[0].y) * height };
			rightCenter = { halfWidth + (0.5f + foveationProfile.centerOffsets[1].x) * halfWidth, (0.5f + foveationProfile.centerOffsets[1].y) * height };
		} else {
			leftCenter = { halfWidth * 0.5f, height * 0.5f };
			rightCenter = { halfWidth + halfWidth * 0.5f, height * 0.5f };
		}

		const auto coverage = GetEffectiveCoverage();
		const float radiusX = coverage.coverageScale * coverage.centerHorizontalScale * 0.5f;
		const float radiusY = coverage.coverageScale * 0.5f;

		auto ellipseTest = [](float ndx, float ndy, float rx, float ry) {
			const float ex = ndx / rx;
			const float ey = ndy / ry;
			return ex * ex + ey * ey;
		};

		for (uint32_t y = 0; y < height; ++y) {
			for (uint32_t x = 0; x < width; ++x) {
				const bool leftEye = x < static_cast<uint32_t>(halfWidth);
				const float cx = leftEye ? leftCenter.x : rightCenter.x;
				const float cy = leftEye ? leftCenter.y : rightCenter.y;

				const float ndx = (static_cast<float>(x) - cx) / halfWidth;
				const float ndy = (static_cast<float>(y) - cy) / height;

				const float innerVal = ellipseTest(ndx, ndy, radiusX * kInnerRadiusFactor, radiusY * kInnerRadiusFactor);
				const float middleVal = ellipseTest(ndx, ndy, radiusX * kMiddleRadiusFactor, radiusY * kMiddleRadiusFactor);
				const float outerVal = ellipseTest(ndx, ndy, radiusX, radiusY);

				uint8_t rate;
				if (innerVal <= 1.0f) {
					rate = 0;
				} else if (middleVal <= 1.0f) {
					rate = 1;
				} else if (outerVal <= 1.0f) {
					rate = 2;
				} else {
					rate = 3;
				}

				if (rate == 0) {
					const float innerValY = ellipseTest(ndx, ndy * 2.0f, radiusX * kInnerRadiusFactor, radiusY * kInnerRadiusFactor);
					const float innerValX = ellipseTest(ndx * 2.0f, ndy, radiusX * kInnerRadiusFactor, radiusY * kInnerRadiusFactor);
					if (innerValY > 1.0f && innerValX > 1.0f) {
						rate = 1;
					}
				}

				buffer[static_cast<size_t>(y) * width + x] = rate;
			}
		}

		globals::d3d::context->UpdateSubresource(srrTexture.get(), 0, nullptr, buffer.data(), rowPitch, 0);
	}

	void VRVariableRateShading::SubmitShadingRateTable(ID3D11DeviceContext* a_context, bool a_enable,
		NV_PIXEL_SHADING_RATE a_rate1, NV_PIXEL_SHADING_RATE a_rate2, NV_PIXEL_SHADING_RATE a_rate3)
	{
		NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewportDesc{};
		viewportDesc.enableVariablePixelShadingRate = a_enable;
		for (auto& r : viewportDesc.shadingRateTable) {
			r = NV_PIXEL_X1_PER_RASTER_PIXEL;
		}
		viewportDesc.shadingRateTable[1] = a_rate1;
		viewportDesc.shadingRateTable[2] = a_rate2;
		viewportDesc.shadingRateTable[3] = a_rate3;

		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC viewportsDesc{};
		viewportsDesc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		viewportsDesc.numViewports = 1;
		viewportsDesc.pViewports = &viewportDesc;

		NvAPI_D3D11_RSSetViewportsPixelShadingRates(a_context, &viewportsDesc);
	}

	void VRVariableRateShading::ApplyForRenderTarget(ID3D11DeviceContext* a_context)
	{
		if (!enabled || !nvapiAvailable) {
			return;
		}

		// Geometry rasterizes at DLSS/FSR's internal render resolution, not the display
		// (screenSize) resolution -- sizing the tile grid off screenSize mismatches the
		// actual render target whenever upscaling is downscaling internally.
		const auto& upscaling = globals::features::upscaling;
		const auto screenSize = globals::state->screenSize;
		const uint32_t renderWidth = static_cast<uint32_t>(screenSize.x * upscaling.dynamicResolutionWidthRatio);
		const uint32_t renderHeight = static_cast<uint32_t>(screenSize.y * upscaling.dynamicResolutionHeightRatio);

		if (renderWidth != currentWidth || renderHeight != currentHeight) {
			CreateShadingRateResource(renderWidth, renderHeight);
		}

		if (!shadingRateView) {
			return;
		}

		NvAPI_Status status = NvAPI_D3D11_RSSetShadingRateResourceView(a_context, shadingRateView.get());
		if (status != NVAPI_OK) {
			logger::error("VRVariableRateShading: RSSetShadingRateResourceView failed ({})", static_cast<int>(status));
		}

		SubmitShadingRateTable(a_context, true, NV_PIXEL_X1_PER_1X2_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS);
	}

	void VRVariableRateShading::ForceFullRate(ID3D11DeviceContext* a_context)
	{
		if (!nvapiAvailable) {
			return;
		}
		SubmitShadingRateTable(a_context, true, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL);
	}

	void VRVariableRateShading::Disable(ID3D11DeviceContext* a_context)
	{
		if (!nvapiAvailable) {
			return;
		}
		SubmitShadingRateTable(a_context, false, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL);
	}

	void VRVariableRateShading::Cleanup()
	{
		shadingRateView = nullptr;
		srrTexture = nullptr;
		currentWidth = 0;
		currentHeight = 0;
	}

	void VRVariableRateShading::SetEnabled(bool a_enabled)
	{
		if (enabled != a_enabled) {
			enabled = a_enabled;
			if (enabled) {
				Initialize();
			} else {
				Cleanup();
			}
		}
	}

	VRVariableRateShading::RegionInfo VRVariableRateShading::GetRegionInfo() const
	{
		const auto coverage = GetEffectiveCoverage();
		RegionInfo info{};
		info.usingDlssFoveation = foveationProfile.available;
		info.coverageScale = coverage.coverageScale;
		info.centerHorizontalScale = coverage.centerHorizontalScale;
		info.outerWidthFraction = coverage.coverageScale * coverage.centerHorizontalScale;
		info.outerHeightFraction = coverage.coverageScale;
		return info;
	}

	void VRVariableRateShading::SetFoveationProfile(const FoveationProfile& a_profile)
	{
		foveationProfile = a_profile;
		foveationProfile.coverageScale = FoveatedCommon::ClampCenterScale(foveationProfile.coverageScale);
		foveationProfile.centerHorizontalScale = FoveatedCommon::ClampCenterHorizontalScale(foveationProfile.centerHorizontalScale);
		if (enabled && shadingRateView) {
			UpdateShadingRatePattern();
		}
	}
}
