# ReJack

**A 2008 magicJack A921 back as a real home phone on Linux**: a baresip module in C, no kernel driver, no
magicJack account, behind a home Asterisk server.

## What I actually had to prove

The A921 shows up on Linux as a USB sound card plus a HID device. The hard part was the line itself: ringing,
hook state and digits are controlled by registers on the dongle's TigerJet TJ580-class chip, and that register
map isn't published anywhere.

I went in expecting to reverse-engineer the whole chip from scratch, probing registers until the phone reacted.
That turned out not to be required. Two independent pieces of software already drove this chip: magicJack's own
macOS driver and GVJackApp v2, a 2013 third-party Windows app. I recovered the line-control registers from
those binaries (static analysis; the macOS updater still carried its symbols), checked that the two agreed, and
verified each register on the hardware instead of re-deriving it:

- **Ring:** `0x3D` bits 4–5 key the on-board ring generator. ReJack times the 2 s on / 4 s off cadence.
- **Hook:** `0x58` bit 1.
- **Digits:** the chip decodes DTMF itself into `0x15`.

> I went in expecting to reverse-engineer the whole chip from scratch.
> Credit to the people who got there first — I was relieved to be
> verifying prior art instead of starting from zero.

The negative results mattered as much:

- **There is no software line-enable on the TJ580.** Talk battery (34 V on-hook, 14 mA loop) is on from
  power-up. Neither driver has a "line on" call, and a buttset confirmed it.
- **No software DTMF detector is needed.** The chip decodes the digits.

Verification: a TS52 PRO buttset and register-level probes (`tools/tjprobe`) on the real dongle. 158 unit checks
pass, also under ASan/UBSan.

A $9 eBay dongle doing a $50 ATA's job.

## The decisions that matter

**The boundary.** Anything about the copper pair is ReJack's job; anything about where a call goes is
Asterisk's. ReJack is a plain SIP extension, so the provider, voicemail and integrations change without touching
it. Two alternatives were considered and rejected on purpose:
- A custom Asterisk channel driver: it couples the code to Asterisk's internals and versions, and the phone and
  the switch could no longer restart or live on separate machines.
- A commercial one-port ATA such as an HT801: it does the job, but it retires the magicJack, which is the whole
  point.

**911 is owned by the line controller, not the switch.** 911 is always attempted and never silently blocked,
and every 911 call notifies the emergency contacts. If the network or the server is down, the line stays open
with a plain announcement that 911 was *not* reached, the call is recorded, and the contacts get a "call back
now" alert with the recording. The test numbers (911/1911/9911) route to congestion, so the fallback can be
exercised safely. It is covered by unit tests; the end-to-end run against the test congestion is next on the
list.

**Production details, not demo details.**
- systemd service as a non-root user, sandboxed (`NoNewPrivileges`, `ProtectSystem=strict`, one writable
  directory).
- Hot-plug recovery: unplugging mid-call drops the call cleanly, and replugging brings dial tone back without a
  restart. If the dongle stays missing or keeps dropping off, a line-down alert goes to the emergency contacts.
- The chip is restored on exit, so a stopped ReJack leaves an ordinary dial-tone line.
- Secrets are gitignored; only `*.example` templates are committed.
- SIP is ACL'd to the LAN and localhost.

## Why it's documented like this

> Nobody handed me this trade. I learned it the way a lot of us did:
> hours of trial and error, man pages, O'Reilly books, and Usenet
> threads. I know exactly what it costs to dig knowledge out the hard
> way — so the deep notes are all here. The learning guide and the
> hardware notebooks live in docs/, documenting the path that works
> and why it works. If you're evaluating the work, this page and
> ARCHITECTURE.md are the short version. If you're a tinkerer who
> found one of these dongles in a drawer, docs/ is the ladder I
> didn't have. Use it, then leave it better for the next person.

## Status

| Area | State |
|---|---|
| Line card functions (talk battery, ring, hook, DTMF, dial tone) | ✅ Verified on hardware |
| Calls through Asterisk | ✅ Both directions, clean audio |
| Service resilience (boot start, crash restart, hot-plug, chip restore) | ✅ Working |
| Line-down alert to the emergency contacts | ✅ Unit tested, deployed · ⏳ test with a real unplug |
| 911 fallback (announcement, recording, alerts) | ✅ Unit tested · ⏳ end-to-end run next |
| Voicemail on Asterisk, message waiting in softphones | ⏳ Configured, being tested |
| Real phone number through a SIP trunk provider, with E911 | ⏳ Next |
| Caller ID (Bell 202 FSK), then hook flash, then message-waiting stutter dial tone | ⏳ After the trunk, in that order |

## How it works

```
 SIP trunk provider ──┐
                      ├── Asterisk (the switch) ──SIP── ReJack (line controller) ──USB── magicJack ── analog phone
 Softphones ──────────┘                            baresip module, C
```

The magicJack is the line card: battery feed, ring generator, hook sense, DTMF receiver, codec. ReJack is the
line controller: it polls hook and digits every 20 ms, keys the ring cadence, plays dial tone, ringback and busy,
carries the audio, and owns the 911 fallback. Asterisk is the switch. [`ARCHITECTURE.md`](ARCHITECTURE.md) has
the full map: who owns what, a call end to end, and a BORSCHT breakdown.

## Layout

| Path | What's there |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | What runs where, who owns what, a call end to end, BORSCHT, scope and decisions |
| [`RING.md`](RING.md) | Ring and line-control reference: register framing, the driver analysis, hardware results |
| [`INTEGRATIONS.md`](INTEGRATIONS.md) | Spec for the integration layer: Discord voice bridge, phone agent |
| [`CHANGELOG.md`](CHANGELOG.md) | What changed when, and the roadmap |
| [`docs/`](docs/) | The teaching layer: [learning guide](docs/LEARNING_GUIDE.md), [hardware notebook](docs/HARDWARE.md), [project history](docs/PROJECT-HISTORY.md) |
| [`captures/`](captures/) | Raw diagnostic output (`lsusb -v`, sysfs, ALSA listings) the docs refer to |
| [`rejack/`](rejack/) | The baresip module, unit tests, config, emergency notifier. Build and run: [`rejack/README.md`](rejack/README.md) |
| [`pbx/`](pbx/) | Asterisk configuration and installer |
| [`tools/`](tools/) | `rejack` (status, live log, top-style dashboard), `tjprobe` (register probe), `aloop` (audio test), `fat16x.py` (FAT16 extractor) |

## Prior work and thanks

- [Gbrothers1/magicjack-hacking](https://github.com/Gbrothers1/magicjack-hacking) — reverse engineering of the
  later, ARM-based magicJack HOME, plus TigerJet's 2004 `tjctl.c` and chip datasheets.
- [baresip](https://github.com/baresip/baresip) and [libre](https://github.com/baresip/re).

ReJack is not affiliated with magicJack. It only works with hardware you own and services you sign up for.

## License

[MIT](LICENSE)
