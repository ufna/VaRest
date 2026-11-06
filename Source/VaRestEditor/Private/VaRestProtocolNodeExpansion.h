// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestApiTypes.h"

class FKismetCompilerContext;
class UK2Node;
class UK2Node_TemporaryVariable;
class UEdGraph;
class UEdGraphPin;

namespace VaRestEditor
{
/** Native metadata stays separate from the closed reflected transfer. */
struct FProtocolTransferConfig
{
	FName TypedOutput, SuccessExec, MetadataOutput, MetadataGetter;
	UClass* MetadataClass = nullptr;
	bool bSplitMetadata = false;
};
bool ExpandProtocolOutputBinding(UK2Node* Source, UEdGraph* Graph, FKismetCompilerContext& Compiler,
	UEdGraphPin* Action, UEdGraphPin*& BindingThen, const FVaRestTypeRef& Type,
	const TMap<FName, UK2Node_TemporaryVariable*>& Outputs, const FProtocolTransferConfig& Config);
}
