// Implements stochastic noise sampling for terrain textures to reduce tiling artifacts and improve visual quality.
// Based on paper "Procedural Stochastic Textures by Tiling and Blending" by Thomas Deliot & Eric Heitz.
// https://eheitzresearch.wordpress.com/722-2/

#ifndef TERRAIN_VARIATION_HLSLI
#define TERRAIN_VARIATION_HLSLI

#include "Common/SharedData.hlsli"

// --------------------- CONSTANTS --------------------- //
static const float2x2 SKEW_MATRIX = float2x2(1.0, 0.0, -0.57735027, 1.15470054);
static const float WORLD_SCALE = 332.54;
static const float2 HASH_MULTIPLIER = float2(1271.5151, 3337.8237);
// Height-vs-stochastic weight (unchanged). Stochastic vertex weights use HEIGHT_INFLUENCE only via heightInfluence below.
static const float HEIGHT_INFLUENCE = 0.3;
static const float3 LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
// Golden ratio for frac(rnd * φ) low-discrepancy jitter; precompute once per pixel at callsite when possible.
static const float STOCHASTIC_LOD_PHI = 1.618;

// --------------------- STRUCTURES --------------------- //
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
	float2 offset3;
	float3 weights;
	// Contrast on the two highest barycentric weights — same for every layer on a pixel.
	float w1Contrast;
	float w2Contrast;
};

// Per-pixel landscape UV gradients. Set once in Lighting.hlsl before the six-way blend so
// StochasticEffect does not duplicate ddx/ddy per layer.
struct TerrainGradients
{
	float2 gradDx;
	float2 gradDy;
};

static TerrainGradients g_terrainStochasticGrad;
static float g_terrainStochasticSecondSampleFade[6];
static float g_terrainStochasticHeightInfluence[6];
static float g_terrainParallaxSecondSampleFade[6];
static float g_terrainParallaxHeightInfluence[6];

// Triangle corner for barycentric sort: pack cell id + weight so each swap updates both.
struct StochasticCorner
{
	float2 cell;
	float w;
};

// --------------------- HASH FUNCTIONS --------------------- //
inline float2 hash2D2D(float2 s)
{
	s = frac(s * HASH_MULTIPLIER);
	s += dot(s, s.yx + 19.19);
	return frac((s.xx + s.yy) * s.yx);
}

inline float2 hashLOD(float2 p)
{
	p = frac(p * 0.318);
	return frac(float2(dot(p, float2(1.0, 17.0)), dot(p, float2(1.0, 23.0))));
}

// --------------------- COMPUTE FUNCTIONS --------------------- //
inline float StochasticContrastWeight(float weight)
{
	float w = saturate(weight);
	float w2 = w * w;
	float w4 = w2 * w2;
	return w4 * w4;
}

inline StochasticOffsets ZeroStochasticOffsets()
{
	StochasticOffsets o;
	o.offset1 = 0;
	o.offset2 = 0;
	o.offset3 = 0;
	o.weights = 0;
	o.w1Contrast = 0;
	o.w2Contrast = 0;
	return o;
}

inline TerrainGradients ComputeTerrainGradients(float2 uv)
{
	TerrainGradients g;
	g.gradDx = ddx(uv);
	g.gradDy = ddy(uv);
	return g;
}

inline void InitTerrainStochasticMip(uint tile, Texture2D tex, float extraLandMipBias)
{
	g_terrainStochasticSecondSampleFade[tile] = 1.0;
	g_terrainStochasticHeightInfluence[tile] = HEIGHT_INFLUENCE;
}

inline void InitTerrainParallaxStochasticFade(uint tile, float mipLevel)
{
	g_terrainParallaxSecondSampleFade[tile] = 1.0;
	g_terrainParallaxHeightInfluence[tile] = HEIGHT_INFLUENCE;
}

inline StochasticOffsets ComputeStochasticOffsets(float2 landscapeUV)
{
	float2 skewUV = mul(SKEW_MATRIX, landscapeUV * WORLD_SCALE);
	float2 vxID = floor(skewUV);
	float2 f = frac(skewUV);
	float bz = 1.0 - f.x - f.y;

	StochasticCorner c0, c1, c2;
	if (bz > 0) {
		c0.cell = vxID;
		c0.w = bz;
		c1.cell = vxID + float2(0, 1);
		c1.w = f.y;
		c2.cell = vxID + float2(1, 0);
		c2.w = f.x;
	} else {
		c0.cell = vxID + 1.0;
		c0.w = -bz;
		c1.cell = vxID + float2(1, 0);
		c1.w = 1.0 - f.y;
		c2.cell = vxID + float2(0, 1);
		c2.w = 1.0 - f.x;
	}

	// Sort by weight descending (3-comparator network). Only c0/c1 are hashed; weights.xy must be the two largest.
	if (c1.w > c0.w) {
		StochasticCorner t = c0;
		c0 = c1;
		c1 = t;
	}
	if (c2.w > c0.w) {
		StochasticCorner t = c0;
		c0 = c2;
		c2 = t;
	}
	if (c2.w > c1.w) {
		StochasticCorner t = c1;
		c1 = c2;
		c2 = t;
	}

	StochasticOffsets o;
	o.offset1 = hash2D2D(c0.cell);
	o.offset2 = hash2D2D(c1.cell);
	o.offset3 = 0;
	o.weights = float3(c0.w, c1.w, c2.w);
	o.w1Contrast = StochasticContrastWeight(c0.w);
	o.w2Contrast = StochasticContrastWeight(c1.w);
	return o;
}

inline StochasticOffsets ComputeStochasticOffsetsLOD(float2 landscapeUV)
{
	float lodOn = SharedData::terrainVariationSettings.enableLODTerrainTilingFix ? 1.0 : 0.0;

	float2 cellID = floor(landscapeUV * 255437.0);
	float2 h1 = hashLOD(cellID);
	float2 h2 = hashLOD(cellID + 127.0);

	StochasticOffsets o;
	o.offset1 = h1 * 0.08 * lodOn;
	o.offset2 = h2 * 0.08 * lodOn;
	o.offset3 = 0;
	o.weights = float3(0.65, 0.35, 0.0) * lodOn;
	o.w1Contrast = StochasticContrastWeight(o.weights.x);
	o.w2Contrast = StochasticContrastWeight(o.weights.y);
	return o;
}

// --------------------- SAMPLING FUNCTIONS --------------------- //

inline float2 StochasticSampleLODJitter(float rnd)
{
	return float2(rnd - 0.5, frac(rnd * STOCHASTIC_LOD_PHI) - 0.5);
}

// secondSampleScale fades the second tap's contribution to zero near the single-sample cutoff so the
// branch boundary is continuous (no pop). At scale 0 the result is exactly s1.
inline float4 StochasticBlendTwoSamples(float4 s1, float4 s2, float w1Contrast, float w2Contrast, float heightInfluence, float blendFactor1, float blendFactor2, float secondSampleScale)
{
	float w1 = w1Contrast * (1.0 + heightInfluence * blendFactor1);
	float w2 = w2Contrast * secondSampleScale * (1.0 + heightInfluence * blendFactor2);
	float denom = max(w1 + w2, 1e-8);
	return lerp(s2, s1, w1 / denom);
}

// LOD terrain stochastic sampling — 2 SampleBias, fixed blend (pass jitter from StochasticSampleLODJitter(screenNoise)).
inline float4 StochasticSampleLOD(float2 jitter, Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsetsLOD)
{
	float lodOn = SharedData::terrainVariationSettings.enableLODTerrainTilingFix ? 1.0 : 0.0;
	float2 j1 = (offsetsLOD.offset1 + jitter) * 0.01;
	float2 j2 = (offsetsLOD.offset2 + float2(jitter.y, -jitter.x)) * 0.01;
	float4 s1 = tex.SampleBias(samp, uv + j1 * lodOn, SharedData::MipBias);
	float4 s2 = tex.SampleBias(samp, uv + j2 * lodOn, SharedData::MipBias);
	float blendW = lerp(0.5, offsetsLOD.weights.x, lodOn);
	return lerp(s2, s1, blendW);
}

// 2-sample height-blended stochastic sampling. Uses one shared gradient (SampleGrad) for both taps so
// filtering stays consistent and anisotropy is preserved.
// Sorting in ComputeStochasticOffsets guarantees offset1/offset2 are the two
// highest-weight barycentric vertices, so dropping offset3 loses minimal quality.
inline float4 StochasticEffect(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, float secondSampleFade, float heightInfluence)
{
	TerrainGradients g = g_terrainStochasticGrad;
	float4 s1 = tex.SampleGrad(samp, uv + offsets.offset1, g.gradDx, g.gradDy);
	float4 s2 = tex.SampleGrad(samp, uv + offsets.offset2, g.gradDx, g.gradDy);

	float h1 = lerp(dot(s1.rgb, LUMINANCE_WEIGHTS), s1.a, step(0.001, s1.a));
	float h2 = lerp(dot(s2.rgb, LUMINANCE_WEIGHTS), s2.a, step(0.001, s2.a));

	return StochasticBlendTwoSamples(s1, s2, offsets.w1Contrast, offsets.w2Contrast, heightInfluence, h1, h2, secondSampleFade);
}

// 2-sample parallax/height sampling. MUST use the same dual-tap stochastic blend (same offsets/weights)
// as StochasticEffect so the displaced height field stays aligned with the de-tiled albedo/normal —
// otherwise parallax is computed against a different surface than the one being shaded.
// Deliberately BRANCHLESS: this is inlined dozens of times across the unrolled ray-march / secant /
// soft-shadow paths, and it's the duplicated control flow (not the second fetch) that explodes FXC
// compile time. secondSampleScale mirrors StochasticEffect (always 1.0 in full-quality mode).
inline float4 StochasticEffectParallax(Texture2D tex, SamplerState samp, float2 uv, float mipLevel, StochasticOffsets offsets, float secondSampleFade, float heightInfluence)
{
	float4 s1 = tex.SampleLevel(samp, uv + offsets.offset1, mipLevel);
	float4 s2 = tex.SampleLevel(samp, uv + offsets.offset2, mipLevel);
	return StochasticBlendTwoSamples(s1, s2, offsets.w1Contrast, offsets.w2Contrast, heightInfluence, s1.a, s2.a, secondSampleFade);
}

inline float4 SampleTerrain(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, uint tileIndex)
{
	return StochasticEffect(tex, samp, uv, offsets, g_terrainStochasticSecondSampleFade[tileIndex], g_terrainStochasticHeightInfluence[tileIndex]);
}

#endif  // TERRAIN_VARIATION_HLSLI