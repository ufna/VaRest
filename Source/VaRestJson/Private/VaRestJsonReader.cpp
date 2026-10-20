// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonReader.h"

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"

#include <cstdint>

namespace VaRest::Json
{
	namespace ReaderPrivate
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

		/**
		 * The reader's parse stack goes through FMemory (LLM, Insights and the engine allocator see it) via the
		 * engine's own RapidJSON allocator, UE::Json::Private::FAllocatorImpl (JsonUtils/RapidJsonUtils.h:19-53).
		 * The default rapidjson::CrtAllocator calls RAPIDJSON_MALLOC (allocators.h:86-88), which is std::malloc
		 * (rapidjson.h:694-696) because RapidJsonIncludes.h:29-44 keeps its FMemory override disabled.
		 * The allocator is the reader's StackAllocator argument (reader.h:538, :547).
		 */
		using FReader = rapidjson::GenericReader<rapidjson::UTF8<>, rapidjson::UTF8<>, UE::Json::Private::FAllocatorImpl>;

		template <unsigned Flags>
		rapidjson::ParseResult Run(char* Text, FBuildHandler& Handler, int64& OutConsumed)
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
		const int64 Limit = FMath::Min<int64>(Error.Offset, Original.Len());
		int32 Line = 1;
		int64 LineStart = 0;
		for (int64 i = 0; i < Limit; ++i)
		{
			if (Original[i] == UTF8CHAR('\n'))
			{
				++Line;
				LineStart = i + 1;
			}
		}
		Error.Line = Line;
		Error.Column = static_cast<int32>(Limit - LineStart) + 1;
	}

	bool ParseInSitu(FDocument& Doc, int64 StartOffset, const FVaRestJsonParseOptions& Options,
		FUtf8StringView OriginalForDiagnostics, FVaRestJsonError& OutError, int64* OutConsumed)
	{
		using namespace ReaderPrivate;
		check(Doc.Input.Num() > 0 && Doc.Input.Last() == 0);
		const int32 MaxDepth = FMath::Max(1, Options.MaxDepth);
		FBuildHandler Handler(Doc, MaxDepth);
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
				? FString::Printf(TEXT("Nesting deeper than %d"), MaxDepth)
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
