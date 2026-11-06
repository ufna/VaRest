// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlanning.h"

namespace VaRest::OpenApi
{
namespace
{
bool LessKey(const FString& A,const FString& B)
{
	const FTCHARToUTF8 Left(*A,A.Len()),Right(*B,B.Len());
	const int32 Common=FMath::Min(Left.Length(),Right.Length());const int32 Compare=FMemory::Memcmp(Left.Get(),Right.Get(),Common);
	return Compare ? Compare<0 : Left.Length()<Right.Length();
}
bool Annotation(const FString& Key) { return Equal(Key,TEXT("description")) || Equal(Key,TEXT("example")) || Equal(Key,TEXT("examples")); }
}
FVaRestJson FPlanner::Canonical(const FVaRestJson& Value) const
{
	if (Value.IsObject())
	{
		auto Out=FVaRestJson::Object();TArray<FString> Keys;FStringSet Seen;
		for (const auto& Member:Value.Members()) if (!Annotation(Member.GetKey()) && !Seen.Contains(Member.GetKey())) { Seen.Add(Member.GetKey());Keys.Add(Member.GetKey()); }
		Keys.Sort(LessKey);for (const auto& Key:Keys) Out.Set(Key,Canonical(Value.Get(Key)));return Out;
	}
	if (Value.IsArray()) { auto Out=FVaRestJson::Array();for (const auto& Item:Value.Elements()) Out.Add(Canonical(Item));return Out; }
	return Value;
}
FString FPlanner::Shape(const FVaRestOpenApiSchemaUse& Use) const
{
	FStringSet Visiting;int32 Work=0;bool bRefused=false;
	TFunction<FVaRestJson(const FVaRestOpenApiSchemaUse&,bool)> Expand;
	Expand=[&](const FVaRestOpenApiSchemaUse& Current,bool bRoot)->FVaRestJson {
		const auto* Node=Schema(Current.Id);if (!Node) { bRefused=true;return {}; }
		if (++Work>1000000 || Visiting.Num()>=VaRest::Json::MaxDepth) { bRefused=true;return {}; }
		if ((!bRoot && Named.Contains(Current.Id)) || Visiting.Contains(Current.Id))
		{
			auto Ref=FVaRestJson::Object().Set(TEXT("$ref"),Current.Id);
			for (const auto& Member:Current.Annotations.Members()) if (!Annotation(Member.GetKey())) Ref.Set(Member.GetKey(),Member.Value);
			return Ref;
		}
		if (Node->Source.IsBool()) return Node->Source;
		Visiting.Add(Current.Id);auto Out=FVaRestJson::Object();
		static const FStringSet Structural={TEXT("$ref"),TEXT("properties"),TEXT("items"),TEXT("additionalProperties"),TEXT("allOf"),TEXT("oneOf"),TEXT("anyOf")};
		for (const auto& Member:Node->Source.Members()) if (!Structural.Contains(Member.GetKey()) && !Annotation(Member.GetKey())) Out.Set(Member.GetKey(),Member.Value);
		if (!Node->Properties.IsEmpty())
		{
			auto Properties=FVaRestJson::Object();for (const auto& Property:Node->Properties) Properties.Set(Property.Name,Expand(Property.Schema,false));Out.Set(TEXT("properties"),Properties);
		}
		if (Node->Items.IsValid()) Out.Set(TEXT("items"),Expand(Node->Items,false));
		if (Node->AdditionalProperties.IsValid()) Out.Set(TEXT("additionalProperties"),Expand(Node->AdditionalProperties,false));
		else if (Node->Source.Has(TEXT("additionalProperties"))) Out.Set(TEXT("additionalProperties"),Node->Source[TEXT("additionalProperties")]);
		const auto Composition=[&](const TCHAR* Key,const TArray<FVaRestOpenApiSchemaUse>& Members) {
			if (Members.IsEmpty()) return;
			auto Rows=FVaRestJson::Array();for (const auto& Member:Members) Rows.Add(Expand(Member,false));Out.Set(Key,Rows);
		};
		Composition(TEXT("allOf"),Node->AllOf);Composition(TEXT("oneOf"),Node->OneOf);Composition(TEXT("anyOf"),Node->AnyOf);
		for (const auto& Member:Current.Annotations.Members()) if (!Annotation(Member.GetKey())) Out.Set(Member.GetKey(),Member.Value);
		Visiting.Remove(Current.Id);return Out;
	};
	const auto Expanded=Expand(Use,true);
	// Refusal cannot establish equality between distinct schemas; it never aliases them through an empty shape.
	return bRefused ? TEXT("unqualified:")+Use.Id : Canonical(Expanded).ToString();
}
}
