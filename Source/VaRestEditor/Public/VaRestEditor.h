// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleInterface.h"
#include "Templates/UniquePtr.h"
#include "Delegates/Delegate.h"

namespace VaRestEditor { class FAssetEntryRegistryOwner; }

class FVaRestEditorModule : public IModuleInterface
{

public:
	virtual ~FVaRestEditorModule() override;
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
private:
	TUniquePtr<VaRestEditor::FAssetEntryRegistryOwner> EntryRegistry;
	FDelegateHandle HttpEndpointProvider;
	FDelegateHandle SocketEndpointProvider;
	FDelegateHandle SioEndpointProvider;
};
