# VaRest Network Inspector

Inspector captures VaRest HTTP requests, WebSocket connections and Socket.IO activity. `VaRestInspector` is the runtime module; `VaRestInspectorUI` supplies the desktop developer UI. The new source compiles on the guarded Linux Editor target; UI/export runtime qualification remains open.

## Open and capture

Open **Tools → VaRest Network Inspector** (the Debug section) or run `VaRest.Inspector.Open`. Desktop Development games open their own window. Null rendering, unattended execution and unavailable Slate do not create a live panel.

**Record/Pause** changes capture, not network operations. **Clear** clears records/statistics and may reattach live work as partial records. **Preserve log** keeps closed PIE scopes across the next PIE session. The selector distinguishes game/editor scopes from process scope 0; unknown context stays unknown.

Project Settings → Plugins → VaRest Inspector controls capture, headers/bodies and limits. Packaged capture defaults off; `-VaRestInspector` enables the development capture route. Shipping compiles capture/export out: the compiled-in query returns false, capture toggle does nothing and export fails. UI is excluded from Shipping/Test.

| Default | Editor | Packaged development |
| --- | --- | --- |
| Store budget | 64 MiB | 8 MiB |
| Records | 10000 | 2000 |
| HTTP excerpt | 65536 bytes | 16384 bytes |
| Text frame excerpt | 4096 bytes | 1024 bytes |
| Frames per socket | 5000 | 1000 |
| Binary excerpt | disabled | disabled |

Limits govern admitted storage, not process RSS. Pending events, snapshots, UI presentation and export copies have separate limits/admission. Refusal/loss and partial capture remain visible; captures are not lossless recordings.

`MaxBinaryExcerptBytes` defaults to 0: binary bodies and frames have descriptions without captured content. A positive value opts into a capped binary prefix; HTTP uses this cap independently of the text body cap, while frames also obey the frame cap. Known secret values are removed before capture. Binary field names cannot be interpreted as redaction keys, so unknown secrets may remain in opted-in content.

## Find a request

Kind buttons select HTTP, WebSocket, Socket.IO, errors or markers. The shared filter supports AND, quoted terms and `-` negation:

```text
status:4xx method:POST -is:cached
scope:"Client 1" slower-than:500ms
is:socket larger-than:10k
```

Other terms include `id:`, `client:`, `endpoint:`, `initiator:`, `tag:` and `is:running/queued/retried/auth/sio`. Header sorting uses numeric counters/time. Columns can be hidden, resized and reordered through Columns. Live tail resumes arrival order and stops on upward scrolling or another sort.

Editor preferences use per-project user configuration under `[VaRest.Inspector.UI]`. Desktop games use defaults without writing editor preferences.

## HTTP details

Overview shows outcome, identity, initiator, context, timing and policy metadata. Request shows captured URL/query and the selected physical attempt's redacted headers; **As built** selects the earlier builder headers. Response separates delivered result from the selected network attempt. Missing delivered preview stays unavailable; a 304 or cache path does not borrow another body.

Timing shows captured phases/gaps; unknown timings stay unknown. Auth and queue replay links open related requests. Temporary reveal can show a linked request outside filters; changing filter/scope/kind ends it. Missing, cross-process and evicted targets have labels.

Body provides Raw, Pretty JSON, Hex and a lazy JSON tree. Pretty uses native colored text runs up to 64 KiB, with plain text beyond that, without interpreting captured text as markup. Copy tree paths/values or excerpts; truncated JSON cannot become a complete tree. **Save excerpt** writes captured bytes only. Editor initiator navigation opens an already loaded Blueprint node or the recorded C++ source location.

Hex and binary **Save excerpt** require an actually captured binary prefix. A description never becomes file content or a hex dump of its label. HAR keeps opted-in binary in explicit Base64 fields; it does not decode those bytes as text.

## Connections and frames

Connection shows attempts, upgrade headers, protocols, close/failure/reconnect, flow/backpressure and current Socket.IO namespace state. Namespace public id/recovery/reason is a census separate from bounded recent notes.

Frames shows direction, kind, channel/namespace, route, message id, protocol, byte count, lane and attachment sizes. Synthetic rows mark attempts, open/close, drops and request completion. Select a frame for its body or follow a retained request/reply or event/ack peer.

```text
dir:in kind:text route:orders ns:/game
-is:control id:42
is:autopong
```

Frame filtering also supports quoted/free text and negation. Engine.IO traffic is hidden by default; toggle it and other controls explicitly. **Follow** stops on upward scrolling. Pending, skipped, stopped, timed-out and evicted excerpt states stay distinct.

## Statistics

Stats follows scope independently of list text filters. The sortable table combines HTTP scope/client/endpoint groups and scope/socket groups. **Copy table (TSV)** preserves displayed order, sanitizes cell separators and marks truncation.

HTTP groups expose outcomes, attempts, retries, auth, cache, wire bytes and log-bucket percentile intervals. Socket groups expose attempts/reconnects, opens/closes/failures, frames/bytes, drops and average observed RTT. Counters survive row/frame eviction. Socket identities are bounded to 256 groups, or fewer in a reduced store, plus unattributed overflow. Recent duplicate windows are bounded. Partial capture excludes unseen history; overflow is excluded from a selected scope.

## Export and copy

**Export HAR** selects all captured traffic, filtered rows or the selected request/socket. An owned native copy is made on the game thread and written by a worker; completion offers **Show in folder**. Admission can fail under snapshot/copy budgets. At most two exports run concurrently; default logical DTO limit is 32 MiB and output limit is 64 MiB.

HAR has one entry per HTTP/network attempt, synthetic no-dispatch entries, and Chrome WebSocket entries per retained connection attempt. Orphan frames keep their attempt identity. `_varest` carries partial/timing/availability, close fields, frame descriptors and namespace state. Binary messages have empty data by default; captured opt-in bytes use annotated base64. Missing UTC is marked; status phrases are synthesized. Chrome import still needs manual qualification.

**Copy as cURL** generates Bash text for a captured HTTP attempt without executing it. Comments describe unavailable/truncated bodies and earlier request excerpts. Socket copy offers URL, socket id and selected frame excerpt.

Core applies redaction at capture; export/clipboard use that view. Review captures before sharing. Binary capture requires explicit opt-in.

## Console and Blueprint

```text
VaRest.Inspector.Capture 0|1
VaRest.Inspector.Clear [-all]
VaRest.Inspector.List [count] [filter] [-all]
VaRest.Inspector.Show <id|http:id|socket:id> [-all]
VaRest.Inspector.Stats [-all]
VaRest.Inspector.Export [path] [-all]
```

World console commands use the current scope unless `-all` is supplied. Bare Show ids can match both identity namespaces; prefixes disambiguate. Show bounds frame output; HAR includes all retained frames within export limits. Default exports go to `Saved/VaRest/Inspector/`.

Blueprint exposes compiled-in/capture controls and asynchronous **Export Inspector HAR** with done/failed outputs. World context supplies scope; `bAllScopes` selects a complete capture. The owning Hub/game instance controls lifetime and cancellation.
