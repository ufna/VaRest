// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRequestAction.h"
#include "VaRestRequestHandle.h"
#include "VaRestApiClient.h"
#include "VaRestApi.h"
#include "VaRestHub.h"
#include "VaRestDynamicRequestAccess.h"
#include "VaRestDynamicBridge.h"
#include "VaRestFrameDispatcher.h"
#include "VaRestClientState.h"
#include "CoreGlobals.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "VaRestBlueprintFrame.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "UObject/Stack.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace VaRest::Http
{
struct FRequestActionState
{
	~FRequestActionState() { check(IsInGameThread()); check(!Preparation); }
	TUniquePtr<TVaRestRequest<FVaRestDynamicResult>> Builder;
	FVaRestClient Native;
	TWeakObjectPtr<const UObject> Owner;
	TWeakObjectPtr<UVaRestApiClient> Client;
	TWeakObjectPtr<UVaRestHub> Hub;
	TWeakObjectPtr<UObject> Target;
	TWeakObjectPtr<UClass> TargetClass;
	TWeakObjectPtr<UFunction> PrepareFunction,ContinueFunction,UnavailableFunction;
	TWeakObjectPtr<UFunction> MetadataFunction;
	TWeakObjectPtr<UFunction> QueuedFunction;
	TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe> Types;
	TSharedPtr<FDynamicFrameTicket,ESPMode::ThreadSafe> ProgressTicket;
	FVaRestProgress Progress;
	FVaRestDynamicResult PlainResult;
	FVaRestRequestTransferDiagnostics Diagnostics;
	EVaRestTypedOutputUnavailableReason UnavailableReason=EVaRestTypedOutputUnavailableReason::OutputNotBound;
	// A synchronous capability only: never a Blueprint stack/frame/property address. Cleared before Prepare returns.
	FDynamicPrepareToken* Preparation=nullptr;
	FString PreparedCookie;
	bool bHasOwner=false,bTypedResponse=false,bTypedError=false,bActivated=false,bBound=false,bReleased=false,bTerminal=false;
	bool bPrepared=false,bPreparedError=false,bContinuing=false,bConsumed=false,bUnavailable=false;
	bool bMetadataBound=false;
	bool bQueuedBound=false;
};
namespace RequestActionPrivate
{
bool Typed(const FVaRestTypeRef& Type)
{
	return Type.bJsonValue || Type.Container!=EVaRestTypeContainer::None || (Type.Kind!=EVaRestTypeKind::None && Type.Kind!=EVaRestTypeKind::Json &&
		Type.Kind!=EVaRestTypeKind::String && Type.Kind!=EVaRestTypeKind::Bytes);
}
template <typename DelegateType>
UFunction* Resolve(const DelegateType& Delegate,FName PropertyName)
{
	auto* Target=Delegate.GetUObject();
	if (!IsValid(Target) || Delegate.GetFunctionName().IsNone()) return nullptr;
	auto* Function=Target->FindFunction(Delegate.GetFunctionName());
	const auto* Property=FindFProperty<FDelegateProperty>(UVaRestRequestAction::StaticClass(),PropertyName);
	return Function && Property && Property->SignatureFunction && Function->IsSignatureCompatibleWith(Property->SignatureFunction) ? Function : nullptr;
}
struct FWildcard { const FProperty* Property=nullptr; void* Address=nullptr; };
bool ScalarText(const FVaRestJson& Json,FString& Text)
{
	if (Json.IsString()) { Text=Json.AsString(); return true; }
	if (Json.IsBool() || Json.IsNumber()) { Text=Json.ToString(); return true; }
	if (Json.IsNull()) { Text.Reset(); return true; }
	return false;
}
FWildcard Step(FFrame& Stack)
{
	FProperty* Chain=Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
	check(!Chain || Chain->HasAnyPropertyFlags(CPF_OutParm));
	uint8 Placeholder=0;
	Stack.MostRecentProperty=nullptr; Stack.MostRecentPropertyAddress=nullptr;
	Stack.StepCompiledIn<FProperty>(Stack.Code ? nullptr : &Placeholder);
	return {Stack.MostRecentProperty ? Stack.MostRecentProperty : Chain,Stack.MostRecentPropertyAddress};
}
}
}

UVaRestRequestAction::UVaRestRequestAction(const FObjectInitializer& Initializer) : Super(Initializer) {}
UVaRestRequestAction::UVaRestRequestAction(FVTableHelper& Helper) : Super(Helper) {}
UVaRestRequestAction::~UVaRestRequestAction() = default;
FVaRestError UVaRestRequestAction::AdmissionError(const FString& Detail,FName Tag,FVaRestResponseData& Http)
{
	Http=FVaRestResponseData();
	FVaRestError Error; Error.Kind=EVaRestErrorKind::Config; Error.Tag=Tag;
	Error.Code=Detail.IsEmpty() ? TEXT("RequestAdmissionRefused") : Detail; Error.Message=Error.Code; return Error;
}
UVaRestRequestAction* UVaRestRequestAction::CreateRequest(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Verb,const FString& Path,
	const FVaRestTypeRef& ResponseType,const FVaRestTypeRef& ErrorType,FString& Error)
{
	check(IsInGameThread()); Error.Reset();
	using namespace VaRest::Http;
	if (FClientState::IsShuttingDown()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	if (!Client) { auto* Hub=UVaRestHub::Get(WorldContextObject); Client=Hub ? Hub->GetDefaultClient() : nullptr; }
	const TStrongObjectPtr<UVaRestApiClient> KeptClient(Client);
	auto* Hub=IsValid(Client) && !Client->bDetached ? Client->Hub.Get() : nullptr;
	const auto NativeState=Client ? FClientAccess::State(Client->Native) : nullptr;
	if (!Hub || !NativeState || NativeState->ManagedAdmissionIsClosed()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	const TStrongObjectPtr<UVaRestHub> KeptHub(Hub);
	FVaRestDynamicResultConfig Config;
	Config.ResponseType=ResponseType; Config.ErrorType=ErrorType; Config.RuleOwner=Client->Asset; Config.Budget=Hub->GetDynamicBudget();
	auto Request=Client->Native.RequestDynamic(Verb,Path,MoveTemp(Config)).Owner(WorldContextObject);
	return AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),RequestActionPrivate::Typed(ResponseType),RequestActionPrivate::Typed(ErrorType),StaticClass(),Error);
}
UVaRestRequestAction* UVaRestRequestAction::CreateEndpointRequest(const UObject* WorldContextObject,UVaRestApi* Api,FGuid EndpointId,FName EndpointName,FName Tag,FString& Error)
{
	check(IsInGameThread()); using namespace VaRest::Http; Error.Reset();
	if (!IsValid(Api)) { Error=TEXT("ApiUnavailable"); return nullptr; }
	const TStrongObjectPtr<UVaRestApi> KeptApi(Api);
	const auto* Selected=EndpointId.IsValid() ? Api->FindEndpoint(EndpointId) : Api->FindEndpoint(EndpointName);
	if (!Selected) { Error=TEXT("EndpointNotFound"); return nullptr; }
	const FVaRestApiEndpoint Endpoint=*Selected; // no pointer into an editable asset survives Hub/client callouts
	auto* Hub=UVaRestHub::Get(WorldContextObject);
	const TStrongObjectPtr<UVaRestHub> KeptHub(Hub);
	auto* Client=Hub ? Hub->GetClient(Api) : nullptr;
	const TStrongObjectPtr<UVaRestApiClient> KeptClient(Client);
	const auto NativeState=Client ? FClientAccess::State(Client->Native) : nullptr;
	if (!IsValid(Client) || Client->bDetached || !NativeState || NativeState->ManagedAdmissionIsClosed()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	FVaRestDynamicResultConfig Config; Config.ResponseType=Endpoint.ResponseType; Config.ErrorType=Endpoint.ErrorType;
	Config.RuleOwner=Api; Config.Budget=Hub->GetDynamicBudget();
	auto Request=Client->Native.RequestDynamic(Endpoint.Verb,Endpoint.Path,MoveTemp(Config)).Owner(WorldContextObject)
		.Endpoint(Endpoint.Name).Tag(Tag).AuthMode(Endpoint.AuthMode);
	if (Endpoint.bIdempotent) Request=MoveTemp(Request).Idempotent();
	if (Endpoint.bQueueWhenOffline) Request=MoveTemp(Request).QueueWhenOffline();
	if (Endpoint.bOverrideRetry) Request=MoveTemp(Request).Retry(Endpoint.Retry);
	if (Endpoint.bOverrideTimeouts) Request=MoveTemp(Request).Timeout(Endpoint.Timeout,Endpoint.ActivityTimeout);
	if (Endpoint.bOverrideCache) Request=MoveTemp(Request).Cache(Endpoint.Cache);
	if (Endpoint.bOverrideEnvelope) Request=MoveTemp(Request).Envelope(Endpoint.Envelope);
	if (Endpoint.bOverrideNaming) Request=MoveTemp(Request).Naming(Endpoint.Naming);
	if (Endpoint.bOverridePriority) Request=MoveTemp(Request).Priority(Endpoint.Priority);
	if (Endpoint.bOverrideMaxJsonBytes) Request=MoveTemp(Request).MaxJsonBytes(Endpoint.MaxJsonBytes);
	if (Endpoint.bOverrideStrictDecode) Request=MoveTemp(Request).StrictDecode(Endpoint.bStrictDecode);
	for (const auto& Pair:Endpoint.DefaultQuery) Request=MoveTemp(Request).Query(Pair.Name,Pair.Value);
	for (const auto& Pair:Endpoint.Headers) FDynamicRequestAccess::AppendHeaderOnGT(Request,Pair.Name,Pair.Value);
	if (!Endpoint.BodyContentType.IsEmpty())
	{
		if (Endpoint.BodyKind==EVaRestEndpointBodyKind::Multipart) FDynamicRequestAccess::FailOnGT(Request,TEXT("multipart Content-Type is generated with its native boundary"));
		else Request=MoveTemp(Request).Header(TEXTVIEW("Content-Type"),Endpoint.BodyContentType);
	}
	return AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),RequestActionPrivate::Typed(Endpoint.ResponseType),RequestActionPrivate::Typed(Endpoint.ErrorType),StaticClass(),Error);
}
void UVaRestRequestAction::ConfigureRequest(const TMap<FString,FString>& Query,const TArray<FVaRestHeader>& Headers,FName Tag,EVaRestAuthMode AuthMode,bool bQueueWhenOffline)
{
	check(IsInGameThread()); using namespace VaRest::Http;
	if (!State || !State->Builder || State->bActivated || State->bReleased) return;
	auto& Request=*State->Builder; Request=MoveTemp(Request).Tag(Tag).AuthMode(AuthMode);
	if (bQueueWhenOffline) Request=MoveTemp(Request).QueueWhenOffline();
	TArray<FString> Names; Query.GetKeys(Names); Names.Sort([](const FString& A,const FString& B) { return A.Compare(B,ESearchCase::CaseSensitive)<0; });
	for (const auto& Name:Names) Request=MoveTemp(Request).Query(Name,Query.FindChecked(Name));
	for (const auto& Pair:Headers) FDynamicRequestAccess::AppendHeaderOnGT(Request,Pair.Name,Pair.Value);
}
bool UVaRestRequestAction::SetParameter(const FString&,EVaRestEndpointParamLocation,EVaRestQueryStyle,const int32&) { checkNoEntry(); return false; }
DEFINE_FUNCTION(UVaRestRequestAction::execSetParameter)
{
	P_GET_PROPERTY(FStrProperty,Name); P_GET_ENUM(EVaRestEndpointParamLocation,Location); P_GET_ENUM(EVaRestQueryStyle,Style);
	const auto Value=VaRest::Http::RequestActionPrivate::Step(Stack); P_FINISH;
	P_NATIVE_BEGIN; *static_cast<bool*>(RESULT_PARAM)=P_THIS->SetParameterOnGT(Name,static_cast<EVaRestEndpointParamLocation>(Location),static_cast<EVaRestQueryStyle>(Style),Value.Property,Value.Address); P_NATIVE_END;
}
bool UVaRestRequestAction::SetParameterOnGT(const FString& Name,EVaRestEndpointParamLocation Location,EVaRestQueryStyle Style,const FProperty* Property,const void* Address)
{
	check(IsInGameThread()); using namespace VaRest::Http;
	if (!State || !State->Builder || State->bActivated || State->bReleased) return false;
	const TStrongObjectPtr<UVaRestRequestAction> Self(this);
	auto Builder=MoveTemp(State->Builder);
	ON_SCOPE_EXIT { if (!State->bReleased && !State->bActivated) State->Builder=MoveTemp(Builder); };
	auto& Request=*Builder; // conversion/default operations may reenter Hub teardown
	const auto Fail=[&] { FDynamicRequestAccess::FailOnGT(Request,TEXT("the endpoint parameter is not a supported scalar or array")); return false; };
	if (!Property || !Address || Name.IsEmpty()) return Fail();
	const auto Json=FVaRestJsonConvert::PropertyToJson(Property,Address,FDynamicRequestAccess::EncodeOptionsOnGT(Request));
	if (State->bReleased || !CanDeliverOnGT()) return false;
	if (!Json.IsValid()) return Fail();
	TArray<FString> Values;
	if (Json.IsArray())
	{
		for (const auto& Element:Json.Elements()) { FString Value; if (!RequestActionPrivate::ScalarText(Element,Value)) return Fail(); Values.Add(MoveTemp(Value)); }
	}
	else { FString Value; if (!RequestActionPrivate::ScalarText(Json,Value)) return Fail(); Values.Add(MoveTemp(Value)); }
	switch (Location)
	{
	case EVaRestEndpointParamLocation::Path:
		if (Json.IsArray()) Request=MoveTemp(Request).ParamValues(Name,Values); else Request=MoveTemp(Request).Path(Name,Values[0]); break;
	case EVaRestEndpointParamLocation::Query:
		if (Json.IsArray()) Request=MoveTemp(Request).Query(Name,Values,Style); else Request=MoveTemp(Request).Query(Name,Values[0]); break;
	case EVaRestEndpointParamLocation::Header:
		Request=MoveTemp(Request).Header(Name,FString::Join(Values,TEXT(","))); break;
	default:return Fail();
	}
	return true;
}
bool UVaRestRequestAction::SetRequestBody(EVaRestEndpointBodyKind,const FString&,const int32&,bool) { checkNoEntry(); return false; }
DEFINE_FUNCTION(UVaRestRequestAction::execSetRequestBody)
{
	P_GET_ENUM(EVaRestEndpointBodyKind,Kind); P_GET_PROPERTY(FStrProperty,ContentType);
	const auto Value=VaRest::Http::RequestActionPrivate::Step(Stack); P_GET_UBOOL(bBytesAsBase64); P_FINISH;
	P_NATIVE_BEGIN; *static_cast<bool*>(RESULT_PARAM)=P_THIS->SetRequestBodyOnGT(static_cast<EVaRestEndpointBodyKind>(Kind),ContentType,Value.Property,Value.Address,bBytesAsBase64); P_NATIVE_END;
}
bool UVaRestRequestAction::SetRequestBodyOnGT(EVaRestEndpointBodyKind Kind,const FString& ContentType,const FProperty* Property,const void* Address,bool bBytesAsBase64)
{
	check(IsInGameThread()); using namespace VaRest::Http;
	if (!State || !State->Builder || State->bActivated || State->bReleased) return false;
	const TStrongObjectPtr<UVaRestRequestAction> Self(this);
	auto Builder=MoveTemp(State->Builder);
	ON_SCOPE_EXIT { if (!State->bReleased && !State->bActivated) State->Builder=MoveTemp(Builder); };
	auto& Request=*Builder;
	const auto Fail=[&](FString Reason) { FDynamicRequestAccess::FailOnGT(Request,MoveTemp(Reason)); return false; };
	if (!Property || !Address) return Fail(TEXT("the request body has no value"));
	if (bBytesAsBase64)
	{
		const auto* Array=CastField<FArrayProperty>(Property);const auto* Byte=Array ? CastField<FByteProperty>(Array->Inner) : nullptr;
		if (Kind!=EVaRestEndpointBodyKind::Json || !Byte || Byte->Enum) return Fail(TEXT("JSON base64 body mode requires a plain byte array"));
	}
	const auto* Struct=CastField<FStructProperty>(Property);
	if (Kind==EVaRestEndpointBodyKind::Multipart)
	{
		if (!ContentType.IsEmpty() || FDynamicRequestAccess::HasHeaderOnGT(Request,TEXTVIEW("Content-Type")) || !Struct || Struct->Struct!=FVaRestMultipartData::StaticStruct()) return Fail(TEXT("multipart requires the checked multipart data and its native boundary"));
		auto Native=static_cast<const FVaRestMultipartData*>(Address)->ToNative();
		if (Native.HasError()) return Fail(Native.StealError());
		Request=MoveTemp(Request).Body(Native.GetValue()); return true;
	}
	if (Kind==EVaRestEndpointBodyKind::Binary)
	{
		const auto* Array=CastField<FArrayProperty>(Property); const auto* Byte=Array ? CastField<FByteProperty>(Array->Inner) : nullptr;
		if (!Byte || Byte->Enum) return Fail(TEXT("binary bodies require a byte array"));
		FScriptArrayHelper Values(Array,Address); TArray<uint8> Bytes; Bytes.SetNumUninitialized(Values.Num());
		for (int32 Index=0;Index<Values.Num();++Index) Bytes[Index]=Byte->GetPropertyValue(Values.GetRawPtr(Index));
		Request=MoveTemp(Request).Body(MoveTemp(Bytes));
	}
	else if (Kind==EVaRestEndpointBodyKind::Text)
	{
		const auto* Text=CastField<FStrProperty>(Property); if (!Text) return Fail(TEXT("text bodies require a string"));
		Request=MoveTemp(Request).Body(Text->GetPropertyValue(Address));
	}
	else if (Kind==EVaRestEndpointBodyKind::Json || Kind==EVaRestEndpointBodyKind::Form)
	{
		auto Encode=FDynamicRequestAccess::EncodeOptionsOnGT(Request);Encode.bBytesAsBase64=bBytesAsBase64;
		const FVaRestJson Json=Struct && Struct->Struct==FVaRestJson::StaticStruct() ? *static_cast<const FVaRestJson*>(Address) :
			FVaRestJsonConvert::PropertyToJson(Property,Address,Encode);
		if (State->bReleased || !CanDeliverOnGT()) return false;
		if (Kind==EVaRestEndpointBodyKind::Json && Struct && Struct->Struct==FVaRestJson::StaticStruct() && !Json.IsValid()) return true; // missing Json is the native no-body value; JSON null remains valid
		if (!Json.IsValid()) return Fail(TEXT("the body cannot be converted to JSON"));
		if (Kind==EVaRestEndpointBodyKind::Json) Request=MoveTemp(Request).Body(Json);
		else
		{
			if (!Json.IsObject()) return Fail(TEXT("form bodies require a flat object"));
			FVaRestForm Form;
			for (const auto& Member:Json.Members())
			{
				const auto Add=[&](const FVaRestJson& Element) { FString Value; if (!RequestActionPrivate::ScalarText(Element,Value)) return false; Form.Add(Member.GetKey(),Value); return true; };
				if (Member.Value.IsArray()) { for (const auto& Element:Member.Value.Elements()) if (!Add(Element)) return Fail(TEXT("form fields cannot contain nested objects/arrays")); }
				else if (!Add(Member.Value)) return Fail(TEXT("form fields cannot contain nested objects/arrays"));
			}
			Request=MoveTemp(Request).Body(Form);
		}
	}
	else return Fail(TEXT("unknown endpoint body kind"));
	if (!ContentType.IsEmpty()) Request=MoveTemp(Request).Header(TEXTVIEW("Content-Type"),ContentType);
	return true;
}
UVaRestRequestAction* UVaRestRequestAction::AdoptOnGT(const UObject* WorldContextObject,UVaRestApiClient* Client,
	TVaRestRequest<FVaRestDynamicResult>&& Request,bool bTypedResponse,bool bTypedError,UClass* ActionClass,FString& Error)
{
	check(IsInGameThread()); using namespace VaRest::Http; Error.Reset();
	ON_SCOPE_EXIT { FDynamicRequestAccess::DiscardOnGT(Request); }; // success moves its Spec into the action; failures consume it here

	const TStrongObjectPtr<UVaRestApiClient> KeptClient(Client);
	auto* Hub=IsValid(Client) && !Client->bDetached ? Client->Hub.Get() : nullptr;
	const auto NativeState=Client ? FClientAccess::State(Client->Native) : nullptr;
	if (!Hub || !NativeState || NativeState->ManagedAdmissionIsClosed() || FClientState::IsShuttingDown()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	const TStrongObjectPtr<UVaRestHub> KeptHub(Hub);
	if (!FDynamicRequestAccess::ValidateOnGT(Request,Error)) return nullptr;
	if (!ActionClass || !ActionClass->IsChildOf(StaticClass())) { Error=TEXT("InvalidActionClass"); return nullptr; }
	auto* Action=NewObject<UVaRestRequestAction>(GetTransientPackage(),ActionClass);
	const TStrongObjectPtr<UVaRestRequestAction> KeptAction(Action);
	Action->State=MakeUnique<FRequestActionState>();
	auto& State=*Action->State;
	State.Native=Client->Native; State.Client=Client; State.Hub=Hub; State.Owner=WorldContextObject; State.bHasOwner=WorldContextObject!=nullptr;
	State.bTypedResponse=bTypedResponse; State.bTypedError=bTypedError;
	State.Types=FDynamicRequestAccess::GetTypesOnGT(Request);
	Action->Handle=NewObject<UVaRestRequestHandle>(Action);
	const TWeakObjectPtr<UVaRestRequestAction> Weak(Action);
	if ((State.bTypedResponse || State.bTypedError) && !FDynamicRequestAccess::SetPreparationOnGT(Request,[Weak](FDynamicPrepareToken& Token) {
		if (auto* Current=Weak.Get()) Current->PrepareOnGT(Token);
	})) { Error=TEXT("DynamicOutputPreparationFailed"); return nullptr; }
	State.Builder=MakeUnique<TVaRestRequest<FVaRestDynamicResult>>(MoveTemp(Request)
		.OnComplete([Weak](const TVaRestResult<FVaRestDynamicResult>& Result) { if (auto* Current=Weak.Get()) Current->CompleteOnGT(Result); })
		.OnQueued([Weak](const FString& QueueId) { if (auto* Current=Weak.Get()) Current->QueuedOnGT(QueueId); }));
	Hub->OnDeinitializing.AddDynamic(Action,&UVaRestRequestAction::HubClosing);
	return Action;
}
bool UVaRestRequestAction::BindOutput(FVaRestPrepareOutput Prepare,FVaRestContinueOutput Continue,FVaRestUnavailableOutput Unavailable)
{
	check(IsInGameThread()); using namespace VaRest::Http::RequestActionPrivate;
	if (!State || State->bActivated || State->bReleased || State->bBound) return false;
	auto* Target=Prepare.GetUObject();
	if (!IsValid(Target) || Continue.GetUObject()!=Target || Unavailable.GetUObject()!=Target || (State->bHasOwner && State->Owner.Get()!=Target)) return false;
	const auto* Class=Cast<UBlueprintGeneratedClass>(Target->GetClass());
	if (!VaRest::Http::HasPreparedBlueprintFrameOnGT(Target,Class)) return false;
	const FString PrepareName=Prepare.GetFunctionName().ToString();
	const FString Prefix=TEXT("__VaRest_Prepare_");
	if (!PrepareName.StartsWith(Prefix,ESearchCase::CaseSensitive)) return false;
	const FString Identity=PrepareName.Mid(Prefix.Len()); FGuid ParsedIdentity;
	if (!FGuid::ParseExact(Identity,EGuidFormats::Digits,ParsedIdentity) || !ParsedIdentity.IsValid() ||
		Continue.GetFunctionName()!=FName(FString(TEXT("__VaRest_Continue_"))+Identity) ||
		Unavailable.GetFunctionName()!=FName(FString(TEXT("__VaRest_Unavailable_"))+Identity)) return false;
	auto* Preparation=Resolve(Prepare,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,PrepareDelegate));
	auto* Continuation=Resolve(Continue,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,ContinueDelegate));
	auto* Availability=Resolve(Unavailable,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,UnavailableDelegate));
	if (!Preparation || !Continuation || !Availability) return false;
	State->bBound=true; State->Target=Target; State->TargetClass=Target->GetClass();
	State->PrepareFunction=Preparation; State->ContinueFunction=Continuation; State->UnavailableFunction=Availability;
	PrepareDelegate=MoveTemp(Prepare); ContinueDelegate=MoveTemp(Continue); UnavailableDelegate=MoveTemp(Unavailable); return true;
}
bool UVaRestRequestAction::BindMetadata(FVaRestContinueOutput Metadata)
{
	check(IsInGameThread()); using namespace VaRest::Http::RequestActionPrivate;
	if (!State || State->bActivated || State->bReleased || !State->bBound || State->bMetadataBound || Metadata.GetUObject()!=State->Target.Get()) return false;
	const FString Identity=PrepareDelegate.GetFunctionName().ToString().Mid(FString(TEXT("__VaRest_Prepare_")).Len());
	auto* Function=Resolve(Metadata,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,MetadataDelegate));
	if (!Function || Metadata.GetFunctionName()!=FName(FString(TEXT("__VaRest_Metadata_"))+Identity)) return false;
	State->bMetadataBound=true; State->MetadataFunction=Function; MetadataDelegate=MoveTemp(Metadata); return true;
}
bool UVaRestRequestAction::BindQueued(FVaRestQueuedOutput Queued)
{
	check(IsInGameThread()); using namespace VaRest::Http::RequestActionPrivate;
	if (!State || State->bActivated || State->bReleased || !State->bBound || State->bQueuedBound || Queued.GetUObject()!=State->Target.Get()) return false;
	const FString Identity=PrepareDelegate.GetFunctionName().ToString().Mid(FString(TEXT("__VaRest_Prepare_")).Len());
	auto* Function=Resolve(Queued,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,QueuedDelegate));
	if (!Function || Queued.GetFunctionName()!=FName(FString(TEXT("__VaRest_Queued_"))+Identity)) return false;
	State->bQueuedBound=true; State->QueuedFunction=Function; QueuedDelegate=MoveTemp(Queued); return true;
}
bool UVaRestRequestAction::CanDeliverOnGT() const
{
	check(IsInGameThread());
	if (!State || State->bReleased || VaRest::Http::FClientState::IsShuttingDown() || (State->bHasOwner && !State->Owner.IsValid())) return false;
	const auto Client=State->Client.Get();
	const auto Native=VaRest::Http::FClientAccess::State(State->Native);
	return IsValid(Client) && !Client->bDetached && State->Hub.IsValid() && Native && !Native->ManagedAdmissionIsClosed();
}
bool UVaRestRequestAction::TargetCurrentOnGT() const
{
	check(IsInGameThread()); using namespace VaRest::Http::RequestActionPrivate;
	auto* Target=State ? State->Target.Get() : nullptr;
	return State && State->bBound && IsValid(Target) && Target->GetClass()==State->TargetClass.Get() &&
		State->PrepareFunction.IsValid() && State->ContinueFunction.IsValid() && State->UnavailableFunction.IsValid() &&
		Resolve(PrepareDelegate,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,PrepareDelegate))==State->PrepareFunction.Get() &&
		Resolve(ContinueDelegate,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,ContinueDelegate))==State->ContinueFunction.Get() &&
		Resolve(UnavailableDelegate,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,UnavailableDelegate))==State->UnavailableFunction.Get() &&
		(!State->bMetadataBound || Resolve(MetadataDelegate,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,MetadataDelegate))==State->MetadataFunction.Get()) &&
		(!State->bQueuedBound || Resolve(QueuedDelegate,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,QueuedDelegate))==State->QueuedFunction.Get());
}
void UVaRestRequestAction::Activate()
{
	check(IsInGameThread());
	if (!State || State->bActivated || State->bReleased || !State->Builder) return;
	State->bActivated=true;
	if (!CanDeliverOnGT()) { ReleaseOnGT(); return; }
	// The reserved GT frame owns the original strong pin and cleanup, including engine-only actions without a GI.
	TUniqueFunction<void()> Cleanup=[Pin=TStrongObjectPtr<UVaRestRequestAction>(this)] { Pin->ReleaseOnGT(); };
	if (!VaRest::Http::FDynamicRequestAccess::SetActionLifetimeOnGT(*State->Builder,MoveTemp(Cleanup))) { ReleaseOnGT(); return; }
	if (State->bHasOwner && GEngine)
		if (auto* World=GEngine->GetWorldFromContextObject(State->Owner.Get(),EGetWorldErrorMode::ReturnNull))
			if (auto* Instance=World->GetGameInstance()) RegisterWithGameInstance(Instance);
	if (OnProgress.IsBound())
	{
		const TWeakObjectPtr<UVaRestRequestAction> Weak(this);
		*State->Builder=MoveTemp(*State->Builder).OnProgress([Weak](const FVaRestProgress& Progress) { if (auto* Current=Weak.Get()) Current->ProgressOnGT(Progress); });
	}
	auto Builder=MoveTemp(State->Builder);
	Handle->Native=MoveTemp(*Builder).Send(); // immediate capture, including the currently executing Blueprint initiator
}
void UVaRestRequestAction::PrepareOnGT(VaRest::Http::FDynamicPrepareToken& Token)
{
	check(IsInGameThread());
	if (!State || !State->bActivated || State->bTerminal || State->Preparation || !CanDeliverOnGT() || !TargetCurrentOnGT()) return;
	const TStrongObjectPtr<UVaRestRequestAction> Self(this);
	const TStrongObjectPtr<UObject> Target(State->Target.Get());
	TGuardValue<VaRest::Http::FDynamicPrepareToken*> Guard(State->Preparation,&Token);
	State->bPreparedError=Token.IsError();
	++State->Diagnostics.Preparations;
	const auto Prepare=PrepareDelegate;
	Prepare.Execute(this,Handle->GetIdText()); // generated function contains no user execution or latent continuation
}
bool UVaRestRequestAction::TakeOnGT(const FProperty* Property,void* Address,bool bError)
{
	check(IsInGameThread());
	if (!State || !State->Preparation || State->bPrepared || !CanDeliverOnGT() || !TargetCurrentOnGT()) return false;
	++State->Diagnostics.AddressBorrows;
	ON_SCOPE_EXIT { --State->Diagnostics.AddressBorrows; };
	const bool bCopied=State->Preparation->Take(Property,Address,bError);
	if (bCopied) ++State->Diagnostics.Copies;
	return bCopied;
}
bool UVaRestRequestAction::TakeResponse(int32&) { checkNoEntry(); return false; }
bool UVaRestRequestAction::TakeErrorBody(int32&) { checkNoEntry(); return false; }
DEFINE_FUNCTION(UVaRestRequestAction::execTakeResponse)
{
	const auto Value=VaRest::Http::RequestActionPrivate::Step(Stack); P_FINISH;
	P_NATIVE_BEGIN; *static_cast<bool*>(RESULT_PARAM)=P_THIS->TakeOnGT(Value.Property,Value.Address,false); P_NATIVE_END;
}
DEFINE_FUNCTION(UVaRestRequestAction::execTakeErrorBody)
{
	const auto Value=VaRest::Http::RequestActionPrivate::Step(Stack); P_FINISH;
	P_NATIVE_BEGIN; *static_cast<bool*>(RESULT_PARAM)=P_THIS->TakeOnGT(Value.Property,Value.Address,true); P_NATIVE_END;
}
bool UVaRestRequestAction::ConfirmPrepared(const FString& RequestIdText)
{
	check(IsInGameThread());
	if (!State || !State->Preparation || State->bPrepared || !Handle || Handle->GetId()==0 || RequestIdText!=Handle->GetIdText() ||
		!CanDeliverOnGT() || !TargetCurrentOnGT() || !State->Preparation->ConfirmPrepared()) return false;
	State->PreparedCookie=RequestIdText; State->bPrepared=true;
	++State->Diagnostics.Confirmations; State->Diagnostics.PreparedFrame=GFrameCounter; return true;
}
bool UVaRestRequestAction::AcceptPreparedOutput(const FString& StoredCookie,const FString& RequestIdText)
{
	check(IsInGameThread());
	if (!State || !State->bContinuing || State->bConsumed || !State->bPrepared || StoredCookie!=State->PreparedCookie ||
		RequestIdText!=State->PreparedCookie || !CanDeliverOnGT() || !TargetCurrentOnGT()) return false;
	const auto Types=State->Types.Pin();
	if (!Types || !Types->IsCurrentForDeliveryOnGT()) return false;
	State->bConsumed=true;
	++State->Diagnostics.Continuations; State->Diagnostics.DeliveredFrame=GFrameCounter;
	return true; // consume before generated continuation enters user execution
}
void UVaRestRequestAction::CompleteOnGT(const TVaRestResult<FVaRestDynamicResult>& Result)
{
	check(IsInGameThread());
	if (!State || State->bTerminal || State->bReleased) return;
	const TStrongObjectPtr<UVaRestRequestAction> Self(this);
	State->bTerminal=true; TerminalResponse=FVaRestResponseData::FromNative(Result.Response); TerminalError=Result.Error;
	State->PlainResult=Result.Value.IsSet() ? Result.Value.GetValue() : FVaRestDynamicResult();
	ON_SCOPE_EXIT { ReleaseOnGT(); };
	if (State->ProgressTicket) State->ProgressTicket->CancelOnGT();
	State->ProgressTicket.Reset();
	if (!CanDeliverOnGT()) return;
	const bool bTyped=Result.IsSuccess() ? State->bTypedResponse : State->bTypedError;
	if (bTyped && State->bPrepared && State->bPreparedError==!Result.IsSuccess())
	{
		if (!TargetCurrentOnGT()) { UnavailableOnGT(EVaRestTypedOutputUnavailableReason::TargetChangedAfterCompletion); return; }
		const auto Types=State->Types.Pin();
		if (!Types || !Types->IsCurrentForDeliveryOnGT()) { UnavailableOnGT(EVaRestTypedOutputUnavailableReason::TypeChangedAfterCompletion); return; }
		const TStrongObjectPtr<UObject> Target(State->Target.Get());
		const auto Continue=ContinueDelegate;
		{ TGuardValue<bool> Guard(State->bContinuing,true); Continue.Execute(this,Handle->GetIdText(),Result.IsSuccess(),TerminalResponse,TerminalError); }
		if (!CanDeliverOnGT()) return;
		if (!State->bConsumed) { UnavailableOnGT(EVaRestTypedOutputUnavailableReason::OutputSuperseded); return; }
	}
	else if (Result.IsSuccess() && State->bTypedResponse)
	{
		UnavailableOnGT(EVaRestTypedOutputUnavailableReason::OutputNotBound); return; // never a default typed success
	}
	if (!State->bConsumed && State->bMetadataBound)
	{
		if (!TargetCurrentOnGT()) { UnavailableOnGT(EVaRestTypedOutputUnavailableReason::TargetChangedAfterCompletion); return; }
		const TStrongObjectPtr<UObject> Target(State->Target.Get()); const auto Metadata=MetadataDelegate;
		Metadata.Execute(this,Handle->GetIdText(),Result.IsSuccess(),TerminalResponse,TerminalError);
		if (!CanDeliverOnGT()) return;
	}
	if (Result.IsSuccess())
	{
		if (Result.Value.IsSet()) BroadcastSuccessOnGT(Result.Value.GetValue());
		else OnSuccess.Broadcast(TerminalResponse);
	}
	else OnError.Broadcast(TerminalError);
}
void UVaRestRequestAction::BroadcastSuccessOnGT(const FVaRestDynamicResult&) { OnSuccess.Broadcast(TerminalResponse); }
void UVaRestRequestAction::UnavailableOnGT(EVaRestTypedOutputUnavailableReason Reason)
{
	check(IsInGameThread());
	if (!State || State->bUnavailable || State->bConsumed || !CanDeliverOnGT()) return;
	State->bUnavailable=true;
	State->UnavailableReason=Reason;
	++State->Diagnostics.Unavailable; State->Diagnostics.DeliveredFrame=GFrameCounter;
	// Availability is metadata-only: resolve the CURRENT signature by name, even when the typed functions changed.
	const auto Callback=UnavailableDelegate;
	if (VaRest::Http::RequestActionPrivate::Resolve(Callback,GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,UnavailableDelegate)))
	{
		const TStrongObjectPtr<const UObject> Target(Callback.GetUObject());
		Callback.Execute(this,Handle->GetIdText(),TerminalResponse,TerminalError,Reason);
	}
	if (CanDeliverOnGT()) OnTypedOutputUnavailable.Broadcast(TerminalResponse,TerminalError,Reason);
}
void UVaRestRequestAction::QueuedOnGT(const FString& QueueId)
{
	check(IsInGameThread());
	if (!State || State->bTerminal || State->bReleased) return;
	const TStrongObjectPtr<UVaRestRequestAction> Self(this);
	State->bTerminal=true; TerminalQueueId=QueueId; TerminalResponse.RequestId=Handle->GetIdText(); ON_SCOPE_EXIT { ReleaseOnGT(); };
	if (CanDeliverOnGT() && State->bQueuedBound)
	{
		if (!TargetCurrentOnGT()) { UnavailableOnGT(EVaRestTypedOutputUnavailableReason::TargetChangedAfterCompletion); return; }
		const TStrongObjectPtr<UObject> Target(State->Target.Get()); const auto Queued=QueuedDelegate;
		Queued.Execute(this,Handle->GetIdText(),QueueId);
	}
	if (CanDeliverOnGT()) OnQueued.Broadcast(QueueId);
}
void UVaRestRequestAction::ProgressOnGT(const FVaRestProgress& Progress)
{
	check(IsInGameThread());
	if (!State || State->bTerminal || !OnProgress.IsBound() || !CanDeliverOnGT()) return;
	State->Progress=Progress;
	if (State->ProgressTicket) return;
	const TWeakObjectPtr<UVaRestRequestAction> Weak(this);
	TUniqueFunction<void()> Callback=[Weak] { if (auto* Action=Weak.Get()) Action->DeliverProgressOnGT(); };
	State->ProgressTicket=VaRest::Http::ReserveDynamicFrameOnGT(Callback);
	if (State->ProgressTicket) State->ProgressTicket->ReadyOnGT(); // coalesce; refuse only the optional progress notification
}
void UVaRestRequestAction::DeliverProgressOnGT()
{
	check(IsInGameThread());
	if (!State) return;
	State->ProgressTicket.Reset();
	if (State->bTerminal || !CanDeliverOnGT()) return;
	const TStrongObjectPtr<UVaRestRequestAction> Self(this);
	const auto Progress=State->Progress;
	OnProgress.Broadcast(Progress.BytesSent,Progress.BytesReceived,Progress.UploadTotal,Progress.DownloadTotal);
}
bool UVaRestRequestAction::HasTerminalMetadata() const { return State && State->bTerminal; }
FVaRestJson UVaRestRequestAction::GetJson() const { check(IsInGameThread()); return State ? State->PlainResult.GetJson() : FVaRestJson(); }
FString UVaRestRequestAction::GetText() const { check(IsInGameThread()); return State ? State->PlainResult.GetText() : FString(); }
TArray<uint8> UVaRestRequestAction::GetBytes() const
{
	check(IsInGameThread()); if (!State) return {};
	const auto Bytes=State->PlainResult.GetBytes(); return TArray<uint8>(Bytes.GetData(),Bytes.Num());
}
FString UVaRestRequestAction::GetErrorText() const
{
	check(IsInGameThread()); if (TerminalResponse.Body.IsEmpty()) return {};
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(TerminalResponse.Body.GetData()),TerminalResponse.Body.Num());
	return FString(Text.Length(),Text.Get());
}
bool UVaRestRequestAction::GetTypedOutputUnavailable(EVaRestTypedOutputUnavailableReason& Reason) const
{
	check(IsInGameThread()); if (!State || !State->bUnavailable) return false;
	Reason=State->UnavailableReason; return true;
}
FVaRestDynamicResult UVaRestRequestAction::GetNativeResult() const
{
	check(IsInGameThread()); return State ? State->PlainResult : FVaRestDynamicResult();
}
FVaRestDynamicResult UVaRestRequestAction::TakeNativeResultOnGT()
{
	check(IsInGameThread());
	return State ? MoveTemp(State->PlainResult) : FVaRestDynamicResult();
}
FVaRestRequestTransferDiagnostics UVaRestRequestAction::GetTransferDiagnostics() const
{
	check(IsInGameThread()); return State ? State->Diagnostics : FVaRestRequestTransferDiagnostics();
}
void UVaRestRequestAction::ReleaseOnGT()
{
	check(IsInGameThread());
	if (!State || State->bReleased) return;
	State->bReleased=true;
	if (Handle) Handle->Cancel();
	if (State->ProgressTicket) State->ProgressTicket->CancelOnGT();
	State->ProgressTicket.Reset();
	PrepareDelegate.Unbind(); ContinueDelegate.Unbind(); UnavailableDelegate.Unbind(); MetadataDelegate.Unbind(); QueuedDelegate.Unbind();
	if (auto* Hub=State->Hub.Get()) Hub->OnDeinitializing.RemoveDynamic(this,&UVaRestRequestAction::HubClosing);
	if (State->Builder) VaRest::Http::FDynamicRequestAccess::DiscardOnGT(*State->Builder);
	State->Builder.Reset(); State->Types.Reset(); State->Native=FVaRestClient();
	State->Client.Reset(); State->Hub.Reset();
	SetReadyToDestroy();
}
void UVaRestRequestAction::HubClosing() { ReleaseOnGT(); }
void UVaRestRequestAction::BeginDestroy() { ReleaseOnGT(); Super::BeginDestroy(); }
