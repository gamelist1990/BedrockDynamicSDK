# Profile data

Canonical profiles use `data/<version>/windows-x64/manifest.json`.
The manifest records exact executable identity, protocol identity, and each class/analysis/metadata part with SHA-256.
`data/manifest.json` indexes the version manifests and their hashes.

Class files contain `symbols`, `layouts`, and `virtual_methods`. References use stable `Class::function` identifiers.
Duplicate definitions and invalid paths/checksums are rejected during assembly.
Native function entries, calling conventions and object fields have separate verification scopes.

The SDK fetches matching manifests and class parts, or embeds them at build time. Historical `analysis/` files are not required for runtime resolution.
There is no automatic flat windows-x64.json generation. An explicit CheckSig export may be created with:

```sh
python tools/profile_parts.py assemble data/<version>/windows-x64/manifest.json --output profile.json
```

See [structure](sdk-structure.md), [maintenance](profile-maintenance.md) and [current status](../data/1.26.60.5/README.md).
