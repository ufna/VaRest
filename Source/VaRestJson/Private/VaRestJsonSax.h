// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonNode.h"
#include "VaRestJsonWriterInternal.h"

namespace VaRest::Json
{
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
		return Sink.Null();
	case EVaRestJsonType::Bool:
		return Sink.Bool(Value.Bool);
	case EVaRestJsonType::Number:
		switch (Value.NumKind)
		{
		case ENumKind::Int:
			return Sink.Int64(Value.Int);
		case ENumKind::UInt:
			return Sink.Uint64(Value.UInt);
		case ENumKind::Double:
			// Writer::Double refuses NaN/Inf only after Prefix() has run (writer.h:195), so test first
			if (FMath::IsFinite(Value.Double))
			{
				return Sink.Double(Value.Double);
			}
			else
			{
				++State.NonFinite;
				return Sink.Null();
			}
		}
		return Sink.Null();
	case EVaRestJsonType::String:
		return Sink.String(reinterpret_cast<const char*>(Value.Str.Data), static_cast<rapidjson::SizeType>(Value.Str.Len), true);
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
		return Sink.EndArray(static_cast<rapidjson::SizeType>(Value.Arr.Num));
	case EVaRestJsonType::Object:
		if (Depth >= MaxDepth || !Sink.StartObject())
		{
			State.bTooDeep = true;
			return false;
		}
		for (uint32 i = 0; i < Value.Obj.Num; ++i)
		{
			const FMember& M = Value.Obj.Members[i];
			if (!Sink.Key(reinterpret_cast<const char*>(M.Key.Data), static_cast<rapidjson::SizeType>(M.Key.Len), true))
				return false;
			if (!EmitNode(Sink, *M.Value, Depth + 1, State))
			{
				return false;
			}
		}
		return Sink.EndObject(static_cast<rapidjson::SizeType>(Value.Obj.Num));
	}
	return Sink.Null();
}
} // namespace VaRest::Json
