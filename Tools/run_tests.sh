#!/usr/bin/env bash
# Build the host editor target and run a VaRest automation filter headless.
#
#   Tools/run_tests.sh [--no-build] [--] [Filter]      (default filter: VaRest)
#
# A bare filter is anchored at the start of the test path (it is passed as ^Filter), so
# benchmarks named Bench.VaRest.* never run in the default suite; run them with
# `Tools/run_tests.sh Bench.VaRest`. Explicit StartsWith:/Group: filters pass through as given.
# VAREST_EXTRA_ARGS is appended to the editor command line (e.g. -llm for the benchmarks).
# VAREST_TEST_TIMEOUT (seconds, default 1800) bounds the editor run: a hung test fails the run
# with "timed out" instead of blocking forever.
# The runner starts Tools/testserver/server.py on ports the OS picks (plain, TLS and silent) and passes its URLs to the editor
# as -VaRestTestServer=<url>, -VaRestTestServerTls=<url> and -VaRestTestServerSilent=<url> for the VaRest.Integration.* and
# Probe.VaRest.* tests (M3 plan decision 14, WebSocket W0 plan decision 5, W1a Task 11); it stops the server on exit, and the
# server also exits by itself when this script dies (--parent-pid). VAREST_NO_TESTSERVER=1 skips it: integration tests
# then fail, by design. The server's output goes to <Host>/Saved/Logs/VaRestTestServer.<runner pid>.log.
#
# Results are read from the run's own log file, never from stdout: an interleaved
# UnrealTraceServer fork clobbers the tail of stdout on Linux (VaCuus dev-loop note).
# Exit code: 0 when every matched test passed and the run completed, 1 on any failure or when
# the run did not complete (crash: no "TEST COMPLETE. EXIT CODE" log line; timeout) or ended with
# a non-zero exit code, 2 on setup errors, 3 when the filter matched no tests.
set -euo pipefail

ENGINE="${VAREST_ENGINE:-/w/Unreal/UnrealEngine}"
HOST="${VAREST_HOST:-/w/Unreal/VaRestHost}"
PROJECT="$HOST/VaRestHost.uproject"
LOG="$HOST/Saved/Logs/VaRestTests.log"

BUILD=1
FILTER="VaRest"
while [[ $# -gt 0 ]]; do
	case "$1" in
		--no-build) BUILD=0; shift ;;
		--) shift; FILTER="${1:-$FILTER}"; shift || true ;;
		*) FILTER="$1"; shift ;;
	esac
done

[[ -f "$PROJECT" ]] || { echo "run_tests: host project not found: $PROJECT" >&2; exit 2; }

# A bare RunTests argument is a SUBSTRING match unless it starts with '^' (bMatchFromStart,
# AutomationCommandline.cpp:211-227), so "VaRest.Json" would also run "Bench.VaRest.Json.*".
# StartsWith: is not usable for a full test name: it appends a trailing '.' to the filter
# (AutomationCommandline.cpp:153-170), so "StartsWith:VaRest.Json.Document.Index" matches nothing.
if [[ "$FILTER" != StartsWith:* && "$FILTER" != Group:* && "$FILTER" != ^* ]]; then
	FILTER="^$FILTER"
fi

# An editor holds the module .so files open; building under it fails or, worse, half-links.
# Match the exact process names, never `pgrep -f <pattern>`: a pattern also matches this
# script's own command line. The kernel truncates comm to 15 characters, so
# UnrealEditor-Cmd appears as "UnrealEditor-Cm".
RUNNING=$(pgrep -x 'UnrealEditor|UnrealEditor-Cm' || true)
if [[ -n "$RUNNING" ]]; then
	echo "run_tests: an editor is running (PIDs: $(echo $RUNNING)); stop it by PID first" >&2
	exit 2
fi

if [[ $BUILD -eq 1 ]]; then
	"$ENGINE/Engine/Build/BatchFiles/Linux/Build.sh" VaRestHostEditor Linux Development \
		-project="$PROJECT" || { echo "run_tests: build failed" >&2; exit 2; }
fi

SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/testserver"
# $$ in the names: two runners at once each get their own (review M-13)
SERVER_URL_FILE="$HOST/Saved/Logs/VaRestTestServer.$$.url"
SERVER_LOG="$HOST/Saved/Logs/VaRestTestServer.$$.log"
SERVER_PID=""
stop_server() {
	# Our own child, by PID (never by name or pattern)
	if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
		kill "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
	rm -f "$SERVER_URL_FILE"
}
trap stop_server EXIT
SERVER_ARGS=()
if [[ "${VAREST_NO_TESTSERVER:-0}" != "1" ]]; then
	command -v python3 >/dev/null || { echo "run_tests: python3 not found; the integration tests need it (VAREST_NO_TESTSERVER=1 runs without)" >&2; exit 2; }
	mkdir -p "$(dirname "$SERVER_URL_FILE")"
	rm -f "$SERVER_URL_FILE"
	# --fixtures-dir: the benchmark fixtures of Tools/gen_bench_json.py, for Bench.VaRest.Http.RequestPath (Task 17).
	# A simple command run with &: bash forks once and execs python3, so the server is this shell's direct child and its
	# getppid() is $$ until this shell dies (review M-13)
	python3 -I "$SERVER_DIR/server.py" --host 127.0.0.1 --port 0 --tls-port 0 --silent-port 0 --ready-file "$SERVER_URL_FILE" --parent-pid $$ \
		--fixtures-dir "$HOST/Saved/VaRestBench" >"$SERVER_LOG" 2>&1 &
	SERVER_PID=$!
	for _ in $(seq 1 100); do
		[[ -s "$SERVER_URL_FILE" ]] && break
		kill -0 "$SERVER_PID" 2>/dev/null || break
		sleep 0.1
	done
	[[ -s "$SERVER_URL_FILE" ]] || { echo "run_tests: the test server did not start -- see $SERVER_LOG" >&2; exit 2; }
	SERVER_ARGS=("-VaRestTestServer=$(head -n 1 "$SERVER_URL_FILE")")
	# WebSocket W0: the TLS listener's base URL is the ready file's https:// line (VaRest::Http::Testing::GetTestServerTlsUrl)
	TLS_URL="$(grep -m 1 '^https://' "$SERVER_URL_FILE" || true)"
	if [[ -n "$TLS_URL" ]]; then
		SERVER_ARGS+=("-VaRestTestServerTls=$TLS_URL")
	else
		echo "run_tests: the test server's ready file has no TLS URL; -VaRestTestServerTls is not passed and TLS tests will fail" >&2
	fi
	# W1a Task 11: the silent listener (accepts, never answers) is its tcp:// line (VaRest::WsTest::FSite::SilentPort)
	SILENT_URL="$(grep -m 1 '^tcp://' "$SERVER_URL_FILE" || true)"
	if [[ -n "$SILENT_URL" ]]; then
		SERVER_ARGS+=("-VaRestTestServerSilent=$SILENT_URL")
	else
		echo "run_tests: the test server's ready file has no silent listener; -VaRestTestServerSilent is not passed and its tests will fail" >&2
	fi
fi

TIMEOUT="${VAREST_TEST_TIMEOUT:-1800}"
PIDFILE="$HOST/Saved/Logs/VaRestTests.pid"
rm -f "$LOG" "$PIDFILE"
# `Automation RunTests X; Quit` -- the semicolon form. Automation splits its own argument
# on ';' and its own Quit exits with the result code once the run completes; a comma
# `, Quit` is a separate console command that never fires and the editor idles forever.
# The inner shell writes its PID and execs the editor, so the PID file names the editor itself:
# if `timeout` leaves it behind, exactly that process is killed (never by name or pattern).
PROXY_CHILD_ENV=(env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY -u no_proxy -u NO_PROXY)
if [[ -n "${VAREST_LWS_PROXY_ENV:-}" || -n "${VAREST_LWS_PROXY_NO_PROXY:-}" ]]; then
	[[ "$FILTER" == '^Probe.VaRest.Lws.Proxy.Attempt' && "${VAREST_NO_TESTSERVER:-0}" == 1 ]] || {
		echo 'run_tests: proxy environment is restricted to the isolated B1 attempt' >&2; exit 2;
	}
	[[ "${VAREST_LWS_PROXY_ENV:-}" =~ ^127\.0\.0\.1:([0-9]{1,5})$ ]] || {
		echo 'run_tests: proxy environment must name a loopback listener' >&2; exit 2;
	}
	PROXY_PORT="${BASH_REMATCH[1]}"
	[[ $((10#$PROXY_PORT)) -gt 0 && $((10#$PROXY_PORT)) -lt 65536 ]] || exit 2
	[[ -z "${VAREST_LWS_PROXY_NO_PROXY:-}" || "$VAREST_LWS_PROXY_NO_PROXY" == '*' ]] || exit 2
	PROXY_CHILD_ENV+=("http_proxy=http://${VAREST_LWS_PROXY_ENV}")
	if [[ -n "${VAREST_LWS_PROXY_NO_PROXY:-}" ]]; then
		PROXY_CHILD_ENV+=("no_proxy=$VAREST_LWS_PROXY_NO_PROXY")
	fi
fi
START=$SECONDS
set +e
# --foreground: without it timeout runs the command in a background process group, so a terminal Ctrl-C no longer
# reaches the editor and the next run refuses to start ("an editor is running"). The command is the editor itself
# (exec), so --foreground's "children are not timed out" caveat does not apply.
# Ordinary runs scrub proxy variables; only the exact B1 child may receive a controlled lowercase proxy environment.
timeout --foreground --kill-after=30 "$TIMEOUT" bash -c 'echo $$ > "$0"; exec "$@"' "$PIDFILE" \
	"${PROXY_CHILD_ENV[@]}" "$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
	-ExecCmds="Automation RunTests $FILTER; Quit" \
	-unattended -ForceDPCVars=HomeScreen.EnableHomeScreen=0 -nullrhi -nosplash -nopause -abslog="$LOG" ${SERVER_ARGS[@]+"${SERVER_ARGS[@]}"} ${VAREST_EXTRA_ARGS:-} >/dev/null 2>&1
STATUS=$?
set -e
# timeout exits 124 after TERM, 137 when it had to KILL; an editor killed by someone else also
# gives 137, so the elapsed time decides.
if [[ ( $STATUS -eq 124 || $STATUS -eq 137 ) && $(( SECONDS - START )) -ge $TIMEOUT ]]; then
	EDITOR_PID=$(cat "$PIDFILE" 2>/dev/null || true)
	if [[ -n "$EDITOR_PID" ]] && kill -0 "$EDITOR_PID" 2>/dev/null \
		&& [[ "$(cat "/proc/$EDITOR_PID/comm" 2>/dev/null)" == UnrealEditor* ]]; then
		kill -9 "$EDITOR_PID" 2>/dev/null || true
	fi
	rm -f "$PIDFILE"
	echo "run_tests: timed out after $TIMEOUT s (hung test?) -- see $LOG" >&2
	exit 1
fi
rm -f "$PIDFILE"

[[ -f "$LOG" ]] || { echo "run_tests: no log written: $LOG" >&2; exit 2; }

COMPLETE=$(grep -oE 'TEST COMPLETE\. EXIT CODE: -?[0-9]+' "$LOG" | tail -1 || true)
if [[ -z "$COMPLETE" ]]; then
	echo "run_tests: run did not complete (crash?) -- see $LOG" >&2
	tail -n 20 "$LOG" >&2
	exit 1
fi
EXITCODE="${COMPLETE##* }"

PASSED=$(grep -c 'Test Completed. Result={Success}' "$LOG" || true)
FAILED=$(grep -c 'Test Completed. Result={Fail}' "$LOG" || true)

if [[ $PASSED -eq 0 && $FAILED -eq 0 ]]; then
	echo "run_tests: filter '$FILTER' matched no tests (see $LOG)" >&2
	exit 3
fi

if [[ $FAILED -gt 0 ]]; then
	echo "---- failures ----"
	grep -E 'Test Completed. Result=\{Fail\}|LogAutomationController: Error' "$LOG" || true
	[[ -n "$SERVER_PID" ]] && echo "run_tests: test server log: $SERVER_LOG"
fi
echo "run_tests: $PASSED passed, $FAILED failed (filter '$FILTER', log $LOG)"
if [[ "$EXITCODE" != "0" ]]; then
	echo "run_tests: run finished with exit code $EXITCODE" >&2
	exit 1
fi
[[ $FAILED -eq 0 ]]
