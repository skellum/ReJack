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

/* Decoded 2-byte HID input report (see HARDWARE.md). */
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

/* Line control. Phase 3 (RING.md) — currently return -ENOTSUP. */
int  tjdev_line_power(struct tjdev *dev, bool on);
int  tjdev_ring(struct tjdev *dev, bool on);

#endif
