// Copyright 2015 Vladimir Alyamkin. All Rights Reserved.
// Original code by https://github.com/unktomi

#include "VaRestEditorPlugin.h"

#define LOCTEXT_NAMESPACE "FVaRestEditorPluginModule"

void FVaRestEditorPluginModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FVaRestEditorPluginModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaRestEditorPluginModule, VaRestEditorPlugin)
