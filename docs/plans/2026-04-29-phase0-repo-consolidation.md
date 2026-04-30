# Phase 0 — Repo Consolidation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use the appropriate execution skill (`executing-plans` or `subagent-driven-development`) to implement this plan.

**Goal:** Collapse 5 scattered repos to 2 canonical repos (`esp32-s3-box-3` firmware + `jarvis-server` backend), archive the rest, add baseline CI, prepare a clean foundation for Phase 1.
**Architecture:** Two-repo split — public firmware repo (current `~/esp-box3-jarvis`) and private server repo (current `~/xiaozhi-server-fly`, renamed to `~/jarvis-server`). Older copies move to `~/Archive/`. Both repos get a GitHub Actions CI smoke test (firmware: `idf.py build`, server: `pytest` + `fly deploy --build-only`).
**Tech Stack:** ESP-IDF v5.5.x, Python 3.11, Docker, Fly.io, GitHub Actions, git
**Assumptions:**
- Assumes the user has admin access to both Fly.io apps (`jarvis-server`, `jarvis-bridge`) — will NOT work if access has been transferred.
- Assumes `~/esp-box3-jarvis` is already on GitHub at `jersilb1400/esp32-s3-box-3` and `~/xiaozhi-server-fly` is local-only — will NOT work if server repo is also on GitHub under a different name (rename remote first).
- Assumes user wants to retain `jarvis-bridge` Fly.io app for cost reasons (it's tiny and stopped) — will NOT work if user wants the app fully deleted (extra step needed).

---

### Task 1: Create archive directory and move deprecated repos

**Files:**
- Create: `~/Archive/`

**Step 1: Move deprecated repos**
```bash
mkdir -p ~/Archive
mv ~/esp-box3 ~/Archive/esp-box3-deprecated-2026-04-29
mv ~/jarvis-bridge ~/Archive/jarvis-bridge-deprecated-2026-04-29
mv ~/jarvis-agent-shim ~/Archive/jarvis-agent-shim-deprecated-2026-04-29
```

**Step 2: Verify**
```bash
ls ~/Archive/
ls ~ | grep -E "esp-box3$|jarvis-bridge$|jarvis-agent-shim$"
```
Expected: 3 dirs in archive; `grep` returns nothing (those repos no longer in `~/`).

**Step 3: Commit (no commit — local-only filesystem move)**

---

### Task 2: Rename server dir to canonical name

**Files:**
- Move: `~/xiaozhi-server-fly` → `~/jarvis-server`

**Step 1: Rename**
```bash
mv ~/xiaozhi-server-fly ~/jarvis-server
```

**Step 2: Verify Fly app still deploys from new path**
```bash
cd ~/jarvis-server && fly status --app jarvis-server
```
Expected: app status reports machine running; no path warnings.

**Step 3: No git change required (server repo is not in git per earlier check). Commit if/when initialized in Task 4.**

---

### Task 3: Rename firmware dir to canonical name

**Files:**
- Move: `~/esp-box3-jarvis` → `~/esp32-s3-box-3` (matches GitHub repo name)

**Step 1: Move**
```bash
mv ~/esp-box3-jarvis ~/esp32-s3-box-3
```

**Step 2: Verify git remote still resolves**
```bash
cd ~/esp32-s3-box-3 && git remote -v
```
Expected: `origin  https://github.com/jersilb1400/esp32-s3-box-3.git (fetch)`

**Step 3: Commit (no firmware code changed — directory rename only, no commit)**

---

### Task 4: Initialize git repo for jarvis-server

**Files:**
- Create: `~/jarvis-server/.git`
- Create: `~/jarvis-server/.gitignore`

**Step 1: Initialize and write gitignore**
```bash
cd ~/jarvis-server
git init --quiet
cat > .gitignore <<'EOF'
.config.yaml
data/
*.pyc
__pycache__/
.venv/
.env
*.db
*.sqlite
EOF
```

**Step 2: First commit (excluding the secret-containing config)**
```bash
git add -A
git commit -m "Initial commit: jarvis-server (xiaozhi-esp32-server Fly.io deployment)"
```

**Step 3: Verify no secrets committed**
```bash
git log --all --full-history -- '*.config.yaml' '*.env' 2>&1 | head
git ls-files | grep -E '\.config\.yaml$|\.env$'
```
Expected: no secret files tracked.

---

### Task 5: Create GitHub Action — firmware build smoke test

**Files:**
- Create: `~/esp32-s3-box-3/.github/workflows/firmware-build.yml`

**Step 1: Add failing CI (branch off main first)**
```bash
cd ~/esp32-s3-box-3
git checkout -b ci/firmware-smoke-test
mkdir -p .github/workflows
```

**Step 2: Write workflow**

`.github/workflows/firmware-build.yml`:
```yaml
name: Firmware Build (smoke test)
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
jobs:
  build:
    runs-on: ubuntu-latest
    container: espressif/idf:v5.5
    steps:
      - uses: actions/checkout@v4
      - name: Set IDF target
        run: . $IDF_PATH/export.sh && idf.py set-target esp32s3
      - name: Build
        run: . $IDF_PATH/export.sh && idf.py build -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp-box3-xiaozhi"
```

**Step 3: Verify**
```bash
cd ~/esp32-s3-box-3
git add .github/workflows/firmware-build.yml
git commit -m "ci: add firmware build smoke test"
git push -u origin ci/firmware-smoke-test
```
Then in GitHub UI, create PR and watch the action run. Expected: green check within ~10 min.

**Step 4: Merge PR after green**
```bash
gh pr merge --squash
```

---

### Task 6: Create GitHub Action — server build smoke test (private repo path)

**Files:**
- Create: `~/jarvis-server/.github/workflows/server-build.yml`
- Create: `~/jarvis-server/tests/test_smoke.py`

**Step 1: Decide on remote**
The server repo is private and currently local-only. Either:
- (a) Push to GitHub as a private repo (recommended), OR
- (b) Skip CI for server until repo is on a host

For (a):
```bash
cd ~/jarvis-server
gh repo create jarvis-server --private --source=. --remote=origin
git push -u origin main
```

**Step 2: Write smoke test**

`tests/test_smoke.py`:
```python
def test_imports():
    """Smoke: ensure core modules import without error."""
    import yaml
    with open("/dev/null", "w"):
        pass
    assert yaml is not None
```

**Step 3: Write workflow**

`.github/workflows/server-build.yml`:
```yaml
name: Server build + test
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.11'
      - run: pip install pyyaml pytest
      - run: pytest tests/ -v
  docker-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: docker build -t jarvis-server-smoke .
```

**Step 4: Commit and verify**
```bash
git add tests/ .github/
git commit -m "ci: add server smoke test and docker build check"
git push
```
Expected: workflows run on GitHub, both green.

---

### Task 7: Add CLAUDE.md to firmware repo

**Files:**
- Create: `~/esp32-s3-box-3/CLAUDE.md`

**Step 1: Use the claude-md-creator skill** (per the routing guide — never write CLAUDE.md by hand)

Invoke: `superpowers-optimized:claude-md-creator` with prompt:
```
Create a minimal CLAUDE.md for ~/esp32-s3-box-3.
- ESP-IDF v5.5.x C++ embedded project
- Backend lives at ~/jarvis-server (separate repo, private)
- Wake word: "jarvis", board: ESP-Box-3 with sensor dock
- Active branch must be main; canonical OTA URL: https://jarvis-server.fly.dev/xiaozhi/ota/
- Direct future agents to docs/plans/ for active enhancement work
```

**Step 2: Verify file created**
```bash
test -f ~/esp32-s3-box-3/CLAUDE.md && head -30 ~/esp32-s3-box-3/CLAUDE.md
```
Expected: file exists with ≤80 lines of project context.

**Step 3: Commit**
```bash
cd ~/esp32-s3-box-3
git add CLAUDE.md
git commit -m "docs: add CLAUDE.md for agent onboarding"
git push
```

---

### Task 8: Add CLAUDE.md to server repo

**Files:**
- Create: `~/jarvis-server/CLAUDE.md`

Mirrors Task 7 but for the server repo. Invoke `claude-md-creator` with the appropriate context (Python, xiaozhi-esp32-server fork, Fly.io app `jarvis-server`, persistent volume at `/opt/xiaozhi-esp32-server/data/`, secrets via `fly secrets`).

---

### Task 9: Document the consolidation in firmware README

**Files:**
- Modify: `~/esp32-s3-box-3/README.md`

**Step 1: Add a "Project topology" section near the top**

After the existing intro paragraph, insert:
```markdown
## Project topology

This repo contains ESP32-S3-Box-3 firmware. The companion backend is in a separate
repo (`jarvis-server`, private) deployed at `https://jarvis-server.fly.dev/`.

Other repos referenced historically (`jarvis-bridge`, `jarvis-agent-shim`,
`esp-box3`) are deprecated and archived locally.
```

**Step 2: Verify**
```bash
grep -A 4 "Project topology" ~/esp32-s3-box-3/README.md
```
Expected: section appears.

**Step 3: Commit**
```bash
git add README.md
git commit -m "docs: clarify project topology after consolidation"
git push
```

---

### Task 10: Verify Fly.io still operates after rename

**Step 1: End-to-end smoke**
```bash
curl -s https://jarvis-server.fly.dev/healthz | head
fly logs --app jarvis-server --no-tail | tail -5
```
Expected: healthz returns OK; logs show recent activity.

**Step 2: Test from device** — say "Jarvis", verify TTS audio plays. No firmware change, no server change in this phase, so behavior must be identical to pre-consolidation.

**Step 3: If broken, rollback**
```bash
mv ~/jarvis-server ~/xiaozhi-server-fly  # undo Task 2 rename
mv ~/esp32-s3-box-3 ~/esp-box3-jarvis    # undo Task 3 rename
```

---

## Definition of Done

- [ ] 3 deprecated repos in `~/Archive/`
- [ ] `~/jarvis-server` and `~/esp32-s3-box-3` are the only active repos in home dir
- [ ] Both repos have green CI on main
- [ ] Both repos have a `CLAUDE.md`
- [ ] Device still responds to "Jarvis" with audio (no regression)
- [ ] Firmware README documents the topology
