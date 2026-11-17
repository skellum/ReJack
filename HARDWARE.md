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

## Unknown / Needs Reverse Engineering

- **Ring control**: how to make the handset ring. Likely a vendor-specific Feature report write (32-byte block via Interface 4 control transfer). Protocol unknown — needs Windows USB traffic capture via `usbmon` + Wireshark.
- **LED state** (if any): unknown.
- **Vendor Feature report format**: the 32-byte feature report on Interface 4 is vendor-defined. Contents unknown.

---

## udev Rule (for consistent device access)

Add to `/etc/udev/rules.d/99-magicjack.rules`:
```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="06e6", ATTRS{idProduct}=="c200", \
  MODE="0660", GROUP="audio", SYMLINK+="magicjack-hid"
```
This creates `/dev/magicjack-hid` as a stable symlink and grants access to users in the `audio` group without requiring root.

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
