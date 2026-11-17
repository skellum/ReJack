# Ring Control — Research Notes

Status as of 2026-11-16: **strong hypothesis, not yet tested on hardware.**

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
