#!/usr/bin/env bash
# Real official Socket.IO peer + ordinary Unreal Editor; no Engine source edits.
set -euo pipefail
EDITOR= PROJECT= OUTPUT= VERSION=all PORT=19700 NO_BUILD=0 NO_INSTALL=0 MODULES_ROOT=
while (($#)); do
  if [[ "$1" == --no-build ]]; then NO_BUILD=1;shift;continue;fi
  if [[ "$1" == --no-install ]]; then NO_INSTALL=1;shift;continue;fi
  [[ $# -ge 2 ]] || { echo 'missing option value' >&2; exit 2; }
  case "$1" in
    --editor) EDITOR=$2;; --project) PROJECT=$2;; --output) OUTPUT=$2;;
    --server) VERSION=$2;; --port) PORT=$2;;
    --modules-root) MODULES_ROOT=$2;;
    *) echo 'unknown interop option' >&2; exit 2;;
  esac
  shift 2
done
[[ -n "$EDITOR" && -n "$PROJECT" && "$OUTPUT" == /* ]] || {
  echo 'required: --editor ABSOLUTE_EDITOR_CMD --project PROJECT.uproject --output NEW_ABSOLUTE_DIR [--server 4.8|4.5|4.4|2|all] [--no-build] [--port 19700]' >&2; exit 2;
}
[[ "$EDITOR" == /* && -x "$EDITOR" && "$PROJECT" == /* && -f "$PROJECT" ]] || { echo 'explicit existing stock Editor and project required' >&2; exit 2; }
[[ "$PORT" =~ ^[1-9][0-9]{3,4}$ && "$PORT" -ge 1024 && "$PORT" -le 65532 ]] || { echo 'invalid peer port range' >&2; exit 2; }
case "$VERSION" in 4.8|4.5|4.4|2|all) ;; *) echo 'fixed official server version required' >&2; exit 2;; esac
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PEER="$SCRIPT_DIR/testserver/sio-interop"
mkdir -m 700 -- "$OUTPUT" # Refuse every occupied output namespace.
if [[ -z "$MODULES_ROOT" ]]; then MODULES_ROOT=$PEER;fi
[[ "$MODULES_ROOT" == /* ]] || { echo 'absolute module prefix required' >&2;exit 2; }
if (( ! NO_INSTALL )); then
  [[ "$MODULES_ROOT" == "$PEER" ]] || { echo 'external closed module prefix requires --no-install' >&2;exit 2; }
  : >"$OUTPUT/npm-user-empty.conf"
  : >"$OUTPUT/npm-global-empty.conf"
  mkdir -m 700 -- "$OUTPUT/npm-cache" "$OUTPUT/npm-logs"
  env -u NODE_PATH -u NODE_OPTIONS -u NPM_TOKEN -u NODE_AUTH_TOKEN \
    -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
    npm ci --prefix "$PEER" --ignore-scripts --omit=optional --no-audit --no-fund --workspaces=false \
    "--userconfig=$OUTPUT/npm-user-empty.conf" "--globalconfig=$OUTPUT/npm-global-empty.conf" \
    "--cache=$OUTPUT/npm-cache" "--logs-dir=$OUTPUT/npm-logs" >"$OUTPUT/npm.stdout" 2>"$OUTPUT/npm.stderr"
fi
if (( ! NO_BUILD )); then
  ENGINE_DIR=$(cd -- "$(dirname -- "$EDITOR")/../.." && pwd -P)
  PROJECT_NAME=$(basename -- "$PROJECT" .uproject)
  "$ENGINE_DIR/Build/BatchFiles/Linux/Build.sh" "${PROJECT_NAME}Editor" Linux Development \
    "-Project=$PROJECT" -NoHotReloadFromIDE -WaitMutex >"$OUTPUT/build.stdout" 2>"$OUTPUT/build.stderr"
fi
if [[ "$VERSION" == all ]]; then
  for version in 4.8 4.5 4.4 2;do
    "$SCRIPT_DIR/run_interop.sh" --editor "$EDITOR" --project "$PROJECT" --output "$OUTPUT/$version" \
      --server "$version" --port "$PORT" --no-build --no-install --modules-root "$MODULES_ROOT"
  done
  python3 -I -B - "$OUTPUT" <<'PY'
import json,pathlib,sys
out=pathlib.Path(sys.argv[1]);rows={v:json.loads((out/v/'result.json').read_bytes()) for v in ['4.8','4.5','4.4','2']}
assert all(row['complete'] for row in rows.values())
with (out/'result.json').open('x') as f:json.dump({'complete':True,'versions':rows},f,indent=2);f.write('\n')
PY
  exit 0
fi
mkdir -m 700 -- "$OUTPUT/user"
NODE=(env -u NODE_PATH -u NODE_OPTIONS -u NODE_EXTRA_CA_CERTS -u DEBUG node --no-global-search-paths)
PEER_PID=
cleanup() {
  if [[ -n "$PEER_PID" ]]; then
    kill -TERM "$PEER_PID" 2>/dev/null || true
    wait "$PEER_PID" || true
    PEER_PID=
  fi
}
trap cleanup EXIT
"${NODE[@]}" "$PEER/server.mjs" --server "$VERSION" --port "$PORT" --receipts "$OUTPUT/peer.jsonl" --modules-root "$MODULES_ROOT" >"$OUTPUT/peer.stdout" 2>"$OUTPUT/peer.stderr" &
PEER_PID=$!
for ((i=0;i<200;i++)); do
  if [[ -s "$OUTPUT/peer.stdout" ]]; then break; fi
  sleep .05
done
grep -q '^VAREST_SIO_REFERENCE_READY ' "$OUTPUT/peer.stdout" || { echo 'official peer failed to become ready' >&2; exit 1; }
set +e
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY -u no_proxy -u NO_PROXY \
  timeout --foreground --kill-after=15 240 "$EDITOR" "$PROJECT" \
  '-ExecCmds=Automation RunTests ^Interop.VaRest.SocketIO.Reference; Quit' \
  -unattended -FORCELOGFLUSH -nullrhi -nosplash -nopause -nosound -nowrite -ddc=NoZenLocalFallback -ForceDPCVars=HomeScreen.EnableHomeScreen=0 \
  "-UserDir=$OUTPUT/user/" "-abslog=$OUTPUT/editor.log" "-ReportExportPath=$OUTPUT/automation" \
  "-VaRestSioInterop=ws://127.0.0.1:$PORT" "-SioReferenceVersion=$VERSION" >"$OUTPUT/editor.stdout" 2>"$OUTPUT/editor.stderr"
EDITOR_RC=$?
kill -TERM "$PEER_PID" 2>/dev/null
wait "$PEER_PID"
PEER_RC=$?
PEER_PID=
set -e
python3 -I -B - "$OUTPUT" "$VERSION" "$EDITOR_RC" "$PEER_RC" <<'PY'
import hashlib,json,pathlib,re,sys
out=pathlib.Path(sys.argv[1]);version=sys.argv[2]
expected={'4.8':['Recovery','ServerAcks','Auth','Payload','Namespaces','Binary','Heartbeat'],
          '4.5':['Recovery'],'4.4':['Payload'],'2':['UnsupportedV2']}[version]
log=(out/'editor.log').read_bytes();text=log.decode('utf-8',errors='replace')
paths=re.findall(r'Test Completed\. Result=\{(\w+)\}.*?Path=\{(Interop\.VaRest\.SocketIO\.Reference\.[^}]+)\}',text)
actual=[p.rsplit('.',1)[-1] for state,p in paths if state=='Success']
failed=[p for state,p in paths if state!='Success']
peer=(out/'peer.jsonl').read_bytes();receipts=[json.loads(line) for line in peer.splitlines()]
good=(sys.argv[3:]==['0','0'] and sorted(actual)==sorted(expected) and not failed
      and re.findall(r'TEST COMPLETE\. EXIT CODE: (-?\d+)',text)==['0']
      and receipts[-1].get('event')=='stopped' and receipts[-1].get('errors')==0
      and not any(r.get('event')=='error' for r in receipts))
result={'complete':good,'version':version,'expected':expected,'actual':actual,'failed':failed,
        'editor_returncode':int(sys.argv[3]),'peer_returncode':int(sys.argv[4]),
        'peer_sha256':hashlib.sha256(peer).hexdigest(),'log_sha256':hashlib.sha256(log).hexdigest(),
        'scope':'plain-loopback real official peer/native automation only; no WSS/Shipping/Blueprint/release acceptance'}
with (out/'result.json').open('x') as f:json.dump(result,f,indent=2,allow_nan=False);f.write('\n')
print(json.dumps(result));sys.exit(0 if good else 1)
PY
