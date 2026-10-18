# JSON benchmark and spec 7.3 gate (2026-10-19)

## Machine and build

| | |
|---|---|
| CPU | AMD Ryzen 9 7950X3D 16-Core Processor (16 cores / 32 threads) |
| RAM | 124.94 GB physical (MemTotal 131014092 kB) |
| OS | Linux 7.2.9-arch1-1 |
| Engine | Unreal Engine 5.8.1, source build (/w/Unreal/UnrealEngine) |
| Configuration | Development editor (VaRestHostEditor Linux Development), `UnrealEditor-Cmd`, `-nullrhi` |
| Fixtures | `python3 Tools/gen_bench_json.py /w/Unreal/VaRestHost/Saved/VaRestBench`: bench_1mb.json 944204 bytes, bench_10mb.json 9448081 bytes (seed 20261019) |
| Command | `VAREST_EXTRA_ARGS=-llm Tools/run_tests.sh Bench.VaRest` (test `Bench.VaRest.Json.ParseAndWrite`) |
| Runs | per invocation, the test takes the median of 15 timed runs (1 MiB) or 7 (10 MiB). The table below shows the median of 3 separate invocations. |

## Memory instrument calibration

LLM was enabled (`-llm`). The test allocates a known 8 MiB array under a scratch tag and asserts LLM sees it within 5%.
Result: LLM reported 8.0 MiB in all 15 invocations (check passed). The memory column is valid.
(LLM tag names must be alphanumeric: a dotted tag name aborts in `ValidateUniqueName`, so tags are `VaRestBenchCalibration` etc.)

"Retained memory" is what LLM attributes to the scoped parse while the result is alive. For VaRest, `Parse(view)` copies the
input into the document (about 9.0 MiB for the 10 MiB fixture), so that copy is included. For the engine path it includes the
temporary FString only if still alive, which it is not (freed before the measurement ends).

## Results (median of 3 invocations; raw values in brackets)

| fixture | path | parse ms | retained memory | write ms |
|---|---|---|---|---|
| bench_1mb.json | engine (FString, v1 path) | 110.86 [110.93, 110.52, 110.86] | 7.9 MiB | 45.14 [45.14, 46.39, 44.64] |
| bench_1mb.json | engine (UTF-8 reader) | 116.48 [116.48, 117.03, 114.90] | | |
| bench_1mb.json | VaRest Parse (copy + in situ) | 3.69 [3.58, 3.72, 3.69] | 4.7 MiB | 2.19 [2.19, 2.14, 2.20] |
| bench_1mb.json | VaRest ParseOwned (minus the test's copy) | 3.64 [3.52, 3.68, 3.64] | | |
| bench_1mb.json | VaRest without UTF-8 validation (not shipped), Parse | 3.09 [3.09, 3.19, 3.01] | 4.7 MiB | 2.09 |
| bench_1mb.json | VaRest without UTF-8 validation (not shipped), ParseOwned | 3.04 [3.03, 3.04, 3.04] | | |
| bench_10mb.json | engine (FString, v1 path) | 1026.18 [1026.18, 1023.78, 1032.56] | 78.4 MiB | 465.53 [474.57, 465.46, 465.53] |
| bench_10mb.json | engine (UTF-8 reader) | 1075.43 [1086.35, 1075.43, 1074.62] | | |
| bench_10mb.json | VaRest Parse (copy + in situ) | 38.94 [38.94, 44.04, 38.47] | 47.1 MiB | 30.95 [33.73, 24.11, 30.95] |
| bench_10mb.json | VaRest ParseOwned (minus the test's copy) | 37.86 [36.39, 41.27, 37.86] | | |
| bench_10mb.json | VaRest without UTF-8 validation (not shipped), Parse | 38.57 [38.57, 36.86, 38.75] | 47.1 MiB | 23.77 [29.86, 23.16, 23.77] |
| bench_10mb.json | VaRest without UTF-8 validation (not shipped), ParseOwned | 34.82 [35.06, 34.17, 34.82] | | |

ParseOwned input is built with `Reserve(Num + 1)` so the NUL terminator costs no reallocation (as the HTTP layer will do), and the
cost of that same copy, measured the same way, is subtracted.

The "without UTF-8 validation" rows are a one-off measurement: `rapidjson::kParseValidateEncodingFlag` removed from `BaseFlags`
in `VaRestJsonReader.cpp` (this disables RapidJSON's SIMD string scanner), rebuilt, three invocations, then the flag was restored
and rebuilt. The change was not committed. Validation costs about 0.6 ms on 1 MiB (Parse 3.69 vs 3.09, ~16%) and on 10 MiB 0.4 ms for Parse (38.94 vs 38.57, ~1%, within noise) and 3.0 ms for ParseOwned (37.86 vs 34.82, ~8%). It is a small cost against a
parse that is already ~26x faster than the engine, so an opt-out is low priority.

Retained memory is identical across the no-validation build, as expected.

## Arena-hint tuning (`ArenaHintFor` = InputBytes * factor, clamped 4 KiB..256 MiB)

Two invocations each, bench_10mb.json VaRest Parse (the 1 MiB figures are in the last column).

| factor | parse ms 10 MiB (per invocation, mean) | retained 10 MiB | parse ms 1 MiB (per invocation) | retained 1 MiB |
|---|---|---|---|---|
| 1 | 44.96, 43.38 (44.17) | 47.1 MiB | 3.69, 3.58 | 4.7 MiB |
| 2 (previous) | 48.03, 47.81 (47.92) | 47.1 MiB | 3.76, 3.72 | 4.8 MiB |
| 4 | 54.90, 46.78 (50.84) | 47.1 MiB | 3.56, 3.50 | 4.8 MiB |

Rule: lowest retained memory among settings whose parse time is within 5% of the best. Only the first chunk is sized from the hint
(later chunks are a fixed 64 KiB), so retained memory is flat at the 0.1 MiB resolution of the column, apart from 0.1 MiB on the
1 MiB fixture where factor 1 is lower. Factor 1 is lowest in memory and fastest (factor 2 is 8.5% slower, factor 4 15% slower
by mean, though the timing noise between invocations is several percent). Chosen: **factor 1**; `ArenaHintFor` changed from `* 2` to `* 1`.

## Decision

Gate (spec 7.3): on bench_10mb.json, VaRest parse is at least 2x faster than "engine (FString, v1 path)" OR retains at least 2x less
memory, and is not worse on the other.

- Parse: 38.94 ms vs 1026.18 ms = **26.4x faster** (ParseOwned 37.86 ms = 27.1x). Threshold 2x met.
- Memory: 47.1 MiB vs 78.4 MiB = 1.66x less (not 2x, but not worse). The memory arm alone would not pass; the speed arm does.

**Decision:** PASS — VaRest arena DOM stays.
