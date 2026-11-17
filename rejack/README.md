# Rejack

A [baresip](https://github.com/baresip/baresip) application module that turns a magicJack A921 USB dongle
and the analog phone plugged into it into a SIP phone. Project docs: `../PROJECT.md`, `../HARDWARE.md`,
`../RING.md`.

## Layout
| File | Role | baresip needed? |
|---|---|---|
| `src/phone.[ch]` | Call state machine (hook, digits, SIP progress → tones/ring/dial/answer) | no |
| `src/tones.[ch]` | North American call-progress tones (dial, ringback, busy, reorder, howler) | no |
| `src/tjdev.[ch]` | Dongle access over hidraw: hook/keypad decode, TigerJet register R/W, ring/line power | no |
| `src/rejack.c` | baresip glue: hidraw fd, timers, SIP events, tone playback, config | yes |
| `test/test_phone.c` | Unit tests for the three core files | no |
| `conf/config.rejack` | baresip config lines for Rejack | — |

## Status
- State machine, tones, HID decoding, E911 status check: implemented, 90 unit checks passing.
- baresip glue: builds against baresip/libre **v4.12.0** in `../deps/`, loads in baresip (`run/` dev config).
- `tjdev_ring()` / `tjdev_line_power()`: return `-ENOTSUP` until Phase 3 (RING.md) finds the mechanism.
- HID input (hook/keypad) decoding follows the report descriptor; not yet confirmed on hardware.

## Build

### 1. libre + baresip v4.12.0 into a local prefix (no root needed)
Ubuntu 24.04 ships baresip 1.0.0 (2020), whose module API is incompatible. Build packages first:
```
sudo apt install cmake libssl-dev zlib1g-dev
```
Then (sources are already cloned in `../ext/re` and `../ext/baresip` at tag v4.12.0):
```
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

## Run (development)
```
rejack/run/start.sh            # baresip + Rejack with config, alerts and recordings under rejack/run/
```
Fill in `rejack/run/emergency.env` (ntfy URL, Telegram token/chat id), then
`REJACK_EMERGENCY_ENV=rejack/run/emergency.env rejack/scripts/rejack-emergency test`.

## Run (installed)
```
LD_LIBRARY_PATH=$PREFIX/lib $PREFIX/bin/baresip     # first run writes ~/.baresip/config
```
Merge `conf/config.rejack` into `~/.baresip/config` (and comment out `module_app menu.so`), add a SIP
account to `~/.baresip/accounts`, and restart. The hidraw node needs access: run as root for now, or
install the udev rule from `../HARDWARE.md`.

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
