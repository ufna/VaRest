// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHubRuntime.h"
#include "VaRestHttpSettings.h"
#include "VaRestNativeLifetimeService.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

namespace VaRest::Http
{
bool FHubRuntime::ValidatePartition(FStringView Text,bool bAllowEmpty,FString& Error)
{
	if ((!bAllowEmpty && Text.IsEmpty()) || Text.Len()>256) { Error=TEXT("InvalidStoragePartition"); return false; }
	for (TCHAR Character:Text) if (Character<0x20 || Character==0x7f) { Error=TEXT("InvalidStoragePartition"); return false; }
	const FString Original(Text);
	const FTCHARToUTF8 Utf8(*Original);
	const FString RoundTrip=FString::ConstructFromPtrSize(Utf8.Get(),Utf8.Length());
	if (Original.Len()!=RoundTrip.Len() || (!Original.IsEmpty() && FMemory::Memcmp(*Original,*RoundTrip,Original.Len()*sizeof(TCHAR))!=0))
		{ Error=TEXT("InvalidStoragePartitionEncoding"); return false; }
	return true;
}
bool FHubRuntime::QueueName(FStringView ApiPath,FStringView StorageProfile,FStringView Environment,FStringView Account,FString& Name,FString& Error)
{
	Name.Reset(); Error.Reset();
	if (!ValidatePartition(StorageProfile,false,Error) || !ValidatePartition(Environment,false,Error) || !ValidatePartition(Account,true,Error)) return false;
	if (Account.IsEmpty()) return true; // explicit account is required before any native named registration exists
	if (ApiPath.IsEmpty() || ApiPath.Len()>1024) { Error=TEXT("InvalidApiStoragePath"); return false; }
	FString Result(TEXT("M5/v1"));
	int32 FieldIndex=0;
	for (FStringView Field:{ApiPath,StorageProfile,Environment,Account})
	{
		for (TCHAR Character:Field) if (Character<0x20 || Character==0x7f) { Error=TEXT("InvalidStoragePartition"); return false; }
		FString Text(Field);
		if (FieldIndex++==0) // Only the complete API path is FName-derived here.
		{
			for (int32 Index=0;Index<Text.Len();++Index)
			{
				const TCHAR Character=Text[Index];
				if (Character>=0x41 && Character<=0x5a)
					Text[Index]=static_cast<TCHAR>(Character+0x20); // ASCII A-Z only.
			}
		}
		const FTCHARToUTF8 Utf8(*Text);
		const FString RoundTrip=FString::ConstructFromPtrSize(Utf8.Get(),Utf8.Length());
		if (Text.Len()!=RoundTrip.Len() || (!Text.IsEmpty() && FMemory::Memcmp(*Text,*RoundTrip,Text.Len()*sizeof(TCHAR))!=0))
			{ Error=TEXT("InvalidStoragePartitionEncoding"); return false; }
		Result+=FString::Printf(TEXT("%d:"),Utf8.Length()); Result+=Text;
		if (Result.Len()>1024) { Error=TEXT("OfflineQueueNameTooLong"); return false; }
	}
	Name=MoveTemp(Result); return true;
}
TSharedPtr<FHubRuntime,ESPMode::ThreadSafe> FHubRuntime::CreateOnGT(FVaRestTraceScope Info,FString Profile,FString& Error)
{
	check(IsInGameThread());
	if (FClientState::IsShuttingDown() || !NativeControlForNewClientOnGT()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	if (!ValidatePartition(Profile,false,Error)) return nullptr;
	const auto Runtime=MakeShared<FHubRuntime,ESPMode::ThreadSafe>();
	Runtime->Profile=MoveTemp(Profile);
	Runtime->Connectivity=FVaRestConnectivity::Make(FVaRestRealClock::Get()); // one observation per actual host
	Runtime->DynamicBudget=FVaRestDynamicBudget::Create(FVaRestDynamicLimits::FromSettingsOnGT(*GetDefault<UVaRestHttpSettings>()),Error);
	if (!Runtime->DynamicBudget) return nullptr;
	Runtime->Scope=FNativeScopeLifetime::Open(MoveTemp(Info),FPlatformTime::Seconds());
	if (!Runtime->Scope) { Error=TEXT("ScopeCapacity"); return nullptr; }
	Error.Reset(); return Runtime;
}
FHubRuntime::~FHubRuntime() { check(IsInGameThread()); RetireOnGT(); }
bool FHubRuntime::SetStorageProfileOnGT(FString NewProfile,FString& Error)
{
	check(IsInGameThread());
	if (bClosed || bEverAdmitted || Creating!=0) { Error=TEXT("StorageProfileAlreadyInUse"); return false; }
	if (!ValidatePartition(NewProfile,false,Error)) return false;
	Profile=MoveTemp(NewProfile); Error.Reset(); return true;
}
FVaRestClient FHubRuntime::CreateClientOnGT(FClientConfig&& Initial,FString& Error,TFunction<void(const FVaRestClient&)> BeforeCreated)
{
	check(IsInGameThread());
	const auto Self=AsShared();
	if (bClosed || !Scope || !Scope->AcceptsClientsOnGT()) { Error=TEXT("LifecycleClosed"); return {}; }
	++Creating;
	ON_SCOPE_EXIT { --Creating; };
	// A stable profile is exclusive among live hosts before their first client publication. Weak bookkeeping is bounded
	// by native client capacity; inactive/failed/retired owners do not keep a profile reserved.
	static auto* Profiles=new TArray<TWeakPtr<FHubRuntime,ESPMode::ThreadSafe>>();
	bool bClaimed=false;
	for (int32 Index=Profiles->Num()-1;Index>=0;--Index)
	{
		const auto Other=(*Profiles)[Index].Pin();
		if (!Other || Other->bClosed || (!Other->bEverAdmitted && Other->Creating==0)) { Profiles->RemoveAtSwap(Index); continue; }
		if (Other.Get()==this) { bClaimed=true; continue; }
		if (Profile.Equals(Other->Profile,ESearchCase::CaseSensitive)) { Error=TEXT("StorageProfileInUse"); return {}; }
	}
	if (!bClaimed)
	{
		if (Profiles->Num()>=256) { Error=TEXT("LifecycleCapacity"); return {}; }
		Profiles->Add(Self);
	}
	Initial.Connectivity=Connectivity;
	FName Reason;
	FVaRestClient Client=FClientAccess::CreateManaged(MoveTemp(Initial),Scope.ToSharedRef(),Reason,
		[Self,Before=MoveTemp(BeforeCreated)](const FVaRestClient& Published) {
			Self->Clients.Add(Published); Self->bEverAdmitted=true; // includes this client in a Created-sink Hub close
			if (Self->bClosed) FClientAccess::CloseManagedAdmission(Published);
			if (Before) Before(Published);
		});
	if (!Client.IsValid()) { Error=Reason.ToString(); return {}; }
	if (bClosed) { FClientAccess::RetireManaged(Client); Error=TEXT("LifecycleClosed"); return {}; }
	Error.Reset(); return Client;
}
void FHubRuntime::CloseAdmissionOnGT()
{
	check(IsInGameThread());
	if (bClosed) return;
	bClosed=true;
	for (const auto& Client:Clients) FClientAccess::CloseManagedAdmission(Client); // all gates FIRST, no user delegate
}
void FHubRuntime::RetireOnGT()
{
	check(IsInGameThread());
	if (bRetired) return;
	CloseAdmissionOnGT(); bRetired=true;
	auto Retiring=MoveTemp(Clients); // reentrant destruction/callouts cannot mutate this live inventory
	for (const auto& Client:Retiring) FClientAccess::RetireManaged(Client);
	if (Scope) Scope->RequestRetirementOnGT(); // actual native slots/fences, rather than wrapper reachability, close it
}
}
