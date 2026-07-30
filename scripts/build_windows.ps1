[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [ValidateSet("x64", "Win32", "ARM64")]
    [string]$Architecture = "x64",
    [string]$LibreDwgRoot = "",
    [Alias("QtPrefix")]
    [string]$QtDir = "",
    [switch]$EnableQtViewer,
    [Alias("NoTests")]
    [switch]$SkipTests,
    [switch]$SkipSubmodules,
    [switch]$SkipLibreDwg,
    [Alias("CleanFirst")]
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

function Resolve-ProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Test-ProjectChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    $rootPath = [System.IO.Path]::GetFullPath($ProjectRoot)
    $separator = [System.IO.Path]::DirectorySeparatorChar.ToString()
    if (-not $rootPath.EndsWith($separator)) {
        $rootPath += $separator
    }
    $candidatePath = [System.IO.Path]::GetFullPath($Path)
    return $candidatePath.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)
}

function Find-LibreDwgLibrary {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$BuildConfiguration
    )

    $candidates = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "libredwg.lib" -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "redwg.lib" -ErrorAction SilentlyContinue
    )
    $configuredCandidate = @($candidates |
        Where-Object { $_.Directory.Name -ieq $BuildConfiguration } |
        Select-Object -First 1)

    if ($configuredCandidate.Count -gt 0) {
        return $configuredCandidate[0].FullName
    }
    if ($candidates.Count -gt 0) {
        return $candidates[0].FullName
    }

    throw "Could not find libredwg.lib under '$Root'."
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $escapedName = [regex]::Escape($Name)
    $line = Get-Content -LiteralPath $CachePath |
        Where-Object { $_ -match "^$escapedName(?::[^=]+)?=(.+)$" } |
        Select-Object -First 1
    if ($null -eq $line) {
        return $null
    }
    return ($line -replace "^$escapedName(?::[^=]+)?=", "")
}

function Assert-LibreDwgConfigured {
    param([Parameter(Mandatory = $true)][string]$BuildPath)

    $cachePath = Join-Path $BuildPath "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        throw "CMake did not create '$cachePath'."
    }

    foreach ($name in @("LIBREDWG_INCLUDE_DIR", "LIBREDWG_LIBRARY")) {
        $value = Get-CMakeCacheValue -CachePath $cachePath -Name $name
        if ([string]::IsNullOrWhiteSpace($value) -or $value -like "*-NOTFOUND") {
            throw "LibreDWG C API was requested, but CMake did not resolve $name."
        }
    }
}

function Get-OutputDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$BuildPath,
        [Parameter(Mandatory = $true)][string]$BuildConfiguration
    )

    $configurationDirectory = Join-Path $BuildPath $BuildConfiguration
    if (Test-Path -LiteralPath $configurationDirectory) {
        return $configurationDirectory
    }
    return $BuildPath
}

function Copy-LibreDwgRuntime {
    param(
        [AllowEmptyString()][string]$Root,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string]$BuildConfiguration
    )

    if ([string]::IsNullOrWhiteSpace($Root) -or
        -not (Test-Path -LiteralPath $Root)) {
        return
    }

    $candidates = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "libredwg.dll" -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "redwg.dll" -ErrorAction SilentlyContinue
    )
    $runtime = @($candidates |
        Where-Object { $_.Directory.Name -ieq $BuildConfiguration } |
        Select-Object -First 1)
    if ($runtime.Count -eq 0) {
        $runtime = @($candidates | Select-Object -First 1)
    }
    if ($runtime.Count -gt 0) {
        Copy-Item -LiteralPath $runtime[0].FullName -Destination $OutputDirectory -Force
        Write-Host "Deployed LibreDWG runtime: $($runtime[0].Name)"
    }
}

function Deploy-QtRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$QtRoot,
        [Parameter(Mandatory = $true)][string]$ViewerPath,
        [Parameter(Mandatory = $true)][string]$OutputDirectory
    )

    $windeployqt = Join-Path (Join-Path $QtRoot "bin") "windeployqt.exe"
    if (-not (Test-Path -LiteralPath $windeployqt)) {
        throw "windeployqt.exe was not found under '$QtRoot\bin'."
    }

    Invoke-Checked -Program $windeployqt -ArgumentList @(
        "--no-translations", "--compiler-runtime", $ViewerPath
    )

    $platformPlugin = Join-Path (Join-Path $OutputDirectory "platforms") "qwindows.dll"
    if (-not (Test-Path -LiteralPath $platformPlugin)) {
        throw "windeployqt did not deploy '$platformPlugin'."
    }
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildPath = Resolve-ProjectPath -Path $BuildDir -ProjectRoot $ProjectRoot
$ResolvedLibreDwgRoot = ""
$ResolvedQtDir = ""

if ($LibreDwgRoot) {
    $ResolvedLibreDwgRoot = Resolve-ProjectPath -Path $LibreDwgRoot -ProjectRoot $ProjectRoot
    if (-not (Test-Path -LiteralPath $ResolvedLibreDwgRoot)) {
        throw "LibreDWG root '$ResolvedLibreDwgRoot' does not exist."
    }
}

if ($EnableQtViewer) {
    if (-not $QtDir) {
        throw "-EnableQtViewer requires -QtDir so windeployqt can deploy the Qt runtime."
    }
    $ResolvedQtDir = Resolve-ProjectPath -Path $QtDir -ProjectRoot $ProjectRoot
    if (-not (Test-Path -LiteralPath $ResolvedQtDir)) {
        throw "Qt root '$ResolvedQtDir' does not exist."
    }
}

Assert-Command "cmake"
$ShouldInitializeSubmodules = -not $SkipSubmodules -and -not $SkipLibreDwg -and
    [string]::IsNullOrWhiteSpace($ResolvedLibreDwgRoot) -and
    (Test-Path -LiteralPath (Join-Path $ProjectRoot ".gitmodules"))
if ($ShouldInitializeSubmodules) {
    Assert-Command "git"
}

$generatorArgs = @("-G", $Generator)
$isVisualStudioGenerator = $Generator -like "Visual Studio*"
$isSingleConfigGenerator = -not $isVisualStudioGenerator -and $Generator -ne "Ninja Multi-Config"
if ($isVisualStudioGenerator) {
    $generatorArgs += @("-A", $Architecture)
}

$configurationArgs = @()
if ($isSingleConfigGenerator) {
    $configurationArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
}

Push-Location $ProjectRoot
try {
    if ($Clean -and (Test-Path -LiteralPath $BuildPath)) {
        if (-not (Test-ProjectChildPath -Path $BuildPath -ProjectRoot $ProjectRoot)) {
            throw "Refusing to remove build directory outside the project root: $BuildPath"
        }
        Write-Host "Cleaning build directory: $BuildPath"
        Remove-Item -LiteralPath $BuildPath -Recurse -Force
    }

    if ($ShouldInitializeSubmodules) {
        Write-Host "[1/5] Initializing git submodules..."
        Invoke-Checked -Program "git" -ArgumentList @(
            "-C", $ProjectRoot, "submodule", "update", "--init", "--recursive", "--depth", "1"
        )
    }

    $mainCmakeArgs = @(
        "-S", $ProjectRoot,
        "-B", $BuildPath
    ) + $generatorArgs + $configurationArgs + @(
        "-DBUILD_TESTS=$(if ($SkipTests) { 'OFF' } else { 'ON' })",
        "-DBUILD_QT_VIEWER=$(if ($EnableQtViewer) { 'ON' } else { 'OFF' })",
        "-DCAD_USE_LIBREDWG_CLI=ON"
    )

    if ($EnableQtViewer) {
        $mainCmakeArgs += "-DCMAKE_PREFIX_PATH=$(ConvertTo-CMakePath $ResolvedQtDir)"
    }

    $useLibreDwgApi = -not $SkipLibreDwg
    $runtimeLibreDwgRoot = ""
    if ($SkipLibreDwg) {
        Write-Host "[2/5] Using the DWG CLI fallback (LibreDWG API disabled)..."
        $mainCmakeArgs += "-DCAD_USE_LIBREDWG_API=OFF"
    } elseif ($ResolvedLibreDwgRoot) {
        Write-Host "[2/5] Using the supplied LibreDWG installation..."
        $mainCmakeArgs += "-DCAD_USE_LIBREDWG_API=ON"
        $mainCmakeArgs += "-DLIBREDWG_ROOT_DIR=$(ConvertTo-CMakePath $ResolvedLibreDwgRoot)"
        $runtimeLibreDwgRoot = $ResolvedLibreDwgRoot
    } else {
        $LibreDwgSource = Join-Path $ProjectRoot "third_party/libredwg"
        $LibreDwgInclude = Join-Path $LibreDwgSource "include"
        if (-not (Test-Path -LiteralPath (Join-Path $LibreDwgInclude "dwg.h"))) {
            throw "LibreDWG source is missing. Run without -SkipSubmodules or pass -SkipLibreDwg."
        }

        $LibreDwgBuild = Join-Path $BuildPath "third_party/libredwg"
        Write-Host "[2/5] Configuring bundled LibreDWG..."
        $libreDwgCmakeArgs = @(
            "-S", $LibreDwgSource,
            "-B", $LibreDwgBuild
        ) + $generatorArgs + $configurationArgs + @(
            "-DBUILD_SHARED_LIBS=OFF",
            "-DBUILD_TESTING=OFF",
            "-DLIBREDWG_LIBONLY=ON",
            "-DLIBREDWG_DISABLE_WRITE=ON",
            "-DLIBREDWG_DISABLE_JSON=ON",
            "-DDISABLE_WERROR=ON",
            "-DENABLE_LTO=OFF"
        )
        Invoke-Checked -Program "cmake" -ArgumentList $libreDwgCmakeArgs

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
    if ($useLibreDwgApi) {
        Assert-LibreDwgConfigured -BuildPath $BuildPath
    }
    Invoke-Checked -Program "cmake" -ArgumentList @(
        "--build", $BuildPath, "--config", $Configuration, "--parallel"
    )

    $OutputDirectory = Get-OutputDirectory -BuildPath $BuildPath -BuildConfiguration $Configuration
    if ($EnableQtViewer) {
        $ViewerPath = Join-Path $OutputDirectory "cad-viewer.exe"
        if (-not (Test-Path -LiteralPath $ViewerPath)) {
            throw "Qt viewer was requested but was not built. Check -QtDir and the Qt CMake package."
        }
        Deploy-QtRuntime -QtRoot $ResolvedQtDir -ViewerPath $ViewerPath -OutputDirectory $OutputDirectory
    }
    Copy-LibreDwgRuntime -Root $runtimeLibreDwgRoot -OutputDirectory $OutputDirectory -BuildConfiguration $Configuration

    if (-not $SkipTests) {
        Write-Host "[5/5] Running tests..."
        Invoke-Checked -Program "cmake" -ArgumentList @(
            "-E", "chdir", $BuildPath, "ctest", "-C", $Configuration, "--output-on-failure"
        )
    }

    Write-Host "Build complete: $BuildPath"
    Get-ChildItem -LiteralPath $OutputDirectory -Filter "*.exe" | ForEach-Object {
        Write-Host "  $($_.Name)"
    }
} finally {
    Pop-Location
}
