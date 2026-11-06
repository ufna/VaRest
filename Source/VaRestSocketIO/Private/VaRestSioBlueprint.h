// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSioArguments.h"
#include "VaRestSioCodec.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

namespace VaRest::Sio::Blueprint
{
	struct FWildcard { const FProperty* Property = nullptr; void* Address = nullptr; };
	inline FWildcard StepWildcard(FFrame& Stack)
	{
		const FProperty* Chain = Stack.Code ? nullptr : CastField<FProperty>(Stack.PropertyChainForCompiledIn);
		checkf(!Chain || Chain->HasAnyPropertyFlags(CPF_OutParm), TEXT("Socket.IO wildcard parameters must be references"));
		uint8 Placeholder = 0; Stack.MostRecentProperty = nullptr; Stack.MostRecentPropertyAddress = nullptr;
		Stack.StepCompiledIn<FProperty>(Stack.Code ? nullptr : &Placeholder);
		return {Stack.MostRecentProperty ? Stack.MostRecentProperty : Chain, Stack.MostRecentPropertyAddress};
	}
	/** Synchronous GTK borrow, under the codec's one conversion/attachment snapshot. */
	inline bool WritePayload(VaRest::Json::FCondensedWriter& Writer, const FVaRestJsonConvertOptions& Options,
		FVaRestJsonError& Error, TFunctionRef<bool()> WithinBudget, const FProperty* Property, const void* Address, EVaRestSioArgs Mode)
	{
		if (!Property || !Address || !WithinBudget()) return false;
		if (Mode == EVaRestSioArgs::First) return VaRest::Json::WritePropertyInternal(Writer, Property, Address, Options, 1, &Error) && WithinBudget();
		const auto* Struct = CastField<FStructProperty>(Property); if (!Struct) return false;
		if (Mode == EVaRestSioArgs::Array)
		{
			if (Struct->Struct != FVaRestJson::StaticStruct()) return false;
			const auto& Array = *static_cast<const FVaRestJson*>(Address); if (!Array.IsArray()) return false;
			FArgumentWriter Into{Writer, Options, Error, WithinBudget};
			for (int32 Index = 0; Index < Array.Num(); ++Index) if (!Into.Value(Array.At(Index))) return false;
			return true;
		}
		if (Mode != EVaRestSioArgs::Positional) return false;
		TArray<const FProperty*> Fields;
		if (!VaRest::Json::TryGetConvertedFieldsInternal(Struct->Struct, Fields, Options, &Error)) return false;
		for (const auto* Field : Fields)
			if (!WithinBudget() || !VaRest::Json::WritePropertyInternal(Writer, Field, Field->ContainerPtrToValuePtr<void>(Address), Options, 1, &Error)) return false;
		return WithinBudget();
	}
}
