/*
 * e911.h — decide whether provider E911 is verified, from a status file written by an external,
 * provider-specific checker (e.g. a daily systemd timer that queries the provider's API).
 *
 * Status file format (one line, written atomically by the checker):
 *
 *     verified <did> <unix-time-of-check>
 *
 * Anything else ("unverified ...", missing file, unreadable, stale, wrong DID) means NOT verified.
 * The check fails closed: every error path returns false with a reason.
 */
#ifndef REJACK_E911_H
#define REJACK_E911_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/*
 * path     status file
 * did      the phone number 911 calls will present (must match the file); empty/NULL = never verified
 * max_age  seconds a check stays valid
 * now      current time (parameter for testability)
 * why      receives a human-readable reason (may be NULL)
 */
bool e911_verified(const char *path, const char *did, unsigned max_age, time_t now,
		   char *why, size_t whylen);

#endif
