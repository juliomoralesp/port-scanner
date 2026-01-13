#!/usr/bin/env bash
# Simple installer for the `ports` binary and man page
set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
BIN_DIR="$PREFIX/bin"
MAN_DIR="$PREFIX/share/man/man1"

usage(){
  cat <<EOF
Usage: $0 [BINARY_PATH]
Installs the ports binary to $BIN_DIR and man page to $MAN_DIR.
If BINARY_PATH is omitted, uses ./ports in the repository root.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

BINARY_PATH=${1:-./ports}

if [[ ! -f "$BINARY_PATH" ]]; then
  echo "Error: binary not found at $BINARY_PATH" >&2
  exit 2
fi

echo "Installing $BINARY_PATH to $BIN_DIR"
sudo mkdir -p "$BIN_DIR" "$MAN_DIR"
sudo install -m 0755 "$BINARY_PATH" "$BIN_DIR/ports"

echo "Installing man page to $MAN_DIR"
sudo install -m 0644 man/ports.1 "$MAN_DIR/ports.1"
if command -v gzip >/dev/null 2>&1; then
  sudo gzip -f "$MAN_DIR/ports.1"
fi

if command -v mandb >/dev/null 2>&1; then
  echo "Updating man database (mandb)"
  sudo mandb || true
fi

echo "Installation complete. You can run: $BIN_DIR/ports"
