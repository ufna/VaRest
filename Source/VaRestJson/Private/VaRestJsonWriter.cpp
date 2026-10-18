// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonWriter.h"

#include "RapidJsonIncludes.h"
#include "VaRestJsonLog.h"

namespace VaRest::Json
{
	namespace
	{
		/** RapidJSON output stream over a TArray. Put/PutReserve/PutUnsafe are found by ADL from writer.h. */
		struct FUtf8Sink
		{
			typedef char Ch;

			TArray<uint8>& Out;

			void Put(char C) { Out.Add(static_cast<uint8>(C)); }
			void Flush() {}
		};

		inline void PutReserve(FUtf8Sink& Sink, size_t Count)
		{
			Sink.Out.Reserve(Sink.Out.Num() + static_cast<int32>(Count));
		}

		inline void PutUnsafe(FUtf8Sink& Sink, char C)
		{
			Sink.Out.Add(static_cast<uint8>(C));
		}

		template <typename WriterType>
		void WriteNode(WriterType& Writer, const FNode& Node, int32& NonFinite)
		{
			switch (Node.Type)
			{
			case EVaRestJsonType::None:
			case EVaRestJsonType::Null:
				Writer.Null();
				break;
			case EVaRestJsonType::Bool:
				Writer.Bool(Node.Bool);
				break;
			case EVaRestJsonType::Number:
				switch (Node.NumKind)
				{
				case ENumKind::Int:
					Writer.Int64(Node.Int);
					break;
				case ENumKind::UInt:
					Writer.Uint64(Node.UInt);
					break;
				case ENumKind::Double:
					if (FMath::IsFinite(Node.Double))
					{
						Writer.Double(Node.Double);
					}
					else
					{
						Writer.Null();
						++NonFinite;
					}
					break;
				}
				break;
			case EVaRestJsonType::String:
				Writer.String(reinterpret_cast<const char*>(Node.Str.Data), static_cast<rapidjson::SizeType>(Node.Str.Len));
				break;
			case EVaRestJsonType::Array:
				Writer.StartArray();
				for (uint32 i = 0; i < Node.Arr.Num; ++i)
				{
					WriteNode(Writer, *Node.Arr.Items[i], NonFinite);
				}
				Writer.EndArray(static_cast<rapidjson::SizeType>(Node.Arr.Num));
				break;
			case EVaRestJsonType::Object:
				Writer.StartObject();
				for (uint32 i = 0; i < Node.Obj.Num; ++i)
				{
					const FMember& M = Node.Obj.Members[i];
					Writer.Key(reinterpret_cast<const char*>(M.Key.Data), static_cast<rapidjson::SizeType>(M.Key.Len));
					WriteNode(Writer, *M.Value, NonFinite);
				}
				Writer.EndObject(static_cast<rapidjson::SizeType>(Node.Obj.Num));
				break;
			}
		}
	}

	void Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out)
	{
		FUtf8Sink Sink{ Out };
		int32 NonFinite = 0;
		if (Format == EVaRestJsonFormat::Pretty)
		{
			rapidjson::PrettyWriter<FUtf8Sink> Writer(Sink);
			Writer.SetIndent(' ', 2);
			WriteNode(Writer, Node, NonFinite);
		}
		else
		{
			rapidjson::Writer<FUtf8Sink> Writer(Sink);
			WriteNode(Writer, Node, NonFinite);
		}
		if (NonFinite > 0)
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), NonFinite);
		}
	}
}
