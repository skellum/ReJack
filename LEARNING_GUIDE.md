# magicJack A921 — What We Know: A Learning Guide

This document walks through every piece of diagnostic output collected during the initial hardware investigation, explaining what each piece means and why it matters.

---

## 1. The First Failure — Reading `dmesg` After Plug-in

### What is `dmesg`?
`dmesg` is the kernel ring buffer — a running log of everything the Linux kernel reports, including hardware events. When you plug in a USB device, the kernel immediately tries to enumerate it and logs every step.

### What we saw (the bad port)

```
[4572414.085359] usb 4-2: new full-speed USB device number 2 using uhci_hcd
[4572425.127436] usb 4-2: device descriptor read/64, error -110
[4572440.998553] usb 4-2: device descriptor read/64, error -110
...
[4572478.979893] usb 4-2: device not accepting address 4, error -110
[4572484.819858] usb 4-2: device not accepting address 5, error -32
```

### Decoding each line

**`usb 4-2`** — This is the kernel's name for the port. The first number (`4`) is the USB bus controller number. The second (`2`) is the port on that bus. So "Bus 4, Port 2."

**`new full-speed USB device`** — "Full-speed" in USB terminology means USB 1.1 speed (12 Mbps). This is a clue: the port the device was plugged into operates at USB 1.1 speeds only.

**`using uhci_hcd`** — `uhci_hcd` is the kernel driver for the Universal Host Controller Interface — the USB 1.1 host controller standard used by Intel chips. This confirms the port is USB 1.1 only.

**`device descriptor read/64, error -110`** — When a USB device is plugged in, the host controller must first read a 64-byte "device descriptor" — a small block of data that identifies the device (manufacturer, product ID, etc.). Error `-110` is `ETIMEDOUT`: the device never responded. The kernel tried twice (you can see two of these lines), waited about 15 seconds each time, and got nothing.

**`device not accepting address 4, error -110`** — After reading the descriptor, the host assigns the device a unique address on the USB bus. This also timed out.

**`device not accepting address 5, error -32`** — Error `-32` is `EPIPE`, meaning a "pipe stall" — the device responded but then rejected the control transfer. This happens when the device is partially alive but something is wrong with the connection (usually power or signal quality on the wrong port type).

### Why did this happen?
The magicJack A921 is a USB 2.0 device. While it is physically backward-compatible with USB 1.1, the USB 1.1 `uhci_hcd` controller didn't provide reliable enough power or signal to complete enumeration. The fix was to plug it into a USB 2.0 port.

---

## 2. Successful Enumeration — The Good `dmesg` Output

```
[Mon Nov 16 17:38:24 2026] usb 2-2: new high-speed USB device number 11 using ehci-pci
[Mon Nov 16 17:38:24 2026] usb 2-2: New USB device found, idVendor=06e6, idProduct=c200, bcdDevice= 0.01
[Mon Nov 16 17:38:24 2026] usb 2-2: Product: USB Internet Phone by TigerJet
[Mon Nov 16 17:38:24 2026] usb 2-2: Manufacturer: TigerJet Network, Inc.
[Mon Nov 16 17:38:24 2026] usb 2-2: SerialNumber: A921XXXXXXXXXX
[Mon Nov 16 17:38:24 2026] usb-storage 2-2:1.0: USB Mass Storage device detected
[Mon Nov 16 17:38:24 2026] scsi host22: usb-storage 2-2:1.0
```

### Decoding each line

**`usb 2-2: new high-speed USB device`** — Now we're on Bus 2, Port 2. "High-speed" = USB 2.0 (480 Mbps). `ehci-pci` is the Enhanced Host Controller Interface driver — the USB 2.0 standard. The device is now talking properly.

**`idVendor=06e6, idProduct=c200`** — These two hexadecimal numbers together form the USB VID:PID (Vendor ID : Product ID). Every USB device has these baked into firmware. `06e6` is assigned to Tiger Jet Network, Inc. by the USB-IF (the standards body). `c200` is TigerJet's product code for this phone device. These numbers are the primary key Linux uses to decide which driver to load.

**`bcdDevice=0.01`** — "BCD" stands for Binary-Coded Decimal. This is the device firmware revision: version 0.01. Very early firmware.

**`SerialNumber: A921XXXXXXXXXX`** — The first four characters `A921` are the model identifier (this is where "A921" in the original directory name comes from). The rest is a unique hardware serial number (masked here as `X`s; magicJack tied accounts to device serials).

**`usb-storage 2-2:1.0: USB Mass Storage device detected`** — The kernel spotted Interface 0 of the device (the `1.0` means Configuration 1, Interface 0) and recognized it as a USB Mass Storage device. It immediately loaded the `usb-storage` driver. This interface holds the Windows softphone application on internal flash memory — it appears to Windows as a small CD-ROM or USB drive.

**`scsi host22: usb-storage 2-2:1.0`** — The `usb-storage` driver registers a fake SCSI host adapter (number 22 in this system) to handle the mass storage interface using the standard SCSI-over-USB protocol.

---

## 3. The System's USB Landscape — `lsusb`

The `lsusb` command lists all currently recognized USB devices. It only shows devices that successfully enumerated. The failed magicJack does not appear; the successful one does.

```
Bus 002 Device 011: ID 06e6:c200 Tiger Jet Network, Inc. Internet Phone
```

Other devices present:
- **Logitech TrackMan Wheel** (`046d:c404`) — trackball mouse on Bus 1
- **Dell KB212-B Keyboard** (`413c:2107`) — quiet keyboard on Bus 1
- **Realtek RTL8188EUS** (`0bda:8179`) — USB Wi-Fi dongle (802.11n) on Bus 2

The Bus structure on this machine:
- **Bus 1**: USB 1.1 (`uhci_hcd`) — mouse, keyboard
- **Bus 2**: USB 2.0 (`ehci_hcd`) — Wi-Fi dongle, magicJack (after fix)
- **Bus 3**: USB 2.0 (`ehci_hcd`) — empty
- **Buses 4-8**: USB 1.1 (`uhci_hcd`) — empty (the bad port was Bus 4)
- **Bus 9**: USB 2.0 (`ehci_hcd`) — empty
- **Bus 10**: USB 3.0 (`xhci_hcd`) — empty

---

## 4. Understanding USB Descriptors — `lsusb -v`

### What are USB descriptors?
Descriptors are structured data blocks stored in the device's firmware that describe its capabilities to the host. When a device enumerates, the host reads these descriptors to understand what the device is and how to talk to it. Think of them as the device's resume.

### The Device Descriptor
```
bcdUSB               2.00        ← USB spec version: 2.0
bDeviceClass            0        ← Class defined per-interface (not at device level)
bMaxPacketSize0        64        ← Control endpoint max packet: 64 bytes (USB 2.0 max)
idVendor           0x06e6        ← TigerJet Network
idProduct          0xc200        ← Internet Phone
bNumConfigurations      1        ← Only one configuration available
MaxPower              500mA      ← Requests maximum bus power (USB 2.0 limit)
```

The fact that `bDeviceClass = 0` means this is a **composite device** — each interface declares its own class rather than the whole device being one type.

### The 5 Interfaces

This device presents 5 logical interfaces to the host. Linux can bind a different driver to each one independently.

#### Interface 0 — USB Mass Storage
```
bInterfaceClass         8  Mass Storage
bInterfaceSubClass      6  SCSI
bInterfaceProtocol     80  Bulk-Only
Endpoint 0x81 (EP 1 IN):  Bulk, 512 bytes
Endpoint 0x02 (EP 2 OUT): Bulk, 512 bytes
```
This is the Windows app storage. The `usb-storage` driver handles it using the standard SCSI-over-USB Bulk-Only Transport protocol. Two bulk endpoints (one for data going in, one going out) carry SCSI commands and data.

#### Interface 1 — USB Audio Control
```
bInterfaceClass         1  Audio
bInterfaceSubClass      1  Control Device
bcdADC               1.00  ← USB Audio Device Class version 1.0
```
This interface does not carry audio data. It is the **control interface** for the audio subsystem — it describes the topology of the audio path and allows the host to set volume, mute, sample rates, etc.

**Audio topology** (read this like a signal flow diagram):

```
Handset Mic (Terminal 1, type=Handset INPUT)
    ↓
Feature Unit 5 (Mute + Volume control)
    ↓
USB Streaming OUTPUT (Terminal 4) → EP 3 IN → Host captures audio

Host sends audio → EP 4 OUT → USB Streaming INPUT (Terminal 2)
    ↓
Feature Unit 6 (Mute + Volume control)
    ↓
Handset Speaker (Terminal 3, type=Handset OUTPUT)
```

The two "Feature Units" are where the host can send control commands to adjust volume or mute in each direction independently. `snd-usb-audio` handles all of this automatically.

#### Interface 2 — Audio Streaming (Capture / Microphone path)
```
Alt Setting 0: zero-bandwidth (idle, no endpoints active)
Alt Setting 1: active
  Format: PCM, 16-bit, mono, 8000 Hz
  Endpoint 0x83 (EP 3 IN): Isochronous, 16 bytes/packet, interval 4
```
This is the **microphone path** — audio coming from the handset microphone to the host. 

**Understanding Isochronous endpoints**: Unlike Bulk transfers (which retry on error), Isochronous transfers are time-critical — they deliver data at a guaranteed rate, no retries. For 8000 Hz, 16-bit mono audio: 8000 samples/sec × 2 bytes = 16,000 bytes/sec. With a 4ms interval (interval field=4 means 2^(4-1) = 8 frames of 125µs each = 1ms... actually at high speed, interval 4 = 2^(4-1) = 8 microframes = 1ms), that's 16 bytes per packet. This matches `wMaxPacketSize = 0x0010 = 16 bytes` exactly.

The device selects Alt Setting 0 (idle) when no audio is active, and the driver switches to Alt Setting 1 when recording begins — this saves USB bandwidth.

#### Interface 3 — Audio Streaming (Playback / Speaker path)
```
Alt Setting 0: zero-bandwidth (idle)
Alt Setting 1: 16000 Hz, 16-bit, mono → Endpoint 0x04 (EP 4 OUT), 32 bytes/packet
Alt Setting 2: 8000 Hz, 16-bit, mono  → Endpoint 0x04 (EP 4 OUT), 16 bytes/packet
```
This is the **speaker path** — audio going from the host to the handset speaker. Two alternate settings allow either 16kHz (wideband voice) or 8kHz (narrowband, classic PSTN quality) playback. The 16kHz option is for higher-quality audio if the VoIP codec supports it.

#### Interface 4 — Human Interface Device (HID)
```
bInterfaceClass         3  HID
Endpoint 0x85 (EP 5 IN): Interrupt, 8 bytes max, interval 7
HID Report Descriptor:  133 bytes
```
This is the **phone event interface**. HID (Human Interface Device) is normally used for keyboards and mice, but the USB HID spec also covers telephony devices. The device sends interrupt-driven reports whenever a button is pressed, the hook state changes, etc.

The `usbhid` driver binds to this interface and exposes it as `/dev/hidraw*`.

---

## 5. The sysfs Device Tree — DEVICES.out

The `/sys/bus/usb/devices/` directory is the kernel's live view of USB devices. The file listing shows which driver is bound to each interface:

```
2-2:1.0  → has ep_02, ep_81, host22    → usb-storage driver (SCSI host 22)
2-2:1.1  → has sound/                  → snd-usb-audio driver (created sound card)
2-2:1.2  → (audio streaming, idle)     → snd-usb-audio driver
2-2:1.3  → (audio streaming, idle)     → snd-usb-audio driver
2-2:1.4  → has 0003:06E6:C200.0005     → usbhid driver
           has usbmisc/
```

The presence of a `sound/` subdirectory under `2-2:1.1` confirms `snd-usb-audio` created an ALSA sound card for the Audio Control interface. The `0003:06E6:C200.0005` entry under `2-2:1.4` is the HID device node created by `usbhid`.

---

## 6. ALSA Audio Devices — APLAY.out and ARECORD.out

ALSA (Advanced Linux Sound Architecture) is the Linux kernel audio subsystem. `aplay -l` lists playback devices; `arecord -l` lists capture devices.

```
card 2: TigerJet [USB Internet Phone by TigerJet], device 0: USB Audio [USB Audio]
```

This confirms `snd-usb-audio` successfully registered the magicJack as ALSA **card 2** with one device (device 0). Both playback and capture are available on the same ALSA device.

ALSA addressing: audio devices are referenced as `hw:CARD,DEVICE` — so this device is `hw:2,0` or equivalently `hw:TigerJet,0`.

To test audio directly:
```bash
# Record 5 seconds from the handset mic:
arecord -D hw:2,0 -f S16_LE -r 8000 -c 1 -d 5 test.wav

# Play audio back through the handset speaker:
aplay -D hw:2,0 test.wav
```

---

## 7. Decoding the HID Report Descriptor — xxd Output

This is the most complex piece. The raw hex output from `xxd` is a binary blob that follows the USB HID Report Descriptor format — a compact bytecode language that describes what data the device sends and what it means.

### The raw bytes
```
050b 0901 a101 0906 a102 19b0 29bb 1501
250c 9501 7504 8100 c005 0b09 2509 2309
2409 2609 2109 2f09 3125 0775 0381 0005
0709 4a09 4b09 4e09 5109 5209 2809 2a25
0775 0381 0009 04a1 0205 0c09 e015 ff25
0195 0175 0281 06c0 0509 a102 1901 2904
1501 2504 9501 7503 8100 c005 0b09 2015
0025 0195 0175 0181 0206 ffff 0900 7508
9520 b100 c0
```

### HID descriptor item encoding
Each item in the descriptor starts with a tag byte structured as:
```
[7:4] bTag  — what kind of item (Usage, Logical Minimum, Report Size, etc.)
[3:2] bType — Main (0), Global (1), Local (2)
[1:0] bSize — number of data bytes that follow (0,1,2,4)
```

For example, `05 0b`:
- `05` = 0000 0101 → tag=0 (Usage Page), type=1 (Global), size=1 (1 data byte follows)
- `0b` = data byte: Usage Page 0x0B = Telephony Devices

### Full decoded descriptor

```
05 0b        Usage Page (Telephony Devices, 0x0B)
09 01        Usage (Phone)
a1 01        Collection (Application)        ← outermost container

  09 06      Usage (Telephony Key Pad)
  a1 02      Collection (Logical)             ← groups the keypad
    19 b0    Usage Minimum (Phone Key 0, 0xB0)
    29 bb    Usage Maximum (Phone Key #, 0xBB)
             [0xB0=0, 0xB1=1, ..., 0xB9=9, 0xBA=*, 0xBB=# → 12 keys]
    15 01    Logical Minimum (1)
    25 0c    Logical Maximum (12)
    95 01    Report Count (1)                 ← one field
    75 04    Report Size (4 bits)             ← field is 4 bits wide
    81 00    Input (Array)                    ← reports INDEX of pressed key
  c0         End Collection

  05 0b      Usage Page (Telephony)
  09 25      Usage (Transfer)
  09 23      Usage (Hold)
  09 24      Usage (Redial)
  09 26      Usage (Drop)
  09 21      Usage (Flash)
  09 2f      Usage (Phone Mute)
  09 31      Usage (Send)
  25 07      Logical Maximum (7)              ← 7 possible functions
  75 03      Report Size (3 bits)
  81 00      Input (Array)                    ← which function key (0=none)

  05 07      Usage Page (Keyboard/Keypad)
  09 4a      Usage (Home)
  09 4b      Usage (Page Up)
  09 4e      Usage (Page Down)
  09 51      Usage (Down Arrow)
  09 52      Usage (Up Arrow)
  09 28      Usage (Return/Enter)
  09 2a      Usage (Backspace)
  25 07      Logical Maximum (7)
  75 03      Report Size (3 bits)
  81 00      Input (Array)                    ← navigation key (0=none)

  09 04      Usage (Keyboard A — used as collection tag only)
  a1 02      Collection (Logical)
    05 0c    Usage Page (Consumer Devices)
    09 e0    Usage (Volume)
    15 ff    Logical Minimum (-1)             ← 0xFF = -1 (signed)
    25 01    Logical Maximum (+1)
    95 01    Report Count (1)
    75 02    Report Size (2 bits)
    81 06    Input (Variable, Relative)        ← -1=down, 0=no change, +1=up
  c0         End Collection

  05 09      Usage Page (Button)
  a1 02      Collection (Logical)
    19 01    Usage Minimum (Button 1)
    29 04    Usage Maximum (Button 4)
    15 01    Logical Minimum (1)
    25 04    Logical Maximum (4)
    95 01    Report Count (1)
    75 03    Report Size (3 bits)
    81 00    Input (Array)                    ← which button (0=none)
  c0         End Collection

  05 0b      Usage Page (Telephony)
  09 20      Usage (Hook Switch)              ← *** THE KEY EVENT ***
  15 00      Logical Minimum (0)
  25 01      Logical Maximum (1)
  95 01      Report Count (1)
  75 01      Report Size (1 bit)
  81 02      Input (Variable, Absolute)       ← 0=on-hook, 1=off-hook

  06 ff ff   Usage Page (Vendor Defined, 0xFFFF)
  09 00      Usage (vendor-specific, 0x00)
  75 08      Report Size (8 bits)
  95 20      Report Count (32)
  b1 00      Feature (Array)                  ← 32-byte vendor config block

c0           End Collection (Application)
```

### How the report bits pack into 2 bytes

HID reports are bit-packed in LSB-first order. The input fields above, in order, give us:

```
Byte 0:
  bits 0-3 (4 bits) : DTMF keypad  (0=none, 1=key0, 2=key1, ..., 10=key9, 11=*, 12=#)
  bits 4-6 (3 bits) : Phone function (0=none, 1=Transfer, 2=Hold, 3=Redial,
                                       4=Drop, 5=Flash, 6=Mute, 7=Send)
  bit  7   (1 bit)  : low bit of keyboard key field

Byte 1:
  bits 0-1 (2 bits) : high bits of keyboard key field
  bits 2-3 (2 bits) : Volume relative (-1, 0, +1)
  bits 4-6 (3 bits) : Button (0=none, 1-4=button number)
  bit  7   (1 bit)  : HOOK SWITCH (0=on-hook, 1=off-hook)
```

**Total input report: 2 bytes (16 bits)**

To read the hook switch in code:
```python
hook_off = bool(report[1] & 0x80)   # True = handset lifted (off-hook)
dtmf_key = report[0] & 0x0F          # 0=none, 1-12 = DTMF digit
```

### The Feature report
The 32-byte vendor feature report is for device configuration — things like ringing control, LED state, etc. These are `b1` (Feature) type rather than `81` (Input), meaning they are read/written via control transfers, not the interrupt endpoint. The protocol for these is undocumented and would need to be reverse-engineered from Windows USB traffic captures to fully understand.

---

## 8. Summary: What Linux Already Gives Us for Free

| Need | Mechanism | Status |
|------|-----------|--------|
| Detect device plug/unplug | udev rules on VID:PID 06e6:c200 | Ready |
| Record audio from handset mic | ALSA hw:2,0, 8kHz 16-bit mono | Ready |
| Play audio to handset speaker | ALSA hw:2,0, 8kHz or 16kHz 16-bit mono | Ready |
| Detect hook state changes | /dev/hidraw*, read bit 15 of 2-byte report | Ready |
| Detect DTMF key presses | /dev/hidraw*, bits 0-3 of byte 0 | Ready |
| Detect phone function keys | /dev/hidraw*, bits 4-6 of byte 0 | Ready |
| Adjust volume | ALSA mixer controls on card 2 | Ready |
| Ring the handset | Unknown — likely vendor Feature report | Needs reveng |
| Access Windows app | /dev/sdX (usb-storage mount) | Ready (not needed) |

**No custom kernel driver is needed.** Everything required for a functional VoIP application is accessible through standard Linux interfaces: ALSA for audio and `/dev/hidraw*` for telephony events.
