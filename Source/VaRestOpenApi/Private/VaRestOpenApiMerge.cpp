// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiMerge.h"
#include "VaRestOpenApiAnalysis.h"

namespace
{
using namespace VaRest::OpenApi;
bool Breaking(const FVaRestOpenApiMergeItem& Item,const FString& Property,const FVaRestJson& Before,const FVaRestJson& After)
{
	if (Equal(Item.Kind,TEXT("field")) || Equal(Item.Kind,TEXT("param")))
		return Equal(Property,TEXT("type")) || (Equal(Item.Kind,TEXT("param")) && Equal(Property,TEXT("required")) && !Before.AsBool() && After.AsBool());
	if (Equal(Item.Kind,TEXT("endpoint"))) return Equal(Property,TEXT("requestType")) || Equal(Property,TEXT("responseType")) || Equal(Property,TEXT("errorType"));
	return false;
}
bool Suppressed(const FString& Id,const FVaRestOpenApiMergeOptions& Options)
{
	return Options.SuppressedIds.ContainsByPredicate([&](const FString& Value) { return Equal(Value,Id); });
}
FVaRestOpenApiPropertyChange Row(const FVaRestOpenApiMergeItem& Item,const FString& Property,EVaRestOpenApiChange Change)
{
	FVaRestOpenApiPropertyChange Out;Out.Kind=Item.Kind;Out.Id=Item.Id;Out.Owner=Item.Owner;Out.Property=Property;Out.Change=Change;return Out;
}
FVaRestJson RemovedProperties(const FVaRestOpenApiMergeItem& Item)
{
	auto Properties=Item.Properties.Clone();
	FString Tooltip=Item.Properties[TEXT("tooltip")].AsString();if (!Tooltip.Contains(TEXT("[Removed upstream"),ESearchCase::CaseSensitive)) Tooltip+=TEXT("\n[Removed upstream]");
	if (Equal(Item.Kind,TEXT("field"))) Properties.Set(TEXT("presence"),TEXT("Ignore")).Set(TEXT("tooltip"),Tooltip);
	else if (Equal(Item.Kind,TEXT("param"))) Properties.Set(TEXT("removed"),true);
	else if (Equal(Item.Kind,TEXT("endpoint"))) Properties.Set(TEXT("deprecated"),true).Set(TEXT("deprecationMessage"),TEXT("Removed from the API"));
	else if (Equal(Item.Kind,TEXT("struct")) || Equal(Item.Kind,TEXT("enum"))) Properties.Set(TEXT("tooltip"),Tooltip);
	// Enum values are retained with their numeric identity. No removal/renumbering operation exists here.
	return Properties;
}
}
bool FVaRestOpenApiMergePlan::HasWrites() const
{
	for (const auto& Row:Changes) if (Row.bApply || Row.bDeferred) return true;return false;
}
TArray<FVaRestOpenApiMergeItem> FVaRestOpenApiMerge::Base(const FVaRestJson& Manifest)
{
	TArray<FVaRestOpenApiMergeItem> Out;
	for (const auto& Row:Manifest[TEXT("items")].Elements())
	{
		FVaRestOpenApiMergeItem Item;Item.Kind=Row[TEXT("kind")].AsString();Item.Id=Row[TEXT("id")].AsString();Item.Owner=Row[TEXT("owner")].AsString();Item.Target=Row[TEXT("target")].AsString();Item.Properties=Row[TEXT("base")];Item.bRemovedUpstream=Row[TEXT("removedUpstream")].AsBool();Out.Add(MoveTemp(Item));
	}
	return Out;
}
FVaRestJson FVaRestOpenApiMerge::ItemsJson(TConstArrayView<FVaRestOpenApiMergeItem> Items)
{
	auto Rows=FVaRestJson::Array();for (const auto& Item:Items) Rows.Add(FVaRestJson::Object().Set(TEXT("kind"),Item.Kind).Set(TEXT("id"),Item.Id).Set(TEXT("owner"),Item.Owner).Set(TEXT("target"),Item.Target).Set(TEXT("base"),Item.Properties).Set(TEXT("removedUpstream"),Item.bRemovedUpstream));return Rows;
}
FVaRestOpenApiMergePlan FVaRestOpenApiMerge::Compare(TConstArrayView<FVaRestOpenApiMergeItem> Old,TConstArrayView<FVaRestOpenApiMergeItem> Mine,TConstArrayView<FVaRestOpenApiMergeItem> Theirs,const FVaRestOpenApiMergeOptions& Options)
{
	using namespace VaRest::OpenApi;FVaRestOpenApiMergePlan Out;
	TStringMap<const FVaRestOpenApiMergeItem*> OldById,MineById,NewById;
	for (const auto& Item:Old) OldById.Add(Item.Id,&Item);for (const auto& Item:Mine) MineById.Add(Item.Id,&Item);for (const auto& Item:Theirs) NewById.Add(Item.Id,&Item);
	for (const auto& Desired:Theirs)
	{
		const auto* OldPtr=OldById.Find(Desired.Id);const auto* LivePtr=MineById.Find(Desired.Id);
		const auto* Previous=OldPtr ? *OldPtr : nullptr;const auto* Live=LivePtr && (*LivePtr)->bPresent ? *LivePtr : nullptr;
		FVaRestOpenApiMergeItem Merged=Desired;if (Live) Merged.Target=Live->Target;else if (Previous) Merged.Target=Previous->Target;
		if (Suppressed(Desired.Id,Options) && !Live)
		{
			Merged.bPresent=false;auto Change=Row(Desired,{},EVaRestOpenApiChange::Suppressed);Change.Theirs=Desired.Properties;Out.Changes.Add(MoveTemp(Change));Out.Items.Add(MoveTemp(Merged));continue;
		}
		if (!Previous || !Live)
		{
			auto Change=Row(Desired,{},Previous ? EVaRestOpenApiChange::Restore : EVaRestOpenApiChange::Added);Change.Theirs=Desired.Properties;
			Change.bBreaking=Equal(Desired.Kind,TEXT("param")) && Desired.Properties[TEXT("required")].AsBool() && OldById.Contains(Desired.Owner);
			Change.bDeferred=Options.bAutomated && Change.bBreaking && !Options.bAllowBreaking;Change.bApply=!Change.bDeferred;
			if (Change.bDeferred) { ++Out.Deferred;Merged.bPresent=false; }Out.Changes.Add(MoveTemp(Change));Out.Items.Add(MoveTemp(Merged));continue;
		}
		Merged.Properties=Live->Properties.Clone();
		// Desired property order first; obsolete base-only properties follow their saved order.
		TArray<FString> Order=Desired.Properties.GetKeys();for (const auto& Member:Previous->Properties.Members()) if (!Desired.Properties.Has(Member.GetKey())) Order.Add(Member.GetKey());
		for (const auto& Key:Order)
		{
			const auto Before=Previous->Properties[Key],Current=Live->Properties[Key],Upstream=Desired.Properties[Key];
			const bool UserChanged=!Current.Equals(Before),UpstreamChanged=!Upstream.Equals(Before);
			if (!UserChanged && !UpstreamChanged) continue;
			if (Current.Equals(Upstream)) continue;
			auto Change=Row(Desired,Key,UserChanged ? (UpstreamChanged ? EVaRestOpenApiChange::Conflict : EVaRestOpenApiChange::KeptUserEdit) : EVaRestOpenApiChange::Changed);
			Change.Base=Before;Change.Mine=Current;Change.Theirs=Upstream;Change.bBreaking=Breaking(Desired,Key,Before,Upstream);
			Change.bDeferred=!UserChanged && Options.bAutomated && Change.bBreaking && !Options.bAllowBreaking;
			Change.bApply=!UserChanged && UpstreamChanged && !Change.bDeferred;
			if (Change.Change==EVaRestOpenApiChange::Conflict) ++Out.Conflicts;
			if (Change.bDeferred) ++Out.Deferred;
			if (Change.bApply) Merged.Properties.Set(Key,Upstream);
			Out.Changes.Add(MoveTemp(Change));
		}
		Out.Items.Add(MoveTemp(Merged));
	}
	for (const auto& Previous:Old) if (!NewById.Contains(Previous.Id))
	{
		const auto* LivePtr=MineById.Find(Previous.Id);const auto* Live=LivePtr && (*LivePtr)->bPresent ? *LivePtr : nullptr;
		FVaRestOpenApiMergeItem Merged=Live ? *Live : Previous;Merged.bPresent=Live!=nullptr;Merged.bRemovedUpstream=true;
		auto Change=Row(Previous,{},EVaRestOpenApiChange::Removed);Change.Base=Previous.Properties;if (Live) Change.Mine=Live->Properties;
		Change.Theirs=Live ? RemovedProperties(*Live) : FVaRestJson();Change.bApply=Live && !Change.Theirs.Equals(Live->Properties);
		if (Change.bApply) Merged.Properties=Change.Theirs;
		Out.Changes.Add(MoveTemp(Change));Out.Items.Add(MoveTemp(Merged));
	}
	if (Options.bAutomated)
	{
		FStringSet BlockedEndpoints,BlockedFields;
		for (const auto& Change:Out.Changes) if (Change.bBreaking && (Change.bDeferred || Change.Change==EVaRestOpenApiChange::Conflict))
		{
			if (Equal(Change.Kind,TEXT("endpoint"))) BlockedEndpoints.Add(Change.Id);
			else if (Equal(Change.Kind,TEXT("param"))) BlockedEndpoints.Add(Change.Owner);
			else if (Equal(Change.Kind,TEXT("field"))) BlockedFields.Add(Change.Id);
		}
		bool GlobalEnvelope=false;
		if (!BlockedEndpoints.IsEmpty()) for (const auto& Change:Out.Changes) if (Equal(Change.Kind,TEXT("envelope")) && Change.bApply) { GlobalEnvelope=true;break; }
		if (GlobalEnvelope) for (const auto& Item:Out.Items) if (Equal(Item.Kind,TEXT("endpoint"))) BlockedEndpoints.Add(Item.Id);
		for (auto& Change:Out.Changes) if (Change.bApply)
		{
			const bool EndpointRuntime=Equal(Change.Kind,TEXT("endpoint")) && BlockedEndpoints.Contains(Change.Id) &&
				!(Equal(Change.Property,TEXT("name")) || Equal(Change.Property,TEXT("category")) || Equal(Change.Property,TEXT("description")) || Equal(Change.Property,TEXT("deprecated")) || Equal(Change.Property,TEXT("deprecationMessage")));
			const bool ParamRuntime=Equal(Change.Kind,TEXT("param")) && BlockedEndpoints.Contains(Change.Owner) && !Equal(Change.Property,TEXT("description")) && !Equal(Change.Property,TEXT("pin"));
			const bool FieldRuntime=Equal(Change.Kind,TEXT("field")) && BlockedFields.Contains(Change.Id) && !Equal(Change.Property,TEXT("name")) && !Equal(Change.Property,TEXT("tooltip"));
			if (EndpointRuntime || ParamRuntime || FieldRuntime || (GlobalEnvelope && Equal(Change.Kind,TEXT("envelope")))) { Change.bApply=false;Change.bDeferred=true; }
		}
	}
	Out.ResolveChoices(Mine,Theirs);return Out;
}
