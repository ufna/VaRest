// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::WebSocket
{
enum class EObservedEngineLog : uint8 { Unknown, BelowVerbose, Verbose };
inline constexpr int32 MaxEngineLogListingChars = 16 * 1024;

/** Core's listing filter is a substring match. Accept only exact category/type rows and consistent duplicates. */
inline EObservedEngineLog ParseEngineLogListing(const FString& Listing)
{
	if (Listing.Len() > MaxEngineLogListingChars) return EObservedEngineLog::Unknown;
	TArray<FString> Lines; Listing.ParseIntoArrayLines(Lines);
	TOptional<EObservedEngineLog> Found;
	for (const FString& Line : Lines)
	{
		TArray<FString> Tokens; Line.ParseIntoArrayWS(Tokens);
		if (Tokens.IsEmpty() || !Tokens[0].Equals(TEXT("LogWebSockets"), ESearchCase::CaseSensitive)) continue;
		if (Tokens.Num() != 2 && !(Tokens.Num() == 4 && Tokens[2] == TEXT("-") && Tokens[3].Equals(TEXT("DebugBreak"), ESearchCase::CaseSensitive)))
			return EObservedEngineLog::Unknown;
		EObservedEngineLog Value = EObservedEngineLog::Unknown;
		if (Tokens[1].Equals(TEXT("Verbose"), ESearchCase::CaseSensitive) || Tokens[1].Equals(TEXT("VeryVerbose"), ESearchCase::CaseSensitive))
			Value = EObservedEngineLog::Verbose;
		else for (const TCHAR* Low : {TEXT("NoLogging"), TEXT("Fatal"), TEXT("Error"), TEXT("Warning"), TEXT("Display"), TEXT("Log")})
			if (Tokens[1].Equals(Low, ESearchCase::CaseSensitive)) { Value = EObservedEngineLog::BelowVerbose; break; }
		if (Value == EObservedEngineLog::Unknown || (Found.IsSet() && Found.GetValue() != Value)) return EObservedEngineLog::Unknown;
		Found = Value;
	}
	return Found.Get(EObservedEngineLog::Unknown);
}

/** Private GTK policy; the production instance belongs to one transport module copy. */
class FQueryWarningLatch
{
public:
	bool IsCandidate(bool bShared, bool bEngineLoaded, FStringView Path, bool bLoggingEnabled = true) const
	{
		if (bWarned || !bLoggingEnabled || !bShared || !bEngineLoaded) return false;
		int32 QueryAt = INDEX_NONE;
		for (int32 Index = 0; Index < Path.Len(); ++Index)
		{
			if (Path[Index] == TEXT('#')) return QueryAt != INDEX_NONE && QueryAt + 1 < Index;
			if (QueryAt == INDEX_NONE && Path[Index] == TEXT('?')) QueryAt = Index;
		}
		return QueryAt != INDEX_NONE && QueryAt + 1 < Path.Len();
	}
	bool Take(bool bShared, bool bEngineLoaded, EObservedEngineLog Level, FStringView Path, bool bLoggingEnabled = true)
	{
		if (!IsCandidate(bShared, bEngineLoaded, Path, bLoggingEnabled) || Level == EObservedEngineLog::BelowVerbose) return false;
		bWarned = true;
		return true;
	}
private:
	bool bWarned = false;
};

namespace Lws
{
/** GTK only; literal read-only public Core command, with bounded output. Unsupported/missing stays Unknown. */
EObservedEngineLog ObserveEngineLogLevel();
/** GTK after target validation; conditional warning has no URL, parameter name or value. */
void WarnForSharedQueryLogging(FStringView Path);
}
}
