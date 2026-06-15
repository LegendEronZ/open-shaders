# VR stereo screen-space effects: two-camera strategy

How Open Shaders reconciles screen-space effects between the two eyes in VR, why
the current per-eye sync passes exist, and the plan to replace them by treating
VR's two cameras as an asset rather than a 2× cost.

## Background: why screen-space effects disagree between eyes

Skyrim VR packs both eyes into one wide side-by-side (SBS) texture: `x ∈ [0, 0.5)`
is the left eye, `x ∈ (0.5, 1]` the right. The engine renders eye 0 into the left
half and eye 1 into the right half via a viewport swap. There is no texture-array /
single-pass / instanced-stereo path available to a mod — `BSGraphics` owns the SBS
layout.

Screen-space effects (SSAO, SSGI, screen-space shadows, SSR, …) are computed
independently per eye. The same world surface lands at a different screen position
in each eye and is sampled through that eye's own depth/color buffer, so the two
eyes produce _different_ results for the _same_ point. In a stereo display that
mismatch reads as binocular rivalry — shimmer or flicker between the eyes.

The reprojection work in #141 already shares the _view-independent_ G-buffer
(material inputs) from eye 0 to eye 1, then runs the view-dependent passes natively
per eye. The per-effect stereo-sync passes (SSGI `stereoSync.cs`, SSS `StereoSyncCS`,
composite `StereoBlendCS`) are a second reconciliation layer: a cross-eye bilateral
blend (Shi/Billeter/Eisemann 2022) that averages the two eyes' results where their
depths agree. Those three passes now share one implementation (`Stereo::StereoSync*`
in `Common/VR.hlsli`).

**The sync passes are a retrofit.** They patch over the fact that we compute each
eye independently. The strategy below replaces that retrofit, per effect, by either
sharing the result or sharing the sample domain.

## VR rendering philosophy: flat-first, VR-aware-at-the-seam

Screen-space effects are a flat-screen-era compromise, and VR exposes their seams:
cost lands worst exactly where the budget is tightest (≈2× pixels at 90 Hz), per-eye
disagreement becomes binocular _rivalry_ (discomfort, not just an artifact, because
the visual system fuses the eyes), and off-screen information loss is worse at VR's
wider FOV. We use them anyway because a mod cannot add a world-space GI / ray-tracing
pipeline to Skyrim's DX11 deferred renderer — screen-space is the only modern-lighting
lever available. This whole strategy is damage control on a constraint we can't remove.

**Two consequences for how we build:**

1. **Lean on the world-space base; treat the screen-space layer as a tunable top-up.**
   For almost every screen-space effect the engine already ships a stereo-correct
   world-space technique, and the screen-space effect only adds detail on top of it:

    | Screen-space effect   | World-space base (stereo-correct, already present) |
    | --------------------- | -------------------------------------------------- |
    | SSS / contact shadows | Cascaded shadow maps                               |
    | SSR                   | Dynamic cubemaps                                   |
    | SSGI / AO             | Baked AO, irradiance probes                        |

    In VR, lean harder on the base and consider dialing the screen-space layer down or
    default-off. This is a **per-effect worth audit** flat-first never triggers: _is the
    screen-space layer's marginal quality over the world-space base worth its VR cost +
    rivalry?_ For SSR the honest answer in VR is probably "mostly rely on cubemaps."

2. **Make the shared seam VR-aware, rather than bolting VR on downstream.** Flat-first
   development is the right default for a community mod — shared flat/VR code keeps the
   project approachable so the same contributors maintain both paths, and most users are
   flat. Its failure mode is that stereo becomes an _afterthought_: every effect is
   designed for one camera and VR is "minimal divergence" bolted on (the sync passes are
   that bolt-on), and the second eye is seen only as a 2× _cost_, never as an _asset_
   (off-screen recovery, the second view). The fix is not to abandon flat-first; it is to
   evolve it to **flat-first, VR-aware-at-the-seam** — the shared helper both paths call
   _knows about stereo_, so the VR-specific intelligence (reproject, view-stable seed,
   stereo-coherent march) lives inside the shared abstraction, not as a retrofit. The
   `Stereo::StereoSync*` helpers and the `GetContactShadowNoiseCoord` pattern are the
   template; the flat path stays byte-identical, so this costs no contributor
   accessibility.

**Forward design rule:** when adding or touching any effect, ask the Class A / Class B
question _at design time_ (is the quantity view-independent → share it / fix the seed;
view-dependent → march both eyes), and keep the answer inside the shared seam. Compute-
per-eye-then-sync is the retrofit we are paying down, not the pattern to repeat.

## The taxonomy: Class A vs Class B

The right approach for an effect is determined by whether its **true value is
view-dependent** — and this is per _component_, not per effect.

|                      | **Class A — view-independent**                                             | **Class B — view-dependent**                                    |
| -------------------- | -------------------------------------------------------------------------- | --------------------------------------------------------------- |
| True value           | Identical from both eyes (a property of geometry + light)                  | Changes with view direction                                     |
| Examples             | Screen-space **shadows** (SSS), **AO**, **diffuse GI**, skylighting        | **SSR** (reflections), **specular GI**, specular occlusion      |
| Why it desyncs today | We compute two independent (often noisy) _estimates_ of the same value     | The value genuinely differs per eye                             |
| Second eye's role    | **Adds coverage** — fills disocclusion, extends the on-screen occluder set | **A second real view** — an independent, equally-valid estimate |
| Correct approach     | **Compute once, reproject the result** (exact transfer)                    | **Share the sample domain** (march both eyes' buffers)          |

Per-component nuance that matters:

-   **SSGI is mostly Class A.** AO and diffuse irradiance are view-independent — the
    irradiance at a surface point does not depend on where you look. Only SSGI's
    _specular_ IL (the experimental HQ-specular path) is Class B. SSGI desyncs today
    not because it is view-dependent but because each eye computes an independent
    Monte-Carlo estimate (different rays/noise) of a view-independent quantity.
    Sharing the estimate is therefore physically valid.
-   **SSS is purely Class A.** "Is this surface occluded from the light" is geometry +
    light only. Both the old (pre-deferred) view-space march and the current Bend
    Studio wavefront march compute the same view-independent quantity twice; the old
    one did it consistently (world-space-anchored march), the Bend one inconsistently
    (screen-space wavefronts) — which is exactly why the #1946 sync was added.

## Leverage strategy: VR's two cameras are an asset

Screen-space techniques' core weakness is **missing off-screen information** — a ray
that leaves the frustum has no data. VR hands us a second, parallax-shifted view of
the same scene, which recovers some of that information. So the goal is not "compute
twice and average," it is "treat the two eyes' buffers as one richer dataset."

### Class A — share the result

1. Compute the effect for eye 0 only (left half of the SBS texture).
2. For each eye-1 pixel, reproject to eye 0 (`Stereo::ConvertMonoUVToOtherEye`) and
   sample eye 0's result. Because the value is view-independent, the transfer is
   **exact** — not an approximation like reprojecting view-dependent color.
3. Recompute only the **disocclusion sliver** — eye-1 pixels whose reprojection
   lands off eye 0's frame or on a depth-mismatched surface (an occluder visible to
   only one eye).

This removes both the second-eye computation _and_ the sync pass. It is the same
move #141 made for the G-buffer, and it is _more_ justified here because the shared
quantity is exactly view-independent. Net: roughly halves the effect's per-frame
cost in VR and eliminates rivalry by construction (eye 1 _is_ eye 0's data).

Honest caveat: screen-space methods can only test on-screen occluders, and the
on-screen set differs slightly per eye. So the reproject is exact for the value but
inherits a small disocclusion-class gap. That gap is strictly smaller than the
artifact we tolerate today (independent noisy estimates + bilateral blur).

#### Class A variant — fix the noise, not the result (LLF contact shadows)

Not every Class-A effect should reproject its result. **Light Limit Fix per-light
contact shadows** (`LightLimitFix::ContactShadows`) are Class A (occlusion from light,
view-independent), but they are computed **inline per-light in the deferred lighting
pass**, not as a standalone buffer — so a result-reproject would mean first splitting
them into their own texture, which is not worth it. And their march is already
**view-space anchored** (both eyes solve the same 3D ray, projected per eye), so unlike
Bend SSS they have **no structural divergence**.

Their only per-eye divergence is the **noise seed**. The right fix is therefore to make
that seed view-stable, inside the one shared helper both paths already call
(`GetContactShadowNoiseCoord`) — never a call-site `#if VR` branch (see the VR-vs-flat
divergence rule in `CLAUDE.md`).

The helper already does half of this: VR seeds from per-eye `screenUV` rather than
`SV_Position`, removing the side-by-side x-offset component of the divergence. The
**residual is parallax** — the same world point lands at a different per-eye UV
(binocular disparity), so near contact shadows still get slightly different noise per
eye. Making the seed fully view-stable means reprojecting the VR coordinate to a single
reference eye (eye 0) using depth — which the call site already has (`viewPosition`) —
so both eyes hash the _same world point_ to the _same_ value. The flat path is
untouched; the VR-specific handling stays entirely inside the helper. Whether the
parallax residual is even visible needs an in-headset A/B (the screen-stable fix may
already be enough); and because this touches the lighting hot path, it gets its own A/B
regardless.

### Class B — share the sample domain (stereo-coherent marching)

For genuinely view-dependent effects, the modern VR-native approach is to ray-march
through the **union of both eyes' G-buffers/color**, not one:

1. **Recovers off-screen data** — a reflector/occluder off-screen in eye 0 may be
   on-screen in eye 1, so a reflection or GI ray finds a hit it otherwise could not.
   This is a strict _quality_ win over mono, independent of stereo.
2. **Gives consistency for free** — both eyes draw from the same shared sample
   domain, so they agree by construction.

This _replaces_ the mono technique + sync rather than patching it.

**This is already implemented for SSR — it is the Class-B reference.** Water/wetness
screen-space reflections (`ISReflectionsRayTracing.hlsl`) march through both eyes via
`Stereo::ResolveMonoUVForEye(raySample, eyeIndex, sampleUV, sampleEyeIndex)`: each ray
sample resolves to whichever eye's frame actually contains it, so when a reflection ray
leaves eye 0's frame it continues into eye 1's — recovering off-screen reflectors — and
the fade takes the closer of the two eyes for consistency (it cites Wu et al. 2024 /
SCSSR directly). So SSR is not a rivalry source; it is the worked example.

**SSGI already does this too.** `Stereo::ResolveMonoUVForEye` is the canonical Class-B
helper in `VR.hlsli`, but SSGI's ray march (`gi.cs:187-220`) **already marches across the
seam**: each sample computes its own `sampleEyeIndex`, and when a ray crosses x=0.5 it
transforms the sample between eye spaces (`WorldToView(ViewToWorld(samplePos, sampleEye),
eye)`). So the off-screen-recovery half is done. What `stereoSync.cs` still reconciles is
the **stochastic estimate** divergence — each eye marches with different rays/noise, so
it produces a different _estimate_ of the (view-independent) AO/diffuse even when both
see the same geometry. Cross-eye _sampling_ doesn't fix that; the estimate-level fix is
sharing the sample budget (Class A) or the bilateral sync. Net: SSGI's Class-B leverage
is already shipped; the remaining SSGI work is Class A (estimate sharing), not marching.

### The sync passes become the fallback

Once Class A effects share their result and Class B effects march stereo-coherently,
the bilateral sync passes are needed only where neither has been implemented yet.
Unifying them into `Stereo::StereoSync*` first is what makes this migration a series
of localized per-effect changes instead of a three-way rewrite.

## Per-effect roadmap

| Effect                        | Class | Target approach                                          | Status                                                           |
| ----------------------------- | ----- | -------------------------------------------------------- | ---------------------------------------------------------------- |
| Screen-space shadows (SSS)    | A     | Compute eye 0, reproject result, recompute disocclusion  | **implemented, default-off, A/B pending**                        |
| LLF per-light contact shadows | A     | View-stable noise seed (Tier-1 screen-stable shipped)    | Tier-2 parallax-correct deferred — hot-path cost, A/B-need-first |
| SSGI (AO + diffuse IL)        | A     | Share estimate from eye 0 (or shared sample budget)      | planned                                                          |
| SSGI specular IL / radiance   | B     | Cross-eye march in `gi.cs`                               | **already shipped** (gi.cs marches across the seam)              |
| SSR (water / wetness)         | B     | Stereo-coherent cross-eye march (SCSSR)                  | **shipped** (`ResolveMonoUVForEye`, reference impl)              |
| Composite `StereoBlend`       | —     | Stays the optional "last-ditch" global net (default off) | shipped                                                          |
| Unified `Stereo::StereoSync*` | —     | Shared bilateral implementation / fallback               | shipped (`refactor/unify-stereo-sync`)                           |

Sequencing rationale: SSS first — it is purely Class A, has no temporal-accumulation
state to complicate the reproject, and #141 already proves the reprojection
machinery. SSGI follows (its accumulation/denoise history makes result-sharing more
involved); its cross-eye march can reuse the already-shipped SSR machinery. Class B
for SSR is **already done** — it is the reference, not pending work.

## Broader VR seams beyond screen-space (audit 2026-06-15)

A full feature-set sweep found the same failure modes outside the screen-space effects.
Verified `file:line` findings, highest value first:

1. **Eye-unstable noise seed — codebase-wide root cause.** The base pipeline seeds
   `Random::InterleavedGradientNoise` from the raw SBS pixel coordinate
   (`SV_Position`, `FrameCount`). The same world point gets a different seed per eye →
   rivalry on everything the noise drives: shadow penumbra PCF rotation
   (`Utility.hlsl`), contact-shadow band edges, hair outlines, stochastic LOD/mip,
   parallax dither, sky/VL banding dither (`Lighting.hlsl:963` + many consumers,
   `ISFullScreenVR.hlsl:60`, `ISApplyVolumetricLighting.hlsl:51`, `Sky.hlsl:243`,
   `ExtendedMaterials.hlsli:77`, `WaterParallax.hlsli:63`). **One eye-stable IGN seed
   in `Common/Random.hlsli` fixes all of them at the root** (the
   `GetContactShadowNoiseCoord` pattern generalized); flat path unchanged. Lighting hot
   path → in-headset A/B. This is the highest-leverage VR change in the repo.
2. **SSS Burley diffusion samples across the SBS seam** (`Burley.hlsli:107`) — clamps to
   `[0,1]` with no eye awareness, so left-eye diffusion bleeds right-eye skin in. Sibling
   `SeparableSSS` clamps correctly. One-line fix: `Stereo::ClampToEyeUV(sampleUV,
eyeIndex)` (eyeIndex already in scope). `fix:`.
3. **VL horizontal bilateral blur tiles span both eyes** (`ISVolumetricLightingBlurHCS.hlsl:33-53`)
   — cross-eye VL smear in a center band. Eye-aware tile clamp. `fix:`.

Leave / minor: Skylighting probe array anchored to eye 0 (a single shared world-space
probe field is the correct Class-A move — leave); Dynamic Cubemaps capture reads only
eye 0's SBS half (could union both eyes for coverage — small win, optional).

Swept clean (correct intrinsic divergence, do not "fix"): SSR (cross-eye march, above),
LLF cluster light grid (deliberate union of both eyes' frusta), terrain/cloud shadows
(world-space), water caustics, hair self-shadow, IBL SH, cubemap specular — all thread
`eyeIndex` correctly.

## Validation

These passes are VR-only and runtime-compiled, so CI shader validation does not
cover them. Each change is gated on the SE + VR runtime test:

-   **SE**: smoke only (the changes are VR-guarded; SE confirms the shared includes
    still compile and nothing regresses).
-   **VR**: the real gate. Capture the same scene/camera with the new path vs. the
    shipping path (`tools/taa-renderdoc-ab.py` / RenderDoc), confirm the result is
    correct and rivalry-free, and measure the per-pass GPU cost in Tracy (express the
    delta as a percent of the 90 fps budget, not raw ms — null-driver wall-clock is
    unreliable).

A Class A result-share is expected to be a net **perf** win (one eye's compute
removed) and a consistency win; label it `perf:` with the measured before/after.

## Risks

-   **Disocclusion handling is the crux of Class A.** A naive reproject leaves holes
    where eye 1 sees what eye 0 cannot. The recompute-the-sliver path must be correct,
    or the holes read as missing shadows/AO at silhouette edges.
-   **Class B stereo marching costs bandwidth** (sampling two G-buffers). It must pay
    for itself in recovered off-screen quality; measure before committing.
-   **Do not regress the #141 reprojection** — it shares the G-buffer and is
    net-positive; this work layers on top of it, it does not replace it.
