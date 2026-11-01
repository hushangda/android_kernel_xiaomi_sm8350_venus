#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual publisher main() with host sensor/lock path fixtures.

No device access. Only the two hardcoded filesystem roots are substituted.
Regular output files retain trailing bytes unlike sysfs; invalidation checks
therefore check the leading invalid marker, and sample checks reset the file.
"""
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "tools/thermal/venus-board-temp/venus-board-temp.c"
CONFIG = ROOT / "tools/thermal/venus-board-temp/venus-board-temp.conf"

with tempfile.TemporaryDirectory(prefix="venus-thermal-main-") as directory:
    base = Path(directory)
    thermal = base / "thermal"
    message = thermal / "thermal_message"
    message.mkdir(parents=True)
    output = message / "board_sensor_temp"
    lock = base / "publisher.lock"
    for i, name in enumerate(("pa_therm1", "wifi_therm", "quiet_therm",
                              "wireless_therm", "laser_therm0", "battery")):
        zone = thermal / f"thermal_zone{i}"
        zone.mkdir()
        (zone / "type").write_text(name + "\n")
        (zone / "temp").write_text("30000\n")
    text = SOURCE.read_text().replace(
        '#define THERMAL_ROOT "/sys/class/thermal"',
        f'#define THERMAL_ROOT "{thermal}"').replace(
        "/data/vendor/thermal/venus-board-temp.lock", str(lock))
    source, binary = base / "fixture.c", base / "fixture"
    source.write_text(text)
    subprocess.run([os.environ.get("HOSTCC", "clang"), "-std=c11", "-Wall",
                    "-Wextra", "-Werror", "-O1", "-g", "-no-pie",
                    "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                   check=True)

    def run(mode, config=CONFIG):
        return subprocess.run([str(binary), mode, str(config)],
                              capture_output=True, text=True, timeout=10)

    print("main-entry: actual producer main() lock/startup lifecycle tests", flush=True)
    seed = "27123\n"
    output.write_text(seed)
    with lock.open("w") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for mode in ("--publish", "--publish-once"):
            result = run(mode)
            assert result.returncode == 1, result
            assert "publisher lock" in result.stderr, result
            assert output.read_text() == seed
        result = run("--sample")
        assert result.returncode == 0 and result.stdout == "28870\n", result
        assert output.read_text() == seed

    output.write_text(seed)
    result = run("--publish", base / "missing-config")
    assert result.returncode == 1 and "model/sensors" in result.stderr, result
    assert output.read_text().startswith("invalid\n")

    output.write_text(seed)
    missing_sensor = thermal / "thermal_zone0/type"
    missing_sensor.unlink()
    result = run("--publish")
    assert result.returncode == 1 and "model/sensors" in result.stderr, result
    assert output.read_text().startswith("invalid\n")
    missing_sensor.write_text("pa_therm1\n")

    # Both failure exits must release the lock so a new writer can publish.
    output.write_text("")
    result = run("--publish-once")
    assert result.returncode == 0 and result.stdout == "28870\n", result
    assert output.read_text() == "28870\n"
    output.write_text("")
    daemon = subprocess.Popen([str(binary), "--publish", str(CONFIG)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            assert daemon.poll() is None, daemon.communicate()
            if output.read_text().startswith("28870\n"):
                break
            time.sleep(0.01)
        else:
            raise AssertionError("daemon did not publish the real sample")
        daemon.terminate()
        stdout, stderr = daemon.communicate(timeout=5)
        assert daemon.returncode == 0, (daemon.returncode, stdout, stderr)
        assert output.read_text().startswith("invalid\n")
    finally:
        if daemon.poll() is None:
            daemon.kill()
            daemon.wait()
    with lock.open("r+") as released:
        fcntl.flock(released, fcntl.LOCK_EX | fcntl.LOCK_NB)
    print("PASS: all writers share flock; sample stays read-only; bad config/"
          "missing sensors invalidate old data; lock release/restart and SIGTERM cleanup")
