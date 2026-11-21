# Ring and line control

**Status (2026-11-17): solved and verified on hardware.** The register map below came from two independent
drivers (magicJack's macOS driver, class `CTj580Phone_Hid`, and GVJackApp v2 for Windows) and was checked with a
TS52 PRO buttset. Everything after the summary is the dated lab notebook that got there, hypotheses included.

## Summary

| Function | Register | Verified |
|---|---|---|
| Register access | 32-byte HID feature report: write `04 <reg> <reg&0xE0> <count> <data…>`; read = select bank, then GET | `tools/tjprobe` |
| Talk battery | None: on from power-up (34 V on-hook, 14 mA loop). No software line-enable exists | Buttset, both drivers |
| Ring | `0x3D \|= 0x30` on, `&= 0xCF` off; the 20 Hz square wave is hardware, the 2 s / 4 s cadence is the host's | Ring detected on every burst |
| Hook | `0x58` bit 1 (set = off-hook) | 4 of 4 transitions |
| DTMF | `0x15`: bit 6 = digit present, low nibble = digit (1–9, A = 0, B = `*`, C = `#`) | 6 of 6 digits |
| Dial tone | `0x34` bit 5 = automatic on off-hook (power-up `0x26`), bit 6 = on command | By ear |

Ring voltage is not yet measured (a published bulletin for the first-generation dongle gives about 85 V square
wave).

---

## Lab notebook

Status as of 2026-11-16, when this section was started: **strong hypothesis, not yet tested on hardware.**

## Sources
- Device storage: `/dev/sr0` → installer only (NSIS, extracted to `images/nsis/`). `/dev/sdb` "PHONE" →
  the softphone folder was destroyed by a past Windows chkdsk (`images/phone/BOOTEX.LOG`). **No softphone binary available.**
- `ext/magicjack-hacking/` — github.com/Gbrothers1/magicjack-hacking. RE of a later **magicJack HOME**
  (same 06e6:c200 ID, but ARM-based chip, 64-byte feature report). Ring + line power verified there.
- `ext/magicjack-hacking/06-magicjack-usb-tigerjet/reference-drivers/tjctl.c` — TigerJet's own 2004 Linux
  userspace driver for the TJ560 FXS reference design (06e6:831c, vendor control transfers, not HID).
- `ext/.../datasheets/Tiger560B.pdf`, `Si3210-ProSLIC.pdf`.
- `ext/magicjack-liberation/` — github.com/saulvalenzuela23/magicjack-liberation (not yet reviewed).

## Why the HOME's ring method probably does NOT apply to the A921
The macOS driver picks the HID report length by chip family: **64 bytes = ARM chip, 32 bytes = non-ARM**.
The A921 descriptor declares a **32-byte** feature report → non-ARM. On ARM units the firmware owns the
ProSLIC (ring = ARM reg0 |= 0x300). On non-ARM units the **host** drives the ProSLIC over the TigerJet
serial port (SPI bridge), as tjctl.c does.

## Hypothesised A921 protocol (HID feature report, report ID 0, 32-byte payload)
| Operation | Bytes |
|---|---|
| Write TJ regs | `04 <reg> <reg&0xE0> <count> <data…>` |
| Read TJ bank | SET `04 <bank> <bank> 00`, then GET → `data[reg & 0x1F]` |
| ProSLIC write | write TJ 0x26..0x29 = `{addr & 0x7F, value, 0x00, 0x27}` |
| ProSLIC read | write TJ 0x26..0x29 = `{addr \| 0x80, 0, 0, 0x67}`, then read TJ 0x27 |
| **Ring on** | ProSLIC reg 64 (Linefeed) = **0x44** (tjctl.c) |
| **Ring off** | ProSLIC reg 64 = **0x11** (forward active) |

Cadence (2 s on / 4 s off, US) is done by the host toggling reg 64. The ring oscillator and DC-DC
converter must be initialised first (tjctl.c `init` sequence: indirect regs incl. RING_OSC 0x7EF0,
RING_X 0x0160, DC-DC power-up, calibration).

## Risks
- On the HOME, the firmware **blocked** host writes to TJ 0x26/0x27/0x29 (EPIPE). Might also happen on the A921.
- ProSLIC DC-DC converter init writes affect a high-voltage power stage. Mis-set values can overheat
  the external transistors. Copy tjctl.c's values only after checking they match this board (Si3210 + TJ560 reference).
- Ring/line state is volatile: unplug/replug resets. Never touch flash opcodes (magic 0x07773456).

## Test plan
1. `tools/tjprobe get` — sends nothing; dump the feature report.
2. `tools/tjprobe regs` — read TJ regs 0x00–0x7F (chip ID at 0x55).
3. `tools/tjprobe slic` — read ProSLIC ID/linefeed/voltages. **Decides everything:** if this works, ringing is
   a few register writes plus an init sequence.
4. (Needs approval) port tjctl.c init + `ring`/`noring` to hidraw.

## Results — 2026-11-16 (first probe, phone on-hook)
- `get`: 32-byte report = TJ register bank 0x00–0x1F (same bytes as `regs` bank 0).
- `regs`: reg 0x01 = 0x6C (passes TjIpSys probe `&0x6C == 0x6C`); **reg 0x55 = 0x13** → TjIpSys "type 2" chip;
  0x56 = 0xCF, 0x57 = 0x11. **reg 0x00 = 0x26** (EXTRST# bit0 = 0). Serial port regs 0x26–0x29 all 0x00.
- Device is USB 2.0 high-speed, so not literally a Tiger560B (USB 1.1); register map nonetheless matches so far.
- `slic`: **SET_FEATURE writes to 0x26–0x29 accepted (no EPIPE)** — unlike the magicJack HOME, the serial
  port is NOT firmware-blocked. But every ProSLIC read returned 0x00.
- Hypothesis: ProSLIC held in reset by EXTRST# low (datasheet reg 0x00 bit0; tjctl.c init does 0x50 → 0x51).
  Next: `tjprobe slic-reset` (sets bit0, reads, restores). Si3210 comes out of reset with DC-DC off and
  linefeed open, so this is electrically safe.

## Results — `slic-reset` (EXTRST# released, then restored)
- 0x29 went 0x67 → 0x66: the serial-port engine ran the transfer (start bit self-cleared).
- 0x26/0x27 read back 0x00 for every ProSLIC register, with and without reset released.
- Conclusion: SPI engine works, but **nothing answers on the serial port** (or reset is wired elsewhere,
  e.g. an AUX pin). The A921 may not follow the TJ560 + Si3210 reference design.
- Alternative lead: TjIpSys "type 2/3" activation (protocol.md, from the Windows DLL, non-ARM path) touches
  TJ regs 0x00, 0x02, 0x57 (RAM clear), 0x3C, 0x30–0x33, 0x55 — suggests an on-chip line/codec block.
  Current values: 0x02=04, 0x30=34 0x31=e2 0x32=19 0x33=79, 0x3C=20, 0x55=13.
- Next: `tjprobe watch` (read-only) — diff registers 0x00–0x7F and HID input across hook transitions.

## Review: saulvalenzuela23/magicjack-liberation (2026-11-16) — not usable for the A921
- Targets an **ARM TJ911/TJ880 "ArmJack" eCos** unit (its firmware dumps contain `/builds/ata/tj911`,
  `ArmJack eCos SDK`). Same family as the HOME repo, not our non-ARM 32-byte chip.
- Single commit, no recorded hardware results. README claims "Hook, Ring, DTMF fully cracked", but the code
  contains **no ring command**; "Command 3 = Toggle Relay" is a guess in a comment.
- Its "10-byte packet" `04 00 00 00 | cmd16 | len16 | 00 55 | payload` matches the **TjIpSys host-internal
  transaction struct** `{len=4, 0, reg, val, dir, 0x55}` and the **TjIpSysCall API opcodes**
  (0 = SYS_OPEN, 16/17 = start/stop detect) documented in the HOME repo — i.e. DLL function arguments,
  not USB wire bytes.
- Sends them as SET_REPORT **Output** (wValue 0x0200); our descriptor has no Output report, and its
  "Method C" uses bRequest 0x03 (GET_PROTOCOL), not a feature write.
- Takeaway: nothing new. Do not run its scripts against the A921.

## Line-power research — 2026-11-16 (evening)

### What the Windows "type 2" activation actually touches
TjIpSys (HOME repo `protocol.md`, from the non-ARM DLL path) runs on SYS_OPEN for chip type 2 (reg 0x55 = 0x13,
which is ours): `0x00 C0→40`, `0x02 20→00`, 128-byte clear at `0x57`, `0x3C=32`, `0x30..0x33 = 80 3E 00 7D`,
`0x55=B4`. None of these are GPIO/AUX/divider registers — it looks like **codec/DSP init, not line power**.
- The values `80 3E 00 7D 90 01` already exist in our chip at **0x4F–0x54** (and the HOME's GET page 0 shows the
  same bytes plus our `0x40–0x4D` block `f4 01 0c fe … 99 01 40 1f 66 0f 0e 0f`). Our 0x30–0x33 read `34 E2 19 79`.
  So either the sequence was never applied here, or TjIpSys register numbers don't map 1:1 to our read window.
- `0x57` "128-byte clear" is risky to replay: unknown whether 0x57 is a FIFO port or a register range. Skipped.

### Board evidence (Sandman magicJack tech bulletin, "Version 1" = first-gen dongle)
Talk battery **50 V DC**, off-hook 4.1 V, loop current **12.5 mA**, ring **85 V AC square wave** (one unit 63 V).
Sandman: https://www.sandman.com/knowledgebase/magic-jack-tech-bulletin
A *square-wave* ring suggests polarity switching of the battery supply at ~20 Hz rather than a ProSLIC
sine/trapezoid — consistent with discrete line circuitry driven by TigerJet GPIO (and with no ProSLIC answering SPI).

### TigerJet GPIO state (Tiger560B register map; `tools/tjprobe gpio`)
- Every GIO pin (0–14) and every AUX pin is an **input**; clock dividers A/B are **off** (0x17–0x19 = 0).
  Nothing is actively driving line circuitry → consistent with an unpowered line.
- The chip has hardware **clock dividers routable to GIO8/9/10** (`0x17/0x18` ratio, `0x19` source, `0x1a–0x1c`
  output select). Divider B from 93.75 kHz or 1 kHz-class sources can make a 20 Hz square wave on a pin —
  a plausible ring-cadence generator that matches the square-wave ring.
- Inputs that changed on their own between reads (no writes in between): **GIO0 1→0** (0x0a a2→a0) and
  **AUX1 1→0** (0x12 9e→9c). Candidates for line/hook/power sense — correlate with what changed physically.
- `0x06` reads 0x66 after the SPI tests = same as `0x29` status → 0x06 looks like a status mirror (ignore).

### Next experiments (in order of risk)
1. **Read-only**: `tjprobe watch` while plugging/unplugging the phone, lifting the handset, base power on/off —
   map which GIO/AUX inputs are sense lines.
2. **Low risk (needs OK)**: `tjprobe activate snap.bin` — replays type-2 activation (minus 0x57), diffs registers;
   `tjprobe restore snap.bin` or replug to undo. Watch LED / cordless "no line" indicator.
3. **Medium risk (needs OK)**: GPIO output probing — set one pin at a time to output *at its current input level*
   (no electrical change), then toggle briefly, watching sense inputs and the phone. Risk: contention if a pin
   is driven externally. Start with pins pulled high (likely active-low enables).

### Result: `tjprobe activate` (2026-11-16, phone plugged in, LED on before)
All 10 writes accepted (0x55 silently ignored — read-only chip type). Device stayed enumerated.
Register changes (snapshot diff):
| Reg | Before | After | Meaning |
|---|---|---|---|
| 0x00 | 26 | 40 | written (C0→40) |
| 0x02 | 04 | 00 | written (20→00) |
| 0x30–0x33 | 34 e2 19 79 | 80 3e 00 7d | written |
| 0x3c | 20 | 32 | written |
| **0x0a** | a0 | **a8** | **GIO1 input 0→1** (not written — external circuit reacted) |
| **0x0f** | 28 | **aa** | **GIO4 and GIO7 inputs 0→1** (not written) |
| 0x12 | bc | 9c | AUX5 1→0 (AUX inputs also drift on their own: 9e/9c/bc seen) |
| 0x15 | 80 | 00 | not written, undocumented |
| 0x58 | 03 | 01 | not written |
| 0x5f | 01 | 00 | not written |
5-second sampling afterwards (508 reads): all of the above steady — no toggling.
Physical effect: (pending — LED, cordless "no line" indicator, dial tone/hum)
Undo: `tjprobe restore <snapshot>` or replug.

### Result: step-through of the type-2 activation (`tools/tj_steps.sh`, 2026-11-16 21:57)
One write at a time, 10 s human watch of LED + VTech handset "No Line" after each: **no visible change at any
step** (LED solid, "No Line" throughout). Conclusion: **the TjIpSys type-2 activation does not power the line.**
- Only input effect: writing **0x02 = 0x20** (clearing bit 2; original 0x04) flips **GIO4 and GIO7 0→1**, and
  0x02 = 0x00 keeps them high. Likely a pin-mux/alternate-function bit, not line circuitry.
- Earlier "GIO1 0→1" was drift, not activation: GIO1 already read 1 at the restored baseline. GIO0/GIO1, AUX1/AUX5,
  0x15, 0x58, 0x5f all change on their own → sense inputs or status registers.
- New lead: the snapshot taken just before activation had **0x09 = 0x80**; it read 0x00 every other time.
  On the Tiger560B, **0x08/0x09 = HID Report Value low/high** — bit 15 is our hook-switch bit. So the chip
  may have reported "off-hook" at that moment (cordless base seizing the line?). Registers 0x08/0x09 give a
  pollable view of the HID report; 0x0C–0x0E are a "software HID report" that firmware/host can inject.

## Board photo (2026-11-16): no ProSLIC — discrete line circuit
See [`docs/HARDWARE.md`](docs/HARDWARE.md) "Board". Discrete boost converter (L4 100 µH, D1, C38, switch transistor), HV transistors
(MMBT5401 ×2 visible, 5551-class NPN), PC814 AC-input opto for loop sense, no SLIC or boost-controller IC on the
component side. The ProSLIC SPI hypothesis is closed. Next: back-side photo (Q1–Q3, U1/U3/U4), then trace which
TigerJet pins reach the switch transistor bases and the opto output — before any GPIO output experiment.

## ★ From magicJack's own macOS driver (2026-11-16 late) — our chip's real code paths
Source: `mjupdate` in magicJack's public `mjisoupdate.dmg` (upgrades.magicjack.com; downloaded to the
gitignored `images/vendor/` for local interoperability analysis only — nothing redistributed). Universal
Mach-O, x86_64 slice, **symbols intact** (libTjSdkMac.a: Tj560BPhone.o, Tj560CPhone.o, **Tj580Phone.o**, Tj780…).

**Class selection** (`CSJHandsetMagicJackDriverBase::CreateTjDeviceInstance`, jump table @ 0x100034760):
devType 1 → **CTj580Phone_Hid**, 4 → CTj780, 5 → CTj880, 7 → CTj980, 8 → CTj911 (HOME = ARM types).
Our bcdDevice is 0.01 → devType 1 → **CTj580Phone_Hid**, which **inherits CTj560CPhone_Hid**
(constructor calls the 560C constructor). The 560C class has **no ProSLIC methods** — it's the discrete-line
family, matching the board photos.

Primitives: `tjoutp(reg, val)` = `WriteTjRegs(reg, &val, 1)` = our `04 <reg> <reg&E0> 01 <val>`;
`tjinp(reg, &val)` = read.

**CTj580Phone_Hid::InitTjHardware()** (@0x10002b580):
```
0x01 = E1 ; 0x01 = 59                 ← unlock key
0x4E = 00 ; 0x55 = 80 ; 0x39 = A0 ; 0x3C = 25
0x34 = (0x34 & 0x90) | 0x06
0x2F =  0x2F & 0xFE
if (0x00 & 0x04) { 0x32 = (0x32 & 0xE0) | 0x1C ; 0x33 = (0x33 & 0x80) | 0x79 }
0x01 = FF ; 0x01 = 00                 ← lock
```
(on first open also `InitDTMFMemory`: 0x00|=0x80, 0x02|=0x20, 128 zero bytes @0x57, then undo — DTMF memory.)
Our earlier "activate" used the 560C/type-2 values **without the E1/59 unlock** — likely why nothing happened.

**Ring / polarity — register 0x3D** (CTj560CPhone_Hid, inherited):
- `Ring(true, x)`: `0x3D |= 0x30` (bits 4+5). `Ring(false, true)`: `&= 0xCF`. `Ring(false, false)`: `&= 0xEF` (keeps bit 5).
  Cadence (2 s / 4 s) is done by the caller; the 20 Hz square wave is generated in hardware.
- `SetTipRingPolarity(b)`: bit 7. `SetLineReversal()`: bit 7 high for 100 ms.
- 560C init clears bit 4 (`&= 0xEF`). Our 0x3D reads 0x00.

**Hook — register 0x58 bit 1** (CTj580Phone_Hid::KeyScan @0x10002b8e0): set = off-hook (debounced in software;
going off-hook while ringing calls Ring(false, true)). Matches the one snapshot where 0x58 = 03 and 0x09 = 80.

**Line power:** no explicit "line on" call exists in the 560C/580 paths (no GPIO/AUX writes; `SetAuxCtrl` is
560B-only). So talk battery is either automatic in hardware once InitTjHardware has run (0x55=80, 0x39=A0 and
0x3C=25 are the candidates), or comes with the ring bits. Hardware test pending:
`tjprobe init580` → watch "No Line"; then `tjprobe ring 2`; `tjprobe hook`.

Default register tables `_tj560c_reg_00_1e / _26_2d / _34_59` (RestoreHardwareDefaultValues) match our chip's
reset values almost byte for byte — confirms the family.

### Hardware test of the driver sequences (2026-11-16 ~22:40, freshly replugged)
- `tjprobe init580`: all writes accepted; changed 0x2F 15→14, 0x32 19→1C, 0x34 26→06, 0x39 90→A0, 0x3C 20→25,
  side effect 0x64 0E→0C. 0x3D read 0x0C. **VTech handset still "No Line"** (no talk battery).
- `tjprobe ring 2` (0x3D 0C↔3C, 2 s on / 4 s off): **GIO4–7 alternate 0110 ↔ 1001 during the bursts** and sit at
  0110 between them — the H-bridge drive pins toggling. So the ring *drive* works. Nothing seen/heard at the base
  (handset battery died; base gives no ring indication). GIO0/1 change between samples even when idle
  (booster feedback? hook sense?).
- Open question: is the boost converter running at all? Next session: Fluke TS53 PRO + banjo on the RJ11 —
  idle battery/polarity, off-hook (0x58 bit 1), ring detection during `tjprobe ring`.
- If the line is dead: the booster probably needs an enable that the updater never calls. Next RE target:
  the softphone driver `mj_dev` (in magicJackSetup.dmg, stripped but with class strings) —
  `SetTelephoneEnable` and the device-vtable calls it makes for CTj580Phone_Hid.

### `mj_dev` (the real softphone driver) — 2026-11-16 late
Source: magicJack's public `magicJackSetup.dmg` → `upgrade` binary → embedded ZIP → `Softphone/magicJack.app/
Contents/MacOS/mj_dev` (universal x86_64 + arm64, stripped, RTTI present for CTj580Phone_Hid / CTj560CPhone_Hid).
Located the TigerJet helpers by byte signature from the symbol-rich updater: `tjoutp` 0x10018fdd0,
`tjinp` 0x10018fdf0, `WriteTjRegs` 0x10019f250 (x86_64 slice). Every call site was decoded (see notes below).
- The 580/560C code in mj_dev is **the same** as in mjupdate (same InitTjHardware, Ring/0x3D, polarity, hook/0x58).
- Only extra write: **`0x4E |= 0x40` in the chip-class destructors** → 0x4E bit 6 = power-down/standby on close
  (init writes 0x4E = 0). Ours reads 0x00, so not the missing piece.
- **`SetTelephoneEnable [1|0]`** is a custom-command handler (func 0x10000b640) on the handset-driver object:
  `SetToneType(0)`, `SetRinging(false)`, `if (!IsOnHook()) SetCustom(1)`, then a flag. Slots resolved via the
  updater's `vtable for CSJHandsetMagicJackDriverTest`. **No hardware "line enable" exists in software.**
- CTj580Phone_Hid vtable resolved in full (mjupdate @0x1000610b0): e.g. slot 0x150 = Ring, 0x0e0 =
  SetTipRingPolarity, 0x1f8 = RestoreHardwareDefaultValues (not called by the 580's InitTjHardware).

**Conclusion:** talk battery on this board is meant to be automatic (TigerJet-controlled boost, likely configured
by the 0x34/0x39/0x3C values InitTjHardware writes). "No Line" after init580 therefore points at the **hardware**:
boost not switching (Q7 gate drive), a failed/dry electrolytic (C38; C39 footprint empty), or talk battery present
but below the VTech's threshold. Next: TS53 PRO for line voltage; **5 V logic probe on Q7's gate** (pulses = boost
being driven); then C38 voltage. GIO0/GIO1 changing even when idle may be the boost PWM/feedback.

## GVJackApp v2 (Windows, 2013) — 2026-11-17
Third-party Google Voice app that rang A921 dongles. Inno Setup 5.5.6 installer (sha256 `6d8534ff…3270b`, kept in
`images/vendor/gvjack/`, not committed), unpacked with innoextract. `GVJackApp.exe` (x86, MFC) drives the TigerJet
**directly over HID feature reports** — no TigerJet DLL. Logs `PhoneJack:SerialNumberFound` (our board's name).
Other bundled DLLs (`Dongle_Hw/Ut` = ProSLIC "ATA" dongles, `CM_HID*`, `BT_EX`, `YLUSBTEL`) serve other vendors.

- Register write (fcn 0x41082f): 33-byte feature report `00 04 <reg> <reg> 01 <val>`; read (0x410915): SET
  `00 04 00 <reg> 00`, GET, value at `[reg]` — same framing as ours. A second 65-byte variant (`20 …`) is used when a
  device flag (+0x984) is set (later chip).
- Per-VID switch: `0x06E6` = TigerJet. (`0x40=0x44` ring / `0x40=0x12` idle is the **0x183D** vendor, not ours.)
- **Init, TigerJet branch (0x415aec):** `01=E1, 01=59, 55=80, 39=A0, 3C=25`, RingOff (`3D=00`), `32=1C, 33=79`
  (unconditional), **no re-lock**. A subset of CTj580 InitTjHardware — no 0x4E, 0x34, 0x2F.
- **Ring:** on `3D=30`, off `3D=00` (65-byte devices: reg `00=03`/`00=00` instead).
- **Hook:** reg `0x58 & 0x02` (same as mj_dev). 65-byte devices: reg `0x18 & 0x80`.
- **DTMF (new):** reg `0x15`: bit 6 = digit present, low nibble = code (`0xB` checked specially; MT8870-style
  1–9, A=0, B=*, C=#). 65-byte devices use reg `0x17`. → chip decodes DTMF; Rejack can use it.
- **Conclusion:** GVJack does nothing to enable line power either. Two independent drivers (magicJack, GVJack)
  agree that talk battery is not a software switch on this board → if the line reads 0 V after init, suspect the
  booster hardware (Q7 drive, C38, D8, L4). `tjprobe gvinit` replays GVJack's exact sequence for completeness.

## Line test with TS52 PRO — 2026-11-17 — **line power works, no init needed**
Dongle unplugged overnight, replugged, nothing written; TS52 PRO on a banjo (no phone).
- **On-hook talk battery: 34 V DC** (cf. Nortel ~51 V, Pairgain ~43 V). **Off-hook loop current: 14 mA**
  (Sandman's V1: 12.5 mA). The booster runs from power-up; there is no software line-enable (as both drivers implied).
  Last night's VTech "No Line" was the cordless base/handset (its threshold, or the dead battery), not the dongle.
- **Hardware dial tone (reg 0x34):** power-up value `0x26`.
  - bit 5 (`0x20`) = **automatic dial tone on off-hook**, cut on the first DTMF digit (switch-like behaviour).
    Cleared → silent line (loop current unchanged). magicJack's init clears it (`0x34 = (0x34&0x90)|0x06`); GVJack leaves it.
  - bit 6 (`0x40`) = **dial tone on command** (`CTj560CPhone_Hid::PlayTone(DIALTONE,on)`: `0x34 |= 0x40`; off: `&= 0x1F`).
  - Verified by ear: `34=06` → silent, `34=46` → tone, `34=06` → silent, `34=26` → back to automatic.
- **Hook:** `0x58` = `0x03` off-hook / `0x01` on-hook (bit 1), 4/4 transitions; HID report byte `0x09` bit 7 mirrors it.
- **DTMF:** `0x15` = `0xC1 C2 C3 CB CA CC` for 1 2 3 * 0 # — bit 6 present, low nibble digit (A=0, B=*, C=#). All 6 decoded.
- Rejack plan: on start clear bit 5 and own dial tone (play it only when registered); hook from 0x58 bit 1;
  digits from 0x15; with Rejack stopped the line still has battery + automatic dial tone.
- **Ring:** `tjprobe ring 3` (`0x3D = 0x30` 2 s / `0x00` 4 s) → **TS52 PRO detected ring on all 3 bursts.** No init
  needed. (Ring voltage not yet measured; Sandman's V1: ~85 V square wave.)
- **Phase 3 is closed:** battery, dial tone, hook, DTMF and ring all work from power-up over HID registers.
