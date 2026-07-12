#ifndef EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI
#define EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI

// Body included inside `namespace ExtendedMaterials` from ExtendedMaterials.hlsli.

#if defined(LANDSCAPE)
	float2 GetParallaxCoords(PS_INPUT input, float2 coords, float mipLevels[6], float3 viewDir, float3x3 tbn, float noise, DisplacementParams params[6],
		StochasticOffsets sharedOffset,
		out float pixelOffset,
#	if defined(VR_STEREO_OPT)
		out bool hasPOM,
#	endif
		out float weights[6])
#else
	float2 GetParallaxCoords(float2 coords, float mipLevel, float3 viewDir, float3x3 tbn, float noise, Texture2D<float4> tex, SamplerState texSampler, uint channel, DisplacementParams params, out float pixelOffset
#	if defined(VR_STEREO_OPT)
		,
		out bool hasPOM
#	endif
	)
#endif
	{
		pixelOffset = 0.0;
#if defined(VR_STEREO_OPT)
		hasPOM = false;
#endif
		float3 viewDirTS = normalize(mul(tbn, viewDir));
		float invViewLen = rsqrt(max(dot(viewDirTS, viewDirTS), 1e-6));
		float ndotv = saturate(viewDirTS.z * invViewLen);  // 1 = looking "down", 0 = grazing

		// UV stride along the height slab. Meshes keep the flatten hack for warping/swim reduction;
		// terrain must use xy/z or grazing rays barely move in UV while depth bounds advance
		// (step count cannot fix that — features get stepped over regardless).
#if defined(LANDSCAPE)
		float parallaxZ = max(abs(viewDirTS.z), 0.0625);
		float2 parallaxDir = viewDirTS.xy / parallaxZ;
#else
		viewDirTS.xy /= viewDirTS.z * 0.7 + 0.3 + params.FlattenAmount;  // Fix for objects at extreme viewing angles
		float2 parallaxDir = viewDirTS.xy;
#endif

#if defined(LANDSCAPE)
		// Ray-march displacement always uses vertex weights + linear height combine (blendFactor 0).
		// Height-biased layer weights for albedo/normal are computed once at the hit UV when enabled.
		float4 w1 = input.LandBlendWeights1;
		float2 w2 = input.LandBlendWeights2.xy;
		const float marchHeightBlendFactor = 0.0;
#	if defined(TRUE_PBR)
		float scale = max(params[0].HeightScale * w1.x, max(params[1].HeightScale * w1.y, max(params[2].HeightScale * w1.z, max(params[3].HeightScale * w1.w, max(params[4].HeightScale * w2.x, params[5].HeightScale * w2.y)))));
		float scalercp = rcp(max(scale, 1e-4));
		float terrainHeightNormMul = scalercp;
		float maxHeight = 0.1 * scale;
#	else
		float scale = 1;
		float terrainHeightNormMul = 1.0;
		float maxHeight = 0.1 * scale;
#	endif
#else
		float scale = params.HeightScale;
		float maxHeight = 0.1 * scale;
#endif
		float minHeight = maxHeight * 0.5;

#if defined(LANDSCAPE)
#	if defined(TRUE_PBR)
		if (scale <= 0.001) {
			weights[0] = input.LandBlendWeights1.x;
			weights[1] = input.LandBlendWeights1.y;
			weights[2] = input.LandBlendWeights1.z;
			weights[3] = input.LandBlendWeights1.w;
			weights[4] = input.LandBlendWeights2.x;
			weights[5] = input.LandBlendWeights2.y;
			pixelOffset = 0.0;
			return coords;
		}
#	endif
#else
		if (scale <= 0.001) {
			pixelOffset = 0.0;
			return coords;
		}
#endif

#if defined(LANDSCAPE)
		// Default out weights for static analysis; ray-march / secant paths overwrite.
		weights[0] = input.LandBlendWeights1.x;
		weights[1] = input.LandBlendWeights1.y;
		weights[2] = input.LandBlendWeights1.z;
		weights[3] = input.LandBlendWeights1.w;
		weights[4] = input.LandBlendWeights2.x;
		weights[5] = input.LandBlendWeights2.y;
#endif

		{
			const float baseMaxSteps = 8;
			const uint minSteps = 4;
#if defined(LANDSCAPE)
			const uint maxStepsCap = 128;
#else
			const uint maxStepsCap = 64;
#endif

			float angleStepMul = clamp(0.5 * rcp(max(ndotv, 0.0625)), 0.5, 8.0);

#if defined(LANDSCAPE)
			float parallaxLODMip = mipLevels[0];
#else
			float parallaxLODMip = mipLevel;
#endif
			float distStepScale = lerp(0.35, 1.0, saturate((4.0 - parallaxLODMip) * (1.0 / 3.0)));

#if defined(LANDSCAPE)
			// Size the march from UV span in texels — the only reliable metric once xy/z is restored.
			float2 texDim;
			TexColorSampler.GetDimensions(texDim.x, texDim.y);
			float uvMarchSpan = dot(abs(parallaxDir), maxHeight + minHeight);
			float texelsPerStep = lerp(4.0, 1.5, saturate((0.4 - ndotv) * (1.0 / 0.4)));
			uint numSteps = max(minSteps, (uint)(uvMarchSpan * max(texDim.x, texDim.y) * rcp(texelsPerStep) * distStepScale + 0.5));
			numSteps = max(numSteps, (uint)(scale * baseMaxSteps * angleStepMul * distStepScale));
			numSteps = min(numSteps, maxStepsCap);
			// 4-wide coarse stride skips narrow peaks; single-step near camera + grazing only.
			bool useDenseMarch = (ndotv < 0.45) && (distStepScale > 0.8);
			if (!useDenseMarch)
				numSteps = (numSteps + 2) & ~3;
#else
			uint numSteps = max(minSteps, (uint)(scale * baseMaxSteps * angleStepMul * distStepScale));
			numSteps = min(numSteps, maxStepsCap);
			numSteps = (numSteps + 2) & ~3;
#endif

			uint secantIters = (uint)(lerp(2.0, 5.0, distStepScale) + 0.5);

			float stepSize = rcp((float)numSteps);

			float2 offsetPerStep = parallaxDir * maxHeight * stepSize;
			float2 prevOffset = parallaxDir * minHeight + coords.xy;

			float prevBound = 1.0;
			float prevHeight = 1.0;

			float2 pt1 = 0;
			float2 pt2 = 0;
			bool intersectionFound = false;

#if defined(LANDSCAPE)
			if (useDenseMarch)
			{
				[loop] while (numSteps > 0)
				{
					float2 currentOffset = prevOffset - offsetPerStep;
					float currentBound = prevBound - stepSize;

					float currHeight = GetTerrainHeight(noise, input, currentOffset, mipLevels, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;

					[branch] if (currHeight >= currentBound)
					{
						intersectionFound = true;
						pt1 = float2(currentBound, currHeight);
						pt2 = float2(prevBound, prevHeight);
						prevOffset = currentOffset;
						break;
					}

					prevOffset = currentOffset;
					prevBound = currentBound;
					prevHeight = currHeight;
					numSteps--;
				}
			}
			else
#endif
			[loop] while (numSteps > 0)
			{
				float4 currentOffset[2];
				currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
				currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
				float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

				float4 currHeight;
#if defined(LANDSCAPE)
				currHeight = GetTerrainHeightQuadRayMarch(noise, input, currentOffset[0].xy, currentOffset[0].zw, currentOffset[1].xy, currentOffset[1].zw, mipLevels, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
				currHeight.x = tex.SampleLevel(texSampler, currentOffset[0].xy, mipLevel)[channel];
				currHeight.y = tex.SampleLevel(texSampler, currentOffset[0].zw, mipLevel)[channel];
				currHeight.z = tex.SampleLevel(texSampler, currentOffset[1].xy, mipLevel)[channel];
				currHeight.w = tex.SampleLevel(texSampler, currentOffset[1].zw, mipLevel)[channel];

				currHeight = AdjustDisplacementNormalized(currHeight, params);
#endif

				bool4 testResult = currHeight >= currentBound;
				[branch] if (any(testResult))
				{
					intersectionFound = true;
					float2 outOffset;
					[branch] if (testResult.x)
					{
						outOffset = prevOffset;
						pt1 = float2(currentBound.x, currHeight.x);
						pt2 = float2(prevBound, prevHeight);
					}
					else if (testResult.y)
					{
						outOffset = currentOffset[0].xy;
						pt1 = float2(currentBound.y, currHeight.y);
						pt2 = float2(currentBound.x, currHeight.x);
					}
					else if (testResult.z)
					{
						outOffset = currentOffset[0].zw;
						pt1 = float2(currentBound.z, currHeight.z);
						pt2 = float2(currentBound.y, currHeight.y);
					}
					else
					{
						outOffset = currentOffset[1].xy;
						pt1 = float2(currentBound.w, currHeight.w);
						pt2 = float2(currentBound.z, currHeight.z);
					}
					prevOffset = outOffset;
					break;
				}

				prevOffset = currentOffset[1].zw;
				prevBound = currentBound.w;
				prevHeight = currHeight.w;
				numSteps -= 4;
			}

			float parallaxAmount = 0.0;
			[branch] if (intersectionFound)
			{
				// Refine coarse hit interval with secant iterations:
				// f(t) = sampledHeight(t) - t, t in [0,1] where t is ray depth bound.
				float tNear = pt1.x;
				float hNear = pt1.y;
				float fNear = hNear - tNear;
				float tFar = pt2.x;
				float hFar = pt2.y;
				float fFar = hFar - tFar;

				[loop] for (uint i = 0; i < secantIters; i++)
				{
					float denominator = fNear - fFar;
					float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
					float tSecant = lerp(tNear, tFar, r);
					float2 secantCoords = coords.xy + parallaxDir * (((1.0 - tSecant) * -maxHeight) + minHeight);

					float hSecant = 0.0;
#if defined(LANDSCAPE)
					hSecant = GetTerrainHeight(noise, input, secantCoords, mipLevels, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
					hSecant = tex.SampleLevel(texSampler, secantCoords, mipLevel)[channel];
					hSecant = AdjustDisplacementNormalized(hSecant, params);
#endif

					float fSecant = hSecant - tSecant;
					[branch] if (fSecant >= 0.0)
					{
						tNear = tSecant;
						hNear = hSecant;
						fNear = fSecant;
					}
					else
					{
						tFar = tSecant;
						hFar = hSecant;
						fFar = fSecant;
					}
				}

				float denominator = fNear - fFar;
				float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
				parallaxAmount = lerp(tNear, tFar, r);
			}

			float offset = (1.0 - parallaxAmount) * -maxHeight + minHeight;
			pixelOffset = saturate(parallaxAmount);
			float2 finalCoords = parallaxDir * offset + coords.xy;
#if defined(LANDSCAPE)
			if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
				float unusedHeight;
				unusedHeight = GetTerrainHeight(noise, input, finalCoords, mipLevels, params, 1.0, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, weights);
			}
#endif
#if defined(VR_STEREO_OPT)
			hasPOM = true;
#endif
			return finalCoords;
		}
	}

#	if !defined(LANDSCAPE)
	// https://advances.realtimerendering.com/s2006/Tatarchuk-POM.pdf
	// Cheap method of creating shadows using height for a given light source
	float GetParallaxSoftShadowMultiplier(float2 coords, float mipLevel, float3 L, float sh0, Texture2D<float4> tex, SamplerState texSampler, uint channel, float quality, float noise, DisplacementParams params)
	{
		[branch] if (quality > 0.0)
		{
			uint tapCount = ParallaxShadowTapCount(quality);
			float shadowStrength = ShadowIntensity * (4.0 / tapCount);
			float2 rayDir = L.xy * 0.1 * params.HeightScale;
			float4 multipliers = rcp((float4(1, 2, 3, 4) + noise));
			float4 sh = sh0.xxxx;
			sh.x = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.x, mipLevel)[channel], params);
			if (quality > 0.25)
				sh.y = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.y, mipLevel)[channel], params);
			if (quality > 0.5)
				sh.z = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.z, mipLevel)[channel], params);
			if (quality > 0.75)
				sh.w = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.w, mipLevel)[channel], params);
			return 1.0 - saturate(dot(max(0, sh - sh0), shadowStrength));
		}
		return 1.0;
	}

#	endif

#endif  // EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI
