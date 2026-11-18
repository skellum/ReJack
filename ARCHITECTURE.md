# Architecture

How ReJack, the magicJack, Asterisk and the softphones fit together, what each one is responsible for, and what is
still missing. In the current setup everything runs on one Linux machine. That isn't required (see
[Placement](#placement)).

```
 Linphone (cell)    ──SIP──┐
                           ├── Asterisk (the switch) ──SIP── ReJack (line controller) ──USB── magicJack ── 2-wire ── analog phone
 Linphone (desktop) ──SIP──┘        │
                                    └── later: SIP provider trunk (real phone number), voicemail, integrations
```

**Rule of thumb: anything about the copper pair is ReJack's job; anything about where a call goes is Asterisk's.**

## The pieces

| Piece | What it is | How it runs | Config | Talks on |
|---|---|---|---|---|
| **magicJack A921** | The line card hardware: battery feed, ringing, hook sense, DTMF receiver, codec | USB dongle | None; ReJack sets its registers | USB: hidraw `/dev/magicjack-hid` (control), ALSA `hw:TigerJet,0` (audio) |
| **ReJack** | Line controller plus phone logic: a module (`rejack.so`) loaded into **baresip**, a small SIP phone program | `rejack/run/start.sh` for now, as a normal user (systemd service planned) | `rejack/run/config` (generated from `config.in`), `rejack/run/accounts` (gitignored) | SIP `127.0.0.1:5070`, localhost only |
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
| Later: phone number (trunk), voicemail, ring groups, integrations (doorbell, chat, AI agents) | Asterisk |

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
dial tone, ringback and busy, and owns the 911 fallback. Register details: [`HARDWARE.md`](HARDWARE.md).

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
| ReJack as a service: start at boot, auto-restart, restore the line after a crash | ⏳ Next (systemd) |
| Unreachable extension: voicemail or an "unavailable" announcement | ⏳ Missing (currently a bare 404) |
| Phone number and outside calls: provider trunk in Asterisk, plus E911 | ⏳ Missing |
| Caller ID to the analog phone (Bell 202 FSK between the first and second ring, generated by ReJack) | ⏳ Missing |
| Hook flash (call waiting, transfer): ReJack tells a flash from a hang-up; Asterisk provides the features | ⏳ Missing |
| Message-waiting indication (stutter dial tone when there's voicemail) | ⏳ Missing |
| Echo cancellation | Not needed so far; revisit with a real trunk |

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
