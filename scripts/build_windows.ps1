[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [ValidateSet("x64", "Win32", "ARM64")]
    [string]$Architecture = "x64",
    [string]$LibreDwgRoot = "",
    [string]$QtPrefix = "",
    [switch]$EnableQtViewer,
    [switch]$SkipTests,
    [switch]$SkipSubmodules,
    [switch]$SkipLibreDwg,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Assert-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found in PATH."
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Program,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    & $Program @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

function ConvertTo-CMakePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return $Path.Replace("\", "/")
}

function Find-LibreDwgLibrary {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$BuildConfiguration
    )

    $candidates = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ieq "libredwg.lib" -or $_.Name -ieq "redwg.lib" })
    $configuredCandidate = @($candidates | Where-Object { $_.Directory.Name -ieq $BuildConfiguration } |
        Select-Object -First 1)

    if ($configuredCandidate.Count -gt 0) {
        return $configuredCandidate[0].FullName
    }
    if ($candidates.Count -gt 0) {
        return $candidates[0].FullName
    }

    throw "Could not find libredwg.lib under '$Root'."
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildPath = [System.IO.Path]::GetFullPath($BuildDir)
} else {
    $BuildPath = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
}

Assert-Command "cmake"
if (-not $SkipSubmodules -and (Test-Path -LiteralPath (Join-Path $ProjectRoot ".gitmodules"))) {
    Assert-Command "git"
}

Push-Location $ProjectRoot
try {
    if ($Clean -and (Test-Path -LiteralPath $BuildPath)) {
        Remove-Item -LiteralPath $BuildPath -Recurse -Force
    }

    if (-not $SkipSubmodules -and (Test-Path -LiteralPath (Join-Path $ProjectRoot ".gitmodules"))) {
        Write-Host "[1/5] Initializing git submodules..."
        Invoke-Checked -Program "git" -ArgumentList @(
            "-C", $ProjectRoot, "submodule", "update", "--init", "--recursive", "--depth", "1"
        )
    }

    $mainCmakeArgs = @(
        "-S", $ProjectRoot,
        "-B", $BuildPath,
        "-G", $Generator,
        "-A", $Architecture,
        "-DBUILD_TESTS=$(if ($SkipTests) { 'OFF' } else { 'ON' })",
        "-DBUILD_QT_VIEWER=$(if ($EnableQtViewer) { 'ON' } else { 'OFF' })",
        "-DCAD_USE_LIBREDWG_CLI=ON"
    )

    if ($QtPrefix) {
        $mainCmakeArgs += "-DCMAKE_PREFIX_PATH=$(ConvertTo-CMakePath $QtPrefix)"
    }

    if ($SkipLibreDwg) {
        Write-Host "[2/5] Using the DWG CLI fallback (LibreDWG API disabled)..."
        $mainCmakeArgs += "-DCAD_USE_LIBREDWG_API=OFF"
    } elseif ($LibreDwgRoot) {
        if (-not (Test-Path -LiteralPath $LibreDwgRoot)) {
            throw "LibreDWG root '$LibreDwgRoot' does not exist."
        }
        Write-Host "[2/5] Using the supplied LibreDWG installation..."
        $mainCmakeArgs += "-DCAD_USE_LIBREDWG_API=ON"
        $mainCmakeArgs += "-DLIBREDWG_ROOT_DIR=$(ConvertTo-CMakePath $LibreDwgRoot)"
    } else {
        $LibreDwgSource = Join-Path $ProjectRoot "third_party/libredwg"
        $LibreDwgInclude = Join-Path $LibreDwgSource "include"
        if (-not (Test-Path -LiteralPath (Join-Path $LibreDwgInclude "dwg.h"))) {
            throw "LibreDWG source is missing. Run without -SkipSubmodules or pass -SkipLibreDwg."
        }

        $LibreDwgBuild = Join-Path $BuildPath "third_party/libredwg"
        Write-Host "[2/5] Configuring bundled LibreDWG..."
        Invoke-Checked -Program "cmake" -ArgumentList @(
            "-S", $LibreDwgSource,
            "-B", $LibreDwgBuild,
            "-G", $Generator,
            "-A", $Architecture,
            "-DBUILD_SHARED_LIBS=OFF",
            "-DLIBREDWG_LIBONLY=ON",
            "-DLIBREDWG_DISABLE_WRITE=ON",
            "-DLIBREDWG_DISABLE_JSON=ON",
            "-DDISABLE_WERROR=ON",
            "-DENABLE_LTO=OFF"
        )

        Write-Host "[3/5] Building bundled LibreDWG..."
        Invoke-Checked -Program "cmake" -ArgumentList @(
            "--build", $LibreDwgBuild, "--config", $Configuration, "--parallel"
        )

        $LibreDwgLibrary = Find-LibreDwgLibrary -Root $LibreDwgBuild -BuildConfiguration $Configuration
        $mainCmakeArgs += "-DCAD_USE_LIBREDWG_API=ON"
        $mainCmakeArgs += "-DLIBREDWG_INCLUDE_DIR=$(ConvertTo-CMakePath $LibreDwgInclude)"
        $mainCmakeArgs += "-DLIBREDWG_LIBRARY=$(ConvertTo-CMakePath $LibreDwgLibrary)"
    }

    Write-Host "[4/5] Configuring and building cad-parser..."
    Invoke-Checked -Program "cmake" -ArgumentList $mainCmakeArgs
    Invoke-Checked -Program "cmake" -ArgumentList @(
        "--build", $BuildPath, "--config", $Configuration, "--parallel"
    )

    if ($EnableQtViewer) {
        $ViewerPath = Join-Path (Join-Path $BuildPath $Configuration) "cad-viewer.exe"
        if (-not (Test-Path -LiteralPath $ViewerPath)) {
            throw "Qt viewer was requested but was not built. Set -QtPrefix to the matching Qt5 installation."
        }
    }

    if (-not $SkipTests) {
        Write-Host "[5/5] Running tests..."
        Invoke-Checked -Program "cmake" -ArgumentList @(
            "-E", "chdir", $BuildPath, "ctest", "-C", $Configuration, "--output-on-failure"
        )
    }

    Write-Host "Build complete: $BuildPath"
    Write-Host "CLI: $(Join-Path (Join-Path $BuildPath $Configuration) 'cad-parser-cli.exe')"
} finally {
    Pop-Location
}
