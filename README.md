# VaRest 2.0

HTTP, JSON, WebSocket and Socket.IO for Unreal Engine, with C++ and Blueprint APIs.

This branch is **2.0.0-dev**, targeting **Unreal Engine 5.8**. Linux Editor, selected packaged Development and Shipping Blueprint checks, and real Socket.IO reference interop have measured passes. Remaining platform and release qualification continue; see the [development passport](docs/passport/2026-11-04-varest2-development.md) for their scope and limits.

## What is in 2.0

- `FVaRestJson`: UTF-8 parsing, document-backed values, JSON writing and struct conversion with naming rules.
- HTTP request builders and API assets: typed results, cancellation, retries, auth refresh, envelopes, caching and a durable offline queue.
- GameInstance Hubs for game clients and sockets; an engine Hub for editor utilities.
- VaRest's own WebSocket transport, routed messages, request/reply, heartbeat, reconnect and send queues.
- Socket.IO namespaces, events, acknowledgements and binary attachments over WebSocket.
- Network Inspector: HTTP attempts, socket frames, namespace state, statistics, redacted cURL and HAR export.
- OpenAPI JSON import, generated types and endpoints, previews, reports and controlled reimport.
- Deprecated 1.x Blueprint nodes retained for existing assets. New graphs use the 2.0 nodes.

## Current limits

- OpenAPI import accepts JSON. YAML and Swagger 2.0 input need conversion before import.
- Socket.IO uses WebSocket transport; HTTP polling is not implemented. WebRTC is outside the 2.0 scope.
- Reflected response conversion uses the game-thread path. Reflected worker/cooked-worker modes remain disabled pending transfer qualification.
- The explicit streaming UTF-8 typed decoder remains opt-in. The nominal 10 MB pilot (9,448,081 B) misses the 1.5x default-switch condition; ordinary typed responses retain DOM conversion.
- HTTP `CONNECT` requests are refused before transport.

## Get started

1. Put this repository in your project's `Plugins/VaRest` directory and enable VaRest.
2. Regenerate project files and build your Unreal Editor target with a compatible C++ toolchain.
3. In Blueprint, use **Get Json** or **Send Request**, or create a `VaRestApi` asset and use **Call Endpoint**. Bind success, error and cancellation paths.
4. Open **Tools → Debug → VaRest Network Inspector** to inspect captured traffic.

For C++, add the modules you use to your project's `Build.cs`: `VaRestJson`, `VaRestHttp`, `VaRestWebSocket`, or `VaRestSocketIO`. The legacy `VaRest` module is needed only by compatibility code.

```cpp
#include "VaRestClient.h"
#include "VaRestJson.h"

// Configure and own the client on the game thread.
FVaRestClient Client = FVaRestClient::Make(TEXTVIEW("https://api.example.test"));
FVaRestRequestHandle Request = Client.Get<FVaRestJson>(TEXTVIEW("/profile"))
    .OnSuccess([](const FVaRestJson& Profile) { /* consume the JSON */ })
    .OnError([](const FVaRestError& Error) { /* handle the failure */ })
    .Send();
```

Keep client state in a game-thread owner and release native client aliases before HTTP module shutdown. A GameInstance Hub supplies game-scoped clients; a standalone `Make()` client uses process scope 0.

## Guides

- [2.0 APIs, ownership, sockets and OpenAPI](docs/dev/varest2.md)
- [Network Inspector](docs/dev/inspector.md)
- [HTTP cache, offline queue and connectivity](docs/dev/cache-offline.md)
- [Migrating existing 1.x projects](docs/MIGRATION.md)
- [Development and qualification status](docs/passport/2026-11-04-varest2-development.md)
- [Local development host](docs/dev/host-project.md)

The descriptor declares Win64, Mac, Linux, Android and iOS runtime modules. The live Inspector window is desktop-only; runtime capture and HAR export are separate from that UI. A descriptor entry does not establish a successful native build on each platform.

## License

See [LICENSE](LICENSE). Unreal and Unreal Engine are trademarks or registered trademarks of Epic Games, Inc.

