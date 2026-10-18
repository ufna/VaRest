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
