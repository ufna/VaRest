// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "VaRestOpenApiFactory.generated.h"

/** Import/reimport remains separate from creating an empty API contract in VaRestEditorTools. */
UCLASS()
class VARESTOPENAPI_API UVaRestOpenApiFactory : public UFactory
{
	GENERATED_BODY()
public:
	UVaRestOpenApiFactory();
	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(UClass* InClass,UObject* InParent,FName InName,EObjectFlags Flags,const FString& Filename,const TCHAR* Parms,FFeedbackContext* Warn,bool& bOutOperationCanceled) override;
	virtual bool CanImportBeCanceled() const override { return true; }
	virtual FText GetDisplayName() const override;
	/** Shared GT importer for the file factory and the controlled URL menu/scripting path. */
	UObject* CreateFromSource(UClass* InClass,UObject* InParent,FName InName,EObjectFlags Flags,const FString& Source,bool bUrl,bool& bOutOperationCanceled);
};
