// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "Modules/ModuleInterface.h"

/** DeveloperTool module: desktop Development/editor only. Game modules must not depend on this module. */
class VARESTINSPECTORUI_API IVaRestInspectorUI : public IModuleInterface
{
public:
	static bool Open();
	virtual bool Show() = 0;
};
