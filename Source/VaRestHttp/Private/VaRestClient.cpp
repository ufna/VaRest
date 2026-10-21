// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClient.h"

#include "Tasks/Task.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpSettings.h"
#include "VaRestRedactor.h"
#include "VaRestTrace.h"

namespace VaRest::Http::ClientPrivate
{
	/** Decision 33 (review M22): said once, where the base URL is set; each request then fails with Kind=Config */
	void WarnIfFragment(const FString& BaseUrl)
	{
		if (BaseUrl.Contains(TEXT("#")))
		{
			UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: the base URL \"%s\" has a fragment; a base URL may carry a query but not a fragment, so this client's requests fail with Kind=Config"),
				*FVaRestRedactor().RedactUrl(BaseUrl));
		}
	}
}

VaRest::Http::FRequestSpec::FRequestSpec(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState, FStringView InVerb, FStringView InPath)
	: State(MoveTemp(InState))
	, Verb(InVerb)
	, Path(InPath)
{
	if (State.IsValid())
	{
		// Any thread: the configuration is an immutable snapshot behind a lock
		ConfigAtBuild = State->GetConfig();
		Naming = ConfigAtBuild->Naming;
	}
}

VaRest::Http::FRequestSpec::~FRequestSpec() = default;

void VaRest::Http::FRequestSpec::Fail(FString Problem)
{
	if (BuildError.IsEmpty())
	{
		BuildError = MoveTemp(Problem);
	}
}

FVaRestRequestHandle VaRest::Http::SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)
{
	check(Spec.IsValid());
	Spec->bSent = true;
	if (!Spec->State.IsValid())
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: Send() on an empty FVaRestClient (make one with FVaRestClient::Make); the %s request is dropped"), *Spec->Verb);
		return FVaRestRequestHandle();
	}
	FVaRestExchangeRef Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	FVaRestExchange& Ex = *Exchange;
	Ex.RequestId = FVaRestTrace::NewRequestId(); // IN-10: assigned at creation, whatever the trace mask
	Ex.ClientId = Spec->State->GetClientId();
	// Spec 3.1, verbatim: "A request snapshots the configuration at Send()" -- here, on the sending thread, under the
	// state's read lock (decision 27). A struct body was encoded in Body() with the naming of that moment.
	Ex.Config = Spec->State->GetConfig();
	// Moved, not copied (review I-1): the builder dies in the caller after this returns, on the sending thread, and its
	// reference must not be the client state's last one there
	Ex.State = MoveTemp(Spec->State);
	Ex.Verb = MoveTemp(Spec->Verb);
	Ex.RawPath = MoveTemp(Spec->Path);
	Ex.PathParams = MoveTemp(Spec->PathParams);
	Ex.Query = MoveTemp(Spec->Query);
	Ex.OwnHeaders = MoveTemp(Spec->Headers);
	Ex.Body = MoveTemp(Spec->Body);
	Ex.BuildError = MoveTemp(Spec->BuildError);
	Ex.Tag = Spec->Tag;
	Ex.Priority = Spec->Priority;
	Ex.bIdempotent = Spec->bIdempotent;
	Ex.bKeepBody = Spec->bKeepBody;
	Ex.bOnHttpThread = Spec->bOnHttpThread;
	Ex.SaveTo = MoveTemp(Spec->SaveTo);
	Ex.TimeoutOverride = Spec->Timeout;
	Ex.ActivityTimeoutOverride = Spec->ActivityTimeout;
	Ex.MaxJsonBytesOverride = Spec->MaxJsonBytes;
	Ex.Naming = Spec->Naming;
	Ex.bStrictDecode = Spec->bStrictDecode;
	Ex.Owner = Spec->Owner;
	Ex.bHasOwner = Spec->bHasOwner;
	Ex.SourceFile = SourceFile;
	Ex.SourceLine = SourceLine;
	Ex.Handler = MoveTemp(Spec->Handler);

	FVaRestRequestHandle Handle(Exchange);
	if (IsInGameThread())
	{
		Ex.State->Enter(Exchange);
	}
	else
	{
		// Spec 1: "A request may be sent from any thread; it is marshalled to the game thread before entering the pipeline".
		// Review N-3: the builder's configuration goes along -- one the game thread replaced since may hold its last
		// reference, and it then dies with this task's body on the game thread (TaskPrivate.h:871-877), not here
		UE::Tasks::Launch(TEXT("VaRest.Enter"), [Exchange, Built = MoveTemp(Spec->ConfigAtBuild)] { Exchange->State->Enter(Exchange); },
			UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	return Handle;
}

FVaRestRequestHandle::FVaRestRequestHandle(const FVaRestExchangeRef& InExchange)
	: Exchange(InExchange)
	, Id(InExchange->RequestId)
{
}

bool FVaRestRequestHandle::IsActive() const
{
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Exchange.Pin();
	return Pinned.IsValid() && !Pinned->IsDone();
}

FVaRestClient FVaRestClient::Make(FStringView BaseUrl)
{
	checkf(IsInGameThread(), TEXT("FVaRestClient::Make runs on the game thread"));
	const UVaRestHttpSettings* Settings = GetDefault<UVaRestHttpSettings>();
	VaRest::Http::FClientConfig Initial;
	Initial.BaseUrl = FString(BaseUrl.TrimStartAndEnd());
	Initial.DisplayName = Initial.BaseUrl;
	Initial.LogLevel = Settings->LogLevel;
	Initial.MaxConcurrent = FMath::Max(1, Settings->MaxConcurrent);
	Initial.Timeout = FMath::Max(0.f, Settings->Timeout);
	Initial.ActivityTimeout = FMath::Max(0.f, Settings->ActivityTimeout);
	Initial.MaxJsonBytes = FMath::Max<int64>(0, Settings->MaxJsonResponseBytes);
	Initial.RedactKeys = Settings->RedactKeys;
	Initial.Transport = VaRest::Http::GetDefaultTransport();
	Initial.Clock = FVaRestRealClock::Get();
	VaRest::Http::ClientPrivate::WarnIfFragment(Initial.BaseUrl);
	return FVaRestClient(VaRest::Http::FClientState::Create(MoveTemp(Initial)));
}

uint64 FVaRestClient::GetId() const
{
	return State.IsValid() ? State->GetClientId() : 0;
}

FString FVaRestClient::GetBaseUrl() const
{
	return State.IsValid() ? State->GetConfig()->BaseUrl : FString();
}

int32 FVaRestClient::GetInFlightCount() const
{
	check(IsInGameThread());
	return State.IsValid() ? State->NumInFlight() : 0;
}

FVaRestClient& FVaRestClient::WithName(FStringView DisplayName)
{
	if (State.IsValid())
	{
		State->EditConfig([DisplayName](VaRest::Http::FClientConfig& Edited) { Edited.DisplayName = FString(DisplayName); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithBaseUrl(FStringView BaseUrl)
{
	if (State.IsValid())
	{
		VaRest::Http::ClientPrivate::WarnIfFragment(FString(BaseUrl.TrimStartAndEnd()));
		State->EditConfig([BaseUrl](VaRest::Http::FClientConfig& Edited) { Edited.BaseUrl = FString(BaseUrl.TrimStartAndEnd()); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithHeader(FStringView Name, FStringView Value)
{
	if (State.IsValid())
	{
		State->EditConfig([Name, Value](VaRest::Http::FClientConfig& Edited) { Edited.DefaultHeaders.Set(Name, Value); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithoutHeader(FStringView Name)
{
	if (State.IsValid())
	{
		State->EditConfig([Name](VaRest::Http::FClientConfig& Edited) { Edited.DefaultHeaders.Remove(Name); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithNaming(EVaRestJsonNaming Naming)
{
	if (State.IsValid())
	{
		State->EditConfig([Naming](VaRest::Http::FClientConfig& Edited) { Edited.Naming = Naming; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithTransport(TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Transport)
{
	if (State.IsValid())
	{
		State->EditConfig([&Transport](VaRest::Http::FClientConfig& Edited) { Edited.Transport = Transport; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithClock(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock)
{
	if (State.IsValid())
	{
		State->EditConfig([&Clock](VaRest::Http::FClientConfig& Edited) { Edited.Clock = Clock; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::Use(FVaRestStageRef Stage, EVaRestStageSlot Slot)
{
	if (State.IsValid())
	{
		State->EditConfig([&Stage, Slot](VaRest::Http::FClientConfig& Edited) { Edited.CustomStages.Emplace(Stage, Slot); });
	}
	return *this;
}
