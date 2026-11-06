# Native owner: actual WebSocket module copy feasibility

2026-11-05 · VaRest-gi0.5.3 · read-only current-artifact/source audit.

The frozen proof plan's full-WebSocket-module A/B arrangement does not currently satisfy its constructor/unload prerequisite. Stop before loading either additional copy. This finding concerns the producer arrangement; the Source271 passive ABI0 implementation and its two passing bounded-output/current-origin observations remain valid within their recorded scope.

## Current primary evidence

The inspected artifact is the actual Source271/normal221 `Binaries/Linux/libUnrealEditor-VaRestWebSocket.so` and its declared `.debug` product. Its current hashes, the exact eight inspected SDK source/artifact paths and subsequent Core binding observation are preserved under `/w/Unreal/VaRest-recovery/m5-native-01a1a188/native-owner-constructor-feasibility-001`. This investigation did not load another module, invoke a loader, inspect a foreign process, compile, modify SDK sources or alter generated code.

`normal-221/module-initializer-symbol-observation.json` records the real dynamic table and symbols. INIT_ARRAY is at ELF VA0x582700, size120: fifteen entries. The final relative relocation targets0x3020d0, `_GLOBAL__sub_I_Module.VaRestWebSocket.cpp`. Its disassembly in `normal-221/module-main-initializer-disassembly.txt` calls the actual imported reflection batch `RegisterCompiledInInfo` fourteen times between0x302175 and0x302439, then package registration at0x302467. The following instructions construct automation tests and register their destructors through `__cxa_atexit`. These callbacks concern existing `/Script/VaRestWebSocket` types and tests, independently of native owner activation.

The installed CoreUObject public constructor `FRegisterCompiledInInfo` (`UObjectBase.h:688`) forwards immediately to registration. `UObjectBase.cpp:782` passes each generated callback, name, and registration-info address into the class/struct/enum registries. `DeferredRegistry.h:95` keys by package/name. With reload enabled it writes `*ExistingInfo = &InInfo` even for unchanged type versions, then stores callback/name/info pointers in its registrations. The class specialization also clears new singleton pointers. These are pointers into the newly loaded image, not a loader reference or a per-copy lifetime lease. Without reload it still appends raw callback/info pointers.

Normal UObject removal is connected to ModuleManager's `OnModulesUnloaded` event (`UObjectBase.cpp:1057–1063`) and is keyed by script packages/module names. A raw `dlclose` of a directly opened copy does not identify that copy to this protocol. Removing `/Script/VaRestWebSocket` through the normal event cannot be assumed to restore only one duplicate image while retaining the live default module's objects. This audit does not call removal events or change that registry.

There is a second concrete lifetime conflict in the test-enabled artifacts. Installed `AutomationTest.cpp:363` refuses a duplicate test name and keeps the existing instance. Every `FAutomationTestBase` destructor nevertheless unregisters by name (`:173`), and `UnregisterAutomationTest` removes the current entry without an instance comparison (`:374`). The current Core DSO observation independently locates the destructor call to `UnregisterAutomationTest` and the latter's map Remove call. Therefore unloading a duplicate test object cannot be assumed to preserve the default registration merely because its own duplicate registration was rejected.

## Disposition

The accepted proof plan section6 explicitly requires the actual module's duplicate constructors/destructors to be safe under unchanged engine/module policy and requires a stop if that cannot be shown. The registration paths above prevent establishing that prerequisite for ordinary full-module A/B direct-open/direct-close. This is a static supported refusal. No duplicate-load crash or actual registry corruption was executed or measured.

Keeping additional caller credits forever, stripping generated files, disabling runtime tests, suppressing UObject registration, creating an unapproved small replacement DSO, or manually broadcasting global package removal does not discharge the frozen arrangement. The existing normal ModuleManager historical E/P limitation is separate and also remains open.

The immutable source mechanism/ABI2 documents remain unchanged. A producer/deployment revision requires an explicit new what/why/cost decision, independent review and a narrow implementation/proof scope. VaRest-81s.21 owns that decision. The current source restriction is no further full-owner implementation or A/B load under the failed arrangement; ordinary unrelated feature work continues.
