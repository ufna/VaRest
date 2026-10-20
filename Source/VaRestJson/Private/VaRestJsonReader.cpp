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
