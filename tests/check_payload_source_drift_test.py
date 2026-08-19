#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "scripts/check_payload_source_drift.py"
SPEC = importlib.util.spec_from_file_location("check_payload_source_drift",
                                              SCRIPT)
assert SPEC is not None and SPEC.loader is not None
drift = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(drift)


def catalog(*profiles: tuple[str, int, bool]) -> dict:
    return {
        "schema_version": 1,
        "profiles": [
            {
                "version_name": name,
                "version_code": code,
                "status": "supported" if allowed else "legacy-researched",
                "default_allowed": allowed,
            }
            for name, code, allowed in profiles
        ],
    }


def sources(*versions: str) -> dict:
    return {
        "schema_version": 1,
        "sources": [
            {
                "package": "com.roblox.client",
                "version_name": version,
                "abi": "x86_64",
                "provider": "uptodown",
            }
            for version in versions
        ],
    }


class PreferredProfileTest(unittest.TestCase):
    def test_picks_the_highest_allowed_version_code(self) -> None:
        preferred = drift.preferred_profile(
            catalog(("2.725.1142", 2546, True), ("2.727.1199", 2628, True))
        )
        self.assertEqual(preferred["version_name"], "2.727.1199")

    def test_ignores_profiles_that_are_not_default_allowed(self) -> None:
        preferred = drift.preferred_profile(
            catalog(("2.725.1142", 2546, True), ("2.740.1", 2900, False))
        )
        self.assertEqual(preferred["version_name"], "2.725.1142")

    def test_rejects_a_catalog_without_a_supported_profile(self) -> None:
        with self.assertRaises(drift.DriftError):
            drift.preferred_profile(catalog(("2.721.1108", 2350, False)))


class FindingsTest(unittest.TestCase):
    def test_reports_nothing_when_metadata_is_current(self) -> None:
        reported = drift.findings(
            catalog(("2.727.1199", 2628, True)),
            sources("2.727.1199"),
            ("2.727.1199", 2628),
        )
        self.assertEqual(reported, [])

    def test_reports_an_unpinned_fallback(self) -> None:
        reported = drift.findings(
            catalog(("2.725.1142", 2546, True), ("2.727.1199", 2628, True)),
            sources("2.725.1142"),
            None,
        )
        self.assertEqual(len(reported), 1)
        self.assertIn("no pinned bootstrap source", reported[0])
        self.assertIn("2.727.1199", reported[0])

    def test_reports_a_catalog_behind_the_published_version(self) -> None:
        reported = drift.findings(
            catalog(("2.727.1199", 2628, True)),
            sources("2.727.1199"),
            ("2.734.917", 2908),
        )
        self.assertEqual(len(reported), 1)
        self.assertIn("2.734.917", reported[0])

    def test_accepts_a_catalog_ahead_of_the_published_version(self) -> None:
        reported = drift.findings(
            catalog(("2.727.1199", 2628, True)),
            sources("2.727.1199"),
            ("2.725.1142", 2546),
        )
        self.assertEqual(reported, [])

    def test_ignores_pinned_sources_for_another_architecture(self) -> None:
        manifest = sources("2.727.1199")
        manifest["sources"][0]["abi"] = "arm64-v8a"
        reported = drift.findings(
            catalog(("2.727.1199", 2628, True)), manifest, None
        )
        self.assertEqual(len(reported), 1)


class MainTest(unittest.TestCase):
    def test_exits_non_zero_on_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "catalog.json").write_text(
                json.dumps(catalog(("2.727.1199", 2628, True)))
            )
            (root / "sources.json").write_text(json.dumps(sources()))
            self.assertEqual(
                drift.main(
                    [
                        "--catalog", str(root / "catalog.json"),
                        "--sources", str(root / "sources.json"),
                    ]
                ),
                1,
            )

    def test_reports_a_broken_manifest_separately(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "catalog.json").write_text("{")
            (root / "sources.json").write_text(json.dumps(sources()))
            self.assertEqual(
                drift.main(
                    [
                        "--catalog", str(root / "catalog.json"),
                        "--sources", str(root / "sources.json"),
                    ]
                ),
                2,
            )


if __name__ == "__main__":
    unittest.main()
