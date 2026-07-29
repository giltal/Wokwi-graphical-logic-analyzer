# SPDX-License-Identifier: MIT
# Build the logic-scope Wokwi custom chip with the WASI SDK.
#
#   .\build.ps1              # build
#   .\build.ps1 -Clean       # remove dist/ first
#
# Set WASI_SDK_PATH to override the default install location.

[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$distDir = Join-Path $root 'dist'
$srcDir = Join-Path $root 'src'

$sdk = $env:WASI_SDK_PATH
if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'wasi-sdk' }

$clang = Join-Path $sdk 'bin\clang.exe'
$sysroot = Join-Path $sdk 'share\wasi-sysroot'

if (-not (Test-Path $clang)) {
    throw "clang not found at '$clang'. Install the WASI SDK or set WASI_SDK_PATH."
}
if (-not (Test-Path $sysroot)) {
    throw "wasi-sysroot not found at '$sysroot'."
}

if ($Clean -and (Test-Path $distDir)) {
    Remove-Item -Recurse -Force $distDir
}
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$sources = Get-ChildItem -Path $srcDir -Recurse -Filter '*.c' | ForEach-Object { $_.FullName }
if (-not $sources) { throw "No .c sources found under '$srcDir'." }

$wasm = Join-Path $distDir 'logic-scope.chip.wasm'

$clangArgs = @(
    # wasi-sdk >= 25 deprecates 'wasm32-unknown-wasi' in favour of 'wasm32-wasip1'.
    # Same ABI (wasi_snapshot_preview1), which is what Wokwi's own builder targets.
    '--target=wasm32-wasip1'
    "--sysroot=$sysroot"
    '-nostartfiles'
    '-Wl,--import-memory'
    '-Wl,--export-table'
    '-Wl,--no-entry'
    # The simulator imports a 2-page (128 KB) memory and grows it on demand, so
    # the declared initial memory (stack + static data) must stay under that.
    # Large buffers are malloc'd instead of being static.
    '-Wl,-z,stack-size=32768'
    '-Wall'
    '-Wextra'
    # wokwi-api.h declares static helpers (get_sim_nanos, timer_start_ns) that we
    # don't always call; the header is vendored verbatim so we silence it here.
    '-Wno-unused-function'
    '-Werror'
    '-Os'
    '-o'
    $wasm
) + $sources

Write-Host "clang $($clangArgs -join ' ')" -ForegroundColor DarkGray
& $clang @clangArgs
if ($LASTEXITCODE -ne 0) { throw "clang failed with exit code $LASTEXITCODE" }

# Wokwi requires the chip JSON next to the wasm, with a matching base name.
Copy-Item (Join-Path $root 'logic-scope.chip.json') (Join-Path $distDir 'logic-scope.chip.json') -Force

# Single-file source for wokwi.com, which compiles one C file per custom chip.
$amalgamate = Join-Path $root 'tools\amalgamate.js'
if (Get-Command node -ErrorAction SilentlyContinue) {
    Push-Location $root
    try { & node $amalgamate } finally { Pop-Location }
} else {
    Write-Host 'node not found, skipping dist/logic-scope.chip.c' -ForegroundColor Yellow
}

$size = (Get-Item $wasm).Length
Write-Host "Built dist/logic-scope.chip.wasm ($size bytes)" -ForegroundColor Green
