// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTarget.h"

#include "CoreGlobals.h"
#include "Misc/URLRequestFilter.h"
#include "VaRestUrl.h"

namespace VaRest::WebSocket::VaRestSocketTargetPrivate
{
	/** The headers LWS writes itself (RFC 6455 4.1) or VaRest sets from Origin and Protocols: a duplicate breaks the upgrade */
	const TCHAR* const ReservedHeaders[] = { TEXT("Host"), TEXT("Upgrade"), TEXT("Connection"), TEXT("Origin") };

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

	/** RFC 4291 2.2: groups of 1-4 hex digits, at most one "::", an optional trailing dotted IPv4 (two groups); 8 groups, or fewer with "::" */
	bool IsIpv6Literal(const FString& Text)
	{
		FString Work = Text;
		int32 Groups = 0;
		int32 LastColon = INDEX_NONE;
		if (Work.FindLastChar(TEXT(':'), LastColon) && Work.Mid(LastColon + 1).Contains(TEXT("."), ESearchCase::CaseSensitive))
		{
			if (!VaRest::WebSocket::IsIpv4Literal(Work.Mid(LastColon + 1)))
			{
				return false;
			}
			Groups = 2;
			// Keep the colon: "1::1.2.3.4" becomes "1::" and "::1.2.3.4" "::"
			Work = Work.Left(LastColon + 1);
			if (!Work.EndsWith(TEXT("::"), ESearchCase::CaseSensitive))
			{
				Work.LeftChopInline(1);
			}
		}
		const int32 Compressed = Work.Find(TEXT("::"), ESearchCase::CaseSensitive);
		if (Compressed != INDEX_NONE && (Work.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Compressed + 1) != INDEX_NONE))
		{
			return false;
		}
		const FString Sides[2] = { Compressed == INDEX_NONE ? Work : Work.Left(Compressed), Compressed == INDEX_NONE ? FString() : Work.Mid(Compressed + 2) };
		for (int32 SideIndex = 0; SideIndex < (Compressed == INDEX_NONE ? 1 : 2); ++SideIndex)
		{
			if (Sides[SideIndex].IsEmpty())
			{
				if (Compressed == INDEX_NONE)
				{
					return false;
				}
				continue;
			}
			TArray<FString> Parts;
			Sides[SideIndex].ParseIntoArray(Parts, TEXT(":"), false);
			for (const FString& Part : Parts)
			{
				if (Part.IsEmpty() || Part.Len() > 4)
				{
					return false;
				}
				for (const TCHAR C : Part)
				{
					if (!FChar::IsHexDigit(C))
					{
						return false;
					}
				}
				++Groups;
			}
		}
		return Compressed == INDEX_NONE ? Groups == 8 : Groups <= 7;
	}

	/** Non-empty labels of at most 63 characters, 253 in all (RFC 1035 2.3.4); a trailing dot is refused, since the Host header and TLS name check would carry it */
	bool IsHostShape(const FString& Host)
	{
		if (Host.Len() > 253)
		{
			return false;
		}
		TArray<FString> Labels;
		Host.ParseIntoArray(Labels, TEXT("."), false);
		for (const FString& Label : Labels)
		{
			if (Label.IsEmpty() || Label.Len() > 63)
			{
				return false;
			}
		}
		return true;
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

	/**
	 * What inet_aton (the resolver's numeric-host parser) would take: 1-4 labels, each decimal, 0x-hex or leading-0 octal.
	 * Decimal-only hosts are refused at any label count (as before); hex labels only within inet_aton's 4
	 */
	bool IsInetAtonNumber(const FString& Host)
	{
		TArray<FString> Labels;
		Host.ParseIntoArray(Labels, TEXT("."), false);
		bool bAllDecimal = true;
		for (const FString& Label : Labels)
		{
			if (Label.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase))
			{
				bAllDecimal = false;
				for (int32 Index = 2; Index < Label.Len(); ++Index)
				{
					if (!FChar::IsHexDigit(Label[Index]))
					{
						return false;
					}
				}
			}
			else if (!IsDigits(Label))
			{
				return false;
			}
		}
		return !Labels.IsEmpty() && (bAllDecimal || Labels.Num() <= 4);
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
		// 01.1.1.1 reads as octal in some stacks and decimal in others: refuse the ambiguity
		if (Digits == 2 && Value == 0)
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
		if (!IsIpv6Literal(Out.Host))
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("brackets hold an IPv6 address (RFC 4291 2.2)"));
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
		if (!IsHostShape(Out.Host))
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the host has an empty or over-long label, or is over 253 characters"));
		}
		Out.bHostIsIp = IsIpv4Literal(Out.Host);
		// A host inet_aton would read as a number (01.1.1.1, 1.2.3, 2130706433, 0x7f.0.0.1, 0X7F.1) reaches the resolver as a
		// hostname and from there loopback under another name: refuse it unless it is a clean dotted-decimal IPv4
		if (!Out.bHostIsIp && IsInetAtonNumber(Out.Host))
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("a numeric host must be four decimal octets without leading zeros"));
		}
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
			return Refuse(EVaRestSocketFailure::BadHandshake, FString(TEXT("a subprotocol is offered twice")));
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
		// RFC 6455 4.1 reserves the whole Sec-WebSocket- prefix for the handshake
		if (Header.Key.StartsWith(TEXT("Sec-WebSocket-"), ESearchCase::IgnoreCase))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("header %s is reserved: the Sec-WebSocket- prefix is the transport's own (set Protocols instead)"), *Header.Key));
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
