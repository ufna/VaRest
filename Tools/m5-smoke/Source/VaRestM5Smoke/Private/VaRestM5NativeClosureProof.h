// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** Separate opt-in cooked proof; never a typed-result getter or a product/public API. */
class FVaRestM5NativeClosureProof final
{
public:
    FVaRestM5NativeClosureProof();
    ~FVaRestM5NativeClosureProof();
    void Start();
private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
