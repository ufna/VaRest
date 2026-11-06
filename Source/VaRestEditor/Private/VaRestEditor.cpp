// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestEditor.h"

#include "Modules/ModuleManager.h"
#include "VaRestJsonStructListener.h"
#include "VaRestDynamicEditorHooks.h"
#include "VaRestAssetEntryActionRegistryPrivate.h"
#include "VaRestTypeRefDetails.h"
#include "VaRestEndpointNodeSpawner.h"
#include "VaRestSocketNodeSpawner.h"
#include "VaRestSioNodeSpawner.h"
#include "VaRestAssetEntryActionRegistry.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "FVaRestEditorModule"

FVaRestEditorModule::~FVaRestEditorModule() = default;

void FVaRestEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	VaRestEditor::StructListener::Start();
	VaRestEditor::DynamicHooks::Start();
	EntryRegistry=VaRestEditor::FAssetEntryRegistryOwner::CreateOnGT();
	HttpEndpointProvider=VaRestEditor::RegisterHttpEndpointProvider();
	SocketEndpointProvider=VaRestEditor::RegisterSocketEndpointProvider();
	SioEndpointProvider=VaRestEditor::RegisterSioEndpointProvider();
	FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).RegisterCustomPropertyTypeLayout(TEXT("VaRestTypeRef"),FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FVaRestTypeRefDetails::MakeInstance));
}

void FVaRestEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	FVaRestAssetEntryActionRegistry::UnregisterProvider(SioEndpointProvider); SioEndpointProvider.Reset();
	FVaRestAssetEntryActionRegistry::UnregisterProvider(SocketEndpointProvider); SocketEndpointProvider.Reset();
	FVaRestAssetEntryActionRegistry::UnregisterProvider(HttpEndpointProvider); HttpEndpointProvider.Reset(); EntryRegistry.Reset();
	if (auto* PropertyEditor=FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"))) PropertyEditor->UnregisterCustomPropertyTypeLayout(TEXT("VaRestTypeRef"));
	VaRestEditor::DynamicHooks::Stop();
	VaRestEditor::StructListener::Stop();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaRestEditorModule, VaRestEditor)
