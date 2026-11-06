// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioEmitWithAck.h"
#include "VaRestSioAckState.h"
#include "VaRestSioState.h"
#include "VaRestSioLog.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Sio
{
static FVaRestError AckFailure(EVaRestErrorKind Kind, FString Text)
{ FVaRestError Out; Out.Kind = Kind; Out.Message = MoveTemp(Text); Out.bRetryable = Kind == EVaRestErrorKind::Connection || Kind == EVaRestErrorKind::Timeout; return Out; }
FLogicalAck::~FLogicalAck()
{ check(IsInGameThread()); Control->bActive.store(false, std::memory_order_release); }
void FLogicalAck::Retire()
{
	check(IsInGameThread()); if (bRetired) return; bRetired = true;
	Control->bActive.store(false, std::memory_order_release);
	{ FScopeLock Lock(&Control->Mutex); Control->Native.Cancel(); Control->Native = {}; }
	Success = nullptr; Error = nullptr; ReplyTransform = nullptr; Frames.Reset(); Codec.Reset();
	if (auto* View = State.Get(); View && !View->bRetired) View->Acks.Remove(Slot);
	auto Retired = MoveTemp(Cleanup); if (Retired) Retired();
}
void FLogicalAck::Failed(FVaRestError Failure)
{
	check(IsInGameThread()); if (bRetired) return;
	const bool bCanceled = Control->bCancelRequested.load(std::memory_order_acquire);
	if (!bCanceled && (!bOwned || Owner.IsValid()) && Failure.Kind == EVaRestErrorKind::Timeout)
		if (const auto* View = State.Get()) if (const auto Stage = View->Stage.Pin()) Stage->RecordAckTimeout(Namespace);
	if (!bCanceled && (!bOwned || Owner.IsValid()) && Retries > 0 && (Failure.Kind == EVaRestErrorKind::Timeout || Failure.Kind == EVaRestErrorKind::Connection))
	{ --Retries; Start(); return; }
	if (bCanceled) Failure = AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO ack canceled"));
	auto Retired = MoveTemp(Cleanup); ON_SCOPE_EXIT { if (Retired) Retired(); };
	auto Callback = MoveTemp(Error); const bool bNotify = !bOwned || Owner.IsValid(); Retire();
	if (bNotify && Callback) Callback(Failure);
}
void FLogicalAck::Replied(const FVaRestSocketMessage& Message, const FVaRestJson& Value)
{
	check(IsInGameThread()); if (bRetired) return;
	if (Control->bCancelRequested.load(std::memory_order_acquire)) { Failed(AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO ack canceled"))); return; }
	if (bOwned && !Owner.IsValid()) { Retire(); return; }
	auto Reply = FVaRestSioEvent::FromMessage(Message); Reply.Name = Event; Reply.Args = Value;
	auto Retired = MoveTemp(Cleanup); ON_SCOPE_EXIT { if (Retired) Retired(); };
	auto Callback = MoveTemp(Success); Retire(); if (Callback) Callback(Reply);
}
void FLogicalAck::Start()
{
	check(IsInGameThread()); if (bRetired) return;
	if (bOwned && !Owner.IsValid()) { Retire(); return; }
	if (Control->bCancelRequested.load(std::memory_order_acquire)) { Failed(AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO ack canceled"))); return; }
	if (PreparationFailure.IsSet()) { Failed(PreparationFailure.GetValue()); return; }
	auto NativeSocket = Socket.Pin();
	if (!NativeSocket.IsValid() || !Codec) { Failed(AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO scope is no longer available"))); return; }
	const auto Self = AsShared(); const uint64 Attempt = ++Serial;
	auto Request = NativeSocket.RequestEncoded(Event, [Self](const FVaRestSocketId& Id, const IVaRestSocketCodec& CurrentCodec, const FVaRestSocketCodecContext&, const FVaRestJsonConvertOptions&) -> TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>
	{
		if (Self->bRetired || !Self->Codec || CurrentCodec.GetProtocolTag() != TEXT("SocketIO")) return MakeError(AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO encoder retired")));
		TArray<FVaRestSocketFrame> Packet = Self->Retries > 0 ? Self->Frames : MoveTemp(Self->Frames);
		return Self->Codec->WithAckId(MoveTemp(Packet), Id);
	});
	Request.InChannel(Namespace).Timeout(Timeout).Owner(bOwned ? Owner.Get() : nullptr);
	const bool bApplyErrorFirst = bErrorFirst;
	Request.TransformReply([Self, bApplyErrorFirst](const FVaRestSocketMessage& Message, FVaRestJson& Args)
	{
		if (!Args.IsArray()) return AckFailure(EVaRestErrorKind::Decode, TEXT("Socket.IO ack requires an argument array"));
		if (bApplyErrorFirst)
		{
			const auto First = Args.At(0);
			if (First.IsValid() && !First.IsNull())
			{
				auto Failure = AckFailure(EVaRestErrorKind::Server, First.IsString() ? First.AsString() : First.Get(TEXT("message")).AsString());
				if (Failure.Message.IsEmpty()) Failure.Message = First.ToString(); Failure.Body = First;
				const auto Code = First.Get(TEXT("code")); if (Code.IsString()) Failure.Code = Code.AsString(); else if (Code.IsNumber()) Failure.Code = Code.ToString();
				return Failure;
			}
			FVaRestJson SuccessArgs = FVaRestJson::Array(); for (int32 Index = 1; Index < Args.Num(); ++Index) SuccessArgs.Add(Args.At(Index));
			Args = MoveTemp(SuccessArgs);
		}
		if (Self->bRetired || (Self->bOwned && !Self->Owner.IsValid()) || Self->Control->bCancelRequested.load(std::memory_order_acquire))
			return AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO ack scope closed before output preparation"));
		const auto Transform = Self->ReplyTransform;
		if (!Transform) return FVaRestError();
		auto Reply = FVaRestSioEvent::FromMessage(Message); Reply.Name = Self->Event; Reply.Args = Args;
		return Transform(Reply);
	});
	Request.OnReply([Self, Attempt](const FVaRestSocketMessage& Message, const FVaRestJson& Args) { if (Self->Serial == Attempt) Self->Replied(Message, Args); });
	Request.OnError([Self, Attempt](const FVaRestError& Failure) { if (Self->Serial == Attempt) Self->Failed(Failure); });
	Request.OnRetired([Self, Attempt]() { if (!Self->bRetired && Self->Serial == Attempt) Self->Retire(); });
	const auto Handle = Request.Send();
	if (!bRetired && Serial == Attempt)
	{
		{ FScopeLock Lock(&Control->Mutex); Control->Native = Handle; if (Control->bCancelRequested.load(std::memory_order_acquire)) Handle.Cancel(); }
		// Owner/scope refusal may intentionally suppress callbacks before a pending row exists.
		if (!Handle.IsActive()) Retire();
	}
	else Handle.Cancel();
}
} // namespace VaRest::Sio

struct FVaRestSioEmitWithAck::FImpl
{
	FVaRestSioSocket Socket;
	TSharedRef<VaRest::Sio::FLogicalAck> Logical = MakeShared<VaRest::Sio::FLogicalAck>();
	bool bSent = false;
	~FImpl()
	{
		check(IsInGameThread());
		if (!bSent) { if (!Logical->bRetired) UE_LOG(LogVaRestSio, Warning, TEXT("Socket.IO EmitWithAck builder discarded without Send")); Logical->Retire(); }
	}
};
void FVaRestSioEmitWithAck::Discard()
{
	check(IsInGameThread()); if (!Impl || Impl->bSent) return;
	Impl->bSent = true; Impl->Logical->Retire();
}
FVaRestSioEmitWithAck::FVaRestSioEmitWithAck(FVaRestSioSocket Socket, FString Namespace, FString Event, FVaRestSocketArgumentsWriter Arguments)
	: Impl(MakeUnique<FImpl>())
{
	check(IsInGameThread()); Impl->Socket = MoveTemp(Socket); auto& Logical = *Impl->Logical;
	Logical.Socket = Impl->Socket.Native.AsWeak(); Logical.Event = MoveTemp(Event); Logical.Namespace = MoveTemp(Namespace);
	auto* View = Impl->Socket.StateOnGT();
	if (!View || View->bRetired) { Logical.PreparationFailure = VaRest::Sio::AckFailure(EVaRestErrorKind::Canceled, TEXT("Socket.IO scope is unavailable")); return; }
	const TStrongObjectPtr<UVaRestSioState> Pin(View);
	if (View->NextAckSlot == MAX_uint64) { Logical.PreparationFailure = VaRest::Sio::AckFailure(EVaRestErrorKind::Config, TEXT("Socket.IO logical ack generation exhausted")); return; }
	Logical.State = View; Logical.Slot = ++View->NextAckSlot; Logical.Timeout = View->AckTimeout; Logical.Codec = View->Codec;
	if (Logical.Namespace.IsEmpty()) Logical.Namespace = View->Options.DefaultNamespace;
	View->Acks.Add(Logical.Slot, Impl->Logical);
	auto Encoded = Impl->Socket.Native.EncodeOnly(Logical.Namespace, [&](const IVaRestSocketCodec& Codec, const FVaRestSocketCodecContext& Context, const FVaRestJsonConvertOptions& Convert, FStringView Channel) -> TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>
	{
		if (Codec.GetProtocolTag() != TEXT("SocketIO")) return MakeError(VaRest::Sio::AckFailure(EVaRestErrorKind::Config, TEXT("Socket.IO encoder unavailable")));
		auto Unused = [](auto&, bool, TConstArrayView<FString>, FVaRestJsonError&) { return false; };
		FVaRestSocketPayloadWriter Payload(Unused, false); Payload.Arguments.Emplace(Arguments); Payload.ConvertOptions = &Convert;
		return Codec.Encode(Channel, Logical.Event, Payload, {}, Context);
	});
	if (Logical.bRetired) return;
	if (Encoded.HasValue()) Logical.Frames = Encoded.StealValue(); else Logical.PreparationFailure = Encoded.StealError();
}
FVaRestSioEmitWithAck::FVaRestSioEmitWithAck(FVaRestSioEmitWithAck&& Other) : Impl(MoveTemp(Other.Impl)) { check(IsInGameThread()); }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::operator=(FVaRestSioEmitWithAck&& Other)
{ check(IsInGameThread()); if (this != &Other) Impl = MoveTemp(Other.Impl); return *this; }
FVaRestSioEmitWithAck::~FVaRestSioEmitWithAck() { check(IsInGameThread() || !Impl); }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::Timeout(double Seconds)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->Timeout = Seconds; return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::ErrorFirst(bool bEnabled)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->bErrorFirst = bEnabled; return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::Retry(int32 Count)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->Retries = FMath::Clamp(Count, 0, 1024); return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::Owner(const UObject* Owner)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) { Impl->Logical->Owner = Owner; Impl->Logical->bOwned = Owner != nullptr; } return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::OnSuccess(TFunction<void(const FVaRestSioEvent&)> Handler)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->Success = MoveTemp(Handler); return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::OnError(TFunction<void(const FVaRestError&)> Handler)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->Error = MoveTemp(Handler); return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::TransformReply(TFunction<FVaRestError(const FVaRestSioEvent&)> Transform)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->ReplyTransform = MoveTemp(Transform); return *this; }
FVaRestSioEmitWithAck& FVaRestSioEmitWithAck::OnRetired(TFunction<void()> Cleanup)
{ check(IsInGameThread()); if (Impl && !Impl->Logical->bRetired) Impl->Logical->Cleanup = MoveTemp(Cleanup); return *this; }
FVaRestSocketWeak FVaRestSioEmitWithAck::NativeSocketOnGT() const
{ check(IsInGameThread()); return Impl ? Impl->Logical->Socket : FVaRestSocketWeak(); }
TFunction<bool()> FVaRestSioEmitWithAck::OwnerGuardOnGT() const
{
	check(IsInGameThread()); if (!Impl) return []() { return false; };
	const TWeakPtr<VaRest::Sio::FLogicalAck> Weak = Impl->Logical;
	return [Weak]() { const auto Held = Weak.Pin(); return Held && (!Held->bOwned || Held->Owner.IsValid()); };
}
FVaRestSioAckHandle FVaRestSioEmitWithAck::Send()
{
	check(IsInGameThread()); if (!Impl || Impl->bSent) return {};
	Impl->bSent = true; const auto Logical = Impl->Logical; FVaRestSioAckHandle Result; Result.Control = Logical->Control;
	Logical->bStarted = true; Logical->Start(); Impl.Reset(); return Result;
}
bool FVaRestSioAckHandle::IsActive() const
{ const auto Held = Control.Pin(); return Held && Held->bActive.load(std::memory_order_acquire) && !Held->bCancelRequested.load(std::memory_order_acquire); }
void FVaRestSioAckHandle::Cancel() const
{
	if (const auto Held = Control.Pin()) { Held->bCancelRequested.store(true, std::memory_order_release); FScopeLock Lock(&Held->Mutex); Held->Native.Cancel(); }
}
FVaRestSioEmitWithAck FVaRestSioSocket::MakeAckBuilder(FStringView Namespace, FStringView Event, FVaRestSocketArgumentsWriter Arguments) const
{ check(IsInGameThread()); return FVaRestSioEmitWithAck(*this, FString(Namespace), FString(Event), Arguments); }
FVaRestSioSocket FVaRestSioSocket::WithAckTimeout(double Seconds)
{
	check(IsInGameThread()); if (auto* View = StateOnGT())
	{ if (FMath::IsFinite(Seconds) && Seconds >= 0.0) View->AckTimeout = Seconds; else View->ReportConfig(TEXT("Socket.IO ack timeout must be finite and nonnegative")); } return *this;
}
