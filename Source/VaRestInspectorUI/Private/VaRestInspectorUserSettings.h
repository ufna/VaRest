// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreGlobals.h"
#include "Misc/ConfigCacheIni.h"
#include "VaRestInspectorViewModel.h"

/** Bounded per-user state. Mutations update the config cache; disk flush belongs to panel/module close. */
struct FVaRestInspectorUserSettings
{
	static constexpr int32 Count = static_cast<int32>(EVaRestInspectorColumn::Count);
	bool bPreserve = false, bAllScopes = true, bAscending = true;
	uint64 ScopeId = 0;
	int32 SortColumn = static_cast<int32>(EVaRestInspectorColumn::Time);
	float Split = .62f;
	FString Filter;
	float Widths[Count];
	bool Hidden[Count];
	int32 Order[Count];
	static const TCHAR* Section() { return TEXT("VaRest.Inspector.UI"); }
	static const FString& File() { return GEditorPerProjectIni; }
	FVaRestInspectorUserSettings()
	{
		for (int32 I = 0; I < Count; ++I)
		{
			Widths[I] = I == 2 || I == 3 || I == 9 ? 200 : 90;
			Hidden[I] = I == 8 || I == 9 || I == 12 || I == 13; Order[I] = I;
		}
	}
	void Load()
	{
		if (!GIsEditor || !GConfig || File().IsEmpty()) return;
		GConfig->GetBool(Section(), TEXT("PreserveLog"), bPreserve, File());
		GConfig->GetBool(Section(), TEXT("AllScopes"), bAllScopes, File());
		GConfig->GetBool(Section(), TEXT("Ascending"), bAscending, File());
		FString Id; GConfig->GetString(Section(), TEXT("ScopeId"), Id, File()); LexTryParseString(ScopeId, *Id);
		GConfig->GetString(Section(), TEXT("Filter"), Filter, File()); Filter.LeftInline(4096);
		GConfig->GetInt(Section(), TEXT("SortColumn"), SortColumn, File()); SortColumn = FMath::Clamp(SortColumn, 0, Count - 1);
		GConfig->GetFloat(Section(), TEXT("Split"), Split, File()); Split = FMath::IsFinite(Split) ? FMath::Clamp(Split, .15f, .85f) : .62f;
		bool Seen[Count]{}; bool ValidOrder = true;
		for (int32 I = 0; I < Count; ++I)
		{
			GConfig->GetFloat(Section(), *FString::Printf(TEXT("Width%d"), I), Widths[I], File());
			Widths[I] = FMath::IsFinite(Widths[I]) ? FMath::Clamp(Widths[I], 40.f, 600.f) : 90.f;
			GConfig->GetBool(Section(), *FString::Printf(TEXT("Hidden%d"), I), Hidden[I], File());
			GConfig->GetInt(Section(), *FString::Printf(TEXT("Order%d"), I), Order[I], File());
			if (Order[I] < 0 || Order[I] >= Count || Seen[Order[I]]) ValidOrder = false; else Seen[Order[I]] = true;
		}
		if (!ValidOrder) for (int32 I = 0; I < Count; ++I) Order[I] = I;
		bool AllHidden = true; for (bool Value : Hidden) AllHidden &= Value; if (AllHidden) Hidden[2] = false;
	}
	void Save(bool Flush = false) const
	{
		if (!GIsEditor || !GConfig || File().IsEmpty()) return;
		GConfig->SetBool(Section(), TEXT("PreserveLog"), bPreserve, File()); GConfig->SetBool(Section(), TEXT("AllScopes"), bAllScopes, File());
		GConfig->SetBool(Section(), TEXT("Ascending"), bAscending, File()); GConfig->SetString(Section(), TEXT("ScopeId"), *LexToString(ScopeId), File());
		GConfig->SetString(Section(), TEXT("Filter"), *Filter.Left(4096), File()); GConfig->SetInt(Section(), TEXT("SortColumn"), SortColumn, File());
		GConfig->SetFloat(Section(), TEXT("Split"), Split, File());
		for (int32 I = 0; I < Count; ++I)
		{
			GConfig->SetFloat(Section(), *FString::Printf(TEXT("Width%d"), I), Widths[I], File());
			GConfig->SetBool(Section(), *FString::Printf(TEXT("Hidden%d"), I), Hidden[I], File());
			GConfig->SetInt(Section(), *FString::Printf(TEXT("Order%d"), I), Order[I], File());
		}
		if (Flush) GConfig->Flush(false, File());
	}
};
