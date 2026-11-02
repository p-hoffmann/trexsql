#!/usr/bin/env bash
# Builds the .trex fixtures and runs test/trex/*.test.
#
# The engine must be configured with the throwaway test key, e.g.:
#   test/trex/run_trex_tests.sh keygen
#   cmake -B build -DDUCKDB_CUSTOM_SIGNING_KEYS="$(cat build/trex_fixtures/test_key.pub)" ...
#   cmake --build build --target unittest loadable_extension_demo_loadable_extension
#   test/trex/run_trex_tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
FIX="$BUILD/trex_fixtures"
SIGN="$ROOT/scripts/extension_signing/duckdb-sign.sh"

if [[ "${1:-}" == "keygen" ]]; then
	mkdir -p "$FIX"
	[[ -f "$FIX/test_key.pem" ]] || bash "$SIGN" keygen -o "$FIX/test_key" >/dev/null
	echo "$FIX/test_key.pub"
	exit 0
fi

[[ -f "$FIX/test_key.pem" ]] || { echo "run '$0 keygen' and rebuild with that key first" >&2; exit 1; }
DEMO="$BUILD/test/extension/loadable_extension_demo.duckdb_extension"
[[ -f "$DEMO" ]] || { echo "missing $DEMO: build loadable_extension_demo_loadable_extension" >&2; exit 1; }

# the init symbol is derived from the file name, so both copies keep the demo's name
rm -rf "$FIX/signed" "$FIX/unsigned"
mkdir -p "$FIX/signed" "$FIX/unsigned"
cp "$DEMO" "$FIX/unsigned/loadable_extension_demo.trex"
bash "$SIGN" sign -k "$FIX/test_key.pem" -o "$FIX/signed/loadable_extension_demo.trex" "$DEMO" >/dev/null

# an empty directory served over http: every trex auto-install gets a 404
WWW="$(mktemp -d)"
LOG="$(mktemp)"
python3 -u -m http.server 0 --bind 127.0.0.1 --directory "$WWW" >"$LOG" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; rm -rf "$WWW" "$LOG"' EXIT
PORT=""
for _ in $(seq 50); do
	PORT="$(grep -oE 'port [0-9]+' "$LOG" | grep -oE '[0-9]+' || true)"
	[[ -n "$PORT" ]] && break
	sleep 0.1
done
[[ -n "$PORT" ]] || { echo "http server did not start" >&2; exit 1; }

cd "$ROOT"
STDERR="$FIX/unittest.stderr"
set +e
env -u TREX_EXTENSIONS TREX_FIXTURES="$FIX" TREX_RELEASE_URL="http://127.0.0.1:$PORT/v0.0.0-trex" \
	"$BUILD/test/unittest" "test/trex/*" 2>"$STDERR"
RC=$?
set -e
[[ $RC -eq 0 ]] || { cat "$STDERR" >&2; echo "FAIL: unittest exited $RC" >&2; exit $RC; }

# startup (DuckDB constructor -> LoadAllExtensions) with TREX_EXTENSIONS unset and a
# 404 release URL: must still succeed and warn on stderr
SHELL_ERR="$FIX/shell.stderr"
rm -rf "$FIX/home" && mkdir -p "$FIX/home"
OUT="$(env -u TREX_EXTENSIONS TREX_RELEASE_URL="http://127.0.0.1:$PORT/v0.0.0-trex" \
	HOME="$FIX/home" "$BUILD/duckdb" -no-agent -noheader -list -c "SELECT 42" 2>"$SHELL_ERR")" ||
	{ cat "$SHELL_ERR" >&2; echo "FAIL: startup failed" >&2; exit 1; }
[[ "$OUT" == "42" ]] || { echo "FAIL: unexpected shell output '$OUT'" >&2; exit 1; }
if ! grep -q "Warning: Failed to download trex extension 'chdb'.*404" "$SHELL_ERR"; then
	cat "$SHELL_ERR" >&2
	echo "FAIL: no 404 warning for the trex auto-install on stderr" >&2
	exit 1
fi
grep "Trex extensions:" "$SHELL_ERR"
echo "PASS: trex tests; startup survived the 404 auto-install and warned"
