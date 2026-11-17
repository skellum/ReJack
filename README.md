# ReJack

**Bringing a 2008 magicJack A921 back to life as a real home phone on Linux.**

The original magicJack was a USB dongle with a phone jack: plug in an ordinary home phone and make calls
through a PC. The service and software are long gone, but the hardware still works. ReJack is an open,
userspace replacement: the dongle plus any analog or cordless phone becomes a SIP phone on a service *you*
choose, with no custom kernel driver and no magicJack account.

## Backstory

While cleaning out the garage I came across my old magicJack, the same one I used to pair with Google Voice
back in the day. That setup is long dead, but the dongle still looked perfectly healthy. I plugged it into a
Linux box just to see what would happen, and it showed up as a sound card and a keypad.

*Hmm... this looks like a tinker-time tech project.*

So here we are: no vendor software, no subscription, just old hardware I already own, an analog phone, and
whatever cheap, legitimate phone service fits.

> **Status: work in progress.** The whole phone line works from Linux with no custom driver: talk battery,
> dial tone, hook, touch-tone digits and **ringing**, all verified with a buttset. Next up: the first real call.

## How it works

```
 SIP provider ──┐
                ├── home phone server ── ReJack ──── magicJack A921 ── analog / cordless phone
 phone apps  ───┘     (Asterisk)    (baresip module)
                          │
                          └── later: home automation, doorbell, notifications, AI agents
```

- The A921 shows up on Linux as a standard **USB audio card** (8 kHz voice in and out) and a **USB HID device**
  (hook switch, keypad, and a vendor report that gives access to the chip's registers).
- **ReJack** is a [baresip](https://github.com/baresip/baresip) module written in C. It turns hook and keypad
  events into calls, plays dial tone, ringback and busy tones, rings the phone, and answers incoming calls.
- A home phone server sits in the middle, so the SIP provider can be swapped and integrations can be added
  without touching the phone software.

## Status

| Area | State |
|---|---|
| USB enumeration, audio (8 kHz full duplex) | ✅ Working with stock Linux drivers |
| Chip register access over HID | ✅ Working (`tools/tjprobe`) |
| Talk battery (34 V, 14 mA loop) and dial tone | ✅ Built into the dongle, on from power-up |
| Hook, touch-tone digits, **ringing** | ✅ Verified on hardware — see [`RING.md`](RING.md) |
| ReJack module (call logic, tones, 911 handling) | ✅ Builds against baresip 4.12, 139 unit checks pass |
| First SIP call | ⏳ Next |
| Phone server, provider, packaging | ⏳ Not started |

## 911

A phone that looks like a landline will eventually be used in an emergency, so ReJack treats 911 as a
first-class feature:

- **911 is always attempted.** It is never silently blocked.
- **Every 911 call notifies your emergency contacts** (ntfy push and Telegram supported), including whether the
  provider has a registered address for the line.
- **If the call can't go through** (no internet, server down, call fails), the line stays open: an announcement
  says plainly that 911 was *not* reached and to use a mobile phone, the line is recorded, and contacts get a
  "call back now" alert with the recording.

Register your address with your provider's E911 service before relying on this phone.

## Repository layout

| Path | What's there |
|---|---|
| [`PROJECT.md`](PROJECT.md) | Scope, architecture, decisions, phase checklists |
| [`HARDWARE.md`](HARDWARE.md) | Device reference: USB interfaces, audio, HID layout, test results, udev rule |
| [`RING.md`](RING.md) | Ring and line-power reverse-engineering notes |
| [`LEARNING_GUIDE.md`](LEARNING_GUIDE.md) | Walkthrough of the diagnostic output, written to teach |
| [`rejack/`](rejack/) | The baresip module, tests, config, emergency notification script |
| [`tools/`](tools/) | `tjprobe` (register probe), `aloop` (audio test), `fat16x.py` (FAT16 extractor) |

## Getting started

Requirements: Linux, a magicJack A921 (USB ID `06e6:c200`) on a **USB 2.0** port, an analog or cordless phone,
`cmake`, `libasound2-dev`, `libssl-dev`, `zlib1g-dev`.

1. Give your user access to the dongle (members of the `audio` group):
   ```
   R='SUBSYSTEM=="hidraw", ATTRS{idVendor}=="06e6", ATTRS{idProduct}=="c200", MODE="0660", GROUP="audio", SYMLINK+="magicjack-hid"'
   echo "$R" | sudo tee /etc/udev/rules.d/99-magicjack.rules
   sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=hidraw
   ```
2. Try the tools:
   ```
   gcc -O2 -o tools/tjprobe tools/tjprobe.c && tools/tjprobe regs
   gcc -O2 -o tools/aloop tools/aloop.c -lasound -lm && tools/aloop levels 3
   ```
3. Build baresip and ReJack: see [`rejack/README.md`](rejack/README.md).

## Prior work and thanks

- [Gbrothers1/magicjack-hacking](https://github.com/Gbrothers1/magicjack-hacking) — reverse engineering of the
  later, ARM-based magicJack HOME, plus TigerJet's 2004 `tjctl.c` and chip datasheets.
- [baresip](https://github.com/baresip/baresip) and [libre](https://github.com/baresip/re).

ReJack is not affiliated with magicJack. It only works with hardware you own and services you sign up for.

## License

[MIT](LICENSE)
