#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

D8_BIN="${D8_BIN:-d8}"
javac --release 8 -d "$TMP_DIR" "$ROOT_DIR/sentinel/Sentinel.java"
"$D8_BIN" --min-api 26 --output "$TMP_DIR" "$TMP_DIR"/*.class
mkdir -p "$ROOT_DIR/assets"
cp "$TMP_DIR/classes.dex" "$ROOT_DIR/assets/sentinel.dex"
echo "Built $ROOT_DIR/assets/sentinel.dex ($(wc -c < "$ROOT_DIR/assets/sentinel.dex") bytes)"
