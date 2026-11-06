// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class IAssetTypeActions;

/** Content Browser creation/editing only. No dependency from a Runtime module to these tools. */
class VARESTEDITORTOOLS_API FVaRestEditorToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Stable extension points for the later OpenAPI tools; no importer is installed by this module. */
	static FName ApiAssetMenuName();
	static FName ApiAssetExtensionSection();

private:
	void RegisterMenus();
	TSharedPtr<IAssetTypeActions> ApiActions;
	FDelegateHandle MenuStartup;
};
