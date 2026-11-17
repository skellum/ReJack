/*
 * phone.c — Rejack call state machine. See phone.h.
 */
#include <string.h>
#include "phone.h"

static void set_tone(struct phone *ph, enum ph_tone t)
{
	if (ph->ops->tone)
		ph->ops->tone(t, ph->arg);
}

static void set_timer(struct phone *ph, unsigned ms)
{
	if (ph->ops->timer)
		ph->ops->timer(ms, ph->arg);
}

static void go(struct phone *ph, enum ph_state st)
{
	ph->st = st;
}

/* Off-hook with nothing useful happening: reorder now, howler later. */
static void disconnect(struct phone *ph, enum ph_tone t)
{
	go(ph, PH_DISCONNECTED);
	set_tone(ph, t);
	set_timer(ph, ph->cfg.howler_ms);
}

/* Back to idle from any off-hook state. */
static void emerg(struct phone *ph, enum ph_emerg ev, const char *reason)
{
	bool verified = ph->ops->e911_ok && ph->ops->e911_ok(ph->arg);

	if (ph->ops->emergency)
		ph->ops->emergency(ev, verified, reason, ph->arg);
}

/* 911 could not go through: keep the line open, announce, record, notify. */
static void emergency_fallback(struct phone *ph, const char *reason)
{
	set_timer(ph, 0);
	set_tone(ph, TONE_NONE);
	ph->have_call = false;
	go(ph, PH_EMERGENCY);
	emerg(ph, EMERG_FAILED, reason);
}

static void to_idle(struct phone *ph)
{
	if (ph->emergency) {
		ph->emergency = false;
		emerg(ph, EMERG_ENDED, NULL);
	}
	set_timer(ph, 0);
	set_tone(ph, TONE_NONE);
	if (ph->have_call && ph->ops->hangup)
		ph->ops->hangup(ph->arg);
	ph->have_call = false;
	ph->ndigits = 0;
	ph->digits[0] = '\0';
	go(ph, PH_IDLE);
}

bool phone_is_emergency(const char *d)
{
	return !strcmp(d, "911") || !strcmp(d, "1911") || !strcmp(d, "9911");
}

bool phone_e911_blocked(const struct phone *ph, const char *digits)
{
	if (!phone_is_emergency(digits))
		return false;
	switch (ph->cfg.e911) {
	case E911_ALLOW:    return false;
	case E911_BLOCK:    return true;
	case E911_VERIFIED: break;
	}
	/* E911_VERIFIED: */
	/* fail closed: no checker, or checker says no */
	return !(ph->ops->e911_ok && ph->ops->e911_ok(ph->arg));
}

static void place_call(struct phone *ph)
{
	set_timer(ph, 0);

	if (ph->ndigits == 0) {
		disconnect(ph, TONE_REORDER);
		return;
	}
	if (phone_is_emergency(ph->digits)) {
		ph->emergency = true;
		emerg(ph, EMERG_DIALED, NULL);
		if (phone_e911_blocked(ph, ph->digits)) {
			emergency_fallback(ph, ph->cfg.e911 == E911_BLOCK ?
					   "911 disabled by policy" : "E911 not verified");
			return;
		}
	}
	if (!ph->ops->dial || ph->ops->dial(ph->digits, ph->arg) != 0) {
		if (ph->emergency)
			emergency_fallback(ph, "could not place call");
		else
			disconnect(ph, TONE_REORDER);
		return;
	}
	ph->have_call = true;
	go(ph, PH_CALLING);
}

static void add_digit(struct phone *ph, char d)
{
	if (ph->ndigits < PH_MAX_DIGITS) {
		ph->digits[ph->ndigits++] = d;
		ph->digits[ph->ndigits] = '\0';
	}
}

void phone_init(struct phone *ph, const struct ph_config *cfg,
		const struct ph_ops *ops, void *arg)
{
	memset(ph, 0, sizeof(*ph));
	ph->ops = ops;
	ph->arg = arg;
	if (cfg)
		ph->cfg = *cfg;
	if (!ph->cfg.dialtone_ms)   ph->cfg.dialtone_ms   = 15000;
	if (!ph->cfg.interdigit_ms) ph->cfg.interdigit_ms = 4000;
	if (!ph->cfg.howler_ms)     ph->cfg.howler_ms     = 30000;
	ph->st = PH_IDLE;
}

bool phone_can_accept(const struct phone *ph)
{
	return ph->st == PH_IDLE && !ph->offhook && !ph->have_call;
}

void phone_event(struct phone *ph, enum ph_event ev, char digit)
{
	/* Hook state is tracked regardless of call state. */
	if (ev == EV_OFFHOOK) {
		if (ph->offhook)
			return;
		ph->offhook = true;
	}
	else if (ev == EV_ONHOOK) {
		if (!ph->offhook)
			return;
		ph->offhook = false;
	}

	switch (ph->st) {

	case PH_IDLE:
		if (ev == EV_OFFHOOK) {
			go(ph, PH_DIALTONE);
			set_tone(ph, TONE_DIAL);
			set_timer(ph, ph->cfg.dialtone_ms);
		}
		else if (ev == EV_INCOMING) {
			ph->have_call = true;
			go(ph, PH_RINGING);
			if (ph->ops->ring)
				ph->ops->ring(true, ph->arg);
		}
		break;

	case PH_RINGING:
		if (ev == EV_OFFHOOK) {
			if (ph->ops->ring)
				ph->ops->ring(false, ph->arg);
			if (ph->ops->answer && ph->ops->answer(ph->arg) == 0)
				go(ph, PH_CONNECTED);
			else {
				ph->have_call = false;
				disconnect(ph, TONE_REORDER);
			}
		}
		else if (ev == EV_CLOSED) {      /* caller gave up */
			if (ph->ops->ring)
				ph->ops->ring(false, ph->arg);
			ph->have_call = false;
			go(ph, PH_IDLE);
		}
		break;

	case PH_DIALTONE:
		if (ev == EV_DIGIT) {
			set_tone(ph, TONE_NONE);
			go(ph, PH_DIALING);
			if (digit == '#') {
				place_call(ph);
				break;
			}
			add_digit(ph, digit);
			set_timer(ph, ph->cfg.interdigit_ms);
		}
		else if (ev == EV_TIMER)
			disconnect(ph, TONE_REORDER);
		else if (ev == EV_ONHOOK)
			to_idle(ph);
		break;

	case PH_DIALING:
		if (ev == EV_DIGIT) {
			if (digit == '#') {
				place_call(ph);
				break;
			}
			add_digit(ph, digit);
			if (ph->ndigits >= PH_MAX_DIGITS)
				place_call(ph);
			else
				set_timer(ph, ph->cfg.interdigit_ms);
		}
		else if (ev == EV_TIMER)
			place_call(ph);
		else if (ev == EV_ONHOOK)
			to_idle(ph);
		break;

	case PH_CALLING:
		if (ev == EV_REMOTE_RINGING)
			set_tone(ph, TONE_RINGBACK);
		else if (ev == EV_REMOTE_PROGRESS)
			set_tone(ph, TONE_NONE);       /* far end supplies early media */
		else if (ev == EV_ESTABLISHED) {
			set_tone(ph, TONE_NONE);
			go(ph, PH_CONNECTED);
		}
		else if (ev == EV_CLOSED) {
			ph->have_call = false;
			if (ph->emergency)
				emergency_fallback(ph, "call failed before answer");
			else
				disconnect(ph, TONE_BUSY);
		}
		else if (ev == EV_ONHOOK)
			to_idle(ph);
		break;

	case PH_EMERGENCY:
		if (ev == EV_ONHOOK)
			to_idle(ph);
		break;

	case PH_CONNECTED:
		if (ev == EV_DIGIT && ph->ops->dtmf)
			ph->ops->dtmf(digit, ph->arg);
		else if (ev == EV_CLOSED) {
			ph->have_call = false;
			disconnect(ph, TONE_REORDER);
		}
		else if (ev == EV_ONHOOK)
			to_idle(ph);
		break;

	case PH_DISCONNECTED:
		if (ev == EV_TIMER)
			set_tone(ph, TONE_HOWLER);
		else if (ev == EV_ONHOOK)
			to_idle(ph);
		break;
	}
}

const char *phone_state_str(enum ph_state st)
{
	switch (st) {
	case PH_IDLE:         return "IDLE";
	case PH_DIALTONE:     return "DIALTONE";
	case PH_DIALING:      return "DIALING";
	case PH_CALLING:      return "CALLING";
	case PH_RINGING:      return "RINGING";
	case PH_CONNECTED:    return "CONNECTED";
	case PH_DISCONNECTED: return "DISCONNECTED";
	case PH_EMERGENCY:    return "EMERGENCY";
	}
	return "?";
}

const char *phone_event_str(enum ph_event ev)
{
	switch (ev) {
	case EV_OFFHOOK:         return "OFFHOOK";
	case EV_ONHOOK:          return "ONHOOK";
	case EV_DIGIT:           return "DIGIT";
	case EV_TIMER:           return "TIMER";
	case EV_INCOMING:        return "INCOMING";
	case EV_REMOTE_RINGING:  return "REMOTE_RINGING";
	case EV_REMOTE_PROGRESS: return "REMOTE_PROGRESS";
	case EV_ESTABLISHED:     return "ESTABLISHED";
	case EV_CLOSED:          return "CLOSED";
	}
	return "?";
}
