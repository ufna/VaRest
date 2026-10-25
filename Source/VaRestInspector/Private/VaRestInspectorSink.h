// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestInspectorQueue.h"
#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
class FService;
class FSink final:public IVaRestTraceSink
{
public:
 FSink(const FCapturePolicy&,uint64,TSharedRef<FCaptureQueue,ESPMode::ThreadSafe>,TSharedRef<FPendingAccount,ESPMode::ThreadSafe>,TWeakPtr<FService,ESPMode::ThreadSafe>);
 FVaRestTraceDetailSettings GetDetail() const override;
 void OnAttached(const FVaRestTraceSnapshot&) override;
 void OnEvent(const FVaRestTraceEvent&) override;
 void Close();
 uint64 GetGeneration() const { return Generation; }
private:
 const FCapturePolicy Policy;
 const uint64 Generation;
 const TSharedRef<FCaptureQueue,ESPMode::ThreadSafe> Queue;
 const TSharedRef<FPendingAccount,ESPMode::ThreadSafe> Pending;
 std::atomic<bool> Closed{false};
 TWeakPtr<FService,ESPMode::ThreadSafe> Attachment; // GT-only, consumed BEFORE registry publication.
};
}
#endif
