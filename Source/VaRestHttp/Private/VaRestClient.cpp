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
				*EscapeForLog(FVaRestRedactor().RedactUrl(BaseUrl)));
		}
	}

	/** Two keys the rules treat as one: equal after FVaRestRedactRules::Normalize */
	bool SameRedactKey(const FString& A, const FString& B)
	{
		const FTCHARToUTF8 AUtf8(*A);
		const FTCHARToUTF8 BUtf8(*B);
		TArray<UTF8CHAR, TInlineAllocator<64>> ANormal;
		TArray<UTF8CHAR, TInlineAllocator<64>> BNormal;
		FVaRestRedactRules::Normalize(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(AUtf8.Get()), AUtf8.Length()), ANormal);
		FVaRestRedactRules::Normalize(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(BUtf8.Get()), BUtf8.Length()), BNormal);
		return ANormal == BNormal;
	}

	/**
	 * Task 14 review M4: a key is checked and de-duplicated once, where it enters. Publish adds every stored key to new rules
	 * on each edit (a header, a log level, each SetSecretValue), so a refused key stored there would warn every time.
	 */
	void AppendRedactKeys(TArray<FString>& Into, TConstArrayView<FString> Keys)
	{
		for (const FString& Key : Keys)
		{
			if (!FVaRestRedactRules::IsAcceptableKey(Key))
			{
				UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest: the redaction key \"%s\" holds '?', ';', '#', '=' or '&', on which URL and form redaction split keys; it is ignored"),
					*EscapeForLog(Key));
				continue;
			}
			if (!Into.ContainsByPredicate([&Key](const FString& Known) { return SameRedactKey(Known, Key); }))
			{
				Into.Add(Key);
			}
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

VaRest::Http::FRequestSpec::~FRequestSpec()
{
#if !UE_BUILD_SHIPPING
	if (!bSent && ConfigAtBuild.IsValid())
	{
		// Spec 3.2: "In non-Shipping builds a builder destroyed unsent also logs an error naming the URL" -- redacted like every log line
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: a %s request to %s was built but never sent (Send() is missing)"),
			*EscapeForLog(Verb), *EscapeForLog(ConfigAtBuild->Redactor->RedactUrl(JoinUrl(ConfigAtBuild->BaseUrl, Path))));
	}
#endif
}

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
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: Send() on an empty FVaRestClient (make one with FVaRestClient::Make); the %s request is dropped"), *EscapeForLog(Spec->Verb));
		return FVaRestRequestHandle();
	}
	// A TSharedPtr, so that the worker path can move it into the Enter task (review I-1; a TSharedRef "move" copies,
	// SharedPointer.h:354-361): this thread then holds no reference that could be the last one
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Created = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	FExchangeState& Ex = Internals(*Created);
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

	FVaRestRequestHandle Handle(Created.ToSharedRef());
	if (IsInGameThread())
	{
		Ex.bSentOnGameThread = true;
		Ex.State->Enter(Created.ToSharedRef());
	}
	else
	{
		// Spec 1: "A request may be sent from any thread; it is marshalled to the game thread before entering the pipeline".
		// The exchange is moved in, never copied (review I-1): were the game thread to run the whole request before this
		// function returns, a reference kept here would be the last. Ex is not touched after the Launch. Review N-3: the
		// builder's configuration goes along -- one the game thread replaced since may hold its last reference, and it
		// then dies with this task's body on the game thread (TaskPrivate.h:871-877), not here.
		UE::Tasks::Launch(TEXT("VaRest.Enter"), [Moved = MoveTemp(Created), Built = MoveTemp(Spec->ConfigAtBuild)]
		{
			const FVaRestExchangeRef Entering = Moved.ToSharedRef();
			Internals(*Entering).State->Enter(Entering);
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	return Handle;
}

FVaRestRequestHandle::FVaRestRequestHandle(const FVaRestExchangeRef& InExchange)
	: Exchange(InExchange)
	, Id(InExchange->GetRequestId())
{
}

bool FVaRestRequestHandle::IsActive() const
{
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Exchange.Pin();
	return Pinned.IsValid() && !Pinned->IsDone();
}

void FVaRestRequestHandle::Cancel() const
{
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Exchange.Pin();
	if (!Pinned.IsValid() || Pinned->IsDone())
	{
		return;
	}
	if (IsInGameThread())
	{
		VaRest::Http::Internals(*Pinned).State->Cancel(Pinned.ToSharedRef(), EVaRestCancelReason::User);
		return;
	}
	// Any thread: the flag now (a decode in flight sees it), the rest where the control plane lives (spec 1). The pin is
	// moved into the task, so this thread never drops the last reference (review I-1).
	VaRest::Http::Internals(*Pinned).bCancelRequested = true;
	UE::Tasks::Launch(TEXT("VaRest.Cancel"), [Target = MoveTemp(Pinned)] { VaRest::Http::Internals(*Target).State->Cancel(Target.ToSharedRef(), EVaRestCancelReason::User); },
		UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

FVaRestClient FVaRestClient::Make(FStringView BaseUrl)
{
	checkf(IsInGameThread(), TEXT("FVaRestClient::Make runs on the game thread"));
	const UVaRestHttpSettings* Settings = GetDefault<UVaRestHttpSettings>();
	VaRest::Http::FClientConfig Initial;
	Initial.BaseUrl = FString(BaseUrl.TrimStartAndEnd());
	// No DisplayName: until WithName, the client is named by its base URL, whichever one it has then (IN-8)
	Initial.LogLevel = Settings->LogLevel;
	Initial.MaxConcurrent = FMath::Max(1, Settings->MaxConcurrent);
	Initial.Timeout = FMath::Max(0.f, Settings->Timeout);
	Initial.ActivityTimeout = FMath::Max(0.f, Settings->ActivityTimeout);
	Initial.MaxJsonBytes = FMath::Max<int64>(0, Settings->MaxJsonResponseBytes);
	VaRest::Http::ClientPrivate::AppendRedactKeys(Initial.RedactKeys, Settings->RedactKeys);
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

void FVaRestClient::CancelAll(FName InTag)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->CancelAll(InTag, EVaRestCancelReason::User);
	}
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
	if (!VaRest::Http::IsValidHeaderName(Name) || !VaRest::Http::IsValidHeaderValue(Value))
	{
		// Decision 11; no request to fail here, so the header is refused and the error logged
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: invalid default header \"%s\" (not a token, or the value holds a control character); not set"),
			*VaRest::Http::EscapeForLog(Name));
		return *this;
	}
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
		State->SetClock(Clock);
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithMaxJsonBytes(int64 Bytes)
{
	if (State.IsValid())
	{
		State->EditConfig([Bytes](VaRest::Http::FClientConfig& Edited) { Edited.MaxJsonBytes = FMath::Max<int64>(0, Bytes); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithMaxConcurrent(int32 MaxConcurrent)
{
	if (State.IsValid())
	{
		State->EditConfig([MaxConcurrent](VaRest::Http::FClientConfig& Edited) { Edited.MaxConcurrent = FMath::Max(1, MaxConcurrent); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithTimeout(float Total, float Activity)
{
	if (State.IsValid())
	{
		State->EditConfig([Total, Activity](VaRest::Http::FClientConfig& Edited)
		{
			Edited.Timeout = FMath::Max(0.f, Total);
			Edited.ActivityTimeout = FMath::Max(0.f, Activity);
		});
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithLogLevel(EVaRestLogLevel Level)
{
	if (State.IsValid())
	{
		State->EditConfig([Level](VaRest::Http::FClientConfig& Edited) { Edited.LogLevel = Level; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithRedactedKeys(TConstArrayView<FString> Keys)
{
	if (State.IsValid())
	{
		State->EditConfig([Keys](VaRest::Http::FClientConfig& Edited) { VaRest::Http::ClientPrivate::AppendRedactKeys(Edited.RedactKeys, Keys); });
	}
	return *this;
}

void FVaRestClient::SetSecretValue(FName Slot, const FString& Value)
{
	if (State.IsValid())
	{
		State->SetSecret(Slot, Value);
	}
}

void FVaRestClient::ClearSecretValues()
{
	if (State.IsValid())
	{
		State->ClearSecrets();
	}
}

FVaRestClient& FVaRestClient::Use(FVaRestStageRef Stage, EVaRestStageSlot Slot)
{
	if (State.IsValid())
	{
		State->EditConfig([&Stage, Slot](VaRest::Http::FClientConfig& Edited) { Edited.CustomStages.Emplace(Stage, Slot); });
	}
	return *this;
}
