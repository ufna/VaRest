// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestApiTypes.h"
#include "Misc/AutomationTest.h"
#include "VaRestBody.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestHttpSettings.h"
#include "VaRestTrace.h"
#include "HAL/PlatformTime.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiTypeDescriptorsTest, "VaRest.Http.Api.TypeDescriptors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiTypeDescriptorsTest::RunTest(const FString& Parameters)
{
	for (uint8 Kind = 0; Kind <= static_cast<uint8>(EVaRestTypeKind::DateTime); ++Kind)
	{
		for (uint8 Container = 0; Container <= static_cast<uint8>(EVaRestTypeContainer::Map); ++Container)
		{
			FVaRestTypeRef Type;
			Type.Kind = static_cast<EVaRestTypeKind>(Kind);
			Type.Container = static_cast<EVaRestTypeContainer>(Container);
			if (Type.Kind == EVaRestTypeKind::Struct)
			{
				Type.Type = FVaRestJson::StaticStruct();
			}
			else if (Type.Kind == EVaRestTypeKind::Enum)
			{
				Type.Type = StaticEnum<EVaRestPriority>();
			}
			FVaRestApiValidationContext Context;
			const bool bValid = Type.Validate(Context, TEXT("ResponseType"));
			const bool bExpected = Container == 0 || (Type.Kind != EVaRestTypeKind::None && Type.Kind != EVaRestTypeKind::Bytes);
			TestEqual(FString::Printf(TEXT("type%d/container%d"), Kind, Container), bValid, bExpected);
			TestEqual(TEXT("validation result agrees with emitted errors"), Context.HasErrors(), !bValid);
		}
	}
	FVaRestTypeRef Type;
	Type.Kind = EVaRestTypeKind::Struct;
	FVaRestApiValidationContext Missing;
	TestFalse(TEXT("a struct descriptor requires its actual reflection type"), Type.Validate(Missing, TEXT("Type")));
	Type.Type = StaticEnum<EVaRestPriority>();
	FVaRestApiValidationContext Wrong;
	TestFalse(TEXT("an enum object cannot stand in for a struct"), Type.Validate(Wrong, TEXT("Type")));
	Type.Kind = EVaRestTypeKind::DateTime;
	FVaRestApiValidationContext Date;
	TestFalse(TEXT("date-time has no arbitrary type object"), Type.Validate(Date, TEXT("Type")));
	Type.Type = nullptr;
	Type.Kind = static_cast<EVaRestTypeKind>(255);
	FVaRestApiValidationContext Corrupt;
	TestFalse(TEXT("unknown serialized kind is refused"), Type.Validate(Corrupt, TEXT("Type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiParameterDescriptorsTest, "VaRest.Http.Api.ParameterDescriptors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiParameterDescriptorsTest::RunTest(const FString& Parameters)
{
	FVaRestEndpointParam Param;
	Param.Name = TEXT("sort");
	Param.PinName = TEXT("SortPin");
	Param.Type.Kind = EVaRestTypeKind::String;
	FVaRestApiValidationContext Valid;
	Param.Validate(Valid, TEXT("Params[0]"));
	TestFalse(TEXT("wire name and distinct pin identity are valid"), Valid.HasErrors());
	Param.In = EVaRestEndpointParamLocation::Path;
	Param.Type.Container = EVaRestTypeContainer::Array;
	FVaRestApiValidationContext Repeat;
	Param.Validate(Repeat, TEXT("Params[0]"));
	TestTrue(TEXT("path arrays cannot silently use repeated-query encoding"), Repeat.HasErrors());
	Param.Style = EVaRestQueryStyle::Csv;
	FVaRestApiValidationContext Csv;
	Param.Validate(Csv, TEXT("Params[0]"));
	TestFalse(TEXT("declared path array uses comma encoding"), Csv.HasErrors());
	Param.Type.Kind = EVaRestTypeKind::Json;
	FVaRestApiValidationContext Json;
	Param.Validate(Json, TEXT("Params[0]"));
	TestTrue(TEXT("an arbitrary JSON body is not a parameter descriptor"), Json.HasErrors());
	Param.Type.Kind = EVaRestTypeKind::String;
	const TCHAR NameUnits[] = {TEXT('i'), TEXT('d'), TCHAR(0), TEXT('s')};
	Param.Name = FString::ConstructFromPtrSize(NameUnits, UE_ARRAY_COUNT(NameUnits));
	if (!TestTrue(TEXT("parameter fixture contains the counted interior NUL"), Param.Name.Len() == 4 && Param.Name[2] == 0))
		return false;
	FVaRestApiValidationContext Null;
	Param.Validate(Null, TEXT("Params[0]"));
	TestTrue(TEXT("embedded null cannot truncate a parameter wire name"), Null.HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiBodyAndHeadersTest, "VaRest.Http.Api.BodyKindsAndOrderedHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiBodyAndHeadersTest::RunTest(const FString& Parameters)
{
	const EVaRestEndpointBodyKind AssetKinds[] = {EVaRestEndpointBodyKind::Json, EVaRestEndpointBodyKind::Form,
		EVaRestEndpointBodyKind::Multipart, EVaRestEndpointBodyKind::Binary, EVaRestEndpointBodyKind::Text};
	const EVaRestBodyKind NativeKinds[] = {EVaRestBodyKind::Json, EVaRestBodyKind::Form,
		EVaRestBodyKind::Multipart, EVaRestBodyKind::Binary, EVaRestBodyKind::Text};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(AssetKinds); ++Index)
	{
		EVaRestBodyKind Native = EVaRestBodyKind::None;
		TestTrue(TEXT("every authored kind has a native mapping"), VaRest::Http::TryGetNativeBodyKind(AssetKinds[Index], Native));
		TestEqual(TEXT("symbolic mapping preserves existing native enum values"), Native, NativeKinds[Index]);
	}
	EVaRestBodyKind Unchanged = EVaRestBodyKind::Stream;
	TestFalse(TEXT("unknown authored kind is refused"), VaRest::Http::TryGetNativeBodyKind(static_cast<EVaRestEndpointBodyKind>(255), Unchanged));
	TestEqual(TEXT("invalid selection does not manufacture a native kind"), Unchanged, EVaRestBodyKind::Stream);
	FVaRestHeaders Native;
	Native.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	Native.Add(TEXT("X-Example"), TEXT("middle"));
	Native.Add(TEXT("Set-Cookie"), TEXT("b=2"));
	const auto Rows = VaRest::Http::ToHeaderData(Native);
	const auto RoundTrip = VaRest::Http::ToNativeHeaders(Rows);
	TestEqual(TEXT("all repeated rows survive reflection conversion"), RoundTrip.Num(), Native.Num());
	for (int32 Index = 0; Index < Native.Num() && Index < RoundTrip.Num(); ++Index)
	{
		TestEqualSensitive(TEXT("header spelling is preserved"), RoundTrip.GetPairs()[Index].Key, Native.GetPairs()[Index].Key);
		TestEqualSensitive(TEXT("header value is preserved"), RoundTrip.GetPairs()[Index].Value, Native.GetPairs()[Index].Value);
	}
	const FVaRestApiEndpoint A;
	const FVaRestApiEndpoint B;
	TestTrue(TEXT("new endpoints get distinct stable identities"), A.Id.IsValid() && B.Id.IsValid() && A.Id != B.Id);
	const FVaRestApiEndpoint Copy = A;
	TestTrue(TEXT("copy/rename preserves endpoint identity"), Copy.Id == A.Id);
	TestEqual(TEXT("new endpoint response defaults to JSON"), A.ResponseType.Kind, EVaRestTypeKind::Json);
	TestEqual(TEXT("new endpoint error has no typed payload"), A.ErrorType.Kind, EVaRestTypeKind::None);
	return true;
}

namespace VaRestInvalidClientCleanupTest
{
class FScopeObserver final : public IVaRestTraceSink
{
public:
	std::atomic<uint64> Expected{0};
	std::atomic<uint32> Closed{0};
	virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Detail; Detail.Flags=EVaRestTraceDetail::Metadata; return Detail; }
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind!=EVaRestTraceKind::Scope) return;
		const auto& Scope=Event.Payload.Get<FVaRestTraceScope>();
		const uint64 Id=Expected.load(std::memory_order_acquire);
		if (Id!=0 && Scope.ScopeId==Id && !Scope.bOpened) Closed.fetch_add(1,std::memory_order_release);
	}
};
class FRun final : public IAutomationLatentCommand
{
public:
	explicit FRun(FAutomationTestBase& InTest) : Test(InTest) {}
	virtual ~FRun() override { Close(); }
	virtual bool Update() override
	{
		if (!bStarted)
		{
			bStarted=true; StartedAt=FPlatformTime::Seconds();
			Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Observer=MakeShared<FScopeObserver,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Observer.ToSharedRef()); bAttached=true;
			Hub.Reset(NewObject<UVaRestHub>()); Api.Reset(NewObject<UVaRestApi>());
			FString Error; const bool Initialized=Hub->InitializeOnGT(nullptr,true,Error);
			if (Test.TestTrue(*FString::Printf(TEXT("owned Hub initializes: %s"),*Error),Initialized))
			{
				Observer->Expected.store(Hub->GetScopeId(),std::memory_order_release);
				Test.TestTrue(TEXT("owned actual scope identity is nonzero"),Hub->GetScopeId()!=0);
				if (Test.TestTrue(TEXT("owned storage profile before client admission"),Hub->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) Exercise();
			}
			// Keep genuine native states only across this synchronous detach check.
			// Release both copies BEFORE waiting for their owned Scope Closed fence.
			{
				auto AssetNative=AssetClient.IsValid() ? AssetClient->GetNativeClient() : FVaRestClient();
				auto PlainNative=PlainClient.IsValid() ? PlainClient->GetNativeClient() : FVaRestClient();
				if (AssetNative.IsValid())
				{
					Test.TestTrue(TEXT("healthy wrapper owns a real session-change subscription"),AssetNative.OnSessionChanged().IsBoundToObject(AssetClient.Get()));
					Test.TestTrue(TEXT("healthy wrapper owns a real session-expired subscription"),AssetNative.OnSessionExpired().IsBoundToObject(AssetClient.Get()));
					Test.TestTrue(TEXT("healthy wrapper owns a real queued-result subscription"),AssetNative.OnQueuedResult().IsBoundToObject(AssetClient.Get()));
				}
				Hub->DeinitializeOnGT(); Hub->DeinitializeOnGT();
				if (AssetNative.IsValid())
				{
					Test.TestFalse(TEXT("detach removes the real session-change subscription"),AssetNative.OnSessionChanged().IsBoundToObject(AssetClient.Get()));
					Test.TestFalse(TEXT("detach removes the real session-expired subscription"),AssetNative.OnSessionExpired().IsBoundToObject(AssetClient.Get()));
					Test.TestFalse(TEXT("detach removes the real queued-result subscription"),AssetNative.OnQueuedResult().IsBoundToObject(AssetClient.Get()));
				}
				if (PlainNative.IsValid()) Test.TestFalse(TEXT("plain-client detach removes its real queue subscription too"),PlainNative.OnQueuedResult().IsBoundToObject(PlainClient.Get()));
			}
			if (AssetClient.IsValid()) Test.TestFalse(TEXT("healthy asset wrapper loses its native client on close"),AssetClient->GetNativeClient().IsValid());
			if (PlainClient.IsValid()) Test.TestFalse(TEXT("healthy plain wrapper loses its native client on close"),PlainClient->GetNativeClient().IsValid());
			Test.TestNull(TEXT("closed Hub cannot admit an asset client"),Hub->GetClient(Api.Get()));
			Test.TestNull(TEXT("closed Hub cannot admit a plain client"),Hub->MakeClient(TEXT("https://invalid-client-cleanup.invalid")));
			return false;
		}
		if (Observer->Expected.load(std::memory_order_acquire)!=0 && Observer->Closed.load(std::memory_order_acquire)==0)
		{
			if (FPlatformTime::Seconds()-StartedAt<15) return false;
			Test.AddError(TEXT("owned client-cleanup scope did not reach its actual Closed barrier")); Close(); return true;
		}
		if (Observer->Expected.load(std::memory_order_acquire)!=0)
			Test.TestEqual(TEXT("repeated Hub close publishes exactly one actual scope Closed"),Observer->Closed.load(std::memory_order_acquire),uint32(1));
		Close(); return true;
	}
private:
	void Exercise()
	{
		FVaRestApiValidationContext Invalid; Api->Validate(Invalid);
		if (!Test.TestTrue(TEXT("real empty API is invalid before native publication"),Invalid.HasErrors())) return;
		Test.AddExpectedMessage(TEXT("VaRest API client refused: InvalidApiConfiguration"),ELogVerbosity::Warning,EAutomationExpectedMessageFlags::Contains,1,false);
		Test.TestNull(TEXT("invalid public GetClient refuses without crashing during cleanup"),Hub->GetClient(Api.Get()));
		const auto* Settings=GetDefault<UVaRestHttpSettings>();
		const FString OriginalCache=Settings->CacheRoot,OriginalQueue=Settings->QueueRoot;
		{
			// Deterministic public MakeClient refusal before any FClientState exists.
			// Restore the process setting at this synchronous GT boundary, before controls.
			auto* MutableSettings=GetMutableDefault<UVaRestHttpSettings>();
			TGuardValue<FString> InvalidCacheRoot(MutableSettings->CacheRoot,FString());
			Test.AddExpectedMessage(TEXT("VaRest client refused: StorageConfig"),ELogVerbosity::Warning,EAutomationExpectedMessageFlags::Contains,1,false);
			Test.TestNull(TEXT("invalid public MakeClient refuses without crashing during cleanup"),Hub->MakeClient(TEXT("https://invalid-client-cleanup.invalid")));
		}
		Test.TestEqualSensitive(TEXT("temporary invalid cache setting restored"),Settings->CacheRoot,OriginalCache);
		Test.TestEqualSensitive(TEXT("queue setting was never modified"),Settings->QueueRoot,OriginalQueue);
		FVaRestApiEnvironment Environment; Environment.Name=TEXT("CleanupControl"); Environment.BaseUrl=TEXT("https://invalid-client-cleanup.invalid/v1");
		Api->Environments.Add(Environment); Api->DefaultEnvironment=Environment.Name; Api->LogLevel=EVaRestLogLevel::None;
		FVaRestApiValidationContext Valid; Api->Validate(Valid);
		if (!Test.TestFalse(TEXT("same API can be repaired into a genuinely valid control"),Valid.HasErrors())) return;
		AssetClient.Reset(Hub->GetClient(Api.Get()));
		if (!Test.TestNotNull(TEXT("failed asset wrapper was removed; repaired API admits a healthy client"),AssetClient.Get())) return;
		Test.TestTrue(TEXT("healthy asset native state is real"),AssetClient->GetNativeClient().IsValid());
		Test.TestTrue(TEXT("healthy asset client remains cached exactly once"),Hub->GetClient(Api.Get())==AssetClient.Get());
		PlainClient.Reset(Hub->MakeClient(TEXT("https://invalid-client-cleanup.invalid/plain")));
		if (!Test.TestNotNull(TEXT("healthy public MakeClient still admits"),PlainClient.Get())) return;
		Test.TestTrue(TEXT("healthy plain native state is real"),PlainClient->GetNativeClient().IsValid());
		Test.TestTrue(TEXT("separate healthy public clients retain distinct actual identities"),AssetClient->GetNativeClient().GetId()!=0 && PlainClient->GetNativeClient().GetId()!=0 && AssetClient->GetNativeClient().GetId()!=PlainClient->GetNativeClient().GetId());
		Test.TestEqual(TEXT("healthy asset client belongs to owned scope"),AssetClient->GetNativeClient().GetScopeId(),Hub->GetScopeId());
		Test.TestEqual(TEXT("healthy plain client belongs to owned scope"),PlainClient->GetNativeClient().GetScopeId(),Hub->GetScopeId());
	}
	void Close()
	{
		if (Hub.IsValid()) Hub->DeinitializeOnGT();
		AssetClient.Reset(); PlainClient.Reset(); Hub.Reset(); Api.Reset();
		if (bAttached) { FVaRestTrace::RemoveSink(Observer.ToSharedRef()); bAttached=false; }
		Observer.Reset(); Isolation.Reset();
	}
	FAutomationTestBase& Test;
	bool bStarted=false,bAttached=false;
	double StartedAt=0;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApi> Api;
	TStrongObjectPtr<UVaRestApiClient> AssetClient,PlainClient;
	TSharedPtr<FScopeObserver,ESPMode::ThreadSafe> Observer;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiInvalidClientCleanupTest,"VaRest.Http.Api.InvalidClientCleanup",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestApiInvalidClientCleanupTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRestInvalidClientCleanupTest::FRun(*this)); return true;
}


namespace VaRestUnicodeStorageTest
{
class FRun final : public IAutomationLatentCommand
{
public:
	explicit FRun(FAutomationTestBase& InTest) : Test(InTest) {}
	virtual ~FRun() override { Close(); }
	virtual bool Update() override
	{
		if (!bStarted)
		{
			bStarted=true; StartedAt=FPlatformTime::Seconds();
			Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
			Observer=MakeShared<VaRestInvalidClientCleanupTest::FScopeObserver,ESPMode::ThreadSafe>();
			FVaRestTrace::AddSink(Observer.ToSharedRef()); bAttached=true;
			Hub.Reset(NewObject<UVaRestHub>());
			FString Error;
			if (Test.TestTrue(TEXT("Unicode fixture owns a real initialized Hub"),Hub->InitializeOnGT(nullptr,true,Error)))
			{
				Observer->Expected.store(Hub->GetScopeId(),std::memory_order_release);
				if (Test.TestTrue(TEXT("Unicode fixture has an actual native scope"),Hub->GetScopeId()!=0)) Exercise();
			}
			// Exercise retains no FVaRestClient alias: the actual state may retire before this owned fence wait.
			Hub->DeinitializeOnGT(); Hub->DeinitializeOnGT();
			if (Client.IsValid()) Test.TestFalse(TEXT("Unicode wrapper has no native alias after Hub close"),Client->GetNativeClient().IsValid());
			return false;
		}
		const uint64 Expected=Observer->Expected.load(std::memory_order_acquire);
		if (Expected!=0 && Observer->Closed.load(std::memory_order_acquire)==0)
		{
			if (FPlatformTime::Seconds()-StartedAt<15) return false;
			Test.AddError(TEXT("Unicode fixture did not reach its owned actual Scope Closed barrier")); Close(); return true;
		}
		if (Expected!=0) Test.TestEqual(TEXT("Unicode fixture closes its real scope exactly once"),Observer->Closed.load(std::memory_order_acquire),uint32(1));
		Close(); return true;
	}
private:
	void Exercise()
	{
		FString Error;
		const auto RejectProfile=[&](const FString& Value,const TCHAR* Expected) {
			Test.TestFalse(TEXT("invalid public storage profile remains refused"),Hub->SetStorageProfile(Value,Error));
			Test.TestEqualSensitive(TEXT("storage profile refusal retains its precise reason"),Error,Expected);
		};
		const TCHAR WithNul[]={TEXT('a'),TCHAR(0),TEXT('b')};
		RejectProfile(FString(),TEXT("InvalidStoragePartition"));
		RejectProfile(FString::ConstructFromPtrSize(WithNul,UE_ARRAY_COUNT(WithNul)),TEXT("InvalidStoragePartition"));
		RejectProfile(TEXT("bad\x7f"),TEXT("InvalidStoragePartition"));
		RejectProfile(FString::ChrN(257,TEXT('p')),TEXT("InvalidStoragePartition"));
		FString BadHigh,BadLow;
		if constexpr (sizeof(TCHAR)>1)
		{
			const TCHAR High[]={TEXT('x'),TCHAR(0xd800),TEXT('y')};
			const TCHAR Low[]={TEXT('x'),TCHAR(0xdc00),TEXT('y')};
			BadHigh=FString::ConstructFromPtrSize(High,UE_ARRAY_COUNT(High));
			BadLow=FString::ConstructFromPtrSize(Low,UE_ARRAY_COUNT(Low));
			RejectProfile(BadHigh,TEXT("InvalidStoragePartitionEncoding"));
			RejectProfile(BadLow,TEXT("InvalidStoragePartitionEncoding"));
		}
		if (!Test.TestTrue(TEXT("exact 256-unit profile boundary remains accepted"),Hub->SetStorageProfile(FString::ChrN(256,TEXT('p')),Error))) return;
		const FString Unique=FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Profile=TEXT("Profile_\u00d6_\u6f22_")+Unique;
		const bool bProfileAccepted=Hub->SetStorageProfile(Profile,Error);
		if (!Test.TestTrue(*FString::Printf(TEXT("real Unicode storage profile accepted: %s"),*Error),bProfileAccepted)) return;
		Test.TestTrue(TEXT("Unicode profile success clears prior validation errors"),Error.IsEmpty());
		Api.Reset(NewObject<UVaRestApi>(Hub.Get(),FName(*(TEXT("Api_\u00d6_")+Unique+TEXT("_47")))));
		Test.TestTrue(TEXT("actual full API object path contains Unicode"),Api->GetPathName().Contains(TEXT("\u00d6"),ESearchCase::CaseSensitive));
		FVaRestApiEnvironment Fallback;
		Fallback.Name=FName(*(TEXT("Fallback_\u00d6_")+Unique+TEXT("_19")));
		Fallback.BaseUrl=TEXT("https://unicode-storage.invalid/fallback");
		FVaRestApiEnvironment Explicit;
		Explicit.Name=FName(*(TEXT("Explicit_")+Unique+TEXT("_29")));
		Explicit.QueueNamespace=TEXT("ExactNamespace_\u00d6_\u6f22");
		Explicit.BaseUrl=TEXT("https://unicode-storage.invalid/explicit");
		Api->Environments.Add(Fallback); Api->Environments.Add(Explicit); Api->DefaultEnvironment=Fallback.Name;
		Api->LogLevel=EVaRestLogLevel::None;
		FVaRestApiValidationContext Validation; Api->Validate(Validation);
		if (!Test.TestFalse(TEXT("actual Unicode API configuration is valid"),Validation.HasErrors())) return;
		Client.Reset(Hub->GetClient(Api.Get()));
		if (!Test.TestNotNull(TEXT("public GetClient accepts the Unicode fallback environment"),Client.Get())) return;
		{
			const FVaRestClient Native=Client->GetNativeClient();
			Test.TestTrue(TEXT("Unicode client owns a real native state"),Native.IsValid());
			Test.TestEqual(TEXT("Unicode client is in this actual owned scope"),Native.GetScopeId(),Hub->GetScopeId());
		}
		Test.TestEqual(TEXT("actual fallback environment is selected"),Client->GetEnvironment(),Fallback.Name);
		Test.TestEqualSensitive(TEXT("empty account does not create a named queue target"),Client->GetOfflineQueueError(),TEXT("QueueAccountRequired"));
		const FString Account=TEXT("ExactAccount_\u00d6_\u6f22");
		const bool bAccountAccepted=Client->SetQueueAccount(Account,Error);
		if (!Test.TestTrue(*FString::Printf(TEXT("Unicode account and full Unicode API path accepted: %s"),*Error),bAccountAccepted)) return;
		Test.TestTrue(TEXT("successful Unicode account selection clears errors"),Error.IsEmpty());
		Test.TestEqual(TEXT("account selection alone never activates the queue"),Client->GetOfflineQueueState(),EVaRestOfflineQueueState::Paused);
		Test.TestEqualSensitive(TEXT("nonempty Unicode target is genuinely configured but not activated"),Client->GetOfflineQueueError(),TEXT("QueueNotActivated"));
		const auto RejectAccount=[&](const FString& Value,const TCHAR* Expected) {
			Test.TestFalse(TEXT("invalid public queue account remains refused"),Client->SetQueueAccount(Value,Error));
			Test.TestEqualSensitive(TEXT("queue account refusal retains its precise reason"),Error,Expected);
			Test.TestEqualSensitive(TEXT("refused account leaves the actual configured target intact"),Client->GetOfflineQueueError(),TEXT("QueueNotActivated"));
		};
		RejectAccount(FString::ConstructFromPtrSize(WithNul,UE_ARRAY_COUNT(WithNul)),TEXT("InvalidStoragePartition"));
		RejectAccount(TEXT("bad\x01"),TEXT("InvalidStoragePartition"));
		RejectAccount(FString::ChrN(257,TEXT('a')),TEXT("InvalidStoragePartition"));
		if constexpr (sizeof(TCHAR)>1)
		{
			RejectAccount(BadHigh,TEXT("InvalidStoragePartitionEncoding"));
			RejectAccount(BadLow,TEXT("InvalidStoragePartitionEncoding"));
			Api->Environments[1].QueueNamespace=BadHigh;
			Test.TestFalse(TEXT("malformed explicit namespace is refused by public environment selection"),Client->SetEnvironment(Explicit.Name,Error));
			Test.TestEqualSensitive(TEXT("explicit namespace refusal has the encoding reason"),Error,TEXT("InvalidStoragePartitionEncoding"));
			Test.TestEqual(TEXT("failed namespace selection leaves the actual fallback environment"),Client->GetEnvironment(),Fallback.Name);
			Test.TestEqualSensitive(TEXT("failed namespace selection leaves the actual base URL"),Client->GetBaseUrl(),Fallback.BaseUrl);
			Api->Environments[1].QueueNamespace=Explicit.QueueNamespace;
		}
		if (!Test.TestTrue(TEXT("public environment switch accepts exact Unicode explicit namespace"),Client->SetEnvironment(Explicit.Name,Error))) return;
		Test.TestEqual(TEXT("actual explicit environment is now selected"),Client->GetEnvironment(),Explicit.Name);
		Test.TestEqualSensitive(TEXT("actual explicit environment target is published"),Client->GetBaseUrl(),Explicit.BaseUrl);
		Test.TestEqualSensitive(TEXT("explicit Unicode target remains unactivated"),Client->GetOfflineQueueError(),TEXT("QueueNotActivated"));
		if (!Test.TestTrue(TEXT("empty account remains an accepted deliberate opt-out"),Client->SetQueueAccount(FString(),Error))) return;
		Test.TestEqualSensitive(TEXT("empty account returns the actual target to account-required"),Client->GetOfflineQueueError(),TEXT("QueueAccountRequired"));
		Test.TestEqual(TEXT("empty account never activates storage"),Client->GetOfflineQueueState(),EVaRestOfflineQueueState::Paused);
		if (!Test.TestTrue(TEXT("Unicode account can be selected again after genuine opt-out"),Client->SetQueueAccount(Account,Error))) return;
		Test.TestEqualSensitive(TEXT("reselected Unicode target is configured but still unactivated"),Client->GetOfflineQueueError(),TEXT("QueueNotActivated"));
	}
	void Close()
	{
		if (Hub.IsValid()) Hub->DeinitializeOnGT();
		Client.Reset(); Api.Reset(); Hub.Reset();
		if (bAttached) { FVaRestTrace::RemoveSink(Observer.ToSharedRef()); bAttached=false; }
		Observer.Reset(); Isolation.Reset();
	}
	FAutomationTestBase& Test;
	bool bStarted=false,bAttached=false;
	double StartedAt=0;
	TStrongObjectPtr<UVaRestHub> Hub;
	TStrongObjectPtr<UVaRestApi> Api;
	TStrongObjectPtr<UVaRestApiClient> Client;
	TSharedPtr<VaRestInvalidClientCleanupTest::FScopeObserver,ESPMode::ThreadSafe> Observer;
	TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiUnicodeStorageTest,"VaRest.Http.Api.UnicodeStoragePartitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestApiUnicodeStorageTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRestUnicodeStorageTest::FRun(*this)); return true;
}

#endif
