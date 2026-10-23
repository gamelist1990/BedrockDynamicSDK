#include <bedrock/native_actors.hpp>

#include <cassert>
#include <string>

int main() {
    constexpr auto profile = R"json({
      "native_actors": {
        "schema": 1,
        "pointer_size": 8,
        "actor_context": {
          "actor_offset": "0x8",
          "registry_offset": "0x8",
          "entity_id_offset": "0x10"
        },
        "actor_owner": {
          "component_hash": "0x85B93800",
          "component_hash_alternate": "0x0",
          "actor_pointer_offset": "0x0"
        },
        "actor_type": { "component_hash": "0x4F6BA419" },
        "runtime_id": { "component_hash": "0xFC0DBBB5" },
        "aabb_shape": {
          "component_hash": "0xBAC1B3CF",
          "component_hash_alternate": "0x0",
          "minimum_offset": "0x0",
          "maximum_offset": "0xC"
        },
        "state_vector": {
          "component_hash": "0x1B5D5238",
          "component_hash_alternate": "0x0",
          "position_offset": "0x0"
        },
        "actor_rotation": {
          "component_hash": "0x75DF36B7",
          "component_hash_alternate": "0x0",
          "pitch_offset": "0x0",
          "yaw_offset": "0x4"
        }
      }
    })json";

    bedrock::NativeActorLayout layout;
    std::string error;
    assert(bedrock::parseNativeActorLayout(profile, layout, error));
    assert(layout.actorOwnerComponentHash == 0x85B93800);
    assert(layout.actorTypeComponentHash == 0x4F6BA419);
    assert(layout.runtimeIdComponentHash == 0xFC0DBBB5);
    assert(layout.aabbShapeComponentHash == 0xBAC1B3CF);
    assert(layout.valid());

    constexpr auto missingViewComponent = R"json({
      "native_actors": {
        "schema": 1,
        "pointer_size": 8,
        "actor_context": { "actor_offset": 8, "registry_offset": 8, "entity_id_offset": 16 },
        "actor_owner": { "component_hash": 1, "component_hash_alternate": 0, "actor_pointer_offset": 0 },
        "aabb_shape": { "component_hash": 2, "component_hash_alternate": 0, "minimum_offset": 0, "maximum_offset": 12 },
        "state_vector": { "component_hash": 3, "component_hash_alternate": 0, "position_offset": 0 },
        "actor_rotation": { "component_hash": 4, "component_hash_alternate": 0, "pitch_offset": 0, "yaw_offset": 4 }
      }
    })json";
    assert(!bedrock::parseNativeActorLayout(missingViewComponent, layout, error));
    assert(!error.empty());
}
