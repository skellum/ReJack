/*
 * test_phone.c — unit tests for the Rejack state machine, tone generator and HID decoding.
 * No baresip or hardware needed:  make -C test   (or via CMake: ctest)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/dtmfdet.h"
#include "../src/e911.h"
#include "../src/linemon.h"
#include "../src/phone.h"
#include "../src/tjdev.h"
#include "../src/tones.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- recording fake ops ---- */
static struct {
	enum ph_tone tone;
	int  ring;            /* -1 unknown, 0 off, 1 on */
	char dialed[64];
	int  dial_result;
	int  answers, hangups, dtmfs;
	unsigned timer_ms;
	char last_dtmf;
	bool e911;            /* what the fake checker reports */
	int  emerg_dialed, emerg_failed, emerg_ended;
	bool emerg_verified;
	char emerg_reason[64];
} rec;

static void f_tone(enum ph_tone t, void *a)      { (void)a; rec.tone = t; }
static void f_ring(bool on, void *a)             { (void)a; rec.ring = on; }
static int  f_dial(const char *d, void *a)       { (void)a; snprintf(rec.dialed, sizeof(rec.dialed), "%s", d); return rec.dial_result; }
static int  f_answer(void *a)                    { (void)a; rec.answers++; return 0; }
static void f_hangup(void *a)                    { (void)a; rec.hangups++; }
static void f_timer(unsigned ms, void *a)        { (void)a; rec.timer_ms = ms; }
static void f_dtmf(char d, void *a)              { (void)a; rec.dtmfs++; rec.last_dtmf = d; }
static bool f_e911(void *a)                      { (void)a; return rec.e911; }
static void f_emerg(enum ph_emerg ev, bool verified, const char *reason, void *a)
{
	(void)a;
	rec.emerg_verified = verified;
	if (ev == EMERG_DIALED) rec.emerg_dialed++;
	if (ev == EMERG_FAILED) {
		rec.emerg_failed++;
		snprintf(rec.emerg_reason, sizeof(rec.emerg_reason), "%s", reason ? reason : "");
	}
	if (ev == EMERG_ENDED)  rec.emerg_ended++;
}

static const struct ph_ops ops = { f_tone, f_ring, f_dial, f_answer, f_hangup, f_timer, f_dtmf, f_e911, f_emerg };
static const struct ph_ops ops_nochecker = { f_tone, f_ring, f_dial, f_answer, f_hangup, f_timer, f_dtmf, NULL, f_emerg };

static void setup_policy(struct phone *ph, enum ph_e911 pol, const struct ph_ops *o)
{
	struct ph_config c = { .e911 = pol };
	memset(&rec, 0, sizeof(rec));
	rec.ring = -1;
	phone_init(ph, &c, o, NULL);
}

static void setup(struct phone *ph, bool block_911)
{
	struct ph_config c = { .e911 = block_911 ? E911_BLOCK : E911_ALLOW };
	memset(&rec, 0, sizeof(rec));
	rec.ring = -1;
	phone_init(ph, &c, &ops, NULL);
}

static void dial(struct phone *ph, const char *s)
{
	for (; *s; s++)
		phone_event(ph, EV_DIGIT, *s);
}

static void test_outgoing_call(void)
{
	struct phone ph;
	setup(&ph, true);

	phone_event(&ph, EV_OFFHOOK, 0);
	CHECK(ph.st == PH_DIALTONE);
	CHECK(rec.tone == TONE_DIAL);
	CHECK(rec.timer_ms == 15000);

	dial(&ph, "555");
	CHECK(ph.st == PH_DIALING);
	CHECK(rec.tone == TONE_NONE);
	CHECK(rec.timer_ms == 4000);

	dial(&ph, "1234");
	phone_event(&ph, EV_TIMER, 0);                /* inter-digit timeout ends dialing */
	CHECK(ph.st == PH_CALLING);
	CHECK(!strcmp(rec.dialed, "5551234"));
	CHECK(rec.timer_ms == 0);

	phone_event(&ph, EV_REMOTE_RINGING, 0);
	CHECK(rec.tone == TONE_RINGBACK);
	phone_event(&ph, EV_ESTABLISHED, 0);
	CHECK(ph.st == PH_CONNECTED);
	CHECK(rec.tone == TONE_NONE);

	phone_event(&ph, EV_DIGIT, '7');               /* digit during call */
	CHECK(rec.dtmfs == 1 && rec.last_dtmf == '7');

	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(ph.st == PH_IDLE);
	CHECK(rec.hangups == 1);
}

static void test_pound_dials_immediately(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "100#");
	CHECK(ph.st == PH_CALLING);
	CHECK(!strcmp(rec.dialed, "100"));
}

static void test_far_end_hangs_up(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "100#");
	phone_event(&ph, EV_ESTABLISHED, 0);
	phone_event(&ph, EV_CLOSED, 0);
	CHECK(ph.st == PH_DISCONNECTED);
	CHECK(rec.tone == TONE_REORDER);
	phone_event(&ph, EV_TIMER, 0);
	CHECK(rec.tone == TONE_HOWLER);
	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(ph.st == PH_IDLE);
	CHECK(rec.tone == TONE_NONE);
	CHECK(rec.hangups == 0);                       /* call already gone: no double hangup */
}

static void test_busy(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "100#");
	phone_event(&ph, EV_CLOSED, 0);                /* 486 Busy / failure */
	CHECK(ph.st == PH_DISCONNECTED);
	CHECK(rec.tone == TONE_BUSY);
}

static void test_dial_failure(void)
{
	struct phone ph;
	setup(&ph, true);
	rec.dial_result = -1;
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "100#");
	CHECK(ph.st == PH_DISCONNECTED);
	CHECK(rec.tone == TONE_REORDER);
}

static void test_dialtone_timeout(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_OFFHOOK, 0);
	phone_event(&ph, EV_TIMER, 0);
	CHECK(ph.st == PH_DISCONNECTED);
	CHECK(rec.tone == TONE_REORDER);
}

static void test_911_blocked(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "911");
	phone_event(&ph, EV_TIMER, 0);
	CHECK(ph.st == PH_EMERGENCY);                  /* fallback, not a dead line */
	CHECK(rec.dialed[0] == '\0');                  /* never sent to SIP */
	CHECK(rec.emerg_dialed == 1 && rec.emerg_failed == 1);
	CHECK(!strcmp(rec.emerg_reason, "911 disabled by policy"));
	CHECK(rec.tone == TONE_NONE);                  /* announcement, not reorder */
	phone_event(&ph, EV_TIMER, 0);                 /* no howler in emergency mode */
	CHECK(ph.st == PH_EMERGENCY && rec.tone == TONE_NONE);
	phone_event(&ph, EV_INCOMING, 0);              /* line stays ours */
	CHECK(ph.st == PH_EMERGENCY);
	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(ph.st == PH_IDLE && rec.emerg_ended == 1);
}

static void test_911_default_always_attempts(void)
{
	struct phone ph;
	struct ph_config c = { 0 };                    /* default policy */
	CHECK(c.e911 == E911_ALLOW);
	memset(&rec, 0, sizeof(rec));
	phone_init(&ph, &c, &ops, NULL);
	rec.e911 = false;                              /* unverified... */
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "911");
	phone_event(&ph, EV_TIMER, 0);
	CHECK(ph.st == PH_CALLING);                    /* ...but 911 is still attempted */
	CHECK(!strcmp(rec.dialed, "911"));
	CHECK(rec.emerg_dialed == 1 && !rec.emerg_verified);
	CHECK(rec.emerg_failed == 0);
	phone_event(&ph, EV_ESTABLISHED, 0);
	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(rec.emerg_ended == 1);
	CHECK(rec.emerg_failed == 0);
}

static void test_911_fails_then_fallback(void)
{
	struct phone ph;
	struct ph_config c = { 0 };
	memset(&rec, 0, sizeof(rec));
	phone_init(&ph, &c, &ops, NULL);

	/* dial error (no network / phone server down) */
	rec.dial_result = -1;
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "911#");
	CHECK(ph.st == PH_EMERGENCY);
	CHECK(!strcmp(rec.emerg_reason, "could not place call"));
	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(rec.emerg_ended == 1);

	/* call rejected / failed before answer */
	memset(&rec, 0, sizeof(rec));
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "911#");
	CHECK(ph.st == PH_CALLING);
	phone_event(&ph, EV_CLOSED, 0);
	CHECK(ph.st == PH_EMERGENCY);
	CHECK(!strcmp(rec.emerg_reason, "call failed before answer"));
	CHECK(rec.tone == TONE_NONE);                  /* not busy tone */
	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(ph.st == PH_IDLE && rec.emerg_ended == 1);

	/* a normal call afterwards is not treated as emergency */
	memset(&rec, 0, sizeof(rec));
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "5551234#");
	phone_event(&ph, EV_CLOSED, 0);
	CHECK(ph.st == PH_DISCONNECTED && rec.tone == TONE_BUSY);
	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(rec.emerg_dialed == 0 && rec.emerg_ended == 0);
}

static void test_911_allowed(void)
{
	struct phone ph;
	setup(&ph, false);
	phone_event(&ph, EV_OFFHOOK, 0);
	dial(&ph, "911#");
	CHECK(ph.st == PH_CALLING);
	CHECK(!strcmp(rec.dialed, "911"));
}

static bool tries_911(struct phone *ph, const char *num)
{
	phone_event(ph, EV_OFFHOOK, 0);
	dial(ph, num);
	phone_event(ph, EV_DIGIT, '#');
	bool dialed = ph->st == PH_CALLING;
	phone_event(ph, EV_ONHOOK, 0);
	return dialed;
}

static void test_911_verified_policy(void)
{
	struct phone ph;

	setup_policy(&ph, E911_VERIFIED, &ops);
	rec.e911 = false;
	CHECK(!tries_911(&ph, "911"));                 /* locked while unverified */
	CHECK(!tries_911(&ph, "9911"));
	CHECK(tries_911(&ph, "5551234"));              /* normal calls unaffected */

	rec.e911 = true;
	CHECK(tries_911(&ph, "911"));                  /* unlocked once verified */

	rec.e911 = false;                              /* verification lapses -> locks again */
	CHECK(!tries_911(&ph, "911"));

	setup_policy(&ph, E911_VERIFIED, &ops_nochecker);
	CHECK(!tries_911(&ph, "911"));                 /* no checker = fail closed */

	setup_policy(&ph, E911_BLOCK, &ops);
	rec.e911 = true;
	CHECK(!tries_911(&ph, "911"));                 /* BLOCK ignores the checker */

	CHECK(phone_is_emergency("911") && phone_is_emergency("1911"));
	CHECK(!phone_is_emergency("9110") && !phone_is_emergency("411"));
}

static void test_incoming_answered(void)
{
	struct phone ph;
	setup(&ph, true);
	CHECK(phone_can_accept(&ph));
	phone_event(&ph, EV_INCOMING, 0);
	CHECK(ph.st == PH_RINGING);
	CHECK(rec.ring == 1);
	CHECK(!phone_can_accept(&ph));                 /* second call gets 486 */

	phone_event(&ph, EV_OFFHOOK, 0);
	CHECK(rec.ring == 0);
	CHECK(rec.answers == 1);
	CHECK(ph.st == PH_CONNECTED);

	phone_event(&ph, EV_ONHOOK, 0);
	CHECK(ph.st == PH_IDLE);
	CHECK(rec.hangups == 1);
}

static void test_incoming_caller_gives_up(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_INCOMING, 0);
	phone_event(&ph, EV_CLOSED, 0);
	CHECK(ph.st == PH_IDLE);
	CHECK(rec.ring == 0);
	CHECK(rec.hangups == 0);
	CHECK(phone_can_accept(&ph));
}

static void test_busy_while_offhook(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_OFFHOOK, 0);
	CHECK(!phone_can_accept(&ph));
}

static void test_duplicate_hook_events_ignored(void)
{
	struct phone ph;
	setup(&ph, true);
	phone_event(&ph, EV_ONHOOK, 0);                /* already on-hook */
	CHECK(ph.st == PH_IDLE);
	phone_event(&ph, EV_OFFHOOK, 0);
	rec.timer_ms = 1;
	phone_event(&ph, EV_OFFHOOK, 0);               /* repeated report */
	CHECK(ph.st == PH_DIALTONE);
	CHECK(rec.timer_ms == 1);                      /* not re-armed */
}

static void test_tones(void)
{
	int16_t *b;
	size_t n;

	CHECK(tone_gen(TONE_NONE, 8000, &b, &n) == -1);

	CHECK(tone_gen(TONE_DIAL, 8000, &b, &n) == 0);
	CHECK(n == 8000);                              /* 1 s continuous, loops seamlessly */
	{
		/* 350 + 440 Hz both have integer cycles in 1 s -> last sample flows into first */
		double peak = 0;
		for (size_t i = 0; i < n; i++)
			if (abs(b[i]) > peak) peak = abs(b[i]);
		CHECK(peak > 6000 && peak < 8000);         /* 2 x 0.11 FS */
	}
	free(b);

	CHECK(tone_gen(TONE_RINGBACK, 8000, &b, &n) == 0);
	CHECK(n == 48000);                             /* 2 s on + 4 s off */
	CHECK(b[20000] == 0 && b[47999] == 0);         /* silent part */
	free(b);

	CHECK(tone_gen(TONE_REORDER, 8000, &b, &n) == 0);
	CHECK(n == 4000);
	CHECK(b[0] == 0);                              /* ramped start */
	free(b);
}

static void test_hid_decode(void)
{
	struct tj_input in;
	uint8_t idle[2]    = { 0x00, 0x00 };
	uint8_t offhook[2] = { 0x00, 0x80 };
	uint8_t five[2]    = { 0x06, 0x80 };           /* keypad index 6 = '5' */
	uint8_t pound[2]   = { 0x0c, 0x80 };
	uint8_t flash[2]   = { 0x50, 0x80 };

	tjdev_decode(idle, &in);    CHECK(!in.offhook && in.key == 0);
	tjdev_decode(offhook, &in); CHECK(in.offhook && in.key == 0);
	tjdev_decode(five, &in);    CHECK(in.key == '5');
	tjdev_decode(pound, &in);   CHECK(in.key == '#');
	tjdev_decode(flash, &in);   CHECK(in.fnkey == 5 && in.key == 0);
}

/* Register-polled line state, values captured on hardware 2026-11-17 (RING.md). */
static void test_line_decode(void)
{
	struct tj_lstate st = { 0 };
	struct tj_line ln;
	const uint8_t ON = 0x01, OFF = 0x03;          /* reg 0x58 on-hook / off-hook */
	const uint8_t idle15 = 0x8a;                   /* 0x15 idle: bit 6 clear (stale code) */
	int changes = 0;

	/* DTMF map: 1 2 3 * 0 # as read from the chip */
	CHECK(tjdev_dtmf_char(0xc1) == '1');
	CHECK(tjdev_dtmf_char(0xc3) == '3');
	CHECK(tjdev_dtmf_char(0xcb) == '*');
	CHECK(tjdev_dtmf_char(0xca) == '0');
	CHECK(tjdev_dtmf_char(0xcc) == '#');
	CHECK(tjdev_dtmf_char(0x8a) == 0);             /* bit 6 clear = no digit */

	/* idle start: nothing reported */
	for (int i = 0; i < 5; i++) {
		tjdev_line_decode(&st, ON, idle15, &ln);
		CHECK(!ln.hook_changed && !ln.offhook && !ln.digit);
	}

	/* a 1-poll glitch is ignored */
	tjdev_line_decode(&st, OFF, idle15, &ln); CHECK(!ln.hook_changed);
	tjdev_line_decode(&st, ON, idle15, &ln);  CHECK(!ln.hook_changed);
	tjdev_line_decode(&st, ON, idle15, &ln);  CHECK(!ln.hook_changed);

	/* off-hook reported once, after TJ_HOOK_DEBOUNCE stable polls */
	for (int i = 0; i < TJ_HOOK_DEBOUNCE + 3; i++) {
		tjdev_line_decode(&st, OFF, idle15, &ln);
		changes += ln.hook_changed;
		if (i == TJ_HOOK_DEBOUNCE - 1)
			CHECK(ln.hook_changed && ln.offhook);
	}
	CHECK(changes == 1);

	/* TS52: bit 6 held with the new nibble; one digit, even if bit 6 stays set for several polls */
	tjdev_line_decode(&st, OFF, 0xc5, &ln); CHECK(ln.digit == '5');
	tjdev_line_decode(&st, OFF, 0xc5, &ln); CHECK(ln.digit == 0);
	tjdev_line_decode(&st, OFF, 0x85, &ln); CHECK(ln.digit == 0);
	for (int i = 0; i < TJ_DTMF_STROBE; i++)
		tjdev_line_decode(&st, OFF, 0x85, &ln);
	tjdev_line_decode(&st, OFF, 0xc5, &ln); CHECK(ln.digit == '5');   /* same key pressed again */
	tjdev_line_decode(&st, OFF, 0x85, &ln); CHECK(ln.digit == 0);

	/* VTech, trace 2026-11-21 (20 ms polls): 1 2 3 change only the nibble; 4 and 0 strobe bit 6 later */
	{
		static const uint8_t seq[] = { 0x81, 0x81, 0x82, 0x82, 0x83, 0x83, 0x84, 0x84, 0xc4, 0x84,
		                               0x8a, 0x8a, 0xca, 0x8a, 0x8a };
		char got[8] = { 0 };
		int n = 0;
		for (size_t i = 0; i < sizeof(seq); i++) {
			tjdev_line_decode(&st, OFF, seq[i], &ln);
			if (ln.digit && n < 7)
				got[n++] = ln.digit;
		}
		CHECK(!strcmp(got, "12340"));
	}
	/* the same key twice: the second press shows only as a bit-6 strobe */
	for (int i = 0; i < TJ_DTMF_STROBE; i++)
		tjdev_line_decode(&st, OFF, 0x8a, &ln);
	tjdev_line_decode(&st, OFF, 0xca, &ln); CHECK(ln.digit == '0');

	/* on-hook: no digits reported while on-hook */
	for (int i = 0; i < TJ_HOOK_DEBOUNCE; i++)
		tjdev_line_decode(&st, ON, 0x85, &ln);
	CHECK(ln.hook_changed && !ln.offhook);
	tjdev_line_decode(&st, ON, 0xc7, &ln); CHECK(ln.digit == 0);
	tjdev_line_decode(&st, ON, 0x83, &ln); CHECK(ln.digit == 0);        /* nibble change on-hook */

	/* phone already off-hook (and a stale digit latched) when Rejack starts */
	struct tj_lstate st2 = { 0 };
	changes = 0;
	for (int i = 0; i < TJ_HOOK_DEBOUNCE + 2; i++) {
		tjdev_line_decode(&st2, OFF, 0xc9, &ln);
		changes += ln.hook_changed;
		CHECK(ln.digit == 0);
	}
	CHECK(changes == 1 && ln.offhook);
}

static void write_status(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	fputs(content, f);
	fclose(f);
}

static void test_e911_status_file(void)
{
	char path[] = "/tmp/rejack-e911-XXXXXX", buf[128], why[200];
	const time_t now = 1800000000;
	const unsigned day = 26 * 3600;
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);

	snprintf(buf, sizeof(buf), "verified +1 (555) 123-4567 %lld\n", (long long)now - 3600);
	write_status(path, buf);   /* spaces inside the number -> malformed, must fail closed */
	CHECK(!e911_verified(path, "+15551234567", day, now, why, sizeof(why)));

	snprintf(buf, sizeof(buf), "verified 15551234567 %lld\n", (long long)now - 3600);
	write_status(path, buf);
	CHECK(e911_verified(path, "+15551234567", day, now, why, sizeof(why)));
	CHECK(e911_verified(path, "(555) 123-4567", day, now, NULL, 0));   /* formats normalise */
	CHECK(!e911_verified(path, "+15559999999", day, now, why, sizeof(why)));   /* other DID */
	CHECK(!e911_verified(path, "", day, now, why, sizeof(why)));              /* DID unset */
	CHECK(!e911_verified(path, NULL, day, now, why, sizeof(why)));
	CHECK(!e911_verified(path, "+15551234567", 1800, now, why, sizeof(why))); /* stale */

	snprintf(buf, sizeof(buf), "verified 15551234567 %lld\n", (long long)now + 86400);
	write_status(path, buf);
	CHECK(!e911_verified(path, "+15551234567", day, now, why, sizeof(why))); /* future */

	snprintf(buf, sizeof(buf), "unverified 15551234567 %lld\n", (long long)now);
	write_status(path, buf);
	CHECK(!e911_verified(path, "+15551234567", day, now, why, sizeof(why)));
	CHECK(strstr(why, "unverified") != NULL);

	write_status(path, "");
	CHECK(!e911_verified(path, "+15551234567", day, now, why, sizeof(why)));
	write_status(path, "verified\n");
	CHECK(!e911_verified(path, "+15551234567", day, now, why, sizeof(why)));

	unlink(path);
	CHECK(!e911_verified(path, "+15551234567", day, now, why, sizeof(why)));  /* missing */
}

static void test_linemon(void)
{
	struct linemon m;
	uint64_t out = 0;

	/* start-up: dongle attaches and stays -> no alert either way */
	linemon_init(&m, 1000, 120000, 30000);
	linemon_attached(&m, 1500);
	CHECK(linemon_check(&m, 2000, NULL) == LM_NONE);
	CHECK(linemon_check(&m, 31500, NULL) == LM_NONE);      /* stable: line up, silently */
	CHECK(m.down_since == 0);

	/* short unplug (moved to another port): back within the delay -> nothing sent */
	linemon_lost(&m, 100000);
	CHECK(linemon_check(&m, 150000, NULL) == LM_NONE);
	linemon_attached(&m, 160000);
	CHECK(linemon_check(&m, 190000, NULL) == LM_NONE);
	CHECK(m.down_since == 0 && !m.alerted);

	/* real outage: one alert after 2 min, not repeated */
	linemon_lost(&m, 200000);
	CHECK(linemon_check(&m, 319999, &out) == LM_NONE);
	CHECK(linemon_check(&m, 320000, &out) == LM_ALERT_DOWN);
	CHECK(out == 120000);
	CHECK(linemon_check(&m, 900000, NULL) == LM_NONE);

	/* flapping (attach, lose, attach...) keeps the outage open and its start time */
	linemon_attached(&m, 1000000);
	CHECK(linemon_check(&m, 1010000, NULL) == LM_NONE);
	linemon_lost(&m, 1015000);
	CHECK(m.down_since == 200000);
	linemon_attached(&m, 1100000);
	CHECK(linemon_check(&m, 1129999, NULL) == LM_NONE);
	/* stable for 30 s -> one "restored", outage measured to the final attach */
	CHECK(linemon_check(&m, 1130000, &out) == LM_ALERT_UP);
	CHECK(out == 1100000 - 200000);
	CHECK(linemon_check(&m, 1200000, NULL) == LM_NONE);

	/* flapping that never settles still alerts */
	linemon_lost(&m, 2000000);
	linemon_attached(&m, 2001000);
	linemon_lost(&m, 2010000);
	linemon_attached(&m, 2100000);
	CHECK(linemon_check(&m, 2120000, NULL) == LM_ALERT_DOWN);

	/* missing at start-up counts as an outage */
	linemon_init(&m, 5000, 120000, 30000);
	CHECK(linemon_check(&m, 125000, NULL) == LM_ALERT_DOWN);

	/* down_ms = 0 disables alerts */
	linemon_init(&m, 5000, 0, 30000);
	CHECK(linemon_check(&m, 99999999, NULL) == LM_NONE);
}

/* ---- DTMF detector on synthetic audio ---- */

struct sig { int16_t *x; size_t n, cap; double ph[4]; unsigned seed; };

static void sig_add(struct sig *s, const double *f, int nf, double amp_db, unsigned ms, double noise_db)
{
	size_t n = (size_t)DTMF_RATE * ms / 1000;
	if (s->n + n > s->cap) {
		s->cap = (s->n + n) * 2;
		s->x = realloc(s->x, s->cap * sizeof(*s->x));
	}
	double a = pow(10, amp_db / 20), na = pow(10, noise_db / 20);
	for (size_t i = 0; i < n; i++) {
		double v = 0;
		for (int k = 0; k < nf; k++) {
			v += a * sin(s->ph[k]);
			s->ph[k] += 2 * M_PI * f[k] / DTMF_RATE;
		}
		if (noise_db > -200) {         /* uniform noise of RMS na */
			s->seed = s->seed * 1103515245u + 12345u;
			v += na * sqrt(3.0) * (((s->seed >> 8) & 0xFFFF) / 32768.0 - 1.0);
		}
		if (v > 1) v = 1;
		if (v < -1) v = -1;
		s->x[s->n++] = (int16_t)lrint(v * 32767);
	}
}

static const double ROW[4] = { 697, 770, 852, 941 }, COL[4] = { 1209, 1336, 1477, 1633 };

static void sig_key(struct sig *s, char key, double db, double twist_db, double ferr, unsigned on, unsigned off,
		    double noise_db, const double *extra, int nextra)
{
	static const char *pad = "123A456B789C*0#D";
	int i = (int)(strchr(pad, key) - pad);
	double f[6] = { ROW[i / 4] * (1 + ferr), COL[i % 4] * (1 + ferr) };
	int nf = 2;
	for (int k = 0; k < nextra; k++)
		f[nf++] = extra[k];
	if (twist_db != 0) {            /* column tone twist_db louder than the row tone */
		double fr[1] = { f[0] }, fc[1] = { f[1] };
		size_t start = s->n;
		double ph0 = s->ph[0];
		sig_add(s, fr, 1, db, on, -300);
		size_t n = s->n - start;
		s->n = start;
		struct sig t = { 0 };
		t.ph[0] = s->ph[1];
		sig_add(&t, fc, 1, db + twist_db, on, -300);
		s->ph[0] = ph0;
		sig_add(s, fr, 1, db, on, noise_db);
		for (size_t k = 0; k < n; k++) {
			int v = s->x[start + k] + t.x[k];
			s->x[start + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		s->ph[1] = t.ph[0];
		free(t.x);
	}
	else
		sig_add(s, f, nf, db, on, noise_db);
	sig_add(s, NULL, 0, 0, off, noise_db);
}

static const char *detect(const struct sig *s, size_t chunk)
{
	static char got[64];
	struct dtmfdet d;
	size_t n = 0;

	dtmfdet_init(&d);
	for (size_t i = 0; i < s->n; i += chunk) {
		size_t len = s->n - i < chunk ? s->n - i : chunk;
		n += dtmfdet_feed(&d, s->x + i, len, got + n, sizeof(got) - 1 - n);
	}
	got[n] = '\0';
	return got;
}

static void test_dtmfdet(void)
{
	static const double dial[2] = { 350, 440 };
	struct sig s = { 0 };

	/* every key, -10 dBFS per tone, 100 ms on / 100 ms off, fed in 160-sample (20 ms) frames */
	for (const char *k = "1234567890*#ABCD"; *k; k++)
		sig_key(&s, *k, -10, 0, 0, 100, 100, -300, NULL, 0);
	CHECK(!strcmp(detect(&s, 160), "1234567890*#ABCD"));
	CHECK(!strcmp(detect(&s, 1), "1234567890*#ABCD"));            /* chunking doesn't matter */
	s.n = 0;

	/* the case the chip misses: the same key again (pre-dial bursts: 70 ms on, 50 ms off) */
	for (const char *k = "1110100"; *k; k++)
		sig_key(&s, *k, -12, 0, 0, 70, 50, -300, NULL, 0);
	CHECK(!strcmp(detect(&s, 160), "1110100"));
	s.n = 0;

	/* a long key press is one digit */
	sig_key(&s, '5', -10, 0, 0, 1500, 200, -300, NULL, 0);
	CHECK(!strcmp(detect(&s, 160), "5"));
	s.n = 0;

	/* quiet (-35 dBFS), twist of 6 dB either way, 1.5 % frequency error, noise 20 dB below */
	sig_key(&s, '1', -35, 0, 0, 80, 80, -300, NULL, 0);
	sig_key(&s, '2', -12, 6, 0, 80, 80, -300, NULL, 0);
	sig_key(&s, '3', -12, -6, 0, 80, 80, -300, NULL, 0);
	sig_key(&s, '4', -12, 0, 0.015, 80, 80, -300, NULL, 0);
	sig_key(&s, '5', -12, 0, -0.015, 80, 80, -300, NULL, 0);
	sig_key(&s, '6', -12, 0, 0, 80, 80, -32, NULL, 0);
	CHECK(!strcmp(detect(&s, 160), "123456"));
	s.n = 0;

	/* first digit dialled over dial tone at the same level */
	sig_add(&s, dial, 2, -13, 300, -300);
	sig_key(&s, '9', -13, 0, 0, 100, 100, -300, dial, 2);
	CHECK(!strcmp(detect(&s, 160), "9"));
	s.n = 0;

	/* not digits: too short (20 ms), too quiet (-50 dBFS), one tone only, dial tone, noise, 4 % off */
	sig_key(&s, '1', -10, 0, 0, 20, 100, -300, NULL, 0);
	sig_key(&s, '2', -50, 0, 0, 100, 100, -300, NULL, 0);
	{ double f1[1] = { 770 }; sig_add(&s, f1, 1, -10, 200, -300); }
	sig_add(&s, dial, 2, -10, 500, -300);
	sig_add(&s, NULL, 0, 0, 500, -15);
	sig_key(&s, '3', -10, 0, 0.04, 100, 100, -300, NULL, 0);
	{ double f3[3] = { 697, 1209, 1336 }; sig_add(&s, f3, 3, -12, 200, -300); }   /* two columns */
	CHECK(!strcmp(detect(&s, 160), ""));
	free(s.x);
}

int main(void)
{
	test_outgoing_call();
	test_pound_dials_immediately();
	test_far_end_hangs_up();
	test_busy();
	test_dial_failure();
	test_dialtone_timeout();
	test_911_blocked();
	test_911_allowed();
	test_911_verified_policy();
	test_911_default_always_attempts();
	test_911_fails_then_fallback();
	test_incoming_answered();
	test_incoming_caller_gives_up();
	test_busy_while_offhook();
	test_duplicate_hook_events_ignored();
	test_tones();
	test_hid_decode();
	test_line_decode();
	test_e911_status_file();
	test_linemon();
	test_dtmfdet();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
