// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleInterface.h"

class FVaRestEditorModule : public IModuleInterface
{

public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
