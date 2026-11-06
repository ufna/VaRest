// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioTypes.h"

FVaRestSioCodecOptions FVaRestSioLimits::ToCodecOptions() const
{
	FVaRestSioCodecOptions Out; Out.MaxAttachments = MaxAttachments; Out.MaxMessageBytes = MaxMessageBytes;
	Out.MaxPacketBytes = MaxPacketBytes; Out.MaxPendingInboundBytes = MaxPendingInboundBytes;
	const int64 Local = FMath::Min(MaxOutboundFrameBytes, MaxSendBytes);
	Out.MaxOutboundFrameBytes = bOverrideFallbackPayloadLimit ? Local : FMath::Min<int64>(1000000, Local); return Out;
}
bool FVaRestSioLimits::Validate(FString& Error) const
{
	if (MaxOutboundFrameBytes < 1 || MaxOutboundFrameBytes > MAX_int32 - VaRest::WebSocket::FrameHeadroom || MaxSendBytes < 1
		|| MaxSendBytes > MAX_int32 - VaRest::WebSocket::FrameHeadroom || MaxBufferedBytes < 0 || SendHighWater < 0
		|| MaxPendingRequests < 0 || MaxChannels < 1 || MaxChannels > 4096 || !FMath::IsFinite(ReadyTimeout) || ReadyTimeout < 0)
	{ Error = TEXT("Invalid Socket.IO connection, send or readiness limit"); return false; }
	return FVaRestSioCodec::ValidateOptions(ToCodecOptions(), Error);
}
