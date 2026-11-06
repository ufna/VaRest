// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class FKismetCompilerContext;
class UEdGraph;
class UK2Node;
class UK2Node_CallFunction;
class UK2Node_CustomEvent;
class UK2Node_TemporaryVariable;

namespace VaRest::Http::Editor
{
/** The qualification fixture and later full K2 nodes use this exact construction. No user exec leaves Prepare. */
struct FRequestTransferExpansion
{
	UK2Node_CallFunction* Bind=nullptr;
	UEdGraphPin* BindingThen=nullptr;
	UK2Node_CustomEvent* Prepare=nullptr;
	UK2Node_CustomEvent* Continue=nullptr;
	UK2Node_CustomEvent* Unavailable=nullptr;
	UK2Node_CustomEvent* Metadata=nullptr;
	UK2Node_CustomEvent* Queued=nullptr;
	UK2Node_TemporaryVariable* Response=nullptr;
	UK2Node_TemporaryVariable* ErrorBody=nullptr;
	UK2Node_TemporaryVariable* ResponseValid=nullptr;
	UK2Node_TemporaryVariable* ErrorValid=nullptr;
	UK2Node_TemporaryVariable* Cookie=nullptr;
	/** Only the true branch AFTER AcceptPreparedOutput. False returns to the native availability decision. */
	UEdGraphPin* AcceptedExec=nullptr;
	bool bValid=false;
};

/** GTK compiler contract for the same closed transfer construction across protocol action classes. */
struct FPreparedTransferContract
{
	UClass* ActionClass = nullptr;
	FName BindOutput, TakeResponse, TakeErrorBody, ConfirmPrepared, AcceptPreparedOutput;
	/** Optional protocol metadata bindings; HTTP supplies both and retains its existing node order/identities. */
	FName BindMetadata, BindQueued;
};

FRequestTransferExpansion ExpandPreparedTransfer(UEdGraph* Graph,UK2Node* Source,const FGuid& NodeId,UEdGraphPin* Action,
	const FEdGraphPinType* ResponseType,const FEdGraphPinType* ErrorType,const FPreparedTransferContract& Contract,FKismetCompilerContext* Compiler=nullptr);

/** Compiler may be null only for a code-authored qualification graph compiled normally afterward. */
FRequestTransferExpansion ExpandRequestTransfer(UEdGraph* Graph,UK2Node* Source,const FGuid& NodeId,UEdGraphPin* Action,
	const FEdGraphPinType* ResponseType,const FEdGraphPinType* ErrorType,FKismetCompilerContext* Compiler=nullptr);
}
