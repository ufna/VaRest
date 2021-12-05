// Alan Verbilo 2021 for VaRest plugin.

#include "VaRestRequestAsync.h"

UVaRestRequestAsync* UVaRestRequestAsync::VaRestRequestAsync(FString URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, FString RequestMessage)
{
	UVaRestRequestAsync* object = NewObject<UVaRestRequestAsync>();

	object->URL = URL;
	object->Verb = Verb;
	object->ContentType = ContentType;
	object->RequestMessage = RequestMessage;
	
	return object;
}

void UVaRestRequestAsync::Activate()
{
	// Create request
	UVaRestSubsystem* VaRestSubsystem = GEngine->GetEngineSubsystem<UVaRestSubsystem>();
	UVaRestRequestJSON* RequestJSON = VaRestSubsystem->ConstructVaRestRequestExt(Verb, ContentType);
	UVaRestJsonObject* JsonObject = RequestJSON->GetRequestObject();
	JsonObject->DecodeJson(RequestMessage);

	// Bind to delegates
	RequestJSON->OnRequestComplete.AddDynamic(this, &UVaRestRequestAsync::OnSuccessResend);
	RequestJSON->OnRequestFail.AddDynamic(this, &UVaRestRequestAsync::OnFailureResend);

	// Start request
	RequestJSON->ProcessURL(URL);
}

void UVaRestRequestAsync::OnSuccessResend(class UVaRestRequestJSON* RequestJSON)
{
	OnSuccess.Broadcast(RequestJSON->GetResponseCode(), RequestJSON->GetResponseContentAsString(false));
	RemoveFromRoot();
}

void UVaRestRequestAsync::OnFailureResend(class UVaRestRequestJSON* RequestJSON)
{
	OnFail.Broadcast(RequestJSON->GetResponseCode(), RequestJSON->GetResponseContentAsString(false));
	RemoveFromRoot();
}