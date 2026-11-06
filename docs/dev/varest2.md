# VaRest 2.0 API guide

This guide describes the development source. Native compilation and runtime qualification are tracked in the [development passport](../passport/2026-11-04-varest2-development.md).

## JSON

Use `VaRestJson` for `FVaRestJson` and struct conversion. A value holds its document alive. Copying a handle shares that document; `Clone()` creates an independent tree. Inserting a value from another document copies its contents. Missing/invalid values and JSON null are distinct.

```cpp
#include "VaRestJson.h"

FVaRestJson User = FVaRestJson::Object();
User.Set(TEXT("userId"), int64(42));
User.Set(TEXT("name"), TEXT("Player"));
FString Encoded = User.ToString();
auto Parsed = FVaRestJson::Parse(FStringView(Encoded));
if (Parsed.HasValue())
{
    const int64 UserId = Parsed.GetValue()[TEXT("userId")].AsInt64();
}
```

Blueprint exposes **Parse Json**, **Make Json**, field/array operations, struct conversion and file helpers. Use conversion rules for aliases, naming and explicit field mappings. Cooked field names must come from supported rules/field maps rather than editor-only property metadata.

The explicit streaming UTF-8 typed decoder is a performance candidate. Ordinary typed responses retain DOM conversion until semantic checks and the specified 10 MiB gate pass. The safe game-thread path remains the route for reflected results; worker/cooked-worker reflected modes are disabled pending transfer qualification.

## HTTP and API assets

Add `VaRestHttp`. Use `FVaRestClient::Make()` for a native process-scoped client, or obtain an API/default client from `UVaRestHub::Get(WorldContextObject)`. A real GameInstance gets its own Hub and scope. Editor utilities use the engine Hub; scope 0 alone does not provide GameInstance isolation.

The native builder supports typed `Get`, `Post` and `Request`, bodies/headers, callbacks and a cancellation handle. Call `Send()` once on the completed builder. Normal completions reach the game thread asynchronously. Blueprint requests expose result and error pins; non-2xx HTTP status follows the error path.

Configure endpoints, environments, policy, rules and socket entries in a `UVaRestApi` asset. Use **Call Endpoint** or its API client. Asset client configuration and callback/UObject ownership belong to the game thread. Cancel/retire owned operations during teardown and release strong native client aliases before HTTP module Stop.

The pipeline combines custom outer stages with offline handling, cache, retries, auth gating, the attempt limiter, auth application, custom per-attempt stages, logging and transport. [Cache/offline documentation](cache-offline.md) covers freshness, identity scopes, queue admission, cancellation and asynchronous Flush. Durable queue admission is a queued result, not a fabricated HTTP success.

## WebSocket

Add `VaRestWebSocket`. VaRest uses its own client transport on engine-bundled libwebsockets and does not depend on Unreal's `WebSockets` module.

Use **Make Socket** for an ad hoc connection or **Get Socket** for an API entry. Check the error, bind lifecycle/message/error delegates, then **Connect**. Open transport and application readiness are separate states. Configure routing and `ReadyWhen` if a welcome or authentication message must precede ready sends.

Use text, bytes, JSON or struct sends, routed Emit, and request/reply nodes as appropriate. Check the send result: queue admission does not promise delivery. Reconnect, heartbeat, queue policy, limits and auth placement belong to configuration. Request/reply has its own cancellation/timeout. **Close**, **Stop Reconnecting** and **Reset Session** express different lifecycle decisions.

Builds sharing libwebsockets with Unreal's WebSockets module can issue one warning when a nonempty query meets Verbose logging or an unobservable engine log level. This is a conservative signal: harmless query parameters can also trigger it. The message uses generic text without including connection parameters.

Native entry points are `FVaRestSocket` and `UVaRestSocketHub`; `UVaRestSocketClient` is the Blueprint view. Retain subscription/cancellation state according to its owning game object. Runtime, route/platform and unresolved production resolver gates remain separate from this API inventory.

## Socket.IO

Add `VaRestSocketIO`. Use **Make Socket.IO** or **Get Socket.IO**, bind namespace connect/disconnect/event delegates, then connect. Socket.IO uses Engine.IO/Socket.IO framing over WebSocket; HTTP polling is outside this implementation.

Namespaces share the underlying connection. An empty namespace argument selects the configured default. Use Emit and Emit With Ack, choosing argument mode deliberately. Positional arguments and binary attachments have distinct framing and limits. Use event argument/attachment accessors rather than treating the whole frame as ordinary JSON. Reply through the supplied acknowledgement value.

API assets describe entries, namespace defaults and auth. Public namespace/session information shown in Inspector is scrubbed at capture. Recovery metadata is internal protocol state.

## OpenAPI import

`VaRestOpenApi` is an Editor module. Import an OpenAPI JSON file through Content Browser, or use `UVaRestOpenApiLibrary::ImportFromFile` / `ImportFromUrl` from editor utilities. Choose an unused destination package such as `/Game/Backend/Api`.

The preview controls operations and type policy. Review diagnostics and the apply plan before accepting changes. Import creates API/type/rule assets and marks packages dirty; interactive import does not silently save them. Unsupported YAML or Swagger input produces conversion diagnostics.

Reimport compares source, previous generated state and local edits, preserving asset/member identities where supported. Resolve conflicts in the preview. Breaking edits and cleanup of removed declarations have separate actions; review referencers before deleting assets.

URL import uses the editor engine Hub with controlled fetches and redirects. Personal fetch headers live in editor preferences rather than the generated API asset. Changing a recorded URL does not issue a request; reimport performs the fetch.

The commandlet accepts these alternatives:

```text
-run=VaRestOpenApi -Check=/Game/Backend/Api
-run=VaRestOpenApi -Reimport=/Game/Backend/Api
-run=VaRestOpenApi -Import=/absolute/path/openapi.json -Dest=/Game/Backend/Api
```

Check reports drift without modifying packages. Import/reimport can save changed packages in the commandlet workflow. Exit code 0 means success/no drift, 1 reports a failed operation, diagnostics or drift, and 2 reports invalid arguments. URL, undo/package and cooked behavior still needs release qualification.

## Existing projects

The `VaRest` module retains serialized v1 class/function identities. Its Blueprint functions are deprecated and hidden from new-node searches. Convert one flow at a time and use explicit deep-copy bridges while old wrapper objects remain. See the [migration guide](../MIGRATION.md) for replacements and behavior differences.

HTTP CONNECT proxy tunnels are unsupported. Native requests and Blueprint request actions refuse CONNECT in any letter case with nonretryable `Config`, before creating a transport attempt. API endpoint validation reports `Endpoint.UnsupportedVerb`; other valid custom method tokens remain supported.

When the game replaces a refresh token while keeping the access token during an active refresh, VaRest invalidates the old answer immediately and starts one replacement refresh on a later game-thread task with the original provider. Parked requests and `RefreshAuth` callers wait for that replacement; rapid token edits coalesce to the latest token. Late success or rejection for the old token cannot replace or expire the current session.

Cancellation returns a limiter grant held by a custom PerAttempt stage on a later game-thread task. If that stage has started a physical call, logical cancellation completes after the current call ends even when the stage still owns its outcome callback. Late stage answers share the original grant’s release claim and cannot free another request’s slot.
