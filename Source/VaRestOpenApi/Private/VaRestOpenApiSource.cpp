// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiSource.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RapidJsonIncludes.h"

namespace VaRest::OpenApi
{
bool ReadSourceFile(const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out)
{
	Out={};
	// Filter before metadata: a ref cannot turn this document reader into a credential/binary reader.
	const FString Extension=FPaths::GetExtension(Request.Uri).ToLower();
	if (Request.bUrl || (Extension!=TEXT("json") && Extension!=TEXT("yaml") && Extension!=TEXT("yml"))) { Out.Error=TEXT("Only local JSON/YAML source documents are accepted by the file reader.");return false; }
	FString Path=FPaths::ConvertRelativePathToFull(Request.Uri);FPaths::NormalizeFilename(Path);
	if (!FPaths::CollapseRelativeDirectories(Path) || Path.Contains(TEXT("://"))) { Out.Error=TEXT("Invalid source path.");return false; }
	FString Root=FPaths::ConvertRelativePathToFull(Request.RootBase);FPaths::NormalizeDirectoryName(Root);Root+=TEXT("/");
	if (!Request.bAllowOutsideRoot && !Path.StartsWith(Root,ESearchCase::CaseSensitive)) { Out.Error=TEXT("The source document is outside the spec folder.");return false; }
	auto& Files=FPlatformFileManager::Get().GetPlatformFile();
	FString Part=Path;
	// All path components must be physically unaliased before OpenRead, including the spec folder.
	while (!Part.IsEmpty())
	{
		if (Files.IsSymlink(*Part)!=ESymlinkResult::NonSymlink) { Out.Error=TEXT("A source path contains a symbolic link or the platform cannot verify aliases.");return false; }
		const FString Parent=FPaths::GetPath(Part);if (Parent==Part) break;Part=Parent;
	}
	TUniquePtr<IFileHandle> Handle(Files.OpenRead(*Path));if (!Handle) { Out.Error=TEXT("Could not open the source document.");return false; }
	const int64 Size=Handle->Size();if (Size<0 || Size>64ll*1024*1024) { Out.Error=TEXT("The source document exceeds 64 MiB or has no readable size.");return false; }
	Out.Bytes.SetNumUninitialized(static_cast<int32>(Size));
	if (Size>0 && !Handle->Read(Out.Bytes.GetData(),Size)) { Out.Bytes.Reset();Out.Error=TEXT("Could not read the complete source document.");return false; }
	Out.CanonicalUri=MoveTemp(Path);return true;
}
bool SniffOpenApi(TConstArrayView<uint8> Bytes)
{
	struct FSniff : rapidjson::BaseReaderHandler<rapidjson::UTF8<>,FSniff>
	{
		int32 Depth=0;bool Found=false;
		bool StartObject() { return ++Depth<=VaRest::Json::MaxDepth; }
		bool StartArray() { return ++Depth<=VaRest::Json::MaxDepth; }
		bool EndObject(rapidjson::SizeType) { --Depth;return true; }
		bool EndArray(rapidjson::SizeType) { --Depth;return true; }
		bool Key(const char* Text,rapidjson::SizeType Length,bool)
		{
			Found=Depth==1 && ((Length==7 && FMemory::Memcmp(Text,"openapi",7)==0) || (Length==7 && FMemory::Memcmp(Text,"swagger",7)==0));return !Found;
		}
	} Handler;
	int32 Start=Bytes.Num()>=3 && Bytes[0]==0xef && Bytes[1]==0xbb && Bytes[2]==0xbf ? 3 : 0;
	if (Bytes.Num()==Start) return false;
	rapidjson::MemoryStream Stream(reinterpret_cast<const char*>(Bytes.GetData()+Start),Bytes.Num()-Start);rapidjson::Reader Reader;Reader.Parse<rapidjson::kParseValidateEncodingFlag>(Stream,Handler);
	if (Handler.Found) return true;
	// YAML is recognized for explicit conversion diagnostics until the strict YAML milestone lands.
	int32 Line=Start;
	while (Line<Bytes.Num())
	{
		if (Bytes.Num()-Line>=8 && (FMemory::Memcmp(Bytes.GetData()+Line,"openapi:",8)==0 || FMemory::Memcmp(Bytes.GetData()+Line,"swagger:",8)==0)) return true;
		while (Line<Bytes.Num() && Bytes[Line]!='\n') ++Line;++Line;
	}
	return false;
}
FVaRestJson SourceRecord(const FString& Uri,TConstArrayView<uint8> Bytes)
{
	FMD5 Hash;Hash.Update(Bytes.GetData(),Bytes.Num());uint8 Digest[16];Hash.Final(Digest);
	return FVaRestJson::Object().Set(TEXT("location"),Uri).Set(TEXT("hash"),BytesToHex(Digest,16)).Set(TEXT("size"),Bytes.Num());
}
}
