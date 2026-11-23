# Quickstart: Asterisk, softphones and a SIP trunk

A home phone server in about an hour: Asterisk on a Linux box, softphones and the ReJack analog line as extensions,
and a SIP trunk provider for a real phone number.

| Part | Status here |
|---|---|
| 1. Asterisk with extensions, voicemail and test numbers | ✅ Running (`pbx/`) |
| 2. Softphones (Linphone) | ✅ Running, as test clients |
| 3. The analog phone (ReJack) | ✅ Running; see [`rejack/README.md`](../rejack/README.md) |
| 4. SIP trunk: phone number, outside calls, E911 | ⏳ **Not yet run on this setup.** The config below is standard PJSIP, untested here |

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
| 911 | **Test only** until part 4: congestion, so ReJack's 911 fallback runs |
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

> ⏳ **Not yet run on this setup.** The steps are what a registration-based trunk needs with Asterisk's PJSIP; the
> repo will get a `pbx/install.sh` trunk step once they have been tested end to end. Names in `UPPER_CASE` are
> yours to fill in.

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

Credentials go in `pbx/secrets.env` (gitignored), never in committed files:

```
TRUNK_HOST=pop.provider.example
TRUNK_USER=sub-account-user
TRUNK_PASS=long-random-password
TRUNK_DID=15551234567
```

PJSIP sections for the trunk (rendered from those values):

```
[trunk]
type=registration
transport=transport-udp
outbound_auth=trunk
server_uri=sip:TRUNK_HOST
client_uri=sip:TRUNK_USER@TRUNK_HOST
contact_user=TRUNK_USER          ; inbound calls arrive addressed to this user
retry_interval=60
forbidden_retry_interval=600     ; don't hammer the provider after a 403
expiration=300

[trunk]
type=auth
auth_type=userpass
username=TRUNK_USER
password=TRUNK_PASS

[trunk]
type=aor
contact=sip:TRUNK_HOST
qualify_frequency=60

[trunk]
type=endpoint
context=from-trunk
disallow=all
allow=ulaw
outbound_auth=trunk
aors=trunk
from_user=TRUNK_USER
from_domain=TRUNK_HOST
direct_media=no
rtp_symmetric=yes
force_rport=yes
rewrite_contact=yes
dtmf_mode=rfc4733

[trunk]
type=identify
endpoint=trunk
match=TRUNK_HOST                 ; resolved when Asterisk loads; add the provider's published IP ranges if it uses several
```

**The ACL.** `acl-home` in `pbx/asterisk/pjsip.conf.in` denies everyone but localhost and the LAN, and that includes
the provider. Add a `permit=` line for each of the provider's published signalling IP ranges (not `0.0.0.0/0`).

**NAT (the server is behind your router).** Registration keeps a path open for signalling, so no port forward is
needed. For audio, tell Asterisk what is local and what the public address is, on `[transport-udp]`:

```
local_net=LAN_CIDR
external_media_address=YOUR_PUBLIC_IP_OR_DDNS_NAME
external_signaling_address=YOUR_PUBLIC_IP_OR_DDNS_NAME
```

With a dynamic public IP, use a dynamic-DNS name. If calls connect but have one-way audio, NAT is the place to look
first.

Check it: `sudo asterisk -rx "pjsip show registrations"` shows the trunk `Registered`.

### 4c. Asterisk: the dial plan

Inbound: the DID rings the house and falls back to voicemail. Outbound: North American numbers go out the trunk with
your DID as caller ID. Add to `pbx/asterisk/extensions.conf`; the outbound patterns go **inside `[rejack-phones]`,
above the `_X.` catch-all**:

```
[globals]
TRUNK_DID=15551234567

[from-trunk]
exten => _X.,1,NoOp(Inbound ${CALLERID(all)} for ${EXTEN})
 same => n,Dial(PJSIP/100&PJSIP/102,25)      ; ring the analog phone and the desktop together
 same => n,VoiceMail(100@default,u)
 same => n,Hangup()

; in [rejack-phones], above "_X.":
exten => _NXXNXXXXXX,1,Goto(1${EXTEN},1)     ; 10 digits -> 1 + 10
exten => _1NXXNXXXXXX,1,Set(CALLERID(num)=${TRUNK_DID})
 same => n,Dial(PJSIP/${EXTEN}@trunk,60)
 same => n,Congestion()
```

"Ring my cell" when nobody answers at home: add `same => n,Dial(PJSIP/1MOBILENUMBER@trunk,25)` before the
voicemail step. That costs an outbound call per forward, and the cell's own voicemail may answer first.

### 4d. 911

This is deliberately the last step, and it changes what the analog phone does in an emergency.

1. The E911 address is registered and confirmed in the portal (4a, step 6).
2. If the provider offers a 911 test number (often **933**, which reads back the registered address), route it out
   the trunk and call it.
3. Only then replace the test 911 entries (`911`, `1911`, `9911`) in `extensions.conf` with a real route:
   `exten => 911,1,Set(CALLERID(num)=${TRUNK_DID})` / `same => n,Dial(PJSIP/911@trunk)`.
4. Keep ReJack's fallback configured (`rejack_emergency_hook`): if 911 can't go through (internet down, trunk not
   registered), the analog phone says so and the emergency contacts are alerted. See [`ARCHITECTURE.md`](../ARCHITECTURE.md#911-policy).

**A VoIP line fails when the power or the internet does.** Keep a charged mobile phone in the house.

### 4e. Test plan

| Test | Expect |
|---|---|
| `pjsip show registrations` | Trunk `Registered` |
| Call the DID from a mobile | Analog phone and desktop ring; both directions of audio |
| Let it ring out | Voicemail for 100; `*97` plays the message |
| Dial a mobile from the analog phone | It rings; caller ID shows the DID (and the registered name, once it has propagated) |
| Call the DID from a mobile, watch the desktop softphone | Shows the mobile's number (and name, if CNAM lookup is on) |
| Dial a menu (bank, pharmacy) and press keys | Digits are recognised (RFC 4733) |
| 933, if offered | Reads back the registered address |

## Troubleshooting

| Symptom | Look at |
|---|---|
| Softphone won't register | `LAN_CIDR` in `secrets.env`; the password (delete and re-add the account); `sudo asterisk -rvvv` while it retries |
| Calls to an extension go straight to voicemail | It isn't registered: `pjsip show contacts`. A phone app in the background isn't registered |
| Trunk `Rejected` / 403 | User name and password; is registration (not IP auth) set on the sub-account? |
| Inbound calls never arrive | The ACL `permit` for the provider; the DID routed to the sub-account; `contact_user` |
| One-way or no audio on trunk calls | NAT: `local_net`, `external_media_address`; RTP ports 10000–10200 |
| Your voice too quiet on voicemail or the echo test | Line levels: `rejack/systemd/99-magicjack.rules` (capture 15, playback 8) |
| Digits not recognised by menus | `dtmf_mode=rfc4733` on both sides |
