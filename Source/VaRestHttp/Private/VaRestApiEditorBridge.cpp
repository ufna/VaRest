// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestApiEditorBridge.h"

FVaRestApiEditorBridge::FOnEdited& FVaRestApiEditorBridge::OnEdited()
{
	static FOnEdited Edited;
	return Edited;
}
