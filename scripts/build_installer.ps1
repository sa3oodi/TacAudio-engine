# Builds TacAudio and its Windows installer.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -Generator "Visual Studio 17 2022"
#
# Needs: Visual Studio (C++ workload), CMake, Python 3 (to fetch the SADIE II heads once),
#        Inno Setup 6 (https://jrsoftware.org/isinfo.php).
# Output: dist\TacAudio-Setup-<version>.exe

param(
    [string]$Generator = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$match = Select-String -Path "CMakeLists.txt" -Pattern 'project\(TacAudio VERSION ([0-9.]+)'
if (-not $match) { throw "Could not read the version from CMakeLists.txt" }
$version = $match.Matches[0].Groups[1].Value
Write-Host "== TacAudio $version"

# 1) Head data shipped with the installer (SADIE II D1 + D2).
$sadie = Join-Path $root "data\hrtf\sadie2"
if (-not ((Test-Path "$sadie\D1.sofa") -and (Test-Path "$sadie\D2.sofa") -and (Test-Path "$sadie\LICENSE.txt"))) {
    Write-Host "== Downloading SADIE II heads (D1, D2)"
    python tools\fetch_sadie.py
    if ($LASTEXITCODE -ne 0) { throw "fetch_sadie.py failed" }
}

# 2) Build (Release, static runtime) and test.
Write-Host "== Configuring"
if ($Generator) { cmake -S . -B build -G $Generator -A x64 } else { cmake -S . -B build }
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
Write-Host "== Building"
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Write-Host "== Testing"
& "build\Release\tac_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "Unit tests failed - not packaging" }

# 3) Stage the installed file layout.
$stage = Join-Path $root "dist\stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\hrtf\sadie2" | Out-Null
Copy-Item "build\Release\TacAudio.exe", "build\Release\tacaudio-cli.exe", "build\Release\tacrender.exe" $stage
Copy-Item "build\Release\hrtf\MIT_KEMAR_normal_pinna.sofa" "$stage\hrtf\"
Copy-Item "$sadie\D1.sofa", "$sadie\D2.sofa", "$sadie\LICENSE.txt" "$stage\hrtf\sadie2\"
Copy-Item "README.md" "$stage\README.md"
Copy-Item "THIRD_PARTY_NOTICES.md" "$stage\THIRD_PARTY_NOTICES.txt"
Copy-Item "installer\after_install.txt" "$stage\GAME-SETUP.txt"

# 4) Compile the installer.
$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found - install it from https://jrsoftware.org/isinfo.php" }
Write-Host "== Packaging with $iscc"
& $iscc "/DAppVersion=$version" "installer\TacAudio.iss"
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }

$out = Join-Path $root "dist\TacAudio-Setup-$version.exe"
$size = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host "== Done: $out ($size MB)"
