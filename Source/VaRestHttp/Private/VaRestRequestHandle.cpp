// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRequestHandle.h"
#include "VaRestHttpBlueprintTypes.h"

void UVaRestRequestHandle::Cancel() const { Native.Cancel(); }
bool UVaRestRequestHandle::IsActive() const { return Native.IsActive(); }
FString UVaRestRequestHandle::GetIdText() const { return VaRest::Http::UnsignedIdText(Native.GetId()); }
