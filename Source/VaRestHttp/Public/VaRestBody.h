// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/Archive.h"
#include "Templates/ValueOrError.h"

class IFileHandle;

/** application/x-www-form-urlencoded fields, in order (spec 3.2). Ref-qualified like FVaRestJson::Set. */
class VARESTHTTP_API FVaRestForm
{
public:
	FVaRestForm& Add(FStringView Name, FStringView Value) &;
	FVaRestForm Add(FStringView Name, FStringView Value) &&;

	const TArray<TPair<FString, FString>>& GetFields() const { return Fields; }

	/** name=value&... with FormEncode'd names and values, as UTF-8 bytes */
	TArray<uint8> Encode() const;

private:
	TArray<TPair<FString, FString>> Fields;
};

/** One multipart/form-data part. Binary bytes are dropped from the descriptors a request keeps for logs and traces. */
struct FVaRestMultipartPart
{
	FString Name;
	FString FileName;
	FString ContentType;
	/** A field's UTF-8 text, or an in-memory file's bytes */
	TArray<uint8> Bytes;
	/** A file streamed at upload time */
	FString FilePath;
	int64 Size = 0;
	/** A text field: no filename, no Content-Type */
	bool bField = false;
};

/** A run of request body bytes: in memory, or a file read while uploading */
struct FVaRestBodySegment
{
	TArray<uint8> Bytes;
	FString FilePath;
	int64 FileSize = 0;

	int64 Size() const { return FilePath.IsEmpty() ? Bytes.Num() : FileSize; }
};

/**
 * multipart/form-data (spec 3.2). File parts are streamed: their size is read when the part is added (on the calling
 * thread), their bytes while uploading. Names and file names are escaped the WHATWG way ('"', CR, LF as %22, %0D, %0A).
 */
class VARESTHTTP_API FVaRestMultipart
{
public:
	/** A random boundary */
	FVaRestMultipart();
	explicit FVaRestMultipart(FStringView InBoundary);

	FVaRestMultipart& AddField(FStringView Name, FStringView Value) &;
	FVaRestMultipart AddField(FStringView Name, FStringView Value) &&;
	FVaRestMultipart& AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &;
	FVaRestMultipart AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &&;
	/** A missing file makes GetError() non-empty, and sending the body fails with Kind=Config */
	FVaRestMultipart& AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType = TEXTVIEW("application/octet-stream"), FStringView FileName = FStringView()) &;
	FVaRestMultipart AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType = TEXTVIEW("application/octet-stream"), FStringView FileName = FStringView()) &&;

	const FString& GetBoundary() const { return Boundary; }
	const TArray<FVaRestMultipartPart>& GetParts() const { return Parts; }
	/** multipart/form-data; boundary=... */
	FString GetContentType() const;
	bool HasFiles() const;
	/** The first problem met while adding parts, or "" */
	const FString& GetError() const { return Error; }

	/** The encoded body: memory runs merged, one segment per file part */
	TArray<FVaRestBodySegment> Encode() const;

private:
	FString Boundary;
	TArray<FVaRestMultipartPart> Parts;
	FString Error;
};

/**
 * An FArchive over body segments, for IHttpRequest::SetContentFromStream, whose Serialize "will be called from another
 * thread other than the game thread" (IHttpRequest.h:250-257). The engine seeks to what it has sent and reads on
 * (HttpRequestPayload.cpp:77-96). A file that shrank since its part was added reads as zeros, with the error flag set
 * and one warning.
 */
class VARESTHTTP_API FVaRestSegmentStream final : public FArchive
{
public:
	explicit FVaRestSegmentStream(TArray<FVaRestBodySegment> InSegments);
	virtual ~FVaRestSegmentStream() override;

	virtual void Serialize(void* Data, int64 Length) override;
	virtual void Seek(int64 InPos) override;
	virtual int64 Tell() override { return Position; }
	virtual int64 TotalSize() override { return Total; }
	virtual bool Close() override;
	virtual FString GetArchiveName() const override { return TEXT("VaRest multipart body"); }

private:
	bool ReadFileSegment(int32 Index, int64 Offset, uint8* Out, int64 Count);

	TArray<FVaRestBodySegment> Segments;
	int64 Total = 0;
	int64 Position = 0;
	int32 OpenIndex = INDEX_NONE;
	TUniquePtr<IFileHandle> OpenFile;
	bool bWarned = false;
};

/** What a body is, for logs and traces (Inspector IN-1 Body.Kind) */
enum class EVaRestBodyKind : uint8
{
	None,
	Json,
	Text,
	Form,
	Multipart,
	Binary,
	/** A streamed file upload */
	Stream
};

/** A request body ready for the transport */
struct VARESTHTTP_API FVaRestRequestBody
{
	EVaRestBodyKind Kind = EVaRestBodyKind::None;
	/** The default Content-Type; a request's own Content-Type header wins (spec 3.2) */
	FString ContentType;
	/** Json, Text, Form, Binary, and Multipart without file parts */
	TArray<uint8> Bytes;
	/** Stream: the file uploaded with SetContentAsStreamedFile */
	FString FilePath;
	/** Multipart with file parts: uploaded with SetContentFromStream */
	TArray<FVaRestBodySegment> Segments;
	/** Multipart: the parts without their binary bytes (text fields keep theirs), for logs and traces */
	TArray<FVaRestMultipartPart> PartDescriptors;
	int64 ContentLength = 0;

	bool IsEmpty() const { return Kind == EVaRestBodyKind::None; }
	bool IsStreamed() const { return !FilePath.IsEmpty() || Segments.Num() > 0; }

	static FVaRestRequestBody FromBytes(TArray<uint8>&& InBytes, EVaRestBodyKind InKind, FStringView InContentType);
	static FVaRestRequestBody FromForm(const FVaRestForm& Form);
	static TValueOrError<FVaRestRequestBody, FString> FromMultipart(const FVaRestMultipart& Multipart);
	static TValueOrError<FVaRestRequestBody, FString> FromFile(FStringView InFilePath, FStringView InContentType);
};
