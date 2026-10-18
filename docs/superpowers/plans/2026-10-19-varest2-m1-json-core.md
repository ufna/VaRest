# VaRest 2 — M1: JSON core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the `VaRestJson` module: the `FVaRestJson` handle over a stable arena DOM, RapidJSON-based parsing and writing, path access, `FJsonValue` bridges, struct ops, and the benchmark that decides the spec §7.3 gate.

**Architecture:** Our own DOM (nodes in a chunked bump arena that never move, so handles never dangle) built by RapidJSON's SAX reader running in situ over a document-owned UTF-8 buffer, and serialised by RapidJSON's writer. `FVaRestJson` is a 24-byte `USTRUCT` handle (`TSharedPtr<FDocument>` + `FNode*`) with reference semantics.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), C++20, engine RapidJSON 1.1.0 through `Runtime/Json/Public/RapidJsonIncludes.h`, UE Automation tests.

**Spec:** `docs/superpowers/specs/2026-10-18-varest2-core-design.md` — §1 (modules, threading rule), §2.1–§2.3 (storage, semantics, C++ API), §2.5 (struct ops only), §7.1, §7.3. Struct ↔ JSON (§2.4) and the BP library (§2.5 nodes) are **M2**, not this plan.

## Global Constraints

- Engine: UE 5.8 only. Build host: `/w/Unreal/VaRestHost` (plugin symlinked). Build + test: `Tools/run_tests.sh [Filter]`.
- Module `VaRestJson`: `"Type": "Runtime"`, `"LoadingPhase": "PreDefault"`, `"PlatformAllowList": ["Win64","Mac","Linux","Android","IOS"]`.
- RapidJSON only through `#include "RapidJsonIncludes.h"` (the engine's macro configuration: `RAPIDJSON_ASSERT`→`check`, SSE4.2/NEON). No new third-party code.
- **v1 paths are frozen**: do not touch anything under `Source/VaRest/` or `Source/VaRestEditor/`. The only allowed v1-adjacent edit is the module list in `VaRest.uplugin`.
- Public types: `FVaRestJson*`, `EVaRestJson*`. Internals: namespace `VaRest::Json`. Private headers are named `VaRestJson*.h`; **never `VaRestJsonParser.h`** (v1 has a private header with that name).
- **JSON keys are case-sensitive.** Never compare keys with `FString`/`TStringView` `operator==` (case-insensitive in UE); use `VaRest::Json::KeyEquals`.
- Parsed documents may be read from many threads at once; nothing on a read path may write (no lazy caches).
- Tests: `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, names `VaRest.Json.*`, flags `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`, inside `#if WITH_DEV_AUTOMATION_TESTS`, files in `Source/VaRestJson/Private/Tests/`. Benchmarks are named `Bench.VaRest.Json.*` with `EAutomationTestFlags::PerfFilter` (the runner's prefix filter keeps them out of the default suite).
- Comments explain *why* and cite engine source as `file:line`; open every line you cite.
- Restore-the-bug: tasks that name a restore-the-bug step must show the test failing with the behaviour broken, then passing restored; report both outcomes in the task report.
- No editor may run during a build; kill only by PID.
- New file header: `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.`
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- **UTF-8 BOM at the start of input** (files saved by Windows editors, some servers): must parse as if absent. Test owner: Task 6.
- **Keys and strings containing `\u0000`**: lengths, lookups and round trips must survive the embedded NUL. Test owner: Tasks 5 and 6.
- **Removing members while iterating `Members()`**: no crash, no out-of-range read. Test owner: Task 4.
- **Concurrent readers of one indexed document** (HTTP parses on a worker, several systems read): correct results, no writes on read paths. Test owner: Task 4.
- **`ImportTextItem` with `,` `)` `"` inside JSON strings** when the handle is a field of another struct's exported text: must consume exactly the JSON value. Test owner: Task 9.

---

## File structure

```
Source/VaRestJson/
  VaRestJson.Build.cs
  Public/VaRestJson.h                    FVaRestJson, EVaRestJsonType, EVaRestJsonFormat, FVaRestJsonError,
                                         FVaRestJsonParseOptions, member/element ranges
  Private/VaRestJsonModule.cpp           module + LogVaRestJson definition
  Private/VaRestJsonLog.h                LogVaRestJson declaration
  Private/VaRestJsonArena.h/.cpp         VaRest::Json::FArena (chunked bump allocator)
  Private/VaRestJsonNode.h               FStr, FMember, FIndex, FNode, KeyEquals, HashKey
  Private/VaRestJsonDocument.h/.cpp      VaRest::Json::FDocument (factories, object/array ops, copy, equality)
  Private/VaRestJson.cpp                 FVaRestJson handle API
  Private/VaRestJsonWriter.h/.cpp        VaRest::Json::Write (RapidJSON writer over a TArray sink)
  Private/VaRestJsonReader.h/.cpp        VaRest::Json::ParseInSitu (RapidJSON SAX -> arena DOM)
  Private/VaRestJsonText.cpp             FVaRestJson::Parse*/ToString/ToUtf8
  Private/VaRestJsonPath.cpp             FVaRestJson::Path
  Private/VaRestJsonBridge.cpp           FJsonValue/FJsonObject bridges
  Private/VaRestJsonStructOps.cpp        ExportTextItem/ImportTextItem/Identical/Serialize
  Private/Tests/VaRestJson*Test.cpp      one file per task
  Private/Tests/VaRestJsonTestTypes.h    test-only USTRUCT (Task 9)
Tools/gen_bench_json.py                  deterministic benchmark fixtures (Task 10)
docs/passport/2026-10-19-json-bench.md   benchmark results + gate decision (Task 10)
```

---

### Task 1: `VaRestJson` module skeleton

**Files:**
- Modify: `VaRest.uplugin`
- Create: `Source/VaRestJson/VaRestJson.Build.cs`, `Source/VaRestJson/Public/VaRestJson.h`, `Source/VaRestJson/Private/VaRestJsonModule.cpp`, `Source/VaRestJson/Private/VaRestJsonLog.h`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonModuleTest.cpp`

**Interfaces:**
- Produces: module `VaRestJson`; log category `LogVaRestJson` (`VaRestJsonLog.h`); `FVaRestJson` with `bool IsValid() const` (expanded in Task 4); `EVaRestJsonType`.

- [ ] **Step 1: Register the module in `VaRest.uplugin`**

Replace the descriptor's top part and module list (keep `DocsURL`/`MarketplaceURL` as they are):

```json
{
	"FileVersion" : 3,
	"FriendlyName" : "VaRest",
	"Version" : 200,
	"VersionName" : "2.0.0-dev",
	"CreatedBy" : "Vladimir Alyamkin",
	"CreatedByURL" : "https://ufna.dev",
	"EngineVersion" : "5.8.0",
	"Description" : "Plugin that makes REST (JSON) server communication easy to use",
	"Category" : "Network",
	"DocsURL": "https://bit.ly/VaRest-Docs",
	"MarketplaceURL" : "com.epicgames.launcher://ue/marketplace/content/e47be161e7a24e928560290abd5dcc4f",
	"Modules" :
	[
		{
			"Name" : "VaRestJson",
			"Type" : "Runtime",
			"LoadingPhase" : "PreDefault",
			"PlatformAllowList" : [ "Win64", "Mac", "Linux", "Android", "IOS" ]
		},
		{
			"Name" : "VaRest",
			"Type" : "Runtime",
			"LoadingPhase": "PreDefault"
		},
		{
			"Name": "VaRestEditor",
			"Type": "UncookedOnly"
		}
	]
}
```

- [ ] **Step 2: Build rules**

`Source/VaRestJson/VaRestJson.Build.cs`:

```csharp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestJson : ModuleRules
{
	public VaRestJson(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",

			// PUBLIC: VaRestJson.h exposes TSharedPtr<FJsonValue>/<FJsonObject> bridges, so a
			// consumer calling them needs Json's include path.
			"Json",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// RapidJSON headers. Json re-exports them publicly (Json.Build.cs), but the
			// dependency is ours, so it is declared rather than inherited.
			"RapidJSON",
		});
	}
}
```

- [ ] **Step 3: Module, log category, minimal public header**

`Source/VaRestJson/Private/VaRestJsonLog.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVaRestJson, Log, All);
```

`Source/VaRestJson/Private/VaRestJsonModule.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VaRestJsonLog.h"

DEFINE_LOG_CATEGORY(LogVaRestJson);

IMPLEMENT_MODULE(FDefaultModuleImpl, VaRestJson);
```

`Source/VaRestJson/Public/VaRestJson.h` (Task 4 replaces it with the full API):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestJson.generated.h"

/** Type of the value a FVaRestJson handle points at. None = invalid handle (no value). */
UENUM(BlueprintType)
enum class EVaRestJsonType : uint8
{
	None,
	Null,
	Bool,
	Number,
	String,
	Array,
	Object
};

USTRUCT(BlueprintType, meta = (DisplayName = "VaRest Json"))
struct VARESTJSON_API FVaRestJson
{
	GENERATED_BODY()

	bool IsValid() const { return false; }
};
```

- [ ] **Step 4: Write the smoke test**

`Source/VaRestJson/Private/Tests/VaRestJsonModuleTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonModuleLoadsTest, "VaRest.Json.Module.Loads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonModuleLoadsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("VaRestJson module is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestJson")));
	TestFalse(TEXT("A default handle is invalid"), FVaRestJson().IsValid());
	return true;
}

#endif
```

- [ ] **Step 5: Restore-the-bug on the runner itself**

Temporarily change `TestFalse(TEXT("A default handle is invalid")...` to `TestTrue(...)`.
Run: `Tools/run_tests.sh VaRest.Json`
Expected: exit code 1, output contains `1 failed` and the `Result={Fail}` line naming `VaRest.Json.Module.Loads`.
Restore `TestFalse`.

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `run_tests: 1 passed, 0 failed`.

- [ ] **Step 7: Commit**

```bash
git add VaRest.uplugin Source/VaRestJson
git commit -m "json: VaRestJson module skeleton

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Arena allocator

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonArena.h`, `Source/VaRestJson/Private/VaRestJsonArena.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonArenaTest.cpp`

**Interfaces:**
- Produces: `VaRest::Json::FArena` — `explicit FArena(SIZE_T FirstChunkSize = 4096)`, `void* Allocate(SIZE_T Size, SIZE_T Alignment)`, `template<class T> T* NewArray(uint32 Count)` (uninitialised, trivially destructible T, returns nullptr for 0), `template<class T, class... A> T* New(A&&...)`, `SIZE_T GetReservedBytes() const`, `SIZE_T GetUsedBytes() const`, `static constexpr SIZE_T MaxChunkSize = 64 * 1024`.

- [ ] **Step 1: Write the failing test**

`Source/VaRestJson/Private/Tests/VaRestJsonArenaTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Templates/AlignmentTemplates.h"
#include "VaRestJsonArena.h"

using VaRest::Json::FArena;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaAlignmentTest, "VaRest.Json.Arena.Alignment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaAlignmentTest::RunTest(const FString& Parameters)
{
	FArena Arena(256);
	for (SIZE_T Alignment : { 1, 2, 4, 8, 16, 64 })
	{
		Arena.Allocate(3, 1); // knock the cursor off alignment first
		void* P = Arena.Allocate(24, Alignment);
		TestTrue(*FString::Printf(TEXT("aligned to %llu"), (uint64)Alignment), IsAligned(P, Alignment));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaStabilityTest, "VaRest.Json.Arena.NothingMoves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaStabilityTest::RunTest(const FString& Parameters)
{
	FArena Arena;
	TArray<uint32*> Blocks;
	for (uint32 i = 0; i < 100000; ++i)
	{
		uint32* P = static_cast<uint32*>(Arena.Allocate(sizeof(uint32) * 6, alignof(uint32)));
		for (uint32 k = 0; k < 6; ++k)
		{
			P[k] = i * 7 + k;
		}
		Blocks.Add(P);
	}
	// Every block must still hold what was written: no chunk was moved or reused.
	int32 Bad = 0;
	for (uint32 i = 0; i < 100000; ++i)
	{
		for (uint32 k = 0; k < 6; ++k)
		{
			Bad += Blocks[i][k] != i * 7 + k;
		}
	}
	TestEqual(TEXT("corrupted words"), Bad, 0);
	TestTrue(TEXT("used <= reserved"), Arena.GetUsedBytes() <= Arena.GetReservedBytes());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonArenaBigBlockTest, "VaRest.Json.Arena.BigBlockOwnChunk",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonArenaBigBlockTest::RunTest(const FString& Parameters)
{
	FArena Arena(4096);
	uint8* Small1 = static_cast<uint8*>(Arena.Allocate(16, 8));
	const SIZE_T Big = 1024 * 1024;
	uint8* BigBlock = static_cast<uint8*>(Arena.Allocate(Big, 16));
	uint8* Small2 = static_cast<uint8*>(Arena.Allocate(16, 8));

	TestTrue(TEXT("big block reserved"), Arena.GetReservedBytes() >= Big + 4096);
	// The big block got a chunk of its own; the current chunk keeps serving small blocks.
	TestTrue(TEXT("small block after the big one is outside it"), Small2 < BigBlock || Small2 >= BigBlock + Big);
	TestTrue(TEXT("small blocks share the first chunk"), Small2 > Small1 && Small2 < Small1 + 4096);
	FMemory::Memset(BigBlock, 0xAB, Big); // writable end to end
	TestTrue(TEXT("zero-count array is null"), Arena.NewArray<uint32>(0) == nullptr);
	return true;
}

#endif
```

- [ ] **Step 2: Run test to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Arena`
Expected: build fails with `'VaRestJsonArena.h' file not found` (the runner exits 2 with `build failed`).

- [ ] **Step 3: Implement**

`Source/VaRestJson/Private/VaRestJsonArena.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <type_traits>

namespace VaRest::Json
{
	/**
	 * Chunked bump allocator. Memory is released only when the arena dies, and nothing it hands
	 * out ever moves -- that is what lets a FVaRestJson handle hold a raw node pointer while the
	 * node's parent keeps growing.
	 */
	class FArena
	{
	public:
		static constexpr SIZE_T MaxChunkSize = 64 * 1024;

		explicit FArena(SIZE_T InFirstChunkSize = 4096);
		~FArena();

		FArena(const FArena&) = delete;
		FArena& operator=(const FArena&) = delete;

		void* Allocate(SIZE_T Size, SIZE_T Alignment);

		/** Uninitialised storage for Count trivially destructible T; nullptr when Count is 0 */
		template <typename T>
		T* NewArray(uint32 Count)
		{
			static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destructed");
			return Count ? static_cast<T*>(Allocate(sizeof(T) * Count, alignof(T))) : nullptr;
		}

		template <typename T, typename... ArgTypes>
		T* New(ArgTypes&&... Args)
		{
			static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destructed");
			return new (Allocate(sizeof(T), alignof(T))) T(Forward<ArgTypes>(Args)...);
		}

		/** Bytes requested from FMemory, chunk headers included */
		SIZE_T GetReservedBytes() const { return ReservedBytes; }

		/** Bytes handed out, alignment padding included */
		SIZE_T GetUsedBytes() const { return UsedBytes; }

	private:
		struct FChunk
		{
			FChunk* Next;
		};

		FChunk* NewChunk(SIZE_T Payload);

		FChunk* Chunks = nullptr;
		uint8* Cursor = nullptr;
		uint8* End = nullptr;
		SIZE_T NextChunkSize;
		SIZE_T ReservedBytes = 0;
		SIZE_T UsedBytes = 0;
	};
}
```

`Source/VaRestJson/Private/VaRestJsonArena.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonArena.h"

#include "HAL/UnrealMemory.h"
#include "Templates/AlignmentTemplates.h"

namespace VaRest::Json
{
	FArena::FArena(SIZE_T InFirstChunkSize)
		: NextChunkSize(FMath::Max<SIZE_T>(InFirstChunkSize, 256))
	{
	}

	FArena::~FArena()
	{
		for (FChunk* Chunk = Chunks; Chunk;)
		{
			FChunk* Next = Chunk->Next;
			FMemory::Free(Chunk);
			Chunk = Next;
		}
	}

	FArena::FChunk* FArena::NewChunk(SIZE_T Payload)
	{
		const SIZE_T Total = sizeof(FChunk) + Payload;
		FChunk* Chunk = static_cast<FChunk*>(FMemory::Malloc(Total, alignof(FChunk)));
		Chunk->Next = Chunks;
		Chunks = Chunk;
		ReservedBytes += Total;
		return Chunk;
	}

	void* FArena::Allocate(SIZE_T Size, SIZE_T Alignment)
	{
		check(FMath::IsPowerOfTwo(Alignment));

		if (Cursor)
		{
			uint8* Aligned = Align(Cursor, Alignment);
			if (Aligned + Size <= End)
			{
				UsedBytes += (Aligned - Cursor) + Size;
				Cursor = Aligned + Size;
				return Aligned;
			}
		}

		const SIZE_T Needed = Size + Alignment;
		if (Cursor && Needed > MaxChunkSize / 2)
		{
			// A big block gets a chunk of its own and the current chunk keeps serving small
			// ones, so one long string does not strand the rest of a nearly empty chunk.
			FChunk* Big = NewChunk(Needed);
			uint8* Payload = Align(reinterpret_cast<uint8*>(Big + 1), Alignment);
			UsedBytes += Size;
			return Payload;
		}

		const SIZE_T Payload = FMath::Max(Needed, NextChunkSize);
		FChunk* Chunk = NewChunk(Payload);
		Cursor = reinterpret_cast<uint8*>(Chunk + 1);
		End = Cursor + Payload;
		// The first chunk may be sized from a hint (a parse sizes it from the input);
		// growth chunks double from 4 KiB up to MaxChunkSize.
		NextChunkSize = FMath::Min<SIZE_T>(FMath::Max<SIZE_T>(NextChunkSize, 4096) * 2, MaxChunkSize);

		uint8* Aligned = Align(Cursor, Alignment);
		UsedBytes += (Aligned - Cursor) + Size;
		Cursor = Aligned + Size;
		return Aligned;
	}
}
```

- [ ] **Step 4: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `4 passed, 0 failed` (3 arena + 1 module).

- [ ] **Step 5: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: chunked bump arena

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Nodes and the document

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonNode.h`, `Source/VaRestJson/Private/VaRestJsonDocument.h`, `Source/VaRestJson/Private/VaRestJsonDocument.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonDocumentTest.cpp`

**Interfaces:**
- Consumes: `FArena` (Task 2), `EVaRestJsonType` (Task 1).
- Produces (all in `namespace VaRest::Json`):
  - `struct FStr { const UTF8CHAR* Data; uint32 Len; uint32 Hash; FUtf8StringView View() const; }`
  - `struct FMember { FStr Key; FNode* Value; }`, `struct FIndex { uint32 Capacity; uint32* Slots; }`
  - `enum class ENumKind : uint8 { Int, UInt, Double }`
  - `struct FNode { EVaRestJsonType Type; ENumKind NumKind; union { bool Bool; int64 Int; uint64 UInt; double Double; FStr Str; FArr Arr; FObj Obj; }; }` with `FArr { FNode** Items; uint32 Num, Max; }`, `FObj { FMember* Members; uint32 Num, Max; FIndex* Index; }`
  - `constexpr uint32 IndexThreshold = 8`; `uint32 HashKey(FUtf8StringView)`; `bool KeyEquals(const FStr&, FUtf8StringView)`
  - `class FDocument` with: `FArena Arena; TArray<uint8> Input; FNode* Root;` `explicit FDocument(SIZE_T ArenaFirstChunk = 4096)`; `NewNull/NewBool/NewInt/NewUInt/NewDouble/NewString(FUtf8StringView)/NewStringRef(const UTF8CHAR*, uint32)/NewArray(uint32 Reserve = 0)/NewObject(uint32 Reserve = 0)`; `FStr CopyStr(FUtf8StringView, bool bHash)`; `static FStr RefStr(const UTF8CHAR*, uint32, bool bHash)`; `static int32 FindMember(const FNode& Object, FUtf8StringView Key, uint32 Hash)`; `static FNode* Find(const FNode& Object, FUtf8StringView Key)`; `void Set(FNode& Object, FUtf8StringView Key, FNode* Value)`; `void Append(FNode& Object, const FStr& Key, FNode* Value)`; `int32 Remove(FNode& Object, FUtf8StringView Key)`; `void BuildIndex(FNode& Object)`; `void Append(FNode& Array, FNode* Value)`; `bool SetAt(FNode& Array, uint32 Index, FNode* Value)`; `bool RemoveAt(FNode& Array, uint32 Index)`; `void AssignArray(FNode& Array, TConstArrayView<FNode*> Items)`; `void AssignObject(FNode& Object, TConstArrayView<FMember> Members)`; `FNode* DeepCopy(const FNode& Src)`; `static bool Equals(const FNode& A, const FNode& B)`.

- [ ] **Step 1: Write the failing test**

`Source/VaRestJson/Private/Tests/VaRestJsonDocumentTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJsonDocument.h"

using namespace VaRest::Json;

namespace VaRestJsonDocumentTest
{
	FUtf8StringView K(const char* S) { return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(S)); }
}
using VaRestJsonDocumentTest::K;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocNumbersTest, "VaRest.Json.Document.Numbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocNumbersTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Small = Doc.NewUInt(42);
	TestTrue(TEXT("uint that fits int64 is stored as Int"), Small->NumKind == ENumKind::Int && Small->Int == 42);
	FNode* Big = Doc.NewUInt(MAX_uint64);
	TestTrue(TEXT("uint above INT64_MAX is stored as UInt"), Big->NumKind == ENumKind::UInt && Big->UInt == MAX_uint64);
	TestTrue(TEXT("1 == 1.0"), FDocument::Equals(*Doc.NewInt(1), *Doc.NewDouble(1.0)));
	TestFalse(TEXT("1 != 1.5"), FDocument::Equals(*Doc.NewInt(1), *Doc.NewDouble(1.5)));
	TestTrue(TEXT("2^63 as UInt == 2^63 as double"), FDocument::Equals(*Doc.NewUInt(9223372036854775808ull), *Doc.NewDouble(9223372036854775808.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocObjectTest, "VaRest.Json.Document.ObjectMembers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocObjectTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Obj = Doc.NewObject();
	Doc.Set(*Obj, K("a"), Doc.NewInt(1));
	Doc.Set(*Obj, K("b"), Doc.NewInt(2));
	Doc.Set(*Obj, K("a"), Doc.NewInt(3)); // replaces in place
	TestEqual(TEXT("replace keeps member count"), (int32)Obj->Obj.Num, 2);
	TestTrue(TEXT("replace keeps order"), KeyEquals(Obj->Obj.Members[0].Key, K("a")));
	TestEqual(TEXT("a replaced"), FDocument::Find(*Obj, K("a"))->Int, (int64)3);

	// Keys are case-sensitive
	TestNull(TEXT("'A' is not 'a'"), FDocument::Find(*Obj, K("A")));

	// Duplicates (the parser keeps them): lookup returns the LAST occurrence
	Doc.Append(*Obj, Doc.CopyStr(K("b"), true), Doc.NewInt(20));
	TestEqual(TEXT("duplicate kept"), (int32)Obj->Obj.Num, 3);
	TestEqual(TEXT("last occurrence wins"), FDocument::Find(*Obj, K("b"))->Int, (int64)20);

	// Remove drops every occurrence and keeps the order of the rest
	Doc.Set(*Obj, K("c"), Doc.NewInt(4));
	TestEqual(TEXT("removed both b"), Doc.Remove(*Obj, K("b")), 2);
	TestEqual(TEXT("two left"), (int32)Obj->Obj.Num, 2);
	TestTrue(TEXT("order a, c"), KeyEquals(Obj->Obj.Members[0].Key, K("a")) && KeyEquals(Obj->Obj.Members[1].Key, K("c")));
	TestEqual(TEXT("removing a missing key"), Doc.Remove(*Obj, K("zzz")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocIndexTest, "VaRest.Json.Document.Index",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocIndexTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Obj = Doc.NewObject();
	for (int32 i = 0; i < (int32)IndexThreshold; ++i)
	{
		Doc.Set(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("k%d"), i)).Get(), Doc.NewInt(i));
	}
	TestNull(TEXT("no index at the threshold"), Obj->Obj.Index);
	for (int32 i = (int32)IndexThreshold; i < 1000; ++i)
	{
		Doc.Set(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("k%d"), i)).Get(), Doc.NewInt(i));
	}
	TestNotNull(TEXT("index above the threshold"), Obj->Obj.Index);

	int32 Wrong = 0;
	for (int32 i = 0; i < 1000; ++i)
	{
		const FNode* N = FDocument::Find(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("k%d"), i)).Get());
		Wrong += !N || N->Int != i;
	}
	TestEqual(TEXT("indexed lookups"), Wrong, 0);

	// Duplicate through the index: last wins
	Doc.Append(*Obj, Doc.CopyStr(K("k5"), true), Doc.NewInt(5000));
	TestEqual(TEXT("indexed last occurrence wins"), FDocument::Find(*Obj, K("k5"))->Int, (int64)5000);

	// Remove shifts members; the index must be rebuilt, not left pointing at old slots
	TestEqual(TEXT("remove k0"), Doc.Remove(*Obj, K("k0")), 1);
	TestEqual(TEXT("k999 after shift"), FDocument::Find(*Obj, K("k999"))->Int, (int64)999);
	TestNull(TEXT("k0 gone"), FDocument::Find(*Obj, K("k0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocStabilityTest, "VaRest.Json.Document.ChildSurvivesParentGrowth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocStabilityTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Root = Doc.NewObject();
	FNode* Child = Doc.NewObject();
	Doc.Set(*Root, K("child"), Child);
	Doc.Set(*Child, K("x"), Doc.NewString(K("kept")));
	FNode* Arr = Doc.NewArray();
	Doc.Set(*Root, K("arr"), Arr);
	FNode* First = Doc.NewInt(7);
	Doc.Append(*Arr, First);

	for (int32 i = 0; i < 1000; ++i)
	{
		Doc.Set(*Root, StringCast<UTF8CHAR>(*FString::Printf(TEXT("grow%d"), i)).Get(), Doc.NewInt(i));
		Doc.Append(*Arr, Doc.NewInt(i));
	}

	TestTrue(TEXT("child pointer still resolves"), FDocument::Find(*Root, K("child")) == Child);
	TestTrue(TEXT("child content intact"), KeyEquals(FDocument::Find(*Child, K("x"))->Str, K("kept")));
	TestTrue(TEXT("first element pointer intact"), Arr->Arr.Items[0] == First && First->Int == 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocCopyTest, "VaRest.Json.Document.DeepCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocCopyTest::RunTest(const FString& Parameters)
{
	FDocument Target;
	FNode* Copy = nullptr;
	{
		FDocument Source;
		FNode* Obj = Source.NewObject();
		Source.Set(*Obj, K("s"), Source.NewString(K("text")));
		FNode* Arr = Source.NewArray();
		Source.Append(*Arr, Source.NewDouble(2.5));
		Source.Set(*Obj, K("a"), Arr);
		for (int32 i = 0; i < 20; ++i)
		{
			Source.Set(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("n%d"), i)).Get(), Source.NewInt(i));
		}
		Copy = Target.DeepCopy(*Obj);
	} // Source and its arena are gone

	TestTrue(TEXT("string copied"), KeyEquals(FDocument::Find(*Copy, K("s"))->Str, K("text")));
	TestEqual(TEXT("array copied"), FDocument::Find(*Copy, K("a"))->Arr.Items[0]->Double, 2.5);
	TestNotNull(TEXT("copied big object is indexed"), Copy->Obj.Index);
	TestEqual(TEXT("indexed lookup in copy"), FDocument::Find(*Copy, K("n19"))->Int, (int64)19);

	// Copying an ancestor under one of its own descendants terminates and does not alias
	FNode* Root = Target.NewObject();
	FNode* Sub = Target.NewObject();
	Target.Set(*Root, K("sub"), Sub);
	Target.Set(*Sub, K("loop"), Target.DeepCopy(*Root));
	const FNode* Loop = FDocument::Find(*Sub, K("loop"));
	TestTrue(TEXT("loop is a copy, not the root"), Loop != Root);
	TestNull(TEXT("copy was taken before the attach"), FDocument::Find(*FDocument::Find(*Loop, K("sub")), K("loop")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocEqualsTest, "VaRest.Json.Document.Equals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocEqualsTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* A = Doc.NewObject();
	Doc.Set(*A, K("x"), Doc.NewInt(1));
	Doc.Set(*A, K("y"), Doc.NewString(K("s")));
	FNode* B = Doc.NewObject();
	Doc.Set(*B, K("y"), Doc.NewString(K("s")));
	Doc.Set(*B, K("x"), Doc.NewDouble(1.0));
	TestTrue(TEXT("objects compare without regard to order"), FDocument::Equals(*A, *B));

	Doc.Append(*B, Doc.CopyStr(K("x"), true), Doc.NewInt(2));
	TestFalse(TEXT("duplicates compare by their last occurrence"), FDocument::Equals(*A, *B));

	FNode* L1 = Doc.NewArray();
	Doc.Append(*L1, Doc.NewInt(1));
	Doc.Append(*L1, Doc.NewInt(2));
	FNode* L2 = Doc.NewArray();
	Doc.Append(*L2, Doc.NewInt(2));
	Doc.Append(*L2, Doc.NewInt(1));
	TestFalse(TEXT("arrays are ordered"), FDocument::Equals(*L1, *L2));
	TestFalse(TEXT("strings are case-sensitive"), FDocument::Equals(*Doc.NewString(K("a")), *Doc.NewString(K("A"))));
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Document`
Expected: build fails, `'VaRestJsonDocument.h' file not found`.

- [ ] **Step 3: Implement `VaRestJsonNode.h`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Hash/CityHash.h"
#include "VaRestJson.h"

namespace VaRest::Json
{
	struct FNode;

	/** UTF-8 bytes inside a document (the in-situ input buffer or the arena), NUL-terminated. */
	struct FStr
	{
		const UTF8CHAR* Data;
		uint32 Len;
		/** CityHash32 of the bytes; set for object keys, 0 otherwise */
		uint32 Hash;

		FUtf8StringView View() const { return FUtf8StringView(Data, Len); }
	};

	struct FMember
	{
		FStr Key;
		FNode* Value;
	};

	/** Open-addressing index over an object's members: slot = member index + 1, 0 = empty. */
	struct FIndex
	{
		uint32 Capacity; // power of two, at least twice the member count
		uint32* Slots;
	};

	enum class ENumKind : uint8
	{
		Int,    // any value representable as int64
		UInt,   // only values above INT64_MAX
		Double
	};

	struct FNode
	{
		struct FArr
		{
			FNode** Items;
			uint32 Num;
			uint32 Max;
		};

		struct FObj
		{
			FMember* Members; // insertion order; duplicates allowed (parser)
			uint32 Num;
			uint32 Max;
			FIndex* Index;    // non-null above IndexThreshold members
		};

		EVaRestJsonType Type = EVaRestJsonType::Null;
		ENumKind NumKind = ENumKind::Int;
		union
		{
			bool Bool;
			int64 Int;
			uint64 UInt;
			double Double;
			FStr Str;
			FArr Arr;
			FObj Obj;
		};

		FNode()
			: Obj{ nullptr, 0, 0, nullptr }
		{
		}
	};

	/** Objects with more members than this carry an FIndex, built at parse time or on mutation -- never on read */
	inline constexpr uint32 IndexThreshold = 8;

	inline uint32 HashKey(FUtf8StringView Key)
	{
		return CityHash32(reinterpret_cast<const char*>(Key.GetData()), static_cast<uint32>(Key.Len()));
	}

	/** Byte-exact comparison. TStringView::operator== is case-insensitive in UE, so it must not be used for keys. */
	inline bool KeyEquals(const FStr& A, FUtf8StringView B)
	{
		return A.Len == static_cast<uint32>(B.Len()) && (A.Len == 0 || FMemory::Memcmp(A.Data, B.GetData(), A.Len) == 0);
	}
}
```

- [ ] **Step 4: Implement `VaRestJsonDocument.h`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonArena.h"
#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/**
	 * Owns every node of one JSON tree. FVaRestJson handles share it through a thread-safe
	 * TSharedPtr; nodes live in the arena and never move, so a handle's raw FNode* stays valid
	 * for as long as any handle keeps the document alive.
	 */
	class FDocument
	{
	public:
		explicit FDocument(SIZE_T ArenaFirstChunk = 4096)
			: Arena(ArenaFirstChunk)
		{
		}

		FArena Arena;

		/** In-situ parse buffer, NUL-terminated; parsed strings and keys point into it */
		TArray<uint8> Input;

		FNode* Root = nullptr;

		FNode* NewNull();
		FNode* NewBool(bool bValue);
		FNode* NewInt(int64 Value);
		/** Stored as Int when it fits int64 */
		FNode* NewUInt(uint64 Value);
		FNode* NewDouble(double Value);
		/** Copies Value into the arena */
		FNode* NewString(FUtf8StringView Value);
		/** No copy: Data must live in Input or the arena, NUL-terminated */
		FNode* NewStringRef(const UTF8CHAR* Data, uint32 Len);
		FNode* NewArray(uint32 Reserve = 0);
		FNode* NewObject(uint32 Reserve = 0);

		FStr CopyStr(FUtf8StringView Value, bool bHash);
		static FStr RefStr(const UTF8CHAR* Data, uint32 Len, bool bHash);

		/** Index of the LAST member with this key, or INDEX_NONE */
		static int32 FindMember(const FNode& Object, FUtf8StringView Key, uint32 Hash);
		static FNode* Find(const FNode& Object, FUtf8StringView Key);

		/** Replace the last occurrence's value, or append */
		void Set(FNode& Object, FUtf8StringView Key, FNode* Value);
		/** Append without looking for duplicates (the parser keeps them) */
		void Append(FNode& Object, const FStr& Key, FNode* Value);
		/** Remove every occurrence; returns how many were removed */
		int32 Remove(FNode& Object, FUtf8StringView Key);
		void BuildIndex(FNode& Object);

		void Append(FNode& Array, FNode* Value);
		bool SetAt(FNode& Array, uint32 Index, FNode* Value);
		bool RemoveAt(FNode& Array, uint32 Index);

		/** Exact-size fill used by the parser */
		void AssignArray(FNode& Array, TConstArrayView<FNode*> Items);
		void AssignObject(FNode& Object, TConstArrayView<FMember> Members);

		/** Deep copy of Src, from any document, into this one. Reads Src completely before returning. */
		FNode* DeepCopy(const FNode& Src);

		/** JSON equality: 1 == 1.0, objects ignore member order and compare duplicates by their last occurrence */
		static bool Equals(const FNode& A, const FNode& B);

	private:
		FNode* NewNode(EVaRestJsonType Type);
		void IndexInsert(FNode& Object, uint32 MemberIndex);
	};
}
```

- [ ] **Step 5: Implement `VaRestJsonDocument.cpp`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonDocument.h"

namespace VaRest::Json
{
	namespace
	{
		template <typename T>
		T* GrowVector(FArena& Arena, T* Old, uint32 Num, uint32& InOutMax)
		{
			// The old vector is abandoned in the arena; the nodes it pointed at do not move.
			const uint32 NewMax = FMath::Max<uint32>(4, InOutMax * 2);
			T* New = Arena.NewArray<T>(NewMax);
			if (Num)
			{
				FMemory::Memcpy(New, Old, sizeof(T) * Num);
			}
			InOutMax = NewMax;
			return New;
		}

		bool NumbersEqual(const FNode& A, const FNode& B)
		{
			if (A.NumKind == B.NumKind)
			{
				switch (A.NumKind)
				{
				case ENumKind::Int:    return A.Int == B.Int;
				case ENumKind::UInt:   return A.UInt == B.UInt;
				case ENumKind::Double: return A.Double == B.Double;
				}
			}
			if (A.NumKind != ENumKind::Double && B.NumKind != ENumKind::Double)
			{
				return false; // Int and UInt never overlap: UInt holds only values above INT64_MAX
			}
			const FNode& D = A.NumKind == ENumKind::Double ? A : B;
			const FNode& I = A.NumKind == ENumKind::Double ? B : A;
			const double V = D.Double;
			if (!FMath::IsFinite(V) || FMath::TruncToDouble(V) != V)
			{
				return false;
			}
			if (I.NumKind == ENumKind::Int)
			{
				return V >= -9223372036854775808.0 && V < 9223372036854775808.0 && static_cast<int64>(V) == I.Int;
			}
			return V >= 9223372036854775808.0 && V < 18446744073709551616.0 && static_cast<uint64>(V) == I.UInt;
		}
	}

	FNode* FDocument::NewNode(EVaRestJsonType Type)
	{
		FNode* Node = Arena.New<FNode>();
		Node->Type = Type;
		return Node;
	}

	FNode* FDocument::NewNull()
	{
		return NewNode(EVaRestJsonType::Null);
	}

	FNode* FDocument::NewBool(bool bValue)
	{
		FNode* Node = NewNode(EVaRestJsonType::Bool);
		Node->Bool = bValue;
		return Node;
	}

	FNode* FDocument::NewInt(int64 Value)
	{
		FNode* Node = NewNode(EVaRestJsonType::Number);
		Node->NumKind = ENumKind::Int;
		Node->Int = Value;
		return Node;
	}

	FNode* FDocument::NewUInt(uint64 Value)
	{
		if (Value <= static_cast<uint64>(MAX_int64))
		{
			return NewInt(static_cast<int64>(Value));
		}
		FNode* Node = NewNode(EVaRestJsonType::Number);
		Node->NumKind = ENumKind::UInt;
		Node->UInt = Value;
		return Node;
	}

	FNode* FDocument::NewDouble(double Value)
	{
		FNode* Node = NewNode(EVaRestJsonType::Number);
		Node->NumKind = ENumKind::Double;
		Node->Double = Value;
		return Node;
	}

	FStr FDocument::CopyStr(FUtf8StringView Value, bool bHash)
	{
		const uint32 Len = static_cast<uint32>(Value.Len());
		UTF8CHAR* Data = Arena.NewArray<UTF8CHAR>(Len + 1);
		if (Len)
		{
			FMemory::Memcpy(Data, Value.GetData(), Len);
		}
		Data[Len] = UTF8CHAR(0);
		return FStr{ Data, Len, bHash ? HashKey(Value) : 0u };
	}

	FStr FDocument::RefStr(const UTF8CHAR* Data, uint32 Len, bool bHash)
	{
		return FStr{ Data, Len, bHash ? HashKey(FUtf8StringView(Data, Len)) : 0u };
	}

	FNode* FDocument::NewString(FUtf8StringView Value)
	{
		FNode* Node = NewNode(EVaRestJsonType::String);
		Node->Str = CopyStr(Value, false);
		return Node;
	}

	FNode* FDocument::NewStringRef(const UTF8CHAR* Data, uint32 Len)
	{
		FNode* Node = NewNode(EVaRestJsonType::String);
		Node->Str = RefStr(Data, Len, false);
		return Node;
	}

	FNode* FDocument::NewArray(uint32 Reserve)
	{
		FNode* Node = NewNode(EVaRestJsonType::Array);
		Node->Arr = FNode::FArr{ Arena.NewArray<FNode*>(Reserve), 0, Reserve };
		return Node;
	}

	FNode* FDocument::NewObject(uint32 Reserve)
	{
		FNode* Node = NewNode(EVaRestJsonType::Object);
		Node->Obj = FNode::FObj{ Arena.NewArray<FMember>(Reserve), 0, Reserve, nullptr };
		return Node;
	}

	int32 FDocument::FindMember(const FNode& Object, FUtf8StringView Key, uint32 Hash)
	{
		check(Object.Type == EVaRestJsonType::Object);
		const FNode::FObj& O = Object.Obj;
		if (const FIndex* Index = O.Index)
		{
			const uint32 Mask = Index->Capacity - 1;
			for (uint32 Slot = Hash & Mask;; Slot = (Slot + 1) & Mask)
			{
				const uint32 Entry = Index->Slots[Slot];
				if (Entry == 0)
				{
					return INDEX_NONE;
				}
				const FMember& M = O.Members[Entry - 1];
				if (M.Key.Hash == Hash && KeyEquals(M.Key, Key))
				{
					return static_cast<int32>(Entry - 1);
				}
			}
		}
		for (int32 i = static_cast<int32>(O.Num) - 1; i >= 0; --i)
		{
			const FMember& M = O.Members[i];
			if (M.Key.Hash == Hash && KeyEquals(M.Key, Key))
			{
				return i; // scanning from the end: the last occurrence wins
			}
		}
		return INDEX_NONE;
	}

	FNode* FDocument::Find(const FNode& Object, FUtf8StringView Key)
	{
		const int32 i = FindMember(Object, Key, HashKey(Key));
		return i == INDEX_NONE ? nullptr : Object.Obj.Members[i].Value;
	}

	void FDocument::Set(FNode& Object, FUtf8StringView Key, FNode* Value)
	{
		const int32 i = FindMember(Object, Key, HashKey(Key));
		if (i != INDEX_NONE)
		{
			Object.Obj.Members[i].Value = Value;
			return;
		}
		Append(Object, CopyStr(Key, true), Value);
	}

	void FDocument::Append(FNode& Object, const FStr& Key, FNode* Value)
	{
		check(Object.Type == EVaRestJsonType::Object);
		FNode::FObj& O = Object.Obj;
		if (O.Num == O.Max)
		{
			O.Members = GrowVector(Arena, O.Members, O.Num, O.Max);
		}
		O.Members[O.Num] = FMember{ Key, Value };
		++O.Num;

		if (O.Index)
		{
			if (O.Num * 2 > O.Index->Capacity)
			{
				BuildIndex(Object);
			}
			else
			{
				IndexInsert(Object, O.Num - 1);
			}
		}
		else if (O.Num > IndexThreshold)
		{
			BuildIndex(Object);
		}
	}

	void FDocument::IndexInsert(FNode& Object, uint32 MemberIndex)
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
				return;
			}
			const FMember& Other = Object.Obj.Members[Entry - 1];
			if (Other.Key.Hash == M.Key.Hash && KeyEquals(Other.Key, M.Key.View()))
			{
				Entry = MemberIndex + 1; // a later duplicate takes the slot: last occurrence wins
				return;
			}
		}
	}

	void FDocument::BuildIndex(FNode& Object)
	{
		FNode::FObj& O = Object.Obj;
		const uint32 Capacity = FMath::RoundUpToPowerOfTwo(FMath::Max<uint32>(16, O.Num * 2));
		FIndex* Index = Arena.New<FIndex>();
		Index->Capacity = Capacity;
		Index->Slots = Arena.NewArray<uint32>(Capacity);
		FMemory::Memzero(Index->Slots, sizeof(uint32) * Capacity);
		O.Index = Index;
		for (uint32 i = 0; i < O.Num; ++i)
		{
			IndexInsert(Object, i);
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
			// Members shifted, so every index slot after the first removal is stale.
			O.Index = nullptr;
			if (O.Num > IndexThreshold)
			{
				BuildIndex(Object);
			}
		}
		return Removed;
	}

	void FDocument::Append(FNode& Array, FNode* Value)
	{
		check(Array.Type == EVaRestJsonType::Array);
		FNode::FArr& A = Array.Arr;
		if (A.Num == A.Max)
		{
			A.Items = GrowVector(Arena, A.Items, A.Num, A.Max);
		}
		A.Items[A.Num++] = Value;
	}

	bool FDocument::SetAt(FNode& Array, uint32 Index, FNode* Value)
	{
		check(Array.Type == EVaRestJsonType::Array);
		if (Index >= Array.Arr.Num)
		{
			return false;
		}
		Array.Arr.Items[Index] = Value;
		return true;
	}

	bool FDocument::RemoveAt(FNode& Array, uint32 Index)
	{
		check(Array.Type == EVaRestJsonType::Array);
		FNode::FArr& A = Array.Arr;
		if (Index >= A.Num)
		{
			return false;
		}
		FMemory::Memmove(A.Items + Index, A.Items + Index + 1, sizeof(FNode*) * (A.Num - Index - 1));
		--A.Num;
		return true;
	}

	void FDocument::AssignArray(FNode& Array, TConstArrayView<FNode*> Items)
	{
		const uint32 Num = static_cast<uint32>(Items.Num());
		FNode** Data = Arena.NewArray<FNode*>(Num);
		if (Num)
		{
			FMemory::Memcpy(Data, Items.GetData(), sizeof(FNode*) * Num);
		}
		Array.Arr = FNode::FArr{ Data, Num, Num };
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
		if (Num > IndexThreshold)
		{
			BuildIndex(Object);
		}
	}

	FNode* FDocument::DeepCopy(const FNode& Src)
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
			const uint32 Num = Src.Arr.Num;
			FNode* Node = NewArray(Num);
			for (uint32 i = 0; i < Num; ++i)
			{
				Node->Arr.Items[i] = DeepCopy(*Src.Arr.Items[i]);
			}
			Node->Arr.Num = Num;
			return Node;
		}
		case EVaRestJsonType::Object:
		{
			const uint32 Num = Src.Obj.Num;
			FNode* Node = NewObject(Num);
			for (uint32 i = 0; i < Num; ++i)
			{
				const FMember& M = Src.Obj.Members[i];
				FStr Key = CopyStr(M.Key.View(), false);
				Key.Hash = M.Key.Hash;
				Node->Obj.Members[i] = FMember{ Key, DeepCopy(*M.Value) };
			}
			Node->Obj.Num = Num;
			if (Num > IndexThreshold)
			{
				BuildIndex(*Node);
			}
			return Node;
		}
		}
		return NewNull();
	}

	bool FDocument::Equals(const FNode& A, const FNode& B)
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
			return NumbersEqual(A, B);
		case EVaRestJsonType::String:
			return KeyEquals(A.Str, B.Str.View());
		case EVaRestJsonType::Array:
			if (A.Arr.Num != B.Arr.Num)
			{
				return false;
			}
			for (uint32 i = 0; i < A.Arr.Num; ++i)
			{
				if (!Equals(*A.Arr.Items[i], *B.Arr.Items[i]))
				{
					return false;
				}
			}
			return true;
		case EVaRestJsonType::Object:
		{
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
				if (j == INDEX_NONE || !Equals(*M.Value, *B.Obj.Members[j].Value))
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
}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `10 passed, 0 failed`.

- [ ] **Step 7: Restore-the-bug (two behaviours)**

1. In `FindMember`'s linear branch, scan ascending (`for (int32 i = 0; i < (int32)O.Num; ++i)`). Run `Tools/run_tests.sh VaRest.Json.Document.ObjectMembers` → expect FAIL on `last occurrence wins`. Restore.
2. In `Remove`, delete the `O.Index = nullptr; if (...) BuildIndex(Object);` block. Run `Tools/run_tests.sh VaRest.Json.Document.Index` → expect FAIL (`k999 after shift` or `k0 gone`). Restore. Run the full filter again → all pass.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: arena DOM document -- nodes that never move, indexed objects, last-wins duplicates

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `FVaRestJson` handle API

**Files:**
- Modify (replace): `Source/VaRestJson/Public/VaRestJson.h`
- Create: `Source/VaRestJson/Private/VaRestJson.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp`

**Interfaces:**
- Consumes: `FDocument`, `FNode`, `KeyEquals` (Task 3); `LogVaRestJson` (Task 1).
- Produces: the full public header below. `Parse*`, `ToString`, `ToUtf8` are **declared here and defined in Tasks 5–6**; `Path` in Task 7; bridges in Task 8. Do not call them in this task.

- [ ] **Step 1: Write the failing test**

`Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiBuildReadTest, "VaRest.Json.Api.BuildAndRead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiBuildReadTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("name"), TEXT("Neo"))
		.Set(TEXT("level"), 42)
		.Set(TEXT("ratio"), 0.5)
		.Set(TEXT("alive"), true)
		.Set(TEXT("nothing"), nullptr)
		.Set(TEXT("tags"), FVaRestJson::Array(TEXT("a"), TEXT("b"), 3));

	TestTrue(TEXT("type"), J.GetType() == EVaRestJsonType::Object);
	TestEqual(TEXT("Num"), J.Num(), 6);
	TestEqual(TEXT("string"), J[TEXT("name")].AsString(), FString(TEXT("Neo")));
	TestEqual(TEXT("int"), J[TEXT("level")].AsInt(), 42);
	TestEqual(TEXT("double"), J[TEXT("ratio")].AsDouble(), 0.5);
	TestTrue(TEXT("bool"), J[TEXT("alive")].AsBool());
	TestTrue(TEXT("null"), J[TEXT("nothing")].IsNull());
	TestEqual(TEXT("array len"), J[TEXT("tags")].Num(), 3);
	TestEqual(TEXT("array elem"), J[TEXT("tags")][1].AsString(), FString(TEXT("b")));
	TestEqual(TEXT("utf8 key"), J[UTF8TEXT("level")].AsInt(), 42);

	// Missing keys and wrong types give the default, never a crash
	TestFalse(TEXT("missing is invalid"), J[TEXT("nope")].IsValid());
	TestEqual(TEXT("missing -> default"), J[TEXT("nope")].AsInt(7), 7);
	TestEqual(TEXT("wrong type -> default"), J[TEXT("name")].AsInt(-1), -1);
	TestTrue(TEXT("index on object -> invalid"), J[0].GetType() == EVaRestJsonType::None);
	TestEqual(TEXT("chained on missing"), J[TEXT("a")][TEXT("b")][3].AsString(TEXT("d")), FString(TEXT("d")));

	// FName / FText / FString / UTF-8 values
	J.Set(TEXT("n"), FName(TEXT("Nm"))).Set(TEXT("t"), FText::FromString(TEXT("Tx"))).Set(TEXT("s"), FString(TEXT("St"))).Set(TEXT("u"), FUtf8StringView(UTF8TEXT("Ут")));
	TestEqual(TEXT("FName"), J[TEXT("n")].AsString(), FString(TEXT("Nm")));
	TestEqual(TEXT("FText"), J[TEXT("t")].AsString(), FString(TEXT("Tx")));
	TestEqual(TEXT("FString"), J[TEXT("s")].AsString(), FString(TEXT("St")));
	TestEqual(TEXT("UTF-8"), J[TEXT("u")].AsString(), FString(TEXT("Ут")));
	TestTrue(TEXT("zero-copy view"), J[TEXT("u")].AsUtf8View().Equals(FUtf8StringView(UTF8TEXT("Ут")), ESearchCase::CaseSensitive));
	J.Set(UTF8TEXT("raw"), 5);
	TestTrue(TEXT("UTF-8 key Set/Has"), J.Has(FUtf8StringView(UTF8TEXT("raw"))) && J[TEXT("raw")].AsInt() == 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiTryGetTest, "VaRest.Json.Api.TryGetRanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiTryGetTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("big"), 300)
		.Set(TEXT("neg"), -1)
		.Set(TEXT("whole"), 3.0)
		.Set(TEXT("frac"), 3.5)
		.Set(TEXT("max64"), MAX_int64)
		.Set(TEXT("umax"), MAX_uint64);

	TestFalse(TEXT("300 is not an int8"), J[TEXT("big")].TryGet<int8>().IsSet());
	TestEqual(TEXT("300 is an int16"), (int32)J[TEXT("big")].TryGet<int16>().Get(0), 300);
	TestFalse(TEXT("-1 is not a uint32"), J[TEXT("neg")].TryGet<uint32>().IsSet());
	TestEqual(TEXT("3.0 reads as int"), J[TEXT("whole")].TryGet<int32>().Get(0), 3);
	TestFalse(TEXT("3.5 does not read as int"), J[TEXT("frac")].TryGet<int32>().IsSet());
	TestEqual(TEXT("int64 max"), J[TEXT("max64")].AsInt64(), MAX_int64);
	TestFalse(TEXT("uint64 max is not an int64"), J[TEXT("umax")].TryGet<int64>().IsSet());
	TestEqual(TEXT("uint64 max"), J[TEXT("umax")].AsUInt64(), MAX_uint64);
	TestEqual(TEXT("float from int"), J[TEXT("big")].AsFloat(), 300.f);
	TestFalse(TEXT("no string coercion"), J[TEXT("big")].TryGet<FString>().IsSet());
	TestFalse(TEXT("no bool coercion"), J[TEXT("big")].TryGet<bool>().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiSemanticsTest, "VaRest.Json.Api.ReferenceSemantics",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiSemanticsTest::RunTest(const FString& Parameters)
{
	FVaRestJson Root = FVaRestJson::Object().Set(TEXT("user"), FVaRestJson::Object());

	// A handle into the tree aliases the node: mutation is visible through the parent
	FVaRestJson User = Root[TEXT("user")];
	User.Set(TEXT("x"), 1);
	TestEqual(TEXT("mutation through child handle"), Root[TEXT("user")][TEXT("x")].AsInt(), 1);

	// Set copies its value: later changes to the source do not reach the target
	FVaRestJson Temp = FVaRestJson::Object();
	Root.Set(TEXT("copy"), Temp);
	Temp.Set(TEXT("y"), 2);
	TestFalse(TEXT("Set copied"), Root[TEXT("copy")].Has(TEXT("y")));

	// Setting a tree into itself copies the tree as it was
	Root.Set(TEXT("self"), Root);
	TestTrue(TEXT("self copy has user"), Root[TEXT("self")][TEXT("user")].IsObject());
	TestFalse(TEXT("self copy predates itself"), Root[TEXT("self")].Has(TEXT("self")));

	// Clone is independent
	FVaRestJson Clone = Root.Clone();
	Clone[TEXT("user")].Set(TEXT("x"), 100);
	TestEqual(TEXT("clone does not alias"), Root[TEXT("user")][TEXT("x")].AsInt(), 1);
	TestTrue(TEXT("clone of unchanged subtree equals"), Clone[TEXT("copy")].Equals(Root[TEXT("copy")]));

	// A child handle keeps the whole document alive
	FVaRestJson Survivor;
	{
		FVaRestJson Scoped = FVaRestJson::Object().Set(TEXT("k"), TEXT("v"));
		Survivor = Scoped[TEXT("k")];
	}
	TestEqual(TEXT("child outlives its root handle"), Survivor.AsString(), FString(TEXT("v")));

	// Auto-vivify: an invalid handle becomes the container the first write needs
	FVaRestJson Fresh;
	Fresh.Set(TEXT("a"), 1);
	TestTrue(TEXT("Set on invalid -> object"), Fresh.IsObject());
	FVaRestJson FreshArray;
	FreshArray.Add(1);
	TestTrue(TEXT("Add on invalid -> array"), FreshArray.IsArray());

	// Writing into the wrong type is an error and a no-op
	AddExpectedError(TEXT("FVaRestJson: cannot Set a field on a String"), EAutomationExpectedErrorFlags::Contains, 1);
	FVaRestJson Str = FVaRestJson::From(TEXT("s"));
	Str.Set(TEXT("a"), 1);
	TestTrue(TEXT("still a string"), Str.IsString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiArrayTest, "VaRest.Json.Api.Arrays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiArrayTest::RunTest(const FString& Parameters)
{
	FVaRestJson A = FVaRestJson::Array(1, 2, 3);
	A.Add(4).SetAt(0, TEXT("zero"));
	TestEqual(TEXT("Num"), A.Num(), 4);
	TestEqual(TEXT("SetAt"), A[0].AsString(), FString(TEXT("zero")));
	TestTrue(TEXT("RemoveAt"), A.RemoveAt(1));
	TestEqual(TEXT("shifted"), A[1].AsInt(), 3);
	TestFalse(TEXT("RemoveAt out of range"), A.RemoveAt(99));
	TestFalse(TEXT("At out of range is invalid"), A[99].IsValid());
	TestFalse(TEXT("negative index is invalid"), A[-1].IsValid());

	int32 Sum = 0;
	for (const FVaRestJson& Item : A.Elements())
	{
		Sum += Item.AsInt();
	}
	TestEqual(TEXT("Elements()"), Sum, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiIterationTest, "VaRest.Json.Api.MembersIteration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiIterationTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object().Set(TEXT("b"), 1).Set(TEXT("a"), 2).Set(TEXT("c"), 3);
	TArray<FString> Keys;
	for (const auto& [Key, Value] : J.Members())
	{
		Keys.Add(FString::ConstructFromPtrSize(Key.GetData(), Key.Len()));
	}
	TestEqual(TEXT("insertion order"), FString::Join(Keys, TEXT(",")), FString(TEXT("b,a,c")));
	TestEqual(TEXT("GetKeys"), FString::Join(J.GetKeys(), TEXT(",")), FString(TEXT("b,a,c")));

	// Removing while iterating must not crash or read out of range
	int32 Visited = 0;
	for (const FVaRestJsonMember& M : J.Members())
	{
		++Visited;
		J.Remove(M.GetKey());
	}
	// Deterministic: visit b (removed -> [a,c]), index 1 is c (removed -> [a]), index 2 >= Num stops.
	TestEqual(TEXT("visits"), Visited, 2);
	TestEqual(TEXT("survivor"), FString::Join(J.GetKeys(), TEXT(",")), FString(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiConcurrentReadTest, "VaRest.Json.Api.ConcurrentReaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiConcurrentReadTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object();
	for (int32 i = 0; i < 2000; ++i)
	{
		J.Set(*FString::Printf(TEXT("key%d"), i), i);
	}
	std::atomic<int32> Wrong{ 0 };
	ParallelFor(64, [&J, &Wrong](int32 Worker)
	{
		for (int32 i = 0; i < 2000; ++i)
		{
			const int32 k = (i * 31 + Worker) % 2000;
			if (J[*FString::Printf(TEXT("key%d"), k)].AsInt(-1) != k)
			{
				++Wrong;
			}
		}
	});
	TestEqual(TEXT("concurrent lookups"), Wrong.load(), 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Api`
Expected: build fails (`no member named 'Object' in 'FVaRestJson'`).

- [ ] **Step 3: Replace `Source/VaRestJson/Public/VaRestJson.h`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"

#include <type_traits>

#include "VaRestJson.generated.h"

class FJsonObject;
class FJsonValue;

namespace VaRest::Json
{
	struct FNode;
	class FDocument;
}

struct FVaRestJsonMember;
class FVaRestJsonMemberRange;
class FVaRestJsonElementRange;

/** Type of the value a FVaRestJson handle points at. None = invalid handle (no value). */
UENUM(BlueprintType)
enum class EVaRestJsonType : uint8
{
	None,
	Null,
	Bool,
	Number,
	String,
	Array,
	Object
};

UENUM(BlueprintType)
enum class EVaRestJsonFormat : uint8
{
	Condensed,
	/** Two-space indent, one member or element per line */
	Pretty
};

USTRUCT(BlueprintType)
struct VARESTJSON_API FVaRestJsonError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	FString Message;

	/** Byte offset into the UTF-8 input, or -1 */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int64 Offset = -1;

	/** 1-based; 0 when unknown (in-situ parses of a stolen buffer cannot recover lines) */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int32 Line = 0;

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	int32 Column = 0;

	/** JSON path of the offending value (decode errors); empty for syntax errors */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Json")
	FString Path;

	FString ToString() const;
};

struct FVaRestJsonParseOptions
{
	/** Nesting deeper than this fails the parse (hostile-input guard) */
	int32 MaxDepth = 512;

	/** Accept comments and trailing commas (config files, not wire data) */
	bool bAllowCommentsAndTrailingCommas = false;
};

/**
 * Handle to a value inside a shared JSON document.
 *
 * REFERENCE semantics: copying a handle aliases the same node, and mutating through any handle is
 * visible through all of them (like VaRest 1 objects and FJsonObjectWrapper). Set/Add COPY their
 * value into this document. Clone() makes an independent tree.
 *
 * Threading: any number of threads may read a document nobody mutates; mutation needs exclusive
 * access; handles are safe to copy across threads.
 */
USTRUCT(BlueprintType, meta = (DisplayName = "VaRest Json"))
struct VARESTJSON_API FVaRestJson
{
	GENERATED_BODY()

	using FDocumentRef = TSharedPtr<VaRest::Json::FDocument, ESPMode::ThreadSafe>;

	FVaRestJson() = default;

	// ---- construction

	static FVaRestJson Object();
	static FVaRestJson Array();
	static FVaRestJson Null();

	/** Array of the given values: FVaRestJson::Array(TEXT("a"), 2, true) */
	template <typename... ItemTypes>
	static FVaRestJson Array(ItemTypes&&... Items)
	{
		FVaRestJson Result = Array();
		(Result.Add(Forward<ItemTypes>(Items)), ...);
		return Result;
	}

	/** A one-value document */
	template <typename ValueType>
	static FVaRestJson From(ValueType&& Value)
	{
		// Locals and parameters never reuse the member names (Doc, Node): UE builds with
		// shadow-variable warnings as errors.
		FDocumentRef NewDoc = NewDocument();
		VaRest::Json::FNode* Root = MakeNodeFor(*NewDoc, Forward<ValueType>(Value));
		return FVaRestJson(MoveTemp(NewDoc), Root);
	}

	// ---- type

	EVaRestJsonType GetType() const;
	bool IsValid() const { return Node != nullptr; }
	bool IsNull() const { return GetType() == EVaRestJsonType::Null; }
	bool IsBool() const { return GetType() == EVaRestJsonType::Bool; }
	bool IsNumber() const { return GetType() == EVaRestJsonType::Number; }
	bool IsString() const { return GetType() == EVaRestJsonType::String; }
	bool IsArray() const { return GetType() == EVaRestJsonType::Array; }
	bool IsObject() const { return GetType() == EVaRestJsonType::Object; }

	/** Members of an object, elements of an array, 0 otherwise */
	int32 Num() const;

	// ---- scalar reads (no coercion between types; integer reads accept integral doubles in range)

	bool AsBool(bool Default = false) const { return TryGetBool().Get(Default); }
	int32 AsInt(int32 Default = 0) const { return TryGet<int32>().Get(Default); }
	int64 AsInt64(int64 Default = 0) const { return TryGetInt64().Get(Default); }
	uint64 AsUInt64(uint64 Default = 0) const { return TryGetUInt64().Get(Default); }
	float AsFloat(float Default = 0.f) const { return TryGet<float>().Get(Default); }
	double AsDouble(double Default = 0.0) const { return TryGetDouble().Get(Default); }
	FString AsString(const FString& Default = FString()) const;

	/** Zero-copy view of a string value; empty if not a string. Valid while any handle to the document lives. */
	FUtf8StringView AsUtf8View() const;

	template <typename T>
	TOptional<T> TryGet() const
	{
		using U = std::decay_t<T>;
		if constexpr (std::is_same_v<U, bool>)
		{
			return TryGetBool();
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
			return V ? TOptional<U>(static_cast<U>(*V)) : TOptional<U>();
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
		else
		{
			static_assert(sizeof(U) == 0, "FVaRestJson::TryGet: unsupported type (structs arrive with FVaRestJsonConvert in M2)");
			return {};
		}
	}

	// ---- objects

	FVaRestJson Get(FStringView Key) const;
	FVaRestJson Get(FUtf8StringView Key) const;
	FVaRestJson operator[](FStringView Key) const { return Get(Key); }
	FVaRestJson operator[](FUtf8StringView Key) const { return Get(Key); }
	FVaRestJson operator[](const TCHAR* Key) const { return Get(FStringView(Key)); }
	FVaRestJson operator[](const UTF8CHAR* Key) const { return Get(FUtf8StringView(Key)); }

	bool Has(FStringView Key) const { return Get(Key).IsValid(); }
	bool Has(FUtf8StringView Key) const { return Get(Key).IsValid(); }

	/** Sets (copies) Value under Key. An invalid handle becomes a new object first. */
	template <typename ValueType>
	FVaRestJson& Set(FStringView Key, ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			SetNode(Key, MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	/** UTF-8 key: no TCHAR -> UTF-8 conversion on hot paths */
	template <typename ValueType>
	FVaRestJson& Set(FUtf8StringView Key, ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Object, TEXT("Set a field")))
		{
			SetNodeUtf8(Key, MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson& Set(const TCHAR* Key, ValueType&& Value) { return Set(FStringView(Key), Forward<ValueType>(Value)); }

	template <typename ValueType>
	FVaRestJson& Set(const UTF8CHAR* Key, ValueType&& Value) { return Set(FUtf8StringView(Key), Forward<ValueType>(Value)); }

	bool Remove(FStringView Key);
	TArray<FString> GetKeys() const;
	FVaRestJsonMemberRange Members() const;
	FVaRestJsonMember MemberAt(int32 Index) const;

	// ---- arrays

	FVaRestJson At(int32 Index) const;
	FVaRestJson operator[](int32 Index) const { return At(Index); }

	/** Appends (copies) Value. An invalid handle becomes a new array first. */
	template <typename ValueType>
	FVaRestJson& Add(ValueType&& Value)
	{
		if (VaRest::Json::FDocument* TargetDoc = PrepareContainer(EVaRestJsonType::Array, TEXT("Add an element")))
		{
			AddNode(MakeNodeFor(*TargetDoc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	template <typename ValueType>
	FVaRestJson& SetAt(int32 Index, ValueType&& Value)
	{
		if (IsArray())
		{
			SetAtNode(Index, MakeNodeFor(*Doc, Forward<ValueType>(Value)));
		}
		return *this;
	}

	bool RemoveAt(int32 Index);
	FVaRestJsonElementRange Elements() const;

	// ---- whole value

	FVaRestJson Clone() const;
	bool Equals(const FVaRestJson& Other) const;
	bool operator==(const FVaRestJson& Other) const { return Equals(Other); }
	bool IsSameNode(const FVaRestJson& Other) const { return Node == Other.Node; }

	/** "a.b[2].c", or an RFC 6901 pointer when it starts with '/'. Invalid syntax or a miss -> invalid handle. (Task 7) */
	FVaRestJson Path(FStringView Expression) const;

	// ---- text (Tasks 5-6)

	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(FUtf8StringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(FStringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	static TValueOrError<FVaRestJson, FVaRestJsonError> Parse(TConstArrayView<uint8> Utf8Bytes, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());
	/** Steals the buffer (an HTTP response body): zero copies; parsed strings point into it */
	static TValueOrError<FVaRestJson, FVaRestJsonError> ParseOwned(TArray<uint8>&& Utf8Bytes, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions());

	/** "" for an invalid handle, "null" for a null value */
	FString ToString(EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;
	/** Appends UTF-8 to Out; nothing for an invalid handle */
	void ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed) const;

	// ---- engine interop (Task 8)

	TSharedPtr<FJsonValue> ToJsonValue() const;
	/** Null when this is not an object */
	TSharedPtr<FJsonObject> ToJsonObject() const;
	static FVaRestJson FromJsonValue(const TSharedPtr<FJsonValue>& Value);
	static FVaRestJson FromJsonObject(const TSharedPtr<FJsonObject>& Object);

	// ---- for sibling VaRest modules and tests; not a stable API

	const FDocumentRef& GetDocumentInternal() const { return Doc; }
	VaRest::Json::FNode* GetNodeInternal() const { return Node; }
	static FVaRestJson MakeInternal(FDocumentRef InDoc, VaRest::Json::FNode* InNode) { return FVaRestJson(MoveTemp(InDoc), InNode); }

private:
	FVaRestJson(FDocumentRef InDoc, VaRest::Json::FNode* InNode)
		: Doc(MoveTemp(InDoc))
		, Node(InNode)
	{
	}

	static FDocumentRef NewDocument(SIZE_T ArenaFirstChunk = 4096);

	VaRest::Json::FDocument* PrepareContainer(EVaRestJsonType Kind, const TCHAR* Operation);
	void SetNode(FStringView Key, VaRest::Json::FNode* Value);
	void SetNodeUtf8(FUtf8StringView Key, VaRest::Json::FNode* Value);
	void AddNode(VaRest::Json::FNode* Value);
	void SetAtNode(int32 Index, VaRest::Json::FNode* Value);

	TOptional<bool> TryGetBool() const;
	TOptional<int64> TryGetInt64() const;
	TOptional<uint64> TryGetUInt64() const;
	TOptional<double> TryGetDouble() const;
	TOptional<FString> TryGetString() const;

	static VaRest::Json::FNode* MakeNull(VaRest::Json::FDocument& InDoc);
	static VaRest::Json::FNode* MakeBool(VaRest::Json::FDocument& InDoc, bool bValue);
	static VaRest::Json::FNode* MakeInt(VaRest::Json::FDocument& InDoc, int64 Value);
	static VaRest::Json::FNode* MakeUInt(VaRest::Json::FDocument& InDoc, uint64 Value);
	static VaRest::Json::FNode* MakeDouble(VaRest::Json::FDocument& InDoc, double Value);
	static VaRest::Json::FNode* MakeString(VaRest::Json::FDocument& InDoc, FStringView Value);
	static VaRest::Json::FNode* MakeUtf8String(VaRest::Json::FDocument& InDoc, FUtf8StringView Value);
	static VaRest::Json::FNode* MakeCopy(VaRest::Json::FDocument& InDoc, const FVaRestJson& Value);

	template <typename ValueType>
	static VaRest::Json::FNode* MakeNodeFor(VaRest::Json::FDocument& InDoc, ValueType&& Value)
	{
		using U = std::decay_t<ValueType>;
		// Routed explicitly: plain overloads would make int -> int64 vs int -> double ambiguous.
		if constexpr (std::is_same_v<U, std::nullptr_t>)
		{
			return MakeNull(InDoc);
		}
		else if constexpr (std::is_same_v<U, bool>)
		{
			return MakeBool(InDoc, Value);
		}
		else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
		{
			return MakeInt(InDoc, static_cast<int64>(Value));
		}
		else if constexpr (std::is_integral_v<U>)
		{
			return MakeUInt(InDoc, static_cast<uint64>(Value));
		}
		else if constexpr (std::is_floating_point_v<U>)
		{
			return MakeDouble(InDoc, static_cast<double>(Value));
		}
		else if constexpr (std::is_same_v<U, FVaRestJson>)
		{
			return MakeCopy(InDoc, Value);
		}
		else if constexpr (std::is_same_v<U, FName> || std::is_same_v<U, FText>)
		{
			return MakeString(InDoc, FStringView(Value.ToString()));
		}
		else if constexpr (std::is_convertible_v<ValueType, FUtf8StringView>)
		{
			return MakeUtf8String(InDoc, FUtf8StringView(Value));
		}
		else if constexpr (std::is_convertible_v<ValueType, FStringView>)
		{
			return MakeString(InDoc, FStringView(Value));
		}
		else
		{
			static_assert(sizeof(U) == 0, "FVaRestJson: unsupported value type (narrow char literals need TEXT(); structs arrive in M2)");
			return nullptr;
		}
	}

	FDocumentRef Doc;
	VaRest::Json::FNode* Node = nullptr;
};

struct FVaRestJsonMember
{
	FUtf8StringView Key;
	FVaRestJson Value;

	FString GetKey() const { return FString::ConstructFromPtrSize(Key.GetData(), Key.Len()); }
};

/** Range over an object's members, in insertion order. Indexes are re-read each step, so removing members while iterating is safe. */
class FVaRestJsonMemberRange
{
public:
	struct FIterator
	{
		const FVaRestJson* Owner;
		int32 Index;

		FVaRestJsonMember operator*() const { return Owner->MemberAt(Index); }
		FIterator& operator++()
		{
			++Index;
			return *this;
		}
		bool operator!=(const FIterator& Other) const { return Index < Other.Index && Index < Owner->Num(); }
	};

	explicit FVaRestJsonMemberRange(const FVaRestJson& InOwner)
		: Owner(InOwner)
	{
	}

	FIterator begin() const { return FIterator{ &Owner, 0 }; }
	FIterator end() const { return FIterator{ &Owner, Owner.IsObject() ? Owner.Num() : 0 }; }

private:
	FVaRestJson Owner; // a copy keeps the document alive during the loop
};

class FVaRestJsonElementRange
{
public:
	struct FIterator
	{
		const FVaRestJson* Owner;
		int32 Index;

		FVaRestJson operator*() const { return Owner->At(Index); }
		FIterator& operator++()
		{
			++Index;
			return *this;
		}
		bool operator!=(const FIterator& Other) const { return Index < Other.Index && Index < Owner->Num(); }
	};

	explicit FVaRestJsonElementRange(const FVaRestJson& InOwner)
		: Owner(InOwner)
	{
	}

	FIterator begin() const { return FIterator{ &Owner, 0 }; }
	FIterator end() const { return FIterator{ &Owner, Owner.IsArray() ? Owner.Num() : 0 }; }

private:
	FVaRestJson Owner;
};
```

- [ ] **Step 4: Implement `Source/VaRestJson/Private/VaRestJson.cpp`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"

using namespace VaRest::Json;

namespace
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
	return MakeShared<FDocument, ESPMode::ThreadSafe>(ArenaFirstChunk);
}

FVaRestJson FVaRestJson::Object()
{
	FDocumentRef NewDoc = NewDocument();
	FNode* Root = NewDoc->NewObject();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Array()
{
	FDocumentRef NewDoc = NewDocument();
	FNode* Root = NewDoc->NewArray();
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::Null()
{
	FDocumentRef NewDoc = NewDocument(256);
	FNode* Root = NewDoc->NewNull();
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
	if (!IsNumber())
	{
		return {};
	}
	switch (Node->NumKind)
	{
	case ENumKind::Int:
		return Node->Int;
	case ENumKind::UInt:
		return {}; // only values above INT64_MAX are stored as UInt
	case ENumKind::Double:
	{
		const double D = Node->Double;
		// [-2^63, 2^63) is the int64 range; both bounds are exact doubles
		if (FMath::IsFinite(D) && FMath::TruncToDouble(D) == D && D >= -9223372036854775808.0 && D < 9223372036854775808.0)
		{
			return static_cast<int64>(D);
		}
		return {};
	}
	}
	return {};
}

TOptional<uint64> FVaRestJson::TryGetUInt64() const
{
	if (!IsNumber())
	{
		return {};
	}
	switch (Node->NumKind)
	{
	case ENumKind::Int:
		return Node->Int >= 0 ? TOptional<uint64>(static_cast<uint64>(Node->Int)) : TOptional<uint64>();
	case ENumKind::UInt:
		return Node->UInt;
	case ENumKind::Double:
	{
		const double D = Node->Double;
		if (FMath::IsFinite(D) && FMath::TruncToDouble(D) == D && D >= 0.0 && D < 18446744073709551616.0)
		{
			return static_cast<uint64>(D);
		}
		return {};
	}
	}
	return {};
}

TOptional<double> FVaRestJson::TryGetDouble() const
{
	if (!IsNumber())
	{
		return {};
	}
	switch (Node->NumKind)
	{
	case ENumKind::Int:    return static_cast<double>(Node->Int);
	case ENumKind::UInt:   return static_cast<double>(Node->UInt);
	case ENumKind::Double: return Node->Double;
	}
	return {};
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
		return {};
	}
	FNode* Found = FDocument::Find(*Node, Key);
	return Found ? FVaRestJson(Doc, Found) : FVaRestJson();
}

FVaRestJson FVaRestJson::Get(FStringView Key) const
{
	if (!IsObject())
	{
		return {};
	}
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	return Get(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

FDocument* FVaRestJson::PrepareContainer(EVaRestJsonType Kind, const TCHAR* Operation)
{
	if (!Node)
	{
		*this = Kind == EVaRestJsonType::Object ? Object() : Array();
		return Doc.Get();
	}
	if (Node->Type == Kind)
	{
		return Doc.Get();
	}
	UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson: cannot %s on a %s"), Operation, TypeName(Node->Type));
	return nullptr;
}

void FVaRestJson::SetNode(FStringView Key, FNode* Value)
{
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	Doc->Set(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length()), Value);
}

void FVaRestJson::SetNodeUtf8(FUtf8StringView Key, FNode* Value)
{
	Doc->Set(*Node, Key, Value);
}

void FVaRestJson::AddNode(FNode* Value)
{
	Doc->Append(*Node, Value);
}

void FVaRestJson::SetAtNode(int32 Index, FNode* Value)
{
	if (Index >= 0)
	{
		Doc->SetAt(*Node, static_cast<uint32>(Index), Value);
	}
}

bool FVaRestJson::Remove(FStringView Key)
{
	if (!IsObject())
	{
		return false;
	}
	const auto Utf8 = StringCast<UTF8CHAR>(Key.GetData(), Key.Len());
	return Doc->Remove(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length())) > 0;
}

TArray<FString> FVaRestJson::GetKeys() const
{
	TArray<FString> Keys;
	if (IsObject())
	{
		Keys.Reserve(Node->Obj.Num);
		for (uint32 i = 0; i < Node->Obj.Num; ++i)
		{
			const FStr& Key = Node->Obj.Members[i].Key;
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
	const FMember& M = Node->Obj.Members[Index];
	return FVaRestJsonMember{ M.Key.View(), FVaRestJson(Doc, M.Value) };
}

FVaRestJson FVaRestJson::At(int32 Index) const
{
	if (!IsArray() || Index < 0 || Index >= static_cast<int32>(Node->Arr.Num))
	{
		return {};
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
	if (!Node)
	{
		return {};
	}
	FDocumentRef NewDoc = NewDocument(FMath::Max<SIZE_T>(4096, Doc->Arena.GetUsedBytes()));
	FNode* Root = NewDoc->DeepCopy(*Node);
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

bool FVaRestJson::Equals(const FVaRestJson& Other) const
{
	if (!Node || !Other.Node)
	{
		return !Node && !Other.Node;
	}
	return Node == Other.Node || FDocument::Equals(*Node, *Other.Node);
}

FNode* FVaRestJson::MakeNull(FDocument& InDoc) { return InDoc.NewNull(); }
FNode* FVaRestJson::MakeBool(FDocument& InDoc, bool bValue) { return InDoc.NewBool(bValue); }
FNode* FVaRestJson::MakeInt(FDocument& InDoc, int64 Value) { return InDoc.NewInt(Value); }
FNode* FVaRestJson::MakeUInt(FDocument& InDoc, uint64 Value) { return InDoc.NewUInt(Value); }
FNode* FVaRestJson::MakeDouble(FDocument& InDoc, double Value) { return InDoc.NewDouble(Value); }
FNode* FVaRestJson::MakeUtf8String(FDocument& InDoc, FUtf8StringView Value) { return InDoc.NewString(Value); }

FNode* FVaRestJson::MakeString(FDocument& InDoc, FStringView Value)
{
	const auto Utf8 = StringCast<UTF8CHAR>(Value.GetData(), Value.Len());
	return InDoc.NewString(FUtf8StringView(Utf8.Get(), Utf8.Length()));
}

FNode* FVaRestJson::MakeCopy(FDocument& InDoc, const FVaRestJson& Value)
{
	// A node has exactly one parent, so even a value from this same document is copied.
	return Value.Node ? InDoc.DeepCopy(*Value.Node) : InDoc.NewNull();
}
```

Note: `From()` sets no `Root` on the document. That is fine: `Root` is only informational; handles carry their node.

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `16 passed, 0 failed`. If UHT rejects a construct in the `USTRUCT` body (templates, `using`), report the exact UHT error rather than restructuring the API.

- [ ] **Step 6: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: FVaRestJson handle API -- reference semantics, copying Set, safe ranges

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Writer — `ToString` / `ToUtf8`

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonWriter.h`, `Source/VaRestJson/Private/VaRestJsonWriter.cpp`, `Source/VaRestJson/Private/VaRestJsonText.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonWriteTest.cpp`

**Interfaces:**
- Consumes: `FNode` (Task 3), `FVaRestJson` (Task 4).
- Produces: `void VaRest::Json::Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out)`; definitions of `FVaRestJson::ToString` and `FVaRestJson::ToUtf8`.

- [ ] **Step 1: Write the failing test**

`Source/VaRestJson/Private/Tests/VaRestJsonWriteTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteCondensedTest, "VaRest.Json.Write.Condensed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteCondensedTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("a"), 1)
		.Set(TEXT("b"), FVaRestJson::Array(true, nullptr, TEXT("x")))
		.Set(TEXT("c"), 1.5)
		.Set(TEXT("d"), FVaRestJson::Object());
	TestEqual(TEXT("condensed"), J.ToString(), FString(TEXT("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":1.5,\"d\":{}}")));
	TestEqual(TEXT("invalid handle -> empty"), FVaRestJson().ToString(), FString());
	TestEqual(TEXT("null"), FVaRestJson::Null().ToString(), FString(TEXT("null")));
	TestEqual(TEXT("scalar root"), FVaRestJson::From(TEXT("hi")).ToString(), FString(TEXT("\"hi\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWritePrettyTest, "VaRest.Json.Write.Pretty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWritePrettyTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), FVaRestJson::Array(1, 2)).Set(TEXT("e"), FVaRestJson::Array());
	const FString Expected = TEXT("{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"e\": []\n}");
	TestEqual(TEXT("pretty, two spaces"), J.ToString(EVaRestJsonFormat::Pretty), Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteValuesTest, "VaRest.Json.Write.Values",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteValuesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("0.1"), FVaRestJson::From(0.1).ToString(), FString(TEXT("0.1")));
	TestEqual(TEXT("1.0 keeps its fraction"), FVaRestJson::From(1.0).ToString(), FString(TEXT("1.0")));
	TestEqual(TEXT("1e300"), FVaRestJson::From(1e300).ToString(), FString(TEXT("1e300")));
	TestEqual(TEXT("int64 min"), FVaRestJson::From(MIN_int64).ToString(), FString(TEXT("-9223372036854775808")));
	TestEqual(TEXT("uint64 max"), FVaRestJson::From(MAX_uint64).ToString(), FString(TEXT("18446744073709551615")));

	const FVaRestJson S = FVaRestJson::From(TEXT("q\"b\\n\n t\t c\x01 Ж"));
	TestEqual(TEXT("escapes; non-ASCII stays raw UTF-8"), S.ToString(), FString(TEXT("\"q\\\"b\\\\n\\n t\\t c\\u0001 Ж\"")));

	// Embedded NUL survives (lengths, not C strings)
	const UTF8CHAR WithNul[] = { 'a', 0, 'b' };
	const FVaRestJson N = FVaRestJson::From(FUtf8StringView(WithNul, 3));
	TestEqual(TEXT("embedded NUL"), N.ToString(), FString(TEXT("\"a\\u0000b\"")));

	// Non-finite numbers cannot be JSON: written as null, with one warning per write
	AddExpectedMessage(TEXT("non-finite number"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FVaRestJson Bad = FVaRestJson::Array(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("NaN/Inf -> null"), Bad.ToString(), FString(TEXT("[null,null]")));

	TArray<uint8> Bytes = { 'x' };
	FVaRestJson::From(2).ToUtf8(Bytes);
	TestEqual(TEXT("ToUtf8 appends"), Bytes.Num(), 2);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Write`
Expected: link error `undefined reference to FVaRestJson::ToString` (the runner reports `build failed`).

- [ ] **Step 3: Implement the writer**

`Source/VaRestJson/Private/VaRestJsonWriter.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonNode.h"

namespace VaRest::Json
{
	/** Appends Node as UTF-8 JSON to Out. Non-finite doubles are written as null with one warning. */
	void Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out);
}
```

`Source/VaRestJson/Private/VaRestJsonWriter.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonWriter.h"

#include "RapidJsonIncludes.h"
#include "VaRestJsonLog.h"

namespace VaRest::Json
{
	namespace
	{
		/** RapidJSON output stream over a TArray. Put/PutReserve/PutUnsafe are found by ADL from writer.h. */
		struct FUtf8Sink
		{
			typedef char Ch;

			TArray<uint8>& Out;

			void Put(char C) { Out.Add(static_cast<uint8>(C)); }
			void Flush() {}
		};

		inline void PutReserve(FUtf8Sink& Sink, size_t Count)
		{
			Sink.Out.Reserve(Sink.Out.Num() + static_cast<int32>(Count));
		}

		inline void PutUnsafe(FUtf8Sink& Sink, char C)
		{
			Sink.Out.Add(static_cast<uint8>(C));
		}

		template <typename WriterType>
		void WriteNode(WriterType& Writer, const FNode& Node, int32& NonFinite)
		{
			switch (Node.Type)
			{
			case EVaRestJsonType::None:
			case EVaRestJsonType::Null:
				Writer.Null();
				break;
			case EVaRestJsonType::Bool:
				Writer.Bool(Node.Bool);
				break;
			case EVaRestJsonType::Number:
				switch (Node.NumKind)
				{
				case ENumKind::Int:
					Writer.Int64(Node.Int);
					break;
				case ENumKind::UInt:
					Writer.Uint64(Node.UInt);
					break;
				case ENumKind::Double:
					if (FMath::IsFinite(Node.Double))
					{
						Writer.Double(Node.Double);
					}
					else
					{
						Writer.Null();
						++NonFinite;
					}
					break;
				}
				break;
			case EVaRestJsonType::String:
				Writer.String(reinterpret_cast<const char*>(Node.Str.Data), static_cast<rapidjson::SizeType>(Node.Str.Len));
				break;
			case EVaRestJsonType::Array:
				Writer.StartArray();
				for (uint32 i = 0; i < Node.Arr.Num; ++i)
				{
					WriteNode(Writer, *Node.Arr.Items[i], NonFinite);
				}
				Writer.EndArray(static_cast<rapidjson::SizeType>(Node.Arr.Num));
				break;
			case EVaRestJsonType::Object:
				Writer.StartObject();
				for (uint32 i = 0; i < Node.Obj.Num; ++i)
				{
					const FMember& M = Node.Obj.Members[i];
					Writer.Key(reinterpret_cast<const char*>(M.Key.Data), static_cast<rapidjson::SizeType>(M.Key.Len));
					WriteNode(Writer, *M.Value, NonFinite);
				}
				Writer.EndObject(static_cast<rapidjson::SizeType>(Node.Obj.Num));
				break;
			}
		}
	}

	void Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out)
	{
		FUtf8Sink Sink{ Out };
		int32 NonFinite = 0;
		if (Format == EVaRestJsonFormat::Pretty)
		{
			rapidjson::PrettyWriter<FUtf8Sink> Writer(Sink);
			Writer.SetIndent(' ', 2);
			WriteNode(Writer, Node, NonFinite);
		}
		else
		{
			rapidjson::Writer<FUtf8Sink> Writer(Sink);
			WriteNode(Writer, Node, NonFinite);
		}
		if (NonFinite > 0)
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), NonFinite);
		}
	}
}
```

`Source/VaRestJson/Private/VaRestJsonText.cpp` (Task 6 appends the parse functions):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "VaRestJsonDocument.h"
#include "VaRestJsonWriter.h"

using namespace VaRest::Json;

void FVaRestJson::ToUtf8(TArray<uint8>& Out, EVaRestJsonFormat Format) const
{
	if (Node)
	{
		Write(*Node, Format, Out);
	}
}

FString FVaRestJson::ToString(EVaRestJsonFormat Format) const
{
	if (!Node)
	{
		return FString();
	}
	TArray<uint8> Bytes;
	Write(*Node, Format, Bytes);
	return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
}
```

- [ ] **Step 4: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `19 passed, 0 failed`. If an exact-output expectation differs only in RapidJSON's formatting of a double (e.g. `1e300`), print the actual string, confirm it round-trips to the same double, and update the expectation **with a comment saying it is RapidJSON's Grisu output** — do not change the writer.

- [ ] **Step 5: Restore-the-bug**

Replace the non-finite branch with `Writer.Double(Node.Double);`. Run `Tools/run_tests.sh VaRest.Json.Write.Values` → expect FAIL (RapidJSON's writer refuses NaN, so the output is truncated or wrong and the expected warning is missing). Restore.

- [ ] **Step 6: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: RapidJSON writer over a TArray sink, NaN/Inf as null

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Reader — `Parse` / `ParseOwned`

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonReader.h`, `Source/VaRestJson/Private/VaRestJsonReader.cpp`
- Modify: `Source/VaRestJson/Private/VaRestJsonText.cpp` (append)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonParseTest.cpp`

**Interfaces:**
- Consumes: `FDocument` (Task 3), writer (Task 5) for round-trip tests.
- Produces: `bool VaRest::Json::ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options, FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed = nullptr)`. `Doc.Input` must be NUL-terminated. `StartOffset` skips a BOM. When `OutConsumed` is non-null, parsing stops after the first complete value (used by `ImportTextItem` in Task 9) and `*OutConsumed` = bytes consumed from `StartOffset`. Definitions of the four `FVaRestJson::Parse*` overloads.

- [ ] **Step 1: Write the failing test**

`Source/VaRestJson/Private/Tests/VaRestJsonParseTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

namespace VaRestJsonParseTest
{
	FVaRestJson P(FAutomationTestBase& Test, const TCHAR* Text)
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(FStringView(Text));
		if (!R.HasValue())
		{
			Test.AddError(FString::Printf(TEXT("parse failed for %s: %s"), Text, *R.GetError().ToString()));
			return FVaRestJson();
		}
		return R.StealValue();
	}

	FVaRestJsonError E(FAutomationTestBase& Test, FUtf8StringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions())
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(Text, Options);
		if (R.HasValue())
		{
			Test.AddError(TEXT("expected a parse error"));
			return FVaRestJsonError();
		}
		return R.GetError();
	}
}
using namespace VaRestJsonParseTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseScalarsTest, "VaRest.Json.Parse.Scalars",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseScalarsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("null"), P(*this, TEXT("null")).IsNull());
	TestTrue(TEXT("true"), P(*this, TEXT("true")).AsBool());
	TestEqual(TEXT("int32 overflow -> int64"), P(*this, TEXT("2147483648")).AsInt64(), 2147483648ll);
	TestEqual(TEXT("int64 max"), P(*this, TEXT("9223372036854775807")).AsInt64(), MAX_int64);
	TestEqual(TEXT("int64 min"), P(*this, TEXT("-9223372036854775808")).AsInt64(), MIN_int64);
	TestEqual(TEXT("2^63 -> uint64"), P(*this, TEXT("9223372036854775808")).AsUInt64(), 9223372036854775808ull);
	TestEqual(TEXT("uint64 max"), P(*this, TEXT("18446744073709551615")).AsUInt64(), MAX_uint64);
	TestFalse(TEXT("2^64 is no longer an integer"), P(*this, TEXT("18446744073709551616")).TryGet<uint64>().IsSet());
	TestEqual(TEXT("huge integer literal -> double"), P(*this, TEXT("100000000000000000000000000000")).AsDouble(), 1e29);
	TestEqual(TEXT("1e308"), P(*this, TEXT("1e308")).AsDouble(), 1e308);
	TestEqual(TEXT("denormal"), P(*this, TEXT("5e-324")).AsDouble(), 5e-324);
	TestEqual(TEXT("full precision"), P(*this, TEXT("0.1")).AsDouble(), 0.1);
	TestEqual(TEXT("-0 is zero"), P(*this, TEXT("-0")).AsDouble(), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseStringsTest, "VaRest.Json.Parse.Strings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseStringsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("escapes"), P(*this, TEXT("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"")).AsString(), FString(TEXT("\"\\/\b\f\n\r\t")));
	TestEqual(TEXT("\\u00e9"), P(*this, TEXT("\"\\u00e9\"")).AsString(), FString(TEXT("é")));
	TestEqual(TEXT("surrogate pair -> 4-byte UTF-8"), P(*this, TEXT("\"\\ud83d\\ude00\"")).AsUtf8View().Len(), 4);
	TestEqual(TEXT("raw UTF-8"), P(*this, TEXT("\"привет\"")).AsString(), FString(TEXT("привет")));

	// Embedded NUL in a key and in a value
	const FVaRestJson J = P(*this, TEXT("{\"a\\u0000b\":\"x\\u0000y\"}"));
	const UTF8CHAR Key[] = { 'a', 0, 'b' };
	TestEqual(TEXT("key with NUL is found"), J[FUtf8StringView(Key, 3)].AsUtf8View().Len(), 3);
	TestEqual(TEXT("NUL round-trips"), J.ToString(), FString(TEXT("{\"a\\u0000b\":\"x\\u0000y\"}")));

	const uint8 BadUtf8[] = { '"', 0xC3, 0x28, '"' };
	TestTrue(TEXT("invalid UTF-8 is rejected"), !FVaRestJson::Parse(TConstArrayView<uint8>(BadUtf8, 4)).HasValue());
	TestFalse(TEXT("unpaired surrogate is rejected"), FVaRestJson::Parse(FStringView(TEXT("\"\\ud800\""))).HasValue());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseStructureTest, "VaRest.Json.Parse.Structure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseStructureTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = P(*this, TEXT(" { \"a\" : [ 1 , { \"b\" : null } , [ ] ] , \"c\" : {} } "));
	TestEqual(TEXT("nested"), J[TEXT("a")].Num(), 3);
	TestTrue(TEXT("deep null"), J[TEXT("a")][1][TEXT("b")].IsNull());
	TestEqual(TEXT("round trip"), J.ToString(), FString(TEXT("{\"a\":[1,{\"b\":null},[]],\"c\":{}}")));

	const FVaRestJson D = P(*this, TEXT("{\"a\":1,\"a\":2}"));
	TestEqual(TEXT("duplicates: last wins"), D[TEXT("a")].AsInt(), 2);
	TestEqual(TEXT("duplicates preserved"), D.Num(), 2);

	FString Big = TEXT("{");
	for (int32 i = 0; i < 1000; ++i)
	{
		Big += FString::Printf(TEXT("%s\"k%d\":%d"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Big += TEXT("}");
	const FVaRestJson B = P(*this, *Big);
	TestEqual(TEXT("big object lookup"), B[TEXT("k777")].AsInt(), 777);

	// UTF-8 BOM is skipped
	const uint8 WithBom[] = { 0xEF, 0xBB, 0xBF, '{', '"', 'a', '"', ':', '1', '}' };
	TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(TConstArrayView<uint8>(WithBom, UE_ARRAY_COUNT(WithBom)));
	TestTrue(TEXT("BOM accepted"), R.HasValue() && R.GetValue()[TEXT("a")].AsInt() == 1);

	// ParseOwned steals the buffer
	TArray<uint8> Owned;
	Owned.Append(reinterpret_cast<const uint8*>("[\"s\"]"), 5);
	TValueOrError<FVaRestJson, FVaRestJsonError> O = FVaRestJson::ParseOwned(MoveTemp(Owned));
	TestTrue(TEXT("owned parse"), O.HasValue() && O.GetValue()[0].AsString() == TEXT("s"));
	TestEqual(TEXT("buffer was moved"), Owned.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseErrorsTest, "VaRest.Json.Parse.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseErrorsTest::RunTest(const FString& Parameters)
{
	const FVaRestJsonError Trailing = E(*this, UTF8TEXT("{\"a\":1,}"));
	TestEqual(TEXT("trailing comma offset"), Trailing.Offset, (int64)7);
	TestEqual(TEXT("trailing comma line"), Trailing.Line, 1);

	const FVaRestJsonError Multi = E(*this, UTF8TEXT("{\n  \"a\": 1,\n  \"b\": x\n}"));
	TestEqual(TEXT("line of the error"), Multi.Line, 3);
	TestEqual(TEXT("column of the error"), Multi.Column, 8);

	TestTrue(TEXT("empty input"), !E(*this, UTF8TEXT("")).Message.IsEmpty());
	TestTrue(TEXT("trailing garbage"), !E(*this, UTF8TEXT("{} x")).Message.IsEmpty());

	FVaRestJsonParseOptions Shallow;
	Shallow.MaxDepth = 4;
	TestTrue(TEXT("depth 4 ok"), FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("[[[[1]]]]")), Shallow).HasValue());
	const FVaRestJsonError Deep = E(*this, UTF8TEXT("[[[[[1]]]]]"), Shallow);
	TestTrue(TEXT("depth 5 fails with a depth message"), Deep.Message.Contains(TEXT("deeper than 4")));

	FString Hostile;
	for (int32 i = 0; i < 100000; ++i)
	{
		Hostile += TEXT("[");
	}
	TestFalse(TEXT("100k-deep input fails cleanly (no stack overflow)"), FVaRestJson::Parse(FStringView(Hostile)).HasValue());

	FVaRestJsonParseOptions Lenient;
	Lenient.bAllowCommentsAndTrailingCommas = true;
	TestTrue(TEXT("comments + trailing comma when allowed"), FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"a\":1, // c\n}")), Lenient).HasValue());

	// A stolen buffer cannot report lines, but still reports the offset
	TArray<uint8> Bad;
	Bad.Append(reinterpret_cast<const uint8*>("[1,,2]"), 6);
	TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::ParseOwned(MoveTemp(Bad));
	TestTrue(TEXT("owned error has offset"), !R.HasValue() && R.GetError().Offset == 3);
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Parse`
Expected: link error `undefined reference to FVaRestJson::Parse`.

- [ ] **Step 3: Implement the reader**

`Source/VaRestJson/Private/VaRestJsonReader.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonDocument.h"

namespace VaRest::Json
{
	/**
	 * Parses Doc.Input (NUL-terminated) in situ from StartOffset into Doc.Root.
	 *
	 * In situ because RapidJSON's SIMD whitespace and string scanners are specialised only for
	 * the null-terminated string streams (rapidjson/reader.h SkipWhitespace overloads), and because
	 * it lets every parsed string point into the buffer with no per-string copy.
	 *
	 * OriginalForDiagnostics: the unmodified input, used only to compute line/column of an error
	 * (in-situ unescaping rewrites the buffer). Empty -> Line/Column stay 0.
	 * OutConsumed: when non-null, stop after the first complete value and report how many bytes it took.
	 */
	bool ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options,
		FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed = nullptr);

	/** 3 when Bytes starts with the UTF-8 BOM, else 0 */
	int64 BomLength(const uint8* Bytes, int64 Num);
}
```

`Source/VaRestJson/Private/VaRestJsonReader.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonReader.h"

#include "RapidJsonIncludes.h"

#include <cstdint>

namespace VaRest::Json
{
	namespace
	{
		/**
		 * SAX handler that builds the arena DOM. Children are collected on two scratch stacks and
		 * copied into exactly-sized arena vectors when their container closes, so parsed containers
		 * never waste growth slack.
		 */
		struct FBuildHandler
		{
			struct FFrame
			{
				FNode* Container;
				int32 Start; // into Members or Elements
			};

			FDocument& Doc;
			int32 MaxDepth;
			TArray<FFrame> Frames;
			TArray<FMember> Members;
			TArray<FNode*> Elements;
			FNode* Result = nullptr;
			bool bDepthExceeded = false;

			FBuildHandler(FDocument& InDoc, int32 InMaxDepth)
				: Doc(InDoc)
				, MaxDepth(InMaxDepth)
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
				if (Frames.Num() >= MaxDepth)
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

		template <unsigned Flags>
		rapidjson::ParseResult Run(char* Text, FBuildHandler& Handler, int64& OutConsumed)
		{
			rapidjson::InsituStringStream Stream(Text);
			rapidjson::Reader Reader;
			const rapidjson::ParseResult Result = Reader.Parse<Flags>(Stream, Handler);
			OutConsumed = static_cast<int64>(Stream.Tell());
			return Result;
		}

		constexpr unsigned BaseFlags = rapidjson::kParseInsituFlag | rapidjson::kParseIterativeFlag
			| rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag;
		constexpr unsigned LenientFlags = rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag;
	}

	int64 BomLength(const uint8* Bytes, int64 Num)
	{
		return Num >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF ? 3 : 0;
	}

	bool ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options,
		FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed)
	{
		check(Doc.Input.Num() > 0 && Doc.Input.Last() == 0);
		FBuildHandler Handler(Doc, FMath::Max(1, Options.MaxDepth));
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
				? FString::Printf(TEXT("Nesting deeper than %d"), Options.MaxDepth)
				: FString(rapidjson::GetParseError_En(Result.Code()));
			if (!OriginalForDiagnostics.IsEmpty())
			{
				const int64 Limit = FMath::Min<int64>(OutError.Offset, OriginalForDiagnostics.Len());
				int32 Line = 1;
				int64 LineStart = 0;
				for (int64 i = 0; i < Limit; ++i)
				{
					if (OriginalForDiagnostics[i] == UTF8CHAR('\n'))
					{
						++Line;
						LineStart = i + 1;
					}
				}
				OutError.Line = Line;
				OutError.Column = static_cast<int32>(Limit - LineStart) + 1;
			}
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

Note on the multi-line test expectation (`line 3`, `column 8`): RapidJSON reports the offset of the offending byte (`x`), which is column 8 of `  "b": x`. If RapidJSON reports a different offset for this input, print it, check it against `rapidjson/reader.h`'s `ParseValue` error path, and update the expected column with a comment citing that line — do not offset-adjust in the reader.

- [ ] **Step 4: Add the parse overloads to `VaRestJsonText.cpp`**

Add `#include "VaRestJsonReader.h"` next to the other includes at the top of the file, then append:

```cpp

namespace
{
	/** First arena chunk sized from the input; the benchmark (Task 10) tunes this factor. */
	SIZE_T ArenaHintFor(int64 InputBytes)
	{
		return static_cast<SIZE_T>(FMath::Clamp<int64>(InputBytes * 2, 4096, 256ll * 1024 * 1024));
	}

	TValueOrError<FVaRestJson, FVaRestJsonError> ParseIntoDocument(TArray<uint8>&& Bytes, const FVaRestJsonParseOptions& Options, FUtf8StringView Original)
	{
		const int64 Bom = BomLength(Bytes.GetData(), Bytes.Num());
		const int64 InputBytes = Bytes.Num();
		TSharedPtr<FDocument, ESPMode::ThreadSafe> Doc = MakeShared<FDocument, ESPMode::ThreadSafe>(ArenaHintFor(InputBytes));
		Doc->Input = MoveTemp(Bytes);
		Doc->Input.Add(0);

		FVaRestJsonError Error;
		if (!ParseInSitu(*Doc, Bom, Options, Original, Error))
		{
			return MakeError(MoveTemp(Error));
		}
		FNode* Root = Doc->Root;
		return MakeValue(FVaRestJson::MakeInternal(MoveTemp(Doc), Root));
	}
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::ParseOwned(TArray<uint8>&& Utf8Bytes, const FVaRestJsonParseOptions& Options)
{
	return ParseIntoDocument(MoveTemp(Utf8Bytes), Options, FUtf8StringView());
}

TValueOrError<FVaRestJson, FVaRestJsonError> FVaRestJson::Parse(TConstArrayView<uint8> Utf8Bytes, const FVaRestJsonParseOptions& Options)
{
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
	const auto Utf8 = StringCast<UTF8CHAR>(Text.GetData(), Text.Len());
	return Parse(FUtf8StringView(Utf8.Get(), Utf8.Length()), Options);
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `23 passed, 0 failed`.

- [ ] **Step 6: Restore-the-bug**

In `FBuildHandler::Open`, delete the depth check (`if (Frames.Num() >= MaxDepth) {...}`). Run `Tools/run_tests.sh VaRest.Json.Parse.Errors` → expect FAIL on `depth 5 fails with a depth message` (the 100k-deep case still fails safely because the parse is iterative, which is itself worth noting in the report). Restore.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: in-situ RapidJSON SAX reader into the arena DOM

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Path access

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonPath.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonPathTest.cpp`

**Interfaces:**
- Consumes: `FVaRestJson::Get/At` (Task 4), `Parse` (Task 6).
- Produces: definition of `FVaRestJson FVaRestJson::Path(FStringView Expression) const`.

Grammar. **Dotted:** `path := segment ('.' segment)*`; `segment := name? ('[' digits ']')*`; `name` = one or more characters other than `.` `[` `]`. An empty name is allowed only for the first segment (root index, as in `[0].a`). An empty expression returns `*this`. **Pointer** (starts with `/`): split on `/`, unescape `~1`→`/` then `~0`→`~`. Array tokens must be `0` or `[1-9][0-9]*`. Syntax errors and misses return an invalid handle.

- [ ] **Step 1: Write the failing test**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonPathTest, "VaRest.Json.Path",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonPathTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Parse(FStringView(TEXT(
		"{\"user\":{\"items\":[{\"name\":\"a\"},{\"name\":\"b\"}],\"a.b\":1,\"x/y\":2,\"t~\":3,\"\":4},\"m\":[[1,2],[3]]}"))).StealValue();

	TestEqual(TEXT("dotted"), J.Path(TEXT("user.items[1].name")).AsString(), FString(TEXT("b")));
	TestEqual(TEXT("nested brackets"), J.Path(TEXT("m[0][1]")).AsInt(), 2);
	TestEqual(TEXT("root index"), FVaRestJson::Array(FVaRestJson::Object().Set(TEXT("q"), 9)).Path(TEXT("[0].q")).AsInt(), 9);
	TestTrue(TEXT("empty path is self"), J.Path(TEXT("")).IsSameNode(J));

	TestEqual(TEXT("pointer"), J.Path(TEXT("/user/items/0/name")).AsString(), FString(TEXT("a")));
	TestEqual(TEXT("pointer: key with a dot"), J.Path(TEXT("/user/a.b")).AsInt(), 1);
	TestEqual(TEXT("pointer: ~1 is /"), J.Path(TEXT("/user/x~1y")).AsInt(), 2);
	TestEqual(TEXT("pointer: ~0 is ~"), J.Path(TEXT("/user/t~0")).AsInt(), 3);
	TestEqual(TEXT("pointer: empty key"), J.Path(TEXT("/user/")).AsInt(), 4);

	TestFalse(TEXT("miss"), J.Path(TEXT("user.nope")).IsValid());
	TestFalse(TEXT("index out of range"), J.Path(TEXT("user.items[5]")).IsValid());
	TestFalse(TEXT("syntax: double dot"), J.Path(TEXT("user..items")).IsValid());
	TestFalse(TEXT("syntax: open bracket"), J.Path(TEXT("m[0")).IsValid());
	TestFalse(TEXT("syntax: non-digit index"), J.Path(TEXT("m[x]")).IsValid());
	TestFalse(TEXT("syntax: trailing dot"), J.Path(TEXT("user.")).IsValid());
	TestFalse(TEXT("pointer: leading zero index"), J.Path(TEXT("/m/01")).IsValid());
	TestFalse(TEXT("index on object"), J.Path(TEXT("user[0]")).IsValid());
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Path`
Expected: link error `undefined reference to FVaRestJson::Path`.

- [ ] **Step 3: Implement `VaRestJsonPath.cpp`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

namespace
{
	bool ParseIndex(FStringView Digits, int32& OutIndex)
	{
		if (Digits.IsEmpty() || Digits.Len() > 9 || (Digits.Len() > 1 && Digits[0] == TEXT('0')))
		{
			return false;
		}
		int32 Value = 0;
		for (TCHAR C : Digits)
		{
			if (C < TEXT('0') || C > TEXT('9'))
			{
				return false;
			}
			Value = Value * 10 + (C - TEXT('0'));
		}
		OutIndex = Value;
		return true;
	}

	FVaRestJson Step(const FVaRestJson& Current, FStringView Token, bool bPointer)
	{
		if (Current.IsArray())
		{
			int32 Index = 0;
			return ParseIndex(Token, Index) ? Current.At(Index) : FVaRestJson();
		}
		if (Current.IsObject())
		{
			if (!bPointer)
			{
				return Current.Get(Token);
			}
			// RFC 6901 §4: ~1 -> '/', then ~0 -> '~'
			FString Key(Token);
			Key.ReplaceInline(TEXT("~1"), TEXT("/"), ESearchCase::CaseSensitive);
			Key.ReplaceInline(TEXT("~0"), TEXT("~"), ESearchCase::CaseSensitive);
			return Current.Get(FStringView(Key));
		}
		return FVaRestJson();
	}

	FVaRestJson WalkPointer(const FVaRestJson& Root, FStringView Expression)
	{
		FVaRestJson Current = Root;
		int32 Start = 1; // skip the leading '/'
		while (Current.IsValid())
		{
			int32 End = Start;
			while (End < Expression.Len() && Expression[End] != TEXT('/'))
			{
				++End;
			}
			Current = Step(Current, Expression.Mid(Start, End - Start), true);
			if (End >= Expression.Len())
			{
				break;
			}
			Start = End + 1;
		}
		return Current;
	}

	FVaRestJson WalkDotted(const FVaRestJson& Root, FStringView Expression)
	{
		FVaRestJson Current = Root;
		int32 i = 0;
		bool bFirst = true;
		while (i < Expression.Len())
		{
			// name
			const int32 NameStart = i;
			while (i < Expression.Len() && Expression[i] != TEXT('.') && Expression[i] != TEXT('[') && Expression[i] != TEXT(']'))
			{
				++i;
			}
			const FStringView Name = Expression.Mid(NameStart, i - NameStart);
			const bool bHasBracket = i < Expression.Len() && Expression[i] == TEXT('[');
			if (Name.IsEmpty() && !(bFirst && bHasBracket))
			{
				return FVaRestJson();
			}
			if (!Name.IsEmpty())
			{
				if (!Current.IsObject())
				{
					return FVaRestJson();
				}
				Current = Current.Get(Name);
			}
			// [index]*
			while (i < Expression.Len() && Expression[i] == TEXT('['))
			{
				const int32 DigitsStart = ++i;
				while (i < Expression.Len() && Expression[i] != TEXT(']'))
				{
					++i;
				}
				int32 Index = 0;
				if (i >= Expression.Len() || !ParseIndex(Expression.Mid(DigitsStart, i - DigitsStart), Index) || !Current.IsArray())
				{
					return FVaRestJson();
				}
				Current = Current.At(Index);
				++i; // ']'
			}
			if (i < Expression.Len())
			{
				if (Expression[i] != TEXT('.') || i + 1 == Expression.Len())
				{
					return FVaRestJson(); // stray ']' or trailing '.'
				}
				++i;
			}
			bFirst = false;
			if (!Current.IsValid())
			{
				return FVaRestJson();
			}
		}
		return Current;
	}
}

FVaRestJson FVaRestJson::Path(FStringView Expression) const
{
	if (Expression.IsEmpty())
	{
		return *this;
	}
	return Expression[0] == TEXT('/') ? WalkPointer(*this, Expression) : WalkDotted(*this, Expression);
}
```

- [ ] **Step 4: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `24 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: dotted and RFC 6901 path access

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: `FJsonValue` / `FJsonObject` bridges

**Files:**
- Create: `Source/VaRestJson/Private/VaRestJsonBridge.cpp`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonBridgeTest.cpp`

**Interfaces:**
- Consumes: `FDocument` (Task 3), `FVaRestJson` (Task 4).
- Produces: definitions of `ToJsonValue`, `ToJsonObject`, `FromJsonValue`, `FromJsonObject`. The v1 shims (M6) depend on these.

UE 5.8 facts the code relies on: `FJsonObject::Values` keys are `FJsonObject::FStringType` (`UE::FSharedString` in this build, `JsonObject.h:99,237`); `*Key` yields `const TCHAR*` for both storage variants. `SetField(FStringView, ...)` exists in both variants (`JsonObject.h:131,257`). `FJsonValueNumberString` exists (`JsonValue.h:218-250`).

- [ ] **Step 1: Write the failing test**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonBridgeTest, "VaRest.Json.Bridge.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonBridgeTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Parse(FStringView(TEXT(
		"{\"s\":\"é\",\"i\":42,\"d\":1.5,\"b\":true,\"n\":null,\"a\":[1,{\"k\":\"v\"}],\"big\":9007199254740993,\"u\":18446744073709551615}"))).StealValue();

	const TSharedPtr<FJsonObject> O = J.ToJsonObject();
	TestTrue(TEXT("object"), O.IsValid());
	TestEqual(TEXT("string"), O->GetStringField(TEXT("s")), FString(TEXT("é")));
	TestEqual(TEXT("int as number"), O->GetNumberField(TEXT("i")), 42.0);
	TestEqual(TEXT("nested"), O->GetArrayField(TEXT("a"))[1]->AsObject()->GetStringField(TEXT("k")), FString(TEXT("v")));

	// 2^53 + 1 does not fit a double: it must travel as a number string
	int64 Big = 0;
	const TSharedPtr<FJsonValue> BigField = O->TryGetField(TEXT("big"));
	TestTrue(TEXT("big int exact"), BigField.IsValid() && BigField->TryGetNumber(Big) && Big == 9007199254740993ll);

	const FVaRestJson Back = FVaRestJson::FromJsonObject(O);
	TestTrue(TEXT("round trip equals"), Back.Equals(J));
	TestEqual(TEXT("big int back"), Back[TEXT("big")].AsInt64(), 9007199254740993ll);
	TestEqual(TEXT("uint64 back"), Back[TEXT("u")].AsUInt64(), MAX_uint64);
	TestEqual(TEXT("engine double 42 comes back as integer"), Back[TEXT("i")].ToString(), FString(TEXT("42")));

	// An engine-parsed tree converts too
	TSharedPtr<FJsonValue> Engine;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[1,\"x\",{\"z\":false}]")), Engine);
	const FVaRestJson FromEngine = FVaRestJson::FromJsonValue(Engine);
	TestEqual(TEXT("engine tree"), FromEngine.ToString(), FString(TEXT("[1,\"x\",{\"z\":false}]")));

	// Duplicates collapse to the last occurrence on the way to FJsonObject
	const FVaRestJson Dup = FVaRestJson::Parse(FStringView(TEXT("{\"a\":1,\"a\":2}"))).StealValue();
	TestEqual(TEXT("dup -> last"), Dup.ToJsonObject()->GetNumberField(TEXT("a")), 2.0);

	TestFalse(TEXT("ToJsonObject on array is null"), FromEngine.ToJsonObject().IsValid());
	TestFalse(TEXT("null shared ptr -> invalid"), FVaRestJson::FromJsonValue(nullptr).IsValid());
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.Bridge`
Expected: link error `undefined reference to FVaRestJson::ToJsonObject`.

- [ ] **Step 3: Implement `VaRestJsonBridge.cpp`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "VaRestJsonDocument.h"

using namespace VaRest::Json;

namespace
{
	constexpr int64 MaxExactDoubleInt = 9007199254740992ll; // 2^53

	FString Utf8ToString(const FStr& S)
	{
		return FString::ConstructFromPtrSize(S.Data, static_cast<int32>(S.Len));
	}

	TSharedPtr<FJsonValue> ToEngine(const FNode& Node)
	{
		switch (Node.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			return MakeShared<FJsonValueNull>();
		case EVaRestJsonType::Bool:
			return MakeShared<FJsonValueBoolean>(Node.Bool);
		case EVaRestJsonType::Number:
			switch (Node.NumKind)
			{
			case ENumKind::Int:
				// FJsonValueNumber holds a double; beyond 2^53 keep the exact digits instead
				if (Node.Int >= -MaxExactDoubleInt && Node.Int <= MaxExactDoubleInt)
				{
					return MakeShared<FJsonValueNumber>(static_cast<double>(Node.Int));
				}
				return MakeShared<FJsonValueNumberString>(LexToString(Node.Int));
			case ENumKind::UInt:
				return MakeShared<FJsonValueNumberString>(LexToString(Node.UInt));
			case ENumKind::Double:
				return MakeShared<FJsonValueNumber>(Node.Double);
			}
			break;
		case EVaRestJsonType::String:
			return MakeShared<FJsonValueString>(Utf8ToString(Node.Str));
		case EVaRestJsonType::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Node.Arr.Num);
			for (uint32 i = 0; i < Node.Arr.Num; ++i)
			{
				Items.Add(ToEngine(*Node.Arr.Items[i]));
			}
			return MakeShared<FJsonValueArray>(Items);
		}
		case EVaRestJsonType::Object:
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			for (uint32 i = 0; i < Node.Obj.Num; ++i)
			{
				const FMember& M = Node.Obj.Members[i];
				Object->SetField(FStringView(Utf8ToString(M.Key)), ToEngine(*M.Value)); // later duplicates overwrite
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		}
		return MakeShared<FJsonValueNull>();
	}

	/**
	 * Exact decimal integer parse with overflow detection. LexTryParseString is not used: it is not
	 * documented to reject out-of-range input, and an overflowed id would come back silently wrong.
	 */
	FNode* TryIntegerNode(FDocument& Doc, const FString& S)
	{
		const bool bNegative = S.StartsWith(TEXT("-"));
		int32 i = bNegative ? 1 : 0;
		if (i >= S.Len())
		{
			return nullptr;
		}
		uint64 Magnitude = 0;
		for (; i < S.Len(); ++i)
		{
			if (!FChar::IsDigit(S[i]))
			{
				return nullptr;
			}
			const uint64 Digit = static_cast<uint64>(S[i] - TEXT('0'));
			if (Magnitude > (MAX_uint64 - Digit) / 10)
			{
				return nullptr; // overflow: not representable as an integer, let it be a double
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		if (bNegative)
		{
			if (Magnitude > 9223372036854775808ull)
			{
				return nullptr;
			}
			return Doc.NewInt(Magnitude == 9223372036854775808ull ? MIN_int64 : -static_cast<int64>(Magnitude));
		}
		return Doc.NewUInt(Magnitude); // stored as Int when it fits int64
	}

	FNode* FromEngine(FDocument& Doc, const FJsonValue& Value)
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
			// FJsonValueNumberString carries exact digits; prefer them when they are an integer
			FString Digits;
			if (Value.TryGetString(Digits))
			{
				if (FNode* Integer = TryIntegerNode(Doc, Digits))
				{
					return Integer;
				}
			}
			const double D = Value.AsNumber();
			// The engine writes integral doubles without a fraction; keep them integers so text matches
			if (FMath::TruncToDouble(D) == D && D >= -9007199254740992.0 && D <= 9007199254740992.0)
			{
				return Doc.NewInt(static_cast<int64>(D));
			}
			return Doc.NewDouble(D);
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value.AsArray();
			FNode* Array = Doc.NewArray(Items.Num());
			for (const TSharedPtr<FJsonValue>& Item : Items)
			{
				Doc.Append(*Array, Item.IsValid() ? FromEngine(Doc, *Item) : Doc.NewNull());
			}
			return Array;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>& Object = Value.AsObject();
			FNode* Node = Doc.NewObject(Object.IsValid() ? Object->Values.Num() : 0);
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
				{
					const FString Key(*Pair.Key); // FSharedString or FString storage: both dereference to TCHAR*
					const auto Utf8 = StringCast<UTF8CHAR>(*Key, Key.Len());
					Doc.Set(*Node, FUtf8StringView(Utf8.Get(), Utf8.Length()), Pair.Value.IsValid() ? FromEngine(Doc, *Pair.Value) : Doc.NewNull());
				}
			}
			return Node;
		}
		}
		return Doc.NewNull();
	}
}

TSharedPtr<FJsonValue> FVaRestJson::ToJsonValue() const
{
	return Node ? ToEngine(*Node) : TSharedPtr<FJsonValue>();
}

TSharedPtr<FJsonObject> FVaRestJson::ToJsonObject() const
{
	return IsObject() ? ToEngine(*Node)->AsObject() : TSharedPtr<FJsonObject>();
}

FVaRestJson FVaRestJson::FromJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->Type == EJson::None)
	{
		return FVaRestJson();
	}
	FDocumentRef NewDoc = NewDocument();
	FNode* Root = FromEngine(*NewDoc, *Value);
	NewDoc->Root = Root;
	return FVaRestJson(MoveTemp(NewDoc), Root);
}

FVaRestJson FVaRestJson::FromJsonObject(const TSharedPtr<FJsonObject>& Object)
{
	return Object.IsValid() ? FromJsonValue(MakeShared<FJsonValueObject>(Object)) : FVaRestJson();
}
```

- [ ] **Step 4: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `25 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: FJsonValue/FJsonObject bridges that keep 64-bit integers exact

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Struct ops — text import/export, identity, serialization

**Files:**
- Modify: `Source/VaRestJson/Public/VaRestJson.h` (declarations + `TStructOpsTypeTraits`)
- Create: `Source/VaRestJson/Private/VaRestJsonStructOps.cpp`, `Source/VaRestJson/Private/Tests/VaRestJsonTestTypes.h`
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonStructOpsTest.cpp`

**Interfaces:**
- Consumes: `ParseInSitu` with `OutConsumed` (Task 6), `ToString` (Task 5), `Equals` (Task 4).
- Produces: `bool ExportTextItem(FString&, const FVaRestJson&, UObject*, int32, UObject*) const`, `bool ImportTextItem(const TCHAR*&, int32, UObject*, FOutputDevice*)`, `bool Identical(const FVaRestJson*, uint32) const`, `bool Serialize(FArchive&)`, and the trait that enables them.

- [ ] **Step 1: Test type (header processed by UHT)**

`Source/VaRestJson/Private/Tests/VaRestJsonTestTypes.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

#include "VaRestJsonTestTypes.generated.h"

/** Test-only: a handle as a field between two others, to prove ImportTextItem consumes exactly its value */
USTRUCT()
struct FVaRestJsonTestHolder
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Before = 0;

	UPROPERTY()
	FVaRestJson Json;

	UPROPERTY()
	int32 After = 0;
};
```

- [ ] **Step 2: Write the failing test**

`Source/VaRestJson/Private/Tests/VaRestJsonStructOpsTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Tests/VaRestJsonTestTypes.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsTextTest, "VaRest.Json.StructOps.Text",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsTextTest::RunTest(const FString& Parameters)
{
	UScriptStruct* HolderStruct = FVaRestJsonTestHolder::StaticStruct();

	FVaRestJsonTestHolder Source;
	Source.Before = 3;
	Source.Json = FVaRestJson::Object().Set(TEXT("tricky"), TEXT("a),b,\"c\"(")).Set(TEXT("n"), FVaRestJson::Array(1, 2));
	Source.After = 7;

	FString Text;
	HolderStruct->ExportText(Text, &Source, nullptr, nullptr, PPF_None, nullptr);

	FVaRestJsonTestHolder Imported;
	HolderStruct->ImportText(*Text, &Imported, nullptr, PPF_None, GLog, HolderStruct->GetName());
	TestEqual(TEXT("field before"), Imported.Before, 3);
	TestTrue(TEXT("json survived delimiters inside strings"), Imported.Json.Equals(Source.Json));
	TestEqual(TEXT("field after was not swallowed"), Imported.After, 7);

	// An invalid handle exports as nothing and imports back as invalid
	FVaRestJsonTestHolder Empty;
	Empty.After = 5;
	FString EmptyText;
	HolderStruct->ExportText(EmptyText, &Empty, nullptr, nullptr, PPF_None, nullptr);
	FVaRestJsonTestHolder EmptyBack;
	EmptyBack.Json = FVaRestJson::Object();
	HolderStruct->ImportText(*EmptyText, &EmptyBack, nullptr, PPF_None, GLog, HolderStruct->GetName());
	// If UScriptStruct::ExportText omits a field whose text is empty, the import never touches it and
	// this check fails for a reason that is not ours: inspect EmptyText, and if "Json=" is absent,
	// assert on that instead and say so in the task report.
	TestFalse(TEXT("invalid round trip"), EmptyBack.Json.IsValid());
	TestEqual(TEXT("after the empty field"), EmptyBack.After, 5);

	// A pin default typed by hand
	FVaRestJson Pin;
	const TCHAR* Buffer = TEXT("  [1, \"x\"] trailing");
	TestTrue(TEXT("ImportTextItem"), Pin.ImportTextItem(Buffer, PPF_None, nullptr, GLog));
	TestEqual(TEXT("value"), Pin.ToString(), FString(TEXT("[1,\"x\"]")));
	TestEqual(TEXT("buffer advanced past the value only"), FString(Buffer), FString(TEXT(" trailing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsIdentitySerializeTest, "VaRest.Json.StructOps.IdenticalAndSerialize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsIdentitySerializeTest::RunTest(const FString& Parameters)
{
	UScriptStruct* JsonStruct = FVaRestJson::StaticStruct();
	TestTrue(TEXT("has serializer"), JsonStruct->GetCppStructOps()->HasSerializer());

	const FVaRestJson A = FVaRestJson::Object().Set(TEXT("x"), 1);
	const FVaRestJson B = FVaRestJson::Object().Set(TEXT("x"), 1.0);
	TestTrue(TEXT("Identical is deep"), JsonStruct->CompareScriptStruct(&A, &B, PPF_None));
	const FVaRestJson C = FVaRestJson::Object().Set(TEXT("x"), 2);
	TestFalse(TEXT("different"), JsonStruct->CompareScriptStruct(&A, &C, PPF_None));

	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FVaRestJson ToSave = FVaRestJson::Object().Set(TEXT("s"), TEXT("ж")).Set(TEXT("u"), MAX_uint64);
	FVaRestJson Invalid;
	ToSave.Serialize(Writer);
	Invalid.Serialize(Writer);

	FMemoryReader Reader(Bytes);
	FVaRestJson Loaded;
	FVaRestJson LoadedInvalid = FVaRestJson::Object();
	Loaded.Serialize(Reader);
	LoadedInvalid.Serialize(Reader);
	TestTrue(TEXT("serialize round trip"), Loaded.Equals(ToSave));
	TestFalse(TEXT("invalid stays invalid"), LoadedInvalid.IsValid());
	TestFalse(TEXT("reader consumed exactly"), Reader.IsError());
	return true;
}

#endif
```

- [ ] **Step 3: Run to verify it fails**

Run: `Tools/run_tests.sh VaRest.Json.StructOps`
Expected: build fails (`no member named 'ImportTextItem' in 'FVaRestJson'`).

- [ ] **Step 4: Declarations and trait in `VaRestJson.h`**

Inside `FVaRestJson`, after the engine-interop block:

```cpp
	// ---- struct ops (TStructOpsTypeTraits below): pin defaults, copy/paste, SaveGame, CDO diffs

	bool ExportTextItem(FString& ValueStr, const FVaRestJson& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const;
	bool ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText);
	bool Identical(const FVaRestJson* Other, uint32 PortFlags) const;
	bool Serialize(FArchive& Ar);
```

After the `FVaRestJson` struct (before `FVaRestJsonMember`):

```cpp
template <>
struct TStructOpsTypeTraits<FVaRestJson> : public TStructOpsTypeTraitsBase2<FVaRestJson>
{
	enum
	{
		WithExportTextItem = true,
		WithImportTextItem = true,
		WithIdentical = true,
		WithSerializer = true,
	};
};
```

- [ ] **Step 5: Implement `VaRestJsonStructOps.cpp`**

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJson.h"

#include "Misc/OutputDevice.h"
#include "Serialization/Archive.h"
#include "VaRestJsonDocument.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonReader.h"

using namespace VaRest::Json;

namespace
{
	constexpr uint8 SerializeVersion = 1;
}

bool FVaRestJson::ExportTextItem(FString& ValueStr, const FVaRestJson& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const
{
	// An invalid handle exports as nothing; ImportTextItem reads "nothing" back as invalid.
	ValueStr += ToString(EVaRestJsonFormat::Condensed);
	return true;
}

bool FVaRestJson::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText)
{
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
	// (kParseStopWhenDoneFlag), and the parent's parser continues from there.
	const auto Utf8 = StringCast<UTF8CHAR>(Cursor);
	TSharedPtr<FDocument, ESPMode::ThreadSafe> NewDoc = MakeShared<FDocument, ESPMode::ThreadSafe>();
	NewDoc->Input.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	NewDoc->Input.Add(0);

	FVaRestJsonParseOptions Options;
	FVaRestJsonError Error;
	int64 Consumed = 0;
	if (!ParseInSitu(*NewDoc, 0, Options, FUtf8StringView(Utf8.Get(), Utf8.Length()), Error, &Consumed))
	{
		if (ErrorText)
		{
			ErrorText->Logf(ELogVerbosity::Warning, TEXT("FVaRestJson::ImportTextItem: %s"), *Error.ToString());
		}
		return false;
	}

	// Map consumed UTF-8 bytes back to TCHARs of the original buffer
	const auto Back = StringCast<TCHAR>(Utf8.Get(), static_cast<int32>(Consumed));
	Buffer = Cursor + Back.Length();

	FNode* Root = NewDoc->Root;
	*this = FVaRestJson(MoveTemp(NewDoc), Root);
	return true;
}

bool FVaRestJson::Identical(const FVaRestJson* Other, uint32 PortFlags) const
{
	return Other ? Equals(*Other) : !IsValid();
}

bool FVaRestJson::Serialize(FArchive& Ar)
{
	uint8 Version = SerializeVersion;
	Ar << Version;
	bool bValid = IsValid();
	Ar << bValid;

	if (Ar.IsLoading())
	{
		*this = FVaRestJson();
		if (bValid)
		{
			TArray<uint8> Bytes;
			Ar << Bytes;
			TValueOrError<FVaRestJson, FVaRestJsonError> Result = ParseOwned(MoveTemp(Bytes));
			if (Result.HasValue())
			{
				*this = Result.StealValue();
			}
			else
			{
				UE_LOG(LogVaRestJson, Error, TEXT("FVaRestJson::Serialize: stored JSON does not parse: %s"), *Result.GetError().ToString());
			}
		}
	}
	else if (bValid)
	{
		TArray<uint8> Bytes;
		ToUtf8(Bytes);
		Ar << Bytes;
	}
	return true;
}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `27 passed, 0 failed`.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestJson
git commit -m "json: struct ops -- text import/export, deep Identical, versioned Serialize

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Benchmark and the spec §7.3 gate

**Files:**
- Create: `Tools/gen_bench_json.py`, `Source/VaRestJson/Private/Tests/VaRestJsonBenchTest.cpp`, `docs/passport/2026-10-19-json-bench.md`
- Modify: `Tools/run_tests.sh` (extra command-line arguments), `docs/superpowers/specs/2026-10-18-varest2-core-design.md` §7.3 (record the decision)

**Interfaces:**
- Consumes: everything above.
- Produces: the gate decision, recorded in the spec and the passport.

- [ ] **Step 1: Fixture generator**

`Tools/gen_bench_json.py`:

```python
#!/usr/bin/env python3
"""Deterministic benchmark fixtures shaped like a game backend's list endpoint.

    python3 Tools/gen_bench_json.py <out_dir>

Writes bench_1mb.json and bench_10mb.json (same seed every run, so numbers are comparable).
"""
import json
import random
import sys
from pathlib import Path

WORDS = ["sword", "shield", "potion", "Ключ", "épée", "盾", "arrow", "helm", "ring", "boots"]


def item(rng, i):
    return {
        "id": i,
        "uid": 9007199254740993 + i,          # beyond 2^53: must stay exact
        "name": f"{rng.choice(WORDS)} #{i}",
        "desc": "line one\nline \"two\"\t" + rng.choice(WORDS) * rng.randint(1, 4),
        "price": round(rng.uniform(0.01, 9999.99), 2),
        "weight": rng.random(),
        "active": rng.random() > 0.3,
        "owner": None if rng.random() > 0.8 else {"id": rng.randint(1, 10**6), "nick": rng.choice(WORDS)},
        "tags": rng.sample(WORDS, rng.randint(0, 4)),
        "stats": {k: rng.randint(-100, 100) for k in ("str", "dex", "int", "luck")},
    }


def build(target_bytes, seed):
    rng = random.Random(seed)
    items, size, i = [], 0, 0
    while size < target_bytes:
        it = item(rng, i)
        items.append(it)
        size += len(json.dumps(it, ensure_ascii=False).encode("utf-8")) + 1
        i += 1
    return {"ok": True, "data": {"items": items, "count": len(items)}}


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, size in (("bench_1mb.json", 1 << 20), ("bench_10mb.json", 10 << 20)):
        text = json.dumps(build(size, 20261019), ensure_ascii=False, separators=(",", ":"))
        (out / name).write_text(text, encoding="utf-8")
        print(f"{name}: {len(text.encode('utf-8'))} bytes")


if __name__ == "__main__":
    main()
```

Run: `python3 Tools/gen_bench_json.py /w/Unreal/VaRestHost/Saved/VaRestBench`
Expected: two lines, `bench_1mb.json: ~1048xxx bytes` and `bench_10mb.json: ~1048xxxx bytes`.

- [ ] **Step 2: Let the runner pass extra arguments (for `-llm`)**

In `Tools/run_tests.sh`, change the editor invocation to append `${VAREST_EXTRA_ARGS:-}`:

```bash
"$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
	-ExecCmds="Automation RunTests $FILTER; Quit" \
	-unattended -nullrhi -nosplash -nopause -abslog="$LOG" ${VAREST_EXTRA_ARGS:-} >/dev/null 2>&1 || true
```

and add to the header comment: `# VAREST_EXTRA_ARGS is appended to the editor command line (e.g. -llm for the benchmarks).`

- [ ] **Step 3: Benchmark test**

`Source/VaRestJson/Private/Tests/VaRestJsonBenchTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "VaRestJson.h"

namespace VaRestJsonBench
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

	/**
	 * Bytes still allocated under Tag after Body returns, while Keep (filled by Body) is alive.
	 * -1 when LLM is off: run with VAREST_EXTRA_ARGS=-llm. LLM aggregates per frame, so stats are
	 * flushed by hand around the measurement (LowLevelMemTracker.h:611).
	 */
	template <typename BodyType>
	int64 RetainedBytes(const TCHAR* Tag, BodyType&& Body)
	{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		if (!FLowLevelMemTracker::IsEnabled())
		{
			return -1;
		}
		const FName TagName(Tag);
		FLowLevelMemTracker& Llm = FLowLevelMemTracker::Get();
		Llm.UpdateStatsPerFrame();
		const int64 Before = Llm.GetTagAmountForTracker(ELLMTracker::Default, TagName, ELLMTagSet::None);
		{
			FLLMScope Scope(TagName, false, ELLMTagSet::None, ELLMTracker::Default);
			Body();
		}
		Llm.UpdateStatsPerFrame();
		return Llm.GetTagAmountForTracker(ELLMTracker::Default, TagName, ELLMTagSet::None) - Before;
#else
		return -1;
#endif
	}

	FString Mb(int64 Bytes)
	{
		return Bytes < 0 ? FString(TEXT("n/a")) : FString::Printf(TEXT("%.1f MiB"), Bytes / (1024.0 * 1024.0));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonBenchTest, "Bench.VaRest.Json.ParseAndWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FVaRestJsonBenchTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonBench;

	// Calibration: the memory instrument must see a known 8 MiB allocation, or its numbers mean nothing
	TArray<uint8> Calibration;
	const int64 Calibrated = RetainedBytes(TEXT("VaRestBench.Calibration"), [&Calibration] { Calibration.SetNumUninitialized(8 << 20); });
	if (Calibrated >= 0)
	{
		TestTrue(TEXT("LLM sees 8 MiB within 5%"), FMath::Abs(Calibrated - (8 << 20)) < (8 << 20) / 20);
	}

	FString Report = TEXT("| fixture | path | parse ms (median) | retained memory | write ms |\n|---|---|---|---|---|\n");
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

		// Engine, the v1 way: UTF-8 -> FString first (VaRestRequestJSON.cpp used GetContentAsString)
		TSharedPtr<FJsonValue> EngineValue;
		const double EngineStringMs = MedianMs(Runs, [&] {
			FString Text = FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len());
			TSharedPtr<FJsonValue> V;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), V);
		});
		const int64 EngineMem = RetainedBytes(TEXT("VaRestBench.Engine"), [&] {
			FString Text = FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len());
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), EngineValue);
		});

		// Engine reading UTF-8 directly (the fairest engine baseline)
		const double EngineUtf8Ms = MedianMs(Runs, [&] {
			TSharedPtr<FJsonValue> V;
			FJsonSerializer::Deserialize(TJsonReaderFactory<UTF8CHAR>::CreateFromView(Utf8), V);
		});

		// VaRest: copy + in situ, and stolen buffer
		const double OursViewMs = MedianMs(Runs, [&] { auto R = FVaRestJson::Parse(Utf8); (void)R; });
		const double OursOwnedMs = MedianMs(Runs, [&] {
			TArray<uint8> Copy = Bytes; // the copy is outside what an HTTP response would pay; subtract below
			auto R = FVaRestJson::ParseOwned(MoveTemp(Copy));
			(void)R;
		});
		const double CopyMs = MedianMs(Runs, [&] { TArray<uint8> Copy = Bytes; });
		FVaRestJson Ours;
		const int64 OursMem = RetainedBytes(TEXT("VaRestBench.VaRest"), [&] { Ours = FVaRestJson::Parse(Utf8).StealValue(); });

		// Writing
		const double EngineWriteMs = MedianMs(Runs, [&] {
			FString Out;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(EngineValue, FString(), W);
		});
		const double OursWriteMs = MedianMs(Runs, [&] { TArray<uint8> Out; Ours.ToUtf8(Out); });

		// Item counts, not full equality: the engine reader may round integers beyond 2^53
		TestEqual(TEXT("same item count"), FVaRestJson::FromJsonValue(EngineValue).Path(TEXT("data.items")).Num(), Ours.Path(TEXT("data.items")).Num());

		Report += FString::Printf(TEXT("| %s | engine (FString, v1 path) | %.2f | %s | %.2f |\n"), Name, EngineStringMs, *Mb(EngineMem), EngineWriteMs);
		Report += FString::Printf(TEXT("| %s | engine (UTF-8 reader) | %.2f | | |\n"), Name, EngineUtf8Ms);
		Report += FString::Printf(TEXT("| %s | VaRest Parse (copy + in situ) | %.2f | %s | %.2f |\n"), Name, OursViewMs, *Mb(OursMem), OursWriteMs);
		Report += FString::Printf(TEXT("| %s | VaRest ParseOwned (minus the test's copy) | %.2f | | |\n"), Name, OursOwnedMs - CopyMs);
	}

	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("results.md")));
	return true;
}

#endif
```

- [ ] **Step 4: Run the benchmark (Development editor, LLM on)**

Run: `VAREST_EXTRA_ARGS=-llm Tools/run_tests.sh Bench.VaRest`
Expected: `1 passed, 0 failed`; `/w/Unreal/VaRestHost/Saved/VaRestBench/results.md` exists. If the calibration check fails, the memory column is invalid: report it and fix the instrument (e.g. the flush call), never the threshold.

- [ ] **Step 5: Tune the arena hint, measured**

Run the benchmark with `ArenaHintFor` factor `* 2` (current), `* 1`, and `* 4` (edit `VaRestJsonText.cpp`, rebuild, rerun). Keep the factor with the lowest retained memory whose parse time is within 5% of the best. Record all three rows in the passport.

- [ ] **Step 6: Decide the gate and record it**

Gate (spec §7.3): pass if, on `bench_10mb.json`, VaRest is ≥2× faster to parse than **engine (FString, v1 path)** **or** retains ≥2× less memory, and is not worse on the other.

Write `docs/passport/2026-10-19-json-bench.md`:
- machine (CPU model from `lscpu`, RAM), engine version, build configuration (`Development` editor), command used;
- the results table verbatim, the arena-hint tuning rows, and the calibration result;
- **Decision:** `PASS — VaRest arena DOM stays` or `FAIL — fall back to FJsonValue-backed FVaRestJson` with the two numbers that decided it.

Append to the spec's §7.3 one line: `**Gate result (2026-10-19):** <PASS|FAIL> — see docs/passport/2026-10-19-json-bench.md.`

**If the gate fails: stop after committing the passport and report to the controller.** The fallback is a separate plan.

- [ ] **Step 7: Run the whole suite once more**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `27 passed, 0 failed` (benchmarks excluded by the prefix filter).

- [ ] **Step 8: Commit**

```bash
git add Tools/gen_bench_json.py Tools/run_tests.sh Source/VaRestJson docs/passport docs/superpowers/specs/2026-10-18-varest2-core-design.md
git commit -m "json: benchmark against FJsonSerializer and the spec 7.3 gate decision

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## After M1

- Monolithic build check: `/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject` must succeed (catches missing includes hidden by the editor's shared PCH).
- Next plan: M2 (struct ↔ JSON, BP JSON library, wildcard nodes, `Make Json`), written against the code this plan produced.
