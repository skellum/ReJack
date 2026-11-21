/*
 * tjdev.h — magicJack A921 (TigerJet 06e6:c200) hardware access over hidraw.
 *
 * Plain POSIX C, no baresip dependency. Register framing is documented in ../../RING.md.
 */
#ifndef REJACK_TJDEV_H
#define REJACK_TJDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TJ_VID 0x06e6
#define TJ_PID 0xc200

struct tjdev;

/* Decoded 2-byte HID input report (see docs/HARDWARE.md). */
struct tj_input {
	bool offhook;
	char key;        /* '0'-'9', '*', '#', or 0 for none */
	uint8_t fnkey;   /* telephony function 0-7 (Flash = 5) */
	uint8_t raw[2];
};

/* Find the hidraw node by VID:PID. path may be NULL/"" for auto. Returns 0 or -errno. */
int  tjdev_open(struct tjdev **devp, const char *path);
void tjdev_close(struct tjdev *dev);
int  tjdev_fd(const struct tjdev *dev);
const char *tjdev_path(const struct tjdev *dev);

/* Read one input report (fd is non-blocking). Returns 1 = got report, 0 = none, -errno on error. */
int  tjdev_read_input(struct tjdev *dev, struct tj_input *in);
void tjdev_decode(const uint8_t raw[2], struct tj_input *in);

/* TigerJet register access via the 32-byte feature report. Return 0/value or -errno. */
int  tjdev_reg_read(struct tjdev *dev, uint8_t reg);
int  tjdev_reg_write(struct tjdev *dev, uint8_t reg, uint8_t val);

/*
 * Line control (registers verified on hardware 2026-11-17, see ../../RING.md):
 *   talk battery  always on from power-up (~34 V on-hook, ~14 mA loop) — nothing to enable
 *   0x34 bit 5    chip's automatic dial tone on off-hook (power-up default; cut on first digit)
 *   0x34 bit 6    dial tone on command
 *   0x3D bits 4+5 ring drive (H-bridge flips at ring frequency)
 *   0x58 bit 1    off-hook
 *   0x15          DTMF decoder: low nibble = last digit (1-9, A=0, B=*, C=#), bit 6 = short "digit
 *                 present" strobe (not seen for every digit on every phone)
 */

/* Take over the line from the chip's standalone mode: automatic dial tone off, ring off.
 * Remembers 0x34 so tjdev_release() can hand the line back. */
int  tjdev_setup(struct tjdev *dev);
/* Ring off, restore the chip's automatic dial tone (standalone line). */
int  tjdev_release(struct tjdev *dev);

int  tjdev_dialtone(struct tjdev *dev, bool on);
int  tjdev_ring(struct tjdev *dev, bool on);

/* Line state from register polling. */
struct tj_line {
	bool offhook;    /* debounced */
	bool hook_changed;
	char digit;      /* new DTMF digit (0x15 nibble change, or bit-6 edge for a repeated key), or 0 */
};

/* Decoder state; zero-initialise. */
struct tj_lstate {
	bool    init;
	bool    offhook;     /* debounced state */
	bool    raw_last;
	uint8_t stable;      /* consecutive polls raw == raw_last */
	bool    dtmf_last;   /* bit 6 at the previous poll */
	uint8_t dtmf_nib;    /* low nibble at the previous poll */
	uint8_t dtmf_pending;/* polls left in which a bit-6 edge belongs to the last nibble change */
};

#define TJ_HOOK_DEBOUNCE 3   /* polls (x 20 ms) */
#define TJ_DTMF_STROBE   5   /* polls (x 20 ms) after a nibble change in which bit 6 is its strobe */

/* Pure decoder (unit-tested): feed one poll of regs 0x58 and 0x15. */
void tjdev_line_decode(struct tj_lstate *s, uint8_t r58, uint8_t r15, struct tj_line *out);
char tjdev_dtmf_char(uint8_t r15);   /* digit character for a 0x15 value with bit 6 set, else 0 */

/* Read 0x58 + 0x15 and decode. Returns 0 or -errno. */
int  tjdev_poll(struct tjdev *dev, struct tj_line *out);

#endif
