#!/usr/bin/env bash
# Build the host editor target and run a VaRest automation filter headless.
#
#   Tools/run_tests.sh [--no-build] [--] [Filter]      (default filter: VaRest)
#
# Results are read from the run's own log file, never from stdout: an interleaved
# UnrealTraceServer fork clobbers the tail of stdout on Linux (VaCuus dev-loop note).
# Exit code: 0 when every matched test passed, 1 on any failure, 2 on setup errors,
# 3 when the filter matched no tests.
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

rm -f "$LOG"
# `Automation RunTests X; Quit` -- the semicolon form. Automation splits its own argument
# on ';' and its own Quit exits with the result code once the run completes; a comma
# `, Quit` is a separate console command that never fires and the editor idles forever.
"$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
	-ExecCmds="Automation RunTests $FILTER; Quit" \
	-unattended -nullrhi -nosplash -nopause -abslog="$LOG" >/dev/null 2>&1 || true

[[ -f "$LOG" ]] || { echo "run_tests: no log written: $LOG" >&2; exit 2; }

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
[[ $FAILED -eq 0 ]]
