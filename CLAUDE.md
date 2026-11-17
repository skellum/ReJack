# Rejack — magicJack A921 revival

## What this directory is
**Rejack**: reverse engineering and userspace software (C, no custom kernel driver) that turns a magicJack A921 USB dongle plus an analog/cordless phone into a home phone line on a legitimate SIP service. Architecture: home phone server (Asterisk) in the middle; Rejack is a baresip-based SIP extension. Integration layer ("Switchboard", nickname Ansible) comes later. See `PROJECT.md` for scope and decisions.

## Read these first in a new session
- `HARDWARE.md` — device identity, interface map, audio specs, HID bit layout, udev rule
- `PROJECT.md` — architecture draft, component breakdown, implementation phases, open questions
- `LEARNING_GUIDE.md` — full educational walkthrough of all diagnostic output (reference only)
- `RING.md` — ring-control research: hypothesised HID→ProSLIC protocol, sources, test plan

## Key facts
- Device: TigerJet Network / magicJack A921, USB VID:PID `06e6:c200`
- Requires USB 2.0 port (ehci-pci) — fails on USB 1.1 (uhci_hcd)
- ALSA card 2 (`hw:2,0`): capture 8kHz mono, playback 8kHz or 16kHz mono
- HID events on `/dev/hidraw*` (number varies, look up by VID:PID): 2-byte report, bit 15 = hook switch, bits 0-3 = DTMF
- All 5 USB interfaces bind to standard Linux drivers — no custom kernel driver needed
- Ring control: hypothesis in `RING.md` (ProSLIC linefeed reg 64 = 0x44 via TigerJet SPI bridge), untested

## Current status (as of 2026-11-16)
Hardware enumerated; TigerJet register access over the HID feature report works (`tools/tjprobe`). ProSLIC-over-SPI ring hypothesis tested negative (see `RING.md`). Audio path verified (`tools/aloop`). Rejack baresip module skeleton in `rejack/` (core unit-tested; glue not yet built — needs baresip v4.12 built into `deps/`). Hardware next step: `tjprobe watch` across hook transitions once the cordless handset is charged.

## Raw diagnostic output files
- `../NOTES.md` (parent of the project directory, not in the repo) — original dmesg from failed USB 1.1 enumeration
- `LSUSB.out` — full `lsusb -v` descriptor dump
- `DEVICES.out` — sysfs interface/driver binding listing
- `APLAY.out` / `ARECORD.out` — ALSA device listings confirming card 2
