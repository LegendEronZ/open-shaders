#pragma once

#include "Effects/ENBAdaptation.h"
#include "Effects/ENBBloom.h"
#include "Effects/ENBEffect.h"
#include "Effects/ENBEffectPostPass.h"
#include "Effects/ENBLens.h"
#include "Profiler.h"

enum class TimeOfDay1Index : int
{
	Dawn,
	Sunrise,
	Day,
	Sunset
};

enum class TimeOfDay2Index : int
{
	Dusk,
	Night,
	InteriorDay,
	InteriorNight
};

enum class TimeOfDayFactorIndex : int
{
	Dawn,
	Sunrise,
	Day,
	Sunset,
	Dusk,
	Night,
	Count
};

class EffectManager
{
public:
	static EffectManager& GetSingleton();

	// Effect execution
	/** @brief Runs the effect chain from a_input into a_output.
		@return true only if a_output was written; false means the caller must fall back to the stock pass. */
	bool ExecuteEffects(RE::BSGraphics::RenderTargetData& a_input, RE::BSGraphics::RenderTargetData& a_output);

	// Lifecycle
	void Initialize();

	void Apply();
	void Load();
	void Save();

	void RegisterSettings();

	// Common variable management
	void UpdateCommonVariablesForEffect(Effect& effect);

public:
	ENBBloom enbBloom;
	ENBLens enbLens;
	ENBAdaptation enbAdaptation;
	ENBEffect enbEffect;
	ENBEffectPostPass enbEffectPostPass;

	// Common resources shared across effects
	void CreateCommonResources();

	// Shared D3D resources
	winrt::com_ptr<ID3D11Buffer> quadVertexBuffer;
	winrt::com_ptr<ID3D11InputLayout> inputLayout;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11BlendState> blendState;

	// Copy shader resources
	winrt::com_ptr<ID3D11VertexShader> copyVertexShader;
	winrt::com_ptr<ID3D11PixelShader> copyPixelShader;
	winrt::com_ptr<ID3D11Buffer> ditherConstantBuffer;

	// Color correction compute shader resources
	winrt::com_ptr<ID3D11ComputeShader> colorCorrectionComputeShader;
	winrt::com_ptr<ID3D11Buffer> colorCorrectionConstantBuffer;

	static std::string LoadShaderFile(const char* path);
	void CreateQuadGeometry();
	void CreateRenderStates();
	void CreateCopyShaders();
	void CreateColorCorrectionShader();

	void RenderEffectsList();

	// Common variable data (updated once, applied to all effects)
	struct CommonVariableData
	{
		float timer[4];
		float weather[4];
		float timeOfDay1[4];
		float timeOfDay2[4];
		float eNightDayFactor;
		float eInteriorFactor;
	} commonData;
	uint32_t frameCount = 0;

	void UpdateCommonData();

	struct SettingIDs
	{
		uint32_t useBloom = 0xFFFFFFFF;
		uint32_t useLens = 0xFFFFFFFF;
		uint32_t useAdaptation = 0xFFFFFFFF;
		uint32_t usePostPass = 0xFFFFFFFF;

		uint32_t enableMultipleWeathers = 0xFFFFFFFF;
		uint32_t enableLocationWeather = 0xFFFFFFFF;

		uint32_t nightTime = 0xFFFFFFFF;
		uint32_t sunriseTime = 0xFFFFFFFF;
		uint32_t dawnDuration = 0xFFFFFFFF;
		uint32_t dayTime = 0xFFFFFFFF;
		uint32_t sunsetTime = 0xFFFFFFFF;
		uint32_t duskDuration = 0xFFFFFFFF;

		uint32_t brightness = 0xFFFFFFFF;
		uint32_t gammaCurve = 0xFFFFFFFF;
	} ids;

	const CommonVariableData& GetCommonData() const { return commonData; }

	bool IsInitialized() const { return initialized; }

	/** @brief True when a usable preset is present; enbeffect.fx is required, so its absence means no preset.
		Effects11 must stay fully inert in that case, leaving the image untouched. */
	bool IsPresetLoaded() const { return enbEffect.IsCompiled(); }

	bool performanceMode = false;

	// Execute a single effect with perf events and common variable setup
	void ExecuteEffect(EffectBase& effect, uint32_t enableSettingID = 0xFFFFFFFF);

	// Texture copy using pixel shader
	void CopyTexture(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination);

	// -1 outside VR; the eye ExecuteEffects's per-eye loop is currently rendering
	// otherwise. GetTextureOriginal() and Effect::RenderPasses both read this --
	// the latter to crop its output viewport to that eye's half of any destination
	// texture that spans the full side-by-side width (see currentMainWidth), the
	// same idiom this codebase already uses elsewhere (compare
	// ScreenSpaceGI::UpdateSB's per-eye loop): populate per-eye data, one dispatch,
	// no separate VR code path.
	int currentEyeIndex = -1;
	// kMAIN's actual current width, cached once per ExecuteEffects call so
	// Effect::RenderPasses can tell "this destination spans the full packed
	// buffer, crop it" (TextureLens, TextureSDRTemp/2, ...) apart from "this is
	// a self-contained fixed-size working canvas, leave it alone" (TextureBloom's
	// 1024x1024, TextureAdaptation's 1x1) purely by comparing widths.
	uint32_t currentMainWidth = 0;

	// The "TextureOriginal" source every effect samples: kMAIN outside VR, or (once
	// RefreshEyeSourceTexture has run for the current currentEyeIndex) a private
	// half-width crop of kMAIN's current eye. Arbitrary third-party .fx content
	// can't be made eye-aware -- this is what makes its ordinary [0,1] sampling
	// land on the correct eye without the preset author needing to know VR exists.
	RE::BSGraphics::RenderTargetData& GetTextureOriginal();
	// Crop-copies kMAIN's a_eyeIndex half into the private texture GetTextureOriginal()
	// then returns. Call once per ExecuteEffects per-eye loop iteration, after
	// currentEyeIndex is set and after kMAIN itself holds this frame's a_input.
	void RefreshEyeSourceTexture(int a_eyeIndex);

	// Crop-copies a_source's current eye half into a private texture and returns its SRV,
	// or a_source.srv.get() unchanged outside VR / when a_source isn't full-width (not
	// subject to the mismatch below). Effect::ExecuteTechniqueSequence's ping-pong
	// read-back (technique 2+ in a multi-technique sequence reading technique 1's own
	// output as its input) has the same UV-vs-physical-crop mismatch GetTextureOriginal()
	// solves for kMAIN, just one level deeper: a prior technique in the SAME sequence
	// wrote its output under a cropped viewport (see Effect::RenderPasses), so the next
	// technique's naive [0,1] sample of that still-full-width texture lands on the wrong
	// physical region exactly like an unfixed TextureOriginal would.
	ID3D11ShaderResourceView* GetEyeCroppedSRV(TextureManager::Texture& a_source);

	// Color correction using compute shader
	void ApplyColorCorrection(ID3D11UnorderedAccessView* textureUAV);

	void ReloadShaders();

	// Error reporting for overlay display
	uint32_t GetFailedEffectCount() const;
	std::vector<std::string> GetAllErrors() const;

private:
	/** @brief Logs the resolved preset location, or why no preset is in use. */
	void LogPresetStatus() const;

	bool initialized = false;

	// Backing storage for GetTextureOriginal()'s VR path -- a private half-width
	// crop of kMAIN, resized on demand in RefreshEyeSourceTexture. eyeSourceData
	// mirrors these raw pointers so GetTextureOriginal() can hand back a
	// RE::BSGraphics::RenderTargetData& just like the engine's own kMAIN.
	winrt::com_ptr<ID3D11Texture2D> eyeSourceTexture;
	winrt::com_ptr<ID3D11RenderTargetView> eyeSourceRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> eyeSourceSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> eyeSourceUAV;
	RE::BSGraphics::RenderTargetData eyeSourceData{};
	winrt::com_ptr<ID3D11PixelShader> eyeCropCopyPS;
	winrt::com_ptr<ID3D11Buffer> eyeCropCB;

	// Backing storage for GetEyeCroppedSRV -- a second, separate half-width scratch
	// texture from eyeSourceTexture, since both can be in use at once within the
	// same technique sequence (TextureOriginal already redirected via eyeSourceData,
	// a ping-pong intermediate cropped via this one).
	winrt::com_ptr<ID3D11Texture2D> inputCropTexture;
	winrt::com_ptr<ID3D11RenderTargetView> inputCropRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> inputCropSRV;

	// Shared by RefreshEyeSourceTexture and GetEyeCroppedSRV: draws a_source cropped to
	// a_eyeIndex's half into a_destRTV (assumed already sized to a_srcWidth/2 x a_srcHeight).
	void CropCopyEyeHalf(ID3D11ShaderResourceView* a_source, uint32_t a_srcWidth, uint32_t a_srcHeight, ID3D11RenderTargetView* a_destRTV, int a_eyeIndex);
};