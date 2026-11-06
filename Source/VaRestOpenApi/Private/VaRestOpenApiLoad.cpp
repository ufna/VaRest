// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiModel.h"
#include "VaRestOpenApiAnalysis.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace VaRest::OpenApi
{
FString Child(const FString& Pointer,const FString& Key)
{
	return Pointer+TEXT("/")+Key.Replace(TEXT("~"),TEXT("~0"),ESearchCase::CaseSensitive).Replace(TEXT("/"),TEXT("~1"),ESearchCase::CaseSensitive);
}
namespace
{
bool Decode(const FString& Text,FString& Out)
{
	const FTCHARToUTF8 Utf8(*Text,Text.Len()); TArray<uint8> Bytes;
	for (int32 Index=0;Index<Utf8.Length();++Index)
	{
		uint8 Value=static_cast<uint8>(Utf8.Get()[Index]);
		if (Value=='%')
		{
			if (Index+2>=Utf8.Length()) return false;
			const auto Hex=[](uint8 Char) { return Char>='0' && Char<='9' ? Char-'0' : Char>='a' && Char<='f' ? Char-'a'+10 : Char>='A' && Char<='F' ? Char-'A'+10 : -1; };
			const int32 A=Hex(Utf8.Get()[Index+1]),B=Hex(Utf8.Get()[Index+2]); if (A<0 || B<0) return false;
			Value=static_cast<uint8>((A<<4)|B); Index+=2;
		}
		if (!Value) return false;
		Bytes.Add(Value);
	}
	if (Bytes.IsEmpty()) { Out.Reset();return true; }
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
	Out=FString(Converted.Length(),Converted.Get());const FTCHARToUTF8 RoundTrip(*Out);
	return RoundTrip.Length()==Bytes.Num() && FMemory::Memcmp(RoundTrip.Get(),Bytes.GetData(),Bytes.Num())==0;
}
bool CleanPath(const FString& Raw,FString& Out,bool bPercentDecode=true)
{
	FString Decoded=Raw;if ((bPercentDecode && !Decode(Raw,Decoded)) || Decoded.Contains(TEXT("\\"))) return false;
	TArray<FString> Parts,Stack; Decoded.ParseIntoArray(Parts,TEXT("/"),false);
	for (const FString& Part:Parts)
	{
		for (TCHAR Char:Part) if (Char<32 || Char==127) return false;
		if (Part==TEXT("..")) { if (Stack.IsEmpty()) return false;Stack.Pop(EAllowShrinking::No); }
		else if (!Part.IsEmpty() && Part!=TEXT(".")) Stack.Add(Part);
	}
	Out=TEXT("/")+FString::Join(Stack,TEXT("/"));return true;
}
FString EncodePath(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);FString Out;constexpr TCHAR Hex[]=TEXT("0123456789ABCDEF");
	for (int32 Index=0;Index<Utf8.Length();++Index)
	{
		const uint8 Byte=Utf8.Get()[Index];
		if ((Byte>='a' && Byte<='z') || (Byte>='A' && Byte<='Z') || (Byte>='0' && Byte<='9') || Byte=='-' || Byte=='_' || Byte=='.' || Byte=='~' || Byte=='/') Out.AppendChar(Byte);
		else { Out.AppendChar(TEXT('%'));Out.AppendChar(Hex[Byte>>4]);Out.AppendChar(Hex[Byte&15]); }
	}
	return Out;
}
bool UrlPath(const FString& Raw,FString& Out)
{
	// Do not turn an encoded segment separator into a different HTTP resource.
	FString Decoded;
	if (Raw.Contains(TEXT("%2f"),ESearchCase::IgnoreCase) || Raw.Contains(TEXT("%5c"),ESearchCase::IgnoreCase) || !Decode(Raw,Decoded) || !CleanPath(Decoded,Out,false)) return false;
	if ((Decoded.EndsWith(TEXT("/")) || Decoded.EndsWith(TEXT("/.")) || Decoded.EndsWith(TEXT("/.."))) && Out!=TEXT("/")) Out+=TEXT("/");
	Out=EncodePath(Out);return true;
}
bool FilePath(const FString& Raw,FString& Out)
{
	FString Standard=Raw.Replace(TEXT("\\"),TEXT("/"),ESearchCase::CaseSensitive),Drive;
	if (Standard.Len()>=3 && FChar::IsAlpha(Standard[0]) && Standard[1]==TEXT(':') && Standard[2]==TEXT('/'))
	{ Drive=Standard.Left(2).ToUpper();Standard=Standard.Mid(2); }
	if (!CleanPath(Standard,Out,false)) return false;
	Out=Drive+Out;return true;
}
bool ValidUtf8(TConstArrayView<uint8> Bytes)
{
	for (int32 Index=0;Index<Bytes.Num();)
	{
		const uint8 Lead=Bytes[Index++];if (Lead<0x80) continue;
		const int32 Count=Lead>=0xc2 && Lead<=0xdf ? 1 : Lead>=0xe0 && Lead<=0xef ? 2 : Lead>=0xf0 && Lead<=0xf4 ? 3 : -1;
		if (Count<0 || Bytes.Num()-Index<Count) return false;
		const uint8 Next=Bytes[Index];
		if ((Lead==0xe0 && Next<0xa0) || (Lead==0xed && Next>=0xa0) || (Lead==0xf0 && Next<0x90) || (Lead==0xf4 && Next>=0x90)) return false;
		for (int32 Part=0;Part<Count;++Part) if ((Bytes[Index++]&0xc0)!=0x80) return false;
	}
	return true;
}
bool Url(const FString& Raw,FString& Origin,FString& Path,FString& Query)
{
	int32 SchemeEnd;if (!Raw.FindChar(TEXT(':'),SchemeEnd)) return false;
	const FString Scheme=Raw.Left(SchemeEnd).ToLower();
	if ((Scheme!=TEXT("http") && Scheme!=TEXT("https")) || Raw.Mid(SchemeEnd,3)!=TEXT("://")) return false;
	int32 End=SchemeEnd+3;while (End<Raw.Len() && Raw[End]!=TEXT('/') && Raw[End]!=TEXT('?') && Raw[End]!=TEXT('#')) ++End;
	FString Authority=Raw.Mid(SchemeEnd+3,End-SchemeEnd-3).ToLower();if (Authority.IsEmpty()) return false;
	for (TCHAR Char:Authority) if (Char<=32 || Char==127 || Char==TEXT('@') || Char==TEXT('\\') || Char==TEXT('%')) return false;
	if ((Scheme==TEXT("http") && Authority.EndsWith(TEXT(":80"))) || (Scheme==TEXT("https") && Authority.EndsWith(TEXT(":443")))) Authority=Authority.Left(Authority.Find(TEXT(":"),ESearchCase::CaseSensitive,ESearchDir::FromEnd));
	if (Authority.IsEmpty()) return false;
	Origin=Scheme+TEXT("://")+Authority;
	FString Tail=Raw.Mid(End);int32 Mark;
	if (Tail.FindChar(TEXT('#'),Mark)) return false;
	Query.Reset();if (Tail.FindChar(TEXT('?'),Mark)) { Query=Tail.Mid(Mark);Tail=Tail.Left(Mark); }
	for (TCHAR Char:Query) if (Char<32 || Char==127) return false;
	return UrlPath(Tail.IsEmpty() ? TEXT("/") : Tail,Path);
}
FString AnnotationsKey(const FString& Key)
{
	static const FStringSet Keys={TEXT("title"),TEXT("description"),TEXT("default"),TEXT("example"),TEXT("examples"),TEXT("readOnly"),TEXT("writeOnly"),TEXT("deprecated"),TEXT("nullable"),TEXT("summary")};
	return Keys.Contains(Key) ? Key : FString();
}
}

bool SplitSourceUrl(const FString& Raw,FString& Origin,FString& Path,FString& Query) { return Url(Raw,Origin,Path,Query); }

FAnalysis::FAnalysis(FString InRoot,const FVaRestOpenApiLoadOptions& InOptions) : Options(InOptions),RootUri(MoveTemp(InRoot))
{
	FString Path,Query;
	if (Url(RootUri,RootOrigin,Path,Query)) { bUrl=true;RootUri=RootOrigin+Path+Query;RootBase=RootOrigin+Path.Left(Path.Find(TEXT("/"),ESearchCase::CaseSensitive,ESearchDir::FromEnd)+1); }
	else if (!FPaths::IsRelative(RootUri) && !RootUri.Contains(TEXT("://")) && FilePath(RootUri,Path)) { RootUri=Path;RootBase=FPaths::GetPath(RootUri)+TEXT("/"); }
	else bFatal=true;
	Result.Model.RootDocument=RootUri;
}
void FAnalysis::Issue(const FVaRestOpenApiLocation& Location,const TCHAR* Code,FString Message,EVaRestOpenApiSeverity Severity)
{
	FVaRestOpenApiIssue Row;Row.Severity=Severity;Row.Code=Code;Row.Message=MoveTemp(Message);Row.Document=Location.Document;Row.Pointer=Location.Pointer;
	Result.Issues.Add(MoveTemp(Row));
	if (FCString::Strcmp(Code,TEXT("OA-DOC-LIMIT"))==0) bFatal=true;
}
FVaRestOpenApiAnalysis FAnalysis::Run(TConstArrayView<uint8> Bytes)
{
	if (bFatal) { Issue({RootUri,{}},TEXT("OA-DOC-ENCODING"),TEXT("The source must have an absolute file path or a valid HTTP(S) source identity."));return MoveTemp(Result); }
	if (Options.MaxDocumentBytes<=0 || Options.MaxExternalDocuments<0 || Options.MaxSchemas<=0 || Options.MaxOperations<=0)
	{ Issue({RootUri,{}},TEXT("OA-DOC-LIMIT"),TEXT("Import limits must be positive (the external-document limit may be zero)."));return MoveTemp(Result); }
	if (const auto Root=Load(RootUri,Bytes)) Normalize(Root.ToSharedRef());else bFatal=true;
	if (!Options.bRetainSourceText || bFatal) LocateIssues();
	if (Options.bRetainSourceText && !bFatal) for (const auto& Uri:Result.Model.SourceDocuments) if (auto* Doc=Documents.Find(Uri)) Result.SourceText.Add({Uri,MoveTemp((*Doc)->Bytes)});
	Result.bDocumentValid=!bFatal;return MoveTemp(Result);
}
TSharedPtr<FDocument> FAnalysis::Load(const FString& Uri,TConstArrayView<uint8> Bytes)
{
	if (Bytes.Num()>Options.MaxDocumentBytes) { Issue({Uri,{}},TEXT("OA-DOC-LIMIT"),TEXT("The document exceeds MaxDocumentBytes."));return nullptr; }
	int32 Start=0;
	if (Bytes.Num()>=3 && Bytes[0]==0xef && Bytes[1]==0xbb && Bytes[2]==0xbf) Start=3;
	for (uint8 Byte:Bytes) if (!Byte) { Issue({Uri,{}},TEXT("OA-DOC-ENCODING"),TEXT("UTF-16/32 and embedded null bytes are not supported; save the document as UTF-8."));return nullptr; }
	while (Start<Bytes.Num() && (Bytes[Start]==' ' || Bytes[Start]=='\r' || Bytes[Start]=='\n' || Bytes[Start]=='\t')) ++Start;
	if (Start>=Bytes.Num() || Bytes[Start]!='{')
	{ Issue({Uri,{}},TEXT("OA-YAML-SYNTAX"),TEXT("This importer currently accepts JSON documents; convert the file to JSON with any YAML 1.2 tool and import the JSON."));return nullptr; }
	FVaRestJsonParseOptions Parse;Parse.MaxInputBytes=Options.MaxDocumentBytes;
	auto Parsed=FVaRestJson::Parse(Bytes,Parse);
	if (Parsed.HasError())
	{
		const auto& Error=Parsed.GetError();Issue({Uri,{}},ValidUtf8(Bytes) ? TEXT("OA-DOC-SYNTAX") : TEXT("OA-DOC-ENCODING"),Error.Message);
		Result.Issues.Last().Line=Error.Line;Result.Issues.Last().Column=Error.Column;return nullptr;
	}
	const auto Doc=MakeShared<FDocument>();Doc->Uri=Uri;Doc->Root=Parsed.StealValue();Doc->Bytes.Append(Bytes.GetData(),Bytes.Num());
	Documents.Add(Uri,Doc);Result.Model.SourceDocuments.Add(Uri);Duplicates(Doc->Root,{Uri,{}});return Doc;
}
void FAnalysis::Duplicates(const FVaRestJson& Node,const FVaRestOpenApiLocation& Location)
{
	if (Node.IsObject())
	{
		FStringSet Keys;
		for (const auto& Member:Node.Members())
		{
			const FString Key=Member.GetKey();const FVaRestOpenApiLocation Here{Location.Document,Child(Location.Pointer,Key)};
			if (Keys.Contains(Key)) Issue(Here,TEXT("OA-DOC-DUPKEY"),TEXT("Duplicate key; the last value wins."),EVaRestOpenApiSeverity::Warning);
			Keys.Add(Key);Duplicates(Member.Value,Here);
		}
	}
	else if (Node.IsArray()) for (int32 Index=0;Index<Node.Num();++Index) Duplicates(Node.At(Index),{Location.Document,Child(Location.Pointer,LexToString(Index))});
}
TSharedPtr<FDocument> FAnalysis::Document(const FString& Uri,const FVaRestOpenApiLocation& From)
{
	if (const auto* Existing=Documents.Find(Uri)) return *Existing;
	if (FailedDocuments.Contains(Uri)) { Issue(From,TEXT("OA-REF-UNRESOLVED"),TEXT("The referenced document could not be loaded."));return nullptr; }
	FailedDocuments.Add(Uri);
	if (ExternalReads>=Options.MaxExternalDocuments) { Issue(From,TEXT("OA-DOC-LIMIT"),TEXT("The import exceeds MaxExternalDocuments (including failed source reads)."));return nullptr; }
	++ExternalReads;
	if (!Options.Read) { Issue(From,TEXT("OA-REF-UNRESOLVED"),TEXT("No source reader was supplied for this external reference."));return nullptr; }
	FVaRestOpenApiReadResult Read;
	if (!Options.Read({Uri,RootBase,bUrl,Options.bAllowOutsideRoot},Read)) { Issue(From,TEXT("OA-REF-UNRESOLVED"),Read.Error.IsEmpty() ? TEXT("The source reader refused the referenced document.") : Read.Error);return nullptr; }
	// A loader may follow a physical alias or HTTP redirect only within its admitted root boundary.
	if (Read.CanonicalUri.IsEmpty() || !Equal(Read.CanonicalUri,Uri))
	{
		FString Canonical,Path,Query,Origin;
		if (bUrl && Url(Read.CanonicalUri,Origin,Path,Query) && Origin==RootOrigin) Canonical=Origin+Path+Query;
		else if (!bUrl && !FPaths::IsRelative(Read.CanonicalUri) && !Read.CanonicalUri.Contains(TEXT("://"))) FilePath(Read.CanonicalUri,Canonical);
		if (Canonical.IsEmpty() || (!Canonical.StartsWith(RootBase,ESearchCase::CaseSensitive) && (bUrl || !Options.bAllowOutsideRoot)))
		{ Issue(From,TEXT("OA-REF-OUTSIDE"),TEXT("The canonical reference source is outside the admitted source root."));return nullptr; }
		if (const auto* Existing=Documents.Find(Canonical)) { const auto Cached=*Existing;Documents.Add(Uri,Cached);return Cached; }
		const auto Loaded=Load(Canonical,Read.Bytes);if (Loaded) Documents.Add(Uri,Loaded);return Loaded;
	}
	return Load(Uri,Read.Bytes);
}
FResolved FAnalysis::At(const FVaRestOpenApiLocation& Location)
{
	const auto Doc=Document(Location.Document,Location);if (!Doc) return {};
	FVaRestJson Value=Doc->Root;
	if (!Location.Pointer.IsEmpty())
	{
		if (!Location.Pointer.StartsWith(TEXT("/"))) return {};
		int32 Start=1;
		while (Start<=Location.Pointer.Len())
		{
			int32 End=Location.Pointer.Find(TEXT("/"),ESearchCase::CaseSensitive,ESearchDir::FromStart,Start);if (End==INDEX_NONE) End=Location.Pointer.Len();
			FString Token;
			for (int32 Index=Start;Index<End;++Index)
			{
				TCHAR Char=Location.Pointer[Index];
				if (Char==TEXT('~')) { if (++Index>=End || (Location.Pointer[Index]!=TEXT('0') && Location.Pointer[Index]!=TEXT('1'))) return {};Char=Location.Pointer[Index]==TEXT('0') ? TEXT('~') : TEXT('/'); }
				Token.AppendChar(Char);
			}
			if (Value.IsObject()) Value=Value.Get(Token);
			else if (Value.IsArray())
			{
				if (Token.IsEmpty() || (Token.Len()>1 && Token[0]==TEXT('0'))) return {};
				int64 Index=0;for (TCHAR Char:Token) { if (Char<TEXT('0') || Char>TEXT('9') || Index>MAX_int32/10) return {};Index=Index*10+Char-TEXT('0'); }
				if (Index>=Value.Num()) return {};Value=Value.At(static_cast<int32>(Index));
			}
			else return {};
			if (!Value.IsValid()) return {};
			if (End==Location.Pointer.Len()) break;Start=End+1;
		}
	}
	return {Value,{}, {Doc->Uri,Location.Pointer}};
}
FResolved FAnalysis::Reference(const FVaRestOpenApiLocation& From,const FString& Ref)
{
	FString Resource=Ref,Pointer;int32 Hash;
	if (Resource.FindChar(TEXT('#'),Hash)) { if (!Decode(Resource.Mid(Hash+1),Pointer)) { Issue(From,TEXT("OA-REF-UNRESOLVED"),TEXT("The reference fragment has invalid percent encoding."));return {}; }Resource=Resource.Left(Hash); }
	FString Uri=From.Document;
	if (!Resource.IsEmpty())
	{
		FString Origin,Path,Query;
		if (Resource.Contains(TEXT("://")) || Resource.StartsWith(TEXT("//")))
		{
			if (!bUrl || !Url(Resource.StartsWith(TEXT("//")) ? RootOrigin.Left(RootOrigin.Find(TEXT(":")))+TEXT(":")+Resource : Resource,Origin,Path,Query) || Origin!=RootOrigin)
			{ Issue(From,TEXT("OA-REF-REMOTE"),TEXT("Remote-origin references are not fetched; bundle the specification first."));return {}; }
			Uri=Origin+Path+Query;
		}
		else if (bUrl)
		{
			FString ParentOrigin,ParentPath,ParentQuery;Url(From.Document,ParentOrigin,ParentPath,ParentQuery);
			int32 Mark=Resource.Find(TEXT("?"));Query=Mark==INDEX_NONE ? FString() : Resource.Mid(Mark);const FString RefPath=Mark==INDEX_NONE ? Resource : Resource.Left(Mark);
			// A query-only reference replaces the query of this document, not its filename.
			// Validate the complete spelling before any reader call, including query controls.
			const FString TargetPath=RefPath.IsEmpty() ? ParentPath : RefPath.StartsWith(TEXT("/")) ? RefPath
				: ParentPath.Left(ParentPath.Find(TEXT("/"),ESearchCase::CaseSensitive,ESearchDir::FromEnd)+1)+RefPath;
			if (!Url(ParentOrigin+TargetPath+Query,Origin,Path,Query))
			{ Issue(From,TEXT("OA-REF-OUTSIDE"),TEXT("The reference path cannot be confined to the source root."));return {}; }
			Uri=Origin+Path+Query;
		}
		else
		{
			FString Decoded;
			if (Resource.Contains(TEXT(":")) || !Decode(Resource,Decoded) || !FilePath(Decoded.StartsWith(TEXT("/")) ? Decoded : FPaths::GetPath(From.Document)+TEXT("/")+Decoded,Uri))
			{ Issue(From,TEXT("OA-REF-OUTSIDE"),TEXT("The reference path cannot be confined to the source root."));return {}; }
		}
		if (!Uri.StartsWith(RootBase,ESearchCase::CaseSensitive) && (bUrl || !Options.bAllowOutsideRoot))
		{ Issue(From,TEXT("OA-REF-OUTSIDE"),TEXT("The reference is outside the source directory tree or URL prefix."));return {}; }
	}
	const auto Target=At({Uri,Pointer});if (!Target.IsValid()) Issue(From,TEXT("OA-REF-UNRESOLVED"),TEXT("The reference does not identify an existing value."));return Target;
}
FResolved FAnalysis::Object(const FVaRestOpenApiLocation& Location,FStringSet* Chain)
{
	auto Value=At(Location);if (!Value.IsValid()) return {};
	const auto Ref=Value.Value[TEXT("$ref")];if (!Ref.IsValid()) return Value;
	if (!Ref.IsString()) { Issue(Location,TEXT("OA-REF-UNRESOLVED"),TEXT("A reference must be a string."));return {}; }
	FStringSet Local;auto& Seen=Chain ? *Chain : Local;const FString Id=Identity(Value.Location);
	if (Seen.Contains(Id)) { Issue(Location,TEXT("OA-REF-LOOP"),TEXT("A pure reference alias loop was found."));return {}; }
	if (Seen.Num()>=VaRest::Json::MaxDepth) { Issue(Location,TEXT("OA-DOC-LIMIT"),TEXT("The reference chain exceeds the supported depth."));return {}; }
	Seen.Add(Id);ON_SCOPE_EXIT { Seen.Remove(Id); };
	const auto Target=Reference(Value.Location,Ref.AsString());if (!Target.IsValid()) return {};
	auto Resolved=Object(Target.Location,&Seen);if (!Resolved.IsValid()) return {};
	if (!Resolved.Annotations.IsValid()) Resolved.Annotations=FVaRestJson::Object();
	for (const auto& Member:Value.Value.Members()) if (!AnnotationsKey(Member.GetKey()).IsEmpty()) Resolved.Annotations.Set(Member.GetKey(),Member.Value);
	return Resolved;
}
FString FAnalysis::Identity(const FVaRestOpenApiLocation& Location) const
{
	const FString Source=Equal(Location.Document,RootUri) ? FString() : Location.Document.StartsWith(RootBase,ESearchCase::CaseSensitive) ? Location.Document.Mid(RootBase.Len()) : Location.Document;
	return (bUrl ? Source : EncodePath(Source))+TEXT("#")+Location.Pointer;
}
}

FVaRestOpenApiAnalysis FVaRestOpenApiLoader::Analyze(FString RootUri,TConstArrayView<uint8> Bytes,const FVaRestOpenApiLoadOptions& Options)
{
	return VaRest::OpenApi::FAnalysis(MoveTemp(RootUri),Options).Run(Bytes);
}
