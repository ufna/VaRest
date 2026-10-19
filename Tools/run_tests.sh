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

TIMEOUT="${VAREST_TEST_TIMEOUT:-1800}"
PIDFILE="$HOST/Saved/Logs/VaRestTests.pid"
rm -f "$LOG" "$PIDFILE"
# `Automation RunTests X; Quit` -- the semicolon form. Automation splits its own argument
# on ';' and its own Quit exits with the result code once the run completes; a comma
# `, Quit` is a separate console command that never fires and the editor idles forever.
# The inner shell writes its PID and execs the editor, so the PID file names the editor itself:
# if `timeout` leaves it behind, exactly that process is killed (never by name or pattern).
START=$SECONDS
set +e
timeout --kill-after=30 "$TIMEOUT" bash -c 'echo $$ > "$0"; exec "$@"' "$PIDFILE" \
	"$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
	-ExecCmds="Automation RunTests $FILTER; Quit" \
	-unattended -nullrhi -nosplash -nopause -abslog="$LOG" ${VAREST_EXTRA_ARGS:-} >/dev/null 2>&1
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
fi
echo "run_tests: $PASSED passed, $FAILED failed (filter '$FILTER', log $LOG)"
if [[ "$EXITCODE" != "0" ]]; then
	echo "run_tests: run finished with exit code $EXITCODE" >&2
	exit 1
fi
[[ $FAILED -eq 0 ]]
