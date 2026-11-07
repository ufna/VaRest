// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlanning.h"
#include <initializer_list>

namespace VaRest::OpenApi
{
namespace
{
bool EnvelopeKeyLess(const FString& A,const FString& B)
{
	const FTCHARToUTF8 Left(*A,A.Len()),Right(*B,B.Len());const int32 Compare=FMemory::Memcmp(Left.Get(),Right.Get(),FMath::Min(Left.Length(),Right.Length()));
	return Compare ? Compare<0 : Left.Length()<Right.Length();
}
}
void FPlanner::DetectEnvelope()
{
	struct FView { TArray<FFlatProperty> Fields;FStringSet Required;FVaRestOpenApiSchemaUse Use;int32 Endpoint=INDEX_NONE; };
	TArray<FView> SuccessViews;
	TArray<int32> ViewIndices;ViewIndices.Init(INDEX_NONE,Plan.Endpoints.Num());
	for (int32 Index=0;Index<SuccessMedia.Num();++Index)
	{
		const auto* Media=SuccessMedia[Index];if (!Media || !Media->Schema.IsValid() || Plan.Endpoints[Index].bMixedSuccessShapes) continue;
		FString Base=Media->Name.ToLower();int32 Semicolon;if (Base.FindChar(TEXT(';'),Semicolon)) Base=Base.Left(Semicolon);
		if (!Equal(Base,TEXT("application/json")) && !Base.EndsWith(TEXT("+json"),ESearchCase::CaseSensitive)) continue;
		FView View;View.Use=Media->Schema;View.Endpoint=Index;FStringSet Visiting;Flatten(View.Use,View.Fields,View.Required,Visiting);
		if (!View.Fields.IsEmpty()) ViewIndices[Index]=SuccessViews.Add(MoveTemp(View));
	}
	Plan.Envelope.ConsideredOperations=SuccessViews.Num();Plan.EnvelopeCandidate.ConsideredOperations=SuccessViews.Num();
	const FView* Representative=nullptr;FString ChosenKey;TArray<FString> ChosenMeta;int32 BestCount=0;
	for (const TCHAR* Key:{TEXT("data"),TEXT("result"),TEXT("payload")})
	{
		TArray<const FView*> Wrapped;TArray<FString> Signatures;TStringMap<int32> Counts;
		for (const auto& View:SuccessViews)
		{
			if (!View.Fields.ContainsByPredicate([&](const auto& Field) { return Equal(Field.Name,Key); })) continue;
			TArray<FString> FieldNames;for (const auto& Field:View.Fields) if (!Equal(Field.Name,Key)) FieldNames.Add(Field.Name);FieldNames.Sort(EnvelopeKeyLess);
			auto Json=FVaRestJson::Array();for (const auto& Name:FieldNames) Json.Add(Name);const FString Signature=Json.ToString();
			Wrapped.Add(&View);Signatures.Add(Signature);++Counts.FindOrAdd(Signature);
		}
		if (Wrapped.Num()<3 || Wrapped.Num()*4<SuccessViews.Num()*3) continue;
		int32 Most=0,Winner=INDEX_NONE;
		for (int32 Index=0;Index<Signatures.Num();++Index) if (Counts.FindChecked(Signatures[Index])>Most) { Most=Counts.FindChecked(Signatures[Index]);Winner=Index; }
		if (Winner==INDEX_NONE || Most*4<Wrapped.Num()*3) continue;
		TArray<FString> MetaNames;for (const auto& Field:Wrapped[Winner]->Fields) if (!Equal(Field.Name,Key)) MetaNames.Add(Field.Name);
		if (MetaNames.Num()>6 || Wrapped.Num()<=BestCount) continue;
		BestCount=Wrapped.Num();ChosenKey=Key;ChosenMeta=MoveTemp(MetaNames);Representative=Wrapped[Winner];
	}
	if (BestCount>0) { Plan.EnvelopeCandidate.DataPath=ChosenKey;Plan.EnvelopeCandidate.WrappedOperations=BestCount; }
	if (BestCount>0 && Options.bDetectEnvelope)
	{
		Plan.Envelope.DataPath=ChosenKey;Plan.Envelope.WrappedOperations=BestCount;
		for (const TCHAR* Predicate:{TEXT("success"),TEXT("ok")})
		{
			if (!ChosenMeta.ContainsByPredicate([&](const FString& Name) { return Equal(Name,Predicate); })) continue;
			int32 Matched=0;
			for (const auto& View:SuccessViews)
			{
				if (!View.Fields.ContainsByPredicate([&](const auto& Field) { return Equal(Field.Name,ChosenKey); })) continue;
				for (const auto& Field:View.Fields) if (Equal(Field.Name,Predicate) && View.Required.Contains(Field.Name))
				{
					const auto* Node=Schema(Field.Use.Id);
					if (Node && (Node->Types.ContainsByPredicate([](const FString& Type) { return Equal(Type,TEXT("boolean")); }) || Node->Const.IsBool())) ++Matched;
					break;
				}
			}
			if (Matched*4>=BestCount*3) { Plan.Envelope.SuccessPath=Predicate;break; }
		}
		for (int32 Index=0;Index<Plan.Endpoints.Num();++Index)
		{
			const FView* View=ViewIndices[Index]!=INDEX_NONE ? &SuccessViews[ViewIndices[Index]] : nullptr;
			const FFlatProperty* Data=View ? View->Fields.FindByPredicate([&](const auto& Field) { return Equal(Field.Name,ChosenKey); }) : nullptr;
			if (!Data) { Plan.Endpoints[Index].bOverrideEnvelope=true;Plan.Endpoints[Index].bDisableEnvelopePredicate=true;Plan.Endpoints[Index].EnvelopeDataPath.Reset();continue; }
			auto Value=Type(Data->Use,SemanticId(Plan.Endpoints[Index].Id,TEXT("response")),FVaRestOpenApiNameAllocator::Pascal(Plan.Endpoints[Index].Name)+TEXT("Response"));
			Plan.Endpoints[Index].ResponseType=Value.Type;
			if (Value.Type.Container==EVaRestOpenApiContainer::None && (Value.Type.Kind==EVaRestOpenApiValueKind::String || Value.Type.Kind==EVaRestOpenApiValueKind::Bytes)) Plan.Endpoints[Index].ResponseType.bJsonValue=true;
			if (!Plan.Envelope.SuccessPath.IsEmpty() && !View->Required.Contains(Plan.Envelope.SuccessPath))
			{ Plan.Endpoints[Index].bOverrideEnvelope=true;Plan.Endpoints[Index].EnvelopeDataPath=ChosenKey;Plan.Endpoints[Index].bDisableEnvelopePredicate=true; }
		}
	}
	TArray<FView> Errors;FStringSet SeenShapes;
	for (const auto& Use:ErrorSchemas)
	{
		const FString Current=Shape(Use);if (SeenShapes.Contains(Current)) continue;SeenShapes.Add(Current);
		FView View;View.Use=Use;FStringSet Visiting;Flatten(Use,View.Fields,View.Required,Visiting);Errors.Add(MoveTemp(View));
	}
	if (Representative)
	{
		FView Meta=*Representative;
		Meta.Fields.RemoveAll([&](const auto& Field) { return !ChosenMeta.ContainsByPredicate([&](const FString& Name) { return Equal(Name,Field.Name); }); });
		Errors.Add(MoveTemp(Meta));
	}
	const auto Present=[&](const FView& View,const FString& Path,bool bString)->bool {
		TArray<FString> Parts;Path.ParseIntoArray(Parts,TEXT("."),false);const FView* Current=&View;FView Nested;
		for (int32 Index=0;Index<Parts.Num();++Index)
		{
			const auto* Field=Current->Fields.FindByPredicate([&](const auto& Item) { return Equal(Item.Name,Parts[Index]); });if (!Field) return false;
			const auto Use=Field->Use;const auto* Node=Schema(Use.Id);if (!Node) return false;
			if (Index==Parts.Num()-1)
			{
				if (!bString) return true;
				if (Node->Types.ContainsByPredicate([](const FString& Kind) { return Equal(Kind,TEXT("string")); }) || Node->Const.IsString()) return true;
				if (Node->Enum.IsArray() && Node->Enum.Num()>0) { for (const auto& Value:Node->Enum.Elements()) if (!Value.IsString()) return false;return true; }
				return false;
			}
			Nested={};Nested.Use=Use;FStringSet Visiting;Flatten(Use,Nested.Fields,Nested.Required,Visiting);Current=&Nested;
		}
		return false;
	};
	const auto Pick=[&](std::initializer_list<const TCHAR*> Paths,bool bString)->FString {
		if (Errors.IsEmpty()) return {};
		for (const TCHAR* Path:Paths) { int32 Count=0;for (const auto& View:Errors) if (Present(View,Path,bString)) ++Count;if (Count*4>=Errors.Num()*3) return Path; }
		return {};
	};
	Plan.Envelope.ErrorMessagePath=Pick({TEXT("error.message"),TEXT("message"),TEXT("error"),TEXT("detail"),TEXT("error_description")},true);
	Plan.Envelope.ErrorCodePath=Pick({TEXT("error.code"),TEXT("code"),TEXT("errorCode"),TEXT("error_code")},false);
	Plan.Envelope.bParseErrorBodies=!Plan.Envelope.ErrorMessagePath.IsEmpty();
}
}
