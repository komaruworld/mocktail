#!/usr/bin/env python3
"""Launch the VR preview with a separately built Monado simulated runtime."""

import argparse
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile


def positive_integer(value):
    number = int(value)
    if not 0 < number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("must be a positive 32-bit integer")
    return number


def prepare_runtime(source, destination):
    """Keep the development runtime beside the build, independent of /tmp."""
    source = source.resolve()
    document = json.loads(source.read_text())
    library = Path(document["runtime"]["library_path"])
    if not library.is_absolute():
        library = source.parent / library
    if not library.is_file():
        raise ValueError(f"Runtime library does not exist: {library}")
    destination.mkdir(parents=True, exist_ok=True)
    output_library = destination / "libopenxr_monado.so"
    if library.resolve() != output_library.resolve():
        shutil.copy2(library, output_library)
    output_manifest = destination / "runtime.json"
    output_manifest.write_text(json.dumps({
        "file_format_version": "1.0.0",
        "runtime": {"name": "Monado (Mocktail simulator)",
                    "library_path": "./libopenxr_monado.so"},
    }, indent=2) + "\n")
    print(f"Prepared simulator: {output_manifest}")


def simulator_environment(parent, manifest, display, wayland, software):
    environment = parent.copy()
    # These overrides belong to this child process; no active_runtime.json is
    # written and the user's WiVRn/system runtime selection remains intact.
    environment.update(
        XR_RUNTIME_JSON=str(manifest.resolve()),
        SIMULATED_ENABLE="1",
        XRT_COMPOSITOR_NULL="0",
        XRT_COMPOSITOR_FORCE_XCB="1" if display else "0",
        XRT_COMPOSITOR_FORCE_WAYLAND="1" if not display and wayland else "0",
        XRT_COMPOSITOR_FORCE_NVIDIA="0",
        XRT_COMPOSITOR_FORCE_RANDR="0",
        XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT="0",
        XRT_COMPOSITOR_FORCE_VK_DISPLAY="-1",
        XRT_COMPOSITOR_XCB_FULLSCREEN="0",
    )
    environment.setdefault("XRT_COMPOSITOR_DEFAULT_FRAMERATE", "30")
    if display:
        environment["DISPLAY"] = display
        environment.pop("WAYLAND_DISPLAY", None)
    if software:
        candidates = [directory / "lvp_icd.json" for directory in
                      (Path("/usr/share/vulkan/icd.d"), Path("/usr/local/share/vulkan/icd.d"))]
        manifest_path = next((path for path in candidates if path.is_file()), None)
        if manifest_path is None:
            raise ValueError("Mesa Lavapipe is not installed; omit --software to use your GPU")
        environment["VK_DRIVER_FILES"] = str(manifest_path)
        environment.pop("VK_ICD_FILENAMES", None)
        environment["XRT_COMPOSITOR_FORCE_GPU_INDEX"] = "0"
        environment["XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX"] = "0"
    return environment


def main():
    location = Path(__file__).resolve().parent
    default_build = location.parent / "build" if location.name == "scripts" else location
    parser = argparse.ArgumentParser(
        description="Show the Mocktail VR test scene on the desktop using a simulated Monado headset.")
    parser.add_argument("--build-dir", type=Path, default=default_build,
                        help="Mocktail build directory (defaults to the directory of this launcher)")
    parser.add_argument("--runtime", type=Path,
                        help="Manifest of an in-process Monado build with the simulated driver enabled")
    parser.add_argument("--prepare", type=Path, metavar="MONADO_MANIFEST",
                        help="Copy an already built Monado runtime next to this build, then exit")
    parser.add_argument("--frames", type=positive_integer,
                        help="Request exit after this many frames; otherwise run until Ctrl+C")
    parser.add_argument("--software", action="store_true", help="Use Mesa Lavapipe instead of a physical GPU")
    parser.add_argument("--headless", action="store_true",
                        help="Use a private Xvfb display for automated checks (defaults to 180 frames)")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    if args.prepare:
        prepare_runtime(args.prepare, build / "vr-simulator")
        return 0
    manifest = args.runtime or build / "vr-simulator/runtime.json"
    if not manifest.is_file():
        parser.error("No simulated Monado runtime is prepared. Use --runtime /path/to/openxr_monado-dev.json "
                     "or --prepare /path/to/openxr_monado-dev.json with a separately built Monado runtime.")
    probe = build / "mocktail-vr-probe"
    if not probe.is_file():
        parser.error(f"Build mocktail_vr_probe with MOCKTAIL_ENABLE_VR=ON first: {probe}")
    display = os.environ.get("DISPLAY")
    wayland = os.environ.get("WAYLAND_DISPLAY")
    if not args.headless and not (display or wayland):
        parser.error("Run from a graphical desktop terminal to see the scene, or use --headless for an automated check.")

    xvfb = None
    try:
        if args.headless:
            if not shutil.which("Xvfb"):
                parser.error("--headless requires Xvfb")
            read_fd, write_fd = os.pipe()
            try:
                xvfb = subprocess.Popen(
                    ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
                    pass_fds=(write_fd,), stdout=subprocess.DEVNULL, start_new_session=True)
            except OSError:
                os.close(read_fd)
                raise
            finally:
                os.close(write_fd)
            with os.fdopen(read_fd) as number:
                if not select.select([number], [], [], 10)[0]:
                    raise ValueError("Xvfb did not start within 10 seconds")
                display_number = number.readline().strip()
                if not display_number.isdigit():
                    raise ValueError("Xvfb failed to allocate a display")
                display = ":" + display_number
            wayland = None
        env = simulator_environment(os.environ, manifest, display, wayland, args.software)
        # Keep simulator settings and runtime files private and temporary. The
        # desktop's XDG_RUNTIME_DIR is retained for Wayland authentication.
        with tempfile.TemporaryDirectory(prefix="mocktail-vr-simulated-") as directory:
            root = Path(directory)
            env["XDG_CONFIG_HOME"] = str(root / "config")
            env["XDG_DATA_HOME"] = str(root / "data")
            if not env.get("XDG_RUNTIME_DIR"):
                env["XDG_RUNTIME_DIR"] = str(root)
            command = [str(probe), "--scene"]
            frames = args.frames or (180 if args.headless else None)
            if frames:
                command += ["--frames", str(frames)]
            print(f"Simulated Monado headset: {manifest.resolve()}", flush=True)
            print("The test scene will appear in a desktop window. Ctrl+C exits." if not args.headless
                  else "Checking the test scene on a private Xvfb display.", flush=True)
            child = None
            previous = {}

            def forward_signal(signum, _frame):
                if child is not None and child.poll() is None:
                    child.send_signal(signum)

            try:
                child = subprocess.Popen(command, env=env, start_new_session=True)
                for signum in (signal.SIGINT, signal.SIGTERM):
                    previous[signum] = signal.signal(signum, forward_signal)
                return child.wait()
            finally:
                if child is not None and child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                for signum, handler in previous.items():
                    signal.signal(signum, handler)
    finally:
        if xvfb is not None:
            xvfb.terminate()
            try:
                xvfb.wait(timeout=5)
            except subprocess.TimeoutExpired:
                xvfb.kill()
                xvfb.wait()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print(f"VR simulator: {error}", file=sys.stderr)
        sys.exit(1)
