/*
 * dtmfscan — run the DTMF detector over a recording: 8 kHz S16_LE mono, WAV (44-byte header) or raw.
 *   arecord -D hw:TigerJet,0 -f S16_LE -r 8000 -c 1 line.wav     (dial on the phone, Ctrl-C)
 *   dtmfscan line.wav
 * Prints each digit with its time, plus the peak level per second so a too-quiet capture is obvious.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/dtmfdet.h"

int main(int argc, char **argv)
{
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
	struct dtmfdet d;
	int16_t buf[160];
	char got[8], all[256] = "";
	size_t n, total = 0, nall = 0;
	int peak = 0;

	if (!f) { perror(argv[1]); return 1; }
	if (argc > 1 && strstr(argv[1], ".wav"))
		fseek(f, 44, SEEK_SET);
	dtmfdet_init(&d);
	while ((n = fread(buf, sizeof(buf[0]), 160, f)) > 0) {
		for (size_t i = 0; i < n; i++) {
			int a = buf[i] < 0 ? -buf[i] : buf[i];
			if (a > peak) peak = a;
		}
		size_t k = dtmfdet_feed(&d, buf, n, got, sizeof(got));
		for (size_t i = 0; i < k; i++) {
			printf("%8.3f s  digit '%c'\n", (double)total / DTMF_RATE, got[i]);
			if (nall < sizeof(all) - 1) all[nall++] = got[i];
		}
		total += n;
		if (total % DTMF_RATE < n) {
			printf("%8.3f s  peak %5.1f dBFS\n", (double)total / DTMF_RATE,
			       peak ? 20 * log10(peak / 32768.0) : -99.0);
			peak = 0;
		}
	}
	printf("digits: %s\n", all);
	return 0;
}
