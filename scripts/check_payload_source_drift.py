#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Report when the pinned Roblox payload metadata has fallen behind.

Two kinds of drift make a first launch fail, and neither one shows up until a
user hits it:

* the compatibility catalog no longer lists the Roblox version the provider
  publishes, so every new install lands on a version Roblox has since retired;
* the preferred supported profile has no pinned bootstrap source, so the
  pinned bootstrap path cannot produce the version the updater falls back to.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

MAX_MANIFEST_BYTES = 4 * 1024 * 1024
PACKAGE = "com.roblox.client"
ARCHITECTURE = "x86_64"


class DriftError(RuntimeError):
    """Raised when the pinned metadata cannot be inspected at all."""


def _load(path: Path) -> dict:
    try:
        encoded = path.read_bytes()
    except OSError as error:
        raise DriftError(f"cannot read {path}") from error
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise DriftError(f"{path} exceeds its size limit")
    try:
        document = json.loads(encoded.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise DriftError(f"cannot parse {path}") from error
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise DriftError(f"{path} has an unsupported schema")
    return document


def supported_profiles(catalog: dict) -> list[dict]:
    profiles = catalog.get("profiles")
    if not isinstance(profiles, list):
        raise DriftError("compatibility catalog has no profiles")
    return [
        profile
        for profile in profiles
        if isinstance(profile, dict)
        and profile.get("status") == "supported"
        and profile.get("default_allowed") is True
        and isinstance(profile.get("version_name"), str)
        and isinstance(profile.get("version_code"), int)
    ]


def preferred_profile(catalog: dict) -> dict:
    """The profile the updater falls back to, matching the C++ coordinator."""
    profiles = supported_profiles(catalog)
    if not profiles:
        raise DriftError("compatibility catalog has no supported profile")
    return max(profiles, key=lambda profile: profile["version_code"])


def pinned_versions(sources: dict) -> set[str]:
    entries = sources.get("sources")
    if not isinstance(entries, list):
        raise DriftError("bootstrap source manifest has no sources")
    return {
        entry["version_name"]
        for entry in entries
        if isinstance(entry, dict)
        and entry.get("package") == PACKAGE
        and entry.get("abi") == ARCHITECTURE
        and isinstance(entry.get("version_name"), str)
    }


def latest_published_version(provider: Path) -> tuple[str, int]:
    completed = subprocess.run(
        [sys.executable, str(provider), "--check", "--package", PACKAGE,
         "--arch", ARCHITECTURE],
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    if completed.returncode != 0:
        raise DriftError(
            "provider metadata is unavailable: "
            + (completed.stderr.strip() or "no diagnostic")
        )
    try:
        reported = json.loads(completed.stdout)
        return str(reported["latest_version_name"]), int(
            reported["latest_version_code"]
        )
    except (json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        raise DriftError("provider metadata is not understood") from error


def findings(catalog: dict, sources: dict, latest: tuple[str, int] | None
             ) -> list[str]:
    reported: list[str] = []
    preferred = preferred_profile(catalog)
    pinned = pinned_versions(sources)
    if preferred["version_name"] not in pinned:
        reported.append(
            f"The preferred supported profile {preferred['version_name']} "
            f"({preferred['version_code']}) has no pinned bootstrap source. "
            "The pinned bootstrap path cannot produce the version the updater "
            "falls back to."
        )
    if latest is not None:
        latest_name, latest_code = latest
        if latest_code > preferred["version_code"]:
            reported.append(
                f"Roblox {latest_name} ({latest_code}) is published, but the "
                f"newest supported profile is {preferred['version_name']} "
                f"({preferred['version_code']}). New installs land on a "
                "version Roblox may already have retired."
            )
    return reported


def main(argv: list[str] | None = None) -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--catalog", type=Path,
        default=root / "config/roblox_compatibility.json")
    parser.add_argument(
        "--sources", type=Path,
        default=root / "config/roblox_bootstrap_sources.json")
    parser.add_argument(
        "--provider", type=Path,
        default=root / "scripts/apk_providers/direct_apkpure.py")
    parser.add_argument(
        "--check-latest", action="store_true",
        help="also ask the provider which version is published")
    arguments = parser.parse_args(argv)

    try:
        catalog = _load(arguments.catalog)
        sources = _load(arguments.sources)
        latest = (
            latest_published_version(arguments.provider)
            if arguments.check_latest
            else None
        )
        reported = findings(catalog, sources, latest)
    except DriftError as error:
        print(f"payload source drift: {error}", file=sys.stderr)
        return 2

    if not reported:
        print("Pinned Roblox payload metadata is current.")
        return 0
    for finding in reported:
        print(f"- {finding}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
