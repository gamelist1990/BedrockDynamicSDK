# Vanilla ID catalog

These JSON catalogs are generated from Mojang's `@minecraft/vanilla-data`
package (npm metadata: MIT). The JSON records the upstream version and archive
integrity. `data/manifest.json` maps each game build to its catalog:
1.26.52.3 uses the 1.26.51 data release; 1.26.60.5 and 1.26.65.1 use 1.26.60.
This is upstream release data, not an exhaustive registry verified against each
exact Windows executable.

The importer supplements `minecraft:item` (dropped stacks) when upstream omits
it. This ID was confirmed with a Bedrock 1.26.65.1 entity selector; it is an
entity constant, not an inventory item constant.

To import a new version, fetch the named package without running npm lifecycle
scripts, verify its integrity against registry metadata, and run:

```sh
npm pack --ignore-scripts @minecraft/vanilla-data@1.26.60
python3 tools/vanilla_data.py import minecraft-vanilla-data-1.26.60.tgz data/vanilla/1.26.60.json
```

Update the version's `vanilla_data` mapping. Normal CMake builds need no npm
or network to generate `bedrock/sdk/vanilla.hpp` and `sdk/generated/vanilla-data.ts`.
