# Rejack

*Reviving a magicJack A921 as a real home phone line on Linux.*

**Status**: Hardware enumerated, audio confirmed via ALSA, HID descriptor decoded, ring control under
investigation (see `RING.md`). No daemon code yet.

---

## Goal

Turn owned, otherwise-dead magicJack A921 hardware (plus an ordinary analog/cordless phone plugged into it)
into a working home phone on a cheap, legitimate SIP service — then make it a voice endpoint for the
rest of the home ecosystem.

## Scope

**MVP — behaves like a normal landline**
1. Incoming call: the phone rings, pick up, two-way audio.
2. Outgoing call: dial tone, dial digits, call goes out.
3. Hang-up in either direction; runs unattended as a service (udev + systemd).
4. 911 handled deliberately — **always attempt, never a dead end** (decided 2026-11-16, option a):
   - 911/1911/9911 are always dialed (provider may route unverified calls to a relay center). E911 status
     (verified/unverified, from a provider checker's status file) is reported, not used to block.
   - Every 911 dial notifies the emergency contacts (ntfy / Telegram / Discord / email, queued if offline).
   - If 911 can't go through (no network, phone server down, call fails): the line stays open, an
     announcement says 911 was NOT reached + "use a mobile phone" + "this line is recorded", the phone's
     audio is recorded locally, contacts are alerted "call back NOW", recording sent on hang-up.
   - Phase 5 (Asterisk): bridge the emergency contact into a failed-911 call, server-side recording.
   - `verified` / `block` policies remain available (fallback still runs).

**Later (post-MVP)**
- Caller ID on the phone's display (Bellcore FSK between rings; also sets the phone's date/time).
- Voicemail (provider first).
- Integration layer (the "Switchboard", working nickname *Ansible* after the Enderverse device) — see below.

**Out of scope**
- magicJack's own service; modifying dongle firmware; more than one phone line; fax/modem.
- Any service used against its terms (e.g. unofficial TextNow clients).

**Biggest risk**: hardware ringing. Without it, incoming calls are unusable.

---

## Naming
- **Rejack** — this project: the daemon that drives the dongle and speaks SIP. Name checked 2026-11-16:
  no software/package collisions (one unrelated GitHub user account named `rejack`).
- **Switchboard / "Ansible"** — the later integration layer. "Ansible" collides with Red Hat's Ansible
  automation tool, so use it as a nickname only, not a package or command name.

---

## Architecture (decided 2026-11-16)

```
 SIP provider (VoIP.ms) ──┐
                          ├──  Home phone server (Asterisk)  ──  Rejack  ── A921 dongle ── analog/cordless phone
 Softphones (Android,  ───┘            │                       (SIP extension)
 iPhone, Windows PC)                   │
                                       └── Integration layer (later): Home Assistant / MQTT,
                                           doorbell, Echo, Discord, Telegram, AI agents
```

- **Rejack** stays small: a SIP extension that registers to the local phone server. All routing,
  provider trunks, voicemail and integrations live in the phone server / integration layer, so new
  integrations never require Rejack changes.
- **VoIP stack**: **baresip** (C, libre). Rejack's hardware support is a baresip module:
  hook/DTMF events in, ring/line control out, call-progress tones generated in software, audio via ALSA.
- **Language**: C (C++ only if a dependency needs it).
- **Event loop**: libre's main loop (epoll-based) for control; ALSA audio on its own real-time thread.
- **Codec**: G.711 μ-law 8 kHz (analog phone is narrowband; capture is 8 kHz only; native to PSTN).

### Rejack internals
```
 libre main loop (epoll)
   ├─ hidraw fd ............ hook / keypad events  (or polled TJ registers if HID input proves unreliable)
   ├─ timerfd .............. ring cadence, dial timeout, inter-digit timer
   ├─ SIP sockets .......... baresip UA
   └─ call state machine ... IDLE → DIALING → CONNECTED;  IDLE → RINGING → CONNECTED
 audio thread (SCHED_FIFO)
   └─ ALSA hw:TigerJet,0 capture 8k / playback 8k  ⇄  baresip audio pipeline
```

---

## Service options (researched 2026-11-16; prices approximate, verify before buying)

| Option | What you get | Approx. cost |
|---|---|---|
| **VoIP.ms** (leaning) | Real US number over SIP, voicemail, CNAM, E911 | DID ≈ $0.85/mo, inbound ≈ $0.009/min, E911 ≈ $1.50/mo |
| Callcentric | Similar pay-per-minute SIP, $0 base plan | verify |
| Telnyx / Twilio | Developer carriers, programmable | ≈ $1/mo per number + per-minute |
| No number | SIP-to-SIP, intercom/extension only | $0 |

Design rule: speak plain SIP; the provider is a phone-server config choice, swappable at any time.

---

## Integration ideas (post-MVP)

| Integration | Feasibility | Approach |
|---|---|---|
| Android / iPhone | Easy | Standard softphone apps (Linphone, Groundwire, Zoiper) registered to the phone server; homebrew app later |
| Windows PC | Easy | Softphone (e.g. MicroSIP) |
| Blink doorbell | Doable, unofficial | Home Assistant community Blink integration → ring the phone. Can break when Blink's cloud changes |
| Echo / Echo Show | Partial | No third-party Alexa calling; announcements/routines via Home Assistant |
| Discord | Doable | Bot joins a voice channel, bridged to the phone server |
| Telegram | Notifications only | Bots can't place voice calls; missed-call / voicemail alerts |
| Teams | Hard / costly | Needs Teams Phone licensing + certified SBC |
| WhatsApp | Not practical | No calling API for personal accounts |
| **AI agents (OpenClaw on ServerName)** | Plausible, depends on agent API | Two directions: (a) agent *calls you* — agent triggers the phone server (e.g. Asterisk ARI/AMI) to ring Rejack and play/stream TTS; (b) *pick up and talk* — a dialable extension that bridges audio to a speech pipeline (STT → agent → TTS). Need to learn what the agent platform exposes (HTTP API, webhooks, voice/realtime audio). |

---

## Hardware Capability Summary

Via standard Linux drivers (see `HARDWARE.md`):
- Line → host audio (ALSA capture, 8 kHz S16 mono); host → line audio (ALSA playback, 8 or 16 kHz)
- HID input report: hook switch, keypad (DTMF), function keys, volume — **to be verified on hardware**
- Vendor 32-byte HID feature report = TigerJet register access (confirmed working for reads/writes)

Unresolved: line power and ringing (`RING.md`).

---

## Component Breakdown

### 1. Hardware access (`tjdev`)
- Locate hidraw node by VID:PID (number varies between boots).
- Read 2-byte input reports; TigerJet register read/write via feature report (`tools/tjprobe.c` has the primitives).
- Line power, ring on/off, hook status.

### 2. Audio I/O
- ALSA `hw:TigerJet,0` (card number varies); 8 kHz S16_LE mono both directions.

### 3. Call state machine

```
         plug detected
               │
           ┌───▼───┐
           │ IDLE  │◄──────────────────────────────┐
           └───┬───┘                               │
               │ off-hook event                    │ on-hook
           ┌───▼───────┐                      ┌────┴────┐
           │  DIALING  │──── call accepted ──►│CONNECTED│
           └───────────┘                      └─────────┘
               │ incoming call
           ┌───▼───────┐
           │  RINGING  │──── off-hook ────────► CONNECTED
           └───────────┘
```

### 4. Tones
- Dial tone (350+440 Hz), ringback, busy, reorder, stutter (voicemail waiting) — generated in software.

### 5. Ring control — see `RING.md`

### 6. udev + systemd
- Stable `/dev/magicjack-hid` symlink and group access; start/stop daemon on plug/unplug.

---

## Implementation Phases

### Phase 1 — Hardware events  *(blocked: waiting on cordless handset charge)*
- [x] Register read path over hidraw (`tools/tjprobe`)
- [ ] `tjprobe watch` across hook transitions — HID input vs register changes
- [ ] Confirm keypad digits from the analog phone are reported (else: software DTMF detection on capture audio)

### Phase 2 — Audio loopback
- [x] Capture works (8 kHz, noise floor ≈ -76 dBFS); full duplex opens with no errors (aplay/arecord)
- [x] `tools/aloop` built and run: all modes, 0 xruns; unity loop self-excites → default loop gain -6 dB
- [ ] Off-hook test: dial tone audible in earpiece; loop = hear yourself (needs powered line + charged handset)
- [ ] Re-check the playback-induced capture bursts (HARDWARE.md) with the line active

### Phase 3 — Line power + ring  *(in progress, `RING.md`)*
- [x] Liberation repo reviewed — not applicable
- [x] ProSLIC-over-SPI hypothesis tested — no SLIC answers on the serial port
- [ ] Find line-control registers (hook diff; TjIpSys type-2 activation sequence)
- [ ] Ring on/off + cadence

### Phase 4 — Rejack daemon (baresip module)  *(skeleton in `rejack/`)*
- [x] Target baresip/libre **v4.12.0** (Ubuntu's 1.0.0 is incompatible); sources in `ext/`
- [x] Call state machine `rejack/src/phone.c` (dial tone, digit collection, `#`/timeout dialing, ringback,
      busy/reorder/howler, incoming ring → answer, 911 block) — unit tested
- [x] Call-progress tones `rejack/src/tones.c`; hidraw access + HID decode `rejack/src/tjdev.c`
- [x] baresip glue `rejack/src/rejack.c` (app module replacing menu.so) — syntax-checked vs v4.12 headers
- [x] libre + baresip v4.12.0 built into `deps/`; rejack.so builds, installs, loads (dev config: `rejack/run/`)
- [x] 911 policy (always attempt) + E911 status-file check (`rejack/src/e911.c`)
- [x] Emergency fallback: EMERGENCY state, local recording (arecord), notify hook
      `rejack/scripts/rejack-emergency` (ntfy/Telegram/Discord/email + offline outbox) — 111 unit checks
- [ ] Provider E911 checker script (once provider chosen; writes `verified <did> <time>`)
- [x] Announcement WAV `rejack/sounds/emergency.wav` (espeak-ng → 8 kHz mono)
- [x] Alert channels chosen: ntfy + Telegram (`rejack/conf/emergency.env.example`)
- [ ] Recording vs call audio share the capture device — use ALSA dsnoop before bridging a contact (Phase 5)
- [ ] udev rule so Rejack can open hidraw without root
- [ ] Tone playback vs call audio both use `hw:TigerJet,0` — confirm tones are stopped before call audio opens
- [ ] Echo canceller (webrtc_aec) on the line path
- [ ] Hook polling fallback if HID input reports don't arrive (HOME repo saw this)

### Phase 5 — Phone server + provider
- [ ] Asterisk with Rejack as an extension
- [ ] VoIP.ms trunk; inbound/outbound routing; E911 or 911 block

### Phase 6 — Packaging
- [ ] udev rules, systemd service, config file

### Phase 7 — Integrations (Switchboard)
- [ ] Home Assistant / MQTT events; softphones; doorbell; AI agent bridge

---

## Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | Name | **Rejack** (integration layer: "Switchboard", nickname *Ansible*) |
| 2 | Language | C (C++ only if needed) |
| 3 | Ring | Hardware ring of the attached phone |
| 4 | Architecture | Home phone server in the middle; Rejack is a SIP extension |
| 5 | VoIP stack | baresip / libre |
| 6 | Event loop | libre epoll loop + real-time ALSA thread |
| 7 | Codec | G.711 μ-law 8 kHz |
| 8 | 911 | Always attempt; notify contacts on every 911; fallback = announce + record + alert |

## Open Questions
1. Phone server: Asterisk (leaning; ARI for integrations) vs FreeSWITCH.
2. Provider: VoIP.ms (leaning) — confirm current pricing at signup.
3. ~~911 policy~~ → decided: always attempt 911; notify contacts; record + alert if it can't go through.
4. Does the A921 detect DTMF from the analog phone on-chip (HID keypad field), or must Rejack decode DTMF from audio?
5. AI agents: what does the OpenClaw deployment expose (API, webhooks, realtime voice)?

---

## Development Environment

- Host: Ubuntu, kernel 6.8.0; device on a USB 2.0 (ehci) port
- ALSA card: TigerJet (was card 2 — look up by name)
- HID node: `/dev/hidraw*` by VID:PID (was hidraw0, now hidraw2)
- Working directory: `/home/user/ReJack/` (example path; scripts resolve their own location)
- Tools: `tools/tjprobe.c` (register/SLIC probe), `tools/fat16x.py` (FAT16 extractor)
- External references: `ext/` (cloned repos), `images/` (device storage images)

---

## Revision History

| Date | Change |
|------|--------|
| 2026-11-16 | Initial creation. Hardware enumeration complete. HID descriptor decoded. No custom driver required. |
| 2026-11-16 | Decided: C/C++, hardware ring. Proposed: epoll + audio thread, G.711 μ-law 8 kHz. |
| 2026-11-16 | Named **Rejack**. Scope defined (MVP landline + 911 policy). Architecture: home phone server + Rejack as baresip-based SIP extension. Service options and integration feasibility recorded. |
