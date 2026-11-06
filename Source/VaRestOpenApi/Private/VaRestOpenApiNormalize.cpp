// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiAnalysis.h"
#include "Misc/ScopeExit.h"

namespace VaRest::OpenApi
{
FString NumberSource(const FDocument& Doc,const FString& Pointer);
namespace
{
const FStringSet& AnnotationNames()
{
	static const FStringSet Names={TEXT("title"),TEXT("description"),TEXT("default"),TEXT("example"),TEXT("examples"),TEXT("readOnly"),TEXT("writeOnly"),TEXT("deprecated"),TEXT("nullable"),TEXT("summary")};return Names;
}
FVaRestJson Annotations(const FVaRestJson& Source)
{
	auto Result=FVaRestJson::Object();
	for (const auto& Member:Source.Members()) if (AnnotationNames().Contains(Member.GetKey())) Result.Set(Member.GetKey(),Member.Value);
	return Result;
}
FVaRestJson Effective(const FResolved& Value,const TCHAR* Name)
{
	return Value.Annotations.Has(Name) ? Value.Annotations[Name] : Value.Value[Name];
}
FVaRestOpenApiLocation Field(const FResolved& Value,const TCHAR* Name) { return {Value.Location.Document,Child(Value.Location.Pointer,Name)}; }
bool OnlyAnnotations(const FVaRestJson& Value,const TCHAR* Structural)
{
	for (const auto& Member:Value.Members()) if (!Equal(Member.GetKey(),Structural) && !AnnotationNames().Contains(Member.GetKey())) return false;
	return true;
}
}

bool FAnalysis::Boolean(const FVaRestJson& Value,const FVaRestOpenApiLocation& Location,bool Default)
{
	if (!Value.IsValid()) return Default;
	if (Value.IsBool()) return Value.AsBool();
	if (Value.IsString())
	{
		const FString Text=Value.AsString().ToLower();
		if (Text==TEXT("yes") || Text==TEXT("on") || Text==TEXT("no") || Text==TEXT("off"))
		{ Issue(Location,TEXT("OA-YAML-11BOOL"),TEXT("A YAML 1.1 boolean word was accepted as a boolean."),EVaRestOpenApiSeverity::Warning);return Text==TEXT("yes") || Text==TEXT("on"); }
	}
	Issue(Location,TEXT("OA-TYPE-UNSUPPORTED"),TEXT("A boolean field has an unsupported value."),EVaRestOpenApiSeverity::Warning);return Default;
}
FVaRestOpenApiSchemaUse FAnalysis::Schema(const FVaRestOpenApiLocation& Location,FStringSet* Chain)
{
	const auto Raw=At(Location);if (!Raw.IsValid() || bFatal) return {};
	const FString Id=Identity(Raw.Location);
	if (SchemaIndices.Contains(Id)) return {Id,Location,{}};
	if (SchemaDepth>=VaRest::Json::MaxDepth) { Issue(Location,TEXT("OA-DOC-LIMIT"),TEXT("The schema graph exceeds the supported traversal depth."));return {}; }
	TGuardValue<int32> Depth(SchemaDepth,SchemaDepth+1);
	if (!Raw.Value.IsObject() && !Raw.Value.IsBool())
	{ Issue(Location,TEXT("OA-TYPE-UNSUPPORTED"),TEXT("A schema must be an object or a boolean."));return {}; }
	FStringSet Local;auto& Seen=Chain ? *Chain : Local;
	if (Seen.Contains(Id)) { Issue(Location,TEXT("OA-REF-LOOP"),TEXT("A pure schema alias loop was found."));return {}; }
	if (Seen.Num()>=VaRest::Json::MaxDepth) { Issue(Location,TEXT("OA-DOC-LIMIT"),TEXT("The schema reference chain exceeds the supported depth."));return {}; }
	Seen.Add(Id);ON_SCOPE_EXIT { Seen.Remove(Id); };
	FVaRestOpenApiSchemaUse Base;
	FVaRestOpenApiLocation BaseLocation;
	bool bHasBase=false;
	const auto Ref=Raw.Value[TEXT("$ref")];
	const bool bDynamic=Raw.Value.Has(TEXT("$id")) || Raw.Value.Has(TEXT("$dynamicRef"));
	if (Ref.IsValid() && !bDynamic)
	{
		if (!Ref.IsString()) { Issue(Location,TEXT("OA-REF-UNRESOLVED"),TEXT("A schema reference must be a string."));return {}; }
		const auto Target=Reference(Raw.Location,Ref.AsString());if (!Target.IsValid()) return {};
		if (OnlyAnnotations(Raw.Value,TEXT("$ref")))
		{
			Base=Schema(Target.Location,&Seen);if (!Base.IsValid()) return {};
			if (!Base.Annotations.IsValid()) Base.Annotations=FVaRestJson::Object();
			for (const auto& Member:Annotations(Raw.Value).Members()) Base.Annotations.Set(Member.GetKey(),Member.Value);
			Base.Location=Location;return Base;
		}
		BaseLocation=Target.Location;bHasBase=true;
	}
	else if (const auto All=Raw.Value[TEXT("allOf")]; All.IsArray() && All.Num()==1 && All.At(0).Has(TEXT("$ref")) && OnlyAnnotations(Raw.Value,TEXT("allOf")) && OnlyAnnotations(All.At(0),TEXT("$ref")))
	{
		Base=Schema({Location.Document,Child(Child(Location.Pointer,TEXT("allOf")),TEXT("0"))},&Seen);
		if (!Base.IsValid()) return {};
		if (!Base.Annotations.IsValid()) Base.Annotations=FVaRestJson::Object();
		for (const auto& Member:Annotations(Raw.Value).Members()) Base.Annotations.Set(Member.GetKey(),Member.Value);
		Base.Location=Location;return Base;
	}
	if (Result.Model.Schemas.Num()>=Options.MaxSchemas) { Issue(Location,TEXT("OA-DOC-LIMIT"),TEXT("The import exceeds MaxSchemas."));return {}; }
	const int32 Index=Result.Model.Schemas.AddDefaulted();SchemaIndices.Add(Id,Index); // reserve before recursive child edges
	FVaRestOpenApiSchema Node;Node.Id=Id;Node.Location=Raw.Location;Node.Source=Raw.Value;
	if (Raw.Value.IsBool()) { Node.bBooleanSchema=true;Node.bAcceptsAny=Raw.Value.AsBool(); }
	else if (bDynamic)
	{ Node.bDynamicFallback=true;Issue(Location,TEXT("OA-REF-DYNAMIC"),TEXT("Dynamic/id-based schema resolution is unsupported; this schema remains Json."),EVaRestOpenApiSeverity::Warning); }
	else
	{
		if (bHasBase)
		{
			// Assertion siblings make this a real graph node. Recursive edges now find its reserved identity.
			Base=Schema(BaseLocation);
			if (!Base.IsValid())
			{
				SchemaIndices.Remove(Id);
				if (Result.Model.Schemas.Num()==Index+1) Result.Model.Schemas.Pop(EAllowShrinking::No);
				return {};
			}
			Node.AllOf.Add(Base);
		}
		const auto Types=Raw.Value[TEXT("type")];
		const auto AddType=[&](const FVaRestJson& Type) {
			if (!Type.IsString()) { Issue(Field(Raw,TEXT("type")),TEXT("OA-TYPE-UNSUPPORTED"),TEXT("A schema type must be a string."),EVaRestOpenApiSeverity::Warning);Node.bDynamicFallback=true;return; }
			const FString Text=Type.AsString();if (Equal(Text,TEXT("null"))) Node.bNullable=true;
			else if (!Node.Types.ContainsByPredicate([&](const FString& Existing) { return Equal(Existing,Text); })) Node.Types.Add(Text);
		};
		if (Types.IsArray()) for (const auto& Type:Types.Elements()) AddType(Type);else if (Types.IsValid()) AddType(Types);
		Node.bNullable=Boolean(Raw.Value[TEXT("nullable")],Field(Raw,TEXT("nullable"))) || Node.bNullable;
		Node.bReadOnly=Boolean(Raw.Value[TEXT("readOnly")],Field(Raw,TEXT("readOnly")));
		Node.bWriteOnly=Boolean(Raw.Value[TEXT("writeOnly")],Field(Raw,TEXT("writeOnly")));
		Node.bDeprecated=Boolean(Raw.Value[TEXT("deprecated")],Field(Raw,TEXT("deprecated")));
		Node.Format=Raw.Value[TEXT("format")].AsString();Node.Description=Raw.Value[TEXT("description")].AsString();
		Node.Enum=Raw.Value[TEXT("enum")];Node.Const=Raw.Value[TEXT("const")];Node.Default=Raw.Value[TEXT("default")];Node.Examples=Raw.Value[TEXT("examples")];Node.Discriminator=Raw.Value[TEXT("discriminator")];
		const auto Mapping=Node.Discriminator[TEXT("mapping")];FStringSet SeenMappings;
		for (const auto& Entry:Mapping.Members())
		{
			const FString Name=Entry.GetKey();if (SeenMappings.Contains(Name)) continue;SeenMappings.Add(Name);
			const auto TargetText=Mapping.Get(Name);const FVaRestOpenApiLocation Here{Location.Document,Child(Child(Child(Location.Pointer,TEXT("discriminator")),TEXT("mapping")),Name)};
			if (!TargetText.IsString()) { Issue(Here,TEXT("OA-REF-UNRESOLVED"),TEXT("A discriminator mapping must identify a schema reference."));continue; }
			const auto Target=Reference(Here,TargetText.AsString());if (!Target.IsValid()) continue;
			auto TargetSchema=Schema(Target.Location);if (TargetSchema.IsValid()) Node.DiscriminatorMappings.Emplace(Name,MoveTemp(TargetSchema));
		}
		const auto Required=Raw.Value[TEXT("required")];FStringSet RequiredNames;
		for (const auto& Name:Required.Elements()) if (Name.IsString() && !RequiredNames.Contains(Name.AsString()))
		{ RequiredNames.Add(Name.AsString());Node.RequiredFields.Add(Name.AsString()); }
		const auto Properties=Raw.Value[TEXT("properties")];
		if (Properties.IsObject())
		{
			if (Node.Types.IsEmpty()) Node.Types.Add(TEXT("object"));
			FStringSet Visited;
			for (const auto& Member:Properties.Members())
			{
				const FString Name=Member.GetKey();if (Visited.Contains(Name)) continue;Visited.Add(Name);
				FVaRestOpenApiProperty Property;Property.Name=Name;Property.bRequired=RequiredNames.Contains(Name);
				Property.Schema=Schema({Location.Document,Child(Child(Location.Pointer,TEXT("properties")),Name)});
				if (Property.Schema.IsValid()) Node.Properties.Add(MoveTemp(Property));
			}
		}
		if (Raw.Value.Has(TEXT("items"))) { Node.Items=Schema(Field(Raw,TEXT("items")));if (Node.Types.IsEmpty()) Node.Types.Add(TEXT("array")); }
		const auto Additional=Raw.Value[TEXT("additionalProperties")];
		if (Additional.IsBool()) Node.bAllowsAdditionalProperties=Additional.AsBool();
		else if (Additional.IsValid()) Node.AdditionalProperties=Schema(Field(Raw,TEXT("additionalProperties")));
		SchemaList(Node.AllOf,Raw,TEXT("allOf"));SchemaList(Node.OneOf,Raw,TEXT("oneOf"));SchemaList(Node.AnyOf,Raw,TEXT("anyOf"));
	}
	Result.Model.Schemas[Index]=MoveTemp(Node);return {Id,Location,{}};
}
void FAnalysis::SchemaList(TArray<FVaRestOpenApiSchemaUse>& Out,const FResolved& Parent,const TCHAR* Key)
{
	const auto Values=Parent.Value[Key];if (!Values.IsValid()) return;
	if (!Values.IsArray()) { Issue(Field(Parent,Key),TEXT("OA-TYPE-UNSUPPORTED"),TEXT("Schema composition must be an array."));return; }
	for (int32 Index=0;Index<Values.Num() && !bFatal;++Index)
	{
		auto Use=Schema({Parent.Location.Document,Child(Child(Parent.Location.Pointer,Key),LexToString(Index))});if (Use.IsValid()) Out.Add(MoveTemp(Use));
	}
}
TArray<FVaRestOpenApiMedia> FAnalysis::Media(const FResolved& Parent,bool& bValid)
{
	TArray<FVaRestOpenApiMedia> OutContent;const auto Content=Parent.Value[TEXT("content")];
	if (!Content.IsValid()) return OutContent;
	if (!Content.IsObject()) { bValid=false;Issue(Field(Parent,TEXT("content")),TEXT("OA-BODY-MEDIA"),TEXT("Media content must be an object."));return OutContent; }
	FStringSet Seen;
	for (const auto& Member:Content.Members())
	{
		const FString Name=Member.GetKey();if (Seen.Contains(Name)) continue;Seen.Add(Name);
		const auto Definition=Content.Get(Name);FVaRestOpenApiMedia Item;Item.Name=Name;Item.Location={Parent.Location.Document,Child(Child(Parent.Location.Pointer,TEXT("content")),Name)};
		if (!Definition.IsObject()) { bValid=false;Issue(Item.Location,TEXT("OA-BODY-MEDIA"),TEXT("A media definition must be an object."));continue; }
		Item.Encoding=Definition[TEXT("encoding")];Item.Examples=Definition[TEXT("examples")];
		if (Definition.Has(TEXT("schema"))) { Item.Schema=Schema({Item.Location.Document,Child(Item.Location.Pointer,TEXT("schema"))});if (!Item.Schema.IsValid()) { bValid=false;continue; } }
		OutContent.Add(MoveTemp(Item));
	}
	return OutContent;
}
bool FAnalysis::Parameter(const FVaRestOpenApiLocation& Location,FVaRestOpenApiParameter& Out)
{
	const auto Resolved=Object(Location);if (!Resolved.IsValid()) return false;
	if (!Resolved.Value.IsObject()) { Issue(Location,TEXT("OA-PARAM-STYLE"),TEXT("A parameter must be an object."));return false; }
	Out.Location=Location;Out.Name=Resolved.Value[TEXT("name")].AsString();Out.In=Resolved.Value[TEXT("in")].AsString();
	if (Out.Name.IsEmpty() || Out.In.IsEmpty()) { Issue(Location,TEXT("OA-PARAM-STYLE"),TEXT("A parameter requires exact nonempty name and in fields."));return false; }
	static const FStringSet Locations={TEXT("path"),TEXT("query"),TEXT("header"),TEXT("cookie"),TEXT("querystring")};
	if (!Locations.Contains(Out.In)) { Issue(Location,TEXT("OA-PARAM-STYLE"),TEXT("The parameter location is unsupported."));return false; }
	Out.bRequired=Equal(Out.In,TEXT("path")) || Boolean(Resolved.Value[TEXT("required")],Field(Resolved,TEXT("required")));
	Out.Style=Resolved.Value[TEXT("style")].AsString(Equal(Out.In,TEXT("query")) || Equal(Out.In,TEXT("cookie")) ? TEXT("form") : TEXT("simple"));
	Out.bExplode=Boolean(Resolved.Value[TEXT("explode")],Field(Resolved,TEXT("explode")),Equal(Out.Style,TEXT("form")));
	Out.bDeprecated=Boolean(Effective(Resolved,TEXT("deprecated")),Location);
	Out.Description=Effective(Resolved,TEXT("description")).AsString();Out.Content=Resolved.Value[TEXT("content")];
	if (Resolved.Value.Has(TEXT("schema")))
	{
		Out.Schema=Schema(Field(Resolved,TEXT("schema")));if (!Out.Schema.IsValid()) return false;
		const auto Raw=At(Field(Resolved,TEXT("schema")));Out.Default=Raw.Value[TEXT("default")];
		if (Out.Schema.Annotations.Has(TEXT("default"))) Out.Default=Out.Schema.Annotations[TEXT("default")];
		else if (!Out.Default.IsValid()) if (const int32* Index=SchemaIndices.Find(Out.Schema.Id)) Out.Default=Result.Model.Schemas[*Index].Default;
	}
	return true;
}
bool FAnalysis::Parameters(TArray<FVaRestOpenApiParameter>& Out,const FResolved& Parent)
{
	const auto List=Parent.Value[TEXT("parameters")];if (!List.IsValid()) return true;
	if (!List.IsArray()) { Issue(Field(Parent,TEXT("parameters")),TEXT("OA-PARAM-STYLE"),TEXT("Parameters must be an array."));return false; }
	bool bValid=true;
	for (int32 Index=0;Index<List.Num();++Index)
	{
		FVaRestOpenApiParameter Param;if (!Parameter({Parent.Location.Document,Child(Child(Parent.Location.Pointer,TEXT("parameters")),LexToString(Index))},Param)) { bValid=false;continue; }
		const int32 Old=Out.IndexOfByPredicate([&](const auto& Value) { return Equal(Value.Name,Param.Name) && Equal(Value.In,Param.In); });
		if (Old==INDEX_NONE) Out.Add(MoveTemp(Param));else Out[Old]=MoveTemp(Param);
	}
	return bValid;
}
void FAnalysis::Operations(const FResolved& Paths)
{
	if (!Paths.Value.IsObject()) return;
	FStringSet SeenPaths;
	for (const auto& PathMember:Paths.Value.Members())
	{
		const FString Path=PathMember.GetKey();if (SeenPaths.Contains(Path)) continue;SeenPaths.Add(Path);
		if (!Path.StartsWith(TEXT("/"))) continue;
		const auto Item=Object({Paths.Location.Document,Child(Paths.Location.Pointer,Path)});
		if (!Item.IsValid() || !Item.Value.IsObject()) { Result.Model.UnavailablePaths.Add(Path);if (Item.IsValid()) Issue(Item.Location,TEXT("OA-OP-VERB"),TEXT("A path item must be an object."));continue; }
		FStringSet SeenMethods;
		for (const auto& MethodMember:Item.Value.Members())
		{
			const FString Method=MethodMember.GetKey();if (SeenMethods.Contains(Method)) continue;SeenMethods.Add(Method);
			const FVaRestOpenApiLocation Location=Field(Item,*Method);
			static const FStringSet SkippedMethods={TEXT("options"),TEXT("trace"),TEXT("query"),TEXT("additionalOperations")};
			static const FStringSet Methods={TEXT("get"),TEXT("post"),TEXT("put"),TEXT("patch"),TEXT("delete"),TEXT("head")};
			if (SkippedMethods.Contains(Method))
			{ Issue(Location,TEXT("OA-OP-VERB"),TEXT("This operation method is not imported."),EVaRestOpenApiSeverity::Warning);continue; }
			if (!Methods.Contains(Method)) continue;
			if (Result.Model.DeclaredOperations.Num()>=Options.MaxOperations) { Issue(Location,TEXT("OA-DOC-LIMIT"),TEXT("The import exceeds MaxOperations (including invalid declarations)."));return; }
			const auto Operation=At(Location);FVaRestOpenApiOperationSource Declared{Method.ToUpper(),Path,Operation.Value[TEXT("operationId")].AsString()};Result.Model.DeclaredOperations.Add(Declared);
			bool Imported=false;ON_SCOPE_EXIT { if (!Imported) Result.Model.FailedOperations.Add(Declared); };
			if (!Operation.Value.IsObject()) { Issue(Location,TEXT("OA-OP-VERB"),TEXT("An operation must be an object."));continue; }
			FVaRestOpenApiOperation Out;Out.Method=Method.ToUpper();Out.Path=Path;Out.Location=Location;
			Out.OperationId=Operation.Value[TEXT("operationId")].AsString();Out.Summary=Operation.Value[TEXT("summary")].AsString();Out.Description=Operation.Value[TEXT("description")].AsString();
			Out.bDeprecated=Boolean(Operation.Value[TEXT("deprecated")],Field(Operation,TEXT("deprecated")));
			for (const auto& Tag:Operation.Value[TEXT("tags")].Elements()) if (Tag.IsString()) Out.Tags.Add(Tag.AsString());
			Out.Security=Operation.Value.Has(TEXT("security")) ? Operation.Value[TEXT("security")] : Result.Model.GlobalSecurity;
			Out.Servers=Operation.Value.Has(TEXT("servers")) ? Operation.Value[TEXT("servers")] : Item.Value.Has(TEXT("servers")) ? Item.Value[TEXT("servers")] : Result.Model.Servers;
			const bool bPathParameters=Parameters(Out.Parameters,Item),bOperationParameters=Parameters(Out.Parameters,Operation);
			if (!bPathParameters || !bOperationParameters) continue;
			bool bValid=true;
			if (Operation.Value.Has(TEXT("requestBody")))
			{
				const auto Body=Object(Field(Operation,TEXT("requestBody")));if (!Body.IsValid()) continue;
				if (!Body.Value.IsObject()) { Issue(Body.Location,TEXT("OA-BODY-MEDIA"),TEXT("A request body must be an object."));continue; }
				Out.bRequestBodyRequired=Boolean(Body.Value[TEXT("required")],Field(Body,TEXT("required")));Out.RequestContent=Media(Body,bValid);
			}
			const auto Responses=Operation.Value[TEXT("responses")];FStringSet SeenResponses;
			if (Responses.IsValid() && !Responses.IsObject()) { Issue(Field(Operation,TEXT("responses")),TEXT("OA-RESP-ERRORS"),TEXT("Responses must be an object."));continue; }
			for (const auto& Response:Responses.Members())
			{
				const FString Status=Response.GetKey();if (SeenResponses.Contains(Status)) continue;SeenResponses.Add(Status);
				const FVaRestOpenApiLocation Here{Location.Document,Child(Child(Location.Pointer,TEXT("responses")),Status)};
				const auto Definition=Object(Here);if (!Definition.IsValid()) { bValid=false;continue; }
				if (!Definition.Value.IsObject()) { bValid=false;Issue(Here,TEXT("OA-RESP-ERRORS"),TEXT("A response must be an object."));continue; }
				FVaRestOpenApiResponse Row;Row.Status=Status;Row.Description=Effective(Definition,TEXT("description")).AsString();Row.Location=Here;Row.Content=Media(Definition,bValid);Out.Responses.Add(MoveTemp(Row));
			}
			if (bValid) { Result.Model.Operations.Add(MoveTemp(Out));Imported=true; }if (bFatal) return;
		}
	}
}
void FAnalysis::Normalize(const TSharedRef<FDocument>& Root)
{
	const auto Document=Root->Root;FString Version;
	auto Value=Document[TEXT("openapi")];
	if (!Value.IsValid())
	{
		Issue({Root->Uri,{}},Document.Has(TEXT("swagger")) ? TEXT("OA-DOC-VERSION") : TEXT("OA-DOC-NOTOPENAPI"),TEXT("An OpenAPI 3 document is required; convert Swagger 2 with swagger2openapi."));bFatal=true;return;
	}
	if (Value.IsString()) Version=Value.AsString();
	else if (Value.IsNumber())
	{ Version=NumberSource(*Root,TEXT("/openapi"));Issue({Root->Uri,TEXT("/openapi")},TEXT("OA-DOC-VERSIONNUMBER"),TEXT("The OpenAPI version was supplied as a number; quote the version string."),EVaRestOpenApiSeverity::Warning); }
	TArray<FString> Parts;Version.ParseIntoArray(Parts,TEXT("."),false);bool Valid=Parts.Num()>=2 && Parts.Num()<=3 && Parts[0]==TEXT("3");
	for (const auto& Part:Parts) { Valid=Valid && !Part.IsEmpty();for (TCHAR Char:Part) Valid=Valid && Char>=TEXT('0') && Char<=TEXT('9'); }
	if (!Valid) { Issue({Root->Uri,TEXT("/openapi")},TEXT("OA-DOC-VERSION"),TEXT("This OpenAPI version is unsupported; convert the specification to OpenAPI 3."));bFatal=true;return; }
	FString Minor=Parts[1];while (Minor.Len()>1 && Minor[0]==TEXT('0')) Minor.RightChopInline(1,EAllowShrinking::No);
	if (Minor.Len()>1 || Minor[0]>TEXT('2')) Issue({Root->Uri,TEXT("/openapi")},TEXT("OA-DOC-NEWER"),TEXT("This document is newer than supported; it is interpreted using OpenAPI 3.1 schema semantics."),EVaRestOpenApiSeverity::Warning);
	Result.Model.Version=Version;const auto Info=Document[TEXT("info")];Result.Model.Title=Info[TEXT("title")].AsString();Result.Model.InfoVersion=Info[TEXT("version")].AsString();Result.Model.Description=Info[TEXT("description")].AsString();
	Result.Model.Servers=Document[TEXT("servers")];Result.Model.GlobalSecurity=Document[TEXT("security")];Result.Model.Tags=Document[TEXT("tags")];
	Result.Model.SecuritySchemes=FVaRestJson::Object();
	const auto SecuritySchemes=Document[TEXT("components")][TEXT("securitySchemes")];FStringSet SeenSchemes;
	for (const auto& Member:SecuritySchemes.Members())
	{
		const FString Name=Member.GetKey();if (SeenSchemes.Contains(Name)) continue;SeenSchemes.Add(Name);
		const auto Scheme=Object({Root->Uri,Child(TEXT("/components/securitySchemes"),Name)});if (!Scheme.IsValid()) continue;
		if (!Scheme.Value.IsObject()) { Issue(Scheme.Location,TEXT("OA-AUTH-UNSUPPORTED"),TEXT("A security scheme must be an object."));continue; }
		auto Definition=Scheme.Value.Clone();for (const auto& Annotation:Scheme.Annotations.Members()) Definition.Set(Annotation.GetKey(),Annotation.Value);
		Result.Model.SecuritySchemes.Set(Name,Definition);
	}
	const auto Schemas=Document[TEXT("components")][TEXT("schemas")];FStringSet Seen;
	for (const auto& Member:Schemas.Members())
	{
		const FString Name=Member.GetKey();if (Seen.Contains(Name)) continue;Seen.Add(Name);
		auto Use=Schema({Root->Uri,Child(TEXT("/components/schemas"),Name)});if (Use.IsValid()) Result.Model.NamedSchemas.Add(MoveTemp(Use));if (bFatal) return;
	}
	Operations({Document[TEXT("paths")],{}, {Root->Uri,TEXT("/paths")}});
}
}
