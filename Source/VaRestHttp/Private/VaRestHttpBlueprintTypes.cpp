// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpBlueprintTypes.h"

#include "VaRestAuth.h"

FString VaRest::Http::UnsignedIdText(uint64 Value)
{
	return LexToString(Value);
}

bool VaRest::Http::TryParseUnsignedId(FStringView Text, uint64& OutValue)
{
	if (Text.IsEmpty() || (Text.Len() > 1 && Text[0] == TEXT('0')))
	{
		return false;
	}
	uint64 Value = 0;
	for (TCHAR Character : Text)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		const uint64 Digit = Character - TEXT('0');
		if (Value > (MAX_uint64 - Digit) / 10)
		{
			return false;
		}
		Value = Value * 10 + Digit;
	}
	OutValue = Value;
	return true;
}

bool VaRest::Http::TryParseGeneration(FStringView Text, uint32& OutValue)
{
	uint64 Value;
	if (!TryParseUnsignedId(Text, Value) || Value > MAX_uint32)
	{
		return false;
	}
	OutValue = static_cast<uint32>(Value);
	return true;
}

FVaRestResponseData FVaRestResponseData::FromNative(const FVaRestResponse& Response)
{
	FVaRestResponseData Result;
	Result.RequestId = VaRest::Http::UnsignedIdText(Response.RequestId);
	Result.Status = Response.Status;
	Result.Headers = VaRest::Http::ToHeaderData(Response.Headers);
	Result.EffectiveUrl = Response.EffectiveUrl;
	Result.ContentType = Response.ContentType;
	Result.Elapsed = Response.Elapsed;
	Result.Attempts = Response.Attempts;
	Result.bFromCache = Response.bFromCache;
	Result.Body = Response.Body;
	Result.SavedFile = Response.SavedFile;
	for (const auto& Warning : Response.DecodeWarnings)
	{
		FVaRestDecodeIssue Issue;
		Issue.Path = Warning.Path;
		Issue.Message = Warning.Message;
		Result.DecodeWarnings.Add(MoveTemp(Issue));
	}
	Result.BytesUp = Response.BytesUp;
	Result.BytesDown = Response.BytesDown;
	return Result;
}

FVaRestQueuedResultData FVaRestQueuedResultData::FromNative(const FVaRestQueuedResult& Result)
{
	FVaRestQueuedResultData Data;
	Data.QueueId = Result.QueueId;
	Data.Tag = Result.Tag;
	Data.bSuccess = Result.bSuccess;
	Data.Response = FVaRestResponseData::FromNative(Result.Response);
	Data.Error = Result.Error;
	return Data;
}

FVaRestSessionDataView FVaRestSessionDataView::FromNative(const FVaRestSessionData& Session)
{
	FVaRestSessionDataView Result;
	Result.IdentityId = Session.IdentityId;
	Result.AccessToken = Session.AccessToken;
	Result.RefreshToken = Session.RefreshToken;
	Result.ExpiresAt = Session.ExpiresAt;
	Result.Lifetime = Session.Lifetime;
	Result.Headers = VaRest::Http::ToHeaderData(Session.Headers);
	Result.Generation = VaRest::Http::UnsignedIdText(Session.Generation);
	Result.CredentialGeneration = VaRest::Http::UnsignedIdText(Session.CredentialGeneration);
	return Result;
}

FVaRestAuthTargetData FVaRestAuthTargetData::FromNative(const FVaRestAuthTarget& Target)
{
	FVaRestAuthTargetData Result;
	Result.Session = FVaRestSessionDataView::FromNative(Target.Session);
	Result.Headers = VaRest::Http::ToHeaderData(Target.Headers);
	for (const auto& Pair : Target.Query)
	{
		FVaRestQueryParameter Param;
		Param.Name = Pair.Key;
		Param.Value = Pair.Value;
		Result.Query.Add(MoveTemp(Param));
	}
	Result.RequestId = VaRest::Http::UnsignedIdText(Target.RequestId);
	return Result;
}

TValueOrError<FVaRestMultipart, FString> FVaRestMultipartData::ToNative() const
{
	for (const auto& Part : Parts)
	{
		if ((Part.bField && (!Part.Bytes.IsEmpty() || !Part.FilePath.IsEmpty() || !Part.FileName.IsEmpty() || !Part.ContentType.IsEmpty())) || (!Part.bField && (!Part.FieldValue.IsEmpty() || (!Part.FilePath.IsEmpty() && !Part.Bytes.IsEmpty()))))
		{
			return MakeError(FString(TEXT("a multipart part must select exactly one field, bytes or file source")));
		}
	}
	FVaRestMultipart Result;
	for (const auto& Part : Parts)
	{
		if (Part.bField)
		{
			Result.AddField(Part.Name, Part.FieldValue);
		}
		else if (!Part.FilePath.IsEmpty())
		{
			Result.AddFile(Part.Name, Part.FilePath, Part.ContentType.IsEmpty() ? FStringView(TEXT("application/octet-stream")) : FStringView(Part.ContentType), Part.FileName);
		}
		else
		{
			Result.AddBytes(Part.Name, Part.FileName, Part.ContentType.IsEmpty() ? FStringView(TEXT("application/octet-stream")) : FStringView(Part.ContentType), Part.Bytes);
		}
		if (!Result.GetError().IsEmpty())
		{
			return MakeError(Result.GetError());
		}
	}
	return MakeValue(MoveTemp(Result));
}
