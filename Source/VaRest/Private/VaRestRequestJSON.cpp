// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRequestJSON.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestLibrary.h"
#include "VaRestSettings.h"
#include "VaRestSubsystem.h"
#include "VaRestHub.h"
#include "VaRestApiClient.h"
#include "VaRestTransport.h"
#include "Async/Async.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"

#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString UVaRestRequestJSON::DeprecatedResponseString(TEXT("DEPRECATED: Please use GetResponseContentAsString() instead"));
namespace
{
TSet<TWeakObjectPtr<UVaRestRequestJSON>> ActiveLegacyRequests;
bool LegacyStopping=false;
}

template <class T>
void FVaRestLatentAction<T>::Cancel()
{
	Abort();
	UObject* Obj = Request.Get();
	if (auto* Legacy=Cast<UVaRestRequestJSON>(Obj);Legacy && static_cast<const void*>(Legacy->ContinueAction)==static_cast<const void*>(this))
	{
		Legacy->Cancel();
	}
}

UVaRestRequestJSON::UVaRestRequestJSON(const class FObjectInitializer& PCIP)
	: Super(PCIP)
	, BinaryContentType(TEXT("application/octet-stream"))
{
	ContinueAction = nullptr;

	RequestVerb = EVaRestRequestVerb::GET;
	RequestContentType = EVaRestRequestContentType::x_www_form_urlencoded_url;

	ResetData();
}

void UVaRestRequestJSON::SetVerb(EVaRestRequestVerb Verb)
{
	RequestVerb = Verb;
}

void UVaRestRequestJSON::SetCustomVerb(FString Verb)
{
	CustomVerb = Verb;
}

void UVaRestRequestJSON::SetContentType(EVaRestRequestContentType ContentType)
{
	RequestContentType = ContentType;
}

void UVaRestRequestJSON::SetBinaryContentType(const FString& ContentType)
{
	BinaryContentType = ContentType;
}

void UVaRestRequestJSON::SetBinaryRequestContent(const TArray<uint8>& Bytes)
{
	RequestBytes = Bytes;
}

void UVaRestRequestJSON::SetStringRequestContent(const FString& Content)
{
	StringRequestContent = Content;
}

void UVaRestRequestJSON::SetHeader(const FString& HeaderName, const FString& HeaderValue)
{
	RequestHeaders.Add(HeaderName, HeaderValue);
}

//////////////////////////////////////////////////////////////////////////
// Destruction and reset

void UVaRestRequestJSON::ResetData()
{
	ResetRequestData();
	ResetResponseData();
}

void UVaRestRequestJSON::ResetRequestData()
{
	if (RequestJsonObj != nullptr)
	{
		RequestJsonObj->Reset();
	}
	else
	{
		RequestJsonObj = NewObject<UVaRestJsonObject>();
	}

	// See issue #90
	// HttpRequest = FHttpModule::Get().CreateRequest();

	RequestBytes.Empty();
	StringRequestContent.Empty();
}

void UVaRestRequestJSON::ResetResponseData()
{
	if (ResponseJsonObj != nullptr)
	{
		ResponseJsonObj->Reset();
	}
	else
	{
		ResponseJsonObj = NewObject<UVaRestJsonObject>();
	}

	if (ResponseJsonValue != nullptr)
	{
		ResponseJsonValue->Reset();
	}
	else
	{
		ResponseJsonValue = NewObject<UVaRestJsonValue>();
	}

	ResponseHeaders.Empty();
	ResponseCode = -1;
	ResponseSize = 0;

	bIsValidJsonResponse = false;

	// #127 Reset string to deprecated state
	ResponseContent = DeprecatedResponseString;

	ResponseBytes.Empty();
	ResponseContentLength = 0;
}

void UVaRestRequestJSON::Cancel()
{
	check(IsInGameThread());
	// A live explicit cancellation must outlast removal of its CallURL map pin.
	// Destruction/unreachable schedules gain no new root or owner callback.
	const TStrongObjectPtr<UVaRestRequestJSON> Self(IsValid(this) && !IsUnreachable() ? this : nullptr);
	if (Self.Get())
	{
		auto* Owner = Cast<UVaRestSubsystem>(GetOuter());
		if (IsValid(Owner) && !Owner->IsUnreachable())
		{
			const TStrongObjectPtr<UVaRestSubsystem> KeptOwner(Owner);
			Owner->RetireCallURLSilently(this);
		}
	}
	const uint64 RetiredEpoch=CoreEpoch==MAX_uint64 ? MAX_uint64 : CoreEpoch+1;RetireCoreOnGT(true);
	if (CoreEpoch==RetiredEpoch) { CoreStatus=EVaRestRequestStatus::Failed;ResetResponseData(); }
}
void UVaRestRequestJSON::RetireCoreOnGT(bool bClearContinuation)
{
	check(IsInGameThread());if (CoreEpoch!=MAX_uint64) ++CoreEpoch;
	ActiveLegacyRequests.Remove(TWeakObjectPtr<UVaRestRequestJSON>(this));
	if (bClearContinuation && ContinueAction) { ContinueAction->Abort();ContinueAction=nullptr;ContinueEpoch=0; }
	auto Previous=CoreHandle;CoreHandle={};ActiveEngineRequest.Reset();const auto Hub=CoreHub;const uint64 Registration=CoreRegistration;CoreHub.Reset();CoreRegistration=0;
	Previous.Cancel();if (auto* Owner=Hub.Get()) Owner->ReleaseInFlight(Registration);
}
void UVaRestRequestJSON::BeginDestroy() { RetireCoreOnGT(true);Super::BeginDestroy(); }

//////////////////////////////////////////////////////////////////////////
// JSON data accessors

UVaRestJsonObject* UVaRestRequestJSON::GetRequestObject() const
{
	check(RequestJsonObj);
	return RequestJsonObj;
}

void UVaRestRequestJSON::SetRequestObject(UVaRestJsonObject* JsonObject)
{
	if (JsonObject == nullptr)
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Provided JsonObject is nullptr"), *VA_FUNC_LINE);
		return;
	}

	RequestJsonObj = JsonObject;
}

UVaRestJsonObject* UVaRestRequestJSON::GetResponseObject() const
{
	check(ResponseJsonObj);
	return ResponseJsonObj;
}

void UVaRestRequestJSON::SetResponseObject(UVaRestJsonObject* JsonObject)
{
	if (JsonObject == nullptr)
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Provided JsonObject is nullptr"), *VA_FUNC_LINE);
		return;
	}

	ResponseJsonObj = JsonObject;
}

UVaRestJsonValue* UVaRestRequestJSON::GetResponseValue() const
{
	check(ResponseJsonValue);
	return ResponseJsonValue;
}

///////////////////////////////////////////////////////////////////////////
// Response data access

FString UVaRestRequestJSON::GetURL() const
{
	return HttpRequest->GetURL();
}

EVaRestRequestVerb UVaRestRequestJSON::GetVerb() const
{
	return RequestVerb;
}

EVaRestRequestStatus UVaRestRequestJSON::GetStatus() const
{
	return CoreStatus;
}

int32 UVaRestRequestJSON::GetResponseCode() const
{
	return ResponseCode;
}

FString UVaRestRequestJSON::GetResponseHeader(const FString& HeaderName)
{
	FString Result;

	FString* Header = ResponseHeaders.Find(HeaderName);
	if (Header != nullptr)
	{
		Result = *Header;
	}

	return Result;
}

TArray<FString> UVaRestRequestJSON::GetAllResponseHeaders() const
{
	TArray<FString> Result;
	for (TMap<FString, FString>::TConstIterator It(ResponseHeaders); It; ++It)
	{
		Result.Add(It.Key() + TEXT(": ") + It.Value());
	}
	return Result;
}

int32 UVaRestRequestJSON::GetResponseContentLength() const
{
	return ResponseContentLength;
}

const TArray<uint8>& UVaRestRequestJSON::GetResponseContent() const
{
	return ResponseBytes;
}

//////////////////////////////////////////////////////////////////////////
// URL processing

void UVaRestRequestJSON::SetURL(const FString& Url)
{
	check(IsInGameThread());if (HttpRequest->GetStatus()==EHttpRequestStatus::Processing || (CoreRegistration!=0 && ActiveEngineRequest.Pin()==HttpRequest)) FreshEngineRequestOnGT();
	// Be sure to trim URL because it can break links on iOS
	FString TrimmedUrl = Url;

	TrimmedUrl.TrimStartInline();
	TrimmedUrl.TrimEndInline();

	HttpRequest->SetURL(TrimmedUrl);
	InputUrl=TrimmedUrl;PreparedUrl.Reset();
}

void UVaRestRequestJSON::ProcessURL(const FString& Url)
{
	SetURL(Url);LegacyWorldContext.Reset();
	ProcessRequest();
}

void UVaRestRequestJSON::ApplyURL(const FString& Url, UVaRestJsonObject*& Result, UObject* WorldContextObject, FLatentActionInfo LatentInfo)
{
	SetURL(Url);LegacyWorldContext=WorldContextObject;

	// Prepare latent action
	if (UWorld* World = GEngine->GetWorldFromContextObjectChecked(WorldContextObject))
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		FVaRestLatentAction<UVaRestJsonObject*>* Kont = LatentActionManager.FindExistingAction<FVaRestLatentAction<UVaRestJsonObject*>>(LatentInfo.CallbackTarget, LatentInfo.UUID);

		if (Kont != nullptr)
		{
			Kont->Cancel();
		}

		LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, ContinueAction = new FVaRestLatentAction<UVaRestJsonObject*>(this, Result, LatentInfo));
		ContinueEpoch=0;
	}

	ProcessRequest();
}

void UVaRestRequestJSON::ExecuteProcessRequest()
{
	if (HttpRequest->GetURL().Len() == 0)
	{
		UE_LOG(LogVaRest, Error, TEXT("Request execution attempt with empty URL"));
		return;
	}

	ProcessRequest();
}

namespace
{
// Weak cache only. A Hub retains one legacy core client for all of its legacy request objects.
TMap<FObjectKey,TWeakObjectPtr<UVaRestApiClient>> LegacyCoreClients;
TSet<FObjectKey> LegacyCreatingClients;
FVaRestClient LegacyClient(UVaRestHub* Hub)
{
	check(IsInGameThread());if (!Hub) return {};
	for (auto It=LegacyCoreClients.CreateIterator();It;++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	const FObjectKey Key(Hub);auto* Found=LegacyCoreClients.Find(Key);auto* Wrapper=Found ? Found->Get() : nullptr;
	if (!Wrapper || !Wrapper->GetNativeClient().IsValid())
	{
		if (LegacyCreatingClients.Contains(Key)) return {};LegacyCreatingClients.Add(Key);ON_SCOPE_EXIT { LegacyCreatingClients.Remove(Key); };
		Wrapper=Hub->MakeClient(TEXT("https://varest-legacy.invalid"));if (!Wrapper) return {};LegacyCoreClients.Add(Key,Wrapper);
	}
	return Wrapper->GetNativeClient();
}
TArray<uint8> LegacyTextBody(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text,Text.Len());return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
}
}
void UVaRestRequestJSON::FreshEngineRequestOnGT()
{
	check(IsInGameThread());const auto Previous=HttpRequest;HttpRequest=FHttpModule::Get().CreateRequest();HttpRequest->SetURL(Previous->GetURL());
	for (const auto& Header:FVaRestHeaders::FromLines(Previous->GetAllHeaders())) HttpRequest->SetHeader(Header.Key,Header.Value);
	if (const auto Timeout=Previous->GetTimeout();Timeout.IsSet()) HttpRequest->SetTimeout(Timeout.GetValue());
}
void UVaRestRequestJSON::ProcessRequest()
{
	check(IsInGameThread());if (!IsValid(this) || LegacyStopping) return;const TStrongObjectPtr<UVaRestRequestJSON> Self(this);
	const bool SharedWithActive=CoreRegistration!=0 && ActiveEngineRequest.Pin()==HttpRequest;
	const uint64 Epoch=CoreEpoch==MAX_uint64 ? MAX_uint64 : CoreEpoch+1;RetireCoreOnGT(false);if (CoreEpoch!=Epoch) return;
	if (ContinueAction && ContinueEpoch!=0) { ContinueAction->Abort();ContinueAction=nullptr; }
	if (ContinueAction) ContinueEpoch=Epoch;
	CoreStatus=EVaRestRequestStatus::Processing;
	if (Epoch==MAX_uint64) { FinishCoreOnGT(Epoch,{},EVaRestErrorKind::Config);return; }
	static bool WarnedChunkedSetting=false;
	if (!WarnedChunkedSetting && UVaRestLibrary::GetVaRestSettings()->bUseChunkedParser)
	{
		WarnedChunkedSetting=true;UE_LOG(LogVaRest,Warning,TEXT("VaRest 2 legacy requests use the v2 JSON parser. bUseChunkedParser is retained for compatibility and is ignored."));
	}
	// A canceled physical call may still own the previous engine request. Never rewrite it for a new invocation.
	if (SharedWithActive || HttpRequest->GetStatus()!=EHttpRequestStatus::NotStarted) FreshEngineRequestOnGT();
	const auto EngineRequest=HttpRequest;
	FString Url=EngineRequest->GetURL();if (PreparedUrl.IsEmpty() || !Url.Equals(PreparedUrl,ESearchCase::CaseSensitive)) InputUrl=Url;else Url=InputUrl;
	FString Verb;
	switch (RequestVerb)
	{
	case EVaRestRequestVerb::GET:Verb=TEXT("GET");break;
	case EVaRestRequestVerb::POST:Verb=TEXT("POST");break;
	case EVaRestRequestVerb::PUT:Verb=TEXT("PUT");break;
	case EVaRestRequestVerb::DEL:Verb=TEXT("DELETE");break;
	case EVaRestRequestVerb::CUSTOM:Verb=CustomVerb;break;
	default:break;
	}
	TArray<uint8> Body;FString ContentType;
	if (RequestContentType==EVaRestRequestContentType::binary) { Body=RequestBytes;ContentType=BinaryContentType; }
	else if (RequestContentType==EVaRestRequestContentType::json)
	{
		ContentType=TEXT("application/json");if (RequestVerb!=EVaRestRequestVerb::GET) Body=LegacyTextBody(RequestJsonObj->EncodeJsonToSingleString());
	}
	else
	{
		ContentType=TEXT("application/x-www-form-urlencoded");FString Form;
		if (RequestJsonObj) for (const auto& Field:RequestJsonObj->GetRootObject()->Values)
		{
			const FString Name(*Field.Key),Value=Field.Value->AsString();if (Name.IsEmpty() || Value.IsEmpty()) continue;
			if (!Form.IsEmpty()) Form+=TEXT("&");Form+=UVaRestLibrary::PercentEncode(Name)+TEXT("=")+UVaRestLibrary::PercentEncode(Value);
		}
		if (RequestContentType==EVaRestRequestContentType::x_www_form_urlencoded_url)
		{
			if (!Form.IsEmpty()) Url+=(Url.Contains(TEXT("?")) ? TEXT("&") : TEXT("?"))+Form;
			if (!StringRequestContent.IsEmpty()) Body=LegacyTextBody(StringRequestContent);
		}
		else Body=LegacyTextBody(StringRequestContent.IsEmpty() ? Form : StringRequestContent);
	}
	PreparedUrl=Url;EngineRequest->SetURL(Url);EngineRequest->SetVerb(Verb);EngineRequest->SetContent(Body);
	EngineRequest->SetHeader(TEXT("Content-Type"),ContentType);for (const auto& Header:RequestHeaders) EngineRequest->SetHeader(Header.Key,Header.Value);
	auto* Hub=UVaRestHub::Get(LegacyWorldContext.IsValid() ? LegacyWorldContext.Get() : this);
	const TWeakObjectPtr<UVaRestRequestJSON> Weak(this);const TWeakObjectPtr<UVaRestHub> WeakHub(Hub);
	const uint64 Registration=Hub ? Hub->RetainInFlight(this,[Weak] { if (auto* Target=Weak.Get()) Target->Cancel(); }) : 0;
	if (!Registration) { FinishCoreOnGT(Epoch,{},EVaRestErrorKind::Config);return; }
	CoreHub=Hub;CoreRegistration=Registration;ActiveEngineRequest=EngineRequest;ActiveLegacyRequests.Add(Weak);auto Client=LegacyClient(Hub);
	if (CoreEpoch!=Epoch) return; // Created/teardown callbacks can invalidate this invocation before Send.
	if (!Client.IsValid()) { FinishCoreOnGT(Epoch,{},EVaRestErrorKind::Config);return; }
	Client.WithTransport(VaRest::Http::TransportForRequest(EngineRequest));ON_SCOPE_EXIT { Client.WithTransport(VaRest::Http::GetDefaultTransport()); };
	auto Builder=Client.Request<FVaRestResponse>(Verb,Url).NoAuth().NoRetry().NoCache().NoEnvelope().KeepBody();
	if (!Body.IsEmpty()) Builder=MoveTemp(Builder).Body(MoveTemp(Body));
	for (const auto& Header:FVaRestHeaders::FromLines(EngineRequest->GetAllHeaders())) Builder=MoveTemp(Builder).Header(Header.Key,Header.Value);
	if (const auto Timeout=EngineRequest->GetTimeout();Timeout.IsSet()) Builder=MoveTemp(Builder).Timeout(Timeout.GetValue());
	if (!Tags.IsEmpty()) Builder=MoveTemp(Builder).Tag(Tags[0]);
	auto Sent=MoveTemp(Builder).OnComplete([Weak,WeakHub,Epoch,Registration](const TVaRestResult<FVaRestResponse>& Result)
	{
		// Admission refusals may originate on a native worker. Only plain bytes/metadata cross this boundary.
		// Raw-response requests own successful bytes in Value; Response contains
		// metadata (and retained bytes for failures that have no decoded Value).
		auto Deliver=[Weak,WeakHub,Epoch,Registration,Response=Result.Value.IsSet() ? Result.GetValue() : Result.Response,Kind=Result.Error.Kind]() mutable
		{
			if (auto* Target=Weak.Get();Target && Target->CoreEpoch==Epoch) Target->FinishCoreOnGT(Epoch,MoveTemp(Response),Kind);
			else if (auto* Owner=WeakHub.Get()) Owner->ReleaseInFlight(Registration);
		};
		if (IsInGameThread()) Deliver();else AsyncTask(ENamedThreads::GameThread,MoveTemp(Deliver));
	}).Send();
	if (CoreEpoch==Epoch && CoreRegistration==Registration) CoreHandle=MoveTemp(Sent);else Sent.Cancel();
}
void UVaRestRequestJSON::FinishCoreOnGT(uint64 Epoch,FVaRestResponse Response,EVaRestErrorKind Kind)
{
	check(IsInGameThread());if (CoreEpoch!=Epoch || !IsValid(this)) return;const TStrongObjectPtr<UVaRestRequestJSON> Self(this);
	ActiveLegacyRequests.Remove(TWeakObjectPtr<UVaRestRequestJSON>(this));
	CoreHandle={};ActiveEngineRequest.Reset();const auto Hub=CoreHub;const uint64 Registration=CoreRegistration;CoreHub.Reset();CoreRegistration=0;
	if (auto* Owner=Hub.Get()) Owner->ReleaseInFlight(Registration);
	ResetResponseData();ResponseCode=Response.Status>0 ? Response.Status : -1;ResponseBytes=MoveTemp(Response.Body);ResponseSize=ResponseBytes.Num();
	for (const auto& Header:Response.Headers) ResponseHeaders.Add(Header.Key,Header.Value);
	const FString Length=Response.Headers.Get(TEXT("Content-Length"));ResponseContentLength=Length.IsEmpty() ? ResponseSize : FMath::Max(0,FCString::Atoi(*Length));
	const bool Completed=Kind==EVaRestErrorKind::None || Kind==EVaRestErrorKind::Http;
	CoreStatus=Completed ? EVaRestRequestStatus::Succeeded : Kind==EVaRestErrorKind::Connection ? EVaRestRequestStatus::Failed_ConnectionError : EVaRestRequestStatus::Failed;
	if (Completed)
	{
		auto Parsed=FVaRestJson::Parse(ResponseBytes);if (Parsed.HasValue())
		{
			auto Value=Parsed.GetValue().ToJsonValue();ResponseJsonValue->SetRootValue(Value);bIsValidJsonResponse=Value.IsValid();
			if (Value.IsValid() && Value->Type==EJson::Object) ResponseJsonObj->SetRootObject(Value->AsObject());
		}
	}
	if (!bIsValidJsonResponse)
	{
		if (ResponseBytes.IsEmpty()) ResponseContent.Reset();else { const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(ResponseBytes.GetData()),ResponseBytes.Num());ResponseContent=FString(Text.Length(),Text.Get()); }
	}
	if (Completed) OnRequestComplete.Broadcast(this);else OnRequestFail.Broadcast(this);
	if (!IsValid(this)) return;
	// Static CallURL bookkeeping must retire its old entry even if a dynamic listener starts another invocation.
	if (Completed) OnStaticRequestComplete.Broadcast(this);else OnStaticRequestFail.Broadcast(this);
	if (CoreEpoch==Epoch && ContinueAction && ContinueEpoch==Epoch)
	{
		auto* Pending=ContinueAction;ContinueAction=nullptr;ContinueEpoch=0;
		if (Completed && Pending->CallbackTarget.IsValid()) Pending->Call(ResponseJsonObj);else Pending->Abort();
	}
}
void UVaRestRequestJSON::OnProcessRequestComplete(FHttpRequestPtr Request,FHttpResponsePtr Response,bool bWasSuccessful)
{
	FVaRestResponse Raw;if (Response) { Raw.Status=Response->GetResponseCode();Raw.Headers=FVaRestHeaders::FromLines(Response->GetAllHeaders());Raw.Body=Response->GetContent(); }
	FinishCoreOnGT(CoreEpoch,MoveTemp(Raw),bWasSuccessful ? EVaRestErrorKind::None : EVaRestErrorKind::Connection);
}
void UVaRestRequestJSON::ShutdownRequestsOnGT()
{
	check(IsInGameThread());LegacyStopping=true;auto Pending=MoveTemp(ActiveLegacyRequests);
	for (const auto& Weak:Pending) if (auto* Request=Weak.Get()) { const TStrongObjectPtr<UVaRestRequestJSON> Kept(Request);Request->Cancel(); }
	LegacyCoreClients.Reset();LegacyCreatingClients.Reset();
}

//////////////////////////////////////////////////////////////////////////
// Tags

void UVaRestRequestJSON::AddTag(FName Tag)
{
	if (Tag != NAME_None)
	{
		Tags.AddUnique(Tag);
	}
}

int32 UVaRestRequestJSON::RemoveTag(FName Tag)
{
	return Tags.Remove(Tag);
}

bool UVaRestRequestJSON::HasTag(FName Tag) const
{
	return (Tag != NAME_None) && Tags.Contains(Tag);
}

//////////////////////////////////////////////////////////////////////////
// Data

FString UVaRestRequestJSON::GetResponseContentAsString(bool bCacheResponseContent)
{
	if (!bIsValidJsonResponse) return ResponseContent;
	if (!ResponseJsonValue || !ResponseJsonValue->GetRootValue().IsValid()) return TEXT("Invalid response");
	if (bCacheResponseContent && ResponseContent!=DeprecatedResponseString) return ResponseContent;
	const FString Text=ResponseJsonValue->GetType()==EVaJson::Object && ResponseJsonObj ? ResponseJsonObj->EncodeJson() : ResponseJsonValue->ToVaRestJson().ToString();
	if (bCacheResponseContent) ResponseContent=Text;return Text;
}
