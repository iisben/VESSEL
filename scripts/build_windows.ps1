<#
.SYNOPSIS
    One-click local native Windows x64 (MSVC) Release build, QA & packager for VESSEL II.
.DESCRIPTION
    Configures and builds VESSEL II (VST3 + Standalone + headless Vessel2QA & Vessel2PresetQA),
    runs all headless DSP/control-plane/preset QA suites, runs pluginval (if installed on PATH),
    and packages dist\windows\VESSEL-II-Windows-x64.zip.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build-windows",
    [string]$JucePath = "",
    [switch]$SkipQA
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot

try {
    $cmakeArgs = @("-S", ".", "-B", $BuildDir, "-A", "x64", "-DVESSEL_BUILD_QA=ON")
    if ($JucePath -ne "") {
        $cmakeArgs += "-DVESSEL_JUCE_PATH=$JucePath"
    }

    Write-Host "==> Configuring VESSEL II (Windows x64 MSVC)..." -ForegroundColor Cyan
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

    Write-Host "==> Building Release VST3, Standalone & QA suites..." -ForegroundColor Cyan
    & cmake --build $BuildDir --config Release --parallel --target Vessel_VST3 Vessel_Standalone Vessel2QA Vessel2PresetQA
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }

    if (-not $SkipQA) {
        Write-Host "==> Running 8-Gate DSP & Control-Plane QA..." -ForegroundColor Cyan
        $qa = Get-ChildItem -Recurse -Filter Vessel2QA.exe $BuildDir | Select-Object -First 1
        & $qa.FullName --strict --stage4-qa
        if ($LASTEXITCODE -ne 0) { throw "Vessel2QA failed ($LASTEXITCODE)" }

        Write-Host "==> Running 20-Preset Audio QA..." -ForegroundColor Cyan
        $pqa = Get-ChildItem -Recurse -Filter Vessel2PresetQA.exe $BuildDir | Select-Object -First 1
        & $pqa.FullName --no-wav
        if ($LASTEXITCODE -ne 0) { throw "Vessel2PresetQA failed ($LASTEXITCODE)" }

        $pval = Get-Command pluginval -ErrorAction SilentlyContinue
        if ($null -ne $pval) {
            Write-Host "==> Running pluginval (strictness level 5)..." -ForegroundColor Cyan
            & $pval.Source --strictness-level 5 --validate-in-process --timeout-ms 120000 --validate "$BuildDir/Vessel_artefacts/Release/VST3/VESSEL.vst3"
            if ($LASTEXITCODE -ne 0) { throw "pluginval failed ($LASTEXITCODE)" }
        } else {
            Write-Host "Note: pluginval not found on PATH; skipping pluginval." -ForegroundColor Yellow
        }
    }

    Write-Host "==> Packaging dist\windows\VESSEL-II-Windows-x64.zip..." -ForegroundColor Cyan
    $stage = "dist/windows/VESSEL-II-Windows-x64"
    $zip = "dist/windows/VESSEL-II-Windows-x64.zip"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    if (Test-Path $zip) { Remove-Item -Force $zip }
    New-Item -ItemType Directory -Force -Path "$stage/Standalone" | Out-Null

    Copy-Item -Recurse "$BuildDir/Vessel_artefacts/Release/VST3/VESSEL.vst3" "$stage/"
    Copy-Item "$BuildDir/Vessel_artefacts/Release/Standalone/VESSEL.exe" "$stage/Standalone/"
    Copy-Item -Recurse "$BuildDir/Vessel_artefacts/Release/Standalone/Wavetables" "$stage/Standalone/"
    Copy-Item "docs/INSTALL-Windows.txt" "$stage/"

    Compress-Archive -Path "$stage" -DestinationPath $zip -Force
    Remove-Item -Recurse -Force $stage

    Write-Host "DONE: $zip" -ForegroundColor Green
}
finally {
    Pop-Location
}
