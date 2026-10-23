param(
    [string]$DevkitPro = 'C:\devkitPro',
    [switch]$Clean,
    [int]$Jobs = 4,
    [switch]$Verify,
    [switch]$NoLighting,
    [switch]$NoFps
)
# Builds the 3DSX from Windows through devkitPro's MSYS2 shell.
$ErrorActionPreference = 'Stop'
$bash = Join-Path $DevkitPro 'msys2\usr\bin\bash.exe'
if (!(Test-Path -LiteralPath $bash)) { throw "devkitPro MSYS2 not found: $bash" }
$port = $PSScriptRoot.Replace('\', '/')
if ($port.Contains("'")) { throw 'The path must not contain single quotes.' }
$python = (& python -c 'import sys; print(sys.executable)').Replace('\', '/')
if ($LASTEXITCODE -ne 0) { throw 'Python 3 is required by the build.' }
$lighting = if ($NoLighting) { 0 } else { 1 }
$fps = if ($NoFps) { 0 } else { 1 }
$command = "make -C '$port' VOXEL_LIGHTING=$lighting SHOW_FPS=$fps PYTHON='$python'"
# The login shell drops APPDATA, where a per-user Pillow (pip install --user) lives.
if ($env:APPDATA) {
    $appdata = $env:APPDATA.Replace('\', '/')
    $command = "export APPDATA='$appdata'; $command"
}
# A native host compiler (for the host tests and the decomp tools) and its
# runtime DLLs must be on PATH; MSYS2's MinGW64 is the usual one.
$hostDirs = @()
$hostCompiler = Get-Command gcc -ErrorAction SilentlyContinue
if ($hostCompiler) { $hostDirs += (Split-Path $hostCompiler.Source) }
if (Test-Path 'C:\msys64\mingw64\bin') { $hostDirs += 'C:\msys64\mingw64\bin' }
foreach ($dir in $hostDirs) {
    $unix = $dir.Replace('\', '/') -replace '^([A-Za-z]):', '/$1'
    $command = "export PATH='$unix':`$PATH; $command"
}
# Compiler warnings go to stderr; they must not abort the script.
$ErrorActionPreference = 'Continue'
if ($Clean) {
    & $bash -lc "$command clean"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$target = if ($Verify) { 'verify' } else { 'all' }
& $bash -lc "$command -j$Jobs $target"
exit $LASTEXITCODE
