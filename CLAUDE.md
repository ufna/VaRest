# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:1105d646 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md for details and anti-patterns.

## Agent Context Profiles

The managed Beads block is task-tracking guidance, not permission to override repository, user, or orchestrator instructions.

- **Conservative (default)**: Use `bd` for task tracking. Do not run git commits, git pushes, or Dolt remote sync unless explicitly asked. At handoff, report changed files, validation, and suggested next commands.
- **Minimal**: Keep tool instruction files as pointers to `bd prime`; use the same conservative git policy unless active instructions say otherwise.
- **Team-maintainer**: Only when the repository explicitly opts in, agents may close beads, run quality gates, commit, and push as part of session close. A current "do not commit" or "do not push" instruction still wins.

## Session Completion

This protocol applies when ending a Beads implementation workflow. It is subordinate to explicit user, repository, and orchestrator instructions.

1. **File issues for remaining work** - Create beads for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Handle git/sync by active profile**:
   ```bash
   # Conservative/minimal/default: report status and proposed commands; wait for approval.
   git status

   # Team-maintainer opt-in only, unless current instructions forbid it:
   git pull --rebase
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
<!-- END BEADS INTEGRATION -->


## Build & Test

**The plugin builds inside a separate host project**, `/w/Unreal/VaRestHost` (not in this repo).
Its `Plugins/VaRest` is a **symlink** to this checkout, so there is one working tree. That only
works because this workstation builds without UBA detours (`bAllowUBAExecutor=false` in the user
`BuildConfiguration.xml`); with detours on, UBA aborts on symlinked plugins
(`cross-process rename-while-open`). Details: `docs/dev/host-project.md`.

```bash
# Build + run the whole VaRest automation suite (reads results from the run's log, not stdout)
Tools/run_tests.sh                    # filter "VaRest"
Tools/run_tests.sh VaRest.Json        # narrower filter
Tools/run_tests.sh --no-build VaRest  # skip the build
python3 -I Tools/testserver/selftest.py   # the HTTP test server's own tests (stdlib only)
python3 -I Tools/testserver/ws_selftest.py  # the WebSocket test server's own tests (ws.py, the TLS listener; stdlib only)
Tools/run_tests.sh Probe.VaRest.Lws         # LWS 3.0 probe (WS W0): minutes long, host-dependent, outside the default ^VaRest filter
Tools/run_tests.sh --no-build Probe.VaRest.Risky.Lws.StatusInConnectionError.Refused  # may crash: one case per run; also ConnectRefused, DnsFailure, NoUpgrade, TlsVerifyFailure, KilledPending

# Build only (editor target)
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
  VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject

# Monolithic game target -- a different code path; check it before a milestone lands
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
  VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
```

`Tools/run_tests.sh` starts `Tools/testserver/server.py` (python3, standard library) on a port the OS picks and passes
`-VaRestTestServer=<url>` for the `VaRest.Integration.*` tests; it stops it on exit and the server leaves by itself if the
runner dies. `VAREST_NO_TESTSERVER=1` runs without it (integration tests then fail, by design). Server log:
`<Host>/Saved/Logs/VaRestTestServer.<runner pid>.log`.

**Dev-loop hazards** (inherited from VaCuus, each one cost real time there):

- **No editor may be running while you build** -- it holds the module `.so` files. Kill **by PID**,
  never `pkill -f <pattern>` (the pattern matches your own shell and kills it).
- **`-ExecCmds` splits on commas.** For automation use `Automation RunTests X; Quit` (Automation
  splits its own argument on `;` and its own Quit exits with the result code). `…, Quit` never
  fires and the editor idles forever.
- **Read results from the log file**, not stdout (`Tools/run_tests.sh` does).
- **`ShutdownModule` never runs** in an `Automation RunTests …; Quit` run.
- **`exit 127` with no callstack is a stale engine module**; the message is on stderr.

## Architecture Overview

VaRest 2 is being built on branch `v2`. The design is
`docs/superpowers/specs/2026-10-18-varest2-core-design.md`, and implementation plans live in
`docs/superpowers/plans/`.

| Module | Type | Role |
| --- | --- | --- |
| `VaRestJson` | Runtime | `FVaRestJson`: own arena DOM, RapidJSON (engine copy) as SAX reader + writer; `FVaRestJsonConvert` struct <-> JSON; `UVaRestJsonLibrary` BP JSON library (CustomThunk wildcards). Has a module class (`VaRestJsonModule.cpp`). Depends on Engine (public) |
| `VaRestHttp` | Runtime | `FVaRestClient` and its `[[nodiscard]]` builder over a stage pipeline (log/redaction, limiter, transport; M4 adds retry, auth, cache, offline), HTTP-thread completion with zero-copy JSON decode off the game thread, the public redactor, the Inspector trace surface, `UVaRestHttpSettings`; M5 adds the `UVaRestApi` asset, `UVaRestHub` and async BP actions |
| `VaRest` | Runtime | the **v1 API at unchanged paths**, reimplemented as deprecated shims (removed in v3) |
| `VaRestEditor` | UncookedOnly | K2 nodes, v1 and v2 (`K2Node_VaRestMakeJson`); drops the convert cache on Blueprint struct changes |
| `VaRestEditorTools` | Editor | asset definition, validation, menus |
| `VaRestTests` | UncookedOnly | test-only USTRUCTs, the `VaRest.Integration.*` suite against `Tools/testserver/`, `Bench.VaRest.Http.*`; never in a game target |

Tag `v1-on-5.8` is v1 with the minimal UE 5.8 port and no behaviour change. The v1 compat
fixture is authored from it.

## Conventions & Patterns

- **Comments explain *why*, and cite engine source as `file:line`. Open every line you cite.**
- **Restore-the-bug is the proof standard.** For a correctness claim: break it deliberately, watch
  the specific test fail, restore, and report both outcomes.
- **Prefer enforcement by shape over enforcement by comment.** Make the wrong call fail to compile
  (`[[nodiscard]]` builders, private members reachable only through the correct path).
- **Threading rule:** control plane on the game thread, data plane off it. Stage logic, callbacks
  and BP events run on the game thread; parse/decode/disk IO run on `UE::Tasks`.
- **v1 paths are frozen.** Never rename, move or change the signature of anything under
  `Source/VaRest` or `Source/VaRestEditor` that v1 shipped; v1 projects load it by path.
- **The v2 Blueprint-visible surface is frozen too** (from M2, 2026-10-21): `UVaRestJsonLibrary` function
  names and *parameter names* (they are pin names in saved graphs), `MakeJsonInternal*`, `UK2Node_VaRestMakeJson`
  and every `UENUM`/`USTRUCT` name under `/Script/VaRestJson` and `/Script/VaRestEditor`. A rename needs a
  CoreRedirect in the same commit.
- **`UVaRestHttpSettings` property names are frozen from M3**: they are persisted config keys
  (`[/Script/VaRestHttp.VaRestHttpSettings]` in a project's ini). `FVaRestError`'s fields, `EVaRestErrorKind`,
  `EVaRestPriority`, `EVaRestLogLevel` and `EVaRestQueryStyle` freeze with M5, when they become pin names. A rename
  needs a CoreRedirect (or a config redirect) in the same commit.
- **Push back with sources** when a premise is wrong.
