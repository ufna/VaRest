// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiSnapshots.h"

TArray<FVaRestOpenApiMergeItem> FVaRestOpenApiMerge::Desired(const FVaRestOpenApiPlan& Plan,const FVaRestJson& Manifest,TConstArrayView<FVaRestOpenApiMergeItem> Live)
{
	using namespace VaRest::OpenApi;const auto Old=Base(Manifest);TStringMap<const FVaRestOpenApiMergeItem*> Previous;
	for (const auto& Item:Old) Previous.Add(Item.Id,&Item);
	TArray<FVaRestOpenApiMergeItem> Out;
	const auto Add=[&](const TCHAR* Kind,const FString& Id,const FString& Owner,const FString& Target,FVaRestJson Properties)->FVaRestOpenApiMergeItem&
	{
		FVaRestOpenApiMergeItem Item;Item.Kind=Kind;Item.Id=Id;Item.Owner=Owner;Item.Target=Target;Item.Properties=MoveTemp(Properties);
		if (const auto* Existing=Previous.Find(Id)) Item.Target=(*Existing)->Target;return Out[Out.Add(MoveTemp(Item))];
	};
	TStringMap<FString> EnumNames;
	for (const auto& Enum:Plan.Enums)
	{
		Add(TEXT("enum"),Enum.Id,{}, {},FVaRestJson::Object().Set(TEXT("tooltip"),Enum.Tooltip));
		TSet<FString> Used;for (const auto& OldValue:Old) if (Equal(OldValue.Kind,TEXT("enumValue")) && Equal(OldValue.Owner,Enum.Id)) Used.Add(OldValue.Target);
		for (const auto& LiveEnum:Live) if (Equal(LiveEnum.Id,Enum.Id)) for (const auto& Name:LiveEnum.ReservedTargets) Used.Add(Name);
		int32 Next=0;
		for (const auto& Value:Enum.Definition.Values)
		{
			const FString Id=EnumValueId(Enum.Id,Value.WireValue,Value.bUnset);FString Name;
			if (const auto* Existing=Previous.Find(Id)) Name=(*Existing)->Target;
			else { do { Name=TEXT("NewEnumerator")+LexToString(Next++); } while (Used.Contains(Name));Used.Add(Name); }
			EnumNames.Add(Enum.Id+TEXT("\n")+Value.InternalName,Name);
			Add(TEXT("enumValue"),Id,Enum.Id,Name,FVaRestJson::Object().Set(TEXT("wire"),Value.WireValue).Set(TEXT("display"),Value.DisplayName).Set(TEXT("unset"),Value.bUnset));
		}
	}
	const auto EnumDefault=[&](const FVaRestOpenApiPlannedType& Type,const FString& Text)
	{
		if (Type.Kind!=EVaRestOpenApiValueKind::Enum) return Text;
		const auto* Found=EnumNames.Find(Type.DefinitionId+TEXT("\n")+Text);return Found ? *Found : Text;
	};
	const auto Json=Plan.ToJson();
	for (int32 StructIndex=0;StructIndex<Plan.Structs.Num();++StructIndex)
	{
		const auto& Struct=Plan.Structs[StructIndex];Add(TEXT("struct"),Struct.Id,{}, {},FVaRestJson::Object().Set(TEXT("tooltip"),Struct.Tooltip));
		for (int32 FieldIndex=0;FieldIndex<Struct.Fields.Num();++FieldIndex)
		{
			const auto& Field=Struct.Fields[FieldIndex];const auto Row=Json[TEXT("structs")].At(StructIndex)[TEXT("fields")].At(FieldIndex);
			auto Properties=OwnedProperties(Row,{TEXT("wire"),TEXT("name"),TEXT("type"),TEXT("presence"),TEXT("nullable"),TEXT("base64"),TEXT("tooltip")});
			const FString Default=Field.bDefaultInTooltipOnly ? FString() : Field.Type.Kind==EVaRestOpenApiValueKind::String && Field.Default.IsString() ? Field.Default.AsString() : EnumDefault(Field.Type,Field.DefaultText);
			Properties.Set(TEXT("defaultText"),Default);Add(TEXT("field"),Field.Id,Struct.Id,{},MoveTemp(Properties));
		}
	}
	auto Envelope=OwnedProperties(Json[TEXT("envelope")],{TEXT("dataPath"),TEXT("successPath"),TEXT("errorMessagePath"),TEXT("errorCodePath"),TEXT("parseErrorBodies")});
	auto SuccessValues=FVaRestJson::Array();if (!Plan.Envelope.SuccessPath.IsEmpty()) SuccessValues.Add(TEXT("true"));Envelope.Set(TEXT("successValues"),SuccessValues);
	Add(TEXT("envelope"),TEXT("envelope"),{}, {},Envelope);
	for (int32 Index=0;Index<Plan.Endpoints.Num();++Index)
	{
		const auto& Endpoint=Plan.Endpoints[Index];const auto Row=Json[TEXT("endpoints")].At(Index);
		auto Properties=OwnedProperties(Row,{TEXT("name"),TEXT("verb"),TEXT("path"),TEXT("category"),TEXT("description"),TEXT("deprecated"),TEXT("idempotent"),TEXT("requestType"),TEXT("responseType"),TEXT("errorType"),TEXT("bodyKind"),TEXT("contentType"),TEXT("authMode"),TEXT("overrideEnvelope")});
		auto EndpointEnvelope=Endpoint.bOverrideEnvelope ? Envelope.Clone() : SnapshotEnvelope(FVaRestEnvelope());
		if (Endpoint.bOverrideEnvelope)
		{
			EndpointEnvelope.Set(TEXT("dataPath"),Endpoint.EnvelopeDataPath);
			if (Endpoint.bDisableEnvelopePredicate) EndpointEnvelope.Set(TEXT("successPath"),TEXT("")).Set(TEXT("successValues"),FVaRestJson::Array());
		}
		Properties.Set(TEXT("envelope"),EndpointEnvelope).Set(TEXT("deprecationMessage"),TEXT(""));Add(TEXT("endpoint"),Endpoint.Id,{}, {},MoveTemp(Properties));
		for (int32 ParamIndex=0;ParamIndex<Endpoint.Parameters.Num();++ParamIndex)
		{
			const auto& Param=Endpoint.Parameters[ParamIndex];const auto ParamRow=Row[TEXT("parameters")].At(ParamIndex);
			auto Params=OwnedProperties(ParamRow,{TEXT("wire"),TEXT("pin"),TEXT("in"),TEXT("type"),TEXT("required"),TEXT("style"),TEXT("description")});FString Default=Param.DefaultText;
			if (Param.Type.Kind==EVaRestOpenApiValueKind::Enum && Param.Type.Container==EVaRestOpenApiContainer::Array && !Default.IsEmpty())
			{
				auto Values=FVaRestJson::Parse(Default);if (Values.HasValue()) { auto Mapped=FVaRestJson::Array();for (const auto& Value:Values.GetValue().Elements()) Mapped.Add(EnumDefault(Param.Type,Value.AsString()));Default=Mapped.ToString(); }
			}
			else Default=EnumDefault(Param.Type,Default);
			Params.Set(TEXT("defaultText"),Default).Set(TEXT("removed"),false);Add(TEXT("param"),Param.Id,Endpoint.Id,Param.PinName,MoveTemp(Params));
		}
	}
	for (const auto& Environment:Plan.Environments) Add(TEXT("environment"),Environment.Id,{},Environment.Name,FVaRestJson::Object().Set(TEXT("name"),Environment.Name).Set(TEXT("url"),Environment.BaseUrl));
	Add(TEXT("defaults"),TEXT("defaults"),{}, {},FVaRestJson::Object().Set(TEXT("defaultEnvironment"),Plan.DefaultEnvironment));
	auto Auth=FVaRestJson::Object().Set(TEXT("kind"),Json[TEXT("auth")][TEXT("kind")]);
	if (Plan.Auth.Kind==EVaRestOpenApiAuthKind::ApiKey) Auth.Set(TEXT("name"),Plan.Auth.Name).Set(TEXT("placement"),Plan.Auth.Placement);
	if (Plan.Auth.Kind==EVaRestOpenApiAuthKind::BearerRefresh || Plan.Auth.Kind==EVaRestOpenApiAuthKind::ClientCredentials) Auth.Set(TEXT("tokenUrl"),Plan.Auth.TokenUrl).Set(TEXT("accessTokenPath"),TEXT("access_token")).Set(TEXT("expiresInPath"),TEXT("expires_in"));
	if (Plan.Auth.Kind==EVaRestOpenApiAuthKind::BearerRefresh) Auth.Set(TEXT("style"),TEXT("OAuth2Form")).Set(TEXT("refreshTokenPath"),TEXT("refresh_token"));
	Add(TEXT("auth"),TEXT("auth"),{}, {},MoveTemp(Auth));
	TStringMap<FVaRestJson> OldRows,NewRows;
	const auto IndexRows=[](const FVaRestJson& Root,TStringMap<FVaRestJson>& Rows)
	{
		for (const TCHAR* Collection:{TEXT("structs"),TEXT("enums"),TEXT("endpoints"),TEXT("environments")}) for (const auto& Row:Root[Collection].Elements())
		{
			Rows.Add(Row[TEXT("id")].AsString(),Row);
			for (const TCHAR* Children:{TEXT("fields"),TEXT("parameters")}) for (const auto& Child:Row[Children].Elements()) Rows.Add(Child[TEXT("id")].AsString(),Child);
		}
	};
	IndexRows(Manifest[TEXT("plan")],OldRows);IndexRows(Json,NewRows);
	for (auto& Item:Out)
	{
		const auto* Before=Previous.Find(Item.Id);const auto* OldRaw=OldRows.Find(Item.Id);const auto* NewRaw=NewRows.Find(Item.Id);
		if (!Before || !OldRaw || !NewRaw) continue;
		const TCHAR* Name=Equal(Item.Kind,TEXT("param")) ? TEXT("pin") : TEXT("name");
		if (Item.Properties.Has(Name) && (*OldRaw)[Name].Equals((*NewRaw)[Name])) Item.Properties.Set(Name,(*Before)->Properties[Name]);
		// A natively rejected default stays rejected until its source/type changes. Do not offer the same invalid text forever.
		bool DeferredDefault=false;for (const auto& Pending:Manifest[TEXT("deferred")].Elements()) if (Equal(Pending[TEXT("id")].AsString(),Item.Id) && (Pending[TEXT("property")].AsString().IsEmpty() || Equal(Pending[TEXT("property")].AsString(),TEXT("defaultText")))) { DeferredDefault=true;break; }
		if (!DeferredDefault && (Equal(Item.Kind,TEXT("field")) || Equal(Item.Kind,TEXT("param"))) && (*OldRaw)[TEXT("default")].Equals((*NewRaw)[TEXT("default")]) &&
			(*OldRaw)[TEXT("defaultText")].Equals((*NewRaw)[TEXT("defaultText")]) && (*OldRaw)[TEXT("type")].Equals((*NewRaw)[TEXT("type")]))
			Item.Properties.Set(TEXT("defaultText"),(*Before)->Properties[TEXT("defaultText")]);
	}
	// Failed declarations are not removal evidence. Preserve the last applied endpoint/params and absent type closure.
	FStringSet Retained,Planned;for (const auto& Item:Out) Planned.Add(Item.Id);
	TStringMap<TArray<const FVaRestOpenApiMergeItem*>> Children;for (const auto& Item:Old) if (!Item.Owner.IsEmpty()) Children.FindOrAdd(Item.Owner).Add(&Item);
	TArray<FString> Work=Plan.UnavailableEndpointIds;
	for (int32 Index=0;Index<Work.Num();++Index)
	{
		const FString Id=Work[Index];if (Retained.Contains(Id) || Planned.Contains(Id)) continue;Retained.Add(Id);
		const auto* Found=Previous.Find(Id);if (!Found) continue;const auto Item=**Found;Out.Add(Item);
		if (const auto* Owned=Children.Find(Id)) for (const auto* Child:*Owned) Work.Add(Child->Id);
		for (const TCHAR* Key:{TEXT("type"),TEXT("requestType"),TEXT("responseType"),TEXT("errorType")})
		{
			const FString Definition=Item.Properties[Key][TEXT("definition")].AsString();if (!Definition.IsEmpty() && Previous.Contains(Definition)) Work.Add(Definition);
		}
	}
	return Out;
}
