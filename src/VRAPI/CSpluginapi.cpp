#include "VRAPI/CSpluginapi.h"

#include "Features/LightLimitFix.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"

#include <algorithm>
#include <atomic>
#include <optional>

namespace CSPluginAPI
{
	namespace
	{
		// Setters stage here because consumers call from arbitrary threads while the
		// settings structs are only safely mutated on the render thread (menu-edit parity).
		// Value is written before its changed flag so the render-thread reader that wins
		// the exchange always observes a fully published value.
		struct StagedSettings
		{
			std::atomic<bool> sssEnabled{ false };
			std::atomic<bool> sssChanged{ false };

			std::atomic<bool> ssgiEnabled{ false };
			std::atomic<bool> ssgiChanged{ false };

			std::atomic<bool> vlExteriorEnabled{ false };
			std::atomic<bool> vlExteriorChanged{ false };

			std::atomic<bool> llfContactShadowsEnabled{ false };
			std::atomic<bool> llfContactShadowsChanged{ false };

			std::atomic<uint32_t> upscalePreset{ 0 };
			std::atomic<bool> upscalePresetChanged{ false };

			std::atomic<uint32_t> dlssProfile{ 0 };
			std::atomic<bool> dlssProfileChanged{ false };

			std::atomic<uint32_t> upscaleMethod{ 0 };
			std::atomic<bool> upscaleMethodChanged{ false };

			std::atomic<bool> renderAtUpscaleRes{ false };
			std::atomic<bool> renderAtUpscaleResChanged{ false };
		};

		StagedSettings stagedSettings;

		CSInterface001 g_interface001;

		bool IsValidInterfaceRequest(const SKSE::MessagingInterface::Message* message)
		{
			return message &&
			       message->type == CSMessage::kMessage_GetInterface &&
			       message->data &&
			       message->dataLen >= sizeof(CSMessage);
		}

		// kHoshipa/kUltraQuality are ABI-valid but have no matching quality mode in this
		// build; values 0-4 map 1:1 onto Upscaling::QualityMode.
		std::optional<uint32_t> UpscalePresetToQualityMode(UpscalePreset preset)
		{
			switch (preset) {
			case UpscalePreset::kNativeAA:
			case UpscalePreset::kQuality:
			case UpscalePreset::kBalanced:
			case UpscalePreset::kPerformance:
			case UpscalePreset::kUltraPerformance:
				return static_cast<uint32_t>(preset);
			case UpscalePreset::kHoshipa:
			case UpscalePreset::kUltraQuality:
				logger::warn("[CS API] Upscaler preset {} is not supported by this build; ignoring", static_cast<uint32_t>(preset));
				return std::nullopt;
			default:
				logger::warn("[CS API] Ignoring invalid upscaler preset value {}", static_cast<uint32_t>(preset));
				return std::nullopt;
			}
		}

		// API kJ..kM map to presetDLSS combo slots 1..4 (slot 0 = Default/auto).
		// kF is ABI-valid but this build's DLSS integration has no preset F.
		std::optional<uint32_t> DLSSProfileToPresetDLSS(DLSSProfile profile)
		{
			switch (profile) {
			case DLSSProfile::kJ:
				return 1u;
			case DLSSProfile::kK:
				return 2u;
			case DLSSProfile::kL:
				return 3u;
			case DLSSProfile::kM:
				return 4u;
			case DLSSProfile::kF:
				logger::warn("[CS API] DLSS profile F is not supported by this build; ignoring");
				return std::nullopt;
			default:
				logger::warn("[CS API] Ignoring invalid DLSS profile value {}", static_cast<uint32_t>(profile));
				return std::nullopt;
			}
		}

		DLSSProfile PresetDLSSToDLSSProfile(uint32_t presetDLSS)
		{
			switch (presetDLSS) {
			case 2:
				return DLSSProfile::kK;
			case 3:
				return DLSSProfile::kL;
			case 4:
				return DLSSProfile::kM;
			default:
				// 1 = J; 0 = Default, whose Streamline auto-selection also resolves to J.
				return DLSSProfile::kJ;
			}
		}

		// API and internal UpscaleMethod enums share values 0-3; validate rather than trust.
		std::optional<uint32_t> ValidateUpscaleMethod(UpscaleMethod method)
		{
			switch (method) {
			case UpscaleMethod::kNone:
			case UpscaleMethod::kTAA:
			case UpscaleMethod::kFSR:
			case UpscaleMethod::kDLSS:
				return static_cast<uint32_t>(method);
			default:
				logger::warn("[CS API] Ignoring invalid upscaler method value {}", static_cast<uint32_t>(method));
				return std::nullopt;
			}
		}

		UpscaleMethod FromInternalUpscaleMethod(Upscaling::UpscaleMethod method)
		{
			switch (method) {
			case Upscaling::UpscaleMethod::kTAA:
				return UpscaleMethod::kTAA;
			case Upscaling::UpscaleMethod::kFSR:
				return UpscaleMethod::kFSR;
			case Upscaling::UpscaleMethod::kDLSS:
				return UpscaleMethod::kDLSS;
			case Upscaling::UpscaleMethod::kNONE:
			default:
				return UpscaleMethod::kNone;
			}
		}

		void StageBool(std::atomic<bool>& value, std::atomic<bool>& changed, bool v)
		{
			value.store(v, std::memory_order_release);
			changed.store(true, std::memory_order_release);
		}

		void StageUint(std::atomic<uint32_t>& value, std::atomic<bool>& changed, uint32_t v)
		{
			value.store(v, std::memory_order_release);
			changed.store(true, std::memory_order_release);
		}
	}

	void* GetApi(unsigned int revisionNumber)
	{
		// Accept revision 0 as "latest" and keep older revisions available for
		// already-compiled consumers. New revisions only append vtable entries.
		if (revisionNumber != 0 &&
			revisionNumber != CSInterfaceRevision001 &&
			revisionNumber != CSInterfaceRevision002 &&
			revisionNumber != CSInterfaceRevision) {
			return nullptr;
		}

		return &g_interface001;
	}

	void ModMessageHandler(SKSE::MessagingInterface::Message* message)
	{
		if (!IsValidInterfaceRequest(message)) {
			return;
		}

		auto* csMessage = static_cast<CSMessage*>(message->data);
		csMessage->GetApiFunction = GetApi;
		logger::info("Provided Community Shaders plugin interface to {}", message->sender ? message->sender : "<unknown>");
	}

	void ProcessStagedSettings()
	{
		if (stagedSettings.sssChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::screenSpaceShadows.bendSettings.Enable =
				stagedSettings.sssEnabled.load(std::memory_order_acquire) ? 1u : 0u;
		}

		if (stagedSettings.ssgiChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::screenSpaceGI.settings.Enabled =
				stagedSettings.ssgiEnabled.load(std::memory_order_acquire);
		}

		if (stagedSettings.vlExteriorChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::volumetricLighting.SetExteriorEnabled(
				stagedSettings.vlExteriorEnabled.load(std::memory_order_acquire));
		}

		if (stagedSettings.llfContactShadowsChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::lightLimitFix.settings.EnableContactShadows =
				stagedSettings.llfContactShadowsEnabled.load(std::memory_order_acquire);
		}

		if (stagedSettings.upscalePresetChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::upscaling.settings.qualityMode =
				stagedSettings.upscalePreset.load(std::memory_order_acquire);
		}

		if (stagedSettings.dlssProfileChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::upscaling.settings.presetDLSS =
				stagedSettings.dlssProfile.load(std::memory_order_acquire);
		}

		if (stagedSettings.upscaleMethodChanged.exchange(false, std::memory_order_acq_rel)) {
			auto& upscaling = globals::features::upscaling;
			const uint32_t method = stagedSettings.upscaleMethod.load(std::memory_order_acquire);
			// Same slot selection as the menu's Method combo: without DLSS the no-DLSS
			// preference is edited instead, and kDLSS coerces to kFSR.
			if (upscaling.streamline.featureDLSS)
				upscaling.settings.upscaleMethod = method;
			else
				upscaling.settings.upscaleMethodNoDLSS =
					std::min(method, static_cast<uint32_t>(Upscaling::UpscaleMethod::kFSR));
		}

		if (stagedSettings.renderAtUpscaleResChanged.exchange(false, std::memory_order_acq_rel)) {
			globals::features::upscaling.settings.renderAtUpscaleRes =
				stagedSettings.renderAtUpscaleRes.load(std::memory_order_acquire);
		}
	}

	unsigned int CSInterface001::getBuildNumber()
	{
		return CSBuildNumber;
	}

	bool CSInterface001::GetSSSEnabled()
	{
		return globals::features::screenSpaceShadows.bendSettings.Enable != 0;
	}

	void CSInterface001::SetSSSEnabled(bool enabled)
	{
		StageBool(stagedSettings.sssEnabled, stagedSettings.sssChanged, enabled);
	}

	bool CSInterface001::GetSSGIEnabled()
	{
		return globals::features::screenSpaceGI.settings.Enabled;
	}

	void CSInterface001::SetSSGIEnabled(bool enabled)
	{
		StageBool(stagedSettings.ssgiEnabled, stagedSettings.ssgiChanged, enabled);
	}

	bool CSInterface001::GetVolumetricLightingExteriorEnabled()
	{
		return globals::features::volumetricLighting.settings.ExteriorEnabled;
	}

	void CSInterface001::SetVolumetricLightingExteriorEnabled(bool enabled)
	{
		StageBool(stagedSettings.vlExteriorEnabled, stagedSettings.vlExteriorChanged, enabled);
	}

	UpscalePreset CSInterface001::GetUpscalePreset()
	{
		auto& upscaling = globals::features::upscaling;
		// While PerfMode's render-target hook is active the boot-latched preset is the
		// one actually rendering; report it rather than a pending selection.
		const uint32_t mode = upscaling.perfMode.IsHookActive() ?
		                          upscaling.bootSnapshot.Boot(&Upscaling::Settings::qualityMode) :
		                          upscaling.settings.qualityMode;
		return static_cast<UpscalePreset>(std::min(mode, static_cast<uint32_t>(Upscaling::QualityMode::kUltraPerformance)));
	}

	void CSInterface001::SetUpscalePreset(UpscalePreset preset)
	{
		if (const auto qualityMode = UpscalePresetToQualityMode(preset))
			StageUint(stagedSettings.upscalePreset, stagedSettings.upscalePresetChanged, *qualityMode);
	}

	bool CSInterface001::GetLightLimitFixContactShadowsEnabled()
	{
		return globals::features::lightLimitFix.settings.EnableContactShadows;
	}

	void CSInterface001::SetLightLimitFixContactShadowsEnabled(bool enabled)
	{
		StageBool(stagedSettings.llfContactShadowsEnabled, stagedSettings.llfContactShadowsChanged, enabled);
	}

	DLSSProfile CSInterface001::GetDLSSProfile()
	{
		return PresetDLSSToDLSSProfile(globals::features::upscaling.settings.presetDLSS);
	}

	void CSInterface001::SetDLSSProfile(DLSSProfile profile)
	{
		if (const auto presetDLSS = DLSSProfileToPresetDLSS(profile))
			StageUint(stagedSettings.dlssProfile, stagedSettings.dlssProfileChanged, *presetDLSS);
	}

	bool CSInterface001::GetRenderAtUpscaleResEnabled()
	{
		return globals::features::upscaling.settings.renderAtUpscaleRes;
	}

	void CSInterface001::SetRenderAtUpscaleResEnabled(bool enabled)
	{
		StageBool(stagedSettings.renderAtUpscaleRes, stagedSettings.renderAtUpscaleResChanged, enabled);
	}

	bool CSInterface001::GetRenderAtUpscaleResActive()
	{
		return globals::features::upscaling.perfMode.IsHookActive();
	}

	void CSInterface001::SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile)
	{
		// No dynamic relatch staging in this build: validate everything, then stage the
		// plain settings writes together (restart-gated fields apply on next boot).
		const auto qualityMode = UpscalePresetToQualityMode(preset);
		const auto presetDLSS = DLSSProfileToPresetDLSS(profile);
		if (!qualityMode || !presetDLSS)
			return;

		StageBool(stagedSettings.renderAtUpscaleRes, stagedSettings.renderAtUpscaleResChanged, renderScaleModeEnabled);
		StageUint(stagedSettings.upscalePreset, stagedSettings.upscalePresetChanged, *qualityMode);
		StageUint(stagedSettings.dlssProfile, stagedSettings.dlssProfileChanged, *presetDLSS);
	}

	UpscaleMethod CSInterface001::GetUpscaleMethod()
	{
		return FromInternalUpscaleMethod(globals::features::upscaling.GetUpscaleMethod());
	}

	void CSInterface001::SetUpscaleMethod(UpscaleMethod method)
	{
		if (const auto value = ValidateUpscaleMethod(method))
			StageUint(stagedSettings.upscaleMethod, stagedSettings.upscaleMethodChanged, *value);
	}

	void CSInterface001::SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile)
	{
		const auto methodValue = ValidateUpscaleMethod(method);
		const auto qualityMode = UpscalePresetToQualityMode(preset);
		const auto presetDLSS = DLSSProfileToPresetDLSS(profile);
		if (!methodValue || !qualityMode || !presetDLSS)
			return;

		StageUint(stagedSettings.upscaleMethod, stagedSettings.upscaleMethodChanged, *methodValue);
		StageBool(stagedSettings.renderAtUpscaleRes, stagedSettings.renderAtUpscaleResChanged, renderScaleModeEnabled);
		StageUint(stagedSettings.upscalePreset, stagedSettings.upscalePresetChanged, *qualityMode);
		StageUint(stagedSettings.dlssProfile, stagedSettings.dlssProfileChanged, *presetDLSS);
	}

	uint32_t CSInterface001::GetVRUpscalingApplyBlockReasons()
	{
		// No transition staging machinery in this build, so applies are never blocked.
		return 0;
	}

	bool CSInterface001::IsVRUpscalingProfileApplyAllowed()
	{
		return true;
	}
}  // namespace CSPluginAPI
