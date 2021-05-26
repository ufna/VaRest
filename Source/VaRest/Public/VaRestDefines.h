// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Runtime/Launch/Resources/Version.h"

#include "CoreMinimal.h"

#include "Logging/LogCategory.h"
#include "Logging/LogMacros.h"
#include "Logging/LogVerbosity.h"

// You should place include statements to your module's private header files here.  You only need to
// add includes for headers that are used in most of your module's source files though.

DECLARE_LOG_CATEGORY_EXTERN(LogVaRest, Log, All);

#define VA_FUNC (FString(__FUNCTION__))              // Current Class Name + Function Name where this is called
#define VA_LINE (FString::FromInt(__LINE__))         // Current Line Number in the code where this is called
#define VA_FUNC_LINE (VA_FUNC + "(" + VA_LINE + ")") // Current Class and Line Number where this is called!
