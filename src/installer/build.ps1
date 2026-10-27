# Rebuilds RTXPlayer.exe and the installer.
#   powershell -ExecutionPolicy Bypass -File build.ps1
# Output: ..\RTXPlayer_Installer\RTXPlayer-Setup-<version>.exe (next to the app folder)
$ErrorActionPreference = "Stop"
$root  = Resolve-Path "$PSScriptRoot\..\.."
$cmake = if ($env:CMAKE) { $env:CMAKE } else { "cmake" }  # e.g. the cmake.exe bundled with Visual Studio
$iscc  = if ($env:ISCC) { $env:ISCC } else { "ISCC.exe" }  # Inno Setup compiler

& $cmake -S "$root\src" -B "$root\src\build" -G "Visual Studio 17 2022" -A x64
& $cmake --build "$root\src\build" --config Release
Copy-Item "$root\src\build\Release\RTXPlayer.exe" $root -Force
& $iscc "$PSScriptRoot\RTXPlayer.iss"
