# JARVIS — Enhancement & Consolidation Plan

**Created:** April 29, 2026
**Author:** Jeremy + Claude (collaborative design)
**Status:** Approved design — awaiting implementation plan
**Repo:** [jersilb1400/esp32-s3-box-3](https://github.com/jersilb1400/esp32-s3-box-3)

---

## Vision

The best, most versatile ESP32-S3 personal AI assistant available. Speaks **only to its owner**, holds genuine conversation, remembers context across sessions, and operates real-world systems (calendar, smart home, music, knowledge bases, work tools) through a clean, reliable, owner-controlled stack.

## Current state (baseline as of 2026-04-29)

- Hardware: ESP-Box-3 with sensor dock (radar, humidity/temp, IMU, IR)
- Wake word: `jarvis` (Multinet 7 quantized, on-device)
- Backend: `jarvis-server.fly.dev` running `xiaozhi-esp32-server` on Fly.io
- LLM: Anthropic Claude (via OpenAI-compatible adapter `AliLLM`)
- STT: Groq Whisper (fast, free tier)
- TTS: Microsoft EdgeTTS, voice `en-GB-RyanNeural` (free, no key)
- Persona: JARVIS (British, dry wit, technical authority) — system prompt in config
- Existing MCP integrations: device controls (volume, brightness, theme, sensor reads, IR/PMOD GPIO)

## Known operational issues being closed by this plan

- 5 separate repos in user's home dir (`esp-box3`, `esp-box3-jarvis`, `xiaozhi-server-fly`, `jarvis-bridge`, `jarvis-agent-shim`) — confusing and drift-prone
- API keys stored as plaintext in `.config.yaml` and `env.example` files — must move to Fly.io secrets
- Jarvis responds to **anyone** speaking the wake word, not just the owner
- No long-term memory across sessions
- Limited integrations: weather + indoor sensors only; no calendar, email, smart home, music, work tools
- TTS provider chain has no failover (already saw quota-exhaustion outage)

---

## Sub-project decomposition

| # | Sub-project | Status | Phase |
|---|---|---|---|
| 1 | Speaker identification & voice gating | Design ready | Phase 1 |
| 2 | Conversational quality & long-term memory | Design ready | Phase 2 |
| 3 | Personal assistant integrations | Design ready | Phase 3 |
| 4 | Reliability & operations | Design ready | Phase 1.5 (parallel) |
| 5 | Code quality & repo consolidation | Design ready | Phase 0 (foundation) |

Order: **Phase 0 (consolidate) → Phase 1 (speaker ID) → Phase 1.5 (reliability, parallel) → Phase 2 (memory) → Phase 3 (integrations)**

---

## Decisions captured (from clarifying Q&A)

| Q | Decision |
|---|---|
| Q1 | **Hybrid speaker gating: biometric on wake word + diarization on utterance** (recommended over either alone — see Sub-project 1 rationale) |
| Q2 | Models run on `jarvis-server` Fly.io |
| Q3 | Voice-driven enrollment on the device (1-2 minutes, prompted phrases) |
| Q4 | Integrations: Microsoft 365, Home Assistant/HomeKit, web search, music (Spotify/Apple/Amazon), llm-wiki vault, Slack/Monday.com/PCO via existing Claude connectors |
| Q5 | Long-term memory: episodic recall + structured profile |
| Q6 | Strict privacy default — voiceprints stay on Fly.io volume, encrypted at rest |
| Q7 | Single-user (owner-only) — but data schema designed so multi-user is a non-breaking future add |
| Q8 | Consolidate 5 repos to 2: `esp32-s3-box-3` (firmware, public) + `jarvis-server-fly` (backend, private) |
| Q9 | Aggressive cost minimization — free tiers and self-hosted where possible |
| Q10 | No fixed timeline — build it right |

---

# Sub-project 1 — Speaker Identification & Voice Gating

## Goal
Jarvis only wakes for, transcribes, and responds to the owner's voice. Bystanders and overheard conversations are silently ignored.

## Architecture

Two layers of defense:

**Layer A — Biometric wake-word verification (50–80ms server-side check)**
1. Device detects wake word locally (Multinet) and streams the wake-word audio buffer + the next ~2 sec of speech to the server.
2. Server extracts a speaker embedding from the wake-word audio.
3. Cosine similarity vs. enrolled owner voiceprint.
4. If similarity < threshold → discard utterance silently, send empty TTS cycle so device returns to idle.
5. If similarity ≥ threshold → continue to Layer B.

**Layer B — Utterance diarization (handles "wake fired correctly, but multiple speakers in the captured audio")**
1. After wake-word verification passes, capture the rest of the utterance.
2. Run lightweight diarization on the full clip; identify owner-attributed segments.
3. Send only owner-attributed segments to STT.
4. If no owner segments detected after the wake word (e.g., owner said "Jarvis" then turned away to talk to someone else) → silently abort, return to idle.

## Technology choices (all free/self-hosted, per Q9)

| Component | Choice | Rationale |
|---|---|---|
| Speaker embedding | **SpeechBrain ECAPA-TDNN** | Open source, ~5MB model, ~50ms inference on CPU, EER ~1% on VoxCeleb |
| Diarization | **pyannote-audio 3.x** (segmentation + embedding) | Free, runs CPU, well-maintained |
| Vector storage | **SQLite + JSON blob** | No vector DB needed for single-user; just store ~10 owner voiceprints, compare via numpy cosine |
| Encryption at rest | Fly.io volume + AES-GCM at app layer using Fly secret as key | Voiceprints never leave the volume in plaintext |

## Enrollment flow (voice-driven, on-device)

1. User says "Jarvis, enroll me" (special MCP command from device).
2. Device displays prompt + Jarvis speaks: "Please repeat after me, sir. Phrase one of five."
3. User says: "The quick brown fox jumps over the lazy dog."
4. Repeat with 4 more phrases (sentences chosen to span phonemes/prosody).
5. Server extracts 5 embeddings, averages them with outlier rejection, stores as owner voiceprint.
6. Jarvis confirms: "Voiceprint enrolled. From now on, I'll respond only to you, sir."
7. Phrases stored: keep audio for 24 hours then delete (allow re-enrollment if first capture was bad), delete embeddings only after explicit user request.

## Threshold tuning

- Similarity threshold starts at 0.65 (configurable).
- Server logs every wake-word check with score (no audio retained).
- After 2 weeks of real-world use, tune threshold based on false-accept (bystander triggered) and false-reject (owner ignored) counts.
- Provide a voice command "Jarvis, raise/lower your voice gate" → adjusts threshold ±0.03.

## Failure modes (adversarial check)

| Failure | Severity | Mitigation |
|---|---|---|
| Owner has cold/laryngitis → voice changes → false reject | Medium | Allow temporary "passphrase fallback" — speak "Jarvis, listen to me, sir" with full sentence; LLM evaluates if utterance contains owner-specific knowledge ("you mentioned X yesterday") and grants temp pass. Time-limited (2 hours). |
| Recording attack (someone plays back owner's voice) | Low–Medium | Add liveness check: require small phrase variation per session ("Jarvis" + a randomly-prompted suffix word shown on device screen for first wake of session). Defer to v2 — initial release uses voiceprint only. Document as accepted risk. |
| Diarization model misattributes owner segment to bystander | Medium | If diarization confidence < 0.5, fall back to "wake-word-verified speaker only" mode (treat the whole utterance as owner's). Log occurrence for tuning. |
| Background noise/music degrades voiceprint | Medium | Apply pyannote VAD to extract clean speech segments before embedding. Reject embedding if SNR < 10 dB. |
| First-run before enrollment | Critical | If no enrolled voiceprint exists, fall back to current address-filter behavior (only respond if utterance contains "jarvis"). Display reminder to enroll on device. |

## Server-side data model

```sql
-- /opt/xiaozhi-esp32-server/data/voiceprints.db
CREATE TABLE voiceprints (
    user_id TEXT PRIMARY KEY,         -- "owner" for now; future-proofs multi-user
    enrolled_at INTEGER NOT NULL,     -- unix timestamp
    embedding_blob BLOB NOT NULL,     -- AES-GCM encrypted 192-dim float32 array
    sample_count INTEGER NOT NULL,
    last_updated INTEGER NOT NULL,
    threshold REAL DEFAULT 0.65
);

CREATE TABLE wake_attempts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    similarity REAL NOT NULL,
    accepted INTEGER NOT NULL,        -- 0/1
    -- no audio, no transcript stored here
);
```

## Non-goals (acknowledged, not in scope)

- Full anti-spoofing (deepfake voice attacks) — future hardening
- Real-time multi-speaker live transcription with attribution — future
- Speaker enrollment via web UI — only voice-driven for v1

---

# Sub-project 2 — Conversational Quality & Long-Term Memory

## Goal
Jarvis remembers what we discussed last week, knows my preferences, builds context over time, holds genuinely intelligent conversation — not just one-shot QA.

## Architecture

Two memory tracks, both stored on Fly.io volume:

**Track 1 — Episodic memory** (what was said, when)
- Every conversation turn is summarized (LLM call: "summarize this exchange in <40 words, capturing facts and intent") and stored.
- Embedded with `sentence-transformers/all-MiniLM-L6-v2` (free, ~80MB, runs CPU).
- On each new utterance: top-3 most relevant past summaries injected into system context as "Earlier you mentioned..."
- 90-day rolling window by default; user can voice-extend ("Jarvis, remember this permanently").

**Track 2 — Structured profile** (durable facts about the owner)
- LLM-extracted facts after each session: people mentioned, preferences, schedule patterns, project context.
- Stored in a structured table, not free text.
- Categories: people, places, projects, preferences, recurring_events, important_dates.
- Always injected at start of every session as "Here's what you know about the user..."

## Technology

| Component | Choice |
|---|---|
| Embeddings | sentence-transformers (free, local) |
| Vector search | sqlite-vss extension (free, fast for <100K vectors) |
| Profile extraction | Anthropic Claude (existing) — runs after session ends |
| RAG retrieval | Hybrid: vector similarity (top 5) + keyword match (top 3), reranked by recency |

## Enhanced conversational ability

Beyond memory, three quality boosters:

1. **Multi-turn context window**: keep last 10 turns of current session in working memory (currently the server treats each utterance more independently).
2. **Persona consistency check**: append to system prompt — "Maintain JARVIS persona: British, composed, dry wit, technical depth. Never break character. If asked something private, deflect politely." Test with adversarial prompts.
3. **Conversation strategy module**: classify each utterance as: command / question / thinking-aloud / social. Adjust response length and tone accordingly. Currently every response is treated the same.

## Schema

```sql
-- episodic.db
CREATE TABLE episodic (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    user_text TEXT NOT NULL,
    jarvis_text TEXT NOT NULL,
    summary TEXT NOT NULL,            -- <40 words
    embedding BLOB NOT NULL,          -- 384-dim sentence-transformers
    permanent INTEGER DEFAULT 0
);

-- profile.db
CREATE TABLE profile_facts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    category TEXT NOT NULL,           -- people | places | projects | preferences | dates
    key TEXT NOT NULL,                -- e.g. "wife_name", "favorite_coffee"
    value TEXT NOT NULL,
    confidence REAL DEFAULT 0.8,
    learned_at INTEGER NOT NULL,
    last_referenced INTEGER NOT NULL
);

CREATE INDEX idx_episodic_session ON episodic(session_id);
CREATE INDEX idx_profile_category ON profile_facts(category);
```

## Failure modes

| Failure | Mitigation |
|---|---|
| LLM hallucinates "facts" into profile | Store only with confidence ≥ 0.8 from extraction prompt; require corroboration across 2 sessions before category=people promotion |
| Memory recall surfaces stale/wrong info | Recency-weight retrieval; user can voice-correct ("Jarvis, that's wrong, forget that") → marks fact as deprecated |
| Database grows unbounded | 90-day TTL on non-permanent episodic; profile facts capped at 500 with LRU eviction |
| Privacy: user wants to wipe memory | "Jarvis, forget everything" voice command → wipes episodic, keeps profile; "Jarvis, forget me entirely" → wipes both + voiceprint |

## Non-goals

- Multi-user shared memory pool (single-user only, per Q7)
- Federated knowledge (other users' Jarvises talking to each other)
- Live external knowledge graph integration — that's Sub-project 3's RAG

---

# Sub-project 3 — Personal Assistant Integrations

## Goal
Jarvis can actually *do* things in the user's digital life: read calendar, send emails, control lights, play music, search the web, query the personal knowledge vault, and operate work tools (Slack, Monday, PCO).

## Architecture

All integrations exposed as **MCP tools** to Jarvis. xiaozhi-esp32-server already has MCP support — extend it. Integrations split into two categories:

### Category A — New MCP servers (we build)
Run as separate Fly.io machines or as plugins within `jarvis-server`.

| Integration | Auth | API | Cost |
|---|---|---|---|
| Microsoft 365 (calendar + email) | OAuth2 device flow | Microsoft Graph | Free for personal |
| Home Assistant | Long-lived access token | REST API | Free (self-hosted HA) |
| HomeKit | requires Homebridge or HA HomeKit bridge | via Home Assistant | Free |
| Web search | API key | **Brave Search API** (2,000 free queries/mo) — fallback to **DuckDuckGo HTML scraper** if exhausted | Free |
| Spotify | OAuth2 | Web API | Free for control with premium account |
| Apple Music | MusicKit JS / dev token | API | Free for control with subscription |
| Amazon Music | (no public API) | **Skip — recommend dropping** unless willing to use Alexa Skills bridge | n/a |
| llm-wiki vault | depends on what llm-wiki is | Need spec from user | Free if self-hosted |

### Category B — Existing Claude connectors (already built by user)
- Slack
- Monday.com
- Planning Center Online

These run somewhere already. Plan: expose them via MCP shim so Jarvis can invoke them through a single uniform interface. Likely option: a small `mcp-bridge` service that translates Jarvis MCP calls to the appropriate Claude connector endpoint.

## Tool design principles

1. **Read-first**: Jarvis can read calendar/email/Slack freely; **writes require explicit voice confirmation** ("Should I send this email to Bob, sir?").
2. **Voice-friendly responses**: Tool outputs go through a "voiceify" filter — long lists become "you have 4 meetings; the next is at 2pm with Sarah."
3. **Failure transparency**: If a tool fails (rate limit, auth expired), Jarvis explains plainly: "My Spotify connection has expired — shall I reconnect when you're at a computer?"

## Privilege levels

| Action | Level |
|---|---|
| Read calendar, email subjects, Slack channel summaries, weather, sensor data, music status | **Auto** (no confirmation) |
| Send email/Slack/Monday update, modify calendar, control smart home device | **Voice confirm required** |
| Delete calendar event, delete email, modify HA automation | **Spoken passphrase confirm** ("Jarvis confirm with passphrase: <random word from screen>") |

## Web search behavior

- Always cite source: "According to BBC, [headline]..."
- Cache identical queries for 1 hour to stay within Brave free tier
- Refuse certain query categories (financial advice, medical diagnosis) and redirect: "That's better answered by a professional, sir."

## Failure modes

| Failure | Mitigation |
|---|---|
| OAuth token expires mid-session | Auto-refresh; if refresh fails, queue the user request and prompt re-auth on dashboard |
| Smart home command issued, device offline | Jarvis acknowledges + offers retry: "The bedroom lamp didn't respond. Try again, sir?" |
| Brave free tier exhausted | Fall back to DuckDuckGo HTML scraper; warn user "I'm on backup search today, sir, results may be slower" |
| LLM hallucinates a fake calendar event | Structured tool outputs only — never let LLM invent events; events come strictly from Graph API JSON |

## Non-goals

- Voice-driven OAuth onboarding for cloud services (do via web dashboard once, then headless)
- Cross-account email (only the owner's primary M365)
- E-commerce/shopping ("Jarvis, buy me X") — explicit non-goal for safety

---

# Sub-project 4 — Reliability & Operations

## Goal
The stack stays up. Failures are loud and recoverable. Secrets are safe. The owner can sleep at night.

## Components

### 1. TTS provider failover
Add chained fallback: EdgeTTS (primary, free) → OpenAITTS (fable, secondary, paid) → MicrosoftAzureTTS (Ryan, tertiary, free tier 500K/mo) → ElevenLabs (only if explicitly topped up).
- Health check each provider before TTS call (HEAD request, 1s timeout).
- On TTS error, retry with next provider; user hears their voice slightly shift but doesn't experience silence.

### 2. Secrets to Fly.io secrets
Move all keys out of `.config.yaml.cloud`:
- `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `GROQ_API_KEY`, `BRAVE_API_KEY`, `MS_GRAPH_*`, `HA_TOKEN`, `SPOTIFY_*`
- `.config.yaml.cloud` references via `${ENV_VAR}` syntax (requires patch to xiaozhi-esp32-server config loader — small change)

### 3. Health monitoring
- Add `/healthz` endpoint that checks: LLM reachable, STT reachable, TTS chain has ≥1 provider up, voiceprint DB readable, episodic DB writable.
- Fly.io health check pinged every 60s; on failure, machine restart.
- Daily report email/Slack: yesterday's stats — wake attempts, accept rate, conversations, tool calls, errors.

### 4. Logging discipline
- All TTS/LLM/tool errors logged with structured JSON (provider, error code, request hash).
- Audio data **never** logged.
- Log retention: 30 days on Fly volume, then deleted.

### 5. Backup & disaster recovery
- Nightly snapshot of `/opt/xiaozhi-esp32-server/data` to encrypted Backblaze B2 bucket (10GB free).
- Voiceprints, episodic, profile all included.
- 7-day retention; manual restore via documented runbook.

## Non-goals

- 99.99% uptime SLA — this is a personal device, ~99% is fine
- Multi-region failover — single Fly.io region (DFW) is acceptable
- PagerDuty integration — daily digest is enough

---

# Sub-project 5 — Code Quality & Repo Consolidation

## Goal
One canonical firmware repo, one canonical backend repo. Everything else archived. Code is testable, reviewable, ready for collaborators.

## Repo consolidation

### Final state — 2 repos

**`esp32-s3-box-3`** (public, current `~/esp-box3-jarvis`)
- ESP-IDF firmware
- Sub-project 1 NVS schema for voiceprint reference
- Local dev `local-bridge/` directory deprecated and removed (production goes through Fly server only)

**`jarvis-server-fly`** (private, current `~/xiaozhi-server-fly`)
- Dockerfile, fly.toml, entrypoint.sh
- Speaker ID module (new in Sub-project 1)
- Memory module (new in Sub-project 2)
- Integration MCP servers (Sub-project 3)
- Reliability/ops scripts (Sub-project 4)
- Renamed canonically — currently the dir is named `xiaozhi-server-fly` but the Fly app is `jarvis-server`. Rename local dir to match: `~/jarvis-server`

### Archived (move to `~/Archive/`, don't delete)
- `~/esp-box3` — older fork, pre-jarvis branding
- `~/jarvis-bridge` — superseded by jarvis-server (the Fly bridge approach was abandoned)
- `~/jarvis-agent-shim` — purpose unclear, archive pending review

## Firmware refactor (selective, not god-class rewrite)

`application.cc` is large. Don't do a wholesale rewrite. Extract three concerns into new files:

1. `ota_handler.cc` — OTA check, download, version logic (currently scattered across `application.cc` and `ota.cc`)
2. `mcp_dispatcher.cc` — MCP tool registration is already in `mcp_server.cc`, but the wiring between application state and MCP lives in `application.cc`. Extract.
3. `wake_event_handler.cc` — wake word detection → audio capture → server send pipeline, currently inline in application.cc

Leave the rest of `application.cc` alone unless extension demands changes. **No rewriting for the sake of rewriting.**

## CI/CD

**Firmware repo**:
- GitHub Action: `idf.py build` on every push to main against ESP-Box-3 target
- Optional: clang-tidy on changed files

**Server repo**:
- GitHub Action: `pytest` on every push (target: Sub-project 1 + 2 modules have unit tests for embedding logic, threshold checks, memory recall)
- `fly deploy --build-only` smoke test (catches Dockerfile breakage)

## Testing strategy

- Speaker ID: dataset of 5+ owner samples + 50 non-owner samples → automated pass rate test on every change
- Memory: synthetic conversation transcripts → verify retrieval precision/recall
- Integrations: mock MCP responses → verify Jarvis handles errors gracefully

## Documentation

Keep `docs/JARVIS_AGENT_SETUP.md` updated. Add:
- `docs/SPEAKER_ENROLLMENT.md` (user-facing how-to)
- `docs/INTEGRATIONS.md` (which services Jarvis can talk to and how to authorize)
- `docs/ARCHITECTURE.md` (one-page diagram of firmware ↔ Fly server ↔ external APIs)
- `docs/RUNBOOK.md` (what to do when X breaks)

## Non-goals

- Open-sourcing the server repo (stays private — contains personal data schemas and integration credentials by reference)
- Rewriting in Rust/Go — Python on the server, C++ on firmware, both stay as-is
- Microservice architecture — monolithic Fly app is correct for this scale

---

# Cross-cutting failure-mode check

Per the brainstorming skill — top 3 ways this whole plan could fail:

1. **Critical: Voice biometric false reject rate too high in real use** — user gets ignored when sick or whispering. *Mitigation*: passphrase fallback (Sub-project 1), threshold auto-tuning, voice command to lower gate temporarily. Documented as known limitation.

2. **Critical: Integration sprawl creates auth nightmares** — 8+ services to keep authorized; tokens expire at random; user can't tell what's broken. *Mitigation*: dedicated Sub-project 4 for health monitoring + daily digest. Each integration has documented re-auth path. **Defer optional integrations until core 4 (M365, HA, web search, music) are stable.**

3. **Minor (accepted): Scope creep delays Sub-project 1 (the user's actual #1 pain point)** — building all 5 sub-projects in parallel risks nothing shipping. *Mitigation*: enforce phase order. Phase 0 (consolidate) + Phase 1 (speaker ID) ship before any Phase 3 work begins.

---

# Roll-out phases

| Phase | Sub-projects | Definition of done |
|---|---|---|
| 0 | 5 (consolidation) | 2 canonical repos, others archived; CI green; docs updated |
| 1 | 1 (speaker ID) | Owner enrolled; bystander false-accept < 5%; documented threshold |
| 1.5 | 4 (reliability) | Secrets in Fly secrets; TTS failover working; daily digest emailing |
| 2 | 2 (memory) | Episodic recall demonstrably working in 3+ multi-day test conversations |
| 3 | 3 (integrations) | M365 calendar + HA + web search + 1 music service operational; remaining integrations queued |

No fixed dates — Q10 says build it right.

---

## Approval checklist

- [x] Scope decomposed into independently-shippable sub-projects
- [x] Failure modes identified and mitigated (or documented as accepted)
- [x] Technology choices justified against constraints (free-first per Q9)
- [x] Privacy posture explicit (Sub-project 1, 2)
- [x] Roll-out order minimizes risk of nothing shipping
- [ ] **User approval to proceed to `writing-plans`** ← awaiting

---

*Next step on user approval: invoke `writing-plans` for Sub-project 0 + 1 (the unblockers).*
