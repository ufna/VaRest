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
