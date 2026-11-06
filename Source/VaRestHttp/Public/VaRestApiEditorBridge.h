// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVaRestApi;

/** Native runtime notification. Editor action/node refresh binds here; Http never links editor modules. */
struct VARESTHTTP_API FVaRestApiEditorBridge
{
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnEdited, UVaRestApi*);
	static FOnEdited& OnEdited();
};
