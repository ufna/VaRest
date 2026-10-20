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
