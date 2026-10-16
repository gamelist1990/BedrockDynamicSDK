"""Read function evidence from an existing Bedrock Lens SQLite catalog.

Not a vendored Lens implementation: this adapter consumes the public Lens schema.
It NEVER treats a function name or BSim match as ABI-verified.
"""
from __future__ import annotations

import re
import sqlite3
from pathlib import Path

_ANONYMOUS = re.compile(r"^(?:FUN_|sub_|thunk_FUN_|nullsub_|j_)", re.I)


def targets_from_lens(database: Path, artifact_id: int, actual_exe_sha256: str,
                      limit: int = 100, name_contains: str | None = None,
                      include_anonymous: bool = False) -> list[dict]:
    if limit < 1 or limit > 50000:
        raise ValueError("limit must be in range 1..50000")
    uri = f"file:{database.resolve()}?mode=ro"
    with sqlite3.connect(uri, uri=True) as db:
        match = db.execute("SELECT sha256 FROM artifacts WHERE id = ?", (artifact_id,)).fetchone()
        if not match:
            raise ValueError(f"Lens artifact {artifact_id} not found")
        if str(match[0]).lower() != actual_exe_sha256.lower():
            raise ValueError("Lens artifact SHA256 does not match supplied PE")
        rows = db.execute(
            """SELECT name, rva, namespace FROM functions
               WHERE artifact_id = ?
               ORDER BY rva""", (artifact_id,)
        )
        result = []
        for name, rva, namespace in rows:
            name = str(name)
            if not name or (not include_anonymous and _ANONYMOUS.match(name)):
                continue
            if name_contains and name_contains.casefold() not in name.casefold():
                continue
            result.append({
                "name": name,
                "rva": int(rva),
                "source": f"bedrock-lens:artifact-{artifact_id}",
                "verified": False,
            })
            if len(result) >= limit:
                break
        return result
