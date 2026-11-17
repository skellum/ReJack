/*
 * tones.h — North American call-progress tones (ANSI T1.401 / Telcordia precise tones).
 * Pure C: generates one full cadence cycle of S16 mono samples for looping playback.
 */
#ifndef REJACK_TONES_H
#define REJACK_TONES_H

#include <stddef.h>
#include <stdint.h>
#include "phone.h"

/* Allocates *out (caller frees with free()). Returns 0, or -1 for TONE_NONE / bad args / no memory. */
int tone_gen(enum ph_tone tone, unsigned srate, int16_t **out, size_t *nsamples);

#endif
