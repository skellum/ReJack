# Changelog

Newest first. Commit hashes refer to this repository.

**Legend:** ✅ completed · 🔄 changed · 💡 proposed or decided · ⏳ pending · ❌ tried and ruled out

---

## [Unreleased]: in progress

**Outgoing digits with a VTech handset** (2026-11-21)
- 🔄 Calls in worked but calls out failed: 101 arrived as `0`. A register trace (new `tjprobe trace`) showed
  that `0x15` bit 6, which Rejack used as "digit present", pulses for ~20 ms on some digits and never appears on
  others; the low nibble changes on every digit. Rejack now takes a nibble change as a digit, and a bit-6 edge
  only for the same key pressed twice. 4 new unit checks from the trace (162 total).

**Line monitor** (2026-11-21)
- ✅ `tools/rejack`: one-shot status, `-v` live log (calls, state changes and errors highlighted, repeats
  folded) and `--visual`, a top-style dashboard: service, dongle (USB port, speed, hidraw, ALSA card), SIP
  registration, line state, current call, today's calls in/out and errors. Read-only: journal, sysfs, systemctl.

**Line-down alert** (2026-11-20; deployed 2026-11-21)
- ✅ Rejack tells the emergency contacts when the phone line is down: dongle missing, or dropping off and
  re-enumerating, for 2 minutes (`rejack_line_alert`) → `line-down`; back and stable for 30 s → `line-up` with
  the outage length. New `src/linemon.c` (pure logic, 19 new unit checks; 158 total).
- Why: on 2026-11-19 the dongle was reset 449 times in 15 hours while a failing USB drive shared its USB 2.0
  controller, then stopped enumerating. Rejack retried for about 32 hours and nobody knew.

**Integrations spec and multi-line plan** (2026-11-18, docs only)
- 💡 `INTEGRATIONS.md`: Switchboard (Node/TS, Asterisk ARI + externalMedia), Discord voice bridge, OpenClaw phone
  agent (the Gateway's chat-completions HTTP API + local STT/TTS; its voice-call plugin has no SIP support).
- 💡 One line per room (`ARCHITECTURE.md`): a 4-line DECT system on 4 dongles; ring group and conference for two extensions.

**Voicemail on Asterisk** (`8c1bd60`; installed 2026-11-18, being tested)
- 💡 Ring 25 s. On no answer or no registration the caller gets the "unavailable" greeting; on busy, the "busy"
  greeting.
- 💡 `*97` opens your own mailbox without a PIN. `*98` opens any mailbox with a PIN.
- 💡 Message-waiting indicator in Linphone through `mailboxes=` on each aor.
- 💡 New `pbx/asterisk/voicemail.conf.in` (PINs in the gitignored `pbx/secrets.env`).
- 💡 New `pbx/asterisk/modules.conf`: turns off the IMAP/ODBC voicemail variants and unused channel drivers.
- 🔄 Plan change: voicemail was "provider first, post-MVP". It now lives in the home Asterisk, so it works before
  there's a trunk.
- ✅ Added `CHANGELOG.md`.
- ✅ Installed with `sudo pbx/install.sh`.
- ⏳ Test unavailable, no-answer, `*97` and MWI, then update the docs.

---

## 2026-11-17: Service and hot-plug (`8f9f57a`)
- ✅ systemd `rejack.service`:
  - Starts at boot, restarts on crash, and runs as a normal user in the `audio` group with a hardened sandbox.
  - `ExecStopPost` runs `tjprobe release`, which hands dial tone back to the chip.
- ✅ Hot-plug: Rejack retries the dongle every 2 s.
  - Unplugging mid-call drops the call cleanly. Replugging brings dial tone back with no restart.
- ✅ Verified: a call through the service, unplug and replug, and stop giving the chip's dial tone.

## 2026-11-17: Architecture doc (`a16fc99`)
- ✅ `ARCHITECTURE.md`:
  - What runs where.
  - Who owns which function (copper pair = Rejack, routing = Asterisk).
  - BORSCHT mapping (magicJack = line card plus ring generator, Rejack = line drawer controller,
    Asterisk = switch).
  - One call end to end.
  - Status table.
  - Placement options.
  - Why not drop Rejack.

## 2026-11-17: First calls (`1cf3939`)
- ✅ Asterisk 20 home PBX from `pbx/`:
  - Ext 100 = Rejack, 101 = Linphone cell, 102 = Linphone desktop.
  - Test extensions: 600 echo test, 601 hello-world, and 911/1911/9911 → test congestion.
  - SIP is limited to the LAN and localhost by ACL.
  - Passwords are in the gitignored `pbx/secrets.env`.
- ✅ Calls work in both directions (cell ↔ analog phone) with clean audio and no echo.
- ✅ Fixed: an outgoing call dropped to a busy beep. The ringback tone was holding the ALSA device; it now stops on
  answer or progress.
- ✅ Fixed: baresip "Protocol not supported" on loopback, by using `net_interface 127.0.0.1`.
- ✅ Every dialled digit is now logged.
- 🔄 VPN access dropped from the ACL and docs. Softphones must be on the home LAN.
- 🔄 Linphone setup: a third-party SIP account is added next to any existing linphone.org account. After a
  password change, delete and re-add the account (Linphone caches the old hash).

## 2026-11-17: The line works (`ba3f997`)
- ✅ Buttset tests (TS52 PRO):
  - Talk battery is on from power-up: 34 V on-hook, 14 mA loop, no init needed.
  - Dial tone works and stops on the first digit.
  - Ring (`0x3D`): every burst was detected.
  - Hook (`0x58` bit 1) and DTMF (`0x15`) are both decoded by the chip.
- ✅ `tjdev` rewritten:
  - Register polling every 20 ms with debounce.
  - Hardware dial tone (`0x34`).
  - The chip is restored on exit.
- ✅ 139 unit checks pass, clean under ASan/UBSan.
- ✅ `tjprobe` gained `gvinit`, `release` and `hook` (with DTMF).
- ✅ Resolved: a cordless set's "No Line" message came from the cordless set, not the dongle.
- ✅ Open question answered: the chip decodes DTMF itself, so Rejack doesn't need software DTMF detection.

## 2026-11-17: GVJack RE (`bc7481c`)
- ✅ Reverse engineered GVJackApp v2 (Windows, 2013). This gave a second, independent source for the init, ring and
  hook registers, and for chip DTMF.
- ❌ A downloaded GVJack package turned out to be a downloader stub with no payload.

## 2026-11-16: Hardware research (`463ac96` → `a7687e9`)
- ✅ Board teardown, and RE of the magicJack macOS driver (TJ580 init, ring, hook).
- ✅ `tjprobe` gained the gpio, activate and restore modes.
- ❌ ProSLIC-over-SPI hypothesis: no SLIC answers on the serial port.
- ❌ magicjack-liberation repo: not applicable to this hardware.
- ❌ mj_dev: no software line-enable for the TJ580. In the end none was needed.

## 2026-11-16: Project start (`971cc32` → `e3abf3b`)
- ✅ Hardware enumeration with standard Linux drivers:
  - ALSA `TigerJet` (8 kHz).
  - The HID feature report gives register access.
  - A udev rule allows access without root.
- ✅ `tools/aloop` audio test: 0 xruns. A unity loop self-excites, so the default loop gain is −6 dB.
- 💡 Decisions:
  - Name: Rejack.
  - Language: C.
  - Ring: hardware ring.
  - Architecture: home PBX in the middle, with Rejack as a SIP extension.
  - VoIP stack: baresip/libre v4.12.
  - Event loop: libre epoll plus an audio thread.
  - Codec: G.711 μ-law.
  - 911: always attempt and notify contacts; if the call can't go through, announce, record and alert.
- 🔄 Phone server: chosen between Asterisk and FreeSWITCH. Asterisk was picked when the PBX was built
  (2026-11-17).
- ✅ MIT license and README.

---

## Pending (roadmap)

| Phase | Item | Notes |
|---|---|---|
| 2 | ⏳ Loopback listening test with the line active; re-check the capture bursts | Low priority |
| 3 | ⏳ Measure the ring voltage | Needs a meter on the tip/ring pair during a ring |
| 4 | ⏳ Provider E911 checker script | After the provider is chosen |
| 4 | ⏳ ALSA dsnoop so the emergency recording and call audio can share capture | Before bridging a contact into a 911 call |
| 4 | ⏳ Echo canceller | None heard so far; revisit with a real trunk |
| 5 | ⏳ Voicemail: test | **Current** |
| 5 | ⏳ Exercise the 911 fallback against the test congestion | Sends real alerts if configured; plan it first |
| 5 | ⏳ SIP trunk provider (real number, inbound/outbound, E911) | Confirm pricing at signup |
| 5+ | 💡 Voicemail notifications (ntfy/Telegram via externnotify) | Proposed |
| 5+ | 💡 MWI stutter dial tone on the analog phone | Proposed |
| post-MVP | 💡 Caller ID (Bell 202 FSK between rings, also sets the phone's clock) | Proposed |
| post-MVP | 💡 Hook flash (call waiting, transfer) | Proposed |
| 6 | ⏳ Packaging (udev + systemd + config as an installable unit) | Pieces exist; not packaged yet |
| 6 | 💡 One line per room: VTech CM18445 + 4 dongles, one extension each | Compatibility test on one line first; see `ARCHITECTURE.md` |
| 6 | 💡 Multi-dongle Rejack (`rejack@.service`, per-port device names) | Needed for household phones |
| 7 | 💡 Switchboard: Discord voice bridge (ext 700), OpenClaw agent calls / dial-the-agent (ext 800) | Spec in `INTEGRATIONS.md` |
| 7 | 💡 Switchboard: Home Assistant/MQTT, doorbell, Telegram alerts | Proposed |

**Still open:** the SIP trunk provider and its pricing; whether the CM18445 accepts the A921's 34 V line.
