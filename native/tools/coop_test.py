"""The co-op test (native/COOP.md): the game started headless in a chunk where Crash and Cortex play together (ui/sweep.cpp's
$TWIN_SWEEP_LEVEL, its scripted input for player 1), player 2 scripted by $TWIN_COOP_P2_INPUT, coop.cpp's log read back.

Two runs of the same chunk: player 2 holding the stick one way, and no player 2 (the game's own Cortex). Passes when player 2 took
the second character, the game didn't crash, and player 2's character went somewhere the game's own didn't (it moved by the pad,
not by following).

    python native/tools/coop_test.py [--chunk Levels\\Earth\\Cavern\\tunnel01] [--seconds 40]
"""
import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "build" / "native" / "cmake" / ("CrashTwinsanity.exe" if os.name == "nt" else "CrashTwinsanity")
FRAME = re.compile(r"coop: frame (\d+) characters (\d+)\+(\d+) pairing (\d+) p1 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) "
                   r"p2 \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) pad (\S+) motion (\d) replaced (\d) present (\d): (.*)")


def run(chunk, seconds, p2_input, home, snapshots=None):
    environment = dict(os.environ, TWINSANITY_AUDIO="off", TWIN_SWEEP_LEVEL=chunk, TWIN_SWEEP_SECONDS=str(seconds),
                       TWIN_SWEEP_TIMEOUT="120", TWIN_COOP="1", TWIN_COOP_LOG="1", HOME=str(home), APPDATA=str(home))
    if p2_input:
        environment["TWIN_COOP_P2_INPUT"] = p2_input
    else:
        environment.pop("TWIN_COOP_P2_INPUT", None)
    command = [str(APP), "--headless", "--quiet-stubs"] + (["--snapshots", str(snapshots)] if snapshots else [])
    process = subprocess.run(command, env=environment, capture_output=True, text=True,
                             errors="replace", timeout=seconds * 6 + 240, cwd=ROOT)
    log = process.stdout + process.stderr
    frames = [m.groups() for m in FRAME.finditer(log)]
    return process.returncode, log, frames


def p2_points(frames):
    # Only the frames with a second character (before the level's made its place reads 0, 0, 0)
    points = [(float(f[7]), float(f[8]), float(f[9])) for f in frames]
    return [p for p in points if p != (0.0, 0.0, 0.0)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--chunk", default="Levels\\Earth\\Cavern\\tunnel01")
    parser.add_argument("--seconds", type=int, default=40)
    parser.add_argument("--keep", help="folder for the runs' logs")
    arguments = parser.parse_args()
    keep = Path(arguments.keep) if arguments.keep else Path(tempfile.mkdtemp(prefix="coop_test_"))
    keep.mkdir(parents=True, exist_ok=True)
    # Player 2: the left stick held right (and a jump every 2 s), in a loop
    scripted = "110:255:128:0,10:255:128:40"
    results = {}
    for name, p2 in (("player2", scripted), ("ai", "")):
        home = keep / f"home_{name}"
        home.mkdir(exist_ok=True)
        snapshots = keep / f"snapshots_{name}"
        snapshots.mkdir(exist_ok=True)
        code, log, frames = run(arguments.chunk, arguments.seconds, p2, home, snapshots)
        (keep / f"{name}.log").write_text(log, encoding="utf-8")
        results[name] = (code, frames, log)
        print(f"{name}: exit {code}, {len(frames)} co-op frames logged")
        for f in frames[:: max(1, len(frames) // 8)]:
            print("   ", " ".join(f))

    failures = []
    code, frames, log = results["player2"]
    if code != 0:
        failures.append(f"player 2 run exited {code}")
    if "coop: player 2 plays" not in log:
        failures.append("player 2 never took the second character")
    played = [f for f in frames if f[10] == "p2"]
    if not played:
        failures.append("no frame logged with player 2's pad on the second character")
    if results["ai"][0] != 0:
        failures.append(f"AI run exited {results['ai'][0]}")
    if "coop: player 2 plays" in results["ai"][2]:
        failures.append("the run without player 2 gave the second character a pad")

    # Player 2's character's path against the game's own: the farthest it got from where it started
    def reach(points):
        return max((math.dist(points[0], p) for p in points), default=0.0) if points else 0.0

    p2_reach, ai_reach = reach(p2_points(frames)), reach(p2_points(results["ai"][1]))
    print(f"second character's reach from its start: player 2 {p2_reach:.1f}, AI {ai_reach:.1f}")
    if played and p2_reach < 2.0:
        failures.append("player 2's character hardly moved")
    paths_differ = any(math.dist(a, b) > 1.0 for a, b in zip(p2_points(frames), p2_points(results["ai"][1])))
    if played and not paths_differ:
        failures.append("player 2's character went exactly where the AI's did (the pad did nothing)")

    print(f"logs in {keep}")
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
