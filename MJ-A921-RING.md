> **⚠️ Unverified AI-generated research.** This document was produced by Google Gemini and is kept for
> reference only. Parts of it are wrong for this device — e.g. the controller chip is most likely a
> TigerJet TJ580, not a TJ3201, and the A921's phone-line chip did not answer on the expected SPI port.
> See [`RING.md`](RING.md) and [`HARDWARE.md`](HARDWARE.md) for what has actually been tested on hardware.

# magicJack A921: Hardware Architecture & Ring Generation Notes

**Model:** magicJack A921 (1st Generation / Silver USB Dongle, circa 2008)  
**Device Class:** USB-to-RJ11 Foreign Exchange Station (FXS) Adapter  
**Standalone Network Support:** None (Requires active USB host)

---

## 1. Core Hardware & Host Requirements

* **No Standalone Network Stack:** Unlike later models (magicJack PLUS, GO, or Express), the A921 has no RJ45 Ethernet port, Wi-Fi transceiver, or onboard SIP user agent. It cannot register directly to a network or SIP server on its own.
* **USB Host Dependency:** The A921 relies on a connected host computer (PC, thin client, or SBC) for 5V USB power, audio codec processing, and control signaling. 
* **USB Enumeration:** When connected to a host OS, the A921 enumerates as a composite USB device consisting of:
  1. **USB Audio Class Device:** Handles bidirectional 8kHz/16-bit PCM voice audio (microphone/earpiece).
  2. **USB Human Interface Device (HID):** Exposes control endpoints for hook-switch detection (on-hook/off-hook), DTMF/keypad events, and ring generator triggers.
  3. **USB Mass Storage (CD-ROM partition):** Contains the legacy Windows/Mac softphone installer.

---

## 2. Ring Voltage Generation & Electrical Limits

* **Boost Converter Architecture:** Because USB only supplies 5V DC, the A921 uses an internal DC-to-DC boost converter and switching circuit (typically built around a TigerJet TJ3201 or Silicon Labs Si321x ProSLIC chipset) to step up voltage for the RJ11 tip/ring lines.
* **Software-Triggered Ringing:** The internal ring generator does not operate autonomously. Plugging the A921 into a 5V USB wall adapter will **not** produce talk battery or ring voltage. The host OS must actively send specific USB HID feature reports/control commands to toggle the high-voltage ring cadence.
* **Electrical Specifications & REN Limits:**
  * **Ring Voltage:** ~58V–85V AC (modulated DC/trapezoidal wave, varies slightly by USB bus voltage stability).
  * **Loop Current (Off-Hook):** ~12.5 mA – 16.5 mA (significantly lower than standard PSTN lines, which supply 20 mA – 35 mA).
  * **Ringer Equivalence Number (REN) Load:** Effectively ~1.0 REN max. 
  * **Vintage Phone Caveat:** The A921 easily rings modern electronic phones and DECT cordless bases, which require minimal current to detect an AC ring signal. However, it frequently lacks the current drive to mechanically actuate heavy electromagnetic ringer coils in vintage rotary or 2500-series desk phones without an external ring voltage booster.

---

## 3. Third-Party SIP & Linux Integration Options

Because the A921 lacks flashable network firmware, it cannot be converted into a standalone SIP Analog Telephone Adapter (ATA). To ring a physical phone without the proprietary magicJack softphone, one of the following architectures is required:

### Option A: Headless Linux Host + Custom HID/Audio Bridge
* **Concept:** Replace the desktop PC with a low-power Linux host (e.g., Raspberry Pi, Wyse thin client) acting as a dedicated USB-to-SIP gateway.
* **Audio Path:** Standard Linux ALSA or PulseAudio drivers natively recognize the USB audio interface (`snd-usb-audio`), allowing local PBX software (Asterisk, FreePBX) or a SIP client to route voice streams directly.
* **Control / Ringing Path:** Requires a custom userspace daemon (e.g., Python with `hidapi` / `pyusb` interacting with `/dev/hidrawX`) to poll the HID endpoint for off-hook transitions and write the proprietary HID byte sequences needed to pulse the ring generator.

### Option B: Hardware Replacement (Standalone SIP ATA)
* **Concept:** Retire the A921 hardware in favor of a dedicated network ATA (e.g., Grandstream HT801/HT802, Cisco/Linksys SPA112) if true PC-free Ethernet operation is a strict requirement.
* **Advantages:** Native SIP/RTP stack, higher loop current (~25 mA), stronger ring voltage (~70V–90V RMS capable of driving 3–5 REN mechanical bells), and zero host maintenance.

---

## 4. Open Engineering Tasks (If Building a Custom A921 Host)

- [ ] **USB Descriptor Dump:** Run `lsusb -v` and `usbhid-dump` on a Linux host to confirm exact Vendor ID / Product ID (typically `0x06e6:0xc200` or similar TigerJet/magicJack IDs) and HID report descriptor lengths.
- [ ] **HID Ring Command Capture:** Capture USB traffic (via Wireshark/USBPcap) during an inbound call to isolate the exact HID output report bytes that enable/disable the ring voltage generator.
- [ ] **Hook-Switch Polling:** Verify the HID input report bitmask corresponding to on-hook vs. off-hook states and pulse/DTMF detection.
- [ ] **Load Testing:** Test ring actuation on the target analog phone to determine if an external ring booster or higher-current USB hub is necessary.

