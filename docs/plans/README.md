# Jarvis Enhancement Plans (April 29, 2026)

## Overview

Comprehensive 5-phase build plan to make this device the most versatile ESP32-S3 personal AI assistant available, with owner-only voice gating, long-term memory, and real-world integrations.

## Documents

| File | Purpose |
|------|---------|
| `2026-04-29-jarvis-enhancement-plan.md` | **Master design doc** — vision, sub-project decomposition, technology choices, failure modes, decisions captured from Q&A |
| `2026-04-29-phase0-repo-consolidation.md` | Phase 0 implementation plan: 5 repos → 2 canonical, baseline CI |
| `2026-04-29-phase1-speaker-identification.md` | Phase 1 implementation plan: voiceprint + diarization (the #1 pain point) |
| `2026-04-29-phase1.5-reliability-operations.md` | Phase 1.5 implementation plan: TTS failover, secrets hygiene, monitoring, backups |
| `2026-04-29-phase2-conversational-memory.md` | Phase 2 implementation plan: episodic recall + structured profile + persona |
| `2026-04-29-phase3-personal-assistant-integrations.md` | Phase 3 implementation plan: M365, HA, web search, music, llm-wiki, Slack/Monday/PCO |

## Reading order

1. Start with the **master design doc** to understand the vision and decision context.
2. Read phase plans in order: 0 → 1 → 1.5 → 2 → 3.
3. Each phase plan can be executed independently once its dependencies are met.

## Phase dependencies

```
Phase 0 (consolidation)
    ↓
Phase 1 (speaker ID) ──┬──→ Phase 2 (memory) ──→ Phase 3 (integrations)
                       │
Phase 1.5 (reliability) ┘  (parallel with Phase 1)
```

## Open spec questions

Resolved decisions for implementation live in the private server repo:
`jarvis-server/docs/PHASE3_SPEC_ANSWERS.md` (synced with this tree when plans are copied).

Legacy prompts (historical):
1. **llm-wiki vault** — default: markdown directory on Fly volume (`/data/vault`); see SPEC answers.
2. **Existing Claude connectors** — base URLs via `fly secrets` + MCP bridge; see SPEC answers.
3. **Spotify vs Apple Music** — Spotify primary; see SPEC answers.

**Out of scope (closed):** **Amazon Music** — no public API; removed from plans (Spotify + optional Apple Music only).

## Open decisions deferred to v2

- Anti-spoofing against deepfake voice attacks
- Multi-user enrollment
- Mobile companion app for OAuth flows
- Enterprise/SSO integrations

## Execution

Pick one approach to execute these plans:

**Recommended (full session):** invoke `superpowers-optimized:subagent-driven-development` with the path to the relevant phase plan. Subagents handle independent tasks in parallel with per-task review gates.

**Alternative (separate sessions):** invoke `superpowers-optimized:executing-plans` in a fresh Claude Code session with the path to one phase plan at a time.

Either way: complete Phase 0 fully before starting Phase 1.
