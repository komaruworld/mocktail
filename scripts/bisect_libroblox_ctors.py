#!/usr/bin/env python3
"""Locate libroblox.so .init_array constructors that break JNI_OnLoad.

The helper uses Mocktail's constructor wrapper knobs:

  MOCKTAIL_MAX_LIBROBLOX_CTORS=N
  MOCKTAIL_SKIP_LIBROBLOX_CTOR_OFFSETS=0x...

It greedily finds the first prefix length that makes Stage 5 fail, adds that
constructor offset to the skip list, and repeats.
"""

import argparse
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import time


BAD_RE = re.compile(r"malloc\(\): corrupted|corrupted top size|\[crash\] stage=5")
GOOD_RE = re.compile(r"JNI_OnLoad returned")
CTOR_OFFSET_RE = re.compile(r"\[ctor\] \[(\d+)/\d+\] offset (0x[0-9a-fA-F]+)")
REF_ADDR_RE = re.compile(r"#\s*([0-9a-fA-F]+)\s+<")
CALL_RE = re.compile(r"\bcall\s+([0-9a-fA-F]+)(?:\s+<([^>]+)>)?")
LOAD_RE = re.compile(
    r"\s*LOAD\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+"
    r"0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")

KNOWN_CALLS = {
    0x2bfc4c2: "__cxa_guard_acquire-like",
    0x2bfc5fa: "__cxa_guard_release-like",
    0x2bfc685: "__cxa_guard_abort-like",
    0x1f5bde8: "std::string literal/init helper",
    0x693e1bc: "generated descriptor/registry helper",
    0x693df1d: "generated descriptor/registry helper",
    0x2c19350: "exception/unwind termination path",
    0x6b234a0: "__stack_chk_fail",
}

HINT_KEYWORDS = [
    ("systemdialog", "system dialog/platform UI service",
     "mock PlatformSystemDialogHandler or bypass generated dialog protocol init"),
    ("purchase", "billing/purchase service",
     "mock billing/purchase callbacks or skip pending-purchase protocol init"),
    ("billing", "billing/purchase service",
     "mock billing/purchase callbacks or skip pending-purchase protocol init"),
    ("nativevideointerface", "video/camera JNI service",
     "register NativeVideoInterface classes/methods or bypass camera/video init"),
    ("camera", "video/camera service",
     "provide camera/video stubs before running this ctor"),
    ("crashpad", "Crashpad/logging service",
     "mock Crashpad/logging init or keep this ctor quarantined"),
    ("fmod", "FMOD/audio backend",
     "provide FMOD/audio stubs before running the ctor"),
    ("vulkan", "graphics/Vulkan backend",
     "route through ANGLE/SDL/Vulkan or keep graphics ctor disabled"),
    ("egl", "graphics/EGL backend",
     "publish real/mock EGL handles before running this ctor"),
    ("surface", "Android Surface/window service",
     "provide ANativeWindow/Surface lifecycle state"),
    ("android", "Android framework/JNI service",
     "add the missing class/object/method in jnivm registration"),
    ("jni", "JNI protocol bridge",
     "register the expected Java class/method or bypass the protocol init"),
    ("protocol", "Roblox universal-app protocol registry",
     "mock the protocol backing service or skip generated protocol registration"),
    ("http", "network/HTTP service",
     "provide HTTP/cookie/cert backing stubs"),
    ("cookie", "cookie manager",
     "wire CookieManager/JNICookieProtocol backing state"),
    ("asset", "asset/filesystem service",
     "ensure AssetManager/path mappings are registered before this ctor"),
    ("locale", "locale/device config service",
     "mock Android locale/resources/configuration"),
    ("protobuf", "protobuf descriptor registry",
     "usually safe to quarantine; otherwise implement descriptor bootstrap"),
    (".proto", "protobuf descriptor registry",
     "usually safe to quarantine; otherwise implement descriptor bootstrap"),
]


class Trial:
  def __init__(self, max_ctors, returncode, outcome, log_path, offsets):
    self.max_ctors = max_ctors
    self.returncode = returncode
    self.outcome = outcome
    self.log_path = log_path
    self.offsets = offsets

  @property
  def bad(self):
    return self.outcome == "bad"

  @property
  def good(self):
    return self.outcome == "good"


class CtorAnnotation:
  def __init__(self, offset, category, expects, fix, evidence):
    self.offset = offset
    self.category = category
    self.expects = expects
    self.fix = fix
    self.evidence = evidence

  def short_lines(self, prefix="  hint: "):
    return [
        f"{prefix}{self.category}",
        f"{prefix}expects: {self.expects}",
        f"{prefix}fix: {self.fix}",
        f"{prefix}evidence: {self.evidence}",
    ]


def reliable_symbol(symbol):
  if not symbol:
    return False
  match = re.search(r"([+-])0x([0-9a-fA-F]+)$", symbol)
  if not match:
    return True
  return int(match.group(2), 16) <= 0x4000


def extract_call_evidence(disasm):
  reliable = []
  nearest = []
  for target_text, symbol in CALL_RE.findall(disasm):
    target = int(target_text, 16)
    if target in KNOWN_CALLS:
      reliable.append(KNOWN_CALLS[target])
    elif reliable_symbol(symbol):
      reliable.append(symbol)
    elif symbol:
      nearest.append(symbol)
  return list(dict.fromkeys(reliable)), list(dict.fromkeys(nearest))


def terminate_process_group(proc):
  if proc.poll() is not None:
    return
  try:
    os.killpg(proc.pid, signal.SIGTERM)
  except ProcessLookupError:
    return
  try:
    proc.wait(timeout=2)
  except subprocess.TimeoutExpired:
    try:
      os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
      pass
    proc.wait(timeout=2)


def classify_line(line):
  if BAD_RE.search(line):
    return "bad"
  if GOOD_RE.search(line):
    return "good"
  return None


def parse_load_segments(lib_path):
  result = subprocess.run(
      ["readelf", "-lW", str(lib_path)],
      check=True,
      text=True,
      stdout=subprocess.PIPE,
      stderr=subprocess.DEVNULL,
  )
  segments = []
  for line in result.stdout.splitlines():
    match = LOAD_RE.match(line)
    if not match:
      continue
    file_offset = int(match.group(1), 16)
    vaddr = int(match.group(2), 16)
    file_size = int(match.group(3), 16)
    mem_size = int(match.group(4), 16)
    segments.append((vaddr, file_offset, file_size, mem_size))
  return segments


def vma_to_file_offset(segments, vma):
  for vaddr, file_offset, file_size, mem_size in segments:
    size = min(file_size, mem_size)
    if vaddr <= vma < vaddr + size:
      return file_offset + (vma - vaddr)
  return None


def read_c_string_at_vma(lib_path, segments, vma, limit=160):
  file_offset = vma_to_file_offset(segments, vma)
  if file_offset is None:
    return None
  try:
    with lib_path.open("rb") as handle:
      handle.seek(file_offset)
      data = handle.read(limit)
  except OSError:
    return None
  data = data.split(b"\x00", 1)[0]
  if len(data) < 4:
    return None
  printable = sum(32 <= byte < 127 or byte in (9, 10, 13) for byte in data)
  if printable / max(1, len(data)) < 0.85:
    return None
  try:
    return data.decode("utf-8", errors="replace")
  except UnicodeDecodeError:
    return None


def disassemble(lib_path, offset, window):
  result = subprocess.run(
      [
          "objdump",
          "-d",
          f"--start-address=0x{offset:x}",
          f"--stop-address=0x{offset + window:x}",
          str(lib_path),
      ],
      text=True,
      stdout=subprocess.PIPE,
      stderr=subprocess.DEVNULL,
  )
  return result.stdout


def classify_ctor(offset, disasm, strings):
  known_calls, _ = extract_call_evidence(disasm)
  lower_blob = ("\n".join(strings) + "\n" + "\n".join(known_calls)).lower()

  if "generated descriptor/registry helper" in known_calls:
    return (
        "generated descriptor/protobuf static initializer",
        "protobuf/static descriptor registry, not a platform service",
        "quarantine it for now; only run after descriptor bootstrap is understood",
    )

  for needle, service, fix in HINT_KEYWORDS:
    if needle in lower_blob:
      return (
          f"{service} initializer",
          service,
          fix,
      )

  if "__cxa_guard_acquire-like" in known_calls:
    return (
        "guarded C++ static initializer",
        "a lazy singleton/global object; exact service is in referenced strings",
        "inspect strings/evidence, then either mock that singleton's backing API "
        "or keep this ctor in an index-range quarantine",
    )

  if 0x1f20000 <= offset <= 0x1f90000:
    return (
        "early generated/static registration initializer",
        "Roblox generated registries/protocol descriptors",
        "prefer index-range quarantine until the matching registry service exists",
    )

  if 0x2c00000 <= offset <= 0x2c90000:
    return (
        "platform/runtime service initializer",
        "Android/runtime backing service",
        "find the referenced Java/native service and add a jnivm/mock implementation",
    )

  return (
      "unclassified constructor",
      "unknown backing state",
      "run with --annotate-offset and inspect the referenced strings/calls",
  )


def annotate_ctor(args, offset_text):
  offset = int(offset_text, 0)
  lib_path = args.libroblox.resolve()
  segments = parse_load_segments(lib_path)
  text = disassemble(lib_path, offset, args.annotate_window)
  ref_addrs = []
  for ref_text in REF_ADDR_RE.findall(text):
    ref = int(ref_text, 16)
    if ref not in ref_addrs:
      ref_addrs.append(ref)

  ref_strings = []
  for ref in ref_addrs[: args.annotate_max_refs]:
    value = read_c_string_at_vma(lib_path, segments, ref)
    if value:
      ref_strings.append(value)

  known_calls, nearest_calls = extract_call_evidence(text)

  category, expects, fix = classify_ctor(offset, text, ref_strings)
  evidence_parts = []
  if known_calls:
    evidence_parts.append("calls=" + ", ".join(dict.fromkeys(known_calls[:5])))
  elif nearest_calls:
    evidence_parts.append("nearest_symbols=" +
                          ", ".join(dict.fromkeys(nearest_calls[:3])))
  if ref_strings:
    clipped = [item if len(item) <= 60 else item[:57] + "..."
               for item in ref_strings[:4]]
    evidence_parts.append("strings=" + " | ".join(clipped))
  if not evidence_parts:
    evidence_parts.append("no useful strings/calls in small disassembly window")
  return CtorAnnotation(offset, category, expects, fix, "; ".join(evidence_parts))


def print_annotation(args, offset, prefix="  hint: "):
  try:
    annotation = annotate_ctor(args, offset)
  except (OSError, subprocess.SubprocessError, ValueError) as exc:
    print(f"{prefix}annotation failed for {offset}: {exc}", flush=True)
    return
  for line in annotation.short_lines(prefix):
    print(line, flush=True)


def run_trial(args, max_ctors, skipped_offsets, trial_id):
  log_path = args.log_dir / f"trial_{trial_id:03d}_max_{max_ctors}.log"
  env = os.environ.copy()
  env.update({
      "ROBLOX_LIB_PATH": str(args.libroblox),
      "MOCKTAIL_RUN_LIBROBLOX_CTORS": "1",
      "MOCKTAIL_LIBROBLOX_CTOR_POLICY": args.ctor_policy,
      "MOCKTAIL_GRAPHICS_BACKEND": args.graphics_backend,
      "MOCKTAIL_LIBROBLOX_CTOR_TIMEOUT_MS": str(args.ctor_timeout_ms),
      "MOCKTAIL_MAX_LIBROBLOX_CTORS": str(max_ctors),
  })
  if skipped_offsets:
    env["MOCKTAIL_SKIP_LIBROBLOX_CTOR_OFFSETS"] = ",".join(skipped_offsets)
  for item in args.env:
    key, _, value = item.partition("=")
    if not key or not _:
      raise ValueError(f"--env must be KEY=VALUE, got {item!r}")
    env[key] = value

  offsets = {}
  outcome = "unknown"
  deadline = time.monotonic() + args.timeout
  cmd = [str(args.run_script)]

  with log_path.open("w", encoding="utf-8", errors="replace") as log:
    proc = subprocess.Popen(
        cmd,
        cwd=args.project_root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        start_new_session=True,
    )

    assert proc.stdout is not None
    while True:
      if proc.poll() is not None:
        for line in proc.stdout:
          log.write(line)
          match = CTOR_OFFSET_RE.search(line)
          if match:
            offsets[int(match.group(1))] = match.group(2).lower()
          line_outcome = classify_line(line)
          if line_outcome is not None:
            outcome = line_outcome
        break

      ready, _, _ = select.select([proc.stdout], [], [], 0.1)
      if ready:
        line = proc.stdout.readline()
        if line:
          log.write(line)
          log.flush()
          match = CTOR_OFFSET_RE.search(line)
          if match:
            offsets[int(match.group(1))] = match.group(2).lower()
          line_outcome = classify_line(line)
          if line_outcome is not None:
            outcome = line_outcome
            terminate_process_group(proc)
            break

      if time.monotonic() >= deadline:
        outcome = "timeout"
        terminate_process_group(proc)
        break

    returncode = proc.poll()
    if returncode is None:
      terminate_process_group(proc)
      returncode = proc.poll()

  return Trial(max_ctors, returncode, outcome, log_path, offsets)


def find_first_bad(args, skipped_offsets, trial_counter):
  full = run_trial(args, args.ctor_count, skipped_offsets, trial_counter[0])
  trial_counter[0] += 1
  print(
      f"full max={args.ctor_count} outcome={full.outcome} "
      f"log={full.log_path}",
      flush=True,
  )
  if full.good:
    return None, full
  if not full.bad:
    return None, full

  low = 0
  high = args.ctor_count
  last_bad = full
  while high - low > 1:
    mid = (low + high) // 2
    trial = run_trial(args, mid, skipped_offsets, trial_counter[0])
    trial_counter[0] += 1
    print(
        f"  max={mid:<4} outcome={trial.outcome:<7} "
        f"log={trial.log_path}",
        flush=True,
    )
    if trial.bad:
      high = mid
      last_bad = trial
    elif trial.good:
      low = mid
    else:
      high = mid
      last_bad = trial

  bad_index = high - 1
  offset = last_bad.offsets.get(bad_index)
  if offset is None:
    confirm = run_trial(args, high, skipped_offsets, trial_counter[0])
    trial_counter[0] += 1
    offset = confirm.offsets.get(bad_index)
    last_bad = confirm
  return (bad_index, offset, last_bad), full


def scan_prefix(args, skipped_offsets, trial_counter, found):
  if args.scan_prefix_limit <= 0:
    return 0

  start = max(0, args.scan_start)
  end = min(args.scan_prefix_limit, args.ctor_count)
  current = start
  found_in_scan = 0
  print(f"prefix scan: start={start} end={end}", flush=True)

  while current < end:
    max_ctors = current + 1
    trial = run_trial(args, max_ctors, skipped_offsets, trial_counter[0])
    trial_counter[0] += 1
    print(
        f"  scan index={current:<4} max={max_ctors:<4} "
        f"outcome={trial.outcome:<7} log={trial.log_path}",
        flush=True,
    )

    if trial.bad:
      offset = trial.offsets.get(current)
      if offset is None:
        print(f"  scan stopped: could not parse offset for index {current}",
              flush=True)
        break
      found.append((current, offset))
      skipped_offsets.append(offset)
      found_in_scan += 1
      print(
          f"  quarantine candidate: index={current} offset={offset}",
          flush=True,
      )
      if args.annotate:
        print_annotation(args, offset, prefix="    hint: ")
      print(f"  current skip list: {','.join(skipped_offsets)}", flush=True)
      continue

    if trial.good:
      current += 1
      continue

    print(f"  scan stopped: index={current} outcome={trial.outcome}",
          flush=True)
    break

  return found_in_scan


def parse_args():
  project_root = Path(__file__).resolve().parents[1]
  run_id = time.strftime("%Y%m%d_%H%M%S")
  parser = argparse.ArgumentParser()
  parser.add_argument("--project-root", type=Path, default=project_root)
  parser.add_argument("--run-script", type=Path,
                      default=project_root / "build" / "mocktail")
  parser.add_argument("--libroblox", type=Path,
                      default=project_root / "rbx_bin" / "libroblox.so")
  parser.add_argument("--log-dir", type=Path,
                      default=project_root / "logs" / "libroblox_ctor_bisect" /
                      run_id)
  parser.add_argument("--ctor-count", type=int, default=3388)
  parser.add_argument("--timeout", type=float, default=20.0)
  parser.add_argument("--ctor-timeout-ms", type=int, default=500)
  parser.add_argument("--ctor-policy", default="all",
                      help="Mocktail ctor policy for trials; default all disables safe quarantine")
  parser.add_argument("--graphics-backend", default="system")
  parser.add_argument("--max-find", type=int, default=8)
  parser.add_argument("--scan-prefix-limit", type=int, default=0,
                      help="Linearly scan prefixes up to this constructor index")
  parser.add_argument("--scan-start", type=int, default=0)
  parser.add_argument("--skip-offset", action="append", default=[])
  parser.add_argument("--skip-offsets", default="",
                      help="Comma-separated initial constructor offset skip list")
  parser.add_argument("--annotate", action=argparse.BooleanOptionalAction,
                      default=True)
  parser.add_argument("--annotate-offset", action="append", default=[])
  parser.add_argument("--annotate-window", type=int, default=0x260)
  parser.add_argument("--annotate-max-refs", type=int, default=16)
  parser.add_argument("--env", action="append", default=[])
  return parser.parse_args()


def main():
  args = parse_args()
  args.project_root = args.project_root.resolve()
  args.run_script = args.run_script.resolve()
  args.libroblox = args.libroblox.resolve()
  args.log_dir.mkdir(parents=True, exist_ok=True)

  if args.annotate_offset:
    for offset in args.annotate_offset:
      print(f"offset {offset}:")
      print_annotation(args, offset, prefix="  ")
    return 0

  skipped_offsets = [offset.lower() for offset in args.skip_offset]
  if args.skip_offsets:
    skipped_offsets.extend(
        offset.strip().lower() for offset in args.skip_offsets.split(",")
        if offset.strip())
  trial_counter = [1]
  found = []

  if skipped_offsets:
    print("initial skip list: " + ",".join(skipped_offsets), flush=True)

  baseline = run_trial(args, 0, skipped_offsets, trial_counter[0])
  trial_counter[0] += 1
  print(f"baseline max=0 outcome={baseline.outcome} log={baseline.log_path}")
  if not baseline.good:
    print("baseline is not good; constructor bisection would be ambiguous",
          file=sys.stderr)
    return 2

  scan_prefix(args, skipped_offsets, trial_counter, found)

  for _ in range(args.max_find):
    result, full = find_first_bad(args, skipped_offsets, trial_counter)
    if result is None:
      if full.good:
        print("full constructor set is Stage-5 clean with current skip list")
        break
      print(f"stopped: full outcome is {full.outcome}")
      break

    bad_index, offset, trial = result
    if offset is None:
      print(f"found bad index {bad_index}, but could not parse its offset")
      break

    found.append((bad_index, offset))
    skipped_offsets.append(offset)
    print(
        f"quarantine candidate: index={bad_index} offset={offset} "
        f"log={trial.log_path}",
        flush=True,
    )
    if args.annotate:
      print_annotation(args, offset, prefix="  hint: ")
    print(f"current skip list: {','.join(skipped_offsets)}", flush=True)

  print("summary:")
  for index, offset in found:
    print(f"  index={index} offset={offset}")
  if skipped_offsets:
    print("MOCKTAIL_SKIP_LIBROBLOX_CTOR_OFFSETS=" + ",".join(skipped_offsets))
  return 0


if __name__ == "__main__":
  sys.exit(main())
