// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class UVaRestHub;
namespace VaRest::Inspector
{
	/** Existing subsystems only; reading scope never creates a Hub or client. */
	UVaRestHub* ExistingHub(const UObject* Context);
	FString DefaultHarPath();
}
