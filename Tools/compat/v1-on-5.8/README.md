# Original v1 Blueprint fixtures

These two serialized Blueprints were authored with the unmodified `v1-on-5.8` provider, commit `2a5b51b7700cecd55a6ef64add3ad0623f5ce2a1`, on Linux Editor UE `5.8.1-0+UE5` (`1.1-r34`). Their exact bytes, 26 original source identities, five additional producer sources, native image identities and actual qualification facts are in [provenance.json](provenance.json).

`BP_FromTagV1` retains all 108 callable legacy function graph identities and the original Make/Break JSON nodes. The producer compiles it and executes the `CustomProjection` event, asserting `kept`. The internal 109th function belongs to that compiler handler. This phase does not execute all 108 events.

`BP_RequestFromTagV1` contains `PrepareLocalGet` and `StartLocalGet` events and Blueprint completion/failure counters. Its producer only authors, compiles and saves it. The v2 consumer must perform the real request separately.

Both author phases passed in separate fresh, private UserDir processes with zero errors and unforced Editor exit0. Startup warnings are retained. These are original-provider assets authored on UE5.8; the producer sources are an additional Editor test module. The prior failed compile and mount-order attempts remain recorded in recovery evidence.

## Reproduce and consume

Use a fresh ordinary project with the 26 tag source/descriptor members under `Plugins/VaRest` and these five producer files under `Plugins/VaRestLegacyTagFixture`. Enable both descriptors, build against the matching UE5.8 SDK after inspecting its full action graph, then run each producer phase in a separate new UserDir:

- `^VaRest.Compat.OriginalTagProducer.Generate$`, with `-VaRestLegacyOriginalProducerPhase=Generate -unattended -nowrite`.
- `^VaRest.Compat.OriginalTagRequestProducer.GenerateRequest$`, with `-VaRestLegacyOriginalProducerPhase=GenerateRequest -unattended -nowrite`.

The files use the serialized package root `/VaRestM6Compat_2a5b51b7/`. Retain their names and copy their bytes into a fresh current-consumer UserDir under `Saved/VaRestM6Compat_2a5b51b7/`; do not resave a fixture before its load test. Current-v2 entry points are:

- `^VaRest.Compat.TagFixture.Load$` with `-VaRestLegacyTagFixturePhase=Load -unattended -nowrite`.
- `^VaRest.Compat.OriginalTagRequest.LoadAndGet$` with `-VaRestLegacyOriginalRequestConsumer -VaRestLegacyRequestConsumerUrl=http://127.0.0.1:PORT/varest-original-fixture -unattended -nowrite`.

The request consumer needs an owned loopback server returning exactly `{"producer":"v1-on-5.8","value":17}` and independent evidence of exactly one GET. The current-v2 consumer C++ and these two consumer runs are unbuilt/unrun at the Source419 checkpoint.

## Distribution boundary

The fixtures live outside `Content`. The parent plugin descriptor stops ordinary nested plugin discovery, and `Config/FilterPlugin.ini` explicitly excludes `/Tools/compat/...`. Ordinary final BuildPlugin filtering omits these nine test files. BuildPlugin first copies the whole plugin tree into its temporary HostProject; that intermediate copy may contain them. Actual final distribution and cooked file inventories remain separate release checks.
