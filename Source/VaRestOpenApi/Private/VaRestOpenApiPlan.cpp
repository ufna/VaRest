// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlan.h"
#include "VaRestOpenApiPlanning.h"

namespace VaRest::OpenApi
{
namespace
{
TArray<FString> ReservedTypeNames(const FVaRestOpenApiPlanOptions& Options)
{
	auto Names=Options.ExistingTypeNames;for (const auto& Definition:Options.ExistingDefinitions) Names.Add(Definition.Value);return Names;
}
FString SeparateDefinitionId(const FString& Id,bool bEnum)
{
	return SemanticId(Id,TEXT("definition"),bEnum ? TEXT("enum") : TEXT("struct"));
}
bool SeparateInlineDefinitions(FVaRestOpenApiPlan& Plan)
{
	// An inline declaration used to inherit its owning field/parameter ID. All merge
	// maps require globally unique item IDs, including across these different kinds.
	// Finalize only actual collisions: existing noncolliding identities stay exact.
	FStringSet Items;bool Unique=true;
	const auto Reserve=[&](const FString& Id) { if (Items.Contains(Id)) Unique=false;Items.Add(Id); };
	for (const auto& Struct:Plan.Structs) for (const auto& Field:Struct.Fields) Reserve(Field.Id);
	for (const auto& Endpoint:Plan.Endpoints) { Reserve(Endpoint.Id);for (const auto& Param:Endpoint.Parameters) Reserve(Param.Id); }
	for (const auto& Environment:Plan.Environments) Reserve(Environment.Id);
	for (const TCHAR* Id:{TEXT("auth"),TEXT("envelope"),TEXT("defaults")}) Reserve(Id);
	if (!Unique) return false;
	const FStringSet Owned=Items;FStringSet Definitions;
	for (const auto& Struct:Plan.Structs) { if (Definitions.Contains(Struct.Id)) return false;Definitions.Add(Struct.Id);Items.Add(Struct.Id); }
	for (const auto& Enum:Plan.Enums) { if (Definitions.Contains(Enum.Id)) return false;Definitions.Add(Enum.Id);Items.Add(Enum.Id); }
	TStringMap<FString> Remapped;
	const auto Remap=[&](const FString& Id,bool bEnum)
	{
		if (!Owned.Contains(Id)) return true;
		const FString NewId=SeparateDefinitionId(Id,bEnum);
		if (Items.Contains(NewId)) return false;
		Items.Add(NewId);Remapped.Add(Id,NewId);return true;
	};
	for (const auto& Struct:Plan.Structs) if (!Remap(Struct.Id,false)) return false;
	for (const auto& Enum:Plan.Enums) if (!Remap(Enum.Id,true)) return false;
	const auto FixType=[&](FVaRestOpenApiPlannedType& Type) { if (const auto* Id=Remapped.Find(Type.DefinitionId)) Type.DefinitionId=*Id; };
	for (auto& Struct:Plan.Structs)
	{
		if (const auto* Id=Remapped.Find(Struct.Id)) Struct.Id=*Id;
		for (auto& Field:Struct.Fields) FixType(Field.Type);
	}
	for (auto& Enum:Plan.Enums) if (const auto* Id=Remapped.Find(Enum.Id)) Enum.Id=*Id;
	for (auto& Endpoint:Plan.Endpoints)
	{
		FixType(Endpoint.RequestType);FixType(Endpoint.ResponseType);FixType(Endpoint.ErrorType);
		for (auto& Param:Endpoint.Parameters) FixType(Param.Type);
	}
	return true;
}
}
FPlanner::FPlanner(const FVaRestOpenApiAnalysis& Analysis,const FVaRestOpenApiPlanOptions& InOptions)
	: Model(Analysis.Model),Options(InOptions),Names(ReservedTypeNames(InOptions))
{
	Plan.bValid=Analysis.bDocumentValid;Plan.Issues=Analysis.Issues;
	for (const auto& Node:Model.Schemas) if (!Node.Id.IsEmpty())
	{
		Schemas.Add(Node.Id,&Node);int32 Slash;
		if (Node.Location.Pointer.FindLastChar(TEXT('/'),Slash))
		{
			const FString Parent=Node.Location.Pointer.Left(Slash);
			if (Parent.EndsWith(TEXT("/schemas"),ESearchCase::CaseSensitive) || Parent.EndsWith(TEXT("/$defs"),ESearchCase::CaseSensitive)) Named.Add(Node.Id);
		}
	}
	for (const auto& Use:Model.NamedSchemas) Named.Add(Use.Id);
	const auto AddVariant=[&](const FString& Base,const FString& Variant) {
		auto& List=Variants.FindOrAdd(Base);
		if (!List.ContainsByPredicate([&](const FString& Existing) { return Equal(Existing,Variant); })) List.Add(Variant);
		Polymorphic.Add(Base);
	};
	for (const auto& Node:Model.Schemas)
	{
		for (const auto& Parent:Node.AllOf) if (const auto* Base=Schema(Parent.Id))
			if (Base->Discriminator.IsObject()) AddVariant(Parent.Id,Node.Id);
		for (const auto& Mapping:Node.DiscriminatorMappings) AddVariant(Node.Id,Mapping.Value.Id);
	}
	FStringSet UsedEndpointIds,OperationIds;
	for (const auto& Operation:Model.Operations)
	{
		FString Id;
		// Exact old operation+route first, then operationId, then route (operationId renamed upstream).
		for (int32 Pass=0;Pass<3 && Id.IsEmpty();++Pass) for (const auto& Existing:Options.ExistingEndpoints)
		{
			if (UsedEndpointIds.Contains(Existing.Id)) continue;
			const bool SameOperation=!Operation.OperationId.IsEmpty() && Equal(Existing.OperationId,Operation.OperationId);
			const bool SameRoute=Equal(Existing.Verb,Operation.Method) && Equal(Existing.Path,Operation.Path);
			if ((Pass==0 && SameOperation && SameRoute) || (Pass==1 && SameOperation) || (Pass==2 && SameRoute)) { Id=Existing.Id;break; }
		}
		const bool Duplicate=!Operation.OperationId.IsEmpty() && OperationIds.Contains(Operation.OperationId);
		if (Id.IsEmpty()) Id=SemanticId(TEXT("endpoint"),Operation.OperationId.IsEmpty() || Duplicate ? TEXT("route") : TEXT("operation"),Operation.OperationId.IsEmpty() || Duplicate ? Operation.Method+TEXT(" ")+Operation.Path : Operation.OperationId);
		if (UsedEndpointIds.Contains(Id)) Id=SemanticId(Id,TEXT("route"),Operation.Method+TEXT(" ")+Operation.Path);
		UsedEndpointIds.Add(Id);OperationIds.Add(Operation.OperationId);EndpointIds.Add(&Operation,Id);
		if (Selected(Operation)) Chosen.Add(&Operation);
	}
	for (const auto& Existing:Options.ExistingEndpoints)
	{
		if (UsedEndpointIds.Contains(Existing.Id)) continue;
		const bool Unavailable=Model.UnavailablePaths.ContainsByPredicate([&](const FString& Path) { return Equal(Path,Existing.Path); }) || Model.FailedOperations.ContainsByPredicate([&](const FVaRestOpenApiOperationSource& Failed)
		{ return (Equal(Failed.Method,Existing.Verb) && Equal(Failed.Path,Existing.Path)) || (!Failed.OperationId.IsEmpty() && Equal(Failed.OperationId,Existing.OperationId)); });
		if (Unavailable) Plan.UnavailableEndpointIds.Add(Existing.Id);
	}
}
FString FPlanner::AllocateTypeName(const FString& Id,const FString& Hint,bool bEnum)
{
	for (const auto& Existing:Options.ExistingDefinitions) if (Equal(Existing.Key,Id)) return Existing.Value;
	// Planning still traverses the original containment IDs. A previously finalized
	// colliding declaration must retain its real asset name on the next reimport.
	const FString Separated=SeparateDefinitionId(Id,bEnum);
	for (const auto& Existing:Options.ExistingDefinitions) if (Equal(Existing.Key,Separated)) return Existing.Value;
	// Query the exact same sanitizer with an empty allocator; do not consume the real allocation twice.
	FVaRestOpenApiNameAllocator Baseline;const FString Candidate=Baseline.AllocateType(Hint,bEnum,Options.TypePrefix);
	const FString Allocated=Names.AllocateType(Hint,bEnum,Options.TypePrefix);
	FVaRestOpenApiLocation Location{Model.RootDocument,{}};
	const auto* Source=DefinitionSources.Find(Id);if (const auto* Node=Schema(Source ? *Source : Id)) Location=Node->Location;
	if (!Equal(Candidate,Allocated)) Issue(Location,TEXT("OA-NAME-COLLISION"),FString::Printf(TEXT("Type name '%s' is already reserved; allocated '%s' in source order."),*Candidate,*Allocated),EVaRestOpenApiSeverity::Info);
	if (Options.ExistingTypeNames.ContainsByPredicate([&](const FString& Existing) { return Existing.Equals(Candidate,ESearchCase::IgnoreCase); }))
		Issue(Location,TEXT("OA-NAME-SHADOW"),FString::Printf(TEXT("A project type already uses '%s'; this plan generates '%s'. Choose a Type prefix to distinguish generated types in type pickers."),*Candidate,*Allocated));
	return Allocated;
}
void FPlanner::Issue(const FVaRestOpenApiLocation& Location,const TCHAR* Code,FString Message,EVaRestOpenApiSeverity Severity)
{
	FVaRestOpenApiIssue Row;Row.Document=Location.Document;Row.Pointer=Location.Pointer;Row.Code=Code;Row.Message=MoveTemp(Message);Row.Severity=Severity;Plan.Issues.Add(MoveTemp(Row));
	if (FCString::Strcmp(Code,TEXT("OA-DOC-LIMIT"))==0) Plan.bValid=false;
}
bool FPlanner::Selected(const FVaRestOpenApiOperation& Operation) const
{
	if (!Options.bSelectAllOperations && Options.SelectedOperations.IsEmpty() && Options.SelectedTags.IsEmpty()) return false;
	if (!Options.SelectedOperations.IsEmpty() && !Options.SelectedOperations.ContainsByPredicate([&](const FString& Name) {
		return Equal(Name,Operation.OperationId) || Equal(Name,Operation.Method+TEXT(" ")+Operation.Path);
	})) return false;
	if (Options.SelectedTags.IsEmpty()) return true;
	for (const auto& Tag:Operation.Tags) if (Options.SelectedTags.ContainsByPredicate([&](const FString& SelectedTag) { return Equal(Tag,SelectedTag); })) return true;
	return false;
}
FVaRestOpenApiPlan FPlanner::Run()
{
	if (!Plan.bValid) return MoveTemp(Plan);
	struct FReach { FVaRestOpenApiSchemaUse Use;FString Owner,Hint; };
	TArray<FReach> Work;
	if (Options.bGenerateUnreferencedSchemas) for (const auto& Use:Model.NamedSchemas) if (const auto* Node=Schema(Use.Id)) Work.Add({Use,Use.Id,TypeHint(*Node,Options.ApiName)});
	for (const auto* Operation:Chosen)
	{
		const FString Endpoint=EndpointIds.FindChecked(Operation);
		const FString Hint=FVaRestOpenApiNameAllocator::Pascal(Operation->OperationId.IsEmpty() ? Operation->Method+TEXT(" ")+Operation->Path : Operation->OperationId);
		for (const auto& Parameter:Operation->Parameters) if (Parameter.Schema.IsValid()) Work.Add({Parameter.Schema,SemanticId(Endpoint,*Parameter.In,Parameter.Name),Hint+FVaRestOpenApiNameAllocator::Pascal(Parameter.Name)});
		for (const auto& Media:Operation->RequestContent) if (Media.Schema.IsValid()) Work.Add({Media.Schema,SemanticId(Endpoint,TEXT("request")),Hint+TEXT("Request")});
		for (const auto& Response:Operation->Responses)
		{
			const bool Success=Response.Status.Len()==3 && Response.Status[0]==TEXT('2');
			for (const auto& Media:Response.Content) if (Media.Schema.IsValid()) Work.Add({Media.Schema,SemanticId(Endpoint,Success ? TEXT("response") : TEXT("error")),Hint+(Success ? TEXT("Response") : TEXT("Error"))});
		}
	}
	FStringSet Reachable;
	for (int32 Index=0;Index<Work.Num();++Index)
	{
		const FReach Current=Work[Index];const auto* Node=Schema(Current.Use.Id);if (!Node || Reachable.Contains(Node->Id)) continue;
		Reachable.Add(Node->Id);const FString Id=TypeId(Node->Id,Current.Owner),NameHint=TypeHint(*Node,Current.Hint);
		for (const auto& Property:Node->Properties) Work.Add({Property.Schema,SemanticId(Id,TEXT("field"),Property.Name),NameHint+FVaRestOpenApiNameAllocator::Pascal(Property.Name)});
		if (Node->Items.IsValid()) Work.Add({Node->Items,SemanticId(Id,TEXT("items")),NameHint+TEXT("Item")});
		if (Node->AdditionalProperties.IsValid()) Work.Add({Node->AdditionalProperties,SemanticId(Id,TEXT("values")),NameHint+TEXT("Value")});
		for (int32 Part=0;Part<Node->AllOf.Num();++Part) Work.Add({Node->AllOf[Part],SemanticId(Id,TEXT("allOf"),LexToString(Part+1)),NameHint+TEXT("Part")+LexToString(Part+1)});
		const auto& Union=Node->OneOf.IsEmpty() ? Node->AnyOf : Node->OneOf;
		for (int32 Part=0;Part<Union.Num();++Part) Work.Add({Union[Part],SemanticId(Id,TEXT("option"),LexToString(Part+1)),NameHint+TEXT("Option")+LexToString(Part+1)});
		if (const auto* Children=Variants.Find(Node->Id)) for (const auto& ChildId:*Children) if (const auto* ChildNode=Schema(ChildId))
			Work.Add({{ChildId,ChildNode->Location,{}},SemanticId(Id,TEXT("variant"),ChildId),TypeHint(*ChildNode,Current.Hint+TEXT("Variant"))});
	}
	TStringMap<FString> Hints;
	for (const auto& Item:Work) if (!Hints.Contains(Item.Use.Id)) Hints.Add(Item.Use.Id,Item.Hint);
	// Source-order roots plus field-order DFS choose containment back-edges deterministically.
	for (const auto& Node:Model.Schemas) if (Reachable.Contains(Node.Id))
	{
		const FString* Hint=Hints.Find(Node.Id);Type({Node.Id,Node.Location,{}},SemanticIds.FindRef(Node.Id),Hint ? *Hint : Options.ApiName,false,true);
		if (!Plan.bValid) return MoveTemp(Plan);
	}
	Environments();Authentication();Endpoints();
	DetectEnvelope(); // error-path detection also runs when automatic data unwrapping is disabled
	Defaults();
	if (!SeparateInlineDefinitions(Plan))
	{
		Issue({Model.RootDocument,{}},TEXT("OA-PLAN-IDENTITY"),TEXT("The selected declarations have conflicting item identities; no assets can be applied."),EVaRestOpenApiSeverity::Error);
		Plan.bValid=false;
	}
	if (Plan.Structs.Num()+Plan.Enums.Num()>300) Issue({Model.RootDocument,{}},TEXT("OA-SCALE"),TEXT("More than 300 types are selected; reduce the tag/operation selection if editor apply cost is excessive."),EVaRestOpenApiSeverity::Info);
	return MoveTemp(Plan);
}
}
FVaRestOpenApiPlan FVaRestOpenApiPlanner::Build(const FVaRestOpenApiAnalysis& Analysis,const FVaRestOpenApiPlanOptions& Options)
{
	auto Plan=VaRest::OpenApi::FPlanner(Analysis,Options).Run();FVaRestOpenApiLoader::LocateIssues(Analysis,Plan.Issues);return Plan;
}
