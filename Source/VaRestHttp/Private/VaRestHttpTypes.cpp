// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpTypes.h"

#include "Interfaces/IHttpResponse.h"
#include "Misc/StringBuilder.h"

void FVaRestHeaders::Set(FStringView Name, FStringView Value)
{
	int32 First = INDEX_NONE;
	for (int32 i = 0; i < Pairs.Num();)
	{
		if (FStringView(Pairs[i].Key).Equals(Name, ESearchCase::IgnoreCase))
		{
			if (First == INDEX_NONE)
			{
				First = i++;
			}
			else
			{
				Pairs.RemoveAt(i);
			}
			continue;
		}
		++i;
	}
	if (First == INDEX_NONE)
	{
		Pairs.Emplace(FString(Name), FString(Value));
	}
	else
	{
		Pairs[First].Key = FString(Name);
		Pairs[First].Value = FString(Value);
	}
}

void FVaRestHeaders::Add(FStringView Name, FStringView Value)
{
	Pairs.Emplace(FString(Name), FString(Value));
}

bool FVaRestHeaders::Remove(FStringView Name)
{
	return Pairs.RemoveAll([Name](const FPair& Pair) { return FStringView(Pair.Key).Equals(Name, ESearchCase::IgnoreCase); }) > 0;
}

const FString* FVaRestHeaders::Find(FStringView Name) const
{
	for (const FPair& Pair : Pairs)
	{
		if (FStringView(Pair.Key).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

FString FVaRestHeaders::Get(FStringView Name) const
{
	const FString* Value = Find(Name);
	return Value ? *Value : FString();
}

FVaRestHeaders FVaRestHeaders::FromLines(TConstArrayView<FString> Lines)
{
	FVaRestHeaders Out;
	for (const FString& Line : Lines)
	{
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), Colon))
		{
			continue;
		}
		const FStringView View(Line);
		Out.Add(View.Left(Colon).TrimStartAndEnd(), View.RightChop(Colon + 1).TrimStartAndEnd());
	}
	return Out;
}

FString FVaRestError::Describe() const
{
	if (!IsError())
	{
		return TEXT("no error");
	}
	TStringBuilder<512> Text;
	Text << LexToString(Kind);
	if (HttpStatus > 0)
	{
		Text << TEXT(" ") << HttpStatus;
	}
	if (!Code.IsEmpty())
	{
		Text << TEXT(" [") << Code << TEXT("]");
	}
	Text << TEXT(": ") << Message;
	if (!Path.IsEmpty())
	{
		Text << TEXT(" at ") << Path;
	}
	if (!Url.IsEmpty())
	{
		Text << TEXT(" (") << Url << TEXT(")");
	}
	return FString(Text.ToView());
}

FString FVaRestError::DefaultMessage(EVaRestErrorKind ForKind, int32 Status)
{
	switch (ForKind)
	{
	case EVaRestErrorKind::None:
		return FString();
	case EVaRestErrorKind::Canceled:
		return TEXT("the request was canceled");
	case EVaRestErrorKind::Connection:
		return TEXT("the server could not be reached");
	case EVaRestErrorKind::Timeout:
		return TEXT("the request timed out");
	case EVaRestErrorKind::Http:
		// The engine's reason phrases (IHttpResponse.h:52-104); an unlisted status reads "Unknown"
		return FString::Printf(TEXT("HTTP %d %s"), Status, *EHttpResponseCodes::GetDescription(static_cast<EHttpResponseCodes::Type>(Status)).ToString());
	case EVaRestErrorKind::Server:
		return TEXT("the server reported a failure");
	case EVaRestErrorKind::Decode:
		return TEXT("the response could not be decoded");
	case EVaRestErrorKind::Auth:
		return TEXT("authentication failed");
	case EVaRestErrorKind::Offline:
		return TEXT("offline");
	case EVaRestErrorKind::Config:
		return TEXT("the request is invalid");
	}
	return FString();
}

bool FVaRestError::IsRetryableStatus(int32 Status)
{
	switch (Status)
	{
	case 408:
	case 425:
	case 429:
	case 500:
	case 502:
	case 503:
	case 504:
		return true;
	default:
		return false;
	}
}

const TCHAR* LexToString(EVaRestErrorKind Kind)
{
	switch (Kind)
	{
	case EVaRestErrorKind::None: return TEXT("None");
	case EVaRestErrorKind::Canceled: return TEXT("Canceled");
	case EVaRestErrorKind::Connection: return TEXT("Connection");
	case EVaRestErrorKind::Timeout: return TEXT("Timeout");
	case EVaRestErrorKind::Http: return TEXT("Http");
	case EVaRestErrorKind::Server: return TEXT("Server");
	case EVaRestErrorKind::Decode: return TEXT("Decode");
	case EVaRestErrorKind::Auth: return TEXT("Auth");
	case EVaRestErrorKind::Offline: return TEXT("Offline");
	case EVaRestErrorKind::Config: return TEXT("Config");
	}
	return TEXT("Unknown");
}

const TCHAR* LexToString(EVaRestTransportFailure Failure)
{
	switch (Failure)
	{
	case EVaRestTransportFailure::None: return TEXT("None");
	case EVaRestTransportFailure::Connection: return TEXT("Connection");
	case EVaRestTransportFailure::Timeout: return TEXT("Timeout");
	case EVaRestTransportFailure::Canceled: return TEXT("Canceled");
	case EVaRestTransportFailure::Other: return TEXT("Other");
	}
	return TEXT("Unknown");
}

const TCHAR* LexToString(EVaRestLogLevel Level)
{
	switch (Level)
	{
	case EVaRestLogLevel::None: return TEXT("None");
	case EVaRestLogLevel::Basic: return TEXT("Basic");
	case EVaRestLogLevel::Headers: return TEXT("Headers");
	case EVaRestLogLevel::Body: return TEXT("Body");
	}
	return TEXT("Unknown");
}

bool VaRest::Http::IsJsonContentType(FStringView ContentType)
{
	int32 Semicolon = INDEX_NONE;
	FStringView Type = ContentType.FindChar(TEXT(';'), Semicolon) ? ContentType.Left(Semicolon) : ContentType;
	Type = Type.TrimStartAndEnd();
	return Type.Equals(TEXTVIEW("application/json"), ESearchCase::IgnoreCase) || Type.EndsWith(TEXTVIEW("+json"), ESearchCase::IgnoreCase);
}
