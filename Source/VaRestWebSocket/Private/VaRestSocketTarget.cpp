// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTarget.h"

#include "CoreGlobals.h"
#include "Misc/URLRequestFilter.h"
#include "VaRestUrl.h"

namespace VaRest::WebSocket::VaRestSocketTargetPrivate
{
	/** The headers LWS writes itself (RFC 6455 4.1) or VaRest sets from Origin and Protocols: a duplicate breaks the upgrade */
	const TCHAR* const ReservedHeaders[] = { TEXT("Host"), TEXT("Upgrade"), TEXT("Connection"), TEXT("Origin"), TEXT("Sec-WebSocket-Key"),
		TEXT("Sec-WebSocket-Version"), TEXT("Sec-WebSocket-Protocol"), TEXT("Sec-WebSocket-Extensions"), TEXT("Sec-WebSocket-Accept") };

	TValueOrError<VaRest::WebSocket::FSocketTarget, VaRest::WebSocket::FTargetError> Refuse(EVaRestSocketFailure Failure, FString Message)
	{
		return MakeError(VaRest::WebSocket::FTargetError{ Failure, MoveTemp(Message) });
	}

	/** ASCII letters, digits, '-', '.' and '_': a non-ASCII host needs punycode, as for HTTP (VaRest::Http::HasNonAsciiHost) */
	bool IsHostChar(TCHAR C)
	{
		return (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9'))
			|| C == TEXT('-') || C == TEXT('.') || C == TEXT('_');
	}

	bool IsDigits(FStringView Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (const TCHAR C : Text)
		{
			if (C < TEXT('0') || C > TEXT('9'))
			{
				return false;
			}
		}
		return true;
	}

	/** VCHAR, space and tab (spec 2.2, D23) */
	bool IsHeaderValueChar(TCHAR C)
	{
		return C == TEXT('\t') || (C >= 0x20 && C <= 0x7E);
	}

	/** RFC 3987 3.1: each UTF-8 byte at or past 0x80, each space and control, as %XX; everything else -- '%' included -- as written */
	FString EncodePathAndQuery(FStringView Text)
	{
		const FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		FString Out;
		Out.Reserve(Utf8.Length());
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
			if (Byte >= 0x80 || Byte <= 0x20 || Byte == 0x7F)
			{
				Out.Appendf(TEXT("%%%02X"), Byte);
			}
			else
			{
				Out.AppendChar(static_cast<TCHAR>(Byte));
			}
		}
		return Out;
	}
}

bool VaRest::WebSocket::IsIpv4Literal(FStringView Host)
{
	int32 Dots = 0;
	int32 Digits = 0;
	int32 Value = 0;
	for (const TCHAR C : Host)
	{
		if (C == TEXT('.'))
		{
			if (Digits == 0)
			{
				return false;
			}
			++Dots;
			Digits = 0;
			Value = 0;
			continue;
		}
		if (C < TEXT('0') || C > TEXT('9') || ++Digits > 3)
		{
			return false;
		}
		Value = Value * 10 + (C - TEXT('0'));
		if (Value > 255)
		{
			return false;
		}
	}
	return Dots == 3 && Digits > 0;
}

TValueOrError<VaRest::WebSocket::FSocketTarget, VaRest::WebSocket::FTargetError> VaRest::WebSocket::PrepareTarget(const FVaRestSocketConnectRequest& Request, bool bShippingRules)
{
	using namespace VaRest::WebSocket::VaRestSocketTargetPrivate;
	FSocketTarget Out;
	const FString& Url = Request.Url;
	const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
	if (SchemeEnd == INDEX_NONE)
	{
		return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the URL has no scheme (ws:// or wss://)"));
	}
	const FString Scheme = Url.Left(SchemeEnd).ToLower();
	bool bInsecureScheme = false;
	if (Scheme.Equals(TEXT("wss"), ESearchCase::CaseSensitive))
	{
		Out.bTls = true;
	}
	else if (Scheme.Equals(TEXT("wss+insecure"), ESearchCase::CaseSensitive) && !bShippingRules)
	{
		Out.bTls = true;
		bInsecureScheme = true;
	}
	else if (!Scheme.Equals(TEXT("ws"), ESearchCase::CaseSensitive))
	{
		return Refuse(EVaRestSocketFailure::BadUrl, bShippingRules ? TEXT("the scheme is not ws or wss") : TEXT("the scheme is not ws, wss or wss+insecure"));
	}
	Out.bInsecure = bInsecureScheme || (Out.bTls && Request.bAllowSelfSignedTls && !bShippingRules);
	const FString Rest = Url.Mid(SchemeEnd + 3);
	int32 Found = INDEX_NONE;
	if (Rest.FindChar(TEXT('#'), Found))
	{
		return Refuse(EVaRestSocketFailure::BadUrl, TEXT("a WebSocket URL has no fragment (#)"));
	}
	int32 AuthorityEnd = Rest.Len();
	for (int32 Index = 0; Index < Rest.Len(); ++Index)
	{
		if (Rest[Index] == TEXT('/') || Rest[Index] == TEXT('?'))
		{
			AuthorityEnd = Index;
			break;
		}
	}
	const FString Authority = Rest.Left(AuthorityEnd);
	if (Authority.FindChar(TEXT('@'), Found))
	{
		return Refuse(EVaRestSocketFailure::BadUrl, TEXT("user info in a WebSocket URL is never sent: put credentials in a header"));
	}
	FString PortText;
	bool bPortGiven = false;
	if (Authority.StartsWith(TEXT("["), ESearchCase::CaseSensitive))
	{
		int32 CloseBracket = INDEX_NONE;
		if (!Authority.FindChar(TEXT(']'), CloseBracket) || CloseBracket < 2)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("an IPv6 host needs [ and ]"));
		}
		Out.Host = Authority.Mid(1, CloseBracket - 1);
		bool bColon = false;
		for (const TCHAR C : Out.Host)
		{
			if (!(FChar::IsHexDigit(C) || C == TEXT(':') || C == TEXT('.')))
			{
				return Refuse(EVaRestSocketFailure::BadUrl, TEXT("an IPv6 host holds only hex digits, ':' and '.'"));
			}
			bColon = bColon || C == TEXT(':');
		}
		if (!bColon)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("brackets hold an IPv6 address"));
		}
		Out.bHostIsIp = true;
		const FString After = Authority.Mid(CloseBracket + 1);
		if (!After.IsEmpty())
		{
			if (!After.StartsWith(TEXT(":"), ESearchCase::CaseSensitive))
			{
				return Refuse(EVaRestSocketFailure::BadUrl, TEXT("only :port may follow an IPv6 host"));
			}
			bPortGiven = true;
			PortText = After.Mid(1);
		}
	}
	else
	{
		int32 Colon = INDEX_NONE;
		if (Authority.FindLastChar(TEXT(':'), Colon))
		{
			Out.Host = Authority.Left(Colon);
			PortText = Authority.Mid(Colon + 1);
			bPortGiven = true;
		}
		else
		{
			Out.Host = Authority;
		}
		if (Out.Host.IsEmpty())
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the URL has no host"));
		}
		for (const TCHAR C : Out.Host)
		{
			if (!IsHostChar(C))
			{
				return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the host holds a character VaRest does not send (a non-ASCII host needs punycode; spaces and controls are refused)"));
			}
		}
		Out.bHostIsIp = IsIpv4Literal(Out.Host);
	}
	const int32 DefaultPort = Out.bTls ? 443 : 80;
	Out.Port = DefaultPort;
	if (bPortGiven)
	{
		if (!IsDigits(PortText) || PortText.Len() > 5)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the port is not a number from 1 to 65535"));
		}
		Out.Port = FCString::Atoi(*PortText);
		if (Out.Port < 1 || Out.Port > 65535)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the port is not a number from 1 to 65535"));
		}
	}
	const FString BracketedHost = Out.Host.Contains(TEXT(":"), ESearchCase::CaseSensitive) ? FString::Printf(TEXT("[%s]"), *Out.Host) : Out.Host;
	// LwsWebSocket.cpp:775-785: the Host header drops the scheme's default port and keeps any other
	Out.HostHeader = Out.Port == DefaultPort ? BracketedHost : FString::Printf(TEXT("%s:%d"), *BracketedHost, Out.Port);
	FString Path = Rest.Mid(AuthorityEnd);
	if (Path.IsEmpty())
	{
		Path = TEXT("/");
	}
	else if (Path[0] == TEXT('?'))
	{
		Path = TEXT("/") + Path;
	}
	Out.PathAndQuery = EncodePathAndQuery(Path);
	// Origin (spec 2.2, D29): derived, given or omitted; sent raw (T4, LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN)
	if (!Request.Origin.IsSet())
	{
		Out.Origin = FString(Out.bTls ? TEXT("https://") : TEXT("http://")) + Out.HostHeader;
	}
	else if (!Request.Origin.GetValue().IsEmpty())
	{
		for (const TCHAR C : Request.Origin.GetValue())
		{
			if (C <= 0x20 || C >= 0x7F)
			{
				return Refuse(EVaRestSocketFailure::BadHandshake, TEXT("the Origin holds a space, a control or a non-ASCII character"));
			}
		}
		Out.Origin = Request.Origin.GetValue();
	}
	// Subprotocols (RFC 6455 4.1: tokens, each offered once; plan decision 14)
	TArray<FString> Offered;
	for (const FString& Protocol : Request.Protocols)
	{
		if (!VaRest::Http::IsValidHeaderName(Protocol))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, TEXT("a subprotocol is not an RFC 7230 token"));
		}
		if (Offered.ContainsByPredicate([&Protocol](const FString& Each) { return Each.Equals(Protocol, ESearchCase::CaseSensitive); }))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("subprotocol %s is offered twice"), *Protocol));
		}
		Offered.Add(Protocol);
	}
	Out.Protocols = FString::Join(Offered, TEXT(", "));
	// Headers (spec 2.2, D23): checked here so the error is immediate; the room is checked again in APPEND_HANDSHAKE_HEADER
	for (const TPair<FString, FString>& Header : Request.Headers)
	{
		if (!VaRest::Http::IsValidHeaderName(Header.Key))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, TEXT("a header name is not an RFC 7230 token"));
		}
		for (const TCHAR* Reserved : ReservedHeaders)
		{
			if (Header.Key.Equals(Reserved, ESearchCase::IgnoreCase))
			{
				return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("header %s is the transport's own (set Origin or Protocols instead)"), Reserved));
			}
		}
		for (const TCHAR C : Header.Value)
		{
			if (!IsHeaderValueChar(C))
			{
				// The name is a token, so it is safe to print; the value never is
				return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("the value of header %s holds a control or a non-ASCII character"), *Header.Key));
			}
		}
		const FString Line = FString::Printf(TEXT("%s: %s\r\n"), *Header.Key, *Header.Value);
		if (Out.HeaderBlock.Num() + Line.Len() > MaxHeaderBlockBytes)
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("the headers are over %d bytes, the handshake buffer (spec 15)"), MaxHeaderBlockBytes));
		}
		for (const TCHAR C : Line)
		{
			Out.HeaderBlock.Add(static_cast<uint8>(C));
		}
	}
	Out.FilterUrl = FString::Printf(TEXT("%s://%s"), Out.bTls ? TEXT("wss") : TEXT("ws"), *Out.HostHeader);
	return MakeValue(MoveTemp(Out));
}

bool VaRest::WebSocket::IsAllowedByFilter(const FString& FilterUrl, const TArray<TPair<FString, TArray<FString>>>* Allowlist)
{
	if (Allowlist)
	{
		UE::Core::FURLRequestFilter::FRequestMap Map;
		for (const TPair<FString, TArray<FString>>& Entry : *Allowlist)
		{
			Map.Emplace(Entry.Key, Entry.Value);
		}
		return UE::Core::FURLRequestFilter(MoveTemp(Map)).IsRequestAllowed(FilterUrl);
	}
	check(IsInGameThread());
	// Built per connect, as the engine does (LwsWebSocket.cpp:114), so an ini change applies to the next attempt
	return UE::Core::FURLRequestFilter(TEXT("Online.HttpManager"), GEngineIni).IsRequestAllowed(FilterUrl);
}
