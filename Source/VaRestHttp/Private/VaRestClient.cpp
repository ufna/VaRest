// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClient.h"

#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestAuth.h"
#include "VaRestAuthInternal.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestHttpTesting.h"
#include "VaRestRedactor.h"
#include "VaRestRequestSpec.h"
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
}

/**
 * Task 14 review M4: a key is checked and de-duplicated once, where it enters. Publish adds every stored key to new rules
 * on each edit (a header, a log level, each SetSecretValue), so a refused key stored there would warn every time. Out of
 * ClientPrivate: a session header's name enters here too (VaRestSession.cpp).
 */
void VaRest::Http::AppendRedactKeys(TArray<FString>& Into, TConstArrayView<FString> Keys)
{
	for (const FString& Key : Keys)
	{
		if (!FVaRestRedactRules::IsAcceptableKey(Key))
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest: the redaction key \"%s\" holds '?', ';', '#', '=' or '&', on which URL and form redaction split keys; it is ignored"),
				*EscapeForLog(Key));
			continue;
		}
		if (!Into.ContainsByPredicate([&Key](const FString& Known) { return ClientPrivate::SameRedactKey(Known, Key); }))
		{
			Into.Add(Key);
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
		// Spec 3.2: "In non-Shipping builds a builder destroyed unsent also logs an error naming the URL" -- redacted like every
		// log line, with its Query() pairs (task 9 review minor 2); the path template is shown as written
		FString Unsent = JoinUrl(ConfigAtBuild->BaseUrl, Path);
		AppendQuery(Unsent, Query);
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: a %s request to %s was built but never sent (Send() is missing)"),
			*EscapeForLog(Verb), *EscapeForLog(ConfigAtBuild->Redactor->RedactUrl(Unsent)));
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

/**
 * Final review M7: a builder of a default-constructed FVaRestClient has no client state, so no exchange and no pipeline,
 * but a UI waiting on OnComplete must not hang. Its handler gets Kind=Config from a later game-thread task (decision 3:
 * never inside Send(), from whichever thread Send() ran on), with Complete's rules: nothing for an owner that is gone or
 * at engine exit (spec 3.2, decision 2); with OnHttpThread() the callbacks run on a worker, the owner pinned for them
 * (decisions 4, 28). The handler -- the user's callbacks and their captures -- dies where Complete's would.
 */
void VaRest::Http::FRequestSpecAccess::DeliverWithoutClient(FRequestSpec& Spec)
{
	if (!Spec.Handler.IsValid())
	{
		return;
	}
	FFinish Failed;
	Failed.Error.Kind = EVaRestErrorKind::Config;
	Failed.Error.Message = TEXT("Send() on an empty FVaRestClient: make one with FVaRestClient::Make");
	Failed.Error.Tag = Spec.Tag;
	// Spec 3.3: "Url (redacted)". No client, so no client keys or secrets: the built-in rules and userinfo.
	Failed.Error.Url = FVaRestRedactor().RedactUrl(Spec.Path);
	UE::Tasks::Launch(TEXT("VaRest.DeliverWithoutClient"),
		[Callbacks = MoveTemp(Spec.Handler), Pending = MoveTemp(Failed), WeakOwner = Spec.Owner, bOwned = Spec.bHasOwner, bOffGameThread = Spec.bOnHttpThread]() mutable
		{
			if ((bOwned && !WeakOwner.IsValid()) || FClientState::IsShuttingDown())
			{
				return;
			}
			if (!bOffGameThread)
			{
				Callbacks->Deliver(Pending);
				return;
			}
			// As Complete's OnHttpThread() branch (VaRestPipeline.cpp, decision 28): pinned here, released on the worker
			TStrongObjectPtr<const UObject> Pin;
			if (bOwned)
			{
				Pin.Reset(WeakOwner.Get());
			}
#if !UE_BUILD_SHIPPING
			++Testing::Counters().OffGameThreadDeliveries;
#endif
			UE::Tasks::Launch(TEXT("VaRest.DeliverOffGameThread"), [Delivered = MoveTemp(Callbacks), Done = MoveTemp(Pending), Kept = MoveTemp(Pin)]() mutable
			{
				Delivered->Deliver(Done);
				Kept.Reset();
			});
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

FVaRestRequestHandle VaRest::Http::FRequestSpecAccess::Send(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)
{
	check(Spec.IsValid());
	Spec->bSent = true;
	if (!Spec->State.IsValid())
	{
		// Spec 4.8: no pipeline, so no request in stat VaRest. Before the VaRest scope below: the delivery task takes the
		// sender's LLM tags at its launch (TaskPrivate.h:192), so the game's callbacks are not charged to VaRest.
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: Send() on an empty FVaRestClient (make one with FVaRestClient::Make); the %s request is not sent, and its callbacks get Kind=Config"),
			*EscapeForLog(Spec->Verb));
		DeliverWithoutClient(*Spec);
		return FVaRestRequestHandle();
	}
	// Spec 4.8: the sender's tags, for the callbacks, are taken before VaRest's own scope starts
	FLlmSenderTags Sender;
	Sender.Capture();
	LLM_SCOPE_BYTAG(VaRest);
	// A TSharedPtr, so that the worker path can move it into the Enter task (review I-1; a TSharedRef "move" copies,
	// SharedPointer.h:354-361): this thread then holds no reference that could be the last one
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Created = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	FExchangeState& Ex = Internals(*Created);
	// IN-10: assigned at creation, whatever the trace mask; a refresh request takes its refresh's id (spec review I-2)
	Ex.RequestId = Spec->PresetRequestId != 0 ? Spec->PresetRequestId : FVaRestTrace::NewRequestId();
	Ex.ClientId = Spec->State->GetClientId();
	// Spec 3.1, verbatim: "A request snapshots the configuration at Send()" -- here, on the sending thread, under the
	// state's read lock (decision 27). A struct body was encoded in Body() with the naming of that moment.
	Ex.Config = Spec->State->GetConfig();
	Ex.PersistenceGuard = Ex.Config->PersistenceGuard;
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
	Ex.RetryOverride = MoveTemp(Spec->Retry);
	Ex.EnvelopeOverride = MoveTemp(Spec->Envelope);
	Ex.bNoEnvelope = Spec->bNoEnvelope;
	Ex.AuthMode = Spec->AuthMode;
	Ex.bIsAuthRefresh = Spec->bAuthRefresh;
	Ex.Owner = Spec->Owner;
	Ex.bHasOwner = Spec->bHasOwner;
	Ex.SourceFile = SourceFile;
	Ex.SourceLine = SourceLine;
	Ex.SenderTags = Sender;
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

FVaRestRequestHandle VaRest::Http::SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)
{
	return FRequestSpecAccess::Send(MoveTemp(Spec), SourceFile, SourceLine);
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
	VaRest::Http::Internals(*Pinned).PersistenceAbort->store(true, std::memory_order_relaxed);
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
	Initial.MaxJsonBytes = FMath::Max<int64>(0, Settings->MaxJsonBytes);
	VaRest::Http::AppendRedactKeys(Initial.RedactedKeys, Settings->RedactedKeys);
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

FVaRestClient& FVaRestClient::WithRandomSeed(int32 Seed)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->GetRandom().Initialize(Seed);
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithRetry(const FVaRestRetryPolicy& Policy)
{
	if (State.IsValid())
	{
		State->EditConfig([&Policy](VaRest::Http::FClientConfig& Edited) { Edited.Retry = Policy; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithEnvelope(const FVaRestEnvelope& InEnvelope)
{
	if (State.IsValid())
	{
		// One immutable copy, shared by every request that snapshots this configuration and read on their decode tasks
		TSharedRef<const FVaRestEnvelope, ESPMode::ThreadSafe> Shared = MakeShared<const FVaRestEnvelope, ESPMode::ThreadSafe>(InEnvelope);
		State->EditConfig([&Shared](VaRest::Http::FClientConfig& Edited) { Edited.Envelope = Shared; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithoutEnvelope()
{
	if (State.IsValid())
	{
		State->EditConfig([](VaRest::Http::FClientConfig& Edited) { Edited.Envelope.Reset(); });
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
		State->EditConfig([Keys](VaRest::Http::FClientConfig& Edited) { VaRest::Http::AppendRedactKeys(Edited.RedactedKeys, Keys); });
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

TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> FVaRestClient::GetRedactor() const
{
	check(IsInGameThread());
	// The published one, as every line about the client's requests starts from (FClientState::GetRedactor); an empty client
	// has none to give, and a fresh one with nothing in it redacts what the default rules do
	return State.IsValid() ? State->GetRedactor() : MakeShared<const FVaRestRedactor, ESPMode::ThreadSafe>();
}

void FVaRestClient::SetBearerToken(FStringView AccessToken, bool bSameIdentity)
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return;
	}
	if (AccessToken.IsEmpty())
	{
		ClearSession();
		return;
	}
	if (!VaRest::Http::IsValidHeaderValue(AccessToken))
	{
		// The token is not in the line: it is a secret, and the reason is enough
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetBearerToken: the token holds CR, LF, NUL or another control character; not set"));
		return;
	}
	// Concurrency review C-1: every session change goes through RunSessionChange, which keeps the state alive past a
	// listener that drops this client; nothing below touches this object after it returns
	const FString Token(AccessToken);
	State->RunSessionChange([&Token, bSameIdentity](VaRest::Http::FClientState& Target) { Target.SetTokensByGame(Token, nullptr, 0.0, 0.0, bSameIdentity); });
}

void FVaRestClient::SetSession(FStringView AccessToken, FStringView RefreshToken, double ExpiresInSeconds, bool bSameIdentity)
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return;
	}
	if (!VaRest::Http::IsValidHeaderValue(AccessToken) || !VaRest::Http::IsValidHeaderValue(RefreshToken))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetSession: a token holds CR, LF, NUL or another control character; not set"));
		return;
	}
	if (AccessToken.IsEmpty() && RefreshToken.IsEmpty())
	{
		// Review M-4: as SetBearerToken(""). A session with no token is a logout, not another identity's login: W5's socket
		// manager would restart its sockets on Replaced with nothing to send.
		ClearSession();
		return;
	}
	const FString Access(AccessToken);
	const FString Refresh(RefreshToken);
	State->RunSessionChange([&Access, &Refresh, ExpiresInSeconds, bSameIdentity](VaRest::Http::FClientState& Target)
	{
		// A login response's expires_in: the token's whole lifetime (re-review N-10)
		Target.SetTokensByGame(Access, &Refresh, ExpiresInSeconds, ExpiresInSeconds, bSameIdentity);
	});
}

void FVaRestClient::SetSessionHeader(FStringView Name, FStringView Value)
{
	check(IsInGameThread());
	if (!VaRest::Http::IsValidHeaderName(Name))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetSessionHeader: the name is not a token (\"%s\"); not set"), *VaRest::Http::EscapeForLog(Name));
		return;
	}
	if (!VaRest::Http::IsValidHeaderValue(Value))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetSessionHeader: the value of \"%s\" holds a control character; not set"), *VaRest::Http::EscapeForLog(Name));
		return;
	}
	if (State.IsValid())
	{
		State->RunSessionChange([Name, Value](VaRest::Http::FClientState& Target) { Target.SetSessionHeader(Name, Value); });
	}
}

void FVaRestClient::RemoveSessionHeader(FStringView Name)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->RunSessionChange([Name](VaRest::Http::FClientState& Target) { Target.RemoveSessionHeader(Name); });
	}
}

void FVaRestClient::ClearSession()
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->RunSessionChange([](VaRest::Http::FClientState& Target) { Target.ClearSession(); });
	}
}

FVaRestSessionData FVaRestClient::GetSession() const
{
	check(IsInGameThread());
	return State.IsValid() ? State->GetSessionData() : FVaRestSessionData();
}

FString FVaRestClient::ExportSession() const
{
	check(IsInGameThread());
	return State.IsValid() ? State->ExportSession() : FString();
}

bool FVaRestClient::ImportSession(FStringView Json, bool bSameIdentity)
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return false;
	}
	// Concurrency review C-1: a change like any other, through the one door. By reference: RunSessionChange runs it
	// synchronously (VaRestSession.cpp RunSessionChange), nothing outlives this frame.
	bool bImported = false;
	State->RunSessionChange([Json, bSameIdentity, &bImported](VaRest::Http::FClientState& Target) { bImported = Target.ImportSession(Json, bSameIdentity); });
	return bImported;
}

void FVaRestClient::NotifySessionExpired()
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->RunSessionChange([](VaRest::Http::FClientState& Target) { Target.ExpireSession(0, TEXT("NotifySessionExpired")); });
	}
}

FVaRestSessionChangedEvent& FVaRestClient::OnSessionChanged()
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		// Leaked, like every singleton here (review I-4)
		static FVaRestSessionChangedEvent* const Never = new FVaRestSessionChangedEvent();
		return *Never;
	}
	return State->OnSessionChanged();
}

FVaRestSessionExpiredEvent& FVaRestClient::OnSessionExpired()
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		static FVaRestSessionExpiredEvent* const Never = new FVaRestSessionExpiredEvent();
		return *Never;
	}
	return State->OnSessionExpired();
}

FVaRestClient& FVaRestClient::WithAuth(TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->SetAuth(InAuth);
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithoutAuth()
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->SetAuth(nullptr);
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithBearer(FStringView InToken)
{
	WithAuth(FVaRestAuth::Bearer());
	SetBearerToken(InToken);
	return *this;
}

bool FVaRestClient::ApplyAuth(FVaRestHeaders& OutHeaders, TArray<TPair<FString, FString>>& OutQuery, FString* OutConfigError) const
{
	check(IsInGameThread());
	if (OutConfigError)
	{
		// Review M-5: a reused string never reads stale on the paths that leave it alone
		OutConfigError->Reset();
	}
	if (!State.IsValid())
	{
		return true;
	}
	// Held for the call: an auth's Apply is game code, and a WithAuth() it made would otherwise release the auth under it
	const TSharedRef<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> Current = State->GetConfig();
	const IVaRestAuth* Auth = Current->Auth.Get();
	if (Auth)
	{
		// Ruling Q7 for sockets (spec review C-1): a typo is Kind=Config there too, not a login screen
		const FString Invalid = Auth->GetConfigError();
		if (!Invalid.IsEmpty())
		{
			if (OutConfigError)
			{
				// Review M-1: a custom auth's text may hold a secret; the socket shows it to the game (W5's Kind=Config)
				*OutConfigError = Current->Redactor->ScrubText(Invalid);
			}
			return false;
		}
	}
	FVaRestAuthTarget Target(OutHeaders, OutQuery, State->GetSessionData());
	return VaRest::Http::ApplyCredentials(Auth, Target);
}

bool FVaRestClient::NeedsAuthRefresh() const
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return false;
	}
	// Held for the call: CanRefresh and NeedsRefresh are game code, and the configuration may be replaced under them
	const TSharedRef<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> Current = State->GetConfig();
	const FVaRestSessionData& Data = State->GetSessionData();
	// M4a final review B5: a misconfigured auth's session is never due (CanRefreshNow)
	return Current->Auth.IsValid() && VaRest::Http::CanRefreshNow(*Current->Auth, Data) && Current->Auth->NeedsRefresh(Data, Current->Clock->Now());
}

void FVaRestClient::RefreshAuth(TUniqueFunction<void(EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind FailedKind)>&& OnDone)
{
	check(IsInGameThread());
	// Held for the call: the auth's Refresh runs code that is not VaRest's (concurrency review C-1's rule), and it may drop
	// this client
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Pinned = State;
	const TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> Auth = Pinned.IsValid() ? Pinned->GetConfig()->Auth : nullptr;
	const auto Wait = [](TUniqueFunction<void(EVaRestAuthRefreshOutcome, EVaRestErrorKind)>&& Into) -> VaRest::Http::FRefreshWaiter
	{
		return [Done = MoveTemp(Into)](const VaRest::Http::FRefreshEnded& End) mutable
		{
			if (!Done)
			{
				return;
			}
			switch (End.End)
			{
			case VaRest::Http::ERefreshEnd::Ready:
				Done(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None);
				break;
			case VaRest::Http::ERefreshEnd::Failed:
				Done(EVaRestAuthRefreshOutcome::Failed, End.Kind);
				break;
			default:
				// Rejected, or the game cleared or expired the session meanwhile: it is gone either way
				Done(EVaRestAuthRefreshOutcome::Rejected, EVaRestErrorKind::None);
				break;
			}
		};
	};
	if (Pinned.IsValid() && Pinned->IsRefreshing())
	{
		// Task 17 review M-8 (ruled): a refresh in flight is waited for whatever the current auth is -- one replaced since by
		// an auth that cannot refresh, or none (WithAuth does not end a refresh, Task 16 review M-5): the refresh exists and
		// its end applies to the session, as the parked requests' does (the auth gate parks every request while one runs)
		Pinned->JoinRefreshInFlight(Pinned->NewWaiterId(), Wait(MoveTemp(OnDone)));
		return;
	}
	if (!Auth.IsValid() || !VaRest::Http::CanRefreshNow(*Auth, Pinned->GetSessionData()))
	{
		// No refresh to wait for and none can start, said from a later task (decision 3). M4a final review B-I-1 (ruling):
		// two answers that ruling Q7 keeps apart. A misconfigured auth is the configuration's (Failed, Config). No auth, or one
		// that cannot refresh this session (no refresh token, a Bearer), is Unavailable: the 401 that asked is the answer, as
		// the auth gate answers an HTTP 401 it cannot refresh (VaRestAuthGate.cpp:307-312) -- not Config, which would show a
		// refused token as a typo (WebSocket spec 3.3).
		if (VaRest::Http::FClientState::IsShuttingDown())
		{
			// M4a final review B7 (review B m-7): at engine exit no callback runs (decision 2), as the refresh's own waiters and
			// the auth's answer are dropped then (VaRestSession.cpp:45-48, :103-107): OnDone goes here, uncalled, on the game
			// thread, and nothing is launched into a task system that may be going
			return;
		}
		const bool bMisconfigured = Auth.IsValid() && !Auth->GetConfigError().IsEmpty();
		UE::Tasks::Launch(TEXT("VaRest.RefreshAuth"), [Answer = MoveTemp(OnDone), bMisconfigured]() mutable
		{
			if (Answer)
			{
				Answer(bMisconfigured ? EVaRestAuthRefreshOutcome::Failed : EVaRestAuthRefreshOutcome::Unavailable,
					bMisconfigured ? EVaRestErrorKind::Config : EVaRestErrorKind::None);
			}
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		return;
	}
	// Not proactive (M4a final review B6): the core cannot tell a socket's 401 from its due expiry, so a request parked behind
	// this refresh keeps spec 4.2's rule when it fails
	Pinned->JoinRefresh(Pinned->NewWaiterId(), Auth.ToSharedRef(), 0, /*bProactive*/ false, Wait(MoveTemp(OnDone)));
}

FVaRestClient& FVaRestClient::Use(FVaRestStageRef Stage, EVaRestStageSlot Slot)
{
	if (State.IsValid())
	{
		State->EditConfig([&Stage, Slot](VaRest::Http::FClientConfig& Edited) { Edited.CustomStages.Emplace(Stage, Slot); });
	}
	return *this;
}
