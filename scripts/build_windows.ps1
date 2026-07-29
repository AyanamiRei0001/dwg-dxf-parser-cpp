[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Architecture = "x64",
    [string]$LibreDwgRoot = "",
    [switch]$EnableQtViewer
)

$ErrorActionPreference = "Stop"

$cmakeArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", $Generator,
    "-A", $Architecture,
    "-DBUILD_TESTS=ON"
)

if (-not $EnableQtViewer) {
    $cmakeArgs += "-DBUILD_QT_VIEWER=OFF"
}

if ($LibreDwgRoot) {
    $cmakeArgs += "-DLIBREDWG_ROOT_DIR=$LibreDwgRoot"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake -E chdir $BuildDir ctest -C $Configuration --output-on-failure
exit $LASTEXITCODE
