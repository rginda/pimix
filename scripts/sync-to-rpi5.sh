#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_HOST="${1:-rpi5-dev}"
REMOTE_DIR="src/pimix"

ssh "$REMOTE_HOST" mkdir -p "$REMOTE_DIR"
rsync -avz --delete \
    --exclude 'build/' \
    --exclude 'node_modules/' \
    --exclude '.git/' \
    "$REPO_ROOT/" "${REMOTE_HOST}:${REMOTE_DIR}/"
