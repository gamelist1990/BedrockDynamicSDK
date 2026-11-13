from pathlib import Path


def test_freecam_camera_visual_uses_sdk_adapter():
    base = Path(__file__).resolve().parents[1] / "exsample/Freecam"
    source = (base / "src/freecam.cpp").read_text()
    for forbidden in ("BDS_HookDirectInstall", "CullerLease", "cullerDetour",
                      "createViewDetour", "cameraRenderDetour", "updateTerrainCulling",
                      "captureVisibility", "struct VisibilitySample", "visibilityMailbox",
                      "spectatorCullerVtable",
                      "0x9a8", "0x9b8", "0x970", "0x654", "0x7c"):
        assert forbidden not in source, forbidden
    for api in ("NativeCameraVisual", "camera_visual.hpp", "noteNativeModeContext",
                "takeStatistics"):
        assert api in source, api
