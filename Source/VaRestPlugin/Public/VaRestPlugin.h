// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class UVaRestRequestController;
class UVaRestSettings;

/**
 * The public interface to this module.  In most cases, this interface is only public to sibling modules 
 * within this plugin.
 */
class IVaRestPlugin : public IModuleInterface
{

public:
	/**
	 * Singleton-like access to this module's interface.  This is just for convenience!
	 * Beware of calling this during the shutdown phase, though.  Your module might have been unloaded already.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed
	 */
	static inline IVaRestPlugin& Get()
	{
		return FModuleManager::LoadModuleChecked<IVaRestPlugin>("VaRestPlugin");
	}

	/**
	 * Checks to see if this module is loaded and ready.  It is only valid to call Get() if IsAvailable() returns true.
	 *
	 * @return True if the module is loaded and ready to use
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VaRestPlugin");
	}

	/** Getter for internal settings object to support runtime configuration changes */
	UVaRestSettings* GetSettings() const;

	/** Get global request controller */
	UVaRestRequestController* GetRequestController(UWorld* World) const;

protected:
	/** Module settings */
	UVaRestSettings* VaRestSettings;

	/** Request controllers (one for each World we have) */
	TMap<UWorld*, UVaRestRequestController*> RequestControllers;
};
