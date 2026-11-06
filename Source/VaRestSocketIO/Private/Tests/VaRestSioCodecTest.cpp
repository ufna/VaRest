// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestSioCodec.h"
#include "VaRestSioArguments.h"
#include "Async/Async.h"
#include "Misc/AutomationTest.h"

namespace VaRestSioCodecTest
{
FVaRestSocketInboundFrame Text(FStringView Value,bool AutoReplied=false)
{
 const FTCHARToUTF8 Utf8(Value.GetData(),Value.Len()); FVaRestSocketInboundFrame Out;
 Out.Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()); Out.bAutoReplied=AutoReplied; return Out;
}
FVaRestSocketInboundFrame Binary(TArray<uint8> Bytes)
{ FVaRestSocketInboundFrame Out; Out.Kind=EVaRestSocketFrameKind::Binary; Out.Bytes=MoveTemp(Bytes); return Out; }
FString Wire(const FVaRestSocketFrame& Frame)
{ const auto Bytes=Frame.GetPayload(); return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num()); }
struct FInput
{
 TArray<FVaRestSocketInboundFrame> Frames;
 FVaRestSioCodecOptions Options;
 bool RequireOpen=false;
};
struct FObservation
{
 EVaRestSocketDecode Status=EVaRestSocketDecode::NeedMore;
 EVaRestSocketMessageKind Kind=EVaRestSocketMessageKind::Message;
 int32 Code=0;
 FString Reason,Channel,Route,Id;
 FName Tag;
 bool HasMessage=false,HasId=false,Numeric=false,Interleaved=false,AutoReplied=false;
 int64 Number=0;
 FVaRestJson Json,Data;
 TArray<uint8> Raw;
 TArray<TArray<uint8>> Attachments;
};
struct FResults { bool Worker=false; TArray<TArray<FObservation>> Cases; };
FResults Decode(TArray<FInput> Inputs)
{
 // A decoder's creation, use and destruction belong to the same native worker.
 // No test object, UObject, client, borrowed writer or GTK callback enters this closure.
 return Async(EAsyncExecution::Thread,[Inputs=MoveTemp(Inputs)]() mutable
 {
  FResults Out; Out.Worker=!IsInGameThread();
  for (auto& Input:Inputs)
  {
   FVaRestSioCodec Codec(Input.Options); if (Input.RequireOpen) Codec.RequireEngineOpen();
   auto Decoder=Codec.MakeDecoder(); auto& Rows=Out.Cases.AddDefaulted_GetRef();
   for (auto& Frame:Input.Frames)
   {
    auto Result=Decoder->Decode(MoveTemp(Frame),{}); auto& Row=Rows.AddDefaulted_GetRef();
    Row.Status=Result.Status; Row.Code=Result.CloseCode; Row.Reason=MoveTemp(Result.Reason); Row.Interleaved=Result.bInterleavedControl;
    if (Result.Message.IsSet())
    {
     const auto& Message=Result.Message.GetValue(); Row.HasMessage=true;
     Row.Kind=Message.Kind; Row.Channel=Message.Channel; Row.Route=Message.Route; Row.Id=Message.Id;
     Row.Tag=Message.ProtocolTag; Row.HasId=Message.bHasId; Row.Numeric=Message.bNumericId; Row.Number=Message.NumericId;
     Row.AutoReplied=Message.bAutoReplied; Row.Json=Message.Json(); Row.Data=Message.Data(); Row.Raw=TArray<uint8>(Message.Bytes());
     for (const auto& Attachment:Message.Attachments) if (Attachment) Row.Attachments.Add(*Attachment);
    }
   }
  }
  return Out;
 }).Get();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioWireCodecTest,"VaRest.SocketIO.Codec.Wire",
 EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestSioWireCodecTest::RunTest(const FString& Parameters)
{
 using namespace VaRestSioCodecTest;
 FInput Valid; Valid.Frames={Text(TEXT("40/room,{\"sid\":\"public\"}")),Text(TEXT("42[\"hello\",7]")),
  Text(TEXT("42/room,9007199254740991[\"hello\",8]")),Text(TEXT("43/room,0[]")),Text(TEXT("42[7,1]")),Text(TEXT("2probe"),true),Text(TEXT("41/room,"))};
 FInput Open; Open.RequireOpen=true; Open.Frames={Text(TEXT("0{\"sid\":\"private\"}")),Text(TEXT("42[\"hello\",1]"))};
 FInput WrongFirst; WrongFirst.RequireOpen=true; WrongFirst.Frames={Text(TEXT("42[\"hello\",1]")),Text(TEXT("0{}"))};
 auto Good=Decode({MoveTemp(Valid),MoveTemp(Open),MoveTemp(WrongFirst)});
 TestTrue(TEXT("decoder work runs off GT"),Good.Worker);
 if (!TestEqual(TEXT("valid packet count"),Good.Cases[0].Num(),7)) return false;
 const auto& V=Good.Cases[0];
 for (const auto& Row:V) TestTrue(TEXT("valid wire delivers one native message"),Row.Status==EVaRestSocketDecode::Message && Row.HasMessage);
 TestEqual(TEXT("namespace CONNECT is a control packet"),V[0].Tag,FName(TEXT("sio:CONNECT")));
 TestEqual(TEXT("default namespace"),V[1].Channel,FString(TEXT("/")));
 TestEqual(TEXT("event route"),V[1].Route,FString(TEXT("hello"))); TestEqual(TEXT("event argument excludes route"),V[1].Data[0].AsInt(),7);
 TestTrue(TEXT("last exact JavaScript id stays int64"),V[2].HasId && V[2].Numeric && V[2].Number==9007199254740991LL && V[2].Kind==EVaRestSocketMessageKind::Request);
 TestEqual(TEXT("explicit namespace"),V[2].Channel,FString(TEXT("/room")));
 TestTrue(TEXT("ack id zero is present and retains an empty array"),V[3].HasId && V[3].Numeric && V[3].Number==0 && V[3].Kind==EVaRestSocketMessageKind::Reply && V[3].Data.IsArray() && V[3].Data.Num()==0);
 TestEqual(TEXT("numeric event name has its wire spelling"),V[4].Route,FString(TEXT("7")));
 TestTrue(TEXT("native ping carries the service reply marker"),V[5].Tag==FName(TEXT("eio:ping")) && V[5].AutoReplied && V[5].Data.AsString()==TEXT("probe"));
 TestEqual(TEXT("namespace DISCONNECT control"),V[6].Tag,FName(TEXT("sio:DISCONNECT")));
 TestTrue(TEXT("required OPEN permits subsequent codec data"),Good.Cases[1][0].Tag==FName(TEXT("eio:open")) && Good.Cases[1][1].Status==EVaRestSocketDecode::Message);
 TestTrue(TEXT("wrong first packet is classified for the session stage, then poisons decoder"),Good.Cases[2][0].Tag==FName(TEXT("eio:invalidfirst")) && Good.Cases[2][1].Status==EVaRestSocketDecode::ProtocolError);

 const TCHAR* Bad[]={TEXT(""),TEXT("7"),TEXT("47"),TEXT("42[]"),TEXT("42{}"),TEXT("42[\"connect\",1]"),
  TEXT("43/room,[1]"),TEXT("43/room,1{}"),TEXT("42/room,9007199254740992[\"x\"]"),TEXT("429999999999999999999999999999[\"x\"]"),
  TEXT("40/room,1{}"),TEXT("42/room[\"x\"]"),TEXT("42/room?q,[\"x\"]"),TEXT("40[]"),TEXT("41{}"),TEXT("42[\"x\","),TEXT("450-[\"x\"]"),TEXT("451[\"x\"]")};
 TArray<FInput> Invalid;
 for (const auto* WireText:Bad) { FInput Input; Input.Frames={Text(WireText),Text(TEXT("42[\"valid\",1]"))}; Invalid.Add(MoveTemp(Input)); }
 FInput Utf8; auto InvalidUtf8=Text(TEXT("42[\"x\"]")); InvalidUtf8.Bytes[4]=0xff; Utf8.Frames={MoveTemp(InvalidUtf8),Text(TEXT("2"))}; Invalid.Add(MoveTemp(Utf8));
 auto Refused=Decode(MoveTemp(Invalid)); TestTrue(TEXT("negative corpus stays native"),Refused.Worker);
 TestEqual(TEXT("all malformed cases observed"),Refused.Cases.Num(),static_cast<int32>(UE_ARRAY_COUNT(Bad))+1);
 for (int32 I=0;I<Refused.Cases.Num();++I)
 {
  const auto& Rows=Refused.Cases[I];
  TestTrue(FString::Printf(TEXT("malformed case %d refuses without a message"),I),Rows[0].Status==EVaRestSocketDecode::ProtocolError && Rows[0].Code==1002 && !Rows[0].HasMessage && !Rows[0].Reason.IsEmpty());
  TestTrue(FString::Printf(TEXT("malformed case %d cannot resume on this connection"),I),Rows[1].Status==EVaRestSocketDecode::ProtocolError && !Rows[1].HasMessage);
 }
 AddInfo(TEXT("Standalone codec grammar and required-first-OPEN classification; OPEN negotiation is owned by the session stage."));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioBinaryCodecTest,"VaRest.SocketIO.Codec.Binary",
 EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestSioBinaryCodecTest::RunTest(const FString& Parameters)
{
 using namespace VaRestSioCodecTest;
 const FString Header=TEXT("452-/room,7[\"bin\",{\"_placeholder\":true,\"num\":1},{\"_placeholder\":true,\"num\":0}]");
 FInput Whole; Whole.Frames={Text(Header),Binary({0,255}),Text(TEXT("2heartbeat"),true),Binary({0,127,255})};
 FInput PacketLimit; PacketLimit.Options.MaxMessageBytes=1024; PacketLimit.Options.MaxPacketBytes=Text(Header).Bytes.Num()+2;
 PacketLimit.Options.MaxPendingInboundBytes=PacketLimit.Options.MaxPacketBytes*2;
 PacketLimit.Frames={Text(Header),Binary({0,255}),Binary({1}),Text(TEXT("2"))};
 FInput FrameLimit; FrameLimit.Options.MaxMessageBytes=4; FrameLimit.Frames={Text(TEXT("42[\"x\"]")),Text(TEXT("2"))};
 FInput Interleaved; Interleaved.Frames={Text(Header),Text(TEXT("42[\"other\"]"))};
 FInput Orphan; Orphan.Frames={Binary({0})};
 FInput Placeholder; Placeholder.Frames={Text(TEXT("451-[\"bin\",{\"_placeholder\":true,\"num\":1}]")),Binary({3})};
 FInput Count; Count.Options.MaxAttachments=1; Count.Frames={Text(Header)};
 FInput Empty; Empty.Frames={Text(TEXT("451-[\"bin\",{\"_placeholder\":true,\"num\":0}]")),Binary({})};
 auto Results=Decode({MoveTemp(Whole),MoveTemp(PacketLimit),MoveTemp(FrameLimit),MoveTemp(Interleaved),MoveTemp(Orphan),MoveTemp(Placeholder),MoveTemp(Count),MoveTemp(Empty)});
 TestTrue(TEXT("binary decoder lifecycle stays on worker"),Results.Worker);
 const auto& W=Results.Cases[0];
 TestTrue(TEXT("header and first attachment wait for the complete packet"),W[0].Status==EVaRestSocketDecode::NeedMore && W[1].Status==EVaRestSocketDecode::NeedMore);
 TestTrue(TEXT("ping interleaves without releasing the pending packet"),W[2].Status==EVaRestSocketDecode::Message && W[2].Interleaved && W[2].AutoReplied && W[2].Tag==FName(TEXT("eio:ping")));
 const auto& Done=W[3];
 if (!TestTrue(TEXT("second attachment completes exactly one packet"),Done.Status==EVaRestSocketDecode::Message && Done.Attachments.Num()==2)) return false;
 TestTrue(TEXT("attachments preserve counted NUL/high bytes and wire order"),Done.Attachments[0]==TArray<uint8>({0,255}) && Done.Attachments[1]==TArray<uint8>({0,127,255}));
 TestTrue(TEXT("placeholder order is independent of attachment order"),Done.Data[0].Get(TEXT("num")).AsInt()==1 && Done.Data[1].Get(TEXT("num")).AsInt()==0);
 TestTrue(TEXT("binary request correlation survives assembly"),Done.HasId && Done.Number==7 && Done.Kind==EVaRestSocketMessageKind::Request && Done.Channel==TEXT("/room"));
 TestTrue(TEXT("whole packet budget counts header plus attachments"),Results.Cases[1][1].Status==EVaRestSocketDecode::NeedMore && Results.Cases[1][2].Status==EVaRestSocketDecode::ProtocolError && Results.Cases[1][2].Code==1009);
 TestTrue(TEXT("overflow poisons subsequent input"),Results.Cases[1][3].Status==EVaRestSocketDecode::ProtocolError);
 TestTrue(TEXT("frame budget is independently enforced"),Results.Cases[2][0].Status==EVaRestSocketDecode::ProtocolError && Results.Cases[2][0].Code==1009);
 TestTrue(TEXT("ordinary text cannot interrupt attachments"),Results.Cases[3][1].Status==EVaRestSocketDecode::ProtocolError);
 TestTrue(TEXT("orphan binary refuses"),Results.Cases[4][0].Status==EVaRestSocketDecode::ProtocolError);
 TestTrue(TEXT("out-of-range placeholder refuses after complete assembly"),Results.Cases[5][0].Status==EVaRestSocketDecode::NeedMore && Results.Cases[5][1].Status==EVaRestSocketDecode::ProtocolError);
 TestTrue(TEXT("attachment-count bound refuses before payload storage"),Results.Cases[6][0].Status==EVaRestSocketDecode::ProtocolError);
 TestTrue(TEXT("empty binary attachment remains a valid counted value"),Results.Cases[7][1].Status==EVaRestSocketDecode::Message && Results.Cases[7][1].Attachments.Num()==1 && Results.Cases[7][1].Attachments[0].IsEmpty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioEncoderCodecTest,"VaRest.SocketIO.Codec.Encoder",
 EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestSioEncoderCodecTest::RunTest(const FString& Parameters)
{
 using namespace VaRestSioCodecTest;
 FVaRestSioCodec Codec; FVaRestSocketCodecContext Context; Context.DefaultChannel=TEXT("/room");
 const auto Value=FVaRestJson::From(7);
 auto Write=[&Value](auto& Writer,bool Members,TConstArrayView<FString> Skip,FVaRestJsonError& Error)
 { return VaRest::WebSocket::WriteJsonPayload(Writer,Value,Members,Skip,Error); };
 FVaRestSocketPayloadWriter Payload(Write,false); Payload.Json=&Value;
 auto Encoded=Codec.Encode({},TEXT("value"),Payload,{},Context);
 if (!TestTrue(TEXT("native scalar event encodes"),Encoded.HasValue())) return false;
 const auto Original=Encoded.GetValue(); const FString Before=Wire(Original[0]);
 TestEqual(TEXT("default channel and scalar wire"),Before,FString(TEXT("42/room,[\"value\",7]")));
 auto Acked=Codec.WithAckId(Original,FVaRestSocketId::Numeric(0));
 if (!TestTrue(TEXT("prepared packet accepts numeric zero ACK id"),Acked.HasValue())) return false;
 TestEqual(TEXT("ACK id is inserted before argument array"),Wire(Acked.GetValue()[0]),FString(TEXT("42/room,0[\"value\",7]")));
 TestTrue(TEXT("prepared original bytes and metadata remain immutable"),Wire(Original[0])==Before && Original[0].Metadata->Id.IsEmpty());
 TestTrue(TEXT("ACK metadata and LWS headroom stay valid"),Acked.GetValue()[0].Metadata->Id==TEXT("0") && Acked.GetValue()[0].Metadata->Kind==EVaRestSocketMessageKind::Request && Acked.GetValue()[0].Offset>=VaRest::WebSocket::FrameHeadroom);
 TestFalse(TEXT("an already identified packet cannot gain another ACK id"),Codec.WithAckId(Acked.GetValue(),FVaRestSocketId::Numeric(1)).HasValue());
 TestFalse(TEXT("string ACK id is refused"),Codec.WithAckId(Original,FVaRestSocketId::String(TEXT("0"))).HasValue());
 TestFalse(TEXT("negative ACK id is refused"),Codec.WithAckId(Original,FVaRestSocketId::Numeric(-1)).HasValue());
 TestFalse(TEXT("inexact JavaScript ACK id is refused"),Codec.WithAckId(Original,FVaRestSocketId::Numeric(9007199254740992LL)).HasValue());
 Codec.SetOutboundFrameLimit(Original[0].Length);
 TestFalse(TEXT("ACK insertion respects the negotiated frame ceiling"),Codec.WithAckId(Original,FVaRestSocketId::Numeric(0)).HasValue());
 Codec.SetOutboundFrameLimit(Original[0].Length+1);
 TestTrue(TEXT("exact ACK frame limit is admitted"),Codec.WithAckId(Original,FVaRestSocketId::Numeric(0)).HasValue());
 Codec.SetOutboundFrameLimit(1000000);
 TestFalse(TEXT("reserved event cannot encode"),Codec.Encode({},TEXT("connect"),Payload,{},Context).HasValue());
 FString Error; auto Options=Codec.GetOptionsSnapshot(); const auto Prior=Options;
 Options.MaxAttachments=-1; TestFalse(TEXT("invalid option update refuses"),Codec.UpdateOptions(Options,Error));
 TestEqual(TEXT("refused options do not mutate active configuration"),Codec.GetOptionsSnapshot().MaxAttachments,Prior.MaxAttachments);
 TestTrue(TEXT("event byte bound accepts 256 ASCII bytes"),FVaRestSioCodec::IsValidEventName(FString::ChrN(256,TEXT('a'))));
 TestFalse(TEXT("event byte bound rejects 257 ASCII bytes"),FVaRestSioCodec::IsValidEventName(FString::ChrN(257,TEXT('a'))));
 TestTrue(TEXT("event UTF8 byte bound accepts 128 two-byte characters"),FVaRestSioCodec::IsValidEventName(FString::ChrN(128,TCHAR(0xe9))));
 TestFalse(TEXT("event UTF8 byte bound rejects 129 two-byte characters"),FVaRestSioCodec::IsValidEventName(FString::ChrN(129,TCHAR(0xe9))));

 const TArray<uint8> Bytes={0,255,7};
 auto Unused=[](auto&,bool,TConstArrayView<FString>,FVaRestJsonError&) { return false; };
 auto Arguments=[&Bytes](auto& Writer,const FVaRestJsonConvertOptions& Convert,FVaRestJsonError& Failure,TFunctionRef<bool()> Budget)
 { VaRest::Sio::FArgumentWriter Into{Writer,Convert,Failure,Budget}; return Into.Value(Bytes) && Into.Value(FString(TEXT("tail"))); };
 FVaRestSocketPayloadWriter BinaryPayload(Unused,false); BinaryPayload.Arguments.Emplace(Arguments);
 auto Packet=Codec.Encode(TEXT("/room"),TEXT("bin"),BinaryPayload,{},Context);
 if (!TestTrue(TEXT("native byte argument encodes to attachment packet"),Packet.HasValue() && Packet.GetValue().Num()==2)) return false;
 auto Identified=Codec.WithAckId(Packet.GetValue(),FVaRestSocketId::Numeric(7));
 if (!TestTrue(TEXT("binary prepared packet accepts ACK id"),Identified.HasValue())) return false;
 const auto& Frames=Identified.GetValue();
 TestTrue(TEXT("binary bytes remain separate from JSON"),Frames[1].Kind==EVaRestSocketFrameKind::Binary && TArray<uint8>(Frames[1].GetPayload())==Bytes);
 TestTrue(TEXT("all frames share immutable packet annotations"),Frames[0].Metadata==Frames[1].Metadata && Frames[0].Metadata->AttachmentSizes==TArray<int64>({3}) && Frames[0].Metadata->Id==TEXT("7"));
 FInput RoundTrip;
 for (const auto& Frame:Frames) { FVaRestSocketInboundFrame Into; Into.Kind=Frame.Kind; Into.Bytes=TArray<uint8>(Frame.GetPayload()); RoundTrip.Frames.Add(MoveTemp(Into)); }
 const auto Decoded=Decode({MoveTemp(RoundTrip)});
 const auto& Last=Decoded.Cases[0].Last();
 TestTrue(TEXT("encoded binary packet round-trips through worker decoder"),Last.Status==EVaRestSocketDecode::Message && Last.Number==7 && Last.Route==TEXT("bin") && Last.Attachments.Num()==1 && Last.Attachments[0]==Bytes && Last.Data[1].AsString()==TEXT("tail"));
 auto Connect=Codec.EncodeControl(0,TEXT("/room"),FVaRestJson::Object().Set(TEXT("token"),TEXT("synthetic")));
 if (!TestTrue(TEXT("CONNECT control encodes"),Connect.HasValue())) return false;
 const auto& Control=Connect.GetValue()[0];
 TestTrue(TEXT("CONNECT annotations redact every auth payload value"),Control.Metadata->bRedactAllExcerptValues && Codec.RedactAllOutgoingExcerptValues(Control.GetPayload()) && Control.GetPayload()[Control.Metadata->ExcerptJsonOffset]=='{');
 return true;
}
#endif
