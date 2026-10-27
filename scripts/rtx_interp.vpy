# mpv VapourSynth filter entry point for RTXPlayer.
#
# mpv provides the globals video_in, container_fps, display_fps and user_data.
# The player passes its parameters through user_data, e.g. "t60-mquality-p720-btrt-s29970":
#   t = target fps, m = model (quality|balanced|fast), p = processing height (0 = native),
#   b = inference backend (trt|rtx), s = source fps x 1000 as measured by the player
#   (mpv's container_fps can be 0 when the filter is rebuilt mid-playback),
#   r = 1 for full-range source levels, x = source matrix (1 = 709, 6 = 601, 9 = 2020, 0 = guess),
#   u = Maxine VSR output height after interpolation (0 = none), q = its quality level (1-4).
#   t0 means no interpolation (Maxine VSR only).
#   mpv keeps the input's colour tags on our output, so the output must stay in the
#   source's range and matrix.
import ctypes
import os
import re
import sys
from fractions import Fraction

sys.path.insert(0, os.path.join(os.environ["RTXP_ROOT"], "scripts"))
import rtxcore  # noqa: E402


def host_reporter(name):
    """RTXPlayer.exe exports RtxpReportFps(double) / RtxpReportFruc(double); None when run
    elsewhere (e.g. mpv.exe)."""
    k32 = ctypes.WinDLL("kernel32")
    k32.GetModuleHandleW.restype = ctypes.c_void_p
    k32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    k32.GetProcAddress.restype = ctypes.c_void_p
    k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    addr = k32.GetProcAddress(k32.GetModuleHandleW(None), name)
    return ctypes.CFUNCTYPE(None, ctypes.c_double)(addr) if addr else None


params = dict(re.findall(r"([a-z])([0-9a-z]+)", user_data or ""))  # noqa: F821
target = int(params.get("t", "60"))
model = params.get("m", "quality")
proc_height = int(params.get("p", "0"))
backend = params.get("b", "trt")
player_fps = int(params.get("s", "0")) / 1000
full_range = params.get("r", "0") == "1"
matrix = {"1": "709", "6": "170m", "9": "2020ncl"}.get(params.get("x", "0"))
sr_height = int(params.get("u", "0"))
sr_quality = int(params.get("q", "3"))

clip = video_in  # noqa: F821
if player_fps > 0:
    src_fps = Fraction(player_fps).limit_denominator(1001)
elif clip.fps_num > 0 and clip.fps_den > 0:
    src_fps = Fraction(clip.fps_num, clip.fps_den)
elif container_fps and container_fps > 0:  # noqa: F821
    src_fps = Fraction(container_fps).limit_denominator(1001)  # noqa: F821
else:
    src_fps = Fraction(24)

# One line per (re)build, next to mpv.log, to make the chosen ratio visible.
try:
    with open(os.path.join(os.environ.get("RTXP_DATA", os.environ["RTXP_ROOT"]), "logs", "interp.log"), "a") as log:
        ratio = 2 if model == "fruc" else rtxcore.frame_multiplier(src_fps, target) if target else 1
        log.write(f"src {float(src_fps):.3f} (player {player_fps:.3f}, mpv {float(container_fps or 0):.3f}) -> "  # noqa: F821
                  f"{target or 'no interpolation'} fps, x{ratio}, {model}/{backend}, proc {proc_height or 'native'}, "
                  f"{clip.width}x{clip.height}, maxine vsr {f'{sr_height}p q{sr_quality}' if sr_height else 'off'}\n")
except OSError:
    pass

if target:
    clip = rtxcore.interpolate(
        clip, src_fps, target,
        model=model, proc_height=proc_height, backend=backend, report=host_reporter(b"RtxpReportFps"),
        full_range=full_range, matrix=matrix, repeat_report=host_reporter(b"RtxpReportFruc"),
    )
if sr_height > clip.height:
    clip = rtxcore.super_res(clip, sr_height, quality=sr_quality, full_range=full_range, matrix=matrix)
clip.set_output()
