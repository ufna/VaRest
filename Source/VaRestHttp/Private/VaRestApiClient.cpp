// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestApiClient.h"
#include "VaRestApi.h"
#include "VaRestBuiltinAuthConfigs.h"
#include "VaRestHub.h"
#include "VaRestHubRuntime.h"
#include "VaRestHttpSettings.h"
#include "VaRestTracePoints.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http::ApiClientPrivate
{
bool Live(const FVaRestClient& Client)
{
	const auto State=FClientAccess::State(Client);
	return State && !State->ManagedAdmissionIsClosed();
}
}

bool UVaRestApiClient::BuildTargetOnGT(FName Name,FStringView Account,VaRest::Http::FClientConfig& Config,FString& Error) const
{
	using namespace VaRest::Http;
	check(IsInGameThread());
	const auto Owner=Runtime.Pin();
	if (!Asset) { Error=TEXT("QueueNotConfigured"); return false; }
	if (!Owner || Owner->IsClosedOnGT()) { Error=TEXT("LifecycleClosed"); return false; }
	const auto* Selected=Asset->FindEnvironment(Name);
	if (!Selected) { Error=TEXT("EnvironmentNotFound"); return false; }
	FString Namespace=Selected->QueueNamespace;
	if (Namespace.IsEmpty())
	{
		Namespace=Selected->Name.GetComparisonNameEntry()->GetPlainNameString();
		if (Selected->Name.GetNumber()!=NAME_NO_NUMBER_INTERNAL)
			Namespace+=FString::Printf(TEXT("_%d"),NAME_INTERNAL_TO_EXTERNAL(Selected->Name.GetNumber()));
	}
	FString QueueName;
	if (!FHubRuntime::QueueName(Asset->GetPathName(),Owner->StorageProfileOnGT(),Namespace,Account,QueueName,Error)) return false;
	Config.BaseUrl=Selected->BaseUrl;
	Config.QueueName=MoveTemp(QueueName); Config.OfflinePolicy=Asset->Offline;
	Config.bQueueActivationRequested=false;
	return true;
}
bool UVaRestApiClient::InitializeOnGT(UVaRestHub* Owner,UVaRestApi* Api,TSharedRef<VaRest::Http::FHubRuntime,ESPMode::ThreadSafe> InRuntime,FString& Error,FStringView BaseUrl)
{
	using namespace VaRest::Http;
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestApiClient> Self(this);
	Hub=Owner; Asset=Api; Runtime=InRuntime;
	FClientConfig Config=FClientAccess::InitialConfig(BaseUrl);
	const auto* Settings=GetDefault<UVaRestHttpSettings>();
	Config.Naming=Settings->Naming;
	if (!FClientAccess::ConfigureStorageRoots(Config,Settings->CacheRoot,Settings->QueueRoot)) { Error=TEXT("StorageConfig"); return false; }
	if (Api)
	{
		FVaRestApiValidationContext Validation; Api->Validate(Validation);
		if (Validation.HasErrors()) { Error=TEXT("InvalidApiConfiguration"); return false; }
		const auto Resolved=Api->ResolveEnvironmentName();
		if (Resolved.HasError()) { Error=TEXT("EnvironmentNotFound"); return false; }
		Environment=Resolved.GetValue();
		if (!BuildTargetOnGT(Environment,QueueAccount,Config,Error)) return false;
		Config.DisplayName=Api->GetPathName(); Config.DefaultHeaders=ToNativeHeaders(Api->Headers);
		if (!Api->UserAgent.IsEmpty()) Config.DefaultHeaders.Set(TEXT("User-Agent"),Api->UserAgent);
		Config.Retry=Api->Retry; Config.Timeout=Api->Timeout; Config.ActivityTimeout=Api->ActivityTimeout;
		Config.MaxConcurrent=Api->MaxConcurrent; Config.MaxJsonBytes=Api->MaxJsonBytes;
		Config.Cache=Api->Cache; Config.Naming=Api->Naming; Config.bStrictDecode=Api->bStrictDecode;
		Config.JsonContentType=Api->JsonContentType; Config.JsonRules=FVaRestJsonRules::Snapshot(Api);
		Config.Envelope=MakeShared<FVaRestEnvelope,ESPMode::ThreadSafe>(Api->Envelope);
		Config.LogLevel=Api->LogLevel; AppendRedactKeys(Config.RedactedKeys,Api->RedactedKeys);
		if (Api->Auth)
		{
			Auth=DuplicateObject<UVaRestAuthConfig>(Api->Auth,this);
			if (!Auth) { Error=TEXT("AuthConfigurationUnavailable"); return false; }
			Auth->SetRuntimeApiOnGT(Api);
			if (auto* Refresh=Cast<UVaRestBearerRefreshAuthConfig>(Auth); Refresh && !Refresh->ResolveEndpoint(*Api,Error)) return false;
			if (auto* Credentials=Cast<UVaRestClientCredentialsAuthConfig>(Auth); Credentials && !Credentials->ResolveEndpoint(*Api,Error)) return false;
			Config.Auth=Auth->CreateAuth();
			if (!Config.Auth) { Error=TEXT("AuthConfigurationUnavailable"); return false; }
		}
	}
	const TWeakObjectPtr<UVaRestApiClient> Weak(this);
	const FVaRestClient Created=InRuntime->CreateClientOnGT(MoveTemp(Config),Error,[Weak](const FVaRestClient& Client) {
		if (auto* Target=Weak.Get()) Target->BindNativeOnGT(Client);
	});
	return Created.IsValid() && !bDetached;
}
void UVaRestApiClient::BindNativeOnGT(const FVaRestClient& Client)
{
	check(IsInGameThread()); Native=Client;
	const TWeakObjectPtr<UVaRestApiClient> Weak(this);
	SessionChangedHandle=Native.OnSessionChanged().AddWeakLambda(this,[Weak](EVaRestSessionChange Change) {
		if (auto* Target=Weak.Get(); Target && !Target->bDetached) { const TStrongObjectPtr<UVaRestApiClient> Kept(Target); Target->OnSessionChanged.Broadcast(Change); }
	});
	SessionExpiredHandle=Native.OnSessionExpired().AddWeakLambda(this,[Weak] {
		if (auto* Target=Weak.Get(); Target && !Target->bDetached) { const TStrongObjectPtr<UVaRestApiClient> Kept(Target); Target->OnSessionExpired.Broadcast(); }
	});
	QueuedHandle=Native.OnQueuedResult().AddWeakLambda(this,[Weak](const FVaRestQueuedResult& Result) {
		if (auto* Target=Weak.Get(); Target && !Target->bDetached) { const TStrongObjectPtr<UVaRestApiClient> Kept(Target); Target->OnQueuedResult.Broadcast(FVaRestQueuedResultData::FromNative(Result)); }
	});
	if (const auto State=VaRest::Http::FClientAccess::State(Native)) ErrorHandle=State->PrimaryError.AddWeakLambda(this,[Weak](const FVaRestError& Error) {
		if (auto* Target=Weak.Get(); Target && !Target->bDetached && VaRest::Http::ApiClientPrivate::Live(Target->Native))
		{
			const TStrongObjectPtr<UVaRestApiClient> Kept(Target); Target->OnError.Broadcast(Error);
		}
	});
}
FString UVaRestApiClient::GetBaseUrl() const
{
	const auto State=VaRest::Http::FClientAccess::State(Native);
	return State ? State->GetConfig()->BaseUrl : FString();
}
bool UVaRestApiClient::SetEnvironment(FName Name,FString& Error)
{
	using namespace VaRest::Http;
	const TStrongObjectPtr<UVaRestApiClient> Self(this);
	const FVaRestClient Client=Native; const auto State=FClientAccess::State(Client);
	if (!State || bDetached) { Error=TEXT("LifecycleClosed"); return false; }
	if (bPublishingTarget) { Error=TEXT("QueueSwitchBusy"); return false; }
	TGuardValue<bool> Publishing(bPublishingTarget,true);
	if (Name==Environment) { Error.Reset(); return true; }
	FClientConfig Target=*State->GetConfig();
	if (!BuildTargetOnGT(Name,QueueAccount,Target,Error) || !State->TrySwitchQueue(MoveTemp(Target),Error)) return false;
	Environment=Name;
	TracePoints::Marker(*State,EVaRestTraceMarkerKind::EnvironmentChanged,0,Name.ToString());
	if (!bDetached) if (auto* Owner=Hub.Get()) Owner->NotifyEnvironmentPublished(Asset);
	return true;
}
bool UVaRestApiClient::SetQueueAccount(const FString& Account,FString& Error)
{
	using namespace VaRest::Http;
	const TStrongObjectPtr<UVaRestApiClient> Self(this);
	const FVaRestClient Client=Native; const auto State=FClientAccess::State(Client);
	if (!State || bDetached) { Error=TEXT("LifecycleClosed"); return false; }
	if (bPublishingTarget) { Error=TEXT("QueueSwitchBusy"); return false; }
	TGuardValue<bool> Publishing(bPublishingTarget,true);
	FClientConfig Target=*State->GetConfig();
	if (!BuildTargetOnGT(Environment,Account,Target,Error) || !State->TrySwitchQueue(MoveTemp(Target),Error)) return false;
	QueueAccount=Account; return true;
}
FVaRestOfflineQueueActivationResult UVaRestApiClient::ActivateOfflineQueue()
{
	using namespace VaRest::Http;
	const TStrongObjectPtr<UVaRestApiClient> Self(this);
	FVaRestOfflineQueueActivationResult Result;
	if (!Asset) { Result.Detail=TEXT("QueueNotConfigured"); return Result; }
	const auto State=FClientAccess::State(Native);
	if (!State || bDetached) { Result.Detail=TEXT("LifecycleClosed"); return Result; }
	const bool bAccepted=State->ActivateManagedQueue(Result.Detail); // true also accepts one still-pending activation intent
	if (bDetached || State->ManagedAdmissionIsClosed()) { Result.Detail=TEXT("LifecycleClosed"); return Result; }
	const auto Status=State->GetConfig()->ManagedQueueStatus;
	Result.Outcome=!bAccepted ? EVaRestOfflineQueueActivation::Refused : Status==EManagedQueueStatus::Active ? EVaRestOfflineQueueActivation::Activated :
		Status==EManagedQueueStatus::PendingBusy ? EVaRestOfflineQueueActivation::PendingBusy : EVaRestOfflineQueueActivation::Refused;
	return Result;
}
bool UVaRestApiClient::PauseOfflineQueue(FString& Error)
{
	if (!Asset) { Error=TEXT("QueueNotConfigured"); return false; }
	const auto State=VaRest::Http::FClientAccess::State(Native);
	if (!State || bDetached) { Error=TEXT("LifecycleClosed"); return false; }
	return State->PauseManagedQueue(Error);
}
EVaRestOfflineQueueState UVaRestApiClient::GetOfflineQueueState() const
{
	using namespace VaRest::Http;
	if (!Asset) return EVaRestOfflineQueueState::Unavailable;
	const auto State=FClientAccess::State(Native);
	if (!State || bDetached || State->ManagedAdmissionIsClosed()) return EVaRestOfflineQueueState::Unavailable;
	switch (State->GetConfig()->ManagedQueueStatus)
	{
	case EManagedQueueStatus::Active:return EVaRestOfflineQueueState::Active;
	case EManagedQueueStatus::PendingBusy:return EVaRestOfflineQueueState::PendingBusy;
	case EManagedQueueStatus::Paused:case EManagedQueueStatus::AccountRequired:return EVaRestOfflineQueueState::Paused;
	default:return EVaRestOfflineQueueState::Unavailable;
	}
}
FString UVaRestApiClient::GetOfflineQueueError() const
{
	if (!Asset) return TEXT("QueueNotConfigured");
	const auto State=VaRest::Http::FClientAccess::State(Native);
	return State && !bDetached ? State->GetConfig()->QueueConfigError : FString(TEXT("LifecycleClosed"));
}
int64 UVaRestApiClient::GetQueuedCount(bool& bLoaded) const
{
	const auto State=VaRest::Http::FClientAccess::State(Native); bLoaded=false;
	return Asset && State && !bDetached ? State->ManagedQueuedCount(bLoaded) : 0;
}
void UVaRestApiClient::SetBearerToken(const FString& Token,bool bSameIdentity) { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.SetBearerToken(Token,bSameIdentity); }
void UVaRestApiClient::SetSession(const FString& Access,const FString& Refresh,double Expires,bool bSameIdentity) { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.SetSession(Access,Refresh,Expires,bSameIdentity); }
bool UVaRestApiClient::ImportSession(const FString& Json,bool bSameIdentity) { auto Client=Native; return VaRest::Http::ApiClientPrivate::Live(Client) && Client.ImportSession(Json,bSameIdentity); }
FString UVaRestApiClient::ExportSession() const { const auto Client=Native; return Client.ExportSession(); }
void UVaRestApiClient::ClearSession() { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.ClearSession(); }
bool UVaRestApiClient::ReplaceAuthOnGT(UVaRestAuthConfig* Candidate,FString& Error)
{
	using namespace VaRest::Http;
	check(IsInGameThread());
	if (bDetached || !Candidate || !ApiClientPrivate::Live(Native)) { Error=TEXT("LifecycleClosed"); return false; }
	const TStrongObjectPtr<UVaRestApiClient> Self(this);
	const TStrongObjectPtr<UVaRestAuthConfig> CandidateOwner(Candidate);
	Candidate->SetRuntimeApiOnGT(Asset);
	FVaRestApiValidationContext Validation; Candidate->Validate(Validation);
	if (Validation.HasErrors()) { Error=TEXT("InvalidAuthConfiguration"); return false; }
	if (auto* Credentials=Cast<UVaRestClientCredentialsAuthConfig>(Candidate); Credentials &&
		(!Asset || !Credentials->ResolveEndpoint(*Asset,Error))) { Error=TEXT("AuthEndpointUnavailable"); return false; }
	const auto Provider=Candidate->CreateAuth();
	if (!Provider) { Error=TEXT("AuthConfigurationUnavailable"); return false; }
	if (bDetached || !ApiClientPrivate::Live(Native)) { Error=TEXT("LifecycleClosed"); return false; }
	auto Client=Native; Auth=Candidate;
	Client.WithAuth(Provider.ToSharedRef()); // actual native identity replacement pauses managed replay before publication
	Error.Reset(); return true;
}
bool UVaRestApiClient::SetApiKey(const FString& Key,FString& Error)
{
	check(IsInGameThread());
	if (!Auth || !Auth->IsA<UVaRestApiKeyAuthConfig>()) { Error=TEXT("AuthTypeMismatch"); return false; }
	auto* Candidate=DuplicateObject<UVaRestApiKeyAuthConfig>(Cast<UVaRestApiKeyAuthConfig>(Auth),this);
	if (!Candidate) { Error=TEXT("AuthConfigurationUnavailable"); return false; }
	Candidate->Key=Key; return ReplaceAuthOnGT(Candidate,Error);
}
bool UVaRestApiClient::SetBasicCredentials(const FString& User,const FString& Password,FString& Error)
{
	check(IsInGameThread());
	if (!Auth || !Auth->IsA<UVaRestBasicAuthConfig>()) { Error=TEXT("AuthTypeMismatch"); return false; }
	auto* Candidate=DuplicateObject<UVaRestBasicAuthConfig>(Cast<UVaRestBasicAuthConfig>(Auth),this);
	if (!Candidate) { Error=TEXT("AuthConfigurationUnavailable"); return false; }
	Candidate->User=User; Candidate->Password=Password; return ReplaceAuthOnGT(Candidate,Error);
}
bool UVaRestApiClient::SetClientCredentials(const FString& ClientId,const FString& ClientSecret,const FString& Scope,FString& Error)
{
	check(IsInGameThread());
	if (!Auth || !Auth->IsA<UVaRestClientCredentialsAuthConfig>()) { Error=TEXT("AuthTypeMismatch"); return false; }
	auto* Candidate=DuplicateObject<UVaRestClientCredentialsAuthConfig>(Cast<UVaRestClientCredentialsAuthConfig>(Auth),this);
	if (!Candidate) { Error=TEXT("AuthConfigurationUnavailable"); return false; }
	Candidate->Config.ClientId=ClientId; Candidate->Config.ClientSecret=ClientSecret; Candidate->Config.Scope=Scope;
	return ReplaceAuthOnGT(Candidate,Error);
}
void UVaRestApiClient::SetSessionHeader(const FString& Name,const FString& Value) { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.SetSessionHeader(Name,Value); }
void UVaRestApiClient::RemoveSessionHeader(const FString& Name) { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.RemoveSessionHeader(Name); }
void UVaRestApiClient::SetHeader(const FString& Name,const FString& Value) { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.WithHeader(Name,Value); }
void UVaRestApiClient::RemoveHeader(const FString& Name) { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.WithoutHeader(Name); }
void UVaRestApiClient::CancelAll(FName Tag) { auto Client=Native; Client.CancelAll(Tag); }
void UVaRestApiClient::ClearCache() { auto Client=Native; if (VaRest::Http::ApiClientPrivate::Live(Client)) Client.ClearCache(); }
void UVaRestApiClient::DetachOnGT()
{
	check(IsInGameThread());
	if (bDetached) return;
	bDetached=true;
	auto Client=MoveTemp(Native);
	Client.OnSessionChanged().Remove(SessionChangedHandle); Client.OnSessionExpired().Remove(SessionExpiredHandle);
	Client.OnQueuedResult().Remove(QueuedHandle);
	if (const auto State=VaRest::Http::FClientAccess::State(Client)) State->PrimaryError.Remove(ErrorHandle);
	VaRest::Http::FClientAccess::CloseManagedAdmission(Client);
	if (Auth) Auth->RetireOnGT();
	VaRest::Http::FClientAccess::RetireManaged(Client);
	Hub.Reset(); Runtime.Reset();
}
void UVaRestApiClient::BeginDestroy() { DetachOnGT(); Super::BeginDestroy(); }
