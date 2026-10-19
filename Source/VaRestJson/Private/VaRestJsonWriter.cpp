// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonWriter.h"

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
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
		// The writers' level stack goes through FMemory via the engine's RapidJSON allocator
		// (JsonUtils/RapidJsonUtils.h:19-53), passed as the StackAllocator argument (writer.h:90, :103;
		// prettywriter.h:47, :58), instead of the default CrtAllocator's std::malloc (rapidjson.h:694-696).
		using FAllocator = UE::Json::Private::FAllocatorImpl;
		FUtf8Sink Sink{ Out };
		FAllocator StackAllocator;
		int32 NonFinite = 0;
		if (Format == EVaRestJsonFormat::Pretty)
		{
			rapidjson::PrettyWriter<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FAllocator> Writer(Sink, &StackAllocator);
			Writer.SetIndent(' ', 2);
			WriteNode(Writer, Node, NonFinite);
		}
		else
		{
			rapidjson::Writer<FUtf8Sink, rapidjson::UTF8<>, rapidjson::UTF8<>, FAllocator> Writer(Sink, &StackAllocator);
			WriteNode(Writer, Node, NonFinite);
		}
		if (NonFinite > 0)
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), NonFinite);
		}
	}
}
