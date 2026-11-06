// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiFactory.h"
#include "EditorReimportHandler.h"
#include "VaRestOpenApiReimportFactory.generated.h"

UCLASS()
class VARESTOPENAPI_API UVaRestOpenApiReimportFactory : public UVaRestOpenApiFactory,public FReimportHandler
{
	GENERATED_BODY()
public:
	UVaRestOpenApiReimportFactory();
	virtual bool FactoryCanImport(const FString&) override { return false; }
	virtual bool CanReimport(UObject* Object,TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Object,const TArray<FString>& NewPaths) override;
	virtual void SetReimportPaths(UObject* Object,const FString& NewPath,int32 SourceIndex) override;
	virtual EReimportResult::Type Reimport(UObject* Object) override;
	virtual int32 GetPriority() const override { return 102; }
};
