// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"
#include "ImageWrapper.h"

UVaRestRequestJSON::UVaRestRequestJSON(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{
	RequestVerb = ERequestVerb::GET;
	RequestContentType = ERequestContentType::x_www_form_urlencoded;

	ResetData();
}

UVaRestRequestJSON* UVaRestRequestJSON::ConstructRequest(UObject* WorldContextObject)
{
	return (UVaRestRequestJSON*)StaticConstructObject(UVaRestRequestJSON::StaticClass());
}

UVaRestRequestJSON* UVaRestRequestJSON::ConstructRequestExt(
	UObject* WorldContextObject, 
	ERequestVerb::Type Verb, 
	ERequestContentType::Type ContentType,ERequestResultType::Type ResultType)
{
	UVaRestRequestJSON* Request = ConstructRequest(WorldContextObject);

	Request->SetVerb(Verb);
	Request->SetContentType(ContentType);
	Request->SetResultType(ResultType);
	
	return Request;
}

void UVaRestRequestJSON::SetVerb(ERequestVerb::Type Verb)
{
	RequestVerb = Verb;
}

void UVaRestRequestJSON::SetContentType(ERequestContentType::Type ContentType)
{
	RequestContentType = ContentType;
}

void UVaRestRequestJSON::SetResultType(ERequestResultType::Type ResultType)
{
	RequestResultType = ResultType; 
}


void UVaRestRequestJSON::SetHeader(const FString& HeaderName, const FString& HeaderValue)
{
	RequestHeaders.Add(HeaderName, HeaderValue);
}

FString UVaRestRequestJSON::PercentEncode(const FString& Text)
{
	FString OutText = Text;

	OutText = OutText.Replace(TEXT("!"), TEXT("%21"));
	OutText = OutText.Replace(TEXT("\""), TEXT("%22"));
	OutText = OutText.Replace(TEXT("#"), TEXT("%23"));
	OutText = OutText.Replace(TEXT("$"), TEXT("%24"));
	//OutText = OutText.Replace(TEXT("&"), TEXT("%26"));
	OutText = OutText.Replace(TEXT("'"), TEXT("%27"));
	OutText = OutText.Replace(TEXT("("), TEXT("%28"));
	OutText = OutText.Replace(TEXT(")"), TEXT("%29"));
	OutText = OutText.Replace(TEXT("*"), TEXT("%2A"));
	OutText = OutText.Replace(TEXT("+"), TEXT("%2B"));
	OutText = OutText.Replace(TEXT(","), TEXT("%2C"));
	//OutText = OutText.Replace(TEXT("/"), TEXT("%2F"));
	OutText = OutText.Replace(TEXT(":"), TEXT("%3A"));
	OutText = OutText.Replace(TEXT(";"), TEXT("%3B"));
	OutText = OutText.Replace(TEXT("="), TEXT("%3D"));
	//OutText = OutText.Replace(TEXT("?"), TEXT("%3F"));
	OutText = OutText.Replace(TEXT("@"), TEXT("%40"));
	OutText = OutText.Replace(TEXT("["), TEXT("%5B"));
	OutText = OutText.Replace(TEXT("]"), TEXT("%5D"));
	OutText = OutText.Replace(TEXT("{"), TEXT("%7B"));
	OutText = OutText.Replace(TEXT("}"), TEXT("%7D"));

	return OutText;
}


//////////////////////////////////////////////////////////////////////////
// Destruction and reset

void UVaRestRequestJSON::ResetData()
{
	ResetRequestData();
}

void UVaRestRequestJSON::ResetRequestData()
{
	if (RequestJsonObj != NULL)
	{
		RequestJsonObj->Reset();
	}
	else
	{
		RequestJsonObj = (UVaRestJsonObject*)StaticConstructObject(UVaRestJsonObject::StaticClass());
	}
}

//////////////////////////////////////////////////////////////////////////
// JSON data accessors

UVaRestJsonObject* UVaRestRequestJSON::GetRequestObject()
{
	return RequestJsonObj;
}

void UVaRestRequestJSON::SetRequestObject(UVaRestJsonObject* JsonObject)
{
	RequestJsonObj = JsonObject;
}

//////////////////////////////////////////////////////////////////////////
// URL processing

void UVaRestRequestJSON::ProcessURL(const FString& Url)
{
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);

	ProcessRequest(HttpRequest);
}

void UVaRestRequestJSON::ProcessRequest(TSharedRef<IHttpRequest> HttpRequest)
{
	// Set verb
	switch (RequestVerb)
	{
	case ERequestVerb::GET:
		HttpRequest->SetVerb("GET");
		break;

	case ERequestVerb::POST:
		HttpRequest->SetVerb("POST");
		break;

	case ERequestVerb::PUT:
		HttpRequest->SetVerb("PUT");
		break;

	default:
		break;
	}

	// Set content-type
	switch (RequestContentType)
	{
	case ERequestContentType::x_www_form_urlencoded:
	{
		HttpRequest->SetHeader("Content-Type", "application/x-www-form-urlencoded");

		FString UrlParams = "";
		uint16 ParamIdx = 0;

		// Loop through all the values and prepare additional url part
		for (auto RequestIt = RequestJsonObj->GetRootObject()->Values.CreateIterator(); RequestIt; ++RequestIt)
		{
			FString Key = RequestIt.Key();
			FString Value = RequestIt.Value().Get()->AsString();

			if (!Key.IsEmpty() && !Value.IsEmpty())
			{
				UrlParams += ParamIdx == 0 ? "?" : "&";
				UrlParams += Key + "=" + Value;
			}

			ParamIdx++;
		}

		// Apply params to the url
		HttpRequest->SetURL(HttpRequest->GetURL() + UVaRestRequestJSON::PercentEncode(UrlParams));

		break;
	}

	case ERequestContentType::json:
	{
		HttpRequest->SetHeader("Content-Type", "application/json");

		// Serialize data to json string
		FString OutputString;
		TSharedRef< TJsonWriter<> > Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(RequestJsonObj->GetRootObject().ToSharedRef(), Writer);

		// Set Json content
		HttpRequest->SetContentAsString(OutputString);

		break;
	}

	default:
		break;
	}

	// Apply additional headers
	for (TMap<FString, FString>::TConstIterator It(RequestHeaders); It; ++It)
	{
		HttpRequest->SetHeader(It.Key(), It.Value());
	}
	
	// Bind event
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UVaRestRequestJSON::OnProcessRequestComplete);

	// Execute the request
	HttpRequest->ProcessRequest();
}


//////////////////////////////////////////////////////////////////////////
// Request callbacks

void UVaRestRequestJSON::OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{

	// Check we have result to process further
	if (!bWasSuccessful)
	{
		UE_LOG(LogVaRest, Error, TEXT("Request failed: %s"), *Request->GetURL());

		// Broadcast the result event
		OnRequestFail.Broadcast();

		return;
	}

	if ( RequestResultType == ERequestResultType::JSON )
	{
		UVaRestJsonObject* ResponseJsonObj  = (UVaRestJsonObject*)StaticConstructObject(UVaRestJsonObject::StaticClass());
		// Try to deserialize data to JSON
		TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(Response->GetContentAsString());
		FJsonSerializer::Deserialize(JsonReader, ResponseJsonObj->GetRootObject());

		// Decide whether the request was successful
		bool bIsValidJsonResponse = bWasSuccessful && ResponseJsonObj->GetRootObject().IsValid();

		// Log errors
		if (!bIsValidJsonResponse)
		{
			if (!ResponseJsonObj->GetRootObject().IsValid())
			{
				// As we assume it's recommended way to use current class, but not the only one,
				// it will be the warning instead of error
				UE_LOG(LogVaRest, Warning, TEXT("JSON could not be decoded!"));
			}
		}

		// Broadcast the result event
		OnRequestCompleteJSON.Broadcast(ResponseJsonObj, bIsValidJsonResponse, Response->GetContentAsString()); 
	}
	else if ( RequestResultType == ERequestResultType::TEXTURE )
	{
		UTexture2D* Texture =  ImageFactory(Response->GetContent()); 
		OnRequestCompleteTexture.Broadcast(Texture, Texture != nullptr); 
	}

}

UTexture2D* UVaRestRequestJSON::ImageFactory(TArray<uint8> Data)
{
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>( FName("ImageWrapper") );
	// Note: PNG format.  Other formats are supported
	IImageWrapperPtr ImageWrapper = ImageWrapperModule.CreateImageWrapper( EImageFormat::PNG );
	if ( ImageWrapper.IsValid() && ImageWrapper->SetCompressed( Data.GetData(), Data.Num() ) )
	{
		const TArray<uint8>* UncompressedBGRA = NULL;
		if ( ImageWrapper->GetRaw( ERGBFormat::BGRA, 8, UncompressedBGRA) )
		{
			// Create the UTexture for rendering
			UTexture2D* MyTexture = UTexture2D::CreateTransient( ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), PF_B8G8R8A8 );

			// Fill in the source data from the file
			uint8* TextureData = (uint8*)MyTexture->PlatformData->Mips[0].BulkData.Lock( LOCK_READ_WRITE );
			FMemory::Memcpy( TextureData, UncompressedBGRA->GetData(), UncompressedBGRA->Num() );
			MyTexture->PlatformData->Mips[0].BulkData.Unlock();

			// Update the rendering resource from data.
			MyTexture->UpdateResource();

			return MyTexture;
		}
	}
	// Add code here to support more formats. 
	return nullptr; 
}
