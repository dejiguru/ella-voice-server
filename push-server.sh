#!/usr/bin/env bash

set -euo pipefail

ROOT_REPO="/home/dejiguru/Arduino/ellabox"
BACKEND_REPO="/home/dejiguru/Arduino/ellabox/backend"
COMMIT_MESSAGE="${1:-Use Nova-3 for Deepgram STT}"

push_repo() {
  local repo_path="$1"

  if [[ ! -d "$repo_path/.git" ]]; then
    echo "Skipping $repo_path: not a git repo"
    return 0
  fi

  if git -C "$repo_path" diff --quiet -- server.js && git -C "$repo_path" diff --quiet --cached -- server.js; then
    echo "No server.js changes in $(basename "$repo_path")"
    return 0
  fi

  git -C "$repo_path" add -- server.js

  if git -C "$repo_path" diff --cached --quiet -- server.js; then
    echo "No staged server.js changes in $(basename "$repo_path")"
    return 0
  fi

  git -C "$repo_path" commit -m "$COMMIT_MESSAGE"
  git -C "$repo_path" push origin main
}

push_repo "$ROOT_REPO"
push_repo "$BACKEND_REPO"
