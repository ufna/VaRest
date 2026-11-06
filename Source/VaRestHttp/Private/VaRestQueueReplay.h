// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestClient.h"
#include "VaRestConnectivity.h"
#include "VaRestNativeOperationStore.h"
#include "VaRestQueueBinding.h"

namespace VaRest::Http
{
class FOfflineQueue;
struct FQueueItem;

/** One actual managed FIFO-head operation, owned until terminal, trace, native handoff and accepted IO settle. */
class FQueueReplayOperation final : public TSharedFromThis<FQueueReplayOperation, ESPMode::ThreadSafe>
{
public:
	FQueueReplayOperation(TSharedRef<FOfflineQueue, ESPMode::ThreadSafe> InQueue, uint64 InId,
		FQueueMemberKey InMember, const FQueueItem& Item, bool bInProbe, FName InTerminalDetail, FVaRestProbeToken Token);
	/** Exclusive managed Send caller, before Ready/Enter/Started. Does not read a Claimed cell or invoke code. */
	void BindManagedBeforePublication(const FNativeOperationRef& Ref, const FVaRestRequestHandle& Handle);
	void BeginTerminalAcceptanceOnGT();
	void PublishTerminalOnGT(const FFinish& Finish, const TArray<uint8>& RawBody);
	void CompletedTraceReturnedOnGT();
	/** Sole caller: the actual GT release anchor, after combined settlement and before publishing this cell Free. */
	void ManagedAnchorSettledOnGT(const FNativeOperationRef& Ref, bool bDiscarded);
	void AdmissionRefusedOnGT();
	void DetachOwnerOnGT();
	const FQueueMemberKey& MemberKey() const { return Member; }

private:
	friend class FOfflineQueue;
	void CompleteProbeOnGT();
	enum class EPhase : uint8
	{
		Request,
		CancelRetain,
		TerminalPublished,
		RetireIo,
		Settled
	};
	enum class EDisposition : uint8
	{
		Unpublished,
		RetainNetwork,
		RetainDetach,
		Retire
	};
	TSharedPtr<FOfflineQueue, ESPMode::ThreadSafe> Queue;
	FNativeOperationRef Managed;
	FVaRestRequestHandle Request;
	FQueueMemberKey Member;
	uint64 ReplayId;
	FString QueueId;
	uint64 Sequence;
	FName Tag;
	FName TerminalDetail;
	TOptional<FVaRestQueuedResult> Result;
	EPhase Phase = EPhase::Request;
	EDisposition Disposition = EDisposition::Unpublished;
	bool bProbe;
	FVaRestProbeToken ProbeToken;
	bool bProbeCompleted = false;
	bool bAcceptanceStarted = false;
	bool bCoreTerminal = false;
	bool bTraceReturned = false;
	bool bManagedSettled = false;
	bool bOwnerDetached = false;
	bool bAdmissionRefused = false;
	bool bRetireIoAdmitted = false;
};
} // namespace VaRest::Http
