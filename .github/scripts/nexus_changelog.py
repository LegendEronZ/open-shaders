"""Helpers for building the Nexus changelog payload from a GitHub release body.

Used by .github/workflows/nexus-upload.yaml. The Nexus "save documentation"
endpoint that BUTR.NexusUploader (`unex changelog`) posts to emits one
changelog row per newline in the supplied text and *appends* rows to the
existing changelog rather than replacing them. A raw multi-line release body
therefore explodes into many rows under a single version, and a re-run appends
them all again — the "doubled up" entries reported in issue #198.

release_changelog() collapses this version's notes into a single line (one row),
and version_already_on_nexus() lets the workflow skip re-posting a version whose
entry is already present so re-runs stay idempotent.
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


def release_changelog(body: str) -> str:
    """Flatten a GitHub release body into a single Nexus changelog line.

    unex posts one Nexus changelog row per '\\n', so the multi-line body must be
    collapsed to one line. Markdown bullet/heading markers are stripped and the
    individual notes are joined with ' • ' so the single row stays readable.
    """
    body = strip_audit(body)
    if not body:
        return ""
    # semantic-release opens the body with "## [x.y.z](compare-url) (date)";
    # Nexus already keys the entry by version, so drop that redundant header.
    body = re.sub(
        r"^\s*#{1,6}\s*\[?\d[\w.\-]*\]?\([^)]*\)\s*\([^)]*\)\s*\n",
        "",
        body,
        count=1,
    )
    items: list[str] = []
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith("---"):
            continue
        # Strip leading markdown heading (#) and list (*, -, +) markers.
        line = re.sub(r"^#{1,6}\s+", "", line)
        line = re.sub(r"^[*+-]\s+", "", line)
        line = line.strip()
        if line:
            items.append(line)
    return " • ".join(items)


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
