#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Host regression: USB audio routing with fake wpctl, no audio hardware.

Requires cc, pkg-config and ALSA/D-Bus/PipeWire development headers.
Run: python3 package/nuubos/nuubos-audio/tests/usb-audio.py
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"

with tempfile.TemporaryDirectory(prefix="nuubos-usb-audio-") as directory:
    root = Path(directory)
    wpctl = root / "wpctl"
    log = root / "wpctl.log"
    wpctl.write_text('''#!/usr/bin/env python3
import json, os, sys
nodes = json.loads(os.environ['PW_FIXTURE'])
args = sys.argv[1:]
if args[0] == 'status':
    print('Audio\\n Sinks:')
    for node_id, properties in nodes.items():
        if properties.get('media.class') == 'Audio/Sink':
            print('   %s. %s' % (node_id, properties['node.name']))
    print(' Sources:\\n   99. alsa_input.usb-Microphone')
elif args[0] == 'inspect':
    for key, value in nodes.get(args[1], {}).items():
        print('    %s = "%s"' % (key, value))
else:
    with open(os.environ['PW_LOG'], 'a') as output:
        output.write(' '.join(args) + '\\n')
    if args[0] == 'set-default' and os.environ.get('PW_FAIL_DEFAULT') == '1':
        sys.exit(1)
''')
    wpctl.chmod(0o755)
    env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"], PW_LOG=str(log))

    def node(name, description=""):
        return {"node.name": name, "node.description": description, "media.class": "Audio/Sink"}

    def fixture(nodes):
        env["PW_FIXTURE"] = json.dumps(nodes)
        log.write_text("")

    def route(*args, expected=0):
        result = subprocess.run(["sh", str(SRC / "nuubos-pw-route"), *args],
                                env=env, text=True, capture_output=True, timeout=5)
        assert result.returncode == expected, (args, result.returncode, result.stderr)
        return result.stdout.strip()

    fixture({"30": node("alsa_output.usb-Headset.analog-stereo", "Bluetooth HDMI Analog Headset")})
    assert route("find", "usb") == "30"
    route("find", "analog", expected=1)
    route("select", "analog", expected=3)
    assert route("select", "usb") == "30"
    assert log.read_text() == "set-default 30\n"  # Preserve the stored USB volume.

    fixture({"30": node("alsa_output.usb-Headset.analog-stereo"),
             "10": node("nuubos_analog"), "70": node("bluez_output.Headset"),
             "80": node("alsa_output.platform.hdmi"), "90": node("nuubos_cast")})
    for output, expected_id in [("speaker", "10"), ("headphones", "10"),
                                ("usb", "30"), ("bluetooth", "70"),
                                ("hdmi", "80"), ("cast", "90")]:
        assert route("find", output) == expected_id
    env["PW_FAIL_DEFAULT"] = "1"
    route("select", "usb", expected=4)
    env.pop("PW_FAIL_DEFAULT")

    fixture({"10": node("nuubos_analog"), "11": node("unknown_device")})
    route("find", "usb", expected=1)
    route("select", "usb", expected=3)
    assert route("select", "speaker") == "10"
    assert "set-mute 10 0\n" in log.read_text()
    assert "set-volume" not in log.read_text()
    fixture({"11": node("unknown_device")})
    route("find", "analog", expected=1)
    print("[PASS] route helper: USB identity, no analog fallback to USB, missing sinks, helper failure")

    # Compile the actual audiod translation unit as a host unit test. No daemon
    # is started: config and helpers are isolated in this temporary directory.
    include = root / "include" / "nuubos"
    include.mkdir(parents=True)
    notify = HERE.parents[1] / "nuubos-notify" / "src" / "notify.h"
    (include / "notify.h").write_bytes(notify.read_bytes())
    flags = shlex.split(os.environ.get("AUDIO_TEST_CFLAGS", ""))
    libs = shlex.split(os.environ.get("AUDIO_TEST_LIBS", ""))
    if not flags:
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "alsa", "dbus-1", "libpipewire-0.3"], text=True))
    if not libs:
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "alsa", "dbus-1", "libpipewire-0.3"], text=True))
    binary = root / "usb-audio-test"
    (root / "nuubos.conf").write_text("")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_GNU_SOURCE",
                    "-Wall", "-Wextra", "-Werror", "-I" + str(root / "include"),
                    '-DCONFIG_PATH="' + str(root / "nuubos.conf") + '"',
                    '-DCONFIG_TMP="' + str(root / "nuubos.conf.tmp") + '"',
                    '-DWPCTL_PATH="' + str(wpctl) + '"',
                    '-DPW_ROUTE_HELPER="' + str(SRC / "nuubos-pw-route") + '"',
                    *flags, str(HERE / "usb-audio.c"), *libs, "-o", str(binary)], check=True)
    fixture({"30": node("alsa_output.usb-Headset.analog-stereo")})
    subprocess.run([str(binary)], env=env, check=True, timeout=10)
    assert "set-volume 30 23%\n" in log.read_text()
