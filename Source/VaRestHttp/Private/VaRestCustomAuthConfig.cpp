// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestCustomAuthConfig.h"
#include "VaRestHttpLog.h"
#include "VaRestJsonRedact.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestGetJsonAction.h"
#include "VaRestHub.h"
#include "VaRestUrl.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http
{
struct FCustomRefreshContextData
{
	explicit FCustomRefreshContextData(const FVaRestAuthRefreshContext& InContext) : Context(InContext) {}
	FVaRestAuthRefreshContext Context; // weak native client and shared one-time ID; no UObject reference
};
struct FCustomRefreshPending
{
	uint64 RefreshId=0;
	TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Done; // original callback, owned/destructed only on GT
};
namespace CustomAuthPrivate
{
bool RoundTrips(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text,Text.Len());
	const FString Back=FString::ConstructFromPtrSize(Utf8.Get(),Utf8.Length());
	return Text.Len()==Back.Len() && (Text.IsEmpty() || FMemory::Memcmp(*Text,*Back,Text.Len()*sizeof(TCHAR))==0);
}
bool PlainIdentity(const FString& Text)
{
	if (Text.IsEmpty() || Text.Len()>256 || !RoundTrips(Text)) return false;
	for (TCHAR Character:Text) if (Character<0x20 || Character==0x7f) return false;
	return true;
}
bool Protectable(const FString& Key)
{
	if (Key.IsEmpty() || !FVaRestRedactRules::IsAcceptableKey(Key)) return false;
	FVaRestRedactRules Rules; Rules.AddKey(Key);
	return Rules.ShouldRedact(Key,EVaRestRedactKeyKind::Json);
}
}

/** Native provider has a weak UObject reference only. Every reflected call is synchronous on GT with a stack pin. */
class FCustomAuthAdapter final : public IVaRestAuth
{
public:
	explicit FCustomAuthAdapter(UVaRestCustomAuthConfig* InConfig) : Config(InConfig) {}
	TStrongObjectPtr<UVaRestCustomAuthConfig> Pin() const { check(IsInGameThread()); return TStrongObjectPtr<UVaRestCustomAuthConfig>(Config.Get()); }
	FName GetAuthName() const override
	{
		const auto Owner=Pin(); if (!Owner || Owner->bRetired) return TEXT("Custom");
		const FName Name=Owner->GetAuthName(); return Name.IsNone() ? FName(TEXT("Custom")) : Name;
	}
	FString GetConfigError() const override
	{
		const auto Owner=Pin();
		if (!Owner || Owner->bRetired) return TEXT("the custom authentication configuration is unavailable");
		const auto Keys=Owner->GetRedactedKeys();
		if (Owner->bRetired) return TEXT("the custom authentication configuration is retired");
		for (const auto& Key:Keys) if (!CustomAuthPrivate::Protectable(Key)) return TEXT("a custom authentication redaction key cannot be protected");
		return {};
	}
	bool Apply(FVaRestAuthTarget& Target) const override
	{
		const auto Owner=Pin();
		if (!Owner || Owner->bRetired) { Target.ConfigError=TEXT("custom authentication is unavailable"); return false; }
		FVaRestAuthTargetData View=FVaRestAuthTargetData::FromNative(Target);
		const bool bApplied=Owner->Apply(View);
		if (Owner->bRetired) { Target.ConfigError=TEXT("custom authentication is retired"); return false; }
		if (!bApplied) return false;
		for (const auto& Header:View.Headers)
			if (!IsValidHeaderName(Header.Name) || !IsValidHeaderValue(Header.Value) || !CustomAuthPrivate::RoundTrips(Header.Name) || !CustomAuthPrivate::RoundTrips(Header.Value))
				{ Target.ConfigError=TEXT("custom authentication returned invalid headers"); return false; }
		for (const auto& Query:View.Query)
			if (!CustomAuthPrivate::RoundTrips(Query.Name) || !CustomAuthPrivate::RoundTrips(Query.Value))
				{ Target.ConfigError=TEXT("custom authentication returned invalid query text"); return false; }
		FVaRestHeaders Headers=ToNativeHeaders(View.Headers);
		TArray<TPair<FString,FString>> Query;
		Query.Reserve(View.Query.Num());
		for (const auto& Item:View.Query) Query.Emplace(Item.Name,Item.Value);
		// Session and RequestId are read-only inputs regardless of what an overriding native implementation wrote in View.
		Target.Headers=MoveTemp(Headers); Target.Query=MoveTemp(Query); return true;
	}
	FName GetIdentityContractName() const override
	{
		const auto Owner=Pin(); if (!Owner || Owner->bRetired) return NAME_None;
		const FName Name=Owner->GetIdentityContractName(); return Owner->bRetired ? NAME_None : Name;
	}
	bool GetIdentity(const FVaRestSessionData& Session,FString& Identity) const override
	{
		Identity.Reset(); const auto Owner=Pin(); if (!Owner || Owner->bRetired) return false;
		FString Candidate;
		if (!Owner->GetIdentity(FVaRestSessionDataView::FromNative(Session),Candidate) || Owner->bRetired || !CustomAuthPrivate::PlainIdentity(Candidate)) return false;
		Identity=MoveTemp(Candidate); return true; // no Apply invocation or speculative activation
	}
	bool CanRefresh(const FVaRestSessionData& Session) const override
	{
		const auto Owner=Pin(); return Owner && !Owner->bRetired && Owner->CanRefresh(FVaRestSessionDataView::FromNative(Session)) && !Owner->bRetired;
	}
	bool NeedsRefresh(const FVaRestSessionData& Session,double Now) const override
	{
		const auto Owner=Pin(); return Owner && !Owner->bRetired && Owner->NeedsRefresh(FVaRestSessionDataView::FromNative(Session),Now) && !Owner->bRetired;
	}
	bool IsAuthFailure(int32 Status) const override
	{
		const auto Owner=Pin(); return Owner && !Owner->bRetired && Owner->IsAuthFailure(Status) && !Owner->bRetired;
	}
	void GetRedactedKeys(TArray<FString>& Out) const override
	{
		const auto Owner=Pin(); if (!Owner || Owner->bRetired) return;
		auto Keys=Owner->GetRedactedKeys(); if (!Owner->bRetired) Out.Append(MoveTemp(Keys));
	}
	void GetSecretValues(TArray<TPair<FName,FString>>& Out) const override
	{
		const auto Owner=Pin(); if (!Owner || Owner->bRetired) return;
		const auto Values=Owner->GetSecretValues(); if (Owner->bRetired) return;
		for (const auto& Value:Values) Out.Emplace(Value.Slot,Value.Value);
	}
	void Refresh(const FVaRestAuthRefreshContext& Context,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
	{
		const auto Owner=Pin();
		if (!Owner || Owner->bRetired) { if (Done) Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Config,TEXT("custom authentication is unavailable"))); return; }
		Owner->BeginRefreshOnGT(Context,MoveTemp(Done));
	}
private:
	TWeakObjectPtr<UVaRestCustomAuthConfig> Config;
};
}

void UVaRestAuthRefreshContext::InitializeOnGT(UVaRestCustomAuthConfig* InOwner,const FVaRestAuthRefreshContext& Native)
{
	check(IsInGameThread()); Owner=InOwner; RefreshId=Native.GetRefreshId();
	Data=MakeShared<VaRest::Http::FCustomRefreshContextData,ESPMode::ThreadSafe>(Native); bActive=true;
}
void UVaRestAuthRefreshContext::EndOnGT() { check(IsInGameThread()); bActive=false; Data.Reset(); Owner.Reset(); }
FVaRestSessionDataView UVaRestAuthRefreshContext::GetSession() const
{
	check(IsInGameThread()); return IsActive() ? FVaRestSessionDataView::FromNative(Data->Context.GetSession()) : FVaRestSessionDataView();
}
bool UVaRestAuthRefreshContext::IsActive() const { check(IsInGameThread()); return bActive && Data && Data->Context.IsCurrentOnGT(); }
FString UVaRestAuthRefreshContext::GetRefreshId() const { return VaRest::Http::UnsignedIdText(RefreshId); }
TVaRestRequest<FVaRestDynamicResult> UVaRestAuthRefreshContext::RequestDynamic(FStringView Verb,FStringView Path,FVaRestDynamicResultConfig&& Config) const
{
	check(IsInGameThread());
	return IsActive() ? Data->Context.RequestDynamic(Verb,Path,MoveTemp(Config)) : FVaRestClient().RequestDynamic(Verb,Path,MoveTemp(Config));
}
UVaRestGetJsonAction* UVaRestAuthRefreshContext::RequestJson(const UObject* WorldContextObject,const FString& Verb,const FString& Path,const FVaRestJson& Body,FString& Error)
{
	check(IsInGameThread()); Error.Reset();
	const TStrongObjectPtr<UVaRestAuthRefreshContext> Self(this);
	const auto* Auth=Owner.Get();
	auto* Client=IsValid(Auth) ? Auth->GetTypedOuter<UVaRestApiClient>() : nullptr;
	if (!IsActive() || !IsValid(Client) || Client->bDetached || !Client->Hub.IsValid()) { Error=TEXT("StaleRefreshContext"); return nullptr; }
	const TStrongObjectPtr<UVaRestApiClient> Kept(Client);
	FVaRestDynamicResultConfig Config;
	Config.ResponseType=FVaRestTypeRef::Json(); Config.RuleOwner=Client->Asset; Config.Budget=Client->Hub.Get()->GetDynamicBudget();
	auto Request=RequestDynamic(Verb,Path,MoveTemp(Config)).Owner(WorldContextObject);
	if (Body.IsValid()) Request=MoveTemp(Request).Body(Body);
	return Cast<UVaRestGetJsonAction>(UVaRestRequestAction::AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),false,false,UVaRestGetJsonAction::StaticClass(),Error));
}
bool UVaRestAuthRefreshContext::FinishRefresh(EVaRestCustomRefreshOutcome Outcome,const FString& AccessToken,const FString& RefreshToken,
	double ExpiresInSeconds,EVaRestErrorKind FailedKind,const FString& Message)
{
	check(IsInGameThread()); const TStrongObjectPtr<UVaRestAuthRefreshContext> Self(this);
	FVaRestAuthRefreshResult Result;
	switch (Outcome)
	{
	case EVaRestCustomRefreshOutcome::Refreshed:Result=FVaRestAuthRefreshResult::Refreshed(AccessToken,RefreshToken,ExpiresInSeconds);break;
	case EVaRestCustomRefreshOutcome::Rejected:Result=FVaRestAuthRefreshResult::Rejected(Message);break;
	case EVaRestCustomRefreshOutcome::Failed:Result=FVaRestAuthRefreshResult::Failed(FailedKind,Message);break;
	default:return false;
	}
	if (auto* Target=Owner.Get(); IsActive() && Target && Target->FinishOnGT(this,MoveTemp(Result))) return true;
	if (!bWarnedStaleFinish) { bWarnedStaleFinish=true; UE_LOG(LogVaRestHttp,Warning,TEXT("VaRest ignored a stale custom-auth refresh finish")); }
	return false;
}
void UVaRestAuthRefreshContext::BeginDestroy()
{
	if (auto* Target=Owner.Get(); bActive && Target) Target->AbandonContextOnGT(this);
	EndOnGT(); Super::BeginDestroy();
}

UVaRestCustomAuthConfig::UVaRestCustomAuthConfig(const FObjectInitializer& Initializer) : Super(Initializer) {}
UVaRestCustomAuthConfig::UVaRestCustomAuthConfig(FVTableHelper& Helper) : Super(Helper) {}
UVaRestCustomAuthConfig::~UVaRestCustomAuthConfig() { check(!Pending); }
TSharedPtr<IVaRestAuth,ESPMode::ThreadSafe> UVaRestCustomAuthConfig::CreateAuth() const
{
	check(IsInGameThread()); if (bRetired) return nullptr;
	return MakeShared<VaRest::Http::FCustomAuthAdapter,ESPMode::ThreadSafe>(const_cast<UVaRestCustomAuthConfig*>(this));
}
void UVaRestCustomAuthConfig::Validate(FVaRestApiValidationContext& Context) const
{
	check(IsInGameThread());
	for (const auto& Key:GetRedactedKeys()) if (!VaRest::Http::CustomAuthPrivate::Protectable(Key))
		{ Context.Error(TEXT("Auth.RedactedKey"),TEXTVIEW("Auth"),FText::FromString(TEXT("A custom authentication redaction key cannot be protected."))); break; }
}
void UVaRestCustomAuthConfig::BeginRefreshOnGT(const FVaRestAuthRefreshContext& Context,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done)
{
	check(IsInGameThread());
	if (!Context.IsCurrentOnGT()) { if (Done) Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Config,TEXT("custom authentication refresh context is stale"))); return; }
	if (PendingContext && !PendingContext->IsActive()) AbandonContextOnGT(PendingContext);
	if (bRetired || Pending) { if (Done) Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Config,TEXT("custom authentication already has a pending refresh"))); return; }
	const TStrongObjectPtr<UVaRestCustomAuthConfig> Self(this);
	Pending=MakeUnique<VaRest::Http::FCustomRefreshPending>(); Pending->RefreshId=Context.GetRefreshId(); Pending->Done=MoveTemp(Done);
	auto* Created=NewObject<UVaRestAuthRefreshContext>(this);
	const TStrongObjectPtr<UVaRestAuthRefreshContext> Kept(Created);
	if (bRetired || !Pending || Pending->RefreshId!=Context.GetRefreshId()) return;
	PendingContext=Created; Created->InitializeOnGT(this,Context);
	Refresh(Created); // no watchdog; a live BP may retain this invocation indefinitely
}
bool UVaRestCustomAuthConfig::FinishOnGT(UVaRestAuthRefreshContext* Context,FVaRestAuthRefreshResult&& Result)
{
	check(IsInGameThread());
	if (bRetired || !Pending || PendingContext!=Context || !Context->IsActive() || Pending->RefreshId!=Context->RefreshId) return false;
	const TStrongObjectPtr<UVaRestCustomAuthConfig> Self(this);
	auto Done=MoveTemp(Pending->Done); Pending.Reset(); PendingContext=nullptr; Context->EndOnGT();
	if (Done) Done(MoveTemp(Result)); // same native once-guard; its normal completion is asynchronous
	return true;
}
void UVaRestCustomAuthConfig::AbandonContextOnGT(UVaRestAuthRefreshContext* Context)
{
	check(IsInGameThread());
	if (PendingContext!=Context) return;
	auto Dropped=MoveTemp(Pending); PendingContext=nullptr; Context->EndOnGT();
	Dropped.Reset(); // a live dropped callback reports Failed(Connection); a closed/stopped guard posts nothing
}
void UVaRestCustomAuthConfig::RetireOnGT()
{
	check(IsInGameThread()); if (bRetired) return;
	bRetired=true;
	auto Dropped=MoveTemp(Pending);
	if (PendingContext) PendingContext->EndOnGT();
	PendingContext=nullptr;
	Dropped.Reset(); // original Done dies on GT; closed managed gate prevents the guard from posting work
}
void UVaRestCustomAuthConfig::BeginDestroy() { RetireOnGT(); Super::BeginDestroy(); }
bool UVaRestCustomAuthConfig::Apply_Implementation(FVaRestAuthTargetData&) const { return false; }
FName UVaRestCustomAuthConfig::GetAuthName_Implementation() const { return TEXT("Custom"); }
FName UVaRestCustomAuthConfig::GetIdentityContractName_Implementation() const { return NAME_None; }
bool UVaRestCustomAuthConfig::GetIdentity_Implementation(const FVaRestSessionDataView&,FString& Identity) const { Identity.Reset(); return false; }
bool UVaRestCustomAuthConfig::CanRefresh_Implementation(const FVaRestSessionDataView&) const { return false; }
bool UVaRestCustomAuthConfig::NeedsRefresh_Implementation(const FVaRestSessionDataView&,double) const { return false; }
bool UVaRestCustomAuthConfig::IsAuthFailure_Implementation(int32 Status) const { return Status==401; }
TArray<FString> UVaRestCustomAuthConfig::GetRedactedKeys_Implementation() const { return {}; }
TArray<FVaRestAuthSecretValue> UVaRestCustomAuthConfig::GetSecretValues_Implementation() const { return {}; }
void UVaRestCustomAuthConfig::Refresh_Implementation(UVaRestAuthRefreshContext* Context)
{
	if (Context) Context->FinishRefresh(EVaRestCustomRefreshOutcome::Failed,TEXT(""),TEXT(""),0,EVaRestErrorKind::Config,TEXT("custom refresh is not implemented"));
}
