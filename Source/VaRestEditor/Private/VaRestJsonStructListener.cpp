// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonStructListener.h"

#include "Kismet2/StructureEditorUtils.h"
#include "VaRestJsonConvert.h"

namespace VaRestEditorStructListenerPrivate
{
	/**
	 * Drops the struct <-> JSON field-map cache whenever a Blueprint struct changes. A recompiled struct keeps its
	 * UObject (UserDefinedStructureCompilerUtils.cpp:595) but destroys its properties (:222-225), so a cached map
	 * would point at freed FProperty objects. PreChange runs before the old properties go (:599), PostChange after the
	 * new ones exist (:661); resetting at both leaves no map built from a half-compiled struct.
	 */
	class FStructListener final : public FStructureEditorUtils::INotifyOnStructChanged
	{
	public:
		virtual void PreChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override
		{
			FVaRestJsonConvert::ResetCache();
		}

		virtual void PostChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override
		{
			FVaRestJsonConvert::ResetCache();
		}
	};

	/**
	 * A raw pointer on purpose: it has no exit-time destructor. ShutdownModule does not run when an automation run quits,
	 * and a static owner (TUniquePtr) would then delete the listener during static destruction, possibly after the
	 * manager it unregisters from -- a function-local static (StructureEditorUtils.cpp:32-36) -- if our StartupModule was
	 * its first use. Left registered at exit it is harmless: the manager's destructor frees its TSet (ListenerManager.h:36)
	 * and calls no listener.
	 */
	FStructListener* GListener = nullptr;
}

void VaRestEditor::StructListener::Start()
{
	using namespace VaRestEditorStructListenerPrivate;
	if (!GListener)
	{
		GListener = new FStructListener(); // registers itself (ListenerManager.h:27)
	}
}

void VaRestEditor::StructListener::Stop()
{
	using namespace VaRestEditorStructListenerPrivate;
	delete GListener; // unregisters itself (ListenerManager.h:31); UnrealEd is still loaded (we depend on it)
	GListener = nullptr;
}
