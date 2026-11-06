// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlanning.h"
#include "Misc/ScopeExit.h"

namespace VaRest::OpenApi
{
FString SemanticId(const FString& Owner,const TCHAR* Role,const FString& Name)
{
	const FTCHARToUTF8 OwnerUtf8(*Owner),NameUtf8(*Name);
	return TEXT("inline:")+LexToString(OwnerUtf8.Length())+TEXT(":")+Owner+TEXT(":")+Role+TEXT(":")+LexToString(NameUtf8.Length())+TEXT(":")+Name;
}
namespace
{
bool HasType(const FVaRestOpenApiSchema& Node,const TCHAR* Name) { return Node.Types.ContainsByPredicate([&](const FString& Type) { return Equal(Type,Name); }); }
bool NullOnly(const FVaRestOpenApiSchema& Node)
{
	return Node.Const.IsNull() || (Node.bNullable && Node.Types.IsEmpty() && Node.Source.Has(TEXT("type")));
}
bool SameType(const FVaRestOpenApiPlannedType& A,const FVaRestOpenApiPlannedType& B)
{
	return A.Kind==B.Kind && A.Container==B.Container && A.bJsonValue==B.bJsonValue && Equal(A.DefinitionId,B.DefinitionId);
}
bool StringLike(const FVaRestOpenApiSchema& Node)
{
	if (HasType(Node,TEXT("string")) || Node.Const.IsString()) return true;
	if (!Node.Enum.IsArray() || Node.Enum.Num()==0) return false;
	for (const auto& Value:Node.Enum.Elements()) if (!Value.IsString()) return false;
	return true;
}
FString Tooltip(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);if (Utf8.Length()<=1024) return Text;
	int32 Count=1021;while (Count>0 && (static_cast<uint8>(Utf8.Get()[Count])&0xc0)==0x80) --Count;
	const FUTF8ToTCHAR Prefix(Utf8.Get(),Count);return FString(Prefix.Length(),Prefix.Get())+TEXT("...");
}
FString Preview(const FVaRestJson& Value,int32 Depth=0)
{
	if (!Value.IsValid()) return {};
	if (Value.IsString())
	{
		const auto Utf8=Value.AsUtf8View();int32 Count=FMath::Min(Utf8.Len(),160);
		while (Count>0 && Count<Utf8.Len() && (static_cast<uint8>(Utf8[Count])&0xc0)==0x80) --Count;
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()),Count);FString Out=TEXT("\"");
		for (int32 Index=0;Index<Text.Length();++Index)
		{
			const TCHAR Char=Text.Get()[Index];if (Char<32) Out+=FString::Printf(TEXT("\\u%04X"),static_cast<uint32>(Char));
			else { if (Char==TEXT('"') || Char==TEXT('\\')) Out.AppendChar(TEXT('\\'));Out.AppendChar(Char); }
		}
		return Out+(Count<Utf8.Len() ? TEXT("...\"") : TEXT("\""));
	}
	if (!Value.IsObject() && !Value.IsArray()) return Value.ToString();
	if (Depth>=2) return Value.IsObject() ? TEXT("{...}") : TEXT("[...]");
	FString Out=Value.IsObject() ? TEXT("{") : TEXT("[");int32 Count=0;
	if (Value.IsObject()) for (const auto& Member:Value.Members()) { if (Count++>=3) break;if (Count>1) Out+=TEXT(", ");Out+=Member.GetKey().Left(64)+TEXT(": ")+Preview(Member.Value,Depth+1); }
	else for (const auto& Element:Value.Elements()) { if (Count++>=3) break;if (Count>1) Out+=TEXT(", ");Out+=Preview(Element,Depth+1); }
	return Out+(Value.Num()>3 ? TEXT(", ...") : TEXT(""))+(Value.IsObject() ? TEXT("}") : TEXT("]"));
}
}
const FVaRestOpenApiSchema* FPlanner::Schema(const FString& Id) const { const auto* Found=Schemas.Find(Id);return Found ? *Found : nullptr; }
FVaRestJson FPlanner::Meta(const FVaRestOpenApiSchemaUse& Use,const TCHAR* Key) const
{
	if (Use.Annotations.Has(Key)) return Use.Annotations[Key];
	const auto* Node=Schema(Use.Id);return Node ? Node->Source[Key] : FVaRestJson();
}
FString FPlanner::TypeId(const FString& SourceId,const FString& Owner)
{
	if (Named.Contains(SourceId)) return SourceId;
	if (const auto* Existing=SemanticIds.Find(SourceId)) return *Existing;
	FString Id=Owner;
	if (const auto* Other=DefinitionSources.Find(Owner))
	{
		const auto* Left=Schema(SourceId);const auto* Right=Schema(*Other);
		if (!Left || !Right || !Equal(Shape({SourceId,Left->Location,{}}),Shape({*Other,Right->Location,{}}))) Id=SemanticId(Owner,TEXT("schemaVariant"),SourceId);
	}
	DefinitionSources.Add(Id,SourceId);SemanticIds.Add(SourceId,Id);return Id;
}
FString FPlanner::TypeHint(const FVaRestOpenApiSchema& Node,const FString& Hint) const
{
	if (!Named.Contains(Node.Id)) return Hint;
	int32 Slash;FString Name=Node.Location.Pointer.FindLastChar(TEXT('/'),Slash) ? Node.Location.Pointer.Mid(Slash+1) : Hint;
	return Name.Replace(TEXT("~1"),TEXT("/"),ESearchCase::CaseSensitive).Replace(TEXT("~0"),TEXT("~"),ESearchCase::CaseSensitive);
}
FPlannedValue FPlanner::Enum(const FVaRestOpenApiSchemaUse& Use,const FVaRestJson& Values,const FString& Owner,const FString& Hint,bool bNeedsUnset)
{
	const FString Id=TypeId(Use.Id,Owner);FPlannedValue Out;Out.Type.Kind=EVaRestOpenApiValueKind::String;
	const int32* Existing=EnumIndices.Find(Id);
	if (Existing && Plan.Enums[*Existing].Definition.bHasUnset) bNeedsUnset=true;
	auto Definition=FVaRestOpenApiEnumPlanner::Build(Values,bNeedsUnset,Options.bEnumsAsString);
	if (Definition.bStringFallback) { Issue(Use.Location,TEXT("OA-ENUM-STRING"),Definition.StringFallbackReason,EVaRestOpenApiSeverity::Info);return Out; }
	if (Existing) Plan.Enums[*Existing].Definition=MoveTemp(Definition);
	else
	{
		const auto* Node=Schema(Use.Id);FVaRestOpenApiPlannedEnum Row;Row.Id=Id;
		Row.AssetName=AllocateTypeName(Id,Node ? TypeHint(*Node,Hint) : Hint,true);
		Row.Tooltip=Tooltip(Meta(Use,TEXT("description")).AsString()+TEXT("\nGenerated from ")+Use.Location.Document+TEXT("#")+Use.Location.Pointer);
		Row.Definition=MoveTemp(Definition);EnumIndices.Add(Id,Plan.Enums.Add(MoveTemp(Row)));
	}
	Out.Type.Kind=EVaRestOpenApiValueKind::Enum;Out.Type.DefinitionId=Id;return Out;
}
FPlannedValue FPlanner::Type(const FVaRestOpenApiSchemaUse& Use,const FString& Owner,const FString& Hint,bool bNeedsUnset,bool bGenerateBase)
{
	FPlannedValue Out;const auto* Node=Schema(Use.Id);
	if (!Node) { Issue(Use.Location,TEXT("OA-REF-UNRESOLVED"),TEXT("The planner could not find the normalized schema."),EVaRestOpenApiSeverity::Error);return Out; }
	Out.bNullable=Use.Annotations.Has(TEXT("nullable")) ? Use.Annotations[TEXT("nullable")].AsBool() : Node->bNullable;
	if (ResolvingTypes.Contains(Use.Id)) { Out.bCycle=true;Issue(Use.Location,TEXT("OA-TYPE-RECURSIVE"),TEXT("This recursive type edge remains Json."),EVaRestOpenApiSeverity::Info);return Out; }
	if (Depth>=VaRest::Json::MaxDepth) { Issue(Use.Location,TEXT("OA-DOC-LIMIT"),TEXT("The plan type graph exceeds the supported traversal depth."),EVaRestOpenApiSeverity::Error);return Out; }
	TGuardValue<int32> DepthGuard(Depth,Depth+1);
	ResolvingTypes.Add(Use.Id);ON_SCOPE_EXIT { ResolvingTypes.Remove(Use.Id); };
	static const FStringSet Unsupported={TEXT("not"),TEXT("if"),TEXT("then"),TEXT("else"),TEXT("prefixItems"),TEXT("patternProperties")};
	bool bUnsupported=Node->bDynamicFallback || Node->bBooleanSchema;
	for (const auto& Key:Unsupported) bUnsupported=bUnsupported || Node->Source.Has(Key);
	if (bUnsupported) { Issue(Use.Location,TEXT("OA-TYPE-UNSUPPORTED"),TEXT("This schema remains Json because its constraints cannot be represented by a Blueprint type."));return Out; }
	if (bGenerateBase && !Node->Properties.IsEmpty() && (HasType(*Node,TEXT("object")) || Node->Types.IsEmpty())) { auto Value=Struct(Use,Owner,Hint);Value.bNullable=Out.bNullable;return Value; }
	const auto& Union=!Node->OneOf.IsEmpty() ? Node->OneOf : Node->AnyOf;
	if (!Union.IsEmpty())
	{
		TArray<FVaRestOpenApiSchemaUse> Members;bool bAllStrings=true,bAllEnums=true,bAllNumbers=true,bAllIntegers=true;
		auto Values=FVaRestJson::Array();FStringSet SeenValues;
		for (const auto& Member:Union)
		{
			const auto* Part=Schema(Member.Id);if (!Part) continue;
			if (NullOnly(*Part)) { Out.bNullable=true;continue; }
			Members.Add(Member);bAllStrings=bAllStrings && StringLike(*Part);bAllNumbers=bAllNumbers && (HasType(*Part,TEXT("integer")) || HasType(*Part,TEXT("number")));
			bAllIntegers=bAllIntegers && HasType(*Part,TEXT("integer"));
			if (Part->Const.IsString()) { const FString Value=Part->Const.AsString();if (!SeenValues.Contains(Value)) { SeenValues.Add(Value);Values.Add(Value); } }
			else if (Part->Enum.IsArray()) for (const auto& Value:Part->Enum.Elements()) { if (!Value.IsString()) { bAllEnums=false;break; }const FString Text=Value.AsString();if (!SeenValues.Contains(Text)) { SeenValues.Add(Text);Values.Add(Text); } }
			else bAllEnums=false;
		}
		if (Members.Num()==1) { const bool Nullable=Out.bNullable;Out=Type(Members[0],Owner,Hint,bNeedsUnset || Nullable,bGenerateBase);Out.bNullable=Out.bNullable || Nullable;return Out; }
		if (Members.IsEmpty()) return Out;
		if (bAllStrings)
		{
			if (bAllEnums && Values.Num()>1) { auto EnumValue=Enum(Use,Values,Owner,Hint,bNeedsUnset || Out.bNullable);EnumValue.bNullable=Out.bNullable;return EnumValue; }
			Out.Type.Kind=EVaRestOpenApiValueKind::String;return Out;
		}
		if (bAllNumbers) { Out.Type.Kind=bAllIntegers ? EVaRestOpenApiValueKind::Int64 : EVaRestOpenApiValueKind::Double;return Out; }
		for (int32 Index=0;Index<Members.Num();++Index) Type(Members[Index],SemanticId(Owner,TEXT("option"),LexToString(Index+1)),Hint+TEXT("Option")+LexToString(Index+1),false,true);
		Issue(Use.Location,TEXT("OA-TYPE-UNION"),TEXT("Mixed union values remain Json; object variants are available as generated structs."),EVaRestOpenApiSeverity::Info);return Out;
	}
	if (Node->Types.Num()>1)
	{
		bool bNumeric=true;for (const auto& Kind:Node->Types) bNumeric=bNumeric && (Equal(Kind,TEXT("integer")) || Equal(Kind,TEXT("number")));
		if (bNumeric) Out.Type.Kind=HasType(*Node,TEXT("number")) ? EVaRestOpenApiValueKind::Double : EVaRestOpenApiValueKind::Int64;
		else Issue(Use.Location,TEXT("OA-TYPE-UNION"),TEXT("This type set remains Json."),EVaRestOpenApiSeverity::Info);
		return Out;
	}
	if (StringLike(*Node))
	{
		if (Node->Enum.IsArray() && Node->Enum.Num()>1) { auto EnumValue=Enum(Use,Node->Enum,Owner,Hint,bNeedsUnset || Out.bNullable);EnumValue.bNullable=Out.bNullable;return EnumValue; }
		if (Equal(Node->Format,TEXT("date-time")) && !Options.bDateTimeAsString) Out.Type.Kind=EVaRestOpenApiValueKind::DateTime;
		else if (Equal(Node->Format,TEXT("byte")) || Equal(Node->Source[TEXT("contentEncoding")].AsString(),TEXT("base64"))) { Out.Type.Kind=EVaRestOpenApiValueKind::Bytes;Out.bBase64=true; }
		else { Out.Type.Kind=EVaRestOpenApiValueKind::String;if (Equal(Node->Format,TEXT("binary"))) Issue(Use.Location,TEXT("OA-TYPE-BINARYJSON"),TEXT("Binary inside a JSON value stays a string.")); }
		return Out;
	}
	if (HasType(*Node,TEXT("integer")))
	{
		Out.Type.Kind=Equal(Node->Format,TEXT("int32")) || (Node->Format.IsEmpty() && Options.bIntegersWithoutFormatAsInt32) ? EVaRestOpenApiValueKind::Int : EVaRestOpenApiValueKind::Int64;
		const auto Minimum=Node->Source[TEXT("minimum")].TryGet<int32>(),Maximum=Node->Source[TEXT("maximum")].TryGet<int32>();
		if (Node->Format.IsEmpty() && Minimum.IsSet() && Maximum.IsSet()) Out.Type.Kind=EVaRestOpenApiValueKind::Int;
		if (Equal(Node->Format,TEXT("uint64"))) Issue(Use.Location,TEXT("OA-TYPE-UINT64"),TEXT("Blueprint has no unsigned 64-bit value; this field uses Integer64."));return Out;
	}
	if (HasType(*Node,TEXT("number"))) { Out.Type.Kind=EVaRestOpenApiValueKind::Double;return Out; }
	if (HasType(*Node,TEXT("boolean"))) { Out.Type.Kind=EVaRestOpenApiValueKind::Bool;return Out; }
	if (Node->Types.IsEmpty() && Node->Const.IsValid())
	{
		if (Node->Const.IsBool()) Out.Type.Kind=EVaRestOpenApiValueKind::Bool;
		else if (Node->Const.IsNumber()) Out.Type.Kind=Node->Const.TryGet<int64>().IsSet() ? EVaRestOpenApiValueKind::Int64 : EVaRestOpenApiValueKind::Double;
		return Out;
	}
	if (HasType(*Node,TEXT("array")) || Node->Items.IsValid())
	{
		if (!Node->Items.IsValid()) return Out;
		auto Inner=Type(Node->Items,SemanticId(Owner,TEXT("items")),Hint+TEXT("Item"));
		if (Inner.bCycle) { Out.bCycle=true;return Out; }
		if (Inner.Type.Container!=EVaRestOpenApiContainer::None || Inner.Type.Kind==EVaRestOpenApiValueKind::Bytes)
		{ Issue(Use.Location,TEXT("OA-TYPE-UNSUPPORTED"),TEXT("Nested containers remain Json because Blueprint pins have one container."));return Out; }
		Out.Type=Inner.Type;Out.Type.Container=EVaRestOpenApiContainer::Array;return Out;
	}
	if (Polymorphic.Contains(Use.Id) && !bGenerateBase)
	{
		Struct(Use,Owner,Hint);
		Issue(Use.Location,TEXT("OA-TYPE-POLYMORPHIC"),TEXT("A polymorphic base remains Json so subtype fields are retained."),EVaRestOpenApiSeverity::Info);return Out;
	}
	if (!Node->Properties.IsEmpty() || !Node->AllOf.IsEmpty())
	{
		auto Value=Struct(Use,Owner,Hint);Value.bNullable=Out.bNullable;return Value;
	}
	if (Node->AdditionalProperties.IsValid())
	{
		auto Inner=Type(Node->AdditionalProperties,SemanticId(Owner,TEXT("values")),Hint+TEXT("Value"));
		if (Inner.bCycle) { Out.bCycle=true;return Out; }
		if (Inner.Type.Container!=EVaRestOpenApiContainer::None || Inner.Type.Kind==EVaRestOpenApiValueKind::Bytes)
		{ Issue(Use.Location,TEXT("OA-TYPE-UNSUPPORTED"),TEXT("Nested map values remain Json because Blueprint pins have one container."));return Out; }
		Out.Type=Inner.Type;Out.Type.Container=EVaRestOpenApiContainer::Map;return Out;
	}
	return Out;
}
void FPlanner::Flatten(const FVaRestOpenApiSchemaUse& Use,TArray<FFlatProperty>& Fields,FStringSet& Required,FStringSet& Visiting)
{
	const auto* Node=Schema(Use.Id);if (!Node || Visiting.Contains(Use.Id)) return;
	if (Visiting.Num()>=VaRest::Json::MaxDepth) { Issue(Use.Location,TEXT("OA-DOC-LIMIT"),TEXT("The allOf graph exceeds the supported traversal depth."),EVaRestOpenApiSeverity::Error);return; }
	Visiting.Add(Use.Id);
	for (const auto& Parent:Node->AllOf) Flatten(Parent,Fields,Required,Visiting);
	for (const auto& Name:Node->RequiredFields) Required.Add(Name);
	for (const auto& Property:Node->Properties)
	{
		const int32 Old=Fields.IndexOfByPredicate([&](const auto& Field) { return Equal(Field.Name,Property.Name); });
		if (Old==INDEX_NONE) { FFlatProperty Field;Field.Name=Property.Name;Field.Use=Property.Schema;Fields.Add(MoveTemp(Field)); }
		else { Fields[Old].PriorTypes.Add(Fields[Old].Use);Fields[Old].Use=Property.Schema; }
	}
	Visiting.Remove(Use.Id);
}
FPlannedValue FPlanner::Struct(const FVaRestOpenApiSchemaUse& Use,const FString& Owner,const FString& Hint)
{
	FPlannedValue Out;
	if (Building.Contains(Use.Id)) { Out.bCycle=true;Issue(Use.Location,TEXT("OA-TYPE-RECURSIVE"),TEXT("This containment back-edge is Json; convert its values explicitly with Json To Struct."),EVaRestOpenApiSeverity::Info);return Out; }
	if (const auto* Existing=Built.Find(Use.Id)) { Out.Type=*Existing;return Out; }
	const auto* Node=Schema(Use.Id);if (!Node) return Out;
	TArray<FFlatProperty> Properties;FStringSet Required,Visiting;Flatten(Use,Properties,Required,Visiting);if (Properties.IsEmpty()) return Out;
	const FString Id=TypeId(Use.Id,Owner);FString Name;
	if (const auto* Existing=BuiltDefinitions.Find(Id)) { Out.Type=*Existing;Built.Add(Use.Id,Out.Type);return Out; }
	if (const auto* Existing=AssetNames.Find(Id)) Name=*Existing;else { Name=AllocateTypeName(Id,TypeHint(*Node,Hint),false);AssetNames.Add(Id,Name); }
	Building.Add(Use.Id);
	FVaRestOpenApiPlannedStruct Row;Row.Id=Id;Row.AssetName=Name;
	Row.Tooltip=Tooltip(Meta(Use,TEXT("description")).AsString()+TEXT("\nGenerated from ")+Use.Location.Document+TEXT("#")+Use.Location.Pointer);
	TArray<FString> Keys;for (const auto& Property:Properties) Keys.Add(Property.Name);const auto FieldNames=FVaRestOpenApiNameAllocator::Fields(Keys);
	for (int32 Index=0;Index<Properties.Num();++Index)
	{
		const auto& Property=Properties[Index];FVaRestOpenApiPlannedField Field;
		Field.Id=SemanticId(Id,TEXT("field"),Property.Name);Field.WireName=Property.Name;Field.FriendlyName=FieldNames[Index].FriendlyName;Field.Location=Property.Use.Location;Field.bRequired=Required.Contains(Property.Name);
		Field.Default=Meta(Property.Use,TEXT("default"));const auto* ChildNode=Schema(Property.Use.Id);
		const bool bConst=ChildNode && (ChildNode->Const.IsValid() || (ChildNode->Enum.IsArray() && ChildNode->Enum.Num()==1));
		if (!Field.Default.IsValid() && bConst) Field.Default=ChildNode->Const.IsValid() ? ChildNode->Const : ChildNode->Enum.At(0);
		auto Value=Type(Property.Use,Field.Id,TypeHint(*Node,Hint)+FVaRestOpenApiNameAllocator::Pascal(Property.Name),!Field.bRequired && !Field.Default.IsValid());
		Field.Type=Value.Type;Field.bNullable=Value.bNullable;Field.bBase64=Value.bBase64;
		for (const auto& Prior:Property.PriorTypes)
		{
			const auto PriorType=Type(Prior,Field.Id,TypeHint(*Node,Hint)+FVaRestOpenApiNameAllocator::Pascal(Property.Name));
			if (!SameType(PriorType.Type,Field.Type)) { Field.Type={};Field.bBase64=false;Issue(Field.Location,TEXT("OA-TYPE-ALLOFCONFLICT"),TEXT("Conflicting allOf member types leave this field as Json."));break; }
		}
		const bool bReadOnly=Meta(Property.Use,TEXT("readOnly")).AsBool();
		Field.Presence=bReadOnly ? EVaRestOpenApiPresence::DecodeOnly : bConst ? EVaRestOpenApiPresence::Always : Field.bRequired && Field.bNullable ? EVaRestOpenApiPresence::NullWhenDefault :
			Field.bRequired || (Field.Type.Kind==EVaRestOpenApiValueKind::Enum && !Field.Default.IsValid()) ? EVaRestOpenApiPresence::Always : EVaRestOpenApiPresence::OmitWhenDefault;
		Field.bDefaultInTooltipOnly=Field.Default.IsValid() && (Field.Type.Container!=EVaRestOpenApiContainer::None || Field.Type.Kind==EVaRestOpenApiValueKind::Struct || Field.Type.Kind==EVaRestOpenApiValueKind::Json || Field.Type.Kind==EVaRestOpenApiValueKind::Bytes || Field.Type.Kind==EVaRestOpenApiValueKind::DateTime);
		Field.Tooltip=Tooltip(Meta(Property.Use,TEXT("description")).AsString()+TEXT("\nJSON: ")+Property.Name+(Field.bRequired ? TEXT(" · required") : TEXT(" · optional"))+(Field.bNullable ? TEXT(" · nullable") : TEXT(""))+
			(Meta(Property.Use,TEXT("deprecated")).AsBool() ? TEXT(" · [Deprecated]") : TEXT("")));
		if (ChildNode)
		{
			FString Details;
			if (!ChildNode->Format.IsEmpty()) Details+=TEXT(" · format: ")+ChildNode->Format;
			for (const TCHAR* Constraint:{TEXT("minimum"),TEXT("maximum"),TEXT("exclusiveMinimum"),TEXT("exclusiveMaximum"),TEXT("minLength"),TEXT("maxLength"),TEXT("minItems"),TEXT("maxItems"),TEXT("pattern")})
				if (ChildNode->Source.Has(Constraint)) Details+=FString(TEXT(" · "))+Constraint+TEXT(": ")+Preview(ChildNode->Source[Constraint]);
			if (Meta(Property.Use,TEXT("writeOnly")).AsBool()) Details+=TEXT(" · write only");
			if (Field.bDefaultInTooltipOnly) Details+=TEXT(" · default (tooltip only): ")+Preview(Field.Default);
			const auto Example=Meta(Property.Use,TEXT("example"));
			if (Example.IsValid()) Details+=TEXT(" · example: ")+Preview(Example);
			else if (const auto Examples=Meta(Property.Use,TEXT("examples")); Examples.IsArray() && Examples.Num()>0) Details+=TEXT(" · example: ")+Preview(Examples.At(0));
			if (ChildNode->Enum.IsArray() && Field.Type.Kind!=EVaRestOpenApiValueKind::Enum) Details+=TEXT(" · values: ")+Preview(ChildNode->Enum);
			if (ChildNode->Source.Has(TEXT("x-enum-varnames"))) Details+=TEXT(" · enum names: ")+Preview(ChildNode->Source[TEXT("x-enum-varnames")]);
			Field.Tooltip=Tooltip(Field.Tooltip+Details);
		}
		if (FieldNames[Index].bSanitized) Issue(Field.Location,TEXT("OA-NAME-SANITISED"),TEXT("The Blueprint friendly name differs from its exact JSON wire key."),EVaRestOpenApiSeverity::Info);
		if (!Field.bRequired && !Field.Default.IsValid() && Field.Type.Container==EVaRestOpenApiContainer::None &&
			(Field.Type.Kind==EVaRestOpenApiValueKind::Bool || Field.Type.Kind==EVaRestOpenApiValueKind::Int || Field.Type.Kind==EVaRestOpenApiValueKind::Int64 || Field.Type.Kind==EVaRestOpenApiValueKind::Double))
			Issue(Field.Location,TEXT("OA-OPT-DEFAULTNOTSENT"),TEXT("This optional field omits false/zero; use Presence Always or Body Json to send it explicitly."),EVaRestOpenApiSeverity::Info);
		Row.Fields.Add(MoveTemp(Field));
	}
	for (const auto& Parent:Node->AllOf)
	{
		const auto* Base=Schema(Parent.Id);if (!Base || !Base->Discriminator.IsObject()) continue;
		const FString Key=Base->Discriminator[TEXT("propertyName")].AsString();if (Key.IsEmpty()) continue;
		FString Value;bool bMapped=false;
		for (const auto& Mapping:Base->DiscriminatorMappings) if (Equal(Mapping.Value.Id,Use.Id)) { Value=Mapping.Key;bMapped=true;break; }
		if (!bMapped)
		{
			int32 Slash;Value=Node->Location.Pointer.FindLastChar(TEXT('/'),Slash) ? Node->Location.Pointer.Mid(Slash+1) : TypeHint(*Node,Hint);
			Value=Value.Replace(TEXT("~1"),TEXT("/"),ESearchCase::CaseSensitive).Replace(TEXT("~0"),TEXT("~"),ESearchCase::CaseSensitive);
		}
		for (auto& Field:Row.Fields) if (Equal(Field.WireName,Key)) { Field.Default=FVaRestJson::From(Value);Field.Presence=EVaRestOpenApiPresence::Always;Field.bDefaultInTooltipOnly=false;break; }
	}
	Building.Remove(Use.Id);
	if (Node->bAllowsAdditionalProperties || Node->AdditionalProperties.IsValid()) Issue(Use.Location,TEXT("OA-TYPE-EXTRAKEYS"),TEXT("Additional keys beside declared struct fields are not captured."),EVaRestOpenApiSeverity::Info);
	Out.Type.Kind=EVaRestOpenApiValueKind::Struct;Out.Type.DefinitionId=Id;Built.Add(Use.Id,Out.Type);BuiltDefinitions.Add(Id,Out.Type);
	Plan.Structs.Add(MoveTemp(Row));return Out; // children completed first: native apply receives topological order
}
FString FPlanner::DefaultText(const FVaRestOpenApiPlannedType& Type,const FVaRestJson& Value,bool bUnset,bool bNullable,const FVaRestOpenApiLocation& Location)
{
	if (Type.Container!=EVaRestOpenApiContainer::None) return {};
	if (Type.Kind==EVaRestOpenApiValueKind::Enum)
	{
		const auto* Index=EnumIndices.Find(Type.DefinitionId);if (!Index) return {};
		const auto Default=Plan.Enums[*Index].Definition.DefaultInternalName(Value,bUnset,bNullable);
		if (Default.IsSet()) return Default.GetValue();
		Issue(Location,TEXT("OA-APPLY-DEFAULT"),TEXT("The declared default does not match an exact enum wire value and was dropped."));
		return Plan.Enums[*Index].Definition.DefaultInternalName({},bUnset,bNullable).Get(FString());
	}
	if (!Value.IsValid() || Value.IsNull()) return {};
	bool bValid=false;
	switch (Type.Kind)
	{
	case EVaRestOpenApiValueKind::String:bValid=Value.IsString();break;
	case EVaRestOpenApiValueKind::Bool:bValid=Value.IsBool();break;
	case EVaRestOpenApiValueKind::Int:bValid=Value.TryGet<int32>().IsSet();break;
	case EVaRestOpenApiValueKind::Int64:bValid=Value.TryGet<int64>().IsSet();break;
	case EVaRestOpenApiValueKind::Double:bValid=Value.IsNumber();break;
	default:return {};
	}
	if (!bValid) { Issue(Location,TEXT("OA-APPLY-DEFAULT"),TEXT("The declared scalar default is incompatible with the selected Blueprint type and was dropped."));return {}; }
	return Value.ToString();
}
void FPlanner::Defaults()
{
	// Unset is combined over all enum uses before defaults resolve to their final internal identity.
	for (auto& Row:Plan.Structs) for (auto& Field:Row.Fields) if (!Field.bDefaultInTooltipOnly)
		Field.DefaultText=DefaultText(Field.Type,Field.Default,!Field.bRequired || Field.bNullable,Field.bNullable,Field.Location);
	for (auto& Endpoint:Plan.Endpoints) for (auto& Parameter:Endpoint.Parameters)
	{
		if (Parameter.Type.Container==EVaRestOpenApiContainer::Array)
		{
			Parameter.DefaultText.Reset();if (!Parameter.Default.IsValid()) continue;
			if (!Parameter.Default.IsArray()) { Issue(Parameter.Location,TEXT("OA-APPLY-DEFAULT"),TEXT("The array parameter default is not an array and was dropped."));continue; }
			auto Values=FVaRestJson::Array();bool Valid=true;
			for (const auto& Value:Parameter.Default.Elements())
			{
				switch (Parameter.Type.Kind)
				{
				case EVaRestOpenApiValueKind::Enum:
				{
					const auto* Enum=EnumIndices.Find(Parameter.Type.DefinitionId);
					const auto Internal=Enum ? Plan.Enums[*Enum].Definition.DefaultInternalName(Value,false,false) : TOptional<FString>();
					if (Internal.IsSet()) Values.Add(Internal.GetValue());else Valid=false;break;
				}
				case EVaRestOpenApiValueKind::String:
					if (!Value.IsString()) Valid=false;
					else { const FString Text=Value.AsString();for (int32 Index=0;Index<Text.Len();++Index) if (!Text[Index]) Valid=false;Values.Add(Text); }break;
				case EVaRestOpenApiValueKind::Bool:Valid=Valid && Value.IsBool();Values.Add(Value);break;
				case EVaRestOpenApiValueKind::Int:Valid=Valid && Value.TryGet<int32>().IsSet();Values.Add(Value);break;
				case EVaRestOpenApiValueKind::Int64:Valid=Valid && Value.TryGet<int64>().IsSet();Values.Add(Value);break;
				case EVaRestOpenApiValueKind::Double:Valid=Valid && Value.IsNumber();Values.Add(Value);break;
				default:Valid=false;break;
				}
			}
			if (Valid) Parameter.DefaultText=Values.ToString();else Issue(Parameter.Location,TEXT("OA-APPLY-DEFAULT"),TEXT("An array default element cannot be represented by its scalar K2 type; the default was dropped."));
			continue;
		}
		Parameter.DefaultText=DefaultText(Parameter.Type,Parameter.Default,false,false,Parameter.Location);
		// K2 string pin defaults contain the string itself; UDS member defaults use quoted import text.
		if (Parameter.Type.Kind==EVaRestOpenApiValueKind::String && Parameter.Default.IsString())
		{
			Parameter.DefaultText=Parameter.Default.AsString();
			for (int32 Index=0;Index<Parameter.DefaultText.Len();++Index) if (!Parameter.DefaultText[Index]) { Parameter.DefaultText.Reset();Issue(Parameter.Location,TEXT("OA-APPLY-DEFAULT"),TEXT("A null character cannot be stored in a scalar pin literal; the default was dropped."));break; }
		}
	}
}
}
