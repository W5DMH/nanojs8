// NanoJS8 version header — single source of truth for the app version.
//
// Update NANOJS8_VERSION on every release. The string is rendered on the
// splash screen, logged at boot, and included in core-dump metadata.
//
// Versioning follows SemVer: MAJOR.MINOR.PATCH
//   - Phase 0  → 0.0.1   splash + SD detect             [done]
//   - Phase 1  → 0.1.0   NVS + SETUP screen             [current]
//   - Phase 2  → 0.2.0   all 7 screens scaffolded
//   - Phase 3  → 0.3.0   USB host + CAT loopback
//   - Phase 4  → 0.4.0   audio + decode
//   - Phase 5  → 0.5.0   TX + COMPOSE + $GPS
//   - Phase 6  → 0.6.0   inbox + GPS + consensus
//   - Phase 7  → 1.0.0   field-ready release

#pragma once

#define NANOJS8_VERSION       "0.3.3"
#define NANOJS8_VERSION_PHASE "Phase 3a - DigiRig + RTS-PTT"

#define NANOJS8_BUILD_DATE __DATE__
#define NANOJS8_BUILD_TIME __TIME__
