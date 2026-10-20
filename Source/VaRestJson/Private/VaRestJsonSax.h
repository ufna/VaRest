// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "VaRestJsonNode.h"

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

	struct FEmitState
	{
		/** Non-finite doubles written as null (JSON has no NaN/Infinity) */
		int32 NonFinite = 0;
		/** A container nested deeper than MaxDepth was met; the walk stopped */
		bool bTooDeep = false;
	};

	/**
	 * SAX walk of Value into Sink: a RapidJSON writer, or FDomBuilder (VaRestJsonBuilder.h, same handler interface).
	 * Depth is the number of containers around Value; a container at Depth >= MaxDepth stops the walk (false,
	 * State.bTooDeep) -- the parser's own rule, so anything that parses also writes. Strings go with copy = true: the
	 * writers ignore it, FDomBuilder copies them into its document.
	 */
	template <typename SinkType>
	bool EmitNode(SinkType& Sink, const FNode& Value, int32 Depth, FEmitState& State)
	{
		switch (Value.Type)
		{
		case EVaRestJsonType::None:
		case EVaRestJsonType::Null:
			Sink.Null();
			return true;
		case EVaRestJsonType::Bool:
			Sink.Bool(Value.Bool);
			return true;
		case EVaRestJsonType::Number:
			switch (Value.NumKind)
			{
			case ENumKind::Int:
				Sink.Int64(Value.Int);
				return true;
			case ENumKind::UInt:
				Sink.Uint64(Value.UInt);
				return true;
			case ENumKind::Double:
				// Writer::Double refuses NaN/Inf only after Prefix() has run (writer.h:195), so test first
				if (FMath::IsFinite(Value.Double))
				{
					Sink.Double(Value.Double);
				}
				else
				{
					Sink.Null();
					++State.NonFinite;
				}
				return true;
			}
			Sink.Null();
			return true;
		case EVaRestJsonType::String:
			Sink.String(reinterpret_cast<const char*>(Value.Str.Data), static_cast<rapidjson::SizeType>(Value.Str.Len), true);
			return true;
		case EVaRestJsonType::Array:
			if (Depth >= MaxDepth || !Sink.StartArray())
			{
				State.bTooDeep = true;
				return false;
			}
			for (uint32 i = 0; i < Value.Arr.Num; ++i)
			{
				if (!EmitNode(Sink, *Value.Arr.Items[i], Depth + 1, State))
				{
					return false;
				}
			}
			Sink.EndArray(static_cast<rapidjson::SizeType>(Value.Arr.Num));
			return true;
		case EVaRestJsonType::Object:
			if (Depth >= MaxDepth || !Sink.StartObject())
			{
				State.bTooDeep = true;
				return false;
			}
			for (uint32 i = 0; i < Value.Obj.Num; ++i)
			{
				const FMember& M = Value.Obj.Members[i];
				Sink.Key(reinterpret_cast<const char*>(M.Key.Data), static_cast<rapidjson::SizeType>(M.Key.Len), true);
				if (!EmitNode(Sink, *M.Value, Depth + 1, State))
				{
					return false;
				}
			}
			Sink.EndObject(static_cast<rapidjson::SizeType>(Value.Obj.Num));
			return true;
		}
		Sink.Null();
		return true;
	}
}
