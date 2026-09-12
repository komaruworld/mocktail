"""Real Vulkan rendering/readback with a scripted OpenXR session lifecycle."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


probe, runtime, mocktail = map(Path, sys.argv[1:4])
with tempfile.TemporaryDirectory(prefix="mocktail-xr-preview-") as directory:
    root = Path(directory)
    manifest = root / "runtime.json"
    manifest.write_text(json.dumps({"file_format_version": "1.0.0",
                                    "runtime": {"library_path": str(runtime.resolve())}}))
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("XR_", "MOCKTAIL_", "VK_"))}
    env.update(XR_RUNTIME_JSON=str(manifest), XR_LOADER_DEBUG="none",
               XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
               VK_LOADER_LAYERS_DISABLE="~implicit~")
    lavapipe = Path("/usr/share/vulkan/icd.d/lvp_icd.json")
    if lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    if Path("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json").exists():
        env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
    trace = root / "trace"
    env["MOCKTAIL_TEST_XR_TRACE"] = str(trace)

    def check(result, code):
        output = result.stdout + result.stderr
        assert result.returncode == code, (result.returncode, output)
        assert "Validation Error" not in output and "VUID-" not in output, output
        events = trace.read_text().splitlines() if trace.exists() else []
        assert not any(event.startswith("contract-error") for event in events), events
        assert events.count("create") == events.count("destroy"), events
        assert events.count("space-create") == events.count("space-destroy"), events
        assert events.count("session-create") == events.count("session-destroy"), events
        assert events.count("swapchain-create") == events.count("swapchain-destroy"), events
        assert events.count("frame-begin") == events.count("frame-end"), events
        assert events.count("image-acquire") == events.count("image-release"), events
        if "swapchain-destroy" in events:
            assert events.index("swapchain-destroy") < events.index("session-destroy") < events.index("destroy")
        return output, events

    cases = [
        ("preview_ready", 0, "rendered: 3, tracked: 3"),
        ("skip_render", 0, "rendered: 2, tracked: 3"),
        ("invalid_tracking", 0, "rendered: 2, tracked: 2"),
        ("image_timeout", 0, "rendered: 3, tracked: 3"),
        ("device_failure", 1, "vkCreateDevice failed"),
        ("session_failure", 1, "xrCreateSession failed"),
        ("swapchain_failure", 1, "xrCreateSwapchain failed"),
        ("bad_image_index", 1, "invalid swapchain image index"),
        ("session_loss", 1, "OpenXR session was lost"),
    ]
    for scenario, code, message in cases:
        trace.unlink(missing_ok=True)
        env["MOCKTAIL_TEST_XR_SCENARIO"] = scenario
        result = subprocess.run([str(probe), "--scene", "--frames", "3"], env=env,
                                capture_output=True, text=True, timeout=15)
        if scenario == "preview_ready" and result.returncode != 0:
            output = result.stdout + result.stderr
            if "Cannot load the host Vulkan loader" in output or "vkCreateInstance failed: -9" in output:
                print("SKIP: no Vulkan implementation available for GPU contract checks")
                sys.exit(77)
        output, events = check(result, code)
        assert message in output, (scenario, output)
        if code == 0:
            assert "pixels-verified" in events, (scenario, events)
            assert events.count("session-exit-request") == events.count("session-end") == 1
            assert "Head position (metres): 0.2, 0, 0" in output, output
        if scenario in ("skip_render", "invalid_tracking", "bad_image_index"):
            assert events.count("empty-submit") == 1, events
        if scenario == "image_timeout":
            assert events.count("image-timeout") == 1, events
        print(scenario + ": passed")

    # VR must traverse the real Roblox startup, never fall back to this scene.
    # Exercise configuration/CLI precedence with a deliberately missing payload.
    for scenario, yaml_value, switches, env_value in [
        ("yaml", "true", [], None),
        ("cli", "false", ["--vr"], "0"),
        ("environment", "false", [], "1"),
    ]:
        case = root / scenario
        config = case / "config"
        config.mkdir(parents=True)
        contents = f"version: 1\nvr:\n  enabled: {yaml_value}\n"
        (config / "config.yaml").write_text(contents)
        env.update(MOCKTAIL_CONFIG_ROOT=str(config), MOCKTAIL_DATA_ROOT=str(case / "data"),
                   MOCKTAIL_STATE_ROOT=str(case / "state"), MOCKTAIL_TEST_XR_SCENARIO="preview_auto_stop")
        if env_value is None:
            env.pop("MOCKTAIL_VR_ENABLED", None)
        else:
            env["MOCKTAIL_VR_ENABLED"] = env_value
        trace.unlink(missing_ok=True)
        result = subprocess.run([str(mocktail), "--headless", "--roblox-lib",
                                 str(case / "missing-libroblox.so"), *switches],
                                env=env, capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode != 0, (scenario, output)
        assert "missing-libroblox.so" in output, (scenario, output)
        assert not trace.exists(), (scenario, trace.read_text() if trace.exists() else "")
        assert (config / "config.yaml").read_text() == contents
        print("Mocktail VR rejects missing Roblox via " + scenario + ": passed")

    env["MOCKTAIL_TEST_XR_SCENARIO"] = "preview_ready"
    trace.unlink(missing_ok=True)
    child = subprocess.Popen([str(probe), "--scene"], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 10
        while not trace.exists() or "layer-submit" not in trace.read_text():
            assert child.poll() is None and time.monotonic() < deadline, "Scene failed to start for interrupt test"
            time.sleep(0.01)
        child.send_signal(signal.SIGINT)
        stdout, stderr = child.communicate(timeout=10)
        _, events = check(subprocess.CompletedProcess(child.args, child.returncode, stdout, stderr), 0)
        assert "session-exit-request" in events and "session-end" in events, events
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
    print("SIGINT session shutdown: passed")

    for count in ("0", "-1", "abc", "4294967296", "1x"):
        result = subprocess.run([str(probe), "--scene", "--frames", count], env=env,
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 1 and "positive integer" in result.stderr, (count, result)
    print("frame limit validation: passed")

    env["XR_RUNTIME_JSON"] = str(root / "missing-runtime.json")
    result = subprocess.run([str(probe), "--scene", "--frames", "1"], env=env,
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, result
    assert "XR_ERROR_RUNTIME_UNAVAILABLE" in result.stdout, result.stdout
    assert "mocktail-vr-simulated" in result.stdout, result.stdout
    print("missing runtime diagnostic offers simulator: passed")
