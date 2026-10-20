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
Result: LLM reported 8.0 MiB in all invocations (check passed). The memory column is valid.
Second calibration (many small blocks, what the DOM does): 100000 allocations of 64 B under their own tag, expect 6,400,000 B, LLM reported 6,400,000 B (exact; passes the 10% check). A first attempt reported 7,217,920 B because the pointer array's growth was inside the tagged scope; it is now reserved outside.
(LLM tag names must be alphanumeric: a dotted tag name aborts in `ValidateUniqueName`, so tags are `VaRestBenchCalibration` etc.)

"Retained memory" is what LLM attributes to the scoped parse while the result is alive. For VaRest, `Parse(view)` copies the
input into the document (about 9.0 MiB for the 10 MiB fixture), so that copy is included. For the engine path it includes the
temporary FString only if still alive, which it is not (freed before the measurement ends).

## Results

All VaRest rows below were measured with arena-hint factor 1 (the committed `ArenaHintFor`). Two measurement sessions exist and
they differ by up to ~17% in absolute time (machine state drifts between sessions; ratios stay stable). The decision table is
Session B, re-run on HEAD with the committed code. Session A is kept for the no-validation experiment, which was only run then.

### Session B (decision table, HEAD, factor 1): median of 3 invocations, raw values in brackets

| fixture | path | parse ms | retained memory | write ms |
|---|---|---|---|---|
| bench_1mb.json | engine (FString, v1 path) | 118.09 [117.40, 118.09, 119.46] | 7.9 MiB | 48.45 [48.45, 48.78, 48.08] |
| bench_1mb.json | engine (UTF-8 reader) | 123.14 [123.47, 122.33, 123.14] | | |
| bench_1mb.json | VaRest Parse (copy + in situ) | 3.78 [3.79, 3.78, 3.76] | 4.7 MiB | 2.14 [2.14, 2.15, 2.12] |
| bench_1mb.json | VaRest ParseOwned (minus the test's copy) | 3.75 [3.75, 3.81, 3.74] | | |
| bench_10mb.json | engine (FString, v1 path) | 1090.20 [1090.20, 1059.01, 1112.34] | 78.4 MiB | 497.78 [507.13, 472.38, 497.78] |
| bench_10mb.json | engine (UTF-8 reader) | 1158.55 [1158.55, 1112.27, 1164.87] | | |
| bench_10mb.json | VaRest Parse (copy + in situ) | 45.44 [45.85, 44.85, 45.44] | 47.1 MiB | 34.67 [23.42, 34.79, 34.67] |
| bench_10mb.json | VaRest ParseOwned (minus the test's copy) | 41.15 [42.55, 41.15, 37.26] | | |

### Session A (earlier, also factor 1): median of 3 invocations, raw values in brackets

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

Measured in Session A, factor edited and rebuilt between sets, two invocations each (not the committed factor for rows 2 and 4).

| factor | parse ms 10 MiB (per invocation, mean) | retained 10 MiB | parse ms 1 MiB (per invocation) | retained 1 MiB |
|---|---|---|---|---|
| 1 | 44.96, 43.38 (44.17) | 47.1 MiB | 3.69, 3.58 | 4.7 MiB |
| 2 (previous) | 48.03, 47.81 (47.92) | 47.1 MiB | 3.76, 3.72 | 4.8 MiB |
| 4 | 54.90, 46.78 (50.84) | 47.1 MiB | 3.56, 3.50 | 4.8 MiB |

Conclusion: **inconclusive within noise.** Retained memory is identical at 0.1 MiB resolution for all three factors (see the arena
accounting below for why), and the timing gaps between factors (8-15% on 10 MiB, with factor 4 fastest on 1 MiB) are the same
order as the ~17% cross-session variation and rest on only two invocations per factor. Factor 1 was chosen because it is the
smallest first chunk, not because it is faster.

## Arena accounting (factor 1, 10 MiB fixture, Session B)

| fixture | input B | arena used B | arena reserved B | used / input | reserved / input |
|---|---|---|---|---|---|
| bench_1mb.json | 944204 | 4015872 | 4024780 | 4.253 | 4.263 |
| bench_10mb.json | 9448081 | 39886464 | 39944481 | 4.222 | 4.228 |

The DOM is about 4.2x the input size. Reserved exceeds used by only 0.15% (58 KB of 39.9 MB), i.e. chunks are filled almost
completely. The controller's hypothesis holds: the first chunk (input size, ~9 MiB at factor 1; 2x or 4x at the other factors)
is not larger than the ~40 MB DOM, so it is filled and the arena simply continues with 64 KiB chunks; the factor only moves where
the chunk boundary falls, and retained is DOM bytes rounded up to the last chunk. Cross-check: 39,944,481 B arena + 9,448,081 B
input copy = 49,392,562 B = 47.1 MiB, exactly the LLM figure for VaRest. (A hint above the DOM size would reserve unused memory;
factor 4 on 10 MiB is 36 MiB, still under the 40 MB DOM, which is why no difference showed.)

## Retained versus peak memory (spec 7.3 says "peak")

The benchmark measures **retained** memory (LLM bytes still allocated under the tag while the result is alive). Peak was not
measured separately. For the engine path, peak is strictly larger than the retained figure: during parse it also holds the
transient UTF-16 FString of the input (about 2x the input bytes, ~18 MiB for 10 MiB), which is freed before the retained figure is
taken. VaRest's retained figure includes its 9 MiB input copy, and its peak is about equal to its retained figure. So "not worse
on memory" holds a fortiori on a peak basis. The spec's other comparands (`FJsonObjectConverter`, typed decode, the v1 request
path) are deferred to M2 and M3, where those pieces exist.

## Decision

Gate (spec 7.3): on bench_10mb.json, VaRest parse is at least 2x faster than "engine (FString, v1 path)" OR retains at least 2x less
memory, and is not worse on the other.

- Parse (Session B, HEAD): 45.44 ms vs 1090.20 ms = **24.0x faster** (ParseOwned 41.15 ms = 26.5x). Threshold 2x met.
  (Session A gave 38.94 vs 1026.18 = 26.4x; the verdict does not depend on the session.)
- Memory (retained, not peak): 47.1 MiB vs 78.4 MiB = 1.66x less (not 2x, but not worse; peak favours VaRest more). The memory arm alone would not pass; the speed arm does.

**Decision:** PASS — VaRest arena DOM stays.

## Addendum (2026-10-19): arena first-chunk sizing after the final review (I3)

Change: small documents no longer pay a 4 KiB first chunk, and a parse no longer reserves a first chunk the
size of the input (in situ, parsed strings stay in the input buffer, so a string-heavy document's DOM can be
tiny). The default first chunk is now 256 B (`FArena::MinChunkSize`; `FVaRestJson::NewDocument`, `FDocument`,
`ImportTextItem`, `FromJsonValue`, `Serialize` load), a parse sizes it as `Clamp(InputBytes / 32, 256 B, 1 MiB)`,
and growth chunks double from the previous chunk size up to `FArena::MaxChunkSize`, raised from 64 KiB to
1 MiB (blocks above `MaxChunkSize / 2` still get a chunk of their own). The `/ 32` is a deviation from the
review's `Clamp(InputBytes, 256 B, 1 MiB)`: with factor 1, a 1 MiB single-string document still reserves a
1 MiB chunk for one node, which the review's own test (< 64 KiB) forbids; at 1/32 the worst case wastes ~3% of
the input the document already owns.

Same machine, fixtures and command as above (`VAREST_EXTRA_ARGS=-llm Tools/run_tests.sh Bench.VaRest`), three
invocations, median shown, raw values in brackets. LLM calibration: 8.0 MiB and 6,400,000 B, both checks passed.

| fixture | path | parse ms | retained memory | write ms |
|---|---|---|---|---|
| bench_1mb.json | engine (FString, v1 path) | 110.21 [111.31, 110.21, 109.30] | 7.9 MiB | 45.22 [44.12, 45.59, 45.22] |
| bench_1mb.json | engine (UTF-8 reader) | 114.46 [115.29, 111.31, 114.46] | | |
| bench_1mb.json | VaRest Parse (copy + in situ) | 3.69 [3.69, 3.64, 3.72] | 5.7 MiB | 2.11 [2.06, 2.11, 2.18] |
| bench_1mb.json | VaRest ParseOwned (minus the test's copy) | 3.65 [3.66, 3.65, 3.64] | | |
| bench_10mb.json | engine (FString, v1 path) | 1008.50 [1008.50, 1005.74, 1016.50] | 78.4 MiB | 471.38 [471.38, 452.04, 473.28] |
| bench_10mb.json | engine (UTF-8 reader) | 1062.33 [1053.93, 1062.33, 1063.24] | | |
| bench_10mb.json | VaRest Parse (copy + in situ) | 40.17 [40.43, 40.17, 40.14] | 47.9 MiB | 31.09 [31.09, 24.27, 32.29] |
| bench_10mb.json | VaRest ParseOwned (minus the test's copy) | 36.81 [35.85, 36.81, 37.28] | | |

Arena accounting (identical in all three invocations):

| fixture | input B | arena used B | arena reserved B | used / input | reserved / input |
|---|---|---|---|---|---|
| bench_1mb.json | 944204 | 4015872 | 5004678 | 4.253 | 5.300 |
| bench_10mb.json | 9448081 | 39886464 | 40731964 | 4.222 | 4.311 |

Small documents (new row): 1000 parses of `{"id":1,"name":"x"}` kept alive, retained bytes per document (LLM,
arrays reserved outside the tagged scope), and the time for all 1000 parses (median of 15 per invocation):

| path | parse ms for 1000 | retained per document |
|---|---|---|
| engine `FJsonObject` (FString reader) | 2.930 [2.962, 2.879, 2.930] | 364 B |
| VaRest `Parse` | 0.679 [0.679, 0.671, 0.691] | 372 B |

A VaRest small document is one 256 B arena chunk (264 B with its header; 144 B used), the 20 B input buffer and
the shared `FDocument`. Before this change the same document reserved a 4 KiB first chunk (4104 B with its
header), so roughly 4.2 KB per document (an estimate: this row is new and was not run on the old code; the
`VaRest.Json.Arena.DocumentSizing` test failed there on every small-document check).

What moved: parse time is unchanged within the cross-session noise noted above (10 MiB Parse 40.17 ms vs
45.44 / 38.94 in Sessions B / A). Retained memory rose by up to one partly filled 1 MiB chunk per document:
1 MiB fixture 4.7 -> 5.7 MiB (reserved / input 4.26 -> 5.30; the last 1 MiB chunk is mostly empty), 10 MiB
fixture 47.1 -> 47.9 MiB (+1.7%). That is the price of the 1 MiB growth cap; in exchange the 10 MiB DOM takes about 40
chunks instead of about 465.

**Gate re-check (10 MiB):** parse 40.17 ms vs 1008.50 ms = **25.1x faster** (ParseOwned 27.4x); retained 47.9 vs
78.4 MiB = 1.64x less (not worse). **Still PASS.**

Sanity run on the final M1 fix commit (0ab7421: the RapidJSON reader and writer stacks now allocate through
FMemory, otherwise the code measured above), one invocation: 10 MiB engine 1015.59 ms, VaRest Parse 40.16 ms
(25.3x), ParseOwned 38.35 ms, retained 47.9 vs 78.4 MiB; small documents 372 B vs 364 B. Same verdict.

## Addendum (2026-10-20): arena growth cap 256 KiB, first chunk Clamp(input, 256 B, 32 KiB) (VaRest-awm)

Why: with a 1 MiB cap and an input/32 first chunk, 64-300 KB responses reserved ~1.9x the arena they used (the new
`VaRest.Json.Arena.MidSizeSlack` test measured 840871 B of slack on a 300 KB document before the change). Same
machine and build settings as above; median of three invocations, raw values in brackets. The generator now also
writes `bench_64kb.json` and `bench_256kb.json` (59092 and 235991 bytes; the nominal sizes are upper targets, the 1 MiB and 10 MiB
fixtures are likewise 944204 and 9448081 bytes), measured with
51 runs each (15 for 1 MiB, 7 for 10 MiB). Both LLM calibration checks passed in all three invocations.

| fixture | path | parse ms (median) | retained memory | write ms |
|---|---|---|---|---|
| bench_64kb.json | engine (FString, v1 path) | 4.70 [4.70, 4.82, 4.68] | 0.5 MiB | 2.72 [2.74, 2.72, 2.67] |
| bench_64kb.json | engine (UTF-8 reader) | 4.96 [5.08, 4.96, 4.85] |  |  |
| bench_64kb.json | VaRest Parse (copy + in situ) | 0.23 [0.23, 0.23, 0.23] | 0.5 MiB | 0.13 [0.13, 0.13, 0.14] |
| bench_64kb.json | VaRest ParseOwned (minus the test's copy) | 0.23 [0.23, 0.22, 0.23] |  |  |
| bench_256kb.json | engine (FString, v1 path) | 24.49 [24.68, 23.80, 24.49] | 2.0 MiB | 10.75 [11.29, 10.75, 10.61] |
| bench_256kb.json | engine (UTF-8 reader) | 24.86 [24.86, 23.59, 25.90] |  |  |
| bench_256kb.json | VaRest Parse (copy + in situ) | 0.95 [0.95, 0.97, 0.94] | 1.2 MiB | 0.53 [0.53, 0.53, 0.53] |
| bench_256kb.json | VaRest ParseOwned (minus the test's copy) | 0.93 [0.93, 0.97, 0.92] |  |  |
| bench_1mb.json | engine (FString, v1 path) | 112.70 [117.51, 112.70, 112.20] | 7.9 MiB | 47.47 [47.58, 47.40, 47.47] |
| bench_1mb.json | engine (UTF-8 reader) | 118.23 [118.23, 118.79, 117.80] |  |  |
| bench_1mb.json | VaRest Parse (copy + in situ) | 3.76 [3.80, 3.74, 3.76] | 4.9 MiB | 2.19 [2.27, 2.12, 2.19] |
| bench_1mb.json | VaRest ParseOwned (minus the test's copy) | 3.79 [3.79, 3.75, 3.86] |  |  |
| bench_10mb.json | engine (FString, v1 path) | 1049.57 [1049.57, 1050.62, 1044.88] | 78.4 MiB | 486.06 [490.40, 486.06, 479.14] |
| bench_10mb.json | engine (UTF-8 reader) | 1103.57 [1104.37, 1103.57, 1089.21] |  |  |
| bench_10mb.json | VaRest Parse (copy + in situ) | 40.50 [42.59, 39.85, 40.50] | 47.2 MiB | 31.54 [33.62, 31.54, 23.18] |
| bench_10mb.json | VaRest ParseOwned (minus the test's copy) | 38.09 [38.67, 36.50, 38.09] |  |  |

Arena accounting (identical in all three invocations):

| fixture | input B | arena used B | arena reserved B | used / input | reserved / input |
|---|---|---|---|---|---|
| bench_64kb.json | 59092 | 253400 | 491552 | 4.288 | 8.318 |
| bench_256kb.json | 235991 | 1008928 | 1015856 | 4.275 | 4.305 |
| bench_1mb.json | 944204 | 4015872 | 4161680 | 4.253 | 4.408 |
| bench_10mb.json | 9448081 | 39886464 | 40094960 | 4.222 | 4.244 |

Small documents: 1000 parses of `{"id":1,"name":"x"}` kept alive, retained bytes per document, and the time for all
1000 parses (median of 15 per invocation):

| path | parse ms for 1000 | retained per document |
|---|---|---|
| engine `FJsonObject` (FString reader) | 2.974 [2.860, 3.005, 2.974] | 364 B |
| VaRest `Parse` | 0.689 [0.689, 0.682, 0.695] | 372 B |

The small-document arena is unchanged (used 144 B, reserved 264 B; 20 B input buffer): the first chunk is still the
256 B minimum.

What moved: reserved / input for the 1 MiB fixture went 5.300 -> 4.408 and for the 10 MiB fixture 4.311 -> 4.244
(the last partly filled chunk is now at most 256 KiB instead of 1 MiB); retained memory 5.7 -> 4.9 MiB and 47.9 ->
47.2 MiB. The 64 KiB and 256 KiB fixtures were not run on the old code; after the change the slack is 238152 B at
64 KiB (32 + 64 + 128 + 256 KiB chunks for a 253400 B DOM, under the 256 KiB bound) and 6928 B at 256 KiB. Parse time at 10 MiB is 40.50 ms vs 40.17 ms
in the previous addendum, within the cross-session noise noted above; the 10 MiB DOM now takes about 160 chunks
instead of about 40.

**Gate re-check (10 MiB):** parse 40.50 ms vs 1049.57 ms = **25.9x faster** (ParseOwned 27.6x); retained 47.2 vs
78.4 MiB = 1.66x less (not worse). **Still PASS.**

## Typed decode and encode (M2, 2026-10-20)

`Bench.VaRest.Json.TypedConvert`: the 1 MiB and 10 MiB fixtures above (bench_1mb.json 944204 bytes, bench_10mb.json
9448081 bytes, as generated by `Tools/gen_bench_json.py`, seed 20261019) decoded into `FVaRestJsonBenchResponse`
(`Source/VaRestJson/Private/Tests/VaRestJsonConvertTestTypes.h`) and encoded back. VaRest goes UTF-8 bytes -> arena
DOM -> struct, and struct -> UTF-8 bytes with the RapidJSON writer (no DOM). The engine rows are `FJsonObjectConverter`
over an FString (the v1 path) and over the UTF-8 reader; its encode row for the UTF-8 reader adds the FString -> UTF-8
conversion a request body needs. Same machine, build and command as above (Development editor, `-nullrhi`; run without
`-llm`, since no memory is measured here). Each invocation takes the median of 15 timed runs (1 MiB) or 7 (10 MiB); the
table shows the median of 3 invocations, raw values in brackets.

The bench struct names its fields so both converters fill every one: bools are `Ok`/`Active` (the engine keeps a `b`
prefix, so `bOk` would not match `ok`) and the engine matches keys case-insensitively on decode, so `Id`/`Name` fill from `id`/`name` in both converters. The test asserts a checksum over every field except `Uid` is equal for both decodes. Encode output was not compared byte for byte.

| fixture | path | decode ms | encode ms |
|---|---|---|---|
| bench_1mb.json | engine FJsonObjectConverter (FString, v1 path) | 31.99 [31.99, 32.77, 31.26] | 30.33 [31.59, 30.07, 30.33] |
| bench_1mb.json | engine (UTF-8 reader; encode + UTF-8 body) | 31.23 [31.23, 30.92, 32.23] | 33.58 [34.01, 32.44, 33.58] |
| bench_1mb.json | VaRest FVaRestJsonConvert (UTF-8 in, UTF-8 out) | 8.13 [8.13, 8.33, 8.10] | 3.37 [3.37, 3.31, 3.40] |
| bench_10mb.json | engine FJsonObjectConverter (FString, v1 path) | 322.59 [322.59, 323.08, 320.94] | 302.36 [304.67, 302.36, 297.94] |
| bench_10mb.json | engine (UTF-8 reader; encode + UTF-8 body) | 323.34 [325.22, 323.34, 320.89] | 321.58 [319.34, 321.58, 323.73] |
| bench_10mb.json | VaRest FVaRestJsonConvert (UTF-8 in, UTF-8 out) | 83.97 [83.97, 82.69, 86.25] | 37.98 [38.91, 36.95, 37.98] |

What it shows: on 10 MiB, VaRest decodes 3.8x faster than both engine rows (83.97 ms vs 322.59 and 323.34 ms) and
encodes 8.0x faster than the engine's FString path (37.98 vs 302.36 ms) and 8.5x faster than the engine path that also
produces UTF-8 (321.58 ms); the 1 MiB ratios are 3.8-3.9x and 9.0-10.0x. The typed decode gain is smaller than the
parse-only gain (25.9x); the split between parse and struct walk was not measured here. Correctness side finding: the engine reads every number through a double, so the fixtures' 64-bit ids above
2^53 come back rounded for 1764 of 3527 items (1 MiB) and 17537 of 35074 items (10 MiB); VaRest decodes all of them
exactly. This VaRest decode row is the DOM + walk baseline that M7's SAX -> struct decoder must beat by 1.5x to become
the default for typed responses (spec §2.4).
