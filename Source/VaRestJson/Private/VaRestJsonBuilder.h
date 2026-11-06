// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "RapidJsonIncludes.h"
#include "VaRestJsonBudget.h"
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
	FJsonBudget* Budget;
	bool bMeasureScratch;
	SIZE_T ScratchPeakAllocatedBytes = 0;

	FDomBuilder(FDocument& InDoc, int32 InDepthLimit, FJsonBudget* InBudget = nullptr, bool bInMeasureScratch = false)
		: Doc(InDoc)
		, DepthLimit(InDepthLimit)
		, Budget(InBudget)
		, bMeasureScratch(bInMeasureScratch)
	{
	}

	bool Allow(int64 Values, int64 Work = 1) { return !Budget || Budget->Charge(0, Values, Work); }

	void ObserveScratch()
	{
		if (bMeasureScratch)
			ScratchPeakAllocatedBytes = FMath::Max(ScratchPeakAllocatedBytes,
				Frames.GetAllocatedSize() + Members.GetAllocatedSize() + Elements.GetAllocatedSize());
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
		ObserveScratch();
		return true;
	}

	bool Open(bool bObject)
	{
		if (Frames.Num() >= DepthLimit)
		{
			bDepthExceeded = true;
			return false; // RapidJSON stops with kParseErrorTermination
		}
		if (!Allow(1))
			return false;
		FNode* Container = bObject ? Doc.NewObject() : Doc.NewArray();
		Frames.Add(FFrame{Container, bObject ? Members.Num() : Elements.Num()});
		ObserveScratch();
		return true;
	}

	static const UTF8CHAR* U(const char* S) { return reinterpret_cast<const UTF8CHAR*>(S); }

	bool Null() { return Allow(1) && Emit(Doc.NewNull()); }
	bool Bool(bool b) { return Allow(1) && Emit(Doc.NewBool(b)); }
	bool Int(int i) { return Allow(1) && Emit(Doc.NewInt(i)); }
	bool Uint(unsigned u) { return Allow(1) && Emit(Doc.NewInt(static_cast<int64>(u))); }
	bool Int64(int64_t i) { return Allow(1) && Emit(Doc.NewInt(i)); }
	bool Uint64(uint64_t u) { return Allow(1) && Emit(Doc.NewUInt(u)); }
	bool Double(double d) { return Allow(1) && Emit(Doc.NewDouble(d)); }
	bool RawNumber(const char*, rapidjson::SizeType, bool) { return false; } // kParseNumbersAsStringsFlag is never set

	bool String(const char* S, rapidjson::SizeType Len, bool bCopy)
	{
		return Allow(1) && Emit(bCopy ? Doc.NewString(FUtf8StringView(U(S), Len)) : Doc.NewStringRef(U(S), Len));
	}

	bool Key(const char* S, rapidjson::SizeType Len, bool bCopy)
	{
		if (!Allow(1))
			return false;
		Members.Add(FMember{bCopy ? Doc.CopyStr(FUtf8StringView(U(S), Len), true) : FDocument::RefStr(U(S), Len, true), nullptr});
		ObserveScratch();
		return true;
	}

	bool StartObject() { return Open(true); }
	bool StartArray() { return Open(false); }

	bool EndObject(rapidjson::SizeType)
	{
		if (!Allow(0))
			return false;
		const FFrame Frame = Frames.Pop(EAllowShrinking::No);
		Doc.AssignObject(*Frame.Container, TConstArrayView<FMember>(Members.GetData() + Frame.Start, Members.Num() - Frame.Start));
		Members.SetNum(Frame.Start, EAllowShrinking::No);
		return Emit(Frame.Container);
	}

	bool EndArray(rapidjson::SizeType)
	{
		if (!Allow(0))
			return false;
		const FFrame Frame = Frames.Pop(EAllowShrinking::No);
		Doc.AssignArray(*Frame.Container, TConstArrayView<FNode*>(Elements.GetData() + Frame.Start, Elements.Num() - Frame.Start));
		Elements.SetNum(Frame.Start, EAllowShrinking::No);
		return Emit(Frame.Container);
	}
};
} // namespace VaRest::Json
