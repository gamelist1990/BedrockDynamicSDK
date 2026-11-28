# Analysis bundle tooling

`re_package.py` (v1.2.0) and `requirements.txt` are vendored unchanged from
`~/WorkSpace/re-package`, including the raw `.pdata` parser. To update them,
copy both files from the desired upstream version and rerun the bundle workflow.

Run **Analysis bundle** from the repository's Actions tab with a four-component
game version. Set the repository's Actions secret `EXE_TRANSFER_TOKEN` to a PAT
with read access to releases in `gamelist1990/bedrock-exe-transfer`. The optional
`exe_url` supplies an HTTPS target download without authentication; the PAT is
still needed to select the newest SDK profile with a matching transfer asset.
The workflow reuses the target as the reference when their versions match,
and verifies the reference's `.text` hash and PE layout against the profile.
The uploaded ZIP contains neither the executable nor local extras.

To export reference symbols locally, run from the repository root:

```sh
python -m tools.re_package.symbols_from_profile \
  data/1.26.65.1/windows-x64/manifest.json --output symbols.json
```
