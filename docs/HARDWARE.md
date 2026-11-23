# magicJack A921 — Hardware Reference

Use this document to restore context in future sessions without repeating discovery steps.

---

## Device Identity

| Field | Value |
|-------|-------|
| Product name | magicJack A921 |
| OEM chip vendor | TigerJet Network, Inc. |
| USB VID:PID | `06e6:c200` |
| USB speed | High-speed (USB 2.0, requires ehci-pci) |
| Bus power draw | 500 mA |
| Firmware version | 0.01 |
| Serial number | A921XXXXXXXXXX (masked) |
| dmesg product string | "USB Internet Phone by TigerJet" |

**Critical**: device fails to enumerate on USB 1.1 (uhci_hcd) ports. Must use a USB 2.0 (ehci-pci) port.

---

## Interface Map

| # | Class | Function | Linux Driver | Notes |
|---|-------|----------|--------------|-------|
| 0 | Mass Storage (SCSI Bulk-Only) | Windows softphone app flash | `usb-storage` | Not needed for Linux VoIP |
| 1 | Audio Control (ADC 1.00) | Audio topology / volume control | `snd-usb-audio` | ALSA card 2 |
| 2 | Audio Streaming IN | Handset mic → host | `snd-usb-audio` | 8 kHz only |
| 3 | Audio Streaming OUT | Host → handset speaker | `snd-usb-audio` | 8 kHz or 16 kHz |
| 4 | HID | Hook switch, DTMF, functions | `usbhid` | `/dev/hidraw*` |

---

## Audio Specification

### Capture (handset microphone → host)
- Interface 2, Alt Setting 1
- Endpoint: `0x83` EP 3 IN (Isochronous)
- Format: PCM, 16-bit signed, mono, **8000 Hz**
- Packet size: 16 bytes @ 1 ms interval
- ALSA address: `hw:2,0`

### Playback (host → handset speaker)
- Interface 3
- Alt Setting 1: PCM, 16-bit signed, mono, **16000 Hz**, EP 4 OUT, 32 bytes/packet
- Alt Setting 2: PCM, 16-bit signed, mono, **8000 Hz**, EP 4 OUT, 16 bytes/packet
- ALSA address: `hw:2,0`

### Quick audio test
```bash
# Record 5s from handset mic
arecord -D hw:2,0 -f S16_LE -r 8000 -c 1 -d 5 /tmp/test.wav

# Play back through handset speaker
aplay -D hw:2,0 /tmp/test.wav
```

---

## HID Event Interface

- Interface 4, Endpoint `0x85` EP 5 IN (Interrupt, 8-byte max, interval 7)
- Input report: **2 bytes** (16 bits packed, LSB-first)
- Feature report: 32 bytes (vendor-defined, protocol unknown)
- Device node: `/dev/hidraw*` — number varies (was hidraw0, seen as **hidraw2** on 2026-11-16). Find it via `HID_ID=0003:000006E6:0000C200` in `/sys/class/hidraw/hidraw*/device/uevent`, or use the udev symlink below.
- Report descriptor has **no report IDs** and **no Output report** — the only host→device HID path is the 32-byte Feature report (ID 0).

### Input Report Bit Layout

```
Byte 0:
  [3:0]  DTMF keypad  — 0=none, 1=0, 2=1, 3=2, 4=3, 5=4, 6=5, 7=6,
                         8=7, 9=8, 10=9, 11=*, 12=#
  [6:4]  Phone func   — 0=none, 1=Transfer, 2=Hold, 3=Redial,
                         4=Drop, 5=Flash, 6=PhoneMute, 7=Send
  [7]    keyboard[0]  — low bit of keyboard key field (see below)

Byte 1:
  [1:0]  keyboard[2:1] — high 2 bits of keyboard key
  [3:2]  Volume        — 2-bit signed relative: 0x3=-1(down), 0=none, 1=up
  [6:4]  Button        — 0=none, 1-4=button number
  [7]    Hook switch   — 0=on-hook (hung up), 1=off-hook (lifted)
```

### Keyboard field values (bits [9:7] across byte boundary)
0=none, 1=Home, 2=PageUp, 3=PageDown, 4=Down, 5=Up, 6=Enter, 7=Backspace

### Reading in Python
```python
import os

fd = os.open('/dev/hidraw0', os.O_RDONLY)
while True:
    data = os.read(fd, 8)
    hook    = bool(data[1] & 0x80)
    dtmf    = data[0] & 0x0F
    fn_key  = (data[0] >> 4) & 0x07
    volume  = (data[1] >> 2) & 0x03   # interpret as signed: 3 = -1
    button  = (data[1] >> 4) & 0x07
    print(f"hook={hook} dtmf={dtmf} fn={fn_key} vol={volume} btn={button}")
```

### Reading in C
```c
#include <fcntl.h>
#include <unistd.h>

int fd = open("/dev/hidraw0", O_RDONLY);
unsigned char buf[8];
while (read(fd, buf, 8) == 8) {
    int hook  = (buf[1] >> 7) & 1;
    int dtmf  = buf[0] & 0x0F;
    int fnkey = (buf[0] >> 4) & 0x07;
}
```

---

## Linux Kernel Bindings (confirmed)

```
/sys/bus/usb/devices/2-2        → device root (Bus 2, Port 2)
/sys/bus/usb/devices/2-2:1.0   → usb-storage  (mass storage)
/sys/bus/usb/devices/2-2:1.1   → snd-usb-audio (audio control → sound card)
/sys/bus/usb/devices/2-2:1.2   → snd-usb-audio (audio stream IN)
/sys/bus/usb/devices/2-2:1.3   → snd-usb-audio (audio stream OUT)
/sys/bus/usb/devices/2-2:1.4   → usbhid        (HID events)
  HID node: 0003:06E6:C200.0005
```

---

## Onboard Storage & Windows Software (found 2026-11-16)

Interface 0 exposes two LUNs:
- `/dev/sr0` — 10 MB ISO9660 "MAGICJACK" (readable by `cdrom` group). Contains only `AUTORUN.EXE/.INF/.ICO`. AUTORUN.EXE is an NSIS 2.27 installer (v1.80.4263.2980, 2007); image saved as `images/sr0-magicjack.iso`, NSIS contents extracted to `images/nsis/`.
- `/dev/sdb` — 17.8 MB vfat "PHONE" (root/disk only). Believed to hold the actual softphone (SJphone-based, by SJ Labs) that the installer launches. **Not yet imaged.**

Findings from the installer DLLs:
- `nsMagicJack2.dll` exports `GetAnyConnectedDeviceTJ580` → controller chip is likely a **TigerJet TJ580** (not the TJ3201 an early guess suggested).
- `magicJack.dll` only enumerates the device and checks USB hub power status (DeviceNotEnoughPower, DeviceCausedOvercurrent); it contains no ring code.
- The softphone is built on **SJphone** (PDB paths `...\SJphone_branch\SJphone\Customers\...`).

## Phone line control (verified 2026-11-17 with a TS52 PRO buttset)

The 32-byte vendor feature report gives TigerJet register access (`04 <reg> <reg&0xE0> <n> <data>`; read: select
bank `04 BB BB 00`, then GET). The analog line needs **no initialisation**:

| Function | Register | Notes |
|---|---|---|
| Talk battery | — | Always on: **34 V** on-hook, **14 mA** loop (cordless bases may still show "No Line") |
| Automatic dial tone | `0x34` bit 5 | Power-up value `0x26`; tone on off-hook, cut on first digit |
| Dial tone on command | `0x34` bit 6 | `|= 0x40` on, `&= 0x1F` all tone off |
| Ring | `0x3D` bits 4+5 | `|= 0x30` ring, `&= 0xCF` stop; host does the 2 s / 4 s cadence |
| Hook | `0x58` bit 1 | `0x03` off-hook, `0x01` on-hook; also HID report byte `0x09` bit 7 |
| DTMF decoder | `0x15` | low nibble = last digit: 1–9, `A`=0, `B`=*, `C`=#; a change means a new digit. Bit 6 = "digit present" strobe: held with the TS52, a ~20 ms pulse or absent with a VTech handset; so a repeated key leaves no trace. Ignores writes. ReJack decodes DTMF from the audio instead |

Full derivation (magicJack and GVJack drivers, test logs) in [`RING.md`](../RING.md). Still unknown: ring voltage (Sandman's V1:
~85 V square wave), LED control.

---

## udev Rule (device access and line levels)

[`rejack/systemd/99-magicjack.rules`](../rejack/systemd/99-magicjack.rules), installed to `/etc/udev/rules.d/` by
`sudo rejack/systemd/install.sh`:
- creates `/dev/magicjack-hid` as a stable symlink and grants access to the `audio` group, so no root is needed;
- sets the line levels on every plug-in: `Headset Capture Volume` 15 and `PCM Playback Volume` 8 (the card powers
  up at 8 and 0). At the power-up levels, speech from the phone was so quiet that voicemail recorded it as silence
  and the echo test needed shouting (2026-11-22).
- switches off the dongle's mass-storage interface (the 18 MB vendor flash). While it was bound, usb-storage
  error handling reset the whole dongle three times in one night (2026-11-23), breaking the audio stream.

## Audio Test Results (2026-11-16, phone on-hook, line state unknown)

- Capture `hw:TigerJet,0` 8 kHz works; idle noise floor peak ±5 counts (≈ -76 dBFS) — codec is live.
- Full duplex (capture + playback simultaneously at 8 kHz) opens and runs; no kernel/USB errors.
- Mixer: `PCM Playback Volume` 0..15 (0 dB..+15 dB, currently 0), `Headset Capture Volume` 0..15 (currently 8).
- Both endpoints report sync type NONE (no feedback endpoint) — expect slow clock drift between host and
  device on long calls; Rejack's audio path must tolerate it (small adaptive buffer / occasional drop/insert).
- **Anomaly:** while a non-silent signal is played, capture shows sporadic ~7.4 ms bursts (peak ≈ -1 dBFS)
  with an *identical* waveform each time (dominant ≈ 3.6 kHz), at irregular 50–450 ms intervals. Playing
  digital silence → none; capture alone → none. Not a byte-shift/byte-swap/μ-law misread of the tone.
  Re-test once the line is powered and the phone is off-hook before drawing conclusions.
- `tools/aloop` (2026-11-16): 10 ms periods full duplex, zero xruns in all modes. Idle capture ≈ -94 dBFS RMS.
  Steady 1 kHz tone at -12 dBFS → capture ≈ -60 dBFS peak (≈ -48 dB return) plus occasional bursts.
  **Loop at unity gain self-excites** (noise builds, bursts reach -17 dBFS); at -6 dB it stays at the noise
  floor. Rejack needs an echo canceller / gain margin on the line path (baresip has AEC modules).
- SCHED_FIFO denied for the normal user — grant via `/etc/security/limits.d` rtprio for the audio group
  or a systemd `LimitRTPRIO=` when packaging.

## Board (component side photographed 2026-11-16; photo in `images/board/`, not committed)

No ProSLIC or other SLIC IC on this side — the phone line is driven by **discrete parts**:

| Ref | Marking | Identification | Likely role |
|---|---|---|---|
| U2 | "Magic Chip 0811 P4MR9-010", 100-pin QFP | Custom-marked TigerJet (2008 wk 11) | USB, audio, HID, GPIO |
| Y1 | JL12.000 | 12.000 MHz crystal | TigerJet clock |
| U5 | Sharp "814A", L0808 | PC814A AC-input optocoupler | Loop-current (hook) sense, both polarities |
| Q4, Q5 | "2L" (SOT-23) | MMBT5401 (150 V PNP) | High-voltage line switching (polarity / ring) |
| Q6 | "82TAF H5551" (SOT-223) | 5551-class high-voltage NPN (160 V) | Boost switch or line feed |
| L4 | "101" shielded | 100 µH power inductor | Boost converter inductor |
| D1 | glass MELF, orange | Diode/zener | Boost rectifier or voltage clamp |
| C38 | large SMD electrolytic | Bulk capacitor | High-voltage (≈50 V) reservoir |
| C39 | — | **Empty footprint** (polarised electrolytic outline, tinned pads) | Second reservoir cap, unpopulated (or lost?) |
| D3 | clear SMD LED | Blue LED | Power/status indicator (solid) |
| J1 / J2 | | USB-A plug / RJ11 jack | |

### Back side — silkscreen "magicjack.com PhoneJack A921 Rev3.0 V1.0"

| Ref | Marking | Identification | Likely role |
|---|---|---|---|
| (flash) | Samsung K9F5608U0D PCB0 (TSOP-48) | 256 Mbit = 32 MB NAND flash | The CD-ROM + "PHONE" storage (10 + 17.8 MB) |
| **Q7** | unreadable, DPAK/TO-252 | Power MOSFET (by package + placement) | **Boost converter switch** (L4 on the other side) |
| **D8** | ES1D MIC | 1 A / 200 V super-fast rectifier | **Boost converter output diode** |
| R101 | R33 (2512) | 0.33 Ω | Current sense (boost switch source?) |
| (SOT-23) | "1P" | MMBT2222A small NPN | Gate driver / level shift for Q7, or regulation loop |
| (SOT-23) | "H" | small diode/zener (unconfirmed) | Bias / clamp |
| (SOT-223) | unmarked | probably 3.3 V LDO | TigerJet core supply |
| **Q3** | 82TAF H5551 (SOT-223) | 5551-class 160 V NPN | Line driver, low side |
| **Q1, Q2** | "2L" | MMBT5401 150 V PNP | Line driver, high side |
| D4–D7 | 4× glass MELF | Diode bridge | Polarity guard (loop sense / line feed) |
| D2, D11 | glass MELF | Diodes / zeners | Clamp / USB power path |

### Line circuit as built (inference from both sides, not yet traced)
- **Boost converter**: USB 5 V → L4 (100 µH) → Q7 MOSFET switch → D8 (ES1D) → C38 (+ empty C39) ≈ 50 V talk battery.
  Gate drive via a 2222-class NPN. **No PWM/boost controller IC anywhere** → the switching signal must come from
  the TigerJet (clock divider A/B routed to GIO8/9/10 is the obvious candidate) and regulation is either a clamp
  (D1 zener?) or firmware bang-bang on a sense input.
- **Line driver**: 2 × 5551 NPN (Q3, Q6) + 4 × 5401 PNP (Q1, Q2, Q4, Q5) → an H-bridge feeding tip/ring.
  Toggling the bridge ≈ 20 Hz from the boosted rail = the **square-wave ring** Sandman measured (85 V p-p class).
- **Hook sense**: PC814 AC-input opto in the loop (with D4–D7 bridge) → a TigerJet GIO/AUX input.
- **Hazards**: a GIO that turns Q7 on and stays on shorts USB 5 V through L4; an unclamped boost on a light load
  can over-volt C38. Only pulsed / divider-driven experiments, with a meter on C38, and only after tracing.

### Tiger560B pinout (if the "Magic Chip" is pin-compatible — unverified; it's a USB 2.0 part, the 560B is 1.1)
GIO0 p2, GIO1 p3, GIO2 p4, GIO3 p6, GIO4 p7, GIO5 p8, GIO6 p9, GIO7 p12, **GIO8 p13, GIO9 p14, GIO10 p16**,
GIO11 p17, GIO12 p18, GIO13 p19, GIO14 p22, EXTRST# p70, AUX0–4 p76–80. Pin 1 = WAKEUP (dot corner).

Implications:
- Explains the dead SPI port: there is no ProSLIC to answer.
- A boost converter (L4/D1/C38 + switch) with **no visible controller IC** means something must switch it —
  possibly a TigerJet GPIO or clock-divider output (GIO8–10 can output divider A/B square waves).
- Square-wave ring (Sandman) fits an H-bridge of HV transistors (5401/5551) toggled at ~20 Hz.
- **Safety:** never hold an unknown GPIO output static high — if it drives the boost switch, the 100 µH inductor
  shorts USB 5 V to ground through the transistor. An unregulated boost on a light load can also over-volt C38.
  No GPIO output experiments until the switch/feedback wiring is understood.
