# RTXPlayer

带 NVIDIA RTX 超分辨率与实时插帧的 Windows 视频播放器（C++ / Win32，基于 libmpv）。
操作习惯参考常见播放器：进度条、倍速、←/→ 跳 10 秒、右键菜单、全屏控制条等。

A Windows video player with NVIDIA RTX super resolution and real-time frame interpolation.
**Source-available for noncommercial use only** (PolyForm Noncommercial 1.0.0).

## 功能

- **先播放、后增强**：打开即原画播放，几秒后自动启用超分 / 插帧。
- **自动判断**：源 ≤1080p 时超分到 2K / 4K；源 ≤30fps 时插帧（默认原帧率 ×2，可选 60–240 fps）。
- **超分辨率算法**
  - NVIDIA Maxine VSR（Video Effects SDK，AI 超分，可调画质，需自行下载 SDK）
  - NVIDIA RTX Video Super Resolution（驱动内置，经 mpv `d3d11vpp`）
  - AMD FSR 1.0、NVIDIA NIS、Anime4K、EWA Lanczos
- **插帧算法**
  - RIFE 4.26 / 4.25 lite / 4.22 lite（vs-mlrt，TensorRT 或 TensorRT-RTX）
  - NVIDIA 光流 FRUC（Optical Flow SDK 硬件光流，固定 ×2，极快，需自行下载 SDK）
  - 帧混合（mpv display-resample）
- **能力不足自动降级**：实时监测输出帧率、音画同步与丢帧，依次降低 Maxine 画质、改用 720p 中间帧、降低目标帧率；每次调整后自动重新对齐音画。
- 精确时间戳（跳转后音画同步）、FRUC 质量回退监测与自动改用 RIFE。

详细说明见 [使用说明.txt](使用说明.txt)。

## 许可

本项目自己的源代码采用 **[PolyForm Noncommercial License 1.0.0](LICENSE.md)**：
可以免费使用、修改、再分发，**但不得用于商业用途**。
（因为禁止商用，它不属于 OSI 定义的“开源许可”，严格说是“源码公开”。）

第三方组件保持各自的许可，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

Required Notice: Copyright (c) 2026 KaneLiX

### 为什么只有源码、没有安装包

- 播放核心 libmpv（常见 Windows 构建为 GPL）和 vs-mlrt（GPL-3.0）如果和本程序打包发布，整个包必须按 GPL 提供，而 GPL 允许商用，与本项目“禁止商用”冲突。
- NVIDIA 各 SDK 的许可不允许转载 SDK 本身或其头文件源码。

所以仓库只放本项目的源码，运行所需组件请从各自官方渠道下载，并由你自己接受它们的许可。

## 运行环境

- Windows 10/11 x64，NVIDIA RTX 显卡（RIFE / Maxine 需要 Tensor Core；FRUC 需要 Turing 及以上的光流硬件）
- 较新的 NVIDIA 驱动（RTX VSR 需在 NVIDIA 控制面板启用“RTX 视频增强”）

## 组装程序目录

编译得到 `RTXPlayer.exe` 后，按下面的结构放置文件：

```
RTXPlayer\
  RTXPlayer.exe
  libmpv-2.dll, d3dcompiler_43.dll   ← shinchiro mpv-dev 构建 (x86_64)
  scripts\   shaders\                ← 本仓库
  plugins\nvfruc.dll, nvvfx.dll      ← 本仓库 src\nvfruc、src\maxine 编译产物（可选）
  vs\                                ← VapourSynth R72 便携版 + Python 3.13 嵌入版
  vsmlrt\                            ← vs-mlrt v15.16 Windows TensorRT 包（vsmlrt.py、vstrt*.dll、models\rife*）
  portable                           ← 可选：空文件，设置与引擎缓存放在程序目录
```

- **libmpv**：<https://github.com/shinchiro/mpv-winbuild-cmake/releases>（`mpv-dev-x86_64-*.7z`）
- **VapourSynth**：<https://github.com/vapoursynth/vapoursynth/releases>（R72 portable），Python 3.13 embeddable 解压到同一 `vs\` 目录；再执行 `vs\python.exe -m pip install onnx onnxconverter-common`
- **vs-mlrt**：<https://github.com/AmusementClub/vs-mlrt/releases>（v15.16，TensorRT 版，含 RIFE 模型）
- **可选 NVIDIA Optical Flow SDK**（FRUC）：<https://developer.nvidia.com/optical-flow-sdk>。播放器会自动查找 `Optical_Flow_SDK_*` 文件夹，或在设置里指定 `NvOFFRUC.dll`。
- **可选 NVIDIA Video Effects SDK**（Maxine VSR）：NGC 上的 VFX SDK Core（Windows x64）+ `nvvfxvideosuperres` 功能包。播放器会自动查找 `VideoFX` 文件夹，或在设置里指定。

首次使用 RIFE 时会在后台为本机显卡生成 TensorRT 引擎（约 1–2 分钟）。

## 编译

需要 Visual Studio 2022（MSVC，C++20）和 CMake ≥ 3.20。

```powershell
# 播放器
cmake -S src -B build\app -G "Visual Studio 17 2022" -A x64
cmake --build build\app --config Release

# FRUC 插件（需要你自己的 Optical Flow SDK 和 VapourSynth 头文件）
cmake -S src\nvfruc -B build\nvfruc -A x64 -DOFSDK_DIR=D:/Optical_Flow_SDK_5.0.7 -DVS_INCLUDE_DIR=D:/RTXPlayer/vs/sdk/include/vapoursynth
cmake --build build\nvfruc --config Release --target nvfruc

# Maxine 插件（需要你自己的 Video Effects SDK）
cmake -S src\maxine -B build\maxine -A x64 -DVFXSDK_DIR=D:/VFXSDK/VideoFX -DVS_INCLUDE_DIR=D:/RTXPlayer/vs/sdk/include/vapoursynth
cmake --build build\maxine --config Release --target nvvfx
```

插件输出到仓库根目录的 `plugins\`。`src\installer\RTXPlayer.iss` 是可选的 Inno Setup 安装脚本（仅供个人自用打包，请勿用它分发含 GPL 组件的安装包）。

`src\nvfruc` 与 `src\maxine` 里的 `*probe.cpp`、`psnr.cpp`、`imgcmp.cpp` 是开发测试工具，不属于播放器运行所需。

## 目录

| 路径 | 内容 |
|---|---|
| `src\*.cpp` | 播放器：窗口、控制条、菜单、设置对话框、增强规划与监测 |
| `src\nvfruc` | VapourSynth 插件：NVIDIA Optical Flow FRUC ×2 插帧 |
| `src\maxine` | VapourSynth 插件：NVIDIA Maxine Video Super Resolution |
| `scripts` | 插帧 / 超分管线（`rtxcore.py`）、mpv 滤镜入口、引擎预热测速 |
| `shaders` | FSR / NIS / Anime4K 着色器（MIT） |
