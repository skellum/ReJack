# Home phone server (Asterisk)

The PBX in the middle: ReJack registers as an extension, softphones register as other extensions, and later a
SIP provider trunk gives the house a real phone number. No phone number is needed for calls between extensions.

| Ext | Who | Login from |
|---|---|---|
| 100 | ReJack (magicJack + analog phone) | localhost (baresip on 127.0.0.1:5070) |
| 101 | Linphone on a cell | home LAN |
| 102 | Linphone on a PC | home LAN |
| 600 | Echo test (hear yourself) | — |
| 601 | "Hello world" + reads back your extension | — |
| 911 | **Test only**: congestion, so ReJack's 911 fallback runs. No trunk = no real emergency calls | — |

## Install
Ubuntu 24.04: `sudo apt install asterisk` (Asterisk 20 LTS, PJSIP). Then:
```
cp pbx/secrets.env.example pbx/secrets.env   # set LAN_CIDR and one password per extension; chmod 600
sudo pbx/install.sh                          # backs up /etc/asterisk/{pjsip,extensions,rtp}.conf, installs, restarts
```
ReJack's account (`rejack/run/accounts`, gitignored):
`<sip:100@127.0.0.1:5060>;auth_pass=<PW_100>;regint=300`

## Softphones (Linphone)
Add a **third-party SIP account** (keep any linphone.org account; Linphone handles several): username = extension,
password from `secrets.env`, domain = the server's LAN IP, transport UDP. If a password was changed, delete and
re-add the account — Linphone caches a hash of the old one. On Android, exempt Linphone from battery optimisation
or it drops the registration in the background (calls to it then fail with 404).

## Security
SIP and RTP (UDP 5060, 10000–10200) are allowed only from localhost and the LAN (`acl-home` in `pjsip.conf.in`).
**Never port-forward 5060**: exposed SIP servers are scanned and attacked constantly.
Logs: `sudo asterisk -rvvv` · registrations: `sudo asterisk -rx "pjsip show contacts"`.
