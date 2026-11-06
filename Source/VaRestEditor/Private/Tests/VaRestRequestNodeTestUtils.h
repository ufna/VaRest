// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreMinimal.h"

class UBlueprint;
class UObject;
class UScriptStruct;
struct FEdGraphPinType;
class FCompilerResultsLog;

namespace VaRest::Http::Editor::TransferTest
{
struct FGraph
{
	UBlueprint* Blueprint=nullptr;
	FName Start,Prepare,Continue,Unavailable;
	bool bPreparationIsClosed=false;
};
/** Same expansion helper as production. The only user continuation copies a value/cookie and increments a count. */
FGraph MakeGraph(const FEdGraphPinType* ResponseType=nullptr,bool bMultipleMetadata=false,bool bRetypeContinuation=false,bool bUserRecompile=false,bool bObserveError=false,bool bReentrant=false);
bool Compile(UBlueprint* Blueprint,FCompilerResultsLog& Log);
int32 ReadInt(UObject* Instance,FName Name);
bool ReadBool(UObject* Instance,FName Name);
FString ReadString(UObject* Instance,FName Name);
int32 ReadResponseValue(UObject* Instance);
bool SetAction(UObject* Instance,UObject* Action);
bool SetTransferProbe(UObject* Instance,UObject* Probe);
struct FStorageObservation { bool bFrame=false,bCookie=false,bValue=false; };
/** All property/frame pointers stay local to this inspection. Only values leave the call. */
FStorageObservation InspectSharedStorage(UObject* Instance,FStringView Cookie,int32 Value,const UScriptStruct* Struct=nullptr,bool bError=false);
bool SupersedeSharedCookie(UObject* Instance,FStringView Cookie);
}
#endif
