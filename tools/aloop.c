/*
 * aloop.c — Phase 2 audio test for the magicJack A921 (ALSA card "TigerJet").
 *
 *   aloop levels [secs]        capture only; print peak/RMS (dBFS) every 250 ms
 *   aloop tone HZ [secs]       play a sine (-12 dBFS) to the line while metering capture
 *   aloop dialtone [secs]      play US dial tone (350+440 Hz) while metering capture
 *   aloop loop [secs]          full duplex: capture -> playback (hear yourself in the earpiece)
 *
 * Options (before the mode):  -D <pcm>  (default hw:TigerJet,0)   -r <rate> (default 8000)
 *                             -g <dB>   loop gain (default -6; <= 0 keeps the line echo from howling)
 *
 * Runs capture and playback in one thread with small periods (10 ms) and reports xruns.
 * Rejack proper will put this loop on a SCHED_FIFO thread; this tool tries to do the same
 * and falls back silently if not permitted.
 *
 * Build:  gcc -O2 -Wall -o aloop aloop.c -lasound -lm
 */
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD_MS 10

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir, unsigned rate,
                           snd_pcm_uframes_t *period) {
    snd_pcm_t *h;
    int err = snd_pcm_open(&h, dev, dir, 0);
    if (err < 0) {
        fprintf(stderr, "open %s (%s): %s\n", dev, dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
                snd_strerror(err));
        exit(1);
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(h, hw);
    snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(h, hw, 1);
    unsigned r = rate;
    snd_pcm_hw_params_set_rate_near(h, hw, &r, 0);
    snd_pcm_uframes_t p = rate * PERIOD_MS / 1000, buf = p * 4;
    snd_pcm_hw_params_set_period_size_near(h, hw, &p, 0);
    snd_pcm_hw_params_set_buffer_size_near(h, hw, &buf);
    if ((err = snd_pcm_hw_params(h, hw)) < 0) {
        fprintf(stderr, "hw_params: %s\n", snd_strerror(err));
        exit(1);
    }
    if (r != rate) { fprintf(stderr, "rate %u not supported (got %u)\n", rate, r); exit(1); }
    *period = p;
    fprintf(stderr, "# %s: %u Hz, period %lu frames, buffer %lu frames\n",
            dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture ", r, p, buf);
    return h;
}

static double dbfs(double v) { return v > 0 ? 20 * log10(v / 32768.0) : -120.0; }

/* Read exactly n frames, recovering from overruns. Returns frames read or -1 to stop. */
static long cap_read(snd_pcm_t *c, int16_t *b, snd_pcm_uframes_t n, int *xruns) {
    for (;;) {
        snd_pcm_sframes_t got = snd_pcm_readi(c, b, n);
        if (got >= 0) return got;
        if (stop) return -1;
        if (got == -EPIPE) (*xruns)++;
        if (snd_pcm_recover(c, (int)got, 1) < 0) return -1;
    }
}

static void play_write(snd_pcm_t *p, const int16_t *b, snd_pcm_uframes_t n, int *xruns) {
    while (n && !stop) {
        snd_pcm_sframes_t w = snd_pcm_writei(p, b, n);
        if (w < 0) {
            if (w == -EPIPE) (*xruns)++;
            if (snd_pcm_recover(p, (int)w, 1) < 0) return;
            continue;
        }
        b += w; n -= w;
    }
}

int main(int argc, char **argv) {
    const char *dev = "hw:TigerJet,0";
    unsigned rate = 8000;
    double gain_db = -6;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a += 2) {
        if (a + 1 >= argc) break;
        if (!strcmp(argv[a], "-D")) dev = argv[a + 1];
        else if (!strcmp(argv[a], "-r")) rate = (unsigned)atoi(argv[a + 1]);
        else if (!strcmp(argv[a], "-g")) gain_db = atof(argv[a + 1]);
    }
    if (a >= argc) {
        fprintf(stderr, "usage: %s [-D pcm] [-r rate] [-g loop_dB] levels|tone HZ|dialtone|loop [secs]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[a++];
    double hz = 0;
    if (!strcmp(mode, "tone")) {
        if (a >= argc) { fprintf(stderr, "tone needs a frequency\n"); return 2; }
        hz = atof(argv[a++]);
    }
    double secs = a < argc ? atof(argv[a]) : 10;
    int play = strcmp(mode, "levels") != 0;
    int loop = !strcmp(mode, "loop");
    int dial = !strcmp(mode, "dialtone");
    if (strcmp(mode, "levels") && strcmp(mode, "tone") && !loop && !dial) {
        fprintf(stderr, "unknown mode %s\n", mode); return 2;
    }

    signal(SIGINT, on_sig);
    struct sched_param sp = { .sched_priority = 50 };
    int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    fprintf(stderr, "# device %s, mode %s, %s\n", dev, mode, rt ? "SCHED_FIFO" : "normal priority");

    snd_pcm_uframes_t per;
    snd_pcm_t *cap = open_pcm(dev, SND_PCM_STREAM_CAPTURE, rate, &per);
    snd_pcm_t *pb = NULL;
    snd_pcm_uframes_t pper = per;
    if (play) pb = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, rate, &pper);
    if (pper != per) { fprintf(stderr, "period mismatch %lu vs %lu\n", per, pper); return 1; }

    double gain = pow(10, gain_db / 20);
    if (loop) fprintf(stderr, "# loop gain %.1f dB\n", gain_db);
    int16_t *buf = calloc(per, sizeof *buf), *out = calloc(per, sizeof *out);
    int cx = 0, px = 0;
    double phase1 = 0, phase2 = 0;
    long total = 0, limit = (long)(secs * rate);
    double sumsq = 0; int peak = 0; long acc = 0, report = rate / 4;

    if (pb) {   /* prime playback with two periods of silence to absorb jitter */
        play_write(pb, out, per, &px);
        play_write(pb, out, per, &px);
    }
    snd_pcm_start(cap);

    printf("#   time   cap_peak_dBFS  cap_rms_dBFS  xrun_cap  xrun_play\n");
    while (!stop && total < limit) {
        long n = cap_read(cap, buf, per, &cx);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            int v = abs(buf[i]);
            if (v > peak) peak = v;
            sumsq += (double)buf[i] * buf[i];
        }
        if (pb) {
            for (long i = 0; i < n; i++) {
                if (loop) {
                    double v = buf[i] * gain;
                    out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
                else if (dial) {
                    out[i] = (int16_t)(4000 * (sin(phase1) + sin(phase2)));  /* ~ -12 dBFS sum */
                    phase1 += 2 * M_PI * 350 / rate; phase2 += 2 * M_PI * 440 / rate;
                } else {
                    out[i] = (int16_t)(8192 * sin(phase1));                  /* -12 dBFS */
                    phase1 += 2 * M_PI * hz / rate;
                }
            }
            phase1 = fmod(phase1, 2 * M_PI); phase2 = fmod(phase2, 2 * M_PI);
            play_write(pb, out, n, &px);
        }
        total += n; acc += n;
        if (acc >= report) {
            printf("  %6.2f   %8.1f      %8.1f     %4d      %4d\n", (double)total / rate,
                   dbfs(peak), dbfs(sqrt(sumsq / acc)), cx, px);
            fflush(stdout);
            acc = 0; sumsq = 0; peak = 0;
        }
    }
    if (pb) { snd_pcm_drop(pb); snd_pcm_close(pb); }
    snd_pcm_drop(cap); snd_pcm_close(cap);
    printf("# done: %.2f s, capture xruns %d, playback xruns %d\n", (double)total / rate, cx, px);
    return 0;
}
