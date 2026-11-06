# Migrating from VaRest 1.x to 2.0

VaRest 2.0 keeps the v1 classes and serialized function names in the `VaRest` module. Existing nodes remain executable compatibility nodes; they are deprecated and hidden from new-node searches. The legacy module is scheduled for removal in **v3**, not in 2.0. C++ deprecation attributes are deferred until 2.1.

This is a development version. A Blueprint fixture authored from the frozen `v1-on-5.8` API now compiles, saves and reloads on Linux Editor. It covers 108 callable functions plus the internal 109th helper through the original Make/Break Json handlers. This is a newly authored fixture, not a binary produced by the old plugin. Request behavior/lifetime and release platform checks remain open; M6 is not release-qualified.

## Migrate one flow at a time

1. Keep the existing request and JSON nodes while replacing one request flow with **Send Request**, **Get Json**, or **Call Endpoint**.
2. Use a `UVaRestApi` asset for shared environments, endpoint descriptions and policy. Obtain its client from the real GameInstance Hub; editor utilities use the engine Hub.
3. Convert a legacy object/value with **To VaRest Json**. Convert back with **To Legacy Json Object** or **To Legacy Json Value** while the rest of the graph still expects legacy wrappers.
4. Store a bridge output in a variable. Each bridge execution makes an independent deep copy. Changes do not propagate across the bridge; nested objects inside the original legacy graph keep their shared-reference behavior.
5. Connect the new **On Success**, **On Error** and cancellation paths before removing the old request object. Old **Make Json**/**Break Json** nodes keep their compiler implementation; add the v2 JSON nodes in new graphs.

| v1 concept | v2 concept |
| --- | --- |
| Construct Json Request + setters + ProcessURL | Send Request inputs and a reusable client |
| CallURL | Send Request / Get Json / Call Endpoint |
| latent ApplyURL | asynchronous completion pins |
| Construct Json Object | Make Json Object / Make Json |
| wrapper field getters/setters | Get Field / Set Field on `FVaRestJson` |
| response object used for every body | typed result, Json, String or Bytes output |
| global request state | per-GameInstance Hub and per-API client |
| request object Cancel | VaRest Request Handle Cancel |

## Behavior to account for

**JSON ownership.** Legacy `GetRootObject()` and `GetRootValue()` still expose the same `FJsonObject`/`FJsonValue` storage. v2 handle copies share their document; `Clone` and the migration bridges make independent copies. Setting a child into another v2 document copies it. Empty/default v2 handles mean missing/invalid; JSON null is a separate value. Missing getters return their supplied default and wildcard getters also report Found.

**Legacy representation.** `FJsonObject` keys compare without case, so copying v2 members such as `A` and `a` into a legacy object collapses them to the last value. v2 object keys retain their byte-exact identity. Legacy numeric getters keep their engine `double` conversion behavior; use v2 integer accessors for exact 64-bit reads. The migration bridge preserves integer number strings when converting them back to v2, while each side keeps its own representation and lookup rules.

**Parsing and files.** Both legacy parser-selection switches are retained but ignored, with a warning. All legacy decoding uses the v2 parser; malformed input clears the legacy object and reports a position without logging the raw input. The private incremental parser is removed. Legacy file writing retains its UTF-16 BOM, and legacy archive loading also accepts UTF-8. New `Load Json File` / `Save Json File` flows use the v2 file format and limits; convert old files or load them through the legacy bridge first. JSON roots that are arrays or scalars are available through the legacy response value and content-string accessors, with raw response bytes retained.

**HTTP outcomes.** Legacy `OnRequestComplete` keeps its v1 transport-completion meaning, including HTTP 4xx/5xx responses. New requests route a non-2xx status to **On Error**. Check the error kind and response status when porting that branch. The compatibility request explicitly disables auth, retries, cache and envelope processing; configure those on the v2 client/API when adopting them.

**Bodies and headers.** The compatibility facade still builds v1 form/query, binary and JSON bodies. v2 validates verbs/headers and ignores GET bodies. Custom verbs that the engine cannot send a body with fail configuration instead of silently losing that body. Form query values join an existing query correctly, and repeated execution avoids accumulating the same generated query. The native `GetHttpRequest()` remains available for the invocation; a reused/canceled invocation may get a fresh engine request. Core response streaming means native callers should read body bytes through the VaRest response accessors, rather than the engine response buffer. The SDK has no public activity-timeout getter, so a custom engine activity timeout must be reapplied when a fresh engine request is created.

**Lifetime and cancellation.** The Hub retains admitted legacy requests until logical completion; request objects no longer call `AddToRoot`. Cancel stops the native operation and invalidates late results. Hub teardown cancels retained work. Canceled/replaced latent actions retire without triggering user execution, and replacing one no longer removes unrelated actions on the same owner. Reentrant invocations use distinct epochs. Pre-admission setup failures can report failure immediately; normal native completions are marshaled to the game thread. Native `FVaRestClient` aliases remain game-thread configured and must be released before HTTP module Stop.

**Logging and settings.** Requests use v2 redaction and trace/log policy. The legacy `bExtendedLog` flag does not enable raw body logging. Configure v2 log level/redaction through VaRest HTTP settings and the client/API. The legacy `UVaRestSettings` config section is retained separately; copy intentional policy settings into v2 rather than renaming the old config section.

**Utilities.** `UVaRestUtilityLibrary` in `VaRestHttp` supplies percent encoding, Base64, version, world URL and v2 settings helpers without requiring the legacy module. New digest helpers explicitly hash UTF-8; old ANSI-based hashes can differ for non-ASCII strings. `Base64EncodeData` keeps the legacy empty-array false/empty-output convention. The replacement world URL struct is `FVaRestWorldUrl`; the old `FVaRestURL` stays in place for existing pins.

## C++ mapping

| Legacy API | v2 API |
| --- | --- |
| `UVaRestRequestJSON` | `FVaRestClient`, a request builder, `FVaRestRequestHandle` |
| `UVaRestSubsystem` request access | `UVaRestHub::Get(WorldContext)` / `GetClient(Api)` |
| `GetRootObject()` / `GetRootValue()` | unchanged for legacy storage; `ToVaRestJson()` copies into v2 |
| constructing a legacy wrapper from new data | `UVaRestJsonObject::FromVaRestJson` / `UVaRestJsonValue::FromVaRestJson` |
| typed field/value getters | `Json[Key].AsString()`, `AsDouble()`, `AsInt64()`, `TryGet<T>()` |
| request object completion state | `TVaRestResult<T>::Response` and `Error` |
| request body object | `.Body(FVaRestJson)` or a reflected native struct |
| request header setters | `.Header(Name, Value)` / client session headers |
| manual parsing | `FVaRestJson::Parse` and its value-or-error result |

```cpp
// Game thread. The Hub owns the configured client; the local alias is released on return.
auto Client = UVaRestHub::Get(WorldContext)->GetClient(Api)->GetNativeClient();
auto Handle = Client.Post<FVaRestJson>(TEXT("/pets"))
    .Body(FVaRestJson::Object().Set(TEXT("name"), TEXT("Milo")))
    .OnSuccess([](const FVaRestJson& Json) { /* consume Json */ })
    .OnError([](const FVaRestError& Error) { /* handle failure */ })
    .Send();
// Keep Handle if this flow needs cancellation. Validate Hub/Api/client admission in application code.
```

Include `VaRestJson` and `VaRestHttp` in the consuming module dependencies. Keep `VaRest` only while legacy types are still referenced. The public v1 include paths, class names, enums, delegates and function signatures remain unchanged; this migration does not rename serialized classes or repoint an existing Blueprint node to a different signature.

## Original Blueprint functions

The table covers the original reflected functions from tag `v1-on-5.8`. New migration bridges are intentionally available in the action menu.

| Legacy class/function | Replacement |
| --- | --- |
| `UVaRestJsonObject.Reset` | Make Json Object, then assign the returned handle |
| `UVaRestJsonObject.EncodeJson` | To String (Json), Pretty enabled |
| `UVaRestJsonObject.EncodeJsonToSingleString` | To String (Json) |
| `UVaRestJsonObject.DecodeJson` | Parse Json |
| `UVaRestJsonObject.GetFieldTypeString` | Get Field, then Get Json Type |
| `UVaRestJsonObject.GetFieldNames` | Get Keys (Json) |
| `UVaRestJsonObject.HasField` | Has Field (VaRest Json) |
| `UVaRestJsonObject.RemoveField` | Remove Field (VaRest Json) |
| `UVaRestJsonObject.GetField` | Get Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetArrayField` | Get Array Field (VaRest Json) |
| `UVaRestJsonObject.SetArrayField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.MergeJsonObject` | Merge Json |
| `UVaRestJsonObject.GetNumberField` | Get Number Field |
| `UVaRestJsonObject.SetNumberField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetNumberFieldDouble` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetIntegerField` | Get Int Field |
| `UVaRestJsonObject.SetIntegerField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetInt64Field` | Get Int64 Field |
| `UVaRestJsonObject.SetInt64Field` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetStringField` | Get String Field (VaRest Json) |
| `UVaRestJsonObject.SetStringField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetBoolField` | Get Bool Field (VaRest Json) |
| `UVaRestJsonObject.SetBoolField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetObjectField` | Get Object Field (VaRest Json) |
| `UVaRestJsonObject.SetObjectField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetMapFields_string` | Set Field for each map entry, or Struct To Json for a whole typed value |
| `UVaRestJsonObject.SetMapFields_uint8` | Set Field for each map entry, or Struct To Json for a whole typed value |
| `UVaRestJsonObject.SetMapFields_int32` | Set Field for each map entry, or Struct To Json for a whole typed value |
| `UVaRestJsonObject.SetMapFields_int64` | Set Field for each map entry, or Struct To Json for a whole typed value |
| `UVaRestJsonObject.SetMapFields_bool` | Set Field for each map entry, or Struct To Json for a whole typed value |
| `UVaRestJsonObject.GetNumberArrayField` | Get Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetIntegerArrayField` | Get Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetNumberArrayField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetNumberArrayFieldDouble` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetStringArrayField` | Get Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetStringArrayField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetBoolArrayField` | Get Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetBoolArrayField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.GetObjectArrayField` | Get Field with the corresponding value/array pin type |
| `UVaRestJsonObject.SetObjectArrayField` | Set Field with the corresponding value/array pin type |
| `UVaRestJsonObject.WriteToFilePath` | Save Json File (VaRest Json) |
| `UVaRestJsonValue.Reset` | clear a VaRest Json variable for an invalid handle |
| `UVaRestJsonValue.GetType` | Get Json Type |
| `UVaRestJsonValue.GetTypeString` | Get Json Type, then enum-to-string |
| `UVaRestJsonValue.IsNull` | Is Null (Json) |
| `UVaRestJsonValue.AsNumber` | As Number (Json) |
| `UVaRestJsonValue.AsInt32` | As Int (Json) |
| `UVaRestJsonValue.AsInt64` | As Int64 (Json) |
| `UVaRestJsonValue.AsString` | As String (Json) |
| `UVaRestJsonValue.AsBool` | As Bool (Json) |
| `UVaRestJsonValue.AsArray` | Array Elements (Json), or Array Get |
| `UVaRestJsonValue.AsObject` | use the VaRest Json handle directly; check Get Json Type |
| `UVaRestLibrary.GetVaRestSettings` | VaRestUtilityLibrary.GetHttpSettings (v2 settings) |
| `UVaRestLibrary.PercentEncode` | VaRestUtilityLibrary.PercentEncode |
| `UVaRestLibrary.Base64Encode` | VaRestUtilityLibrary.Base64Encode |
| `UVaRestLibrary.Base64Decode` | VaRestUtilityLibrary.Base64Decode |
| `UVaRestLibrary.Base64EncodeData` | VaRestUtilityLibrary.Base64EncodeData |
| `UVaRestLibrary.Base64DecodeData` | VaRestUtilityLibrary.Base64DecodeData |
| `UVaRestLibrary.StringToMd5` | MD5 (UTF-8) |
| `UVaRestLibrary.StringToSha1` | SHA1 (UTF-8) |
| `UVaRestLibrary.HTTPStatusIntToEnum` | v2 response Status (int), with Switch on Int |
| `UVaRestLibrary.GetVaRestVersion` | VaRestUtilityLibrary.GetVaRestVersion |
| `UVaRestLibrary.GetWorldURL` | VaRestUtilityLibrary.GetWorldUrl (FVaRestWorldUrl) |
| `UVaRestRequestJSON.SetVerb` | Send Request Verb input, or an API endpoint Verb |
| `UVaRestRequestJSON.SetCustomVerb` | Send Request Verb input |
| `UVaRestRequestJSON.SetContentType` | Send Request Body Kind input |
| `UVaRestRequestJSON.SetBinaryContentType` | Send Request Content Type input |
| `UVaRestRequestJSON.SetBinaryRequestContent` | Send Request with Binary body |
| `UVaRestRequestJSON.SetStringRequestContent` | Send Request with Text or Form body |
| `UVaRestRequestJSON.SetHeader` | Send Request Headers input, or client session headers |
| `UVaRestRequestJSON.ResetData` | start a new Send Request invocation |
| `UVaRestRequestJSON.ResetRequestData` | build a new Send Request body and headers |
| `UVaRestRequestJSON.ResetResponseData` | replace the stored response/result variable |
| `UVaRestRequestJSON.Cancel` | Cancel on VaRest Request Handle |
| `UVaRestRequestJSON.GetRequestObject` | keep the VaRest Json body passed to Send Request |
| `UVaRestRequestJSON.SetRequestObject` | Send Request Body input |
| `UVaRestRequestJSON.GetResponseObject` | Send Request JSON output |
| `UVaRestRequestJSON.SetResponseObject` | assign a VaRest Json variable |
| `UVaRestRequestJSON.GetResponseValue` | Send Request JSON output |
| `UVaRestRequestJSON.GetURL` | response metadata and the configured client/endpoint URL |
| `UVaRestRequestJSON.GetVerb` | Send Request Verb input or API endpoint Verb |
| `UVaRestRequestJSON.GetStatus` | On Success / On Error and response metadata |
| `UVaRestRequestJSON.GetResponseCode` | response Status |
| `UVaRestRequestJSON.GetResponseHeader` | response Headers |
| `UVaRestRequestJSON.GetAllResponseHeaders` | response Headers |
| `UVaRestRequestJSON.GetResponseContentLength` | response Headers or response BytesDown |
| `UVaRestRequestJSON.GetResponseContent` | Send Request Bytes output |
| `UVaRestRequestJSON.SetURL` | Send Request Path input, or an API environment BaseUrl |
| `UVaRestRequestJSON.ProcessURL` | Send Request |
| `UVaRestRequestJSON.ApplyURL` | Send Request async completion pins |
| `UVaRestRequestJSON.ExecuteProcessRequest` | Send Request |
| `UVaRestRequestJSON.AddTag` | Send Request Tag input |
| `UVaRestRequestJSON.RemoveTag` | set Tag on the next Send Request invocation |
| `UVaRestRequestJSON.HasTag` | keep application tag state; use request Tag in metadata |
| `UVaRestRequestJSON.GetResponseContentAsString` | Send Request String output, or To String (Json) |
| `UVaRestSubsystem.CallURL` | Send Request, Get Json, or Call Endpoint |
| `UVaRestSubsystem.ConstructVaRestRequest` | Send Request; use Hub.GetDefaultClient for shared client state |
| `UVaRestSubsystem.ConstructVaRestRequestExt` | Send Request with Verb and Body |
| `UVaRestSubsystem.ConstructVaRestJsonObject` | Make Json Object |
| `UVaRestSubsystem.StaticConstructVaRestJsonObject` | Make Json |
| `UVaRestSubsystem.ConstructJsonValueNumber` | Json From Number |
| `UVaRestSubsystem.ConstructJsonValueString` | Json From String |
| `UVaRestSubsystem.ConstructJsonValueBool` | Json From Bool |
| `UVaRestSubsystem.ConstructJsonValueArray` | Make Json Array and Array Add |
| `UVaRestSubsystem.ConstructJsonValueObject` | use the VaRest Json object handle directly |
| `UVaRestSubsystem.DecodeJsonValue` | Parse Json |
| `UVaRestSubsystem.DecodeJsonObject` | Parse Json, then Get Json Type |
| `UVaRestSubsystem.LoadJsonFromFile` | Load Json File (VaRest Json) |
