# Quickstart: Asterisk, softphones and a SIP trunk

A home phone server in about an hour: Asterisk on a Linux box, softphones and the ReJack analog line as extensions,
and a SIP trunk provider for a real phone number.

| Part | Status here |
|---|---|
| 1. Asterisk with extensions, voicemail and test numbers | ✅ Running (`pbx/`) |
| 2. Softphones (Linphone) | ✅ Running, as test clients |
| 3. The analog phone (ReJack) | ✅ Running; see [`rejack/README.md`](../rejack/README.md) |
| 4. SIP trunk: phone number, outside calls, E911 | ✅ Calls in and out working (`pbx/install.sh`); ✅ E911 address confirmed by the provider's test line (2026-11-24) |

```
 analog phone ── magicJack ── ReJack (ext 100) ──┐
 Linphone, PC (ext 102) ─────────────────────────┼── Asterisk ── SIP trunk provider ── the phone network
 Linphone, cell (ext 101, test client) ──────────┘
```

**You need:** a Linux machine that stays on (Ubuntu 24.04 here), on your home LAN with a fixed LAN address (a DHCP
reservation is enough). For part 4: an account with a SIP trunk provider that offers registration-based trunks,
a phone number (DID) and E911.

## 1. Asterisk

```
sudo apt install asterisk                    # Asterisk 20 LTS with PJSIP
cp pbx/secrets.env.example pbx/secrets.env   # LAN_CIDR, one password and voicemail PIN per extension
chmod 600 pbx/secrets.env
sudo pbx/install.sh                          # backs up /etc/asterisk, installs the config, restarts Asterisk
```

`LAN_CIDR` is your home network, for example `192.168.1.0/255.255.255.0`. Only localhost and that network may talk SIP
to the server. **Never port-forward 5060**: exposed SIP servers are scanned and attacked within hours.

Check it: `sudo asterisk -rx "pjsip show endpoints"` lists 100, 101 and 102.

| Number | What it does |
|---|---|
| 100–102 | Extensions. Ring 25 s, then voicemail |
| `*97` | Your own mailbox (no PIN from your own phone) |
| `*98` | Any mailbox, with its PIN |
| 600 | Echo test: you hear yourself, so it tests both audio directions |
| 601 | "Hello world", then reads back your extension |
| 911 | Without a trunk: **test only**, congestion, so ReJack's 911 fallback runs. With a trunk: a real 911 call (4d) |
| anything else | "Cannot complete as dialed", then 404 |

## 2. Softphones (Linphone)

In Linphone (desktop or mobile): **Use a SIP account** (a third-party account; keep any linphone.org account next to it).

| Field | Value |
|---|---|
| Username | the extension, e.g. `102` |
| Password | `PW_102` from `pbx/secrets.env` |
| Domain | the server's LAN address, e.g. `192.168.1.10` |
| Transport | UDP |

Check it: `sudo asterisk -rx "pjsip show contacts"` shows the phone as `Avail`; dial 600 from it.

- Changed a password? Delete and re-add the account; Linphone caches the old one.
- **A phone is a test client here, not an extension calls rely on.** Phones suspend background apps, so a cell
  softphone is only reachable while the app is open, and keeping it registered all day costs battery. Calls for
  someone away from home should go to their real mobile number over the trunk (part 4, "ring my cell").

## 3. The analog phone

Build and install ReJack per [`rejack/README.md`](../rejack/README.md); it registers as extension 100 from
localhost. Pick up: dial tone; dial 600; dial 102 to ring the desktop softphone.

## 4. SIP trunk provider

> ✅ **Run on this setup (2026-11-23)**: registration, calls in and out, behind carrier-grade NAT. ✅ **E911 address
> confirmed by the provider's test line (2026-11-24).** Names in `UPPER_CASE` are yours to fill in.

### 4a. In the provider's portal

1. **Create a sub-account (or "SIP user") for the server.** Keep the main web login separate from the SIP
   credentials; use a long random SIP password.
2. **Authentication: user name and password (registration).** Not IP-based: a home connection's public IP changes.
3. **Pick the nearest server (point of presence).** Note its host name: `TRUNK_HOST`.
4. **Codecs: G.711 μ-law** (ulaw). **DTMF: RFC 2833 / RFC 4733.** No transcoding, clean digits for menus.
5. **Order a phone number (DID)** and route its inbound calls to that sub-account: `TRUNK_DID` (11 digits, `1NXXNXXXXXX`).
6. **Register the E911 address for the DID** (street address, unit). An unregistered number can't send your address to
   the 911 center. Many providers charge a small monthly fee for this; it isn't optional for a home line.
7. **Caller ID.**
   - **Outbound number:** send your own DID, set in the dial plan (4c) or as an override on the sub-account. Pick one
     place; a sub-account override usually wins over what Asterisk sends. Under STIR/SHAKEN the provider can only
     fully attest numbers you own, so any other number arrives less trusted and is more likely to be flagged as spam.
   - **Outbound name:** not sent with the call. The called party's carrier looks your number up in a caller-name
     (CNAM) database, so `CALLERID(name)` is ignored on calls to the phone network. Register a name for the DID in the
     portal if the provider offers it (often a small fee; it can take days to weeks to reach other carriers).
   - **Inbound number:** passed through; Asterisk has it in `${CALLERID(num)}` and softphones show it.
   - **Inbound name:** turn on CNAM lookup for the DID if you want names (usually a small per-call charge); the name
     then arrives with the call. The analog phone doesn't show caller ID yet (on the ReJack roadmap).
8. **Limits:** turn off international calling unless you need it, and set a daily spending limit. A stolen SIP
   password is a bill; limits cap it.

### 4b. Asterisk: the trunk (PJSIP)

`pbx/install.sh` sets the trunk up when `TRUNK_HOST` is set in `pbx/secrets.env` (gitignored; leave it empty for
no trunk). The credentials stay in that file, never in committed ones:

```
TRUNK_HOST=pop.provider.example      # the point of presence from 4a
TRUNK_USER=sub-account-user
TRUNK_PASS='long-random-password'    # single quotes; avoid | & \ ; # and spaces
TRUNK_DID=15551234567
TRUNK_PORT=5080                      # the provider's SIP port: 5060 by default, see "Port 5060" below
```

Then `sudo pbx/install.sh`. It renders two files from templates and adds the provider's server to the ACL:

| File | What |
|---|---|
| [`pjsip_trunk.conf.in`](../pbx/asterisk/pjsip_trunk.conf.in) | Its own UDP transport (local port `TRUNK_LOCAL_PORT`, 5160 by default), the registration, auth, endpoint (ulaw, RFC 4733 DTMF, symmetric RTP) and identify-by-IP |
| [`extensions_trunk.conf.in`](../pbx/asterisk/extensions_trunk.conf.in) | `[from-trunk]` for calls in and `[trunk-out]` for calls out (4c) |
| `pjsip.conf.in` | `permit=` for the provider's server in `acl-home`, which otherwise allows only localhost and the LAN |

Check it: `sudo asterisk -rx "pjsip show registrations"` shows the trunk `Registered`.

The ACL permits the one address `TRUNK_HOST` resolves to when you install. Calls arrive from the server you register
to, so that is enough here. Rerun `install.sh` if the provider changes that address. A provider that sends calls from
several servers needs a `permit=` for each of its published signalling ranges (never `0.0.0.0/0`).

**NAT.** The server registers out to the provider, which keeps a path open through the router, so there is no port
forward. The endpoint uses symmetric RTP, and an OPTIONS ping every 25 s keeps the NAT mapping open between calls.
This also works behind **carrier-grade NAT (CGNAT)**, common with mobile and fixed-wireless home internet, where the
house has no public IPv4 address. Behind CGNAT, dynamic DNS and `external_media_address` don't help: there is no
address of your own to point them at, so leave them out. With an ordinary home router and a public IP, symmetric
RTP is usually enough too. If you get one-way audio, set `local_net` and `external_media_address` on the trunk
transport.

**Port 5060.** Some ISPs silently drop SIP INVITEs on port 5060 and pass everything else, including the REGISTER
and OPTIONS. The trunk then shows `Registered`, but calls out get no reply (dead air), and calls in never arrive
(the provider plays busy). If that happens, set `TRUNK_PORT` to the provider's alternate SIP port (many offer
5080). The trunk already has its own local port, 5160, so the house phones stay on 5060. A quick test is to compare
the provider's answer to an INVITE on 5060 with one on the alternate port: with this problem, only the alternate
port gets `100 Trying`.

### 4c. Asterisk: the dial plan

Inbound: the DID rings the house and falls back to voicemail. Outbound: North American numbers go out the trunk with
your DID as caller ID. This is in [`extensions_trunk.conf.in`](../pbx/asterisk/extensions_trunk.conf.in), shown
here in short. `[rejack-phones]` includes `[trunk-out]` and then the `[intercept]` catch-all, in that order. An
include is searched after a context's own extensions, so 911 and the test numbers still win.

```
[from-trunk]                                 ; the provider may address it to the DID, the SIP user or "s"
exten => inbound,1,Dial(PJSIP/100&PJSIP/102,25)   ; analog phone + desktop; unregistered ones are skipped
 same => n,VoiceMail(100@default,u)

[trunk-out]
exten => _NXXNXXXXXX,1,Goto(1${EXTEN},1)     ; 10 digits -> 1 + 10
exten => _1NXXNXXXXXX,1,Set(CALLERID(num)=${TRUNK_DID})
 same => n,Dial(PJSIP/${EXTEN}@trunk,60)
exten => 933,1,...                           ; a 911 test line on some providers (4d)

[emergency]                                  ; 911 / 1911 / 9911 from the house (4d)
exten => 911,1,Set(CALLERID(num)=${TRUNK_DID})
 same => n,Dial(PJSIP/911@trunk)
 same => n,Congestion(5)                     ; failed: ReJack's fallback runs
```

"Ring my cell" when nobody answers at home: add `same => n,Dial(PJSIP/1MOBILENUMBER@trunk,25)` before the
voicemail step. That costs an outbound call per forward, and the cell's own voicemail may answer first.

### 4d. 911

This is deliberately the last step, and it changes what the analog phone does in an emergency.

1. The E911 address is registered and confirmed in the portal (4a, step 6).
2. Call the provider's 911 test line from the analog phone. Some use **933**; others publish an ordinary number
   (look in the E911 section of the portal). It should read back the caller ID and the registered address and
   confirm that 911 is enabled. 933 is routed out the trunk in `[trunk-out]`; an ordinary number goes out like any
   other call. Here 933 got an instant 503 and the provider's own test number worked.
3. With a trunk, `pbx/install.sh` routes 911, 1911 and 9911 to `[emergency]`: out the trunk as `911` with the DID
   as caller ID, no ring timeout, and congestion if it fails. Without a trunk, `[emergency]` is the test congestion.
   **Don't test it by dialling 911.** Check the route with `asterisk -rx "dialplan show 911@emergency"`.
4. Keep ReJack's fallback configured (`rejack_emergency_hook`): if 911 can't go through (internet down, trunk not
   registered), the analog phone says so and the emergency contacts are alerted. See [`ARCHITECTURE.md`](../ARCHITECTURE.md#911-policy).

**A VoIP line fails when the power or the internet does.** Keep a charged mobile phone in the house.

### 4e. Test plan

| Test | Expect | Here |
|---|---|---|
| `pjsip show registrations` | Trunk `Registered` | ✅ |
| Call the DID from a mobile | Analog phone and desktop ring; both directions of audio | ✅ |
| Dial a mobile from the analog phone | It rings; caller ID shows the DID | ✅ |
| Call in again after 15+ minutes idle | Still rings (the NAT mapping stayed open) | ⏳ |
| Let it ring out | Voicemail for 100; `*97` plays the message | ⏳ |
| Dial a menu (bank, pharmacy) and press keys | Digits are recognised (RFC 4733) | ⏳ |
| The provider's 911 test line (933 or a published number) | Reads back the caller ID and address; "911 enabled" | ✅ |
| `dialplan show 911@emergency` | `Dial(PJSIP/911@trunk)` with the DID as caller ID | ⏳ |

## Troubleshooting

| Symptom | Look at |
|---|---|
| Softphone won't register | `LAN_CIDR` in `secrets.env`; the password (delete and re-add the account); `sudo asterisk -rvvv` while it retries |
| Calls to an extension go straight to voicemail | It isn't registered: `pjsip show contacts`. A phone app in the background isn't registered |
| Trunk `Rejected` / 403 | User name and password; is registration (not IP auth) set on the sub-account? |
| Inbound calls never arrive (the caller hears busy) | The DID routed to the sub-account; the ACL `permit` for the provider; the ISP dropping INVITEs on 5060 (`TRUNK_PORT`, 4b) |
| Calls out: dead air, then nothing | No reply to the INVITE: the ISP dropping INVITEs on 5060 (`TRUNK_PORT`, 4b). See it with `sudo asterisk -rx "pjsip set logger on"` |
| "Cannot be completed as dialed" on a call out | The number as dialled isn't a valid North American number: ReJack's log shows the digits it heard (`journalctl -u rejack`) |
| One-way or no audio on trunk calls | NAT: symmetric RTP on the provider's side (its NAT setting), then `local_net` / `external_media_address` (not behind CGNAT); RTP ports 10000–10200 |
| Your voice too quiet on voicemail or the echo test | Line levels: `rejack/systemd/99-magicjack.rules` (capture 15, playback 8) |
| Digits not recognised by menus | `dtmf_mode=rfc4733` on both sides |
