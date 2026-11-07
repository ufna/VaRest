// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketQueryWarning.h"
#include "VaRestLwsCopyPolicy.h"
#include "Misc/CoreMisc.h"
#include "Misc/OutputDevice.h"
#include "Modules/ModuleManager.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws::QueryWarningPrivate
{
/** Passed directly to Core exec, never attached to GLog or the global category registry. */
class FCappedListing final : public FOutputDevice
{
public:
	void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
	{
		if (bOverflow || !Text) return;
		const int32 Remaining = MaxEngineLogListingChars - Listing.Len();
		int32 Length = 0;
		while (Length < Remaining && Text[Length] != TCHAR(0)) ++Length;
		if (Length == Remaining) { bOverflow = true; Listing.Reset(); return; }
		Listing.Append(Text, Length); Listing.AppendChar(TEXT('\n'));
	}
	FString Listing;
	bool bOverflow = false;
};
}

VaRest::WebSocket::EObservedEngineLog VaRest::WebSocket::Lws::ObserveEngineLogLevel()
{
	check(IsInGameThread());
#if UE_ALLOW_EXEC_COMMANDS && !NO_LOGGING
	QueryWarningPrivate::FCappedListing Output;
	if (FSelfRegisteringExec::StaticExec(nullptr, TEXT("Log list LogWebSockets"), Output) && !Output.bOverflow)
		return ParseEngineLogListing(Output.Listing);
#endif
	return EObservedEngineLog::Unknown;
}

void VaRest::WebSocket::Lws::WarnForSharedQueryLogging(FStringView Path)
{
	check(IsInGameThread());
#if !NO_LOGGING
	constexpr bool bShared = SharesEngineLws;
	if (!bShared) return;
	const bool bEngineLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets"));
	static FQueryWarningLatch Once;
	if (Once.IsCandidate(bShared, bEngineLoaded, Path) && Once.Take(bShared, bEngineLoaded, ObserveEngineLogLevel(), Path))
		UE_LOG(LogVaRestSocket, Warning, TEXT("Shared libwebsockets may expose WebSocket query credentials if LogWebSockets is Verbose; disable Verbose logging in credential-bearing builds"));
#endif
}
