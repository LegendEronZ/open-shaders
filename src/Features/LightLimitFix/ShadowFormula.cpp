// ShadowFormula.cpp
// exprtk-backed scoring formulas for the shadow caster scheduler: the
// FormulaHelper wrapper, the shared symbol table, and the per-frame /
// per-light parameter setup. The only translation unit that includes
// exprtk.hpp (heavy header; keep it out of the other SCM modules).

#include "../../Globals.h"
#include "ShadowCasterInternal.h"

#include <exprtk.hpp>

namespace ShadowCasterManager
{
	struct FormulaWrapper
	{
		exprtk::expression<double> expression;
		exprtk::parser<double> parser;
	};

	static double s_formulaParams[kFormulaParam_Max];
	static exprtk::symbol_table<double> s_symbolTable;
	static bool s_formulaInited = false;

	static void InitFormulaSystem()
	{
		if (s_formulaInited)
			return;
		s_formulaInited = true;

		memset(s_formulaParams, 0, sizeof(double) * kFormulaParam_Max);

		for (const auto& v : kFormulaVars)
			s_symbolTable.add_variable(v.name, s_formulaParams[v.index]);
	}

	FormulaHelper::FormulaHelper() :
		_ptr(nullptr) { InitFormulaSystem(); }

	FormulaHelper::~FormulaHelper()
	{
		if (_ptr)
			delete static_cast<FormulaWrapper*>(_ptr);
	}

	bool FormulaHelper::Parse(const std::string& input)
	{
		if (_ptr)
			return false;
		auto* w = new FormulaWrapper();
		w->expression.register_symbol_table(s_symbolTable);
		// Defer the _ptr assignment until compile succeeds. Otherwise a
		// failed compile leaves the helper in a "parsed" state (Calculate
		// would evaluate an uncompiled expression and the early-return
		// guard above would block subsequent Parse retries).
		if (!w->parser.compile(input, w->expression)) {
			delete w;
			return false;
		}
		_ptr = w;
		return true;
	}

	double FormulaHelper::Calculate()
	{
		auto* w = static_cast<FormulaWrapper*>(_ptr);
		return w ? w->expression.value() : 0.0;
	}

	bool FormulaHelper::Reparse(const std::string& input)
	{
		std::string err;
		if (!Validate(input, err))
			return false;
		if (_ptr)
			delete static_cast<FormulaWrapper*>(_ptr);
		_ptr = nullptr;
		return Parse(input);
	}

	bool FormulaHelper::Validate(const std::string& input, std::string& errorOut)
	{
		InitFormulaSystem();
		FormulaWrapper tmp;
		tmp.expression.register_symbol_table(s_symbolTable);
		if (tmp.parser.compile(input, tmp.expression))
			return true;
		if (tmp.parser.error_count() > 0)
			errorOut = tmp.parser.get_error(0).diagnostic;
		else
			errorOut = "Unknown parse error";
		return false;
	}

	void FormulaHelper::SetParam(int32_t index, double value) { s_formulaParams[index] = value; }
	double FormulaHelper::GetParam(int32_t index) { return s_formulaParams[index]; }

	// =========================================================================
	// Formula helpers
	//
	// SetupSceneFormula: called once per frame, sets camera/scene params.
	// SetupLightFormula: called per candidate light, sets all light params.
	// CalculateLightScore: evaluates s_formulaScore if available.
	// =========================================================================

	void SetupSceneFormula(const RE::NiCamera* camera)
	{
		if (camera) {
			FormulaHelper::SetParam(kFormulaParam_CameraX, camera->world.translate.x);
			FormulaHelper::SetParam(kFormulaParam_CameraY, camera->world.translate.y);
			FormulaHelper::SetParam(kFormulaParam_CameraZ, camera->world.translate.z);
		} else {
			FormulaHelper::SetParam(kFormulaParam_CameraX, 0.0);
			FormulaHelper::SetParam(kFormulaParam_CameraY, 0.0);
			FormulaHelper::SetParam(kFormulaParam_CameraZ, 0.0);
		}

		FormulaHelper::SetParam(kFormulaParam_IsInterior, 0);
		auto* plr = RE::PlayerCharacter::GetSingleton();
		if (plr) {
			auto* cell = plr->parentCell;
			if (cell && cell->IsInteriorCell())
				FormulaHelper::SetParam(kFormulaParam_IsInterior, 1);
		}

		// Time of day from GameHour global
		auto* cal = RE::Calendar::GetSingleton();
		if (cal)
			FormulaHelper::SetParam(kFormulaParam_TimeOfDay, cal->GetHour());
	}

	void SetupLightFormula(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index)
	{
		FormulaHelper::SetParam(kFormulaParam_LightConverted, 0.0);
		FormulaHelper::SetParam(kFormulaParam_LightIndex, index);
		FormulaHelper::SetParam(kFormulaParam_LightDisplacement, 0.0);    // overridden per-entry in redraw interval loop
		FormulaHelper::SetParam(kFormulaParam_PlayerLightDistance, 0.0);  // overridden below after light position is known
		FormulaHelper::SetParam(kFormulaParam_LightImportance, 0.0);      // overridden per-entry in redraw interval loop; 0 in score formula

		// Temporal stickiness signals. Both derived from the slot pool in one
		// pass: chosenLastFrame is the boolean kept for backward-compat with
		// user formulas; framesSinceRender is a continuous age that decays to
		// zero stickiness once the slot has been stale long enough to no
		// longer represent a true rank-drift case. Sentinel 1e6 covers the
		// "no slot" and "never rendered" branches so the default formula's
		// max(0, 1 - age/window) decay term cleanly collapses to 0.
		double chosenLastFrame = 0.0;
		double framesSinceRender = 1e6;
		{
			const int32_t now = *globals::game::frameCounter;
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
				const auto& e = s_lights.Lights[i];
				if (e.Light != light)
					continue;
				chosenLastFrame = 1.0;
				if (e.LastDrawnFrame >= 0)
					framesSinceRender = static_cast<double>(now - e.LastDrawnFrame);
				break;
			}
		}
		FormulaHelper::SetParam(kFormulaParam_LightChosenLastFrame, chosenLastFrame);
		FormulaHelper::SetParam(kFormulaParam_LightFramesSinceRender, framesSinceRender);

		FormulaHelper::SetParam(kFormulaParam_LightNeverFades, light->lodFade ? 0.0 : 1.0);
		FormulaHelper::SetParam(kFormulaParam_LightPortalStrict, light->portalStrict ? 1.0 : 0.0);
		FormulaHelper::SetParam(kFormulaParam_LightNS, 0.0);

		// Spot detection + cone-aware visibility prior (option 1 from spot
		// preservation analysis). Non-spots get spotvisible=1 so existing
		// omni-tuned formulas are unaffected. For spots, we read last
		// frame's UpdateCamera verdict (frustumCull / lodDimmer) -- the
		// score runs BEFORE this frame's validation pass updates those,
		// but cameras move continuously so last-frame's cone-vs-frustum
		// is a strong predictor of this-frame's. Trading a one-frame lag
		// for not double-calling UpdateCamera is a worthwhile cost since
		// the score is a preference, not a gate.
		const bool isSpot = (skyrim_cast<const RE::BSShadowFrustumLight*>(light) != nullptr);
		double spotVisible = 1.0;  // default for non-spots: always "visible"
		if (isSpot) {
			// frustumCull == 0 means "in frustum"; engine sets 0xff when
			// cone-vs-frustum rejects. lodDimmer > 0 means the LOD fader
			// hasn't zeroed the light. Both must hold for a spot to count
			// as plausibly visible.
			// Note: the engine field is misspelled "frustrumCull" in the SDK
			// (matches Bethesda's original symbol). 0 = visible, 0xff = culled.
			const bool inFrustum = (light->frustrumCull == 0);
			const bool lodLit = (light->lodDimmer > 0.0f);
			spotVisible = (inFrustum && lodLit) ? 1.0 : 0.0;
		}
		FormulaHelper::SetParam(kFormulaParam_LightIsSpot, isSpot ? 1.0 : 0.0);
		FormulaHelper::SetParam(kFormulaParam_LightSpotVisible, spotVisible);

		float x, y, z;

		auto* nilight = light->light.get();
		if (nilight) {
			FormulaHelper::SetParam(kFormulaParam_LightIntensity, nilight->GetLightRuntimeData().fade);
			FormulaHelper::SetParam(kFormulaParam_LightRadius, nilight->GetLightRuntimeData().radius.x);
			FormulaHelper::SetParam(kFormulaParam_LightR, nilight->GetLightRuntimeData().diffuse.red);
			FormulaHelper::SetParam(kFormulaParam_LightG, nilight->GetLightRuntimeData().diffuse.green);
			FormulaHelper::SetParam(kFormulaParam_LightB, nilight->GetLightRuntimeData().diffuse.blue);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientR, nilight->GetLightRuntimeData().ambient.red);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientG, nilight->GetLightRuntimeData().ambient.green);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientB, nilight->GetLightRuntimeData().ambient.blue);
			x = nilight->world.translate.x;
			y = nilight->world.translate.y;
			z = nilight->world.translate.z;

			if (s_settings.PromoteNormalToShadow)
				FormulaHelper::SetParam(kFormulaParam_LightNS, IsPromotedLight(nilight) ? 1.0 : 0.0);
		} else {
			FormulaHelper::SetParam(kFormulaParam_LightIntensity, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightRadius, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightR, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightG, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightB, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientR, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientG, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientB, 1.0);
			x = light->worldTranslate.x;
			y = light->worldTranslate.y;
			z = light->worldTranslate.z;
		}

		FormulaHelper::SetParam(kFormulaParam_LightX, x);
		FormulaHelper::SetParam(kFormulaParam_LightY, y);
		FormulaHelper::SetParam(kFormulaParam_LightZ, z);

		float camx = camera ? camera->world.translate.x : (float)FormulaHelper::GetParam(kFormulaParam_CameraX);
		float camy = camera ? camera->world.translate.y : (float)FormulaHelper::GetParam(kFormulaParam_CameraY);
		float camz = camera ? camera->world.translate.z : (float)FormulaHelper::GetParam(kFormulaParam_CameraZ);

		float dx = x - camx, dy = y - camy, dz = z - camz;
		FormulaHelper::SetParam(kFormulaParam_LightDistance, sqrtf(dx * dx + dy * dy + dz * dz));

		// Player-to-light distance: ensures third-person shadow maps redraw when the
		// player character is inside a light's radius even if the camera is outside.
		double playerLightDist = FormulaHelper::GetParam(kFormulaParam_LightDistance);
		auto* plr = RE::PlayerCharacter::GetSingleton();
		if (plr) {
			auto pp = plr->GetPosition();
			float pdx = x - pp.x, pdy = y - pp.y, pdz = z - pp.z;
			playerLightDist = static_cast<double>(sqrtf(pdx * pdx + pdy * pdy + pdz * pdz));
		}
		FormulaHelper::SetParam(kFormulaParam_PlayerLightDistance, playerLightDist);
	}

	double CalculateLightScore(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index)
	{
		SetupLightFormula(light, camera, index);

		if (s_formulaScore)
			return s_formulaScore->Calculate();

		return 0.0;
	}
}
