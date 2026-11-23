# Home phone server (Asterisk)

The PBX in the middle: ReJack registers as an extension, softphones register as other extensions, and an optional
SIP provider trunk gives the house a real phone number. No phone number is needed for calls between extensions.

| Ext | Who | Login from |
|---|---|---|
| 100 | ReJack (magicJack + analog phone) | localhost (baresip on 127.0.0.1:5070) |
| 101 | Linphone on a cell | home LAN |
| 102 | Linphone on a PC | home LAN |
| 600 | Echo test (hear yourself) | — |
| 601 | "Hello world" + reads back your extension | — |
| 911 | **Test only**: congestion, so ReJack's 911 fallback runs. Not routed to the trunk until E911 is confirmed | — |
| 933 | The trunk provider's 911 test line (reads back the registered address) | trunk |
| 10 / 11 digits | Outside calls over the trunk, caller ID = the DID | trunk |

## Install
Ubuntu 24.04: `sudo apt install asterisk` (Asterisk 20 LTS, PJSIP). Then:
```
cp pbx/secrets.env.example pbx/secrets.env   # set LAN_CIDR and one password per extension; chmod 600
sudo pbx/install.sh                          # backs up /etc/asterisk/{pjsip,extensions,rtp}.conf, installs, restarts
```
**SIP trunk (optional):** set the `TRUNK_*` values in `secrets.env` and rerun `install.sh`. It renders
`pjsip_trunk.conf` and `extensions_trunk.conf` (stubs when `TRUNK_HOST` is empty) and permits the provider's
server in the ACL. Calls to the number ring 100 and 102, then voicemail 100. Details, NAT and the port-5060
problem are in [`docs/QUICKSTART.md`](../docs/QUICKSTART.md) part 4.
ReJack's account (`rejack/run/accounts`, gitignored):
`<sip:100@127.0.0.1:5060>;auth_pass=<PW_100>;regint=300`

## Softphones (Linphone)
Add a **third-party SIP account** (keep any linphone.org account; Linphone handles several): username = extension,
password from `secrets.env`, domain = the server's LAN IP, transport UDP. If a password was changed, delete and
re-add the account — Linphone caches a hash of the old one.

**A cell softphone is a test client, not an extension calls rely on.** Phones suspend background apps, so it is
reachable only while the app is open, and keeping it registered all day costs battery. Reaching someone away from
home is the trunk's job: ring their real mobile number (see [`docs/QUICKSTART.md`](../docs/QUICKSTART.md)).

Step-by-step setup including the trunk: [`docs/QUICKSTART.md`](../docs/QUICKSTART.md).

## Security
SIP and RTP (UDP 5060, 10000–10200) are allowed only from localhost and the LAN (`acl-home` in `pjsip.conf.in`),
plus the trunk provider's server when a trunk is set up (it connects on the trunk's own port, 5160).
**Never port-forward 5060**: exposed SIP servers are scanned and attacked constantly.
Logs: `sudo asterisk -rvvv` · registrations: `sudo asterisk -rx "pjsip show contacts"` · trunk:
`sudo asterisk -rx "pjsip show registrations"` · SIP trace: `sudo asterisk -rx "pjsip set logger on"`.
