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

case "$(uname -m)" in
	x86_64) ASSET_PLATFORM=linux-amd64 ;;
	aarch64 | arm64) ASSET_PLATFORM=linux-arm64 ;;
	*) echo "unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
RELEASE="v0.0.0-trex"
URL="http://127.0.0.1:$PORT/$RELEASE"
SHELL_ERR="$FIX/shell.stderr"

fail() { cat "$SHELL_ERR" >&2; echo "FAIL: $*" >&2; exit 1; }
requests() { grep -c '"GET ' "$LOG" || true; }

# open a database through the shell (DuckDB constructor -> LoadAllExtensions) with a fresh
# HOME; extra args are env assignments. The query must still answer 42
start_db() {
	rm -rf "$FIX/home" && mkdir -p "$FIX/home"
	local out
	out="$(env -u TREX_EXTENSIONS TREX_RELEASE_URL="$URL" HOME="$FIX/home" "$@" \
		"$BUILD/duckdb" -no-agent -noheader -list -c "SELECT 42" 2>"$SHELL_ERR")" || fail "startup failed"
	[[ "$out" == "42" ]] || fail "unexpected shell output '$out'"
}

# 1. TREX_EXTENSIONS unset, every asset 404s: startup succeeds and warns once per extension
before=$(requests)
start_db
grep -q "\"GET /$RELEASE/chdb-$ASSET_PLATFORM.trex HTTP" "$LOG" || fail "no request for chdb-$ASSET_PLATFORM.trex"
grep -q "Warning: Failed to download trex extension 'chdb'.*404" "$SHELL_ERR" || fail "no 404 warning"
# each warning is a single line: nothing but warnings and the summary, no suggestion block
if grep -vE "^(Warning: Failed to download trex extension '[a-z_]+': .*|Trex extensions: .*)$" "$SHELL_ERR"; then
	fail "unexpected stderr lines (multi-line warning?)"
fi
[[ $(grep -c '^Warning: ' "$SHELL_ERR") -eq 9 ]] || fail "expected 9 single-line warnings"
grep "Trex extensions:" "$SHELL_ERR"
echo "ok: 404 auto-install warned, $(( $(requests) - before )) requests for <ext>-$ASSET_PLATFORM.trex"

# 2. TREX_EXTENSIONS=none / empty: no download attempt, no output
for value in none ""; do
	before=$(requests)
	start_db TREX_EXTENSIONS="$value"
	[[ ! -s "$SHELL_ERR" ]] || fail "TREX_EXTENSIONS='$value' wrote to stderr"
	[[ $(requests) -eq $before ]] || fail "TREX_EXTENSIONS='$value' made download requests"
	echo "ok: TREX_EXTENSIONS='$value' skipped the auto-install"
done

# 3. a published <ext>-<platform>.trex installs as <ext>.trex and loads by name
mkdir -p "$WWW/$RELEASE"
cp "$FIX/signed/loadable_extension_demo.trex" "$WWW/$RELEASE/loadable_extension_demo-$ASSET_PLATFORM.trex"
start_db TREX_EXTENSIONS=loadable_extension_demo
grep -q "1 downloaded, 0 skipped, 0 failed" "$SHELL_ERR" || fail "platform asset was not installed"
INSTALLED="$(find "$FIX/home" -name '*.trex')"
[[ "$(basename "$INSTALLED")" == "loadable_extension_demo.trex" ]] || fail "installed as '$INSTALLED'"
OUT="$(HOME="$FIX/home" TREX_EXTENSIONS=none "$BUILD/duckdb" -no-agent -noheader -list \
	-cmd "LOAD loadable_extension_demo" -c "SELECT test_alias_hello()" 2>&1)" || { echo "$OUT" >&2; fail "LOAD by name failed"; }
[[ "$OUT" == "Hello Alias!" ]] || fail "unexpected demo output '$OUT'"
echo "ok: $RELEASE/loadable_extension_demo-$ASSET_PLATFORM.trex installed as loadable_extension_demo.trex and loaded"

echo "PASS: trex tests"
