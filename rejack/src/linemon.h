/*
 * linemon.h — "is the phone line usable?" monitor for the line-down alert.
 *
 * Pure logic, no I/O: the caller reports dongle loss/attach and calls linemon_check() periodically
 * with a millisecond clock. The line counts as down from the first loss (or from start-up) until the
 * dongle has stayed attached for `stable_ms`, so a dongle that keeps dropping off and re-enumerating
 * is still "down". One alert per outage after `down_ms`, and one "restored" only if an alert was sent.
 *
 * Copyright (c) 2026 Skellum. SPDX-License-Identifier: MIT (see ../../LICENSE)
 */
#ifndef REJACK_LINEMON_H
#define REJACK_LINEMON_H

#include <stdbool.h>
#include <stdint.h>

struct linemon {
	uint64_t down_ms;     /* outage length before alerting (0 = alerts disabled) */
	uint64_t stable_ms;   /* attached this long = line back */
	uint64_t down_since;  /* 0 = line up */
	uint64_t up_since;    /* when the dongle last attached */
	bool     attached;
	bool     alerted;     /* a "down" alert went out for this outage */
};

enum lm_action { LM_NONE, LM_ALERT_DOWN, LM_ALERT_UP };

/* Starts "down" (the dongle isn't open yet). `now` must be > 0. */
void linemon_init(struct linemon *m, uint64_t now, uint64_t down_ms, uint64_t stable_ms);
void linemon_lost(struct linemon *m, uint64_t now);
void linemon_attached(struct linemon *m, uint64_t now);
/* What to send now, if anything. `outage_ms` (optional) gets the outage length so far. */
enum lm_action linemon_check(struct linemon *m, uint64_t now, uint64_t *outage_ms);

#endif
