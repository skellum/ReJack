/*
 * dtmfdet.c — see dtmfdet.h.
 *
 * Per block, a digit is a hit when:
 *   - the strongest row and the strongest column are both above an absolute floor (about -40 dBFS per tone),
 *   - their levels are within DTMF_TWIST of each other (analog and cordless phones vary),
 *   - each beats the other tones of its group by DTMF_REL_PEAK (rejects speech and wide-band noise),
 *   - together they carry at least DTMF_TONE_SHARE of the block's energy (rejects tones mixed with other
 *     sound; dial tone may still be on the line during the first digit, so this is not strict).
 */
#include <math.h>
#include <string.h>
#include "dtmfdet.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float freqs[8] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
static const char  keys[4][4] = {
	{ '1', '2', '3', 'A' },
	{ '4', '5', '6', 'B' },
	{ '7', '8', '9', 'C' },
	{ '*', '0', '#', 'D' },
};

#define DTMF_TWIST      6.3f   /* 8 dB, power ratio */
#define DTMF_REL_PEAK   6.3f   /* 8 dB over the next tone in the same group */
#define DTMF_TONE_SHARE 0.35f
/* Tone of amplitude A (full scale 1.0) gives Goertzel power (A*N/2)^2; floor at A = 0.01 (-40 dBFS). */
#define DTMF_FLOOR      ((0.01f * DTMF_BLOCK / 2) * (0.01f * DTMF_BLOCK / 2))

static float coef[8];
static int   coef_ready;

void dtmfdet_init(struct dtmfdet *d)
{
	memset(d, 0, sizeof(*d));
	if (!coef_ready) {
		for (int i = 0; i < 8; i++)
			coef[i] = 2.0f * (float)cos(2.0 * M_PI * freqs[i] / DTMF_RATE);
		coef_ready = 1;
	}
}

static char block_result(const struct dtmfdet *d)
{
	float p[8];
	int r = 0, c = 4;

	for (int i = 0; i < 8; i++)
		p[i] = d->s1[i] * d->s1[i] + d->s2[i] * d->s2[i] - coef[i] * d->s1[i] * d->s2[i];
	for (int i = 1; i < 4; i++)
		if (p[i] > p[r])
			r = i;
	for (int i = 5; i < 8; i++)
		if (p[i] > p[c])
			c = i;

	if (p[r] < DTMF_FLOOR || p[c] < DTMF_FLOOR)
		return 0;
	if (p[c] > p[r] * DTMF_TWIST || p[r] > p[c] * DTMF_TWIST)
		return 0;
	for (int i = 0; i < 4; i++)
		if (i != r && p[i] * DTMF_REL_PEAK > p[r])
			return 0;
	for (int i = 4; i < 8; i++)
		if (i != c && p[i] * DTMF_REL_PEAK > p[c])
			return 0;
	/* a pure tone pair gives p[r] + p[c] = energy * N / 2 */
	if (p[r] + p[c] < DTMF_TONE_SHARE * d->energy * DTMF_BLOCK / 2)
		return 0;

	return keys[r][c - 4];
}

size_t dtmfdet_feed(struct dtmfdet *d, const int16_t *x, size_t n, char *out, size_t outsz)
{
	size_t nout = 0;

	for (size_t k = 0; k < n; k++) {
		float v = x[k] / 32768.0f;

		for (int i = 0; i < 8; i++) {
			float s = v + coef[i] * d->s1[i] - d->s2[i];
			d->s2[i] = d->s1[i];
			d->s1[i] = s;
		}
		d->energy += v * v;
		if (++d->n < DTMF_BLOCK)
			continue;

		char hit = block_result(d);

		if (hit && hit == d->last_hit && hit != d->current) {
			d->current = hit;
			d->misses = 0;
			if (nout < outsz)
				out[nout++] = hit;
		}
		else if (d->current && hit != d->current) {
			if (++d->misses >= 2) {
				d->current = 0;
				d->misses = 0;
			}
		}
		else if (hit == d->current) {
			d->misses = 0;
		}
		d->last_hit = hit;

		memset(d->s1, 0, sizeof(d->s1));
		memset(d->s2, 0, sizeof(d->s2));
		d->energy = 0;
		d->n = 0;
	}
	return nout;
}
