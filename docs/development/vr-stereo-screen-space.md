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

### Class B — share the sample domain (stereo-coherent marching)

For genuinely view-dependent effects, the modern VR-native approach is to ray-march
through the **union of both eyes' G-buffers/color**, not one:

1. **Recovers off-screen data** — a reflector/occluder off-screen in eye 0 may be
   on-screen in eye 1, so a reflection or GI ray finds a hit it otherwise could not.
   This is a strict _quality_ win over mono, independent of stereo.
2. **Gives consistency for free** — both eyes draw from the same shared sample
   domain, so they agree by construction.

This _replaces_ the mono technique + sync rather than patching it. It is the
direction of Stereo-Coherent SSR (Wu et al. 2024 / SCSSR) and generalizes to
specular GI.

### The sync passes become the fallback

Once Class A effects share their result and Class B effects march stereo-coherently,
the bilateral sync passes are needed only where neither has been implemented yet.
Unifying them into `Stereo::StereoSync*` first is what makes this migration a series
of localized per-effect changes instead of a three-way rewrite.

## Per-effect roadmap

| Effect                        | Class | Target approach                                          | Status                                    |
| ----------------------------- | ----- | -------------------------------------------------------- | ----------------------------------------- |
| Screen-space shadows (SSS)    | A     | Compute eye 0, reproject result, recompute disocclusion  | **implemented, default-off, A/B pending** |
| SSGI (AO + diffuse IL)        | A     | Share estimate from eye 0 (or shared sample budget)      | planned                                   |
| SSGI specular IL              | B     | Stereo-coherent marching                                 | planned                                   |
| SSR                           | B     | Stereo-coherent marching (SCSSR-style)                   | planned                                   |
| Composite `StereoBlend`       | —     | Stays the optional "last-ditch" global net (default off) | shipped                                   |
| Unified `Stereo::StereoSync*` | —     | Shared bilateral implementation / fallback               | shipped (`refactor/unify-stereo-sync`)    |

Sequencing rationale: SSS first — it is purely Class A, has no temporal-accumulation
state to complicate the reproject, and #141 already proves the reprojection
machinery. SSGI follows (its accumulation/denoise history makes result-sharing more
involved). Class B (SSR, specular) is the larger research effort and comes last.

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
