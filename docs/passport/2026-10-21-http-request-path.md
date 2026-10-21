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
| Runs | per invocation, 9 runs per path for 1 MiB and 5 for 10 MiB, after one untimed warm-up run of v1 and of v2; the test takes the median of each. Three invocations (the first built the code, the other two ran `--no-build` on the same binaries). Re-run 2026-10-21 after the M3 final fix wave B (P17.3: a GC after each v1 run): the tables below are that re-run. |
| Load | the re-run shared the machine: load average 10.0-13.7 (of 32 hardware threads) in `uptime` samples taken before and after each invocation |

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
waits about 40-55 ms of transfer and decode, one otherwise empty step per 0.2 ms sleep. After each v1 run, outside the
clock, the benchmark runs `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS)` and then one untimed pump step, so v1's request
objects, `FJsonObject` trees and response bytes (about 1 GiB over the test otherwise) do not pile up for the later runs,
and the work the collection leaves for the next tick does not land in the next run's clock (task 17 review M2).

## Results

Median of the three invocations; the three raw values (each the median of that invocation's runs) in brackets. ms of
wall time.

| fixture | path | runs | latency ms | game-thread ms | longest step ms |
|---|---|---|---|---|---|
| bench_1mb.json | v1 UVaRestRequestJSON, as shipped (logs the body on desktop) | 9 | 38.70 [38.25, 43.39, 38.70] | 37.68 [37.48, 42.63, 37.68] | 37.57 [37.38, 42.51, 37.57] |
| bench_1mb.json | v1 UVaRestRequestJSON, LogVaRest off | 9 | 34.44 [34.44, 35.20, 30.60] | 33.43 [33.43, 34.43, 29.84] | 33.33 [33.33, 34.22, 29.68] |
| bench_1mb.json | v2 FVaRestClient -> FVaRestJson (Basic log) | 9 | 6.63 [6.63, 6.24, 6.72] | 0.51 [0.53, 0.50, 0.51] | 0.06 [0.06, 0.06, 0.05] |
| bench_10mb.json | v1 UVaRestRequestJSON, as shipped (logs the body on desktop) | 5 | 357.32 [354.37, 357.32, 358.06] | 355.29 [351.58, 355.29, 355.51] | 355.13 [351.31, 355.13, 355.31] |
| bench_10mb.json | v1 UVaRestRequestJSON, LogVaRest off | 5 | 292.74 [292.74, 296.06, 273.14] | 290.70 [290.70, 293.26, 270.21] | 290.33 [290.33, 293.06, 270.01] |
| bench_10mb.json | v2 FVaRestClient -> FVaRestJson (Basic log) | 5 | 53.65 [57.24, 53.65, 53.18] | 3.10 [2.95, 3.13, 3.10] | 0.06 [0.06, 0.08, 0.06] |

All three invocations passed both checks: every run parsed the fixture whole, and at 10 MiB v2's game-thread
milliseconds are below v1's with its logging off.

Every row is higher than in the first measurement of this passport (commit 1584deb, before the GC; for example v1 at
10 MiB with logging off 223.77 ms of game thread then, 290.70 ms now; v2 1.34 ms then, 3.10 ms now), on a machine that
was busy this time (Load, above); the GC, if anything, lowers v1's figures. Single invocations taken next to each other while
the GC step was being added, not in the table: without the GC, v1 at 10 MiB gave 269.86 ms (logging off) and 379.18 ms
(as shipped) of game thread; with the GC and no settle step, 238.89 and 294.26 ms, but v2's longest step went from
0.05-0.08 ms to 0.23-0.26 ms, which the untimed pump step after the GC brings back to the 0.05-0.08 ms above.

## Reading

At 10 MiB v1 holds the game thread for 355.29 ms as shipped and 290.70 ms with `LogVaRest` off, nearly all of it in
one step (355.13 and 290.33 ms: a frame hitch of that length), where v2 spends 3.10 ms on the game thread in total and
0.06 ms in its longest step; at 1 MiB it is 37.68 / 33.43 ms against 0.51 ms. v1's body logging is 11-18% of its cost
as shipped (355.29 - 290.70 = 64.59 ms of 355.29 at 10 MiB, 4.25 ms of 37.68 at 1 MiB), so turning it off does not
change the picture: the parse on the game thread is the cost. Most of v2's game-thread total is the pump's own idle
steps while it waits; the hop and the callbacks fit in the 0.06 ms longest step. Latency includes the loopback
transfer and is not the claim (v2 is faster end to end too, 53.65 vs 292.74 ms at 10 MiB, but that is the parser's
speed, not the threading).

One comparison with the JSON passport (`2026-10-19-json-bench.md`): its first tables ran with `-llm`, which slows every
allocation, and the engine's DOM allocates per value, so their "engine (FString, v1 path)" parse of bench_10mb.json is
about 1000-1090 ms. Its correction of 2026-10-21, without `-llm`, measures that parse at 239.72 ms, in line with v1's
290.70 ms of game thread here, which adds the completion and `GetContentAsString()`. One extra invocation of this
benchmark with `VAREST_EXTRA_ARGS=-llm`, before the GC was added (not in the table), gave v1 850.33 ms of game thread at
10 MiB with logging off (916.18 ms as shipped) and v2 1.65 ms: the difference is the instrument, not the path.
