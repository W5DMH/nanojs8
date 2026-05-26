# NanoJS8 — apply_gfsk8_patches.ps1
#
# Clones gfsk8-modem-clean (jfrancis42) into components/gfsk8_modem/upstream/
# and stages it for patch application. Idempotent: safe to re-run.
#
# Phase 0 behavior: just clones the upstream and pins to a specific commit.
# Phase 4 behavior: also applies the .patch files in patches/.
#
# Usage:   .\tools\apply_gfsk8_patches.ps1
# Run from the project root (the folder that contains main\ and components\).

$ErrorActionPreference = "Stop"

# -----------------------------------------------------------------------------
# Constants — pin upstream to a specific commit for reproducibility.
# When you update this, also update BUILD_ENVIRONMENT.md's gfsk8 section.
# -----------------------------------------------------------------------------
$UPSTREAM_URL    = "https://github.com/jfrancis42/gfsk8-modem-clean.git"
$UPSTREAM_REF    = "main"   # Phase 4 will pin a SHA here; for Phase 0 main is fine.
$UPSTREAM_DIR    = "components\gfsk8_modem\upstream"
$PATCHES_DIR     = "components\gfsk8_modem\patches"

# -----------------------------------------------------------------------------
# Pre-flight checks.
# -----------------------------------------------------------------------------
if (-not (Test-Path "main\main.cpp")) {
    Write-Error "This script must be run from the NanoJS8 project root (the folder containing main\ and components\)."
    exit 1
}

$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if (-not $gitCmd) {
    Write-Error "git is not on PATH. Install Git for Windows (https://git-scm.com/) or open a PowerShell where git is available."
    exit 1
}

# -----------------------------------------------------------------------------
# Clone (or update) upstream.
# -----------------------------------------------------------------------------
if (Test-Path "$UPSTREAM_DIR\.git") {
    Write-Host "[gfsk8] upstream already cloned, fetching latest on '$UPSTREAM_REF'..."
    Push-Location $UPSTREAM_DIR
    git fetch origin $UPSTREAM_REF | Out-Null
    git checkout $UPSTREAM_REF      | Out-Null
    git reset --hard "origin/$UPSTREAM_REF" | Out-Null
    Pop-Location
} else {
    if (Test-Path $UPSTREAM_DIR) {
        # Directory exists but not a git checkout (probably empty placeholder from zip).
        # Remove it so git clone can succeed.
        Write-Host "[gfsk8] removing empty placeholder upstream dir..."
        Remove-Item -Path $UPSTREAM_DIR -Recurse -Force
    }
    Write-Host "[gfsk8] cloning $UPSTREAM_URL into $UPSTREAM_DIR ..."
    git clone --branch $UPSTREAM_REF $UPSTREAM_URL $UPSTREAM_DIR
    if ($LASTEXITCODE -ne 0) {
        Write-Error "git clone failed. Check network access to github.com."
        exit 1
    }
}

# Record the actual commit SHA we just landed on, for BUILD_ENVIRONMENT.md.
Push-Location $UPSTREAM_DIR
$shortSha = (git rev-parse --short HEAD).Trim()
$longSha  = (git rev-parse HEAD).Trim()
Pop-Location
Write-Host "[gfsk8] upstream pinned to $shortSha ($longSha)"

# -----------------------------------------------------------------------------
# Phase 0: do NOT apply patches yet. They are placeholders.
# Phase 4 will replace this section with `git apply` calls against real .patch
# files regenerated to match upstream HEAD.
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[gfsk8] Phase 0: patches in $PATCHES_DIR are documentation placeholders."
Write-Host "[gfsk8] Real .patch generation deferred to Phase 4."
Write-Host "[gfsk8] The Phase 0 build does NOT link gfsk8 yet (see main\CMakeLists.txt)."
Write-Host ""
Write-Host "[gfsk8] Done. Proceed with: idf.py set-target esp32s3 && idf.py build"
