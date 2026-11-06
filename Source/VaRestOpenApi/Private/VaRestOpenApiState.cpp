// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "Misc/Compression.h"

namespace VaRest::OpenApi
{
namespace { constexpr int32 MaxStateBytes=256*1024*1024; }
bool EncodeState(const FVaRestJson& Json,FVaRestApiImportState& State)
{
	TArray<uint8> Bytes;auto Written=Json.TryToUtf8(Bytes);if (Written.HasError() || Bytes.Num()>MaxStateBytes) return false;
	int64 Capacity=0;if (!FCompression::CompressMemoryBound(NAME_Zlib,Capacity,static_cast<int64>(Bytes.Num())) || Capacity>MAX_int32-8) return false;
	TArray<uint8> Blob;Blob.SetNumUninitialized(static_cast<int32>(Capacity)+8);
	Blob[0]='V';Blob[1]='O';Blob[2]='A';Blob[3]=1;
	const uint32 Size=Bytes.Num();for (int32 Index=0;Index<4;++Index) Blob[4+Index]=static_cast<uint8>(Size>>(Index*8));
	if (!FCompression::CompressMemory(NAME_Zlib,Blob.GetData()+8,Capacity,Bytes.GetData(),static_cast<int64>(Bytes.Num()))) return false;
	Blob.SetNum(static_cast<int32>(Capacity)+8);State.Importer=TEXT("VaRestOpenApi");State.Version=1;State.Blob=MoveTemp(Blob);return true;
}
TOptional<FVaRestJson> DecodeState(const FVaRestApiImportState& State)
{
	const auto& Blob=State.Blob;if (State.Importer!=TEXT("VaRestOpenApi") || State.Version!=1 || Blob.Num()<8 || Blob[0]!='V' || Blob[1]!='O' || Blob[2]!='A' || Blob[3]!=1) return {};
	uint32 Size=0;for (int32 Index=0;Index<4;++Index) Size|=static_cast<uint32>(Blob[4+Index])<<(Index*8);
	if (Size==0 || Size>MaxStateBytes) return {};TArray<uint8> Bytes;Bytes.SetNumUninitialized(Size);
	if (!FCompression::UncompressMemory(NAME_Zlib,Bytes.GetData(),static_cast<int64>(Size),Blob.GetData()+8,static_cast<int64>(Blob.Num()-8))) return {};
	auto Parsed=FVaRestJson::ParseOwned(MoveTemp(Bytes));if (Parsed.HasError() || !Parsed.GetValue().IsObject()) return {};return Parsed.StealValue();
}
}
