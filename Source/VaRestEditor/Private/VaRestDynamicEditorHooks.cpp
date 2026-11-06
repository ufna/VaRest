// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicEditorHooks.h"
#include "VaRestDynamicTypes.h"
#include "VaRestHub.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/PackageReload.h"

namespace VaRestEditor::DynamicHooks
{
namespace
{
namespace Types=VaRest::Http::DynamicTypes;
class FHooks final : public FStructureEditorUtils::INotifyOnStructChanged, public FEnumEditorUtils::INotifyOnEnumChanged
{
public:
	FHooks()
	{
		PlayHandle=FEditorDelegates::PreBeginPIE.AddLambda([](bool) { UVaRestHub::BeginPlaySessionOnGT(); });
		PackageHandle=FCoreUObjectDelegates::OnPackageReloaded.AddLambda([](EPackageReloadPhase Phase,FPackageReloadedEvent*) {
			if (Phase==EPackageReloadPhase::PrePackageLoad || Phase==EPackageReloadPhase::PrePackageFixup) Types::InvalidateOnGT();
			else if (Phase==EPackageReloadPhase::PostBatchPostGC) Types::ScheduleSettleOnGT();
		});
		FrameHandle=FCoreDelegates::OnEndFrame.AddRaw(this,&FHooks::RefreshBindings);
		RefreshBindings();
	}
	~FHooks()
	{
		// Reset all tracked carriers while old layouts/hooks still exist, before unregistering either listener base.
		Types::SetEditorHooksAvailableOnGT(false);
		UnbindEditor();
		FCoreDelegates::OnEndFrame.Remove(FrameHandle);
		FCoreUObjectDelegates::OnPackageReloaded.Remove(PackageHandle);
		FEditorDelegates::PreBeginPIE.Remove(PlayHandle);
	}
	void PreChange(const UUserDefinedStruct*,FStructureEditorUtils::EStructureEditorChangeInfo) override { Types::InvalidateOnGT(); }
	void PostChange(const UUserDefinedStruct*,FStructureEditorUtils::EStructureEditorChangeInfo) override { Types::ScheduleSettleOnGT(); }
	void PreChange(const UUserDefinedEnum*,FEnumEditorUtils::EEnumEditorChangeInfo) override { Types::InvalidateOnGT(); }
	void PostChange(const UUserDefinedEnum*,FEnumEditorUtils::EEnumEditorChangeInfo) override { Types::ScheduleSettleOnGT(); }
private:
	void UnbindEditor()
	{
		if (auto* Bound=Buffer.Get())
		{
			Bound->OnBeforeRedoUndo().Remove(BeforeUndoHandle);
			Bound->OnUndo().Remove(UndoHandle); Bound->OnRedo().Remove(RedoHandle);
		}
		if (auto* Bound=Editor.Get())
		{
			Bound->OnBlueprintPreCompile().Remove(PreCompileHandle);
			Bound->OnBlueprintCompiled().Remove(CompiledHandle);
			Bound->OnBlueprintReinstanced().Remove(ReinstancedHandle);
		}
		Buffer.Reset(); Editor.Reset();
		BeforeUndoHandle.Reset(); UndoHandle.Reset(); RedoHandle.Reset();
		PreCompileHandle.Reset(); CompiledHandle.Reset(); ReinstancedHandle.Reset();
	}
	void RefreshBindings()
	{
		check(IsInGameThread());
		auto* CurrentEditor=GEditor;
		auto* CurrentBuffer=CurrentEditor ? Cast<UTransBuffer>(CurrentEditor->Trans) : nullptr;
		if (Editor.Get()==CurrentEditor && Buffer.Get()==CurrentBuffer && CurrentBuffer) return;
		Types::SetEditorHooksAvailableOnGT(false);
		UnbindEditor();
		if (!CurrentEditor || !CurrentBuffer) return; // commandlet/startup/replaced unsupported transactor: Config
		Editor=CurrentEditor; Buffer=CurrentBuffer;
		BeforeUndoHandle=CurrentBuffer->OnBeforeRedoUndo().AddLambda([](const FTransactionContext&) { Types::InvalidateOnGT(); });
		UndoHandle=CurrentBuffer->OnUndo().AddLambda([](const FTransactionContext&,bool) { Types::ScheduleSettleOnGT(); });
		RedoHandle=CurrentBuffer->OnRedo().AddLambda([](const FTransactionContext&,bool) { Types::ScheduleSettleOnGT(); });
		PreCompileHandle=CurrentEditor->OnBlueprintPreCompile().AddLambda([](UBlueprint*) { Types::InvalidateOnGT(); });
		CompiledHandle=CurrentEditor->OnBlueprintCompiled().AddLambda([] { Types::ScheduleSettleOnGT(); });
		ReinstancedHandle=CurrentEditor->OnBlueprintReinstanced().AddLambda([] { Types::ScheduleSettleOnGT(); });
		const bool bBound=BeforeUndoHandle.IsValid() && UndoHandle.IsValid() && RedoHandle.IsValid() &&
			PreCompileHandle.IsValid() && CompiledHandle.IsValid() && ReinstancedHandle.IsValid() && PackageHandle.IsValid();
		Types::SetEditorHooksAvailableOnGT(bBound,[this] {
			return Editor.IsValid() && Buffer.IsValid() && GEditor==Editor.Get() && GEditor->Trans==Buffer.Get();
		}); // checked again at activation/finalization/Take, including replacement between end-frame hooks
	}
	TWeakObjectPtr<UEditorEngine> Editor;
	TWeakObjectPtr<UTransBuffer> Buffer;
	FDelegateHandle FrameHandle,PackageHandle,BeforeUndoHandle,UndoHandle,RedoHandle,PreCompileHandle,CompiledHandle,ReinstancedHandle,PlayHandle;
};
// Like StructListener: explicit module lifecycle, no static destructor calling already-destroyed editor managers.
FHooks* Hooks=nullptr;
}
void Start() { check(IsInGameThread()); if (!Hooks) Hooks=new FHooks(); }
void Stop() { check(IsInGameThread()); delete Hooks; Hooks=nullptr; }
}
