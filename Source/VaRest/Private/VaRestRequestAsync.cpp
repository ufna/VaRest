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
	VaRestSubsystem = GEngine->GetEngineSubsystem<UVaRestSubsystem>();
	JSONRequest = VaRestSubsystem->ConstructVaRestRequestExt(Verb, ContentType);
	JsonObject = JSONRequest->GetRequestObject();
	JsonObject->DecodeJson(RequestMessage);
	
	JSONRequest->OnRequestComplete.AddDynamic(this, &UVaRestRequestAsync::OnSuccessResend);
	JSONRequest->OnRequestFail.AddDynamic(this, &UVaRestRequestAsync::OnFailureResend);
	
	JSONRequest->ProcessURL(URL);
}

void UVaRestRequestAsync::OnSuccessResend(class UVaRestRequestJSON* RequestJSON)
{
	OnSuccess.Broadcast(RequestJSON->GetResponseContentAsString());
	RemoveFromRoot();
}

void UVaRestRequestAsync::OnFailureResend(class UVaRestRequestJSON* RequestJSON)
{
	OnFail.Broadcast(RequestJSON->GetResponseContentAsString());
	RemoveFromRoot();
}
