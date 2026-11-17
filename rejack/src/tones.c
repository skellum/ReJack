/*
 * tones.c — see tones.h.
 */
#include <math.h>
#include <stdlib.h>
#include "tones.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct tone_def {
	double f[4];      /* component frequencies, 0 = unused */
	unsigned on_ms;   /* tone on per cycle */
	unsigned off_ms;  /* silence per cycle (0 = continuous) */
	double amp;       /* per-component amplitude, full scale = 1.0 */
};

static const struct tone_def *lookup(enum ph_tone t)
{
	/* ~ -13 dBm0 per component for the standard tones */
	static const struct tone_def dial     = { {350, 440},  1000,    0, 0.11 };
	static const struct tone_def ringback = { {440, 480},  2000, 4000, 0.07 };
	static const struct tone_def busy     = { {480, 620},   500,  500, 0.09 };
	static const struct tone_def reorder  = { {480, 620},   250,  250, 0.09 };
	static const struct tone_def howler   = { {1400, 2060, 2450, 2600}, 100, 100, 0.12 };

	switch (t) {
	case TONE_DIAL:     return &dial;
	case TONE_RINGBACK: return &ringback;
	case TONE_BUSY:     return &busy;
	case TONE_REORDER:  return &reorder;
	case TONE_HOWLER:   return &howler;
	case TONE_NONE:     break;
	}
	return NULL;
}

int tone_gen(enum ph_tone tone, unsigned srate, int16_t **out, size_t *nsamples)
{
	const struct tone_def *d = lookup(tone);
	if (!d || !srate || !out || !nsamples)
		return -1;

	size_t on  = (size_t)srate * d->on_ms / 1000;
	size_t off = (size_t)srate * d->off_ms / 1000;
	int16_t *b = calloc(on + off ? on + off : 1, sizeof(*b));
	if (!b)
		return -1;

	/* 5 ms raised-cosine ramps avoid clicks at cadence edges (not for continuous tones) */
	size_t ramp = d->off_ms ? (size_t)srate * 5 / 1000 : 0;

	for (size_t i = 0; i < on; i++) {
		double v = 0;
		for (int k = 0; k < 4 && d->f[k] > 0; k++)
			v += d->amp * sin(2 * M_PI * d->f[k] * (double)i / srate);
		if (ramp) {
			if (i < ramp)
				v *= 0.5 - 0.5 * cos(M_PI * (double)i / ramp);
			else if (i >= on - ramp)
				v *= 0.5 - 0.5 * cos(M_PI * (double)(on - 1 - i) / ramp);
		}
		b[i] = (int16_t)lrint(v * 32767);
	}

	*out = b;
	*nsamples = on + off;
	return 0;
}
