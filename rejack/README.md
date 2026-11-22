# Rejack

A [baresip](https://github.com/baresip/baresip) application module that turns a magicJack A921 USB dongle
and the analog phone plugged into it into a SIP phone. Project docs: [`../ARCHITECTURE.md`](../ARCHITECTURE.md),
[`../docs/HARDWARE.md`](../docs/HARDWARE.md), [`../RING.md`](../RING.md).

## Layout
| File | Role | baresip needed? |
|---|---|---|
| `src/phone.[ch]` | Call state machine (hook, digits, SIP progress → tones/ring/dial/answer) | no |
| `src/tones.[ch]` | North American call-progress tones (dial, ringback, busy, reorder, howler) | no |
| `src/tjdev.[ch]` | Dongle access over hidraw: hook/keypad decode, TigerJet register R/W, ring/line power | no |
| `src/linemon.[ch]` | Line-down monitor: when to send the "line down" / "restored" alerts | no |
| `src/dtmfdet.[ch]` | DTMF detector (Goertzel) for the line audio | no |
| `src/rejack.c` | baresip glue: hidraw fd, timers, SIP events, tone playback, config | yes |
| `test/test_phone.c` | Unit tests for the core files | no |
| `test/dtmfscan.c` | Runs the DTMF detector over a recording (`arecord -D hw:TigerJet,0 -f S16_LE -r 8000 -c 1`) | no |
| `conf/config.rejack` | baresip config lines for Rejack | — |

## Status
- State machine, tones, line-register decoding, E911 status check, line-down monitor, DTMF detector: implemented, 169 unit checks passing.
- baresip glue: builds against baresip/libre **v4.12.0** in `../deps/`, loads in baresip (`run/` dev config).
- Digits are decoded from the line audio (`src/dtmfdet.c`): the capture runs whenever no call is up and
  closes before call audio or the emergency recording needs the device. The chip's decoder (`0x15`) is the
  fallback (`rejack_dtmf_detect chip`, or if the capture can't open): it keeps the last digit and doesn't
  always strobe "digit present", so a repeated key, or a call starting with the previous call's last digit,
  is lost there.
- Line control (`src/tjdev.c`, registers verified on hardware): hook `0x58` and DTMF `0x15` polled every 20 ms
  (hook debounced over 3 polls), ring `0x3D`, dial tone from the chip's generator (`0x34` bit 6;
  `rejack_dialtone sw` uses the baresip player instead). On start Rejack turns off the chip's automatic dial
  tone (`0x34` bit 5) and restores it on exit, so a stopped Rejack leaves an ordinary "dial tone, goes nowhere"
  line. A hard kill skips the restore: replug the dongle.
- Runs against the home Asterisk (`../pbx/`) as extension 100: calls verified in both directions.

## Build

### 1. libre + baresip v4.12.0 into a local prefix (no root needed)
Ubuntu 24.04 ships baresip 1.0.0 (2020), whose module API is incompatible. Build packages first:
```
sudo apt install cmake libssl-dev zlib1g-dev
```
Then clone the sources at tag v4.12.0 into `../ext/` (gitignored) and build them:
```
git clone --depth 1 --branch v4.12.0 https://github.com/baresip/re ../ext/re
git clone --depth 1 --branch v4.12.0 https://github.com/baresip/baresip ../ext/baresip
PREFIX=$(cd .. && pwd)/deps      # run from rejack/
cmake -S ../ext/re -B ../ext/re/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX
cmake --build ../ext/re/build -j && cmake --install ../ext/re/build
cmake -S ../ext/baresip -B ../ext/baresip/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_PREFIX_PATH=$PREFIX -DMODULES="g711;alsa;stdio"
cmake --build ../ext/baresip/build -j && cmake --install ../ext/baresip/build
```

### 2. Rejack
```
PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig cmake -S . -B build
cmake --build build && ctest --test-dir build
cmake --install build          # rejack.so -> $PREFIX/lib/baresip/modules
```
Stop `rejack.service` before `cmake --install` if it is running (it has the old `rejack.so` loaded).

The unit tests also pass under AddressSanitizer and UndefinedBehaviorSanitizer:
```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --target test_phone && build-asan/test_phone
```

## Run (development)
```
rejack/run/start.sh            # baresip + Rejack with config, alerts and recordings under rejack/run/
```
Fill in `rejack/run/emergency.env` (ntfy URL, Telegram token/chat id), then
`REJACK_EMERGENCY_ENV=rejack/run/emergency.env rejack/scripts/rejack-emergency test`.

## Run as a service (systemd)
```
sudo rejack/systemd/install.sh     # renders rejack/systemd/rejack.service.in, enables + starts rejack.service
journalctl -u rejack -f
```
Runs `rejack/run/start.sh` as the invoking user (needs the `audio` group). Restarts on failure; after any stop,
`tools/tjprobe release` hands the line back to the chip. If the dongle is missing or unplugged, Rejack keeps
running, ends any call, and re-attaches within ~2 s when it returns (baresip would otherwise run on silently
without a module that failed to load).

**Line-down alert:** if the dongle is missing, or keeps dropping off and re-enumerating, for
`rejack_line_alert` seconds (default 120; 0 = off), Rejack runs the hook with `line-down - <detail> -`. Once
the dongle has stayed attached for 30 s it sends `line-up` with the outage length (only after a down alert).
`scripts/rejack-emergency` posts both on the configured channels (ntfy priority high / default).
A dongle that stops enumerating (USB `error -71`/`-110`) needs a physical replug; the alert is how you find out.

## Watch it
`tools/rejack` (Python 3, standard library only) shows what the line is doing. It is read-only: it reads the
service journal, sysfs and systemctl, and never opens the dongle (a second reader of the chip's registers
scrambles ReJack's own reads).
```
tools/rejack                 # one-shot status: service, dongle, SIP, line state, today's calls and errors
tools/rejack -v              # follow every log line; calls, state changes and errors highlighted, repeats folded
tools/rejack --visual        # top-style dashboard (-V); keys: q quit, v all lines/events, p pause, c clear errors
tools/rejack --since "2026-11-20 08:00"      # widen the window (default: today)
```
Needs read access to the journal (`adm` or `systemd-journal` group).

## Run (installed)
```
LD_LIBRARY_PATH=$PREFIX/lib $PREFIX/bin/baresip     # first run writes ~/.baresip/config
```
Merge `conf/config.rejack` into `~/.baresip/config` (and comment out `module_app menu.so`), add a SIP
account to `~/.baresip/accounts`, and restart. The hidraw node needs access: run as root for now, or
install the udev rule from [`../docs/HARDWARE.md`](../docs/HARDWARE.md).

## 911 — always attempted, never a dead end
- `rejack_911 allow` (default): 911/1911/9911 are always dialed. `verified` dials only with a fresh E911
  status file (`verified <did> <unix-time>`, see `src/e911.h`); `block` never dials.
- Every 911 runs `rejack_emergency_hook dialed <verified|unverified> - -` → contacts notified.
- If 911 can't go through (dial error, call fails before answer, or policy refuses): the phone enters
  EMERGENCY — announcement (`rejack_emergency_announce`, busy tone if missing), local recording to
  `rejack_record_dir`, hook `failed <e911> <reason> <wav>`. On hang-up: hook `ended … <wav>`.
- `scripts/rejack-emergency` sends to ntfy / Telegram / Discord / email per `/etc/rejack/emergency.env`,
  attaches the recording where possible, and queues failed sends in an outbox (`rejack-emergency flush`).
- The announcement must say plainly that 911 was **not** reached; text in `sounds/emergency.txt`,
  audio in `sounds/emergency.wav` (espeak-ng en-us → 8 kHz mono, 17 s, played twice).

## License
MIT — see [`../LICENSE`](../LICENSE).
