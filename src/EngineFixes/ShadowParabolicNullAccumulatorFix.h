#pragma once

/**
 * @brief Guards against a null-this crash in the engine's dual-paraboloid shadow accumulator setup.
 *
 * BSParabolicCullingProcess::SetBackHemisphereAccumulator does an unconditional ref-counted
 * assignment through `this` with no null check. The engine calls it from
 * BSShadowLight::Accumulate whenever a light's shadowMapCount == 2 (omni/dual-paraboloid),
 * assuming the light's per-instance BSParabolicCullingProcess has already been lazily
 * allocated by ShadowSceneNode::AccumulateLight. A light whose culling process hasn't been
 * allocated yet -- e.g. one newly promoted normal->shadow (always full-sphere FOV, so always
 * dual-paraboloid) and Accumulate'd directly by our own scheduler before the engine's own
 * lazy-allocation path has run for it -- reaches this with a null `this` and crashes.
 */
struct ShadowParabolicNullAccumulatorFix : EngineFix
{
	/** @brief Returns the human-readable name of this fix. */
	std::string GetName() override { return "Shadow Parabolic Null Accumulator Fix"; }

	/** @brief Installs the null-guard hook over SetBackHemisphereAccumulator. */
	void Install() override;

	struct BSParabolicCullingProcess_SetBackHemisphereAccumulator
	{
		static void thunk(void* a_this, void* a_accumulator);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
