/*
 * dtmfdet.h — DTMF detector for 8 kHz S16 mono audio (Goertzel), pure C.
 *
 * Why: the chip's own decoder (reg 0x15) keeps the last digit after it is released and does not always
 * strobe "digit present", so a key pressed again (the same digit twice, or a call starting with the
 * previous call's last digit) leaves no trace there. Decoding the line audio does not have that gap.
 *
 * Blocks of DTMF_BLOCK samples (12.75 ms). A digit is reported once, after two consecutive blocks agree;
 * it must then be absent for two blocks before the same digit can be reported again.
 */
#ifndef REJACK_DTMFDET_H
#define REJACK_DTMFDET_H

#include <stddef.h>
#include <stdint.h>

#define DTMF_RATE  8000
#define DTMF_BLOCK 102

struct dtmfdet {
	float  s1[8], s2[8];   /* Goertzel state: 4 row + 4 column frequencies */
	float  energy;         /* sum of x^2 in the current block */
	size_t n;              /* samples in the current block */
	char   last_hit;       /* block result before this one ('\0' = none) */
	char   current;        /* digit currently held down and already reported */
	int    misses;         /* consecutive blocks without `current` */
};

void dtmfdet_init(struct dtmfdet *d);

/*
 * Feed samples. Each newly detected digit ('0'-'9', '*', '#', 'A'-'D') is written to out[] (up to
 * outsz); returns the number written.
 */
size_t dtmfdet_feed(struct dtmfdet *d, const int16_t *x, size_t n, char *out, size_t outsz);

#endif
