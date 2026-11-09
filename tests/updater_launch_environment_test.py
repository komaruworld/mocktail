#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import tempfile


binary, helper = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="mocktail_updater_launch_") as directory:
    root = Path(directory)
    for index, (configured, arguments, expected) in enumerate([
        ("system", [], "system"),
        ("system", ["--graphics", "opengl"], "opengl"),
        ("direct-vulkan", [], "direct-vulkan"),
    ]):
        case = root / str(index)
        config = case / "config"
        config.mkdir(parents=True)
        (config / "config.yaml").write_text(
            f"version: 1\ngraphics:\n  backend: {configured}\n"
            "performance:\n  gamemode: off\n"
        )
        capture = case / "backend"
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("MOCKTAIL_") and key not in
                       ("ROBLOX_LIB_PATH", "DISPLAY", "WAYLAND_DISPLAY")}
        environment.update({
            "HOME": str(case),
            "MOCKTAIL_CONFIG_ROOT": str(config),
            "MOCKTAIL_DATA_ROOT": str(case / "data"),
            "MOCKTAIL_CACHE_ROOT": str(case / "cache"),
            "MOCKTAIL_STATE_ROOT": str(case / "state"),
            "MOCKTAIL_UPDATE_HELPER": helper,
            "MOCKTAIL_TEST_UPDATE_BACKEND_CAPTURE": str(capture),
            "MOCKTAIL_TEST_UPDATE_EXIT": "7",
            "MOCKTAIL_DISABLE_FAILURE_DIALOG": "1",
            "MOCKTAIL_DISABLE_SUPPORT_BUNDLE": "1",
        })
        completed = subprocess.run(
            [binary, "--headless", *arguments], env=environment, cwd=case,
            capture_output=True, text=True, timeout=15,
        )
        assert completed.returncode == 1, completed.stderr
        assert capture.is_file(), completed.stdout + completed.stderr
        assert capture.read_text().strip() == expected, capture.read_text()
print("Configured and command-line graphics backends reached the updater")
