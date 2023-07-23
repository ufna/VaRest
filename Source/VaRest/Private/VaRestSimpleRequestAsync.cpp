// Alan Verbilo 2021-2023 for VaRest plugin.

#include "VaRestSimpleRequestAsync.h"

UVaRestSimpleRequestAsync* UVaRestSimpleRequestAsync::VaRestSimpleRequestAsync(const FString URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, const FString RequestMessage)
{
	UVaRestSimpleRequestAsync* object = NewObject<UVaRestSimpleRequestAsync>();

	object->URL = URL;
	object->Verb = Verb;
	object->ContentType = ContentType;
	object->RequestMessage = RequestMessage;

	return object;
}

void UVaRestSimpleRequestAsync::Activate()
{
	// Create request
	UVaRestSubsystem* VaRestSubsystem = GEngine->GetEngineSubsystem<UVaRestSubsystem>();
	UVaRestRequestJSON* RequestJSON = VaRestSubsystem->ConstructVaRestRequestExt(Verb, ContentType);
	UVaRestJsonObject* JsonObject = RequestJSON->GetRequestObject();
	JsonObject->DecodeJson(RequestMessage);

	// Bind to delegates
	RequestJSON->OnRequestComplete.AddDynamic(this, &UVaRestSimpleRequestAsync::OnSuccessResend);
	RequestJSON->OnRequestFail.AddDynamic(this, &UVaRestSimpleRequestAsync::OnFailureResend);

	// Start request
	RequestJSON->ProcessURL(URL);
}

void UVaRestSimpleRequestAsync::OnSuccessResend(class UVaRestRequestJSON* RequestJSON)
{
	// Unbind delegates
	RequestJSON->OnRequestComplete.RemoveDynamic(this, &UVaRestSimpleRequestAsync::OnSuccessResend);
	RequestJSON->OnRequestFail.RemoveDynamic(this, &UVaRestSimpleRequestAsync::OnFailureResend);

	OnSuccess.Broadcast(RequestJSON->GetResponseCode(), RequestJSON->GetResponseContentAsString());
}

void UVaRestSimpleRequestAsync::OnFailureResend(class UVaRestRequestJSON* RequestJSON)
{
	// Unbind delegates
	RequestJSON->OnRequestComplete.RemoveDynamic(this, &UVaRestSimpleRequestAsync::OnSuccessResend);
	RequestJSON->OnRequestFail.RemoveDynamic(this, &UVaRestSimpleRequestAsync::OnFailureResend);

	OnFail.Broadcast(RequestJSON->GetResponseCode(), RequestJSON->GetResponseContentAsString());
}