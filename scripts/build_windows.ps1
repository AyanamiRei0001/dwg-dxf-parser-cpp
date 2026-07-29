[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Architecture = "x64",
    [string]$LibreDwgRoot = "",
    [string]$QtDir = "",          # e.g. "D:/Qt/5.15.2/msvc2019_64"
    [switch]$EnableQtViewer,
    [switch]$NoTests,
    [switch]$CleanFirst
)

$ErrorActionPreference = "Stop"

if ($CleanFirst -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory: $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# ---- Build cmake arguments ----
$cmakeArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", $Generator,
    "-A", $Architecture
)

# Tests
if (-not $NoTests) {
    $cmakeArgs += "-DBUILD_TESTS=ON"
}

# Qt viewer
if ($EnableQtViewer) {
    $cmakeArgs += "-DBUILD_QT_VIEWER=ON"
    if ($QtDir) {
        $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir"
    }
} else {
    $cmakeArgs += "-DBUILD_QT_VIEWER=OFF"
}

# libredwg
if ($LibreDwgRoot) {
    $cmakeArgs += "-DLIBREDWG_ROOT_DIR=$LibreDwgRoot"
}

# ---- Configure ----
Write-Host ""
Write-Host "=== Configuring cad-parser ==="
Write-Host "Generator:     $Generator"
Write-Host "Architecture:  $Architecture"
Write-Host "Configuration: $Configuration"
Write-Host "Build dir:     $BuildDir"
Write-Host "Qt viewer:     $EnableQtViewer"
if ($QtDir) { Write-Host "Qt path:       $QtDir" }
Write-Host ""

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit $LASTEXITCODE }

# ---- Build ----
Write-Host ""
Write-Host "=== Building cad-parser ==="
& cmake --build $BuildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit $LASTEXITCODE }

# ---- Test ----
if (-not $NoTests) {
    Write-Host ""
    Write-Host "=== Running tests ==="
    & cmake -E chdir $BuildDir ctest -C $Configuration --output-on-failure
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) {
        Write-Warning "Some tests failed (exit code: $testExit)"
    }
}

# ---- Summarize ----
Write-Host ""
Write-Host "=== Build complete ==="
$outDir = Join-Path $BuildDir $Configuration
Get-ChildItem $outDir -Filter "*.exe" | ForEach-Object {
    Write-Host "  $($_.Name)  ($([math]::Round($_.Length/1KB, 1)) KB)"
}
Get-ChildItem $outDir -Filter "*.lib" | ForEach-Object {
    Write-Host "  $($_.Name)  ($([math]::Round($_.Length/1KB, 1)) KB)"
}

# --- Qt deployment hint ---
if ($EnableQtViewer -and $QtDir) {
    $windeployqt = Join-Path $QtDir "bin" "windeployqt.exe"
    if (Test-Path $windeployqt) {
        Write-Host ""
        Write-Host "Run windeployqt to bundle Qt DLLs:"
        Write-Host "  & `"$windeployqt`" `"$(Join-Path $outDir 'cad-viewer.exe')`" --no-translations"
    }
}

exit 0
