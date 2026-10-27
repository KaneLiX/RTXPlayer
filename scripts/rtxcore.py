"""RTXPlayer interpolation pipeline: RIFE on NVIDIA TensorRT for RTX (vs-mlrt).

Shared by the mpv filter script (rtx_interp.vpy) and the warm-up tool
(warmup.py) so both resolve to the same cached TensorRT engine files.
"""
import os
import sys
import threading
import time
from fractions import Fraction

import vapoursynth as vs

core = vs.core

ROOT = os.environ.get("RTXP_ROOT") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MLRT_DIR = os.path.join(ROOT, "vsmlrt")
# Engines are per-GPU user data: %LOCALAPPDATA%\RTXPlayer\engines for installed
# copies (RTXP_DATA is set by RTXPlayer.exe), the program folder when portable.
ENGINE_DIR = os.path.join(os.environ.get("RTXP_DATA") or ROOT, "engines")

def ensure_plugins():
    """Load the TensorRT plugins into the *current* core.

    mpv re-evaluates the filter script in the same process with a fresh core
    each time the filter chain is rebuilt, while this module stays imported,
    so module-level loading alone would only cover the first core.
    """
    for name, dll in (("trt", "vstrt.dll"), ("trt_rtx", "vstrt_rtx.dll")):
        if not hasattr(core, name):
            core.std.LoadPlugin(os.path.join(MLRT_DIR, dll))


ensure_plugins()
if MLRT_DIR not in sys.path:
    sys.path.insert(0, MLRT_DIR)

import vsmlrt  # noqa: E402  (the trt plugins must be loaded before import)

MODELS = {
    "quality": vsmlrt.RIFEModel.v4_26,
    "balanced": vsmlrt.RIFEModel.v4_25_lite,
    "fast": vsmlrt.RIFEModel.v4_22_lite,
}
# RIFE (implementation 1) needs width/height divisible by this per model.
PAD_MULTIPLE = {"quality": 64, "balanced": 128, "fast": 32}

# One dynamic-shape engine per model/backend covers every processing size we
# use, so a new video resolution never triggers an engine rebuild. Kernels are
# tuned for 720p, the size RIFE runs at most often (720p sources and the
# 720p intermediate for 1080p sources).
MIN_SHAPE = (64, 64)
OPT_SHAPE = (1280, 768)
MAX_SHAPE = (2560, 1472)

# trt: classic TensorRT. Engine build ~90 s (done once by warmup.py), but a
#      filter (re)start costs only ~0.5 s, so enabling interpolation mid-playback
#      is a barely visible hitch. Default.
# rtx: NVIDIA TensorRT for RTX. ~20-30% higher throughput and half the VRAM,
#      but it JIT-compiles kernels on every load: ~6-7 s pause when enabled.
BACKENDS = ("trt", "rtx")

SCENE_THRESHOLD = 0.12  # luma PlaneStatsDiff above this = hard cut, don't morph across it

# "fruc": NVIDIA Optical Flow SDK FRUC (hardware optical flow + CUDA warping)
# through plugins\nvfruc.dll. Always x2, at native resolution, no engine to build.
# NvOFFRUC.dll itself comes from the user's SDK folder (RTXP_FRUC_DLL).
FRUC_PLUGIN = os.path.join(ROOT, "plugins", "nvfruc.dll")


# Maxine VSR: NVIDIA Video Effects SDK AI super resolution through plugins\nvvfx.dll.
# The SDK comes from the user's copy (RTXP_VFX_SDK).
VFX_PLUGIN = os.path.join(ROOT, "plugins", "nvvfx.dll")
VFX_MATRIX = {"709": 1, "170m": 6, "2020ncl": 9}


def super_res(clip, out_height, quality=3, full_range=False, matrix=None):
    """Upscale to out_height (width keeps the aspect ratio) with Maxine VSR.
    Keeps the source's range and matrix, like interpolate()."""
    if not hasattr(core, "nvvfx"):
        core.std.LoadPlugin(VFX_PLUGIN)
    if not matrix:
        matrix = "709" if clip.height >= 600 or clip.width >= 1024 else "170m"
    range_s = "full" if full_range else "limited"
    if clip.format.id != vs.YUV420P8:  # the plugin takes 8-bit 4:2:0
        clip = core.resize.Point(clip, format=vs.YUV420P8, dither_type="ordered",
                                 range_in_s=range_s, range_s=range_s)
    out_width = int(round(clip.width * out_height / clip.height / 2)) * 2
    return core.nvvfx.SuperRes(clip, out_width, out_height, quality=quality,
                               matrix=VFX_MATRIX.get(matrix, 1), full=int(full_range))


def _fruc(clip, range_s):
    if not hasattr(core, "nvfruc"):
        core.std.LoadPlugin(FRUC_PLUGIN)
    if clip.format.id != vs.YUV420P8:  # FRUC takes 8-bit NV12 only
        clip = core.resize.Point(clip, format=vs.YUV420P8, dither_type="ordered",
                                 range_in_s=range_s, range_s=range_s)
    # Frame k of `mid` lies halfway between source frames k and k+1.
    mid = core.nvfruc.Interpolate(clip)
    return core.std.Interleave([clip, mid])


def make_backend(kind="trt"):
    shapes = dict(static_shape=False, min_shapes=MIN_SHAPE, opt_shapes=OPT_SHAPE, max_shapes=MAX_SHAPE)
    if kind == "rtx":
        return vsmlrt.Backend.TRT_RTX(fp16=True, num_streams=2, engine_folder=ENGINE_DIR, **shapes)
    return vsmlrt.Backend.TRT(fp16=True, num_streams=2, output_format=1, engine_folder=ENGINE_DIR, **shapes)


def processing_size(width, height, proc_height, multiple=64):
    """Size RIFE runs at (before padding). proc_height=0 means native, capped so
    the padded size still fits the engine's MAX_SHAPE."""
    max_w = MAX_SHAPE[0]
    max_h = MAX_SHAPE[1] - multiple + 2
    limit_h = max_h if not proc_height else min(proc_height, max_h)
    scale = min(1.0, limit_h / height, max_w / width)
    pw = max(64, int(round(width * scale / 2)) * 2)
    ph = max(64, int(round(height * scale / 2)) * 2)
    return pw, ph


def _pad_mirror(clip, multiple):
    """Pad right/bottom up to a multiple by mirroring edge content (keeps optical flow sane at borders)."""
    padw = -clip.width % multiple
    padh = -clip.height % multiple
    if padw:
        edge = core.std.Crop(clip, left=clip.width - padw).std.FlipHorizontal()
        clip = core.std.StackHorizontal([clip, edge])
    if padh:
        edge = core.std.Crop(clip, top=clip.height - padh).std.FlipVertical()
        clip = core.std.StackVertical([clip, edge])
    return clip, padw, padh


def _mark_scene_changes(clip):
    """Returns the clip with _SceneChangeNext set, plus the tiny luma clip used for it."""
    small = core.resize.Bilinear(clip, 160, 90, format=vs.GRAY8)
    stats = core.std.PlaneStats(small, small[1:] + small[-1])

    def mark(n, f):
        out = f[0].copy()
        out.props["_SceneChangeNext"] = int(f[1].props["PlaneStatsDiff"] > SCENE_THRESHOLD)
        return out

    return core.std.ModifyFrame(clip, [clip, stats], mark), small


def _phases(multi):
    """Per output phase k of an a:b cycle: (left source index, position within it,
    source frames advanced until the next output frame (0 or 1), next position)."""
    a, b = multi.numerator, multi.denominator
    table = []
    for k in range(a):
        p, q = Fraction(k * b, a), Fraction((k + 1) * b, a)
        li, lj = p.numerator // p.denominator, q.numerator // q.denominator
        table.append((li, p - li, lj - li, q - lj))
    return table


def _finish(clip, small, multi, fallback, report=None, repeat_report=None):
    """Timestamp the output exactly and meter real-time throughput.

    mpv timestamps VapourSynth output by adding up each output frame's
    _DurationNum/_DurationDen, starting from the first input frame. Source
    durations are not always uniform: after an exact seek the decoder starts at
    a keyframe and drops frames up to the target, so the keyframe's duration
    covers that whole gap. Each output frame therefore gets the real time from
    its own position on the source timeline to the next output frame's
    position. (A uniform duration, e.g. AssumeFPS, would shift every later
    timestamp away from the picture: audio out of sync after seeking.)

    Also calls report(frames_per_second) about once a second with the
    real-time output throughput (used by the player to downgrade if needed),
    and repeat_report(share) with the share of FRUC in-betweens that the
    library replaced by a copy of the previous frame (its quality fallback).
    """
    a, b = multi.numerator, multi.denominator
    table = _phases(multi)
    cycles = small.num_frames // b

    # dur_l / dur_r frame n = source frame left of / right after output frame n (durations only).
    def at(c, off):
        c = c if b == 1 else core.std.SelectEvery(c, cycle=b, offsets=off, modify_duration=False)
        return c[:cycles]

    def weave(clips):
        return clips[0] if len(clips) == 1 else core.std.Interleave(clips, modify_duration=False)

    nxt = small[1:] + small[-1]
    dur_l = weave([at(small, li) for li, _, _, _ in table])
    dur_r = weave([at(nxt, li) for li, _, _, _ in table])

    def duration(f):
        num, den = f.props.get("_DurationNum", 0), f.props.get("_DurationDen", 0)
        return Fraction(num, den) if num > 0 and den > 0 else fallback * multi

    lock = threading.Lock()
    state = {"n": 0, "t0": time.perf_counter(), "mid": 0, "rep": 0}

    def fix(n, f):
        _, t, step, u = table[n % a]
        dl = duration(f[1])
        d = (u - t) * dl if step == 0 else (1 - t) * dl + u * duration(f[2])
        if d <= 0:
            d = fallback
        out = f[0].copy()
        out.props["_DurationNum"] = d.numerator
        out.props["_DurationDen"] = d.denominator
        if report is not None:
            with lock:
                state["n"] += 1
                repeated = f[0].props.get("FrucRepeated")
                if repeated is not None:
                    state["mid"] += 1
                    state["rep"] += repeated
                now = time.perf_counter()
                dt = now - state["t0"]
                if dt >= 1.0:
                    report(state["n"] / dt)
                    if repeat_report is not None and state["mid"]:
                        repeat_report(state["rep"] / state["mid"])
                    state.update(n=0, t0=now, mid=0, rep=0)
        return out

    return core.std.ModifyFrame(clip, [clip, dur_l, dur_r], fix)


def frame_multiplier(src_fps, target_fps):
    """Output/input frame ratio as a small fraction.

    NTSC rates are treated as their integer neighbours (23.976 -> 24), the way
    SVP does: 24 -> 60 becomes x5/2 and the output runs at 59.94 fps. This keeps
    the phase pattern short instead of 1001 phases per cycle.
    """
    nominal = Fraction(round(float(src_fps) * 1000), 1000)
    if abs(float(nominal) - round(float(nominal))) < 0.05:
        nominal = Fraction(round(float(nominal)))
    return (Fraction(target_fps) / nominal).limit_denominator(8)


def _schedule(rgb, multi, model, backend):
    """Interleave source frames and RIFE in-betweens for a periodic a:b pattern.

    Every cycle of b source frames yields a output frames. All in-between frames
    of a cycle go through one RIFEMerge instance (one engine, one set of streams).
    """
    a, b = multi.numerator, multi.denominator
    nxt = rgb[1:] + rgb[-1]

    phases = []          # per output phase: ("src", left_idx) or ("rife", rife_slot)
    lefts, tps = [], []
    for k in range(a):
        pos = Fraction(k * b, a)
        left = pos.numerator // pos.denominator
        tp = pos - left
        if tp == 0:
            phases.append(("src", left))
        else:
            phases.append(("rife", len(tps)))
            lefts.append(left)
            tps.append(float(tp))

    cycles = rgb.num_frames // b

    def every(c, cycle, off):
        return c if cycle == 1 else core.std.SelectEvery(c, cycle=cycle, offsets=off)

    def sel(c, off):
        return every(c, b, off)[:cycles]

    out_phases = []
    if tps:
        m = len(tps)
        left_all = core.std.Interleave([sel(rgb, li) for li in lefts])
        right_all = core.std.Interleave([sel(nxt, li) for li in lefts])
        mask_all = core.std.Interleave([
            core.std.BlankClip(rgb, format=vs.GRAYH, color=tp, length=cycles, keep=True) for tp in tps
        ])
        # Implementation 1: TensorRT-RTX rejects the fp16 build of the v2 graphs.
        merged = vsmlrt.RIFEMerge(
            left_all, right_all, mask_all,
            model=MODELS[model], backend=make_backend(backend), _implementation=1,
        )

        # Across a hard cut, show the previous frame instead of a ghosted morph.
        def pick(n, f):
            return f[1] if f[1].props.get("_SceneChangeNext", 0) else f[0]

        merged = core.std.ModifyFrame(merged, [merged, left_all], pick)
        rife_phase = [every(merged, m, j) for j in range(m)]

    for kind, idx in phases:
        out_phases.append(sel(rgb, idx) if kind == "src" else rife_phase[idx])
    return out_phases[0] if len(out_phases) == 1 else core.std.Interleave(out_phases)


def interpolate(clip, src_fps, target_fps, model="quality", proc_height=0, backend="trt", report=None,
                full_range=False, matrix=None, repeat_report=None):
    """matrix: zimg name ("709", "170m", "2020ncl") or None to guess from the size.
    The output keeps the source's range and matrix: mpv tags it with the input's."""
    ensure_plugins()
    range_s = "full" if full_range else "limited"
    src_fps = Fraction(src_fps)
    multi = Fraction(2) if model == "fruc" else frame_multiplier(src_fps, target_fps)
    if multi <= 1:
        return clip

    # Keep every derived frame count in int32 range: mpv hands us an effectively endless clip.
    max_len = (2**31 - 1) * multi.denominator // (multi.numerator + 1)
    if clip.num_frames > max_len:
        clip = clip[:max_len]

    if model == "fruc":
        clip, small = _mark_scene_changes(clip)
        return _finish(_fruc(clip, range_s), small, multi, fallback=1 / (src_fps * multi), report=report,
                       repeat_report=repeat_report)

    if not matrix:
        matrix = "709" if clip.height >= 600 or clip.width >= 1024 else "170m"
    multiple = PAD_MULTIPLE[model]
    pw, ph = processing_size(clip.width, clip.height, proc_height, multiple)

    clip, small = _mark_scene_changes(clip)
    rgb = core.resize.Bicubic(clip, pw, ph, format=vs.RGBH, matrix_in_s=matrix, range_in_s=range_s)
    rgb, padw, padh = _pad_mirror(rgb, multiple)

    out = _schedule(rgb, multi, model, backend)
    if padw or padh:
        out = core.std.Crop(out, right=padw, bottom=padh)
    out = core.resize.Bicubic(out, format=vs.YUV420P10, matrix_s=matrix, range_s=range_s)
    return _finish(out, small, multi, fallback=1 / (src_fps * multi), report=report)
