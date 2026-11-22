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
    # Final state (plugin-system completion): freecam consumes only the
    # public CameraControl facade; the native visual adapter it used
    # mid-migration must stay gone.
    for api in ("CameraControl", "takeStatistics", "takeEffectStatistics"):
        assert api in source, api
    for gone in ("NativeCameraVisual", "NativeOutlineRenderer", "camera_visual.hpp"):
        assert gone not in source, gone
