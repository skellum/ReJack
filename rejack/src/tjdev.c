/*
 * tjdev.c — see tjdev.h.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "tjdev.h"

#define REPORT_LEN 32               /* vendor feature report payload */
#define BUF_LEN    (REPORT_LEN + 1) /* + report ID 0 */

struct tjdev {
	int  fd;
	char path[64];
	int  bank;   /* currently selected register bank, -1 = unknown */
	int  saved34;   /* 0x34 before tjdev_setup(), -1 = not saved */
	struct tj_lstate ls;
};

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static int find_hidraw(char *path, size_t n)
{
	DIR *d = opendir("/sys/class/hidraw");
	struct dirent *e;
	char want[32];

	if (!d)
		return -errno;
	snprintf(want, sizeof(want), "%08X:%08X", TJ_VID, TJ_PID);

	while ((e = readdir(d))) {
		char p[300], line[256];
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device/uevent", e->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "HID_ID=", 7) && strstr(line, want)) {
				snprintf(path, n, "/dev/%.32s", e->d_name);
				fclose(f);
				closedir(d);
				return 0;
			}
		}
		fclose(f);
	}
	closedir(d);
	return -ENODEV;
}

int tjdev_open(struct tjdev **devp, const char *path)
{
	struct tjdev *dev;
	int err;

	if (!devp)
		return -EINVAL;
	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -ENOMEM;
	dev->bank = -1;
	dev->saved34 = -1;

	if (path && *path)
		snprintf(dev->path, sizeof(dev->path), "%s", path);
	else if ((err = find_hidraw(dev->path, sizeof(dev->path))) < 0) {
		free(dev);
		return err;
	}

	dev->fd = open(dev->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (dev->fd < 0) {
		err = -errno;
		free(dev);
		return err;
	}
	*devp = dev;
	return 0;
}

void tjdev_close(struct tjdev *dev)
{
	if (!dev)
		return;
	if (dev->fd >= 0)
		close(dev->fd);
	free(dev);
}

int tjdev_fd(const struct tjdev *dev)         { return dev ? dev->fd : -1; }
const char *tjdev_path(const struct tjdev *dev) { return dev ? dev->path : ""; }

void tjdev_decode(const uint8_t raw[2], struct tj_input *in)
{
	/* keypad array: 1..10 = '0'..'9', 11 = '*', 12 = '#' */
	static const char keys[] = "0123456789*#";
	unsigned k = raw[0] & 0x0f;

	memset(in, 0, sizeof(*in));
	in->raw[0]  = raw[0];
	in->raw[1]  = raw[1];
	in->key     = (k >= 1 && k <= 12) ? keys[k - 1] : 0;
	in->fnkey   = (raw[0] >> 4) & 0x07;
	in->offhook = (raw[1] & 0x80) != 0;
}

int tjdev_read_input(struct tjdev *dev, struct tj_input *in)
{
	uint8_t buf[8];
	ssize_t n = read(dev->fd, buf, sizeof(buf));

	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
	if (n < 2)
		return 0;
	tjdev_decode(buf, in);
	return 1;
}

static int set_feature(struct tjdev *dev, const uint8_t *data, int len)
{
	uint8_t b[BUF_LEN] = { 0 };

	memcpy(b + 1, data, len);
	return ioctl(dev->fd, HIDIOCSFEATURE(BUF_LEN), b) < 0 ? -errno : 0;
}

static int get_feature(struct tjdev *dev, uint8_t *out)
{
	uint8_t b[BUF_LEN] = { 0 };

	if (ioctl(dev->fd, HIDIOCGFEATURE(BUF_LEN), b) < 0)
		return -errno;
	memcpy(out, b + 1, REPORT_LEN);
	return 0;
}

int tjdev_reg_read(struct tjdev *dev, uint8_t reg)
{
	uint8_t bank = reg & 0xE0, data[REPORT_LEN];
	int err;

	if (dev->bank != bank) {
		const uint8_t sel[4] = { 0x04, bank, bank, 0x00 };
		if ((err = set_feature(dev, sel, 4)) < 0)
			return err;
		dev->bank = bank;
		msleep(4);
	}
	if ((err = get_feature(dev, data)) < 0)
		return err;
	return data[reg & 0x1F];
}

int tjdev_reg_write(struct tjdev *dev, uint8_t reg, uint8_t val)
{
	const uint8_t p[5] = { 0x04, reg, (uint8_t)(reg & 0xE0), 0x01, val };

	dev->bank = -1;   /* a write may change the device's selected bank */
	return set_feature(dev, p, 5);
}

static int reg_rmw(struct tjdev *dev, uint8_t reg, uint8_t and_mask, uint8_t or_bits)
{
	int v = tjdev_reg_read(dev, reg);

	if (v < 0)
		return v;
	return tjdev_reg_write(dev, reg, (uint8_t)((v & and_mask) | or_bits));
}

int tjdev_setup(struct tjdev *dev)
{
	int v = tjdev_reg_read(dev, 0x34);

	if (v < 0)
		return v;
	dev->saved34 = v;
	(void)tjdev_ring(dev, false);
	return tjdev_reg_write(dev, 0x34, (uint8_t)(v & ~0x60));   /* auto + forced dial tone off */
}

int tjdev_release(struct tjdev *dev)
{
	int err = tjdev_ring(dev, false);
	uint8_t v = dev->saved34 >= 0 ? (uint8_t)dev->saved34 : 0x26;   /* 0x26 = power-up value */

	if (!err)
		err = tjdev_reg_write(dev, 0x34, (uint8_t)((v & ~0x40) | 0x20));
	return err;
}

/* CTj560CPhone_Hid::PlayTone(DIALTONE): on |= 0x40, off &= 0x1F. We keep bit 7 and leave bit 5 alone. */
int tjdev_dialtone(struct tjdev *dev, bool on)
{
	return on ? reg_rmw(dev, 0x34, 0xFF, 0x40) : reg_rmw(dev, 0x34, 0xBF, 0x00);
}

/* CTj560CPhone_Hid::Ring(): on |= 0x30, off &= 0xCF. */
int tjdev_ring(struct tjdev *dev, bool on)
{
	return on ? reg_rmw(dev, 0x3D, 0xFF, 0x30) : reg_rmw(dev, 0x3D, 0xCF, 0x00);
}

char tjdev_dtmf_char(uint8_t r15)
{
	static const char map[16] = "D1234567890*#ABC";

	return (r15 & 0x40) ? map[r15 & 0x0F] : 0;
}

void tjdev_line_decode(struct tj_lstate *s, uint8_t r58, uint8_t r15, struct tj_line *out)
{
	bool raw = (r58 & 0x02) != 0;
	bool dv  = (r15 & 0x40) != 0;

	memset(out, 0, sizeof(*out));
	if (!s->init) {
		/* start "on-hook": a phone already off-hook is reported as a change once debounced */
		s->init = true;
		s->offhook = false;
		s->raw_last = raw;
		s->stable = 0;
		s->dtmf_last = dv;           /* don't report a digit latched before we started */
	}

	if (raw == s->raw_last) {
		if (s->stable < TJ_HOOK_DEBOUNCE)
			s->stable++;
	}
	else {
		s->raw_last = raw;
		s->stable = 1;
	}
	if (s->stable >= TJ_HOOK_DEBOUNCE && raw != s->offhook) {
		s->offhook = raw;
		out->hook_changed = true;
	}
	out->offhook = s->offhook;

	if (dv && !s->dtmf_last && s->offhook)
		out->digit = tjdev_dtmf_char(r15);
	s->dtmf_last = dv;
}

int tjdev_poll(struct tjdev *dev, struct tj_line *out)
{
	int r58 = tjdev_reg_read(dev, 0x58);
	int r15;

	if (r58 < 0)
		return r58;
	r15 = tjdev_reg_read(dev, 0x15);
	if (r15 < 0)
		return r15;
	tjdev_line_decode(&dev->ls, (uint8_t)r58, (uint8_t)r15, out);
	return 0;
}
