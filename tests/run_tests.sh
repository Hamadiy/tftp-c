#!/bin/sh
# Integration tests: start the server, then exercise it with the client.
set -u

PORT=6969
CLIENT=./bin/tftp
SERVER=./bin/tftpd
ROOT=$(mktemp -d)
WORK=$(mktemp -d)
pass=0
fail=0

"$SERVER" -p "$PORT" -d "$ROOT" > "$WORK/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null; rm -rf "$ROOT" "$WORK"' EXIT
sleep 0.5

ok() { echo "  [PASS] $1"; pass=$((pass + 1)); }
ko() { echo "  [FAIL] $1"; fail=$((fail + 1)); }

tftp() { "$CLIENT" -p "$PORT" 127.0.0.1 "$@" > /dev/null 2>&1; }

roundtrip() { # name size
    head -c "$2" /dev/urandom > "$WORK/$1.src"
    if tftp put "$WORK/$1.src" "$1" && tftp get "$1" "$WORK/$1.dst" \
        && cmp -s "$WORK/$1.src" "$WORK/$1.dst"; then
        ok "upload + download of a $2-byte file"
    else
        ko "upload + download of a $2-byte file"
    fi
}

echo "Running TFTP integration tests"
roundtrip big 300000      # many blocks
roundtrip aligned 1024    # multiple of 512: needs a final empty DATA block
roundtrip empty 0         # empty file
roundtrip small 100       # single short block

if tftp get does-not-exist "$WORK/none"; then ko "missing file is reported"; else ok "missing file is reported"; fi
if [ ! -e "$WORK/none" ]; then ok "no partial file left on failure"; else ko "no partial file left on failure"; fi
if tftp get ../etc/passwd "$WORK/passwd"; then ko "path traversal is refused"; else ok "path traversal is refused"; fi
if tftp put "$WORK/big.src" big; then ko "existing file is not overwritten"; else ok "existing file is not overwritten"; fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
