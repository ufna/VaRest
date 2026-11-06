// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UnrealType.h"

namespace VaRest::Http
{
/** GTK admission check. The SDK getter ensures on an absent frame, which is a normal refusal here. */
inline bool HasPreparedBlueprintFrameOnGT(UObject* Target,const UBlueprintGeneratedClass* Class)
{
 check(IsInGameThread());
 if (!IsValid(Target) || !IsValid(Class) || Target->GetClass()!=Class || !UBlueprintGeneratedClass::UsePersistentUberGraphFrame()
  || (Class->HasAnyClassFlags(CLASS_NewerVersionExists) || Class->bLayoutChanging) || !Class->UberGraphFunction) return false;
 const auto* Property=Class->UberGraphFramePointerProperty;
 if (!Property || Property->GetOwnerStruct()!=Class || Class->UberGraphFunction->GetOuter()!=Class) return false;
 const auto* Frame=Property->ContainerPtrToValuePtr<FPointerToUberGraphFrame>(Target);
 if (!Frame || !Frame->RawPointer) return false;
#if VALIDATE_UBER_GRAPH_PERSISTENT_FRAME
 if (Frame->UberGraphFunctionKey!=Class->UberGraphFunctionKey) return false;
#endif
 return true;
}
}
