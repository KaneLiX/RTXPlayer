# Third-party components

RTXPlayer's own source code is licensed under the PolyForm Noncommercial License 1.0.0
(see `LICENSE.md`). That license applies **only** to the files written for this project.
The components below keep their own licenses.

## Included in this repository

| Files | Component | License |
|---|---|---|
| `src/third_party/mpv/include/mpv/*.h` | mpv client API headers | ISC (see file headers) |
| `shaders/FSR.glsl` | AMD FidelityFX Super Resolution 1.0 (mpv port by agyild) | MIT (see file header) |
| `shaders/NVScaler.glsl` | NVIDIA Image Scaling (mpv port by agyild) | MIT (see file header) |
| `shaders/Anime4K_*.glsl` | Anime4K v4.0.1 by bloc97 | MIT (see file headers) |

These files are redistributed unmodified under their original permissive licenses,
which allow any use, including commercial use. The noncommercial restriction of this
project does not apply to them.

## Not included: obtain them yourself

The player loads these at runtime. They are **not** part of this repository, and their
licenses are independent of RTXPlayer's:

| Component | Version used | License | Notes |
|---|---|---|---|
| libmpv (`libmpv-2.dll`), shinchiro build | mpv 0.41 (2026-10-19 build) | GPLv2+ (this build) | Playback core |
| VapourSynth (portable) + embeddable Python | R72, Python 3.13 | LGPL-2.1 / PSF | Filter host |
| vs-mlrt (`vsmlrt.py`, `vstrt*.dll`, RIFE models) | v15.16 | GPL-3.0 (models: MIT) | RIFE inference |
| NVIDIA TensorRT / TensorRT for RTX / CUDA runtime | shipped with vs-mlrt | NVIDIA license agreements | |
| NVIDIA Optical Flow SDK (`NvOFFRUC.dll`, headers) | 5.0.7 | NVIDIA DesignWorks SDK License | Optional, FRUC interpolation |
| NVIDIA Video Effects SDK + `nvvfxvideosuperres` | 1.3.0.0 (Windows x64) | NVIDIA Software License Agreement | Optional, Maxine VSR |
| `d3dcompiler_43.dll` | DirectX runtime | Microsoft DirectX redistributable terms | Comes with the mpv package |

### Why there are no binary releases

- libmpv in common Windows builds and vs-mlrt are GPL. A binary package that bundles them
  with RTXPlayer would have to be offered under the GPL as a whole, which permits commercial
  use and so conflicts with this project's noncommercial license.
- The NVIDIA SDK licenses do not allow redistributing the SDKs or their headers as source,
  and their runtime DLLs are only obtainable from NVIDIA.

So this repository contains source code only. Each user downloads the runtime components
from their original publishers and accepts their licenses directly.

### NVIDIA SDK source code

The FRUC (`src/nvfruc`) and Maxine (`src/maxine`) plugins were written against the public
API headers of the NVIDIA SDKs. They do not contain NVIDIA sample code. To build them you need
your own copy of each SDK (the build references it by path; nothing from it is copied here).
`src/maxine` compiles the proxy loader sources from your VFX SDK copy at build time.

## Trademarks

NVIDIA, RTX, TensorRT, Maxine and GeForce are trademarks of NVIDIA Corporation. AMD and
FidelityFX are trademarks of Advanced Micro Devices, Inc. PotPlayer is a trademark of its
owner; RTXPlayer only follows a similar hotkey and menu layout and contains no PotPlayer code
or files. This project is not affiliated with, sponsored or endorsed by any of these companies.
