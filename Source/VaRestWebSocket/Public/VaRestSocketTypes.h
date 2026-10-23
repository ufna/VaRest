// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestSocketTypes.generated.h"

/** A data message's kind (RFC 6455 5.6). W3's FVaRestSocketMessage carries it to Blueprints: it freezes with W5 */
UENUM(BlueprintType)
enum class EVaRestSocketFrameKind : uint8
{
	Text,
	Binary
};

/**
 * Who ended a connection (WebSocket spec 2.5): the transport knows, so classification never infers it from the code
 * (E14). W2's FVaRestSocketClose::By: it freezes with W5.
 */
UENUM(BlueprintType)
enum class EVaRestSocketCloseBy : uint8
{
	/** The app's Close(), or its last handle released */
	Client,
	/** The server sent the first close frame */
	Server,
	/** A connect failure, an end without a close frame (1006), or VaRest's transport closing for a protocol reason (1007, 1009) */
	Transport,
	/** W1b: no pong and no inbound bytes within PingTimeout, or a stage's idle deadline */
	Heartbeat,
	/** ConnectTimeout */
	Timeout
};

/**
 * Why a connection never opened, or why the transport ended it (WebSocket spec 2.2, 2.3, 3.4, 22 A2). W2 classifies on
 * it (who, code, failure); it freezes with W5.
 */
UENUM(BlueprintType)
enum class EVaRestSocketFailure : uint8
{
	/** A close the app or the server asked for, or a drop (1006): Code and By say which */
	None,
	/** Closed or released before the open */
	Canceled,
	/** The transport shut down (engine exit, a fork) or could not start */
	Shutdown,
	/** Not ws://, wss:// or (not in Shipping) wss+insecure://; no host, a bad port, user info, a fragment, a non-ASCII host */
	BadUrl,
	/** The engine's URL request filter refused it ([Online.HttpManager <scheme>] in the engine ini) */
	NotAllowed,
	/** A header, a subprotocol or the origin VaRest refuses, or headers that do not fit the 32 KiB handshake buffer */
	BadHandshake,
	/** DNS, TCP, a proxy, or a TLS handshake failure other than the certificate check: no HTTP status arrived */
	ConnectFailed,
	/** The server's certificate was refused (the chain, the name or a pinned key); Detail names the X509 error and depth */
	TlsVerification,
	/** ConnectTimeout passed (VaRest's kill), or libwebsockets' own handshake timeout */
	ConnectTimeout,
	/** The upgrade was answered with a redirect, which VaRest never follows (ruling Q2) */
	Redirect,
	/** An HTTP status other than 101 (HttpStatus) */
	UpgradeRefused,
	/** The upgrade went out and the connection ended before any status line (spec 22 A2) */
	NoUpgradeResponse,
	/** A 101 a client must refuse: Sec-WebSocket-Accept, Upgrade or Connection wrong or missing */
	BadUpgradeResponse,
	/** The server selected a subprotocol that was not offered */
	ProtocolMismatch,
	/** An inbound message over MaxMessageBytes: VaRest closed with 1009 */
	MessageTooBig,
	/** A text message that is not UTF-8: VaRest closed with 1007 (RFC 6455 8.1) */
	InvalidUtf8,
	/** W1b: no pong and no inbound bytes within PingTimeout */
	HeartbeatTimeout,
	/** W1b: no inbound data frame within a stage's idle deadline */
	IdleTimeout,
	/** Any other libwebsockets failure */
	Unknown
};

namespace VaRest::WebSocket
{
	/**
	 * Bytes before a frame's payload that the transport writes the frame header into: LWS_PRE, 16 on every target
	 * (libwebsockets.h:4773: _LWS_PAD(4 + 10 + 2) on a 4- or 8-byte pointer); VaRestLwsService.cpp holds it with a static_assert
	 */
	inline constexpr int32 FrameHeadroom = 16;
	/** RFC 6455 5.5: a control frame carries at most 125 bytes, so a close reason at most 123 after its code */
	inline constexpr int32 MaxCloseReasonBytes = 123;
}

/**
 * One outbound message (WebSocket spec 2.4, 22 SW-4): Storage[Offset, Offset + Length) is the payload, and the Offset bytes
 * before it are headroom the transport writes the frame header into, so an encoder can reserve a larger gap and write a
 * variable-length prefix after the body. libwebsockets masks a client payload in place (RFC 6455 5.3) and the transport
 * writes fragment headers into the bytes before each fragment (plan decision 6): a frame is consumed by sending, so move
 * it in. Any thread; a frame is not shared between threads.
 */
struct VARESTWEBSOCKET_API FVaRestSocketFrame
{
	EVaRestSocketFrameKind Kind = EVaRestSocketFrameKind::Text;
	TArray<uint8> Storage;
	int32 Offset = VaRest::WebSocket::FrameHeadroom;
	int32 Length = 0;

	/** UTF-8 text, copied once after FrameHeadroom; not validated (the caller's bytes are the caller's). Past 2 GiB: an invalid frame */
	static FVaRestSocketFrame Text(FUtf8StringView InText);
	/** Text as UTF-8; an unpaired surrogate becomes U+FFFD (VaRest::Json::WriteUtf8). Past 2 GiB: an invalid frame */
	static FVaRestSocketFrame Text(FStringView InText);
	/** The bytes, copied once after FrameHeadroom. Past 2 GiB: an invalid frame */
	static FVaRestSocketFrame Binary(TConstArrayView<uint8> InBytes);
	/** An encoder's buffer as it is: unset unless InOffset >= FrameHeadroom and InOffset + InLength <= its size */
	static TOptional<FVaRestSocketFrame> FromStorage(EVaRestSocketFrameKind InKind, TArray<uint8>&& InStorage, int32 InOffset, int32 InLength);

	bool IsValid() const
	{
		return Offset >= VaRest::WebSocket::FrameHeadroom && Length >= 0 && static_cast<int64>(Offset) + Length <= Storage.Num();
	}

	TConstArrayView<uint8> GetPayload() const
	{
		return IsValid() ? TConstArrayView<uint8>(Storage.GetData() + Offset, Length) : TConstArrayView<uint8>();
	}
};

/** How a connection ended (WebSocket spec 2.5, 14), carried by its Closed or Error event */
struct FVaRestSocketEnd
{
	/** The close code: the peer's (1005 when it gave none), ours, 1006 for an end without a close frame; 0 when it never opened */
	int32 Code = 0;
	/** The close reason, decoded as UTF-8 (ill-formed bytes as U+FFFD) */
	FString Reason;
	/**
	 * The peer's close was echoed, or a close VaRest started ended before both its configured close deadline and LWS's
	 * five-second acknowledgement cutoff. LWS 3.0 exposes no callback for the reply itself (plan decision 4,
	 * [close.ours_sequence]), so this inferred clean end cannot distinguish an echo from a TCP end.
	 */
	bool bWasClean = false;
	EVaRestSocketCloseBy By = EVaRestSocketCloseBy::Transport;
	EVaRestSocketFailure Failure = EVaRestSocketFailure::None;
	/** UpgradeRefused: the upgrade's HTTP status. 0 elsewhere, a redirect included: LWS 3.0 follows it before any callback sees it */
	int32 HttpStatus = 0;
	/** For logs and messages: libwebsockets' error string, the X509 error, which timeout fired, what VaRest refused */
	FString Detail;
};

enum class EVaRestSocketEventType : uint8
{
	Open,
	Message,
	/** W1b */
	Pong,
	Drained,
	Closed,
	Error
};

/** One item of a connection's ordered event queue (WebSocket spec 2.1, 7.1) */
struct FVaRestSocketEvent
{
	EVaRestSocketEventType Type = EVaRestSocketEventType::Message;
	/** Message */
	EVaRestSocketFrameKind Kind = EVaRestSocketFrameKind::Text;
	/** Message: always true in 2.0 -- the transport queues whole messages (spec 2.1), so a streaming mode needs no format change */
	bool bFinal = true;
	/** Message: the payload, with one byte of slack (Max() > Num()) so FVaRestJson::ParseOwned takes it without a copy (core 2.1) */
	TArray<uint8> Bytes;
	/** Open: the subprotocol the server selected; empty when none */
	FString Protocol;
	/** Open: seconds from the connect to the open; Pong (W1b): the round trip */
	double Seconds = 0.0;
	/** Closed and Error */
	FVaRestSocketEnd End;
	/** FPlatformTime::Seconds() when the transport queued it */
	double Time = 0.0;

	bool IsTerminal() const { return Type == EVaRestSocketEventType::Closed || Type == EVaRestSocketEventType::Error; }
};

namespace VaRest::WebSocket
{
	/** Codes a close frame may carry, sent or received: 1000-1003, 1007-1014, 3000-4999 (RFC 6455 7.4; spec 2.5, 3.5) */
	VARESTWEBSOCKET_API bool IsValidCloseCode(int32 Code);

	/** Spec 3.5: Code when it may be sent, else 1000 (bOutReplaced) */
	VARESTWEBSOCKET_API int32 SanitizeCloseCode(int32 Code, bool& bOutReplaced);

	/** Reason as UTF-8, at most MaxCloseReasonBytes, cut on a code-point boundary (bOutTruncated) */
	VARESTWEBSOCKET_API TArray<uint8> CloseReasonBytes(FStringView Reason, bool& bOutTruncated);

	/** A peer's close, as spec 2.5 reads it */
	struct FPeerClose
	{
		int32 Code = 1005;
		/** The two bytes as received; -1 when the payload had fewer */
		int32 ReceivedCode = -1;
		FString Reason;
		/** One byte: no room for a code (1002) */
		bool bMalformed = false;
		/** A code a peer must not send, reported as 1005 */
		bool bCodeNormalised = false;
		/** The reason was not UTF-8 */
		bool bReasonRepaired = false;
	};

	/** Spec 2.5: 0 bytes -> 1005, 1 byte -> 1002, a code IsValidCloseCode refuses -> 1005, the reason as UTF-8 (ill-formed bytes U+FFFD) */
	VARESTWEBSOCKET_API FPeerClose ParsePeerClose(TConstArrayView<uint8> Payload);
}

VARESTWEBSOCKET_API const TCHAR* LexToString(EVaRestSocketFailure Failure);
VARESTWEBSOCKET_API const TCHAR* LexToString(EVaRestSocketCloseBy By);
VARESTWEBSOCKET_API const TCHAR* LexToString(EVaRestSocketEventType Type);
