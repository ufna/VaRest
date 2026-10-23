// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsErrors.h"

#include "Containers/StringConv.h"

namespace VaRest::WebSocket::Lws::VaRestLwsErrorsPrivate
{
	struct FRow
	{
		const TCHAR* Text;
		EVaRestSocketFailure Failure;
		int32 Status;
	};

	const FRow Rows[] = {
		// The status-line parser (T7; passport hs.refusal_401, hs.refusal_403): the status is in the string
		{ TEXT("HS: ws upgrade unauthorized"), EVaRestSocketFailure::UpgradeRefused, 401 },
		{ TEXT("HS: ws upgrade forbidden"), EVaRestSocketFailure::UpgradeRefused, 403 },
		// A 3xx LWS could not follow (it follows the rest, plan decision 10) or a downgrade it refuses [hs.redirect_downgrade]
		{ TEXT("HS: Redirect failed"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: Redirect attempted SSL downgrade"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: Redirect code but no Location"), EVaRestSocketFailure::Redirect, 0 },
		// A subprotocol the client did not offer [hs.unoffered_selected]
		{ TEXT("HS: PROTOCOL malformed"), EVaRestSocketFailure::ProtocolMismatch, 0 },
		{ TEXT("HS: Cannot match protocol"), EVaRestSocketFailure::ProtocolMismatch, 0 },
		// A 101 a client must refuse [hs.bad_accept, hs.missing_upgrade, hs.missing_connection]
		{ TEXT("HS: ACCEPT missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Accept hash wrong"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: UPGRADE missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: UPGRADE malformed"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Upgrade to something other than websocket"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: CONNECTION missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: :status missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI did not parse"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: h2 / ws upgrade unsupported"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed by client filter"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		// VaRest's own refusals in FILTER_PRE_ESTABLISH and ESTABLISHED: the connection's own failure overrides these
		{ TEXT("HS: Rejected by filter cb"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Rejected at CLIENT_ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed at ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		// 22 A2: the upgrade went out and the connection closed before any status line [hs.no_upgrade_response]
		{ TEXT("read failed"), EVaRestSocketFailure::NoUpgradeResponse, 0 },
		// No HTTP status at all: TCP [hs.connect_refused], TLS [tls.untrusted_rejected], DNS, the proxy
		{ TEXT("connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("client connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("getaddrinfo failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("socket connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("lws_ssl_client_connect1 failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("bio_create failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("proxy write failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("ERROR reading from proxy socket"), EVaRestSocketFailure::ConnectFailed, 0 },
		// The archive's other client strings (review spec M-3): the socket could not be set up or serviced
		{ TEXT("set socket opts failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("insert wsi failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("POLLOUT set failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("first service failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("HS: SO_SNDBUF failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("Timed out waiting SSL"), EVaRestSocketFailure::ConnectTimeout, 0 },
	};

	const TCHAR* const NotOneOhOne = TEXT("HS: ws upgrade not 101, was ");
	const TCHAR* const ProxyError = TEXT("ERROR proxy: ");

	/** The string without trailing whitespace and NULs: what both the classification and a Detail read */
	FStringView TrimLwsText(FStringView Text)
	{
		FStringView Trimmed = Text;
		while (!Trimmed.IsEmpty() && (Trimmed[Trimmed.Len() - 1] == 0 || FChar::IsWhitespace(Trimmed[Trimmed.Len() - 1])))
		{
			Trimmed.LeftChopInline(1);
		}
		return Trimmed;
	}
}

VaRest::WebSocket::Lws::FConnectionErrorClass VaRest::WebSocket::Lws::ClassifyConnectionError(FStringView Text)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsErrorsPrivate;
	const FStringView Trimmed = TrimLwsText(Text);
	FConnectionErrorClass Out;
	if (Trimmed.IsEmpty())
	{
		// LWS's own stage timeouts and a kill while pending give "" [timeout.default_secs, hs.status_in_connection_error.killed_pending]
		Out.bEmpty = true;
		return Out;
	}
	if (Trimmed.StartsWith(NotOneOhOne, ESearchCase::CaseSensitive))
	{
		const FStringView Digits = Trimmed.RightChop(FCString::Strlen(NotOneOhOne));
		int32 Status = 0;
		for (const TCHAR C : Digits)
		{
			if (!FChar::IsDigit(C) || Digits.Len() > 3)
			{
				return Out;
			}
			Status = Status * 10 + (C - TEXT('0'));
		}
		if (Digits.IsEmpty())
		{
			return Out;
		}
		// Only 100-599 is an HTTP status; HttpStatus 0 means "none", so a bogus number must not reach an error message
		if (Status < 100 || Status > 599)
		{
			return Out;
		}
		// A 3xx that reached the parser is a redirect LWS did not follow, refused all the same (ruling Q2)
		Out.Failure = Status >= 300 && Status <= 399 ? EVaRestSocketFailure::Redirect : EVaRestSocketFailure::UpgradeRefused;
		Out.HttpStatus = Status;
		return Out;
	}
	if (Trimmed.StartsWith(ProxyError, ESearchCase::CaseSensitive))
	{
		Out.Failure = EVaRestSocketFailure::ConnectFailed;
		return Out;
	}
	for (const FRow& Row : Rows)
	{
		if (Trimmed.Equals(Row.Text, ESearchCase::CaseSensitive))
		{
			Out.Failure = Row.Failure;
			Out.HttpStatus = Row.Status;
			return Out;
		}
	}
	return Out;
}

VaRest::WebSocket::Lws::FConnectionErrorClass VaRest::WebSocket::Lws::ClassifyEmptyConnectionError(double TookSeconds, FString& OutDetail)
{
	// "" is one of LWS's stage timers -- its fixed 20 s for the TCP connect and the proxy, or timeout_secs -- or a kill VaRest
	// did not make. VaRest's own ConnectTimeout (at most timeout_secs) normally fires first, so only the time taken tells them apart
	FConnectionErrorClass Out;
	Out.bEmpty = true;
	// NaN and negative compare false here and fall to Unknown
	if (TookSeconds >= LwsConnectStageSeconds - LwsStageToleranceSeconds)
	{
		Out.Failure = EVaRestSocketFailure::ConnectTimeout;
		// Within one more second of the stage it is the 20 s timer; later only timeout_secs can have fired
		OutDetail = TookSeconds < LwsConnectStageSeconds + 1.0
			? FString::Printf(TEXT("libwebsockets' own 20 s connect stage timed out after %.1f s"), TookSeconds)
			: FString::Printf(TEXT("libwebsockets' own timeout_secs timed out after %.1f s"), TookSeconds);
	}
	else
	{
		OutDetail = FString::Printf(TEXT("an empty CONNECTION_ERROR from libwebsockets after %.1f s"), TookSeconds);
	}
	return Out;
}

FString VaRest::WebSocket::Lws::CleanConnectionErrorText(FStringView Text)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsErrorsPrivate;
	const FStringView Trimmed = TrimLwsText(Text);
	int32 Keep = FMath::Min(Trimmed.Len(), MaxLwsErrorTextChars);
	// TCHAR is UTF-16 (Linux included): a cut after a high surrogate would leave half a code point
	if (Keep < Trimmed.Len() && Keep > 0 && StringConv::IsHighSurrogate(static_cast<uint32>(Trimmed[Keep - 1])))
	{
		--Keep;
	}
	FString Out(Trimmed.Left(Keep));
	for (TCHAR& Char : Out)
	{
		// A proxy's bytes may hold anything: no line breaks or terminal controls reach a log line
		if (Char < 0x20 || (Char >= 0x7F && Char <= 0x9F))
		{
			Char = TEXT('?');
		}
	}
	if (Keep < Trimmed.Len())
	{
		Out += TEXT("...");
	}
	return Out;
}

VaRest::WebSocket::Lws::FConnectionErrorEnd VaRest::WebSocket::Lws::DescribeConnectionError(FStringView Text, double TookSeconds)
{
	// Classified on the string as LWS gave it: cleaning it first could change what the table matches
	const FConnectionErrorClass Class = ClassifyConnectionError(Text);
	FConnectionErrorEnd Out;
	Out.Failure = Class.Failure;
	Out.HttpStatus = Class.HttpStatus;
	// Every failure keeps the string, Unknown above all: there it is the only clue (Task 5 review M-4)
	Out.Detail = FString::Printf(TEXT("libwebsockets: \"%s\""), *CleanConnectionErrorText(Text));
	if (Class.bEmpty)
	{
		// Review conc M-1, M-2: which of LWS's own timers, or a kill VaRest did not make, by the time taken
		Out.Failure = ClassifyEmptyConnectionError(TookSeconds, Out.Detail).Failure;
	}
	Out.By = Out.Failure == EVaRestSocketFailure::ConnectTimeout ? EVaRestSocketCloseBy::Timeout : EVaRestSocketCloseBy::Transport;
	return Out;
}
