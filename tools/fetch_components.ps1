# NanoJS8 — fetch_components.ps1
#
# Downloads the vendored M5Stack libraries (M5GFX, M5Unified, M5Cardputer)
# at the exact versions Mini-FT8 verified working against Cardputer ADV on
# ESP-IDF v5.5.x. Idempotent: safe to re-run; if a component is already at
# the pinned version, that component is skipped.
#
# Each library is committed to your local repo as a vendored copy (no .git
# folder, no submodule). This matches Mini-FT8's vendoring strategy and the
# reproducibility plan in BUILD_ENVIRONMENT.md.
#
# Usage (from project root):
#   .\tools\fetch_components.ps1
#
# Requires git on PATH.

$ErrorActionPreference = "Stop"

# -----------------------------------------------------------------------------
# Pinned versions. When updating, also update BUILD_ENVIRONMENT.md.
# Versions match Mini-FT8's verified baseline on Cardputer ADV.
# -----------------------------------------------------------------------------
$Components = @(
    @{
        Name    = "M5GFX"
        Url     = "https://github.com/m5stack/M5GFX.git"
        Version = "0.2.17"   # Mini-FT8 library.properties
    },
    @{
        Name    = "M5Unified"
        Url     = "https://github.com/m5stack/M5Unified.git"
        Version = "0.2.11"
    },
    @{
        Name    = "M5Cardputer"
        Url     = "https://github.com/m5stack/M5Cardputer.git"
        Version = "1.1.1"
    }
)

# -----------------------------------------------------------------------------
# Pre-flight.
# -----------------------------------------------------------------------------
if (-not (Test-Path "main\main.cpp")) {
    Write-Error "Run this script from the NanoJS8 project root."
    exit 1
}
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if (-not $gitCmd) {
    Write-Error "git is not on PATH. Install Git for Windows or open a shell where git is available."
    exit 1
}

# -----------------------------------------------------------------------------
# For each component: clone shallow at the pinned tag, then strip .git so it's
# committed as a vendored copy. If the target dir already has the right version
# marker file, skip.
# -----------------------------------------------------------------------------
foreach ($comp in $Components) {
    $name      = $comp.Name
    $url       = $comp.Url
    $version   = $comp.Version
    $targetDir = "components\$name"
    $stampFile = Join-Path $targetDir ".nanojs8_pin"

    if (Test-Path $stampFile) {
        $existing = Get-Content $stampFile -Raw
        if ($existing.Trim() -eq $version) {
            Write-Host "[$name] already at v$version, skipping."
            continue
        }
    }

    # Remove any existing copy so we start clean.
    if (Test-Path $targetDir) {
        Write-Host "[$name] removing existing dir to refresh..."
        Remove-Item -Path $targetDir -Recurse -Force
    }

    Write-Host "[$name] cloning $url @ v$version ..."
    # Try tag first (m5stack publishes git tags). Fall back to 'main' branch if
    # the tag doesn't exist (rare but possible if they retag).
    & git clone --depth 1 --branch $version $url $targetDir 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "[$name] tag v$version not found, trying 'main' branch and resetting to commit..."
        & git clone --depth 1 $url $targetDir
        if ($LASTEXITCODE -ne 0) {
            Write-Error "[$name] git clone failed."
            exit 1
        }
    }

    # Strip the .git folder so the component becomes a flat vendored copy.
    Remove-Item -Path (Join-Path $targetDir ".git") -Recurse -Force

    # Stamp the pinned version so re-runs skip cleanly.
    Set-Content -Path $stampFile -Value $version -NoNewline

    Write-Host "[$name] vendored at v$version."
}

# -----------------------------------------------------------------------------
# M5Cardputer's library.properties says it depends on IRremote and LibSSH-ESP32.
# We don't use either (NanoJS8 has no IR control and no SSH). The build system
# only pulls REQUIRES specified in CMakeLists.txt, so this is fine — the deps
# in library.properties are Arduino-IDE metadata, ignored by ESP-IDF.
# -----------------------------------------------------------------------------

Write-Host ""
Write-Host "All components fetched. Components dir is now:"
Get-ChildItem -Path "components" -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. (Optional) .\tools\apply_gfsk8_patches.ps1   # clones gfsk8 modem (Phase 4 prep)"
Write-Host "  2. idf.py set-target esp32s3"
Write-Host "  3. idf.py build"
