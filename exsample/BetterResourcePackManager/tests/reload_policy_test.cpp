#include <bedrock/api/resource_packs.hpp>
#include <cassert>

int main() {
    using Policy = bedrock::ResourcePackMenuPolicy;
    assert(Policy::liveGlobalSelection("screen_world_controls_and_settings - global_texture_pack_tab", true));
    assert(Policy::liveGlobalSelection("screen_world_controls_and_settings_fullscreen - global_texture_pack_tab", true));
    assert(!Policy::liveGlobalSelection("screen_world_controls_and_settings - global_texture_pack_tab", false));
    assert(!Policy::liveGlobalSelection("screen_world_controls_and_settings - level_texture_pack_tab", true));
    assert(!Policy::liveGlobalSelection("unrelated_global_texture_pack_tab", true));
    assert(!Policy::liveGlobalSelection("progress_popup", true));
    // Immediate apply must not normalize/drop selected reference identity.
    for (bool original : {false, true}) {
        const auto flags = Policy::immediateApplyFlags(original);
        assert(flags.nativeSelectionMode == original && flags.save);
    }
    bedrock::ResourcePackMenuPolicy policy;
    assert(!policy.preGame(false, "unrelated_global_texture_pack_tab", true));
    assert(!policy.preGame(false, "hud_screen", true));
    assert(!policy.preGame(false, "screen_world_controls_and_settings - video_tab", true));
    assert(policy.preGame(false, "screen_world_controls_and_settings - global_texture_pack_tab",
                          true));
    assert(policy.preGame(false, "screen", true));
    assert(policy.preGame(false, "progress_popup", true));
    assert(!policy.preGame(false, "pause_screen", true));
    assert(!policy.preGame(false, "progress_popup", true));
    assert(policy.preGame(true, "hud_screen", true));
    assert(policy.preGame(
        false, "screen_world_controls_and_settings_fullscreen - global_texture_pack_tab", true));
    assert(!policy.preGame(false, "screen_world_controls_and_settings - video_tab", true));
    assert(!policy.preGame(false, "screen", true));
    assert(policy.preGame(false, "screen_world_controls_and_settings - global_texture_pack_tab",
                          true));
    assert(!policy.preGame(false, "start_screen", false));
    assert(!policy.preGame(false, "screen", true));
    assert(policy.preGame(true, "start_screen", false));
    bedrock::ResourcePackMenuPolicy otherClient;
    assert(!otherClient.preGame(false, "screen", true));
}
