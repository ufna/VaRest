// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorSink.h"
#include "VaRestInspectorService.h"
#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
FSink::FSink(const FCapturePolicy& P,uint64 G,TSharedRef<FCaptureQueue,ESPMode::ThreadSafe> Q,TSharedRef<FPendingAccount,ESPMode::ThreadSafe> A,TWeakPtr<FService,ESPMode::ThreadSafe> S)
 : Policy(P),Generation(G),Queue(MoveTemp(Q)),Pending(MoveTemp(A)),Attachment(MoveTemp(S)) {}
FVaRestTraceDetailSettings FSink::GetDetail() const { check(IsInGameThread()); return Policy.Detail(); }
void FSink::OnAttached(const FVaRestTraceSnapshot& Snapshot)
{
 check(IsInGameThread());
 TWeakPtr<FService,ESPMode::ThreadSafe> Once=MoveTemp(Attachment);
 Attachment.Reset();
 if (!Closed.load(std::memory_order_acquire)) if (auto Service=Once.Pin()) Service->ApplyAttached(Snapshot);
}
void FSink::OnEvent(const FVaRestTraceEvent& Event)
{
 // Worker-safe immutable queue path; no Store, Service, Module or application callback.
 if (Closed.load(std::memory_order_acquire)) return;
 Queue->TryPush(Event);
}
void FSink::Close() { Closed.store(true,std::memory_order_release); Queue->Close(); }
}
#endif
