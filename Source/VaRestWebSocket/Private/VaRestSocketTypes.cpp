// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTypes.h"

#include "VaRestJsonUtf8.h"

namespace VaRest::WebSocket::VaRestSocketTypesPrivate
{
	/** A frame of Size payload bytes with zeroed headroom; Size must fit an int32 with the headroom */
	FVaRestSocketFrame Allocate(EVaRestSocketFrameKind Kind, int32 Size)
	{
		FVaRestSocketFrame Out;
		Out.Kind = Kind;
		Out.Storage.SetNumUninitialized(VaRest::WebSocket::FrameHeadroom + Size);
		FMemory::Memzero(Out.Storage.GetData(), VaRest::WebSocket::FrameHeadroom);
		Out.Length = Size;
		return Out;
	}

	FString Utf8ToString(const uint8* Data, int32 Num)
	{
		return Num > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Data), Num) : FString();
	}
}

FVaRestSocketFrame FVaRestSocketFrame::Text(FUtf8StringView InText)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	FVaRestSocketFrame Out = Allocate(EVaRestSocketFrameKind::Text, InText.Len());
	if (InText.Len() > 0)
	{
		FMemory::Memcpy(Out.Storage.GetData() + VaRest::WebSocket::FrameHeadroom, InText.GetData(), InText.Len());
	}
	return Out;
}

FVaRestSocketFrame FVaRestSocketFrame::Text(FStringView InText)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	const int64 Bytes = VaRest::Json::Utf8Length(InText);
	if (Bytes > MAX_int32 - VaRest::WebSocket::FrameHeadroom)
	{
		// Past what a TArray holds: an invalid frame, which Send refuses
		FVaRestSocketFrame Invalid;
		Invalid.Length = -1;
		return Invalid;
	}
	FVaRestSocketFrame Out = Allocate(EVaRestSocketFrameKind::Text, static_cast<int32>(Bytes));
	int32 Replaced = 0;
	// Dest is exactly Utf8Length bytes, which WriteUtf8 fills (VaRestJsonUtf8.h)
	const int32 Written = VaRest::Json::WriteUtf8(InText,
		TArrayView<UTF8CHAR>(reinterpret_cast<UTF8CHAR*>(Out.Storage.GetData() + VaRest::WebSocket::FrameHeadroom), static_cast<int32>(Bytes)), Replaced);
	Out.Length = FMath::Max(Written, 0);
	Out.Storage.SetNum(VaRest::WebSocket::FrameHeadroom + Out.Length);
	return Out;
}

FVaRestSocketFrame FVaRestSocketFrame::Binary(TConstArrayView<uint8> InBytes)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	FVaRestSocketFrame Out = Allocate(EVaRestSocketFrameKind::Binary, InBytes.Num());
	if (InBytes.Num() > 0)
	{
		FMemory::Memcpy(Out.Storage.GetData() + VaRest::WebSocket::FrameHeadroom, InBytes.GetData(), InBytes.Num());
	}
	return Out;
}

TOptional<FVaRestSocketFrame> FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind InKind, TArray<uint8>&& InStorage, int32 InOffset, int32 InLength)
{
	if (InOffset < VaRest::WebSocket::FrameHeadroom || InLength < 0 || static_cast<int64>(InOffset) + InLength > InStorage.Num())
	{
		return TOptional<FVaRestSocketFrame>();
	}
	FVaRestSocketFrame Out;
	Out.Kind = InKind;
	Out.Storage = MoveTemp(InStorage);
	Out.Offset = InOffset;
	Out.Length = InLength;
	return Out;
}

bool VaRest::WebSocket::IsValidCloseCode(int32 Code)
{
	// RFC 6455 7.4.1 and 7.4.2 with the IANA registry: 1004-1006 and 1015 are never on the wire, 1016-2999 are reserved,
	// 3000-4999 belong to libraries and applications
	return (Code >= 1000 && Code <= 1003) || (Code >= 1007 && Code <= 1014) || (Code >= 3000 && Code <= 4999);
}

int32 VaRest::WebSocket::SanitizeCloseCode(int32 Code, bool& bOutReplaced)
{
	bOutReplaced = !IsValidCloseCode(Code);
	return bOutReplaced ? 1000 : Code;
}

TArray<uint8> VaRest::WebSocket::CloseReasonBytes(FStringView Reason, bool& bOutTruncated)
{
	// Each UTF-16 unit is at least one UTF-8 byte, so 124 units reach past 123 bytes whenever the reason is longer; a surrogate
	// pair split at unit 124 becomes U+FFFD at byte 123 or later, which the cut below drops (review spec I-1)
	const FStringView Head = Reason.Left(MaxCloseReasonBytes + 1);
	TArray<uint8> Out;
	Out.SetNumUninitialized(static_cast<int32>(VaRest::Json::Utf8Length(Head)));
	int32 Replaced = 0;
	const int32 Written = VaRest::Json::WriteUtf8(Head, TArrayView<UTF8CHAR>(reinterpret_cast<UTF8CHAR*>(Out.GetData()), Out.Num()), Replaced);
	Out.SetNum(FMath::Max(Written, 0));
	// Over the limit only when the converted head is: a reason of more than 124 units is always over it
	bOutTruncated = Out.Num() > MaxCloseReasonBytes;
	if (Out.Num() > MaxCloseReasonBytes)
	{
		// Back to the first byte of the code point the cut falls in: continuation bytes are 10xxxxxx
		int32 Cut = MaxCloseReasonBytes;
		while (Cut > 0 && (Out[Cut] & 0xC0) == 0x80)
		{
			--Cut;
		}
		Out.SetNum(Cut);
	}
	return Out;
}

VaRest::WebSocket::FPeerClose VaRest::WebSocket::ParsePeerClose(TConstArrayView<uint8> Payload)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	FPeerClose Out;
	if (Payload.Num() == 0)
	{
		// RFC 6455 7.1.5: no status code was received
		Out.Code = 1005;
		return Out;
	}
	if (Payload.Num() == 1)
	{
		Out.Code = 1002;
		Out.bMalformed = true;
		return Out;
	}
	Out.ReceivedCode = (static_cast<int32>(Payload[0]) << 8) | static_cast<int32>(Payload[1]);
	if (IsValidCloseCode(Out.ReceivedCode))
	{
		Out.Code = Out.ReceivedCode;
	}
	else
	{
		Out.Code = 1005;
		Out.bCodeNormalised = true;
	}
	const FUtf8StringView Reason(reinterpret_cast<const UTF8CHAR*>(Payload.GetData() + 2), Payload.Num() - 2);
	if (VaRest::Json::IsValidUtf8(Reason))
	{
		Out.Reason = Utf8ToString(Payload.GetData() + 2, Payload.Num() - 2);
	}
	else
	{
		VaRest::Json::FUtf8Scratch Repaired;
		VaRest::Json::AppendRepairedUtf8(Reason, Repaired);
		Out.Reason = Utf8ToString(reinterpret_cast<const uint8*>(Repaired.GetData()), Repaired.Num());
		Out.bReasonRepaired = true;
	}
	return Out;
}

const TCHAR* LexToString(EVaRestSocketFailure Failure)
{
	switch (Failure)
	{
	case EVaRestSocketFailure::None: return TEXT("None");
	case EVaRestSocketFailure::Canceled: return TEXT("Canceled");
	case EVaRestSocketFailure::Shutdown: return TEXT("Shutdown");
	case EVaRestSocketFailure::BadUrl: return TEXT("BadUrl");
	case EVaRestSocketFailure::NotAllowed: return TEXT("NotAllowed");
	case EVaRestSocketFailure::BadHandshake: return TEXT("BadHandshake");
	case EVaRestSocketFailure::ConnectFailed: return TEXT("ConnectFailed");
	case EVaRestSocketFailure::TlsVerification: return TEXT("TlsVerification");
	case EVaRestSocketFailure::ConnectTimeout: return TEXT("ConnectTimeout");
	case EVaRestSocketFailure::Redirect: return TEXT("Redirect");
	case EVaRestSocketFailure::UpgradeRefused: return TEXT("UpgradeRefused");
	case EVaRestSocketFailure::NoUpgradeResponse: return TEXT("NoUpgradeResponse");
	case EVaRestSocketFailure::BadUpgradeResponse: return TEXT("BadUpgradeResponse");
	case EVaRestSocketFailure::ProtocolMismatch: return TEXT("ProtocolMismatch");
	case EVaRestSocketFailure::MessageTooBig: return TEXT("MessageTooBig");
	case EVaRestSocketFailure::InvalidUtf8: return TEXT("InvalidUtf8");
	case EVaRestSocketFailure::HeartbeatTimeout: return TEXT("HeartbeatTimeout");
	case EVaRestSocketFailure::IdleTimeout: return TEXT("IdleTimeout");
	case EVaRestSocketFailure::Unknown: return TEXT("Unknown");
	default: return TEXT("?");
	}
}

const TCHAR* LexToString(EVaRestSocketCloseBy By)
{
	switch (By)
	{
	case EVaRestSocketCloseBy::Client: return TEXT("Client");
	case EVaRestSocketCloseBy::Server: return TEXT("Server");
	case EVaRestSocketCloseBy::Transport: return TEXT("Transport");
	case EVaRestSocketCloseBy::Heartbeat: return TEXT("Heartbeat");
	case EVaRestSocketCloseBy::Timeout: return TEXT("Timeout");
	default: return TEXT("?");
	}
}

const TCHAR* LexToString(EVaRestSocketEventType Type)
{
	switch (Type)
	{
	case EVaRestSocketEventType::Open: return TEXT("Open");
	case EVaRestSocketEventType::Message: return TEXT("Message");
	case EVaRestSocketEventType::Pong: return TEXT("Pong");
	case EVaRestSocketEventType::Drained: return TEXT("Drained");
	case EVaRestSocketEventType::Closed: return TEXT("Closed");
	case EVaRestSocketEventType::Error: return TEXT("Error");
	default: return TEXT("?");
	}
}
