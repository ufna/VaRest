# HTTP request path: v1 vs v2 (2026-10-21)

## Machine and build

| | |
|---|---|
| CPU | AMD Ryzen 9 7950X3D 16-Core Processor (16 cores / 32 threads) |
| RAM | 124.94 GB physical (MemTotal 131014092 kB) |
| OS | Linux 7.2.9-arch1-1 |
| Engine | Unreal Engine 5.8.1, source build (/w/Unreal/UnrealEngine) |
| Configuration | Development editor (VaRestHostEditor Linux Development), `UnrealEditor-Cmd`, `-nullrhi` (no `-llm`) |
| Fixtures | `python3 Tools/gen_bench_json.py /w/Unreal/VaRestHost/Saved/VaRestBench`: bench_1mb.json 944204 bytes, bench_10mb.json 9448081 bytes (seed 20261019) |
| Command | `Tools/run_tests.sh Bench.VaRest.Http` (test `Bench.VaRest.Http.RequestPath`) |
| Runs | per invocation, 9 runs per path for 1 MiB and 5 for 10 MiB, after one untimed warm-up run of v1 and of v2; the test takes the median of each. Three invocations (the first built the code, the other two ran `--no-build` on the same binaries). |

## Method

The server is the local `Tools/testserver/server.py` that `Tools/run_tests.sh` starts, serving the fixture from memory
(`/fixture/<name>`, read from `--fixtures-dir` once) over loopback. The game thread is timed from outside, in the
benchmark's own pump: each step ticks the core ticker (the HTTP manager, so v1's game-thread completion and its parse)
and runs the game-thread tasks (v2's hop and callbacks); the sleeps between steps are left out. The send step (v1:
`NewObject`, `SetVerb`, `SetContentType`, `ProcessURL`; v2: `Client.Get(Path)`, the test's callbacks, `Send()`) is timed
as one more game-thread step, and latency runs from its start to the callback. v1 runs as shipped: the engine's default
`CompleteOnGameThread` policy, and the parse with `FJsonSerializer` over `GetContentAsString()` on the game thread
(`VaRestRequestJSON.cpp:541-556`; `bUseChunkedParser` is off by default, `VaRestSettings.cpp:9`); on desktop it also
logs the whole body at `Log` (`VaRestRequestJSON.cpp:510-513`), so it is measured once as shipped and once with
`Log LogVaRest Off`. v2 is an `FVaRestClient` at its default log level (Basic), completed on the HTTP thread, the body
parsed with `ParseOwned` on a `UE::Tasks` worker (`VaRestDecode.cpp:90`) and the result moved to the game thread. Each
timed run checks the result's `data.count` against the fixture's, after the clock stops. The three paths interleave per
run, so drift in the machine's state spreads over all three. The pump's own idle steps count in every row: at 10 MiB v2
waits about 40 ms of transfer and decode, one otherwise empty step per 0.2 ms sleep.

## Results

Median of the three invocations; the three raw values (each the median of that invocation's runs) in brackets. ms of
wall time.

| fixture | path | runs | latency ms | game-thread ms | longest step ms |
|---|---|---|---|---|---|
| bench_1mb.json | v1 UVaRestRequestJSON, as shipped (logs the body on desktop) | 9 | 28.17 [28.92, 27.98, 28.17] | 27.25 [28.15, 27.25, 27.24] | 27.19 [28.08, 27.18, 27.19] |
| bench_1mb.json | v1 UVaRestRequestJSON, LogVaRest off | 9 | 23.79 [24.43, 23.38, 23.79] | 22.77 [23.49, 22.62, 22.77] | 22.67 [23.41, 22.52, 22.67] |
| bench_1mb.json | v2 FVaRestClient -> FVaRestJson (Basic log) | 9 | 5.13 [4.96, 5.72, 5.13] | 0.24 [0.24, 0.22, 0.28] | 0.04 [0.03, 0.04, 0.04] |
| bench_10mb.json | v1 UVaRestRequestJSON, as shipped (logs the body on desktop) | 5 | 281.50 [343.69, 278.06, 281.50] | 278.70 [341.41, 275.83, 278.70] | 278.45 [341.23, 275.70, 278.45] |
| bench_10mb.json | v1 UVaRestRequestJSON, LogVaRest off | 5 | 226.06 [255.16, 226.06, 222.08] | 223.77 [250.59, 223.77, 219.53] | 223.56 [250.35, 223.56, 219.28] |
| bench_10mb.json | v2 FVaRestClient -> FVaRestJson (Basic log) | 5 | 40.79 [42.11, 40.65, 40.79] | 1.34 [1.35, 1.21, 1.34] | 0.04 [0.05, 0.04, 0.04] |

All three invocations passed both checks: every run parsed the fixture whole, and at 10 MiB v2's game-thread
milliseconds are below v1's with its logging off.

v1's 10 MiB rows vary between invocations by more than v2's whole cost: the first invocation (right after the build)
measured 341.41 / 250.59 ms against 275.83-278.70 / 219.53-223.77 for the other two. A fourth invocation, after the
task's last code changes (a comment and an `#include`, no behaviour change; not in the table), gave at 10 MiB 373.40 ms
(as shipped), 278.03 ms (logging off) and 1.30 ms (v2) of game thread, longest steps 373.08 / 277.76 / 0.04 ms, latency
377.84 / 283.19 / 44.18 ms; at 1 MiB 26.93 / 22.53 / 0.18 ms of game thread. It also passed both checks.

## Reading

At 10 MiB v1 holds the game thread for 278.70 ms as shipped and 223.77 ms with `LogVaRest` off, nearly all of it in
one step (278.45 and 223.56 ms: a frame hitch of that length), where v2 spends 1.34 ms on the game thread in total and
0.04 ms in its longest step; at 1 MiB it is 27.25 / 22.77 ms against 0.24 ms. v1's body logging is 16-20% of its
cost as shipped (278.70 - 223.77 = 54.93 ms of 278.70 at 10 MiB, 4.48 ms of 27.25 at 1 MiB), so turning it off does not
change the picture: the parse on the game thread is the cost. Most of v2's game-thread total is the pump's own idle
steps while it waits; the hop and the callbacks fit in the 0.04 ms longest step. Latency includes the loopback
transfer and is not the claim (v2 is faster end to end too, 40.79 vs 226.06 ms at 10 MiB, but that is the parser's
speed, not the threading).

One comparison with the JSON passport (`2026-10-19-json-bench.md`): its "engine (FString, v1 path)" parse of
bench_10mb.json is about 1000-1090 ms, four times v1's ~220 ms here. That benchmark ran with `-llm`, which slows
every allocation and the engine's DOM allocates per value. One extra invocation of this benchmark with
`VAREST_EXTRA_ARGS=-llm` (not in the table) gave v1 850.33 ms of game thread at 10 MiB with logging off (916.18 ms as
shipped) and v2 1.65 ms, so most of the difference is the instrument, not the path.
