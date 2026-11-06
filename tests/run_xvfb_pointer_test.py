#!/usr/bin/env python3

import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile


xvfb, binary = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix="mocktail_pointer_xvfb_") as directory:
    root = Path(directory)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    with (root / "xvfb.log").open("w+") as log:
        server = subprocess.Popen(
            [xvfb, "-displayfd", "1", "-screen", "0", "3840x1080x24",
             "-nolisten", "tcp"], stdout=subprocess.PIPE, stderr=log,
        )
        try:
            readable, _, _ = select.select([server.stdout], [], [], 10)
            number = server.stdout.readline().decode().strip() if readable else ""
            if not number.isdecimal():
                log.seek(0)
                raise RuntimeError("Xvfb did not start: " + log.read())
            environment = {
                key: value for key, value in os.environ.items()
                if not key.startswith("SDL_") and key not in
                ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY")
            }
            environment.update({
                "DISPLAY": ":" + number,
                "SDL_VIDEODRIVER": "x11",
                "XDG_RUNTIME_DIR": str(runtime),
                "MOCKTAIL_POINTER_TEST_XVFB": "1",
            })
            completed = subprocess.run(
                [binary, *sys.argv[3:]], env=environment, cwd=root, timeout=45,
            )
            exit_code = completed.returncode
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
sys.exit(exit_code)
