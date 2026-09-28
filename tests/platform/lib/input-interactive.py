#!/usr/bin/env python3

import argparse
import hashlib
import os
import re
import select
import struct
import subprocess
import sys
import time

EVENT = struct.Struct("<qqHHi")
READY_MARKER = b"__NUBOS_INPUT_READY__"


def parse_args():
    p = argparse.ArgumentParser()

    p.add_argument("mode", choices=("key", "abs"))
    p.add_argument("--target", required=True)
    p.add_argument("--event", required=True)
    p.add_argument("--code", required=True, type=int)
    p.add_argument("--label", required=True)
    p.add_argument("--timeout", type=float, default=10.0)

    p.add_argument("--direction", choices=("low", "high"))
    p.add_argument("--threshold", type=int)

    return p.parse_args()


def have_terminal():
    if os.environ.get("NO_COLOR"):
        return False

    try:
        fd = os.open("/dev/tty", os.O_WRONLY)
        result = os.isatty(fd)
        os.close(fd)
        return result
    except OSError:
        return False


USE_COLOR = have_terminal()


def colored(text, code):
    if not USE_COLOR:
        return text

    return f"\033[{code}m{text}\033[0m"


def action(message):
    print()
    print(colored(f"[ACTION] {message}", "1;33"), flush=True)


def observed(message):
    print(colored(f"[OBSERVED] {message}", "1;32"), flush=True)


def setup(message):
    print(colored(f"[SETUP] {message}", "1;36"), flush=True)


def error(tag, message):
    print(colored(f"[{tag}] {message}", "1;31"), flush=True)


def stop(proc):
    if proc is None or proc.poll() is not None:
        return

    proc.terminate()

    try:
        proc.wait(timeout=0.5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def control_path(target):
    digest = hashlib.sha256(target.encode()).hexdigest()[:12]
    return f"/tmp/nuubos-input-{os.getuid()}-{digest}.sock"


def master_alive(path, target):
    result = subprocess.run(
        [
            "ssh",
            "-S", path,
            "-O", "check",
            target,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    return result.returncode == 0


def ensure_master(target):
    path = control_path(target)

    if master_alive(path, target):
        return path

    # Remove only a stale control socket from a previous test run.
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

    cmd = [
        "ssh",
        "-MNf",
        "-S", path,
        "-o", "ControlMaster=yes",
        "-o", "ControlPersist=120",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=7",
        "-o", "ServerAliveInterval=5",
        "-o", "ServerAliveCountMax=2",
        target,
    ]

    result = subprocess.run(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )

    if result.returncode != 0:
        message = result.stderr.strip() or "unable to create SSH master"
        error("SETUP", message)
        return None

    if not master_alive(path, target):
        error("SETUP", "SSH master created but control socket is unavailable")
        return None

    setup("SSH input session ready")
    return path


def wait_remote_ready(proc, timeout=5.0):
    if proc.stderr is None:
        return False

    fd = proc.stderr.fileno()
    os.set_blocking(fd, False)

    data = bytearray()
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if READY_MARKER in data:
            return True

        if proc.poll() is not None:
            break

        remaining = deadline - time.monotonic()

        ready, _, _ = select.select(
            [fd],
            [],
            [],
            min(0.1, remaining),
        )

        if not ready:
            continue

        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue

        if chunk:
            data.extend(chunk)

    if READY_MARKER in data:
        return True

    message = bytes(data).decode(errors="replace").strip()

    if message:
        setup(f"reader error: {message}")

    return False


def open_reader(args, control):
    remote_cmd = (
        f"exec 3</dev/input/{args.event} || exit 1; "
        f"echo {READY_MARKER.decode()} >&2; "
        f"exec cat <&3"
    )

    # A command-channel failure is not an input failure.
    # Retry reader setup before asking the operator to touch anything.
    for attempt in range(1, 3):
        cmd = [
            "ssh",
            "-T",
            "-S", control,
            "-o", "BatchMode=yes",
            args.target,
            remote_cmd,
        ]

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )

        if wait_remote_ready(proc):
            return proc

        stop(proc)

        if attempt < 2:
            setup("reader channel not ready; reopening channel")

    return None


def main():
    args = parse_args()

    if not re.fullmatch(r"event[0-9]+", args.event):
        error("SETUP", "invalid event node")
        return 2

    if args.mode == "abs":
        if args.direction is None or args.threshold is None:
            error(
                "SETUP",
                "ABS mode requires --direction and --threshold",
            )
            return 2

    control = ensure_master(args.target)

    if control is None:
        return 2

    proc = open_reader(args, control)

    if proc is None:
        error("SETUP", "remote input reader could not be armed")
        return 2

    try:
        if proc.stdout is None:
            return 2

        fd = proc.stdout.fileno()
        os.set_blocking(fd, False)

        # IMPORTANT:
        # We print ACTION only after the remote event node is open.
        if args.mode == "key":
            action(f"Premi e rilascia: {args.label}")
        else:
            direction = (
                "MINIMO"
                if args.direction == "low"
                else "MASSIMO"
            )

            action(
                f"{args.label} -> porta lo stick fino al "
                f"{direction} e poi torna al centro"
            )

        deadline = time.monotonic() + args.timeout
        buffer = bytearray()
        pressed = False

        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()

            ready, _, _ = select.select(
                [fd],
                [],
                [],
                min(0.1, remaining),
            )

            if not ready:
                continue

            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue

            if not chunk:
                break

            buffer.extend(chunk)

            while len(buffer) >= EVENT.size:
                raw = bytes(buffer[:EVENT.size])
                del buffer[:EVENT.size]

                sec, usec, typ, code, value = EVENT.unpack(raw)

                if args.mode == "key":
                    if typ != 1 or code != args.code:
                        continue

                    if value == 1:
                        pressed = True

                    elif value == 0 and pressed:
                        observed(
                            f"code={code} PRESS+RELEASE"
                        )
                        return 0

                else:
                    if typ != 3 or code != args.code:
                        continue

                    if args.direction == "low":
                        matched = value <= args.threshold
                    else:
                        matched = value >= args.threshold

                    if matched:
                        observed(
                            f"code={code} value={value}"
                        )
                        return 0

        error(
            "TIMEOUT",
            f"evento atteso non osservato: {args.label}",
        )
        return 1

    finally:
        stop(proc)


if __name__ == "__main__":
    raise SystemExit(main())
