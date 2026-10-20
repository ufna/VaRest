// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestJson.h"
#include "VaRestJsonLog.h"

DEFINE_LOG_CATEGORY(LogVaRestJson);

namespace VaRest::Json::ModulePrivate
{
	// Written once on the game thread in StartupModule, read from any thread afterwards
	UScriptStruct* GTimespanStruct = nullptr;
}

UScriptStruct* VaRest::Json::GetTimespanStruct()
{
	// Not looked up here: StaticFindObject from a worker thread is Fatal while garbage collecting
	// (UObjectGlobals.cpp:536) and races with loading, so the game thread resolves it once
	check(ModulePrivate::GTimespanStruct);
	return ModulePrivate::GTimespanStruct;
}

class FVaRestJsonModule : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override
	{
		// PreDefault loads after CoreUObject, which registers its noexport structs (NoExportTypes.h:2252-2253) in its own
		// StartupModule/static init, so /Script/CoreUObject.Timespan exists here
		VaRest::Json::ModulePrivate::GTimespanStruct = FindObjectChecked<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.Timespan"));
	}
};

IMPLEMENT_MODULE(FVaRestJsonModule, VaRestJson);
