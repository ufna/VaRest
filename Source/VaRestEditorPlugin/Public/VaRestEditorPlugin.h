// Copyright 2015 Vladimir Alyamkin. All Rights Reserved.
// Original code by https://github.com/unktomi

#pragma once

#include "Modules/ModuleManager.h"

class FVaRestEditorPluginModule : public IModuleInterface
{

public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
