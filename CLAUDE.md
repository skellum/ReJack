# Rejack — magicJack A921 revival

## What this directory is
**Rejack**: reverse engineering and userspace software (C, no custom kernel driver) that turns a magicJack A921 USB dongle plus an analog/cordless phone into a home phone line on a legitimate SIP service. Architecture: home phone server (Asterisk) in the middle; Rejack is a baresip-based SIP extension. Integration layer ("Switchboard", nickname Ansible) comes later. See `PROJECT.md` for scope, decisions and phase checklists.

Public repo: https://github.com/skellum/ReJack (MIT). Keep personal data out of committed files: no usernames/home paths (use relative paths or `/home/user/ReJack` as an example), no device serial (masked `A921XXXXXXXXXX`), no server hostnames (say "ServerName"), no secrets (`*.env` is gitignored; only `*.env.example` is committed).

## Read these first in a new session
- `ARCHITECTURE.md` — what runs where, who does what (ReJack vs Asterisk vs magicJack), a call end to end
- `PROJECT.md` — scope, architecture, decisions table, phase checklists, open questions
- `HARDWARE.md` — device identity, interface map, audio specs + test results, HID bit layout, udev rule
- `RING.md` — ring/line-power research: protocol framing, sources, probe results, next steps
- `rejack/README.md` — module layout, build, run, 911 behaviour
- `LEARNING_GUIDE.md` — educational walkthrough of the original diagnostic output (reference only)
- `MJ-A921-RING.md` — unverified Gemini notes (partly wrong; kept for reference)

## Key facts
- Device: TigerJet Network / magicJack A921, USB VID:PID `06e6:c200`; USB 2.0 port required
- ALSA card `TigerJet` (use `hw:TigerJet,0`, the number varies): capture 8 kHz mono, playback 8/16 kHz mono
- HID: `/dev/magicjack-hid` (udev rule installed; `audio` group, no sudo needed). Line state comes from
  registers, not the 2-byte input report (its keypad field is for TigerJet USB handsets)
- TigerJet registers: read/write via 32-byte HID feature report (`04 <reg> <reg&0xE0> <n> <data>`) — working
- Chip type reg 0x55 = 0x13 (non-ARM). SPI serial port (0x26–0x29) runs, but no ProSLIC answers
- **Line works from power-up, no init** (TS52 PRO, 2026-11-17): 34 V on-hook, 14 mA loop. Registers (all verified):
  ring `0x3D |= 0x30` / `&= 0xCF`; hook `0x58` bit 1; DTMF `0x15` (bit 6 present, low nibble 1-9/A=0/B=*/C=#);
  dial tone `0x34` bit 5 = chip automatic (power-up `0x26`), bit 6 = on command. Rejack clears bit 5 while running
  and restores it on exit (a hard kill leaves the line silent until replug)
- Line path echoes: unity audio loopback self-excites → Rejack needs echo cancellation

## Current status (as of 2026-11-17)
- **Phase 1 (hardware events)**: done. Hook (0x58) and DTMF (0x15) verified with a TS52 PRO buttset.
- **Phase 2 (audio)**: done at the USB/ALSA level (`tools/aloop`, 0 xruns). Off-hook listening test pending.
- **Phase 3 (line power / ring)**: done. Talk battery is always on; ring detected by the TS52 PRO on every burst.
  Sources: magicJack macOS driver (CTj580Phone_Hid) + GVJackApp v2 (Windows) — see `RING.md`. The VTech CS6719's
  "No Line" was the cordless set, not the dongle. Ring voltage not yet measured.
- **Phase 4 (Rejack module)**: builds against baresip/libre **v4.12.0** in `deps/`, loads, opens the dongle.
  State machine, tones, register-polled hook/DTMF (20 ms, debounced), hardware dial tone + ring, E911 status
  check: 139 unit checks pass. Runs against Asterisk as ext 100 (`rejack/run/start.sh`; account in gitignored
  `rejack/run/accounts`). 911 policy: always attempt;
  contacts notified (ntfy + Telegram via `rejack/scripts/rejack-emergency`); if 911 can't go through →
  announcement (`rejack/sounds/emergency.wav`) + local recording + alerts. Not yet run with a SIP account.
- **Phase 5 (phone server)**: Asterisk 20 (system package) configured from `pbx/` (`sudo pbx/install.sh`;
  passwords in gitignored `pbx/secrets.env`). Rejack = ext 100 (baresip localhost-only, `net_interface 127.0.0.1`,
  port 5070), Linphone 101 (cell) / 102 (desktop). **First calls 2026-11-17: both directions, clean audio, no echo.**
  Rejack runs as systemd `rejack.service` (`sudo rejack/systemd/install.sh`; logs `journalctl -u rejack -f`;
  after any stop `tools/tjprobe release` restores the chip's dial tone). Rejack retries the dongle every 2 s
  (hot-plug). Next: unavailable/voicemail, 911 fallback test, VoIP.ms trunk.
- **Phase 6+ (packaging, integrations)**: not started.

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
