// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioEvent.h"

EVaRestSendResult FVaRestSioAck::SendArray(const FVaRestJson& Args) const
{
	if (!Args.IsArray()) return EVaRestSendResult::Rejected;
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> WithinBudget)
	{
		VaRest::Sio::FArgumentWriter Into{Writer, Options, Error, WithinBudget};
		for (int32 Index = 0; Index < Args.Num(); ++Index) if (!Into.Value(Args.At(Index))) return false;
		return true;
	};
	return Message.ReplyArguments(Write);
}
TConstArrayView<uint8> FVaRestSioEvent::Attachment(int32 Index) const
{
	return Native.Attachments.IsValidIndex(Index) && Native.Attachments[Index] ? MakeArrayView(*Native.Attachments[Index]) : TConstArrayView<uint8>();
}
FVaRestSioEvent FVaRestSioEvent::FromMessage(FVaRestSocketMessage Message, FString RecoveryOffset)
{
	FVaRestSioEvent Out; Out.Namespace = Message.Channel; Out.Name = Message.Route; Out.Args = Message.Data();
	Out.NumAttachments = Message.Attachments.Num(); Out.Offset = MoveTemp(RecoveryOffset); Out.Ack = FVaRestSioAck::FromMessage(Message);
	Out.Native = MoveTemp(Message); return Out;
}
