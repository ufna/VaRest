// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClient.h"
#include "VaRestConnectivity.h"

namespace VaRest::Http
{
/**
 * The one way VaRestHttp's own code reaches FRequestSpec's fields (bead VaRest-uq3). Declared a friend in the public
 * header and defined only here, in a Private header, so a module outside VaRestHttp cannot use it.
 */
struct FQueueItem;
struct FExchangeState;
struct FLlmSenderTags;
class FNativeInputBorrow;
class FNativeLogicalTailLease;
class FQueueReplayOperation;
struct FRequestSpecAccess
{
	/** SendRequest's body: the spec into an exchange, the exchange into its client's pipeline (spec 1, 3.1) */
	static FVaRestRequestHandle Send(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
	static FVaRestRequestHandle SendManaged(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
	static void InitializeExchange(FRequestSpec& Spec, FExchangeState& Ex, const FLlmSenderTags& Sender,
		const char* SourceFile, uint32 SourceLine);
	static void QueueManagedRefusal(FNativeInputBorrow&& Sender, FName Reason, const FLlmSenderTags& Tags);
	static void EnterManaged(FNativeLogicalTailLease&& Handoff, FNativeLogicalTailLease&& Completion,
		FNativeLogicalTailLease&& DeliveryWorker, bool bSentOnGT);
	static void EnterManaged(FNativeLogicalTailLease&& Handoff, bool bSentOnGT);
	/** Final review M7: Kind=Config to the callbacks of a builder made on an empty FVaRestClient */
	static void DeliverWithoutClient(FRequestSpec& Spec);
	static FVaRestRequestHandle SendQueued(const TSharedRef<FClientState, ESPMode::ThreadSafe>& State,
		const FQueueItem& Item, bool bProbe, TUniqueFunction<void(const TVaRestResult<TArray<uint8>>&)>&& Done, FName TerminalDetail = NAME_None,
		TSharedPtr<FQueueReplayOperation, ESPMode::ThreadSafe> Replay = nullptr);
	static FVaRestRequestHandle SendProbe(const TSharedRef<FClientState, ESPMode::ThreadSafe>& State,
		FStringView Url, TUniqueFunction<void()>&& Done,FVaRestProbeToken Token={});
#if WITH_DEV_AUTOMATION_TESTS
	/** Real spec/capture construction for the private native ownership fixtures; no Send or transport is simulated. */
	static TUniquePtr<FRequestSpec> MakeNativeOwnershipPayload(const TSharedPtr<FClientState, ESPMode::ThreadSafe>& State,
		TUniqueFunction<void(const FString&)>&& Capture)
	{
		TUniquePtr<FRequestSpec> Spec(new FRequestSpec(State, TEXTVIEW("GET"), TEXTVIEW("/native-input")));
		Spec->OnQueued = MoveTemp(Capture);
		return Spec;
	}
	static TWeakPtr<const FClientConfig, ESPMode::ThreadSafe> NativeOwnershipBuildConfig(const FRequestSpec& Spec)
	{
		return TWeakPtr<const FClientConfig, ESPMode::ThreadSafe>(Spec.ConfigAtBuild);
	}
#endif
};
} // namespace VaRest::Http
