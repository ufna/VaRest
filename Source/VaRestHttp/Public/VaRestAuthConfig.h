// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestApiTypes.h"

#include "VaRestAuthConfig.generated.h"

class IVaRestAuth;
class UVaRestApi;

/** Asset-owned provider configuration. Provider construction cannot observe or mutate a partially configured client. */
UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced)
class VARESTHTTP_API UVaRestAuthConfig : public UObject
{
	GENERATED_BODY()

public:
	/** A new native provider/weak adapter. Apply/Refresh execute only after the real client has been configured. */
	virtual TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> CreateAuth() const PURE_VIRTUAL(UVaRestAuthConfig::CreateAuth, return nullptr;);
	virtual void Validate(FVaRestApiValidationContext& Context) const PURE_VIRTUAL(UVaRestAuthConfig::Validate, );
	/** Duplicated per-wrapper configs retain only a weak API context for resolving authored endpoint names. */
	void SetRuntimeApiOnGT(const UVaRestApi* Api);
	const UVaRestApi* GetApiOnGT() const;
	/** Wrapper closes native admissions first, then drops any GT-owned BP refresh payload. */
	virtual void RetireOnGT() {}
private:
	TWeakObjectPtr<const UVaRestApi> RuntimeApi;
};
