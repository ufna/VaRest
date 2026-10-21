// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBodySink.h"

#include "Async/UniqueLock.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

VaRest::Http::FBodySink::FBodySink(int64 InMaxBytes)
	: MaxBytes(FMath::Max<int64>(0, InMaxBytes))
{
}

VaRest::Http::FBodySink::FBodySink(FString InFilePath, int64 InMaxBytes)
	: FilePath(MoveTemp(InFilePath))
	, MaxBytes(FMath::Max<int64>(0, InMaxBytes))
{
}

VaRest::Http::FBodySink::~FBodySink() = default;

bool VaRest::Http::FBodySink::Write(const void* Data, int64 Length, int64 ContentLength)
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	if (bError || bSealed)
	{
		return false;
	}
	if (Length <= 0)
	{
		return true;
	}
	const int64 NewNum = Written + Length;
	if (MaxBytes > 0 && NewNum > MaxBytes)
	{
		// Review M13: bounded memory on untrusted input -- the transfer stops here, the request ends ResponseTooLarge
		bError = true;
		bTooLarge = true;
		return false;
	}
	if (IsFile())
	{
		if (!File.IsValid())
		{
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			PlatformFile.CreateDirectoryTree(*FPaths::GetPath(FilePath));
			File.Reset(PlatformFile.OpenWrite(*FilePath));
		}
		if (!File.IsValid() || !File->Write(static_cast<const uint8*>(Data), Length))
		{
			bError = true;
			return false;
		}
		Written = NewNum;
		return true;
	}

	if (NewNum + 1 > MAX_int32)
	{
		// TArray<uint8> holds int32 elements; SaveTo is the way for a larger body
		bError = true;
		return false;
	}
	if (Bytes.Max() == 0 && ContentLength > 0)
	{
		// Content-Length + 1: the terminator ParseOwned appends fits without a reallocation (spec 2.1). Capped, because a
		// Content-Length is a claim: 2000000000 would otherwise allocate 2 GB before a byte arrived (review M-6).
		const int64 Claimed = FMath::Min(ContentLength, FMath::Max(MaxBytes, ReserveCap));
		Bytes.Reserve(static_cast<int32>(FMath::Min<int64>(MAX_int32 - 1, FMath::Max(Claimed, NewNum)) + 1));
	}
	else if (NewNum + 1 > Bytes.Max())
	{
		// Unknown, wrong or capped length (chunked, compressed): grow geometrically, always leaving a byte free
		Bytes.Reserve(static_cast<int32>(FMath::Min<int64>(MAX_int32, FMath::Max<int64>(NewNum + 1, static_cast<int64>(Bytes.Max()) * 2))));
	}
	Bytes.Append(static_cast<const uint8*>(Data), static_cast<int32>(Length));
	Written = NewNum;
	return true;
}

TArray<uint8> VaRest::Http::FBodySink::TakeBytes()
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	bSealed = true;
	if (Bytes.Max() == Bytes.Num())
	{
		Bytes.Reserve(Bytes.Num() + 1);
	}
	return MoveTemp(Bytes);
}

bool VaRest::Http::FBodySink::CloseFile()
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	bSealed = true;
	if (File.IsValid())
	{
		if (!File->Flush())
		{
			bError = true;
		}
		File.Reset();
	}
	return !bError;
}

int64 VaRest::Http::FBodySink::Num() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return Written;
}

bool VaRest::Http::FBodySink::HasError() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return bError;
}

bool VaRest::Http::FBodySink::IsTooLarge() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return bTooLarge;
}

const uint8* VaRest::Http::FBodySink::GetDataInternal() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return Bytes.GetData();
}

int32 VaRest::Http::FBodySink::GetCapacityInternal() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return Bytes.Max();
}
