"""Build the TensorRT engines ahead of time and measure RIFE throughput.

Usage: python warmup.py <model>.<backend> [...]     e.g. quality.trt fast.rtx
       python warmup.py all

Each pair is built and benchmarked in its own child process so every engine
instance and its VRAM is released before the next one (8 GB cards). For each
pair this records the measured output fps at 1080p and 720p processing size in
engines/bench.ini and then writes engines/<model>.<backend>.ready.
"""
import configparser
import os
import subprocess
import sys
import time
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

ALL = [f"{m}.{b}" for m in ("quality", "balanced", "fast") for b in ("trt", "rtx")]
BENCH_TARGET = 120        # 24 -> 120 = x5: 4 of 5 output frames are inferred,
BENCH_SRC = Fraction(24)  # so the result approximates the max sustainable output fps
BENCH_SECONDS = 6


def log(msg):
    print(msg, flush=True)


def single_instance():
    """Hold a named mutex for our lifetime; the player checks it before spawning another warm-up."""
    import ctypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateMutexW.restype = ctypes.c_void_p
    handle = k32.CreateMutexW(None, True, "Local\\RTXPlayerWarmup")
    if ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
        log("another warm-up is already running")
        sys.exit(0)
    return handle


def run_one(key):
    """Child process: build + benchmark one model/backend pair."""
    import vapoursynth as vs
    import rtxcore
    core = vs.core
    model, backend = key.split(".")

    def source(width, height):
        # RIFE's cost does not depend on content, so a flat clip is enough to build and time it.
        return core.std.BlankClip(width=width, height=height, format=vs.YUV420P8,
                                  length=100000, fpsnum=24, fpsden=1, color=[100, 128, 128])

    def measure(width, height):
        clip = rtxcore.interpolate(source(width, height), BENCH_SRC, BENCH_TARGET, model=model, backend=backend)
        frames = clip.frames(prefetch=16, backlog=48)
        for _ in range(40):
            next(frames)
        n, t0 = 0, time.perf_counter()
        while time.perf_counter() - t0 < BENCH_SECONDS:
            next(frames)
            n += 1
        return n / (time.perf_counter() - t0)

    log(f"STAGE build {key}")
    t0 = time.perf_counter()
    rtxcore.interpolate(source(1280, 720), BENCH_SRC, 60, model=model, backend=backend).get_frame(1)
    log(f"engine ready for {key} in {time.perf_counter() - t0:.1f}s")

    log(f"STAGE bench {key}")
    fps720 = measure(1280, 720)
    fps1080 = measure(1920, 1080)
    log(f"bench {key}: 1080p {fps1080:.1f} fps, 720p {fps720:.1f} fps")

    bench_path = os.path.join(rtxcore.ENGINE_DIR, "bench.ini")
    bench = configparser.ConfigParser()
    bench.read(bench_path)
    bench[key] = {"fps1080": f"{fps1080:.1f}", "fps720": f"{fps720:.1f}"}
    with open(bench_path, "w") as f:
        bench.write(f)
    with open(os.path.join(rtxcore.ENGINE_DIR, f"{key}.ready"), "w") as f:
        f.write("ok\n")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--one":
        run_one(sys.argv[2])
        return

    _mutex = single_instance()  # noqa: F841  (released when the process exits)
    keys = ALL if sys.argv[1:] in ([], ["all"]) else sys.argv[1:]
    failed = 0
    for key in keys:
        if key not in ALL:
            log(f"unknown engine {key}")
            failed += 1
            continue
        rc = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "--one", key]).returncode
        if rc != 0:
            log(f"FAILED {key} (exit {rc})")
            failed += 1
    log("STAGE done")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
