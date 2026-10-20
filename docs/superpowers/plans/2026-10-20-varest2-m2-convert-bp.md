# VaRest 2 — M2: Struct ↔ JSON and the Blueprint JSON surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the JSON layer for 2.0: the M1 follow-ups and the four §2.2 rulings, `FVaRestJsonConvert` (struct ↔ JSON with naming policies, tolerant decode and a thread-safe field-map cache), typed `TryGet<T>`, the Blueprint JSON library with wildcard `CustomThunk` nodes, the v2 `Make Json` K2 node, and a Blueprint smoke test that builds, compiles and runs a graph.

**Architecture:** Everything runtime stays in module `VaRestJson` (spec §1 lists struct ↔ JSON and the BP JSON library there); it gains a public `Engine` dependency for `UBlueprintFunctionLibrary`. Encode walks `FProperty` reflection and emits SAX events into either the RapidJSON writer (straight to UTF-8 bytes, no DOM) or a DOM builder — one templated walker, two sinks. Decode walks the arena DOM. Field maps are cached per `(FObjectKey(struct), naming policy)` behind an `FRWLock`; the editor module drops the cache when a Blueprint struct changes. Editor pieces (`Make Json` node, struct-change listener, editor-only tests) are new files in `VaRestEditor`.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), C++20, engine RapidJSON 1.1.0 through `RapidJsonIncludes.h`, UE Automation tests, Kismet compiler (K2 nodes, `CustomThunk`).

**Spec:** `docs/superpowers/specs/2026-10-18-varest2-core-design.md` — §1 (modules, threading), §2.2 (all, including the four rulings of 2026-10-20), §2.3 (typed access), §2.4 (struct ↔ JSON), §2.5 (BP surface), §5.4 (JSON nodes), §7.1, §7.3 (deferred typed benchmarks), §8 row M2 ("Convert tests green; BP smoke"), §9.

## Controller rulings applied to this plan (2026-10-20)

Revision 1 after the adversarial review (`.superpowers/sdd/m2-plan-review.md`) and the Socket.IO gap note (`.superpowers/sdd/sio-m2-gap.md`):

- R1: Json To Struct resets its output before decoding (Task 12); `TryGet` of `FDateTime`/`FGuid`/`FTimespan`/soft paths refuses null and missing values (Task 9); both with tests and restore-the-bug steps.
- R2: the struct-change listener is a raw pointer created in `FVaRestEditorModule::StartupModule` and deleted in `ShutdownModule` through `VaRestEditor::StructListener::Start/Stop` (Task 13); leaking it at exit is accepted.
- R3: Socket.IO J-1 (`FVaRestJsonBytesWriter`, `FVaRestJsonConvertOptions::BytesWriter`) and the writer entries `WriteJsonInternal`, `WriteStructInternal`, `WritePropertyInternal`, `WriteJsonMembersInternal` are part of Task 7, in `VaRestJsonWriterInternal.h`, with tests `Encode.BytesWriter` and `Encode.WriterEntries`.
- R4: a `TOptional` field keeps its previous value when the JSON value does not fit (Task 8).
- R5: one warning per repaired string stays (Task 4); spec §2.2 now says "one warning is logged per repaired string".
- R6: Review Focus gains Json To Struct defaults, `TryGet` on null, cooked Blueprint struct names, forwarding an absent field and the decode half of the field-name collision; the pure Make Json node's tooltip states its caveat.
- R7: the review's cheap, local minors are applied in their tasks; anything declined is listed under "Deferred review minors" at the end.

## Global Constraints

- Engine: UE 5.8 only. Build host: `/w/Unreal/VaRestHost` (plugin symlinked). Build + test: `Tools/run_tests.sh [Filter]` (bare filters are anchored, so `VaRest.Json` never runs `Bench.VaRest.*`).
- Runtime module `VaRestJson`: `"Type": "Runtime"`, `"LoadingPhase": "PreDefault"`, `"PlatformAllowList": ["Win64","Mac","Linux","Android","IOS"]` (unchanged in `VaRest.uplugin`). No new modules in M2.
- RapidJSON only through `#include "RapidJsonIncludes.h"` (engine macro configuration: `RAPIDJSON_ASSERT`→`check`, SSE4.2/NEON). No new third-party code.
- **v1 paths are frozen.** Nothing under `Source/VaRest/` changes. Under `Source/VaRestEditor/` only **new** files are added, plus the `VaRestJson` and `ToolMenus` entries in `VaRestEditor.Build.cs`; `VaRestEditor.cpp` gains exactly two calls in the module's StartupModule/ShutdownModule bodies; no v1 signature, path or type changes. `VaRest_BreakJson.*` and `VaRestEditor.h` are not edited.
- Public types: `FVaRestJson*`, `EVaRestJson*`, `UVaRestJson*`, `UK2Node_VaRest*`. Internals: namespace `VaRest::Json` (struct conversion in `VaRest::Json::Convert`). Private headers are named `VaRestJson*.h`, **never `VaRestJsonParser.h`** (v1 owns that name).
- **JSON keys are case-sensitive.** Never compare keys with `FString`/`TStringView` `operator==` or `TestEqual` (case-insensitive in UE); use `VaRest::Json::KeyEquals`, byte compares, `ESearchCase::CaseSensitive` and `TestEqualSensitive`. `TSet<FString>`/`TMap<FString,…>` keys are case-insensitive too.
- One depth limit: `VaRest::Json::MaxDepth` (512) for every recursive operation (spec §2.2). Exceeding it fails the whole operation with an error; nothing truncates silently.
- Strings entering a document are valid UTF-8 (spec §2.2): byte-level input is repaired to U+FFFD with one warning per repaired string, UTF-16 input maps unpaired surrogates to U+FFFD.
- Threading (spec §1): `FVaRestJsonConvert` and every read of a parsed document may run on any thread; nothing on a read path writes, and the field-map cache (the only shared mutable state) is behind its `FRWLock`. The BP library, K2 node and struct listener run on the game thread.
- Unity builds: no file-scope `using namespace` and no anonymous namespaces in `.cpp` files; file-local helpers live in `namespace VaRest::Json::<FileStem>Private` (or `<Module><Stem>Private` outside VaRestJson). Block-scope `using namespace` inside a function body is fine.
- MSVC-clean by construction (only Linux clang builds here): no local or parameter named like a member — data **or function** — of the enclosing class (C4458; for `FVaRestJson` that includes `Doc`, `Node`, `Object`, `Array`, `Null`, `Path`, `Num`, `Get`, `Set`, `Add`, `Has`, `Remove`, `Clone`, `Equals`, `Members`, `Elements`, `Parse`, `Miss`), and no local hiding a namespace-scope variable (clang `-Wshadow`): never a local named `MaxDepth` inside `VaRest::Json`.
- `UE_LOG` format arguments are string literals (no `?:` between two `TEXT()`s).
- Tests: `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, inside `#if WITH_DEV_AUTOMATION_TESTS`. Runtime tests: `Source/VaRestJson/Private/Tests/`, flags `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`, names `VaRest.Json.*` / `VaRest.Json.Convert.*` / `VaRest.Json.Library.*`. Editor tests: `Source/VaRestEditor/Private/Tests/`, inside `#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR`, flags `EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter`, names `VaRest.Json.Convert.*` / `VaRest.Json.Blueprint.*`. Benchmarks: `Bench.VaRest.*` with `EAutomationTestFlags::PerfFilter`.
- Expected log lines in tests use **`AddExpectedErrorPlain` / `AddExpectedMessagePlain`** with exact occurrence counts. The non-`Plain` forms treat the pattern as a regex (`AutomationTest.h:1782`, `:1854`), so `U+FFFD`, `(` or `[` would not match literally. Any unexpected `Error` log fails a test; `Warning` logs are expected explicitly.
- Comments explain *why* and cite engine source as `file:line`; open every line you cite.
- Restore-the-bug: a task that names a restore-the-bug step shows the named test failing with the behaviour deliberately broken, then passing restored; report both outcomes in the task report.
- No editor may run during a build; kill only by PID (never `pkill -f`).
- New file header: `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.`
- Every commit message ends with `Co-Authored-By: Claude <Model> <noreply@anthropic.com>`, naming the model that actually wrote the commit (for example `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`).

## Review Focus

- **Two struct fields that map to one JSON name under the naming policy** (`user_id` and `UserId` both become `userId`): encode must not emit a duplicate key and decode must be deterministic — the later field is skipped with a warning. Test owner: Task 6 (`VaRest.Json.Convert.FieldMap`).
- **JSON keys that differ only by case decoded into `TMap<FString, T>`** (UE `FString` keys compare case-insensitively): no duplicate pairs, no broken map, the last pair wins. Test owner: Task 8 (`VaRest.Json.Convert.Decode.Containers`).
- **A cyclic `FJsonObject` graph passed to `FromJsonValue`** (engine JSON objects can hold themselves through shared pointers): must fail with the depth error, not overflow the stack. Test owner: Task 3 (`VaRest.Json.Depth.PastLimitFails`).
- **A Blueprint `Get Field` reused in a loop whose later call misses**: the output must be reset to the type's default, never keep the previous iteration's value. Test owner: Task 12 (`VaRest.Json.Library.WildcardImpl`).
- **`Make Json` keys that differ only by case (`id` and `ID`)**: pin names are case-insensitive `FName`s, JSON keys are not — both keys must reach the JSON. Test owner: Task 15 (`VaRest.Json.Blueprint.MakeJsonSmoke`).
- **`Json To Struct` called twice (a loop, or a later event) with a field missing the second time**: the field must get its default, not the previous call's value (the output is a reused BP temporary). Test owner: Task 12 (`VaRest.Json.Library.WildcardImpl`, `... starts from defaults`).
- **`TryGet<FDateTime>` / `TryGet<FGuid>` on JSON `null`**: unset, never a set `0001-01-01` / zero GUID. Test owner: Task 9 (`VaRest.Json.Api.TryGetTyped`, `null is not a date`).
- **A Blueprint struct encoded in PIE**: it writes the cooked names (`PlayerScore`, not the friendly `Player Score`), so PIE and a packaged game send the same JSON (spec §2.4). Test owner: Task 13 (`VaRest.Json.Convert.UserStruct.Names`).
- **Forwarding an absent field** (`Body.Set("p", Resp["p"])` when the response has no `p`): nothing is written, no `null`. Test owner: Task 5 (`VaRest.Json.Semantics.MissingIsUndefined`).
- **Decoding the key two fields share** (`{"userId":5}` into `user_id` + `UserId`): only the surviving first field is filled. Test owner: Task 8 (`VaRest.Json.Convert.Decode.TolerantNames`).
- **A pure `Make Json` read by two nodes, one of them through Set Field**: each reader gets its own object, so the second does not see the Set. Not cheaply testable; the node's tooltip says so (Task 14).

---

## File structure

```
Source/VaRestJson/
  VaRestJson.Build.cs                     + Engine (public, UBlueprintFunctionLibrary), JsonUtilities (private, benchmarks)
  Public/VaRestJson.h                     move semantics, MaxDepth, TryToString/TryToUtf8, missing = undefined Set,
                                          typed TryGet<T>/As<T> (USTRUCT, TArray, TMap<FString,T>)
  Public/VaRestJsonConvert.h              EVaRestJsonNaming, FVaRestJsonConvertOptions, FVaRestJsonDecodeReport,
                                          FVaRestJsonRename, FVaRestJsonNames, FVaRestJsonConvert
  Public/VaRestJsonLibrary.h              UVaRestJsonLibrary (BP functions + CustomThunk wildcards)
  Private/VaRestJson.cpp                  handle API (hygiene, SetAt wording, UTF-8 entry, depth-aware copies)
  Private/VaRestJsonNode.h                + bDuplicateKeys flag, NodeToInt64/UInt64/Double
  Private/VaRestJsonDocument.h/.cpp       + duplicate tracking, Set collapse, depth-limited DeepCopy/Equals
  Private/VaRestJsonArena.h               MaxChunkSize 256 KiB
  Private/VaRestJsonSax.h                 NEW: EmitNode (depth-limited SAX walk); its writer types move to the public
                                          VaRestJsonWriterInternal.h in Task 7
  Private/VaRestJsonBuilder.h             NEW: FDomBuilder (SAX -> arena DOM), moved out of the reader
  Private/VaRestJsonWriter.h/.cpp         Write() returns false past MaxDepth
  Private/VaRestJsonReader.h/.cpp         depth clamp, code-point columns, uses FDomBuilder
  Private/VaRestJsonText.cpp              hint Clamp(input, 256 B, 32 KiB), TryToString/TryToUtf8, one UTF-16 converter
  Public/VaRestJsonUtf8.h                 NEW, exported: UTF-8 validation/repair, UTF-16 -> UTF-8 with U+FFFD (other VaRest modules validate the same way)
  Private/VaRestJsonUtf8.cpp
  Public/VaRestJsonWriterInternal.h       NEW, internal API for sibling modules: UTF-8 writer types, WriteStructMembersInternal,
                                          WriteJsonInternal, WriteJsonMembersInternal, WriteStructInternal, WritePropertyInternal
  Private/VaRestJsonBridge.cpp            depth-limited bridges, U+FFFD strings
  Private/VaRestJsonStructOps.cpp         Serialize refuses past MaxDepth, ImportTextItem converter
  Private/VaRestJsonConvertPrivate.h      NEW: field map, cache, naming helpers, special structs, contexts
  Private/VaRestJsonConvertNames.cpp      NEW: naming policies, BP field names, field maps + cache, rename registry
  Private/VaRestJsonConvertEncode.cpp     NEW: struct -> UTF-8 bytes / DOM
  Private/VaRestJsonConvertDecode.cpp     NEW: DOM -> struct (tolerant, strict, paths)
  Private/VaRestJsonLibrary.cpp           NEW: UVaRestJsonLibrary + thunks
  Private/Tests/VaRestJsonConvertTestTypes.h   NEW: test USTRUCTs/UENUMs for conversion and benchmarks
  Private/Tests/VaRestJson{Depth,Utf8,Semantics,ConvertNames,ConvertEncode,ConvertDecode,TypedGet,Library,LibraryWildcard}Test.cpp  NEW
  Private/Tests/VaRestJsonConvertBenchTest.cpp NEW: Bench.VaRest.Json.TypedConvert
Source/VaRestEditor/
  VaRestEditor.Build.cs                   + VaRestJson, ToolMenus (private)
  Private/VaRestEditor.cpp                two calls in StartupModule/ShutdownModule bodies (controller ruling R2)
  Private/VaRestJsonStructListener.h/.cpp NEW: drops the field-map cache on Blueprint struct changes (Start/Stop)
  Private/K2Node_VaRestMakeJson.h/.cpp    NEW: v2 Make Json node (FVaRestMakeJsonEntry, UK2Node_VaRestMakeJson)
  Private/Tests/VaRestJsonBlueprintTestUtils.h NEW: build/compile/run helpers for test Blueprints
  Private/Tests/VaRestJsonUserStructTest.cpp   NEW: BP struct names, cache invalidation
  Private/Tests/VaRestJsonMakeJsonNodeTest.cpp NEW: node pins, keys, validation
  Private/Tests/VaRestJsonBlueprintTest.cpp    NEW: BP smoke (build, compile, run)
Tools/run_tests.sh                        timeout --foreground
Tools/gen_bench_json.py                   + 64 KiB and 256 KiB fixtures
docs/passport/2026-10-19-json-bench.md    two dated addenda (arena retune, typed convert)
```

---

### Task 1: Prep — move semantics, runner Ctrl-C, unity hygiene, M1 deferred minors

Beads: VaRest-2o9 (move leaves `Node` set), VaRest-6ky (`timeout --foreground`), and the VaRest-rfd "by M2" items that are code/test changes: SetAt-on-miss wording, `TryGet<float>` at `FLT_MAX`, indexed-path case-sensitivity test, last-wins-after-Remove test, `static_assert` tying `NewDocument`'s default to `FArena::MinChunkSize`, file-scope usings / anonymous namespaces in non-test `.cpp` files. (The other rfd "by M2" items are covered by Tasks 3–5: depth policy → Task 3; error columns and the error-path converter → Task 4; `Fresh.Set(K, Fresh)` → Task 5.)

**Files:**
- Modify: `Source/VaRestJson/Public/VaRestJson.h`
- Modify: `Source/VaRestJson/Private/VaRestJsonNode.h`
- Replace: `Source/VaRestJson/Private/VaRestJson.cpp`, `Source/VaRestJson/Private/VaRestJsonText.cpp`
- Modify (hygiene only): `Source/VaRestJson/Private/VaRestJsonDocument.cpp`, `VaRestJsonWriter.cpp`, `VaRestJsonReader.cpp`, `VaRestJsonBridge.cpp`, `VaRestJsonStructOps.cpp`
- Modify: `Tools/run_tests.sh`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp`, `VaRestJsonDocumentTest.cpp`, `VaRestJsonPathTest.cpp`

**Interfaces:**
- Produces: `FVaRestJson(FVaRestJson&&)` / `operator=(FVaRestJson&&)` that leave the source a default handle (`Node == nullptr`, `Doc == nullptr`); `FVaRestJson::DefaultArenaChunk` (private, `static constexpr SIZE_T = 256`); `inline constexpr double VaRest::Json::FloatRoundsToInfinity = 340282356779733661637539395458142568448.0` (public, `VaRestJson.h`); `bool VaRest::Json::NodeToInt64(const FNode&, int64&)`, `bool NodeToUInt64(const FNode&, uint64&)`, `bool NodeToDouble(const FNode&, double&)` (`VaRestJsonNode.h`); file-local helper namespaces `VaRest::Json::HandlePrivate`, `TextPrivate`, `DocumentPrivate`, `WriterPrivate`, `ReaderPrivate`, `BridgePrivate`, `StructOpsPrivate`.

- [ ] **Step 1: Write the failing tests**

Append to `Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp`, before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiMoveTest, "VaRest.Json.Api.MoveLeavesSourceEmpty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiMoveTest::RunTest(const FString& Parameters)
{
	// The implicit move moved Doc but copied Node, so a moved-from handle claimed IsValid() over a document it no
	// longer kept alive (VaRest-2o9): reading it was a use-after-free once the new owner let go.
	FVaRestJson A = FVaRestJson::Object().Set(TEXT("k"), 1);
	FVaRestJson B = MoveTemp(A);
	TestFalse(TEXT("moved-from handle is invalid"), A.IsValid());
	TestTrue(TEXT("moved-from handle has no document"), A.GetDocumentInternal() == nullptr);
	TestEqual(TEXT("moved-to handle keeps the value"), B[TEXT("k")].AsInt(), 1);

	FVaRestJson C;
	C = MoveTemp(B);
	TestFalse(TEXT("move-assigned-from handle is invalid"), B.IsValid());
	TestTrue(TEXT("move-assigned-from handle has no document"), B.GetDocumentInternal() == nullptr);
	TestEqual(TEXT("move-assigned-to handle keeps the value"), C[TEXT("k")].AsInt(), 1);

	// The fluent && overloads end in MoveTemp(*this): the named source must end up empty, not dangling
	FVaRestJson D = FVaRestJson::Object();
	FVaRestJson E = MoveTemp(D).Set(TEXT("a"), 1);
	TestFalse(TEXT("MoveTemp(X).Set(...) leaves X invalid"), D.IsValid());
	TestEqual(TEXT("the chain's result holds the write"), E[TEXT("a")].AsInt(), 1);

	// A moved-from handle behaves like a default handle: its first Set auto-vivifies
	D.Set(TEXT("b"), 2);
	TestTrue(TEXT("moved-from handle auto-vivifies"), D.IsObject() && D[TEXT("b")].AsInt() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiFloatLimitTest, "VaRest.Json.Api.FloatAtFltMax",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiFloatLimitTest::RunTest(const FString& Parameters)
{
	// 3.4028235e38 is the shortest text of FLT_MAX; as a double it lies just above FLT_MAX but still rounds to it
	FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("max"), 3.4028235e38)
		.Set(TEXT("halfway"), VaRest::Json::FloatRoundsToInfinity)
		.Set(TEXT("below"), std::nextafter(VaRest::Json::FloatRoundsToInfinity, 0.0))
		.Set(TEXT("negmax"), -3.4028235e38);
	TestTrue(TEXT("FLT_MAX text reads as a float"), J[TEXT("max")].TryGet<float>().IsSet());
	TestEqual(TEXT("... and is FLT_MAX"), J[TEXT("max")].AsFloat(), TNumericLimits<float>::Max());
	TestEqual(TEXT("-FLT_MAX text reads as -FLT_MAX"), J[TEXT("negmax")].AsFloat(), -TNumericLimits<float>::Max());
	TestFalse(TEXT("the halfway point rounds to infinity: not a float"), J[TEXT("halfway")].TryGet<float>().IsSet());
	TestTrue(TEXT("just below halfway is still a float"), J[TEXT("below")].TryGet<float>().IsSet());
	return true;
}
```

Add `#include <cmath>` to the include block at the top of `VaRestJsonApiTest.cpp` (for `std::nextafter`).

Append to `Source/VaRestJson/Private/Tests/VaRestJsonDocumentTest.cpp`, before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocLastWinsAfterRemoveTest, "VaRest.Json.Document.LastWinsAfterRemove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocLastWinsAfterRemoveTest::RunTest(const FString& Parameters)
{
	// 11 members, "k" twice: indexed (more than IndexThreshold = 8), lookup gives the last "k"
	FVaRestJson J = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT(
		"{\"k\":1,\"a\":0,\"b\":0,\"c\":0,\"d\":0,\"e\":0,\"f\":0,\"g\":0,\"h\":0,\"k\":2,\"z\":0}"))).StealValue();
	TestTrue(TEXT("indexed"), J.GetNodeInternal()->Obj.Index != nullptr);
	TestEqual(TEXT("last occurrence wins"), J[TEXT("k")].AsInt(), 2);

	J.Remove(TEXT("a")); // 10 members: the index is cleared and refilled in place
	TestTrue(TEXT("still indexed"), J.GetNodeInternal()->Obj.Index != nullptr);
	TestEqual(TEXT("last occurrence still wins after Remove reindexes"), J[TEXT("k")].AsInt(), 2);

	J.Remove(TEXT("b"));
	J.Remove(TEXT("c")); // 8 members: the index is dropped, lookups scan from the end
	TestTrue(TEXT("index released at 8 members"), J.GetNodeInternal()->Obj.Index == nullptr);
	TestEqual(TEXT("last occurrence still wins on the linear scan"), J[TEXT("k")].AsInt(), 2);
	return true;
}
```

Append to `Source/VaRestJson/Private/Tests/VaRestJsonPathTest.cpp`, before the final `#endif` (add `#include "VaRestJsonDocument.h"` to its includes if absent):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonPathIndexedCaseTest, "VaRest.Json.Path.IndexedCaseSensitive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonPathIndexedCaseTest::RunTest(const FString& Parameters)
{
	// More than 8 members: "User" is found through the hash index, which must stay byte-exact
	FVaRestJson Wide = FVaRestJson::Object();
	for (int32 i = 0; i < 12; ++i)
	{
		Wide.Set(*FString::Printf(TEXT("k%d"), i), i);
	}
	Wide.Set(TEXT("User"), FVaRestJson::Object().Set(TEXT("Id"), 7));
	TestTrue(TEXT("indexed root"), Wide.GetNodeInternal()->Obj.Index != nullptr);
	TestEqual(TEXT("exact dotted path"), Wide.Path(TEXT("User.Id")).AsInt(), 7);
	TestEqual(TEXT("exact pointer"), Wide.Path(TEXT("/User/Id")).AsInt(), 7);
	TestFalse(TEXT("indexed dotted lookup is case-sensitive"), Wide.Path(TEXT("user.Id")).IsValid());
	TestFalse(TEXT("indexed pointer lookup is case-sensitive"), Wide.Path(TEXT("/USER/Id")).IsValid());
	TestFalse(TEXT("inner key is case-sensitive"), Wide.Path(TEXT("User.ID")).IsValid());
	return true;
}
```

- [ ] **Step 2: Run the tests to verify the new ones fail**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: build fails first on `VaRest::Json::FloatRoundsToInfinity` (undeclared). That is the RED for the constant; to see the behaviour RED, temporarily replace `VaRest::Json::FloatRoundsToInfinity` in the test with the literal `340282356779733661637539395458142568448.0`, run again and expect `VaRest.Json.Api.MoveLeavesSourceEmpty` (`moved-from handle is invalid` fails) and `VaRest.Json.Api.FloatAtFltMax` (`FLT_MAX text reads as a float` fails) to fail; `LastWinsAfterRemove` and `IndexedCaseSensitive` pass (they pin existing behaviour). Restore the constant name.

- [ ] **Step 3: Header changes in `VaRestJson.h`**

1. After the forward declarations block (`struct FVaRestJsonMember; ...`), add:

```cpp
namespace VaRest::Json
{
	/**
	 * A double at or above this magnitude rounds to an infinite float: it is halfway between FLT_MAX (2^128 - 2^104)
	 * and 2^128, and round-half-even rounds FLT_MAX's odd mantissa up. Below it the cast gives FLT_MAX at most.
	 * Exact as a double: 2^103 * (2^25 - 1).
	 */
	inline constexpr double FloatRoundsToInfinity = 340282356779733661637539395458142568448.0;
}
```

2. Replace `FVaRestJson() = default;` with:

```cpp
	FVaRestJson() = default;
	FVaRestJson(const FVaRestJson&) = default;
	FVaRestJson& operator=(const FVaRestJson&) = default;

	/**
	 * Moving leaves the source an ordinary default handle. The implicit move moved Doc but copied Node, so a
	 * moved-from handle (MoveTemp(X).Set(...) ends in MoveTemp(*this)) still claimed IsValid() over a document it
	 * no longer kept alive.
	 */
	FVaRestJson(FVaRestJson&& Other)
		: Doc(MoveTemp(Other.Doc))
		, Node(Other.Node)
	{
		Other.Node = nullptr;
	}

	FVaRestJson& operator=(FVaRestJson&& Other)
	{
		if (this != &Other)
		{
			Doc = MoveTemp(Other.Doc);
			Node = Other.Node;
			Other.Node = nullptr;
		}
		return *this;
	}
```

3. In `TryGet`, replace the `if constexpr (std::is_same_v<U, float>)` block with:

```cpp
				if constexpr (std::is_same_v<U, float>)
				{
					// Not "> FLT_MAX": 3.4028235e38, the shortest text of FLT_MAX, parses to a double just above it
					// and still rounds to FLT_MAX. Only at the halfway point does the cast give infinity.
					if (FMath::IsFinite(*V) && FMath::Abs(*V) >= VaRest::Json::FloatRoundsToInfinity)
					{
						return {};
					}
				}
```

4. Replace the private `NewDocument` declaration and its comment with:

```cpp
	/** First arena chunk of an API-built document: FArena::MinChunkSize (static_assert in VaRestJson.cpp), so a small document pays one 256 B chunk */
	static constexpr SIZE_T DefaultArenaChunk = 256;
	static FDocumentRef NewDocument(SIZE_T ArenaFirstChunk = DefaultArenaChunk);
```

- [ ] **Step 4: Number helpers in `VaRestJsonNode.h`**

Inside `namespace VaRest::Json`, after `KeyEquals`, add:

```cpp
	/** Number -> int64: any Int, and integral doubles in [-2^63, 2^63) (both bounds are exact doubles). UInt holds only values above INT64_MAX. */
	inline bool NodeToInt64(const FNode& Value, int64& Out)
	{
		if (Value.Type != EVaRestJsonType::Number)
		{
			return false;
		}
		switch (Value.NumKind)
		{
		case ENumKind::Int:
			Out = Value.Int;
			return true;
		case ENumKind::UInt:
			return false;
		case ENumKind::Double:
			if (FMath::IsFinite(Value.Double) && FMath::TruncToDouble(Value.Double) == Value.Double
				&& Value.Double >= -9223372036854775808.0 && Value.Double < 9223372036854775808.0)
			{
				Out = static_cast<int64>(Value.Double);
				return true;
			}
			return false;
		}
		return false;
	}

	/** Number -> uint64: non-negative Int, UInt, and integral doubles in [0, 2^64) */
	inline bool NodeToUInt64(const FNode& Value, uint64& Out)
	{
		if (Value.Type != EVaRestJsonType::Number)
		{
			return false;
		}
		switch (Value.NumKind)
		{
		case ENumKind::Int:
			if (Value.Int < 0)
			{
				return false;
			}
			Out = static_cast<uint64>(Value.Int);
			return true;
		case ENumKind::UInt:
			Out = Value.UInt;
			return true;
		case ENumKind::Double:
			if (FMath::IsFinite(Value.Double) && FMath::TruncToDouble(Value.Double) == Value.Double
				&& Value.Double >= 0.0 && Value.Double < 18446744073709551616.0)
			{
				Out = static_cast<uint64>(Value.Double);
				return true;
			}
			return false;
		}
		return false;
	}

	inline bool NodeToDouble(const FNode& Value, double& Out)
	{
		if (Value.Type != EVaRestJsonType::Number)
		{
			return false;
		}
		switch (Value.NumKind)
		{
		case ENumKind::Int:    Out = static_cast<double>(Value.Int); return true;
		case ENumKind::UInt:   Out = static_cast<double>(Value.UInt); return true;
		case ENumKind::Double: Out = Value.Double; return true;
		}
		return false;
	}
```

- [ ] **Step 5: Replace `Source/VaRestJson/Private/VaRestJson.cpp`** (hygiene, SetAt wording, static_assert, number helpers; behaviour otherwise unchanged)

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"

// The module builds in unity blobs, where a file-scope using-directive or an anonymous namespace leaks into every
// later .cpp of the same blob. File-local helpers live in a namespace named after the file; member functions spell
// VaRest::Json:: in their signatures and open a using-directive inside their body when they need many internals.
namespace VaRest::Json::HandlePrivate
{
	const TCHAR* TypeName(EVaRestJsonType Type)
	{
		switch (Type)
		{
		case EVaRestJsonType::None:   return TEXT("invalid handle");
		case EVaRestJsonType::Null:   return TEXT("Null");
		case EVaRestJsonType::Bool:   return TEXT("Bool");
		case EVaRestJsonType::Number: return TEXT("Number");
		case EVaRestJsonType::String: return TEXT("String");
		case EVaRestJsonType::Array:  return TEXT("Array");
		case EVaRestJsonType::Object: return TEXT("Object");
		}
		return TEXT("?");
	}
}

FString FVaRestJsonError::ToString() const
{
	FString Result = Message;
	if (Line > 0)
	{
		Result += FString::Printf(TEXT(" (line %d, column %d)"), Line, Column);
	}
	else if (Offset >= 0)
	{
		Result += FString::Printf(TEXT(" (byte %lld)"), Offset);
	}
	if (!Path.IsEmpty())
	{
		Result += FString::Printf(TEXT(" at %s"), *Path);
	}
	return Result;
}

FVaRestJson::FDocumentRef FVaRestJson::NewDocument(SIZE_T ArenaFirstChunk)
{
	static_assert(DefaultArenaChunk == VaRest::Json::FArena::MinChunkSize, "an API-built document starts with one minimum-size arena chunk");
	return MakeShared<VaRest::Json::FDocument, ESPMode::ThreadSafe>(ArenaFirstChunk);
}

FVaRestJson FVaRestJson::Object()
{
	FDocumentRef NewDoc = NewDocument();
	VaRest::Json::FNode* Root = NewDoc->NewObject();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Array()
{
	FDocumentRef NewDoc = NewDocument();
	VaRest::Json::FNode* Root = NewDoc->NewArray();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Null()
{
	FDocumentRef NewDoc = NewDocument();
	VaRest::Json::FNode* Root = NewDoc->NewNull();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

EVaRestJsonType FVaRestJson::GetType() const
{
	return Node ? Node->Type : EVaRestJsonType::None;
}

int32 FVaRestJson::Num() const
{
	switch (GetType())
	{
	case EVaRestJsonType::Array:  return static_cast<int32>(Node->Arr.Num);
	case EVaRestJsonType::Object: return static_cast<int32>(Node->Obj.Num);
	default:                      return 0;
	}
}

TOptional<bool> FVaRestJson::TryGetBool() const
{
	return IsBool() ? TOptional<bool>(Node->Bool) : TOptional<bool>();
}

TOptional<int64> FVaRestJson::TryGetInt64() const
{
	int64 Value = 0;
	return Node && VaRest::Json::NodeToInt64(*Node, Value) ? TOptional<int64>(Value) : TOptional<int64>();
}

TOptional<uint64> FVaRestJson::TryGetUInt64() const
{
	uint64 Value = 0;
	return Node && VaRest::Json::NodeToUInt64(*Node, Value) ? TOptional<uint64>(Value) : TOptional<uint64>();
}

TOptional<double> FVaRestJson::TryGetDouble() const
{
	double Value = 0.0;
	return Node && VaRest::Json::NodeToDouble(*Node, Value) ? TOptional<double>(Value) : TOptional<double>();
}

TOptional<FString> FVaRestJson::TryGetString() const
{
	if (!IsString())
	{
		return {};
	}
	return FString::ConstructFromPtrSize(Node->Str.Data, static_cast<int32>(Node->Str.Len));
}

FString FVaRestJson::AsString(const FString& Default) const
{
	TOptional<FString> S = TryGetString();
	return S ? MoveTemp(*S) : Default;
}

FUtf8StringView FVaRestJson::AsUtf8View() const
{
	return IsString() ? Node->Str.View() : FUtf8StringView();
}

FVaRestJson FVaRestJson::Get(FUtf8StringView Key) const
{
	if (!IsObject())
	{
		return Miss();
	}
	VaRest::Json::FNode* Found = VaRest::Json::FDocument::Find(*Node, Key);
	return Found ? FVaRestJson(Doc, Found) : Miss();
}

FVaRestJson FVaRestJson::Get(FStringView Key) const
{
	if (!IsObject())
	{
		return Miss();
	}
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	return Get(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

VaRest::Json::FDocument* FVaRestJson::PrepareContainer(EVaRestJsonType Kind, const TCHAR* Operation)
{
	if (!Node)
	{
		if (Doc)
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot %s on a missing value (a lookup missed); set the field on its parent first, e.g. Parent.Set(Key, FVaRestJson::Object())"), Operation);
			return nullptr;
		}
		*this = Kind == EVaRestJsonType::Object ? Object() : Array();
		return Doc.Get();
	}
	if (Node->Type == Kind)
	{
		return Doc.Get();
	}
	UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot %s on a %s"), Operation, VaRest::Json::HandlePrivate::TypeName(Node->Type));
	return nullptr;
}

void FVaRestJson::SetNode(FStringView Key, VaRest::Json::FNode* Value)
{
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	Doc->Set(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length()), Value);
}

void FVaRestJson::SetNodeUtf8(FUtf8StringView Key, VaRest::Json::FNode* Value)
{
	Doc->Set(*Node, Key, Value);
}

void FVaRestJson::AddNode(VaRest::Json::FNode* Value)
{
	Doc->Append(*Node, Value);
}

void FVaRestJson::SetAtNode(int32 Index, VaRest::Json::FNode* Value)
{
	if (Index >= 0)
	{
		Doc->SetAt(*Node, static_cast<uint32>(Index), Value);
	}
}

bool FVaRestJson::Remove(FUtf8StringView Key)
{
	return IsObject() && Doc->Remove(*Node, Key) > 0;
}

bool FVaRestJson::Remove(FStringView Key)
{
	if (!IsObject())
	{
		return false;
	}
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	return Remove(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

VaRest::Json::FDocument* FVaRestJson::PrepareSetAt(int32 Index)
{
	// One message per cause: "an invalid handle of 0 elements" (M1) named neither the missed lookup nor the type.
	if (!Node)
	{
		if (Doc)
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on a missing value (a lookup missed)"), Index);
		}
		else
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on an invalid handle"), Index);
		}
		return nullptr;
	}
	if (Node->Type != EVaRestJsonType::Array)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on a %s"), Index, VaRest::Json::HandlePrivate::TypeName(Node->Type));
		return nullptr;
	}
	if (Index < 0 || Index >= static_cast<int32>(Node->Arr.Num))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot SetAt index %d on an array of %d elements"), Index, static_cast<int32>(Node->Arr.Num));
		return nullptr;
	}
	return Doc.Get();
}

TArray<FString> FVaRestJson::GetKeys() const
{
	TArray<FString> Keys;
	if (IsObject())
	{
		Keys.Reserve(Node->Obj.Num);
		for (uint32 i = 0; i < Node->Obj.Num; ++i)
		{
			const VaRest::Json::FStr& Key = Node->Obj.Members[i].Key;
			Keys.Add(FString::ConstructFromPtrSize(Key.Data, static_cast<int32>(Key.Len)));
		}
	}
	return Keys;
}

FVaRestJsonMemberRange FVaRestJson::Members() const
{
	return FVaRestJsonMemberRange(*this);
}

FVaRestJsonMember FVaRestJson::MemberAt(int32 Index) const
{
	if (!IsObject() || Index < 0 || Index >= static_cast<int32>(Node->Obj.Num))
	{
		return FVaRestJsonMember{};
	}
	const VaRest::Json::FMember& M = Node->Obj.Members[Index];
	return FVaRestJsonMember{ M.Key.View(), FVaRestJson(Doc, M.Value) };
}

FVaRestJson FVaRestJson::At(int32 Index) const
{
	if (!IsArray() || Index < 0 || Index >= static_cast<int32>(Node->Arr.Num))
	{
		return Miss();
	}
	return FVaRestJson(Doc, Node->Arr.Items[Index]);
}

bool FVaRestJson::RemoveAt(int32 Index)
{
	return IsArray() && Index >= 0 && Doc->RemoveAt(*Node, static_cast<uint32>(Index));
}

FVaRestJsonElementRange FVaRestJson::Elements() const
{
	return FVaRestJsonElementRange(*this);
}

FVaRestJson FVaRestJson::Clone() const
{
	using VaRest::Json::FArena;
	if (!Node)
	{
		return {};
	}
	// A root clone needs about what the source used; a subtree clone starts small and grows.
	FDocumentRef NewDoc = NewDocument(Node == Doc->Root ? FMath::Max<SIZE_T>(FArena::MinChunkSize, Doc->Arena.GetUsedBytes()) : FArena::MinChunkSize);
	VaRest::Json::FNode* Root = NewDoc->DeepCopy(*Node);
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

bool FVaRestJson::Equals(const FVaRestJson& Other) const
{
	if (!Node || !Other.Node)
	{
		return !Node && !Other.Node;
	}
	return Node == Other.Node || VaRest::Json::FDocument::Equals(*Node, *Other.Node);
}

VaRest::Json::FNode* FVaRestJson::MakeNull(VaRest::Json::FDocument& InDoc) { return InDoc.NewNull(); }
VaRest::Json::FNode* FVaRestJson::MakeBool(VaRest::Json::FDocument& InDoc, bool bValue) { return InDoc.NewBool(bValue); }
VaRest::Json::FNode* FVaRestJson::MakeInt(VaRest::Json::FDocument& InDoc, int64 Value) { return InDoc.NewInt(Value); }
VaRest::Json::FNode* FVaRestJson::MakeUInt(VaRest::Json::FDocument& InDoc, uint64 Value) { return InDoc.NewUInt(Value); }
VaRest::Json::FNode* FVaRestJson::MakeDouble(VaRest::Json::FDocument& InDoc, double Value) { return InDoc.NewDouble(Value); }
VaRest::Json::FNode* FVaRestJson::MakeUtf8String(VaRest::Json::FDocument& InDoc, FUtf8StringView Value) { return InDoc.NewString(Value); }

VaRest::Json::FNode* FVaRestJson::MakeString(VaRest::Json::FDocument& InDoc, FStringView Value)
{
	const auto Utf8 = StringCast<UTF8CHAR>(Value.GetData(), Value.Len());
	return InDoc.NewString(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

VaRest::Json::FNode* FVaRestJson::MakeCopy(VaRest::Json::FDocument& InDoc, const FVaRestJson& Value)
{
	// A node has exactly one parent, so even a value from this same document is copied.
	return Value.Node ? InDoc.DeepCopy(*Value.Node) : InDoc.NewNull();
}
```

- [ ] **Step 6: Replace `Source/VaRestJson/Private/VaRestJsonText.cpp`** (hygiene only)

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonReader.h"
#include "VaRestJsonWriter.h"

namespace VaRest::Json::TextPrivate
{
	/**
	 * First arena chunk for a parse: input / 32, clamped to [FArena::MinChunkSize, FArena::MaxChunkSize].
	 * Parsed strings stay in the input buffer (in situ), so the DOM can be far smaller than the input: a
	 * 1 MiB single-string document holds one node. Sizing the first chunk like the input reserved that
	 * whole size for nothing; at 1/32 the worst case (all strings) wastes ~3% of the input the document
	 * already owns. Node-heavy input (the DOM is ~4.2x the input, docs/passport/2026-10-19-json-bench.md)
	 * reaches MaxChunkSize after a few doublings; the passport's addendum measures this sizing.
	 */
	SIZE_T ArenaHintFor(int64 InputBytes)
	{
		return static_cast<SIZE_T>(FMath::Clamp<int64>(InputBytes / 32, FArena::MinChunkSize, FArena::MaxChunkSize));
	}

	/** Rejects oversize input before anything is allocated */
	bool ExceedsLimit(int64 InputBytes, const FVaRestJsonParseOptions& Options, FVaRestJsonError& OutError)
	{
		if (Options.MaxInputBytes > 0 && InputBytes > Options.MaxInputBytes)
		{
			OutError = FVaRestJsonError();
			OutError.Offset = -1;
			OutError.Message = FString::Printf(TEXT("Input of %lld bytes exceeds MaxInputBytes (%lld)"), InputBytes, Options.MaxInputBytes);
			return true;
		}
		return false;
	}

	TValueOrError<FVaRestJson, FVaRestJsonError> ParseIntoDocument(TArray<uint8>&& Bytes, const FVaRestJsonParseOptions& Options, FUtf8StringView Original)
	{
		FVaRestJsonError Error;
		if (ExceedsLimit(Bytes.Num(), Options, Error))
		{
			return MakeError(MoveTemp(Error));
		}
		const int64 Bom = BomLength(Bytes.GetData(), Bytes.Num());
		const int64 InputBytes = Bytes.Num();
		TSharedPtr<FDocument, ESPMode::ThreadSafe> Doc = MakeShared<FDocument, ESPMode::ThreadSafe>(ArenaHintFor(InputBytes));
		Doc->Input = MoveTemp(Bytes);
		Doc->Input.Add(0); // no reallocation when the caller left a byte of slack

		if (!ParseInSitu(*Doc, Bom, Options, Original, Error))
		{
			return MakeError(MoveTemp(Error));
		}
		FNode* Root = Doc->Root;
		return MakeValue(FVaRestJson::MakeInternal(MoveTemp(Doc), Root));
	}
}

void FVaRestJson::ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format) const
{
	if (Node)
	{
		VaRest::Json::Write(*Node, Format, Out);
	}
}

FString FVaRestJson::ToString(EVaRestJsonFormat Format) const
{
	if (!Node)
	{
		return FString();
	}
	TArray<uint8> Bytes;
	VaRest::Json::Write(*Node, Format, Bytes);
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::ParseOwned(TArray<uint8>&& Utf8Bytes, const FVaRestJsonParseOptions& Options)
{
	return VaRest::Json::TextPrivate::ParseIntoDocument(MoveTemp(Utf8Bytes), Options, FUtf8StringView());
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(TConstArrayView<uint8> Utf8Bytes, const FVaRestJsonParseOptions& Options)
{
	using namespace VaRest::Json::TextPrivate;
	FVaRestJsonError Error;
	if (ExceedsLimit(Utf8Bytes.Num(), Options, Error))
	{
		return MakeError(MoveTemp(Error));
	}
	TArray<uint8> Copy;
	Copy.Reserve(Utf8Bytes.Num() + 1);
	Copy.Append(Utf8Bytes.GetData(), Utf8Bytes.Num());
	return ParseIntoDocument(MoveTemp(Copy), Options, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Utf8Bytes.GetData()), Utf8Bytes.Num()));
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(FUtf8StringView Text, const FVaRestJsonParseOptions& Options)
{
	return Parse(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len()), Options);
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(FStringView Text, const FVaRestJsonParseOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::TextPrivate;
	// One copy: convert straight into the owned buffer (with a byte of slack for the terminator). The in-situ
	// parse rewrites that buffer, so line/column are computed on the error path only, by converting again.
	const int32 Utf8Len = FPlatformString::ConvertedLength<UTF8CHAR>(Text.GetData(), Text.Len());
	FVaRestJsonError Error;
	if (ExceedsLimit(Utf8Len, Options, Error))
	{
		return MakeError(MoveTemp(Error));
	}
	TArray<uint8> Bytes;
	Bytes.Reserve(Utf8Len + 1);
	Bytes.AddUninitialized(Utf8Len);
	FPlatformString::Convert(reinterpret_cast<UTF8CHAR*>(Bytes.GetData()), Utf8Len, Text.GetData(), Text.Len());

	TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseIntoDocument(MoveTemp(Bytes), Options, FUtf8StringView());
	if (!Result.HasValue() && Result.GetError().Offset >= 0)
	{
		FVaRestJsonError Diagnosed = Result.GetError();
		const auto Original = StringCast<UTF8CHAR>(Text.GetData(), Text.Len());
		FillLineColumn(Diagnosed, FUtf8StringView(Original.Get(), Original.Length()));
		return MakeError(MoveTemp(Diagnosed));
	}
	return Result;
}
```

- [ ] **Step 7: Hygiene in the remaining `.cpp` files** (mechanical; no behaviour change)

- `VaRestJsonDocument.cpp`: rename the inner `namespace` (the anonymous one holding `GrowVector` and `NumbersEqual`) to `namespace DocumentPrivate`. Call sites become `DocumentPrivate::GrowVector(` (two, in `Append` for objects and arrays) and `DocumentPrivate::NumbersEqual(` (one, in `Equals`).
- `VaRestJsonWriter.cpp`: rename the inner anonymous `namespace` to `namespace WriterPrivate`. In `Write`, use `WriterPrivate::FUtf8Sink` and `WriterPrivate::WriteNode(`. (`PutReserve`/`PutUnsafe` stay in `WriterPrivate` next to `FUtf8Sink`; RapidJSON finds them by ADL in the sink's namespace.)
- `VaRestJsonReader.cpp`: rename the inner anonymous `namespace` to `namespace ReaderPrivate`. Add `using namespace ReaderPrivate;` as the first statement of `ParseInSitu`'s body.
- `VaRestJsonBridge.cpp`: delete the file-scope `using namespace VaRest::Json;`; change `namespace\n{` (the helpers block) to `namespace VaRest::Json::BridgePrivate\n{`. In the four member functions at the bottom (`ToJsonValue`, `ToJsonObject`, `FromJsonValue`, `FromJsonObject`) add `using namespace VaRest::Json; using namespace VaRest::Json::BridgePrivate;` as the first statement where they use `FNode`, `ToEngine` or `FromEngine`.
- `VaRestJsonStructOps.cpp`: delete the file-scope `using namespace VaRest::Json;`; change the anonymous namespace holding `SerializeVersion`/`MaxSerializedBytes` to `namespace VaRest::Json::StructOpsPrivate`. Add `using namespace VaRest::Json;` as the first statement of `ImportTextItem`, and `using namespace VaRest::Json::StructOpsPrivate;` as the first statement of `Serialize`.

Gate (no output expected):

```bash
grep -nE '^using namespace|^\s*namespace\s*(\{.*)?$' Source/VaRestJson/Private/*.cpp
```

- [ ] **Step 8: `Tools/run_tests.sh` — let Ctrl-C reach the editor (VaRest-6ky)**

Replace the `timeout --kill-after=30 "$TIMEOUT" bash -c ...` line with:

```bash
# --foreground: without it timeout runs the command in a background process group, so a terminal Ctrl-C no longer
# reaches the editor and the next run refuses to start ("an editor is running"). The command is the editor itself
# (exec), so --foreground's "children are not timed out" caveat does not apply.
timeout --foreground --kill-after=30 "$TIMEOUT" bash -c 'echo $$ > "$0"; exec "$@"' "$PIDFILE" \
```

(The following continuation lines stay as they are.) Check by hand: start `Tools/run_tests.sh --no-build VaRest.Json`, press Ctrl-C while the editor runs, then `pgrep -x 'UnrealEditor|UnrealEditor-Cm'` prints nothing. Report the result.

- [ ] **Step 9: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`; the four new tests pass alongside the 43 from M1.

- [ ] **Step 10: Commit**

```bash
git add Source/VaRestJson Tools/run_tests.sh
git commit -m "json: move leaves the source empty; float limit at FLT_MAX; unity hygiene; runner forwards Ctrl-C

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 2: Arena retune and benchmark addendum (VaRest-awm)

**Files:**
- Modify: `Source/VaRestJson/Private/VaRestJsonArena.h`, `Source/VaRestJson/Private/VaRestJsonText.cpp` (`ArenaHintFor`)
- Modify: `Tools/gen_bench_json.py`, `Source/VaRestJson/Private/Tests/VaRestJsonBenchTest.cpp` (fixture list)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonArenaTest.cpp`
- Modify: `docs/passport/2026-10-19-json-bench.md` (dated addendum)

**Interfaces:**
- Consumes: `VaRest::Json::TextPrivate::ArenaHintFor` (Task 1).
- Produces: `FArena::MaxChunkSize == 256 * 1024`; `ArenaHintFor(InputBytes) == Clamp(InputBytes, 256, 32 KiB)`.

- [ ] **Step 1: Write the failing test**

Append to `Source/VaRestJson/Private/Tests/VaRestJsonArenaTest.cpp`, before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaMidSizeTest, "VaRest.Json.Arena.MidSizeSlack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaMidSizeTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::FArena;

	// A typical mid-size API response (~300 KB, number-heavy): the arena may hold at most one partly filled
	// growth chunk beyond what the DOM uses. With 1 MiB growth chunks (M1) the slack reached ~950 KB here.
	FString Text = TEXT("[");
	for (int32 i = 0; Text.Len() < 300 * 1024; ++i)
	{
		Text += FString::Printf(TEXT("%s{\"id\":%d,\"x\":%d.5,\"tags\":[1,2,3],\"ok\":true}"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Text += TEXT("]");
	const FVaRestJson J = FVaRestJson::Parse(FStringView(Text)).StealValue();
	if (!TestTrue(TEXT("parsed"), J.IsArray()))
	{
		return false;
	}
	const SIZE_T Used = J.GetDocumentInternal()->Arena.GetUsedBytes();
	const SIZE_T Reserved = J.GetDocumentInternal()->Arena.GetReservedBytes();
	TestTrue(*FString::Printf(TEXT("slack %llu B <= one 256 KiB chunk + headers (used %llu, reserved %llu)"), (uint64)(Reserved - Used), (uint64)Used, (uint64)Reserved),
		Reserved - Used <= 256 * 1024 + 64 * 64);
	TestEqual(TEXT("growth cap"), FArena::MaxChunkSize, static_cast<SIZE_T>(256 * 1024));
	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Arena`
Expected: `VaRest.Json.Arena.MidSizeSlack` fails (`growth cap`, and normally the slack check).

- [ ] **Step 3: Retune**

In `VaRestJsonArena.h` replace the `MaxChunkSize` comment and value:

```cpp
		/**
		 * Growth chunks double from the previous chunk size up to this. 256 KiB bounds the slack of a mid-size document
		 * (64-300 KB responses reserved ~1.9x what they used with a 1 MiB cap: VaRest-awm) at one partly filled chunk,
		 * while the 10 MiB benchmark DOM (~40 MB) still takes only ~160 chunks
		 * (docs/passport/2026-10-19-json-bench.md, addendum of 2026-10-20).
		 * A block larger than MaxChunkSize / 2 gets a chunk of its own.
		 */
		static constexpr SIZE_T MaxChunkSize = 256 * 1024;
```

In `VaRestJsonText.cpp` replace `ArenaHintFor` and its comment:

```cpp
	/**
	 * First arena chunk for a parse: the input size clamped to [FArena::MinChunkSize, 32 KiB] (VaRest-awm). A small
	 * response fits its first chunk; a large one starts at 32 KiB and doubles to FArena::MaxChunkSize, so a string-heavy
	 * input (strings stay in the input buffer, in situ) never reserves a chunk the size of the input for a few nodes.
	 */
	SIZE_T ArenaHintFor(int64 InputBytes)
	{
		return static_cast<SIZE_T>(FMath::Clamp<int64>(InputBytes, FArena::MinChunkSize, 32 * 1024));
	}
```

- [ ] **Step 4: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed` (the existing `DocumentSizing` single-string check `< 64 KiB` still holds: first chunk 32 KiB).

- [ ] **Step 5: Mid-size fixtures in the benchmark**

In `Tools/gen_bench_json.py` replace the fixture tuple in `main()`:

```python
    for name, size in (("bench_64kb.json", 64 << 10), ("bench_256kb.json", 256 << 10),
                       ("bench_1mb.json", 1 << 20), ("bench_10mb.json", 10 << 20)):
```

and the module docstring's second line with `Writes bench_64kb.json, bench_256kb.json, bench_1mb.json and bench_10mb.json (same seed every run, so numbers are comparable).`

In `VaRestJsonBenchTest.cpp` replace `for (const TCHAR* Name : { TEXT("bench_1mb.json"), TEXT("bench_10mb.json") })` with:

```cpp
	for (const TCHAR* Name : { TEXT("bench_64kb.json"), TEXT("bench_256kb.json"), TEXT("bench_1mb.json"), TEXT("bench_10mb.json") })
```

and `const int32 Runs = Bytes.Num() > (4 << 20) ? 7 : 15;` with:

```cpp
		const int32 Runs = Bytes.Num() > (4 << 20) ? 7 : Bytes.Num() > (512 << 10) ? 15 : 51;
```

- [ ] **Step 6: Re-run the benchmark three times**

```bash
python3 Tools/gen_bench_json.py /w/Unreal/VaRestHost/Saved/VaRestBench
for i in 1 2 3; do VAREST_EXTRA_ARGS=-llm Tools/run_tests.sh --no-build Bench.VaRest.Json.ParseAndWrite && cp /w/Unreal/VaRestHost/Saved/VaRestBench/results.md /tmp/varest-awm-$i.md; done
```

(The first `run_tests.sh` call needs the build; drop `--no-build` on it if the editor target is not yet built.) Each invocation must pass both LLM calibration checks.

- [ ] **Step 7: Passport addendum**

Append to `docs/passport/2026-10-19-json-bench.md`:

```markdown
## Addendum (2026-10-20): arena growth cap 256 KiB, first chunk Clamp(input, 256 B, 32 KiB) (VaRest-awm)

Why: with a 1 MiB cap and an input/32 first chunk, 64-300 KB responses reserved ~1.9x the arena they used. Same
machine and build settings as above; median of three invocations, raw values in brackets.

| fixture | path | parse ms (median) | retained memory | write ms |
|---|---|---|---|---|
(one row per fixture and path, as in Session B, for 64 KiB, 256 KiB, 1 MiB and 10 MiB)

| fixture | input B | arena used B | arena reserved B | used / input | reserved / input |
|---|---|---|---|---|---|
(one row per fixture)

Small documents: (the small-document table, as in the previous addendum)

What moved: (reserved / input before vs after for each fixture; parse time change within noise or not)

**Gate re-check (10 MiB):** parse <VaRest> ms vs <engine> ms = <x>x faster; retained <VaRest> vs <engine> MiB. **PASS / FAIL**
```

Fill every table from the three result files, writing measured values (no placeholders may remain). If the gate no longer holds (≥2× faster parse **or** ≥2× lower retained memory on 10 MiB, and not worse on the other), STOP and report instead of committing.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestJson Tools/gen_bench_json.py docs/passport/2026-10-19-json-bench.md
git commit -m "json: arena growth cap 256 KiB, first chunk sized like the input up to 32 KiB; bench addendum

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 3: One depth limit for every recursive operation (spec §2.2, VaRest-a1n)

**Files:**
- Modify: `Source/VaRestJson/Public/VaRestJson.h`
- Create: `Source/VaRestJson/Private/VaRestJsonSax.h`
- Replace: `Source/VaRestJson/Private/VaRestJsonWriter.h`, `VaRestJsonWriter.cpp`, `VaRestJsonBridge.cpp`
- Modify: `VaRestJsonDocument.h/.cpp` (`DeepCopy`, `Equals`), `VaRestJson.cpp` (`Clone`, `Equals`, `MakeCopy`), `VaRestJsonText.cpp` (`ToString`, `ToUtf8`, new `TryToString`, `TryToUtf8`), `VaRestJsonStructOps.cpp` (`Serialize` save), `VaRestJsonReader.cpp` (clamp)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonDepthTest.cpp`

**Interfaces:**
- Consumes: Task 1's hygiene namespaces.
- Produces: `inline constexpr int32 VaRest::Json::MaxDepth = 512;` (`VaRestJson.h`); `FVaRestJson::From(V)` returns a default (invalid) handle when the value cannot be made; `TValueOrError<FString, FVaRestJsonError> FVaRestJson::TryToString(EVaRestJsonFormat) const`; `TValueOrError<void, FVaRestJsonError> FVaRestJson::TryToUtf8(TArray<uint8>&, EVaRestJsonFormat) const`; `bool VaRest::Json::Write(const FNode&, EVaRestJsonFormat, TArray<uint8>&)` (false past `MaxDepth`, `Out` restored); `VaRestJsonSax.h`: `VaRest::Json::FUtf8Sink`, `FWriterAllocator`, `FCondensedWriter`, `FPrettyWriter`, `FEmitState { int32 NonFinite; bool bTooDeep; }`, `template <typename SinkType> bool EmitNode(SinkType&, const FNode&, int32 Depth, FEmitState&)`; `FNode* FDocument::DeepCopy(const FNode&)` returns `nullptr` past `MaxDepth`; `static bool FDocument::Equals(const FNode&, const FNode&, bool* bOutTooDeep = nullptr)`. Depth = number of containers around a value: the 512th nested container is allowed, the 513th fails — the parser's rule (`Frames.Num() >= limit` before opening).

- [ ] **Step 1: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonDepthTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VaRestJson.h"

namespace VaRestJsonDepthTest
{
	/** Levels nested arrays built through Add, which does not check depth on the way in (spec 2.2) */
	FVaRestJson NestedArrays(int32 Levels)
	{
		FVaRestJson Root = FVaRestJson::Array();
		FVaRestJson Current = Root;
		for (int32 i = 1; i < Levels; ++i)
		{
			Current.Add(FVaRestJson::Array());
			Current = Current[Current.Num() - 1];
		}
		return Root;
	}

	FString NestedText(int32 Levels)
	{
		return FString::ChrN(Levels, TEXT('[')) + FString::ChrN(Levels, TEXT(']'));
	}

	TSharedPtr<FJsonValue> NestedEngine(int32 Levels)
	{
		TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
		for (int32 i = 1; i < Levels; ++i)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Add(Value);
			Value = MakeShared<FJsonValueArray>(Items);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDepthAtLimitTest, "VaRest.Json.Depth.AtLimitWorks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDepthAtLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonDepthTest;
	const FString Text = NestedText(512);
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Text));
	if (!TestTrue(TEXT("512 levels parse"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson J = Parsed.StealValue();
	TestEqualSensitive(TEXT("512 levels write back"), J.ToString(), Text);
	TestTrue(TEXT("TryToString succeeds"), J.TryToString().HasValue());
	const FVaRestJson Copy = J.Clone();
	TestTrue(TEXT("Clone at the limit"), Copy.IsArray() && Copy.Equals(J));
	const TSharedPtr<FJsonValue> Engine = J.ToJsonValue();
	TestTrue(TEXT("ToJsonValue at the limit"), Engine.IsValid());
	TestTrue(TEXT("FromJsonValue at the limit"), FVaRestJson::FromJsonValue(Engine).Equals(J));

	TArray<uint8> Saved;
	FMemoryWriter Writer(Saved);
	FVaRestJson ToSave = J;
	ToSave.Serialize(Writer);
	FMemoryReader Reader(Saved);
	FVaRestJson Loaded;
	Loaded.Serialize(Reader);
	TestTrue(TEXT("Serialize round trip at the limit"), Loaded.Equals(J));

	FVaRestJson Holder = FVaRestJson::Object();
	Holder.Set(TEXT("deep"), NestedArrays(512));
	TestTrue(TEXT("Set copies a 512-level value"), Holder.Has(TEXT("deep")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDepthPastLimitTest, "VaRest.Json.Depth.PastLimitFails",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDepthPastLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonDepthTest;
	// One error line each from ToString, ToUtf8, Clone, Equals, ToJsonValue, FromJsonValue, FromJsonObject (cycle),
	// Serialize and Set; TryToString/TryToUtf8 and Parse report through their return value instead.
	AddExpectedErrorPlain(TEXT("deeper than 512"), EAutomationExpectedErrorFlags::Contains, 9);

	const FVaRestJson Deep = NestedArrays(513);
	TestEqualSensitive(TEXT("ToString past the limit is empty"), Deep.ToString(), FString());
	TArray<uint8> Out;
	Out.Add('x');
	Deep.ToUtf8(Out);
	TestEqual(TEXT("ToUtf8 appends nothing"), Out.Num(), 1);
	const TValueOrError<FString, FVaRestJsonError> Text = Deep.TryToString();
	TestTrue(TEXT("TryToString reports the depth"), Text.HasError() && Text.GetError().Message.Contains(TEXT("deeper than 512")));
	const TValueOrError<void, FVaRestJsonError> Bytes = Deep.TryToUtf8(Out);
	TestTrue(TEXT("TryToUtf8 reports and leaves Out as it was"), Bytes.HasError() && Out.Num() == 1);

	TestFalse(TEXT("Clone past the limit is invalid"), Deep.Clone().IsValid());
	TestFalse(TEXT("Equals past the limit is false"), Deep.Equals(NestedArrays(513)));
	TestFalse(TEXT("ToJsonValue past the limit is null"), Deep.ToJsonValue().IsValid());
	TestFalse(TEXT("FromJsonValue past the limit is invalid"), FVaRestJson::FromJsonValue(NestedEngine(513)).IsValid());

	// Review Focus: a cyclic engine graph is infinitely deep; it must hit the limit, not overflow the stack
	TSharedPtr<FJsonObject> Cycle = MakeShared<FJsonObject>();
	Cycle->SetObjectField(TEXT("self"), Cycle);
	TestFalse(TEXT("a cyclic FJsonObject is refused"), FVaRestJson::FromJsonObject(Cycle).IsValid());
	Cycle->RemoveField(TEXT("self")); // break the shared-pointer cycle so the object is freed

	TArray<uint8> Saved;
	FMemoryWriter Writer(Saved);
	FVaRestJson ToSave = Deep;
	ToSave.Serialize(Writer);
	FMemoryReader Reader(Saved);
	FVaRestJson Loaded;
	Loaded.Serialize(Reader);
	TestFalse(TEXT("a value too deep to save loads as an invalid handle"), Loaded.IsValid());
	TestFalse(TEXT("the archive itself is intact"), Reader.IsError());

	FVaRestJson Holder = FVaRestJson::Object();
	Holder.Set(TEXT("deep"), Deep);
	TestFalse(TEXT("Set of a value too deep to copy writes nothing"), Holder.Has(TEXT("deep")));

	// Parse clamps MaxDepth to 512, so anything that parses also writes
	FVaRestJsonParseOptions Generous;
	Generous.MaxDepth = 100000;
	const TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(NestedText(513)), Generous);
	TestTrue(TEXT("a larger MaxDepth does not lift the limit"), Parsed.HasError() && Parsed.GetError().Message.Contains(TEXT("deeper than 512")));
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify the tests fail**

Run: `Tools/run_tests.sh VaRest.Json.Depth`
Expected: compile errors for `TryToString`/`TryToUtf8` (not declared). That is the RED; the behaviour checks are seen failing in Step 9's restore-the-bug run.

- [ ] **Step 3: `VaRestJson.h`**

1. In the `namespace VaRest::Json` block added in Task 1 (with `FloatRoundsToInfinity`), add:

```cpp
	/**
	 * The one nesting limit (spec 2.2). Parse, write, Clone, Equals, the FJsonValue bridges, Serialize and struct convert
	 * all recurse once per level (the parser is iterative but shares the limit), so this bounds stack use on worker
	 * threads too. A tree built deeper through Set/Add is not checked on the way in (that would cost every Set); every
	 * recursive operation on it fails as a whole with an error instead of truncating.
	 */
	inline constexpr int32 MaxDepth = 512;
```

2. In `FVaRestJsonParseOptions`, replace the `MaxDepth` member and its comment with:

```cpp
	/** Nesting deeper than this fails the parse. Clamped to [1, VaRest::Json::MaxDepth]: anything that parses also writes. */
	int32 MaxDepth = VaRest::Json::MaxDepth;
```

3. Replace the whole Set/Add/SetAt template block (from `template <typename ValueType> FVaRestJson& Set(FStringView Key, ValueType&& Value) &` through the `SetAt` `&&` overload, keeping the `Remove`/`GetKeys`/`Members`/`MemberAt`/`At`/`operator[](int32)` declarations that sit between them) so every write skips a value that could not be made (a copy past `MaxDepth`):

```cpp
	template <typename ValueType>
	FVaRestJson& Set(FStringView Key, ValueType&& Value) &
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			// nullptr: copying a value nested deeper than MaxDepth failed (logged); nothing is written
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetNode(Key, NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson Set(FStringView Key, ValueType&& Value) &&
	{
		Set(Key, Forward<ValueType>(Value)); // *this is an lvalue here: the & overload
		return MoveTemp(*this);
	}

	/** UTF-8 key: no TCHAR -> UTF-8 conversion on hot paths */
	template <typename ValueType>
	FVaRestJson& Set(FUtf8StringView Key, ValueType&& Value) &
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetNodeUtf8(Key, NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson Set(FUtf8StringView Key, ValueType&& Value) &&
	{
		Set(Key, Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
	FVaRestJson& Set(const TCHAR* Key, ValueType&& Value) & { return Set(FStringView(Key), Forward<ValueType>(Value)); }

	template <typename ValueType>
	FVaRestJson Set(const TCHAR* Key, ValueType&& Value) &&
	{
		Set(FStringView(Key), Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
	FVaRestJson& Set(const UTF8CHAR* Key, ValueType&& Value) & { return Set(FUtf8StringView(Key), Forward<ValueType>(Value)); }

	template <typename ValueType>
	FVaRestJson Set(const UTF8CHAR* Key, ValueType&& Value) &&
	{
		Set(FUtf8StringView(Key), Forward<ValueType>(Value));
		return MoveTemp(*this);
	}
```

and the array writers:

```cpp
	/** Appends (copies) Value. An invalid handle becomes a new array first. Ref-qualified like Set. */
	template <typename ValueType>
	FVaRestJson& Add(ValueType&& Value) &
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Array, TEXT("Add an element")))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				AddNode(NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson Add(ValueType&& Value) &&
	{
		Add(Forward<ValueType>(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
	FVaRestJson& SetAt(int32 Index, ValueType&& Value) &
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareSetAt(Index))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetAtNode(Index, NewValue);
			}
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson SetAt(int32 Index, ValueType&& Value) &&
	{
		SetAt(Index, Forward<ValueType>(Value));
		return MoveTemp(*this);
	}
```

4. Replace the `ToString`/`ToUtf8` declarations and their comments with:

```cpp
	/**
	 * "" for an invalid handle, "null" for a null value. A value nested deeper than VaRest::Json::MaxDepth (possible
	 * only when built through Set/Add) gives "" and an error log (spec 2.2); TryToString reports it instead.
	 */
	FString ToString(EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** Appends UTF-8 to Out; nothing for an invalid handle; nothing plus an error log past MaxDepth */
	void ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** ToString with the depth failure as an error value instead of a log line ("" for an invalid handle) */
	TValueOrError<FString, FVaRestJsonError> TryToString(EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** ToUtf8 with the depth failure as an error value; Out keeps its previous contents on failure */
	TValueOrError<void, FVaRestJsonError> TryToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
```

and add to the `Clone`, `Equals`, `ToJsonValue` and `FromJsonValue` declarations a line each: `/** Past VaRest::Json::MaxDepth: an invalid handle (nullptr / false) and an error log, never a truncated copy */`.

5. `MakeNodeFor` can now return null (a copy past `MaxDepth`), so in `From` replace `return FVaRestJson(MoveTemp(NewDoc), Root);` with:

```cpp
		// A value that could not be made (a copy past MaxDepth, logged) gives a default handle, not a "miss" handle
		// that holds a document and would refuse writes with "a lookup missed" errors
		return Root ? FVaRestJson(MoveTemp(NewDoc), Root) : FVaRestJson();
```

- [ ] **Step 4: Create `Source/VaRestJson/Private/VaRestJsonSax.h`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/** RapidJSON output stream over a TArray. Put/PutReserve/PutUnsafe are found by ADL from writer.h. */
	struct FUtf8Sink
	{
		typedef char Ch;

		TArray<uint8>& Out;

		void Put(char C) { Out.Add(static_cast<uint8>(C)); }
		void Flush() {}
	};

	/**
	 * No-op on purpose. RapidJSON's escape path reserves 2 + length * 6 bytes per string
	 * (writer.h:398), which over-reserves up to 6x and whose int32 cast can overflow. The reserve
	 * is only a hint, so TArray's geometric growth in Put/PutUnsafe is used instead. This overload
	 * exists so ADL picks it over rapidjson's generic template, which would call Put per character.
	 */
	inline void PutReserve(FUtf8Sink& Sink, size_t Count)
	{
		(void)Sink;
		(void)Count;
	}

	inline void PutUnsafe(FUtf8Sink& Sink, char C)
	{
		Sink.Out.Add(static_cast<uint8>(C));
	}

	/**
	 * The writers' level stack goes through FMemory via the engine's RapidJSON allocator
	 * (JsonUtils/RapidJsonUtils.h:19-53), passed as the StackAllocator argument (writer.h:90, :103;
	 * prettywriter.h:47, :58), instead of the default CrtAllocator's std::malloc (rapidjson.h:694-696).
	 */
	using FWriterAllocator = UE::Json::Private::FAllocatorImpl;
	using FCondensedWriter = rapidjson::Writer<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FWriterAllocator>;
	using FPrettyWriter = rapidjson::PrettyWriter<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FWriterAllocator>;

	struct FEmitState
	{
		/** Non-finite doubles written as null (JSON has no NaN/Infinity) */
		int32 NonFinite = 0;
		/** A container nested deeper than MaxDepth was met; the walk stopped */
		bool bTooDeep = false;
	};

	/**
	 * SAX walk of Value into Sink: a RapidJSON writer, or FDomBuilder (VaRestJsonBuilder.h, same handler interface).
	 * Depth is the number of containers around Value; a container at Depth >= MaxDepth stops the walk (false,
	 * State.bTooDeep) -- the parser's own rule, so anything that parses also writes. Strings go with copy = true: the
	 * writers ignore it, FDomBuilder copies them into its document.
	 */
	template <typename SinkType>
	bool EmitNode(SinkType& Sink, const FNode& Value, int32 Depth, FEmitState& State)
	{
		switch (Value.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			Sink.Null();
			return true;
		case EVaRestJsonType::Bool:
			Sink.Bool(Value.Bool);
			return true;
		case EVaRestJsonType::Number:
			switch (Value.NumKind)
			{
			case ENumKind::Int:
				Sink.Int64(Value.Int);
				return true;
			case ENumKind::UInt:
				Sink.Uint64(Value.UInt);
				return true;
			case ENumKind::Double:
				// Writer::Double refuses NaN/Inf only after Prefix() has run (writer.h:195), so test first
				if (FMath::IsFinite(Value.Double))
				{
					Sink.Double(Value.Double);
				}
				else
				{
					Sink.Null();
					++State.NonFinite;
				}
				return true;
			}
			Sink.Null();
			return true;
		case EVaRestJsonType::String:
			Sink.String(reinterpret_cast<const char*>(Value.Str.Data), static_cast<rapidjson::SizeType>(Value.Str.Len), true);
			return true;
		case EVaRestJsonType::Array:
			if (Depth >= MaxDepth || !Sink.StartArray())
			{
				State.bTooDeep = true;
				return false;
			}
			for (uint32 i = 0; i < Value.Arr.Num; ++i)
			{
				if (!EmitNode(Sink, *Value.Arr.Items[i], Depth + 1, State))
				{
					return false;
				}
			}
			Sink.EndArray(static_cast<rapidjson::SizeType>(Value.Arr.Num));
			return true;
		case EVaRestJsonType::Object:
			if (Depth >= MaxDepth || !Sink.StartObject())
			{
				State.bTooDeep = true;
				return false;
			}
			for (uint32 i = 0; i < Value.Obj.Num; ++i)
			{
				const FMember& M = Value.Obj.Members[i];
				Sink.Key(reinterpret_cast<const char*>(M.Key.Data), static_cast<rapidjson::SizeType>(M.Key.Len), true);
				if (!EmitNode(Sink, *M.Value, Depth + 1, State))
				{
					return false;
				}
			}
			Sink.EndObject(static_cast<rapidjson::SizeType>(Value.Obj.Num));
			return true;
		}
		Sink.Null();
		return true;
	}
}
```

- [ ] **Step 5: Replace the writer**

`Source/VaRestJson/Private/VaRestJsonWriter.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/**
	 * Appends Node as UTF-8 JSON to Out. Non-finite doubles are written as null with one warning. Returns false, with
	 * Out back at its previous length, when Node nests deeper than MaxDepth (spec 2.2: the operation fails as a whole).
	 */
	bool Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out);
}
```

`Source/VaRestJson/Private/VaRestJsonWriter.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonWriter.h"

#include "VaRestJsonLog.h"
#include "VaRestJsonSax.h"

namespace VaRest::Json
{
	bool Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out)
	{
		const int32 Start = Out.Num();
		FUtf8Sink Sink{ Out };
		FWriterAllocator StackAllocator;
		FEmitState State;
		bool bWritten = false;
		if (Format == EVaRestJsonFormat::Pretty)
		{
			FPrettyWriter Writer(Sink, &StackAllocator);
			Writer.SetIndent(' ', 2);
			bWritten = EmitNode(Writer, Node, 0, State);
		}
		else
		{
			FCondensedWriter Writer(Sink, &StackAllocator);
			bWritten = EmitNode(Writer, Node, 0, State);
		}
		if (!bWritten)
		{
			Out.SetNum(Start, EAllowShrinking::No);
			return false;
		}
		if (State.NonFinite > 0)
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), State.NonFinite);
		}
		return true;
	}
}
```

- [ ] **Step 6: Text, copies, equality**

In `VaRestJsonText.cpp`: add `#include "VaRestJsonLog.h"`; inside `namespace VaRest::Json::TextPrivate` add

```cpp
	FVaRestJsonError TooDeepToWrite()
	{
		FVaRestJsonError Error;
		Error.Message = FString::Printf(TEXT("cannot write a value nested deeper than %d levels"), MaxDepth);
		return Error;
	}
```

and replace `FVaRestJson::ToUtf8` and `FVaRestJson::ToString` with:

```cpp
void FVaRestJson::ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format) const
{
	if (Node && !VaRest::Json::Write(*Node, Format, Out))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::ToUtf8: cannot write a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
	}
}

FString FVaRestJson::ToString(EVaRestJsonFormat Format) const
{
	if (!Node)
	{
		return FString();
	}
	TArray<uint8> Bytes;
	if (!VaRest::Json::Write(*Node, Format, Bytes))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::ToString: cannot write a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
		return FString();
	}
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}

TValueOrError<FString, FVaRestJsonError> FVaRestJson::TryToString(EVaRestJsonFormat Format) const
{
	TArray<uint8> Bytes;
	if (Node && !VaRest::Json::Write(*Node, Format, Bytes))
	{
		FVaRestJsonError Error = VaRest::Json::TextPrivate::TooDeepToWrite();
		return MakeError(MoveTemp(Error));
	}
	FString Text = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	return MakeValue(MoveTemp(Text));
}

TValueOrError<void, FVaRestJsonError> FVaRestJson::TryToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format) const
{
	if (Node && !VaRest::Json::Write(*Node, Format, Out))
	{
		FVaRestJsonError Error = VaRest::Json::TextPrivate::TooDeepToWrite();
		return MakeError(MoveTemp(Error));
	}
	return MakeValue();
}
```

In `VaRestJsonDocument.h` replace the `DeepCopy` and `Equals` declarations with:

```cpp
		/** Deep copy of Src, from any document, into this one; reads Src completely before returning. nullptr when Src nests deeper than MaxDepth (nodes copied so far stay in the arena). */
		FNode* DeepCopy(const FNode& Src);

		/** JSON equality: 1 == 1.0, objects ignore member order and compare duplicates by their last occurrence. False, and *bOutTooDeep set, past MaxDepth. */
		static bool Equals(const FNode& A, const FNode& B, bool* bOutTooDeep = nullptr);
```

and add to its `private:` section:

```cpp
		FNode* DeepCopyAt(const FNode& Src, int32 Depth);
		static bool EqualsAt(const FNode& A, const FNode& B, int32 Depth, bool& bTooDeep);
```

In `VaRestJsonDocument.cpp` replace `FDocument::DeepCopy` and `FDocument::Equals` with:

```cpp
	FNode* FDocument::DeepCopy(const FNode& Src)
	{
		return DeepCopyAt(Src, 0);
	}

	FNode* FDocument::DeepCopyAt(const FNode& Src, int32 Depth)
	{
		switch (Src.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return NewNull();
		case EVaRestJsonType::Bool:
			return NewBool(Src.Bool);
		case EVaRestJsonType::Number:
			switch (Src.NumKind)
			{
			case ENumKind::Int:    return NewInt(Src.Int);
			case ENumKind::UInt:   return NewUInt(Src.UInt);
			case ENumKind::Double: return NewDouble(Src.Double);
			}
			return NewNull();
		case EVaRestJsonType::String:
			return NewString(Src.Str.View());
		case EVaRestJsonType::Array:
		{
			if (Depth >= MaxDepth)
			{
				return nullptr;
			}
			const uint32 Count = Src.Arr.Num;
			FNode* Copy = NewArray(Count);
			for (uint32 i = 0; i < Count; ++i)
			{
				FNode* Item = DeepCopyAt(*Src.Arr.Items[i], Depth + 1);
				if (!Item)
				{
					return nullptr;
				}
				Copy->Arr.Items[i] = Item;
			}
			Copy->Arr.Num = Count;
			return Copy;
		}
		case EVaRestJsonType::Object:
		{
			if (Depth >= MaxDepth)
			{
				return nullptr;
			}
			const uint32 Count = Src.Obj.Num;
			FNode* Copy = NewObject(Count);
			for (uint32 i = 0; i < Count; ++i)
			{
				const FMember& M = Src.Obj.Members[i];
				FNode* Item = DeepCopyAt(*M.Value, Depth + 1);
				if (!Item)
				{
					return nullptr;
				}
				FStr Key = CopyStr(M.Key.View(), false);
				Key.Hash = M.Key.Hash;
				Copy->Obj.Members[i] = FMember{ Key, Item };
			}
			Copy->Obj.Num = Count;
			if (Count > IndexThreshold)
			{
				BuildIndex(*Copy);
			}
			return Copy;
		}
		}
		return NewNull();
	}

	bool FDocument::Equals(const FNode& A, const FNode& B, bool* bOutTooDeep)
	{
		bool bTooDeep = false;
		const bool bEqual = EqualsAt(A, B, 0, bTooDeep);
		if (bOutTooDeep)
		{
			*bOutTooDeep = bTooDeep;
		}
		return bEqual && !bTooDeep;
	}

	bool FDocument::EqualsAt(const FNode& A, const FNode& B, int32 Depth, bool& bTooDeep)
	{
		if (A.Type != B.Type)
		{
			return false;
		}
		switch (A.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return true;
		case EVaRestJsonType::Bool:
			return A.Bool == B.Bool;
		case EVaRestJsonType::Number:
			return DocumentPrivate::NumbersEqual(A, B);
		case EVaRestJsonType::String:
			return KeyEquals(A.Str, B.Str.View());
		case EVaRestJsonType::Array:
			if (A.Arr.Num != B.Arr.Num)
			{
				return false;
			}
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return false;
			}
			for (uint32 i = 0; i < A.Arr.Num; ++i)
			{
				if (!EqualsAt(*A.Arr.Items[i], *B.Arr.Items[i], Depth + 1, bTooDeep))
				{
					return false;
				}
			}
			return true;
		case EVaRestJsonType::Object:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return false;
			}
			// Distinct keys, each by its last occurrence -- the same view Find gives.
			uint32 DistinctA = 0;
			for (uint32 i = 0; i < A.Obj.Num; ++i)
			{
				const FMember& M = A.Obj.Members[i];
				if (FindMember(A, M.Key.View(), M.Key.Hash) != static_cast<int32>(i))
				{
					continue; // an earlier duplicate
				}
				++DistinctA;
				const int32 j = FindMember(B, M.Key.View(), M.Key.Hash);
				if (j == INDEX_NONE || !EqualsAt(*M.Value, *B.Obj.Members[j].Value, Depth + 1, bTooDeep))
				{
					return false;
				}
			}
			uint32 DistinctB = 0;
			for (uint32 i = 0; i < B.Obj.Num; ++i)
			{
				const FMember& M = B.Obj.Members[i];
				DistinctB += FindMember(B, M.Key.View(), M.Key.Hash) == static_cast<int32>(i);
			}
			return DistinctA == DistinctB;
		}
		}
		return false;
	}
```

In `VaRestJson.cpp` replace `Clone`, `Equals` and `MakeCopy` with:

```cpp
FVaRestJson FVaRestJson::Clone() const
{
	using VaRest::Json::FArena;
	if (!Node)
	{
		return {};
	}
	// A root clone needs about what the source used; a subtree clone starts small and grows.
	FDocumentRef NewDoc = NewDocument(Node == Doc->Root ? FMath::Max<SIZE_T>(FArena::MinChunkSize, Doc->Arena.GetUsedBytes()) : FArena::MinChunkSize);
	VaRest::Json::FNode* Root = NewDoc->DeepCopy(*Node);
	if (!Root)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Clone: cannot copy a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
		return {};
	}
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

bool FVaRestJson::Equals(const FVaRestJson& Other) const
{
	if (!Node || !Other.Node)
	{
		return !Node && !Other.Node;
	}
	if (Node == Other.Node)
	{
		return true;
	}
	bool bTooDeep = false;
	const bool bEqual = VaRest::Json::FDocument::Equals(*Node, *Other.Node, &bTooDeep);
	if (bTooDeep)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Equals: cannot compare values nested deeper than %d levels; treated as not equal"), VaRest::Json::MaxDepth);
	}
	return bEqual;
}
```

```cpp
VaRest::Json::FNode* FVaRestJson::MakeCopy(VaRest::Json::FDocument& InDoc, const FVaRestJson& Value)
{
	if (!Value.Node)
	{
		return InDoc.NewNull();
	}
	// A node has exactly one parent, so even a value from this same document is copied.
	VaRest::Json::FNode* Copy = InDoc.DeepCopy(*Value.Node);
	if (!Copy)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot copy a value nested deeper than %d levels; nothing was written"), VaRest::Json::MaxDepth);
	}
	return Copy;
}
```

- [ ] **Step 7: Bridges, Serialize, parse clamp**

Replace `Source/VaRestJson/Private/VaRestJsonBridge.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"

namespace VaRest::Json::BridgePrivate
{
	constexpr int64 MaxExactDoubleInt = 9007199254740992ll; // 2^53

	FString Utf8ToString(const FStr& S)
	{
		return FString::ConstructFromPtrSize(S.Data, static_cast<int32>(S.Len));
	}

	/** Engine value for Value; nullptr with bTooDeep past MaxDepth. Depth counts the containers around Value. */
	TSharedPtr<FJsonValue> ToEngine(const FNode& Value, int32 Depth, bool& bTooDeep)
	{
		switch (Value.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return MakeShared<FJsonValueNull>();
		case EVaRestJsonType::Bool:
			return MakeShared<FJsonValueBoolean>(Value.Bool);
		case EVaRestJsonType::Number:
			switch (Value.NumKind)
			{
			case ENumKind::Int:
				// FJsonValueNumber holds a double; beyond 2^53 keep the exact digits instead
				if (Value.Int >= -MaxExactDoubleInt && Value.Int <= MaxExactDoubleInt)
				{
					return MakeShared<FJsonValueNumber>(static_cast<double>(Value.Int));
				}
				return MakeShared<FJsonValueNumberString>(LexToString(Value.Int));
			case ENumKind::UInt:
				return MakeShared<FJsonValueNumberString>(LexToString(Value.UInt));
			case ENumKind::Double:
				return MakeShared<FJsonValueNumber>(Value.Double);
			}
			break;
		case EVaRestJsonType::String:
			return MakeShared<FJsonValueString>(Utf8ToString(Value.Str));
		case EVaRestJsonType::Array:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Value.Arr.Num);
			for (uint32 i = 0; i < Value.Arr.Num; ++i)
			{
				TSharedPtr<FJsonValue> Item = ToEngine(*Value.Arr.Items[i], Depth + 1, bTooDeep);
				if (bTooDeep)
				{
					return nullptr;
				}
				Items.Add(MoveTemp(Item));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		case EVaRestJsonType::Object:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
			for (uint32 i = 0; i < Value.Obj.Num; ++i)
			{
				const FMember& M = Value.Obj.Members[i];
				TSharedPtr<FJsonValue> Member = ToEngine(*M.Value, Depth + 1, bTooDeep);
				if (bTooDeep)
				{
					return nullptr;
				}
				Fields->SetField(FStringView(Utf8ToString(M.Key)), Member); // later duplicates overwrite
			}
			return MakeShared<FJsonValueObject>(Fields);
		}
		}
		return MakeShared<FJsonValueNull>();
	}

	/**
	 * Exact decimal integer: sign and magnitude. False for anything that is not an integer within the
	 * int64 (negative) or uint64 (positive) range. Allocates nothing, so a rejected candidate leaves no node behind.
	 */
	bool ParseIntegerDigits(const FString& S, bool& bNegative, uint64& Magnitude)
	{
		bNegative = S.StartsWith(TEXT("-"));
		int32 i = bNegative ? 1 : 0;
		if (i >= S.Len())
		{
			return false;
		}
		Magnitude = 0;
		for (; i < S.Len(); ++i)
		{
			if (!FChar::IsDigit(S[i]))
			{
				return false;
			}
			const uint64 Digit = static_cast<uint64>(S[i] - TEXT('0'));
			if (Magnitude > (MAX_uint64 - Digit) / 10)
			{
				return false; // overflow
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		return !bNegative || Magnitude <= 9223372036854775808ull;
	}

	double ToDouble(bool bNegative, uint64 Magnitude)
	{
		const double Result = static_cast<double>(Magnitude); // round to nearest, as strtod does
		return bNegative ? -Result : Result;
	}

	FNode* MakeIntegerNode(FDocument& Doc, bool bNegative, uint64 Magnitude)
	{
		if (!bNegative)
		{
			return Doc.NewUInt(Magnitude); // stored as Int when it fits int64
		}
		return Doc.NewInt(Magnitude == 9223372036854775808ull ? MIN_int64 : -static_cast<int64>(Magnitude));
	}

	/** Our node for Value; nullptr with bTooDeep past MaxDepth (an FJsonObject graph may even be cyclic) */
	FNode* FromEngine(FDocument& Doc, const FJsonValue& Value, int32 Depth, bool& bTooDeep)
	{
		switch (Value.Type)
		{
		case EJson::None:
		case EJson::Null:
			return Doc.NewNull();
		case EJson::Boolean:
			return Doc.NewBool(Value.AsBool());
		case EJson::String:
		{
			const FString S = Value.AsString();
			const auto Utf8 = StringCast<UTF8CHAR>(*S, S.Len());
			return Doc.NewString(FUtf8StringView(Utf8.Get(), Utf8.Length()));
		}
		case EJson::Number:
		{
			double D = 0.0;
			if (!Value.TryGetNumber(D))
			{
				return Doc.NewNull(); // malformed number string; no engine log
			}
			// Exact digits first. FJsonValueNumber::TryGetString is SanitizeFloat with 6 fractional digits and
			// trailing zeros trimmed (JsonValue.cpp:446-450), so for a plain double its text is not authoritative:
			// 0.9999999 would read as "1". Take the digits only when they round to the same double, which every
			// genuine number string does, so 64-bit values above 2^53 still come back exact.
			FString Digits;
			bool bNegative = false;
			uint64 Magnitude = 0;
			if (Value.TryGetString(Digits) && ParseIntegerDigits(Digits, bNegative, Magnitude) && ToDouble(bNegative, Magnitude) == D)
			{
				return MakeIntegerNode(Doc, bNegative, Magnitude);
			}
			// The engine writes integral doubles without a fraction, so 5.0 becomes 5 and -0.0 becomes 0 by design
			if (FMath::TruncToDouble(D) == D && D >= -9007199254740992.0 && D <= 9007199254740992.0)
			{
				return Doc.NewInt(static_cast<int64>(D));
			}
			return Doc.NewDouble(D);
		}
		case EJson::Array:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			const TArray<TSharedPtr<FJsonValue>>& Items = Value.AsArray();
			FNode* Result = Doc.NewArray(Items.Num());
			for (const TSharedPtr<FJsonValue>& Item : Items)
			{
				FNode* Element = Item.IsValid() ? FromEngine(Doc, *Item, Depth + 1, bTooDeep) : Doc.NewNull();
				if (!Element)
				{
					return nullptr;
				}
				Doc.Append(*Result, Element);
			}
			return Result;
		}
		case EJson::Object:
		{
			if (Depth >= MaxDepth)
			{
				bTooDeep = true;
				return nullptr;
			}
			const TSharedPtr<FJsonObject>& Fields = Value.AsObject();
			FNode* Result = Doc.NewObject(Fields.IsValid() ? Fields->Values.Num() : 0);
			if (Fields.IsValid())
			{
				for (const auto& Pair : Fields->Values)
				{
					FNode* Member = Pair.Value.IsValid() ? FromEngine(Doc, *Pair.Value, Depth + 1, bTooDeep) : Doc.NewNull();
					if (!Member)
					{
						return nullptr;
					}
					const FString Key(*Pair.Key); // FSharedString or FString storage: both dereference to TCHAR*
					const auto Utf8 = StringCast<UTF8CHAR>(*Key, Key.Len());
					Doc.Set(*Result, FUtf8StringView(Utf8.Get(), Utf8.Length()), Member);
				}
			}
			return Result;
		}
		}
		return Doc.NewNull();
	}
}

TSharedPtr<FJsonValue> FVaRestJson::ToJsonValue() const
{
	if (!Node)
	{
		return nullptr;
	}
	bool bTooDeep = false;
	TSharedPtr<FJsonValue> Result = VaRest::Json::BridgePrivate::ToEngine(*Node, 0, bTooDeep);
	if (bTooDeep)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::ToJsonValue: cannot convert a value nested deeper than %d levels"), VaRest::Json::MaxDepth);
		return nullptr;
	}
	return Result;
}

TSharedPtr<FJsonObject> FVaRestJson::ToJsonObject() const
{
	if (!IsObject())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Converted = ToJsonValue();
	return Converted.IsValid() ? Converted->AsObject() : nullptr;
}

FVaRestJson FVaRestJson::FromJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->Type == EJson::None)
	{
		return FVaRestJson();
	}
	FDocumentRef NewDoc = NewDocument();
	bool bTooDeep = false;
	VaRest::Json::FNode* Root = VaRest::Json::BridgePrivate::FromEngine(*NewDoc, *Value, 0, bTooDeep);
	if (bTooDeep)
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::FromJsonValue: cannot convert a value nested deeper than %d levels (or a cyclic FJsonObject graph)"), VaRest::Json::MaxDepth);
		return FVaRestJson();
	}
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::FromJsonObject(const TSharedPtr<FJsonObject>& InObject)
{
	return InObject.IsValid() ? FromJsonValue(MakeShared<FJsonValueObject>(InObject)) : FVaRestJson();
}
```

In `VaRestJsonStructOps.cpp` add `#include "VaRestJsonWriter.h"` and replace everything in `Serialize` after the `if (Ar.IsLoading()) { ... }` block (from the save-side `Ar << Version;` to the final `return true;`) with:

```cpp
	Ar << Version;
	// Write the text first: a value nested deeper than MaxDepth cannot be written, and a load would reject it anyway
	// (spec 2.2), so it is saved as an invalid handle plus an error instead of a payload nothing reads back.
	TArray<uint8> Bytes;
	if (ValidByte != 0 && !VaRest::Json::Write(*Node, EVaRestJsonFormat::Condensed, Bytes))
	{
		UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Serialize: cannot save a value nested deeper than %d levels; saved as an invalid handle"), VaRest::Json::MaxDepth);
		ValidByte = 0;
	}
	Ar << ValidByte;
	if (ValidByte != 0)
	{
		int64 Length = Bytes.Num();
		Ar << Length;
		if (Length > 0)
		{
			Ar.Serialize(Bytes.GetData(), Length);
		}
	}
	return true;
```

In `VaRestJsonReader.cpp`: rename `FBuildHandler`'s member `MaxDepth` to `DepthLimit` and its constructor parameter `InMaxDepth` to `InDepthLimit` (clang `-Wshadow` would flag a name equal to `VaRest::Json::MaxDepth`), and in `ParseInSitu` replace `const int32 MaxDepth = FMath::Max(1, Options.MaxDepth);` with

```cpp
		const int32 DepthLimit = FMath::Clamp(Options.MaxDepth, 1, MaxDepth); // spec 2.2: one limit, so anything that parses also writes
```

and the two later uses of the local (`FBuildHandler Handler(Doc, ...)`, the `"Nesting deeper than %d"` message) to `DepthLimit`.

- [ ] **Step 8: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including `VaRest.Json.Depth.AtLimitWorks` and `VaRest.Json.Depth.PastLimitFails`.

- [ ] **Step 9: Restore-the-bug (depth limit)**

In `VaRestJsonSax.h` delete `Depth >= MaxDepth ||` from the `Array` case. Run `Tools/run_tests.sh VaRest.Json.Depth`. Expected: `VaRest.Json.Depth.PastLimitFails` fails (`ToString past the limit is empty`, and the expected-error count). Restore, run again, expect it green. Report both.

- [ ] **Step 10: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: one depth limit (512) for write, Clone, Equals, bridges and Serialize; TryToString/TryToUtf8

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 4: Strings entering a document are valid UTF-8 (spec §2.2, VaRest-qop) and code-point error columns

Also the VaRest-rfd items "error columns in bytes incl. BOM" and "StringCast vs FPlatformString::Convert on the error path": both paths of `Parse(FStringView)` now use one converter.

**Engine facts this task rests on:** the engine's UTF-16 → UTF-8 conversion maps an unpaired surrogate to `UNICODE_BOGUS_CHAR_CODEPOINT`, which is `'?'` (`GenericPlatformString.cpp:791-797`, `GenericPlatformString.h:55`), not U+FFFD — so the spec's "engine conversion with lone surrogates mapped to U+FFFD" needs a converter of our own. RapidJSON already rejects unpaired `\u` surrogate escapes in parsed text (`reader.h:1026-1041`), so parsed strings stay valid.

**Files:**
- Create: `Source/VaRestJson/Public/VaRestJsonUtf8.h` (public and exported: the WebSocket/Socket.IO modules validate byte strings the same way, WebSocket spec §19 item 11), `Source/VaRestJson/Private/VaRestJsonUtf8.cpp`
- Modify: `Source/VaRestJson/Public/VaRestJson.h` (`FVaRestJsonError::Column` doc, Set doc)
- Modify: `VaRestJson.cpp` (`Get`, `Remove`, `SetNode`, `SetNodeUtf8`, `MakeString`, `MakeUtf8String`), `VaRestJsonText.cpp` (`Parse(FStringView)`), `VaRestJsonReader.cpp` (`FillLineColumn`), `VaRestJsonBridge.cpp` (`FromEngine` strings and keys), `VaRestJsonStructOps.cpp` (`ImportTextItem`)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonUtf8Test.cpp`

**Interfaces:**
- Produces (`VaRestJsonUtf8.h`, namespace `VaRest::Json`, every function `VARESTJSON_API`): `using FUtf8Scratch = TArray<UTF8CHAR, TInlineAllocator<256>>`; `bool IsValidUtf8(FUtf8StringView)`; `int32 AppendRepairedUtf8(FUtf8StringView, FUtf8Scratch&)`; `int64 Utf8Length(FStringView)`; `int32 WriteUtf8(FStringView, UTF8CHAR* Dest)`; `FUtf8StringView ConvertForDocument(FStringView Text, FUtf8Scratch& Scratch, bool bWarn = true)`; `FUtf8StringView ValidateForDocument(FUtf8StringView Text, FUtf8Scratch& Scratch)`. Every repair logs one `Warning` containing `replaced with U+FFFD`. `FVaRestJsonError::Column` counts Unicode code points and skips a leading BOM; `Offset` stays a byte offset.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonUtf8Test.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonUtf8.h"

namespace VaRestJsonUtf8Test
{
	FUtf8StringView Bytes(const char* Text, int32 Len)
	{
		return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text), Len);
	}

	TArray<uint8> Repair(const TArray<uint8>& In, int32& OutReplaced)
	{
		VaRest::Json::FUtf8Scratch Out;
		OutReplaced = VaRest::Json::AppendRepairedUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(In.GetData()), In.Num()), Out);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Out.GetData()), Out.Num());
	}

	TArray<uint8> Convert(const FString& In, int32& OutReplaced)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(static_cast<int32>(VaRest::Json::Utf8Length(In)));
		OutReplaced = VaRest::Json::WriteUtf8(In, reinterpret_cast<UTF8CHAR*>(Out.GetData()));
		return Out;
	}

	FString WithUnit(const TCHAR* Before, uint16 Unit, const TCHAR* After)
	{
		FString Result = Before;
		Result.AppendChar(static_cast<TCHAR>(Unit));
		Result += After;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8RepairTest, "VaRest.Json.Utf8.Repair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8RepairTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	struct FCase
	{
		const TCHAR* Name;
		TArray<uint8> In;
		TArray<uint8> Out;
		int32 Replaced;
	};
	// One U+FFFD per maximal subpart (The Unicode Standard, ch. 3 "U+FFFD Substitution of Maximal Subparts")
	const FCase Cases[] = {
		{ TEXT("stray continuation"), { 'a', 0x80, 'b' }, { 'a', 0xEF, 0xBF, 0xBD, 'b' }, 1 },
		{ TEXT("overlong lead C0"), { 0xC0, 0xAF }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 2 },
		{ TEXT("overlong E0 80 80"), { 0xE0, 0x80, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 3 },
		{ TEXT("truncated 3-byte"), { 0xE2, 0x82 }, { 0xEF, 0xBF, 0xBD }, 1 },
		{ TEXT("CESU surrogate"), { 0xED, 0xA0, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 3 },
		{ TEXT("above U+10FFFF"), { 0xF4, 0x90, 0x80, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 4 },
		{ TEXT("truncated 4-byte then ASCII"), { 0xF0, 0x9F, 0x98, 'x' }, { 0xEF, 0xBF, 0xBD, 'x' }, 1 },
		{ TEXT("valid incl. NUL"), { 0xD0, 0x9A, 0xF0, 0x9F, 0x98, 0x80, 0x00, 'z' }, { 0xD0, 0x9A, 0xF0, 0x9F, 0x98, 0x80, 0x00, 'z' }, 0 },
	};
	for (const FCase& Case : Cases)
	{
		int32 Replaced = -1;
		const TArray<uint8> Out = Repair(Case.In, Replaced);
		TestTrue(FString::Printf(TEXT("%s: bytes"), Case.Name), Out == Case.Out);
		TestEqual(FString::Printf(TEXT("%s: replacements"), Case.Name), Replaced, Case.Replaced);
		const bool bValid = VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Case.In.GetData()), Case.In.Num()));
		TestTrue(FString::Printf(TEXT("%s: IsValidUtf8"), Case.Name), bValid == (Case.Replaced == 0));
	}

	int32 Replaced = -1;
	TestTrue(TEXT("lone high surrogate at the end"), Convert(WithUnit(TEXT("a"), 0xD800, TEXT("")), Replaced) == TArray<uint8>({ 'a', 0xEF, 0xBF, 0xBD }) && Replaced == 1);
	TestTrue(TEXT("lone low surrogate"), Convert(WithUnit(TEXT(""), 0xDC00, TEXT("b")), Replaced) == TArray<uint8>({ 0xEF, 0xBF, 0xBD, 'b' }) && Replaced == 1);
	FString Reversed;
	Reversed.AppendChar(static_cast<TCHAR>(0xDC00));
	Reversed.AppendChar(static_cast<TCHAR>(0xD800));
	TestTrue(TEXT("reversed pair is two replacements"), Convert(Reversed, Replaced).Num() == 6 && Replaced == 2);
	FString Pair;
	Pair.AppendChar(static_cast<TCHAR>(0xD83D));
	Pair.AppendChar(static_cast<TCHAR>(0xDE00));
	TestTrue(TEXT("a real pair is one 4-byte character"), Convert(Pair, Replaced) == TArray<uint8>({ 0xF0, 0x9F, 0x98, 0x80 }) && Replaced == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8ApiTest, "VaRest.Json.Utf8.ApiStrings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8ApiTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	// key + value of the byte Set (2), the FString value (1), the FString key (1), the bridge string (1); lookups store nothing and stay quiet
	AddExpectedMessagePlain(TEXT("replaced with U+FFFD"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 5);

	FVaRestJson J = FVaRestJson::Object();
	J.Set(Bytes("k\x80", 2), Bytes("v\xFF", 2));
	TestEqualSensitive(TEXT("repaired key and value"), J.ToString(), FString(TEXT("{\"k�\":\"v�\"}")));
	TestTrue(TEXT("what is written parses back"), FVaRestJson::Parse(FStringView(J.ToString())).HasValue());

	const FString Lone = WithUnit(TEXT("a"), 0xD800, TEXT(""));
	J.Set(TEXT("s"), Lone);
	TestTrue(TEXT("lone surrogate value -> U+FFFD"), J[TEXT("s")].AsUtf8View().Equals(Bytes("a\xEF\xBF\xBD", 4), ESearchCase::CaseSensitive));
	J.Set(Lone, 1);
	TestEqual(TEXT("a key with a lone surrogate is found by the same text"), J[FStringView(Lone)].AsInt(), 1);

	J.Set(TEXT("ansi"), "plain");
	TestEqualSensitive(TEXT("an ANSI literal is a UTF-8 string value"), J[TEXT("ansi")].AsString(), FString(TEXT("plain")));

	const FVaRestJson Bridged = FVaRestJson::FromJsonValue(MakeShared<FJsonValueString>(Lone));
	TestTrue(TEXT("bridge strings are converted the same way"), Bridged.AsUtf8View().Equals(Bytes("a\xEF\xBF\xBD", 4), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8ColumnsTest, "VaRest.Json.Utf8.ParseAndColumns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8ColumnsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	// The two FString parses with an unpaired surrogate warn once each
	AddExpectedMessagePlain(TEXT("replaced with U+FFFD"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	// One converter for the parse and its diagnostics: an unpaired surrogate is U+FFFD (3 bytes) in both
	const FVaRestJsonError Lone = FVaRestJson::Parse(FStringView(WithUnit(TEXT("[\""), 0xD800, TEXT("\",x]")))).GetError();
	const FVaRestJsonError Fffd = FVaRestJson::Parse(FStringView(TEXT("[\"�\",x]"))).GetError();
	TestEqual(TEXT("same offset as a real U+FFFD"), Lone.Offset, Fffd.Offset);
	TestEqual(TEXT("same column as a real U+FFFD"), Lone.Column, Fffd.Column);

	const FVaRestJson Parsed = FVaRestJson::Parse(FStringView(WithUnit(TEXT("{\"s\":\""), 0xD800, TEXT("\"}")))).StealValue();
	TestTrue(TEXT("a lone surrogate inside a string parses as U+FFFD"), Parsed[TEXT("s")].AsUtf8View().Equals(Bytes("\xEF\xBF\xBD", 3), ESearchCase::CaseSensitive));

	// Columns count code points; a BOM is not a column (Offset stays in bytes)
	const FVaRestJsonError Ascii = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"e\":x}"))).GetError();
	const FVaRestJsonError Accented = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"é\":x}"))).GetError();
	TestEqual(TEXT("a two-byte character is one column"), Accented.Column, Ascii.Column);
	TestEqual(TEXT("... and two bytes"), Accented.Offset, Ascii.Offset + 1);
	TArray<uint8> WithBom = { 0xEF, 0xBB, 0xBF, '{', '"', 'e', '"', ':', 'x', '}' };
	const FVaRestJsonError Bom = FVaRestJson::Parse(TConstArrayView<uint8>(WithBom)).GetError();
	TestEqual(TEXT("the BOM is not a column"), Bom.Column, Ascii.Column);
	TestEqual(TEXT("the BOM is three bytes"), Bom.Offset, Ascii.Offset + 3);
	TestEqual(TEXT("line 1"), Bom.Line, 1);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Utf8`
Expected: compile error (`VaRestJsonUtf8.h` not found). After Step 3 only (helpers exist, call sites unchanged), `ApiStrings` and `ParseAndColumns` fail (no U+FFFD, byte columns), `Repair` passes. The `ansi` assertion is not a RED: an ANSI literal already takes the validated `FUtf8StringView` branch; it pins that behaviour.

- [ ] **Step 3: Create the helpers**

`Source/VaRestJson/Public/VaRestJsonUtf8.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Public on purpose: VaRest's other modules (WebSocket, Socket.IO) take byte strings too, and spec 2.2's rule for them
// must be this one validator, not a second copy.
namespace VaRest::Json
{
	/** Scratch for one API string converted or repaired on its way into a document */
	using FUtf8Scratch = TArray<UTF8CHAR, TInlineAllocator<256>>;

	/** True when Text is well-formed UTF-8 (U+0000 included) */
	VARESTJSON_API bool IsValidUtf8(FUtf8StringView Text);

	/**
	 * Appends Text with every ill-formed subsequence replaced by one U+FFFD each (The Unicode Standard, ch. 3,
	 * "U+FFFD Substitution of Maximal Subparts", the practice WHATWG decoders follow). Returns the replacements.
	 */
	VARESTJSON_API int32 AppendRepairedUtf8(FUtf8StringView Text, FUtf8Scratch& Out);

	/** UTF-8 length of UTF-16 Text, an unpaired surrogate counted as U+FFFD (3 bytes) */
	VARESTJSON_API int64 Utf8Length(FStringView Text);

	/** Writes exactly Utf8Length(Text) bytes to Dest; returns how many unpaired surrogates became U+FFFD */
	VARESTJSON_API int32 WriteUtf8(FStringView Text, UTF8CHAR* Dest);

	/**
	 * Text as UTF-8 for a document (spec 2.2): converted into Scratch, unpaired surrogates as U+FFFD, one warning when
	 * any was replaced and bWarn is set (lookups pass false: they store nothing). Empty, with an error, past 2 GiB.
	 */
	VARESTJSON_API FUtf8StringView ConvertForDocument(FStringView Text, FUtf8Scratch& Scratch, bool bWarn = true);

	/** Text itself when it is valid UTF-8, else a repaired copy in Scratch and one warning (spec 2.2) */
	VARESTJSON_API FUtf8StringView ValidateForDocument(FUtf8StringView Text, FUtf8Scratch& Scratch);
}
```

`Source/VaRestJson/Private/VaRestJsonUtf8.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonUtf8.h"

#include "VaRestJsonLog.h"

namespace VaRest::Json
{
	// Every VaRest platform has a 16-bit TCHAR -- wchar_t on Windows, char16_t elsewhere (PLATFORM_TCHAR_IS_CHAR16:
	// UnixPlatform.h:46, AndroidPlatform.h:47, MacPlatform.h:46, IOSPlatform.h:29) -- so FString is UTF-16.
	static_assert(sizeof(TCHAR) == 2, "VaRest converts FString as UTF-16");

	namespace Utf8Private
	{
		bool IsHighSurrogate(uint32 C) { return C >= 0xD800 && C <= 0xDBFF; }
		bool IsLowSurrogate(uint32 C) { return C >= 0xDC00 && C <= 0xDFFF; }

		/**
		 * Length of the well-formed sequence at Bytes[At], or 0 when it is ill-formed; then OutBad is the length of the
		 * maximal subpart that one U+FFFD replaces (the byte that broke the sequence is examined again). Lead and second
		 * byte ranges per The Unicode Standard, Table 3-7 "Well-Formed UTF-8 Byte Sequences".
		 */
		int32 SequenceAt(const uint8* Bytes, int32 Count, int32 At, int32& OutBad)
		{
			const uint8 Lead = Bytes[At];
			if (Lead < 0x80)
			{
				return 1;
			}
			int32 Trail = 0;
			uint8 SecondLow = 0x80;
			uint8 SecondHigh = 0xBF;
			if (Lead >= 0xC2 && Lead <= 0xDF)
			{
				Trail = 1;
			}
			else if (Lead == 0xE0)
			{
				Trail = 2;
				SecondLow = 0xA0; // no overlong 3-byte forms
			}
			else if ((Lead >= 0xE1 && Lead <= 0xEC) || Lead == 0xEE || Lead == 0xEF)
			{
				Trail = 2;
			}
			else if (Lead == 0xED)
			{
				Trail = 2;
				SecondHigh = 0x9F; // no UTF-16 surrogates (U+D800..U+DFFF)
			}
			else if (Lead == 0xF0)
			{
				Trail = 3;
				SecondLow = 0x90; // no overlong 4-byte forms
			}
			else if (Lead >= 0xF1 && Lead <= 0xF3)
			{
				Trail = 3;
			}
			else if (Lead == 0xF4)
			{
				Trail = 3;
				SecondHigh = 0x8F; // nothing above U+10FFFF
			}
			else
			{
				OutBad = 1; // 80..C1 and F5..FF never start a sequence
				return 0;
			}
			for (int32 k = 1; k <= Trail; ++k)
			{
				const uint8 Low = k == 1 ? SecondLow : uint8(0x80);
				const uint8 High = k == 1 ? SecondHigh : uint8(0xBF);
				if (At + k >= Count || Bytes[At + k] < Low || Bytes[At + k] > High)
				{
					OutBad = k;
					return 0;
				}
			}
			return Trail + 1;
		}

		void AppendReplacement(FUtf8Scratch& Out)
		{
			Out.Add(static_cast<UTF8CHAR>(0xEF));
			Out.Add(static_cast<UTF8CHAR>(0xBF));
			Out.Add(static_cast<UTF8CHAR>(0xBD));
		}
	}

	bool IsValidUtf8(FUtf8StringView Text)
	{
		const uint8* Bytes = reinterpret_cast<const uint8*>(Text.GetData());
		const int32 Count = Text.Len();
		for (int32 At = 0; At < Count;)
		{
			if (Bytes[At] < 0x80)
			{
				++At;
				continue;
			}
			int32 Bad = 0;
			const int32 Length = Utf8Private::SequenceAt(Bytes, Count, At, Bad);
			if (Length == 0)
			{
				return false;
			}
			At += Length;
		}
		return true;
	}

	int32 AppendRepairedUtf8(FUtf8StringView Text, FUtf8Scratch& Out)
	{
		const uint8* Bytes = reinterpret_cast<const uint8*>(Text.GetData());
		const int32 Count = Text.Len();
		int32 Replaced = 0;
		for (int32 At = 0; At < Count;)
		{
			int32 Bad = 0;
			const int32 Length = Utf8Private::SequenceAt(Bytes, Count, At, Bad);
			if (Length > 0)
			{
				Out.Append(Text.GetData() + At, Length);
				At += Length;
			}
			else
			{
				Utf8Private::AppendReplacement(Out);
				++Replaced;
				At += Bad;
			}
		}
		return Replaced;
	}

	int64 Utf8Length(FStringView Text)
	{
		const TCHAR* Data = Text.GetData();
		const int32 Count = Text.Len();
		int64 Bytes = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			const uint32 C = static_cast<uint16>(Data[i]);
			if (C < 0x80)
			{
				Bytes += 1;
			}
			else if (C < 0x800)
			{
				Bytes += 2;
			}
			else if (Utf8Private::IsHighSurrogate(C) && i + 1 < Count && Utf8Private::IsLowSurrogate(static_cast<uint16>(Data[i + 1])))
			{
				Bytes += 4;
				++i;
			}
			else
			{
				Bytes += 3; // the rest of the BMP, or an unpaired surrogate written as U+FFFD
			}
		}
		return Bytes;
	}

	int32 WriteUtf8(FStringView Text, UTF8CHAR* Dest)
	{
		const TCHAR* Data = Text.GetData();
		const int32 Count = Text.Len();
		int32 Replaced = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			uint32 C = static_cast<uint16>(Data[i]);
			if (C < 0x80)
			{
				*Dest++ = static_cast<UTF8CHAR>(C);
			}
			else if (C < 0x800)
			{
				*Dest++ = static_cast<UTF8CHAR>(0xC0 | (C >> 6));
				*Dest++ = static_cast<UTF8CHAR>(0x80 | (C & 0x3F));
			}
			else if (Utf8Private::IsHighSurrogate(C) && i + 1 < Count && Utf8Private::IsLowSurrogate(static_cast<uint16>(Data[i + 1])))
			{
				const uint32 CodePoint = 0x10000 + ((C - 0xD800) << 10) + (static_cast<uint16>(Data[i + 1]) - 0xDC00);
				*Dest++ = static_cast<UTF8CHAR>(0xF0 | (CodePoint >> 18));
				*Dest++ = static_cast<UTF8CHAR>(0x80 | ((CodePoint >> 12) & 0x3F));
				*Dest++ = static_cast<UTF8CHAR>(0x80 | ((CodePoint >> 6) & 0x3F));
				*Dest++ = static_cast<UTF8CHAR>(0x80 | (CodePoint & 0x3F));
				++i;
			}
			else
			{
				if (C >= 0xD800 && C <= 0xDFFF)
				{
					C = 0xFFFD;
					++Replaced;
				}
				*Dest++ = static_cast<UTF8CHAR>(0xE0 | (C >> 12));
				*Dest++ = static_cast<UTF8CHAR>(0x80 | ((C >> 6) & 0x3F));
				*Dest++ = static_cast<UTF8CHAR>(0x80 | (C & 0x3F));
			}
		}
		return Replaced;
	}

	FUtf8StringView ConvertForDocument(FStringView Text, FUtf8Scratch& Scratch, bool bWarn)
	{
		const int64 Length = Utf8Length(Text);
		if (Length > MAX_int32)
		{
			UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: a string of %lld UTF-8 bytes is too long and was stored as empty"), Length);
			Scratch.Reset();
			return FUtf8StringView(Scratch.GetData(), 0);
		}
		Scratch.SetNumUninitialized(static_cast<int32>(Length));
		const int32 Replaced = WriteUtf8(Text, Scratch.GetData());
		if (Replaced > 0 && bWarn)
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d unpaired UTF-16 surrogate(s) replaced with U+FFFD"), Replaced);
		}
		return FUtf8StringView(Scratch.GetData(), Scratch.Num());
	}

	FUtf8StringView ValidateForDocument(FUtf8StringView Text, FUtf8Scratch& Scratch)
	{
		if (IsValidUtf8(Text))
		{
			return Text;
		}
		Scratch.Reset();
		const int32 Replaced = AppendRepairedUtf8(Text, Scratch);
		UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d invalid UTF-8 sequence(s) replaced with U+FFFD"), Replaced);
		return FUtf8StringView(Scratch.GetData(), Scratch.Num());
	}
}
```

- [ ] **Step 4: Call sites**

`VaRestJson.cpp`: add `#include "VaRestJsonUtf8.h"` and replace these six functions:

```cpp
FVaRestJson FVaRestJson::Get(FStringView Key) const
{
	if (!IsObject())
	{
		return Miss();
	}
	// The same conversion Set uses, so a key with an unpaired surrogate finds what Set stored; a lookup stores nothing, so no warning
	VaRest::Json::FUtf8Scratch Scratch;
	return Get(VaRest::Json::ConvertForDocument(Key, Scratch, false));
}

void FVaRestJson::SetNode(FStringView Key, VaRest::Json::FNode* Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	Doc->Set(*Node, VaRest::Json::ConvertForDocument(Key, Scratch), Value);
}

void FVaRestJson::SetNodeUtf8(FUtf8StringView Key, VaRest::Json::FNode* Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	Doc->Set(*Node, VaRest::Json::ValidateForDocument(Key, Scratch), Value);
}

bool FVaRestJson::Remove(FStringView Key)
{
	if (!IsObject())
	{
		return false;
	}
	VaRest::Json::FUtf8Scratch Scratch;
	return Remove(VaRest::Json::ConvertForDocument(Key, Scratch, false));
}

VaRest::Json::FNode* FVaRestJson::MakeUtf8String(VaRest::Json::FDocument& InDoc, FUtf8StringView Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	return InDoc.NewString(VaRest::Json::ValidateForDocument(Value, Scratch));
}

VaRest::Json::FNode* FVaRestJson::MakeString(VaRest::Json::FDocument& InDoc, FStringView Value)
{
	VaRest::Json::FUtf8Scratch Scratch;
	return InDoc.NewString(VaRest::Json::ConvertForDocument(Value, Scratch));
}
```

`VaRestJson.h`: no branch is added to `MakeNodeFor` for ANSI text. `ANSICHAR` is encoding-compatible with `UTF8CHAR` (`Traits/IsCharEncodingCompatibleWith.h:25`) and `TStringView` has a non-explicit converting constructor from any compatible character pointer (`StringView.h:139-144`), so `"plain"` and an `FAnsiStringView` already take the `FUtf8StringView` branch, which validates them. In `FVaRestJsonError` replace the `Column` member's (missing) comment by `/** 1-based, in Unicode code points from the start of the line; a leading BOM is not counted. 0 when unknown. */`. Add to the `Set` doc comment: `UTF-8 and ANSI keys and values are validated: an invalid sequence becomes U+FFFD with one warning per string; FString text maps unpaired surrogates to U+FFFD.`

`VaRestJsonText.cpp`: add `#include "VaRestJsonUtf8.h"` and replace `FVaRestJson::Parse(FStringView Text, ...)`:

```cpp
TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(FStringView Text, const FVaRestJsonParseOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::TextPrivate;
	// One conversion for the parse and for its diagnostics, so line/column always describe the bytes that were parsed
	// (M1 converted with FPlatformString::Convert here and StringCast on the error path; spec 2.2 wants U+FFFD for an
	// unpaired surrogate, the engine writes '?': GenericPlatformString.cpp:791-797).
	const int64 Utf8Len = Utf8Length(Text);
	FVaRestJsonError Error;
	if (ExceedsLimit(Utf8Len, Options, Error))
	{
		return MakeError(MoveTemp(Error));
	}
	if (Utf8Len >= MAX_int32)
	{
		Error.Message = FString::Printf(TEXT("Input of %lld UTF-8 bytes is larger than 2 GiB"), Utf8Len);
		return MakeError(MoveTemp(Error));
	}
	TArray<uint8> Bytes;
	Bytes.Reserve(static_cast<int32>(Utf8Len) + 1); // the in-situ terminator fits without a reallocation
	Bytes.AddUninitialized(static_cast<int32>(Utf8Len));
	const int32 Replaced = WriteUtf8(Text, reinterpret_cast<UTF8CHAR*>(Bytes.GetData()));
	if (Replaced > 0)
	{
		UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson::Parse: %d unpaired UTF-16 surrogate(s) replaced with U+FFFD"), Replaced);
	}

	TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseIntoDocument(MoveTemp(Bytes), Options, FUtf8StringView());
	if (!Result.HasValue() && Result.GetError().Offset >= 0)
	{
		// The in-situ parse rewrote Bytes; convert again for the diagnostics, with the very same converter
		FVaRestJsonError Diagnosed = Result.GetError();
		TArray<UTF8CHAR> Original;
		Original.SetNumUninitialized(static_cast<int32>(Utf8Len));
		WriteUtf8(Text, Original.GetData());
		FillLineColumn(Diagnosed, FUtf8StringView(Original.GetData(), Original.Num()));
		return MakeError(MoveTemp(Diagnosed));
	}
	return Result;
}
```

`VaRestJsonReader.cpp`: replace `FillLineColumn` with:

```cpp
	void FillLineColumn(FVaRestJsonError& Error, FUtf8StringView Original)
	{
		if (Original.IsEmpty())
		{
			return;
		}
		// Columns count code points (bytes that are not 10xxxxxx continuation bytes): "é" is one column, not two.
		// A leading BOM is not text and is not counted; Offset stays a byte offset into the input.
		const int64 Limit = FMath::Min<int64>(Error.Offset, Original.Len());
		const int64 Start = BomLength(reinterpret_cast<const uint8*>(Original.GetData()), Original.Len());
		int32 Line = 1;
		int32 Column = 1;
		for (int64 i = Start; i < Limit; ++i)
		{
			const uint8 Byte = static_cast<uint8>(Original[i]);
			if (Byte == '\n')
			{
				++Line;
				Column = 1;
			}
			else if ((Byte & 0xC0) != 0x80)
			{
				++Column;
			}
		}
		Error.Line = Line;
		Error.Column = Column;
	}
```

`VaRestJsonBridge.cpp`: add `#include "VaRestJsonUtf8.h"`; in `FromEngine` replace the `EJson::String` case body with

```cpp
		{
			FUtf8Scratch Scratch;
			return Doc.NewString(ConvertForDocument(Value.AsString(), Scratch));
		}
```

and in the `EJson::Object` loop replace the three lines from `const FString Key(*Pair.Key);` to the `Doc.Set(...)` call with

```cpp
					FUtf8Scratch Scratch;
					Doc.Set(*Result, ConvertForDocument(FString(*Pair.Key), Scratch), Member); // FSharedString or FString key: both dereference to TCHAR*
```

`VaRestJsonStructOps.cpp`: add `#include "VaRestJsonUtf8.h"` and replace `ImportTextItem` with:

```cpp
// Each call converts the whole remaining buffer tail to UTF-8 (O(tail) per field): fine for pin defaults,
// a known cost for large T3D arrays of handles. The resulting document keeps only the value's bytes.
bool FVaRestJson::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText)
{
	using namespace VaRest::Json;
	const TCHAR* Cursor = Buffer;
	while (FChar::IsWhitespace(*Cursor))
	{
		++Cursor;
	}
	if (*Cursor == TEXT('\0') || *Cursor == TEXT(',') || *Cursor == TEXT(')'))
	{
		*this = FVaRestJson(); // an empty field inside a parent struct's text
		Buffer = Cursor;
		return true;
	}

	// Parse a prefix of the rest of the buffer: the JSON value ends where RapidJSON stops
	// (kParseStopWhenDoneFlag), and the parent's parser continues from there. The conversion is the one every
	// other entry point uses (an unpaired surrogate becomes U+FFFD); U+FFFD converts back to one TCHAR, as the
	// surrogate was, so mapping the consumed bytes back to TCHARs below still lands on the right character.
	// No warning (bWarn = false): the rest of the buffer is engine-produced text (a pin default, a T3D blob) that may hold
	// unrelated fields after this one, and a lone surrogate there is not this field's problem.
	FUtf8Scratch Converted;
	const FUtf8StringView Utf8 = ConvertForDocument(FStringView(Cursor), Converted, false);
	// (No parameter is named Num: it would hide FVaRestJson::Num, an MSVC C4458 error.)
	const auto MakeInput = [](FDocument& Target, const UTF8CHAR* Bytes, int64 Count)
	{
		Target.Input.Reserve(static_cast<int32>(Count) + 1); // the terminator fits without a reallocation
		Target.Input.Append(reinterpret_cast<const uint8*>(Bytes), static_cast<int32>(Count));
		Target.Input.Add(0);
	};
	FDocumentRef NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
	MakeInput(*NewDoc, Utf8.GetData(), Utf8.Len());

	FVaRestJsonParseOptions Options;
	FVaRestJsonError Error;
	int64 Consumed = 0;
	if (!ParseInSitu(*NewDoc, 0, Options, Utf8, Error, &Consumed))
	{
		if (ErrorText)
		{
			ErrorText->Logf(ELogVerbosity::Warning, TEXT("FVaRestJson::ImportTextItem: %s"), *Error.ToString());
		}
		return false;
	}

	if (Consumed < Utf8.Len())
	{
		// The document would otherwise own the whole remaining text (a T3D blob, the rest of a parent
		// struct's text) for as long as the handle lives. Re-parse exactly the value's bytes into a
		// document of their own: the in-situ parse rewrote NewDoc's copy, never the converted text.
		FDocumentRef Exact = MakeShared<FDocument, ESPMode::ThreadSafe>();
		MakeInput(*Exact, Utf8.GetData(), Consumed);
		if (!ParseInSitu(*Exact, 0, Options, FUtf8StringView(Utf8.GetData(), static_cast<int32>(Consumed)), Error))
		{
			// Unreachable for a value that just parsed as a prefix; fail rather than keep the big buffer
			if (ErrorText)
			{
				ErrorText->Logf(ELogVerbosity::Warning, TEXT("FVaRestJson::ImportTextItem: %s"), *Error.ToString());
			}
			return false;
		}
		NewDoc = MoveTemp(Exact);
	}

	// Map consumed UTF-8 bytes back to TCHARs of the original buffer
	const auto Back = StringCast<TCHAR>(Utf8.GetData(), static_cast<int32>(Consumed));
	Buffer = Cursor + Back.Length();

	FNode* Root = NewDoc->Root;
	*this = FVaRestJson(MoveTemp(NewDoc), Root);
	return true;
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed` (the three new `VaRest.Json.Utf8.*` tests and every M1 test, including `StructOps.UnicodeText` and `StructOps.Text`).

- [ ] **Step 6: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: API strings enter as valid UTF-8 (U+FFFD + one warning); one UTF-16 converter; code-point columns

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 5: A missing value is undefined; Set collapses duplicate keys (spec §2.2, VaRest-4en, VaRest-tpa)

Also the VaRest-rfd item `Fresh.Set(K, Fresh)`.

**Files:**
- Modify: `Source/VaRestJson/Public/VaRestJson.h` (the two view `Set` overloads, class and `Set` doc comments)
- Modify: `Source/VaRestJson/Private/VaRestJsonNode.h` (`bDuplicateKeys`), `VaRestJsonDocument.h/.cpp` (`Set`, `AssignObject`, `Remove`, `BuildIndex`, `IndexInsert`, new `Reindex`, `DeepCopyAt`)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonSemanticsTest.cpp`

**Interfaces:**
- Consumes: Task 3's Set templates (nullptr-checked `MakeNodeFor`).
- Produces: `bool VaRest::Json::FNode::bDuplicateKeys` (objects: set at parse/build time when a key occurs twice, copied by `DeepCopy`); `bool FDocument::BuildIndex(FNode&)` (true when it met a duplicate); `void FDocument::Reindex(FNode&)` (private); `FDocument::Set` collapses duplicates, keeping the first occurrence's position.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonSemanticsTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonDocument.h"

namespace VaRestJsonSemanticsTest
{
	FVaRestJson ParseText(const UTF8CHAR* Text)
	{
		return FVaRestJson::Parse(FUtf8StringView(Text)).StealValue();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMissingIsUndefinedTest, "VaRest.Json.Semantics.MissingIsUndefined",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMissingIsUndefinedTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonSemanticsTest;
	const FVaRestJson Resp = ParseText(UTF8TEXT("{\"q\":1}"));

	FVaRestJson Body = FVaRestJson::Object();
	Body.Set(TEXT("p"), Resp[TEXT("p")]);
	TestFalse(TEXT("a missed lookup is not written (forwarding an absent field)"), Body.Has(TEXT("p")));
	Body.Set(TEXT("q"), Resp[TEXT("q")]);
	TestEqual(TEXT("a present value is copied"), Body[TEXT("q")].AsInt(), 1);
	Body.Set(TEXT("q"), FVaRestJson());
	TestFalse(TEXT("Set with a default handle removes the key"), Body.Has(TEXT("q")));
	Body.Set(UTF8TEXT("u"), 1).Set(FUtf8StringView(UTF8TEXT("u")), Resp[TEXT("nope")]);
	TestFalse(TEXT("the UTF-8 key overload removes too"), Body.Has(TEXT("u")));
	TestEqualSensitive(TEXT("no undefined key reaches the text"), Body.ToString(), FString(TEXT("{}")));

	FVaRestJson Fresh;
	Fresh.Set(TEXT("k"), Fresh);
	TestTrue(TEXT("Fresh.Set(K, Fresh) is an empty object"), Fresh.IsObject() && Fresh.Num() == 0);

	Body.Set(TEXT("n"), FVaRestJson::Null()).Set(TEXT("m"), nullptr);
	TestTrue(TEXT("explicit null is written"), Body[TEXT("n")].IsNull() && Body[TEXT("m")].IsNull());

	FVaRestJson List = FVaRestJson::Array();
	List.Add(Resp[TEXT("nope")]).Add(1);
	List.SetAt(1, FVaRestJson());
	TestEqualSensitive(TEXT("Add/SetAt write null, like JSON.stringify([undefined])"), List.ToString(), FString(TEXT("[null,null]")));

	AddExpectedErrorPlain(TEXT("cannot Set a field on a missing value"), EAutomationExpectedErrorFlags::Contains, 1);
	FVaRestJson Missed = Resp[TEXT("nope")];
	Missed.Set(TEXT("k"), Missed);
	TestFalse(TEXT("a missed lookup still refuses writes"), Missed.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonSetCollapsesTest, "VaRest.Json.Semantics.SetCollapsesDuplicates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonSetCollapsesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonSemanticsTest;
	FVaRestJson Small = ParseText(UTF8TEXT("{\"a\":1,\"b\":2,\"a\":3}"));
	TestTrue(TEXT("parse records duplicates"), Small.GetNodeInternal()->bDuplicateKeys);
	TestEqual(TEXT("lookup sees the last"), Small[TEXT("a")].AsInt(), 3);
	Small.Set(TEXT("a"), 5);
	TestEqualSensitive(TEXT("one a, where the first one was"), Small.ToString(), FString(TEXT("{\"a\":5,\"b\":2}")));
	TestEqual(TEXT("Num"), Small.Num(), 2);

	const FVaRestJson Clean = ParseText(UTF8TEXT("{\"a\":1,\"b\":2}"));
	TestFalse(TEXT("no duplicates, no flag: Set pays nothing extra"), Clean.GetNodeInternal()->bDuplicateKeys);

	FVaRestJson Wide = ParseText(UTF8TEXT("{\"k\":1,\"a\":0,\"b\":0,\"c\":0,\"d\":0,\"e\":0,\"f\":0,\"g\":0,\"h\":0,\"k\":2,\"z\":9}"));
	TestTrue(TEXT("indexed parse records duplicates"), Wide.GetNodeInternal()->bDuplicateKeys);
	Wide.Set(TEXT("k"), 7);
	TestEqual(TEXT("one k left"), Wide.Num(), 10);
	TestEqual(TEXT("k"), Wide[TEXT("k")].AsInt(), 7);
	TestEqual(TEXT("other keys are still found after the reindex"), Wide[TEXT("z")].AsInt(), 9);
	TestTrue(TEXT("k kept the first position"), Wide.ToString().StartsWith(TEXT("{\"k\":7,\"a\":0"), ESearchCase::CaseSensitive));

	FVaRestJson Copy = ParseText(UTF8TEXT("{\"x\":1,\"x\":2}")).Clone();
	TestEqualSensitive(TEXT("Clone keeps the duplicates"), Copy.ToString(), FString(TEXT("{\"x\":1,\"x\":2}")));
	Copy.Set(TEXT("x"), 3);
	TestEqualSensitive(TEXT("Set on the clone collapses them"), Copy.ToString(), FString(TEXT("{\"x\":3}")));

	FVaRestJson Nested = ParseText(UTF8TEXT("{\"o\":{\"y\":1,\"y\":2}}"));
	Nested[TEXT("o")].Set(TEXT("y"), 0);
	TestEqualSensitive(TEXT("a nested object collapses"), Nested.ToString(), FString(TEXT("{\"o\":{\"y\":0}}")));
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Semantics`
Expected: compile error on `bDuplicateKeys`. With only Step 3's node field added, both tests fail on behaviour (`a missed lookup is not written`, `one a, where the first one was`).

- [ ] **Step 3: The flag** — in `VaRestJsonNode.h`, in `struct FNode` after `ENumKind NumKind = ENumKind::Int;` add:

```cpp
		/** Objects: some key occurs more than once (only parsed or copied input can do that). Set collapses duplicates only where this is set (spec 2.2). */
		bool bDuplicateKeys = false;
```

and after the struct:

```cpp
	// Type, NumKind and bDuplicateKeys share the padding before the 8-byte-aligned union: the node stays 32 bytes
	static_assert(sizeof(FNode) == 32, "FNode is 32 bytes");
```

- [ ] **Step 4: Document** — in `VaRestJsonDocument.h` replace the `Set` and `BuildIndex` declarations and add `Reindex`:

```cpp
		/** Replace the value of Key, or append. On an object with duplicate keys the other occurrences are dropped (spec 2.2). */
		void Set(FNode& Object, FUtf8StringView Key, FNode* Value);
```

```cpp
		/** Returns true when two members share a key (the later one took the slot: last occurrence wins) */
		bool BuildIndex(FNode& Object);
```

and in `private:` change `void IndexInsert(FNode& Object, uint32 MemberIndex);` to `bool IndexInsert(FNode& Object, uint32 MemberIndex);` and add `void Reindex(FNode& Object);`.

In `VaRestJsonDocument.cpp`, inside `namespace DocumentPrivate` add:

```cpp
		/** At most IndexThreshold members: at most 28 hash comparisons */
		bool HasDuplicateKeys(const FMember* Members, uint32 Count)
		{
			for (uint32 i = 1; i < Count; ++i)
			{
				for (uint32 j = 0; j < i; ++j)
				{
					if (Members[i].Key.Hash == Members[j].Key.Hash && KeyEquals(Members[j].Key, Members[i].Key.View()))
					{
						return true;
					}
				}
			}
			return false;
		}
```

and replace `Set`, `IndexInsert`, `BuildIndex`, `Remove`, `AssignObject` with:

```cpp
	void FDocument::Set(FNode& Object, FUtf8StringView Key, FNode* Value)
	{
		const uint32 Hash = HashKey(Key);
		if (!Object.bDuplicateKeys)
		{
			const int32 Found = FindMember(Object, Key, Hash);
			if (Found != INDEX_NONE)
			{
				Object.Obj.Members[Found].Value = Value;
				return;
			}
			Append(Object, CopyStr(Key, true), Value);
			return;
		}

		// Parsed with duplicate keys (lookup sees the last). Spec 2.2: Set leaves exactly one member with this key, so
		// ToString never emits a duplicate it was asked to replace. The survivor keeps the FIRST occurrence's position,
		// where JSON.parse puts the key ({"a":1,"b":2,"a":3} becomes a:3, b:2).
		FNode::FObj& O = Object.Obj;
		int32 First = INDEX_NONE;
		uint32 Write = 0;
		for (uint32 Read = 0; Read < O.Num; ++Read)
		{
			const FMember M = O.Members[Read];
			if (M.Key.Hash == Hash && KeyEquals(M.Key, Key))
			{
				if (First != INDEX_NONE)
				{
					continue; // a later occurrence: dropped
				}
				First = static_cast<int32>(Write);
			}
			O.Members[Write++] = M;
		}
		if (First == INDEX_NONE)
		{
			Append(Object, CopyStr(Key, true), Value);
			return;
		}
		O.Members[First].Value = Value;
		if (Write != O.Num)
		{
			O.Num = Write;
			Reindex(Object);
		}
	}

	bool FDocument::IndexInsert(FNode& Object, uint32 MemberIndex)
	{
		FIndex& Index = *Object.Obj.Index;
		const FMember& M = Object.Obj.Members[MemberIndex];
		const uint32 Mask = Index.Capacity - 1;
		for (uint32 Slot = M.Key.Hash & Mask;; Slot = (Slot + 1) & Mask)
		{
			uint32& Entry = Index.Slots[Slot];
			if (Entry == 0)
			{
				Entry = MemberIndex + 1;
				return false;
			}
			const FMember& Other = Object.Obj.Members[Entry - 1];
			if (Other.Key.Hash == M.Key.Hash && KeyEquals(Other.Key, M.Key.View()))
			{
				Entry = MemberIndex + 1; // a later duplicate takes the slot: last occurrence wins
				return true;
			}
		}
	}

	bool FDocument::BuildIndex(FNode& Object)
	{
		FNode::FObj& O = Object.Obj;
		const uint32 Capacity = FMath::RoundUpToPowerOfTwo(FMath::Max<uint32>(16, O.Num * 2));
		FIndex* Index = Arena.New<FIndex>();
		Index->Capacity = Capacity;
		Index->Slots = Arena.NewArray<uint32>(Capacity);
		FMemory::Memzero(Index->Slots, sizeof(uint32) * Capacity);
		O.Index = Index;
		bool bDuplicates = false;
		for (uint32 i = 0; i < O.Num; ++i)
		{
			if (IndexInsert(Object, i))
			{
				bDuplicates = true;
			}
		}
		return bDuplicates;
	}

	void FDocument::Reindex(FNode& Object)
	{
		// Members shifted, so every index slot after the first removal is stale. The arena never frees, so the existing
		// slot array is cleared and refilled while it is large enough instead of allocating a new one.
		FNode::FObj& O = Object.Obj;
		if (O.Num <= IndexThreshold)
		{
			O.Index = nullptr;
		}
		else if (O.Index && O.Index->Capacity >= O.Num * 2)
		{
			FMemory::Memzero(O.Index->Slots, sizeof(uint32) * O.Index->Capacity);
			for (uint32 i = 0; i < O.Num; ++i)
			{
				IndexInsert(Object, i);
			}
		}
		else
		{
			BuildIndex(Object);
		}
	}

	int32 FDocument::Remove(FNode& Object, FUtf8StringView Key)
	{
		check(Object.Type == EVaRestJsonType::Object);
		FNode::FObj& O = Object.Obj;
		const uint32 Hash = HashKey(Key);
		uint32 Write = 0;
		for (uint32 Read = 0; Read < O.Num; ++Read)
		{
			const FMember& M = O.Members[Read];
			if (!(M.Key.Hash == Hash && KeyEquals(M.Key, Key)))
			{
				O.Members[Write++] = M;
			}
		}
		const int32 Removed = static_cast<int32>(O.Num - Write);
		O.Num = Write;
		if (Removed > 0)
		{
			Reindex(Object);
		}
		return Removed;
	}

	void FDocument::AssignObject(FNode& Object, TConstArrayView<FMember> Members)
	{
		const uint32 Num = static_cast<uint32>(Members.Num());
		FMember* Data = Arena.NewArray<FMember>(Num);
		if (Num)
		{
			FMemory::Memcpy(Data, Members.GetData(), sizeof(FMember) * Num);
		}
		Object.Obj = FNode::FObj{ Data, Num, Num, nullptr };
		// Recorded once, here at parse time, so Set pays for duplicate handling only on objects that have duplicates
		// (spec 2.2): above IndexThreshold the index build sees every collision, below it a pairwise check does.
		Object.bDuplicateKeys = Num > IndexThreshold ? BuildIndex(Object) : DocumentPrivate::HasDuplicateKeys(Data, Num);
	}
```

(`Set`'s loop copies `M` by value: `O.Members[Write++] = M` may overwrite the slot a reference would still read.)

In `DeepCopyAt`'s `Object` case, after `Copy->Obj.Num = Count;` add `Copy->bDuplicateKeys = Src.bDuplicateKeys;`.

`Append(FNode& Object, const FStr& Key, FNode* Value)` never sets `bDuplicateKeys`, so it must only add a key that is not there. In `VaRestJsonDocument.h` replace its comment (`the parser keeps them`, stale: the parser uses `AssignObject`) with `/** Append a member whose key is not present (callers: Set after a miss), so bDuplicateKeys stays true to the object */`, and in `VaRestJsonDocument.cpp` add after its `check(...)`:

```cpp
		checkSlow(FindMember(Object, Key.View(), Key.Hash) == INDEX_NONE); // the bDuplicateKeys invariant
```

- [ ] **Step 5: Missing = undefined in `VaRestJson.h`** — replace the two view `Set` `&` overloads (Task 3's text) with:

```cpp
	template <typename ValueType>
	FVaRestJson& Set(FStringView Key, ValueType&& Value) &
	{
		if constexpr (std::is_same_v<std::decay_t<ValueType>, FVaRestJson>)
		{
			if (!Value.IsValid())
			{
				// Spec 2.2: a missing value is undefined, not null -- the key is removed. Checked before PrepareContainer,
				// which may replace *this, because Value may be *this (Fresh.Set(K, Fresh)).
				if (PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
				{
					Remove(Key);
				}
				return *this;
			}
		}
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			// nullptr: copying a value nested deeper than MaxDepth failed (logged); nothing is written
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetNode(Key, NewValue);
			}
		}
		return *this;
	}
```

```cpp
	/** UTF-8 key: no TCHAR -> UTF-8 conversion on hot paths */
	template <typename ValueType>
	FVaRestJson& Set(FUtf8StringView Key, ValueType&& Value) &
	{
		if constexpr (std::is_same_v<std::decay_t<ValueType>, FVaRestJson>)
		{
			if (!Value.IsValid())
			{
				if (PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
				{
					Remove(Key);
				}
				return *this;
			}
		}
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			if (VaRest::Json::FNode* NewValue = MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)))
			{
				SetNodeUtf8(Key, NewValue);
			}
		}
		return *this;
	}
```

Update the comments: in the class comment replace `A default-constructed handle (no document) auto-vivifies on its first Set/Add.` with `A default-constructed handle (no document) auto-vivifies on its first Set/Add. A missing value (a miss or a default handle) passed as a VALUE is undefined (spec 2.2): Set removes the key, Add/SetAt write null.`; in the `Set` doc comment add the paragraph: `A missing Value is undefined, not null (spec 2.2): the key is removed, the way obj.k = undefined disappears from JSON.stringify, so Body.Set("p", Resp["p"]) forwards p only when it exists. Explicit null is FVaRestJson::Null() or nullptr. On an object parsed with Key more than once, Set leaves one member with that key, at the first occurrence's position.` If an M1 test asserted that `Set(Key, Missing)` writes `null`, change it to the ruling and say so in the report.

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`.

- [ ] **Step 7: Restore-the-bug (missing = undefined; duplicate collapse)**

(a) Delete the `if constexpr (...) { ... }` block from the `FStringView` `Set` overload; run `Tools/run_tests.sh VaRest.Json.Semantics`; expect `MissingIsUndefined` to fail (`a missed lookup is not written`). Restore.
(b) In `FDocument::Set` replace `if (!Object.bDuplicateKeys)` with `if (true)`; run again; expect `SetCollapsesDuplicates` to fail (`one a, where the first one was`). Restore, run, expect green. Report all outcomes.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: a missing value is undefined (Set removes, Add/SetAt write null); Set collapses parsed duplicate keys

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 6: Field names, naming policies, the field-map cache and renames (spec §2.4)

**Engine facts:** a Blueprint struct field's property name is `<Name>_<n>_<32-hex GUID>` (`StructureEditorUtils.cpp:234-262`). `UUserDefinedStruct::GetAuthoredNameForField` returns the editor-only friendly name in editor builds (`UserDefinedStruct.cpp:289-298`) and strips the suffix in cooked builds (`:300-313`); VaRest always runs the cooked algorithm. `FJsonObjectConverter` skips `CPF_Deprecated | CPF_Transient` by default (`JsonObjectConverter.cpp:407-411`). `FObjectKey` holds an object index plus serial number (`ObjectKey.h:29-38`), so a key never matches a later object that reuses the address. `TBaseStructure<T>::Get()` exists only for USTRUCTs and the specialised core types (`UObject/Class.h:5443-5463`).

**Files:**
- Create: `Source/VaRestJson/Public/VaRestJsonConvert.h`
- Create: `Source/VaRestJson/Private/VaRestJsonConvertPrivate.h`, `Source/VaRestJson/Private/VaRestJsonConvertNames.cpp`
- Create: `Source/VaRestJson/Private/Tests/VaRestJsonConvertTestTypes.h`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonConvertNamesTest.cpp`

**Interfaces:**
- Consumes: `VaRest::Json::HashKey`, `FUtf8Scratch`, `ConvertForDocument` (Task 4).
- Produces (public): `enum class EVaRestJsonNaming : uint8 { AsIs, CamelCase, SnakeCase, PascalCase }` (UENUM, BlueprintType); `struct FVaRestJsonConvertOptions { EVaRestJsonNaming Naming = CamelCase; bool bStrict = false; bool bEnumsAsNumbers = false; bool bBytesAsBase64 = false; }` (Task 8 adds `BytesResolver`); `struct FVaRestJsonDecodeReport { TArray<FVaRestJsonError> Warnings, Errors; }`; `struct FVaRestJsonRename { FName Field; FString Json; }`; `FVaRestJsonNames::Register(const UScriptStruct*, TConstArrayView<FVaRestJsonRename>)`, `Register<T>(std::initializer_list<FVaRestJsonRename>)`, `Unregister(const UScriptStruct*)`; `FVaRestJsonConvert::ResetCache()`, `FVaRestJsonConvert::NumCachedFieldMapsInternal()`.
- Produces (private, `VaRest::Json::Convert`): `FFieldInfo { const FProperty* Property; TArray<UTF8CHAR> Name, Folded, Normalized; FUtf8StringView NameView() const; }`; `FFieldMap { TArray<FFieldInfo> Fields; TMultiMap<uint32,int32> ByName, ByFolded, ByNormalized; int32 Match(FUtf8StringView Key, uint32 KeyHash, FUtf8Scratch& Scratch, uint8& OutTier) const; }`; `using FFieldMapRef = TSharedRef<const FFieldMap, ESPMode::ThreadSafe>`; `FFieldMapRef GetFieldMap(const UScriptStruct*, EVaRestJsonNaming)`; `FString StripGuidSuffix(const FString&)`; `FString AuthoredFieldName(const UScriptStruct* Owner, const FProperty*)`; `FString ApplyNaming(const FString& AuthoredName, bool bBoolField, EVaRestJsonNaming)`; `void FoldUtf8(FUtf8StringView, FUtf8Scratch&)`; `void NormalizeUtf8(FUtf8StringView, FUtf8Scratch&)`; `bool IsSupported(const FProperty*, FString& OutWhy)`; `enum class ESpecialStruct : uint8 { None, Json, DateTime, Timespan, Guid, SoftObjectPath }`; `ESpecialStruct ClassifyStruct(const UScriptStruct*)`; `bool IsUnsignedInteger(const FNumericProperty*)`; `bool IsPlainByteArray(const FArrayProperty*)`.
- Test types (`VaRestJsonConvertTestTypes.h`): `FVaRestJsonTestInner { int32 Id; FString Name; }`, `FVaRestJsonTestNaming { int32 UserId; bool bIsActive; FString URLPath; int32 HTTPStatus2; FString user_name; UPROPERTY(Transient) int32 TransientValue; }`, `FVaRestJsonTestCollision { int32 user_id = 1; int32 UserId = 2; }`, `FVaRestJsonTestSkipped { int32 Kept; TObjectPtr<UObject> ObjectRef; TMap<FGuid, int32> GuidKeys; TArray<TObjectPtr<UObject>> Objects; }`.

Decisions this task makes (the spec leaves them open): camelCase/snake_case/PascalCase split words at `_`/`-`/space, at lower→upper and digit→upper changes, and at the end of an acronym (`URLPath` → URL|Path), and lowercase the rest of each word (`UserID` → `userId`); a bool's UE `b` prefix (`bIsActive`) is dropped by every policy except `AsIs`; deprecated, transient and editor-only (`CPF_EditorOnly`, absent from a cooked game) properties are not converted; a map built while the cache was reset or a rename registered is used once and not cached (an epoch counter); a field whose JSON name repeats an earlier field's is skipped with a warning; a spelling two fields share is left out of the tolerant tables (only an exact key reaches either); a registered rename is the exact key under every policy.

- [ ] **Step 1: Test types** — `Source/VaRestJson/Private/Tests/VaRestJsonConvertTestTypes.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

#include "VaRestJsonConvertTestTypes.generated.h"

// Test-only types. Like VaRestJsonTestTypes.h they ship in the module's reflection data until the VaRest-rfd
// M3 item moves test types out of the runtime module.

USTRUCT()
struct FVaRestJsonTestInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	FString Name;
};

USTRUCT()
struct FVaRestJsonTestNaming
{
	GENERATED_BODY()

	UPROPERTY()
	int32 UserId = 0;

	UPROPERTY()
	bool bIsActive = false;

	UPROPERTY()
	FString URLPath;

	UPROPERTY()
	int32 HTTPStatus2 = 0;

	UPROPERTY()
	FString user_name;

	UPROPERTY(Transient)
	int32 TransientValue = 0;
};

/** Both fields become "userId" in camelCase: the second is skipped */
USTRUCT()
struct FVaRestJsonTestCollision
{
	GENERATED_BODY()

	UPROPERTY()
	int32 user_id = 1;

	UPROPERTY()
	int32 UserId = 2;
};

/** Fields encode/decode do not convert: each is skipped with one warning per field-map build */
USTRUCT()
struct FVaRestJsonTestSkipped
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Kept = 0;

	UPROPERTY()
	TObjectPtr<UObject> ObjectRef;

	UPROPERTY()
	TMap<FGuid, int32> GuidKeys;

	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;
};
```

- [ ] **Step 2: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonConvertNamesTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonConvertTestTypes.h"

namespace VaRestJsonConvertNamesTest
{
	FString NameOf(const VaRest::Json::Convert::FFieldInfo& Field)
	{
		return FString::ConstructFromPtrSize(Field.Name.GetData(), Field.Name.Num());
	}

	FString Names(const VaRest::Json::Convert::FFieldMapRef& Map)
	{
		TArray<FString> Result;
		for (const VaRest::Json::Convert::FFieldInfo& Field : Map->Fields)
		{
			Result.Add(NameOf(Field));
		}
		return FString::Join(Result, TEXT(","));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertNamingTest, "VaRest.Json.Convert.Naming",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertNamingTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::Convert::ApplyNaming;
	struct FCase
	{
		const TCHAR* Authored;
		bool bBool;
		const TCHAR* AsIs;
		const TCHAR* Camel;
		const TCHAR* Snake;
		const TCHAR* Pascal;
	};
	const FCase Cases[] = {
		{ TEXT("UserId"),      false, TEXT("UserId"),      TEXT("userId"),      TEXT("user_id"),      TEXT("UserId") },
		{ TEXT("UserID"),      false, TEXT("UserID"),      TEXT("userId"),      TEXT("user_id"),      TEXT("UserId") },
		{ TEXT("URLPath"),     false, TEXT("URLPath"),     TEXT("urlPath"),     TEXT("url_path"),     TEXT("UrlPath") },
		{ TEXT("bIsActive"),   true,  TEXT("bIsActive"),   TEXT("isActive"),    TEXT("is_active"),    TEXT("IsActive") },
		{ TEXT("bIsActive"),   false, TEXT("bIsActive"),   TEXT("bIsActive"),   TEXT("b_is_active"),  TEXT("BIsActive") },
		{ TEXT("HTTPStatus2"), false, TEXT("HTTPStatus2"), TEXT("httpStatus2"), TEXT("http_status2"), TEXT("HttpStatus2") },
		{ TEXT("Vector3D"),    false, TEXT("Vector3D"),    TEXT("vector3D"),    TEXT("vector3_d"),    TEXT("Vector3D") },
		{ TEXT("user_name"),   false, TEXT("user_name"),   TEXT("userName"),    TEXT("user_name"),    TEXT("UserName") },
		{ TEXT("X"),           false, TEXT("X"),           TEXT("x"),           TEXT("x"),            TEXT("X") },
		{ TEXT("ID"),          false, TEXT("ID"),          TEXT("id"),          TEXT("id"),           TEXT("Id") },
		{ TEXT("maxHP"),       false, TEXT("maxHP"),       TEXT("maxHp"),       TEXT("max_hp"),       TEXT("MaxHp") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("%s AsIs"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::AsIs)), FStringView(Case.AsIs));
		TestEqualSensitive(FString::Printf(TEXT("%s camelCase"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::CamelCase)), FStringView(Case.Camel));
		TestEqualSensitive(FString::Printf(TEXT("%s snake_case"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::SnakeCase)), FStringView(Case.Snake));
		TestEqualSensitive(FString::Printf(TEXT("%s PascalCase"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::PascalCase)), FStringView(Case.Pascal));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertUserStructNameTest, "VaRest.Json.Convert.UserStructNameAlgorithm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertUserStructNameTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::Convert::StripGuidSuffix;
	// The cooked-build algorithm of UUserDefinedStruct::GetAuthoredNameForField (UserDefinedStruct.cpp:300-313)
	TestEqualSensitive(TEXT("Name_<n>_<guid>"), StripGuidSuffix(TEXT("Score_3_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("Score")));
	TestEqualSensitive(TEXT("underscores inside the name survive"), StripGuidSuffix(TEXT("My_Var_12_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("My_Var")));
	TestEqualSensitive(TEXT("a short (C++) name is untouched"), StripGuidSuffix(TEXT("Score")), FString(TEXT("Score")));
	TestEqualSensitive(TEXT("35 characters or fewer are untouched"), StripGuidSuffix(TEXT("A_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("A_0123456789ABCDEF0123456789ABCDEF")));
	TestEqualSensitive(TEXT("no '_' after position 0 keeps the whole name"), StripGuidSuffix(TEXT("_12_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("_12_0123456789ABCDEF0123456789ABCDEF")));
	TestEqualSensitive(TEXT("C++ struct fields use the property name"),
		VaRest::Json::Convert::AuthoredFieldName(FVaRestJsonTestNaming::StaticStruct(), FindFProperty<FProperty>(FVaRestJsonTestNaming::StaticStruct(), TEXT("URLPath"))),
		FString(TEXT("URLPath")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertFieldMapTest, "VaRest.Json.Convert.FieldMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertFieldMapTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRestJsonConvertNamesTest;
	FVaRestJsonConvert::ResetCache();

	const FFieldMapRef Camel = GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqualSensitive(TEXT("camelCase names, transient skipped"), Names(Camel), FString(TEXT("userId,isActive,urlPath,httpStatus2,userName")));
	TestEqualSensitive(TEXT("snake_case names"), Names(GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::SnakeCase)), FString(TEXT("user_id,is_active,url_path,http_status2,user_name")));
	TestEqualSensitive(TEXT("AsIs names"), Names(GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::AsIs)), FString(TEXT("UserId,bIsActive,URLPath,HTTPStatus2,user_name")));
	TestTrue(TEXT("a second lookup returns the cached map"), &GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::CamelCase).Get() == &Camel.Get());
	TestEqual(TEXT("one map per (struct, policy)"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 3);

	// Review Focus: two fields with one JSON name -- the later one is skipped, so no key is written twice
	AddExpectedMessagePlain(TEXT("is already used by another field"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FFieldMapRef Collision = GetFieldMap(FVaRestJsonTestCollision::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqual(TEXT("one field survives"), Collision->Fields.Num(), 1);
	TestTrue(TEXT("the first one"), Collision->Fields.Num() == 1 && Collision->Fields[0].Property->GetFName() == FName(TEXT("user_id")));

	// Unsupported fields: one warning each per map build (the struct's reflected name has no 'F')
	AddExpectedMessagePlain(TEXT("VaRestJsonTestSkipped."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	const FFieldMapRef Skipped = GetFieldMap(FVaRestJsonTestSkipped::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqualSensitive(TEXT("only Kept is converted"), Names(Skipped), FString(TEXT("kept")));

	// Decode matching tiers
	FUtf8Scratch Scratch;
	uint8 Tier = MAX_uint8;
	const auto MatchKey = [&Camel, &Scratch, &Tier](const UTF8CHAR* Key)
	{
		const FUtf8StringView KeyView(Key);
		return Camel->Match(KeyView, HashKey(KeyView), Scratch, Tier);
	};
	TestEqual(TEXT("exact"), MatchKey(UTF8TEXT("userId")), 0);
	TestEqual(TEXT("exact is tier 0"), static_cast<int32>(Tier), 0);
	TestEqual(TEXT("case-insensitive"), MatchKey(UTF8TEXT("USERID")), 0);
	TestEqual(TEXT("case-insensitive is tier 1"), static_cast<int32>(Tier), 1);
	TestEqual(TEXT("normalised"), MatchKey(UTF8TEXT("user_id")), 0);
	TestEqual(TEXT("normalised is tier 2"), static_cast<int32>(Tier), 2);
	TestEqual(TEXT("a bool without its b"), MatchKey(UTF8TEXT("is_active")), 1);
	TestEqual(TEXT("unknown key"), MatchKey(UTF8TEXT("nope")), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertCacheTest, "VaRest.Json.Convert.FieldMapCacheConcurrent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertCacheTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::Convert;
	FVaRestJsonConvert::ResetCache();
	TestEqual(TEXT("reset empties the cache"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 0);

	// Decode runs off the game thread: many threads asking for the same maps at once must end with one map per key
	const UScriptStruct* Structs[] = { FVaRestJsonTestNaming::StaticStruct(), FVaRestJsonTestInner::StaticStruct() };
	const EVaRestJsonNaming Policies[] = { EVaRestJsonNaming::AsIs, EVaRestJsonNaming::CamelCase, EVaRestJsonNaming::SnakeCase, EVaRestJsonNaming::PascalCase };
	TArray<const FFieldMap*> Seen;
	Seen.SetNumZeroed(64 * 8);
	ParallelFor(64, [&Seen, &Structs, &Policies](int32 Worker)
	{
		for (int32 k = 0; k < 8; ++k)
		{
			Seen[Worker * 8 + k] = &GetFieldMap(Structs[k % 2], Policies[k / 2]).Get();
		}
	});
	TestEqual(TEXT("eight maps"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 8);
	int32 Strays = 0;
	for (int32 i = 0; i < Seen.Num(); ++i)
	{
		const int32 k = i % 8;
		Strays += Seen[i] != &GetFieldMap(Structs[k % 2], Policies[k / 2]).Get();
	}
	TestEqual(TEXT("every thread got the cached map"), Strays, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertRenamesTest, "VaRest.Json.Convert.Renames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertRenamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::Convert;
	using namespace VaRestJsonConvertNamesTest;
	FVaRestJsonNames::Register<FVaRestJsonTestInner>({ { TEXT("Id"), TEXT("user_id") } });
	const FFieldMapRef Renamed = GetFieldMap(FVaRestJsonTestInner::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqualSensitive(TEXT("the rename wins over the policy"), Names(Renamed), FString(TEXT("user_id,name")));
	FVaRestJsonNames::Unregister(FVaRestJsonTestInner::StaticStruct());
	TestEqualSensitive(TEXT("unregistered: the policy again"), Names(GetFieldMap(FVaRestJsonTestInner::StaticStruct(), EVaRestJsonNaming::CamelCase)), FString(TEXT("id,name")));
	return true;
}

#endif
```

- [ ] **Step 3: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Convert`
Expected: compile error (`VaRestJsonConvert.h` not found).

- [ ] **Step 4: Public header** — `Source/VaRestJson/Public/VaRestJsonConvert.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "VaRestJson.h"

#include <initializer_list>

#include "VaRestJsonConvert.generated.h"

class FProperty;

/** How struct field names become JSON keys (spec 2.4). Decode accepts every policy's spelling regardless. */
UENUM(BlueprintType)
enum class EVaRestJsonNaming : uint8
{
	/** The authored field name unchanged: the C++ property name, or a Blueprint struct field's name */
	AsIs,
	/** userId (the default) */
	CamelCase,
	/** user_id */
	SnakeCase,
	/** UserId */
	PascalCase
};

struct FVaRestJsonConvertOptions
{
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;

	/** Decode: lenient coercions ("42" -> 42, 1 -> true) are refused and every mismatch is an error */
	bool bStrict = false;

	/** Encode enums as their numeric value instead of the enumerator name (decode accepts both) */
	bool bEnumsAsNumbers = false;

	/** Encode TArray<uint8> as one base64 string instead of an array of numbers (decode accepts both) */
	bool bBytesAsBase64 = false;
};

/** What a decode found, each entry with the JSON path of the value ("items[3].name"). Strict mode reports Errors. */
struct FVaRestJsonDecodeReport
{
	TArray<FVaRestJsonError> Warnings;
	TArray<FVaRestJsonError> Errors;
};

/** A C++ field rename: Field is the property name, Json the key it is written as (no naming policy applies to it) */
struct FVaRestJsonRename
{
	FName Field;
	FString Json;
};

/**
 * C++ per-field renames (spec 2.4): FVaRestJsonNames::Register<FMyStruct>({ { TEXT("UserId"), TEXT("user_id") } }).
 * UPROPERTY meta is editor-only data, so names cannot come from meta in a cooked game; register at startup instead.
 */
struct VARESTJSON_API FVaRestJsonNames
{
	static void Register(const UScriptStruct* Struct, TConstArrayView<FVaRestJsonRename> Renames);

	template <typename T>
	static void Register(std::initializer_list<FVaRestJsonRename> Renames)
	{
		Register(TBaseStructure<T>::Get(), TConstArrayView<FVaRestJsonRename>(Renames.begin(), static_cast<int32>(Renames.size())));
	}

	static void Unregister(const UScriptStruct* Struct);
};

/**
 * Struct <-> JSON (spec 2.4). Every function may run on any thread. The struct's reflection must not change meanwhile:
 * a Blueprint struct recompiled in the editor while a worker thread decodes into it frees the FProperty objects that
 * decode is reading (UserDefinedStructureCompilerUtils.cpp:221-225), and dropping the cache cannot help a decode
 * already in flight. Any reflection user shares that hazard; it exists only in the editor.
 */
struct VARESTJSON_API FVaRestJsonConvert
{
	/** Drops every cached field map. The editor calls it when a Blueprint struct changes; FVaRestJsonNames calls it too. */
	static void ResetCache();

	/** For tests: how many (struct, naming policy) field maps are cached */
	static int32 NumCachedFieldMapsInternal();
};
```

- [ ] **Step 5: Private header** — `Source/VaRestJson/Private/VaRestJsonConvertPrivate.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonNode.h"
#include "VaRestJsonUtf8.h"

namespace VaRest::Json::Convert
{
	/** One convertible field of a struct, with the three spellings decode matches a JSON key against (spec 2.4) */
	struct FFieldInfo
	{
		const FProperty* Property = nullptr;
		/** The key encode writes: the naming policy (or a registered rename) applied to the authored name, UTF-8 */
		TArray<UTF8CHAR> Name;
		/** Name with ASCII letters lowered: the case-insensitive spelling */
		TArray<UTF8CHAR> Folded;
		/** Lowered, without '_', '-', ' ' and without a bool's 'b': user_id ~ UserId ~ userId */
		TArray<UTF8CHAR> Normalized;

		FUtf8StringView NameView() const { return FUtf8StringView(Name.GetData(), Name.Num()); }
	};

	/** The convertible fields of one struct under one naming policy. Immutable once built, so any thread may read it. */
	struct FFieldMap
	{
		TArray<FFieldInfo> Fields;
		/** Key hash (HashKey: the hash parsed object keys already carry) -> field index, per spelling */
		TMultiMap<uint32, int32> ByName;
		TMultiMap<uint32, int32> ByFolded;
		TMultiMap<uint32, int32> ByNormalized;

		/** The field a JSON key feeds: exact (OutTier 0), then case-insensitive (1), then normalised (2); INDEX_NONE when none */
		int32 Match(FUtf8StringView Key, uint32 KeyHash, FUtf8Scratch& Scratch, uint8& OutTier) const;
	};

	using FFieldMapRef = TSharedRef<const FFieldMap, ESPMode::ThreadSafe>;

	/** Cached per (struct, naming policy), built on first use from any thread */
	FFieldMapRef GetFieldMap(const UScriptStruct* Struct, EVaRestJsonNaming Naming);

	/** The cooked-build Blueprint struct field name: "Score_3_<32 hex>" -> "Score" (UserDefinedStruct.cpp:300-313) */
	FString StripGuidSuffix(const FString& PropertyName);

	/** The name the policy applies to: the property name, or for a Blueprint struct the stripped name (never the editor-only friendly one) */
	FString AuthoredFieldName(const UScriptStruct* Owner, const FProperty* Property);

	FString ApplyNaming(const FString& AuthoredName, bool bBoolField, EVaRestJsonNaming Naming);

	/** ASCII letters lowered into Out (reset first) */
	void FoldUtf8(FUtf8StringView Text, FUtf8Scratch& Out);

	/** FoldUtf8 without '_', '-' and ' ' */
	void NormalizeUtf8(FUtf8StringView Text, FUtf8Scratch& Out);

	/** Whether encode/decode convert this property; OutWhy says why not */
	bool IsSupported(const FProperty* Property, FString& OutWhy);

	/** Structs with a JSON form of their own instead of an object of fields */
	enum class ESpecialStruct : uint8
	{
		None,
		/** FVaRestJson: raw passthrough */
		Json,
		/** ISO 8601 string */
		DateTime,
		/** seconds, a number */
		Timespan,
		/** DigitsWithHyphens string */
		Guid,
		/** FSoftObjectPath / FSoftClassPath: the path string */
		SoftObjectPath
	};

	ESpecialStruct ClassifyStruct(const UScriptStruct* Struct);

	bool IsUnsignedInteger(const FNumericProperty* Property);

	/** TArray<uint8> without an enum: the one array that may be base64 */
	bool IsPlainByteArray(const FArrayProperty* Property);
}
```

- [ ] **Step 6: Implementation** — `Source/VaRestJson/Private/VaRestJsonConvertNames.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvertPrivate.h"

#include "Misc/ScopeRWLock.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectKey.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "VaRestJsonLog.h"

namespace VaRest::Json::Convert
{
	namespace NamesPrivate
	{
		/**
		 * The process-wide field-map cache and rename registry. Keys are FObjectKey (index + serial number,
		 * ObjectKey.h:29-38), so a struct that dies and a new one at the same address never share a map. Reads take the
		 * read lock, so concurrent decodes on worker threads share maps; maps are immutable once inserted.
		 */
		struct FCache
		{
			FRWLock Lock;
			/**
			 * Bumped by ResetCache, Register and Unregister. Build runs outside the lock, so a map built from the
			 * renames (or struct) of before a reset could otherwise be inserted after it and outlive it.
			 */
			uint64 Epoch = 0;
			TMap<TPair<FObjectKey, uint8>, FFieldMapRef> Maps;
			TMap<FObjectKey, TMap<FName, FString>> Renames;
		};

		FCache& Cache()
		{
			static FCache Instance; // a function-local static: its first use is thread-safe
			return Instance;
		}

		bool IsSeparator(TCHAR C)
		{
			return C == TEXT('_') || C == TEXT('-') || C == TEXT(' ');
		}

		/** "URLPath" -> URL|Path, "UserId" -> User|Id, "user_id" -> user|id, "HTTPStatus2" -> HTTP|Status2, "Vector3D" -> Vector3|D */
		TArray<FString> SplitWords(const FString& Name)
		{
			TArray<FString> Words;
			FString Current;
			for (int32 i = 0; i < Name.Len(); ++i)
			{
				const TCHAR C = Name[i];
				if (IsSeparator(C))
				{
					if (!Current.IsEmpty())
					{
						Words.Add(Current);
						Current.Reset();
					}
					continue;
				}
				if (!Current.IsEmpty())
				{
					const TCHAR Prev = Current[Current.Len() - 1];
					const bool bLowerToUpper = (FChar::IsLower(Prev) || FChar::IsDigit(Prev)) && FChar::IsUpper(C);
					const bool bAcronymEnds = FChar::IsUpper(Prev) && FChar::IsUpper(C) && i + 1 < Name.Len() && FChar::IsLower(Name[i + 1]);
					if (bLowerToUpper || bAcronymEnds)
					{
						Words.Add(Current);
						Current.Reset();
					}
				}
				Current.AppendChar(C);
			}
			if (!Current.IsEmpty())
			{
				Words.Add(Current);
			}
			return Words;
		}

		FString Capitalized(const FString& Word)
		{
			FString Result = Word.ToLower();
			if (Result.Len() > 0)
			{
				Result[0] = FChar::ToUpper(Result[0]);
			}
			return Result;
		}

		FUtf8StringView View(const TArray<UTF8CHAR>& Bytes)
		{
			return FUtf8StringView(Bytes.GetData(), Bytes.Num());
		}

		void AssignUtf8(TArray<UTF8CHAR>& Out, const FString& Text)
		{
			FUtf8Scratch Scratch;
			const FUtf8StringView Utf8 = ConvertForDocument(Text, Scratch);
			Out.Reset(Utf8.Len());
			Out.Append(Utf8.GetData(), Utf8.Len());
		}

		int32 FindIn(const TMultiMap<uint32, int32>& Table, const TArray<FFieldInfo>& Fields, TArray<UTF8CHAR> FFieldInfo::* Spelling, FUtf8StringView Key, uint32 Hash)
		{
			for (auto It = Table.CreateConstKeyIterator(Hash); It; ++It)
			{
				const TArray<UTF8CHAR>& Candidate = Fields[It.Value()].*Spelling;
				if (Candidate.Num() == Key.Len() && (Key.Len() == 0 || FMemory::Memcmp(Candidate.GetData(), Key.GetData(), Key.Len()) == 0))
				{
					return It.Value();
				}
			}
			return INDEX_NONE;
		}

		/** A spelling two fields share stays out of the tolerant tables: only an exact key reaches either field */
		void AddUnambiguous(TMultiMap<uint32, int32>& Table, const TArray<FFieldInfo>& Fields, TArray<UTF8CHAR> FFieldInfo::* Spelling)
		{
			for (int32 i = 0; i < Fields.Num(); ++i)
			{
				const FUtf8StringView Mine = View(Fields[i].*Spelling);
				bool bShared = false;
				for (int32 j = 0; j < Fields.Num() && !bShared; ++j)
				{
					bShared = j != i && View(Fields[j].*Spelling).Equals(Mine, ESearchCase::CaseSensitive);
				}
				if (!bShared)
				{
					Table.Add(HashKey(Mine), i);
				}
			}
		}

		/** OutEpoch: the cache epoch the renames were read at (read together, under one lock) */
		FFieldMapRef Build(const UScriptStruct* Struct, EVaRestJsonNaming Naming, uint64& OutEpoch)
		{
			TMap<FName, FString> StructRenames;
			{
				FReadScopeLock ReadLock(Cache().Lock);
				OutEpoch = Cache().Epoch;
				if (const TMap<FName, FString>* Found = Cache().Renames.Find(FObjectKey(Struct)))
				{
					StructRenames = *Found;
				}
			}

			const TSharedRef<FFieldMap, ESPMode::ThreadSafe> Map = MakeShared<FFieldMap, ESPMode::ThreadSafe>();
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty* Property = *It;
				// The flags FJsonObjectConverter skips by default (JsonObjectConverter.cpp:407-411), plus editor-only data:
				// such a field exists in PIE and not in a cooked game, the divergence spec 2.4 rules out for BP struct names
				if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_EditorOnly))
				{
					continue;
				}
				FString Why;
				if (!IsSupported(Property, Why))
				{
					UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %s.%s is skipped: %s"), *Struct->GetName(), *Property->GetName(), *Why);
					continue;
				}
				const bool bBool = Property->IsA<FBoolProperty>();
				const FString Authored = AuthoredFieldName(Struct, Property);
				const FString* Renamed = StructRenames.Find(Property->GetFName());
				const FString JsonName = Renamed ? *Renamed : ApplyNaming(Authored, bBool, Naming);
				// Normalised from the camelCase spelling, so a bool's 'b' prefix never takes part (bIsActive ~ is_active)
				const FString Loose = Renamed ? *Renamed : ApplyNaming(Authored, bBool, EVaRestJsonNaming::CamelCase);

				FFieldInfo Field;
				Field.Property = Property;
				AssignUtf8(Field.Name, JsonName);
				if (FindIn(Map->ByName, Map->Fields, &FFieldInfo::Name, View(Field.Name), HashKey(View(Field.Name))) != INDEX_NONE)
				{
					// One key twice would write a duplicate and make decode depend on member order
					UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %s.%s is skipped: its JSON name \"%s\" is already used by another field"), *Struct->GetName(), *Property->GetName(), *JsonName);
					continue;
				}
				FUtf8Scratch Scratch;
				FoldUtf8(View(Field.Name), Scratch);
				Field.Folded.Append(Scratch.GetData(), Scratch.Num());
				TArray<UTF8CHAR> LooseUtf8;
				AssignUtf8(LooseUtf8, Loose);
				NormalizeUtf8(View(LooseUtf8), Scratch);
				Field.Normalized.Append(Scratch.GetData(), Scratch.Num());

				const uint32 NameHash = HashKey(View(Field.Name));
				const int32 FieldIndex = Map->Fields.Add(MoveTemp(Field));
				Map->ByName.Add(NameHash, FieldIndex);
			}
			AddUnambiguous(Map->ByFolded, Map->Fields, &FFieldInfo::Folded);
			AddUnambiguous(Map->ByNormalized, Map->Fields, &FFieldInfo::Normalized);
			return Map;
		}
	}

	FFieldMapRef GetFieldMap(const UScriptStruct* Struct, EVaRestJsonNaming Naming)
	{
		NamesPrivate::FCache& Shared = NamesPrivate::Cache();
		const TPair<FObjectKey, uint8> Key(FObjectKey(Struct), static_cast<uint8>(Naming));
		{
			FReadScopeLock ReadLock(Shared.Lock);
			if (const FFieldMapRef* Found = Shared.Maps.Find(Key))
			{
				return *Found;
			}
		}
		// Built outside the lock: Build reads the rename registry under the same lock, and FRWLock is not recursive
		uint64 BuiltEpoch = 0;
		const FFieldMapRef Built = NamesPrivate::Build(Struct, Naming, BuiltEpoch);
		FWriteScopeLock WriteLock(Shared.Lock);
		if (const FFieldMapRef* Raced = Shared.Maps.Find(Key))
		{
			return *Raced; // another thread built it first: every caller shares one map
		}
		if (Shared.Epoch != BuiltEpoch)
		{
			return Built; // reset or renamed while this map was built: used for this call, never cached
		}
		Shared.Maps.Add(Key, Built);
		return Built;
	}

	int32 FFieldMap::Match(FUtf8StringView Key, uint32 KeyHash, FUtf8Scratch& Scratch, uint8& OutTier) const
	{
		int32 Found = NamesPrivate::FindIn(ByName, Fields, &FFieldInfo::Name, Key, KeyHash);
		if (Found != INDEX_NONE)
		{
			OutTier = 0;
			return Found;
		}
		FoldUtf8(Key, Scratch);
		const FUtf8StringView FoldedKey(Scratch.GetData(), Scratch.Num());
		Found = NamesPrivate::FindIn(ByFolded, Fields, &FFieldInfo::Folded, FoldedKey, HashKey(FoldedKey));
		if (Found != INDEX_NONE)
		{
			OutTier = 1;
			return Found;
		}
		NormalizeUtf8(Key, Scratch);
		const FUtf8StringView NormalizedKey(Scratch.GetData(), Scratch.Num());
		Found = NamesPrivate::FindIn(ByNormalized, Fields, &FFieldInfo::Normalized, NormalizedKey, HashKey(NormalizedKey));
		if (Found != INDEX_NONE)
		{
			OutTier = 2;
			return Found;
		}
		return INDEX_NONE;
	}

	FString StripGuidSuffix(const FString& PropertyName)
	{
		// UUserDefinedStruct::GetAuthoredNameForField, cooked branch (UserDefinedStruct.cpp:300-313), line for line
		const int32 GuidStrLen = 32;
		const int32 MinimalPostfixLen = GuidStrLen + 3;
		if (PropertyName.Len() > MinimalPostfixLen)
		{
			const FString DisplayName = PropertyName.LeftChop(GuidStrLen + 1);
			int32 FirstCharToRemove = -1;
			if (DisplayName.FindLastChar(TCHAR('_'), FirstCharToRemove) && FirstCharToRemove > 0)
			{
				return DisplayName.Mid(0, FirstCharToRemove);
			}
		}
		return PropertyName;
	}

	FString AuthoredFieldName(const UScriptStruct* Owner, const FProperty* Property)
	{
		// Never Property->GetAuthoredName(): for a Blueprint struct it is the editor-only friendly name in editor builds
		// (UserDefinedStruct.cpp:289-298) and the stripped name in cooked ones, so PIE and a packaged game would write
		// different JSON (spec 2.4). The cooked algorithm runs everywhere instead.
		return Owner && Owner->IsA<UUserDefinedStruct>() ? StripGuidSuffix(Property->GetName()) : Property->GetName();
	}

	FString ApplyNaming(const FString& AuthoredName, bool bBoolField, EVaRestJsonNaming Naming)
	{
		if (Naming == EVaRestJsonNaming::AsIs)
		{
			return AuthoredName;
		}
		// UE names bools bSomething (coding standard); the b is a C++ convention, not part of the JSON name
		const bool bBoolPrefix = bBoolField && AuthoredName.Len() >= 2 && AuthoredName[0] == TEXT('b') && FChar::IsUpper(AuthoredName[1]);
		const TArray<FString> Words = NamesPrivate::SplitWords(bBoolPrefix ? AuthoredName.RightChop(1) : AuthoredName);
		if (Words.Num() == 0)
		{
			return AuthoredName;
		}
		FString Result;
		for (int32 i = 0; i < Words.Num(); ++i)
		{
			switch (Naming)
			{
			case EVaRestJsonNaming::CamelCase:
				Result += i == 0 ? Words[i].ToLower() : NamesPrivate::Capitalized(Words[i]);
				break;
			case EVaRestJsonNaming::SnakeCase:
				if (i > 0)
				{
					Result.AppendChar(TEXT('_'));
				}
				Result += Words[i].ToLower();
				break;
			case EVaRestJsonNaming::PascalCase:
				Result += NamesPrivate::Capitalized(Words[i]);
				break;
			case EVaRestJsonNaming::AsIs:
				break;
			}
		}
		return Result;
	}

	void FoldUtf8(FUtf8StringView Text, FUtf8Scratch& Out)
	{
		Out.Reset();
		for (const UTF8CHAR C : Text)
		{
			const uint8 Byte = static_cast<uint8>(C);
			Out.Add(static_cast<UTF8CHAR>(Byte >= 'A' && Byte <= 'Z' ? Byte + ('a' - 'A') : Byte));
		}
	}

	void NormalizeUtf8(FUtf8StringView Text, FUtf8Scratch& Out)
	{
		Out.Reset();
		for (const UTF8CHAR C : Text)
		{
			const uint8 Byte = static_cast<uint8>(C);
			if (Byte == '_' || Byte == '-' || Byte == ' ')
			{
				continue;
			}
			Out.Add(static_cast<UTF8CHAR>(Byte >= 'A' && Byte <= 'Z' ? Byte + ('a' - 'A') : Byte));
		}
	}

	bool IsSupported(const FProperty* Property, FString& OutWhy)
	{
		if (Property->IsA<FBoolProperty>() || Property->IsA<FNumericProperty>() || Property->IsA<FEnumProperty>()
			|| Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>()
			|| Property->IsA<FStructProperty>() || Property->IsA<FSoftObjectProperty>()) // FSoftClassProperty is one (UnrealType.h:3550)
		{
			return true;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return IsSupported(ArrayProperty->Inner, OutWhy);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			return IsSupported(SetProperty->ElementProp, OutWhy);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			const FProperty* KeyProperty = MapProperty->KeyProp;
			const FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty);
			const bool bKeyFits = KeyProperty->IsA<FStrProperty>() || KeyProperty->IsA<FNameProperty>() || KeyProperty->IsA<FEnumProperty>()
				|| (NumericKey && (NumericKey->IsInteger() || NumericKey->IsEnum()));
			if (!bKeyFits)
			{
				OutWhy = TEXT("map keys must be strings, names, integers or enums (they become JSON object keys)");
				return false;
			}
			return IsSupported(MapProperty->ValueProp, OutWhy);
		}
		if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(Property))
		{
			return IsSupported(OptionalProperty->GetValueProperty(), OutWhy);
		}
		if (Property->IsA<FObjectPropertyBase>() || Property->IsA<FInterfaceProperty>())
		{
			OutWhy = TEXT("object references are not converted");
			return false;
		}
		OutWhy = FString::Printf(TEXT("%s properties are not converted"), *Property->GetClass()->GetName());
		return false;
	}

	ESpecialStruct ClassifyStruct(const UScriptStruct* Struct)
	{
		if (Struct == FVaRestJson::StaticStruct())
		{
			return ESpecialStruct::Json;
		}
		if (Struct == TBaseStructure<FDateTime>::Get())
		{
			return ESpecialStruct::DateTime;
		}
		if (Struct == TBaseStructure<FGuid>::Get())
		{
			return ESpecialStruct::Guid;
		}
		if (Struct == FSoftObjectPath::StaticStruct() || Struct == FSoftClassPath::StaticStruct())
		{
			return ESpecialStruct::SoftObjectPath;
		}
		// FTimespan has no TBaseStructure specialisation (UObject/Class.h declares FDateTime's at :5645, none for
		// FTimespan); its reflection is the noexport struct /Script/CoreUObject.Timespan (NoExportTypes.h:2252-2253).
		static const FName TimespanName(TEXT("Timespan"));
		static const FName CoreUObjectPackage(TEXT("/Script/CoreUObject"));
		if (Struct->GetFName() == TimespanName && Struct->GetPackage()->GetFName() == CoreUObjectPackage)
		{
			return ESpecialStruct::Timespan;
		}
		return ESpecialStruct::None;
	}

	bool IsUnsignedInteger(const FNumericProperty* Property)
	{
		return Property->IsA<FByteProperty>() || Property->IsA<FUInt16Property>() || Property->IsA<FUInt32Property>() || Property->IsA<FUInt64Property>();
	}

	bool IsPlainByteArray(const FArrayProperty* Property)
	{
		const FByteProperty* Inner = CastField<FByteProperty>(Property->Inner);
		return Inner && !Inner->Enum;
	}
}

void FVaRestJsonNames::Register(const UScriptStruct* Struct, TConstArrayView<FVaRestJsonRename> Renames)
{
	using namespace VaRest::Json::Convert;
	if (!Struct)
	{
		return;
	}
	TMap<FName, FString> ByField;
	for (const FVaRestJsonRename& Rename : Renames)
	{
		ByField.Add(Rename.Field, Rename.Json);
	}
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FWriteScopeLock WriteLock(Shared.Lock);
	Shared.Renames.Add(FObjectKey(Struct), MoveTemp(ByField));
	Shared.Maps.Reset(); // maps built before the rename are stale
	++Shared.Epoch;
}

void FVaRestJsonNames::Unregister(const UScriptStruct* Struct)
{
	using namespace VaRest::Json::Convert;
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FWriteScopeLock WriteLock(Shared.Lock);
	Shared.Renames.Remove(FObjectKey(Struct));
	Shared.Maps.Reset();
	++Shared.Epoch;
}

void FVaRestJsonConvert::ResetCache()
{
	using namespace VaRest::Json::Convert;
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FWriteScopeLock WriteLock(Shared.Lock);
	Shared.Maps.Reset();
	++Shared.Epoch;
}

int32 FVaRestJsonConvert::NumCachedFieldMapsInternal()
{
	using namespace VaRest::Json::Convert;
	NamesPrivate::FCache& Shared = NamesPrivate::Cache();
	FReadScopeLock ReadLock(Shared.Lock);
	return Shared.Maps.Num();
}
```

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including the five `VaRest.Json.Convert.*` tests above.

- [ ] **Step 8: Restore-the-bug (field-name algorithm)**

In `StripGuidSuffix` change `PropertyName.LeftChop(GuidStrLen + 1)` to `PropertyName.LeftChop(GuidStrLen)`. Run `Tools/run_tests.sh VaRest.Json.Convert.UserStructNameAlgorithm`; expect it to fail (`Name_<n>_<guid>` gives `Score_3`). Restore, run, expect green. Report both. (Task 13 repeats the proof on a real Blueprint struct in the editor.)

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: naming policies, Blueprint struct field names, thread-safe field-map cache, C++ renames

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 7: Encode — straight to UTF-8 bytes, no DOM (spec §2.4)

One templated walker emits SAX events; the RapidJSON writer (bytes) and `FDomBuilder` (a DOM, for the BP nodes and `Set`) are its two sinks, so both outputs agree by construction. Sibling modules get internal entries that write into a writer they already have open (WebSocket spec `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` §19 item 9: one-pass envelopes such as `{"type":"x","data":{...}}`; Socket.IO spec §18.2 J-2: positional arguments; WebSocket §6.1: a payload's members next to the route), and an encode hook for `TArray<uint8>` values (Socket.IO §18.2 J-1: binary attachments become placeholders). See `.superpowers/sdd/sio-m2-gap.md`.

**Engine facts:** `rapidjson::Writer` asserts on a null string pointer (`writer.h:198`, `:205`) and on a key without a value (`:236`); `Double` refuses NaN/Inf after `Prefix()` (`:195`). Enum names are written the way `FJsonObjectConverter` writes them, `UEnum::GetAuthoredNameStringByIndex` (`JsonObjectConverter.cpp:109-114`), which for a Blueprint enum is its display text (`UserDefinedEnum.cpp:208-220`). `FDateTime::ToIso8601` is `%Y-%m-%dT%H:%M:%S.%sZ` (`DateTime.cpp:216-219`). `EGuidFormats::DigitsWithHyphens` is `%08X-%04X-%04X-%04X-%04X%08X` (`Guid.cpp:61-62`). The `Json` module depends on `RapidJSON` publicly (`Json.Build.cs:9-15`), so a public VaRestJson header may include `RapidJsonIncludes.h`.

**Files:**
- Create: `Source/VaRestJson/Public/VaRestJsonWriterInternal.h` (writer types moved out of `VaRestJsonSax.h`, plus `WriteStructMembersInternal`, `WriteJsonInternal`, `WriteJsonMembersInternal`, `WriteStructInternal`, `WritePropertyInternal`)
- Modify: `Source/VaRestJson/Private/VaRestJsonSax.h`, `Source/VaRestJson/VaRestJson.Build.cs`
- Create: `Source/VaRestJson/Private/VaRestJsonBuilder.h`; replace `Source/VaRestJson/Private/VaRestJsonReader.cpp` (uses it)
- Create: `Source/VaRestJson/Private/VaRestJsonConvertEncode.cpp`
- Modify: `Source/VaRestJson/Public/VaRestJsonConvert.h` (encode API, `FVaRestJsonBytesWriter`, `BytesWriter` option), `Source/VaRestJson/Private/VaRestJsonConvertPrivate.h` (`FloatToJsonDouble`)
- Modify: `Source/VaRestJson/Private/Tests/VaRestJsonConvertTestTypes.h` (append)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonConvertEncodeTest.cpp`

**Interfaces:**
- Consumes: Task 3 (`EmitNode`, `FEmitState`), Task 4 (`ConvertForDocument`), Task 6 (field maps, `ClassifyStruct`, `IsUnsignedInteger`, `IsPlainByteArray`).
- Produces: public internal header `VaRestJsonWriterInternal.h` with `VaRest::Json::FUtf8Sink`, `FWriterAllocator`, `FCondensedWriter`, `FPrettyWriter` (moved from `VaRestJsonSax.h`, same names) and, all `VARESTJSON_API` in `VaRest::Json`: `bool WriteStructMembersInternal(FCondensedWriter& / FPrettyWriter& Writer, const UScriptStruct*, const void* Data, const FVaRestJsonConvertOptions& = {}, int32 OpenContainers = 1, FVaRestJsonError* OutError = nullptr)`; `bool WriteJsonInternal(FCondensedWriter&, const FVaRestJson& Value, int32 OpenContainers = 0, FVaRestJsonError* = nullptr)`; `bool WriteJsonMembersInternal(FCondensedWriter&, const FVaRestJson& Object, int32 OpenContainers = 1, FVaRestJsonError* = nullptr)`; `bool WriteStructInternal(FCondensedWriter&, const UScriptStruct*, const void* Data, const FVaRestJsonConvertOptions& = {}, int32 OpenContainers = 0, FVaRestJsonError* = nullptr)`; `bool WritePropertyInternal(FCondensedWriter&, const FProperty*, const void* Value, const FVaRestJsonConvertOptions& = {}, int32 OpenContainers = 0, FVaRestJsonError* = nullptr)`; `using FVaRestJsonBytesWriter = TFunction<bool(TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement)>` and `FVaRestJsonConvertOptions::BytesWriter` (`VaRestJsonConvert.h`); `VaRest::Json::FDomBuilder(FDocument&, int32 DepthLimit)` with the RapidJSON handler interface and `FNode* Result`; `double VaRest::Json::Convert::FloatToJsonDouble(float)`; `FVaRestJsonConvert::StructToUtf8(const UScriptStruct*, const void* Data, TArray<uint8>& Out, const FVaRestJsonConvertOptions& = {}, EVaRestJsonFormat = Condensed, FVaRestJsonError* OutError = nullptr) -> bool`; `StructToString(...) -> FString`; `StructToJson(const UScriptStruct*, const void*, Options) -> FVaRestJson`; `PropertyToJson(const FProperty*, const void* Value, Options) -> FVaRestJson`; `template <typename T> ToUtf8(const T&, Options, Format) -> TArray<uint8>`; `template <typename T> ToJson(const T&, Options) -> FVaRestJson`. Test types `EVaRestJsonTestColor`, `EVaRestJsonTestLegacy::Type`, `FVaRestJsonTestAll`, `FVaRestJsonTestTree`, `FVaRestJsonTestBlob { TArray<uint8> Data; }`, `FVaRestJsonTestAttachments { TArray<uint8> First; TArray<FVaRestJsonTestBlob> List; TMap<FString, FVaRestJsonTestBlob> ByName; }`.

Encode rules (spec §2.4, plus decisions): an unset `TOptional` field and an invalid `FVaRestJson` field are omitted (missing = undefined); inside a container they are `null`; a float is written as the shortest decimal that reads back as the same float; `FTimespan` is total seconds; `TMap` keys become object keys (strings, names, decimal integers, enumerator names or numbers); a C-style array member is a JSON array of `ArrayDim` elements (never omitted as a whole; an invalid element is `null`); `FText` is written as `BuildSourceString()`, the culture-invariant source text spec §2.4 asks for (`Text.h:591-592` → `BuildInvariantDisplayString`, `Text.cpp:1348-1351`), not `ToString()`'s current-culture display text that `FJsonObjectConverter` writes; a `TArray<uint8>` value is offered to `Options.BytesWriter` first (walk order: field declaration order, then container order; empty arrays too), then written as numbers or base64; NaN/Inf become `null` with one warning per encode; any failure (depth) fails the whole encode. Nested containers (`TArray<TArray<uint8>>`, a `TMap` of arrays) are not reflectable (UHT rejects them), so bytes inside containers always sit in a struct.

- [ ] **Step 1: Append the test types** to `VaRestJsonConvertTestTypes.h` (and add `#include "UObject/SoftObjectPath.h"` and `#include "UObject/SoftObjectPtr.h"` to its include block, before the `.generated.h` include)

```cpp
UENUM()
enum class EVaRestJsonTestColor : uint8
{
	Red,
	Green,
	Blue
};

UENUM()
namespace EVaRestJsonTestLegacy
{
	enum Type : int
	{
		First,
		Second
	};
}

/** Every kind of field the converter supports */
USTRUCT()
struct FVaRestJsonTestAll
{
	GENERATED_BODY()

	FVaRestJsonTestAll()
	{
		for (int32& Element : FixedArray)
		{
			Element = 0;
		}
	}

	UPROPERTY()
	bool bIsActive = false;

	UPROPERTY()
	int8 Int8Value = 0;

	UPROPERTY()
	int16 Int16Value = 0;

	UPROPERTY()
	int32 Int32Value = 0;

	UPROPERTY()
	int64 Int64Value = 0;

	UPROPERTY()
	uint8 ByteValue = 0;

	UPROPERTY()
	uint16 Uint16Value = 0;

	UPROPERTY()
	uint32 Uint32Value = 0;

	UPROPERTY()
	uint64 Uint64Value = 0;

	UPROPERTY()
	float FloatValue = 0.f;

	UPROPERTY()
	double DoubleValue = 0.0;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FName NameValue;

	UPROPERTY()
	FText TextValue;

	UPROPERTY()
	EVaRestJsonTestColor Color = EVaRestJsonTestColor::Red;

	UPROPERTY()
	TEnumAsByte<EVaRestJsonTestLegacy::Type> Legacy = EVaRestJsonTestLegacy::First;

	UPROPERTY()
	FVaRestJsonTestInner Inner;

	UPROPERTY()
	TArray<int32> Numbers;

	UPROPERTY()
	TArray<FVaRestJsonTestInner> Items;

	UPROPERTY()
	TSet<FString> Tags;

	UPROPERTY()
	TMap<FString, int32> Scores;

	UPROPERTY()
	TMap<int32, FString> ById;

	UPROPERTY()
	TMap<FName, int32> ByName;

	UPROPERTY()
	TMap<EVaRestJsonTestColor, int32> ByColor;

	UPROPERTY()
	FVaRestJson Extra;

	UPROPERTY()
	FDateTime When;

	UPROPERTY()
	FTimespan Duration;

	UPROPERTY()
	FGuid Guid;

	UPROPERTY()
	FSoftObjectPath AssetPath;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObject;

	UPROPERTY()
	TArray<uint8> Bytes;

	UPROPERTY()
	TOptional<int32> MaybeNumber;

	UPROPERTY()
	int32 FixedArray[3];
};

/** Recursion through a container of itself: nesting is bounded only by the depth limit */
USTRUCT()
struct FVaRestJsonTestTree
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVaRestJsonTestTree> Children;
};

/** Bytes in a struct, so they can sit inside containers (UHT rejects nested containers) */
USTRUCT()
struct FVaRestJsonTestBlob
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint8> Data;
};

/** Every place a TArray<uint8> can meet the BytesWriter hook: a field, array elements, map values */
USTRUCT()
struct FVaRestJsonTestAttachments
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint8> First;

	UPROPERTY()
	TArray<FVaRestJsonTestBlob> List;

	UPROPERTY()
	TMap<FString, FVaRestJsonTestBlob> ByName;
};
```

- [ ] **Step 2: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonConvertEncodeTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonConvertTestTypes.h"
#include "VaRestJsonWriterInternal.h"

#include <limits>

namespace VaRestJsonConvertEncodeTest
{
	FString Utf8Text(const TArray<uint8>& Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	}

	FVaRestJsonTestAll MakeFilled()
	{
		FVaRestJsonTestAll All;
		All.bIsActive = true;
		All.Int8Value = -8;
		All.Int16Value = -16;
		All.Int32Value = -32;
		All.Int64Value = MIN_int64;
		All.ByteValue = 255;
		All.Uint16Value = 65535;
		All.Uint32Value = MAX_uint32;
		All.Uint64Value = MAX_uint64;
		All.FloatValue = 0.1f;
		All.DoubleValue = 0.1;
		All.StringValue = TEXT("Ключ \"q\"");
		All.NameValue = FName(TEXT("Nm"));
		All.TextValue = FText::FromString(TEXT("Hello"));
		All.Color = EVaRestJsonTestColor::Green;
		All.Legacy = EVaRestJsonTestLegacy::Second;
		All.Inner.Id = 1;
		All.Inner.Name = TEXT("in");
		All.Numbers = { 1, 2, 3 };
		FVaRestJsonTestInner Item;
		Item.Id = 2;
		Item.Name = TEXT("two");
		All.Items.Add(Item);
		All.Tags.Add(TEXT("a"));
		All.Tags.Add(TEXT("b"));
		All.Scores.Add(TEXT("x"), 10);
		All.ById.Add(5, TEXT("five"));
		All.ByName.Add(FName(TEXT("n")), 2);
		All.ByColor.Add(EVaRestJsonTestColor::Blue, 3);
		All.Extra = FVaRestJson::Object().Set(TEXT("raw"), FVaRestJson::Array(1, 2));
		All.When = FDateTime(2026, 10, 20, 12, 34, 56, 789);
		All.Duration = FTimespan::FromSeconds(90.5);
		All.Guid = FGuid(0x01234567, 0x89ABCDEF, 0x01234567, 0x89ABCDEF);
		All.AssetPath = FSoftObjectPath(TEXT("/Game/Maps/Entry.Entry"));
		All.SoftObject = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/A.A")));
		All.Bytes = { 1, 2, 255 };
		All.MaybeNumber = 5;
		All.FixedArray[0] = 4;
		All.FixedArray[1] = 5;
		All.FixedArray[2] = 6;
		return All;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeExactTest, "VaRest.Json.Convert.Encode.Exact",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeExactTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertEncodeTest;
	FVaRestJsonTestInner Inner;
	Inner.Id = 7;
	Inner.Name = TEXT("Neo");
	TArray<uint8> Bytes;
	Bytes.Add('>');
	TestTrue(TEXT("encodes"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestInner::StaticStruct(), &Inner, Bytes));
	TestEqualSensitive(TEXT("appends condensed UTF-8"), Utf8Text(Bytes), FString(TEXT(">{\"id\":7,\"name\":\"Neo\"}")));
	TestEqualSensitive(TEXT("pretty"), FVaRestJsonConvert::StructToString(FVaRestJsonTestInner::StaticStruct(), &Inner, FVaRestJsonConvertOptions(), EVaRestJsonFormat::Pretty),
		FString(TEXT("{\n  \"id\": 7,\n  \"name\": \"Neo\"\n}")));
	TestEqualSensitive(TEXT("ToUtf8<T>"), Utf8Text(FVaRestJsonConvert::ToUtf8(Inner)), FString(TEXT("{\"id\":7,\"name\":\"Neo\"}")));
	TestEqualSensitive(TEXT("ToJson<T>"), FVaRestJsonConvert::ToJson(Inner).ToString(), FString(TEXT("{\"id\":7,\"name\":\"Neo\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeMembersTest, "VaRest.Json.Convert.Encode.MembersIntoOpenWriter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeMembersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRestJsonConvertEncodeTest;
	// What a socket layer does (WebSocket spec §19 item 9): its own envelope, the struct's members inside, one pass
	FVaRestJsonTestInner Inner;
	Inner.Id = 7;
	Inner.Name = TEXT("Neo");
	TArray<uint8> Bytes;
	FUtf8Sink Sink{ Bytes };
	FWriterAllocator StackAllocator;
	FCondensedWriter Writer(Sink, &StackAllocator);
	Writer.StartObject();
	Writer.Key("type");
	Writer.String("hello");
	Writer.Key("data");
	Writer.StartObject();
	TestTrue(TEXT("members written"), WriteStructMembersInternal(Writer, FVaRestJsonTestInner::StaticStruct(), &Inner, FVaRestJsonConvertOptions(), 2));
	Writer.EndObject();
	Writer.EndObject();
	TestEqualSensitive(TEXT("one-pass envelope"), Utf8Text(Bytes), FString(TEXT("{\"type\":\"hello\",\"data\":{\"id\":7,\"name\":\"Neo\"}}")));

	// A struct with a scalar JSON form has no members to write
	TArray<uint8> Other;
	FUtf8Sink OtherSink{ Other };
	FCondensedWriter OtherWriter(OtherSink, &StackAllocator);
	OtherWriter.StartObject();
	const FGuid Guid;
	FVaRestJsonError Error;
	TestFalse(TEXT("a GUID has no members"), WriteStructMembersInternal(OtherWriter, TBaseStructure<FGuid>::Get(), &Guid, FVaRestJsonConvertOptions(), 1, &Error));
	TestTrue(TEXT("... and says so"), Error.Message.Contains(TEXT("has no members")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeAllTypesTest, "VaRest.Json.Convert.Encode.AllTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeAllTypesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertEncodeTest;
	const FVaRestJsonTestAll All = MakeFilled();
	const FVaRestJson J = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &All);
	if (!TestTrue(TEXT("encoded"), J.IsObject()))
	{
		return false;
	}
	TestTrue(TEXT("bool, 'b' dropped"), J[TEXT("isActive")].IsBool() && J[TEXT("isActive")].AsBool());
	TestEqual(TEXT("int8"), J[TEXT("int8Value")].AsInt(), -8);
	TestEqual(TEXT("int16"), J[TEXT("int16Value")].AsInt(), -16);
	TestEqual(TEXT("int64 min exact"), J[TEXT("int64Value")].AsInt64(), MIN_int64);
	TestEqual(TEXT("byte"), J[TEXT("byteValue")].AsInt(), 255);
	TestEqual(TEXT("uint32 max"), J[TEXT("uint32Value")].AsInt64(), static_cast<int64>(MAX_uint32));
	TestTrue(TEXT("uint64 max exact"), J[TEXT("uint64Value")].AsUInt64() == MAX_uint64);
	TestTrue(TEXT("float written shortest"), J[TEXT("floatValue")].AsDouble() == 0.1);
	TestTrue(TEXT("double"), J[TEXT("doubleValue")].AsDouble() == 0.1);
	TestEqualSensitive(TEXT("string"), J[TEXT("stringValue")].AsString(), FString(TEXT("Ключ \"q\"")));
	TestEqualSensitive(TEXT("name"), J[TEXT("nameValue")].AsString(), FString(TEXT("Nm")));
	TestEqualSensitive(TEXT("text"), J[TEXT("textValue")].AsString(), FString(TEXT("Hello")));
	TestEqualSensitive(TEXT("enum class by name"), J[TEXT("color")].AsString(), FString(TEXT("Green")));
	TestEqualSensitive(TEXT("byte enum by name"), J[TEXT("legacy")].AsString(), FString(TEXT("Second")));
	TestEqual(TEXT("nested struct"), J.Path(TEXT("inner.id")).AsInt(), 1);
	TestEqualSensitive(TEXT("numbers"), J[TEXT("numbers")].ToString(), FString(TEXT("[1,2,3]")));
	TestEqualSensitive(TEXT("array of structs"), J[TEXT("items")].ToString(), FString(TEXT("[{\"id\":2,\"name\":\"two\"}]")));
	TestEqual(TEXT("set as array"), J[TEXT("tags")].Num(), 2);
	TestEqual(TEXT("string-keyed map"), J.Path(TEXT("scores.x")).AsInt(), 10);
	TestEqualSensitive(TEXT("int-keyed map"), J[TEXT("byId")].ToString(), FString(TEXT("{\"5\":\"five\"}")));
	TestEqual(TEXT("name-keyed map"), J.Path(TEXT("byName.n")).AsInt(), 2);
	TestEqual(TEXT("enum-keyed map"), J.Path(TEXT("byColor.Blue")).AsInt(), 3);
	TestEqualSensitive(TEXT("FVaRestJson passthrough"), J[TEXT("extra")].ToString(), FString(TEXT("{\"raw\":[1,2]}")));
	TestEqualSensitive(TEXT("ISO 8601"), J[TEXT("when")].AsString(), FString(TEXT("2026-10-20T12:34:56.789Z")));
	TestTrue(TEXT("timespan in seconds"), J[TEXT("duration")].AsDouble() == 90.5);
	TestEqualSensitive(TEXT("guid"), J[TEXT("guid")].AsString(), FString(TEXT("01234567-89AB-CDEF-0123-456789ABCDEF")));
	TestEqualSensitive(TEXT("soft path"), J[TEXT("assetPath")].AsString(), FString(TEXT("/Game/Maps/Entry.Entry")));
	TestEqualSensitive(TEXT("soft object"), J[TEXT("softObject")].AsString(), FString(TEXT("/Game/A.A")));
	TestEqualSensitive(TEXT("bytes as numbers"), J[TEXT("bytes")].ToString(), FString(TEXT("[1,2,255]")));
	TestEqual(TEXT("optional set"), J[TEXT("maybeNumber")].AsInt(), 5);
	TestEqualSensitive(TEXT("C-style array"), J[TEXT("fixedArray")].ToString(), FString(TEXT("[4,5,6]")));
	TestEqual(TEXT("field count"), J.Num(), 33);

	// Both sinks write the same text
	TestEqualSensitive(TEXT("bytes path == DOM path"), FVaRestJsonConvert::StructToString(FVaRestJsonTestAll::StaticStruct(), &All), J.ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeOptionsTest, "VaRest.Json.Convert.Encode.Options",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeOptionsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertEncodeTest;
	FVaRestJsonTestAll All = MakeFilled();
	All.MaybeNumber.Reset();
	All.Extra = FVaRestJson();
	FVaRestJsonConvertOptions Options;
	Options.bEnumsAsNumbers = true;
	Options.bBytesAsBase64 = true;
	Options.Naming = EVaRestJsonNaming::SnakeCase;
	const FVaRestJson J = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &All, Options);
	TestEqual(TEXT("enum as number"), J[TEXT("color")].AsInt(), 1);
	TestEqual(TEXT("byte enum as number"), J[TEXT("legacy")].AsInt(), 1);
	TestEqual(TEXT("enum map key as number"), J.Path(TEXT("by_color.2")).AsInt(), 3);
	TestEqualSensitive(TEXT("bytes as base64"), J[TEXT("bytes")].AsString(), FString(TEXT("AQL/")));
	TestFalse(TEXT("an unset optional is omitted"), J.Has(TEXT("maybe_number")));
	TestFalse(TEXT("a missing FVaRestJson is omitted"), J.Has(TEXT("extra")));
	TestTrue(TEXT("snake_case names"), J.Has(TEXT("is_active")) && J.Has(TEXT("int64_value")) && J.Has(TEXT("by_id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeDepthTest, "VaRest.Json.Convert.Encode.Depth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeDepthTest::RunTest(const FString& Parameters)
{
	// Each tree level is an object plus an array: 300 levels nest 600 containers, 200 levels nest 400
	FVaRestJsonTestTree Deep;
	FVaRestJsonTestTree* Cursor = &Deep;
	for (int32 i = 0; i < 300; ++i)
	{
		Cursor->Children.AddDefaulted();
		Cursor = &Cursor->Children[0];
	}
	AddExpectedErrorPlain(TEXT("deeper than 512"), EAutomationExpectedErrorFlags::Contains, 2);
	TArray<uint8> Out;
	Out.Add('x');
	TestFalse(TEXT("too deep to encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Deep, Out));
	TestEqual(TEXT("Out unchanged"), Out.Num(), 1);
	TestFalse(TEXT("the DOM path fails too"), FVaRestJsonConvert::StructToJson(FVaRestJsonTestTree::StaticStruct(), &Deep).IsValid());
	FVaRestJsonError Error;
	TestFalse(TEXT("with an error out-param nothing is logged"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Deep, Out, FVaRestJsonConvertOptions(), EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("the error says why"), Error.Message.Contains(TEXT("deeper than 512")));

	FVaRestJsonTestTree Fine;
	Cursor = &Fine;
	for (int32 i = 0; i < 200; ++i)
	{
		Cursor->Children.AddDefaulted();
		Cursor = &Cursor->Children[0];
	}
	TestTrue(TEXT("400 levels encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Fine, Out));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeFloatTest, "VaRest.Json.Convert.Encode.FloatShortest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeFloatTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::Convert::FloatToJsonDouble;
	const float Samples[] = { 0.1f, 1.5f, 3.14159f, 1e-7f, 123456.79f, 16777216.f, TNumericLimits<float>::Max(), -2.5e-3f, TNumericLimits<float>::Min() };
	for (const float Sample : Samples)
	{
		TestTrue(FString::Printf(TEXT("%g reads back as the same float"), Sample), static_cast<float>(FloatToJsonDouble(Sample)) == Sample);
	}
	TestEqualSensitive(TEXT("0.1f is written 0.1"), FVaRestJson::Object().Set(TEXT("f"), FloatToJsonDouble(0.1f)).ToString(), FString(TEXT("{\"f\":0.1}")));
	TestEqualSensitive(TEXT("1.5f is written 1.5"), FVaRestJson::Object().Set(TEXT("f"), FloatToJsonDouble(1.5f)).ToString(), FString(TEXT("{\"f\":1.5}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeSkippedTest, "VaRest.Json.Convert.Encode.SkippedAndNonFinite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeSkippedTest::RunTest(const FString& Parameters)
{
	FVaRestJsonConvert::ResetCache();
	AddExpectedMessagePlain(TEXT("VaRestJsonTestSkipped."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	FVaRestJsonTestSkipped Skipped;
	Skipped.Kept = 4;
	TestEqualSensitive(TEXT("only convertible fields"), FVaRestJsonConvert::StructToString(FVaRestJsonTestSkipped::StaticStruct(), &Skipped), FString(TEXT("{\"kept\":4}")));

	AddExpectedMessagePlain(TEXT("non-finite"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestAll All;
	All.DoubleValue = std::numeric_limits<double>::quiet_NaN();
	const FVaRestJson J = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &All);
	TestTrue(TEXT("NaN written as null"), J[TEXT("doubleValue")].IsNull());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeBytesWriterTest, "VaRest.Json.Convert.Encode.BytesWriter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeBytesWriterTest::RunTest(const FString& Parameters)
{
	// Socket.IO spec §18.2 J-1: binary values become numbered placeholders, written in place in one pass
	FVaRestJsonTestAttachments Value;
	Value.First = { 1, 2 };
	Value.List.AddDefaulted(2);
	Value.List[0].Data = { 3 }; // List[1].Data stays empty: an empty array is offered too
	Value.ByName.Add(TEXT("k")).Data = { 4, 5, 6 };

	TArray<int32> Seen; // the sizes the hook was offered, in walk order
	FVaRestJsonConvertOptions Options;
	Options.BytesWriter = [&Seen](TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement)
	{
		OutReplacement = FVaRestJson::Object().Set(TEXT("_placeholder"), true).Set(TEXT("num"), Seen.Num());
		Seen.Add(Bytes.Num());
		return true;
	};
	const FString Text = FVaRestJsonConvert::StructToString(FVaRestJsonTestAttachments::StaticStruct(), &Value, Options);
	TestEqualSensitive(TEXT("replacements in place, numbered in walk order"), Text, FString(TEXT(
		"{\"first\":{\"_placeholder\":true,\"num\":0},\"list\":[{\"data\":{\"_placeholder\":true,\"num\":1}},{\"data\":{\"_placeholder\":true,\"num\":2}}],"
		"\"byName\":{\"k\":{\"data\":{\"_placeholder\":true,\"num\":3}}}}")));
	TestTrue(TEXT("the hook saw every value's bytes, the empty one too"), Seen == TArray<int32>({ 2, 1, 0, 3 }));

	Seen.Reset();
	TestEqualSensitive(TEXT("the DOM path agrees"), FVaRestJsonConvert::StructToJson(FVaRestJsonTestAttachments::StaticStruct(), &Value, Options).ToString(), Text);

	FVaRestJsonConvertOptions Declining;
	Declining.BytesWriter = [](TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement) { return false; };
	TestEqualSensitive(TEXT("a declining hook leaves the built-in form"), FVaRestJsonConvert::StructToString(FVaRestJsonTestAttachments::StaticStruct(), &Value, Declining),
		FString(TEXT("{\"first\":[1,2],\"list\":[{\"data\":[3]},{\"data\":[]}],\"byName\":{\"k\":{\"data\":[4,5,6]}}}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeWriterEntriesTest, "VaRest.Json.Convert.Encode.WriterEntries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeWriterEntriesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRestJsonConvertEncodeTest;
	// Socket.IO positional arguments (§18.2 J-2) and WebSocket's Emit(Route, FVaRestJson) (§6.1), into one open array
	FVaRestJsonTestInner Inner;
	Inner.Id = 7;
	Inner.Name = TEXT("n");
	const FDateTime When(2026, 10, 20, 12, 0, 0);
	const FVaRestJson Payload = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), FVaRestJson::Array(true, nullptr));
	const FProperty* NameProperty = FindFProperty<FProperty>(FVaRestJsonTestInner::StaticStruct(), FName(TEXT("Name")));
	const void* NameValue = NameProperty->ContainerPtrToValuePtr<void>(&Inner);

	TArray<uint8> Bytes;
	FUtf8Sink Sink{ Bytes };
	FWriterAllocator StackAllocator;
	FCondensedWriter Writer(Sink, &StackAllocator);
	Writer.StartArray();
	TestTrue(TEXT("WriteJsonInternal"), WriteJsonInternal(Writer, Payload, 1));
	TestTrue(TEXT("an invalid handle writes null"), WriteJsonInternal(Writer, FVaRestJson(), 1));
	TestTrue(TEXT("WriteStructInternal, an object"), WriteStructInternal(Writer, FVaRestJsonTestInner::StaticStruct(), &Inner, FVaRestJsonConvertOptions(), 1));
	TestTrue(TEXT("WriteStructInternal, a value struct"), WriteStructInternal(Writer, TBaseStructure<FDateTime>::Get(), &When, FVaRestJsonConvertOptions(), 1));
	TestTrue(TEXT("WritePropertyInternal"), WritePropertyInternal(Writer, NameProperty, NameValue, FVaRestJsonConvertOptions(), 1));
	Writer.StartObject();
	TestTrue(TEXT("WriteJsonMembersInternal"), WriteJsonMembersInternal(Writer, Payload, 2));
	Writer.EndObject();
	Writer.EndArray();
	const FString Expected = FString::Printf(TEXT("[%s,null,%s,\"%s\",%s,%s]"), *Payload.ToString(),
		*FVaRestJsonConvert::StructToString(FVaRestJsonTestInner::StaticStruct(), &Inner), *When.ToIso8601(),
		*FVaRestJsonConvert::PropertyToJson(NameProperty, NameValue).ToString(), *Payload.ToString());
	TestEqualSensitive(TEXT("each entry writes what the DOM path builds"), Utf8Text(Bytes), Expected);

	// The depth limit counts the caller's open containers; OutError keeps the failure out of the log
	FVaRestJsonError Error;
	TArray<uint8> DeepBytes;
	FUtf8Sink DeepSink{ DeepBytes };
	FCondensedWriter DeepWriter(DeepSink, &StackAllocator);
	TestFalse(TEXT("an object inside MaxDepth open containers fails"), WriteJsonInternal(DeepWriter, Payload, MaxDepth, &Error));
	TestTrue(TEXT("... with the depth error"), Error.Message.Contains(TEXT("deeper than")));
	TArray<uint8> ScalarBytes;
	FUtf8Sink ScalarSink{ ScalarBytes };
	FCondensedWriter ScalarWriter(ScalarSink, &StackAllocator);
	TestTrue(TEXT("a scalar there is fine"), WriteJsonInternal(ScalarWriter, FVaRestJson::From(1), MaxDepth));
	TArray<uint8> NotObjectBytes;
	FUtf8Sink NotObjectSink{ NotObjectBytes };
	FCondensedWriter NotObjectWriter(NotObjectSink, &StackAllocator);
	TestFalse(TEXT("members of a non-object"), WriteJsonMembersInternal(NotObjectWriter, FVaRestJson::Array(1), 1, &Error));
	return true;
}

#endif
```

- [ ] **Step 3: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Convert.Encode`
Expected: compile errors (`VaRestJsonWriterInternal.h` not found, `StructToUtf8` etc. not members).

- [ ] **Step 4: The writer types become a public internal header**

`Source/VaRestJson/Public/VaRestJsonWriterInternal.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "VaRestJsonConvert.h"

/**
 * INTERNAL API for VaRest's sibling modules (WebSocket, Socket.IO), not a stable API: the UTF-8 RapidJSON writer
 * VaRestJson writes with, so a socket layer can write an envelope such as {"type":"x","data":{...struct...}} in one
 * pass (WebSocket spec §19 item 9). It exposes RapidJSON types and changes with them.
 */
namespace VaRest::Json
{
	/** RapidJSON output stream over a TArray. Put/PutReserve/PutUnsafe are found by ADL from writer.h. */
	struct FUtf8Sink
	{
		typedef char Ch;

		TArray<uint8>& Out;

		void Put(char C) { Out.Add(static_cast<uint8>(C)); }
		void Flush() {}
	};

	/**
	 * No-op on purpose. RapidJSON's escape path reserves 2 + length * 6 bytes per string
	 * (writer.h:398), which over-reserves up to 6x and whose int32 cast can overflow. The reserve
	 * is only a hint, so TArray's geometric growth in Put/PutUnsafe is used instead. This overload
	 * exists so ADL picks it over rapidjson's generic template, which would call Put per character.
	 */
	inline void PutReserve(FUtf8Sink& Sink, size_t Count)
	{
		(void)Sink;
		(void)Count;
	}

	inline void PutUnsafe(FUtf8Sink& Sink, char C)
	{
		Sink.Out.Add(static_cast<uint8>(C));
	}

	/**
	 * The writers' level stack goes through FMemory via the engine's RapidJSON allocator
	 * (JsonUtils/RapidJsonUtils.h:19-53), passed as the StackAllocator argument (writer.h:90, :103;
	 * prettywriter.h:47, :58), instead of the default CrtAllocator's std::malloc (rapidjson.h:694-696).
	 */
	using FWriterAllocator = UE::Json::Private::FAllocatorImpl;
	using FCondensedWriter = rapidjson::Writer<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FWriterAllocator>;
	using FPrettyWriter = rapidjson::PrettyWriter<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FWriterAllocator>;

	/**
	 * Writes Struct's fields as members of the object Writer has open: keys and values only, no braces. Same rules and
	 * naming as FVaRestJsonConvert::StructToUtf8. OpenContainers is how many containers the caller has open around these
	 * members (the shared depth limit counts them). False past VaRest::Json::MaxDepth, or for a struct written as a
	 * single value (FVaRestJson, FDateTime, FGuid...); the writer is then mid-object and its output must be discarded.
	 */
	VARESTJSON_API bool WriteStructMembersInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 1, FVaRestJsonError* OutError = nullptr);
	VARESTJSON_API bool WriteStructMembersInternal(FPrettyWriter& Writer, const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 1, FVaRestJsonError* OutError = nullptr);

	// The entries below share WriteStructMembersInternal's contract: OpenContainers counts the containers the caller has
	// open (the shared MaxDepth includes them); false past VaRest::Json::MaxDepth, after which the writer's output must
	// be discarded; OutError receives the failure (without it, an error is logged); any thread, for the caller's data.
	// Condensed only: wire encoders are condensed (Socket.IO spec §18.2 J-2).

	/**
	 * Writes Value as one JSON value where Writer expects a value (an array element, or after a key). An invalid
	 * (missing) handle writes null, as FVaRestJson::Add does (spec 2.2). The NaN/Infinity and depth rules of ToUtf8.
	 */
	VARESTJSON_API bool WriteJsonInternal(FCondensedWriter& Writer, const FVaRestJson& Value, int32 OpenContainers = 0,
		FVaRestJsonError* OutError = nullptr);

	/**
	 * Writes Object's members (keys and values, no braces) into the object Writer has open: a payload's members next
	 * to the caller's own keys in one pass (WebSocket spec §6.1). False when Object is not an object.
	 */
	VARESTJSON_API bool WriteJsonMembersInternal(FCondensedWriter& Writer, const FVaRestJson& Object, int32 OpenContainers = 1,
		FVaRestJsonError* OutError = nullptr);

	/**
	 * Writes Struct as one JSON value: an object for an ordinary struct, the single-value form for FDateTime, FTimespan,
	 * FGuid, FVaRestJson and soft paths. Same rules and naming as StructToUtf8, including Options.BytesWriter.
	 */
	VARESTJSON_API bool WriteStructInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 0, FVaRestJsonError* OutError = nullptr);

	/**
	 * Writes one property's value (Value points at the value, not at its container) as one JSON value: what
	 * PropertyToJson builds, without the DOM. Socket.IO's positional encode is
	 * `for (TFieldIterator<FProperty> It(Struct); It; ++It) WritePropertyInternal(Writer, *It, It->ContainerPtrToValuePtr<void>(Data), Options, 1)`.
	 */
	VARESTJSON_API bool WritePropertyInternal(FCondensedWriter& Writer, const FProperty* Property, const void* Value,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 0, FVaRestJsonError* OutError = nullptr);
}
```

In `Source/VaRestJson/Private/VaRestJsonSax.h`: delete `FUtf8Sink`, `PutReserve`, `PutUnsafe` and the three `using` aliases with their comments (they now live in the public header, same namespace and names), and replace its two RapidJSON includes with `#include "VaRestJsonWriterInternal.h"` (keep `#include "VaRestJsonNode.h"`). `EmitNode` and `FEmitState` stay.

`Source/VaRestJson/VaRestJson.Build.cs` — the dependency lists become:

```csharp
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",

			// PUBLIC: VaRestJson.h exposes TSharedPtr<FJsonValue>/<FJsonObject> bridges, so a
			// consumer calling them needs Json's include path.
			"Json",

			// PUBLIC: VaRestJsonWriterInternal.h hands the RapidJSON writer type to sibling modules
			// (WebSocket/Socket.IO envelopes). Json re-exports it too (Json.Build.cs:9-15); declared, not inherited.
			"RapidJSON",
		});
```

(the `PrivateDependencyModuleNames` block, which only held `RapidJSON`, is removed).

- [ ] **Step 5: `VaRestJsonBuilder.h` and the reader**

`Source/VaRestJson/Private/VaRestJsonBuilder.h` (the reader's handler, moved and renamed so the encoder can use it):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "RapidJsonIncludes.h"
#include "VaRestJsonDocument.h"

#include <cstdint>

namespace VaRest::Json
{
	/**
	 * SAX handler that builds the arena DOM: the parser's handler, and the struct encoder's DOM sink (it has the
	 * RapidJSON writer's interface, so one encoder serves both). Children are collected on two scratch stacks and
	 * copied into exactly-sized arena vectors when their container closes, so built containers never waste growth
	 * slack. A Start* beyond DepthLimit fails (returns false).
	 */
	struct FDomBuilder
	{
		struct FFrame
		{
			FNode* Container;
			int32 Start; // into Members or Elements
		};

		FDocument& Doc;
		int32 DepthLimit;
		TArray<FFrame> Frames;
		TArray<FMember> Members;
		TArray<FNode*> Elements;
		FNode* Result = nullptr;
		bool bDepthExceeded = false;

		FDomBuilder(FDocument& InDoc, int32 InDepthLimit)
			: Doc(InDoc)
			, DepthLimit(InDepthLimit)
		{
		}

		bool Emit(FNode* Value)
		{
			if (Frames.Num() == 0)
			{
				Result = Value;
			}
			else if (Frames.Last().Container->Type == EVaRestJsonType::Object)
			{
				Members.Last().Value = Value; // its key arrived just before
			}
			else
			{
				Elements.Add(Value);
			}
			return true;
		}

		bool Open(FNode* Container)
		{
			if (Frames.Num() >= DepthLimit)
			{
				bDepthExceeded = true;
				return false; // RapidJSON stops with kParseErrorTermination
			}
			const bool bObject = Container->Type == EVaRestJsonType::Object;
			Frames.Add(FFrame{ Container, bObject ? Members.Num() : Elements.Num() });
			return true;
		}

		static const UTF8CHAR* U(const char* S) { return reinterpret_cast<const UTF8CHAR*>(S); }

		bool Null() { return Emit(Doc.NewNull()); }
		bool Bool(bool b) { return Emit(Doc.NewBool(b)); }
		bool Int(int i) { return Emit(Doc.NewInt(i)); }
		bool Uint(unsigned u) { return Emit(Doc.NewInt(static_cast<int64>(u))); }
		bool Int64(int64_t i) { return Emit(Doc.NewInt(i)); }
		bool Uint64(uint64_t u) { return Emit(Doc.NewUInt(u)); }
		bool Double(double d) { return Emit(Doc.NewDouble(d)); }
		bool RawNumber(const char*, rapidjson::SizeType, bool) { return false; } // kParseNumbersAsStringsFlag is never set

		bool String(const char* S, rapidjson::SizeType Len, bool bCopy)
		{
			return Emit(bCopy ? Doc.NewString(FUtf8StringView(U(S), Len)) : Doc.NewStringRef(U(S), Len));
		}

		bool Key(const char* S, rapidjson::SizeType Len, bool bCopy)
		{
			Members.Add(FMember{ bCopy ? Doc.CopyStr(FUtf8StringView(U(S), Len), true) : FDocument::RefStr(U(S), Len, true), nullptr });
			return true;
		}

		bool StartObject() { return Open(Doc.NewObject()); }
		bool StartArray() { return Open(Doc.NewArray()); }

		bool EndObject(rapidjson::SizeType)
		{
			const FFrame Frame = Frames.Pop(EAllowShrinking::No);
			Doc.AssignObject(*Frame.Container, TConstArrayView<FMember>(Members.GetData() + Frame.Start, Members.Num() - Frame.Start));
			Members.SetNum(Frame.Start, EAllowShrinking::No);
			return Emit(Frame.Container);
		}

		bool EndArray(rapidjson::SizeType)
		{
			const FFrame Frame = Frames.Pop(EAllowShrinking::No);
			Doc.AssignArray(*Frame.Container, TConstArrayView<FNode*>(Elements.GetData() + Frame.Start, Elements.Num() - Frame.Start));
			Elements.SetNum(Frame.Start, EAllowShrinking::No);
			return Emit(Frame.Container);
		}
	};
}
```

Replace `Source/VaRestJson/Private/VaRestJsonReader.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonReader.h"

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "VaRestJsonBuilder.h"

namespace VaRest::Json
{
	namespace ReaderPrivate
	{
		/**
		 * The reader's parse stack goes through FMemory (LLM, Insights and the engine allocator see it) via the
		 * engine's own RapidJSON allocator, UE::Json::Private::FAllocatorImpl (JsonUtils/RapidJsonUtils.h:19-53).
		 * The default rapidjson::CrtAllocator calls RAPIDJSON_MALLOC (allocators.h:86-88), which is std::malloc
		 * (rapidjson.h:694-696) because RapidJsonIncludes.h:29-44 keeps its FMemory override disabled.
		 * The allocator is the reader's StackAllocator argument (reader.h:538, :547).
		 */
		using FReader = rapidjson::GenericReader<rapidjson::UTF8<>, rapidjson::UTF8<>, UE::Json::Private::FAllocatorImpl>;

		template <unsigned Flags>
		rapidjson::ParseResult Run(char* Text, FDomBuilder& Handler, int64& OutConsumed)
		{
			rapidjson::InsituStringStream Stream(Text);
			UE::Json::Private::FAllocatorImpl StackAllocator;
			FReader Reader(&StackAllocator);
			const rapidjson::ParseResult Result = Reader.Parse<Flags>(Stream, Handler);
			OutConsumed = static_cast<int64>(Stream.Tell());
			return Result;
		}

		constexpr unsigned BaseFlags = rapidjson::kParseInsituFlag | rapidjson::kParseIterativeFlag
			| rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag;
		static_assert((BaseFlags & rapidjson::kParseIterativeFlag) != 0, "The reader must stay iterative: recursive parsing overflows the stack on hostile depth");
		constexpr unsigned LenientFlags = rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag;
	}

	int64 BomLength(const uint8* Bytes, int64 Num)
	{
		return Num >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF ? 3 : 0;
	}

	void FillLineColumn(FVaRestJsonError& Error, FUtf8StringView Original)
	{
		if (Original.IsEmpty())
		{
			return;
		}
		// Columns count code points (bytes that are not 10xxxxxx continuation bytes): "é" is one column, not two.
		// A leading BOM is not text and is not counted; Offset stays a byte offset into the input.
		const int64 Limit = FMath::Min<int64>(Error.Offset, Original.Len());
		const int64 Start = BomLength(reinterpret_cast<const uint8*>(Original.GetData()), Original.Len());
		int32 Line = 1;
		int32 Column = 1;
		for (int64 i = Start; i < Limit; ++i)
		{
			const uint8 Byte = static_cast<uint8>(Original[i]);
			if (Byte == '\n')
			{
				++Line;
				Column = 1;
			}
			else if ((Byte & 0xC0) != 0x80)
			{
				++Column;
			}
		}
		Error.Line = Line;
		Error.Column = Column;
	}

	bool ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options,
		FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed)
	{
		using namespace ReaderPrivate;
		check(Doc.Input.Num() > 0 && Doc.Input.Last() == 0);
		const int32 DepthLimit = FMath::Clamp(Options.MaxDepth, 1, MaxDepth); // spec 2.2: one limit, so anything that parses also writes
		FDomBuilder Handler(Doc, DepthLimit);
		char* Text = reinterpret_cast<char*>(Doc.Input.GetData() + StartOffset);

		const bool bLenient = Options.bAllowCommentsAndTrailingCommas;
		const bool bPrefix = OutConsumed != nullptr;
		int64 Consumed = 0;
		const rapidjson::ParseResult Result =
			  bLenient && bPrefix ? Run<BaseFlags | LenientFlags | rapidjson::kParseStopWhenDoneFlag>(Text, Handler, Consumed)
			: bLenient            ? Run<BaseFlags | LenientFlags>(Text, Handler, Consumed)
			: bPrefix             ? Run<BaseFlags | rapidjson::kParseStopWhenDoneFlag>(Text, Handler, Consumed)
			:                       Run<BaseFlags>(Text, Handler, Consumed);

		if (Result.IsError())
		{
			OutError = FVaRestJsonError();
			OutError.Offset = StartOffset + static_cast<int64>(Result.Offset());
			OutError.Message = Handler.bDepthExceeded
				? FString::Printf(TEXT("Nesting deeper than %d"), DepthLimit)
				: FString(rapidjson::GetParseError_En(Result.Code()));
			FillLineColumn(OutError, OriginalForDiagnostics);
			return false;
		}

		// A raw NUL ends RapidJSON's iterative loop (reader.h:2199) as if the document had ended; reject what follows it
		if (!bPrefix && StartOffset + Consumed != Doc.Input.Num() - 1)
		{
			OutError = FVaRestJsonError();
			OutError.Offset = StartOffset + Consumed;
			OutError.Message = TEXT("Unexpected NUL byte in input");
			FillLineColumn(OutError, OriginalForDiagnostics);
			return false;
		}

		Doc.Root = Handler.Result;
		if (OutConsumed)
		{
			*OutConsumed = Consumed;
		}
		return true;
	}
}
```

- [ ] **Step 6: Encode API** — in `VaRestJsonConvert.h`, add `#include "Containers/ArrayView.h"` and `#include "Templates/Function.h"` to the includes, then before `struct FVaRestJsonConvertOptions`:

```cpp
/**
 * Encode: offered every TArray<uint8> value (a field, or bytes inside a struct in a container) before the built-in
 * forms (an array of numbers; a base64 string with bBytesAsBase64). Return true after setting OutReplacement: the
 * encoder writes OutReplacement in place of the bytes through the same sink (RapidJSON writer or DOM builder),
 * counting its depth. Return false for the built-in forms. Called in walk order (field declaration order, then
 * container order), so a caller that numbers the values gets a deterministic numbering; empty arrays are offered too.
 * Socket.IO writes {"_placeholder":true,"num":N} and keeps a copy of Bytes (spec §18.2 J-1). Runs on the encoding thread.
 */
using FVaRestJsonBytesWriter = TFunction<bool(TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement)>;
```

and as the last member of `FVaRestJsonConvertOptions`:

```cpp
	/** Encode: per-value replacement for TArray<uint8> values (see FVaRestJsonBytesWriter); unset: the built-in forms */
	FVaRestJsonBytesWriter BytesWriter;
```

Then inside `struct FVaRestJsonConvert`, before `ResetCache`:

```cpp
	// ---- encode: straight to UTF-8 bytes with the RapidJSON writer, no DOM, no FString (spec 2.4)

	/**
	 * Appends Struct's JSON to Out. False when a value nests deeper than VaRest::Json::MaxDepth: Out is left as it was
	 * and OutError is filled (or, without OutError, an error is logged).
	 */
	static bool StructToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed,
		FVaRestJsonError* OutError = nullptr);

	/** StructToUtf8 as an FString; "" plus an error log on failure */
	static FString StructToString(const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed);

	/** Struct as a DOM (to Set into another document); an invalid handle plus an error log on failure */
	static FVaRestJson StructToJson(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions());

	/** One property's value as a DOM; Value points at the value, not at its container (the BP wildcard nodes' encode) */
	static FVaRestJson PropertyToJson(const FProperty* Property, const void* Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions());

	template <typename T>
	static TArray<uint8> ToUtf8(const T& Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed)
	{
		TArray<uint8> Out;
		StructToUtf8(TBaseStructure<T>::Get(), &Value, Out, Options, Format);
		return Out;
	}

	template <typename T>
	static FVaRestJson ToJson(const T& Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions())
	{
		return StructToJson(TBaseStructure<T>::Get(), &Value, Options);
	}
```

In `VaRestJsonConvertPrivate.h` add to the namespace:

```cpp
	/**
	 * The double to write for a float: the shortest decimal (6 to 9 significant digits) that reads back as the same
	 * float, so 0.1f is written 0.1 and not 0.10000000149011612. Falls back to the exact double outside 1e-22..1e22.
	 */
	double FloatToJsonDouble(float Value);
```

- [ ] **Step 7: Encoder** — `Source/VaRestJson/Private/VaRestJsonConvertEncode.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvert.h"

#include "Misc/Base64.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonBuilder.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonSax.h"
#include "VaRestJsonUtf8.h"
#include "VaRestJsonWriterInternal.h"

namespace VaRest::Json::Convert
{
	double FloatToJsonDouble(float Value)
	{
		const double Exact = static_cast<double>(Value);
		if (!FMath::IsFinite(Value) || Value == 0.0f)
		{
			return Exact;
		}
		// Powers of ten up to 10^22 are exact doubles, so Round(x * 10^k) / 10^k is the double nearest that decimal; the
		// check keeps only a candidate that converts back to the very same float.
		static constexpr double Pow10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
			1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
		const int32 Exponent = FMath::FloorToInt32(FMath::LogX(10.0, FMath::Abs(Exact)));
		for (int32 Digits = 6; Digits <= 9; ++Digits) // 9 significant digits always round-trip a float
		{
			const int32 Shift = Digits - 1 - Exponent;
			if (Shift > 22 || Shift < -22)
			{
				break;
			}
			const double Scale = Pow10[FMath::Abs(Shift)];
			const double Candidate = Shift >= 0 ? FMath::RoundToDouble(Exact * Scale) / Scale : FMath::RoundToDouble(Exact / Scale) * Scale;
			if (static_cast<float>(Candidate) == Value)
			{
				return Candidate;
			}
		}
		return Exact;
	}

	namespace EncodePrivate
	{
		/** RapidJSON asserts on a null string pointer (writer.h:198, :205); an empty view may carry one */
		const char EmptyText[1] = { 0 };

		struct FEncodeContext
		{
			const FVaRestJsonConvertOptions& Options;
			/** Containers open around the value being written: the count the parser and EmitNode limit */
			int32 Depth = 0;
			/** First failure; any failure fails the whole encode (spec 2.2) */
			FString Error;
			int32 NonFinite = 0;
			FUtf8Scratch Scratch;

			explicit FEncodeContext(const FVaRestJsonConvertOptions& InOptions)
				: Options(InOptions)
			{
			}

			bool Enter()
			{
				if (Depth >= MaxDepth)
				{
					Fail(FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
					return false;
				}
				++Depth;
				return true;
			}

			void Leave()
			{
				--Depth;
			}

			void Fail(FString Message)
			{
				if (Error.IsEmpty())
				{
					Error = MoveTemp(Message);
				}
			}
		};

		template <typename SinkType>
		void WriteUtf8Text(SinkType& Sink, FUtf8StringView Text)
		{
			Sink.String(Text.Len() ? reinterpret_cast<const char*>(Text.GetData()) : EmptyText, static_cast<rapidjson::SizeType>(Text.Len()), true);
		}

		template <typename SinkType>
		void WriteText(SinkType& Sink, FStringView Text, FEncodeContext& Context)
		{
			// The sink copies at once (writer output, or FDomBuilder::String with copy = true), so Scratch is free again after
			WriteUtf8Text(Sink, ConvertForDocument(Text, Context.Scratch));
		}

		template <typename SinkType>
		void WriteKey(SinkType& Sink, FUtf8StringView Key)
		{
			Sink.Key(Key.Len() ? reinterpret_cast<const char*>(Key.GetData()) : EmptyText, static_cast<rapidjson::SizeType>(Key.Len()), true);
		}

		template <typename SinkType>
		void WriteEnum(SinkType& Sink, const UEnum* Enum, int64 Value, FEncodeContext& Context)
		{
			if (!Context.Options.bEnumsAsNumbers)
			{
				const int32 Index = Enum->GetIndexByValue(Value);
				if (Index != INDEX_NONE)
				{
					// The authored name, as FJsonObjectConverter writes it (JsonObjectConverter.cpp:109-114): a C++
					// enumerator's short name, a Blueprint enum's display text (UserDefinedEnum.cpp:208-220)
					WriteText(Sink, Enum->GetAuthoredNameStringByIndex(Index), Context);
					return;
				}
				// A value with no enumerator (a bitmask, a cast) keeps its data as a number
			}
			Sink.Int64(Value);
		}

		template <typename SinkType>
		void WriteNumber(SinkType& Sink, const FNumericProperty* Numeric, const void* Value, FEncodeContext& Context)
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				WriteEnum(Sink, Enum, Numeric->GetSignedIntPropertyValue(Value), Context);
				return;
			}
			if (Numeric->IsFloatingPoint())
			{
				const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Numeric);
				const double D = FloatProperty ? FloatToJsonDouble(FloatProperty->GetPropertyValue(Value)) : Numeric->GetFloatingPointPropertyValue(Value);
				if (FMath::IsFinite(D))
				{
					Sink.Double(D);
				}
				else
				{
					Sink.Null(); // JSON has no NaN/Infinity; one warning per encode (Finish)
					++Context.NonFinite;
				}
				return;
			}
			if (IsUnsignedInteger(Numeric))
			{
				Sink.Uint64(Numeric->GetUnsignedIntPropertyValue(Value));
			}
			else
			{
				Sink.Int64(Numeric->GetSignedIntPropertyValue(Value));
			}
		}

		/** A map key as JSON object key text */
		bool MapKeyText(const FProperty* KeyProperty, const void* Key, FEncodeContext& Context, FString& Out)
		{
			if (const FStrProperty* StrKey = CastField<FStrProperty>(KeyProperty))
			{
				Out = StrKey->GetPropertyValue(Key);
				return true;
			}
			if (const FNameProperty* NameKey = CastField<FNameProperty>(KeyProperty))
			{
				Out = NameKey->GetPropertyValue(Key).ToString();
				return true;
			}
			const UEnum* Enum = nullptr;
			int64 EnumValue = 0;
			if (const FEnumProperty* EnumKey = CastField<FEnumProperty>(KeyProperty))
			{
				Enum = EnumKey->GetEnum();
				EnumValue = EnumKey->GetUnderlyingProperty()->GetSignedIntPropertyValue(Key);
			}
			else if (const FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty))
			{
				if (const UEnum* ByteEnum = NumericKey->GetIntPropertyEnum())
				{
					Enum = ByteEnum;
					EnumValue = NumericKey->GetSignedIntPropertyValue(Key);
				}
				else
				{
					Out = IsUnsignedInteger(NumericKey) ? LexToString(NumericKey->GetUnsignedIntPropertyValue(Key)) : LexToString(NumericKey->GetSignedIntPropertyValue(Key));
					return true;
				}
			}
			if (Enum)
			{
				const int32 Index = Context.Options.bEnumsAsNumbers ? INDEX_NONE : Enum->GetIndexByValue(EnumValue);
				Out = Index != INDEX_NONE ? Enum->GetAuthoredNameStringByIndex(Index) : LexToString(EnumValue);
				return true;
			}
			Context.Fail(FString::Printf(TEXT("a map key of type %s cannot be a JSON key"), *KeyProperty->GetClass()->GetName()));
			return false;
		}

		template <typename SinkType>
		bool EncodeValue(SinkType& Sink, const FProperty* Property, const void* Value, FEncodeContext& Context);

		/** An FVaRestJson value through the sink at the current depth; a missing handle is null (spec 2.2) */
		template <typename SinkType>
		bool EmitJson(SinkType& Sink, const FVaRestJson& Json, FEncodeContext& Context)
		{
			if (!Json.IsValid())
			{
				Sink.Null(); // inside a container; a missing struct FIELD is omitted before this point
				return true;
			}
			FEmitState State;
			const bool bEmitted = EmitNode(Sink, *Json.GetNodeInternal(), Context.Depth, State);
			Context.NonFinite += State.NonFinite;
			if (!bEmitted)
			{
				Context.Fail(FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
			}
			return bEmitted;
		}

		template <typename SinkType>
		bool EncodeSpecial(SinkType& Sink, ESpecialStruct Special, const void* Data, FEncodeContext& Context)
		{
			switch (Special)
			{
			case ESpecialStruct::Json:
				return EmitJson(Sink, *static_cast<const FVaRestJson*>(Data), Context);
			case ESpecialStruct::DateTime:
				WriteText(Sink, static_cast<const FDateTime*>(Data)->ToIso8601(), Context);
				return true;
			case ESpecialStruct::Timespan:
				Sink.Double(static_cast<const FTimespan*>(Data)->GetTotalSeconds());
				return true;
			case ESpecialStruct::Guid:
				WriteText(Sink, static_cast<const FGuid*>(Data)->ToString(EGuidFormats::DigitsWithHyphens), Context);
				return true;
			case ESpecialStruct::SoftObjectPath:
				WriteText(Sink, static_cast<const FSoftObjectPath*>(Data)->ToString(), Context);
				return true;
			case ESpecialStruct::None:
				break;
			}
			return true;
		}

		/** Spec 2.4 / 2.2: an unset TOptional and a missing FVaRestJson are a missing key, not null */
		bool IsOmitted(const FProperty* Property, const void* Value)
		{
			if (Property->ArrayDim != 1)
			{
				return false; // a C-style array member is always written; an invalid element is null
			}
			if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
			{
				return !Optional->IsSet(Value);
			}
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			return StructProperty && StructProperty->Struct == FVaRestJson::StaticStruct() && !static_cast<const FVaRestJson*>(Value)->IsValid();
		}

		template <typename SinkType>
		bool EncodeField(SinkType& Sink, const FProperty* Property, const void* Value, FEncodeContext& Context)
		{
			if (Property->ArrayDim == 1)
			{
				return EncodeValue(Sink, Property, Value, Context);
			}
			// A C-style array member (int32 Values[3]) is a JSON array of its ArrayDim elements
			if (!Context.Enter())
			{
				return false;
			}
			Sink.StartArray();
			for (int32 i = 0; i < Property->ArrayDim; ++i)
			{
				if (!EncodeValue(Sink, Property, static_cast<const uint8*>(Value) + static_cast<SIZE_T>(Property->GetElementSize()) * i, Context))
				{
					return false;
				}
			}
			Sink.EndArray(0);
			Context.Leave();
			return true;
		}

		/** The key/value pairs of Data's fields, in field-map order; no braces (the object is open around them) */
		template <typename SinkType>
		bool EncodeMembers(SinkType& Sink, const FFieldMap& Fields, const void* Data, FEncodeContext& Context)
		{
			for (const FFieldInfo& Field : Fields.Fields)
			{
				const void* Value = Field.Property->ContainerPtrToValuePtr<void>(Data);
				if (IsOmitted(Field.Property, Value))
				{
					continue;
				}
				WriteKey(Sink, Field.NameView());
				if (!EncodeField(Sink, Field.Property, Value, Context))
				{
					return false;
				}
			}
			return true;
		}

		template <typename SinkType>
		bool EncodeStruct(SinkType& Sink, const UScriptStruct* Struct, const void* Data, FEncodeContext& Context)
		{
			const ESpecialStruct Special = ClassifyStruct(Struct);
			if (Special != ESpecialStruct::None)
			{
				return EncodeSpecial(Sink, Special, Data, Context);
			}
			if (!Context.Enter())
			{
				return false;
			}
			const FFieldMapRef Fields = GetFieldMap(Struct, Context.Options.Naming);
			Sink.StartObject();
			if (!EncodeMembers(Sink, *Fields, Data, Context))
			{
				return false;
			}
			Sink.EndObject(0);
			Context.Leave();
			return true;
		}

		template <typename SinkType>
		bool EncodeValue(SinkType& Sink, const FProperty* Property, const void* Value, FEncodeContext& Context)
		{
			if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				Sink.Bool(BoolProperty->GetPropertyValue(Value));
				return true;
			}
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				WriteEnum(Sink, EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value), Context);
				return true;
			}
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
			{
				WriteNumber(Sink, Numeric, Value, Context);
				return true;
			}
			if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
			{
				WriteText(Sink, StrProperty->GetPropertyValue(Value), Context);
				return true;
			}
			if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				WriteText(Sink, NameProperty->GetPropertyValue(Value).ToString(), Context);
				return true;
			}
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				// Culture-invariant (spec 2.4): ToString() would give the current culture's display text
				WriteText(Sink, TextProperty->GetPropertyValue(Value).BuildSourceString(), Context);
				return true;
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				return EncodeStruct(Sink, StructProperty->Struct, Value, Context);
			}
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(ArrayProperty, Value);
				if (Context.Options.BytesWriter && IsPlainByteArray(ArrayProperty))
				{
					// Socket.IO attachments (spec §18.2 J-1): the caller's replacement, written in place at this depth
					const TConstArrayView<uint8> Bytes(Helper.Num() ? Helper.GetRawPtr(0) : nullptr, Helper.Num());
					FVaRestJson Replacement;
					if (Context.Options.BytesWriter(Bytes, Replacement))
					{
						return EmitJson(Sink, Replacement, Context);
					}
				}
				if (Context.Options.bBytesAsBase64 && IsPlainByteArray(ArrayProperty))
				{
					WriteText(Sink, Helper.Num() ? FBase64::Encode(Helper.GetRawPtr(0), static_cast<uint32>(Helper.Num())) : FString(), Context);
					return true;
				}
				if (!Context.Enter())
				{
					return false;
				}
				Sink.StartArray();
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					if (!EncodeValue(Sink, ArrayProperty->Inner, Helper.GetRawPtr(i), Context))
					{
						return false;
					}
				}
				Sink.EndArray(0);
				Context.Leave();
				return true;
			}
			if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				FScriptSetHelper Helper(SetProperty, Value);
				if (!Context.Enter())
				{
					return false;
				}
				Sink.StartArray();
				for (FScriptSetHelper::FIterator It(Helper); It; ++It)
				{
					if (!EncodeValue(Sink, SetProperty->ElementProp, Helper.GetElementPtr(It), Context))
					{
						return false;
					}
				}
				Sink.EndArray(0);
				Context.Leave();
				return true;
			}
			if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper Helper(MapProperty, Value);
				if (!Context.Enter())
				{
					return false;
				}
				Sink.StartObject();
				FString KeyText;
				for (FScriptMapHelper::FIterator It(Helper); It; ++It)
				{
					if (!MapKeyText(MapProperty->KeyProp, Helper.GetKeyPtr(It), Context, KeyText))
					{
						return false;
					}
					WriteKey(Sink, ConvertForDocument(KeyText, Context.Scratch));
					if (!EncodeValue(Sink, MapProperty->ValueProp, Helper.GetValuePtr(It), Context))
					{
						return false;
					}
				}
				Sink.EndObject(0);
				Context.Leave();
				return true;
			}
			if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
			{
				const void* Inner = Optional->GetValuePointerForReadIfSet(Value);
				if (!Inner)
				{
					Sink.Null(); // inside a container; an unset optional FIELD is omitted before this point
					return true;
				}
				return EncodeValue(Sink, Optional->GetValueProperty(), Inner, Context);
			}
			if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
			{
				WriteText(Sink, SoftProperty->GetPropertyValue(Value).ToSoftObjectPath().ToString(), Context);
				return true;
			}
			Context.Fail(FString::Printf(TEXT("%s properties are not converted"), *Property->GetClass()->GetName()));
			return false;
		}

		bool Finish(bool bEncoded, const FEncodeContext& Context, const FString& What, FVaRestJsonError* OutError)
		{
			if (!bEncoded)
			{
				const FString Message = FString::Printf(TEXT("FVaRestJsonConvert: cannot encode %s: %s"), *What, *Context.Error);
				if (OutError)
				{
					*OutError = FVaRestJsonError();
					OutError->Message = Message;
				}
				else
				{
					UE_LOG(LogVaRestJson, Error, TEXT("%s"), *Message);
				}
				return false;
			}
			if (Context.NonFinite > 0)
			{
				UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), Context.NonFinite);
			}
			return true;
		}

		/** Runs Body against a fresh document's DOM builder */
		template <typename BodyType>
		FVaRestJson BuildDocument(BodyType&& Body)
		{
			FVaRestJson::FDocumentRef NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
			FDomBuilder Builder(*NewDoc, MaxDepth);
			if (!Body(Builder))
			{
				return FVaRestJson();
			}
			NewDoc->Root = Builder.Result;
			FNode* Root = NewDoc->Root;
			return FVaRestJson::MakeInternal(MoveTemp(NewDoc), Root);
		}

		template <typename WriterType>
		bool WriteMembers(WriterType& Writer, const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options,
			int32 OpenContainers, FVaRestJsonError* OutError)
		{
			FEncodeContext Context(Options);
			if (!Struct || !Data)
			{
				Context.Fail(TEXT("no struct to encode"));
				return Finish(false, Context, TEXT("a null struct"), OutError);
			}
			if (ClassifyStruct(Struct) != ESpecialStruct::None)
			{
				Context.Fail(TEXT("it is written as a single value and has no members"));
				return Finish(false, Context, Struct->GetName(), OutError);
			}
			Context.Depth = FMath::Max(0, OpenContainers);
			return Finish(EncodeMembers(Writer, *GetFieldMap(Struct, Options.Naming), Data, Context), Context, Struct->GetName(), OutError);
		}
	}
}

bool VaRest::Json::WriteStructMembersInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	return Convert::EncodePrivate::WriteMembers(Writer, Struct, Data, Options, OpenContainers, OutError);
}

bool VaRest::Json::WriteStructMembersInternal(FPrettyWriter& Writer, const UScriptStruct* Struct, const void* Data,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	return Convert::EncodePrivate::WriteMembers(Writer, Struct, Data, Options, OpenContainers, OutError);
}

bool VaRest::Json::WriteJsonInternal(FCondensedWriter& Writer, const FVaRestJson& Value, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert::EncodePrivate;
	const FVaRestJsonConvertOptions Defaults;
	FEncodeContext Context(Defaults);
	Context.Depth = FMath::Max(0, OpenContainers);
	return Finish(EmitJson(Writer, Value, Context), Context, TEXT("a VaRest Json value"), OutError);
}

bool VaRest::Json::WriteJsonMembersInternal(FCondensedWriter& Writer, const FVaRestJson& Object, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert::EncodePrivate;
	const FVaRestJsonConvertOptions Defaults;
	FEncodeContext Context(Defaults);
	if (!Object.IsObject())
	{
		Context.Fail(TEXT("it is not an object"));
		return Finish(false, Context, TEXT("a VaRest Json value"), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	// The members as stored, duplicates included, exactly as EmitNode writes an object's body
	const FNode& Source = *Object.GetNodeInternal();
	bool bEmitted = true;
	for (uint32 i = 0; i < Source.Obj.Num && bEmitted; ++i)
	{
		const FMember& Member = Source.Obj.Members[i];
		WriteKey(Writer, Member.Key.View());
		FEmitState State;
		bEmitted = EmitNode(Writer, *Member.Value, Context.Depth, State);
		Context.NonFinite += State.NonFinite;
	}
	if (!bEmitted)
	{
		Context.Fail(FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
	}
	return Finish(bEmitted, Context, TEXT("a VaRest Json value"), OutError);
}

bool VaRest::Json::WriteStructInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		return Finish(false, Context, TEXT("a null struct"), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	return Finish(EncodeStruct(Writer, Struct, Data, Context), Context, Struct->GetName(), OutError);
}

bool VaRest::Json::WritePropertyInternal(FCondensedWriter& Writer, const FProperty* Property, const void* Value,
	const FVaRestJsonConvertOptions& Options, int32 OpenContainers, FVaRestJsonError* OutError)
{
	using namespace Convert;
	using namespace Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Property || !Value)
	{
		Context.Fail(TEXT("no value to encode"));
		return Finish(false, Context, TEXT("a null property"), OutError);
	}
	FString Why;
	if (!IsSupported(Property, Why))
	{
		Context.Fail(Why);
		return Finish(false, Context, Property->GetName(), OutError);
	}
	Context.Depth = FMath::Max(0, OpenContainers);
	return Finish(EncodeField(Writer, Property, Value, Context), Context, Property->GetName(), OutError);
}

bool FVaRestJsonConvert::StructToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
	const FVaRestJsonConvertOptions& Options, EVaRestJsonFormat Format, FVaRestJsonError* OutError)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		return Finish(false, Context, TEXT("a null struct"), OutError);
	}
	const int32 Start = Out.Num();
	FUtf8Sink Sink{ Out };
	FWriterAllocator StackAllocator;
	bool bEncoded = false;
	if (Format == EVaRestJsonFormat::Pretty)
	{
		FPrettyWriter Writer(Sink, &StackAllocator);
		Writer.SetIndent(' ', 2);
		bEncoded = EncodeStruct(Writer, Struct, Data, Context);
	}
	else
	{
		FCondensedWriter Writer(Sink, &StackAllocator);
		bEncoded = EncodeStruct(Writer, Struct, Data, Context);
	}
	if (!bEncoded)
	{
		Out.SetNum(Start, EAllowShrinking::No);
	}
	return Finish(bEncoded, Context, Struct->GetName(), OutError);
}

FString FVaRestJsonConvert::StructToString(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options, EVaRestJsonFormat Format)
{
	TArray<uint8> Bytes;
	if (!StructToUtf8(Struct, Data, Bytes, Options, Format))
	{
		return FString();
	}
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}

FVaRestJson FVaRestJsonConvert::StructToJson(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Struct || !Data)
	{
		Context.Fail(TEXT("no struct to encode"));
		Finish(false, Context, TEXT("a null struct"), nullptr);
		return FVaRestJson();
	}
	FVaRestJson Result = BuildDocument([&](FDomBuilder& Builder) { return EncodeStruct(Builder, Struct, Data, Context); });
	return Finish(Result.IsValid(), Context, Struct->GetName(), nullptr) ? Result : FVaRestJson();
}

FVaRestJson FVaRestJsonConvert::PropertyToJson(const FProperty* Property, const void* Value, const FVaRestJsonConvertOptions& Options)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRest::Json::Convert::EncodePrivate;
	FEncodeContext Context(Options);
	if (!Property || !Value)
	{
		Context.Fail(TEXT("no value to encode"));
		Finish(false, Context, TEXT("a null property"), nullptr);
		return FVaRestJson();
	}
	FString Why;
	if (!IsSupported(Property, Why))
	{
		Context.Fail(Why);
		Finish(false, Context, Property->GetName(), nullptr);
		return FVaRestJson();
	}
	FVaRestJson Result = BuildDocument([&](FDomBuilder& Builder) { return EncodeField(Builder, Property, Value, Context); });
	return Finish(Result.IsValid(), Context, Property->GetName(), nullptr) ? Result : FVaRestJson();
}
```

- [ ] **Step 8: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including the nine `VaRest.Json.Convert.Encode.*` tests and every parse test (the reader now uses `FDomBuilder`).

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: struct encode straight to UTF-8 or a DOM through one SAX walker; writer entries, bytes hook

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 8: Decode — tolerant by default, strict on request, every issue with its JSON path (spec §2.4)

Also the WebSocket spec's §19 item 10: a caller-supplied resolver for `TArray<uint8>` fields (Socket.IO binary attachments plug in there).

**Engine facts:** `FScriptSetHelper::AddElement` / `FScriptMapHelper::AddPair` hash and compare with the key property (`UnrealType.h:6208-6236`, `:5402-5440`), so duplicate JSON keys collapse instead of leaving a broken container (the `AddDefaultValue_Invalid_NeedsRehash` + `Rehash` pattern `FJsonObjectConverter` uses, `JsonObjectConverter.cpp:739-797`, would keep both). `FStrProperty` keys compare with `FString ==` (`UnrealType.h:1760-1773`), which ignores case. `FTimespan::FromSeconds` casts to ticks without a range check (`Timespan.h:673-676`), so seconds are range-checked first. `FOptionalPropertyLayout::MarkSetAndGetInitializedValuePointerToReplace` / `MarkUnset` (`PropertyOptional.h:35-73`).

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonConvertDecode.cpp`
- Modify: `Source/VaRestJson/Public/VaRestJsonConvert.h` (decode API, `FVaRestJsonBytesResolver`, `BytesResolver` option)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonConvertDecodeTest.cpp`

**Interfaces:**
- Consumes: Task 1 node helpers, Task 6 field maps (`Match`), Task 7 encode (tests round-trip through it).
- Produces: `using FVaRestJsonBytesResolver = TFunction<bool(const FVaRestJson& Value, const FString& Path, TArray<uint8>& OutBytes)>`; `FVaRestJsonConvertOptions::BytesResolver`; `FVaRestJsonConvert::JsonToStruct(const FVaRestJson&, const UScriptStruct*, void* Data, const FVaRestJsonConvertOptions& = {}, FVaRestJsonDecodeReport* = nullptr) -> bool`; `Utf8ToStruct(TConstArrayView<uint8>, const UScriptStruct*, void*, Options, Report) -> bool`; `JsonToProperty(const FVaRestJson&, const FProperty*, void* Value, Options, Report) -> bool` (true when the top-level value was stored).

Decode rules (spec §2.4 plus decisions): keys match fields exactly, then case-insensitively (ASCII), then normalised; the best tier wins regardless of member order, the last occurrence within a tier — in lenient and strict mode alike (names are tolerant by design; strict governs values). Unknown keys are ignored. An absent field keeps its value. `null` keeps a struct (and FDateTime/FGuid/...), clears arrays/sets/maps, unsets a `TOptional`, is JSON null in an `FVaRestJson` field, and is a mismatch for a scalar. Lenient coercions: a string holding a JSON number into a number field, `0`/`1` and `"true"`/`"false"` into a bool, a number or bool into a string/name/text field, an enumerator name that differs only by case; each coercion and each mismatch is recorded (a warning; an error in strict mode, where coercions are refused). An `FVaRestJson` field receives a `Clone` (a decoded struct never shares a document with the response). Containers are replaced, not appended; a map value or set element that does not decode is skipped; a value that does not fit leaves its field as it was (a set `TOptional` keeps its value). An integer or enum map key must be exactly a JSON integer (`5`, `-5`; not `05`, `5.0`, `1e0`, ` 5`). A string coerced to a number is at most 64 bytes. A `TArray<uint8>` value is offered to `BytesResolver` first, then read as a number array or a base64 string. Without a report, one summary warning per decode is logged.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonConvertDecodeTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Tasks/Task.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

#include <atomic>

namespace VaRestJsonConvertDecodeTest
{
	FVaRestJson ParseText(const UTF8CHAR* Text)
	{
		return FVaRestJson::Parse(FUtf8StringView(Text)).StealValue();
	}

	bool HasIssueAt(const TArray<FVaRestJsonError>& Issues, const TCHAR* Path)
	{
		return Issues.ContainsByPredicate([Path](const FVaRestJsonError& Issue) { return Issue.Path.Equals(Path, ESearchCase::CaseSensitive); });
	}

	FVaRestJsonTestAll MakeFilled()
	{
		FVaRestJsonTestAll All;
		All.bIsActive = true;
		All.Int8Value = -8;
		All.Int16Value = -16;
		All.Int32Value = -32;
		All.Int64Value = MIN_int64;
		All.ByteValue = 255;
		All.Uint16Value = 65535;
		All.Uint32Value = MAX_uint32;
		All.Uint64Value = MAX_uint64;
		All.FloatValue = 0.1f;
		All.DoubleValue = 0.1;
		All.StringValue = TEXT("Ключ \"q\"");
		All.NameValue = FName(TEXT("Nm"));
		All.TextValue = FText::FromString(TEXT("Hello"));
		All.Color = EVaRestJsonTestColor::Green;
		All.Legacy = EVaRestJsonTestLegacy::Second;
		All.Inner.Id = 1;
		All.Inner.Name = TEXT("in");
		All.Numbers = { 1, 2, 3 };
		FVaRestJsonTestInner Item;
		Item.Id = 2;
		Item.Name = TEXT("two");
		All.Items.Add(Item);
		All.Tags.Add(TEXT("a"));
		All.Tags.Add(TEXT("b"));
		All.Scores.Add(TEXT("x"), 10);
		All.ById.Add(5, TEXT("five"));
		All.ByName.Add(FName(TEXT("n")), 2);
		All.ByColor.Add(EVaRestJsonTestColor::Blue, 3);
		All.Extra = FVaRestJson::Object().Set(TEXT("raw"), FVaRestJson::Array(1, 2));
		All.When = FDateTime(2026, 10, 20, 12, 34, 56, 789);
		All.Duration = FTimespan::FromSeconds(90.5);
		All.Guid = FGuid(0x01234567, 0x89ABCDEF, 0x01234567, 0x89ABCDEF);
		All.AssetPath = FSoftObjectPath(TEXT("/Game/Maps/Entry.Entry"));
		All.SoftObject = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/A.A")));
		All.Bytes = { 1, 2, 255 };
		All.MaybeNumber = 5;
		All.FixedArray[0] = 4;
		All.FixedArray[1] = 5;
		All.FixedArray[2] = 6;
		return All;
	}

	void ExpectSame(FAutomationTestBase& Test, const FVaRestJsonTestAll& A, const FVaRestJsonTestAll& B)
	{
		Test.TestTrue(TEXT("bool"), A.bIsActive == B.bIsActive);
		Test.TestTrue(TEXT("signed"), A.Int8Value == B.Int8Value && A.Int16Value == B.Int16Value && A.Int32Value == B.Int32Value && A.Int64Value == B.Int64Value);
		Test.TestTrue(TEXT("unsigned"), A.ByteValue == B.ByteValue && A.Uint16Value == B.Uint16Value && A.Uint32Value == B.Uint32Value && A.Uint64Value == B.Uint64Value);
		Test.TestTrue(TEXT("float is exact"), A.FloatValue == B.FloatValue);
		Test.TestTrue(TEXT("double"), A.DoubleValue == B.DoubleValue);
		Test.TestTrue(TEXT("string"), A.StringValue.Equals(B.StringValue, ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("name"), A.NameValue.ToString().Equals(B.NameValue.ToString(), ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("text"), A.TextValue.ToString().Equals(B.TextValue.ToString(), ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("enums"), A.Color == B.Color && A.Legacy == B.Legacy);
		Test.TestTrue(TEXT("nested"), A.Inner.Id == B.Inner.Id && A.Inner.Name.Equals(B.Inner.Name, ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("arrays"), A.Numbers == B.Numbers && A.Items.Num() == B.Items.Num() && A.Items.Num() > 0 && A.Items[0].Id == B.Items[0].Id);
		Test.TestTrue(TEXT("set"), A.Tags.Num() == B.Tags.Num() && A.Tags.Contains(TEXT("a")) && A.Tags.Contains(TEXT("b")));
		Test.TestTrue(TEXT("maps"), A.Scores.FindRef(TEXT("x")) == B.Scores.FindRef(TEXT("x")) && A.ById.FindRef(5).Equals(B.ById.FindRef(5), ESearchCase::CaseSensitive)
			&& A.ByName.FindRef(FName(TEXT("n"))) == B.ByName.FindRef(FName(TEXT("n"))) && A.ByColor.FindRef(EVaRestJsonTestColor::Blue) == B.ByColor.FindRef(EVaRestJsonTestColor::Blue));
		Test.TestTrue(TEXT("FVaRestJson field"), A.Extra.Equals(B.Extra));
		Test.TestTrue(TEXT("date"), A.When == B.When);
		Test.TestTrue(TEXT("timespan"), A.Duration == B.Duration);
		Test.TestTrue(TEXT("guid"), A.Guid == B.Guid);
		Test.TestTrue(TEXT("soft path"), A.AssetPath == B.AssetPath);
		Test.TestTrue(TEXT("soft object"), A.SoftObject.ToSoftObjectPath() == B.SoftObject.ToSoftObjectPath());
		Test.TestTrue(TEXT("bytes"), A.Bytes == B.Bytes);
		Test.TestTrue(TEXT("optional"), A.MaybeNumber == B.MaybeNumber);
		Test.TestTrue(TEXT("C-style array"), A.FixedArray[0] == B.FixedArray[0] && A.FixedArray[1] == B.FixedArray[1] && A.FixedArray[2] == B.FixedArray[2]);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeRoundTripTest, "VaRest.Json.Convert.Decode.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	const FVaRestJsonTestAll Filled = MakeFilled();
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		FVaRestJsonConvertOptions Options;
		Options.bEnumsAsNumbers = Variant == 1;
		Options.bBytesAsBase64 = Variant == 1;
		TArray<uint8> Bytes;
		TestTrue(TEXT("encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Filled, Bytes, Options));
		FVaRestJsonTestAll Decoded;
		FVaRestJsonDecodeReport Report;
		// Decoded with default options: numeric enums and base64 bytes are accepted either way
		TestTrue(TEXT("decode"), FVaRestJsonConvert::Utf8ToStruct(Bytes, FVaRestJsonTestAll::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &Report));
		TestEqual(TEXT("a round trip has no warnings"), Report.Warnings.Num(), 0);
		ExpectSame(*this, Decoded, Filled);
		TestFalse(TEXT("an FVaRestJson field gets its own document"), Decoded.Extra.GetDocumentInternal() == Filled.Extra.GetDocumentInternal());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeTolerantNamesTest, "VaRest.Json.Convert.Decode.TolerantNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeTolerantNamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestNaming Naming;
	const auto Decode = [&Naming](const UTF8CHAR* Text, const FVaRestJsonConvertOptions& Options)
	{
		Naming = FVaRestJsonTestNaming();
		FVaRestJsonDecodeReport Report;
		FVaRestJsonConvert::JsonToStruct(ParseText(Text), FVaRestJsonTestNaming::StaticStruct(), &Naming, Options, &Report);
		return Report;
	};
	const FVaRestJsonConvertOptions Defaults;
	Decode(UTF8TEXT("{\"userId\":1}"), Defaults);
	TestEqual(TEXT("exact"), Naming.UserId, 1);
	Decode(UTF8TEXT("{\"USERID\":2}"), Defaults);
	TestEqual(TEXT("case-insensitive"), Naming.UserId, 2);
	Decode(UTF8TEXT("{\"user_id\":3}"), Defaults);
	TestEqual(TEXT("normalised"), Naming.UserId, 3);
	Decode(UTF8TEXT("{\"user_id\":5,\"userId\":6}"), Defaults);
	TestEqual(TEXT("exact wins"), Naming.UserId, 6);
	Decode(UTF8TEXT("{\"userId\":6,\"user_id\":5}"), Defaults);
	TestEqual(TEXT("exact wins even when the tolerant key comes later"), Naming.UserId, 6);
	Decode(UTF8TEXT("{\"userId\":1,\"userId\":9}"), Defaults);
	TestEqual(TEXT("last occurrence within a tier"), Naming.UserId, 9);
	Decode(UTF8TEXT("{\"is_active\":true,\"url_path\":\"/x\",\"http_status2\":7,\"userName\":\"n\"}"), Defaults);
	TestTrue(TEXT("every field by another policy's spelling"), Naming.bIsActive && Naming.URLPath.Equals(TEXT("/x"), ESearchCase::CaseSensitive)
		&& Naming.HTTPStatus2 == 7 && Naming.user_name.Equals(TEXT("n"), ESearchCase::CaseSensitive));
	const FVaRestJsonDecodeReport Unknown = Decode(UTF8TEXT("{\"nope\":1,\"userId\":1}"), Defaults);
	TestEqual(TEXT("unknown keys are ignored silently"), Unknown.Warnings.Num(), 0);

	FVaRestJsonConvertOptions Snake;
	Snake.Naming = EVaRestJsonNaming::SnakeCase;
	Decode(UTF8TEXT("{\"userId\":4}"), Snake);
	TestEqual(TEXT("under snake_case, camelCase is a tolerant match"), Naming.UserId, 4);
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	Decode(UTF8TEXT("{\"USERID\":2}"), Strict);
	TestEqual(TEXT("names stay tolerant in strict mode"), Naming.UserId, 2);

	// Review Focus, decode half of the field-name collision (Task 6 keeps user_id and skips UserId): the shared key
	// fills the surviving field only. A fresh map build logs the skip once.
	FVaRestJsonConvert::ResetCache();
	AddExpectedMessagePlain(TEXT("is already used by another field"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestCollision Collision;
	FVaRestJsonDecodeReport CollisionReport;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"userId\":5}")), FVaRestJsonTestCollision::StaticStruct(), &Collision, Defaults, &CollisionReport);
	TestTrue(TEXT("a key two fields map to fills the first field only"), Collision.user_id == 5 && Collision.UserId == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeCoercionsTest, "VaRest.Json.Convert.Decode.Coercions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeCoercionsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	const FVaRestJson Input = ParseText(UTF8TEXT("{\"int32Value\":\"42\",\"isActive\":1,\"stringValue\":42,\"doubleValue\":\"2.5\",\"int16Value\":\"x\",\"int8Value\":300}"));

	FVaRestJsonTestAll Lenient;
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("lenient decode succeeds"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Lenient, FVaRestJsonConvertOptions(), &Report));
	TestEqual(TEXT("\"42\" -> 42"), Lenient.Int32Value, 42);
	TestTrue(TEXT("1 -> true"), Lenient.bIsActive);
	TestEqualSensitive(TEXT("42 -> \"42\""), Lenient.StringValue, FString(TEXT("42")));
	TestTrue(TEXT("\"2.5\" -> 2.5"), Lenient.DoubleValue == 2.5);
	TestEqual(TEXT("\"x\" keeps the default"), static_cast<int32>(Lenient.Int16Value), 0);
	TestEqual(TEXT("out of range keeps the default"), static_cast<int32>(Lenient.Int8Value), 0);
	TestEqual(TEXT("four coercions and two mismatches are warnings"), Report.Warnings.Num(), 6);
	TestEqual(TEXT("no errors when lenient"), Report.Errors.Num(), 0);
	TestTrue(TEXT("warnings carry the JSON path"), HasIssueAt(Report.Warnings, TEXT("int8Value")) && HasIssueAt(Report.Warnings, TEXT("int32Value")));

	FVaRestJsonTestAll Strict;
	FVaRestJsonDecodeReport StrictReport;
	FVaRestJsonConvertOptions StrictOptions;
	StrictOptions.bStrict = true;
	TestFalse(TEXT("strict decode fails"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Strict, StrictOptions, &StrictReport));
	TestEqual(TEXT("strict: no number from a string"), Strict.Int32Value, 0);
	TestFalse(TEXT("strict: no bool from 1"), Strict.bIsActive);
	TestEqual(TEXT("strict: the same six are errors"), StrictReport.Errors.Num(), 6);
	TestEqual(TEXT("strict: no warnings"), StrictReport.Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeNullTest, "VaRest.Json.Convert.Decode.NullAndMissing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeNullTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestAll All = MakeFilled();
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"inner\":null,\"numbers\":null,\"scores\":null,\"maybeNumber\":null,\"extra\":null,\"when\":null,\"int32Value\":null}")),
		FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestEqual(TEXT("null keeps a struct"), All.Inner.Id, 1);
	TestTrue(TEXT("null keeps a date"), All.When == FDateTime(2026, 10, 20, 12, 34, 56, 789));
	TestEqual(TEXT("null clears an array"), All.Numbers.Num(), 0);
	TestEqual(TEXT("null clears a map"), All.Scores.Num(), 0);
	TestFalse(TEXT("null unsets an optional"), All.MaybeNumber.IsSet());
	TestTrue(TEXT("null into FVaRestJson is JSON null"), All.Extra.IsNull());
	TestEqual(TEXT("null into an int keeps it"), All.Int32Value, -32);
	TestEqual(TEXT("... with one warning"), Report.Warnings.Num(), 1);
	TestTrue(TEXT("absent fields keep their values"), All.Int64Value == MIN_int64 && All.Tags.Num() == 2);

	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"items\":[{\"name\":\"only\"}],\"numbers\":[9]}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestTrue(TEXT("containers are replaced, not appended"), All.Numbers == TArray<int32>({ 9 }) && All.Items.Num() == 1);
	TestEqual(TEXT("a new element starts from defaults"), All.Items.Num() == 1 ? All.Items[0].Id : -1, 0);

	All.MaybeNumber = 5;
	FVaRestJsonDecodeReport Mismatch;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"maybeNumber\":\"x\"}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Mismatch);
	TestTrue(TEXT("a mismatch keeps a set optional"), All.MaybeNumber.IsSet() && *All.MaybeNumber == 5 && Mismatch.Warnings.Num() == 1);
	All.MaybeNumber.Reset();
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"maybeNumber\":\"x\"}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Mismatch);
	TestFalse(TEXT("... and an unset one unset"), All.MaybeNumber.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodePathsTest, "VaRest.Json.Convert.Decode.WarningsHavePaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodePathsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	const FVaRestJson Input = ParseText(UTF8TEXT("{\"items\":[{\"id\":1},{\"id\":\"x\"}],\"byName\":{\"k\":\"y\"}}"));
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestEqual(TEXT("two warnings"), Report.Warnings.Num(), 2);
	TestTrue(TEXT("array element path"), HasIssueAt(Report.Warnings, TEXT("items[1].id")));
	TestTrue(TEXT("map value path"), HasIssueAt(Report.Warnings, TEXT("byName.k")));
	TestFalse(TEXT("a map value that does not decode is skipped"), All.ByName.Contains(FName(TEXT("k"))));

	// Without a report: one summary line per decode, never one per issue
	AddExpectedMessagePlain(TEXT("decoding VaRestJsonTestAll: 2 warning(s), 0 error(s); first at 'items[1].id'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestAll Quiet;
	FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Quiet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeContainersTest, "VaRest.Json.Convert.Decode.Containers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeContainersTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT(
		"{\"tags\":[\"a\",\"a\",\"b\"],\"scores\":{\"a\":1,\"A\":2},\"byId\":{\"5\":\"five\",\"x\":\"bad\",\"07\":\"lead\"},"
		"\"byColor\":{\"Blue\":3,\"1\":4},\"bytes\":\"AQL/\",\"fixedArray\":[1,2,3,4]}")),
		FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestEqual(TEXT("a set collapses duplicates"), All.Tags.Num(), 2);
	// Review Focus: FString map keys ignore case in UE; "a" and "A" become one pair, the last value wins, no broken map
	TestEqual(TEXT("case-variant keys collapse"), All.Scores.Num(), 1);
	TestEqual(TEXT("... the last value wins"), All.Scores.FindRef(TEXT("a")), 2);
	TestTrue(TEXT("integer keys"), All.ById.Num() == 1 && All.ById.FindRef(5).Equals(TEXT("five"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("bad integer keys are warnings"), HasIssueAt(Report.Warnings, TEXT("byId.x")) && HasIssueAt(Report.Warnings, TEXT("byId.07")));
	TestTrue(TEXT("enum keys by name and by number"), All.ByColor.FindRef(EVaRestJsonTestColor::Blue) == 3 && All.ByColor.FindRef(EVaRestJsonTestColor::Green) == 4);
	TestTrue(TEXT("bytes from base64"), All.Bytes == TArray<uint8>({ 1, 2, 255 }));
	TestTrue(TEXT("a C-style array takes what fits"), All.FixedArray[0] == 1 && All.FixedArray[2] == 3);
	TestTrue(TEXT("... and warns about the rest"), HasIssueAt(Report.Warnings, TEXT("fixedArray")));

	FVaRestJsonDecodeReport Bytes;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"bytes\":[1,256,3]}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Bytes);
	TestTrue(TEXT("bytes from numbers; an out-of-range element stays 0"), All.Bytes == TArray<uint8>({ 1, 0, 3 }) && HasIssueAt(Bytes.Warnings, TEXT("bytes[1]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeBytesResolverTest, "VaRest.Json.Convert.Decode.BytesResolver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeBytesResolverTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// A Socket.IO-style binary placeholder: the bytes come from elsewhere (WebSocket spec §19 item 10)
	const TArray<uint8> Attachment = { 9, 8, 7 };
	FString SeenPath;
	FVaRestJsonConvertOptions Options;
	Options.BytesResolver = [&Attachment, &SeenPath](const FVaRestJson& Value, const FString& Path, TArray<uint8>& OutBytes)
	{
		if (!Value[TEXT("_placeholder")].AsBool())
		{
			return false; // not ours: the built-in forms apply
		}
		SeenPath = Path;
		OutBytes = Attachment;
		return true;
	};
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("decodes"), FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"bytes\":{\"_placeholder\":true,\"num\":0}}")), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report));
	TestTrue(TEXT("the resolver supplied the bytes"), All.Bytes == Attachment && Report.Warnings.Num() == 0);
	TestEqualSensitive(TEXT("... for the value's path"), SeenPath, FString(TEXT("bytes")));
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"bytes\":[1,2]}")), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report);
	TestTrue(TEXT("declined: the built-in forms still apply"), All.Bytes == TArray<uint8>({ 1, 2 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeEnumsTest, "VaRest.Json.Convert.Decode.Enums",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeEnumsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	const auto Color = [&All, &Report](const UTF8CHAR* Text, bool bStrictMode)
	{
		All.Color = EVaRestJsonTestColor::Red;
		Report = FVaRestJsonDecodeReport();
		FVaRestJsonConvertOptions Options;
		Options.bStrict = bStrictMode;
		FVaRestJsonConvert::JsonToStruct(ParseText(Text), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report);
		return All.Color;
	};
	TestTrue(TEXT("by name"), Color(UTF8TEXT("{\"color\":\"Green\"}"), false) == EVaRestJsonTestColor::Green && Report.Warnings.Num() == 0);
	TestTrue(TEXT("by name ignoring case, with a warning"), Color(UTF8TEXT("{\"color\":\"green\"}"), false) == EVaRestJsonTestColor::Green && Report.Warnings.Num() == 1);
	TestTrue(TEXT("strict refuses a case-different name"), Color(UTF8TEXT("{\"color\":\"green\"}"), true) == EVaRestJsonTestColor::Red && Report.Errors.Num() == 1);
	TestTrue(TEXT("by value"), Color(UTF8TEXT("{\"color\":2}"), false) == EVaRestJsonTestColor::Blue);
	TestTrue(TEXT("an unknown value keeps the field"), Color(UTF8TEXT("{\"color\":99}"), false) == EVaRestJsonTestColor::Red && Report.Warnings.Num() == 1);
	FVaRestJsonDecodeReport LegacyReport;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"legacy\":\"Second\"}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &LegacyReport);
	TestTrue(TEXT("a byte enum by name"), All.Legacy == EVaRestJsonTestLegacy::Second);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeDepthTest, "VaRest.Json.Convert.Decode.Depth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeDepthTest::RunTest(const FString& Parameters)
{
	// 300 levels of {"children":[...]}, built through Set (unchecked on the way in): 600 containers
	FVaRestJson Root = FVaRestJson::Object();
	FVaRestJson Current = Root;
	for (int32 i = 0; i < 300; ++i)
	{
		Current.Set(TEXT("children"), FVaRestJson::Array(FVaRestJson::Object()));
		Current = Current[TEXT("children")][0];
	}
	FVaRestJsonTestTree Tree;
	FVaRestJsonDecodeReport Report;
	TestFalse(TEXT("too deep to decode, lenient or not"), FVaRestJsonConvert::JsonToStruct(Root, FVaRestJsonTestTree::StaticStruct(), &Tree, FVaRestJsonConvertOptions(), &Report));
	TestTrue(TEXT("one error says why"), Report.Errors.Num() == 1 && Report.Errors[0].Message.Contains(TEXT("deeper than 512")));

	// The limit exists to bound stack use on worker threads (spec 2.2), so run encode and decode there, at the limit
	FVaRestJsonTestTree AtLimit;
	FVaRestJsonTestTree* Cursor = &AtLimit;
	for (int32 i = 0; i < 254; ++i) // 255 objects + 255 arrays = 510 containers
	{
		Cursor->Children.AddDefaulted();
		Cursor = &Cursor->Children[0];
	}
	bool bWorkerEncoded = false;
	bool bWorkerDecoded = false;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [&AtLimit, &bWorkerEncoded, &bWorkerDecoded]
	{
		TArray<uint8> Bytes;
		bWorkerEncoded = FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &AtLimit, Bytes);
		FVaRestJsonTestTree Decoded;
		FVaRestJsonDecodeReport WorkerReport;
		bWorkerDecoded = bWorkerEncoded && FVaRestJsonConvert::Utf8ToStruct(Bytes, FVaRestJsonTestTree::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &WorkerReport);
	}).Wait();
	TestTrue(TEXT("510 containers encode on a worker thread"), bWorkerEncoded);
	TestTrue(TEXT("... and decode there"), bWorkerDecoded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeConcurrentTest, "VaRest.Json.Convert.Decode.ConcurrentReaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeConcurrentTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// One parsed response decoded by many threads at once (spec 2.2 thread contract): reads only, shared field maps
	const FVaRestJsonTestAll Filled = MakeFilled();
	const FVaRestJson Json = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &Filled);
	FVaRestJsonConvert::ResetCache();
	std::atomic<int32> Wrong{ 0 };
	ParallelFor(32, [&Json, &Wrong](int32 Worker)
	{
		for (int32 i = 0; i < 20; ++i)
		{
			FVaRestJsonTestAll Decoded;
			FVaRestJsonDecodeReport Report;
			if (!FVaRestJsonConvert::JsonToStruct(Json, FVaRestJsonTestAll::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &Report)
				|| Decoded.Int64Value != MIN_int64 || Decoded.Items.Num() != 1 || Report.Warnings.Num() != 0)
			{
				++Wrong;
			}
		}
	});
	TestEqual(TEXT("every concurrent decode is right"), Wrong.load(), 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Convert.Decode`
Expected: compile errors (`JsonToStruct`, `Utf8ToStruct`, `BytesResolver` not members).

- [ ] **Step 3: Decode API** — in `VaRestJsonConvert.h`:

Before `struct FVaRestJsonConvertOptions` add:

```cpp
/**
 * Decode: offered every value bound for a TArray<uint8> before the built-in forms (an array of numbers, a base64
 * string). Return true after filling OutBytes; false falls back to the built-in forms. Socket.IO binary attachments
 * plug in here ({"_placeholder":true,"num":0}). Path is the value's JSON path. Runs on the decoding thread. Value is
 * read-only: other threads may be reading the same document (spec 2.2 thread contract).
 */
using FVaRestJsonBytesResolver = TFunction<bool(const FVaRestJson& Value, const FString& Path, TArray<uint8>& OutBytes)>;
```

and add as the last member of `FVaRestJsonConvertOptions`:

```cpp
	/** Decode: per-value source of TArray<uint8> fields (see FVaRestJsonBytesResolver); unset: the built-in forms only */
	FVaRestJsonBytesResolver BytesResolver;
```

Inside `struct FVaRestJsonConvert`, after the encode block:

```cpp
	// ---- decode: bytes -> arena DOM -> struct walk (spec 2.4; a SAX -> struct decoder is M7)

	/**
	 * Fills Data, an instance of Struct, from Json. Keys match fields exactly, then ignoring case, then normalised
	 * (user_id ~ UserId ~ userId), the best match winning whatever the member order; unknown keys are ignored; an absent
	 * field keeps its value; null keeps a struct and clears a container. Lenient coercions ("42" -> 42, 1 -> true) are
	 * refused in strict mode. Every mismatch is recorded with its JSON path in OutReport (without one: one summary
	 * warning per call). False when Json is neither an object nor null, past VaRest::Json::MaxDepth, or in strict mode
	 * on any error; Data may then be partly written. Any thread; Json's document must not be mutated meanwhile.
	 * Untrusted input: [0,0,...] into a TArray of a large struct allocates Count * sizeof(element) (two input bytes per
	 * element), and every distinct string decoded into an FName stays in the name table for the process's life (names
	 * over 1023 characters take the engine's name error path); a string bound for a number field is coerced only up to
	 * 64 bytes.
	 */
	static bool JsonToStruct(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr);

	/** Parse, then JsonToStruct; a syntax error is reported as an Error (with its line and column) */
	static bool Utf8ToStruct(TConstArrayView<uint8> Utf8, const UScriptStruct* Struct, void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr);

	/** One property's value (Value points at the value): the BP wildcard nodes' decode. True when the value was stored. */
	static bool JsonToProperty(const FVaRestJson& Json, const FProperty* Property, void* Value,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), FVaRestJsonDecodeReport* OutReport = nullptr);
```

- [ ] **Step 4: Decoder** — `Source/VaRestJson/Private/VaRestJsonConvertDecode.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonConvert.h"

#include "Misc/Base64.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonWriter.h"

namespace VaRest::Json::Convert::DecodePrivate
{
	struct FSegment
	{
		/** Object key when Index < 0 */
		FUtf8StringView Key;
		int32 Index = -1;
	};

	const TCHAR* TypeName(const FNode& Value)
	{
		switch (Value.Type)
		{
		case EVaRestJsonType::Null:   return TEXT("null");
		case EVaRestJsonType::Bool:   return TEXT("a bool");
		case EVaRestJsonType::Number: return TEXT("a number");
		case EVaRestJsonType::String: return TEXT("a string");
		case EVaRestJsonType::Array:  return TEXT("an array");
		case EVaRestJsonType::Object: return TEXT("an object");
		case EVaRestJsonType::None:   break;
		}
		return TEXT("nothing");
	}

	FString TextOf(const FNode& Value)
	{
		return FString::ConstructFromPtrSize(Value.Str.Data, static_cast<int32>(Value.Str.Len));
	}

	struct FDecodeContext
	{
		const FVaRestJsonConvertOptions& Options;
		/** The document being read: FVaRestJson fields get a Clone of their subtree */
		const FVaRestJson::FDocumentRef& Source;
		FVaRestJsonDecodeReport* Report;
		/** Where the walk is; formatted only when an issue is recorded */
		TArray<FSegment, TInlineAllocator<16>> Path;
		int32 Depth = 0;
		int32 NumWarnings = 0;
		int32 NumErrors = 0;
		/** A failure that stops the whole decode (depth): nothing more is read */
		bool bAborted = false;
		FVaRestJsonError FirstIssue;
		FUtf8Scratch Scratch;

		FDecodeContext(const FVaRestJsonConvertOptions& InOptions, const FVaRestJson::FDocumentRef& InSource, FVaRestJsonDecodeReport* InReport)
			: Options(InOptions)
			, Source(InSource)
			, Report(InReport)
		{
		}

		/** "items[3].name", the syntax FVaRestJson::Path reads */
		FString PathString() const
		{
			FString Out;
			for (const FSegment& Segment : Path)
			{
				if (Segment.Index >= 0)
				{
					Out += FString::Printf(TEXT("[%d]"), Segment.Index);
					continue;
				}
				if (!Out.IsEmpty())
				{
					Out.AppendChar(TEXT('.'));
				}
				Out += FString::ConstructFromPtrSize(Segment.Key.GetData(), Segment.Key.Len());
			}
			return Out;
		}

		void Record(bool bError, FString Message)
		{
			FVaRestJsonError Issue;
			Issue.Message = MoveTemp(Message);
			Issue.Path = PathString();
			if (bError)
			{
				++NumErrors;
			}
			else
			{
				++NumWarnings;
			}
			if (NumErrors + NumWarnings == 1)
			{
				FirstIssue = Issue;
			}
			if (Report)
			{
				(bError ? Report->Errors : Report->Warnings).Add(MoveTemp(Issue));
			}
		}

		/** The value does not fit its field, which keeps its value. Spec 2.4: a warning; an error in strict mode. */
		bool Mismatch(const FNode& Value, const TCHAR* Expected)
		{
			Record(Options.bStrict, FString::Printf(TEXT("expected %s, got %s"), Expected, TypeName(Value)));
			return false;
		}

		/** A lenient coercion that worked ("42" -> 42): recorded like a mismatch (strict mode never coerces) */
		void Coerced(const TCHAR* What)
		{
			Record(Options.bStrict, FString(What));
		}

		bool Enter()
		{
			if (Depth >= MaxDepth)
			{
				Record(true, FString::Printf(TEXT("value nested deeper than %d levels"), MaxDepth));
				bAborted = true;
				return false;
			}
			++Depth;
			return true;
		}

		void Leave()
		{
			--Depth;
		}
	};

	bool DecodeValue(const FNode& Value, const FProperty* Property, void* Address, FDecodeContext& Context);
	bool DecodeStruct(const FNode& Value, const UScriptStruct* Struct, void* Data, FDecodeContext& Context);

	/** The number Value holds; lenient decode also takes the number a string spells ("42"). Holder keeps that parse alive. */
	const FNode* NumberOf(const FNode& Value, const FDecodeContext& Context, FVaRestJson& Holder)
	{
		if (Value.Type == EVaRestJsonType::Number)
		{
			return &Value;
		}
		if (Value.Type != EVaRestJsonType::String || Context.Options.bStrict)
		{
			return nullptr;
		}
		if (Value.Str.Len > 64)
		{
			return nullptr; // no number worth coercing is that long; bounds the parse on hostile input
		}
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(Value.Str.View());
		if (!Parsed.HasValue() || !Parsed.GetValue().IsNumber())
		{
			return nullptr;
		}
		Holder = Parsed.StealValue();
		return Holder.GetNodeInternal();
	}

	bool DecodeBool(const FNode& Value, const FBoolProperty* BoolProperty, void* Address, FDecodeContext& Context)
	{
		if (Value.Type == EVaRestJsonType::Bool)
		{
			BoolProperty->SetPropertyValue(Address, Value.Bool);
			return true;
		}
		if (!Context.Options.bStrict)
		{
			int64 Number = 0;
			if (NodeToInt64(Value, Number) && (Number == 0 || Number == 1))
			{
				BoolProperty->SetPropertyValue(Address, Number == 1);
				Context.Coerced(TEXT("a bool was read from 0/1"));
				return true;
			}
			if (Value.Type == EVaRestJsonType::String)
			{
				const FUtf8StringView Text = Value.Str.View();
				const bool bTrue = Text.Equals(UTF8TEXT("true"), ESearchCase::CaseSensitive);
				if (bTrue || Text.Equals(UTF8TEXT("false"), ESearchCase::CaseSensitive))
				{
					BoolProperty->SetPropertyValue(Address, bTrue);
					Context.Coerced(TEXT("a bool was read from a string"));
					return true;
				}
			}
		}
		return Context.Mismatch(Value, TEXT("a bool"));
	}

	bool DecodeInteger(const FNode& Value, const FNumericProperty* Numeric, void* Address, FDecodeContext& Context)
	{
		FVaRestJson Holder;
		const FNode* Number = NumberOf(Value, Context, Holder);
		bool bStored = false;
		if (Number && IsUnsignedInteger(Numeric))
		{
			uint64 Unsigned = 0;
			if (NodeToUInt64(*Number, Unsigned) && Numeric->CanHoldValue(Unsigned))
			{
				Numeric->SetIntPropertyValue(Address, Unsigned);
				bStored = true;
			}
		}
		else if (Number)
		{
			int64 Signed = 0;
			if (NodeToInt64(*Number, Signed) && Numeric->CanHoldValue(Signed))
			{
				Numeric->SetIntPropertyValue(Address, Signed);
				bStored = true;
			}
		}
		if (!bStored)
		{
			return Context.Mismatch(Value, TEXT("an integer in the field's range"));
		}
		if (Number != &Value)
		{
			Context.Coerced(TEXT("an integer was read from a string"));
		}
		return true;
	}

	bool DecodeFloatingPoint(const FNode& Value, const FNumericProperty* Numeric, void* Address, FDecodeContext& Context)
	{
		FVaRestJson Holder;
		const FNode* Number = NumberOf(Value, Context, Holder);
		double D = 0.0;
		if (!Number || !NodeToDouble(*Number, D))
		{
			return Context.Mismatch(Value, TEXT("a number"));
		}
		if (Numeric->IsA<FFloatProperty>() && FMath::Abs(D) >= FloatRoundsToInfinity)
		{
			return Context.Mismatch(Value, TEXT("a number in float range"));
		}
		Numeric->SetFloatingPointPropertyValue(Address, D);
		if (Number != &Value)
		{
			Context.Coerced(TEXT("a number was read from a string"));
		}
		return true;
	}

	/** Text for a string/name/text field: a string; lenient decode also takes a number's or bool's JSON text */
	bool TextFor(const FNode& Value, FDecodeContext& Context, FString& Out)
	{
		if (Value.Type == EVaRestJsonType::String)
		{
			Out = TextOf(Value);
			return true;
		}
		if (!Context.Options.bStrict && (Value.Type == EVaRestJsonType::Number || Value.Type == EVaRestJsonType::Bool))
		{
			TArray<uint8> Bytes;
			Write(Value, EVaRestJsonFormat::Condensed, Bytes); // a scalar: never too deep
			Out = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
			Context.Coerced(TEXT("a string was read from a number or bool"));
			return true;
		}
		return Context.Mismatch(Value, TEXT("a string"));
	}

	/** By authored name (as encode writes it), exact first; the autogenerated _MAX entry is not a value anyone sends */
	bool FindEnumerator(const UEnum* Enum, const FString& Name, bool bAllowCase, int64& OutValue, bool& bOutCaseOnly)
	{
		const int32 Count = Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0);
		for (int32 i = 0; i < Count; ++i)
		{
			if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::CaseSensitive))
			{
				OutValue = Enum->GetValueByIndex(i);
				bOutCaseOnly = false;
				return true;
			}
		}
		if (bAllowCase)
		{
			for (int32 i = 0; i < Count; ++i)
			{
				if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::IgnoreCase))
				{
					OutValue = Enum->GetValueByIndex(i);
					bOutCaseOnly = true;
					return true;
				}
			}
		}
		return false;
	}

	bool DecodeEnum(const FNode& Value, const UEnum* Enum, const FNumericProperty* Underlying, void* Address, FDecodeContext& Context)
	{
		int64 EnumValue = 0;
		if (Value.Type == EVaRestJsonType::String)
		{
			bool bCaseOnly = false;
			if (!FindEnumerator(Enum, TextOf(Value), !Context.Options.bStrict, EnumValue, bCaseOnly))
			{
				return Context.Mismatch(Value, *FString::Printf(TEXT("an enumerator of %s"), *Enum->GetName()));
			}
			Underlying->SetIntPropertyValue(Address, EnumValue);
			if (bCaseOnly)
			{
				Context.Coerced(TEXT("an enumerator name was matched ignoring case"));
			}
			return true;
		}
		if (NodeToInt64(Value, EnumValue) && Enum->IsValidEnumValue(EnumValue))
		{
			Underlying->SetIntPropertyValue(Address, EnumValue);
			return true;
		}
		return Context.Mismatch(Value, *FString::Printf(TEXT("an enumerator of %s"), *Enum->GetName()));
	}

	/** A JSON object key into a map key: string, name, integer (the whole key a JSON integer), enumerator name or value */
	/**
	 * The whole key is a JSON integer: optional '-', digits, no leading zero, within 64 bits. Not a JSON parse: that
	 * would allocate a document per key and accept " 5", "5.0" and "1e0", collapsing them silently with "5".
	 */
	bool ParseIntegerKey(FUtf8StringView Text, bool& bNegative, uint64& Magnitude)
	{
		bNegative = Text.Len() > 0 && Text[0] == UTF8CHAR('-');
		int32 i = bNegative ? 1 : 0;
		if (i >= Text.Len() || (Text[i] == UTF8CHAR('0') && Text.Len() > i + 1))
		{
			return false;
		}
		Magnitude = 0;
		for (; i < Text.Len(); ++i)
		{
			const uint32 C = static_cast<uint8>(Text[i]);
			if (C < '0' || C > '9')
			{
				return false;
			}
			const uint64 Digit = C - '0';
			if (Magnitude > (MAX_uint64 - Digit) / 10)
			{
				return false;
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		return !bNegative || Magnitude <= 9223372036854775808ull;
	}

	bool DecodeMapKey(FUtf8StringView Text, const FProperty* KeyProperty, void* Key, FDecodeContext& Context)
	{
		const FString KeyText = FString::ConstructFromPtrSize(Text.GetData(), Text.Len());
		if (const FStrProperty* StrKey = CastField<FStrProperty>(KeyProperty))
		{
			StrKey->SetPropertyValue(Key, KeyText);
			return true;
		}
		if (const FNameProperty* NameKey = CastField<FNameProperty>(KeyProperty))
		{
			NameKey->SetPropertyValue(Key, FName(*KeyText));
			return true;
		}
		const FNumericProperty* Underlying = nullptr;
		const UEnum* Enum = nullptr;
		if (const FEnumProperty* EnumKey = CastField<FEnumProperty>(KeyProperty))
		{
			Underlying = EnumKey->GetUnderlyingProperty();
			Enum = EnumKey->GetEnum();
		}
		else if (const FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty))
		{
			Underlying = NumericKey;
			Enum = NumericKey->GetIntPropertyEnum();
		}
		if (Underlying)
		{
			int64 EnumValue = 0;
			bool bCaseOnly = false;
			if (Enum && FindEnumerator(Enum, KeyText, !Context.Options.bStrict, EnumValue, bCaseOnly))
			{
				Underlying->SetIntPropertyValue(Key, EnumValue);
				return true;
			}
			bool bNegative = false;
			uint64 Magnitude = 0;
			if (ParseIntegerKey(Text, bNegative, Magnitude)) // "5", "-5" yes; "05", "5.0", "1e0", " 5", "5x" no
			{
				if (!bNegative && !Enum && IsUnsignedInteger(Underlying) && Underlying->CanHoldValue(Magnitude))
				{
					Underlying->SetIntPropertyValue(Key, Magnitude);
					return true;
				}
				if (bNegative || Magnitude <= static_cast<uint64>(MAX_int64))
				{
					const int64 Signed = !bNegative ? static_cast<int64>(Magnitude)
						: Magnitude == 9223372036854775808ull ? MIN_int64 : -static_cast<int64>(Magnitude);
					if (Enum ? Enum->IsValidEnumValue(Signed) : Underlying->CanHoldValue(Signed))
					{
						Underlying->SetIntPropertyValue(Key, Signed);
						return true;
					}
				}
			}
		}
		Context.Record(Context.Options.bStrict, FString::Printf(TEXT("map key \"%s\" does not fit %s"), *KeyText, *KeyProperty->GetClass()->GetName()));
		return false;
	}

	void AssignBytes(FScriptArrayHelper& Helper, const TArray<uint8>& Bytes)
	{
		Helper.EmptyValues(Bytes.Num());
		Helper.AddValues(Bytes.Num());
		if (Bytes.Num() > 0)
		{
			FMemory::Memcpy(Helper.GetRawPtr(0), Bytes.GetData(), Bytes.Num());
		}
	}

	bool DecodeArray(const FNode& Value, const FArrayProperty* ArrayProperty, void* Address, FDecodeContext& Context)
	{
		FScriptArrayHelper Helper(ArrayProperty, Address);
		const bool bBytes = IsPlainByteArray(ArrayProperty);
		if (bBytes && Context.Options.BytesResolver)
		{
			// WebSocket spec §19 item 10: the caller decides first (Socket.IO attachments); declining keeps the built-in forms
			TArray<uint8> Resolved;
			if (Context.Options.BytesResolver(FVaRestJson::MakeInternal(Context.Source, const_cast<FNode*>(&Value)), Context.PathString(), Resolved))
			{
				AssignBytes(Helper, Resolved);
				return true;
			}
		}
		if (Value.Type == EVaRestJsonType::Null)
		{
			Helper.EmptyValues(); // spec 2.4: null clears a container
			return true;
		}
		if (Value.Type == EVaRestJsonType::String && bBytes)
		{
			TArray<uint8> Decoded;
			if (!FBase64::Decode(TextOf(Value), Decoded))
			{
				return Context.Mismatch(Value, TEXT("base64 text or an array of bytes"));
			}
			AssignBytes(Helper, Decoded);
			return true;
		}
		if (Value.Type != EVaRestJsonType::Array)
		{
			return Context.Mismatch(Value, TEXT("an array"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		// Replaced, not appended: every element starts from the inner type's defaults
		const int32 Count = static_cast<int32>(Value.Arr.Num);
		Helper.EmptyValues(Count);
		Helper.AddValues(Count);
		for (int32 i = 0; i < Count && !Context.bAborted; ++i)
		{
			Context.Path.Add(FSegment{ FUtf8StringView(), i });
			DecodeValue(*Value.Arr.Items[i], ArrayProperty->Inner, Helper.GetRawPtr(i), Context);
			Context.Path.Pop(EAllowShrinking::No);
		}
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeSet(const FNode& Value, const FSetProperty* SetProperty, void* Address, FDecodeContext& Context)
	{
		FScriptSetHelper Helper(SetProperty, Address);
		if (Value.Type == EVaRestJsonType::Null)
		{
			Helper.EmptyElements();
			return true;
		}
		if (Value.Type != EVaRestJsonType::Array)
		{
			return Context.Mismatch(Value, TEXT("an array"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const FProperty* ElementProperty = SetProperty->ElementProp;
		Helper.EmptyElements(static_cast<int32>(Value.Arr.Num));
		void* Element = ElementProperty->AllocateAndInitializeValue();
		for (uint32 i = 0; i < Value.Arr.Num && !Context.bAborted; ++i)
		{
			ElementProperty->DestroyValue(Element);
			ElementProperty->InitializeValue(Element);
			Context.Path.Add(FSegment{ FUtf8StringView(), static_cast<int32>(i) });
			if (DecodeValue(*Value.Arr.Items[i], ElementProperty, Element, Context))
			{
				Helper.AddElement(Element); // hashes and compares: duplicates collapse (UnrealType.h:6208-6236)
			}
			Context.Path.Pop(EAllowShrinking::No);
		}
		ElementProperty->DestroyAndFreeValue(Element);
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeMap(const FNode& Value, const FMapProperty* MapProperty, void* Address, FDecodeContext& Context)
	{
		FScriptMapHelper Helper(MapProperty, Address);
		if (Value.Type == EVaRestJsonType::Null)
		{
			Helper.EmptyValues();
			return true;
		}
		if (Value.Type != EVaRestJsonType::Object)
		{
			return Context.Mismatch(Value, TEXT("an object"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const FProperty* KeyProperty = MapProperty->KeyProp;
		const FProperty* ValueProperty = MapProperty->ValueProp;
		Helper.EmptyValues(static_cast<int32>(Value.Obj.Num));
		void* Key = KeyProperty->AllocateAndInitializeValue();
		void* Item = ValueProperty->AllocateAndInitializeValue();
		for (uint32 m = 0; m < Value.Obj.Num && !Context.bAborted; ++m)
		{
			const FMember& M = Value.Obj.Members[m];
			KeyProperty->DestroyValue(Key);
			KeyProperty->InitializeValue(Key);
			ValueProperty->DestroyValue(Item);
			ValueProperty->InitializeValue(Item);
			Context.Path.Add(FSegment{ M.Key.View(), -1 });
			if (DecodeMapKey(M.Key.View(), KeyProperty, Key, Context) && DecodeValue(*M.Value, ValueProperty, Item, Context))
			{
				// AddPair overwrites an equal key (UnrealType.h:5402-5440): case-variant FString keys collapse, the last value wins
				Helper.AddPair(Key, Item);
			}
			Context.Path.Pop(EAllowShrinking::No);
		}
		KeyProperty->DestroyAndFreeValue(Key);
		ValueProperty->DestroyAndFreeValue(Item);
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeSpecial(const FNode& Value, ESpecialStruct Special, void* Data, FDecodeContext& Context)
	{
		if (Special == ESpecialStruct::Json)
		{
			// Its own copy: a decoded struct never shares a document with the response it came from (spec 2.2 thread contract)
			*static_cast<FVaRestJson*>(Data) = FVaRestJson::MakeInternal(Context.Source, const_cast<FNode*>(&Value)).Clone();
			return true;
		}
		if (Value.Type == EVaRestJsonType::Null)
		{
			return true; // spec 2.4: null keeps a struct's value
		}
		switch (Special)
		{
		case ESpecialStruct::DateTime:
		{
			FDateTime Parsed;
			if (Value.Type == EVaRestJsonType::String && FDateTime::ParseIso8601(*TextOf(Value), Parsed))
			{
				*static_cast<FDateTime*>(Data) = Parsed;
				return true;
			}
			return Context.Mismatch(Value, TEXT("an ISO 8601 date-time"));
		}
		case ESpecialStruct::Timespan:
		{
			FVaRestJson Holder;
			const FNode* Number = NumberOf(Value, Context, Holder);
			double Seconds = 0.0;
			// FromSeconds casts to int64 ticks unchecked (Timespan.h:673-676): range-check hostile input first
			if (Number && NodeToDouble(*Number, Seconds) && Seconds >= FTimespan::MinValue().GetTotalSeconds() && Seconds <= FTimespan::MaxValue().GetTotalSeconds())
			{
				*static_cast<FTimespan*>(Data) = FTimespan::FromSeconds(Seconds);
				if (Number != &Value)
				{
					Context.Coerced(TEXT("seconds were read from a string"));
				}
				return true;
			}
			return Context.Mismatch(Value, TEXT("a number of seconds"));
		}
		case ESpecialStruct::Guid:
		{
			FGuid Parsed;
			if (Value.Type == EVaRestJsonType::String && FGuid::Parse(TextOf(Value), Parsed))
			{
				*static_cast<FGuid*>(Data) = Parsed;
				return true;
			}
			return Context.Mismatch(Value, TEXT("a GUID"));
		}
		case ESpecialStruct::SoftObjectPath:
			if (Value.Type == EVaRestJsonType::String)
			{
				static_cast<FSoftObjectPath*>(Data)->SetPath(FStringView(TextOf(Value)));
				return true;
			}
			return Context.Mismatch(Value, TEXT("an object path"));
		case ESpecialStruct::None:
		case ESpecialStruct::Json:
			break;
		}
		return true;
	}

	/** A field value, including C-style array members (a JSON array of up to ArrayDim elements) */
	bool DecodeField(const FNode& Value, const FProperty* Property, void* Address, FDecodeContext& Context)
	{
		if (Property->ArrayDim == 1)
		{
			return DecodeValue(Value, Property, Address, Context);
		}
		if (Value.Type != EVaRestJsonType::Array)
		{
			return Context.Mismatch(Value, TEXT("an array"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const int32 Count = FMath::Min<int32>(static_cast<int32>(Value.Arr.Num), Property->ArrayDim);
		if (static_cast<int32>(Value.Arr.Num) > Property->ArrayDim)
		{
			Context.Record(Context.Options.bStrict, FString::Printf(TEXT("%u elements for a fixed array of %d; the rest are ignored"), Value.Arr.Num, Property->ArrayDim));
		}
		for (int32 i = 0; i < Count && !Context.bAborted; ++i)
		{
			Context.Path.Add(FSegment{ FUtf8StringView(), i });
			DecodeValue(*Value.Arr.Items[i], Property, static_cast<uint8*>(Address) + static_cast<SIZE_T>(Property->GetElementSize()) * i, Context);
			Context.Path.Pop(EAllowShrinking::No);
		}
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeStruct(const FNode& Value, const UScriptStruct* Struct, void* Data, FDecodeContext& Context)
	{
		const ESpecialStruct Special = ClassifyStruct(Struct);
		if (Special != ESpecialStruct::None)
		{
			return DecodeSpecial(Value, Special, Data, Context);
		}
		if (Value.Type == EVaRestJsonType::Null)
		{
			return true; // spec 2.4: null keeps a struct's values
		}
		if (Value.Type != EVaRestJsonType::Object)
		{
			return Context.Mismatch(Value, TEXT("an object"));
		}
		if (!Context.Enter())
		{
			return false;
		}
		const FFieldMapRef Fields = GetFieldMap(Struct, Context.Options.Naming);
		const int32 FieldCount = Fields->Fields.Num();
		// Per field, the member that feeds it: the best tier wins (exact beats case-insensitive beats normalised, in any
		// member order); within a tier the last occurrence wins, as lookups do (spec 2.1).
		TArray<int32, TInlineAllocator<32>> Chosen;
		TArray<uint8, TInlineAllocator<32>> ChosenTier;
		Chosen.Init(INDEX_NONE, FieldCount);
		ChosenTier.Init(MAX_uint8, FieldCount);
		const FNode::FObj& Obj = Value.Obj;
		for (uint32 m = 0; m < Obj.Num; ++m)
		{
			const FMember& M = Obj.Members[m];
			uint8 Tier = 0;
			const int32 Field = Fields->Match(M.Key.View(), M.Key.Hash, Context.Scratch, Tier);
			if (Field != INDEX_NONE && Tier <= ChosenTier[Field])
			{
				Chosen[Field] = static_cast<int32>(m);
				ChosenTier[Field] = Tier;
			}
		}
		for (int32 f = 0; f < FieldCount && !Context.bAborted; ++f)
		{
			if (Chosen[f] == INDEX_NONE)
			{
				continue; // spec 2.4: an absent field keeps its value
			}
			const FMember& M = Obj.Members[Chosen[f]];
			const FProperty* Property = Fields->Fields[f].Property;
			Context.Path.Add(FSegment{ M.Key.View(), -1 });
			DecodeField(*M.Value, Property, Property->ContainerPtrToValuePtr<void>(Data), Context);
			Context.Path.Pop(EAllowShrinking::No);
		}
		Context.Leave();
		return !Context.bAborted;
	}

	bool DecodeValue(const FNode& Value, const FProperty* Property, void* Address, FDecodeContext& Context)
	{
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return DecodeBool(Value, BoolProperty, Address, Context);
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return DecodeEnum(Value, EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty(), Address, Context);
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				return DecodeEnum(Value, Enum, Numeric, Address, Context);
			}
			return Numeric->IsFloatingPoint() ? DecodeFloatingPoint(Value, Numeric, Address, Context) : DecodeInteger(Value, Numeric, Address, Context);
		}
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			FString Text;
			if (!TextFor(Value, Context, Text))
			{
				return false;
			}
			StrProperty->SetPropertyValue(Address, Text);
			return true;
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString Text;
			if (!TextFor(Value, Context, Text))
			{
				return false;
			}
			NameProperty->SetPropertyValue(Address, FName(*Text));
			return true;
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			FString Text;
			if (!TextFor(Value, Context, Text))
			{
				return false;
			}
			TextProperty->SetPropertyValue(Address, FText::AsCultureInvariant(MoveTemp(Text))); // spec 2.4: culture-invariant
			return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return DecodeStruct(Value, StructProperty->Struct, Address, Context);
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return DecodeArray(Value, ArrayProperty, Address, Context);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			return DecodeSet(Value, SetProperty, Address, Context);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			return DecodeMap(Value, MapProperty, Address, Context);
		}
		if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
		{
			if (Value.Type == EVaRestJsonType::Null)
			{
				Optional->MarkUnset(Address); // spec 2.4: null <-> unset
				return true;
			}
			const bool bWasSet = Optional->IsSet(Address);
			// The existing value when set (PropertyOptional.h:35-56), a fresh one otherwise
			void* Inner = Optional->MarkSetAndGetInitializedValuePointerToReplace(Address);
			if (!DecodeValue(Value, Optional->GetValueProperty(), Inner, Context))
			{
				// A mismatch keeps the field as it was (spec 2.4): unset stays unset, a set value is left untouched
				if (!bWasSet)
				{
					Optional->MarkUnset(Address);
				}
				return false;
			}
			return true;
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (Value.Type == EVaRestJsonType::Null)
			{
				SoftProperty->SetPropertyValue(Address, FSoftObjectPtr());
				return true;
			}
			if (Value.Type == EVaRestJsonType::String)
			{
				SoftProperty->SetPropertyValue(Address, FSoftObjectPtr(FSoftObjectPath(TextOf(Value))));
				return true;
			}
			return Context.Mismatch(Value, TEXT("an object path"));
		}
		Context.Record(true, FString::Printf(TEXT("%s properties are not converted"), *Property->GetClass()->GetName()));
		return false;
	}

	bool Refuse(FVaRestJsonDecodeReport* Report, const FString& Message)
	{
		FVaRestJsonError Error;
		Error.Message = Message;
		if (Report)
		{
			Report->Errors.Add(MoveTemp(Error));
		}
		else
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: %s"), *Message);
		}
		return false;
	}

	bool Finish(const FDecodeContext& Context, bool bStored, const FString& What)
	{
		if (!Context.Report && Context.NumWarnings + Context.NumErrors > 0)
		{
			// One line per decode, not per issue: a big response with a systematic mismatch must not flood the log
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJsonConvert: decoding %s: %d warning(s), %d error(s); first at '%s': %s"),
				*What, Context.NumWarnings, Context.NumErrors, *Context.FirstIssue.Path, *Context.FirstIssue.Message);
		}
		return bStored && !Context.bAborted && Context.NumErrors == 0;
	}
}

bool FVaRestJsonConvert::JsonToStruct(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport)
{
	using namespace VaRest::Json::Convert::DecodePrivate;
	if (!Struct || !Data)
	{
		return Refuse(OutReport, TEXT("no struct to decode into"));
	}
	if (!Json.IsValid())
	{
		return Refuse(OutReport, FString::Printf(TEXT("decoding %s: the value is missing"), *Struct->GetName()));
	}
	FDecodeContext Context(Options, Json.GetDocumentInternal(), OutReport);
	const bool bStored = DecodeStruct(*Json.GetNodeInternal(), Struct, Data, Context);
	return Finish(Context, bStored, Struct->GetName());
}

bool FVaRestJsonConvert::Utf8ToStruct(TConstArrayView<uint8> Utf8, const UScriptStruct* Struct, void* Data, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport)
{
	using namespace VaRest::Json::Convert::DecodePrivate;
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(Utf8);
	if (!Parsed.HasValue())
	{
		if (OutReport)
		{
			OutReport->Errors.Add(Parsed.GetError());
			return false;
		}
		return Refuse(nullptr, FString::Printf(TEXT("cannot decode %s: %s"), Struct ? *Struct->GetName() : TEXT("a null struct"), *Parsed.GetError().ToString()));
	}
	return JsonToStruct(Parsed.GetValue(), Struct, Data, Options, OutReport);
}

bool FVaRestJsonConvert::JsonToProperty(const FVaRestJson& Json, const FProperty* Property, void* Value, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport* OutReport)
{
	using namespace VaRest::Json::Convert::DecodePrivate;
	if (!Property || !Value)
	{
		return Refuse(OutReport, TEXT("no property to decode into"));
	}
	if (!Json.IsValid())
	{
		return Refuse(OutReport, FString::Printf(TEXT("decoding %s: the value is missing"), *Property->GetName()));
	}
	FDecodeContext Context(Options, Json.GetDocumentInternal(), OutReport);
	const bool bStored = DecodeField(*Json.GetNodeInternal(), Property, Value, Context);
	return Finish(Context, bStored, Property->GetName());
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including the ten `VaRest.Json.Convert.Decode.*` tests.

- [ ] **Step 6: Restore-the-bug (tolerant decode order)**

In `DecodeStruct` change `Tier <= ChosenTier[Field]` to `true` (the last matching member wins whatever its tier). Run `Tools/run_tests.sh VaRest.Json.Convert.Decode.TolerantNames`; expect `exact wins even when the tolerant key comes later` to fail. Restore, run, expect green. Report both.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: struct decode -- tolerant names, lenient/strict values, paths on every issue, bytes resolver

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 9: Typed access — `TryGet<T>` / `As<T>` for USTRUCTs, `TArray<T>`, `TMap<FString, T>` (spec §2.3)

**Engine facts:** `TBaseStructure<T>::Get()` is declared only when `T` has `StaticStruct()` or a specialisation exists (`UObject/Class.h:5443-5463`, `:5733-5749` for FVector and the other core variants), so `requires { TBaseStructure<U>::Get(); }` tells a USTRUCT from anything else. `UScriptStruct::InitializeStruct` builds a value the way reflection does, defaults included.

**Files:**
- Modify: `Source/VaRestJson/Public/VaRestJson.h` (TryGet moves out of the class body; `As<T>`; struct/container branches)
- Modify: `Source/VaRestJson/Public/VaRestJsonConvert.h` (`FromJson<T>`), `Source/VaRestJson/Private/VaRestJsonConvertDecode.cpp` (`DecodeStructForTryGet`)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonTypedGetTest.cpp`

**Interfaces:**
- Consumes: `FVaRestJsonConvert::JsonToStruct` (Task 8).
- Produces: `template <typename T> TOptional<T> FVaRestJson::TryGet() const` (adds USTRUCTs and TBaseStructure core types, `TArray<E>`, `TMap<FString, V>`); `template <typename T> T FVaRestJson::As(T Default = T()) const`; `VARESTJSON_API bool VaRest::Json::DecodeStructForTryGet(const FVaRestJson&, const UScriptStruct*, void* Data)`; `template <typename T> static TValueOrError<T, FVaRestJsonError> FVaRestJsonConvert::FromJson(const FVaRestJson&, const FVaRestJsonConvertOptions& = {})`.

Decisions: `TryGet<Struct>` of anything but an object (null included) is unset — the question is "is this a T"; for a value struct (`FDateTime`, `FGuid`, `FTimespan`, soft paths) only its scalar JSON form counts, never `null` or a missing value; one element or value that does not convert makes the whole `TArray`/`TMap` unset; `TMap<FString, T>` keys collapse case-insensitively (FString), the last value winning; `TryGet` decodes with default options (camelCase, lenient) and never logs.

- [ ] **Step 1: Write the failing test** — `Source/VaRestJson/Private/Tests/VaRestJsonTypedGetTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonTypedGetTest, "VaRest.Json.Api.TryGetTyped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonTypedGetTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Parse(FStringView(TEXT(
		"{\"inner\":{\"id\":7,\"name\":\"Neo\"},\"nums\":[1,2,3],\"mixed\":[1,\"x\"],\"items\":[{\"id\":1},{\"id\":2}],"
		"\"scores\":{\"a\":1,\"b\":2},\"bad\":{\"a\":\"x\"},\"pos\":{\"x\":1,\"y\":2,\"z\":3},\"n\":null}"))).StealValue();

	const TOptional<FVaRestJsonTestInner> Inner = J[TEXT("inner")].TryGet<FVaRestJsonTestInner>();
	TestTrue(TEXT("a USTRUCT"), Inner.IsSet() && Inner->Id == 7 && Inner->Name.Equals(TEXT("Neo"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("null is not a struct"), J[TEXT("n")].TryGet<FVaRestJsonTestInner>().IsSet());
	TestFalse(TEXT("a missing value is not a struct"), J[TEXT("nope")].TryGet<FVaRestJsonTestInner>().IsSet());
	TestFalse(TEXT("an array is not a struct"), J[TEXT("nums")].TryGet<FVaRestJsonTestInner>().IsSet());
	// Review Focus: a value struct is not set from null (its field decode keeps the value on null)
	TestFalse(TEXT("null is not a date"), J[TEXT("n")].TryGet<FDateTime>().IsSet());
	TestFalse(TEXT("null is not a GUID"), J[TEXT("n")].TryGet<FGuid>().IsSet());
	TestFalse(TEXT("a number is not a GUID"), J.Path(TEXT("inner.id")).TryGet<FGuid>().IsSet());

	const TOptional<TArray<int32>> Nums = J[TEXT("nums")].TryGet<TArray<int32>>();
	TestTrue(TEXT("TArray<int32>"), Nums.IsSet() && *Nums == TArray<int32>({ 1, 2, 3 }));
	TestFalse(TEXT("one bad element fails the array"), J[TEXT("mixed")].TryGet<TArray<int32>>().IsSet());
	const TOptional<TArray<FVaRestJsonTestInner>> Items = J[TEXT("items")].TryGet<TArray<FVaRestJsonTestInner>>();
	TestTrue(TEXT("TArray of structs"), Items.IsSet() && Items->Num() == 2 && (*Items)[1].Id == 2);

	const TOptional<TMap<FString, int32>> Scores = J[TEXT("scores")].TryGet<TMap<FString, int32>>();
	TestTrue(TEXT("TMap<FString, int32>"), Scores.IsSet() && Scores->Num() == 2 && Scores->FindRef(TEXT("b")) == 2);
	TestFalse(TEXT("one bad value fails the map"), J[TEXT("bad")].TryGet<TMap<FString, int32>>().IsSet());

	const TOptional<FVector> Pos = J[TEXT("pos")].TryGet<FVector>();
	TestTrue(TEXT("an engine struct through TBaseStructure"), Pos.IsSet() && *Pos == FVector(1, 2, 3));

	FVaRestJsonTestInner Fallback;
	Fallback.Id = -1;
	TestEqual(TEXT("As<T> gives the default on a miss"), J[TEXT("nope")].As<FVaRestJsonTestInner>(Fallback).Id, -1);
	TestEqual(TEXT("As<TArray>"), J[TEXT("nums")].As<TArray<int32>>().Num(), 3);
	TestEqual(TEXT("As<int32> like AsInt"), J.Path(TEXT("inner.id")).As<int32>(0), 7);

	const TValueOrError<FVaRestJsonTestInner, FVaRestJsonError> Decoded = FVaRestJsonConvert::FromJson<FVaRestJsonTestInner>(J[TEXT("inner")]);
	TestTrue(TEXT("FromJson<T>"), Decoded.HasValue() && Decoded.GetValue().Id == 7);
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	const TValueOrError<FVaRestJsonTestInner, FVaRestJsonError> Failed =
		FVaRestJsonConvert::FromJson<FVaRestJsonTestInner>(FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"id\":\"7\"}"))).StealValue(), Strict);
	TestTrue(TEXT("strict FromJson reports the error with its path"), Failed.HasError() && Failed.GetError().Path.Equals(TEXT("id"), ESearchCase::CaseSensitive));
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Api.TryGetTyped`
Expected: compile error (the old `TryGet` hits its "structs arrive with FVaRestJsonConvert in M2" `static_assert`; `As` and `FromJson` do not exist).

- [ ] **Step 3: `VaRestJson.h`**

1. Add `#include "UObject/Class.h"` after `#include "Templates/ValueOrError.h"` (TBaseStructure and UScriptStruct). It costs nothing new: `VaRestJson.generated.h` already pulls it in through `UObject/ScriptMacros.h` → `UObject/UnrealType.h`; the explicit include is for include-what-you-use. In `MakeNodeFor`'s final `else`, replace the `static_assert` message `"FVaRestJson: unsupported value type (structs arrive with FVaRestJsonConvert in M2)"` with `"FVaRestJson: unsupported value type; encode a struct with FVaRestJsonConvert::ToJson / StructToJson"`.
2. Replace the whole in-class `TryGet` definition (from `template <typename T>` / `TOptional<T> TryGet() const` through its closing brace) with:

```cpp
	/**
	 * The value as T, unset when it is not one: bool, every integer type (range-checked), float, double, FString, FName,
	 * FText, FVaRestJson, any USTRUCT (and engine structs with a TBaseStructure such as FVector), TArray<T> and
	 * TMap<FString, T>. No coercion between JSON types. A struct is decoded with default options (camelCase, lenient)
	 * and must be an object (a value struct such as FDateTime or FGuid: its own JSON form; null is never a value); one
	 * element or value that does not convert makes a whole TArray/TMap unset. Defined at the end of this header.
	 */
	template <typename T>
	TOptional<T> TryGet() const;

	/** TryGet with a fallback: the value as T, or Default */
	template <typename T>
	T As(T Default = T()) const
	{
		TOptional<T> Value = TryGet<T>();
		return Value ? MoveTemp(Value.GetValue()) : MoveTemp(Default);
	}
```

3. At the end of the header, after `class FVaRestJsonElementRange { ... };`, add:

```cpp
namespace VaRest::Json
{
	/** TryGet<USTRUCT>: decodes with default options; false when Json is not an object. Defined in VaRestJsonConvertDecode.cpp. */
	VARESTJSON_API bool DecodeStructForTryGet(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data);

	template <typename T>
	struct TStringKeyedMap
	{
		static constexpr bool bValue = false;
	};

	template <typename ValueType, typename SetAllocator, typename KeyFuncs>
	struct TStringKeyedMap<TMap<FString, ValueType, SetAllocator, KeyFuncs>>
	{
		static constexpr bool bValue = true;
		using FValue = ValueType;
	};
}

// Defined out of the class so FVaRestJsonMember and the ranges are complete types here
template <typename T>
TOptional<T> FVaRestJson::TryGet() const
{
	using U = std::decay_t<T>;
	if constexpr (std::is_same_v<U, bool>)
	{
		return TryGetBool();
	}
	else if constexpr (std::is_same_v<U, char> || std::is_same_v<U, wchar_t> || std::is_same_v<U, char8_t> || std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t>)
	{
		// Like MakeNodeFor: a character type would otherwise read a number as a code unit
		static_assert(sizeof(U) == 0, "FVaRestJson::TryGet: a single character is not a JSON value; read a string");
		return {};
	}
	else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
	{
		const TOptional<int64> V = TryGetInt64();
		if (V && *V >= static_cast<int64>(TNumericLimits<U>::Min()) && *V <= static_cast<int64>(TNumericLimits<U>::Max()))
		{
			return static_cast<U>(*V);
		}
		return {};
	}
	else if constexpr (std::is_integral_v<U>)
	{
		const TOptional<uint64> V = TryGetUInt64();
		if (V && *V <= static_cast<uint64>(TNumericLimits<U>::Max()))
		{
			return static_cast<U>(*V);
		}
		return {};
	}
	else if constexpr (std::is_floating_point_v<U>)
	{
		const TOptional<double> V = TryGetDouble();
		if (!V)
		{
			return {};
		}
		if constexpr (std::is_same_v<U, float>)
		{
			// Not "> FLT_MAX": 3.4028235e38, the shortest text of FLT_MAX, parses to a double just above it
			// and still rounds to FLT_MAX. Only at the halfway point does the cast give infinity.
			if (FMath::IsFinite(*V) && FMath::Abs(*V) >= VaRest::Json::FloatRoundsToInfinity)
			{
				return {};
			}
		}
		return TOptional<U>(static_cast<U>(*V));
	}
	else if constexpr (std::is_same_v<U, FString>)
	{
		return TryGetString();
	}
	else if constexpr (std::is_same_v<U, FName>)
	{
		const TOptional<FString> S = TryGetString();
		return S ? TOptional<FName>(FName(*S)) : TOptional<FName>();
	}
	else if constexpr (std::is_same_v<U, FText>)
	{
		const TOptional<FString> S = TryGetString();
		return S ? TOptional<FText>(FText::FromString(*S)) : TOptional<FText>();
	}
	else if constexpr (std::is_same_v<U, FVaRestJson>)
	{
		return IsValid() ? TOptional<FVaRestJson>(*this) : TOptional<FVaRestJson>();
	}
	else if constexpr (TIsTArray_V<U>)
	{
		using FElement = typename U::ElementType;
		if (!IsArray())
		{
			return {};
		}
		U Result;
		Result.Reserve(Num());
		for (int32 i = 0; i < Num(); ++i)
		{
			TOptional<FElement> Item = At(i).template TryGet<FElement>();
			if (!Item)
			{
				return {};
			}
			Result.Add(MoveTemp(Item.GetValue()));
		}
		return TOptional<U>(MoveTemp(Result));
	}
	else if constexpr (VaRest::Json::TStringKeyedMap<U>::bValue)
	{
		using FMapValue = typename VaRest::Json::TStringKeyedMap<U>::FValue;
		if (!IsObject())
		{
			return {};
		}
		U Result;
		for (const FVaRestJsonMember& Member : Members())
		{
			TOptional<FMapValue> Item = Member.Value.template TryGet<FMapValue>();
			if (!Item)
			{
				return {};
			}
			Result.Add(Member.GetKey(), MoveTemp(Item.GetValue())); // a later duplicate overwrites: the last wins, as lookups do
		}
		return TOptional<U>(MoveTemp(Result));
	}
	else if constexpr (requires { TBaseStructure<U>::Get(); })
	{
		// Any USTRUCT, and engine structs with a TBaseStructure (FVector, FGuid, FDateTime...). Built the way reflection
		// builds it, so fields the JSON lacks keep the struct's defaults even for no-op-constructed types.
		UScriptStruct* Struct = TBaseStructure<U>::Get();
		alignas(U) uint8 Storage[sizeof(U)];
		Struct->InitializeStruct(Storage);
		TOptional<U> Result;
		if (VaRest::Json::DecodeStructForTryGet(*this, Struct, Storage))
		{
			Result.Emplace(MoveTemp(*reinterpret_cast<U*>(Storage)));
		}
		Struct->DestroyStruct(Storage);
		return Result;
	}
	else
	{
		static_assert(sizeof(U) == 0, "FVaRestJson::TryGet: unsupported type");
		return {};
	}
}
```

- [ ] **Step 4: `FromJson<T>` and `DecodeStructForTryGet`**

In `VaRestJsonConvert.h`, inside `struct FVaRestJsonConvert` after `JsonToProperty`:

```cpp
	/**
	 * JsonToStruct into a new T (USTRUCT or TBaseStructure type); the error is the first error (or warning) reported.
	 * JSON null gives a default T (null keeps a struct's values, spec 2.4); TryGet treats null as absent instead.
	 */
	template <typename T>
	static TValueOrError<T, FVaRestJsonError> FromJson(const FVaRestJson& Json, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions())
	{
		UScriptStruct* Struct = TBaseStructure<T>::Get();
		alignas(T) uint8 Storage[sizeof(T)];
		Struct->InitializeStruct(Storage);
		FVaRestJsonDecodeReport Report;
		const bool bDecoded = JsonToStruct(Json, Struct, Storage, Options, &Report);
		if (bDecoded)
		{
			T Value(MoveTemp(*reinterpret_cast<T*>(Storage)));
			Struct->DestroyStruct(Storage);
			return MakeValue(MoveTemp(Value));
		}
		Struct->DestroyStruct(Storage);
		FVaRestJsonError Error = Report.Errors.Num() > 0 ? Report.Errors[0] : Report.Warnings.Num() > 0 ? Report.Warnings[0] : FVaRestJsonError();
		if (Error.Message.IsEmpty())
		{
			Error.Message = TEXT("the value is not an object");
		}
		return MakeError(MoveTemp(Error));
	}
```

At the end of `VaRestJsonConvertDecode.cpp`:

```cpp
bool VaRest::Json::DecodeStructForTryGet(const FVaRestJson& Json, const UScriptStruct* Struct, void* Data)
{
	// TryGet asks "is this a T". A missing value or null is not one, not even for a struct with a scalar form, whose
	// field decode keeps the value on null (spec 2.4) and would hand back a default FDateTime/FGuid as if one was sent.
	if (!Json.IsValid() || Json.IsNull())
	{
		return false;
	}
	// Anything but an object is not an ordinary struct; a value struct (FGuid, FDateTime...) takes its scalar form
	if (Convert::ClassifyStruct(Struct) == Convert::ESpecialStruct::None && !Json.IsObject())
	{
		return false;
	}
	FVaRestJsonDecodeReport Report; // a report keeps TryGet quiet; lenient warnings are not failures
	return FVaRestJsonConvert::JsonToStruct(Json, Struct, Data, FVaRestJsonConvertOptions(), &Report);
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including `VaRest.Json.Api.TryGetTyped` and the M1 `TryGetRanges`/`FloatAtFltMax` tests.

- [ ] **Step 6: Restore-the-bug (null is not a value struct)**

In `DecodeStructForTryGet` delete the `if (!Json.IsValid() || Json.IsNull())` block. Run `Tools/run_tests.sh VaRest.Json.Api.TryGetTyped`; expect `null is not a date` and `null is not a GUID` to fail (a default `FDateTime`/`FGuid` comes back set). Restore, run, expect green. Report both.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: TryGet/As for USTRUCTs, TArray and TMap<FString, T>; FVaRestJsonConvert::FromJson<T>

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 10: Typed decode and encode benchmarks vs `FJsonObjectConverter` (spec §7.3 deferred items)

**Engine facts:** `FJsonObjectConverter::JsonObjectStringToUStruct` parses an FString then converts (`JsonObjectConverter.h:312-335`); attributes are found by `GetAuthoredName()` in a case-insensitive map and a JSON null is skipped (`JsonObjectConverter.cpp:1340-1360`); a bool's `b` prefix is not dropped, so the bench struct names its bools `Ok`/`Active` to let both converters fill every field. `UStructToJsonObjectString(const UStruct*, const void*, FString&, CheckFlags, SkipFlags, Indent, ExportCb, bPrettyPrint)` (`JsonObjectConverter.h:140`). Engine already links `JsonUtilities` (`Engine.Build.cs:79-88`).

**Files:**
- Modify: `Source/VaRestJson/VaRestJson.Build.cs` (`JsonUtilities`, private)
- Modify: `Source/VaRestJson/Private/Tests/VaRestJsonConvertTestTypes.h` (append bench shapes)
- Create: `Source/VaRestJson/Private/Tests/VaRestJsonConvertBenchTest.cpp`
- Modify: `docs/passport/2026-10-19-json-bench.md` (dated section)

**Interfaces:**
- Consumes: `FVaRestJsonConvert::Utf8ToStruct`, `StructToUtf8` (Tasks 7–8); fixtures from `Tools/gen_bench_json.py` (Task 2).
- Produces: `Bench.VaRest.Json.TypedConvert` writing `<Host>/Saved/VaRestBench/typed-results.md`.

- [ ] **Step 1: Build rule** — add to `VaRestJson.Build.cs`:

```csharp
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Bench.VaRest.Json.TypedConvert compares against FJsonObjectConverter. Engine already depends on
			// JsonUtilities publicly (Engine.Build.cs:88), so every game links it anyway and gains no code from this.
			"JsonUtilities",
		});
```

- [ ] **Step 2: Bench shapes** — append to `VaRestJsonConvertTestTypes.h`:

```cpp
/** The shape of Tools/gen_bench_json.py. Bools are Ok/Active, not bOk/bActive: FJsonObjectConverter matches
 *  GetAuthoredName() and keeps a 'b' prefix, so with these names both converters decode every field. */
USTRUCT()
struct FVaRestJsonBenchOwner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	FString Nick;
};

USTRUCT()
struct FVaRestJsonBenchItem
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Id = 0;

	UPROPERTY()
	int64 Uid = 0;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Desc;

	UPROPERTY()
	double Price = 0.0;

	UPROPERTY()
	double Weight = 0.0;

	UPROPERTY()
	bool Active = false;

	UPROPERTY()
	FVaRestJsonBenchOwner Owner;

	UPROPERTY()
	TArray<FString> Tags;

	UPROPERTY()
	TMap<FString, int32> Stats;
};

USTRUCT()
struct FVaRestJsonBenchData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVaRestJsonBenchItem> Items;

	UPROPERTY()
	int32 Count = 0;
};

USTRUCT()
struct FVaRestJsonBenchResponse
{
	GENERATED_BODY()

	UPROPERTY()
	bool Ok = false;

	UPROPERTY()
	FVaRestJsonBenchData Data;
};
```

- [ ] **Step 3: The benchmark** — `Source/VaRestJson/Private/Tests/VaRestJsonConvertBenchTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

namespace VaRestJsonConvertBench
{
	template <typename BodyType>
	double MedianMs(int32 Runs, BodyType&& Body)
	{
		TArray<double> Times;
		for (int32 i = 0; i < Runs; ++i)
		{
			const double Start = FPlatformTime::Seconds();
			Body();
			Times.Add((FPlatformTime::Seconds() - Start) * 1000.0);
		}
		Times.Sort();
		return Times[Runs / 2];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertBenchTest, "Bench.VaRest.Json.TypedConvert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FVaRestJsonConvertBenchTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertBench;
	const UScriptStruct* Struct = FVaRestJsonBenchResponse::StaticStruct();
	FString Report = TEXT("| fixture | path | decode ms (median) | encode ms (median) |\n|---|---|---|---|\n");
	for (const TCHAR* Name : { TEXT("bench_1mb.json"), TEXT("bench_10mb.json") })
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("VaRestBench") / Name;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			AddError(FString::Printf(TEXT("missing %s -- run: python3 Tools/gen_bench_json.py <Host>/Saved/VaRestBench"), *Path));
			return false;
		}
		const int32 Runs = Bytes.Num() > (4 << 20) ? 7 : 15;
		const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());

		// VaRest: UTF-8 bytes -> arena DOM -> struct
		const double OursDecodeMs = MedianMs(Runs, [&] {
			FVaRestJsonBenchResponse Response;
			FVaRestJsonDecodeReport Issues;
			FVaRestJsonConvert::Utf8ToStruct(Bytes, Struct, &Response, FVaRestJsonConvertOptions(), &Issues);
		});
		FVaRestJsonBenchResponse Ours;
		FVaRestJsonDecodeReport OursIssues;
		TestTrue(TEXT("VaRest decodes"), FVaRestJsonConvert::Utf8ToStruct(Bytes, Struct, &Ours, FVaRestJsonConvertOptions(), &OursIssues));
		TestEqual(TEXT("VaRest decode is clean"), OursIssues.Warnings.Num(), 0);

		// Engine, the v1 way: UTF-8 -> FString -> FJsonObject -> struct
		const double EngineDecodeMs = MedianMs(Runs, [&] {
			const FString Text = FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len());
			FVaRestJsonBenchResponse Response;
			FJsonObjectConverter::JsonObjectStringToUStruct(Text, &Response);
		});
		// Engine reading UTF-8 directly (the fairest engine baseline)
		const double EngineUtf8DecodeMs = MedianMs(Runs, [&] {
			TSharedPtr<FJsonObject> Object;
			FJsonSerializer::Deserialize(TJsonReaderFactory<UTF8CHAR>::CreateFromView(Utf8), Object);
			FVaRestJsonBenchResponse Response;
			FJsonObjectConverter::JsonObjectToUStruct(Object.ToSharedRef(), &Response);
		});
		FVaRestJsonBenchResponse Engine;
		FJsonObjectConverter::JsonObjectStringToUStruct(FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len()), &Engine);
		TestEqual(TEXT("same item count"), Engine.Data.Items.Num(), Ours.Data.Items.Num());
		TestTrue(TEXT("64-bit ids stay exact (generator: 9007199254740993 + i)"), Ours.Data.Items.Num() > 1 && Ours.Data.Items[1].Uid == 9007199254740994ll);

		// Encode: VaRest straight to UTF-8 bytes; the engine to an FString, then the UTF-8 an HTTP body needs
		const double OursEncodeMs = MedianMs(Runs, [&] {
			TArray<uint8> Out;
			FVaRestJsonConvert::StructToUtf8(Struct, &Ours, Out);
		});
		const double EngineEncodeMs = MedianMs(Runs, [&] {
			FString Out;
			FJsonObjectConverter::UStructToJsonObjectString(Struct, &Ours, Out, 0, 0, 0, nullptr, false);
		});
		const double EngineEncodeUtf8Ms = MedianMs(Runs, [&] {
			FString Out;
			FJsonObjectConverter::UStructToJsonObjectString(Struct, &Ours, Out, 0, 0, 0, nullptr, false);
			const FTCHARToUTF8 Body(*Out, Out.Len());
			(void)Body.Length();
		});

		Report += FString::Printf(TEXT("| %s | engine FJsonObjectConverter (FString, v1 path) | %.2f | %.2f |\n"), Name, EngineDecodeMs, EngineEncodeMs);
		Report += FString::Printf(TEXT("| %s | engine (UTF-8 reader; encode + UTF-8 body) | %.2f | %.2f |\n"), Name, EngineUtf8DecodeMs, EngineEncodeUtf8Ms);
		Report += FString::Printf(TEXT("| %s | VaRest FVaRestJsonConvert (UTF-8 in, UTF-8 out) | %.2f | %.2f |\n"), Name, OursDecodeMs, OursEncodeMs);
	}
	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("typed-results.md")));
	return true;
}

#endif
```

- [ ] **Step 4: Run three times**

```bash
for i in 1 2 3; do Tools/run_tests.sh --no-build Bench.VaRest.Json.TypedConvert && cp /w/Unreal/VaRestHost/Saved/VaRestBench/typed-results.md /tmp/varest-typed-$i.md; done
```

(Build first with a plain `Tools/run_tests.sh VaRest.Json.Convert` if needed.) Each run must pass its checks; if `VaRest decode is clean` fails, print `OursIssues.Warnings` and fix the mismatch before measuring.

- [ ] **Step 5: Passport** — append to `docs/passport/2026-10-19-json-bench.md`:

```markdown
## Typed decode and encode (M2, 2026-10-20)

`Bench.VaRest.Json.TypedConvert`: the fixtures above decoded into `FVaRestJsonBenchResponse`
(`Source/VaRestJson/Private/Tests/VaRestJsonConvertTestTypes.h`) and encoded back. VaRest goes UTF-8 bytes → arena
DOM → struct, and struct → UTF-8 bytes with the RapidJSON writer (no DOM). The engine rows are `FJsonObjectConverter`
over an FString (the v1 path) and over the UTF-8 reader; its encode row adds the FString → UTF-8 conversion a request
body needs. Median of three invocations, raw values in brackets.

| fixture | path | decode ms | encode ms |
|---|---|---|---|
(one row per fixture and path from typed-results.md)

What it shows: (the decode and encode speed-ups on 10 MiB against both engine rows, in one or two sentences).
This VaRest decode row is the DOM + walk baseline that M7's SAX -> struct decoder must beat by 1.5x to become
the default for typed responses (spec §2.4).
```

Fill the table and the paragraph with the measured numbers (no placeholders may remain).

- [ ] **Step 6: Commit**

```bash
git add Source/VaRestJson docs/passport/2026-10-19-json-bench.md
git commit -m "json: typed decode/encode benchmark vs FJsonObjectConverter; passport section

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 11: The Blueprint JSON library — values, text, files, typed getters (spec §2.5)

`UVaRestJsonLibrary` lives in `VaRestJson` (spec §1 lists the BP JSON library there). That needs `Engine` (`UBlueprintFunctionLibrary` is declared in `Engine/Classes/Kismet/BlueprintFunctionLibrary.h`); every game links Engine already and v1's `VaRest` module depends on it at the same `PreDefault` phase, so projects that "only want JSON" still link nothing new.

**Files:**
- Modify: `Source/VaRestJson/VaRestJson.Build.cs` (`Engine`, public)
- Create: `Source/VaRestJson/Public/VaRestJsonLibrary.h`, `Source/VaRestJson/Private/VaRestJsonLibrary.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonLibraryTest.cpp`

**Interfaces:**
- Consumes: the `FVaRestJson` API (Tasks 1–5, 9).
- Produces (`UVaRestJsonLibrary`, all `static`, category `VaRest|Json`): `ParseJson(const FString& Text, FVaRestJson& Json, FVaRestJsonError& Error) -> bool`; `JsonToString(const FVaRestJson&, bool bPretty = false) -> FString`; `LoadJsonFile(const FString& Path, FVaRestJson& Json, FVaRestJsonError& Error) -> bool`; `SaveJsonFile(const FVaRestJson&, const FString& Path, bool bPretty = false) -> bool`; `MakeJsonObject()`, `MakeJsonArray()` (impure); `JsonFromBool(bool)`, `JsonFromNumber(double)`, `JsonFromString(const FString&)`, `JsonNull()`; `GetJsonType`, `IsValidJson`, `IsNullJson`; `GetStringField/GetIntField/GetInt64Field/GetNumberField/GetBoolField(const FVaRestJson&, const FString& Key, Default)`, `GetObjectField/GetArrayField(const FVaRestJson&, const FString& Key, const FVaRestJson& Default)`; `GetJsonKeys`, `HasField`, `RemoveField`, `MergeJson(const FVaRestJson& Target, const FVaRestJson& Source) -> FVaRestJson`, `JsonLength`, `ArrayRemoveAt`, `CloneJson`, `JsonEquals`.

Decisions: Make Json Object/Array are impure (a pure maker hands every consumer a new object, so a Set and a later To String would see different objects); `Merge` is shallow (each Source member is `Set` into Target, later wins, duplicates collapse); `Json From Number` stores an integral value within ±2^53 as an integer, so 3 is written `3`, as `JSON.stringify` does; `Load Json File` parses a copy (keeps line/column in errors); `Get Object/Array Field` return the handle into the same document (reference semantics, like `Get` in C++).

- [ ] **Step 1: Write the failing test** — `Source/VaRestJson/Private/Tests/VaRestJsonLibraryTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestJsonLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryValuesTest, "VaRest.Json.Library.Values",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryValuesTest::RunTest(const FString& Parameters)
{
	using L = UVaRestJsonLibrary;
	FVaRestJson Json;
	FVaRestJsonError Error;
	TestTrue(TEXT("Parse Json"), L::ParseJson(TEXT("{\"name\":\"Neo\",\"level\":42,\"big\":9007199254740993,\"ratio\":0.5,\"alive\":true,\"o\":{\"x\":1},\"a\":[1,2,3]}"), Json, Error));
	TestEqualSensitive(TEXT("Get String Field"), L::GetStringField(Json, TEXT("name"), TEXT("?")), FString(TEXT("Neo")));
	TestEqualSensitive(TEXT("... default on a miss"), L::GetStringField(Json, TEXT("nope"), TEXT("?")), FString(TEXT("?")));
	TestEqual(TEXT("Get Int Field"), L::GetIntField(Json, TEXT("level"), -1), 42);
	TestEqual(TEXT("... default on a type mismatch"), L::GetIntField(Json, TEXT("name"), -1), -1);
	TestEqual(TEXT("Get Int64 Field keeps 64 bits"), L::GetInt64Field(Json, TEXT("big"), 0), 9007199254740993ll);
	TestTrue(TEXT("Get Number Field"), L::GetNumberField(Json, TEXT("ratio"), 0.0) == 0.5);
	TestTrue(TEXT("Get Bool Field"), L::GetBoolField(Json, TEXT("alive"), false));
	TestTrue(TEXT("Get Object Field"), L::GetObjectField(Json, TEXT("o"), FVaRestJson()).IsObject());
	TestTrue(TEXT("Get Array Field"), L::GetArrayField(Json, TEXT("a"), FVaRestJson()).IsArray());
	TestFalse(TEXT("Get Object Field default on a mismatch"), L::GetObjectField(Json, TEXT("a"), FVaRestJson()).IsValid());

	// Reference semantics: the object field is the same JSON, so a write through it shows in the parent
	FVaRestJson Inner = L::GetObjectField(Json, TEXT("o"), FVaRestJson());
	Inner.Set(TEXT("y"), 2);
	TestEqual(TEXT("handles alias the document"), Json.Path(TEXT("o.y")).AsInt(), 2);

	TestFalse(TEXT("Parse Json failure"), L::ParseJson(TEXT("{\"a\":"), Json, Error));
	TestTrue(TEXT("... invalid Json and a located error"), !Json.IsValid() && Error.Line == 1 && !Error.Message.IsEmpty());

	TestEqualSensitive(TEXT("From Number writes integers like JSON.stringify"), L::JsonToString(L::JsonFromNumber(3.0)), FString(TEXT("3")));
	TestEqualSensitive(TEXT("From Number keeps fractions"), L::JsonToString(L::JsonFromNumber(2.5)), FString(TEXT("2.5")));
	TestEqualSensitive(TEXT("From Bool / String / Null"), L::JsonToString(L::JsonFromBool(true)) + L::JsonToString(L::JsonFromString(TEXT("s"))) + L::JsonToString(L::JsonNull()), FString(TEXT("true\"s\"null")));
	TestTrue(TEXT("Get Json Type"), L::GetJsonType(L::MakeJsonArray()) == EVaRestJsonType::Array);
	TestTrue(TEXT("Is Valid / Is Null"), !L::IsValidJson(FVaRestJson()) && L::IsNullJson(L::JsonNull()));
	TestEqualSensitive(TEXT("To String pretty"), L::JsonToString(FVaRestJson::Object().Set(TEXT("a"), 1), true), FString(TEXT("{\n  \"a\": 1\n}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryEditTest, "VaRest.Json.Library.ObjectsAndArrays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryEditTest::RunTest(const FString& Parameters)
{
	using L = UVaRestJsonLibrary;
	const FVaRestJson Target = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), 2);
	TestEqualSensitive(TEXT("Get Keys keeps order"), FString::Join(L::GetJsonKeys(Target), TEXT(",")), FString(TEXT("a,b")));
	TestTrue(TEXT("Has Field"), L::HasField(Target, TEXT("a")) && !L::HasField(Target, TEXT("A")));
	TestTrue(TEXT("Remove Field"), L::RemoveField(Target, TEXT("a")) && !L::HasField(Target, TEXT("a")));
	const FVaRestJson Merged = L::MergeJson(Target, FVaRestJson::Object().Set(TEXT("b"), 3).Set(TEXT("c"), 4));
	TestEqualSensitive(TEXT("Merge: shallow, later wins"), L::JsonToString(Merged), FString(TEXT("{\"b\":3,\"c\":4}")));
	TestTrue(TEXT("Merge writes into Target (handles alias)"), Merged.IsSameNode(Target));

	const FVaRestJson List = FVaRestJson::Array(1, 2, 3);
	TestEqual(TEXT("Length"), L::JsonLength(List), 3);
	TestTrue(TEXT("Array Remove At"), L::ArrayRemoveAt(List, 0) && L::JsonLength(List) == 2);
	const FVaRestJson Copy = L::CloneJson(List);
	TestTrue(TEXT("Clone is equal and independent"), L::JsonEquals(Copy, List) && !Copy.IsSameNode(List));
	TestFalse(TEXT("Equals is deep"), L::JsonEquals(Copy, FVaRestJson::Array(2, 4)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryFilesTest, "VaRest.Json.Library.Files",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryFilesTest::RunTest(const FString& Parameters)
{
	using L = UVaRestJsonLibrary;
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VaRestTests");
	const FString Path = Dir / TEXT("library.json");
	const FVaRestJson Original = FVaRestJson::Object().Set(TEXT("name"), TEXT("\u041a\u043b\u044e\u0447")).Set(TEXT("n"), 1);
	TestTrue(TEXT("Save Json File"), L::SaveJsonFile(Original, Path));
	TArray<uint8> Raw;
	FFileHelper::LoadFileToArray(Raw, *Path);
	TestTrue(TEXT("UTF-8 without a BOM"), Raw.Num() > 3 && Raw[0] == '{');
	FVaRestJson Loaded;
	FVaRestJsonError Error;
	TestTrue(TEXT("Load Json File"), L::LoadJsonFile(Path, Loaded, Error) && Loaded.Equals(Original));

	// A file saved by an editor with a BOM still loads
	TArray<uint8> WithBom = { 0xEF, 0xBB, 0xBF };
	WithBom.Append(Raw);
	FFileHelper::SaveArrayToFile(WithBom, *Path);
	TestTrue(TEXT("BOM is skipped"), L::LoadJsonFile(Path, Loaded, Error) && Loaded.Equals(Original));

	TestFalse(TEXT("a missing file fails"), L::LoadJsonFile(Dir / TEXT("missing.json"), Loaded, Error));
	TestTrue(TEXT("... with a message and an invalid Json"), !Loaded.IsValid() && Error.Message.Contains(TEXT("missing.json")));
	IFileManager::Get().Delete(*Path);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Library`
Expected: compile error (`VaRestJsonLibrary.h` not found).

- [ ] **Step 3: Build rule** — in `VaRestJson.Build.cs` add to `PublicDependencyModuleNames` (after `RapidJSON`):

```csharp
			// PUBLIC: VaRestJsonLibrary.h derives from UBlueprintFunctionLibrary (Engine/Classes/Kismet/
			// BlueprintFunctionLibrary.h). Every game links Engine; v1's VaRest module needs it at PreDefault too.
			"Engine",
```

- [ ] **Step 4: Header** — `Source/VaRestJson/Public/VaRestJsonLibrary.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"

#include "VaRestJsonLibrary.generated.h"

/**
 * The Blueprint JSON library (spec 2.5). A VaRest Json value is a HANDLE: copying one aliases the same JSON, so Set
 * Field on any copy changes it everywhere (like VaRest 1 objects); Clone makes an independent copy. Make Json Object and
 * Make Json Array are impure on purpose: a pure maker would hand every consumer a fresh object.
 */
UCLASS()
class VARESTJSON_API UVaRestJsonLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---- text and files

	/** Parses JSON text. On failure Json is invalid and Error says where (line and column). */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Parse Json"))
	static UPARAM(DisplayName = "Success") bool ParseJson(const FString& Text, FVaRestJson& Json, FVaRestJsonError& Error);

	/** JSON text of Json; "" for an invalid handle */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "To String (Json)"))
	static FString JsonToString(const FVaRestJson& Json, bool bPretty = false);

	/** Reads a UTF-8 JSON file (a BOM is skipped) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Load Json File"))
	static UPARAM(DisplayName = "Success") bool LoadJsonFile(const FString& Path, FVaRestJson& Json, FVaRestJsonError& Error);

	/** Writes Json as UTF-8 without a BOM */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Save Json File"))
	static UPARAM(DisplayName = "Success") bool SaveJsonFile(const FVaRestJson& Json, const FString& Path, bool bPretty = false);

	// ---- making values

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Make Json Object"))
	static FVaRestJson MakeJsonObject();

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Make Json Array"))
	static FVaRestJson MakeJsonArray();

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json From Bool"))
	static FVaRestJson JsonFromBool(bool Value);

	/** An integral value within +-2^53 is stored as an integer, so 3 is written 3 (as JSON.stringify does) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json From Number"))
	static FVaRestJson JsonFromNumber(double Value);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json From String"))
	static FVaRestJson JsonFromString(const FString& Value);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json Null"))
	static FVaRestJson JsonNull();

	// ---- type

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Json Type"))
	static EVaRestJsonType GetJsonType(const FVaRestJson& Json);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Is Valid (Json)"))
	static bool IsValidJson(const FVaRestJson& Json);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Is Null (Json)"))
	static bool IsNullJson(const FVaRestJson& Json);

	// ---- typed getters: the field's value, or Default when it is missing or of another type

	UFUNCTION(BlueprintPure, Category = "VaRest|Json")
	static FString GetStringField(const FVaRestJson& Json, const FString& Key, const FString& Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json")
	static int32 GetIntField(const FVaRestJson& Json, const FString& Key, int32 Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json")
	static int64 GetInt64Field(const FVaRestJson& Json, const FString& Key, int64 Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json")
	static double GetNumberField(const FVaRestJson& Json, const FString& Key, double Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json")
	static bool GetBoolField(const FVaRestJson& Json, const FString& Key, bool Default);

	/** The object field itself (a handle into the same JSON), or Default */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (AutoCreateRefTerm = "Default"))
	static FVaRestJson GetObjectField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default);

	/** The array field itself (a handle into the same JSON), or Default */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (AutoCreateRefTerm = "Default"))
	static FVaRestJson GetArrayField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default);

	// ---- objects and arrays

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Keys (Json)"))
	static TArray<FString> GetJsonKeys(const FVaRestJson& Json);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Has Field"))
	static bool HasField(const FVaRestJson& Json, const FString& Key);

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Remove Field"))
	static UPARAM(DisplayName = "Removed") bool RemoveField(const FVaRestJson& Json, const FString& Key);

	/** Copies every member of Source into Target (shallow, later wins) and returns Target */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Merge Json"))
	static FVaRestJson MergeJson(const FVaRestJson& Target, const FVaRestJson& Source);

	/** Elements of an array, members of an object, 0 otherwise */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Length (Json)"))
	static int32 JsonLength(const FVaRestJson& Json);

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Array Remove At"))
	static UPARAM(DisplayName = "Removed") bool ArrayRemoveAt(const FVaRestJson& Json, int32 Index);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Clone (Json)"))
	static FVaRestJson CloneJson(const FVaRestJson& Json);

	/** Deep JSON equality (1 == 1.0, member order ignored) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Equals (Json)"))
	static bool JsonEquals(const FVaRestJson& A, const FVaRestJson& B);
};
```

- [ ] **Step 5: Implementation** — `Source/VaRestJson/Private/VaRestJsonLibrary.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonLibrary.h"

#include "Misc/FileHelper.h"

bool UVaRestJsonLibrary::ParseJson(const FString& Text, FVaRestJson& Json, FVaRestJsonError& Error)
{
	TValueOrError<FVaRestJson, FVaRestJsonError> Result = FVaRestJson::Parse(FStringView(Text));
	if (Result.HasValue())
	{
		Json = Result.StealValue();
		Error = FVaRestJsonError();
		return true;
	}
	Json = FVaRestJson();
	Error = Result.StealError();
	return false;
}

FString UVaRestJsonLibrary::JsonToString(const FVaRestJson& Json, bool bPretty)
{
	return Json.ToString(bPretty ? EVaRestJsonFormat::Pretty : EVaRestJsonFormat::Condensed);
}

bool UVaRestJsonLibrary::LoadJsonFile(const FString& Path, FVaRestJson& Json, FVaRestJsonError& Error)
{
	Json = FVaRestJson();
	Error = FVaRestJsonError();
	TArray<uint8> Bytes;
	// Silent: LoadFileToArray would log its own warning (FileHelper.cpp:47-54); the caller gets Error instead
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		Error.Message = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	// Parse (one copy) rather than ParseOwned: a copy keeps the original bytes for line/column in the error
	TValueOrError<FVaRestJson, FVaRestJsonError> Result = FVaRestJson::Parse(TConstArrayView<uint8>(Bytes));
	if (!Result.HasValue())
	{
		Error = Result.StealError();
		return false;
	}
	Json = Result.StealValue();
	return true;
}

bool UVaRestJsonLibrary::SaveJsonFile(const FVaRestJson& Json, const FString& Path, bool bPretty)
{
	if (!Json.IsValid())
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Save Json File %s: the Json is invalid"), *Path), ELogVerbosity::Warning);
		return false;
	}
	TArray<uint8> Bytes;
	const TValueOrError<void, FVaRestJsonError> Written = Json.TryToUtf8(Bytes, bPretty ? EVaRestJsonFormat::Pretty : EVaRestJsonFormat::Condensed);
	if (Written.HasError())
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Save Json File %s: %s"), *Path, *Written.GetError().Message), ELogVerbosity::Warning);
		return false;
	}
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

FVaRestJson UVaRestJsonLibrary::MakeJsonObject()
{
	return FVaRestJson::Object();
}

FVaRestJson UVaRestJsonLibrary::MakeJsonArray()
{
	return FVaRestJson::Array();
}

FVaRestJson UVaRestJsonLibrary::JsonFromBool(bool Value)
{
	return FVaRestJson::From(Value);
}

FVaRestJson UVaRestJsonLibrary::JsonFromNumber(double Value)
{
	// BP numbers are doubles; an integral one is written 3, not 3.0, as JSON.stringify writes it. Within 2^53 the
	// integer is exact.
	if (FMath::IsFinite(Value) && FMath::TruncToDouble(Value) == Value && FMath::Abs(Value) <= 9007199254740992.0)
	{
		return FVaRestJson::From(static_cast<int64>(Value));
	}
	return FVaRestJson::From(Value);
}

FVaRestJson UVaRestJsonLibrary::JsonFromString(const FString& Value)
{
	return FVaRestJson::From(FStringView(Value));
}

FVaRestJson UVaRestJsonLibrary::JsonNull()
{
	return FVaRestJson::Null();
}

EVaRestJsonType UVaRestJsonLibrary::GetJsonType(const FVaRestJson& Json)
{
	return Json.GetType();
}

bool UVaRestJsonLibrary::IsValidJson(const FVaRestJson& Json)
{
	return Json.IsValid();
}

bool UVaRestJsonLibrary::IsNullJson(const FVaRestJson& Json)
{
	return Json.IsNull();
}

FString UVaRestJsonLibrary::GetStringField(const FVaRestJson& Json, const FString& Key, const FString& Default)
{
	return Json.Get(Key).AsString(Default);
}

int32 UVaRestJsonLibrary::GetIntField(const FVaRestJson& Json, const FString& Key, int32 Default)
{
	return Json.Get(Key).AsInt(Default);
}

int64 UVaRestJsonLibrary::GetInt64Field(const FVaRestJson& Json, const FString& Key, int64 Default)
{
	return Json.Get(Key).AsInt64(Default);
}

double UVaRestJsonLibrary::GetNumberField(const FVaRestJson& Json, const FString& Key, double Default)
{
	return Json.Get(Key).AsDouble(Default);
}

bool UVaRestJsonLibrary::GetBoolField(const FVaRestJson& Json, const FString& Key, bool Default)
{
	return Json.Get(Key).AsBool(Default);
}

FVaRestJson UVaRestJsonLibrary::GetObjectField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default)
{
	const FVaRestJson Field = Json.Get(Key);
	return Field.IsObject() ? Field : Default;
}

FVaRestJson UVaRestJsonLibrary::GetArrayField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default)
{
	const FVaRestJson Field = Json.Get(Key);
	return Field.IsArray() ? Field : Default;
}

TArray<FString> UVaRestJsonLibrary::GetJsonKeys(const FVaRestJson& Json)
{
	return Json.GetKeys();
}

bool UVaRestJsonLibrary::HasField(const FVaRestJson& Json, const FString& Key)
{
	return Json.Has(Key);
}

bool UVaRestJsonLibrary::RemoveField(const FVaRestJson& Json, const FString& Key)
{
	FVaRestJson Target = Json; // a copy of the handle aliases the same object
	return Target.Remove(Key);
}

FVaRestJson UVaRestJsonLibrary::MergeJson(const FVaRestJson& Target, const FVaRestJson& Source)
{
	if (!Target.IsObject() || !Source.IsObject())
	{
		FFrame::KismetExecutionMessage(TEXT("Merge Json: both Target and Source must be objects"), ELogVerbosity::Warning);
		return Target;
	}
	FVaRestJson Result = Target;
	for (const FVaRestJsonMember& Member : Source.Members())
	{
		Result.Set(Member.Key, Member.Value); // copies; later members win, duplicates collapse (spec 2.2)
	}
	return Result;
}

int32 UVaRestJsonLibrary::JsonLength(const FVaRestJson& Json)
{
	return Json.Num();
}

bool UVaRestJsonLibrary::ArrayRemoveAt(const FVaRestJson& Json, int32 Index)
{
	FVaRestJson Target = Json;
	return Target.RemoveAt(Index);
}

FVaRestJson UVaRestJsonLibrary::CloneJson(const FVaRestJson& Json)
{
	return Json.Clone();
}

bool UVaRestJsonLibrary::JsonEquals(const FVaRestJson& A, const FVaRestJson& B)
{
	return A.Equals(B);
}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including the three `VaRest.Json.Library.*` tests.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: UVaRestJsonLibrary -- parse, text, files, values, typed getters, object/array edits

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 12: Wildcard nodes — `Get Field`, `Set Field`, `Get Path`, `Array Get`, `Array Add`, `Struct To Json`, `Json To Struct`, `Struct To Json String` (CustomThunk, spec §2.5)

**Engine facts:** the engine's JSON nodes use the same pattern: a `CustomThunk` UFUNCTION with `CustomStructureParam`, whose thunk reads the real property with `Stack.StepCompiledIn<FProperty>(nullptr)` and `Stack.MostRecentProperty`/`MostRecentPropertyAddress` (`JsonBlueprintFunctionLibrary.h:34-46`, `JsonBlueprintFunctionLibrary.cpp:79-122`). A plain call node adapts such a pin itself: `FCustomStructureParamHelper` turns it into a wildcard that takes the linked pin's type (`K2Node_CallFunction.cpp:96-170`, called on allocate `:754`, reconstruct `:1292` and connection change `:1353`), and wildcard inputs accept arrays by default (`K2Node.h:432-433`), so these functions need no custom K2 node (the engine also exposes one directly, `DataTableFunctionLibrary.h:236`). A missing value is a script exception (`Script.h:460`). Called natively through `ProcessEvent` there is no bytecode: `StepCompiledIn` walks the parameter chain (`Stack.h:479-493`) and `StepExplicitProperty` sets only `MostRecentPropertyAddress` (`ScriptCore.cpp:561-585`), from the out-parameter list `ProcessEvent` builds (`ScriptCore.cpp:2184-2215`); the engine's thunks then see no property. A `const T&` parameter is a reference parameter (`CPF_OutParm | CPF_ReferenceParm | CPF_ConstParm`, flags `0x0010000008000182` on the engine's identical `SetField` parameter, `Engine/Plugins/JsonBlueprintUtilities/Intermediate/Build/Linux/UnrealEditor/Inc/JsonBlueprintUtilities/UHT/JsonBlueprintFunctionLibrary.gen.cpp:481`), so the native step copies nothing into the null result. `FFrame::KismetExecutionMessage` logs `Script Msg: <message>` to `LogScript` at the given verbosity and, inside a Blueprint call, a second line `Script Msg called by: <object>` (`ScriptCore.cpp:684-748`, `CoreMisc.cpp:409-419`); Python never sees CustomThunk functions (`PyGenUtil.cpp:1622`).

**Files:**
- Modify: `Source/VaRestJson/Public/VaRestJsonLibrary.h` (wildcard block), `Source/VaRestJson/Private/VaRestJsonLibrary.cpp` (Impl functions and thunks)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonLibraryWildcardTest.cpp`

**Interfaces:**
- Consumes: `FVaRestJsonConvert::PropertyToJson`, `JsonToProperty` (Tasks 7–8), Task 11's library.
- Produces (UFUNCTIONs, CustomThunk, wildcard pin `Value`): `GetField(const FVaRestJson& Json, const FString& Key, int32& Value) -> bool` (pure, "Found"); `SetField(const FVaRestJson& Json, const FString& Key, const int32& Value) -> FVaRestJson`; `GetPath(const FVaRestJson& Json, const FString& Path, int32& Value) -> bool` (pure); `ArrayGet(const FVaRestJson& Json, int32 Index, int32& Value) -> bool` (pure); `ArrayAdd(const FVaRestJson& Json, const int32& Value) -> FVaRestJson`; `StructToJson(const int32& Value, EVaRestJsonNaming Naming = CamelCase) -> FVaRestJson` (pure); `JsonToStruct(const FVaRestJson& Json, int32& Value, EVaRestJsonNaming Naming = CamelCase, bool bStrict = false) -> bool`; `StructToJsonString(const int32& Value, EVaRestJsonNaming Naming = CamelCase, bool bPretty = false) -> FString` (pure); internal (BlueprintInternalUseOnly, for Task 14): `MakeJsonInternalObject() -> FVaRestJson` (pure), `MakeJsonInternalSet(const FVaRestJson& Json, const FString& Key, const int32& Value) -> FVaRestJson` (pure, CustomThunk). C++ entry points: `GetFieldImpl(const FVaRestJson&, const FString& Key, const FProperty*, void* Address) -> bool`, `SetFieldImpl(const FVaRestJson&, const FString& Key, const FProperty*, const void* Address) -> FVaRestJson`, `GetPathImpl(..., const FString& Path, ...) -> bool`, `ArrayGetImpl(..., int32 Index, ...) -> bool`, `ArrayAddImpl(const FVaRestJson&, const FProperty*, const void*) -> FVaRestJson`, `StructToJsonImpl(const FProperty*, const void*, EVaRestJsonNaming) -> FVaRestJson`, `JsonToStructImpl(const FVaRestJson&, const FProperty*, void*, EVaRestJsonNaming, bool bStrict) -> bool`, `StructToJsonStringImpl(const FProperty*, const void*, EVaRestJsonNaming, bool bPretty) -> FString`.

Decisions: thunks take the wildcard's property from the parameter chain when there is no bytecode, so native `ProcessEvent` callers (and the `Thunks` test, which pins every thunk's parameter order without a Blueprint) work too; every "get" and `Json To Struct` reset the output to the type's default before reading (a BP temporary is reused across loop iterations and, in an event graph, kept between events in the persistent ubergraph frame; spec §2.4 "missing fields keep defaults" means the type's defaults, not the previous call's values); `Found` is true when the value exists, is not `null`, and converts to the pin's type (lenient), false otherwise (null into a typed pin is not found, as `TryGet`); a value `Set Field`/`Array Add` cannot encode leaves the object/array unchanged (the encode logs why) instead of removing the key or appending null; `Set Field`, `Array Add` and `Make Json` write struct values with camelCase field names (per-client naming arrives with the HTTP layer; `Struct To Json` has a `Naming` pin); a Json-typed output pin receives the handle into the same document (as `Get Object Field` does), while a Json field inside a decoded struct is cloned (Task 8); `Set Field`/`Array Add` on something that is not an object/array log a script warning and return the input unchanged — no auto-vivify, because the node holds a copy of the handle and a new object would never reach the caller's variable; a connected Json handle is set as itself (a missing one removes the key, spec §2.2); `Struct To Json`/`Json To Struct` accept any connected type, not only structs.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonLibraryWildcardTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/StructOnScope.h"
#include "VaRestJsonConvertTestTypes.h"
#include "VaRestJsonLibrary.h"

namespace VaRestJsonLibraryWildcardTest
{
	FVaRestJson ParseText(const UTF8CHAR* Text)
	{
		return FVaRestJson::Parse(FUtf8StringView(Text)).StealValue();
	}

	/** A field of FVaRestJsonTestAll as the (property, address) pair a thunk receives */
	struct FSlot
	{
		const FProperty* Property;
		void* Address;
	};

	FSlot SlotOf(FVaRestJsonTestAll& All, const TCHAR* Name)
	{
		const FProperty* Property = FindFProperty<FProperty>(FVaRestJsonTestAll::StaticStruct(), FName(Name));
		return FSlot{ Property, Property->ContainerPtrToValuePtr<void>(&All) };
	}

	/** Calls a UVaRestJsonLibrary UFUNCTION the way native code reaches a thunk: ProcessEvent on the CDO with a parameter block */
	struct FCall
	{
		UFunction* Function;
		FStructOnScope Params;

		explicit FCall(const TCHAR* Name)
			: Function(UVaRestJsonLibrary::StaticClass()->FindFunctionByName(FName(Name)))
			, Params(Function)
		{
		}

		template <typename T>
		T& Param(const TCHAR* Name)
		{
			return *FindFProperty<FProperty>(Function, FName(Name))->ContainerPtrToValuePtr<T>(Params.GetStructMemory());
		}

		void Invoke()
		{
			GetMutableDefault<UVaRestJsonLibrary>()->ProcessEvent(Function, Params.GetStructMemory());
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryWildcardImplTest, "VaRest.Json.Library.WildcardImpl",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryWildcardImplTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonLibraryWildcardTest;
	using L = UVaRestJsonLibrary;
	FVaRestJsonTestAll All;
	const FSlot Int = SlotOf(All, TEXT("Int32Value"));
	const FSlot Items = SlotOf(All, TEXT("Items"));
	const FSlot Json = SlotOf(All, TEXT("Extra"));
	const FSlot Inner = SlotOf(All, TEXT("Inner"));
	const FVaRestJson Source = ParseText(UTF8TEXT("{\"n\":42,\"items\":[{\"id\":1,\"name\":\"a\"},{\"id\":2}],\"o\":{\"x\":true},\"s\":\"str\",\"z\":null}"));

	TestTrue(TEXT("Get Field int"), L::GetFieldImpl(Source, TEXT("n"), Int.Property, Int.Address) && All.Int32Value == 42);
	// Review Focus: a miss resets the output, so a BP loop never sees the previous iteration's value
	TestFalse(TEXT("a missing key is not found"), L::GetFieldImpl(Source, TEXT("missing"), Int.Property, Int.Address));
	TestEqual(TEXT("... and resets the output"), All.Int32Value, 0);
	All.Int32Value = 5;
	TestFalse(TEXT("a value of another type is not found"), L::GetFieldImpl(Source, TEXT("s"), Int.Property, Int.Address));
	TestEqual(TEXT("... and resets the output too"), All.Int32Value, 0);
	TestFalse(TEXT("null into a typed pin is not found"), L::GetFieldImpl(Source, TEXT("z"), Inner.Property, Inner.Address));
	TestTrue(TEXT("Get Field array of structs"), L::GetFieldImpl(Source, TEXT("items"), Items.Property, Items.Address) && All.Items.Num() == 2 && All.Items[1].Id == 2);
	TestTrue(TEXT("Get Field into a Json pin"), L::GetFieldImpl(Source, TEXT("o"), Json.Property, Json.Address) && All.Extra[TEXT("x")].AsBool());
	TestTrue(TEXT("... is a handle into the same document"), All.Extra.GetDocumentInternal() == Source.GetDocumentInternal());
	TestTrue(TEXT("Get Path"), L::GetPathImpl(Source, TEXT("items[1].id"), Int.Property, Int.Address) && All.Int32Value == 2);
	TestTrue(TEXT("Array Get"), L::ArrayGetImpl(Source[TEXT("items")], 0, Inner.Property, Inner.Address) && All.Inner.Id == 1 && All.Inner.Name.Equals(TEXT("a"), ESearchCase::CaseSensitive));

	FVaRestJson Target = FVaRestJson::Object();
	All.Int32Value = 7;
	L::SetFieldImpl(Target, TEXT("n"), Int.Property, Int.Address);
	L::SetFieldImpl(Target, TEXT("items"), Items.Property, Items.Address);
	All.Extra = FVaRestJson();
	L::SetFieldImpl(Target, TEXT("gone"), Json.Property, Json.Address); // a missing handle is undefined: no key
	TestEqualSensitive(TEXT("Set Field of an int, an array of structs, a missing handle"), Target.ToString(),
		FString(TEXT("{\"n\":7,\"items\":[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"name\":\"\"}]}")));

	FVaRestJson List = FVaRestJson::Array();
	L::ArrayAddImpl(List, Int.Property, Int.Address);
	L::ArrayAddImpl(List, Json.Property, Json.Address); // a missing handle in an array is null (spec 2.2)
	TestEqualSensitive(TEXT("Array Add"), List.ToString(), FString(TEXT("[7,null]")));

	AddExpectedMessagePlain(TEXT("not an object"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Set Field on an invalid handle does not auto-vivify"), L::SetFieldImpl(FVaRestJson(), TEXT("n"), Int.Property, Int.Address).IsValid());

	All.Inner.Id = 3;
	All.Inner.Name = TEXT("x");
	TestEqualSensitive(TEXT("Struct To Json"), L::StructToJsonImpl(Inner.Property, Inner.Address, EVaRestJsonNaming::SnakeCase).ToString(), FString(TEXT("{\"id\":3,\"name\":\"x\"}")));
	TestEqualSensitive(TEXT("Struct To Json String"), L::StructToJsonStringImpl(Inner.Property, Inner.Address, EVaRestJsonNaming::PascalCase, false), FString(TEXT("{\"Id\":3,\"Name\":\"x\"}")));
	All.Inner.Id = 3;
	All.Inner.Name = TEXT("stale");
	TestTrue(TEXT("Json To Struct"), L::JsonToStructImpl(ParseText(UTF8TEXT("{\"id\":9}")), Inner.Property, Inner.Address, EVaRestJsonNaming::CamelCase, false));
	// Review Focus: the Value output is a reused BP temporary; a field the JSON lacks gets the default, not the last value
	TestTrue(TEXT("... starts from defaults, not from the previous call (a reused BP temporary)"), All.Inner.Id == 9 && All.Inner.Name.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryThunksTest, "VaRest.Json.Library.Thunks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryThunksTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonLibraryWildcardTest;
	// Native calls through ProcessEvent read each parameter in declaration order (the int32 placeholder is the
	// "connected" type here), so this pins every thunk's parameter order before a Blueprint ever calls it.
	const FVaRestJson Target = FVaRestJson::Object();
	{
		FCall Call(TEXT("SetField"));
		Call.Param<FVaRestJson>(TEXT("Json")) = Target;
		Call.Param<FString>(TEXT("Key")) = TEXT("n");
		Call.Param<int32>(TEXT("Value")) = 42;
		Call.Invoke();
		TestEqual(TEXT("SetField wrote through the handle"), Target[TEXT("n")].AsInt(), 42);
		TestTrue(TEXT("... and returned it"), Call.Param<FVaRestJson>(TEXT("ReturnValue")).IsSameNode(Target));
	}
	{
		FCall Call(TEXT("GetField"));
		Call.Param<FVaRestJson>(TEXT("Json")) = Target;
		Call.Param<FString>(TEXT("Key")) = TEXT("n");
		Call.Param<int32>(TEXT("Value")) = -1;
		Call.Invoke();
		TestTrue(TEXT("GetField"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 42);
	}
	{
		FCall Call(TEXT("GetPath"));
		Call.Param<FVaRestJson>(TEXT("Json")) = Target;
		Call.Param<FString>(TEXT("Path")) = TEXT("n");
		Call.Invoke();
		TestTrue(TEXT("GetPath"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 42);
	}
	const FVaRestJson List = FVaRestJson::Array(5, 6);
	{
		FCall Call(TEXT("ArrayGet"));
		Call.Param<FVaRestJson>(TEXT("Json")) = List;
		Call.Param<int32>(TEXT("Index")) = 1;
		Call.Invoke();
		TestTrue(TEXT("ArrayGet"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 6);
	}
	{
		FCall Call(TEXT("ArrayAdd"));
		Call.Param<FVaRestJson>(TEXT("Json")) = List;
		Call.Param<int32>(TEXT("Value")) = 7;
		Call.Invoke();
		TestEqualSensitive(TEXT("ArrayAdd"), List.ToString(), FString(TEXT("[5,6,7]")));
	}
	{
		FCall Call(TEXT("StructToJson"));
		Call.Param<int32>(TEXT("Value")) = 9;
		Call.Invoke();
		TestEqual(TEXT("StructToJson of any value"), Call.Param<FVaRestJson>(TEXT("ReturnValue")).AsInt(), 9);
	}
	{
		FCall Call(TEXT("StructToJsonString"));
		Call.Param<int32>(TEXT("Value")) = 5;
		Call.Invoke();
		TestEqualSensitive(TEXT("StructToJsonString"), Call.Param<FString>(TEXT("ReturnValue")), FString(TEXT("5")));
	}
	{
		FCall Call(TEXT("JsonToStruct"));
		Call.Param<FVaRestJson>(TEXT("Json")) = FVaRestJson::From(3);
		Call.Invoke();
		TestTrue(TEXT("JsonToStruct"), Call.Param<bool>(TEXT("ReturnValue")) && Call.Param<int32>(TEXT("Value")) == 3);
	}
	{
		FCall Call(TEXT("MakeJsonInternalSet"));
		Call.Param<FVaRestJson>(TEXT("Json")) = FVaRestJson::Object();
		Call.Param<FString>(TEXT("Key")) = TEXT("k");
		Call.Param<int32>(TEXT("Value")) = 1;
		Call.Invoke();
		TestEqualSensitive(TEXT("MakeJsonInternalSet"), Call.Param<FVaRestJson>(TEXT("ReturnValue")).ToString(), FString(TEXT("{\"k\":1}")));
	}
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Library`
Expected: compile errors (`GetFieldImpl` etc. not members).

- [ ] **Step 3: Header** — in `VaRestJsonLibrary.h`, append inside the class after `JsonEquals`:

```cpp
	// ---- wildcard nodes (CustomThunk, like the engine's JsonBlueprintFunctionLibrary.h:34-46). The int32 in each
	// signature is a placeholder: CustomStructureParam makes the pin a wildcard that takes the connected type
	// (K2Node_CallFunction.cpp:96-170), and the thunk reads the real property from the VM stack.

	/** The field converted to the connected type. Found is false, and Value the type's default, when the key is missing, null, or does not convert. */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Get Field"))
	static UPARAM(DisplayName = "Found") bool GetField(const FVaRestJson& Json, const FString& Key, int32& Value);
	DECLARE_FUNCTION(execGetField);

	/**
	 * Sets Key on the object Json to Value (any type) and returns Json. A connected invalid Json handle removes the key
	 * (spec 2.2); a value that cannot be encoded leaves the key as it was. Struct values use camelCase field names
	 * (Struct To Json has a Naming input).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Set Field"))
	static FVaRestJson SetField(const FVaRestJson& Json, const FString& Key, const int32& Value);
	DECLARE_FUNCTION(execSetField);

	/** Like Get Field, by path: "a.b[2].c", or an RFC 6901 pointer starting with '/' */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Get Path"))
	static UPARAM(DisplayName = "Found") bool GetPath(const FVaRestJson& Json, const FString& Path, int32& Value);
	DECLARE_FUNCTION(execGetPath);

	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Array Get"))
	static UPARAM(DisplayName = "Found") bool ArrayGet(const FVaRestJson& Json, int32 Index, int32& Value);
	DECLARE_FUNCTION(execArrayGet);

	/** Appends Value (any type) to the array Json and returns Json; nothing when Value cannot be encoded. Structs use camelCase field names. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Array Add"))
	static FVaRestJson ArrayAdd(const FVaRestJson& Json, const int32& Value);
	DECLARE_FUNCTION(execArrayAdd);

	/** Value (a struct, an array of structs, any type) as JSON */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Struct To Json"))
	static FVaRestJson StructToJson(const int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase);
	DECLARE_FUNCTION(execStructToJson);

	/** Fills Value (any type) from Json: tolerant names; strict refuses coercions. Success is false when Json does not fit. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Json To Struct"))
	static UPARAM(DisplayName = "Success") bool JsonToStruct(const FVaRestJson& Json, int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase, bool bStrict = false);
	DECLARE_FUNCTION(execJsonToStruct);

	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Struct To Json String"))
	static FString StructToJsonString(const int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase, bool bPretty = false);
	DECLARE_FUNCTION(execStructToJsonString);

	/** Make Json node expansion (K2Node_VaRestMakeJson): a fresh object, pure so the expanded chain stays pure */
	UFUNCTION(BlueprintPure, meta = (BlueprintInternalUseOnly = "true"))
	static FVaRestJson MakeJsonInternalObject();

	/** Make Json node expansion: Set Field, pure */
	UFUNCTION(BlueprintPure, CustomThunk, meta = (BlueprintInternalUseOnly = "true", CustomStructureParam = "Value", AutoCreateRefTerm = "Value"))
	static FVaRestJson MakeJsonInternalSet(const FVaRestJson& Json, const FString& Key, const int32& Value);
	DECLARE_FUNCTION(execMakeJsonInternalSet);

	// ---- the wildcard nodes' work, callable from C++: Property/Address describe the connected value

	static bool GetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, void* Address);
	static FVaRestJson SetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, const void* Address);
	static bool GetPathImpl(const FVaRestJson& Json, const FString& Path, const FProperty* Property, void* Address);
	static bool ArrayGetImpl(const FVaRestJson& Json, int32 Index, const FProperty* Property, void* Address);
	static FVaRestJson ArrayAddImpl(const FVaRestJson& Json, const FProperty* Property, const void* Address);
	static FVaRestJson StructToJsonImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming);
	static bool JsonToStructImpl(const FVaRestJson& Json, const FProperty* Property, void* Address, EVaRestJsonNaming Naming, bool bStrict);
	static FString StructToJsonStringImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming, bool bPretty);
```

- [ ] **Step 4: Implementation** — append to `VaRestJsonLibrary.cpp` (add `#include "Blueprint/BlueprintExceptionInfo.h"` and `#include "UObject/UnrealType.h"` to its includes):

```cpp
namespace VaRest::Json::LibraryPrivate
{
	bool IsJsonPin(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct == FVaRestJson::StaticStruct();
	}

	/** A connected Json handle stays itself (a missing one stays missing: spec 2.2); any other value is converted */
	FVaRestJson ValueAsJson(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming)
	{
		if (IsJsonPin(Property))
		{
			return *static_cast<const FVaRestJson*>(Address);
		}
		FVaRestJsonConvertOptions Options;
		Options.Naming = Naming;
		return FVaRestJsonConvert::PropertyToJson(Property, Address, Options);
	}

	/**
	 * Get Field / Get Path / Array Get. The output is reset first: a BP temporary is reused across loop iterations, so a
	 * miss must not leave the previous call's value. A Json pin receives the handle itself (reference semantics, as
	 * Get Object Field); other types convert leniently and Found says whether they did.
	 */
	bool ReadInto(const FVaRestJson& Source, const FProperty* Property, void* Address)
	{
		Property->ClearValue(Address);
		if (!Source.IsValid())
		{
			return false;
		}
		if (IsJsonPin(Property))
		{
			*static_cast<FVaRestJson*>(Address) = Source;
			return true;
		}
		if (Source.IsNull())
		{
			return false; // null is no value for a typed pin (as TryGet): Found false, the output its default
		}
		FVaRestJsonDecodeReport Report; // Found carries the outcome; lenient warnings stay out of the log
		return FVaRestJsonConvert::JsonToProperty(Source, Property, Address, FVaRestJsonConvertOptions(), &Report);
	}

	FString TypeOf(const FVaRestJson& Json)
	{
		return StaticEnum<EVaRestJsonType>()->GetNameStringByValue(static_cast<int64>(Json.GetType()));
	}

	struct FWildcard
	{
		const FProperty* Property = nullptr;
		void* Address = nullptr;
	};

	/**
	 * Steps over the wildcard parameter. From a Blueprint, Step reports the connected property in MostRecentProperty
	 * (JsonBlueprintFunctionLibrary.cpp:84-86 relies on it). Called natively through ProcessEvent there is no bytecode:
	 * StepCompiledIn takes the next property of the parameter chain (Stack.h:479-493) and StepExplicitProperty sets only
	 * the address (ScriptCore.cpp:561-585), so the property is read from the chain before the step. The wildcard is a
	 * reference parameter (CPF_OutParm), so the native step copies nothing into the null result.
	 */
	FWildcard StepWildcard(FFrame& Stack)
	{
		FProperty* const ChainProperty = Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
		Stack.MostRecentProperty = nullptr;
		Stack.MostRecentPropertyAddress = nullptr;
		Stack.StepCompiledIn<FProperty>(nullptr);
		FWildcard Result;
		Result.Property = Stack.MostRecentProperty ? Stack.MostRecentProperty : ChainProperty;
		Result.Address = Stack.MostRecentPropertyAddress;
		return Result;
	}

	/** The wildcard pin had no value (a broken graph): a script exception, as the engine's JSON nodes raise (JsonBlueprintFunctionLibrary.cpp:90-97) */
	void ThrowMissingValue(UObject* ActiveObject, FFrame& Stack, const TCHAR* NodeName)
	{
		const FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation,
			FText::Format(NSLOCTEXT("VaRestJsonLibrary", "MissingValue", "{0}: the Value pin has no value"), FText::FromString(NodeName)));
		FBlueprintCoreDelegates::ThrowScriptException(ActiveObject, Stack, ExceptionInfo);
	}
}

bool UVaRestJsonLibrary::GetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, void* Address)
{
	return VaRest::Json::LibraryPrivate::ReadInto(Json.Get(Key), Property, Address);
}

FVaRestJson UVaRestJsonLibrary::SetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, const void* Address)
{
	using namespace VaRest::Json::LibraryPrivate;
	if (!Json.IsObject())
	{
		// No auto-vivify: the node holds a copy of the handle, so a new object would never reach the caller's variable
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Set Field \"%s\": Json is %s, not an object (use Make Json Object)"), *Key, *TypeOf(Json)), ELogVerbosity::Warning);
		return Json;
	}
	const FVaRestJson NewValue = ValueAsJson(Property, Address, EVaRestJsonNaming::CamelCase);
	if (!NewValue.IsValid() && !IsJsonPin(Property))
	{
		return Json; // the encode failed and logged why; the field keeps its value (Set would read "missing" as remove)
	}
	FVaRestJson Target = Json;
	Target.Set(Key, NewValue);
	return Target;
}

bool UVaRestJsonLibrary::GetPathImpl(const FVaRestJson& Json, const FString& Path, const FProperty* Property, void* Address)
{
	return VaRest::Json::LibraryPrivate::ReadInto(Json.Path(Path), Property, Address);
}

bool UVaRestJsonLibrary::ArrayGetImpl(const FVaRestJson& Json, int32 Index, const FProperty* Property, void* Address)
{
	return VaRest::Json::LibraryPrivate::ReadInto(Json.At(Index), Property, Address);
}

FVaRestJson UVaRestJsonLibrary::ArrayAddImpl(const FVaRestJson& Json, const FProperty* Property, const void* Address)
{
	using namespace VaRest::Json::LibraryPrivate;
	if (!Json.IsArray())
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Array Add: Json is %s, not an array (use Make Json Array)"), *TypeOf(Json)), ELogVerbosity::Warning);
		return Json;
	}
	const FVaRestJson NewValue = ValueAsJson(Property, Address, EVaRestJsonNaming::CamelCase);
	if (!NewValue.IsValid() && !IsJsonPin(Property))
	{
		return Json; // the encode failed and logged why; nothing is appended (Add would write null)
	}
	FVaRestJson Target = Json;
	Target.Add(NewValue);
	return Target;
}

FVaRestJson UVaRestJsonLibrary::StructToJsonImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming)
{
	return VaRest::Json::LibraryPrivate::ValueAsJson(Property, Address, Naming);
}

bool UVaRestJsonLibrary::JsonToStructImpl(const FVaRestJson& Json, const FProperty* Property, void* Address, EVaRestJsonNaming Naming, bool bStrict)
{
	// The output is a Blueprint temporary: reused across loop iterations, and kept between events in the ubergraph's
	// persistent frame. Start from the type's defaults (FStructProperty: ClearScriptStruct, PropertyStruct.cpp:351-353),
	// so "missing fields keep defaults" (spec 2.4) means the type's defaults, never the previous call's values.
	Property->ClearValue(Address);
	FVaRestJsonConvertOptions Options;
	Options.Naming = Naming;
	Options.bStrict = bStrict;
	FVaRestJsonDecodeReport Report;
	const bool bDecoded = FVaRestJsonConvert::JsonToProperty(Json, Property, Address, Options, &Report);
	if (Report.Errors.Num() + Report.Warnings.Num() > 0)
	{
		const FVaRestJsonError& First = Report.Errors.Num() > 0 ? Report.Errors[0] : Report.Warnings[0];
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("Json To Struct: %d issue(s); first at '%s': %s"),
			Report.Errors.Num() + Report.Warnings.Num(), *First.Path, *First.Message), ELogVerbosity::Log);
	}
	return bDecoded;
}

FString UVaRestJsonLibrary::StructToJsonStringImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming, bool bPretty)
{
	return StructToJsonImpl(Property, Address, Naming).ToString(bPretty ? EVaRestJsonFormat::Pretty : EVaRestJsonFormat::Condensed);
}

FVaRestJson UVaRestJsonLibrary::MakeJsonInternalObject()
{
	return FVaRestJson::Object();
}

// Thunks: parameters are read in declaration order; the wildcard's real property comes from StepWildcard
// (JsonBlueprintFunctionLibrary.cpp:79-122 does the same with StepCompiledIn alone).

DEFINE_FUNCTION(UVaRestJsonLibrary::execGetField)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Key);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Get Field"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = GetFieldImpl(Json, Key, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execSetField)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Key);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Set Field"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = Json;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = SetFieldImpl(Json, Key, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execGetPath)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Path);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Get Path"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = GetPathImpl(Json, Path, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execArrayGet)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FIntProperty, Index);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Array Get"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = ArrayGetImpl(Json, Index, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execArrayAdd)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Array Add"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = Json;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = ArrayAddImpl(Json, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execStructToJson)
{
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Struct To Json"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = FVaRestJson();
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = StructToJsonImpl(ValueProperty, ValueAddress, Naming);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execJsonToStruct)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	void* ValueAddress = Wildcard.Address;
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_GET_UBOOL(bStrict);
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Json To Struct"));
		*static_cast<bool*>(RESULT_PARAM) = false;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = JsonToStructImpl(Json, ValueProperty, ValueAddress, Naming, bStrict);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execStructToJsonString)
{
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_GET_ENUM(EVaRestJsonNaming, Naming);
	P_GET_UBOOL(bPretty);
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Struct To Json String"));
		*static_cast<FString*>(RESULT_PARAM) = FString();
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FString*>(RESULT_PARAM) = StructToJsonStringImpl(ValueProperty, ValueAddress, Naming, bPretty);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UVaRestJsonLibrary::execMakeJsonInternalSet)
{
	P_GET_STRUCT_REF(FVaRestJson, Json);
	P_GET_PROPERTY(FStrProperty, Key);
	const VaRest::Json::LibraryPrivate::FWildcard Wildcard = VaRest::Json::LibraryPrivate::StepWildcard(Stack);
	const FProperty* ValueProperty = Wildcard.Property;
	const void* ValueAddress = Wildcard.Address;
	P_FINISH;
	if (!ValueProperty || !ValueAddress)
	{
		VaRest::Json::LibraryPrivate::ThrowMissingValue(P_THIS_OBJECT, Stack, TEXT("Make Json"));
		*static_cast<FVaRestJson*>(RESULT_PARAM) = Json;
		return;
	}
	P_NATIVE_BEGIN;
	*static_cast<FVaRestJson*>(RESULT_PARAM) = SetFieldImpl(Json, Key, ValueProperty, ValueAddress);
	P_NATIVE_END;
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including `VaRest.Json.Library.WildcardImpl` and `VaRest.Json.Library.Thunks`.

- [ ] **Step 6: Restore-the-bug (reset on a miss)**

Delete `Property->ClearValue(Address);` from `ReadInto`. Run `Tools/run_tests.sh VaRest.Json.Library.WildcardImpl`; expect `... and resets the output` to fail. Restore, run, expect green. Then delete the `Property->ClearValue(Address);` at the top of `JsonToStructImpl`; expect `... starts from defaults, not from the previous call` to fail (`Name` keeps `stale`). Restore, run, expect green. Report all four outcomes.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: wildcard Blueprint nodes (Get/Set Field, Get Path, Array Get/Add, Struct <-> Json) via CustomThunk

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 13: Blueprint structs in the editor — cache invalidation and real field names (spec §2.4)

**Engine facts:** a recompiled Blueprint struct keeps its `UUserDefinedStruct` object (`UserDefinedStructureCompilerUtils.cpp:595`) but destroys and rebuilds its properties (`:222-225`), so a cached field map would point at freed `FProperty` objects. The editor announces each recompile to `FStructureEditorUtils::INotifyOnStructChanged` listeners: `BroadcastPreChange` (`UserDefinedStructureCompilerUtils.cpp:599`) before the old properties go and `BroadcastPostChange` (`:661`) after the new ones exist (`StructureEditorUtils.cpp:1026-1044`). A listener registers in its constructor and unregisters in its destructor (`ListenerManager.h:25-32`); the manager is a function-local static (`StructureEditorUtils.cpp:32-36`). Adding, renaming or retyping a member recompiles through `OnStructureChanged` (`StructureEditorUtils.cpp:569-580`). A member's property name is `<Name>_<UniqueId>_<GUID>`, where `<Name>` is the friendly name with invalid object-name characters (spaces included) removed (`StructureEditorUtils.cpp:234-262`, `UObjectGlobals.cpp:2803-2824`, `String.cpp.inl:2033-2047`); a rename regenerates it (`StructureEditorUtils.cpp:347-373`). In the editor `GetAuthoredName()` returns the friendly name ("Player Score"); a cooked game strips the property name ("PlayerScore") (`UserDefinedStruct.cpp:290-313`). New structs start with one bool member (`StructureEditorUtils.cpp:42-63`); they are enabled by `BaseEditor.ini:336`.

**Files:**
- Modify: `Source/VaRestEditor/VaRestEditor.Build.cs` (`VaRestJson`, private)
- Create: `Source/VaRestEditor/Private/VaRestJsonStructListener.h`, `Source/VaRestEditor/Private/VaRestJsonStructListener.cpp`
- Modify (bodies only, controller ruling R2): `Source/VaRestEditor/Private/VaRestEditor.cpp` — one call each in `FVaRestEditorModule::StartupModule` / `ShutdownModule`; no signature, path or type changes
- Test: `Source/VaRestEditor/Private/Tests/VaRestJsonUserStructTest.cpp`

**Interfaces:**
- Consumes: `FVaRestJsonConvert::ResetCache()`, `NumCachedFieldMapsInternal()` (Task 6); `StructToString`, `JsonToStruct` (Tasks 7–8).
- Produces: `void VaRestEditor::StructListener::Start()` / `Stop()` (`VaRestJsonStructListener.h`), called from the module's `StartupModule` / `ShutdownModule`.

Decisions: the listener's lifetime belongs to the module (controller ruling R2): a raw pointer created in `StartupModule` and deleted in `ShutdownModule` through two free functions, because the v1 module class (`Source/VaRestEditor/Public/VaRestEditor.h`) keeps its type and cannot become the listener the way engine editor modules do (`StructUtilsEditorModule.h:52`). A raw pointer has no exit-time destructor: `ShutdownModule` does not run when an `Automation RunTests …; Quit` run exits (CLAUDE.md), and a static owner would then delete the listener during static destruction, possibly after the manager it unregisters from (a function-local static, `StructureEditorUtils.cpp:32-36`) if our `StartupModule` was the manager's first use. Leaking it at exit is accepted and harmless: the manager's destructor only frees its `TSet` (`ListenerManager.h:36`) and calls no listener; every change of any Blueprint struct drops the whole cache (struct edits are rare, editor-only, and a struct embedding the changed one is covered without tracking dependencies).

- [ ] **Step 1: Build rule** — in `Source/VaRestEditor/VaRestEditor.Build.cs` add to `PrivateDependencyModuleNames`, after `"RenderCore"`:

```csharp
                "RenderCore",
                // VaRest 2: struct <-> JSON cache invalidation, the v2 Make Json node, their tests
                "VaRestJson"
```

(`"RenderCore"` gains a trailing comma; nothing else in the v1 file changes.)

- [ ] **Step 2: Write the failing tests** — `Source/VaRestEditor/Private/Tests/VaRestJsonUserStructTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"

namespace VaRestJsonUserStructTest
{
	FEdGraphPinType PinType(FName Category)
	{
		return FEdGraphPinType(Category, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	}

	/** A transient Blueprint struct: the bool every new struct starts with, renamed to BoolName, then an int named IntName */
	UUserDefinedStruct* MakeStruct(const TCHAR* BoolName, const TCHAR* IntName)
	{
		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), FName(TEXT("VaRestJsonTestStruct"))), RF_Transient);
		if (!Struct)
		{
			return nullptr;
		}
		FStructureEditorUtils::AddVariable(Struct, PinType(UEdGraphSchema_K2::PC_Int));
		const FGuid BoolGuid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
		const FGuid IntGuid = FStructureEditorUtils::GetVarDesc(Struct)[1].VarGuid;
		FStructureEditorUtils::RenameVariable(Struct, BoolGuid, BoolName);
		FStructureEditorUtils::RenameVariable(Struct, IntGuid, IntName);
		return Struct;
	}

	template <typename PropertyType>
	PropertyType* FirstField(const UUserDefinedStruct* Struct)
	{
		TFieldIterator<PropertyType> It(Struct);
		return It ? *It : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUserStructNamesTest, "VaRest.Json.Convert.UserStruct.Names",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUserStructNamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	UUserDefinedStruct* Struct = MakeStruct(TEXT("Is Alive"), TEXT("Player Score"));
	if (!TestNotNull(TEXT("Blueprint structs are enabled (BaseEditor.ini [UserDefinedStructure])"), Struct))
	{
		return false;
	}
	FBoolProperty* Alive = FirstField<FBoolProperty>(Struct);
	FIntProperty* Score = FirstField<FIntProperty>(Struct);
	if (!TestTrue(TEXT("both members exist"), Alive && Score))
	{
		return false;
	}
	TestTrue(TEXT("the property name is <Name>_<id>_<GUID>"), Score->GetName().StartsWith(TEXT("PlayerScore_"), ESearchCase::CaseSensitive) && Score->GetName().Len() > 32 + 3);
	// The divergence the converter avoids: the editor's authored name keeps the space, a cooked game's does not
	TestEqualSensitive(TEXT("editor GetAuthoredName is the friendly name"), Score->GetAuthoredName(), FString(TEXT("Player Score")));

	FStructOnScope Instance(Struct);
	FVaRestJsonConvertOptions AsIs;
	AsIs.Naming = EVaRestJsonNaming::AsIs;
	TestEqualSensitive(TEXT("AsIs writes the cooked name in the editor too"),
		FVaRestJsonConvert::StructToString(Struct, Instance.GetStructMemory(), AsIs), FString(TEXT("{\"IsAlive\":false,\"PlayerScore\":0}")));
	TestEqualSensitive(TEXT("camelCase"),
		FVaRestJsonConvert::StructToString(Struct, Instance.GetStructMemory()), FString(TEXT("{\"isAlive\":false,\"playerScore\":0}")));

	const FVaRestJson Body = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"is_alive\":true,\"PlayerScore\":5}"))).StealValue();
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("decode by other spellings"), FVaRestJsonConvert::JsonToStruct(Body, Struct, Instance.GetStructMemory(), FVaRestJsonConvertOptions(), &Report));
	TestTrue(TEXT("... fills both members"), Alive->GetPropertyValue_InContainer(Instance.GetStructMemory()) && Score->GetPropertyValue_InContainer(Instance.GetStructMemory()) == 5);
	TestEqual(TEXT("... without warnings"), Report.Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUserStructCacheTest, "VaRest.Json.Convert.UserStruct.CacheInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUserStructCacheTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUserStructTest;
	UUserDefinedStruct* Struct = MakeStruct(TEXT("Alive"), TEXT("Score"));
	if (!TestNotNull(TEXT("Blueprint struct"), Struct))
	{
		return false;
	}
	FVaRestJsonConvert::ResetCache();
	{
		FStructOnScope Before(Struct);
		TestEqualSensitive(TEXT("before the change"), FVaRestJsonConvert::StructToString(Struct, Before.GetStructMemory()), FString(TEXT("{\"alive\":false,\"score\":0}")));
	}
	TestTrue(TEXT("the field map is cached"), FVaRestJsonConvert::NumCachedFieldMapsInternal() > 0);

	TestTrue(TEXT("rename Score to Points"), FStructureEditorUtils::RenameVariable(Struct, FStructureEditorUtils::GetVarDesc(Struct)[1].VarGuid, TEXT("Points")));
	// The recompile destroyed the old properties: a map that survived would point at freed memory, so stop before using it
	if (!TestEqual(TEXT("the rename dropped the cached maps"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 0))
	{
		return false;
	}
	{
		FStructOnScope Renamed(Struct);
		TestEqualSensitive(TEXT("after a rename"), FVaRestJsonConvert::StructToString(Struct, Renamed.GetStructMemory()), FString(TEXT("{\"alive\":false,\"points\":0}")));
	}

	TestTrue(TEXT("add a string member"), FStructureEditorUtils::AddVariable(Struct, PinType(UEdGraphSchema_K2::PC_String)));
	if (!TestEqual(TEXT("adding a member dropped the cached maps"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 0))
	{
		return false;
	}
	FStructOnScope Added(Struct);
	// A new member is MemberVar_<id>_<GUID>; the cooked algorithm drops the id too
	TestEqualSensitive(TEXT("after adding a member"), FVaRestJsonConvert::StructToString(Struct, Added.GetStructMemory()),
		FString(TEXT("{\"alive\":false,\"points\":0,\"memberVar\":\"\"}")));
	return true;
}

#endif
```

- [ ] **Step 3: Run to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json.Convert.UserStruct`
Expected: `VaRest.Json.Convert.UserStruct.Names` passes (Task 6 already implements the cooked algorithm); `CacheInvalidation` FAILS at `the rename dropped the cached maps` (nothing listens yet) and returns before touching the stale map.

- [ ] **Step 4: The listener** — `Source/VaRestEditor/Private/VaRestJsonStructListener.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

namespace VaRestEditor::StructListener
{
	/** From FVaRestEditorModule::StartupModule: drop the struct <-> JSON field-map cache whenever a Blueprint struct changes */
	void Start();

	/** From FVaRestEditorModule::ShutdownModule (which does not run when an automation run quits) */
	void Stop();
}
```

`Source/VaRestEditor/Private/VaRestJsonStructListener.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonStructListener.h"

#include "Kismet2/StructureEditorUtils.h"
#include "VaRestJsonConvert.h"

namespace VaRestEditorStructListenerPrivate
{
	/**
	 * Drops the struct <-> JSON field-map cache whenever a Blueprint struct changes. A recompiled struct keeps its
	 * UObject (UserDefinedStructureCompilerUtils.cpp:595) but destroys its properties (:222-225), so a cached map
	 * would point at freed FProperty objects. PreChange runs before the old properties go (:599), PostChange after the
	 * new ones exist (:661); resetting at both leaves no map built from a half-compiled struct.
	 */
	class FStructListener final : public FStructureEditorUtils::INotifyOnStructChanged
	{
	public:
		virtual void PreChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override
		{
			FVaRestJsonConvert::ResetCache();
		}

		virtual void PostChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override
		{
			FVaRestJsonConvert::ResetCache();
		}
	};

	/**
	 * A raw pointer on purpose: it has no exit-time destructor. ShutdownModule does not run when an automation run quits,
	 * and a static owner (TUniquePtr) would then delete the listener during static destruction, possibly after the
	 * manager it unregisters from -- a function-local static (StructureEditorUtils.cpp:32-36) -- if our StartupModule was
	 * its first use. Left registered at exit it is harmless: the manager's destructor frees its TSet (ListenerManager.h:36)
	 * and calls no listener.
	 */
	FStructListener* GListener = nullptr;
}

void VaRestEditor::StructListener::Start()
{
	using namespace VaRestEditorStructListenerPrivate;
	if (!GListener)
	{
		GListener = new FStructListener(); // registers itself (ListenerManager.h:27)
	}
}

void VaRestEditor::StructListener::Stop()
{
	using namespace VaRestEditorStructListenerPrivate;
	delete GListener; // unregisters itself (ListenerManager.h:31); UnrealEd is still loaded (we depend on it)
	GListener = nullptr;
}
```

`Source/VaRestEditor/Private/VaRestEditor.cpp` (bodies only): add `#include "VaRestJsonStructListener.h"` after `#include "Modules/ModuleManager.h"`; make `VaRestEditor::StructListener::Start();` the last line of `StartupModule`'s body and `VaRestEditor::StructListener::Stop();` the last line of `ShutdownModule`'s body. Keep the v1 comments; change nothing else in the file.

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including both `VaRest.Json.Convert.UserStruct.*` tests.

- [ ] **Step 6: Restore-the-bug (cache invalidation)**

Empty both overrides' bodies (delete the two `FVaRestJsonConvert::ResetCache();` lines in `FStructListener`). Run `Tools/run_tests.sh VaRest.Json.Convert.UserStruct.CacheInvalidation`; expect `the rename dropped the cached maps` to fail (and the test to stop there). Restore, run, expect green. Report both.

- [ ] **Step 7: Restore-the-bug (field-name algorithm on a real Blueprint struct)**

In `StripGuidSuffix` (`VaRestJsonConvertNames.cpp`) change `PropertyName.LeftChop(GuidStrLen + 1)` to `PropertyName.LeftChop(GuidStrLen)`. Run `Tools/run_tests.sh VaRest.Json.Convert.UserStruct.Names`; expect `AsIs writes the cooked name in the editor too` to fail (`PlayerScore_<id>` instead of `PlayerScore`). Restore, run, expect green. Report both.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestEditor
git commit -m "editor: drop the struct <-> JSON cache on Blueprint struct changes (listener owned by the module); BP struct name tests

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 14: The v2 `Make Json` K2 node (spec §2.5)

**Engine facts:** a node implementing `IK2Node_AddPinInterface` gets the "+ Add pin" button (`NodeFactory.cpp:132` → `SGraphNodeK2Sequence.cpp:54-67`, which calls `AddInputPin`; interface `K2Node_AddPinInterface.h:64-89`); node properties show in the Details panel only when `ShouldShowNodeProperties()` is true (`K2Node.h:273`). `UK2Node::ReconstructNode` resets pins, calls `ReallocatePinsDuringReconstruction(OldPins)` (default: `AllocateDefaultPins` + `RestoreSplitPins`, `K2Node.cpp:646-651`), rewires old links to new pins by name, then `PostReconstructNode` (`K2Node.cpp:753-761`). Connection changes reach `NotifyPinConnectionListChanged` (`K2Node.cpp:515-600`). Expansion: `SpawnIntermediateNode` (`KismetCompiler.h:245`), `MovePinLinksToIntermediate` (`:270`); a `CustomStructureParam` pin on an intermediate call takes the original pin's type before its links move (`K2Node_GetDataTableRow.cpp:405-407`), the same type `FCustomStructureParamHelper` gives a linked wildcard (`K2Node_CallFunction.cpp:111-147`, the copy at `:132`). Pin names are `FName`s (case-insensitive), so pins cannot be named after case-sensitive keys. Compiler messages go to the log unless the log is silent (`CompilerResultsLog.cpp:448`). Context-menu pin removal follows `K2Node_DoOnceMultiInput.cpp:283-306` (`UToolMenu`, module `ToolMenus`).

**Files:**
- Modify: `Source/VaRestEditor/VaRestEditor.Build.cs` (`ToolMenus`, private)
- Create: `Source/VaRestEditor/Private/K2Node_VaRestMakeJson.h`, `Source/VaRestEditor/Private/K2Node_VaRestMakeJson.cpp`
- Create: `Source/VaRestEditor/Private/Tests/VaRestJsonBlueprintTestUtils.h` (Blueprint-building helpers, also used by Task 15)
- Test: `Source/VaRestEditor/Private/Tests/VaRestJsonMakeJsonNodeTest.cpp`

**Interfaces:**
- Consumes: `UVaRestJsonLibrary::MakeJsonInternalObject`, `MakeJsonInternalSet` (Task 12).
- Produces: `USTRUCT FVaRestMakeJsonEntry { FString Key; int32 Id; }`; `UCLASS UK2Node_VaRestMakeJson : UK2Node, IK2Node_AddPinInterface` with `TArray<FVaRestMakeJsonEntry> Entries`, `UEdGraphPin* GetJsonPin() const`, `UEdGraphPin* GetValuePin(int32 Index) const`, `int32 IndexOfValuePin(const UEdGraphPin*) const`, `AddInputPin()`, `RemoveInputPin(UEdGraphPin*)`, `static const FName JsonPinName` (`"Json"`); test helpers in `VaRest::Json::BlueprintTest`: `FTestBlueprint { UBlueprint* Blueprint; UEdGraph* Graph; UK2Node_FunctionEntry* Entry; }`, `FTestBlueprint MakeBlueprint(const TCHAR* BaseName)`, `UK2Node_CallFunction* AddCall(UEdGraph*, UClass*, FName)`, `UK2Node_VariableSet* AddVariableSet(UEdGraph*, FName)`, `bool Link(UEdGraphPin* From, UEdGraphPin* To)`, `bool AddVariable(UBlueprint*, FName VariableName, FName PinCategory)`, `bool Compile(UBlueprint*, FCompilerResultsLog&)`, `UObject* Run(UBlueprint*)`, `FString Messages(const FCompilerResultsLog&)`.

Decisions: value pins are named `Value_<Id>` with a stable per-entry id and show the key as their friendly name (keys stay case-sensitive; removing a middle key does not shift other pins' links); keys are edited in the Details panel, added with "+", removed from the pin's context menu; the node is pure, like Make Array (each consumer gets its own object; `Make Json Object` + `Set Field` builds one incrementally); it is titled "Make Json" in category "VaRest|Json" — v1's node keeps its own title in "Utilities|VaRest"; an empty key, an exact duplicate key or an unconnected value is a compile error; pins refuse exec, object, class, interface and delegate connections (objects are never converted, Task 6 `IsSupported`).

- [ ] **Step 1: Test helpers** — `Source/VaRestEditor/Private/Tests/VaRestJsonBlueprintTestUtils.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Package.h"

namespace VaRest::Json::BlueprintTest
{
	/** A transient Blueprint of UObject (Kismet2.cpp:1080-1081 allows it) with an empty user function "Run" */
	struct FTestBlueprint
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
	};

	inline FTestBlueprint MakeBlueprint(const TCHAR* BaseName)
	{
		FTestBlueprint Result;
		const FName Name = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), FName(BaseName));
		Result.Blueprint = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), GetTransientPackage(), Name, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Result.Blueprint)
		{
			return Result;
		}
		Result.Graph = FBlueprintEditorUtils::CreateNewGraph(Result.Blueprint, FName(TEXT("Run")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Result.Blueprint, Result.Graph, true, nullptr);
		TArray<UK2Node_FunctionEntry*> FunctionEntries;
		Result.Graph->GetNodesOfClass(FunctionEntries);
		Result.Entry = FunctionEntries.Num() > 0 ? FunctionEntries[0] : nullptr;
		return Result;
	}

	inline UK2Node_CallFunction* AddCall(UEdGraph* Graph, UClass* Class, FName FunctionName)
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode(false);
		Node->SetFromFunction(Class->FindFunctionByName(FunctionName));
		Creator.Finalize();
		return Node;
	}

	/** A Set node for a member variable; add the variable first (its pins come from the skeleton class) */
	inline UK2Node_VariableSet* AddVariableSet(UEdGraph* Graph, FName VariableName)
	{
		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* Node = Creator.CreateNode(false);
		Node->VariableReference.SetSelfMember(VariableName);
		Creator.Finalize();
		return Node;
	}

	inline bool AddVariable(UBlueprint* Blueprint, FName VariableName, FName Category)
	{
		return FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName,
			FEdGraphPinType(Category, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	}

	/** TryCreateConnection notifies both nodes, so wildcard pins take the linked type (EdGraphSchema.cpp:518-524) */
	inline bool Link(UEdGraphPin* From, UEdGraphPin* To)
	{
		return From && To && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To);
	}

	/** Compiles with a silent log: messages stay in Results instead of reaching LogBlueprint (CompilerResultsLog.cpp:448) */
	inline bool Compile(UBlueprint* Blueprint, FCompilerResultsLog& Results)
	{
		Results.SetSilentMode(true);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		return Results.NumErrors == 0 && Blueprint->Status != BS_Error;
	}

	inline FString Messages(const FCompilerResultsLog& Results)
	{
		FString Text;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			Text += Message->ToText().ToString() + TEXT("\n");
		}
		return Text;
	}

	/** Calls the Blueprint's "Run" on a new instance and returns the instance */
	inline UObject* Run(UBlueprint* Blueprint)
	{
		UObject* Instance = NewObject<UObject>(GetTransientPackage(), Blueprint->GeneratedClass);
		UFunction* Function = Instance->FindFunction(FName(TEXT("Run")));
		if (!Function)
		{
			return nullptr;
		}
		Instance->ProcessEvent(Function, nullptr);
		return Instance;
	}
}

#endif
```

- [ ] **Step 2: Write the failing test** — `Source/VaRestEditor/Private/Tests/VaRestJsonMakeJsonNodeTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "K2Node_VaRestMakeJson.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "Tests/VaRestJsonBlueprintTestUtils.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMakeJsonNodeTest, "VaRest.Json.Blueprint.MakeJsonNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMakeJsonNodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestMakeJsonNodeTest"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_VaRestMakeJson> Creator(*Test.Graph);
	UK2Node_VaRestMakeJson* Node = Creator.CreateNode(false);
	Creator.Finalize();

	const UEdGraphPin* JsonPin = Node->GetJsonPin();
	TestTrue(TEXT("the output is a VaRest Json"), JsonPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && JsonPin->PinType.PinSubCategoryObject == FVaRestJson::StaticStruct());
	TestEqual(TEXT("a new node has no keys"), Node->Entries.Num(), 0);

	Node->AddInputPin();
	Node->AddInputPin();
	TestEqualSensitive(TEXT("added keys are unique"), Node->Entries[0].Key + TEXT(",") + Node->Entries[1].Key, FString(TEXT("key,key2")));

	// Review Focus: keys that differ only by case are two pins and two JSON keys
	Node->Entries[0].Key = TEXT("id");
	Node->Entries[1].Key = TEXT("ID");
	Node->ReconstructNode();
	UEdGraphPin* IdPin = Node->GetValuePin(0);
	UEdGraphPin* UpperPin = Node->GetValuePin(1);
	if (!TestTrue(TEXT("\"id\" and \"ID\" are two pins"), IdPin && UpperPin && IdPin != UpperPin))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a pin shows its key"), IdPin->PinFriendlyName.ToString() + TEXT(",") + UpperPin->PinFriendlyName.ToString(), FString(TEXT("id,ID")));
	TestTrue(TEXT("a new value pin is a wildcard"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard);

	UK2Node_CallFunction* Literal = AddCall(Test.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt));
	TestTrue(TEXT("connect an int"), Link(Literal->GetReturnValuePin(), IdPin));
	TestTrue(TEXT("the pin takes the connected type"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
	Node->ReconstructNode();
	IdPin = Node->GetValuePin(0);
	TestTrue(TEXT("reconstruction keeps the type and the link"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int && IdPin->LinkedTo.Num() == 1);
	IdPin->BreakAllPinLinks(true);
	TestTrue(TEXT("disconnecting makes it a wildcard again"), IdPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard);

	FString Reason;
	TestTrue(TEXT("an exec pin cannot be a value"), Node->IsConnectionDisallowed(IdPin, Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), Reason));

	FCompilerResultsLog Log;
	Log.SetSilentMode(true);
	Node->ValidateNodeDuringCompilation(Log);
	TestEqual(TEXT("two unconnected values are two errors"), Log.NumErrors, 2);
	Node->Entries[1].Key = TEXT("id");
	FCompilerResultsLog DuplicateLog;
	DuplicateLog.SetSilentMode(true);
	Node->ValidateNodeDuringCompilation(DuplicateLog);
	TestEqual(TEXT("an exact duplicate key is one more error"), DuplicateLog.NumErrors, 3);

	const FName FirstPinName = IdPin->PinName;
	Node->RemoveInputPin(IdPin);
	TestTrue(TEXT("Remove key drops the entry and its pin"), Node->Entries.Num() == 1 && Node->FindPin(FirstPinName) == nullptr && Node->GetValuePin(0) != nullptr);
	return true;
}

#endif
```

- [ ] **Step 3: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Blueprint.MakeJsonNode`
Expected: compile error (`K2Node_VaRestMakeJson.h` not found).

- [ ] **Step 4: Build rule** — in `VaRestEditor.Build.cs`, after the `"VaRestJson"` line Task 13 added:

```csharp
                // VaRest 2: the v2 Make Json node's "Remove key" context menu (UToolMenu)
                "ToolMenus",
                "VaRestJson"
```

(i.e. insert `"ToolMenus",` with its comment before `"VaRestJson"`.)

- [ ] **Step 5: Node header** — `Source/VaRestEditor/Private/K2Node_VaRestMakeJson.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "K2Node_AddPinInterface.h"

#include "K2Node_VaRestMakeJson.generated.h"

/** One key of a Make Json node */
USTRUCT()
struct FVaRestMakeJsonEntry
{
	GENERATED_BODY()

	/** The JSON key, exactly as written: keys are case-sensitive ("id" and "ID" are two keys) */
	UPROPERTY(EditAnywhere, Category = "Make Json")
	FString Key;

	/** Names the value pin. Pins are FNames, which ignore case, so they cannot be named after keys. 0 = not assigned yet. */
	UPROPERTY()
	int32 Id = 0;
};

/**
 * VaRest 2 Make Json (spec 2.5): a JSON object from named values, each value pin a wildcard that takes the connected
 * type. Pure, like Make Array: every consumer gets its own object. Expands to MakeJsonInternalObject and one
 * MakeJsonInternalSet per key.
 */
UCLASS()
class UK2Node_VaRestMakeJson : public UK2Node, public IK2Node_AddPinInterface
{
	GENERATED_BODY()

public:
	/** The keys, in output order. Edit them in the Details panel; "+" adds one, a value pin's context menu removes it. */
	UPROPERTY(EditAnywhere, Category = "Make Json")
	TArray<FVaRestMakeJsonEntry> Entries;

	static const FName JsonPinName;

	// UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;

	// UK2Node
	virtual bool IsNodePure() const override { return true; }
	virtual bool ShouldShowNodeProperties() const override { return true; }
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void PostReconstructNode() override;
	virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual bool IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;

	// IK2Node_AddPinInterface
	virtual void AddInputPin() override;
	virtual void RemoveInputPin(UEdGraphPin* Pin) override;
	virtual bool CanRemovePin(const UEdGraphPin* Pin) const override;

	UEdGraphPin* GetJsonPin() const;

	/** The value pin of Entries[Index], or null */
	UEdGraphPin* GetValuePin(int32 Index) const;

	/** Index into Entries of one of this node's value pins; INDEX_NONE for any other pin */
	int32 IndexOfValuePin(const UEdGraphPin* Pin) const;

private:
	UPROPERTY()
	int32 NextId = 1;

	static FName PinNameFor(int32 Id);

	/** Entries added or duplicated in the Details panel arrive with Id 0 or a copied Id */
	void AssignEntryIds();

	void CreateValuePin(const FVaRestMakeJsonEntry& Entry);

	/** A linked value pin has the linked type; an unlinked one is a wildcard */
	static void SyncValuePinType(UEdGraphPin* ValuePin);
};
```

- [ ] **Step 6: Node implementation** — `Source/VaRestEditor/Private/K2Node_VaRestMakeJson.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "K2Node_VaRestMakeJson.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Commands/UIAction.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompiler.h"
#include "ScopedTransaction.h"
#include "Textures/SlateIcon.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "VaRestJson.h"
#include "VaRestJsonLibrary.h"

#define LOCTEXT_NAMESPACE "K2Node_VaRestMakeJson"

const FName UK2Node_VaRestMakeJson::JsonPinName(TEXT("Json"));

FName UK2Node_VaRestMakeJson::PinNameFor(int32 Id)
{
	return FName(*FString::Printf(TEXT("Value_%d"), Id));
}

void UK2Node_VaRestMakeJson::AssignEntryIds()
{
	TSet<int32> Used;
	for (FVaRestMakeJsonEntry& Entry : Entries)
	{
		bool bAlreadyUsed = false;
		Used.Add(Entry.Id, &bAlreadyUsed);
		if (Entry.Id <= 0 || bAlreadyUsed)
		{
			Entry.Id = NextId++;
			Used.Add(Entry.Id);
		}
		NextId = FMath::Max(NextId, Entry.Id + 1);
	}
}

void UK2Node_VaRestMakeJson::CreateValuePin(const FVaRestMakeJsonEntry& Entry)
{
	UEdGraphPin* ValuePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, PinNameFor(Entry.Id));
	ValuePin->PinFriendlyName = Entry.Key.IsEmpty() ? LOCTEXT("EmptyKey", "(empty key)") : FText::FromString(Entry.Key);
}

void UK2Node_VaRestMakeJson::AllocateDefaultPins()
{
	AssignEntryIds();
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Struct, FVaRestJson::StaticStruct(), JsonPinName);
	for (const FVaRestMakeJsonEntry& Entry : Entries)
	{
		CreateValuePin(Entry);
	}
	Super::AllocateDefaultPins();
}

FText UK2Node_VaRestMakeJson::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("Title", "Make Json");
}

FText UK2Node_VaRestMakeJson::GetTooltipText() const
{
	return LOCTEXT("Tooltip", "VaRest 2: a JSON object from named values. Each value pin takes whatever is connected (number, string, struct, array, map, Json); structs use camelCase field names. Keys are case-sensitive; edit them in the Details panel.\nPure, like Make Array: every node that reads the output gets its own new object. To add fields with Set Field, use Set Field's return value (or Make Json Object) - a second reader of this pin would not see them.");
}

UEdGraphPin* UK2Node_VaRestMakeJson::GetJsonPin() const
{
	return FindPinChecked(JsonPinName, EGPD_Output);
}

UEdGraphPin* UK2Node_VaRestMakeJson::GetValuePin(int32 Index) const
{
	return Entries.IsValidIndex(Index) ? FindPin(PinNameFor(Entries[Index].Id), EGPD_Input) : nullptr;
}

int32 UK2Node_VaRestMakeJson::IndexOfValuePin(const UEdGraphPin* Pin) const
{
	if (!Pin || Pin->Direction != EGPD_Input || Pin->GetOwningNodeUnchecked() != this)
	{
		return INDEX_NONE;
	}
	return Entries.IndexOfByPredicate([Pin](const FVaRestMakeJsonEntry& Entry) { return PinNameFor(Entry.Id) == Pin->PinName; });
}

void UK2Node_VaRestMakeJson::SyncValuePinType(UEdGraphPin* ValuePin)
{
	if (ValuePin->LinkedTo.Num() > 0 && ValuePin->LinkedTo[0]->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
	{
		ValuePin->PinType = ValuePin->LinkedTo[0]->PinType; // what FCustomStructureParamHelper does (K2Node_CallFunction.cpp:132)
		return;
	}
	if (ValuePin->LinkedTo.Num() == 0)
	{
		ValuePin->PinType = FEdGraphPinType();
		ValuePin->PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
	}
}

void UK2Node_VaRestMakeJson::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	Super::ReallocatePinsDuringReconstruction(OldPins); // AllocateDefaultPins + RestoreSplitPins (K2Node.cpp:646-651)
	// New value pins start as wildcards; give each the type its old pin had, so the by-name rewiring that follows
	// (K2Node.cpp:758) reconnects pins of the same type
	for (const UEdGraphPin* OldPin : OldPins)
	{
		if (OldPin->Direction == EGPD_Input && OldPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			if (UEdGraphPin* NewPin = FindPin(OldPin->PinName, EGPD_Input))
			{
				NewPin->PinType = OldPin->PinType;
			}
		}
	}
}

void UK2Node_VaRestMakeJson::PostReconstructNode()
{
	Super::PostReconstructNode();
	for (UEdGraphPin* NodePin : Pins)
	{
		if (IndexOfValuePin(NodePin) != INDEX_NONE)
		{
			SyncValuePinType(NodePin);
		}
	}
}

void UK2Node_VaRestMakeJson::NotifyPinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::NotifyPinConnectionListChanged(Pin);
	if (IndexOfValuePin(Pin) != INDEX_NONE)
	{
		SyncValuePinType(Pin);
		GetGraph()->NotifyNodeChanged(this);
	}
}

bool UK2Node_VaRestMakeJson::IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const
{
	if (IndexOfValuePin(MyPin) != INDEX_NONE)
	{
		const FName Category = OtherPin->PinType.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_Exec || Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_Interface || Category == UEdGraphSchema_K2::PC_Delegate || Category == UEdGraphSchema_K2::PC_MCDelegate)
		{
			OutReason = LOCTEXT("NotJson", "Make Json values must be data: objects, classes, delegates and exec are not converted to JSON").ToString();
			return true;
		}
	}
	return Super::IsConnectionDisallowed(MyPin, OtherPin, OutReason);
}

void UK2Node_VaRestMakeJson::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	Super::ValidateNodeDuringCompilation(MessageLog);
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const FString& Key = Entries[Index].Key;
		// FCompilerResultsLog tokenizes "@@" in the format (CompilerResultsLog.h:160-190): a key must not supply one
		const FString SafeKey = Key.Replace(TEXT("@@"), TEXT("@ @"));
		if (Key.IsEmpty())
		{
			MessageLog.Error(*LOCTEXT("EmptyKeyError", "@@ has a value with an empty key").ToString(), this);
		}
		// Case-sensitive on purpose: TSet<FString> would treat "id" and "ID" as one key
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			if (!Key.IsEmpty() && Entries[Earlier].Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				MessageLog.Error(*FString::Printf(TEXT("@@: the key \"%s\" appears twice"), *SafeKey), this);
				break;
			}
		}
		const UEdGraphPin* ValuePin = GetValuePin(Index);
		if (!ValuePin || ValuePin->LinkedTo.Num() == 0 || ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			MessageLog.Error(*FString::Printf(TEXT("@@: connect a value for the key \"%s\""), *SafeKey), this);
		}
	}
}

void UK2Node_VaRestMakeJson::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	UK2Node_CallFunction* MakeObject = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	MakeObject->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonInternalObject), UVaRestJsonLibrary::StaticClass());
	MakeObject->AllocateDefaultPins();
	UEdGraphPin* Current = MakeObject->GetReturnValuePin();

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		UEdGraphPin* ValuePin = GetValuePin(Index);
		if (!ValuePin || ValuePin->LinkedTo.Num() == 0 || ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			continue; // ValidateNodeDuringCompilation reported it; the compile fails
		}
		UK2Node_CallFunction* SetNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		SetNode->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonInternalSet), UVaRestJsonLibrary::StaticClass());
		SetNode->AllocateDefaultPins();
		Current->MakeLinkTo(SetNode->FindPinChecked(FName(TEXT("Json"))));
		SetNode->FindPinChecked(FName(TEXT("Key")))->DefaultValue = Entries[Index].Key;
		UEdGraphPin* SetValuePin = SetNode->FindPinChecked(FName(TEXT("Value")));
		// The CustomStructureParam pin takes our type before the links move (K2Node_GetDataTableRow.cpp:405-407)
		SetValuePin->PinType = ValuePin->PinType;
		CompilerContext.MovePinLinksToIntermediate(*ValuePin, *SetValuePin);
		Current = SetNode->GetReturnValuePin();
	}
	CompilerContext.MovePinLinksToIntermediate(*GetJsonPin(), *Current);
	BreakAllNodeLinks();
}

void UK2Node_VaRestMakeJson::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	// Registered under the node class, as v1's Make Json does (VaRest_BreakJson.cpp:483-502)
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);
		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_VaRestMakeJson::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "VaRest|Json");
}

void UK2Node_VaRestMakeJson::AddInputPin()
{
	Modify();
	FVaRestMakeJsonEntry Entry;
	Entry.Id = NextId++;
	Entry.Key = TEXT("key");
	for (int32 Suffix = 2; Entries.ContainsByPredicate([&Entry](const FVaRestMakeJsonEntry& Other) { return Other.Key.Equals(Entry.Key, ESearchCase::CaseSensitive); }); ++Suffix)
	{
		Entry.Key = FString::Printf(TEXT("key%d"), Suffix);
	}
	Entries.Add(Entry);
	CreateValuePin(Entry);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
}

bool UK2Node_VaRestMakeJson::CanRemovePin(const UEdGraphPin* Pin) const
{
	return IndexOfValuePin(Pin) != INDEX_NONE;
}

void UK2Node_VaRestMakeJson::RemoveInputPin(UEdGraphPin* Pin)
{
	const int32 Index = IndexOfValuePin(Pin);
	if (Index == INDEX_NONE)
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("RemoveKeyTransaction", "Remove Make Json Key"));
	Modify();
	Pin->BreakAllPinLinks(true);
	Entries.RemoveAt(Index);
	RemovePin(Pin); // EdGraphNode.cpp:464-485
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
}

void UK2Node_VaRestMakeJson::GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	Super::GetNodeContextMenuActions(Menu, Context);
	if (Context->bIsDebugging || !Context->Pin || !CanRemovePin(Context->Pin))
	{
		return;
	}
	// As K2Node_DoOnceMultiInput.cpp:291-305 offers "Remove pin"
	FToolMenuSection& Section = Menu->AddSection(FName(TEXT("VaRestMakeJson")), LOCTEXT("MenuSection", "Make Json"));
	Section.AddMenuEntry(FName(TEXT("RemoveKey")), LOCTEXT("RemoveKey", "Remove key"), LOCTEXT("RemoveKeyTooltip", "Remove this key and its value pin"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateUObject(const_cast<UK2Node_VaRestMakeJson*>(this), &UK2Node_VaRestMakeJson::RemoveInputPin, const_cast<UEdGraphPin*>(Context->Pin))));
}

void UK2Node_VaRestMakeJson::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// A key edited in the Details panel: new pins and names (v1's Make Json does the same, VaRest_BreakJson.cpp:468-481)
	AssignEntryIds();
	ReconstructNode();
	GetGraph()->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(GetBlueprint());
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

#undef LOCTEXT_NAMESPACE
```

(`IndexOfValuePin` uses `GetOwningNodeUnchecked()`, `EdGraphPin.h:469`: `GetOwningNode()` checks the owner is set, `:468`, and the context menu can pass any pin.)

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including `VaRest.Json.Blueprint.MakeJsonNode`.

- [ ] **Step 8: Restore-the-bug (keys that differ only by case)**

Name pins after keys everywhere: in `CreateValuePin`, `GetValuePin` and `IndexOfValuePin` replace `PinNameFor(Entry.Id)` / `PinNameFor(Entries[Index].Id)` with `FName(*Entry.Key)` / `FName(*Entries[Index].Key)`. Run `Tools/run_tests.sh VaRest.Json.Blueprint.MakeJsonNode`; expect `"id" and "ID" are two pins` to fail for the right reason: `FindPin(FName("ID"))` returns the `id` pin (FName comparison ignores case), so `IdPin == UpperPin`. (Changing `CreateValuePin` alone would make `GetValuePin` miss every key and prove nothing about case.) Restore, run, expect green. Report both.

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestEditor
git commit -m "editor: v2 Make Json node -- case-sensitive keys, wildcard values, Remove key

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 15: Blueprint smoke tests — build, compile with zero errors, run (spec §2.5, M2 exit criterion)

A Blueprint is built in code, compiled, and its function executed through the Blueprint VM — the path the C++ tests of Tasks 12 and 14 cannot reach (the thunks' bytecode branch, the K2 node's expansion, `FCustomStructureParamHelper` typing pins on connection).

**Engine facts:** Blueprint creation and compile: `KismetEditorUtilities.h:124`, `:169`; member variables `BlueprintEditorUtils.h:858` (regenerates the skeleton, so a Set node finds the variable: `BlueprintEditorUtils.cpp:1901-1906`); function graphs `BlueprintEditorUtils.h:338`, `:399` (`AddFunctionGraph` marks the function `BlueprintCallable | BlueprintEvent | Public`, `:369-388`); node creation `EdGraph.h:274-326`; literal makers `KismetSystemLibrary.h:477`, `:533`; `MakeVector` `KismetMathLibrary.h:1280`. A script warning inside a Blueprint call logs two lines (`Script Msg:` and `Script Msg called by:`, `ScriptCore.cpp:724-747`, `CoreMisc.cpp:409-419`), so these graphs are built to raise none.

**Files:**
- Test: `Source/VaRestEditor/Private/Tests/VaRestJsonBlueprintTest.cpp`

**Interfaces:**
- Consumes: `VaRest::Json::BlueprintTest` helpers and `UK2Node_VaRestMakeJson` (Task 14); `UVaRestJsonLibrary::JsonToString`, `MakeJsonObject`, `SetField`, `GetField` (Tasks 11–12).
- Produces: `VaRest.Json.Blueprint.MakeJsonSmoke`, `VaRest.Json.Blueprint.WildcardSmoke`.

- [ ] **Step 1: Write the tests** — `Source/VaRestEditor/Private/Tests/VaRestJsonBlueprintTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "K2Node_VaRestMakeJson.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "Tests/VaRestJsonBlueprintTestUtils.h"
#include "VaRestJson.h"
#include "VaRestJsonLibrary.h"

namespace VaRestJsonBlueprintTest
{
	UK2Node_CallFunction* AddLiteralInt(UEdGraph* Graph, const TCHAR* Value)
	{
		UK2Node_CallFunction* Node = VaRest::Json::BlueprintTest::AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt));
		Node->FindPinChecked(FName(TEXT("Value")))->DefaultValue = Value;
		return Node;
	}

	UK2Node_CallFunction* AddLibraryCall(UEdGraph* Graph, FName FunctionName)
	{
		return VaRest::Json::BlueprintTest::AddCall(Graph, UVaRestJsonLibrary::StaticClass(), FunctionName);
	}

	FString ReadString(UObject* Instance, const TCHAR* Name)
	{
		const FStrProperty* Property = FindFProperty<FStrProperty>(Instance->GetClass(), FName(Name));
		return Property ? Property->GetPropertyValue_InContainer(Instance) : FString(TEXT("<no property>"));
	}

	int32 ReadInt(UObject* Instance, const TCHAR* Name)
	{
		const FIntProperty* Property = FindFProperty<FIntProperty>(Instance->GetClass(), FName(Name));
		return Property ? Property->GetPropertyValue_InContainer(Instance) : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMakeJsonSmokeTest, "VaRest.Json.Blueprint.MakeJsonSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMakeJsonSmokeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	using namespace VaRestJsonBlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestMakeJsonSmoke"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry) || !TestTrue(TEXT("Result variable"), AddVariable(Test.Blueprint, FName(TEXT("Result")), UEdGraphSchema_K2::PC_String)))
	{
		return false;
	}

	// Run: Result = To String(Make Json { id: 1, ID: 2, name: "Neo" })
	FGraphNodeCreator<UK2Node_VaRestMakeJson> Creator(*Test.Graph);
	UK2Node_VaRestMakeJson* Make = Creator.CreateNode(false);
	Creator.Finalize();
	Make->AddInputPin();
	Make->AddInputPin();
	Make->AddInputPin();
	Make->Entries[0].Key = TEXT("id");
	Make->Entries[1].Key = TEXT("ID"); // Review Focus: differs from "id" only by case
	Make->Entries[2].Key = TEXT("name");
	Make->ReconstructNode();

	UK2Node_CallFunction* Name = AddCall(Test.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralString));
	Name->FindPinChecked(FName(TEXT("Value")))->DefaultValue = TEXT("Neo");
	UK2Node_CallFunction* ToText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_VariableSet* SetResult = AddVariableSet(Test.Graph, FName(TEXT("Result")));

	TestTrue(TEXT("link id"), Link(AddLiteralInt(Test.Graph, TEXT("1"))->GetReturnValuePin(), Make->GetValuePin(0)));
	TestTrue(TEXT("link ID"), Link(AddLiteralInt(Test.Graph, TEXT("2"))->GetReturnValuePin(), Make->GetValuePin(1)));
	TestTrue(TEXT("link name"), Link(Name->GetReturnValuePin(), Make->GetValuePin(2)));
	TestTrue(TEXT("link Make Json -> To String"), Link(Make->GetJsonPin(), ToText->FindPinChecked(FName(TEXT("Json")))));
	TestTrue(TEXT("link To String -> Result"), Link(ToText->GetReturnValuePin(), SetResult->FindPinChecked(FName(TEXT("Result")))));
	TestTrue(TEXT("link exec"), Link(Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), SetResult->GetExecPin()));

	FCompilerResultsLog Results;
	if (!TestTrue(FString::Printf(TEXT("compiles with zero errors:\n%s"), *Messages(Results)), Compile(Test.Blueprint, Results)))
	{
		return false;
	}
	TestEqual(TEXT("... and zero warnings"), Results.NumWarnings, 0);
	UObject* Instance = Run(Test.Blueprint);
	if (!TestNotNull(TEXT("Run exists and ran"), Instance))
	{
		return false;
	}
	TestEqualSensitive(TEXT("both case variants reach the JSON, in order"), ReadString(Instance, TEXT("Result")), FString(TEXT("{\"id\":1,\"ID\":2,\"name\":\"Neo\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWildcardSmokeTest, "VaRest.Json.Blueprint.WildcardSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWildcardSmokeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::BlueprintTest;
	using namespace VaRestJsonBlueprintTest;
	const FTestBlueprint Test = MakeBlueprint(TEXT("VaRestWildcardSmoke"));
	if (!TestTrue(TEXT("test Blueprint"), Test.Blueprint && Test.Graph && Test.Entry)
		|| !TestTrue(TEXT("variables"), AddVariable(Test.Blueprint, FName(TEXT("Result")), UEdGraphSchema_K2::PC_String) && AddVariable(Test.Blueprint, FName(TEXT("Number")), UEdGraphSchema_K2::PC_Int)))
	{
		return false;
	}

	// Run: J = Make Json Object; Set Field(J, "n", 42); Set Field(J, "v", Make Vector(1, 2, 3)); Number = Get Field(J, "n"); Result = To String(J)
	UK2Node_CallFunction* MakeObject = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonObject));
	UK2Node_CallFunction* SetNumberField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField));
	UK2Node_CallFunction* SetVectorField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, SetField));
	UK2Node_CallFunction* GetNumberField = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, GetField));
	UK2Node_CallFunction* ToText = AddLibraryCall(Test.Graph, GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, JsonToString));
	UK2Node_CallFunction* Vector = AddCall(Test.Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, MakeVector));
	UK2Node_VariableSet* SetNumber = AddVariableSet(Test.Graph, FName(TEXT("Number")));
	UK2Node_VariableSet* SetResult = AddVariableSet(Test.Graph, FName(TEXT("Result")));
	Vector->FindPinChecked(FName(TEXT("X")))->DefaultValue = TEXT("1");
	Vector->FindPinChecked(FName(TEXT("Y")))->DefaultValue = TEXT("2");
	Vector->FindPinChecked(FName(TEXT("Z")))->DefaultValue = TEXT("3");
	SetNumberField->FindPinChecked(FName(TEXT("Key")))->DefaultValue = TEXT("n");
	SetVectorField->FindPinChecked(FName(TEXT("Key")))->DefaultValue = TEXT("v");
	GetNumberField->FindPinChecked(FName(TEXT("Key")))->DefaultValue = TEXT("n");

	UEdGraphPin* Json = MakeObject->GetReturnValuePin();
	TestTrue(TEXT("data links"),
		Link(Json, SetNumberField->FindPinChecked(FName(TEXT("Json"))))
		&& Link(AddLiteralInt(Test.Graph, TEXT("42"))->GetReturnValuePin(), SetNumberField->FindPinChecked(FName(TEXT("Value"))))
		&& Link(Json, SetVectorField->FindPinChecked(FName(TEXT("Json"))))
		&& Link(Vector->GetReturnValuePin(), SetVectorField->FindPinChecked(FName(TEXT("Value"))))
		&& Link(Json, GetNumberField->FindPinChecked(FName(TEXT("Json"))))
		&& Link(GetNumberField->FindPinChecked(FName(TEXT("Value"))), SetNumber->FindPinChecked(FName(TEXT("Number"))))
		&& Link(Json, ToText->FindPinChecked(FName(TEXT("Json"))))
		&& Link(ToText->GetReturnValuePin(), SetResult->FindPinChecked(FName(TEXT("Result")))));
	TestTrue(TEXT("wildcards took the connected types"),
		SetNumberField->FindPinChecked(FName(TEXT("Value")))->PinType.PinCategory == UEdGraphSchema_K2::PC_Int
		&& SetVectorField->FindPinChecked(FName(TEXT("Value")))->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
		&& GetNumberField->FindPinChecked(FName(TEXT("Value")))->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
	TestTrue(TEXT("exec chain"),
		Link(Test.Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then), MakeObject->GetExecPin())
		&& Link(MakeObject->GetThenPin(), SetNumberField->GetExecPin())
		&& Link(SetNumberField->GetThenPin(), SetVectorField->GetExecPin())
		&& Link(SetVectorField->GetThenPin(), SetNumber->GetExecPin())
		&& Link(SetNumber->GetThenPin(), SetResult->GetExecPin()));

	FCompilerResultsLog Results;
	if (!TestTrue(FString::Printf(TEXT("compiles with zero errors:\n%s"), *Messages(Results)), Compile(Test.Blueprint, Results)))
	{
		return false;
	}
	TestEqual(TEXT("... and zero warnings"), Results.NumWarnings, 0);
	UObject* Instance = Run(Test.Blueprint);
	if (!TestNotNull(TEXT("Run exists and ran"), Instance))
	{
		return false;
	}
	TestEqual(TEXT("Get Field read the int back through the VM"), ReadInt(Instance, TEXT("Number")), 42);
	const TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(ReadString(Instance, TEXT("Result"))));
	if (!TestTrue(TEXT("Result is JSON"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson& Out = Parsed.GetValue();
	TestTrue(TEXT("Set Field wrote an int and a struct (camelCase fields)"),
		Out[TEXT("n")].AsInt() == 42 && Out.Path(TEXT("v.x")).AsDouble() == 1.0 && Out.Path(TEXT("v.y")).AsDouble() == 2.0 && Out.Path(TEXT("v.z")).AsDouble() == 3.0);
	TestEqual(TEXT("nothing else"), Out.Num(), 2);
	return true;
}

#endif
```

- [ ] **Step 2: Run**

Run: `Tools/run_tests.sh VaRest.Json.Blueprint`
Expected: `0 failed` — `MakeJsonNode`, `MakeJsonSmoke`, `WildcardSmoke`. Tasks 12 and 14 already implement everything these tests exercise, so this task is the integration proof. If a smoke test fails, the compile log is in the failure message; fix the node or thunk (not the test) and re-run. Then prove the test can fail: in `ExpandNode` change `Entries[Index].Key` to `Entries[Index].Key.ToLower()`; expect `MakeJsonSmoke` to fail (the two keys become one `"id"`, which Set collapses); restore; expect green. Report both.

- [ ] **Step 3: Commit**

```bash
git add Source/VaRestEditor
git commit -m "editor: Blueprint smoke tests -- Make Json and the wildcard nodes built, compiled and run

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 16: Milestone close — monolithic build, full suite, MSVC review, docs, beads

**Files:**
- Modify: `CLAUDE.md` (module table rows for `VaRestJson` and `VaRestEditor`)
- No code changes unless a step below finds a defect.

- [ ] **Step 1: Monolithic game build**

No editor may be running (kill by PID only). Run:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
  VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject 2>&1 | tee /tmp/varest-m2-game.log | tail -5
grep -nE "(warning|error)" /tmp/varest-m2-game.log | grep -i "VaRest" || echo "no VaRest warnings or errors"
```

Expected: `Result: Succeeded` and `no VaRest warnings or errors`. The game target has no `VaRestEditor` and no `WITH_EDITOR`: this is where a missing include hidden by the editor's PCH, an `#if WITH_EDITOR` mismatch or a unity-build name clash shows up.

- [ ] **Step 2: Editor build and the full suite**

```bash
Tools/run_tests.sh VaRest
```

Expected: `0 failed`. Record the test count (it should be the M1 count plus every test this plan adds).

- [ ] **Step 3: MSVC review (this workstation builds with clang only)**

MSVC rejects what clang accepts in two ways this codebase has met: C4458 (a local or parameter hides a class member, member functions included) and C4456/C4457 (nested shadowing). Run:

```bash
# locals/parameters named like a member of the class whose member function declares them
grep -nE "\b(FVaRestJson|FString|int32|bool|const FVaRestJson&|FVaRestJsonError|TArray<uint8>)\s+(Path|Members|Keys|Num|Get|Set|Add|At|Has|Remove|Clone|Equals|Object|Array|Null|From|Parse|Node|Doc|As|TryGet|TryToString|TryToUtf8|ToUtf8|ToString)\b\s*[=;(,)]" \
  Source/VaRestJson/Private/VaRestJson.cpp Source/VaRestJson/Private/VaRestJsonText.cpp Source/VaRestJson/Private/VaRestJsonStructOps.cpp Source/VaRestJson/Private/VaRestJsonBridge.cpp
grep -nE "\b(FVaRestJson|FString|bool|TArray<uint8>|FVaRestJsonError)\s+(StructToUtf8|StructToString|StructToJson|PropertyToJson|JsonToStruct|Utf8ToStruct|JsonToProperty|FromJson|ToJson|ToUtf8|ResetCache)\b\s*[=;(,)]" \
  Source/VaRestJson/Private/VaRestJsonConvert*.cpp
grep -nE "\b(Entries|NextId|JsonPinName|Pins)\b\s*[=;]" Source/VaRestEditor/Private/K2Node_VaRestMakeJson.cpp
grep -nE "\b(FVaRestJson|bool|FString)\s+(GetField|SetField|GetPath|ArrayGet|ArrayAdd|StructToJson|JsonToStruct|MergeJson|CloneJson)\b\s*[=;]" Source/VaRestJson/Private/VaRestJsonLibrary.cpp
```

Every hit inside a member function of the class that owns the name must be renamed (hits in free functions and in the `Entries`-assigning lines of the node's own members are fine — inspect each). Rebuild and re-run Step 2 if anything changed.

- [ ] **Step 4: Docs** — in `CLAUDE.md`, replace the two module-table rows:

```markdown
| `VaRestJson` | Runtime | `FVaRestJson`: own arena DOM, RapidJSON (engine copy) as SAX reader + writer; `FVaRestJsonConvert` struct <-> JSON; `UVaRestJsonLibrary` BP JSON library (CustomThunk wildcards). Depends on Engine (public) |
| `VaRestEditor` | UncookedOnly | K2 nodes, v1 and v2 (`K2Node_VaRestMakeJson`); drops the convert cache on Blueprint struct changes |
```

- [ ] **Step 5: Beads**

```bash
bd close VaRest-2o9 VaRest-awm VaRest-6ky --reason="M2 plan Tasks 1-2"
bd update VaRest-rfd --notes="by-M2 items done in M2 (Tasks 1, 3, 4, 5); the M3 item (test types out of the runtime module) stays open"
bd list --status=open | grep -i "M2"
```

Close each listed M2 bead whose deliverable a task above completed (`bd close <id> --reason="..."`); file a bead for anything found and not fixed (`bd create --title=... --description=... --type=bug --priority=2`).

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md
git status --short
git commit -m "docs: M2 closes -- module table, monolithic build and full suite green

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

## After M2

Not in this plan, recorded so they are not lost:

- **SAX → struct decoder (spec §2.4, M7):** becomes the default for typed responses only if it beats Task 10's DOM + walk decode row by ≥1.5x on the 10 MiB fixture.
- **Test types out of the runtime module** (VaRest-rfd, M3): `VaRestJsonConvertTestTypes.h` ships in `VaRestJson`'s reflection data like M1's test types.
- **v1 `Make Json` node:** it stays titled "Make Json" under "Utilities|VaRest" next to v2's under "VaRest|Json" until the v1 shims are deprecated (M3).
- **Field-map cache growth:** entries for structs that are destroyed (property bags rebuilt at runtime, transient Blueprint structs) stay until the next `ResetCache`; harmless (keys are `FObjectKey`, never reused) but unbounded in a long editor session.

## Deferred review minors

Every Minor finding of `.superpowers/sdd/m2-plan-review.md` (7-27) is applied in its task; none is declined. Two are applied in part, on purpose:

- 17 (FText) → encode writes `BuildSourceString()` (culture-invariant, as spec §2.4 says); decode already builds `FText::AsCultureInvariant` (Task 8), so the spec needs no amendment.
- 27 (`JsonUtilities` in shipping) → kept as a private dependency with the reason in the comment (`Engine.Build.cs:88` links it publicly anyway); no `Shipping` guard, which would only add a configuration-dependent dependency list.
