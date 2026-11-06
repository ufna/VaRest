// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonRedact.h"

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "String/Find.h"
#include "VaRestJsonLog.h"
#include "VaRestJsonUtf8.h"
#include "VaRestJsonWriterInternal.h"

namespace VaRest::Json::RedactPrivate
{
	using FKeyBuffer = TArray<UTF8CHAR, TInlineAllocator<64>>;

	/** Inspector spec 2.2 layer 1: a normalised key containing one of these is a secret */
	const UTF8CHAR* const Substrings[] = { UTF8TEXT("password"), UTF8TEXT("passwd"), UTF8TEXT("secret"), UTF8TEXT("token"),
		UTF8TEXT("apikey"), UTF8TEXT("authorization"), UTF8TEXT("credential"), UTF8TEXT("signature"), UTF8TEXT("session"),
		UTF8TEXT("cookie"), UTF8TEXT("privatekey"), UTF8TEXT("jwt") };

	/** ... unless it is exactly one of these */
	const UTF8CHAR* const Allowed[] = { UTF8TEXT("idempotencykey"), UTF8TEXT("tokentype") };

	bool IsExactly(FUtf8StringView Key, const UTF8CHAR* Name)
	{
		return Key.Equals(FUtf8StringView(Name), ESearchCase::CaseSensitive);
	}

	/**
	 * Same flags as the parser's (VaRestJsonReader.cpp) minus in situ -- Prefix is a view we must not modify -- plus
	 * kParseNumbersAsStringsFlag, so a number is re-written exactly as it was sent. Iterative: hostile depth cannot
	 * overflow the stack. The reader's stack goes through FMemory (JsonUtils/RapidJsonUtils.h:19-53).
	 */
	using FReader = rapidjson::GenericReader<rapidjson::UTF8<>, rapidjson::UTF8<>, UE::Json::Private::FAllocatorImpl>;
	constexpr unsigned Flags = rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag;
	static_assert((Flags & rapidjson::kParseIterativeFlag) != 0, "The reader must stay iterative: recursive parsing overflows the stack on hostile depth");

	/** SAX handler that mirrors each event into Writer, except a secret's value, which becomes "***" */
	template <typename WriterType>
	class TRedactHandler
	{
	public:
		TRedactHandler(WriterType& InWriter, const FVaRestRedactRules& InRules, const rapidjson::MemoryStream& InStream, bool bInCutAtEnd)
			: Writer(InWriter)
			, Rules(InRules)
			, Stream(InStream)
			, bCutAtEnd(bInCutAtEnd)
		{
		}

		bool Null() { return Scalar([this] { Writer.Null(); }); }
		bool Bool(bool bValue) { return Scalar([this, bValue] { Writer.Bool(bValue); }); }
		// kParseNumbersAsStringsFlag sends every number to RawNumber; these complete the handler concept (reader.h:169-187)
		bool Int(int Value) { return Scalar([this, Value] { Writer.Int(Value); }); }
		bool Uint(unsigned Value) { return Scalar([this, Value] { Writer.Uint(Value); }); }
		bool Int64(int64_t Value) { return Scalar([this, Value] { Writer.Int64(Value); }); }
		bool Uint64(uint64_t Value) { return Scalar([this, Value] { Writer.Uint64(Value); }); }
		bool Double(double Value) { return Scalar([this, Value] { Writer.Double(Value); }); }

		bool RawNumber(const char* Text, rapidjson::SizeType Length, bool)
		{
			// The number's digits are consumed when this fires, so Tell() is where it ends. Ending exactly where a cut input
			// ends, it may be the front of a longer number ("12" of "1234"): never written (Inspector spec 2.2).
			if (bCutAtEnd && Stream.Tell() == Stream.size_)
			{
				bStoppedAtCut = true;
				return false;
			}
			// RawValue, not Writer::RawNumber: that one quotes its text (writer.h:197-202)
			return Scalar([this, Text, Length] { Writer.RawValue(Text, Length, rapidjson::kNumberType); });
		}

		bool String(const char* Text, rapidjson::SizeType Length, bool)
		{
			return Scalar([this, Text, Length] { Writer.String(Text, Length); });
		}

		bool Key(const char* Text, rapidjson::SizeType Length, bool)
		{
			if (SkipLevel != INDEX_NONE)
			{
				return true;
			}
			Writer.Key(Text, Length);
			if (Rules.ShouldRedact(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text), Length), EVaRestRedactKeyKind::Json))
			{
				// Written before the value is read, so a value of any type, complete or cut, never shows (Inspector spec 2.2)
				Writer.String("***", 3);
				bRedacted = true;
				bSkipValue = true;
			}
			else
			{
				bValuePending = true;
				// The reader calls Key when the key string closes, before it reads the colon (reader.h:2045-2055)
				KeyEnd = Stream.Tell();
			}
			return true;
		}

		bool StartObject() { return Open([this] { Writer.StartObject(); }); }
		bool StartArray() { return Open([this] { Writer.StartArray(); }); }
		bool EndObject(rapidjson::SizeType) { return Close([this] { Writer.EndObject(); }); }
		bool EndArray(rapidjson::SizeType) { return Close([this] { Writer.EndArray(); }); }

		bool bRedacted = false;
		bool bStoppedAtCut = false;
		bool bTooDeep = false;
		/** A key was written and its value has not been (RapidJSON's writer emits the ':' only with the value) */
		bool bValuePending = false;
		/** Where the pending key ended in the input: its colon, if the input has one, is after this */
		size_t KeyEnd = 0;

	private:
		template <typename FuncType>
		bool Scalar(FuncType&& Write)
		{
			if (SkipLevel != INDEX_NONE)
			{
				return true;
			}
			if (bSkipValue)
			{
				bSkipValue = false;
				return true;
			}
			bValuePending = false;
			Write();
			return true;
		}

		template <typename FuncType>
		bool Open(FuncType&& Write)
		{
			if (Depth >= VaRest::Json::MaxDepth)
			{
				bTooDeep = true;
				return false;
			}
			++Depth;
			if (SkipLevel != INDEX_NONE)
			{
				return true;
			}
			if (bSkipValue)
			{
				// A secret's value is a container: skip everything until it closes
				bSkipValue = false;
				SkipLevel = Depth;
				return true;
			}
			bValuePending = false;
			Write();
			return true;
		}

		template <typename FuncType>
		bool Close(FuncType&& Write)
		{
			const int32 Closing = Depth--;
			if (SkipLevel != INDEX_NONE)
			{
				if (Closing == SkipLevel)
				{
					SkipLevel = INDEX_NONE;
				}
				return true;
			}
			Write();
			return true;
		}

		WriterType& Writer;
		const FVaRestRedactRules& Rules;
		const rapidjson::MemoryStream& Stream;
		const bool bCutAtEnd;
		int32 Depth = 0;
		/** Depth of the redacted container being skipped, or INDEX_NONE */
		int32 SkipLevel = INDEX_NONE;
		/** The next value belongs to a redacted key */
		bool bSkipValue = false;
	};

	/**
	 * The first of '?', ';', '#', '=' and '&' in Key, else 0. URL and form redaction start a candidate key after '?', ';'
	 * and '#' and end it at '=' or '&'; a key holding one of these could never be matched there, and the linear candidate
	 * scan (VaRestRedactor.cpp) relies on exact keys being free of them.
	 */
	TCHAR FindKeySeparator(FStringView Key)
	{
		for (const TCHAR Separator : { TEXT('?'), TEXT(';'), TEXT('#'), TEXT('='), TEXT('&') })
		{
			int32 Found = INDEX_NONE;
			if (Key.FindChar(Separator, Found))
			{
				return Separator;
			}
		}
		return 0;
	}
}

void FVaRestRedactRules::Normalize(FUtf8StringView Key, TArray<UTF8CHAR, TInlineAllocator<64>>& Out)
{
	Out.Reset(Key.Len());
	for (const UTF8CHAR Ch : Key)
	{
		if (Ch == '_' || Ch == '-' || Ch == '.')
		{
			continue;
		}
		Out.Add(Ch >= 'A' && Ch <= 'Z' ? static_cast<UTF8CHAR>(Ch - 'A' + 'a') : Ch);
	}
}

bool FVaRestRedactRules::IsAcceptableKey(FStringView Key)
{
	return VaRest::Json::RedactPrivate::FindKeySeparator(Key) == 0;
}

void FVaRestRedactRules::Append(const FVaRestRedactRules& Other)
{
	if (this == &Other) return;
	bAllJsonValues |= Other.bAllJsonValues;
	for (const auto& Key : Other.AddedKeys) if (!AddedKeys.Contains(Key)) AddedKeys.Add(Key);
}

void FVaRestRedactRules::AddKey(FStringView Key)
{
	if (const TCHAR Separator = VaRest::Json::RedactPrivate::FindKeySeparator(Key))
	{
		UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestRedactRules::AddKey: ignored the key '%s', which contains '%c' (a key cannot hold a URL or form separator)"), *FString(Key), Separator);
		return;
	}
	VaRest::Json::FUtf8Scratch Scratch;
	VaRest::Json::RedactPrivate::FKeyBuffer Normalized;
	Normalize(VaRest::Json::ConvertForDocument(Key, Scratch, /*bWarn*/ false), Normalized);
	if (Normalized.Num() == 0)
	{
		return;
	}
	const FUtf8StringView View(Normalized.GetData(), Normalized.Num());
	for (const TArray<UTF8CHAR>& Existing : AddedKeys)
	{
		if (View.Equals(FUtf8StringView(Existing.GetData(), Existing.Num()), ESearchCase::CaseSensitive))
		{
			return;
		}
	}
	AddedKeys.Emplace(Normalized.GetData(), Normalized.Num());
}

bool FVaRestRedactRules::ShouldRedact(FUtf8StringView Key, EVaRestRedactKeyKind Kind) const
{
	VaRest::Json::RedactPrivate::FKeyBuffer Normalized;
	Normalize(Key, Normalized);
	return MatchesNormalized(FUtf8StringView(Normalized.GetData(), Normalized.Num()), Kind);
}

bool FVaRestRedactRules::ShouldRedact(FStringView Key, EVaRestRedactKeyKind Kind) const
{
	VaRest::Json::FUtf8Scratch Scratch;
	return ShouldRedact(VaRest::Json::ConvertForDocument(Key, Scratch, /*bWarn*/ false), Kind);
}

bool FVaRestRedactRules::MatchesNormalized(FUtf8StringView Normalized, EVaRestRedactKeyKind Kind) const
{
	if (bAllJsonValues && Kind == EVaRestRedactKeyKind::Json) return true;
	using namespace VaRest::Json::RedactPrivate;
	if (Normalized.IsEmpty())
	{
		return false;
	}
	for (const TArray<UTF8CHAR>& Added : AddedKeys)
	{
		if (Normalized.Equals(FUtf8StringView(Added.GetData(), Added.Num()), ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	for (const UTF8CHAR* Name : Allowed)
	{
		if (IsExactly(Normalized, Name))
		{
			return false;
		}
	}
	if (IsExactly(Normalized, UTF8TEXT("sig")) || IsExactly(Normalized, UTF8TEXT("key")))
	{
		return true;
	}
	if (Kind == EVaRestRedactKeyKind::Query && IsExactly(Normalized, UTF8TEXT("code")))
	{
		return true;
	}
	for (const UTF8CHAR* Part : Substrings)
	{
		if (UE::String::FindFirst(Normalized, FUtf8StringView(Part), ESearchCase::CaseSensitive) != INDEX_NONE)
		{
			return true;
		}
	}
	return false;
}

FVaRestRedactResult VaRest::Json::RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules& Rules, EVaRestJsonFormat Format,
	TArray<uint8>& Out, bool bWholeInput)
{
	using namespace RedactPrivate;
	// The document parser skips a UTF-8 BOM (VaRestJsonReader.cpp:39-42, used at :53); so does the excerpt, or a body with one
	// would excerpt as "parse error at byte 0". Offsets below stay relative to the input as given.
	const int32 Bom = Prefix.StartsWith(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>("\xEF\xBB\xBF"), 3)) ? 3 : 0;
	Prefix.RightChopInline(Bom);
	// MemoryStream: a bounded byte stream whose Peek() is '\0' at the end (memorystream.h:42-47), so the view needs
	// neither a terminator nor a copy
	rapidjson::MemoryStream Stream(reinterpret_cast<const char*>(Prefix.GetData()), static_cast<size_t>(Prefix.Len()));
	FUtf8Sink Sink{ Out };
	FWriterAllocator WriterStack;
	UE::Json::Private::FAllocatorImpl ReaderStack;
	FReader Reader(&ReaderStack);
	rapidjson::ParseResult Parsed;
	bool bRedacted = false;
	bool bStoppedAtCut = false;
	bool bTooDeep = false;
	bool bValuePending = false;
	size_t KeyEnd = 0;

	auto Run = [&](auto& Writer)
	{
		TRedactHandler<std::decay_t<decltype(Writer)>> Handler(Writer, Rules, Stream, !bWholeInput);
		Parsed = Reader.Parse<Flags>(Stream, Handler);
		bRedacted = Handler.bRedacted;
		bStoppedAtCut = Handler.bStoppedAtCut;
		bTooDeep = Handler.bTooDeep;
		bValuePending = Handler.bValuePending;
		KeyEnd = Handler.KeyEnd;
	};
	if (Format == EVaRestJsonFormat::Pretty)
	{
		FPrettyWriter Writer(Sink, &WriterStack);
		Writer.SetIndent(' ', 2);
		Run(Writer);
	}
	else
	{
		FCondensedWriter Writer(Sink, &WriterStack);
		Run(Writer);
	}

	FVaRestRedactResult Result;
	Result.bRedacted = bRedacted;
	if (!Parsed.IsError())
	{
		return Result;
	}
	// The reader stopped here: the end of the input for a cut, the offending byte for a syntax error
	const size_t StopAt = Stream.Tell();
	const bool bAtEnd = StopAt == Stream.size_;
	if (bValuePending)
	{
		// The input stopped after a key. The writer defers the ':' to the value (writer.h:479-487; pretty:
		// prettywriter.h:231-233), so write it here, but only when the input holds it: the output then ends at the last
		// complete token, the key and its colon, and a missing colon is not papered over.
		size_t Pos = KeyEnd;
		while (Pos < StopAt && (Prefix[Pos] == ' ' || Prefix[Pos] == '\t' || Prefix[Pos] == '\n' || Prefix[Pos] == '\r'))
		{
			++Pos;
		}
		if (Pos < StopAt && Prefix[Pos] == ':')
		{
			Out.Add(':');
			if (Format == EVaRestJsonFormat::Pretty)
			{
				Out.Add(' ');
			}
		}
	}
	const int64 Offset = static_cast<int64>(Parsed.Offset()) + Bom;
	if (bTooDeep)
	{
		Result.StopReason = EVaRestRedactStop::ParseError;
		Result.Error = FString::Printf(TEXT("Nesting deeper than %d"), VaRest::Json::MaxDepth);
		Result.ErrorOffset = Offset;
		return Result;
	}
	// A cut input runs out mid-token, so the reader stops with the stream exhausted: MemoryStream::Take does not advance
	// past the end (memorystream.h:46) and string, escape, UTF-8, literal and number errors all Peek or Take the end. A
	// syntax error leaves Tell() on the offending byte. Deciding by the error offset instead misses escapes: RapidJSON
	// reports a bad \u escape or surrogate pair at the escape's backslash (reader.h:1011, :919, :1031), many bytes before
	// the end of a valid cut.
	if (!bWholeInput && (bStoppedAtCut || bAtEnd))
	{
		Result.StopReason = EVaRestRedactStop::Cap;
		return Result;
	}
	Result.StopReason = EVaRestRedactStop::ParseError;
	Result.Error = FString(rapidjson::GetParseError_En(Parsed.Code()));
	Result.ErrorOffset = Offset;
	return Result;
}
