// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "VaRestJsonConvert.h"

/**
 * INTERNAL API for VaRest's sibling modules (WebSocket, Socket.IO), not a stable API: the UTF-8 RapidJSON writer
 * VaRestJson writes with, so a socket layer can write an envelope such as {"type":"x","data":{...struct...}} in one
 * pass (WebSocket spec §19 item 9). It exposes RapidJSON types and changes with them.
 */
namespace VaRest::Json
{
	/** RapidJSON output stream over a TArray. Put/PutReserve/PutUnsafe are found by ADL from writer.h. */
	struct FUtf8Sink
	{
		typedef char Ch;

		TArray<uint8>& Out;

		void Put(char C) { Out.Add(static_cast<uint8>(C)); }
		void Flush() {}
	};

	/**
	 * No-op on purpose. RapidJSON's escape path reserves 2 + length * 6 bytes per string
	 * (writer.h:398), which over-reserves up to 6x and whose int32 cast can overflow. The reserve
	 * is only a hint, so TArray's geometric growth in Put/PutUnsafe is used instead. This overload
	 * exists so ADL picks it over rapidjson's generic template, which would call Put per character.
	 */
	inline void PutReserve(FUtf8Sink& Sink, size_t Count)
	{
		(void)Sink;
		(void)Count;
	}

	inline void PutUnsafe(FUtf8Sink& Sink, char C)
	{
		Sink.Out.Add(static_cast<uint8>(C));
	}

	/**
	 * The writers' level stack goes through FMemory via the engine's RapidJSON allocator
	 * (JsonUtils/RapidJsonUtils.h:19-53), passed as the StackAllocator argument (writer.h:90, :103;
	 * prettywriter.h:47, :58), instead of the default CrtAllocator's std::malloc (rapidjson.h:694-696).
	 */
	using FWriterAllocator = UE::Json::Private::FAllocatorImpl;
	using FCondensedWriter = rapidjson::Writer<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FWriterAllocator>;
	using FPrettyWriter = rapidjson::PrettyWriter<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FWriterAllocator>;

	/**
	 * Writes Struct's fields as members of the object Writer has open: keys and values only, no braces. Same rules and
	 * naming as FVaRestJsonConvert::StructToUtf8. OpenContainers is how many containers the caller has open around these
	 * members (the shared depth limit counts them). False past VaRest::Json::MaxDepth, or for a struct written as a
	 * single value (FVaRestJson, FDateTime, FGuid...); the writer is then mid-object and its output must be discarded.
	 */
	VARESTJSON_API bool WriteStructMembersInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 1, FVaRestJsonError* OutError = nullptr);
	VARESTJSON_API bool WriteStructMembersInternal(FPrettyWriter& Writer, const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 1, FVaRestJsonError* OutError = nullptr);

	// The entries below share WriteStructMembersInternal's contract: OpenContainers counts the containers the caller has
	// open (the shared MaxDepth includes them); false past VaRest::Json::MaxDepth, after which the writer's output must
	// be discarded; OutError receives the failure (without it, an error is logged); any thread, for the caller's data.
	// Condensed only: wire encoders are condensed (Socket.IO spec §18.2 J-2).

	/**
	 * Writes Value as one JSON value where Writer expects a value (an array element, or after a key). An invalid
	 * (missing) handle writes null, as FVaRestJson::Add does (spec 2.2). The NaN/Infinity and depth rules of ToUtf8.
	 */
	VARESTJSON_API bool WriteJsonInternal(FCondensedWriter& Writer, const FVaRestJson& Value, int32 OpenContainers = 0,
		FVaRestJsonError* OutError = nullptr);

	/**
	 * Writes Object's members (keys and values, no braces) into the object Writer has open: a payload's members next
	 * to the caller's own keys in one pass (WebSocket spec §6.1). False when Object is not an object.
	 */
	VARESTJSON_API bool WriteJsonMembersInternal(FCondensedWriter& Writer, const FVaRestJson& Object, int32 OpenContainers = 1,
		FVaRestJsonError* OutError = nullptr);

	/**
	 * Writes Struct as one JSON value: an object for an ordinary struct, the single-value form for FDateTime, FTimespan,
	 * FGuid, FVaRestJson and soft paths. Same rules and naming as StructToUtf8, including Options.BytesWriter.
	 */
	VARESTJSON_API bool WriteStructInternal(FCondensedWriter& Writer, const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 0, FVaRestJsonError* OutError = nullptr);

	/**
	 * Writes one property's value (Value points at the value, not at its container) as one JSON value: what
	 * PropertyToJson builds, without the DOM. Socket.IO's positional encode is
	 * `for (TFieldIterator<FProperty> It(Struct); It; ++It) WritePropertyInternal(Writer, *It, It->ContainerPtrToValuePtr<void>(Data), Options, 1)`.
	 */
	VARESTJSON_API bool WritePropertyInternal(FCondensedWriter& Writer, const FProperty* Property, const void* Value,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), int32 OpenContainers = 0, FVaRestJsonError* OutError = nullptr);
}
