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
 *   tjprobe gpio       Read-only. Decodes GPIO/AUX direction+level and clock dividers (Tiger560B map).
 *   tjprobe activate [snapshot.bin]
 *                      EXPERIMENT (writes): replay the TjIpSys "type 2" activation sequence
 *                      (0x00 C0->40, 0x02 20->00, 0x3C=32, 0x30..0x33=80 3E 00 7D, 0x55=B4),
 *                      skipping the 128-byte 0x57 clear. Saves all 128 registers first, prints a diff.
 *   tjprobe restore snapshot.bin
 *                      Writes back the activation-touched registers from a snapshot. (Replug = full reset.)
 *   tjprobe init580    Replays CTj580Phone_Hid::InitTjHardware() from magicJack's macOS driver exactly:
 *                      0x01=E1,59 (unlock) ... 0x01=FF,00 (lock). Read-modify-write where the driver does.
 *   tjprobe release    Ring off and the chip's automatic dial tone back on (standalone line). Used by systemd.
 *   tjprobe gvinit     Replays GVJackApp v2's PhoneJack init (Windows, 2013): like init580 but 0x32/0x33
 *                      written unconditionally, 0x3D=00, and the chip left unlocked.
 *   tjprobe ring [cycles]   CTj560CPhone_Hid::Ring(): 0x3D |= 0x30 for 2 s, then &= 0xCF, 4 s off.
 *   tjprobe hook       Poll the hook bit (0x58 bit 1 = off-hook) and the chip DTMF decoder (0x15, per GVJack).
 *   tjprobe write REG VAL   Write one TigerJet register (hex), print the inputs before/after.
 *   tjprobe inputs     Read-only one-liner: GIO/AUX levels + registers seen to change.
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


static int read_all(int fd, unsigned char *r /* 128 */) {
    for (int bank = 0; bank < 4; bank++)
        if (read_bank(fd, bank * 0x20, r + bank * 0x20)) return -1;
    return 0;
}

static void diff_regs(const unsigned char *a, const unsigned char *b) {
    int n = 0;
    for (int i = 0; i < 128; i++)
        if (a[i] != b[i]) { printf("  reg 0x%02x: %02x -> %02x\n", i, a[i], b[i]); n++; }
    if (!n) printf("  (no register changed)\n");
}

static void gio_pair(const char *tag, int v, int p0) {
    for (int k = 0; k < 4 && p0 + k <= 7; k++)
        printf("  GIO%-2d %s level %d\n", p0 + k, (v >> (2 * k)) & 1 ? "OUTPUT" : "input ", (v >> (2 * k + 1)) & 1);
    (void)tag;
}


static int rd(int fd, int reg) { unsigned char b[REPORT_LEN]; return read_bank(fd, reg & 0xE0, b) ? -1 : b[reg & 0x1F]; }
static int wr(int fd, int reg, int val) {
    unsigned char v = (unsigned char)val;
    int r = write_regs(fd, reg, &v, 1);
    printf("  0x%02x = 0x%02x  %s\n", reg, val & 0xff, r ? strerror(-r) : "ok");
    return r;
}
static int rmw(int fd, int reg, int and_mask, int or_mask) {
    int v = rd(fd, reg);
    if (v < 0) { printf("  read 0x%02x failed\n", reg); return -1; }
    printf("  (0x%02x was 0x%02x)", reg, v);
    return wr(fd, reg, (v & and_mask) | or_mask);
}

static const unsigned char act_regs[] = { 0x00, 0x02, 0x30, 0x31, 0x32, 0x33, 0x3C, 0x55 };

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
    } else if (!strcmp(mode, "gpio")) {
        unsigned char r[128];
        if (read_all(fd, r)) { printf("read failed\n"); return 1; }
        printf("# reg 0x0a=0x%02x 0x0f=0x%02x (GIO0-7: direction/level)\n", r[0x0a], r[0x0f]);
        gio_pair("", r[0x0a], 0); gio_pair("", r[0x0f], 4);
        for (int k = 0; k < 2; k++) {
            int v = r[0x10]; int p = 11 + k;
            printf("  GIO%-2d %s level %d src %d\n", p, (v >> (2*k)) & 1 ? "OUTPUT" : "input ", (v >> (2*k+1)) & 1, (v >> (4+2*k)) & 3);
        }
        for (int k = 0; k < 2; k++) {
            int v = r[0x11]; int p = 13 + k;
            printf("  GIO%-2d %s level %d src %d\n", p, (v >> (2*k)) & 1 ? "OUTPUT" : "input ", (v >> (2*k+1)) & 1, (v >> (4+2*k)) & 3);
        }
        for (int p = 8; p <= 10; p++) {
            int v = r[0x1a + p - 8];
            printf("  GIO%-2d %s level %d src %d (0 reg, 1 divA, 2 divB)\n", p, v & 1 ? "OUTPUT" : "input ", (v >> 1) & 1, (v >> 2) & 3);
        }
        printf("  AUX7..0 data 0x%02x dir 0x%02x (1 = output)\n", r[0x12], r[0x13]);
        printf("  clock divA ratio 0x%02x, divB ratio 0x%02x, ctl 0x%02x\n", r[0x17], r[0x18], r[0x19]);
        printf("  ctl 0x00 = 0x%02x (EXTRST# %d)\n", r[0x00], r[0x00] & 1);
    } else if (!strcmp(mode, "activate")) {
        const char *snap = argc > 2 ? argv[2] : "tjsnapshot.bin";
        unsigned char before[128], after[128];
        static const struct { unsigned char reg, val; int wait_ms; } seq[] = {
            { 0x00, 0xC0, 5 }, { 0x02, 0x20, 5 },
            /* 0x57 128-byte clear skipped: unknown whether 0x57 is a port or a range */
            { 0x00, 0x40, 10 }, { 0x02, 0x00, 5 },
            { 0x3C, 0x32, 2 }, { 0x30, 0x80, 2 }, { 0x31, 0x3E, 2 }, { 0x32, 0x00, 2 },
            { 0x33, 0x7D, 2 }, { 0x55, 0xB4, 20 },
        };
        if (read_all(fd, before)) { printf("snapshot read failed\n"); return 1; }
        FILE *f = fopen(snap, "wb");
        if (!f || fwrite(before, 1, 128, f) != 128) { perror(snap); return 1; }
        fclose(f);
        printf("# saved 128 registers to %s\n", snap);
        for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
            int r = write_regs(fd, seq[i].reg, &seq[i].val, 1);
            printf("  write 0x%02x = 0x%02x  %s\n", seq[i].reg, seq[i].val, r ? strerror(-r) : "ok");
            msleep(seq[i].wait_ms);
        }
        msleep(200);
        if (read_all(fd, after)) { printf("post-read failed (device re-enumerated?)\n"); return 1; }
        printf("# register changes:\n");
        diff_regs(before, after);
        printf("# Now check: dongle LED, cordless 'no line' indicator, dial tone/hum when off-hook.\n"
               "# Undo: tjprobe restore %s   (or unplug/replug the dongle)\n", snap);
    } else if (!strcmp(mode, "restore")) {
        unsigned char snapv[128], now[128];
        FILE *f = argc > 2 ? fopen(argv[2], "rb") : NULL;
        if (!f || fread(snapv, 1, 128, f) != 128) { fprintf(stderr, "usage: %s restore snapshot.bin\n", argv[0]); return 2; }
        fclose(f);
        for (size_t i = 0; i < sizeof act_regs; i++) {
            int r = write_regs(fd, act_regs[i], &snapv[act_regs[i]], 1);
            printf("  write 0x%02x = 0x%02x  %s\n", act_regs[i], snapv[act_regs[i]], r ? strerror(-r) : "ok");
        }
        if (!read_all(fd, now)) { printf("# remaining differences vs snapshot:\n"); diff_regs(snapv, now); }
    } else if (!strcmp(mode, "init580")) {
        /* CTj580Phone_Hid::InitTjHardware() (mjupdate x86_64 @ 0x10002b580), InitDTMFMemory skipped */
        unsigned char before[128], after[128];
        if (read_all(fd, before)) { printf("read failed\n"); return 1; }
        wr(fd, 0x01, 0xE1);  wr(fd, 0x01, 0x59);      /* unlock */
        wr(fd, 0x4E, 0x00);  wr(fd, 0x55, 0x80);
        wr(fd, 0x39, 0xA0);  wr(fd, 0x3C, 0x25);
        rmw(fd, 0x34, 0x90, 0x06);
        rmw(fd, 0x2F, 0xFE, 0x00);
        int c0 = rd(fd, 0x00);
        if (c0 >= 0 && (c0 & 0x04)) {
            rmw(fd, 0x32, 0xE0, 0x1C);
            rmw(fd, 0x33, 0x80, 0x79);
        } else printf("  (0x00 = 0x%02x, bit 2 clear: skipping 0x32/0x33)\n", c0);
        wr(fd, 0x01, 0xFF);  wr(fd, 0x01, 0x00);      /* lock */
        msleep(200);
        if (!read_all(fd, after)) { printf("# register changes:\n"); diff_regs(before, after); }
    } else if (!strcmp(mode, "release")) {
        /* Hand the line back to the chip's standalone mode (what Rejack's tjdev_release() does):
         * ring off, forced dial tone off, automatic dial tone on. Used by the systemd unit after
         * Rejack stops for any reason, including a crash. */
        int r3d = rd(fd, 0x3D), r34 = rd(fd, 0x34);
        if (r3d < 0 || r34 < 0) { printf("read failed\n"); return 1; }
        wr(fd, 0x3D, (unsigned char)(r3d & 0xCF));
        wr(fd, 0x34, (unsigned char)((r34 & ~0x40) | 0x20));
        printf("released: 0x3d 0x%02x -> 0x%02x, 0x34 0x%02x -> 0x%02x\n",
               r3d, r3d & 0xCF, r34, (r34 & ~0x40) | 0x20);
    } else if (!strcmp(mode, "gvinit")) {
        /* GVJackApp v2 (Windows, 2013) TigerJet/PhoneJack init @ 0x415aec: unconditional 0x32/0x33,
         * ring-off = 0x3D := 0x00, and the chip is left UNLOCKED (no 0x01=FF,00). */
        unsigned char before[128], after[128];
        if (read_all(fd, before)) { printf("read failed\n"); return 1; }
        wr(fd, 0x01, 0xE1);  wr(fd, 0x01, 0x59);      /* unlock */
        wr(fd, 0x55, 0x80);  wr(fd, 0x39, 0xA0);  wr(fd, 0x3C, 0x25);
        wr(fd, 0x3D, 0x00);                           /* RingOff */
        wr(fd, 0x32, 0x1C);  wr(fd, 0x33, 0x79);
        msleep(200);
        if (!read_all(fd, after)) { printf("# register changes:\n"); diff_regs(before, after); }
    } else if (!strcmp(mode, "ring")) {
        int cycles = argc > 2 ? atoi(argv[2]) : 1;
        for (int c = 0; c < cycles; c++) {
            printf("# ring ON  (cycle %d/%d)\n", c + 1, cycles); fflush(stdout);
            rmw(fd, 0x3D, 0xFF, 0x30);
            msleep(2000);
            printf("# ring OFF\n"); fflush(stdout);
            rmw(fd, 0x3D, 0xCF, 0x00);
            if (c + 1 < cycles) msleep(4000);
        }
    } else if (!strcmp(mode, "hook")) {
        int last = -1, lastd = 0;
        printf("# polling 0x58 bit1 (off-hook) + 0x15 DTMF (bit6 valid, low nibble digit) every 50 ms; Ctrl-C to stop\n");
        for (;;) {
            int h = rd(fd, 0x58), r9 = rd(fd, 0x09), d = rd(fd, 0x15);
            if (h < 0) { msleep(200); continue; }
            int now = (h >> 1) & 1;
            if (now != last) { printf("%s  (0x58=0x%02x, 0x09=0x%02x)\n", now ? "OFF-HOOK" : "on-hook ", h, r9); fflush(stdout); last = now; }
            /* GVJackApp: 0x15 & 0x40 = digit present, & 0x0F = code (MT8870 style: 1-9, 0xA='0', 0xB='*', 0xC='#') */
            int dv = d >= 0 && (d & 0x40);
            if (dv && !lastd) {
                static const char map[] = "D1234567890*#ABC";
                printf("DTMF '%c'  (0x15=0x%02x)\n", map[d & 0x0F], d); fflush(stdout);
            }
            lastd = dv;
            msleep(50);
        }
    } else if (!strcmp(mode, "inputs") || !strcmp(mode, "write")) {
        unsigned char r[128];
        if (!strcmp(mode, "write")) {
            if (argc < 4) { fprintf(stderr, "usage: %s write REG VAL (hex)\n", argv[0]); return 2; }
            unsigned reg = (unsigned)strtoul(argv[2], NULL, 16), val = (unsigned)strtoul(argv[3], NULL, 16);
            if (reg > 0x7f || val > 0xff) { fprintf(stderr, "reg 00-7f, val 00-ff\n"); return 2; }
            unsigned char v = (unsigned char)val;
            int w = write_regs(fd, (int)reg, &v, 1);
            printf("write 0x%02x = 0x%02x: %s\n", reg, val, w ? strerror(-w) : "ok");
            msleep(50);
        }
        if (read_all(fd, r)) { printf("read failed\n"); return 1; }
        int gio = 0;
        for (int k = 0; k < 4; k++) gio |= ((r[0x0a] >> (2*k+1)) & 1) << k;
        for (int k = 0; k < 4; k++) gio |= ((r[0x0f] >> (2*k+1)) & 1) << (4 + k);
        gio |= ((r[0x1a] >> 1) & 1) << 8 | ((r[0x1b] >> 1) & 1) << 9 | ((r[0x1c] >> 1) & 1) << 10;
        gio |= ((r[0x10] >> 1) & 1) << 11 | ((r[0x10] >> 3) & 1) << 12 | ((r[0x11] >> 1) & 1) << 13 | ((r[0x11] >> 3) & 1) << 14;
        printf("inputs: GIO14..0 ");
        for (int b = 14; b >= 0; b--) printf("%d%s", (gio >> b) & 1, b % 4 == 0 && b ? "_" : "");
        printf("  AUX7..0 ");
        for (int b = 7; b >= 0; b--) printf("%d", (r[0x12] >> b) & 1);
        printf("  | 00=%02x 02=%02x 15=%02x 30-33=%02x%02x%02x%02x 3c=%02x 58=%02x 5f=%02x\n",
               r[0x00], r[0x02], r[0x15], r[0x30], r[0x31], r[0x32], r[0x33], r[0x3c], r[0x58], r[0x5f]);
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
        fprintf(stderr, "usage: %s [get|regs|gpio|inputs|write R V|init580|ring [n]|hook|slic|slic-reset|watch|activate [snap]|restore snap]\n", argv[0]);
        return 2;
    }
    close(fd);
    return 0;
}
