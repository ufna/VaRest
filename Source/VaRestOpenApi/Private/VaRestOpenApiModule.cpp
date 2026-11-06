// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "Modules/ModuleManager.h"
#include "MessageLogModule.h"
#include "ToolMenus.h"
#include "VaRestOpenApiMenus.h"
#include "VaRestOpenApiUrlTransport.h"
#include "VaRestOpenApiUrlSource.h"

class FVaRestOpenApiModule final : public IModuleInterface
{
	FDelegateHandle MenuStartup;
public:
	virtual void StartupModule() override
	{
		FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog")).RegisterLogListing(TEXT("VaRestOpenAPI"),NSLOCTEXT("VaRestOpenApi","LogName","VaRest OpenAPI"));
		MenuStartup=UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&VaRest::OpenApi::RegisterSourceMenus));
	}
	virtual void ShutdownModule() override
	{
		VaRest::OpenApi::CancelUrlSourcesOnGT();VaRest::OpenApi::ShutdownSourceTransport();
		UToolMenus::UnRegisterStartupCallback(MenuStartup);UToolMenus::UnregisterOwner(TEXT("VaRestOpenApi"));
		if (auto* Log=FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog"))) Log->UnregisterLogListing(TEXT("VaRestOpenAPI"));
	}
	virtual bool SupportsDynamicReloading() override { return false; }
};
IMPLEMENT_MODULE(FVaRestOpenApiModule, VaRestOpenApi)
