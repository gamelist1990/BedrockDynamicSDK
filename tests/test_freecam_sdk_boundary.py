from pathlib import Path


def test_freecam_camera_visual_uses_sdk_adapter():
    base = Path(__file__).resolve().parents[1] / "exsample/DynamicClient"
    source = "\n".join(path.read_text() for path in (base / "src/freecam").glob("*.cpp"))
    for forbidden in ("BDS_HookDirectInstall", "CullerLease", "cullerDetour",
                      "createViewDetour", "cameraRenderDetour", "updateTerrainCulling",
                      "captureVisibility", "struct VisibilitySample", "visibilityMailbox",
                      "spectatorCullerVtable",
                      "0x9a8", "0x9b8", "0x970", "0x654", "0x7c"):
        assert forbidden not in source, forbidden
    for api in ("NativeCameraVisual", "NativeOutlineRenderer", "camera_visual.hpp", "noteNativeModeContext",
                "takeStatistics"):
        assert api in source, api
