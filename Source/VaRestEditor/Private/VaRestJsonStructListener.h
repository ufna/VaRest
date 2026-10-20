// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

namespace VaRestEditor::StructListener
{
	/** From FVaRestEditorModule::StartupModule: drop the struct <-> JSON field-map cache whenever a Blueprint struct changes */
	void Start();

	/** From FVaRestEditorModule::ShutdownModule (which does not run when an automation run quits) */
	void Stop();
}
