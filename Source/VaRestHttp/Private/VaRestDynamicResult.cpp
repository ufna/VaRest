// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicResult.h"

#include "VaRestDynamicResultData.h"

VaRest::Http::FDynamicResultData::~FDynamicResultData()
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ReleaseProbe)
	{
		ReleaseProbe->bDestroyedOnGameThread.store(IsInGameThread(),std::memory_order_relaxed);
		ReleaseProbe->bDestroyed.store(true,std::memory_order_release);
	}
#endif
}

FVaRestDynamicResult::FVaRestDynamicResult(TSharedRef<const VaRest::Http::FDynamicResultData, ESPMode::ThreadSafe> InData)
	: Data(MoveTemp(InData)) {}
FVaRestDynamicTypeInfo FVaRestDynamicResult::GetType() const { return Data ? Data->Type : FVaRestDynamicTypeInfo(); }
FVaRestDynamicTypeInfo FVaRestDynamicResult::GetErrorType() const { return Data ? Data->ErrorType : FVaRestDynamicTypeInfo(); }
FVaRestJson FVaRestDynamicResult::GetJson() const { return Data ? Data->Json : FVaRestJson(); }
FVaRestJson FVaRestDynamicResult::GetErrorJson() const { return Data ? Data->ErrorJson : FVaRestJson(); }
FString FVaRestDynamicResult::GetText() const { return Data ? Data->Text : FString(); }
TConstArrayView<uint8> FVaRestDynamicResult::GetBytes() const
{
	return Data ? TConstArrayView<uint8>(Data->Bytes.GetData(),Data->Bytes.Num()) : TConstArrayView<uint8>();
}
bool FVaRestDynamicResult::HasBody() const { return Data && Data->bHasBody; }
#if WITH_DEV_AUTOMATION_TESTS
int32 FVaRestDynamicResult::GetNativeOwnerCountForTesting() const { return Data.GetSharedReferenceCount(); }
bool FVaRestDynamicResult::AttachReleaseProbeForTesting(TSharedRef<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe> Probe) const
{
	check(IsInGameThread()); if (!Data || Data->ReleaseProbe) return false;
	Data->ReleaseProbe=MoveTemp(Probe); return true;
}
#endif
