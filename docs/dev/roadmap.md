# Roadmap

## Release plan (user decision 2026-10-07)
Supersedes the 2026-10-05 sequence (Settings polish → OOB → Home → emulation → OSD/in-game QM), all since implemented.
1. Alpha = the features implemented today (Web Mode excluded).
2. Alpha → RC: bug fixing and polish only; no new Epics or platform work. Blockers found in the current step are never skipped.
3. After the RC, in order: Linux 7.3 (once stable); deep sleep (suspend-to-RAM instead of s2idle); wireless casting EPIC-029 (needs Wi-Fi Direct/P2P for RTL8821CS — rtw88 exposes no P2P modes — and a Cedrus H.264 encoder — mainline Cedrus is decode-only; until then mirroring = software encoding); Spotify Connect EPIC-056 (Connect receiver through Product Audio, librespot candidate, redistribution terms to verify); Web Mode EPIC-030 (implemented, parked).
EPIC-014 (standalone emulators) removed 2026-10-07: RetroArch/libretro is the only backend.
