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
#include "../src/e911.h"
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
	test_e911_status_file();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
