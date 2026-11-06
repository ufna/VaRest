// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Http
{
class FClientState;
class FNativeScopeLifetime;
namespace TracePrivate
{
/** GT attachment stack only. No deferred task owns a managed client or scope snapshot. */
void HoldSnapshotScopeOnGT(TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope);
void HoldManagedSnapshotClientOnGT(TSharedRef<FClientState, ESPMode::ThreadSafe> Client);
}
}
