#!/usr/bin/env bash
# Create a NEW GitHub repository from this firmware tree and push the current branch.
# Requires: GitHub CLI (https://cli.github.com/) authenticated via `gh auth login`.
#
# Usage:
#   ./scripts/publish-new-github-repo.sh <owner>/<repo-name> [remote-name]
# Example:
#   ./scripts/publish-new-github-repo.sh jersilb1400/fw-jarvis-voice jarvis-voice

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OWNER_REPO="${1:-}"
REMOTE_NAME="${2:-jarvis-voice}"
DESC="${DESC:-ESP32 Box-3 Jarvis firmware: wake-only chat, Multinet sleep phrase, silence hang-up, session JSON control, contrib speaker-verify tooling.}"

if [[ -z "$OWNER_REPO" || "$OWNER_REPO" != *"/"* ]]; then
  echo "Usage: $0 <owner>/<repo-name> [remote-name]"
  echo "Example: $0 jersilb1400/fw-jarvis-voice jarvis-voice"
  exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "error: gh (GitHub CLI) not found. Install from https://cli.github.com/"
  exit 1
fi

git status >/dev/null 2>&1 || {
  echo "error: $ROOT is not a git repository."
  exit 1
}

# Commit local changes so the remote contains everything.
if [[ -n "$(git status --porcelain=v1 2>/dev/null)" ]]; then
  git add -A
  git commit -m "$(cat <<'EOF'
feat(voice): wake-only, sleep word, silence end session, session JSON, speaker verify

Multinet wake/sleep commands, idle standby with NVS, optional speaker_verify hello flag,
and contrib jarvis-server speaker verification helpers.
EOF
)"
fi

CURRENT_BRANCH="$(git branch --show-current)"

if gh repo view "$OWNER_REPO" >/dev/null 2>&1; then
  echo "Repository $OWNER_REPO already exists — pushing branch $CURRENT_BRANCH to remote $REMOTE_NAME."
  if git remote get-url "$REMOTE_NAME" >/dev/null 2>&1; then
    git remote set-url "$REMOTE_NAME" "https://github.com/${OWNER_REPO}.git"
  else
    git remote add "$REMOTE_NAME" "https://github.com/${OWNER_REPO}.git"
  fi
  git push -u "$REMOTE_NAME" "$CURRENT_BRANCH"
else
  echo "Creating $OWNER_REPO and pushing from $PWD ..."
  gh repo create "$OWNER_REPO" \
    --public \
    --description "$DESC" \
    --source=. \
    --remote="$REMOTE_NAME" \
    --push
fi

echo "Done. Remote $REMOTE_NAME -> https://github.com/${OWNER_REPO}"
echo "(Original origin remote is untouched so you can still pull upstream if needed.)"
