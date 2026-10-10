import re
import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


class Effects11HDRTests(unittest.TestCase):
    def test_present_frame_and_peak_brightness(self):
        self.run_hdr_case(effects11_enabled=True)

    def test_build_without_effects11_preserves_native_hdr(self):
        self.run_hdr_case(effects11_enabled=False)

    def run_hdr_case(self, effects11_enabled):
        effects = (ROOT / "src/Features/Effects11.cpp").read_text(encoding="utf-8-sig")
        hdr = (ROOT / "src/Features/HDRDisplay.cpp").read_text(encoding="utf-8-sig")
        header = (ROOT / "src/Features/HDRDisplay.h").read_text(encoding="utf-8-sig")
        source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
using uint = uint32_t;
namespace logger {
template<class... T> void error(T&&...) {}
}
namespace RE {
using RENDER_TARGET = unsigned;
struct TweenMenu { static constexpr int MENU_NAME = 1; };
}
struct Effects11 {
    uint tonemapReplacedFrame = UINT32_MAX;
    bool tonemapFailed = false;
    bool RenderTonemap(RE::RENDER_TARGET, RE::RENDER_TARGET);
    bool ReplacedTonemapperThisFrame() const;
};
struct EffectManager {
    bool initialized = true, writesOutput = true;
    static EffectManager& GetSingleton() { static EffectManager instance; return instance; }
    bool IsInitialized() const { return initialized; }
    bool ExecuteEffects(int&, int&) { return writesOutput; }
};
namespace globals {
struct State {
    uint frameCount = 10;
    bool IsMainOrLoadingMenuOpen(void*) const { return false; }
} stateStorage;
auto* state = &stateStorage;
namespace game {
struct UI { bool IsMenuOpen(int) const { return false; } } uiStorage;
auto* ui = &uiStorage;
struct Renderer {
    struct Data { std::array<int, 2> renderTargets{}; } data;
    Data& GetRuntimeData() { return data; }
} rendererStorage;
auto* renderer = &rendererStorage;
}
namespace features {
Effects11 effects11;
struct LinearLighting { bool IsLinearLightingActive() const { return false; } } linearLighting;
}
}
struct HDRDisplay {
    SETTINGS;
    CONSTANTS
    struct DATA;
    Settings settings;
    bool IsFGCompositingThisFrame() const { return false; }
    HDRDataCB BuildHDRData() const;
};
METHODS
int main() {
    auto& effects = globals::features::effects11;
    auto& manager = EffectManager::GetSingleton();
    auto& frame = globals::state->frameCount;
    HDRDisplay hdr;
    hdr.settings.enableHDR = true;
    hdr.settings.hdrPaperWhite = 203;
    hdr.settings.hdrPeakNits = 800;
    assert(!effects.ReplacedTonemapperThisFrame());
    assert(effects.RenderTonemap(0, 1));
    ++frame;
    assert(effects.ReplacedTonemapperThisFrame());
    auto data = hdr.BuildHDRData();
#ifdef ENABLE_EFFECTS11
    assert(data.applyAutoHDR == 1.0f && data.peakNits == 500.0f);
#else
    assert(data.applyAutoHDR == 0.0f && data.peakNits == 800.0f);
#endif
    assert(data.paperWhite == 203.0f);
    assert(hdr.settings.hdrPeakNits == 800);
    hdr.settings.hdrPeakNits = 400;
    assert(hdr.BuildHDRData().peakNits == 400.0f);
    hdr.settings.hdrPeakNits = 1000;
#ifdef ENABLE_EFFECTS11
    assert(hdr.BuildHDRData().peakNits == 500.0f);
#else
    assert(hdr.BuildHDRData().peakNits == 1000.0f);
#endif
    manager.writesOutput = false;
    assert(!effects.RenderTonemap(0, 1));
    ++frame;
    assert(!effects.ReplacedTonemapperThisFrame());
    data = hdr.BuildHDRData();
    assert(data.applyAutoHDR == 0.0f && data.peakNits == 1000.0f);
    manager.initialized = false;
    manager.writesOutput = true;
    assert(!effects.RenderTonemap(0, 1));
    ++frame;
    assert(!effects.ReplacedTonemapperThisFrame());
    manager.initialized = true;
    frame = UINT32_MAX;
    assert(effects.RenderTonemap(0, 1));
    ++frame;
    assert(effects.ReplacedTonemapperThisFrame());
}
'''
        source = source.replace("SETTINGS", braced(header, "struct Settings"))
        source = source.replace("CONSTANTS", "\n".join(re.findall(
            r"static constexpr uint k(?:HdrPeakNitsMin|HdrPeakNitsMax|AutoHDRMaxNits) = [^;]+;", header)))
        source = source.replace("DATA", braced(header, "HDRDataCB\n"))
        source = source.replace("METHODS", "\n".join((
            braced(effects, "bool Effects11::RenderTonemap("),
            braced(effects, "bool Effects11::ReplacedTonemapperThisFrame("),
            braced(hdr, "HDRDisplay::HDRDataCB HDRDisplay::BuildHDRData("),
        )))
        if effects11_enabled:
            source = "#define ENABLE_EFFECTS11\n" + source
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)


if __name__ == "__main__":
    unittest.main()
