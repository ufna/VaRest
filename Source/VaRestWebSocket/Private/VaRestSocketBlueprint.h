// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

namespace VaRest::WebSocket::Blueprint
{
	struct FWildcard { const FProperty* Property = nullptr; void* Address = nullptr; };
	/** Mirrors the JSON library's VM/native reference handling; a native step must never copy into this placeholder. */
	inline FWildcard StepWildcard(FFrame& Stack)
	{
		const FProperty* Chain = Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
		checkf(!Chain || Chain->HasAnyPropertyFlags(CPF_OutParm), TEXT("Socket wildcard parameters must be references"));
		uint8 Placeholder = 0;
		Stack.MostRecentProperty = nullptr; Stack.MostRecentPropertyAddress = nullptr;
		Stack.StepCompiledIn<FProperty>(Stack.Code ? nullptr : &Placeholder);
		return {Stack.MostRecentProperty ? Stack.MostRecentProperty : Chain, Stack.MostRecentPropertyAddress};
	}
}
