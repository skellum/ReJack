# Architecture

How ReJack, the magicJack, Asterisk and the softphones fit together, what each one is responsible for, and what is
still missing. In the current setup everything runs on one Linux machine. That isn't required (see
[Placement](#placement)).

```
 Linphone (cell)    ──SIP──┐
                           ├── Asterisk (the switch) ──SIP── ReJack (line controller) ──USB── magicJack ── 2-wire ── analog phone
 Linphone (desktop) ──SIP──┘        │
                                    └── later: SIP trunk provider (real phone number), integrations
```

**Rule of thumb: anything about the copper pair is ReJack's job; anything about where a call goes is Asterisk's.**

## The pieces

| Piece | What it is | How it runs | Config | Talks on |
|---|---|---|---|---|
| **magicJack A921** | The line card hardware: battery feed, ringing, hook sense, DTMF receiver, codec | USB dongle | None; ReJack sets its registers | USB: hidraw `/dev/magicjack-hid` (control), ALSA `hw:TigerJet,0` (audio) |
| **ReJack** | Line controller plus phone logic: a **baresip** module (`rejack.so`) | systemd `rejack.service` (installed by `sudo rejack/systemd/install.sh`), as a normal user in the `audio` group; restarts on crash | `rejack/run/config` (generated from `config.in`), `rejack/run/accounts` (gitignored) | SIP `127.0.0.1:5070`, localhost only |
| **Asterisk** | The switch: registrations, dial plan, call routing | System service (`asterisk` user), starts at boot | `/etc/asterisk/`, installed from [`pbx/`](pbx/) by `sudo pbx/install.sh` | SIP UDP 5060, RTP UDP 10000–10200; LAN and localhost only |
| **Softphones** | Ordinary SIP phones (Linphone on a cell = ext 101, on a PC = ext 102) | Apps on your devices | In each app | Register with Asterisk over the LAN |

## Who does what

| Function | Owner |
|---|---|
| Talk battery, ring voltage, hook detection, DTMF decoding, A/D conversion | magicJack |
| Dial tone, digit collection, end of dialling (pause or `#`) | ReJack |
| Ring cadence, answer on off-hook, hang up on on-hook | ReJack |
| Ringback and busy heard on the analog phone | ReJack |
| 911 policy and fallback (announcement, recording, alerts), even when Asterisk or the network is down | ReJack |
| Knowing where each extension is (registrations) | Asterisk |
| What a dialled number means (dial plan: 101, 600, 911, …) | Asterisk |
| Relaying call audio between phones (`direct_media=no`, so all audio passes through Asterisk) | Asterisk |
| Call logs | Asterisk (`/var/log/asterisk/cdr-csv/`) |
| Voicemail and message waiting | Asterisk |
| Later: phone number (trunk), ring groups, integrations ([`INTEGRATIONS.md`](INTEGRATIONS.md)) | Asterisk + Switchboard |

## For telephone people: a line card in software

The magicJack is the line card plus its own ringing generator. ReJack is the line drawer controller, passing
supervision up to the switch and ringing commands down to the line, plus a few switch functions pushed down to the line.
Asterisk is the switch core. Mapped onto BORSCHT:

| BORSCHT | Where it lives |
|---|---|
| **B**attery feed | magicJack: on-board booster, always on (34 V on-hook, about 14 mA loop) |
| **O**vervoltage protection | magicJack hardware (minimal) |
| **R**inging | Generated per line on the magicJack (booster plus H-bridge, square wave); ReJack keys it (reg `0x3D`) and times the 2 s on / 4 s off cadence |
| **S**upervision | magicJack senses the loop; ReJack reads it (reg `0x58`), debounces it and reports off-hook/on-hook to the switch |
| **C**odec | magicJack chip: G.711 at 8 kHz over USB audio; ReJack carries it to and from SIP |
| **H**ybrid (2-to-4 wire) | magicJack hardware |
| **T**esting | `tools/tjprobe` (register-level probe) |

Unlike a central-office line card, ReJack also collects digits (the chip decodes them into reg `0x15`), supplies
dial tone, ringback and busy, and owns the 911 fallback. Register details: [`docs/HARDWARE.md`](docs/HARDWARE.md) and [`RING.md`](RING.md).

## One call end to end: the analog phone dials 101

1. The phone goes off-hook. The magicJack sets reg `0x58`; ReJack sees it within about 60 ms and turns on dial tone.
2. Digits `1` `0` `1`: the chip decodes each into reg `0x15`. ReJack collects them and stops dial tone on the
   first.
3. After a 4-second pause, or `#`, ReJack (extension 100) sends Asterisk a SIP INVITE for `101`.
4. Asterisk looks `101` up in the dial plan, finds the softphone's current registration and rings it. It answers
   ReJack with `180 Ringing`, and ReJack plays ringback to the analog phone.
5. The softphone answers. ReJack stops ringback and opens the USB audio. Voice flows: analog phone ⇄ magicJack ⇄
   ReJack ⇄ Asterisk ⇄ softphone, in G.711 μ-law.
6. Either side hangs up. ReJack ends the SIP call; on-hook returns it to idle.

An incoming call is the mirror image: Asterisk sends ReJack an INVITE, ReJack rings the phone, and off-hook answers.

## Status

| Area | State |
|---|---|
| Line card, switch, calls in both directions, clean audio | ✅ Working |
| ReJack as a service: start at boot, auto-restart, restore the line after a crash; survives unplug/replug | ✅ Working |
| Line-down alert: dongle missing or dropping off for 2 min → emergency contacts notified; "restored" when it's back | ✅ Working |
| 911 fallback (announcement, recording, alerts) | ✅ Unit tested; ⏳ end-to-end run against the test congestion |
| Unreachable extension: voicemail (Asterisk `VoiceMail`, message waiting via `mailboxes=`) | ⏳ Configured, being tested |
| Phone number and outside calls: SIP trunk provider in Asterisk, plus E911 | ⏳ Missing |
| Caller ID to the analog phone (Bell 202 FSK between the first and second ring, generated by ReJack) | ⏳ Missing |
| Hook flash (call waiting, transfer): ReJack tells a flash from a hang-up; Asterisk provides the features | ⏳ Missing |
| Message-waiting indication (stutter dial tone when there's voicemail) | ⏳ Missing |
| Echo cancellation | Not needed so far; revisit with a real trunk |

## Scope

**The target:** the analog phone behaves like an ordinary landline.
1. Incoming call: the phone rings, you pick up, two-way audio.
2. Outgoing call: dial tone, dial digits, the call goes out.
3. Hang-up from either side; runs unattended as a service.
4. 911 handled deliberately (below).

**Not in scope:** magicJack's own service; modifying the dongle's firmware; fax/modem; any service used against
its terms.

## 911 policy

The phone looks like a landline, so it will eventually be used in an emergency.
- `rejack_911 allow` (default): 911/1911/9911 are **always dialed**. The provider may route an unverified call
  to a relay center; that is better than a dead line. The E911 address status (from a provider checker's status
  file) is reported, not used to block. `verified` and `block` exist as policies; the fallback still runs.
- **Every 911 notifies the emergency contacts** (ntfy, Telegram, Discord or email; queued and retried if offline),
  including whether the provider has a registered address.
- **If 911 can't go through** (no network, phone server down, call fails): the line stays open, an announcement
  says 911 was **not** reached and to use a mobile phone, the line is recorded locally, and the contacts get a
  "call back now" alert; the recording follows on hang-up.
- Planned with the trunk: bridge an emergency contact into a failed 911 call, and record on the server.

Register your address with the provider's E911 service before relying on this phone.

## Decisions

| Topic | Decision | Why |
|---|---|---|
| Architecture | Phone server in the middle; ReJack is a SIP extension | Provider, voicemail and integrations change without touching ReJack |
| Language | C | Small daemon close to the hardware; baresip and libre are C |
| VoIP stack | baresip / libre **v4.12.0** | ReJack is an application module; Ubuntu's baresip 1.0.0 has an incompatible module API |
| Phone server | Asterisk 20 | ARI for integrations; packaged by the distribution |
| Line state | Register polling every 20 ms, hook debounced over 3 polls | The HID input report's keypad field is for TigerJet's USB handsets, not the analog line |
| Dial tone | The chip's tone generator; ReJack switches off the chip's automatic dial tone while running | ReJack decides when dial tone plays; after any stop the service hands the chip's automatic tone back (`tjprobe release`) |
| Ring | Hardware ring generator keyed by ReJack, which owns the cadence | The generator has no cadence of its own |
| Codec | G.711 μ-law, 8 kHz | Narrowband analog line, 8 kHz capture, native to the PSTN |
| 911 | Always attempt; notify on every 911; fallback = announce, record, alert | See [911 policy](#911-policy) |

## Planned: one line per room

A used 4-line DECT phone system (VTech CM18445: a corded console plus cordless handsets) on four A921 dongles,
one dongle per line, gives four extensions with their own numbers in Asterisk. The console sits next to the
server and is the DECT base for the handsets.
- Extensions per room, a ring group that rings two extensions together (first to answer takes the call), a small
  conference for both, and a whole-house ring group for the main number once there is a trunk.
- Rules per extension (quiet hours, outside-call allowlist) are dialplan.
- ReJack work: one instance per dongle (`rejack@.service`), devices named by USB port, configurable HID path and
  ALSA card.
- First step: a compatibility test of the console on the current dongle. Does it accept the A921's 34 V on-hook
  battery (lower than the usual ~48 V), detect ring, and ring a station only on its own line?
- Constraints: a powered USB 2.0 hub for four dongles ringing at once, on the high-speed controllers, kept off
  the controller that bulk storage uses (a failing drive on the same controller took the dongle down once).

## Open questions

1. Which SIP trunk provider, at what price (confirm at signup).
2. Does the CM18445 work on the A921's 34 V line?
3. Ring voltage under load (not yet measured; a published bulletin for the first-generation dongle gives about
   85 V square wave).

## Placement

ReJack and Asterisk only need to reach each other over SIP. Running both on the machine the dongle is plugged into
is the simplest choice for one home line. Asterisk can move to another server later, with ReJack staying next to
the phone, for example on a Raspberry Pi. That needs one config change: ReJack's account points at the server, and
baresip listens on the LAN instead of localhost.

## Why not drop ReJack?

- **Asterisk can't drive the dongle by itself.** It speaks SIP to network devices; the magicJack is a USB gadget
  that needs its hook, digits, ringing and tones handled register by register.
- **A custom Asterisk channel driver** could replace ReJack, but it would tie the code to Asterisk's internals and
  versions, and the phone and switch could no longer restart or live separately.
- **A one-port commercial ATA** (for example a Grandstream HT801) does all of this in a box. It would make ReJack
  unnecessary, but it would also retire the magicJack, which is the whole point of this project.
