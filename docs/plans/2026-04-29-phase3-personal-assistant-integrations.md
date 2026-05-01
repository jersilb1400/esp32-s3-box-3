# Phase 3 — Personal Assistant Integrations Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use the appropriate execution skill (`executing-plans` or `subagent-driven-development`) to implement this plan.

**Goal:** Jarvis can read and act on real-world systems: Microsoft 365 calendar/email, Home Assistant smart home, web search, music control, personal knowledge base, and existing work tools (Slack, Monday, Planning Center) via existing Claude connectors.
**Architecture:** Each integration is exposed as MCP tools to xiaozhi-esp32-server. New integrations live in `~/jarvis-server/integrations/<service>/` as Python modules; existing Claude connectors are wrapped via a thin MCP shim (`integrations/claude_connector_bridge/`). Read-only operations are auto-approved; writes require voice confirmation; destructive actions require a spoken passphrase. Tool outputs pass through a "voiceify" filter that converts data into natural speech.
**Tech Stack:** Python 3.11, msal (Microsoft auth), aiohttp, requests, brave-search SDK or HTTP, spotipy, SQLite for OAuth token cache.
**Assumptions:**
- Assumes Phase 1 (speaker ID) is complete — without it, anyone could trigger calendar reads or email sends.
- Assumes Phase 1.5 (secrets in Fly secrets, structured logging) is complete — needed for OAuth client secrets.
- Assumes user has paid Spotify or Apple Music subscription for full control (free Spotify can only see, not control playback).
- Assumes "llm-wiki" is a self-hosted markdown knowledge vault accessible via filesystem or HTTP — needs spec confirmation in Task 14 before implementation.

---

## Sub-phase 3a: Microsoft 365 (Calendar + Email)

### Task 1: Register Azure AD app, get client ID
- App Registrations → new app → "Jarvis Personal Assistant"
- Redirect URI: device code flow (no redirect needed)
- Permissions: `Calendars.Read`, `Calendars.ReadWrite`, `Mail.Read`, `Mail.Send`, `User.Read`
- Grant admin consent (personal account: self-consent at first run)
- Set Fly secret: `M365_CLIENT_ID`

### Task 2: Device-code OAuth flow
**Files:** `~/jarvis-server/integrations/m365/auth.py`
First-run: server logs a device code URL + code; user opens URL on phone, signs in. Token saved to `data/m365_token.json` (encrypted via VOICEPRINT_KEY-derived key).

### Task 3: Calendar read tools
- `m365.calendar.next_event` — what's my next meeting?
- `m365.calendar.events_today` — list today
- `m365.calendar.events_in_range(start, end)`
- All return structured JSON; voiceify filter converts to "Your next meeting is at 2pm with Sarah, in Dallas."

### Task 4: Email read tools
- `m365.email.unread_count`
- `m365.email.recent_subjects(limit=5)` — read aloud headers, no body
- `m365.email.search(query)` — search subject/sender

### Task 5: Calendar write (voice-confirm gated)
- `m365.calendar.create_event(...)` — Jarvis: "Should I add 'Lunch with Mike' tomorrow at noon, sir?" → user confirms verbally → tool fires.
- Voice-confirm pattern: implemented in Task 23 (shared utility).

### Task 6: Email send (voice-confirm gated)
- `m365.email.send(to, subject, body)` — same confirm pattern.

### Task 7: Calendar delete (passphrase-confirm gated)
- `m365.calendar.delete_event(id)` — destructive; require random passphrase from screen.

### Task 8: Tests + integration
Mock Graph API responses; verify each tool. Add to MCP registry. End-to-end smoke: "Jarvis, what's on my calendar?" → device speaks the answer.

---

## Sub-phase 3b: Home Assistant / HomeKit

### Task 9: Provision HA long-lived token
User generates LLAT in Home Assistant UI → set Fly secret `HA_TOKEN`, `HA_BASE_URL`.

### Task 10: HA client wrapper
**Files:** `~/jarvis-server/integrations/homeassistant/client.py`
Async REST client: list entities, call services. WebSocket for state subscription (optional v2).

### Task 11: Smart home MCP tools
- `ha.list_devices()` — returns devices grouped by domain (light, switch, climate, lock)
- `ha.set_state(entity_id, state)` — turn on/off, brightness, color
- `ha.get_state(entity_id)`
- `ha.run_scene(scene_id)` — e.g. "movie night"

### Task 12: Voiceify for HA
"Turn on the bedroom lamp" → Jarvis maps "bedroom lamp" to entity ID via fuzzy match; calls `ha.set_state`; confirms "Done, sir." Failure ("device not responding") → "The bedroom lamp didn't respond. Try again, sir?"

### Task 13: HomeKit
HomeKit access requires Homebridge or HA's HomeKit Bridge integration. Document setup in `docs/INTEGRATIONS.md`. No separate code — Jarvis controls HomeKit devices through HA.

---

## Sub-phase 3c: Personal Knowledge Vault (llm-wiki)

### Task 14: Spec confirmation (BLOCKING)
**This task must be answered before implementation can begin.**

Ask user:
- What is llm-wiki? (own project, third-party, format?)
- Where does it live (filesystem path, HTTP endpoint, git repo)?
- What's the auth model?
- Update frequency?

### Task 15: Vault indexer
Once spec is known: pull markdown/text files, chunk (~500 tokens with 50-token overlap), embed with sentence-transformers, store in `vault.db` (SQLite + sqlite-vss).

### Task 16: Re-index job
Cron job every 4 hours: detect changed files (mtime or git diff), re-embed only those.

### Task 17: RAG retrieval tool
- `vault.search(query, limit=3)` — returns top chunks with source filename
- Injected into LLM context when user's question matches a "lookup" intent (classified by Task 10 from Phase 2)

### Task 18: Citation in voice responses
"According to your notes from Tuesday, the answer is X."

---

## Sub-phase 3d: Web Search

### Task 19: Brave Search API integration
**Files:** `~/jarvis-server/integrations/web/brave.py`
- Free tier: 2,000 queries/month
- Set `BRAVE_API_KEY` Fly secret
- Tool: `web.search(query, limit=5)`

### Task 20: DuckDuckGo HTML fallback
When Brave returns 429 (rate-limited), fall through to DuckDuckGo HTML scraper. Parse top 3 results.

### Task 21: Result caching
1-hour LRU cache on (query) to avoid burning quota on repeats.

### Task 22: Refusal categories
For queries matching financial/medical/legal advice patterns, refuse and redirect: "That's better answered by a professional, sir."

---

## Sub-phase 3e: Music Control

### Task 23: Spotify (primary)
**Files:** `~/jarvis-server/integrations/music/spotify.py`
- OAuth via spotipy
- Tools: `music.spotify.play(uri)`, `music.spotify.pause`, `music.spotify.next`, `music.spotify.now_playing`, `music.spotify.search(query)`
- Requires Premium for playback control on remote devices

### Task 24: Apple Music
MusicKit web API. More complex auth (developer token + Apple ID user token). Defer if Spotify covers user's needs. Document blocker.

**Amazon Music:** out of scope — no usable public playback API; no tasks or fallbacks planned.

---

## Sub-phase 3f: Existing Claude Connectors (Slack, Monday.com, PCO)

### Task 26: MCP shim — `claude_connector_bridge`
**Files:** `~/jarvis-server/integrations/claude_connector_bridge/`

Tools delegate to existing Claude connectors. Architecture:
- Identify how user's existing Claude connectors expose their endpoints (hosted by Anthropic? self-hosted? webhook?)
- Build a thin shim that translates xiaozhi-MCP tool calls to the connector's expected format
- Surface tools as: `slack.summarize_channel`, `slack.send_message`, `monday.list_my_items`, `monday.update_status`, `pco.next_service`, `pco.list_team_members`

### Task 27: Per-connector configuration
**Each connector needs spec confirmation** — same gate as Task 14. Ask user for endpoint URL, auth header format, tool list.

---

## Cross-cutting tasks

### Task 28: Voice-confirm utility
**Files:** `~/jarvis-server/utils/confirm.py`
Shared helper. Pattern:
1. Action proposed → Jarvis speaks "Should I do X, sir?"
2. Wait up to 10s for next utterance.
3. Match against affirmation patterns ("yes" / "go ahead" / "do it") → fire.
4. Match negation ("no" / "cancel" / "stop") → abort.
5. Anything else → ask again once, then abort.

### Task 29: Passphrase-confirm utility
For destructive actions (delete email, delete calendar event, modify HA automation):
1. Server picks random word from a 50-word list
2. Display word on device screen
3. Prompt: "Confirm by saying the word on screen, sir."
4. Match user utterance against the displayed word (allow 1 phonetic mistake)
5. On match → fire; on miss → abort, log attempt.

### Task 30: Voiceify filter
**Files:** `~/jarvis-server/utils/voiceify.py`
Convert structured data → natural speech:
- Lists with > 3 items: "You have 8 unread emails. The most recent is from Sarah, subject 'project update'."
- Times: "tomorrow at 2 PM" not "2026-04-30T14:00:00"
- Numbers: "76 degrees" not "76°F"

### Task 31: Privilege levels enforced
Decorator `@requires_confirm("voice"|"passphrase")` on tools so engineering errors don't bypass gates.

### Task 32: Integration test matrix
For each integration: mock external API → verify tool → end-to-end with TTS playback.

### Task 33: Documentation
**Files:** `~/esp32-s3-box-3/docs/INTEGRATIONS.md`
For each integration: setup steps (OAuth, tokens), available voice commands, troubleshooting.

### Task 34: Onboarding wizard
Voice-driven setup: "Jarvis, set up Microsoft 365." → Jarvis walks user through device-code flow on a separate device.

---

## Definition of Done

- [ ] M365 calendar + email read/write working with voice-confirm
- [ ] Home Assistant integration covering at least 5 devices
- [ ] Web search via Brave with DuckDuckGo fallback
- [ ] Spotify control end-to-end (assuming Premium account)
- [ ] llm-wiki vault indexed and queryable (post-spec confirmation)
- [ ] Slack/Monday/PCO Claude connectors callable via MCP shim (post-spec confirmation)
- [ ] Voice-confirm and passphrase-confirm gates working on all write/delete actions
- [ ] Voiceify filter handles lists/times/numbers naturally
- [ ] INTEGRATIONS.md documents setup for every integration
