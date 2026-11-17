/*
 * phone.h — Rejack call state machine.
 *
 * Pure logic: no baresip, no hardware. Inputs are events (hook, digits, SIP call progress, timers);
 * outputs are calls through `struct ph_ops`. This keeps the behaviour unit-testable (test/test_phone.c).
 */
#ifndef REJACK_PHONE_H
#define REJACK_PHONE_H

#include <stdbool.h>
#include <stddef.h>

enum ph_state {
	PH_IDLE,          /* on-hook, no call */
	PH_DIALTONE,      /* off-hook, dial tone, no digits yet */
	PH_DIALING,       /* collecting digits */
	PH_CALLING,       /* outgoing call placed, waiting for answer */
	PH_RINGING,       /* incoming call, phone ringing, on-hook */
	PH_CONNECTED,     /* call up */
	PH_DISCONNECTED,  /* off-hook but no call (far end hung up, dial failed, timeout) */
	PH_EMERGENCY,     /* emergency call could not go through: local fallback (announce + record) */
};

/* Emergency notifications to the integration layer (see ops->emergency). */
enum ph_emerg {
	EMERG_DIALED,     /* 911 being dialed (verified or not) — notify contacts immediately */
	EMERG_FAILED,     /* 911 could not be placed/completed — start local fallback */
	EMERG_ENDED,      /* phone hung up after an emergency call or fallback */
};

enum ph_event {
	EV_OFFHOOK,
	EV_ONHOOK,
	EV_DIGIT,           /* arg: '0'-'9', '*', '#' */
	EV_TIMER,           /* the single phone timer fired (dial-tone / inter-digit timeout) */
	EV_INCOMING,        /* SIP: new incoming call */
	EV_REMOTE_RINGING,  /* SIP: 180 Ringing for our outgoing call */
	EV_REMOTE_PROGRESS, /* SIP: 183 with early media — provider plays its own ringback */
	EV_ESTABLISHED,     /* SIP: call is up (either direction) */
	EV_CLOSED,          /* SIP: call ended or failed */
};

enum ph_tone {
	TONE_NONE,
	TONE_DIAL,          /* 350+440 Hz continuous */
	TONE_RINGBACK,      /* 440+480 Hz, 2 s on / 4 s off */
	TONE_BUSY,          /* 480+620 Hz, 0.5 s on / 0.5 s off */
	TONE_REORDER,       /* 480+620 Hz, 0.25 s on / 0.25 s off (fast busy) */
	TONE_HOWLER,        /* off-hook warning */
};

struct ph_ops {
	void (*tone)(enum ph_tone tone, void *arg);
	void (*ring)(bool on, void *arg);             /* start/stop ring cadence */
	int  (*dial)(const char *digits, void *arg);  /* place outgoing call; 0 = ok */
	int  (*answer)(void *arg);                    /* answer the incoming call */
	void (*hangup)(void *arg);                    /* hang up / reject current call */
	void (*timer)(unsigned ms, void *arg);        /* (re)arm the phone timer; 0 = cancel */
	void (*dtmf)(char digit, void *arg);          /* digit pressed during a call (may be NULL) */
	bool (*e911_ok)(void *arg);                   /* is provider E911 verified right now? (may be NULL = no) */
	/* Emergency events; `verified` = E911 address verified; `reason` set for EMERG_FAILED. May be NULL. */
	void (*emergency)(enum ph_emerg ev, bool verified, const char *reason, void *arg);
};

/* Emergency-call policy. Emergency numbers are 911, 1911, 9911. In every mode the emergency
 * contacts are notified (EMERG_DIALED), and if the call can't go through the local fallback runs. */
enum ph_e911 {
	E911_ALLOW = 0,     /* default: always attempt 911 (provider may route unverified calls to a relay center) */
	E911_VERIFIED,      /* attempt 911 only while ops->e911_ok() says E911 is verified; else fallback only */
	E911_BLOCK,         /* never attempt 911; fallback only */
};

struct ph_config {
	unsigned dialtone_ms;   /* off-hook with no digits before reorder (default 15000) */
	unsigned interdigit_ms; /* pause that ends dialing (default 4000) */
	unsigned howler_ms;     /* reorder before howler (default 30000) */
	enum ph_e911 e911;      /* emergency-call policy (see PROJECT.md 911 policy) */
};

#define PH_MAX_DIGITS 32

struct phone {
	enum ph_state st;
	struct ph_config cfg;
	char   digits[PH_MAX_DIGITS + 1];
	size_t ndigits;
	bool   offhook;
	bool   have_call;    /* a SIP call object exists (incoming or outgoing) */
	bool   emergency;    /* current/last call attempt is an emergency call */
	const struct ph_ops *ops;
	void  *arg;
};

void phone_init(struct phone *ph, const struct ph_config *cfg,
		const struct ph_ops *ops, void *arg);
void phone_event(struct phone *ph, enum ph_event ev, char digit);

/* True if dialing `digits` now would be refused by the emergency policy. */
bool phone_e911_blocked(const struct phone *ph, const char *digits);
bool phone_is_emergency(const char *digits);

/* True when a new incoming SIP call should be accepted (phone idle and on-hook). */
bool phone_can_accept(const struct phone *ph);

const char *phone_state_str(enum ph_state st);
const char *phone_event_str(enum ph_event ev);

#endif
