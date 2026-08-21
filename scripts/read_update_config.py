#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

"""Read the updater-owned section of Mocktail's versioned YAML config."""

from __future__ import annotations

import json
import os
import pathlib
import re
import stat
import sys

import yaml


MAX_CONFIG_BYTES = 1024 * 1024
SUPPORTED_SOURCES = {"apk-pure"}
VERSION_PATTERN = re.compile(r"^[0-9][0-9A-Za-z._-]{0,127}$")


def fail(message: str) -> None:
    print(f"update config: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_regular_text(
    path: pathlib.Path,
    maximum_bytes: int,
    description: str,
    *,
    missing_ok: bool = False,
) -> tuple[str, os.stat_result] | None:
    try:
        descriptor = os.open(
            path,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
        )
    except FileNotFoundError:
        if missing_ok:
            return None
        fail(f"{description} is unavailable")
    except OSError:
        fail(f"{description} must be a regular file")

    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_size < 0
            or metadata.st_size > maximum_bytes
        ):
            fail(
                f"{description} must be a regular file no larger than "
                f"{maximum_bytes // 1024} KiB"
            )
        encoded = bytearray()
        while len(encoded) <= maximum_bytes:
            chunk = os.read(
                descriptor,
                min(64 * 1024, maximum_bytes + 1 - len(encoded)),
            )
            if not chunk:
                break
            encoded.extend(chunk)
        if len(encoded) > maximum_bytes:
            fail(f"{description} exceeds its size limit")
    except OSError:
        fail(f"cannot read {description}")
    finally:
        os.close(descriptor)

    try:
        return encoded.decode("utf-8"), metadata
    except UnicodeError:
        fail(f"{description} is not UTF-8")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: read_update_config.py CONFIG.yaml")
    path = pathlib.Path(sys.argv[1])
    loaded = read_regular_text(
        path,
        MAX_CONFIG_BYTES,
        "configuration",
        missing_ok=True,
    )
    if loaded is None:
        document = {}
    else:
        contents, _ = loaded
        try:
            document = yaml.safe_load(contents)
        except yaml.YAMLError:
            fail("cannot parse configuration")
    if document is None:
        document = {}
    if not isinstance(document, dict):
        fail("configuration root must be a mapping")
    if document.get("version", 1) != 1:
        fail("only configuration version 1 is supported")
    updates = document.get("updates", {})
    if not isinstance(updates, dict):
        fail("updates must be a mapping")
    allowed = {
        "automatic",
        "testing_latest_only",
        "source",
        "version",
        "launch_after_update",
    }
    unknown = set(updates) - allowed
    if unknown:
        fail(f"unknown updates key: {sorted(unknown)[0]}")
    for key in ("automatic", "testing_latest_only", "launch_after_update"):
        if key in updates and not isinstance(updates[key], bool):
            fail(f"updates.{key} must be true or false")
    if "source" in updates and updates["source"] not in SUPPORTED_SOURCES:
        fail("updates.source must be apk-pure")
    if "version" in updates and (
        not isinstance(updates["version"], str)
        or VERSION_PATTERN.fullmatch(updates["version"]) is None
    ):
        fail("updates.version must be a Roblox version name")
    updates = dict(updates)
    if "testing_latest_only" in updates:
        print(
            "update config: warning: updates.testing_latest_only is no longer "
            "supported and is ignored",
            file=sys.stderr,
        )
        del updates["testing_latest_only"]
    print(json.dumps(updates, separators=(",", ":")))


if __name__ == "__main__":
    main()
