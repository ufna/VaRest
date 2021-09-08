// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestEditor.h"

#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FVaRestEditorModule"

void FVaRestEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FVaRestEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaRestEditorModule, VaRestEditor)
