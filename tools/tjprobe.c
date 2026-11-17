/*
 * tjprobe.c — probe the magicJack A921 (TigerJet 06e6:c200) control path over hidraw.
 *
 * Modes (in increasing order of intrusiveness, none of them changes line state):
 *   tjprobe            GET_FEATURE only. Sends nothing to the device. Dumps the 32-byte report.
 *   tjprobe regs       Reads TigerJet registers 0x00-0x7F. For each 32-register bank it sends
 *                      a "bank select" SET_FEATURE that writes zero registers (04 BB BB 00).
 *   tjprobe slic       Asks the TigerJet serial port (regs 0x26-0x29) to READ ProSLIC registers
 *                      over SPI: ID, linefeed, loop status, battery voltages. Tells us whether the
 *                      host can reach the SLIC at all, which is what ringing needs.
 *
 *   tjprobe slic-reset Sets TJ reg 0x00 bit0 (release SLIC external reset), repeats the SLIC
 *                      reads, then restores reg 0x00. Writes exactly one TigerJet register.
 *
 *   tjprobe watch      Read-only. Prints HID input reports and TJ registers 0x00-0x7F that change.
 *
 * Framing (non-ARM TigerJet, 32-byte feature report, no report ID), from public RE work in
 * github.com/Gbrothers1/magicjack-hacking and TigerJet's tjctl.c:
 *   write regs:  04 <reg> <reg&0xE0> <count> <data...>
 *   read bank:   SET 04 <bank> <bank> 00, then GET -> data[reg & 0x1F]
 *   SLIC read:   write 0x26..0x29 = { addr|0x80, 0, 0, 0x67 }, then read reg 0x27
 *
 * Build:  gcc -O2 -Wall -o tjprobe tjprobe.c
 * Run:    sudo ./tjprobe [regs|slic]     (or install the udev rule from HARDWARE.md)
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

#define REPORT_LEN 32               /* feature report payload size from the HID descriptor */
#define BUF_LEN    (REPORT_LEN + 1) /* + leading report-ID byte (0) */

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int find_hidraw(char *path, size_t n) {
    DIR *d = opendir("/sys/class/hidraw");
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) {
        char p[512], line[256];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "HID_ID=") && strstr(line, "000006E6:0000C200")) {
                snprintf(path, n, "/dev/%s", e->d_name);
                fclose(f); closedir(d);
                return 0;
            }
        }
        fclose(f);
    }
    closedir(d);
    return -1;
}

static int get_feature(int fd, unsigned char *out /* REPORT_LEN */) {
    unsigned char b[BUF_LEN] = { 0 };
    int n = ioctl(fd, HIDIOCGFEATURE(BUF_LEN), b);
    if (n < 0) return -errno;
    memcpy(out, b + 1, REPORT_LEN);
    return n - 1;
}

static int set_feature(int fd, const unsigned char *data, int len) {
    unsigned char b[BUF_LEN] = { 0 };
    memcpy(b + 1, data, len);
    int n = ioctl(fd, HIDIOCSFEATURE(BUF_LEN), b);
    return n < 0 ? -errno : 0;
}

static void dump(const unsigned char *b, int len, int base) {
    for (int o = 0; o < len; o += 16) {
        printf("  %02x:", base + o);
        for (int i = o; i < o + 16 && i < len; i++) printf(" %02x", b[i]);
        printf("\n");
    }
}

/* Read a whole 32-register bank. Returns 0 or -errno. */
static int read_bank(int fd, int bank, unsigned char *out) {
    unsigned char sel[4] = { 0x04, (unsigned char)bank, (unsigned char)bank, 0x00 };
    int r = set_feature(fd, sel, 4);
    if (r) return r;
    msleep(4);
    r = get_feature(fd, out);
    return r < 0 ? r : 0;
}

static int write_regs(int fd, int reg, const unsigned char *val, int count) {
    unsigned char p[REPORT_LEN] = { 0x04, (unsigned char)reg, (unsigned char)(reg & 0xE0),
                                    (unsigned char)count };
    memcpy(p + 4, val, count);
    return set_feature(fd, p, 4 + count);
}

static int verbose = 0;

/* Read one TigerJet register. Returns value or -errno. */
static int tj_read(int fd, int reg) {
    unsigned char bank[REPORT_LEN];
    int r = read_bank(fd, reg & 0xE0, bank);
    return r ? r : bank[reg & 0x1F];
}

/* ProSLIC direct-register read via the TigerJet serial port. Returns value or -errno. */
static int slic_read(int fd, int addr) {
    unsigned char cmd[4] = { (unsigned char)(addr | 0x80), 0, 0, 0x67 };
    unsigned char bank[REPORT_LEN];
    int r = write_regs(fd, 0x26, cmd, 4);
    if (r) return r;
    /* wait for 0x29 bit0 (start/busy) to clear, up to ~50 ms */
    for (int i = 0; i < 25; i++) {
        msleep(2);
        r = read_bank(fd, 0x20, bank);
        if (r) return r;
        if (!(bank[0x09] & 0x01)) break;
    }
    if (verbose)
        printf("      raw 0x26..0x29 after read of %d: %02x %02x %02x %02x\n",
               addr, bank[0x06], bank[0x07], bank[0x08], bank[0x09]);
    return bank[0x27 & 0x1F];
}

static void slic_report(int fd) {
    static const struct { int reg; const char *name; } regs[] = {
        { 0,  "ID / revision (Si3210 part number in bits 5:4, rev in 3:0)" },
        { 64, "Linefeed (0x00 open, 0x11 fwd active, 0x44 ringing)" },
        { 68, "Loop/ring-trip status (bit0 = loop closure / off-hook)" },
        { 80, "TIP voltage (x 0.376 V)" },
        { 81, "RING voltage (x 0.376 V)" },
        { 82, "Battery VBAT (x 0.376 V)" },
        { 14, "Power-down control (Si3210 reset default 0x10 = DC-DC off)" },
        { 11, "Loopback/hybrid (Si3210 reset default 0x33)" },
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        int v = slic_read(fd, regs[i].reg);
        if (v < 0) printf("  SLIC reg %3d: FAILED (%s)  %s\n", regs[i].reg, strerror(-v), regs[i].name);
        else       printf("  SLIC reg %3d: 0x%02x  %s\n", regs[i].reg, v, regs[i].name);
    }
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "get";
    char path[300];
    unsigned char rep[REPORT_LEN];

    if (find_hidraw(path, sizeof path)) {
        fprintf(stderr, "magicJack (06e6:c200) hidraw node not found\n");
        return 1;
    }
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror(path); return 1; }
    printf("# device: %s  mode: %s\n", path, mode);

    if (!strcmp(mode, "get")) {
        int n = get_feature(fd, rep);
        if (n < 0) { printf("GET_FEATURE failed: %s\n", strerror(-n)); return 1; }
        printf("# GET_FEATURE returned %d bytes:\n", n);
        dump(rep, REPORT_LEN, 0);
    } else if (!strcmp(mode, "regs")) {
        for (int bank = 0x00; bank <= 0x60; bank += 0x20) {
            int r = read_bank(fd, bank, rep);
            if (r) { printf("  bank %02x: failed (%s)\n", bank, strerror(-r)); continue; }
            dump(rep, REPORT_LEN, bank);
        }
        printf("# hints: reg 0x01 & 0x6C should be 0x6C; reg 0x55 = chip type (0x12/0x13 seen on TJ)\n");
    } else if (!strcmp(mode, "slic")) {
        verbose = 1;
        slic_report(fd);
        printf("# EPIPE/EIO on every read = serial port blocked by firmware (as on the magicJack HOME).\n"
               "# Same value for every register (00/ff) = path open but SLIC not answering.\n");
    } else if (!strcmp(mode, "slic-reset")) {
        /* Release the SLIC's external reset (TJ reg 0x00 bit0 = EXTRST#), read, then restore.
         * A freshly reset Si3210 powers up with DC-DC converter off and linefeed open, so this
         * cannot put voltage on the line. Replugging the dongle also restores everything. */
        verbose = 1;
        int r0 = tj_read(fd, 0x00);
        if (r0 < 0) { printf("read of TJ reg 0x00 failed: %s\n", strerror(-r0)); return 1; }
        printf("# TJ reg 0x00 = 0x%02x (EXTRST# bit0 = %d)\n", r0, r0 & 1);
        unsigned char v = (unsigned char)(r0 | 0x01);
        int r = write_regs(fd, 0x00, &v, 1);
        if (r) { printf("write TJ reg 0x00 = 0x%02x failed: %s\n", v, strerror(-r)); return 1; }
        msleep(100);
        printf("# wrote 0x%02x, read back 0x%02x; waited 100 ms\n", v, tj_read(fd, 0x00));
        slic_report(fd);
        unsigned char orig = (unsigned char)r0;
        r = write_regs(fd, 0x00, &orig, 1);
        printf("# restored TJ reg 0x00 = 0x%02x (%s), read back 0x%02x\n", orig,
               r ? strerror(-r) : "ok", tj_read(fd, 0x00));
    } else if (!strcmp(mode, "watch")) {
        /* Read-only: print HID input reports (hook/keys) and any TJ register 0x00-0x7F that changes.
         * Lift/hang up the handset and press keys while this runs. Ctrl-C to stop. */
        unsigned char prev[128], cur[128];
        int have = 0;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        printf("# watching; lift/replace handset, press keys. Ctrl-C to stop.\n");
        for (;;) {
            unsigned char in[8];
            int n;
            while ((n = read(fd, in, sizeof in)) > 0) {
                printf("[input] %d bytes:", n);
                for (int i = 0; i < n; i++) printf(" %02x", in[i]);
                if (n >= 2) printf("   hook=%s dtmf=%d", (in[1] & 0x80) ? "OFF" : "on", in[0] & 0x0F);
                printf("\n");
            }
            int ok = 1;
            for (int bank = 0; bank < 4 && ok; bank++)
                if (read_bank(fd, bank * 0x20, cur + bank * 0x20)) ok = 0;
            if (!ok) { msleep(200); continue; }
            if (have) {
                for (int r = 0; r < 128; r++)
                    if (cur[r] != prev[r])
                        printf("[reg] 0x%02x: %02x -> %02x\n", r, prev[r], cur[r]);
            }
            memcpy(prev, cur, sizeof prev);
            have = 1;
            fflush(stdout);
            msleep(150);
        }
    } else {
        fprintf(stderr, "usage: %s [get|regs|slic|slic-reset|watch]\n", argv[0]);
        return 2;
    }
    close(fd);
    return 0;
}
