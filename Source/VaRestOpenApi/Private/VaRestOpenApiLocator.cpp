// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiAnalysis.h"
#include "RapidJsonIncludes.h"

namespace VaRest::OpenApi
{
namespace
{
struct FSpan { int32 Begin=0,End=0; };
/** One SAX pass, a depth stack and only the requested pointer positions; no per-node position table. */
struct FLocator final : rapidjson::BaseReaderHandler<rapidjson::UTF8<>,FLocator>
{
	struct FContainer { FString Pointer,Key;int32 Index=0;bool bObject=false; };
	TConstArrayView<uint8> Bytes;
	rapidjson::MemoryStream Stream;
	const FStringSet& Requested;
	TArray<FContainer> Stack;
	TStringMap<FSpan> Spans;
	int32 Bom=0,Cursor=0;
	FLocator(TConstArrayView<uint8> InBytes,const FStringSet& InRequested)
		: Bytes(InBytes),Stream(reinterpret_cast<const char*>(InBytes.GetData()),InBytes.Num()),Requested(InRequested)
	{
		if (Bytes.Num()>=3 && Bytes[0]==0xef && Bytes[1]==0xbb && Bytes[2]==0xbf)
		{ Bom=3;Stream.Take();Stream.Take();Stream.Take(); }
		Cursor=Bom;
	}
	FString Pointer() const
	{
		if (Stack.IsEmpty()) return {};
		const auto& Parent=Stack.Last();return Child(Parent.Pointer,Parent.bObject ? Parent.Key : LexToString(Parent.Index));
	}
	int32 Start() const
	{
		int32 Position=Cursor;
		while (Position<Bytes.Num())
		{
			const uint8 Byte=Bytes[Position];if (Byte!=' ' && Byte!='\t' && Byte!='\r' && Byte!='\n' && Byte!=',' && Byte!=':') break;
			++Position;
		}
		return Position;
	}
	void Consumed()
	{
		Cursor=static_cast<int32>(Stream.Tell());
		if (!Stack.IsEmpty()) { if (Stack.Last().bObject) Stack.Last().Key.Reset();else ++Stack.Last().Index; }
	}
	bool Scalar()
	{
		const FString Here=Pointer();if (Requested.Contains(Here)) Spans.Add(Here,{Start(),static_cast<int32>(Stream.Tell())});Consumed();return true;
	}
	bool Null() { return Scalar(); }
	bool Bool(bool) { return Scalar(); }
	bool RawNumber(const char*,rapidjson::SizeType,bool) { return Scalar(); }
	bool String(const char*,rapidjson::SizeType,bool) { return Scalar(); }
	bool Key(const char* Text,rapidjson::SizeType Length,bool)
	{
		if (Stack.IsEmpty()) return false;
		const FUTF8ToTCHAR Converted(Text,Length);Stack.Last().Key=FString(Converted.Length(),Converted.Get());Cursor=static_cast<int32>(Stream.Tell());return true;
	}
	bool Begin(bool bObject)
	{
		if (Stack.Num()>=VaRest::Json::MaxDepth) return false;
		const FString Here=Pointer();if (Requested.Contains(Here)) Spans.Add(Here,{Start(),0});
		Stack.Add({Here,{},0,bObject});Cursor=static_cast<int32>(Stream.Tell());return true;
	}
	bool StartObject() { return Begin(true); }
	bool StartArray() { return Begin(false); }
	bool End()
	{
		if (Stack.IsEmpty()) return false;
		if (auto* Span=Spans.Find(Stack.Last().Pointer)) Span->End=static_cast<int32>(Stream.Tell());
		Stack.Pop(EAllowShrinking::No);Consumed();return true;
	}
	bool EndObject(rapidjson::SizeType) { return End(); }
	bool EndArray(rapidjson::SizeType) { return End(); }
	bool Run()
	{
		rapidjson::Reader Reader;
		return bool(Reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag>(Stream,*this));
	}
};
}
FString NumberSource(const FDocument& Doc,const FString& Pointer)
{
	const FStringSet Requested={Pointer};FLocator Locator(Doc.Bytes,Requested);
	if (!Locator.Run()) return {};
	const auto* Span=Locator.Spans.Find(Pointer);if (!Span || Span->End<=Span->Begin) return {};
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Doc.Bytes.GetData()+Span->Begin),Span->End-Span->Begin);
	return FString(Converted.Length(),Converted.Get());
}
namespace
{
void LocateDocument(TConstArrayView<uint8> Bytes,const FString& Uri,TArrayView<FVaRestOpenApiIssue> Issues)
{
	TArray<int32> Indices;FStringSet Requested;
	for (int32 Index=0;Index<Issues.Num();++Index) if (!Issues[Index].Line && Equal(Issues[Index].Document,Uri))
	{
		Indices.Add(Index);FString Pointer=Issues[Index].Pointer;
		for (;;) { Requested.Add(Pointer);int32 Slash;if (!Pointer.FindLastChar(TEXT('/'),Slash)) break;Pointer.LeftInline(Slash); }
	}
	if (Indices.IsEmpty()) return;FLocator Locator(Bytes,Requested);if (!Locator.Run()) return;
	TArray<TPair<int32,int32>> Offsets;
	for (int32 Index:Indices)
	{
		FString Pointer=Issues[Index].Pointer;
		for (;;)
		{
			if (const auto* Span=Locator.Spans.Find(Pointer)) { Offsets.Emplace(Span->Begin,Index);break; }
			int32 Slash;if (!Pointer.FindLastChar(TEXT('/'),Slash)) break;Pointer.LeftInline(Slash);
		}
	}
	Offsets.Sort([](const auto& A,const auto& B) { return A.Key<B.Key; });
	int32 Cursor=Locator.Bom,Line=1,Column=1;
	for (const auto& Offset:Offsets)
	{
		while (Cursor<Offset.Key)
		{
			const uint8 Byte=Bytes[Cursor++];
			if (Byte=='\r') { if (Cursor<Bytes.Num() && Bytes[Cursor]=='\n') ++Cursor;++Line;Column=1; }
			else if (Byte=='\n') { ++Line;Column=1; }
			else if ((Byte&0xc0)!=0x80) ++Column;
		}
		Issues[Offset.Value].Line=Line;Issues[Offset.Value].Column=Column;
	}
}
}
void FAnalysis::LocateIssues()
{
	for (const auto& Uri:Result.Model.SourceDocuments) if (const auto* Doc=Documents.Find(Uri)) LocateDocument((*Doc)->Bytes,Uri,Result.Issues);
}
}
void FVaRestOpenApiLoader::LocateIssues(const FVaRestOpenApiAnalysis& Analysis,TArrayView<FVaRestOpenApiIssue> Issues)
{
	for (const auto& Source:Analysis.SourceText) VaRest::OpenApi::LocateDocument(Source.Bytes,Source.Document,Issues);
}
