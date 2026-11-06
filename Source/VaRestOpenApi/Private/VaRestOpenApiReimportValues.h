// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiSnapshots.h"

namespace VaRest::OpenApi
{
TOptional<FVaRestOpenApiPlannedType> PlannedType(const FVaRestJson& Value);
template <typename E> E NativeEnum(const FVaRestJson& Value,E Fallback)
{
	const UEnum* Enum=StaticEnum<E>();const FString Name=Value.AsString();const int32 Index=Enum->GetIndexByNameString(Name);
	return Index!=INDEX_NONE && Equal(Enum->GetNameStringByIndex(Index),Name) ? static_cast<E>(Enum->GetValueByIndex(Index)) : Fallback;
}
void ReadEnvelope(const FVaRestJson& Json,FVaRestEnvelope& Out);
void ReadAuth(const FVaRestJson& Json,UVaRestApi& Api);
}
