// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiMerge.h"
#include "VaRestOpenApiAnalysis.h"

void FVaRestOpenApiMergePlan::ResolveChoices(TConstArrayView<FVaRestOpenApiMergeItem> Mine,TConstArrayView<FVaRestOpenApiMergeItem> Theirs)
{
	using namespace VaRest::OpenApi;TStringMap<const FVaRestOpenApiMergeItem*> Live,Upstream;
	for (const auto& Item:Mine) Live.Add(Item.Id,&Item);for (const auto& Item:Theirs) Upstream.Add(Item.Id,&Item);
	TStringMap<TArray<const FVaRestOpenApiPropertyChange*>> ByItem;for (const auto& Change:Changes) ByItem.FindOrAdd(Change.Id).Add(&Change);
	Deferred=Conflicts=0;
	for (auto& Item:Items)
	{
		const auto* LiveItem=Live.Find(Item.Id);const auto* NewItem=Upstream.Find(Item.Id);
		Item.bPresent=LiveItem && (*LiveItem)->bPresent;
		if (Item.bPresent) Item.Target=(*LiveItem)->Target;
		Item.Properties=Item.bPresent ? (*LiveItem)->Properties.Clone() : NewItem ? (*NewItem)->Properties.Clone() : Item.Properties.Clone();
		const auto* Rows=ByItem.Find(Item.Id);if (!Rows) continue;
		for (const auto* ChangePtr:*Rows)
		{
			const auto& Change=*ChangePtr;
			Deferred+=Change.bDeferred;Conflicts+=Change.Change==EVaRestOpenApiChange::Conflict;
			if (Change.Property.IsEmpty())
			{
				if (Change.Change==EVaRestOpenApiChange::Added || Change.Change==EVaRestOpenApiChange::Restore || Change.Change==EVaRestOpenApiChange::Suppressed)
				{
					Item.bPresent=Change.bApply && !Change.bDeferred && !Change.bSuppress;
					if (Item.bPresent) Item.Properties=Change.Theirs.Clone();
				}
				else if (Change.bApply && !Change.bDeferred) Item.Properties=Change.Theirs.Clone();
			}
			else if (Change.bApply && !Change.bDeferred) Item.Properties.Set(Change.Property,Change.Theirs);
		}
	}
	// A suppressed/deferred missing parent cannot have restored children without a physical target.
	TStringMap<bool> Present;for (const auto& Item:Items) Present.Add(Item.Id,Item.bPresent);
	for (int32 Pass=0;Pass<Items.Num();++Pass)
	{
		bool Changed=false;
		for (auto& Item:Items) if (Item.bPresent && !Item.Owner.IsEmpty()) if (const auto* Owner=Present.Find(Item.Owner); Owner && !*Owner)
		{ Item.bPresent=false;Present.Add(Item.Id,false);Changed=true; }
		if (!Changed) break;
	}
}
TArray<FVaRestOpenApiMergeItem> FVaRestOpenApiMergePlan::NextBase(TConstArrayView<FVaRestOpenApiMergeItem> Previous,TConstArrayView<FVaRestOpenApiMergeItem> Theirs) const
{
	using namespace VaRest::OpenApi;TArray<FVaRestOpenApiMergeItem> Out;
	TStringMap<const FVaRestOpenApiMergeItem*> Old,Upstream;
	for (const auto& Item:Previous) Old.Add(Item.Id,&Item);for (const auto& Item:Theirs) Upstream.Add(Item.Id,&Item);
	TStringMap<TArray<const FVaRestOpenApiPropertyChange*>> DeferredRows;for (const auto& Change:Changes) if (Change.bDeferred) DeferredRows.FindOrAdd(Change.Id).Add(&Change);
	for (const auto& Applied:Items)
	{
		const auto* Before=Old.Find(Applied.Id);const auto* NewItem=Upstream.Find(Applied.Id);
		FVaRestOpenApiMergeItem Item=NewItem ? **NewItem : Applied;Item.Target=Applied.Target;
		Item.Properties=Item.Properties.Clone();bool EntirelyDeferred=false;
		const auto* Rows=DeferredRows.Find(Applied.Id);
		if (Rows) for (const auto* ChangePtr:*Rows)
		{
			const auto& Change=*ChangePtr;
			if (Change.Property.IsEmpty()) { EntirelyDeferred=true;break; }
			if (Before) Item.Properties.Set(Change.Property,(*Before)->Properties[Change.Property]);else Item.Properties.Remove(Change.Property);
		}
		if (EntirelyDeferred) { if (Before) Out.Add(**Before);continue; }
		Out.Add(MoveTemp(Item));
	}
	return Out;
}
