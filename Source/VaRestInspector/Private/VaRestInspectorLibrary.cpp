// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorLibrary.h"
#include "VaRestInspector.h"
#include "VaRestInspectorContext.h"
#include "VaRestHub.h"
#include "VaRestEngineSubsystem.h"
#include "VaRestGameInstanceSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/Paths.h"

bool UVaRestInspectorLibrary::IsInspectorCompiledIn() { return IVaRestInspector::IsCompiledIn(); }
bool UVaRestInspectorLibrary::SetInspectorCapture(bool Enabled)
{
	check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
	auto* Inspector = IVaRestInspector::TryGet(); if (!Inspector) return false; Inspector->SetCapture(Enabled); return Inspector->IsCapturing() == Enabled;
#else
	return false;
#endif
}
bool UVaRestInspectorLibrary::IsInspectorCapturing()
{
	check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
	const auto* Inspector = IVaRestInspector::TryGet(); return Inspector && Inspector->IsCapturing();
#else
	return false;
#endif
}
namespace VaRest::Inspector
{
UVaRestHub* ExistingHub(const UObject* Context)
{
	check(IsInGameThread()); if (!GEngine || !IsValid(Context)) return nullptr;
	const UWorld* World = GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull);
	if (World && World->bIsTearingDown) return nullptr;
	if (World) if (const auto* GI = World->GetGameInstance())
		if (const auto* Subsystem = GI->GetSubsystem<UVaRestGameInstanceSubsystem>()) return Subsystem->GetHub();
	const auto* Subsystem = GEngine->GetEngineSubsystem<UVaRestEngineSubsystem>(); return Subsystem ? Subsystem->GetHub() : nullptr;
}
FString DefaultHarPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VaRest/Inspector"), FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".har"));
}
}
