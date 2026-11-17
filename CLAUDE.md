# Rejack — magicJack A921 revival

## What this directory is
**Rejack**: reverse engineering and userspace software (C, no custom kernel driver) that turns a magicJack A921 USB dongle plus an analog/cordless phone into a home phone line on a legitimate SIP service. Architecture: home phone server (Asterisk) in the middle; Rejack is a baresip-based SIP extension. Integration layer ("Switchboard", nickname Ansible) comes later. See `PROJECT.md` for scope, decisions and phase checklists.

Public repo: https://github.com/skellum/ReJack (MIT). Keep personal data out of committed files: no usernames/home paths (use relative paths or `/home/user/ReJack` as an example), no device serial (masked `A921XXXXXXXXXX`), no server hostnames (say "ServerName"), no secrets (`*.env` is gitignored; only `*.env.example` is committed).

## Read these first in a new session
- `PROJECT.md` — scope, architecture, decisions table, phase checklists, open questions
- `HARDWARE.md` — device identity, interface map, audio specs + test results, HID bit layout, udev rule
- `RING.md` — ring/line-power research: protocol framing, sources, probe results, next steps
- `rejack/README.md` — module layout, build, run, 911 behaviour
- `LEARNING_GUIDE.md` — educational walkthrough of the original diagnostic output (reference only)
- `MJ-A921-RING.md` — unverified Gemini notes (partly wrong; kept for reference)

## Key facts
- Device: TigerJet Network / magicJack A921, USB VID:PID `06e6:c200`; USB 2.0 port required
- ALSA card `TigerJet` (use `hw:TigerJet,0`, the number varies): capture 8 kHz mono, playback 8/16 kHz mono
- HID: `/dev/magicjack-hid` (udev rule installed; `audio` group, no sudo needed). 2-byte input report
  (bit 15 hook, bits 0-3 keypad) — not yet confirmed on hardware
- TigerJet registers: read/write via 32-byte HID feature report (`04 <reg> <reg&0xE0> <n> <data>`) — working
- Chip type reg 0x55 = 0x13 (non-ARM). SPI serial port (0x26–0x29) runs, but no ProSLIC answers
- Ring drive = 0x3D bits 4+5 (works); hook = 0x58 bit 1; line power (boost converter) still **not on** — Phase 3 risk
- Line path echoes: unity audio loopback self-excites → Rejack needs echo cancellation

## Current status (as of 2026-11-16)
- **Phase 1 (hardware events)**: register access works. Blocked: hook test needs the cordless handset charged
  (`rejack/run/start.sh` or `tools/tjprobe watch`, then lift/hang up/press keys).
- **Phase 2 (audio)**: done at the USB/ALSA level (`tools/aloop`, 0 xruns). Off-hook listening test pending.
- **Phase 3 (line power / ring)**: board = discrete booster + HV H-bridge (no SLIC). From magicJack's macOS driver:
  our class is CTj580Phone_Hid; ring = 0x3D bits 4+5 (verified: H-bridge pins toggle), hook = 0x58 bit 1. Line
  still dead after init580 — next: TS53 PRO on the line, then RE `mj_dev` SetTelephoneEnable.
- **Phase 4 (Rejack module)**: builds against baresip/libre **v4.12.0** in `deps/`, loads, opens the dongle.
  State machine, tones, HID decode, E911 status check: 111 unit checks pass. 911 policy: always attempt;
  contacts notified (ntfy + Telegram via `rejack/scripts/rejack-emergency`); if 911 can't go through →
  announcement (`rejack/sounds/emergency.wav`) + local recording + alerts. Not yet run with a SIP account.
- **Phase 5+ (Asterisk, provider, packaging, integrations)**: not started. Provider leaning VoIP.ms.

## Working notes
- Build/run: `deps/` holds libre + baresip (not committed; rebuild per `rejack/README.md`).
  `cmake --build rejack/build && ctest --test-dir rejack/build`; dev run: `rejack/run/start.sh`
  (generates `rejack/run/config` from `config.in`).
- `ext/` (gitignored): cloned references — magicjack-hacking (HOME/ARM RE, tjctl.c, datasheets),
  magicjack-liberation (reviewed, not applicable), baresip + re sources at v4.12.0.
- `images/` (gitignored): device storage images (vendor installer — never commit).
- Version pinning: use releases at least two weeks old.

## Raw diagnostic output files
- `../NOTES.md` (parent of the project directory, not in the repo) — original dmesg from failed USB 1.1 enumeration
- `LSUSB.out` — full `lsusb -v` descriptor dump (serial masked)
- `DEVICES.out` — sysfs interface/driver binding listing
- `APLAY.out` / `ARECORD.out` — ALSA device listings
