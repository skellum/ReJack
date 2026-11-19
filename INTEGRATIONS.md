# Integrations spec: Switchboard, Discord, OpenClaw

Status: **spec, not started** (written 2026-11-18). This is Phase 7 in `PROJECT.md`.

Switchboard is the integration layer, nicknamed *Ansible*. It is a separate service from Rejack. Rejack stays a
plain SIP extension and never changes for an integration. Every integration talks to **Asterisk**, never to
Rejack or the dongle.

---

## 1. Switchboard core

### Where and what
- Runs on the **PBX box**, next to Asterisk and Rejack, as systemd `switchboard.service`.
- **Node.js 22 / TypeScript** (Node 22 is already installed). Chosen for the Discord voice library
  (`@discordjs/voice`), the ARI clients, and because OpenClaw is TypeScript too.
- Lives in `switchboard/` in this repo. Secrets go in a gitignored `switchboard/switchboard.env`, and a
  `switchboard.env.example` is committed.

### How it talks to Asterisk: ARI
Asterisk REST Interface (ARI) over HTTP and WebSocket on **localhost only** (`http.conf` binds 127.0.0.1;
`ari.conf` has one user whose password lives in `pbx/secrets.env`).

- The dialplan hands a call to Switchboard with `Stasis(switchboard,<feature>)`. The `<feature>` value is
  `discord` or `agent`.
- **Audio in and out of a call**: ARI `externalMedia`. Asterisk sends and receives plain RTP on a localhost
  UDP port, as `slin16` (16 kHz signed linear, no codec work in Switchboard). Asterisk converts to and from
  G.711 μ-law for Rejack.
- **Placing calls** (OpenClaw "agent calls you"): ARI `channels/originate` to `PJSIP/<ext>`, then into the
  Stasis app.

### Shared audio core
Both voice features need the same pipeline, so it is built once:

```
Asterisk call ── externalMedia RTP (slin16, 20 ms frames) ──► AudioPort
                                                                ├─► Discord bridge (§2)
                                                                └─► Speech loop: STT → OpenClaw → TTS (§3)
```

`AudioPort` provides:
- an RTP socket with a small jitter buffer, as a frame stream in each direction;
- resampling (16 kHz ↔ 48 kHz) and mixing;
- a **noise gate / VAD**, so line hiss doesn't count as speech;
- DTMF events, which arrive through ARI as `ChannelDtmfReceived`.

### Local HTTP API (for OpenClaw and scripts)
Listens on the LAN with a bearer token (`SWITCHBOARD_TOKEN`). OpenClaw runs on another host.

| Method | Path | Does |
|---|---|---|
| `POST` | `/api/call` | `{ "to": "100", "message": "…", "reply": "none\|dtmf\|talk" }`: ring an extension and speak |
| `GET` | `/api/status` | Registrations, active calls, Discord bridge state |

---

## 2. Discord connector: phone ↔ voice channel

### Goal
Pick up the house phone, dial **700**, and talk to friends in **one** voice channel on your own Discord server.

### Decisions
| Topic | Decision |
|---|---|
| Channels | One fixed voice channel (`DISCORD_VOICE_CHANNEL_ID`). One extension per channel can come later (701, …) |
| Server | Your own server. The bot is a normal Discord application with a bot token (never a user-account "selfbot") |
| Bot name | Something like "House Phone ☎": friends see the bot, not who is on the phone |
| Ring the house from Discord | `/ring-house` is **off by default**. When enabled: role-gated, quiet hours, internal extensions only |
| Language | Node/TypeScript, `discord.js` + `@discordjs/voice` |

### Call flow: phone → Discord
1. You dial 700. The dialplan runs `Answer()` then `Stasis(switchboard,discord)`.
2. Switchboard creates an `externalMedia` channel and bridges it to the caller.
3. The bot joins the voice channel and posts "☎ House phone joined" in the channel's text chat.
4. Audio:
   - **Discord → phone**: one Opus 48 kHz stream per speaker → decode → mix → resample to 16 kHz → RTP.
   - **Phone → Discord**: RTP 16 kHz → noise gate → resample to 48 kHz → Opus → bot speaks.
5. Hang up and the bot leaves the channel. If the bot is kicked or Discord drops, the phone hears a short tone and
   the call ends.

If nobody is in the channel, play a prompt ("nobody's in the channel; waiting") and stay connected.

### In-call keypad
| Key | Does |
|---|---|
| `*6` | Mute / unmute yourself toward Discord |
| `#` | Read out who's in the channel (TTS) |

### Optional: Discord → ring the house (`/ring-house [ext]`)
- Only members with a configured role can use it. Refused during quiet hours, e.g. 21:00–08:00.
- It may ring only internal extensions or the house ring group (200), never anything that reaches a trunk.
- Rate limit: once per 5 minutes per member.
- On answer, the call is bridged into the voice channel the same way as dialing 700.

### Risks and constraints
- **Encryption is mandatory.** Since early March 2026 Discord only accepts end-to-end encrypted (DAVE) voice calls,
  bots included. `@discordjs/voice` handles DAVE through `@snazzah/davey`. Pin versions and test after upgrades.
- **Receiving audio is undocumented** by Discord, so `@discordjs/voice` doesn't guarantee it. It works in practice
  but can break when Discord changes something. This is the main long-term risk.
- Phone-side audio is narrowband (8 kHz line), so you will sound like a landline. Expect about 150–250 ms of delay.
- Echo: Rejack's line echo canceller covers the analog side. Friends on speakers rely on Discord's own echo
  cancellation.

### Config (`switchboard.env`)
```
DISCORD_BOT_TOKEN=…
DISCORD_GUILD_ID=…
DISCORD_VOICE_CHANNEL_ID=…
DISCORD_TEXT_CHANNEL_ID=…          # join/leave notices
DISCORD_RING_ROLE_ID=              # empty = /ring-house disabled
QUIET_HOURS=21:00-08:00
```

### Test plan (no dongle needed)
1. Dial 700 from Linphone on the cell (ext 101) with a second Discord account in the channel: check both audio
   directions, the mix with 2+ speakers, mute, and leave on hang-up.
2. Same test from the house phone (ext 100) once home: check echo and line noise against the gate.
3. Kick the bot mid-call and restart Discord: the call ends cleanly and Switchboard recovers.

---

## 3. OpenClaw connector: talk to the agent by phone

OpenClaw is **not set up yet**. These facts come from its docs (2026-11-18); re-check them when it is installed.

### What OpenClaw offers, and what that means here
| OpenClaw feature | Use here |
|---|---|
| `POST /v1/chat/completions` on the Gateway (OpenAI-compatible, **off by default**: `gateway.http.endpoints.chatCompletions.enabled`) | **Main path.** A synchronous agent turn: text in, text out. Bearer token, `x-openclaw-agent-id` header |
| `POST /hooks/agent` / `/hooks/wake` (off by default; `hooks.enabled`, dedicated `hooks.token`) | Asynchronous: hand the agent an event, e.g. "the reminder call was confirmed with 1" |
| Skills (`SKILL.md`) | An OpenClaw skill calls Switchboard's `POST /api/call` so the agent can phone the house |
| Voice-call plugin (`@openclaw/voice-call`) | **Not used.** Only supports Twilio, Telnyx and Plivo (plus a mock provider), not SIP. Emulating Twilio's API to feed it audio would be fragile |

The doc warns that the Gateway token is **full operator access**. It lives only in Switchboard's env file. Both
the Gateway and Switchboard listen on the LAN only, never the internet.

### 3a. Agent calls you (`reply: none | dtmf`)
1. OpenClaw (through the skill) sends `POST /api/call {to:"100", message:"Trash goes out tonight", reply:"dtmf"}`.
2. Switchboard originates to `PJSIP/100`, which rings the house phone. Ring timeout 30 s; no answer → return
   `no-answer`, no voicemail.
3. On answer: TTS of the message, then "press 1 to confirm, 2 to snooze, or hang up".
4. The result goes back to OpenClaw through `/hooks/agent` (`message: "Call result: confirmed"`).
5. Quiet hours apply unless the request sets `urgent: true`.

### 3b. Dial the agent (extension 800) and talk
Dialplan `Stasis(switchboard,agent)` → speech loop:
1. **VAD** detects the end of what you said (about 700 ms of silence).
2. **STT**: transcribe the utterance.
3. Send it to `/v1/chat/completions` with `stream: true`, keeping one session per call so the agent remembers the
   conversation.
4. **TTS** each sentence as it streams in, so the reply starts quickly.
5. **Barge-in**: if you start talking, playback stops.
6. Hang up to end. Say "goodbye" for a clean exit.

Target delay: under about 2 s from when you stop talking to the first word of the reply.

### Speech engines
Run them locally: household audio shouldn't go to a cloud by default.
- **STT**: faster-whisper (CTranslate2) on the **GTX 1080 Ti**, model `small.en` or `medium.en`.
- **TTS**: Piper (fast, offline, many voices).
- ⚠ **The PBX box CPU is an i7-960, which has no AVX.** Many prebuilt ML wheels and ONNX runtimes require AVX.
  Verify before building on it. Fallbacks: GPU-only builds, building from source without AVX, or running the speech
  services on the OpenClaw host and calling them over the LAN.

### Config (`switchboard.env`)
```
OPENCLAW_URL=http://ServerName:18789
OPENCLAW_TOKEN=…                    # Gateway token (chat completions)
OPENCLAW_HOOKS_TOKEN=…              # hooks token (results back to the agent)
OPENCLAW_AGENT_ID=main
STT_MODEL=small.en
TTS_VOICE=en_US-lessac-medium
```

---

## 4. Dialplan additions

```
; Discord voice channel
exten => 700,1,Answer()
 same => n,Stasis(switchboard,discord)
 same => n,Hangup()
; OpenClaw agent
exten => 800,1,Answer()
 same => n,Stasis(switchboard,agent)
 same => n,Hangup()
```

Extensions are only reachable from the internal `rejack-phones` context, never from a trunk. They are still
subject to the per-extension rules in `PROJECT.md` (Household phones).

---

## 5. Build order
1. **Core**: ARI connection, `Stasis` app, `externalMedia` echo test (dial 799 → hear yourself through Switchboard).
2. **Discord bridge** (§2), tested from Linphone.
3. **Agent calls you** with DTMF reply (§3a): needs OpenClaw installed, plus TTS.
4. **Dial the agent** (§3b): adds STT, streaming and barge-in.
5. `/ring-house`, if wanted.

## Sources
- Discord DAVE enforcement: https://discord.com/blog/every-voice-and-video-call-on-discord-is-now-end-to-end-encrypted,
  https://docs.discord.com/developers/topics/voice-connections
- `@discordjs/voice` (DAVE via `@snazzah/davey`; receive is undocumented): https://discord.js.org/docs/packages/voice/stable
- OpenClaw webhooks: https://docs.openclaw.ai/automation/cron-jobs/webhooks, https://docs.openclaw.ai/gateway/config-hooks
- OpenClaw OpenAI-compatible API: https://docs.openclaw.ai/gateway/openai-http-api
- OpenClaw voice-call plugin: https://docs.openclaw.ai/plugins/voice-call
