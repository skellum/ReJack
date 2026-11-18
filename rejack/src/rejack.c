/**
 * @file rejack.c  Rejack — magicJack A921 FXS port as a baresip phone.
 *
 * baresip "application" module that turns the analog phone on the dongle into the user interface:
 *   - hook (reg 0x58) + DTMF (reg 0x15), polled  -> call state machine (phone.c)
 *   - dial tone                                  -> chip tone generator (reg 0x34), or software
 *   - other call-progress tones (ringback, busy) -> baresip player on the dongle's ALSA device
 *   - ring cadence                               -> tjdev_ring() (reg 0x3D)
 *   - SIP events (incoming, ringing, answered)   -> state machine
 *
 * Load it INSTEAD of menu.so (menu also answers calls and plays ring tones).
 *
 * Config (baresip `config` file):
 \verbatim
  rejack_hid         auto              # hidraw node, or "auto" to find 06e6:c200
  rejack_audio_mod   alsa              # player module for tones
  rejack_audio_dev   hw:TigerJet,0     # player device for tones
  rejack_account     <aor>             # account to dial from (default: first UA)
  rejack_interdigit  4000              # ms pause that ends dialing
  rejack_911         allow             # allow (default) | verified | block   (see below)
  rejack_e911_did    +15551234567      # number 911 calls present (must match the checker's status)
  rejack_e911_status /var/lib/rejack/e911   # status file written by the provider checker
  rejack_e911_max_age 26               # hours a successful check stays valid
  rejack_emergency_hook     /usr/local/bin/rejack-emergency   # notify script (see scripts/)
  rejack_emergency_announce /usr/local/share/rejack/emergency.wav
  rejack_record_dir  /var/lib/rejack/recordings
  rejack_record_dev  hw:TigerJet,0     # capture device for the fallback recording
  rejack_dtmf        inband            # inband | send  (send = also signal digits to the far end)
  rejack_dialtone    hw                # hw (chip generator, default) | sw (baresip player)
 \endverbatim
 *
 * 911 policy (PROJECT.md): "allow" (default) always attempts 911. Every 911 runs the emergency hook
 * ("dialed"). If 911 can't go through — no network, call fails, or policy "verified"/"block" refuses
 * it — the line stays open: announcement, local recording, hook ("failed"), and on hang-up ("ended").
 * "verified" attempts 911 only while the status file says `verified <did> <time>` for
 * rejack_e911_did and is younger than rejack_e911_max_age.
 * Copyright (c) 2026 Skellum. SPDX-License-Identifier: MIT (see ../../LICENSE)
 */
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include "e911.h"
#include "phone.h"
#include "tjdev.h"
#include "tones.h"

extern char **environ;

#define RING_ON_MS   2000
#define RING_OFF_MS  4000
#define TONE_SRATE   8000
#define POLL_MS      20      /* hook/DTMF register poll */

struct rejack {
	struct tjdev  *dev;
	struct re_fhs *fhs;
	struct phone   ph;
	struct tmr     tmr_phone;    /* dial-tone / inter-digit / howler timer */
	struct tmr     tmr_ring;     /* ring cadence */
	bool           ring_active;  /* cadence running */
	bool           ring_phase;   /* true = ringing burst on */
	bool           ring_warned;  /* logged tjdev_ring() failure once */
	struct play   *play;         /* current call-progress tone */
	struct ua     *ua;           /* UA of the current call */
	struct call   *call;         /* current call, or NULL */
	struct tmr     tmr_poll;     /* hook/DTMF register poll */
	bool           poll_warned;
	bool           hw_dialtone;  /* dial tone from the chip (rejack_dialtone hw) */
	bool           hw_dt_on;

	char hid[64];
	char audio_mod[32];
	char audio_dev[64];
	char account[256];
	bool send_dtmf;
	char emerg_hook[256];
	char emerg_announce[256];
	char record_dir[256];
	char record_dev[64];
	char e911_did[32];
	char e911_status[256];
	uint32_t e911_max_age_h;
};

static struct rejack rj;


/* ---- phone ops -------------------------------------------------------- */

static void op_tone(enum ph_tone tone, void *arg)
{
	struct mbuf *mb;
	int16_t *pcm;
	size_t n;
	int err;
	(void)arg;

	rj.play = mem_deref(rj.play);
	if (rj.hw_dt_on && tone != TONE_DIAL) {
		(void)tjdev_dialtone(rj.dev, false);
		rj.hw_dt_on = false;
	}
	if (tone == TONE_NONE)
		return;
	if (tone == TONE_DIAL && rj.hw_dialtone) {
		int e = tjdev_dialtone(rj.dev, true);
		if (!e) {
			rj.hw_dt_on = true;
			return;
		}
		warning("rejack: hardware dial tone failed (%m), using software\n", -e);
	}

	if (tone_gen(tone, TONE_SRATE, &pcm, &n)) {
		warning("rejack: tone_gen(%d) failed\n", tone);
		return;
	}
	mb = mbuf_alloc(n * sizeof(*pcm));
	if (!mb) {
		free(pcm);
		return;
	}
	(void)mbuf_write_mem(mb, (const uint8_t *)pcm, n * sizeof(*pcm));
	mb->pos = 0;
	free(pcm);

	/* repeat -1 = loop until stopped */
	err = play_tone(&rj.play, baresip_player(), mb, TONE_SRATE, 1, -1,
			rj.audio_mod, rj.audio_dev);
	if (err)
		warning("rejack: play_tone failed (%m)\n", err);
	mem_deref(mb);
}

static void ring_tick(void *arg)
{
	int err;
	(void)arg;

	if (!rj.ring_active)
		return;

	rj.ring_phase = !rj.ring_phase;
	err = tjdev_ring(rj.dev, rj.ring_phase);
	if (err && !rj.ring_warned) {
		warning("rejack: ring register write failed (%m)\n", -err);
		rj.ring_warned = true;
	}
	tmr_start(&rj.tmr_ring, rj.ring_phase ? RING_ON_MS : RING_OFF_MS, ring_tick, NULL);
}

static void op_ring(bool on, void *arg)
{
	(void)arg;

	tmr_cancel(&rj.tmr_ring);
	rj.ring_active = on;
	rj.ring_phase  = false;
	if (on) {
		info("rejack: ringing\n");
		ring_tick(NULL);         /* starts with a burst */
	}
	else {
		(void)tjdev_ring(rj.dev, false);
	}
}

static struct ua *dial_ua(void)
{
	struct le *le;

	if (rj.account[0]) {
		struct ua *ua = uag_find_aor(rj.account);
		if (ua)
			return ua;
		warning("rejack: account '%s' not found, using first UA\n", rj.account);
	}
	le = list_head(uag_list());
	return le ? le->data : NULL;
}

static int op_dial(const char *digits, void *arg)
{
	struct ua *ua = dial_ua();
	char *uri = NULL;
	struct pl pl;
	int err;
	(void)arg;

	if (!ua) {
		warning("rejack: no user agent configured\n");
		return ENOENT;
	}

	pl_set_str(&pl, digits);
	err = account_uri_complete_strdup(ua_account(ua), &uri, &pl);
	if (err)
		return err;

	info("rejack: dialing %s\n", uri);
	op_tone(TONE_NONE, NULL);      /* free the ALSA device for call audio */
	err = ua_connect(ua, &rj.call, NULL, uri, VIDMODE_OFF);
	if (!err)
		rj.ua = ua;
	else
		warning("rejack: ua_connect failed (%m)\n", err);

	mem_deref(uri);
	return err;
}

static int op_answer(void *arg)
{
	(void)arg;

	if (!rj.call)
		return ENOENT;
	op_tone(TONE_NONE, NULL);
	info("rejack: answering\n");
	return ua_answer(rj.ua, rj.call, VIDMODE_OFF);
}

static void op_hangup(void *arg)
{
	(void)arg;

	if (!rj.call)
		return;
	info("rejack: hanging up\n");
	ua_hangup(rj.ua, rj.call, 0, NULL);
	rj.call = NULL;
	rj.ua   = NULL;
}

static void phone_timer_handler(void *arg)
{
	(void)arg;
	phone_event(&rj.ph, EV_TIMER, 0);
}

static void op_timer(unsigned ms, void *arg)
{
	(void)arg;

	if (ms)
		tmr_start(&rj.tmr_phone, ms, phone_timer_handler, NULL);
	else
		tmr_cancel(&rj.tmr_phone);
}

static void op_dtmf(char digit, void *arg)
{
	(void)arg;

	/* The analog phone's DTMF is already in the audio path; only signal it
	 * out-of-band when configured (and when the provider needs RFC 4733). */
	if (!rj.send_dtmf || !rj.call)
		return;
	(void)call_send_digit(rj.call, digit);
	(void)call_send_digit(rj.call, KEYCODE_REL);
}

static bool op_e911_ok(void *arg)
{
	char why[200];
	bool ok;
	(void)arg;

	ok = e911_verified(rj.e911_status, rj.e911_did, rj.e911_max_age_h * 3600,
			   time(NULL), why, sizeof(why));
	if (ok)
		info("rejack: E911 verified: %s\n", why);
	else
		warning("rejack: E911 NOT verified: %s\n", why);
	return ok;
}


/* ---- emergency fallback: announcement, local recording, notification hook ---- */

#define MAX_KIDS 8
static pid_t kids[MAX_KIDS];
static struct tmr tmr_reap;
static pid_t recorder;               /* arecord child while recording, else 0 */
static char  recording[512];         /* path of the current/last emergency recording */

static void reap(void *arg)
{
	bool any = false;
	(void)arg;

	for (int i = 0; i < MAX_KIDS; i++) {
		if (!kids[i])
			continue;
		if (waitpid(kids[i], NULL, WNOHANG) == kids[i]) {
			if (kids[i] == recorder)
				recorder = 0;
			kids[i] = 0;
		}
		else
			any = true;
	}
	if (any)
		tmr_start(&tmr_reap, 2000, reap, NULL);
}

/* Start a child without waiting for it. Returns pid or 0. */
static pid_t spawn(char *const argv[])
{
	pid_t pid;
	int err = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);

	if (err) {
		warning("rejack: cannot run %s (%m)\n", argv[0], err);
		return 0;
	}
	for (int i = 0; i < MAX_KIDS; i++) {
		if (!kids[i]) {
			kids[i] = pid;
			break;
		}
	}
	tmr_start(&tmr_reap, 2000, reap, NULL);
	return pid;
}

/* rejack_emergency_hook <event> <verified|unverified> <reason|-> <recording|-> */
static void run_hook(const char *event, bool verified, const char *reason, const char *rec)
{
	char *argv[] = { rj.emerg_hook, (char *)event, verified ? "verified" : "unverified",
			 (char *)(reason && *reason ? reason : "-"),
			 (char *)(rec && *rec ? rec : "-"), NULL };

	if (!rj.emerg_hook[0]) {
		warning("rejack: no rejack_emergency_hook configured — nobody is notified\n");
		return;
	}
	(void)spawn(argv);
}

static void start_recording(void)
{
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
	snprintf(recording, sizeof(recording), "%s/emergency-%s.wav", rj.record_dir, ts);

	char *argv[] = { "arecord", "-q", "-D", rj.record_dev, "-f", "S16_LE", "-r", "8000",
			 "-c", "1", "-t", "wav", recording, NULL };
	recorder = spawn(argv);
	if (recorder)
		info("rejack: EMERGENCY: recording to %s\n", recording);
	else
		recording[0] = '\0';
}

static void stop_recording(void)
{
	if (!recorder)
		return;
	(void)kill(recorder, SIGINT);   /* arecord finalises the WAV header on SIGINT */
	info("rejack: EMERGENCY: recording stopped (%s)\n", recording);
}

static void announce(void)
{
	int err;

	rj.play = mem_deref(rj.play);
	if (rj.emerg_announce[0]) {
		err = play_file(&rj.play, baresip_player(), rj.emerg_announce, 2,
				rj.audio_mod, rj.audio_dev);
		if (!err)
			return;
		warning("rejack: cannot play %s (%m)\n", rj.emerg_announce, err);
	}
	/* No announcement available: slow busy at least tells the caller this is not 911. */
	op_tone(TONE_BUSY, NULL);
}

static void op_emergency(enum ph_emerg ev, bool verified, const char *reason, void *arg)
{
	(void)arg;

	switch (ev) {

	case EMERG_DIALED:
		warning("rejack: EMERGENCY: 911 dialed (E911 %s)\n",
			verified ? "verified" : "NOT verified");
		recording[0] = '\0';
		run_hook("dialed", verified, NULL, NULL);
		break;

	case EMERG_FAILED:
		warning("rejack: EMERGENCY: 911 did not go through: %s — fallback\n", reason);
		announce();
		start_recording();
		run_hook("failed", verified, reason, recording);
		break;

	case EMERG_ENDED:
		rj.play = mem_deref(rj.play);
		stop_recording();
		run_hook("ended", verified, NULL, recording);
		break;
	}
}

static const struct ph_ops ops = {
	.tone   = op_tone,
	.ring   = op_ring,
	.dial   = op_dial,
	.answer = op_answer,
	.hangup = op_hangup,
	.timer  = op_timer,
	.dtmf   = op_dtmf,
	.e911_ok = op_e911_ok,
	.emergency = op_emergency,
};


/* ---- hardware events -------------------------------------------------- */

static void feed(enum ph_event ev, char digit)
{
	enum ph_state before = rj.ph.st;

	phone_event(&rj.ph, ev, digit);
	if (rj.ph.st != before)
		info("rejack: %s --%s--> %s\n", phone_state_str(before),
		     phone_event_str(ev), phone_state_str(rj.ph.st));
}

/* The interrupt report's keypad field belongs to TigerJet's USB handsets; for an analog phone the
 * line state comes from registers (poll_tick). Drain reports so the fd doesn't stay readable, and use
 * the fd to notice an unplug. */
static void hid_handler(int flags, void *arg)
{
	struct tj_input in;
	int r;
	(void)arg;

	if (flags & FD_EXCEPT) {
		warning("rejack: hidraw exception — device unplugged?\n");
		rj.fhs = fd_close(rj.fhs);
		return;
	}
	while ((r = tjdev_read_input(rj.dev, &in)) > 0)
		;
	if (r < 0) {
		warning("rejack: hidraw read error (%m)\n", -r);
		rj.fhs = fd_close(rj.fhs);
	}
}

static void poll_tick(void *arg)
{
	struct tj_line ln;
	int err;
	(void)arg;

	err = tjdev_poll(rj.dev, &ln);
	if (err) {
		if (!rj.poll_warned)
			warning("rejack: register poll failed (%m)\n", -err);
		rj.poll_warned = true;
		tmr_start(&rj.tmr_poll, 1000, poll_tick, NULL);   /* back off */
		return;
	}
	if (rj.poll_warned) {
		info("rejack: register poll recovered\n");
		rj.poll_warned = false;
	}
	if (ln.hook_changed)
		feed(ln.offhook ? EV_OFFHOOK : EV_ONHOOK, 0);
	if (ln.digit) {
		if (rj.hw_dt_on) {           /* the chip doesn't cut forced dial tone itself */
			(void)tjdev_dialtone(rj.dev, false);
			rj.hw_dt_on = false;
		}
		info("rejack: digit '%c'\n", ln.digit);
		feed(EV_DIGIT, ln.digit);
	}
	tmr_start(&rj.tmr_poll, POLL_MS, poll_tick, NULL);
}


/* ---- SIP events ------------------------------------------------------- */

static bool is_ours(const struct call *call)
{
	return call && call == rj.call;
}

static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);
	struct ua *ua = bevent_get_ua(event);
	const struct sip_msg *msg = bevent_get_msg(event);
	(void)arg;

	switch (ev) {

	case BEVENT_SIPSESS_CONN:
		/* New incoming INVITE. Accept only when the phone is free. */
		if (!phone_can_accept(&rj.ph)) {
			(void)sip_treply(NULL, uag_sip(), msg, 486, "Busy Here");
			info("rejack: incoming call rejected: busy\n");
			bevent_stop(event);
			return;
		}
		if (ua_accept(uag_find_msg(msg), msg))
			warning("rejack: could not accept incoming call\n");
		bevent_stop(event);
		break;

	case BEVENT_CALL_INCOMING:
		if (rj.call)
			break;                   /* already busy; should not happen */
		rj.call = call;
		rj.ua   = ua;
		info("rejack: incoming call from %s\n", call_peeruri(call));
		feed(EV_INCOMING, 0);
		break;

	case BEVENT_CALL_RINGING:
		if (is_ours(call))
			feed(EV_REMOTE_RINGING, 0);
		break;

	case BEVENT_CALL_PROGRESS:
		if (is_ours(call)) {
			op_tone(TONE_NONE, NULL);   /* free hw:TigerJet before early media opens it */
			feed(EV_REMOTE_PROGRESS, 0);
		}
		break;

	case BEVENT_CALL_ANSWERED:
		/* Emitted before baresip starts call audio (ESTABLISHED comes after). Tones and call
		 * audio share hw:TigerJet,0, so the ringback player must be closed now or the audio
		 * start fails with EBUSY and the call drops. */
		if (is_ours(call))
			op_tone(TONE_NONE, NULL);
		break;

	case BEVENT_CALL_ESTABLISHED:
		if (is_ours(call))
			feed(EV_ESTABLISHED, 0);
		break;

	case BEVENT_CALL_CLOSED:
		if (is_ours(call)) {
			info("rejack: call closed: %s\n", bevent_get_text(event));
			rj.call = NULL;
			rj.ua   = NULL;
			feed(EV_CLOSED, 0);
		}
		break;

	default:
		break;
	}
}


/* ---- module ----------------------------------------------------------- */

static void load_config(struct ph_config *pc)
{
	struct conf *conf = conf_cur();
	char v[32] = "";
	uint32_t u;

	strcpy(rj.hid, "auto");
	strcpy(rj.audio_mod, "alsa");
	strcpy(rj.audio_dev, "hw:TigerJet,0");
	rj.account[0] = '\0';

	(void)conf_get_str(conf, "rejack_hid", rj.hid, sizeof(rj.hid));
	(void)conf_get_str(conf, "rejack_audio_mod", rj.audio_mod, sizeof(rj.audio_mod));
	(void)conf_get_str(conf, "rejack_audio_dev", rj.audio_dev, sizeof(rj.audio_dev));
	(void)conf_get_str(conf, "rejack_account", rj.account, sizeof(rj.account));

	memset(pc, 0, sizeof(*pc));
	if (!conf_get_u32(conf, "rejack_interdigit", &u))
		pc->interdigit_ms = u;

	pc->e911 = E911_ALLOW;                      /* default: always attempt 911 */
	if (!conf_get_str(conf, "rejack_911", v, sizeof(v))) {
		if (!strcmp(v, "block"))
			pc->e911 = E911_BLOCK;
		else if (!strcmp(v, "verified"))
			pc->e911 = E911_VERIFIED;
		else if (strcmp(v, "allow"))
			warning("rejack: unknown rejack_911 '%s', using 'allow'\n", v);
	}
	rj.emerg_hook[0] = rj.emerg_announce[0] = '\0';
	strcpy(rj.record_dir, "/var/lib/rejack/recordings");
	snprintf(rj.record_dev, sizeof(rj.record_dev), "%s", rj.audio_dev);
	(void)conf_get_str(conf, "rejack_emergency_hook", rj.emerg_hook, sizeof(rj.emerg_hook));
	(void)conf_get_str(conf, "rejack_emergency_announce", rj.emerg_announce,
			   sizeof(rj.emerg_announce));
	(void)conf_get_str(conf, "rejack_record_dir", rj.record_dir, sizeof(rj.record_dir));
	(void)conf_get_str(conf, "rejack_record_dev", rj.record_dev, sizeof(rj.record_dev));
	strcpy(rj.e911_status, "/var/lib/rejack/e911");
	rj.e911_did[0] = '\0';
	rj.e911_max_age_h = 26;
	(void)conf_get_str(conf, "rejack_e911_did", rj.e911_did, sizeof(rj.e911_did));
	(void)conf_get_str(conf, "rejack_e911_status", rj.e911_status, sizeof(rj.e911_status));
	(void)conf_get_u32(conf, "rejack_e911_max_age", &rj.e911_max_age_h);

	v[0] = '\0';
	rj.hw_dialtone = !(!conf_get_str(conf, "rejack_dialtone", v, sizeof(v)) && !strcmp(v, "sw"));
	v[0] = '\0';
	rj.send_dtmf = !conf_get_str(conf, "rejack_dtmf", v, sizeof(v)) && !strcmp(v, "send");

	info("rejack: hid=%s tones=%s,%s dialtone=%s 911=%s dtmf=%s\n", rj.hid, rj.audio_mod,
	     rj.audio_dev, rj.hw_dialtone ? "hw" : "sw",
	     pc->e911 == E911_BLOCK ? "block" : pc->e911 == E911_ALLOW ? "allow" : "verified",
	     rj.send_dtmf ? "send" : "inband");
	if (!rj.emerg_hook[0])
		warning("rejack: rejack_emergency_hook not set: 911 events notify nobody\n");
	if (!rj.emerg_announce[0])
		warning("rejack: rejack_emergency_announce not set: fallback plays busy tone only\n");
}

static int module_init(void)
{
	struct ph_config pc;
	int err;

	memset(&rj, 0, sizeof(rj));
	tmr_init(&rj.tmr_phone);
	tmr_init(&rj.tmr_ring);
	tmr_init(&rj.tmr_poll);
	load_config(&pc);
	phone_init(&rj.ph, &pc, &ops, NULL);

	tmr_init(&tmr_reap);
	(void)op_e911_ok(NULL);                   /* log current E911 status at startup */

	err = tjdev_open(&rj.dev, strcmp(rj.hid, "auto") ? rj.hid : NULL);
	if (err) {
		warning("rejack: cannot open magicJack HID device (%m)\n", -err);
		return -err;
	}
	info("rejack: using %s\n", tjdev_path(rj.dev));

	err = tjdev_setup(rj.dev);       /* we own dial tone and ring from here on */
	if (err)
		warning("rejack: line setup failed (%m)\n", -err);
	tmr_start(&rj.tmr_poll, POLL_MS, poll_tick, NULL);

	err = fd_listen(&rj.fhs, tjdev_fd(rj.dev), FD_READ, hid_handler, NULL);
	if (err)
		goto fail;

	err = bevent_register(event_handler, NULL);
	if (err)
		goto fail;

	return 0;

 fail:
	tmr_cancel(&rj.tmr_poll);
	(void)tjdev_release(rj.dev);
	rj.fhs = fd_close(rj.fhs);
	tjdev_close(rj.dev);
	rj.dev = NULL;
	return err;
}

static int module_close(void)
{
	bevent_unregister(event_handler);
	stop_recording();
	tmr_cancel(&tmr_reap);
	tmr_cancel(&rj.tmr_phone);
	tmr_cancel(&rj.tmr_ring);
	tmr_cancel(&rj.tmr_poll);
	if (rj.dev)
		(void)tjdev_release(rj.dev);   /* ring off, chip's automatic dial tone back on */
	rj.play = mem_deref(rj.play);
	rj.fhs  = fd_close(rj.fhs);
	tjdev_close(rj.dev);
	rj.dev = NULL;
	return 0;
}

const struct mod_export DECL_EXPORTS(rejack) = {
	"rejack",
	"application",
	module_init,
	module_close
};
