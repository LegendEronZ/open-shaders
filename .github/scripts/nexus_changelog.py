"""Helpers for the Nexus changelog post in .github/workflows/nexus-upload.yaml.

The Nexus "save documentation" endpoint that BUTR.NexusUploader (`unex
changelog`) posts to *appends* the supplied text to the existing changelog
rather than replacing it. Re-running the upload for a version already on Nexus
therefore appends that version's notes a second time — the "doubled up" entries
reported in issue #198.

version_already_on_nexus() lets the workflow skip the changelog for any version
already present so re-runs stay idempotent. strip_audit() drops the internal
Feature Version Audit block from the release body before it is posted.
"""

from __future__ import annotations

import json
import re
import urllib.error
import urllib.request

# Trailing "# Feature Version Audit" block appended to the release body is
# internal bookkeeping, not user-facing changelog content.
_AUDIT_RE = re.compile(r"\n---\n+# Feature Version Audit\b.*", re.DOTALL)


def strip_audit(body: str) -> str:
    """Drop the internal Feature Version Audit section from a release body."""
    return _AUDIT_RE.sub("", body or "").strip()


def version_already_on_nexus(
    api_key: str,
    game_id: str,
    mod_id: str,
    version: str,
    user_agent: str = "open-shaders-ci/1.0",
    timeout: int = 15,
) -> bool | None:
    """Return True/False if `version` is on the mod's file list, None if unknown.

    None means the query could not be completed (missing key/mod id, network or
    auth error) — callers should treat that as "don't suppress the changelog".
    """
    if not api_key or not mod_id:
        return None
    url = f"https://api.nexusmods.com/v1/games/{game_id}/mods/{mod_id}/files.json"
    req = urllib.request.Request(
        url,
        headers={
            "apikey": api_key,
            "User-Agent": user_agent,
            "Accept": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            files = json.load(resp).get("files", [])
    except (urllib.error.URLError, TimeoutError, ValueError, OSError):
        return None
    return any(f.get("version") == version for f in files)
