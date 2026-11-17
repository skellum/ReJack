/*
 * e911.c — see e911.h.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "e911.h"

static bool fail(char *why, size_t n, const char *fmt, ...)
{
	if (why && n) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(why, n, fmt, ap);
		va_end(ap);
	}
	return false;
}

/* Compare phone numbers ignoring everything but digits, and a leading US country code "1". */
static void digits_only(const char *in, char *out, size_t n)
{
	size_t j = 0;
	for (; *in && j + 1 < n; in++)
		if (*in >= '0' && *in <= '9')
			out[j++] = *in;
	out[j] = '\0';
	if (j == 11 && out[0] == '1')
		memmove(out, out + 1, j);   /* includes terminator */
}

bool e911_verified(const char *path, const char *did, unsigned max_age, time_t now,
		   char *why, size_t whylen)
{
	char line[256], status[32], fdid[64], a[32], b[32];
	long long ts;
	FILE *f;
	int n;

	if (!did || !*did)
		return fail(why, whylen, "rejack_e911_did not configured");
	if (!path || !*path)
		return fail(why, whylen, "no status file configured");

	f = fopen(path, "r");
	if (!f)
		return fail(why, whylen, "cannot read %s: %s", path, strerror(errno));
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return fail(why, whylen, "%s is empty", path);
	}
	fclose(f);

	n = sscanf(line, "%31s %63s %lld", status, fdid, &ts);
	if (n != 3)
		return fail(why, whylen, "%s: malformed (expected 'verified <did> <time>')", path);
	if (strcmp(status, "verified"))
		return fail(why, whylen, "checker reports '%s'", status);

	digits_only(did, a, sizeof(a));
	digits_only(fdid, b, sizeof(b));
	if (!*a || strcmp(a, b))
		return fail(why, whylen, "verified number %s does not match rejack_e911_did %s", fdid, did);

	if (ts > (long long)now + 300)
		return fail(why, whylen, "check time is in the future");
	if ((long long)now - ts > (long long)max_age)
		return fail(why, whylen, "last check is %lld h old (limit %u h)",
			    ((long long)now - ts) / 3600, max_age / 3600);

	if (why && whylen)
		snprintf(why, whylen, "E911 verified for %s, checked %lld min ago",
			 fdid, ((long long)now - ts) / 60);
	return true;
}
