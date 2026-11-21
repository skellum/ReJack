/*
 * linemon.c — line-down monitor (see linemon.h).
 * Copyright (c) 2026 Skellum. SPDX-License-Identifier: MIT (see ../../LICENSE)
 */
#include <string.h>
#include "linemon.h"

void linemon_init(struct linemon *m, uint64_t now, uint64_t down_ms, uint64_t stable_ms)
{
	memset(m, 0, sizeof(*m));
	m->down_ms = down_ms;
	m->stable_ms = stable_ms;
	m->down_since = now;
}

void linemon_lost(struct linemon *m, uint64_t now)
{
	m->attached = false;
	if (!m->down_since)
		m->down_since = now;
}

void linemon_attached(struct linemon *m, uint64_t now)
{
	m->attached = true;
	m->up_since = now;
}

enum lm_action linemon_check(struct linemon *m, uint64_t now, uint64_t *outage_ms)
{
	uint64_t out = m->down_since && now > m->down_since ? now - m->down_since : 0;

	if (outage_ms)
		*outage_ms = out;
	if (!m->down_since)
		return LM_NONE;

	if (m->attached && now - m->up_since >= m->stable_ms) {
		bool was = m->alerted;

		if (outage_ms)       /* the outage ended when the dongle came back for good */
			*outage_ms = m->up_since > m->down_since ? m->up_since - m->down_since : 0;
		m->down_since = 0;
		m->alerted = false;
		return was ? LM_ALERT_UP : LM_NONE;
	}
	if (m->down_ms && !m->alerted && out >= m->down_ms) {
		m->alerted = true;
		return LM_ALERT_DOWN;
	}
	return LM_NONE;
}
