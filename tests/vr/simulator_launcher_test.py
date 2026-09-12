"""Check process-local runtime selection and preparation independently of VR hardware."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


launcher = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mocktail-simulator-launcher-") as directory:
    root = Path(directory)
    build = root / "build with spaces"
    build.mkdir()
    source = root / "source"
    source.mkdir()
    library = source / "runtime.so"
    library.write_bytes(b"runtime fixture for preparation only")
    manifest = source / "manifest.json"
    manifest.write_text(json.dumps({"runtime": {"library_path": "./runtime.so"}}))
    command = [sys.executable, str(launcher), "--build-dir", str(build)]
    result = subprocess.run([*command, "--prepare", str(manifest)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    prepared = build / "vr-simulator/runtime.json"
    document = json.loads(prepared.read_text())
    relative_library = Path(document["runtime"]["library_path"])
    assert not relative_library.is_absolute()
    assert (prepared.parent / relative_library).read_bytes() == library.read_bytes()

    # Moving the prepared build must not depend on the original /tmp/source path.
    relocated = root / "relocated build"
    build.rename(relocated)
    build = relocated
    prepared = build / "vr-simulator/runtime.json"
    command = [sys.executable, str(launcher), "--build-dir", str(build)]
    probe = build / "mocktail-vr-probe"
    probe.write_text("""#!/usr/bin/env python3
import json, os, sys
names = ('XR_RUNTIME_JSON', 'SIMULATED_ENABLE', 'XRT_COMPOSITOR_NULL',
         'XRT_COMPOSITOR_FORCE_XCB', 'XRT_COMPOSITOR_FORCE_WAYLAND',
         'XDG_CONFIG_HOME', 'XDG_RUNTIME_DIR')
print(json.dumps({'args': sys.argv[1:], 'env': {key: os.environ.get(key) for key in names}}))
sys.exit(7)
""")
    probe.chmod(0o755)
    env = os.environ.copy()
    env.update(DISPLAY=":123", WAYLAND_DISPLAY="wayland-test",
               XR_RUNTIME_JSON=str(root / "real-headset.json"),
               XRT_COMPOSITOR_NULL="1", XRT_COMPOSITOR_FORCE_WAYLAND="1",
               XDG_CONFIG_HOME=str(root / "user-config"), XDG_RUNTIME_DIR=str(root / "desktop-runtime"))
    original_selection = env["XR_RUNTIME_JSON"]
    original_config = Path(env["XDG_CONFIG_HOME"])
    original_config.mkdir()
    marker = original_config / "keep"
    marker.write_text("preserve user settings")

    for graphical_backend in ("xcb", "wayland"):
        if graphical_backend == "wayland":
            env.pop("DISPLAY")
        result = subprocess.run([*command, "--frames", "4"], env=env,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 7, (result.stdout, result.stderr)
        child = json.loads(result.stdout.splitlines()[-1])
        assert child["args"] == ["--scene", "--frames", "4"]
        selected = child["env"]
        assert selected["XR_RUNTIME_JSON"] == str(prepared)
        assert selected["SIMULATED_ENABLE"] == "1" and selected["XRT_COMPOSITOR_NULL"] == "0"
        assert selected["XRT_COMPOSITOR_FORCE_XCB"] == ("1" if graphical_backend == "xcb" else "0")
        assert selected["XRT_COMPOSITOR_FORCE_WAYLAND"] == ("0" if graphical_backend == "xcb" else "1")
        assert selected["XDG_CONFIG_HOME"] != str(original_config)
        assert selected["XDG_RUNTIME_DIR"] == env["XDG_RUNTIME_DIR"]
        assert env["XR_RUNTIME_JSON"] == original_selection
        assert marker.read_text() == "preserve user settings"
        assert not (original_config / "openxr").exists()
        print(graphical_backend + " process-local selection: passed")

    env.pop("WAYLAND_DISPLAY")
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
    assert result.returncode == 2 and "graphical desktop" in result.stderr, result
    result = subprocess.run([*command, "--runtime", str(root / "missing.json")], env=env,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 2 and "No simulated Monado runtime" in result.stderr, result
    for count in ("0", "-1", "4294967296"):
        result = subprocess.run([*command, "--frames", count], env=env,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and "positive 32-bit integer" in result.stderr, result
    print("missing runtime/display and frame limit diagnostics: passed")
