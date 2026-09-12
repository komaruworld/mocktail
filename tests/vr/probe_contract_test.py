"""Run the actual OpenXR loader against a discovery-only test runtime."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


probe = Path(sys.argv[1])
if sys.argv[2] == "--disabled":
    result = subprocess.run([str(probe)], text=True, capture_output=True, timeout=15)
    assert result.returncode == 2, (result.returncode, result.stdout, result.stderr)
    assert "does not include OpenXR support" in result.stdout, result.stdout
    result = subprocess.run([str(probe), "--scene", "--frames", "1"],
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 1 and "does not include OpenXR support" in result.stdout, result
    with tempfile.TemporaryDirectory(prefix="mocktail-xr-disabled-") as directory:
        root = Path(directory)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("XR_", "MOCKTAIL_"))}
        for kind in ("CONFIG", "DATA", "STATE"):
            env["MOCKTAIL_" + kind + "_ROOT"] = str(root / kind.lower())
        result = subprocess.run([sys.argv[3], "--headless", "--vr"], env=env,
                                text=True, capture_output=True, timeout=15)
        assert result.returncode == 1 and "does not include OpenXR support" in result.stderr, result
        assert not (root / "data").exists() and not (root / "state").exists()
    print("OpenXR-disabled build: passed")
    sys.exit(0)

runtime = Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="mocktail-xr-probe-") as directory:
    root = Path(directory)
    manifest = root / "runtime.json"
    manifest.write_text(json.dumps({
        "file_format_version": "1.0.0",
        "runtime": {"library_path": str(runtime.resolve())},
    }))
    environment = {k: v for k, v in os.environ.items()
                   if not k.startswith(("XR_", "MOCKTAIL_"))}
    environment["XR_RUNTIME_JSON"] = str(manifest)
    environment["XR_LOADER_DEBUG"] = "none"
    environment["XDG_CONFIG_HOME"] = str(root / "config")
    environment["XDG_DATA_HOME"] = str(root / "data")
    environment["XR_API_LAYER_PATH"] = str(root / "layers")
    (root / "layers").mkdir()
    trace = root / "trace"
    environment["MOCKTAIL_TEST_XR_TRACE"] = str(trace)

    cases = [
        ("ready", 0, "discovered successfully"),
        ("no_headset", 4, "Connect a headset"),
        ("no_vulkan", 5, "does not expose XR_KHR_vulkan_enable2"),
        ("bad_stereo", 5, "two usable stereo views"),
        ("api_error", 1, "xrGetInstanceProperties failed"),
        ("create_failure", 1, "xrCreateInstance failed"),
        ("enumeration_changed", 0, "discovered successfully"),
        ("graphics_error", 1, "xrGetVulkanGraphicsRequirements2KHR failed"),
    ]
    for scenario, code, message in cases:
        trace.unlink(missing_ok=True)
        environment["MOCKTAIL_TEST_XR_SCENARIO"] = scenario
        result = subprocess.run([str(probe)], env=environment,
                                text=True, capture_output=True, timeout=15)
        assert result.returncode == code, (scenario, result.returncode, result.stdout, result.stderr)
        assert message in result.stdout, (scenario, result.stdout, result.stderr)
        assert "SDK: 1.1.63" in result.stdout, result.stdout
        assert "Roblox VR rendering: not implemented yet" in result.stdout
        events = trace.read_text().splitlines() if trace.exists() else []
        assert events == ([] if scenario == "create_failure" else ["create", "destroy"]), (scenario, events)
        if code == 0:
            assert "Eye 0: 1440x1600" in result.stdout
            assert "Eye 1: 1440x1600" in result.stdout
            assert "minimum=1.1.0" in result.stdout
        print(scenario + ": passed")

    environment["XR_RUNTIME_JSON"] = str(root / "does-not-exist.json")
    result = subprocess.run([str(probe)], env=environment,
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 3, (result.returncode, result.stdout, result.stderr)
    assert "runtime" in result.stdout.lower(), result.stdout
    print("missing runtime: passed")
