// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiPlanning.h"
#include "VaRestUrl.h"
#include "UObject/NameTypes.h"
#include "Misc/SecureHash.h"

namespace VaRest::OpenApi
{
namespace
{
FString BaseMedia(const FString& Name)
{
	int32 Semicolon;return (Name.FindChar(TEXT(';'),Semicolon) ? Name.Left(Semicolon) : Name).TrimStartAndEnd().ToLower();
}
bool JsonMedia(const FString& Name) { const FString Base=BaseMedia(Name);return Equal(Base,TEXT("application/json")) || Base.EndsWith(TEXT("+json"),ESearchCase::CaseSensitive); }
bool Status(const FString& Text)
{
	return Text.Len()==3 && Text[0]>=TEXT('1') && Text[0]<=TEXT('5') && Text[1]>=TEXT('0') && Text[1]<=TEXT('9') && Text[2]>=TEXT('0') && Text[2]<=TEXT('9');
}
FString GeneratedOperation(const FVaRestOpenApiOperation& Operation)
{
	FString Name=Operation.Method.ToLower();int32 Index=0;
	while (Index<Operation.Path.Len())
	{
		if (Operation.Path[Index]==TEXT('{'))
		{
			const int32 End=Operation.Path.Find(TEXT("}"),ESearchCase::CaseSensitive,ESearchDir::FromStart,Index+1);
			if (End==INDEX_NONE) break;Name+=TEXT("By")+FVaRestOpenApiNameAllocator::Pascal(Operation.Path.Mid(Index+1,End-Index-1));Index=End+1;
		}
		else
		{
			int32 End=Index;while (End<Operation.Path.Len() && Operation.Path[End]!=TEXT('/') && Operation.Path[End]!=TEXT('{')) ++End;
			if (End>Index) Name+=FVaRestOpenApiNameAllocator::Pascal(Operation.Path.Mid(Index,End-Index));Index=End==Index ? Index+1 : End;
		}
	}
	return Name;
}
FString OperationName(const FVaRestOpenApiOperation& Operation)
{
	FString Name=Operation.OperationId;
	bool Valid=!Name.IsEmpty() && Name.Len()<=128;
	for (TCHAR Char:Name) Valid=Valid && Char>=32 && Char!=127;
	Valid=Valid && FName::IsValidXName(Name,INVALID_NAME_CHARACTERS);
	if (!Valid) Name=Operation.OperationId.IsEmpty() ? GeneratedOperation(Operation) : FVaRestOpenApiNameAllocator::Pascal(Operation.OperationId);
	if (Name.Len()>128) { const FTCHARToUTF8 Utf8(*Name);uint8 Digest[20];FSHA1::HashBuffer(Utf8.Get(),Utf8.Length(),Digest);Name=Name.Left(121)+TEXT("_")+BytesToHex(Digest,20).Left(6); }
	return Name;
}
bool SamePlannedType(const FVaRestOpenApiPlannedType& A,const FVaRestOpenApiPlannedType& B)
{
	return A.Kind==B.Kind && A.Container==B.Container && A.bJsonValue==B.bJsonValue && Equal(A.DefinitionId,B.DefinitionId);
}
}
const FVaRestOpenApiMedia* FPlanner::Preferred(const TArray<FVaRestOpenApiMedia>& Content,bool bResponse) const
{
	const FVaRestOpenApiMedia* Best=nullptr;int32 BestRank=MAX_int32;
	for (const auto& Media:Content)
	{
		const FString Base=BaseMedia(Media.Name);
		const int32 Rank=JsonMedia(Media.Name) ? 0 : bResponse ? (Base.StartsWith(TEXT("text/"),ESearchCase::CaseSensitive) ? 1 : 2) :
			Equal(Base,TEXT("application/x-www-form-urlencoded")) ? 1 : Equal(Base,TEXT("multipart/form-data")) ? 2 : Base.StartsWith(TEXT("text/"),ESearchCase::CaseSensitive) ? 3 : 4;
		if (Rank<BestRank) { BestRank=Rank;Best=&Media; }
	}
	return Best;
}
FString FPlanner::Category(const FVaRestOpenApiOperation& Operation)
{
	if (Operation.Tags.IsEmpty()) return {};
	TArray<FString> Parts;FStringSet Seen;FString Current=Operation.Tags[0];
	while (!Current.IsEmpty())
	{
		if (Seen.Contains(Current) || Seen.Num()>=VaRest::Json::MaxDepth) { Issue(Operation.Location,TEXT("OA-TAG-PARENT"),TEXT("The tag parent chain is cyclic or too deep; its category was shortened."));break; }
		Seen.Add(Current);Parts.Insert(Current,0);FString Parent;
		for (const auto& Tag:Model.Tags.Elements()) if (Equal(Tag[TEXT("name")].AsString(),Current)) { Parent=Tag[TEXT("parent")].AsString();break; }
		Current=MoveTemp(Parent);
	}
	return FString::Join(Parts,TEXT("|"));
}
void FPlanner::Endpoints()
{
	TSet<FString> UsedNames;TMap<FString,int32> Suffixes;FStringSet OperationIds;
	for (const auto* Operation:Chosen)
	{
		FVaRestOpenApiPlannedEndpoint Out;Out.OperationId=Operation->OperationId;Out.Verb=Operation->Method;Out.Path=Operation->Path;Out.Location=Operation->Location;
		const bool Duplicate=!Operation->OperationId.IsEmpty() && OperationIds.Contains(Operation->OperationId);
		if (!Operation->OperationId.IsEmpty()) OperationIds.Add(Operation->OperationId);
		Out.Id=EndpointIds.FindChecked(Operation);
		const FString Base=OperationName(*Operation);Out.Name=Base;int32& Suffix=Suffixes.FindOrAdd(Base);Suffix=FMath::Max(Suffix,2);
		while (UsedNames.Contains(Out.Name)) { const FString Tail=TEXT("_")+LexToString(Suffix++);Out.Name=Base.Left(128-Tail.Len())+Tail; }
		if (Duplicate || !Equal(Out.Name,Base)) Issue(Out.Location,TEXT("OA-OP-DUPID"),TEXT("The operation's Blueprint name/identity was disambiguated from a duplicate."));UsedNames.Add(Out.Name);
		Out.Category=Category(*Operation);Out.Description=Operation->Summary+(Operation->Summary.IsEmpty() || Operation->Description.IsEmpty() ? TEXT("") : TEXT("\n"))+Operation->Description;Out.bDeprecated=Operation->bDeprecated;
		if (Operation->Servers.IsArray() && Operation->Servers.Num()>0 && !Operation->Servers.Equals(Model.Servers))
		{
			FString BaseUrl=ServerUrl(Operation->Servers.At(0),Operation->Location);
			if (!VaRest::Http::IsAbsoluteHttpUrl(BaseUrl) && !Plan.Environments.IsEmpty()) BaseUrl=ResolveHttpReference(Plan.Environments[0].BaseUrl,BaseUrl,true);
			if (!VaRest::Http::IsAbsoluteHttpUrl(BaseUrl)) { Issue(Out.Location,TEXT("OA-OP-SERVERS"),TEXT("This operation server needs an absolute host; the operation was skipped."),EVaRestOpenApiSeverity::Error);continue; }
			Out.Path=VaRest::Http::JoinUrl(BaseUrl,Out.Path);Issue(Out.Location,TEXT("OA-OP-SERVERS"),TEXT("This operation uses its own absolute server URL; API environments do not replace its host."),EVaRestOpenApiSeverity::Info);
		}
		if (!Operation->Security.IsValid() || (Operation->Security.IsArray() && Operation->Security.Num()==0)) Out.AuthMode=EVaRestOpenApiAuthMode::None;
		else
		{
			bool Anonymous=false,Satisfied=false,Composite=false;
			for (const auto& Alternative:Operation->Security.Elements()) if (Alternative.IsObject())
			{
				Anonymous=Anonymous || Alternative.Num()==0;
				if (!Plan.Auth.SchemeName.IsEmpty() && Alternative.Has(Plan.Auth.SchemeName)) { Satisfied=true;Composite=Composite || Alternative.Num()>1; }
			}
			if (Anonymous) Out.AuthMode=EVaRestOpenApiAuthMode::IfAvailable;
			else if (!Satisfied) Issue(Out.Location,TEXT("OA-AUTH-PEROP"),TEXT("This operation requires a different scheme; the API's selected auth is applied."));
			else if (Composite) Issue(Out.Location,TEXT("OA-AUTH-AND"),TEXT("This operation requires composite auth; only the API's selected scheme is applied."));
		}
		Out.RequestType.Kind=EVaRestOpenApiValueKind::None;Out.ResponseType.Kind=EVaRestOpenApiValueKind::None;Out.ErrorType.Kind=EVaRestOpenApiValueKind::None;
		TArray<FVaRestOpenApiParameter> Parameters;
		for (const auto& Parameter:Operation->Parameters)
		{
			if (Equal(Parameter.In,TEXT("cookie")) || Equal(Parameter.In,TEXT("querystring"))) { Issue(Parameter.Location,Equal(Parameter.In,TEXT("cookie")) ? TEXT("OA-PARAM-COOKIE") : TEXT("OA-PARAM-STYLE"),TEXT("This parameter location is not imported."));continue; }
			if (Equal(Parameter.In,TEXT("header")))
			{
				if (Parameter.Name.Equals(TEXT("Idempotency-Key"),ESearchCase::IgnoreCase)) { Out.bIdempotent=true;continue; }
				if (Parameter.Name.Equals(TEXT("Accept"),ESearchCase::IgnoreCase) || Parameter.Name.Equals(TEXT("Content-Type"),ESearchCase::IgnoreCase) || Parameter.Name.Equals(TEXT("Authorization"),ESearchCase::IgnoreCase)) continue;
			}
			Parameters.Add(Parameter);
		}
		const auto PinNames=FVaRestOpenApiNameAllocator::Parameters(Parameters);
		for (int32 Index=0;Index<Parameters.Num();++Index)
		{
			const auto& Input=Parameters[Index];FVaRestOpenApiPlannedParameter Param;
			Param.Name=Input.Name;Param.PinName=PinNames[Index].PinName;Param.In=Input.In;Param.Location=Input.Location;Param.Default=Input.Default;Param.Description=Input.Description+(Input.bDeprecated ? TEXT(" [Deprecated]") : TEXT(""));Param.bRequired=Input.bRequired;
			Param.Id=SemanticId(Out.Id,*Input.In,Input.Name);
			if (Input.Content.IsValid() || !Input.Schema.IsValid())
			{ Param.Type.Kind=EVaRestOpenApiValueKind::String;Issue(Input.Location,TEXT("OA-PARAM-CONTENT"),TEXT("A content/untyped parameter uses a String pin.")); }
			else Param.Type=Type(Input.Schema,Param.Id,FVaRestOpenApiNameAllocator::Pascal(Out.Name)+FVaRestOpenApiNameAllocator::Pascal(Input.Name)).Type;
			if (Param.Type.Kind==EVaRestOpenApiValueKind::Bytes || Param.Type.Kind==EVaRestOpenApiValueKind::DateTime) { Param.Type.Kind=EVaRestOpenApiValueKind::String;Param.Type.Container=EVaRestOpenApiContainer::None; }
			if (Param.Type.Kind==EVaRestOpenApiValueKind::Struct || Param.Type.Kind==EVaRestOpenApiValueKind::Json || Param.Type.Container==EVaRestOpenApiContainer::Map || Equal(Input.Style,TEXT("deepObject")))
			{
				Issue(Input.Location,TEXT("OA-PARAM-STYLE"),TEXT("Object/deep parameter serialization needs an explicit String value or the Query map."));
				if (Equal(Input.In,TEXT("query"))) continue;
				Param.Type={};Param.Type.Kind=EVaRestOpenApiValueKind::String;
			}
			if (Param.Type.Container==EVaRestOpenApiContainer::Array)
			{
				if (!Equal(Input.In,TEXT("query"))) Param.Style=EVaRestOpenApiQueryStyle::Csv;
				else if (Equal(Input.Style,TEXT("form"))) Param.Style=Input.bExplode ? EVaRestOpenApiQueryStyle::Repeat : EVaRestOpenApiQueryStyle::Csv;
				else if (Equal(Input.Style,TEXT("spaceDelimited"))) Param.Style=EVaRestOpenApiQueryStyle::Ssv;
				else if (Equal(Input.Style,TEXT("pipeDelimited"))) Param.Style=EVaRestOpenApiQueryStyle::Pipes;
				else { Issue(Input.Location,TEXT("OA-PARAM-STYLE"),TEXT("This array query style is unsupported; use the Query map."));continue; }
			}
			Out.Parameters.Add(MoveTemp(Param));
		}
		if (const auto* Media=Preferred(Operation->RequestContent,false))
		{
			Out.bHasBody=true;Out.RequestMedia=Media->Name;Out.BodyContentType=Media->Name;const FString BaseType=BaseMedia(Media->Name);
			if (JsonMedia(Media->Name))
			{
				Out.BodyKind=EVaRestOpenApiBodyKind::Json;Out.RequestType=Media->Schema.IsValid() ? Type(Media->Schema,SemanticId(Out.Id,TEXT("request")),FVaRestOpenApiNameAllocator::Pascal(Out.Name)+TEXT("Request")).Type : FVaRestOpenApiPlannedType();
				if (Out.RequestType.Container==EVaRestOpenApiContainer::None && (Out.RequestType.Kind==EVaRestOpenApiValueKind::String || Out.RequestType.Kind==EVaRestOpenApiValueKind::Bytes)) Out.RequestType.bJsonValue=true;
			}
			else if (Equal(BaseType,TEXT("application/x-www-form-urlencoded")))
			{
				Out.BodyKind=EVaRestOpenApiBodyKind::Form;Out.RequestType=Media->Schema.IsValid() ? Type(Media->Schema,SemanticId(Out.Id,TEXT("request")),FVaRestOpenApiNameAllocator::Pascal(Out.Name)+TEXT("Request")).Type : FVaRestOpenApiPlannedType();
				bool Nested=Out.RequestType.Kind!=EVaRestOpenApiValueKind::Struct;
				for (const auto& Row:Plan.Structs) if (Equal(Row.Id,Out.RequestType.DefinitionId)) for (const auto& Field:Row.Fields)
					Nested=Nested || Field.Type.Kind==EVaRestOpenApiValueKind::Struct || Field.Type.Kind==EVaRestOpenApiValueKind::Json || Field.Type.Container==EVaRestOpenApiContainer::Map;
				if (Nested) { Out.RequestType={};Issue(Media->Location,TEXT("OA-BODY-FORMNESTED"),TEXT("This form uses a Json input; supply a flat object with scalar/repeated scalar values.")); }
			}
			else if (Equal(BaseType,TEXT("multipart/form-data"))) { Out.BodyKind=EVaRestOpenApiBodyKind::Multipart;Out.BodyContentType.Reset(); }
			else if (BaseType.StartsWith(TEXT("text/"),ESearchCase::CaseSensitive)) { Out.BodyKind=EVaRestOpenApiBodyKind::Text;Out.RequestType.Kind=EVaRestOpenApiValueKind::String; }
			else { Out.BodyKind=EVaRestOpenApiBodyKind::Binary;Out.RequestType.Kind=EVaRestOpenApiValueKind::Bytes;if (!Equal(BaseType,TEXT("application/octet-stream")) && !BaseType.StartsWith(TEXT("image/"),ESearchCase::CaseSensitive)) Issue(Media->Location,TEXT("OA-BODY-MEDIA"),TEXT("This media uses an explicit raw byte body.")); }
		}
		const FVaRestOpenApiResponse* Success=nullptr;int32 BestRank=MAX_int32;bool HasStatus=false;
		for (const auto& Response:Operation->Responses)
		{
			HasStatus=HasStatus || !Equal(Response.Status,TEXT("default"));int32 Rank=MAX_int32;
			if (Status(Response.Status) && Response.Status[0]==TEXT('2')) Rank=Equal(Response.Status,TEXT("200")) ? 0 : Equal(Response.Status,TEXT("201")) ? 1 : FCString::Atoi(*Response.Status);
			else if (Equal(Response.Status,TEXT("2XX"))) Rank=1000;
			if (Rank<BestRank) { BestRank=Rank;Success=&Response; }
		}
		if (!Success && !HasStatus) for (const auto& Response:Operation->Responses) if (Equal(Response.Status,TEXT("default"))) { Success=&Response;break; }
		const auto* SuccessBody=Success ? Preferred(Success->Content,true) : nullptr;
		const auto ResponseType=[&](const FVaRestOpenApiMedia* Media,const FString& Role)->FVaRestOpenApiPlannedType {
			FVaRestOpenApiPlannedType TypeValue;TypeValue.Kind=EVaRestOpenApiValueKind::None;if (!Media) return TypeValue;
			if (JsonMedia(Media->Name)) { TypeValue=Media->Schema.IsValid() ? Type(Media->Schema,SemanticId(Out.Id,*Role),FVaRestOpenApiNameAllocator::Pascal(Out.Name)+(Equal(Role,TEXT("response")) ? TEXT("Response") : TEXT("Error"))).Type : FVaRestOpenApiPlannedType();
				if (TypeValue.Container==EVaRestOpenApiContainer::None && (TypeValue.Kind==EVaRestOpenApiValueKind::String || TypeValue.Kind==EVaRestOpenApiValueKind::Bytes)) TypeValue.bJsonValue=true; }
			else TypeValue.Kind=BaseMedia(Media->Name).StartsWith(TEXT("text/"),ESearchCase::CaseSensitive) ? EVaRestOpenApiValueKind::String : EVaRestOpenApiValueKind::Bytes;
			return TypeValue;
		};
		Out.ResponseType=ResponseType(SuccessBody,TEXT("response"));
		bool NonJsonSuccess=SuccessBody && !JsonMedia(SuccessBody->Name);
		for (const auto& Response:Operation->Responses) if ((Status(Response.Status) && Response.Status[0]==TEXT('2')) || Equal(Response.Status,TEXT("2XX")))
			if (const auto* Body=Preferred(Response.Content,true)) NonJsonSuccess=NonJsonSuccess || !JsonMedia(Body->Name);
		for (const auto& Response:Operation->Responses) if ((Status(Response.Status) && Response.Status[0]==TEXT('2')) || Equal(Response.Status,TEXT("2XX")))
		{
			const auto* Body=Preferred(Response.Content,true);
			if (!SamePlannedType(Out.ResponseType,ResponseType(Body,TEXT("response"))) || (SuccessBody && Body && JsonMedia(SuccessBody->Name) && JsonMedia(Body->Name) && !Equal(Shape(SuccessBody->Schema),Shape(Body->Schema))))
			{
				Out.ResponseType={};Out.bMixedSuccessShapes=true;
				if (NonJsonSuccess) Out.ResponseType.Kind=EVaRestOpenApiValueKind::Bytes;
				Issue(Out.Location,TEXT("OA-RESP-MULTI2XX"),Out.ResponseType.Kind==EVaRestOpenApiValueKind::Bytes ? TEXT("Success responses mix JSON/text/binary media; the output retains raw bytes.") : TEXT("Success responses have different shapes; the output is Json."));break;
			}
		}
		const FVaRestOpenApiMedia* ErrorBody=nullptr;FString ErrorShape;bool SameErrors=true;
		for (const auto& Response:Operation->Responses) if (Equal(Response.Status,TEXT("default")) || Equal(Response.Status,TEXT("4XX")) || Equal(Response.Status,TEXT("5XX")) ||
			(Status(Response.Status) && (Response.Status[0]==TEXT('4') || Response.Status[0]==TEXT('5'))))
		{
			const auto* Body=Preferred(Response.Content,true);if (!Body || !JsonMedia(Body->Name) || !Body->Schema.IsValid()) { SameErrors=false;continue; }
			ErrorSchemas.Add(Body->Schema);const FString CurrentShape=Shape(Body->Schema);
			if (!ErrorBody) { ErrorBody=Body;ErrorShape=CurrentShape; }else if (!Equal(ErrorBody->Schema.Id,Body->Schema.Id) && !Equal(ErrorShape,CurrentShape)) SameErrors=false;
		}
		if (ErrorBody && SameErrors) Out.ErrorType=ResponseType(ErrorBody,TEXT("error"));
		else if (ErrorBody) Issue(Out.Location,TEXT("OA-RESP-ERRORS"),TEXT("There is no single JSON error shape; raw error metadata/body remains available."),EVaRestOpenApiSeverity::Info);
		Plan.Endpoints.Add(MoveTemp(Out));SuccessResponses.Add(Success);SuccessMedia.Add(SuccessBody);
	}
}
}
