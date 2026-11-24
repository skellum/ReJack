# Changelog

Newest first. Commit hashes refer to this repository.

**Legend:** ✅ completed · 🔄 changed · 💡 proposed or decided · ⏳ pending · ❌ tried and ruled out

---

## [Unreleased]: in progress

**E911 confirmed; 911 routed out the trunk** (2026-11-24)
- ✅ The provider's 911 test line, called from the analog phone over the trunk, read back the caller ID, passed
  the test and said 911 is enabled for the number. 933 had given an instant 503 on this provider; its own
  published test number is the one that works.
- 🔄 911, 1911 and 9911 now go to a new `[emergency]` context, rendered by `pbx/install.sh`. With a trunk, it's a
  real 911 call: out the trunk with the DID as caller ID, no ring timeout, and congestion if it fails, so ReJack's
  fallback (announcement, recording, alerts) still runs. Without a trunk it stays the test congestion.

**Call quality: crackle found and fixed** (2026-11-24)
- ✅ The crackle and pops toward the analog phone were lost USB packets: 2–7% of them, more with louder audio,
  with the dongle on an extension cable and on the same USB controller as a Wi-Fi adapter. Plugged directly into a
  rear port on its own controller: 0 errors, and a clean echo test. Measurements and the method in
  `docs/HARDWARE.md` ("USB link quality").
- ❌ Ruled out: playback gain, the vendor codec init, host clock drift, the kernel's generic implicit feedback.
- 🔄 udev rule: desktop sound servers ignore the dongle (`PULSE_IGNORE`, `ACP_IGNORE`).

**Single-program build; software ATA plan** (2026-11-24)
- 💡 `ARCHITECTURE.md`: ReJack as a software ATA, standalone on a provider account or behind any PBX.
- ✅ baresip can be built with ReJack, its modules and libre compiled in: one program that needs only system
  libraries (`rejack/README.md`, step 3). It passed live calls in both directions, and now runs the line.

**ALSA error spin; stale line-down in the status tool** (2026-11-23)
- 🔄 The dongle dropped off USB 16 s after a call (`disabled by hub (EMI?)`, then it stayed gone until replugged).
  A baresip playback thread was still open and retried its write to the missing device in a tight loop: a full core
  for 20 minutes, with ~100,000 `alsa: write error` lines a second in the journal. The new
  `rejack/patches/baresip-alsa-error-backoff.patch` makes baresip's ALSA read and write loops wait one frame after
  an error, and playback logs once per run of errors. The patch is applied before the baresip build
  (`rejack/README.md`). Which object kept the player open isn't known yet: no thread outlives a call in normal use.
- 🔄 `tools/rejack`: a service restart clears a line-down alert whose line-up never came. The old process died
  before its 30 s "stable" check, and the tool kept showing LINE DOWN with the dongle attached.
- ⏳ The emergency contacts got the line-down alert but no line-up, because the restart lost the monitor's state.
  Proposed: keep the line-monitor state across restarts.

**SIP trunk: a real phone number** (2026-11-23)
- ✅ `pbx/install.sh` sets up a registration-based trunk when `TRUNK_HOST` is set in `secrets.env`, rendered from
  the new `pjsip_trunk.conf.in` and `extensions_trunk.conf.in`. Calls to the number ring the analog phone and the
  desktop, then voicemail 100. 10 or 11 digits from the house go out with the number as caller ID, and 933 reaches
  the provider's 911 test line. 911 stays test-only until E911 is confirmed.
- ❌ First try: `Registered`, but no call went through. 14 INVITEs out got no reply (dead air), and calls in never
  arrived (callers heard busy). The home internet is behind carrier-grade NAT. Probe packets showed the ISP drops
  INVITEs on port 5060, whatever their size, while REGISTER and OPTIONS pass.
- ✅ The trunk now uses the provider's alternate port (`TRUNK_PORT=5080`) and its own local transport (5160). Calls
  in and out work, with audio both ways. The house phones are pinned to the 5060 transport. Endpoints are now
  identified by IP first, so a caller ID like `100` can't match an extension. The `_X.` intercept moved to its
  own `[intercept]` context, included after `[trunk-out]`.
- 🔄 `docs/QUICKSTART.md` part 4 is now marked tested, with CGNAT and port-5060 notes, a test plan with results,
  and new troubleshooting rows.

**Quickstart; mobile softphone decision** (2026-11-23, docs)
- ✅ `docs/QUICKSTART.md`: Asterisk, Linphone and a SIP trunk provider, step by step. Parts 1–3 as run here; the
  trunk part (portal checklist, PJSIP registration, ACL/NAT, dial plan, E911 order, test plan) is marked untested.
- 💡 Decided: a cell softphone is a test client, not a reachable extension. Calls to ext 101 went to voicemail
  whenever the app was in the background, and keeping it registered costs battery. People away from home are
  reached on their real mobile number over the trunk.

**Capture watchdog; vendor flash off** (2026-11-23)
- 🔄 The always-on DTMF capture spun a core for 5 hours: the dongle was USB-reset at 03:01, and baresip's ALSA
  reader retries a failed read at once, forever. Rejack now reopens the capture if no audio arrives for 1 s
  (backing off 2 s → 64 s while it keeps failing; the chip decoder covers the gap).
- 🔄 The resets (3 that night) followed the dongle's built-in flash drive re-appearing (`sdb`, 18 MB): the udev rule
  now deauthorizes that mass-storage interface. Unused by Rejack; the vendor image is already saved.

**Line levels** (2026-11-22)
- 🔄 The card powers up at capture 8 / playback 0 (0–15 dB): speech from the phone was so quiet that voicemail
  saved it as silence and the 600 echo test needed shouting. Now capture 15, playback 8, set on every plug-in
  by `rejack/systemd/99-magicjack.rules` (installed by `rejack/systemd/install.sh`). Unknown numbers now get the
  intercept message as early media and a 404, so they no longer show as answered.

**Outgoing digits with a VTech handset** (2026-11-21/22)
- 🔄 Calls in worked but calls out failed: 101 arrived as `0`. A register trace (new `tjprobe trace`) showed
  that `0x15` bit 6, which Rejack used as "digit present", pulses for ~20 ms on some digits and never appears on
  others; the low nibble changes on every digit. First fix: a nibble change is a digit.
- ❌ Not enough: the nibble keeps the last digit, so a repeated key, or a call starting with the previous call's
  last digit (101 after 101 → `01`), leaves no trace. `0x15` ignores writes; a full register watch found no
  other key-press signal.
- ✅ Rejack now decodes DTMF from the line audio (`src/dtmfdet.c`, Goertzel; 7 new unit checks, 169 total). On
  a recording of the line (`test/dtmfscan`) it found 12 of 12 digits, `111002` dialled slowly and pre-dialled,
  where the chip register gave `002` and `1002`. The chip decoder stays as the fallback.

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
| 4 | ✅ **Call quality**: crackle/pops on audio played to the analog phone | Fixed 2026-11-24: lost USB packets (extension cable, shared controller) |
| 3 | ⏳ Measure the ring voltage | Needs a meter on the tip/ring pair during a ring |
| 4 | ⏳ Provider E911 checker script | Provider chosen; E911 confirmed 2026-11-24 |
| 4 | ⏳ ALSA dsnoop so the emergency recording and call audio can share capture | Before bridging a contact into a 911 call |
| 4 | ⏳ Echo canceller | None heard so far, including the first trunk calls |
| 5 | ⏳ Voicemail: test | **Current** |
| 5 | ⏳ Exercise the 911 fallback end to end | Backlog. 911 is a real call since 2026-11-24: never test-dial it on the live line. Run it on a dev setup without a trunk (test congestion), or in a planned window with `[emergency]` pointed back to congestion. Sends real alerts if configured |
| 5 | ✅ **E911 on the trunk**: address registered, test line read it back, 911 routed out the trunk | 2026-11-24; `docs/QUICKSTART.md` 4d |
| 5 | ⏳ Trunk: idle inbound (15+ min, CGNAT mapping), menu DTMF, voicemail on ring-out | Test plan in `docs/QUICKSTART.md` 4e |
| 5 | 💡 Ring the cell's real number over the trunk when home doesn't answer | Replaces a background softphone |
| 4 | 💡 Open the DTMF capture at off-hook only | The VTech waits before sending pre-dialled digits; measure the gap first |
| 5+ | 💡 Voicemail notifications (ntfy/Telegram via externnotify) | Proposed |
| 5+ | 💡 MWI stutter dial tone on the analog phone | Proposed |
| post-MVP | 💡 Caller ID (Bell 202 FSK between rings, also sets the phone's clock) | Proposed |
| post-MVP | 💡 Hook flash (call waiting, transfer) | Proposed |
| 6 | 🔄 Packaging (udev + systemd + config as an installable unit) | Single program + packaging in a separate repo (rejack-solo); running the line since 2026-11-24 |
| 6 | 💡 One line per room: VTech CM18445 + 4 dongles, one extension each | Compatibility test on one line first; see `ARCHITECTURE.md` |
| 6 | 💡 Multi-dongle Rejack (`rejack@.service`, per-port device names) | Needed for household phones |
| 7 | 💡 Switchboard: Discord voice bridge (ext 700), OpenClaw agent calls / dial-the-agent (ext 800) | Spec in `INTEGRATIONS.md` |
| 7 | 💡 Switchboard: Home Assistant/MQTT, doorbell, Telegram alerts | Proposed |

**Call quality (backlog, 2026-11-22; solved 2026-11-24, see the entry at the top).** Crackle and pops on the network → phone direction, heard on Asterisk's
prompts and on the 600 echo test. What is known:
- Not the prompt codec: still there after installing the WAV prompts (only GSM ones were installed before).
- No ALSA or baresip errors in the log during the calls; `tools/aloop` ran 0 xruns on 2026-11-16.
- Levels changed the same day: playback 0 → 8 dB, capture 8 → 15 dB (udev rule). Crackle with playback lower
  is not yet tried, so clipping isn't ruled out.
- Earlier anomaly, never re-tested: with audio playing, the capture shows ~7.4 ms bursts near -1 dBFS (≈ 3.6 kHz)
  at 50–450 ms intervals (`docs/HARDWARE.md`, audio test results).
- Next: record a call on both ends (Asterisk `MixMonitor` and a baresip-side capture), and compare playback
  0/4/8; check host/device clock drift (both USB endpoints have no feedback) and the baresip `aubuf` settings.

**Still open:** whether the CM18445 accepts the A921's 34 V line.
