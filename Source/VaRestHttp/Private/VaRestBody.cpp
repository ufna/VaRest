// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBody.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "VaRestHttpLog.h"
#include "VaRestJsonUtf8.h"
#include "VaRestUrl.h"

namespace VaRest::Http::BodyPrivate
{
	void AppendUtf8(TArray<uint8>& Out, FStringView Text)
	{
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
		Out.Append(reinterpret_cast<const uint8*>(Utf8.GetData()), Utf8.Len());
	}

	bool HasNul(FStringView Text)
	{
		for (const TCHAR C : Text)
		{
			if (C == 0)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Review I3 and M8: the boundary goes into the Content-Type header -- 1 to 70 RFC 2046 bchars (2046 5.1.1), so no
	 * CR/LF (header injection) and no '"'. Empty when valid. The text is not repeated in the message: it may hold control
	 * characters.
	 */
	FString BoundaryProblem(const FString& Boundary)
	{
		bool bValidBoundary = Boundary.Len() >= 1 && Boundary.Len() <= 70 && !Boundary.EndsWith(TEXT(" "));
		for (const TCHAR C : Boundary)
		{
			const bool bAlnum = (C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'));
			// C != 0: Strchr finds the terminator, so NUL would pass (FGenericWidePlatformString::Strchr,
			// GenericWidePlatformString.h:157-165, used on Linux, Mac, iOS and Android; Windows' _tcschr,
			// MicrosoftPlatformString.h:144-147, does the same; review N-M1, M3)
			bValidBoundary &= bAlnum || (C != 0 && FCString::Strchr(TEXT("'()+_,-./:=? "), C) != nullptr);
		}
		return bValidBoundary ? FString() : FString(TEXT("the multipart boundary must be 1-70 characters of digits, letters and '()+_,-./:=? (no space at the end; RFC 2046 5.1.1)"));
	}

	/** Review M6: WHATWG escaping cannot express NUL, so a name or file name holding one is refused */
	void RefuseNul(FString& Error, FStringView Name, FStringView FileName)
	{
		if (Error.IsEmpty() && (HasNul(Name) || HasNul(FileName)))
		{
			Error = FString(TEXT("a multipart part name or file name holds NUL"));
		}
	}

	/** WHATWG HTML, multipart/form-data encoding: '"', CR and LF in a name or file name are percent-escaped */
	FString EscapeQuoted(FStringView Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		for (const TCHAR C : Text)
		{
			if (C == TEXT('"'))
			{
				Out += TEXT("%22");
			}
			else if (C == TEXT('\r'))
			{
				Out += TEXT("%0D");
			}
			else if (C == TEXT('\n'))
			{
				Out += TEXT("%0A");
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}
}

FVaRestForm& FVaRestForm::Add(FStringView Name, FStringView Value) &
{
	Fields.Emplace(FString(Name), FString(Value));
	return *this;
}

FVaRestForm FVaRestForm::Add(FStringView Name, FStringView Value) &&
{
	Add(Name, Value);
	return MoveTemp(*this);
}

TArray<uint8> FVaRestForm::Encode() const
{
	FString Joined;
	for (int32 i = 0; i < Fields.Num(); ++i)
	{
		if (i > 0)
		{
			Joined.AppendChar(TEXT('&'));
		}
		Joined += VaRest::Http::FormEncode(Fields[i].Key);
		Joined.AppendChar(TEXT('='));
		Joined += VaRest::Http::FormEncode(Fields[i].Value);
	}
	TArray<uint8> Out;
	VaRest::Http::BodyPrivate::AppendUtf8(Out, Joined); // ASCII after FormEncode
	return Out;
}

FVaRestMultipart::FVaRestMultipart()
	: Boundary(TEXT("----VaRest") + FGuid::NewGuid().ToString(EGuidFormats::Digits))
{
}

FVaRestMultipart::FVaRestMultipart(FStringView InBoundary)
	: Boundary(InBoundary)
{
	// Review M8: every path (GetContentType, Encode, FromMultipart) sees a bad boundary through GetError()
	Error = VaRest::Http::BodyPrivate::BoundaryProblem(Boundary);
}

FVaRestMultipart& FVaRestMultipart::AddField(FStringView Name, FStringView Value) &
{
	VaRest::Http::BodyPrivate::RefuseNul(Error, Name, FStringView());
	FVaRestMultipartPart& Part = Parts.AddDefaulted_GetRef();
	Part.Name = FString(Name);
	Part.bField = true;
	VaRest::Http::BodyPrivate::AppendUtf8(Part.Bytes, Value);
	Part.Size = Part.Bytes.Num();
	return *this;
}

FVaRestMultipart FVaRestMultipart::AddField(FStringView Name, FStringView Value) &&
{
	AddField(Name, Value);
	return MoveTemp(*this);
}

FVaRestMultipart& FVaRestMultipart::AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &
{
	if (!VaRest::Http::IsValidHeaderValue(PartContentType) && Error.IsEmpty())
	{
		Error = FString::Printf(TEXT("multipart part \"%s\": the content type holds CR, LF or NUL"), *FString(Name));
	}
	VaRest::Http::BodyPrivate::RefuseNul(Error, Name, FileName);
	FVaRestMultipartPart& Part = Parts.AddDefaulted_GetRef();
	Part.Name = FString(Name);
	Part.FileName = FString(FileName);
	Part.ContentType = PartContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : FString(PartContentType);
	Part.Size = InBytes.Num();
	Part.Bytes = MoveTemp(InBytes);
	return *this;
}

FVaRestMultipart FVaRestMultipart::AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &&
{
	AddBytes(Name, FileName, PartContentType, MoveTemp(InBytes));
	return MoveTemp(*this);
}

FVaRestMultipart& FVaRestMultipart::AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType, FStringView FileName) &
{
	const FString Path(FilePath);
	const int64 FileSize = IFileManager::Get().FileSize(*Path);
	if (FileSize < 0 && Error.IsEmpty())
	{
		Error = FString::Printf(TEXT("multipart part \"%s\": file not found: %s"), *FString(Name), *Path);
	}
	if (!VaRest::Http::IsValidHeaderValue(PartContentType) && Error.IsEmpty())
	{
		Error = FString::Printf(TEXT("multipart part \"%s\": the content type holds CR, LF or NUL"), *FString(Name));
	}
	VaRest::Http::BodyPrivate::RefuseNul(Error, Name, FileName);
	FVaRestMultipartPart& Part = Parts.AddDefaulted_GetRef();
	Part.Name = FString(Name);
	Part.FileName = FileName.IsEmpty() ? FPaths::GetCleanFilename(Path) : FString(FileName);
	Part.ContentType = PartContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : FString(PartContentType);
	Part.FilePath = Path;
	Part.Size = FMath::Max<int64>(FileSize, 0);
	return *this;
}

FVaRestMultipart FVaRestMultipart::AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType, FStringView FileName) &&
{
	AddFile(Name, FilePath, PartContentType, FileName);
	return MoveTemp(*this);
}

FString FVaRestMultipart::GetContentType() const
{
	// Review I3: bchars include RFC 2045 tspecials ( ) , / : = ? and space, which a parameter value may hold only as a
	// quoted-string. The validator excludes '"' and '\\', so the quoted form needs no escaping.
	return TEXT("multipart/form-data; boundary=") + (VaRest::Http::IsValidHeaderName(Boundary) ? Boundary : TEXT("\"") + Boundary + TEXT("\""));
}

bool FVaRestMultipart::HasFiles() const
{
	return Parts.ContainsByPredicate([](const FVaRestMultipartPart& Part) { return !Part.FilePath.IsEmpty(); });
}

TArray<FVaRestBodySegment> FVaRestMultipart::Encode() const
{
	using VaRest::Http::BodyPrivate::AppendUtf8;
	using VaRest::Http::BodyPrivate::EscapeQuoted;
	TArray<FVaRestBodySegment> Segments;
	TArray<uint8> Pending;
	for (const FVaRestMultipartPart& Part : Parts)
	{
		AppendUtf8(Pending, TEXT("--"));
		AppendUtf8(Pending, Boundary);
		AppendUtf8(Pending, TEXT("\r\nContent-Disposition: form-data; name=\""));
		AppendUtf8(Pending, EscapeQuoted(Part.Name));
		AppendUtf8(Pending, TEXT("\""));
		if (!Part.bField)
		{
			AppendUtf8(Pending, TEXT("; filename=\""));
			AppendUtf8(Pending, EscapeQuoted(Part.FileName));
			AppendUtf8(Pending, TEXT("\"\r\nContent-Type: "));
			AppendUtf8(Pending, Part.ContentType);
		}
		AppendUtf8(Pending, TEXT("\r\n\r\n"));
		if (Part.FilePath.IsEmpty())
		{
			Pending.Append(Part.Bytes);
		}
		else
		{
			FVaRestBodySegment& Head = Segments.AddDefaulted_GetRef();
			Head.Bytes = MoveTemp(Pending);
			Pending.Reset();
			FVaRestBodySegment& File = Segments.AddDefaulted_GetRef();
			File.FilePath = Part.FilePath;
			File.FileSize = Part.Size;
		}
		AppendUtf8(Pending, TEXT("\r\n"));
	}
	AppendUtf8(Pending, TEXT("--"));
	AppendUtf8(Pending, Boundary);
	AppendUtf8(Pending, TEXT("--\r\n"));
	FVaRestBodySegment& Tail = Segments.AddDefaulted_GetRef();
	Tail.Bytes = MoveTemp(Pending);
	return Segments;
}

FVaRestSegmentStream::FVaRestSegmentStream(TArray<FVaRestBodySegment> InSegments)
	: Segments(MoveTemp(InSegments))
{
	SetIsLoading(true);
	SetIsPersistent(false);
	for (const FVaRestBodySegment& Segment : Segments)
	{
		Total += Segment.Size();
	}
}

FVaRestSegmentStream::~FVaRestSegmentStream() = default;

void FVaRestSegmentStream::Seek(int64 InPos)
{
	Position = FMath::Clamp<int64>(InPos, 0, Total);
}

bool FVaRestSegmentStream::Close()
{
	OpenFile.Reset();
	OpenIndex = INDEX_NONE;
	return !IsError();
}

void FVaRestSegmentStream::FailRead()
{
	SetError();
	bReadFailed = true;
}

bool FVaRestSegmentStream::ReadFileSegment(int32 Index, int64 Offset, uint8* Out, int64 Count)
{
	if (OpenIndex != Index)
	{
		OpenFile.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Segments[Index].FilePath));
		OpenIndex = Index;
		// Review M4: Count is capped at the size recorded when the part was added, so a file that grew would otherwise send
		// the first N bytes of its new content as if nothing had happened
		if (OpenFile.IsValid() && OpenFile->Size() != Segments[Index].FileSize)
		{
			OpenFile.Reset();
		}
	}
	return OpenFile.IsValid() && OpenFile->Seek(Offset) && OpenFile->Read(Out, Count);
}

void FVaRestSegmentStream::Serialize(void* Data, int64 Length)
{
	uint8* Out = static_cast<uint8*>(Data);
	while (Length > 0)
	{
		int64 SegmentStart = 0;
		int32 Index = 0;
		for (; Index < Segments.Num(); ++Index)
		{
			const int64 SegmentSize = Segments[Index].Size();
			if (Position < SegmentStart + SegmentSize)
			{
				break;
			}
			SegmentStart += SegmentSize;
		}
		if (Index >= Segments.Num())
		{
			// Past the end: the caller asked for more than TotalSize()
			FMemory::Memzero(Out, Length);
			FailRead();
			return;
		}
		const FVaRestBodySegment& Segment = Segments[Index];
		const int64 Offset = Position - SegmentStart;
		const int64 Count = FMath::Min<int64>(Length, Segment.Size() - Offset);
		if (Segment.FilePath.IsEmpty())
		{
			FMemory::Memcpy(Out, Segment.Bytes.GetData() + Offset, Count);
		}
		else if (!ReadFileSegment(Index, Offset, Out, Count))
		{
			FMemory::Memzero(Out, Count);
			FailRead();
			if (!bWarned)
			{
				bWarned = true;
				// The file name only: a full local path is not for logs (review M4)
				UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest: upload file %s could not be read at byte %lld (missing, or its size changed since the body was built); sending zeros"), *FPaths::GetCleanFilename(Segment.FilePath), Offset);
			}
		}
		Out += Count;
		Position += Count;
		Length -= Count;
	}
}

FVaRestRequestBody FVaRestRequestBody::FromBytes(TArray<uint8>&& InBytes, EVaRestBodyKind InKind, FStringView InContentType)
{
	FVaRestRequestBody Out;
	Out.Kind = InKind;
	// Review M7: FromFile refuses a content type holding CR, LF or NUL. This one cannot return an error (frozen signature),
	// so it sends no default Content-Type rather than one that could end the header.
	if (VaRest::Http::IsValidHeaderValue(InContentType))
	{
		Out.ContentType = FString(InContentType);
	}
	else
	{
		UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest: a request body content type holding a control character was dropped"));
	}
	Out.ContentLength = InBytes.Num();
	Out.Bytes = MoveTemp(InBytes);
	return Out;
}

FVaRestRequestBody FVaRestRequestBody::FromForm(const FVaRestForm& Form)
{
	return FromBytes(Form.Encode(), EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"));
}

TValueOrError<FVaRestRequestBody, FString> FVaRestRequestBody::FromMultipart(const FVaRestMultipart& Multipart)
{
	if (!Multipart.GetError().IsEmpty())
	{
		return MakeError(Multipart.GetError());
	}
	FVaRestRequestBody Out;
	Out.Kind = EVaRestBodyKind::Multipart;
	Out.ContentType = Multipart.GetContentType();
	TArray<FVaRestBodySegment> Encoded = Multipart.Encode();
	for (const FVaRestBodySegment& Segment : Encoded)
	{
		Out.ContentLength += Segment.Size();
	}
	if (Multipart.HasFiles())
	{
		Out.Segments = MoveTemp(Encoded);
	}
	else
	{
		Out.Bytes = MoveTemp(Encoded[0].Bytes);
	}
	for (const FVaRestMultipartPart& Part : Multipart.GetParts())
	{
		// Field by field: copying the part and emptying its bytes would copy a large in-memory file first (review M10)
		FVaRestMultipartPart& Descriptor = Out.PartDescriptors.AddDefaulted_GetRef();
		Descriptor.Name = Part.Name;
		Descriptor.FileName = Part.FileName;
		Descriptor.ContentType = Part.ContentType;
		Descriptor.FilePath = Part.FilePath;
		Descriptor.Size = Part.Size;
		Descriptor.bField = Part.bField;
		if (Part.bField)
		{
			Descriptor.Bytes = Part.Bytes;
		}
	}
	return MakeValue(MoveTemp(Out));
}

TValueOrError<FVaRestRequestBody, FString> FVaRestRequestBody::FromFile(FStringView InFilePath, FStringView InContentType)
{
	const FString Path(InFilePath);
	const int64 FileSize = IFileManager::Get().FileSize(*Path);
	if (FileSize < 0)
	{
		return MakeError(FString::Printf(TEXT("upload file not found: %s"), *Path));
	}
	if (!VaRest::Http::IsValidHeaderValue(InContentType))
	{
		return MakeError(FString(TEXT("the upload content type holds CR, LF or NUL")));
	}
	FVaRestRequestBody Out;
	Out.Kind = EVaRestBodyKind::Stream;
	Out.ContentType = InContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : FString(InContentType);
	Out.FilePath = Path;
	Out.ContentLength = FileSize;
	return MakeValue(MoveTemp(Out));
}
