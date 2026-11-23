# Project history and lab notes

How ReJack went from a dongle in a drawer to a working phone line, phase by phase, with the plan as it was
written at the time. The current design is in [`../ARCHITECTURE.md`](../ARCHITECTURE.md); what changed when is in
[`../CHANGELOG.md`](../CHANGELOG.md). This page keeps the working notes: what each phase set out to prove, how it
was checked, and the facts it established.

---

## How it started

While cleaning out the garage I came across my old magicJack, the same one I used to pair with Google Voice
back in the day. That setup is long dead, but the dongle still looked perfectly healthy. I plugged it into a
Linux box just to see what would happen, and it showed up as a sound card and a keypad.

*Hmm... this looks like a tinker-time tech project.*

The goal: turn owned, otherwise-dead hardware, plus an ordinary analog or cordless phone, into a working home
phone on a cheap, legitimate SIP service, and then make it a voice endpoint for the rest of the house. No vendor
software, no subscription.

**The biggest risk, as written on day one: hardware ringing.** Without it, incoming calls are useless. Audio
came for free from the stock USB audio driver; ringing and hook state did not. That risk shaped the order of the
phases below.

## Naming

- **ReJack**: the daemon that drives the dongle and speaks SIP. The name was checked on 2026-11-16 for software
  or package collisions (none; one unrelated GitHub account).
- **Switchboard**, nickname *Ansible* (after the instant-communication device in the Enderverse): the integration
  layer. "Ansible" collides with Red Hat's automation tool, so it stays a nickname, never a package or command.

## The first design sketch (2026-11-16)

Written before any code, to decide the shape:

```
 libre main loop (epoll)
   ├─ hidraw fd ............ hook / keypad events  (or polled registers if the HID input proves unreliable)
   ├─ timers ............... ring cadence, dial timeout, inter-digit timer
   ├─ SIP sockets .......... baresip user agent
   └─ call state machine ... IDLE → DIALING → CONNECTED;  IDLE → RINGING → CONNECTED
 audio ................... ALSA hw:TigerJet,0, 8 kHz  ⇄  baresip audio pipeline
```

The "or polled registers" branch is the one that won: the HID input report's keypad field belongs to TigerJet's
USB handsets, so the analog line's hook and digits are read from chip registers every 20 ms (Phase 1).

Why a phone server in the middle instead of pointing ReJack straight at a provider: the provider becomes a
config choice in Asterisk, swappable at any time, and every later feature (voicemail, ring groups,
integrations) lands in the server without touching the C code. The design rule: **speak plain SIP**.

## Service options considered (2026-11-16)

Prices change; check at signup.

| Option | What you get | Rough cost |
|---|---|---|
| Pay-per-minute SIP trunk provider | A real US number over SIP, voicemail, caller name, E911 | About $1/month per number + about $0.01/min inbound + about $1.50/month E911 |
| Developer-oriented carrier | Programmable numbers and calls | About $1/month per number + per minute |
| No number | SIP-to-SIP only: intercom and extensions | $0 |

---

## Phase 1: hardware events (done 2026-11-17)

**Goal:** see the phone's hook switch and keypad from Linux.

- Register reads over hidraw: a 32-byte HID feature report, `04 <reg> <reg&0xE0> <count> <data…>` to write,
  select-then-GET to read. Built into `tools/tjprobe`.
- **Hook:** register `0x58` bit 1 (the HID report's byte `0x09` bit 7 mirrors it). 4 of 4 transitions seen with
  a TS52 PRO buttset.
- **Digits:** the chip decodes DTMF itself into register `0x15`: bit 6 = digit present, low nibble = digit
  (1–9, A = 0, B = `*`, C = `#`). All six test digits decoded. No software DTMF detector needed.

## Phase 2: audio (done at the USB/ALSA level)

**Goal:** full-duplex 8 kHz audio through the dongle.

- Capture works at 8 kHz with a noise floor around −76 dBFS; full duplex opens with no errors.
- `tools/aloop` (all modes) runs with 0 xruns. A unity-gain loop through the line self-excites (the 2-wire
  hybrid echoes), so the default loop gain is −6 dB, and an echo canceller is on the list for real trunks.
- Off-hook, the chip's own dial tone is audible on the buttset.
- Still to do: listen to yourself through `tools/aloop loop` with the line active, and re-check the
  playback-induced capture bursts noted in [`HARDWARE.md`](HARDWARE.md).

## Phase 3: line power and ring (done 2026-11-17)

**Goal:** talk battery on the line, and the phone ringing on command. The full derivation is in
[`../RING.md`](../RING.md).

- The line-control registers came from magicJack's macOS driver (class `CTj580Phone_Hid`) and GVJackApp v2
  (Windows, 2013): two independent programs that agree.
- **Talk battery needs no initialization:** 34 V on-hook, 14 mA loop current, from power-up. Neither driver
  has a "line on" call.
- **Ring:** `0x3D |= 0x30` on, `&= 0xCF` off. The TS52 PRO detected ring on every burst. ReJack does the
  cadence (2 s on, 4 s off).
- **Dial tone:** register `0x34`. Bit 5 = automatic dial tone on off-hook (power-up value `0x26`), bit 6 = dial
  tone on command.
- Still to do: measure the ring voltage.

## Phase 4: the ReJack module (working: first calls 2026-11-17)

**Goal:** a baresip module that turns hook and digits into calls.

- Targets baresip/libre **v4.12.0** (Ubuntu's 1.0.0 has an incompatible module API), built into a local prefix.
- `src/phone.c`: the call state machine. Dial tone, digit collection, dialing on `#` or a 4-second pause,
  ringback, busy, reorder and howler, incoming ring and answer, 911 policy. Unit tested.
- `src/tones.c`: North American call-progress tones. `src/tjdev.c`: hidraw access and line decoding.
- `src/rejack.c`: the baresip glue (an application module that replaces `menu.so`).
- 911: always-attempt policy, E911 status-file check (`src/e911.c`), EMERGENCY state with announcement, local
  recording (`arecord`) and the notification hook `scripts/rejack-emergency` (ntfy, Telegram, Discord, email,
  with an offline outbox). Announcement WAV made with espeak-ng at 8 kHz mono.
- Hook and digits by register polling (20 ms, debounced over 3 polls); hardware dial tone; the chip is handed
  back on exit.
- A udev rule gives the `audio` group access to the dongle without root.
- Fixed during first calls: tone playback and call audio both use `hw:TigerJet,0`, so the tone is stopped as
  soon as the call is answered or reports progress (it was dropping outgoing calls with `EBUSY`). Later:
  `dmix`/`dsnoop` so they can overlap.
- Still to do: a provider E911 checker script (writes `verified <did> <time>`), `dsnoop` so the emergency
  recording and the call can share capture, and an echo canceller (none heard so far).

## Phase 5: phone server (in progress)

**Goal:** a home switch with ReJack as one extension.

- Asterisk 20 (distribution package), configured from `pbx/`. ReJack = extension 100 on localhost, softphones
  101 (cell) and 102 (desktop), echo test 600, readback 601. SIP ACL'd to the LAN. 911 routes to test congestion
  until there is a trunk. Calls verified both ways with clean audio.
- systemd service for ReJack: boot start, restart on crash, `tjprobe release` after any stop. ReJack survives
  unplug and replug (retries every 2 s). Verified: a call, an unplug mid-call, a stop.
- Line-down alert (2026-11-20): if the dongle is missing or keeps dropping off for 2 minutes, the emergency
  contacts are told; "restored" follows when it's back.
- Voicemail on Asterisk: configured and installed, being tested.
- SIP trunk (2026-11-23): a registration-based trunk, set up by `pbx/install.sh` from `TRUNK_*` in `secrets.env`.
  The first try registered but no call went through either way: the server behind the home internet's
  carrier-grade NAT got replies to REGISTER and OPTIONS, yet none to 14 INVITEs, and the provider played busy to
  callers. Probe packets showed the ISP was dropping INVITEs on port 5060 whatever their size, while the
  provider's alternate port 5080 answered. With the trunk on 5080 (and its own local port, 5160), calls in and
  out worked, with audio both ways and no NAT settings beyond symmetric RTP.
- Still to do: E911 (register the address, 933, then route 911 out the trunk), and exercise the 911 fallback.

## Phase 6: packaging and more lines (not started)

- udev rule, systemd service and config as an installable unit (the pieces exist).
- One ReJack instance per dongle for the multi-line plan in [`../ARCHITECTURE.md`](../ARCHITECTURE.md#planned-one-line-per-room).

## Phase 7: integrations (spec written)

See [`../INTEGRATIONS.md`](../INTEGRATIONS.md): a Discord voice bridge, a phone agent, and later home automation
events and a doorbell.

---

## Development environment

- Ubuntu, kernel 6.8; the dongle on a USB 2.0 (EHCI) port.
- ALSA card `TigerJet` (look it up by name; the card number varies).
- HID node: `/dev/hidraw*` by VID:PID `06e6:c200` (the number varies), or the udev symlink `/dev/magicjack-hid`.
- Example checkout path: `/home/user/ReJack/` (scripts resolve their own location).
- Tools: `tools/tjprobe.c` (register probe), `tools/aloop.c` (audio test), `tools/fat16x.py` (FAT16 extractor).
- References kept locally, not committed: `ext/` (cloned repositories), `images/` (device storage images,
  which contain the vendor's copyrighted installer).

## Revision history of the plan

| Date | Change |
|------|--------|
| 2026-11-16 | Hardware enumeration complete. HID descriptor decoded. No custom driver required. |
| 2026-11-16 | Decided: C, hardware ring. Proposed: epoll loop plus audio thread, G.711 μ-law 8 kHz. |
| 2026-11-16 | Named ReJack. Scope defined (landline behavior plus the 911 policy). Architecture: home phone server with ReJack as a baresip-based SIP extension. |
| 2026-11-17 | Line works from power-up; ring, hook and DTMF verified. First calls through Asterisk. Service and hot-plug. |
| 2026-11-18 | Integrations spec. Multi-line plan. Voicemail configured. |
| 2026-11-20 | Line-down alert. |
