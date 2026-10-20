# VaRest 2 — OpenAPI / Swagger import design

Status: **revision 1**, 2026-10-20. Revision 0 was reviewed adversarially
(`.superpowers/sdd/openapi-spec-review.md`: 2 Critical, 13 Important, 11 Minor); this revision applies the
controller's rulings R1–R8 (binding, §19) and every Critical and Important finding, and disposes of the Minor ones
in Appendix B. One of the four 2.0 extras (core §0.2). Target: Unreal Engine 5.8.1, branch `v2`. Builds on
`2026-10-18-varest2-core-design.md` ("the core"); § numbers prefixed with *core* refer to it. Written before core
M5 on purpose: §17 is the tight list of what M5 keeps and what the import adds, to be folded into the M5 plan.
Bead: VaRest-brb.

---

## 0. Context

### 0.1 Intent

A designer drags `openapi.yaml` (or pastes `http://localhost:8000/openapi.json`) into the Content Browser
and gets, in one step: an API asset with environments, auth and every endpoint; a Blueprint struct for every
schema and an enum for every string enum; and a *Call Endpoint* node per operation with typed path/query/header
pins, a typed body and a typed response — including `Array of S_Pet` for list endpoints. When the backend
changes, *Reimport* shows what changed, keeps what the designer edited, and never silently breaks a graph.

What success looks like:

* Petstore (3.0 and 3.1; Swagger 2.0 as a stretch, §15) imports with zero errors and zero manual steps. For every
  example in the spec: decode → encode → decode gives an `Identical` struct, and every key present with a
  non-default value — **including an optional enum's first value** — is written back with its exact spelling.
* Wire names and presence are exact and **stay exact** after the designer renames a field or an enum display name,
  in PIE and in a packaged game (a cooked-build test proves it, §14.3), whether or not a *Call Endpoint* ran first.
* Import and reimport leave nothing in the undo buffer that could later revert them (§8.5).
* Reimporting an unchanged spec dirties no package. Reimporting a changed one keeps user edits, flags breaking
  changes before applying (automated reimports defer them), and leaves removed items in place, deprecated.
* GitHub's public spec (≈10 MB JSON, ~1000 operations and schemas) imports within the budget gated in O3 step 0
  (§13), and a designer can import just the tags they need.

### 0.2 What already exists (verified in the engine)

**No YAML parser is usable from a module.** LLVM's `YAMLParser.cpp` exists only inside ShaderConductor's
DirectXShaderCompiler sources (not a module); `yaml-cpp` only as static libraries linked into OpenColorIO
(`OpenColorIOLib.Build.cs:37-58`, no headers); PyYAML only as a pip requirement of two experimental Python
plugins (`HairCardGenerator.uplugin:65`, `MLflow.uplugin:46`). §2.3 decides.

**Epic's experimental WebAPI plugin** (`Plugins/Experimental/Web/WebAPI`, `"IsExperimentalVersion": true`)
imports OpenAPI 2/3, but: JSON only (`WebAPIOpenAPIFactoryBase.cpp:12-18`; the `yaml` constant at `:8` is never
used for import), it sniffs a file by parsing all of it with `FJsonSerializer` (`WebAPIOpenAPIFactory.cpp:17-26`),
and it generates **C++ code through an external LiquidJS app** launched as a process
(`WebAPILiquidJSProcess.cpp:30-37`) and then compiled. VaRest generates assets usable immediately in Blueprint,
reads YAML, needs no external process, and merges on reimport. Its factory claims `.json` at priority 101
(`WebAPIDefinitionFactory.cpp:39`), which §11.1 accounts for.

**Epic's DataLink JSON→struct generator** (`Plugins/VirtualProduction/DataLink/.../DataLinkJsonEditorStructGenerator.cpp`)
is the precedent for building Blueprint structs in bulk through exported API: it empties and fills
`FStructureEditorUtils::GetVarDesc` (`:141`, `:171`), compiles once (`:208`), and wraps generation in a transaction
it cancels (`:44-54`). §8 follows it, with two corrections: VaRest keeps the engine's `<Name>_<n>_<GUID>` member
name (DataLink writes `<Name>_<GUID>`, `:136`, which the cooked name strip turns from `user_id` into `user`,
`UserDefinedStruct.cpp:300-313`), and VaRest self-checks every compile (§8.1).

**Facilities this design uses** (each cited where it matters): `UFactory` import with additional objects
(`Factory.h:140-144`), `FReimportHandler` (`EditorReimportHandler.h:206-265`), `UAssetImportData` with
multi-file MD5-tracked sources (`AssetImportData.h:28-46`, `:93-117`), `UAssetImportTask` for scripted import
(`AssetImportTask.h:36-99`), `FStructureEditorUtils` / `FEnumEditorUtils`, the cooked struct-member GUID map
(`UUserDefinedStruct::PropertyIDs`, §6.2), property bags (§17 N5), `FMessageLog` (`MessageLog.h:20`), the
DataValidation plugin (`EditorValidatorBase.h:27-30`, enabled by default, `DataValidation.uplugin:13`), the
`/Temp/` in-memory mount for tests (`PackageName.cpp:808`).

### 0.3 Non-goals (2.0)

* **AsyncAPI** (sockets). The WebSocket spec §11 says the importer does not fill `Sockets`. Normalize is separate
  from plan/apply, so an AsyncAPI front end can reuse plan, apply, manifest and merge later.
* Runtime validation of schema constraints (`pattern`, `minLength`, `maximum`…). They go into tooltips.
* Polymorphic decoding into a "union" struct (oneOf with discriminator). Variants are generated; the field is
  `Json` and the designer converts with *Json To Struct* (§6.5).
* Per-endpoint auth configs, browser OAuth flows (authorization code / PKCE), cookie auth, mutual TLS (§7.7).
* Callbacks, webhooks, links, streamed responses (3.2 `itemSchema`), GraphQL, Postman collections.
* Wrapped "optional" Blueprint types (`IsSet` + `Value`) — rejected in §6.3.
* Automatic deletion of anything on reimport. Removed items are deprecated and cleaned up on request (§9.5).
* Everything in §18 *Deferred to 2.x* (ruling R7).

---

## 1. Module and pipeline

New module **`VaRestOpenApi`**, type `Editor`, in the VaRest plugin (core §1 lists it). Depends on
`VaRestJson`, `VaRestHttp` (asset types; the engine-scoped Hub client fetches URLs), `UnrealEd`, `AssetTools`,
`AssetRegistry`, `BlueprintGraph`, `Kismet`, `ToolMenus`, `ContentBrowser`, `Slate`, `SlateCore`,
`PropertyEditor`, `MessageLog`, `DesktopPlatform`, `DataValidation`. Nothing runtime depends on it: a packaged game
never sees the importer, only the assets it produced.

```
 Load ──► Resolve ──► Normalize ──► Plan ─────────────► Diff ──────► Apply ──► Report
 bytes    $refs       3.0/3.1/3.2   types, names,   ▲    plan vs     UDS, enums, Message Log,
 JSON or  (confined)  (+2.0 O1b)    endpoints       │    snapshot    API asset,  last report
 YAML                                               │                manifest    on the asset
 └──────────────── background task (UE::Tasks) ─────┼────────────┘   └── game thread ──┘
                                     Snapshot "mine" (game thread)
```

* **Load → Plan are pure data** (`FVaRestOpenApiModel`, `FVaRestOpenApiPlan`; no `UObject`), run on a background
  task while the options dialog shows "Analyzing…", and are deterministic: the same bytes and options give the
  same plan (source order, no GUIDs). The plan serialises to JSON for golden tests (§14).
* **Snapshot** runs on the game thread (core §1: control plane on the game thread; a struct recompile destroys
  and rebuilds properties): it copies "mine" — the asset's endpoints, environments, auth, rules, manifest and every
  generated struct's member descriptions and enum's values — into plain data. Cheap: names, types, defaults, rules.
* **Diff** compares plan, manifest and snapshot on the worker and yields a change set — the reimport preview (§9).
* **Apply** is the only stage that touches `UObject`s, on the game thread, inside one cancelled transaction (§8.5),
  under an `FScopedSlowTask` with progress. Cancel is honoured until the commit phase.
* Issues from every stage carry a stable code, a severity and a JSON pointer into the source (§10).

Public C++ surface (editor scripting and CI, all in `VaRestOpenApi`): `UVaRestOpenApiImportOptions` (UObject;
also the `Options` of a `UAssetImportTask`), `UVaRestOpenApiLibrary` (BlueprintCallable editor functions:
`ImportFromFile`, `ImportFromUrl`, `Reimport`, `CheckForChanges`), `UVaRestOpenApiCommandlet`,
`UVaRestOpenApiValidator` (a `UEditorValidatorBase`).

---

## 2. Inputs

### 2.1 Versions

| `openapi` / `swagger` value | Path | Notes |
| --- | --- | --- |
| `3.0.x` | 3.0 normalizer | `nullable`, `discriminator`, components incl. `requestBodies`/`parameters`/`responses`/`headers` |
| `3.1.x` | 3.1 normalizer | JSON Schema 2020-12: `type` arrays, `const`, `$defs`, `$ref` siblings, `contentEncoding`, `pathItems`, `webhooks` (ignored) |
| `3.2.x` | 3.1 normalizer + additions | tag `parent` nests categories (§7.1); QUERY/`additionalOperations`, `in: querystring`, `itemSchema` skipped with an issue |
| `3.3+` | 3.1 normalizer | warning `OA-DOC-NEWER` |
| `"2.0"` (`swagger`) | 2.0 normalizer — **stretch, milestone O1b, droppable** | `definitions`, body/formData params, `consumes`/`produces`, `host`+`basePath`+`schemes`, `securityDefinitions`, `collectionFormat`, `x-nullable`. If O1b is dropped: `OA-DOC-VERSION` with the conversion hint |
| `1.x`, missing, other | — | error `OA-DOC-VERSION` ("convert to OpenAPI 3", e.g. with swagger2openapi) |

A number instead of a string (`openapi: 3.1`, `swagger: 2.0`, common in hand-written YAML) is accepted from its
source text with warning `OA-DOC-VERSIONNUMBER` (§2.3). Each normalizer converts its dialect into one model
(OpenAPI 3.1-shaped, refs resolved, path-level parameters merged into operations); everything after it is
version-blind.

### 2.2 Sources

* **File**: `.json`, `.yaml`, `.yml` by drag-drop or *Import* (factory, §11.1).
* **URL**: *Add ▸ VaRest ▸ Import OpenAPI from URL…*. Fetched with the core client on the engine-scoped Hub
  (core §5.2): GET, `Accept: application/json, application/yaml;q=0.9, */*;q=0.5`, 30 s timeout, 64 MiB cap.
  Format by `Content-Type` (`json`/`yaml` substrings), else sniffed. Optional fetch headers (a private spec's
  `Authorization`) live in per-user editor settings keyed by host, **never in the asset** (assets go to VCS).
* **Format detection**: first non-whitespace byte after an optional UTF-8 BOM is `{` → JSON, else YAML. UTF-16/32
  input is an error (`OA-DOC-ENCODING`).
* **Duplicate keys** in one object, JSON or YAML: last wins, warning `OA-DOC-DUPKEY` (core's JSON lookup already
  returns the last occurrence, core §2.1; the normalizer sees both through `Members()` and reports).

### 2.3 YAML: our own strict subset parser (decision; ruling R5)

No engine parser is usable (§0.2) and most hand-written specs are YAML; the big specs (GitHub, Stripe) publish
JSON too, so YAML matters for hand-written and small/medium framework output, where a strict subset suffices and
speed hardly matters. VaRest ships **a YAML parser for the subset OpenAPI documents use**, editor-only (≈1500
lines), building an `FVaRestJson` document. It is **its own milestone OY, in parallel with O2–O4**; if it slips,
2.0 ships JSON-only with the conversion hint below. The governing rule: **anything outside the subset is an error
with `file:line:col` and a code — the parser never guesses.** Every error message ends with: "or convert the file
to JSON with any YAML 1.2 tool and import the JSON".

Supported:

* UTF-8, one document. Line breaks LF, CRLF and lone CR (Windows-authored files). Optional `%YAML 1.1|1.2`
  directive (ignored), optional `---`, optional `...` at the end.
* Comments; block mappings and sequences by indentation, including sequences at the parent key's indentation
  (`parameters:\n- name: id`) and compact mappings in sequence entries (`- name: id\n  in: path`).
* Explicit keys `? key` with a **scalar** key (PyYAML emits them for keys over 128 characters).
* Flow collections `[…]` and `{…}`, nested, multi-line, with a trailing comma.
* Scalars: plain (multi-line folding), single-quoted (`''`), double-quoted (all YAML 1.2 escapes:
  `\0 \a \b \t \n \v \f \r \e \  \" \/ \\ \N \_ \L \P \xXX \uXXXX \UXXXXXXXX`, line continuation), literal `|`
  and folded `>` block scalars with chomping (`-`, `+`) and an indentation indicator (`1`–`9`).
* Anchors (`&a`) and aliases (`*a`), expanded as deep copies; **expansion budget**: the document may not grow
  beyond 4× the parsed node count + 100 000 nodes through aliases (`OA-YAML-ALIASBOMB`). Merge keys `<<` with a
  mapping or a sequence of mappings (YAML 1.1 merge semantics; explicit keys win).
* Tags `!!str !!int !!float !!bool !!null !!map !!seq` (and `!!binary`, kept as the string).
* Scalar resolution = **YAML 1.2 core schema**: `~`/`null`/`Null`/`NULL`/empty → null;
  `true|True|TRUE|false|False|FALSE` → bool; decimal, `0o`, `0x` integers (beyond int64 → double with
  `OA-YAML-BIGINT`); floats with exponent; quoted → always string. `.inf`/`.nan` → error `OA-YAML-NONFINITE`
  (JSON cannot hold them, core §2.1). YAML 1.1 words (`yes`, `no`, `on`, `off`) stay **strings** — no Norway
  problem; where an OpenAPI field expects a boolean (`required: yes`) the normalizer accepts them with
  `OA-YAML-11BOOL`.
* **Source text of plain scalars is kept** (a side table keyed by node) whenever a plain scalar resolves to a
  non-string. Wherever the OpenAPI field or the schema expects a string, the normalizer uses the source text
  (`OA-YAML-COERCED`, info): `version: 1.10` stays `"1.10"`, `type: string, enum: [01, 02]` keeps `"01"`/`"02"` as
  wire values, `example: 0123` keeps its zero, `openapi: 3.1` reads as `"3.1"`.
* Keys are scalars and **always use their source text** (`200:` → `"200"`, `1.10:` → `"1.10"`). A null key is an error.

Errors (each `file:line:col: message [code]`): tab indentation (`OA-YAML-TAB`); a collection or alias as a key,
or `? ` with a non-scalar key (`OA-YAML-COMPLEXKEY`); a second document (`OA-YAML-MULTIDOC`); `%TAG` directives
and other tags (`OA-YAML-TAG`); unknown anchors (`OA-YAML-ALIAS`); bad indentation, unterminated quotes/flows,
invalid escapes, reserved indicators `@`/`` ` `` starting a plain scalar (`OA-YAML-SYNTAX`); depth beyond core's
512 (`OA-YAML-DEPTH`). Duplicate keys follow §2.2 (warning, last wins).

JSON goes through `FVaRestJson::Parse` (core §2.1): 25× faster than `FJsonSerializer` on 10 MiB (core §7.3),
exact error positions, 64-bit integers kept.

### 2.4 Limits

64 MiB per document, 256 external documents, 512 depth (core), 50 000 schemas, 20 000 operations. Each limit
is an error naming the limit (`OA-DOC-LIMIT`); none truncates silently.

---

## 3. `$ref` resolution

* **Local**: `#/…` RFC 6901 pointers (`~0`, `~1`, percent-decoded fragment), into the same document.
* **Relative**: `common.yaml#/components/schemas/Error`, `./schemas/pet.json` — resolved against the referencing
  document's location, loaded once per absolute URI, JSON or YAML. Every external file is recorded as a source of
  the asset (§9.6), so editing it marks the asset for reimport.
* **Confinement**: a resolved reference is loaded only if it lies under the root's base — the root document's
  directory tree for a file root, the same origin and path prefix for a URL root. A file outside the tree is
  `OA-REF-OUTSIDE` (error) unless *Allow references outside the spec folder* is ticked for that import (stored with
  its options); a downloaded vendor spec cannot pull `../../secrets.json` into tooltips or the committed report.
* **Remote**: any other `http(s)://` target (another origin, or a URL from a file root) is never fetched in 2.0:
  `OA-REF-REMOTE` with the hint to bundle the spec (e.g. `redocly bundle`). (Ruling R7.)
* **Annotation siblings** (finding 8): `title`, `description`, `default`, `example`/`examples`, `readOnly`,
  `writeOnly`, `deprecated` and 3.0 `nullable` beside a `$ref` (3.1) **or inside a single-`$ref` `allOf` wrapper**
  (3.0 tools' idiom) are **use-site annotations** of the field or parameter that holds the reference — never merged
  into the shared target, which stays one struct or enum. Assertion siblings (`properties`, `required`, `minimum`…)
  turn the wrapper into a real `allOf` (§6.5). 3.0 documents get the same treatment (generators emit siblings
  even though 3.0 says to ignore them; ignoring a use-site `default` silently changes what is omitted).
* 3.1 `$id`-based and `$dynamicRef` resolution are unsupported: `OA-REF-DYNAMIC`, the schema becomes `Json`.
* **Cycles**: references resolve lazily by identity, so recursive schemas are fine; structure cycles are broken in
  planning (§6.6). A pure alias loop (`A: $ref B`, `B: $ref A`) is `OA-REF-LOOP` (error).
* **Identity** of a named schema is its canonical pointer (`#/components/schemas/Pet`, or
  `<relative path>#/pointer` for an external file). Inline schemas are identified by their semantic owner (§9.1).
* An unresolvable ref is an error on the item that uses it (`OA-REF-UNRESOLVED`; that operation or field is
  skipped); the import continues (§10.1).

---

## 4. The model (normalizer output)

`FVaRestOpenApiModel` (plain C++, editor module): `Info` (title, version, description), `Servers`
(`{UrlTemplate, Description, Variables{Default, Enum}}`), `SecuritySchemes` (by name), `GlobalSecurity`,
`Schemas` (by id; each a normalized JSON Schema node: type set, format, properties in source order — each property
a `{schema ref or inline schema, use-site annotations}` pair, required, items, additionalProperties, enum/const
with source texts, default, nullable, readOnly/writeOnly, deprecated, description, examples,
allOf/oneOf/anyOf/discriminator), `Operations` (method, path template, operationId, tags, summary/description,
deprecated, parameters merged from path level by `(name, in)`, request body by media type, responses by status and
media type, security, servers), `Tags` (with 3.2 `parent`). Nodes keep their source pointer and document.

---

## 5. What is generated

### 5.1 Outputs

| Source | Output | Section |
| --- | --- | --- |
| the document | one `UVaRestApi` asset (the factory's primary object) | §7 |
| object schemas (named or inline) reachable from the selected operations | one `UUserDefinedStruct` (BP struct) each | §6 |
| string enums | one `UUserDefinedEnum` each | §6.4 |
| field/enum wire names, presence and flags | `UVaRestApi::JsonTypes` (runtime data in the asset), always loaded (§6.2) | §17 N1, N2, N6 |
| import state | editor-only `AssetImportData` + `ImportState` in the asset | §9, §17 N13 |
| project setting entry | the asset is appended to `UVaRestHttpSettings::Apis` (preloaded at startup, cooked) | §6.2, §17 N7 |

**Blueprint assets are the output** (decision). The owner's audience configures from Blueprints; generated BP
structs work the moment the import finishes, with no compile and no editor restart. C++ header export is deferred
to 2.x (ruling R7, §18).

### 5.2 Folder layout and asset names

```
/Game/Backend/                 (where the file was dropped)
  petstore                     UVaRestApi — named after the file: the engine creates that package before the
                               factory runs (AssetTools.cpp:4119, :4289)
  petstore_Types/              option "Types folder"; default <asset dir>/<asset name>_Types
    S_Pet  S_Category  S_Tag  S_Order  S_User  S_ApiResponse
    E_PetStatus  E_OrderStatus
```

* Struct `S_<Name>`, enum `E_<Name>`, plus an optional *Type prefix* (default empty).
* `<Name>`: named schemas → PascalCase of the component name split at non-alphanumerics and case changes
  (`pet_owner` → `PetOwner`, `Page«User»` → `PageUser`, `v1.UserDTO` → `V1UserDTO`). Inline schemas → semantic
  owner (§9.1): field `<Owner><Field>` (array item `<Owner><Field>Item`, map value `<Owner><Field>Value`), request
  body `<Operation>Request`, success response `<Operation>Response`, error `<Operation>Error` (or
  `<Operation>Error4xx`/`5xx`/`Default` when the classes differ), inline param enum `<Operation><Param>`.
* Collisions → `_2`, `_3` in source order (`OA-NAME-COLLISION`, info). Names longer than 64 characters are cut to
  57 + `_` + 6 hex digits of their SHA-1 (path-length safety on Windows).
* If an asset with the same name exists elsewhere in the project (asset registry), `OA-NAME-SHADOW` suggests a
  type prefix — two `S_Error`s in one BP type picker are confusing.
* Generated structs and enums get a tooltip line `Generated from petstore.yaml#/components/schemas/Pet`.

---

## 6. Types

### 6.1 Type mapping

`Json` below is `FVaRestJson` (`USTRUCT(BlueprintType)`, `VaRestJson.h:113-114`), usable as a BP struct member.

| Schema | BP struct field | Notes |
| --- | --- | --- |
| `string` | String | |
| `format: date-time` | Date Time | core decode parses offsets and normalises to UTC (`DateTime.cpp:716-720`, `:821-869`); encode writes `%Y-%m-%dT%H:%M:%S.%sZ` (`:216-219`): **milliseconds only** — use *Date-time as String* when sub-millisecond timestamps must round-trip (optimistic concurrency) |
| `format: date`, `time`, `duration` | String | `FDateTime` would add a time; core's `FTimespan` is seconds, not ISO 8601 durations |
| `format: uuid` | String | core writes `FGuid` uppercase (`Guid.cpp:59-60`); ids are opaque, their case must round-trip |
| `format: byte`, 3.1 `contentEncoding: base64` | Byte array + rule `Base64` | |
| `format: binary` inside JSON | String + `OA-TYPE-BINARYJSON` | binary is meaningful as a whole body or a multipart part (§7.4) |
| other string formats (`email`, `uri`, `password`…) | String | format shown in the tooltip |
| string `enum` | `E_…` (§6.4) | String when §6.4 says so |
| 3.1 `const`, single-value enum | String, default = the value, presence `Always` | discriminator values |
| `integer` + `int32` (or `minimum`/`maximum` within int32) | Integer | |
| `integer` + `int64`, **or no format** | Integer64 | int↔int64 autocast both ways (`KismetMathLibrary.h:3593-3602`); option *Integers without format* = Int32 |
| `uint32`, `uint64` | Integer64 (`OA-TYPE-UINT64` for uint64) | BP has no unsigned 64-bit |
| `number` (any format) | Float (double) | BP floats are doubles in UE 5 |
| integer `enum` | Integer / Integer64, values (and `x-enum-varnames`) in the tooltip | core writes enums as names; numeric enums must stay numbers |
| `boolean` | Boolean | |
| `array` of T | Array of T | `uniqueItems` ignored: order kept, no `TSet` |
| array of arrays, array of maps, map of arrays/maps | Json | a BP pin has one container (`EPinContainerType`, `EdGraphNode.h:123-129`) |
| `object` with `properties` | `S_…` | |
| `object` with only `additionalProperties: T` | Map String → T | T a container → Json |
| `properties` + `additionalProperties` | `S_…` (extra keys not captured, `OA-TYPE-EXTRAKEYS`) | |
| free-form (`{}`, `type: object` without properties, `additionalProperties: true`) | Json | |
| object with no properties and `additionalProperties: false` | Json | a BP struct cannot be empty (`StructureEditorUtils.cpp:109-116`) |
| `allOf`, `oneOf`, `anyOf`, `discriminator` | §6.5 | |
| `nullable: true`, `type: [T, "null"]`, `oneOf: [T, {type: null}]` | T + rule `Nullable` (§6.3) | |
| `not`, `if/then/else`, `prefixItems`, `patternProperties`, `$dynamicRef` | Json + `OA-TYPE-UNSUPPORTED` | |
| structure cycle back-edge | Json + `OA-TYPE-RECURSIVE` (§6.6) | |

Options *Enums as String* and *Date-time as String* exist for teams that prefer plain strings; defaults favour
Blueprint sugar (Switch on enum, date math). Stripe-style expandable fields (`anyOf: [string, $ref X]`) become
`Json` by §6.5; the user guide says so (typing them is deferred, §18).

### 6.2 Field names, wire names, and rules that are always loaded (ruling R4)

The struct editor names a member's property `<Name>_<UniqueId>_<GUID>`, where `<Name>` is the friendly name if it
is a valid object name (`StructureEditorUtils.cpp:232-265`; invalid characters `"' ,/.:|&!~\n\r\t@#(){}[]=;^%$\``,
`NameTypes.h:191`). A cooked game and VaRest (core §2.4) read the JSON name back by stripping `_<UniqueId>_<GUID>`
(`UserDefinedStruct.cpp:300-313`). A rename in the struct editor **regenerates the property name** from the same
GUID (`StructureEditorUtils.cpp:347-373`), and friendly names are unique only case-insensitively (`FString ==`,
`:490-497`). Struct text import cuts an unquoted property name at `=([.<` (`Property.cpp:1604-1605`) while export
writes names unquoted (`Class.cpp:3585`), so `<` — which `NameTypes.h:191` allows — breaks copy/paste and pin
defaults.

So, per struct, in two passes over the JSON keys in source order:

1. **Pass 1 — friendly name = K** for every key K that is non-empty, at most 96 characters, a valid object name
   (`FName::IsValidXName(K, INVALID_OBJECTNAME_CHARACTERS)`), free of `<`, and not equal case-insensitively to a
   key that already took its name in this pass. The property is then `K_<n>_<GUID>` and the cooked name is K
   exactly: `user_id`, `_id`, `__v`, `user-id`, `2fa`, `名前` all qualify.
2. **Pass 2 — friendly name = sanitised K** for the rest: each invalid character (and `<`) → `_`, runs collapsed,
   `_` trimmed at the ends, empty → `Field`, cut to 96, then `_2`, `_3`… against every name taken so far,
   case-insensitively (`@type` → `type_2` when a real `type` exists, else `type`; `ID` after `id` → `ID_2`;
   `a.b` → `a_b`; `OA-NAME-SANITISED`, info). Exact keys always win over sanitised ones.
3. **Every generated member gets a field rule** (§17 N1): `{Json = K, Presence, bNullable, bBase64}`, keyed by the
   member's **GUID**. At runtime the member is identified with the engine's cooked GUID map:
   `UScriptStruct::FindPropertyGuidFromName` / `FindPropertyNameFromGuid` / `ArePropertyGuidsAvailable`
   (`Class.h:989-995`), implemented by `UUserDefinedStruct` over its `PropertyIDs` (`UserDefinedStruct.cpp:65-87`,
   `UserDefinedStruct.h:96-98`, outside `WITH_EDITORONLY_DATA`, so cooked), rebuilt from the member descriptions on
   every compile (`UserDefinedStructureCompilerUtils.cpp:231`, `:305-329`) — a rename keeps the GUID and the map
   follows. Only when `ArePropertyGuidsAvailable()` is false (`UserDefinedStruct.h:140-143`, a struct saved before
   the map existed) does the converter fall back to parsing the name's 32-hex suffix.
4. **The rules are always loaded.** Without them a conversion would use friendly names under the caller's naming
   policy — the BP JSON library and code-first conversions default to camelCase (M2 Task 6), so `user_id` would go
   out as `userId` — and every `Presence`/`Nullable`/`Base64` rule would vanish (optional fields sent as `""`/`0`).
   So the importer appends the asset to `UVaRestHttpSettings::Apis` (§17 N7): core loads every listed API at
   startup, before any game instance or Blueprint runs, and adds them to the cook, so the rules are registered in
   the editor, in PIE and in a packaged game before the first conversion, whether or not a *Call Endpoint* ever
   loads the asset. The API's own naming policy is `AsIs`.
5. A struct lying under a registered types folder that has no rules logs one warning per struct in non-Shipping
   builds (§17 N6) — the symptom of a removed settings entry.

Effects: renaming a generated field in the struct editor (designers will, for nicer pins) never changes the wire
name; pins still display friendly (`user_id` shows as "User Id"). Cost: every listed API and its types load at
startup — for a 50-operation selection, tens of small packages; `OA-SCALE` (§13) flags large imports.

### 6.3 Required and optional (decision; ruling R1)

Blueprint cannot hold `TOptional`: pins have no optional container (`EdGraphNode.h:123-129`), and although UHT
marks `TOptional` members Blueprint-supported when their value type is (`UhtOptionalProperty.cs:104-111`), the K2
schema has no `FOptionalProperty` mapping (none in `EdGraphSchema_K2.cpp`). Options considered:

| Option | Read path (responses, ~95 % of use) | Write path |
| --- | --- | --- |
| A. Plain fields, always encode | simple | optional fields sent as `""`/`0`/`false`: PATCH overwrites, format validators reject `""` |
| **B. Plain fields + presence that does not rely on "equals default" where a value can carry it** | simple | exact for enums and strings; one documented edge for optional bool/number |
| C. Wrapper structs (`IsSet`, `Value`) for optional fields | every read needs a break or autocast; most real APIs mark most fields optional | exact |

**B**, per value kind:

* **Enums: exact.** An enum used by an optional field without a (use-site or schema) default, or by a nullable
  field, gets a leading **`(unset)`** enumerator (§6.4) whose enum rule says *no value*. Such fields default to
  `(unset)`; the field's Presence decides what *no value* encodes as (omitted, or `null`). Every declared value —
  the first included — is written. `(unset)` also tells designers on the read path that the key was absent.
* **Strings, arrays, maps, structs, Json: exact in practice.** Empty means absent (`OmitWhenDefault` with an empty
  default); sending an explicit `""` needs the escape hatch below.
* **Booleans and numbers: `OmitWhenDefault`.** With a schema default, omission is exactly what the API defines.
  Without one, `false`/`0` is not sent. The planner lists every such field of every request body
  (`OA-OPT-DEFAULTNOTSENT`, info, aggregated per struct); fixes: set that field's Presence to `Always` (global to
  the struct) or, for one call, link the node's **`Body Json`** input (§7.4, §17 N10) — exact control, no new
  semantics.

Presence (field rule, editable per field in the asset's details panel):

| Presence | Set by the importer for | Encode | Decode |
| --- | --- | --- | --- |
| `Always` | `required` fields; `const`/discriminator fields; optional enums without a default (they default to `(unset)`) | written; an `(unset)` enum is omitted | normal |
| `OmitWhenDefault` | optional fields with a use-site/schema default; optional non-enum fields | skipped when `Identical` to the struct's default instance value | normal |
| `NullWhenDefault` | `required` + nullable fields | `null` when `Identical` to the default (`(unset)` for enums), else the value | `null` keeps the default |
| `DecodeOnly` | `readOnly` fields (the spec: "SHOULD NOT be sent") | never written | normal |
| `Ignore` | fields removed upstream (§9.5) | never written | ignored |

`writeOnly` fields use `Always`/`OmitWhenDefault` like any other; decode simply never finds them. `bNullable` makes
`null` keep the value without the scalar-mismatch warning M2's decode records (M2 plan Task 8). Defaults come from
the use-site annotation (§3) first, then the schema's `default`, then the type's zero (or `(unset)`).

Parameters are exact: an optional param pin that is unlinked and at its autogenerated default is not sent (§7.3).

### 6.4 Enums

String enums become `UUserDefinedEnum`s whose **display names are the wire values**: VaRest encodes a BP enum as
`GetAuthoredNameStringByIndex`, the *source string* of the display text (`UserDefinedEnum.cpp:208-221`), so
culture and translation never change the wire, and `DisplayNameMap` is cooked (`UserDefinedEnum.h:41-42`).
Enumerator internal names stay `NewEnumeratorN`; an **enum rule** (§17 N2) maps each internal name to its wire
value, so a designer prettifying a display name ("in_stock" → "In Stock") does not change the wire.

* **`(unset)`** (finding 1): added as enumerator 0 when §6.3 requires it; display `(unset)` (`(unset)_2` in the
  impossible case of a real value `(unset)`); enum rule `{Json = "", bUnset = true}`. It counts against the limit
  below. Required non-nullable fields of that enum default to the first real value.
* **Unknown values** from the server decode as core's mismatch: the field keeps its default (`(unset)` where
  present) and a warning is recorded; a reimport adds the new value. An opt-in `(unknown)` enumerator for required
  enums is deferred (§18).
* **String fallback** (`OA-ENUM-STRING`, naming the reason): more than 254 enumerators including `(unset)`
  (UDEnums are byte-backed, `UserDefinedEnum.cpp:231`); a value that cannot be a display name — empty, whitespace,
  or equal to another value case-insensitively (`EnumEditorUtils.cpp:540-556`); or the option *Enums as String*.
* Default value: use-site/schema `default`, else `(unset)` or the first value per §6.3.

Build path and order: §8.3.

### 6.5 Composition

* **`allOf`**: flattened into one struct (BP structs have no inheritance): properties in member order, later
  members win on equal types, conflicting types → that field is Json (`OA-TYPE-ALLOFCONFLICT`); `required` is the
  union. A single-`$ref` `allOf` whose other members carry only annotations is a use-site annotation wrapper (§3),
  not a new struct. A subtype of a base with a `discriminator` gets the discriminator field with its mapping value
  as default and `Presence = Always`, so encoding an `S_Cat` sends `"petType": "Cat"` by itself.
* **`oneOf` / `anyOf`**, normalised in order: drop `null` members (→ nullable); one member left → that type;
  all members string-like (string/enum/const) → String, or a merged enum if all are enum/const; all numeric →
  Integer64 if all integer, else Float; otherwise → **Json**, with every object member generated as a struct
  (inline ones `<Owner><Field>Option<N>`) and the tooltip listing the variants and the discriminator
  (`OA-TYPE-UNION`, info).
* **Polymorphic base** (a schema with a `discriminator` and subtypes) used as a field or response type → Json
  plus all variants (`OA-TYPE-POLYMORPHIC`): decoding a base struct would silently drop subtype fields. The base
  struct is still generated for *Json To Struct*.

### 6.6 Recursion and struct dependencies

The engine refuses a struct that contains itself through a member or an array inner, directly or via another
struct (`StructureEditorUtils.cpp:86-192`, members and array inners at `:129-139`). The planner builds the
containment graph from struct members, array items **and map values** (map values are our safe choice, not an
engine rule), finds strongly connected components (Tarjan), and types each back-edge as **Json** — DFS from schemas
in source order, so the choice is deterministic (`OA-TYPE-RECURSIVE`, info: "S_Node.children is Json; convert
items with Json To Struct (S_Node)"). Structs are compiled in topological order, because a member struct must
already be compiled (`:118-127`).

### 6.7 Defaults, descriptions, constraints

* `default` → member default value for scalars, strings, enums (enumerator internal name), validated before it is
  written (§8.1). Array/object/date defaults go to the tooltip only.
* Tooltip = description, then one generated line: `JSON: user_id · required · format: email · 1..64 chars ·
  pattern ^[a-z]+$ · example: "neo"`, capped at 1 KiB. `deprecated: true` adds `[Deprecated]`.
* Struct tooltip = schema description + the source line (§5.2).
* Member order = source order. The importer never reorders existing members.

---

## 7. The API asset

### 7.1 Operations → endpoints

| OpenAPI | Endpoint (core §5.1 + §17) |
| --- | --- |
| `operationId` | `Name` — verbatim when it has no `INVALID_NAME_CHARACTERS` and ≤ 128 chars, else PascalCase; missing → `<verb><PathWords>` (`get /pets/{petId}/photos` → `getPetsByPetIdPhotos`); duplicates → `_2` + `OA-OP-DUPID` |
| — | `Id` (stable GUID, assigned once, kept by the manifest — §17 N8) |
| method | `Verb`; `options`, `trace`, 3.2 `query`/`additionalOperations` skipped (`OA-OP-VERB`) |
| path | `Path` (`/pets/{petId}`, same template syntax as core) |
| first tag (3.2: its `parent` chain) | `Category` (`Store\|Orders`) — the action menu becomes *VaRest ▸ petstore ▸ Store ▸ getOrderById* |
| `summary` + `description` | `Description` (tooltip; Markdown kept as text) |
| `deprecated` | `bDeprecated` + message (node shows the engine's deprecation warning) |
| `parameters` | `Params` (§7.3) |
| `requestBody` | body kind + type (§7.4) |
| success response | `ResponseType` (§7.5) |
| error responses | `ErrorType` (§7.5) |
| `security` | `AuthMode` (§7.7) |
| header param `Idempotency-Key` | `bIdempotent = true`; no pin (core adds the key, core §3.2) |
| operation/path-level `servers` | `Path` = absolute URL (core: an absolute path overrides the base) + `OA-OP-SERVERS` (environments do not apply) |

Unselected operations (§11.2) are not imported. Callbacks, links, response headers, examples are not mapped (one
aggregated info issue each).

### 7.2 Servers → environments

* Each `servers[]` entry → environment: `Name` = description (sanitised) or host; `BaseUrl` = template with
  variables substituted by their defaults (`OA-SRV-VARS` lists enum alternatives; the designer adds environments
  as needed — the merge keeps them, §9.3). First server → `DefaultEnvironment`. `ShippingEnvironment` is left unset
  with an info issue: picking production by guessing names is not the importer's call.
* A relative server URL resolves against the spec URL when imported from a URL; from a file it stays relative,
  `OA-SRV-RELATIVE` (warning), and asset validation reports it until a host is set.
* Swagger 2.0 (O1b): one environment per `scheme` (`https` first) from `host` + `basePath`; no `host` → relative.
* No servers → one environment `Default` = `/` (3.x rule) → `OA-SRV-RELATIVE`.
* Base URL + path is **concatenation** (`https://h/v1` + `/pets` → `https://h/v1/pets`), never RFC 3986 reference
  resolution, which would drop `/v1` (§17 N14).

### 7.3 Parameters → typed pins

`FVaRestEndpointParam` (§17 N9) per `(name, in)`:

* `in: path` — required, typed pin; arrays comma-joined (`simple` style); objects → String pin + `OA-PARAM-STYLE`.
* `in: query` — typed pin; `form` + `explode: true` (the default) → repeated key (`?tag=a&tag=b`);
  `explode: false` → comma; `spaceDelimited`/`pipeDelimited` (2.0 `collectionFormat` `ssv`/`pipes`/`multi`/`csv`)
  → their separators; `deepObject` and object params → skipped (`OA-PARAM-STYLE`; the node's Query map still works).
* `in: header` — typed pin; arrays comma-joined. `Accept`, `Content-Type`, `Authorization` are ignored, as the
  OpenAPI spec requires.
* `in: cookie` — skipped (`OA-PARAM-COOKIE`): core has no cookie jar (core §0.4).
* 3.x `content` instead of `schema` → String pin (`OA-PARAM-CONTENT`).
* Types: String, Integer, Integer64, Float, Boolean, enum (inline string enums generate `E_<Operation><Param>`, no
  `(unset)`: pins are exact), or an array of those. Pin names are `FName`s, case-insensitive: within one location a
  case-insensitive clash gets `_2` (`sort`, `Sort_2`); across locations ` (query)`/` (header)`. The wire name stays
  exact in `FVaRestEndpointParam::Name`.
* **Optional params sit in the node's advanced (collapsed) section**, and an optional pin that is unlinked and at its
  autogenerated default is not sent — the K2 node knows link state at compile time.
* `default` → pin default; `description` → pin tooltip; `deprecated` → `[Deprecated]` in the tooltip.
* A param removed upstream stays declared with `bRemoved` (never sent, `[Removed upstream]` tooltip) until *Clean
  up* (§9.5): a linked pin that disappears is a compile error (`K2Node.cpp:1093-1104`).

### 7.4 Request bodies

Media type preference: `application/json` or `*+json` → **Json** body, typed by the schema (struct, array of
struct, scalar — §17 N11); `application/x-www-form-urlencoded` → **Form** (a flat struct encoded as a form; nested
members → `OA-BODY-FORMNESTED` and the type becomes Json); `multipart/form-data` → **Multipart** (`FVaRestMultipart`
pin; parts listed in the tooltip); `application/octet-stream`, `image/*`, other binary → **Binary** (bytes pin);
`text/plain` → **Text** (String pin). Other media types → listed in `OA-BODY-MEDIA`. Swagger 2.0 (O1b) `in: body` →
Json; `formData` → Form, or Multipart when any part is `type: file`. Inline schemas generate `S_<Operation>Request`.

Every Json-bodied node also has an advanced **`Body Json`** input (`FVaRestJson`): when linked, it is sent instead
of the typed body (§6.3 escape hatch, §17 N10).

### 7.5 Responses and errors

* **Success**: `200`, else `201`, else the lowest `2xx`, else `2XX`; `default` only when no status codes are
  declared. Media preference as §7.4 (JSON, then text → String, binary → bytes). Several 2xx with different schemas
  → Json + `OA-RESP-MULTI2XX`. `204`/no content → no response pin.
* **Errors**: if every `4xx`/`5xx`/`default` JSON schema resolves to one named schema **or all are identical by
  canonical shape** (sorted canonical JSON with descriptions and examples removed, hashed — hand-written specs
  repeat the same inline `{message}` under every status), that type becomes `ErrorType` (§17 N12; an inline shape
  generates one `S_<Api>Error`) and *On Error* gains a typed `Error Body`; otherwise none + `OA-RESP-ERRORS` (the
  raw body stays in `FVaRestError.Body`, core §3.3).

### 7.6 Envelope detection (decision: detect conservatively, confirm in the dialog)

Over operations whose JSON success schema is an object with properties, for each candidate key P in `data`,
`result`, `payload` (first match wins on a tie):

1. **Wrapped set** M = operations whose schema has property P. Require |M| ≥ 3 and |M| ≥ 75 % of the operations
   considered.
2. **Meta** = the schema's other property names. The most common meta set must cover ≥ 75 % of M and have at most
   6 names (wrappers are small).
3. Success predicate: a boolean `success` or `ok` in that meta set, required in ≥ 75 % of M →
   `SuccessPath = <name>`, `SuccessValues = {true}` (core §3.4).
4. Error message/code: over the distinct error-response schemas (by canonical shape, §7.5) and the meta set, the
   first of `error.message`, `message`, `error` (string), `detail` (string, FastAPI), `error_description` present as
   a string in ≥ 75 % → `ErrorMessagePath`; first of `error.code`, `code`, `errorCode`, `error_code` →
   `ErrorCodePath`; `bParseErrorBodies = true` when a message path was found.

When detected, the dialog shows "Unwrap `data` (41 of 44 operations)" checked; wrapped operations get the type of P
as `ResponseType`, the others a per-endpoint envelope override with an empty `DataPath` (core §5.1 overrides).
Unchecked → the wrapper structs are the response types. Step 4 runs even without a data wrapper.

### 7.7 Security → auth

| Scheme | Core auth (core §4.2) | Issue / what the designer does |
| --- | --- | --- |
| `http` `bearer` | `Bearer` | info: call *Set Bearer Token* after login (`bearerFormat` noted) |
| `http` `basic` | `Basic` | credentials at runtime |
| `http` other (digest…) | — | `OA-AUTH-UNSUPPORTED` |
| `apiKey` in header / query | `ApiKey` (Header / Query, name) | the key name joins the asset's redaction additions (core §4.5, §17 N15) |
| `apiKey` in cookie | — | `OA-AUTH-COOKIE` (no cookie jar) |
| `oauth2` `password` | `BearerRefresh`, refresh style `OAuth2Form`, `tokenUrl` as refresh path, paths `access_token`/`refresh_token`/`expires_in` | info: obtain the first token with *Send Request* to `tokenUrl`, then *Import Session* (a generated token endpoint is deferred, §18) |
| `oauth2` `clientCredentials` | same, `client_credentials` grant | `OA-AUTH-CLIENTSECRET` (warning: a secret in a shipped game is extractable) |
| `oauth2` `authorizationCode`, `implicit`, 3.2 `deviceAuthorization`; `openIdConnect` | `Bearer` | info: obtain the token with your platform/browser flow, then *Set Bearer Token* |
| `mutualTLS` | — | `OA-AUTH-UNSUPPORTED` |

The API's auth = the first fully supported alternative of the global `security`; an AND requirement
(`{ApiKey: [], Bearer: []}`) → the first supported scheme + `OA-AUTH-AND` (composite auth is deferred, §18). Per
operation: `security: []` → `AuthMode = None`; an alternative list containing `{}` → `IfAvailable`; a requirement
the API auth satisfies → nothing; a different scheme → `OA-AUTH-PEROP` (VaRest applies the API's auth).

---

## 8. Apply

### 8.1 Bulk struct construction (ruling R2)

There is no batched public path: every `FStructureEditorUtils` mutator ends in `OnStructureChanged` →
`CompileStructure` (`StructureEditorUtils.cpp:569-580`), so the per-call path costs **2–3 compiles per field**
(`AddVariable` `:288-318`, `RenameVariable` `:347-373`, `ChangeVariableDefaultValue` `:431-487` when a default exists;
tooltips do not compile), and each compile walks the whole UObject heap (§13). Like DataLink (§0.2), VaRest fills the
member descriptions through exported API and compiles once:

1. **Enums first** (§8.3), then struct **shells**: `FStructureEditorUtils::CreateUserDefinedStruct` (`:42-63`) for
   every new struct, so members can reference any struct.
2. **Validate before writing**: each default with `UEdGraphSchema_K2::DefaultValueSimpleValidation`
   (`EdGraphSchema_K2.h:715`) — the check `ChangeVariableDefaultValue` runs (`StructureEditorUtils.cpp:433-458`);
   a bad default written directly would fail `RecreateDefaultInstance` and leave the struct `UDSS_Error`
   (`UserDefinedStructureCompilerUtils.cpp:419-427`). A rejected default is dropped (`OA-APPLY-DEFAULT`, warning).
   Each pin type with `CanHaveAMemberVariableOfType` (`StructureEditorUtils.h:180`).
3. **Fill**: replace `FStructureEditorUtils::GetVarDesc(Struct)` (exported, mutable, `StructureEditorUtils.h:151`)
   with `FStructVariableDescription`s: `VarGuid` new, `FriendlyName` per §6.2, `VarName` =
   `<FriendlyName>_<GenerateUniqueNameIdForMemberVariable()>_<GUID Digits>` — the engine's format
   (`StructureEditorUtils.cpp:256-262`; the public id generator, `UserDefinedStructEditorData.h:172`; §6.2 only
   produces valid object names, so the engine's sanitising branch `:241-244` is never needed), `SetPinType`,
   `DefaultValue`, `ToolTip`. The shell's initial bool is replaced.
4. **Compile once**: `FStructureEditorUtils::OnStructureChanged(Struct)` (`StructureEditorUtils.h:197`), leaves first
   (topological order, §6.6).
5. **Self-check every compile** with engine APIs: `Status == UDSS_UpToDate`; per member
   `FindPropertyNameFromGuid(VarGuid) == VarName` (`UserDefinedStruct.cpp:65-75`),
   `FStructureEditorUtils::GetGuidFromPropertyName(VarName) == VarGuid` (`StructureEditorUtils.cpp:998-1001`), and the
   core strip of `VarName` == `FriendlyName`. On any failure, **that struct** is rebuilt automatically through the
   per-call public API (`AddVariable` + `RenameVariable` + `ChangeVariableDefaultValue` + `ChangeVariableTooltip`)
   and reported (`OA-APPLY-SLOWPATH`, warning). If it still fails: `OA-APPLY-COMPILE` (error), the endpoints using it
   fall back to `Json`. Engine drift therefore degrades to slowness, never to corrupt structs. The cvar
   `VaRest.OpenApi.SlowStructBuild 1` forces the slow path (test hook), and an equivalence test compares both paths.

### 8.2 Reimport edits to existing structs

Same helper, in place: unchanged members untouched; a type change regenerates `VarName` from the same GUID and clears
the default, as `ChangeVariableType` does (`StructureEditorUtils.cpp:419-421`); new members appended; removed members
kept (§9.5). One compile per changed struct, then the self-check. Unchanged structs are not touched — but the
compiler also recompiles and dirties every struct that contains a changed one (`UserDefinedStructureCompilerUtils.cpp:131-146`,
`:662`) and marks dependent Blueprints modified (`:629-653`); the preview states the resulting package count (§9.4).

### 8.3 Enums: order and batching

`AddNewEnumeratorForUserDefinedEnum` broadcasts with data resolution (`EnumEditorUtils.cpp:126`): it scans every
byte/enum property and every live object and **recompiles every struct that has a member of that enum**
(`:276-354`) — per added value. So:

* **New enums are built completely before any struct exists** that references them (§8.1 step 1): one `SetEnums`
  with all names, then `SetEnumeratorDisplayName` per value (`:499-538`, public; it gives each display text the
  package localization namespace, `:513-528`). With no users, the broadcasts find nothing to resolve.
* **Reimport appends only** (the importer never removes or reorders, §9.5), so no value is renumbered and data
  resolution is unnecessary: per enum, `FEnumEditorManager::Get().PreChange` (public, `ListenerManager.h:52`) +
  `Modify`, one `SetEnums` with the appended names
  (fresh `NewEnumeratorN` names), `EnsureAllDisplayNamesExist` (`EnumEditorUtils.h:97`), then the public
  `FEnumEditorUtils::PostEditUndo` (`EnumEditorUtils.cpp:267-271`: `UpdateAfterPathChanged` + `BroadcastChanges(…,
  false)`, which skips the resolution block and only reconstructs nodes using the enum), then
  `SetEnumeratorDisplayName` for the new values. O3 step 0 verifies this path; if it misbehaves, the per-call
  public API is the fallback and the spike records its cost.

### 8.4 The API asset and the factory plumbing

Environments, auth, envelope, endpoints, `JsonTypes` rules and the editor-only import state are written in one pass,
then `UVaRestApi::NotifyEdited()` (§17 N16) runs once: validation, action-database refresh
(`FBlueprintActionDatabase::RefreshAssetActions`, `BlueprintActionDatabase.h:126`), Call Endpoint node
reconstruction in loaded Blueprints. The asset is appended to `UVaRestHttpSettings::Apis` on first import.

The engine creates the primary package before calling the factory (`AssetTools.cpp:4119`, `:4289`) and registers the
returned object plus `GetAdditionalImportedObjects()` (`:4297-4308`, `Factory.h:140-144`). The factory returns the
`UVaRestApi`; structs and enums are created in their own packages (`CreatePackage`) and listed as additional
objects. `FAssetRegistryModule::AssetCreated` is called for them **only at commit**, so a cancelled phase 1 leaves
no registry entries for garbage packages. Packages are marked dirty, not saved (`UAssetImportTask::bSave` saves in
automation).

### 8.5 Undo (Critical finding 2; ruling R1)

The struct and enum utilities open their own transactions (`AddVariable` `StructureEditorUtils.cpp:292-293`, reached
from `CreateUserDefinedStruct` `:59-61`; enum utils `EnumEditorUtils.cpp:104`, `:505`), and asset import opens none
around factories — so without a mechanism every generated struct would leave an "Add Variable" entry whose undo
empties it (`UserDefinedStructEditorData.cpp:148-166` recompiles on undo), and an older user transaction on a struct
the reimport changed would revert the reimport while the manifest says it is applied. Mechanism:

1. **All of Apply (both phases) runs inside one `FScopedTransaction` that is cancelled at the end**, on success and
   on failure — DataLink's pattern (`DataLinkJsonEditorStructGenerator.cpp:44-54`). Nested engine transactions fold
   into it; `Cancel` pops it without undoing anything (`ScopedTransaction.cpp:42-49` → `UTransBuffer::Cancel`,
   `EditorTransaction.cpp:1411-1455`). Imports and reimports are therefore **not undoable**, like other UE imports.
2. **After Apply**, if any object Apply touched is still in the transaction buffer from an earlier transaction
   (`GEditor->IsObjectInTransactionBuffer`, `EditorEngine.h:1072`), the importer calls
   `GEditor->ResetTransaction(…)` (`EditorEngine.h:1067`) — the engine's own reimport precedent
   (`EditorFactories.cpp:6579-6583`) — and the completion toast says the undo history was cleared.
3. **Deletions are never inferred** (§9.3): a generated member, value or endpoint missing from the asset — deleted
   by the user, or lost any other way — is offered for **restore** (on by default); only an explicit untick in the
   preview records a suppression.
4. **Cancel** (phase 1 only): new objects are marked garbage, no registry entries exist yet (§8.4).

Tests: `Import.UndoAfterImportIsInert` (import, Ctrl+Z ×N: every generated struct `UDSS_UpToDate` with all members)
and `Reimport.UndoAfterReimport` (edit a struct, reimport, Ctrl+Z: no partial revert). Restore-the-bug: remove the
cancelled transaction → the first fails.

---

## 9. Reimport

### 9.1 Identity

| Item | Key |
| --- | --- |
| named struct, enum | schema id (§3) |
| inline struct, enum | **semantic owner** (finding 12): bodies `(endpoint identity, role ∈ {request, response, error[:class]})`; nested `(owner schema id, property key[, items \| values \| option<N>])`; params `(endpoint identity, in, name)`. Media type and status code are left out whenever the role is unique, so renaming `{petId}` → `{id}`, adding `; charset=utf-8` or 200 → 201 keeps `S_UpdatePetRequest` |
| field | (struct id, JSON key) → member `VarGuid` |
| enum value | (enum id, wire value) → enumerator internal name |
| endpoint | `operationId`; if no match, `(verb, path)` — so an operationId rename upstream keeps the endpoint and its `Id` |
| param | (endpoint, in, name) |
| environment | server URL template |
| auth, envelope | singletons |

### 9.2 The manifest

Editor-only, in the asset: `AssetImportData` (`UAssetImportData`, the `UDataTable` pattern: `DataTable.h:151-160`,
`DataTable.cpp:347-352`, `:363-368`) and `ImportState` (§17 N13), a compressed JSON blob owned by this module:
source (kind, location, external files, hash, fetch time), the options used, the selection, suppressions, deferred
breaking changes, and per item `{kind, id, target (asset path / GUID / enumerator), base}` where **base** is a
snapshot of the importer-owned properties as last applied — field: type, default, tooltip, JSON key, presence,
flags; endpoint: name, verb, path, category, description, deprecation, body, response, error type, auth mode,
idempotent, envelope override; param: type, required, default, style, description, removed; environment: name,
base URL; auth: class and properties. Policies the spec does not speak about (retry, timeouts, cache, offline, log
level) are never importer-owned and never touched. Base snapshots are stored, not recomputed from the old source,
because a newer importer would compute a different "base" and mistake its own changes for user edits.

### 9.3 Three-way merge

Per importer-owned property: *base* (manifest), *theirs* (new plan), *mine* (snapshot of the asset).

| Case | Result |
| --- | --- |
| mine == base | take theirs |
| mine ≠ base, theirs == base | keep mine (user edit) |
| mine ≠ base, theirs ≠ base | **conflict**: keep mine, listed in the preview with "take upstream" |
| new upstream item | create |
| item in base, missing from the asset | **restore** row, on by default (never inferred as a deletion, §8.5); unticking records a suppression that later reimports honour |
| suppressed item | not restored; the preview's "Show suppressed" can undo the suppression |
| item in the asset, not in base | user-owned: never touched |

After apply, base := theirs for every applied or declined item, so a declined change reads as a user edit next
time and is not offered again. Deferred breaking changes (§9.4) keep their old base and are offered again.

### 9.4 Preview, automated reimport, no-op cleanliness

*Reimport* (interactive) opens a preview: a tree of Added / Changed / Removed / Restore / Conflicts /
Kept-your-edits, grouped by struct, enum and endpoint, with checkboxes, and a footer "**N packages will be
modified**" from an asset-registry referencer walk (containing structs and dependent Blueprints, §8.2) — Perforce
users see the checkout cost before applying. Rows marked **Breaking**: field type change, param type change, required
param added, endpoint body/response/error type change. (Removed fields, params and endpoints are kept and compile,
§9.5, so they are not Breaking.)

**Automated reimport** (auto-reimport, `UAssetImportTask` with `bAutomated`, commandlet) applies only non-Breaking,
non-conflicting changes. Breaking ones are **deferred**: recorded in the manifest, reported
(`OA-REIMPORT-BREAKINGDEFERRED`), announced by a toast "N breaking changes need review", and flagged by asset
validation until an interactive reimport applies or declines them. The commandlet takes `-AllowBreaking` to opt in.

**A reimport with no changes marks no package dirty** — unchanged structs, enums and the asset are not touched, not
even to rewrite the manifest or the source hash: when the diff is empty nothing is written, even if the source bytes
changed in ways the plan ignores (the stored hash then lags the file; the diff, not the hash, decides). It ends with a
toast "Up to date".

### 9.5 Never break a graph silently

* **Field removed upstream**: kept; `Presence = Ignore` (never sent — strict servers reject unknown keys — and never
  decoded); tooltip `[Removed upstream <date>]`; validation warning until cleaned up.
* **Field type changed**: applied (interactive), marked Breaking. Graphs wired to the old type fail to compile with
  the engine's own message — the API broke, and keeping the old type would decode wrong values silently, which is
  worse. Declining it keeps the old type (decode warnings will say why). Automated: deferred.
* **Enum value removed**: enumerator kept; listed. Enumerators are never deleted by the importer
  (`RemoveEnumeratorFromUserDefinedEnum` renumbers values, `EnumEditorUtils.cpp:146-151`).
* **Endpoint removed** (or deselected): kept, `bDeprecated` with "Removed from the API" — nodes warn, still compile.
* **Param removed or renamed**: the old param stays declared with `bRemoved` (§7.3): its pin survives, linked graphs
  compile, the value is never sent. A renamed param also appears as a new pin.
* **Schema removed**: struct asset kept, tooltip flagged, listed. A removed schema whose shape equals an added one is
  reported as a possible rename (`OA-REIMPORT-MAYBERENAME`), not acted on.
* **Struct used by a DataTable or SaveGame**: when a Breaking struct change hits a struct that the asset registry
  shows a `UDataTable` referencing, the preview says so (row data of a changed member is reset by the engine).
* **Clean up removed items…** (asset menu): lists items in `Ignore`/deprecated/`bRemoved` state with their
  referencers (asset registry, per asset) and deletes the ticked ones.

### 9.6 Sources and auto-reimport

* File sources: `AssetImportData` records the root and every external `$ref` file with MD5
  (`AssetImportData.h:113-117`), so the editor's auto-reimport notices edits to any of them; the `FReimportHandler`
  (`EditorReimportHandler.h:206-265`) serves the standard *Reimport* action.
* URL sources: the reimport manager treats a source whose `FileSize` is `INDEX_NONE` as missing and asks for a new
  file (`Editor.cpp:443`), so URLs cannot be engine source files. A URL-sourced asset's handler returns `false`
  from `CanReimport`; *VaRest ▸ Reimport from source* (asset menu) re-fetches and works for both kinds. *Change
  source…* switches between them.

---

## 10. Validation and the report

### 10.1 Issues

`FVaRestOpenApiIssue { Severity (Error / Warning / Info), Code, Message, Document, Pointer, Line, Column,
AssetPath }`. Codes are stable (Appendix A) — tests assert them, docs link them. Only document-level errors
(cannot read, not OpenAPI, unsupported version, limits) abort; an item-level error skips that item (an operation,
a field) and the import continues, so one broken `$ref` in 900 operations does not block the other 899.

Line/column: issues carry the pointer during processing; at the end one **locator pass** re-scans each source that
has issues (RapidJSON SAX through `RapidJsonIncludes.h` for JSON, the YAML parser in locate mode) and fills lines
for exactly the pointers asked for. No per-node position table is kept for 10 MB documents.

### 10.2 Where it shows

* Message Log page **VaRest OpenAPI** (`FMessageLog`, `MessageLog.h:20`): one entry per issue with severity, code,
  message, `petstore.yaml:42:7 #/paths/~1pet/post/requestBody`, and an asset token (`FUObjectToken`,
  `UObjectToken.h:19`). Opens automatically when there are warnings or errors.
* The last report is kept (compressed) in the import state: *VaRest ▸ Show last import report*.
* Aggregation: repeated info issues collapse per code ("37 response header definitions ignored").

### 10.3 Asset validation

`UVaRestOpenApiValidator`, a `UEditorValidatorBase` subclass (`EditorValidatorBase.h:27-30`; the DataValidation
plugin is enabled by default, `DataValidation.uplugin:13`), on `UVaRestApi` assets with an import state: items in
removed state (warning); deferred breaking changes (warning, §9.4); a relative or empty environment base URL
(warning); an endpoint whose type asset was deleted (error); the asset missing from `UVaRestHttpSettings::Apis`
(warning — its rules would not be loaded at startup, §6.2); the API naming policy changed away from `AsIs` (info:
rules still win, user-added members follow the new policy); manifest present but source missing (info).

---

## 11. UI

### 11.1 Entry points

* **Drag-drop / Import** — `UVaRestOpenApiFactory` (`.json`, `.yaml`, `.yml`). The asset tools pick the first factory
  by priority whose `FactoryCanImport` passes (`AssetTools.cpp:4020-4038`) and, if none passes, fall back to the
  highest-priority one anyway (`:4041-4050`). So: priority **102** (default 100, `Factory.cpp:37`; WebAPI 101) and a
  **cheap, exact sniff**: JSON → a RapidJSON SAX pass that tracks depth only and stops at the first depth-1 key
  `openapi` or `swagger` (Stripe's spec has that key *after* its 7 MB `components`, so a prefix check is not
  enough; ≈10 ms); YAML → a line scan for `openapi:`/`swagger:` at column 0. A file the sniff accepted but that
  fails to load as OpenAPI ends with `OA-DOC-NOTOPENAPI`, as does a non-OpenAPI `.yaml` handed to us by the
  fallback. The project setting *Register for .json files* (default **on**; **off ⇒ priority -1**, and factories with
  a negative priority are excluded, `Factory.h:450-453`) lets users of Epic's WebAPI keep it.

  Other `.json` files are unaffected: the DataTable reimport factory derives from the CSV factory
  (`ReimportDataTableFactory.h:12`), inherits its priority 99 (`CSVImportFactory.cpp:72-73`), claims `.json`
  (`:508-512`) and accepts **any** file (`:514-517`). A JSON file that is not OpenAPI fails our sniff, then WebAPI's,
  and reaches the DataTable factory exactly as without VaRest.
* **From URL** — Content Browser *Add ▸ VaRest ▸ Import OpenAPI from URL…* (folder context too).
* **Asset context menu** (on `UVaRestApi`, extending core's `VaRestEditorTools` asset definition): *Reimport from
  source*, *Check for API changes* (dry run, report only), *Show last import report*, *Clean up removed items…*,
  *Change source…*.

### 11.2 Import options dialog

Modal window: header (title, version, OpenAPI version, counts, issue summary from the background plan, and an
**estimated apply time** = structs × the per-struct cost measured in O3 step 0), a details view of
`UVaRestOpenApiImportOptions`, and an **operation tree** (tag → operations, checkboxes, search, counts, "select all").
The details view gives every option for free; only the tree is custom Slate.

Options: *Types folder*, *Type prefix*, *Unwrap envelope* (pre-set by §7.6), *Integers without format*
(Int64/Int32), *Date-time as String*, *Enums as String*, *Generate unreferenced schemas* (off), *Allow references
outside the spec folder* (off, §3), *Include new operations on reimport* (on when everything was selected).

Selection default: everything when the spec has ≤ 150 operations (the threshold may drop after the O3 spike);
otherwise nothing, with the tree focused — the designer picks the tags they use, and only schemas reachable from
them are generated. Last-used options persist per user; the options an asset was imported with are stored in its
manifest and are what reimport uses.

### 11.3 Reimport preview

As §9.4; same window shell, the change tree instead of the operation tree; a "Change options…" button reopens §11.2
with the stored options (changing the selection is a change like any other).

---

## 12. Automation

### 12.1 Scripting

`UAssetImportTask` with `Options = UVaRestOpenApiImportOptions` imports headless (`AssetImportTask.h:36-99`,
`IAssetTools::ImportAssetTasks`, `IAssetTools.h:542`) — Python works out of the box. `UVaRestOpenApiLibrary` exposes
the same operations to editor utility Blueprints and Python.

### 12.2 Commandlet: API drift in CI

`UnrealEditor-Cmd <proj> -run=VaRestOpenApi -Check=/Game/Backend/petstore` → exit code 0 when the source and the
asset agree, 1 with the change list otherwise (`-Reimport=` applies non-Breaking changes, `-AllowBreaking` all;
`-Import=<file> -Dest=<path>` creates). A backend team can fail CI when the game's API asset is stale.

---

## 13. Performance and limits

Each struct compile is not O(struct): it duplicates the struct (`UserDefinedStructureCompilerUtils.cpp:86-106`,
`:594-598`), iterates every struct property in memory (`:116-120`) and every Blueprint (`:199-203`), and the
reinstancer visits every UObject and class default object (`StructReinstancer.cpp:88-133`); the transient
`STRUCT_REINST_*` duplicates are only un-rooted (`UserDefinedStructureCompilerUtils.cpp:660-669`) and stay until GC.
The bulk path still costs two heap walks per new struct (the shell's `AddVariable` compile and the fill compile).

**O3 step 0 — engine-only spike (ruling R3)**, before any VaRest apply code: in the host editor, build 1000 structs ×
15 members through the §8.1 path (plus 100 enums through §8.3, with the cancelled transaction of §8.5), under two
loads — (a) the idle host project, (b) a **realistic editor load** the spike generates in-editor: 500 Blueprints
with 20 nodes and a struct variable each, 200 structs, and engine content loaded until `stat uobjects` reports at
least 500 000 live objects. Record per-struct compile time (median, p95), total time, memory before/after GC, and the
enum append path, in `docs/passport/`.

**Gate (proposed for agreement):** at load (b), apply of a ~10 MB spec — GitHub, all operations, ≈1000 structs —
≤ **180 s**; a 50-operation selection ≤ **15 s**. If missed, the documented fallback, applied in order until met:
(1) build shells without the shell compile (`NewObject<UUserDefinedStruct>` plus editor data exactly as
`CreateUserDefinedStruct` does, `StructureEditorUtils.cpp:46-56`, all public members) — one heap walk fewer per
struct; (2) garbage-collect between batches of 100 structs to drop the reinstancing duplicates; (3) lower the
"select all" threshold to ≤ 50 operations and show the estimate in the dialog; (4) if still over, record the
measured numbers in the passport and document very large specs as "import by tag".

Other budgets (O5 exit, measured on the Linux host, recorded in `docs/passport/`):

| Measure | Budget |
| --- | --- |
| JSON 10 MB parse | ≤ 0.3 s |
| YAML 1 MB parse (YAML matters for hand-written specs, §2.3) | ≤ 0.5 s |
| Resolve + normalize + plan, GitHub spec, all operations | ≤ 2 s |
| Reimport, unchanged GitHub import | ≤ 5 s, **0 packages dirtied** |
| Peak editor memory during plan | ≤ 10 × source size |

Parse/resolve/normalize/plan run on a background task (the dialog stays responsive). Loading an API asset loads every
type it references, and listed APIs load at startup (§6.2); `OA-SCALE` (info) suggests a selection above 300
generated types. A missed budget is recorded with the measurement and either fixed in O5 or carried as a bead.

---

## 14. Verification

### 14.1 Fixtures (repo-only, `Tools/fixtures/openapi/`, excluded from the Fab package)

* `petstore-3.0.yaml`, `petstore-3.1.yaml`, `petstore-2.0.json` (O1b) — **authored by us in petstore's shape**, not
  copied, each with examples for every schema, including `Pet.status = available` (the first enum value).
* `tricky.yaml` + `tricky-common.yaml` (external ref) + `outside/secret.json` (confinement): recursion (tree +
  mutual), allOf with discriminator, oneOf unions with and without discriminator, 3.1 `type: [string, "null"]`,
  `const`, maps, free-form, array of arrays, enums with empty/case-duplicate/300 values and leading-zero values
  (`[01, 02]`), keys `@type`, `$ref`, `user-id`, `a<b`, `id`+`ID`, `名前`, 120-char names; **3.1 `$ref` siblings and
  3.0 single-`$ref` `allOf` wrappers carrying `default`/`readOnly`/`nullable`**; required+nullable fields; optional
  enums with and without defaults; missing and duplicate operationIds, path-level and `$ref` params, params `sort`
  and `Sort`, header and cookie params, form, multipart, binary, 204, multiple 2xx, `default` responses, repeated
  identical inline error schemas, an envelope API, server variables and a relative server, global + per-operation
  security incl. `[]`, AND requirements and an authorizationCode flow, deprecated items.
* `yaml/` — conformance cases: every supported construct incl. CRLF/CR files, `? ` scalar keys, plain-scalar source
  text; every unsupported one with its expected code and position; curated cases from the public YAML test suite
  inside the subset (vendored only if its license allows; else rewritten as our own), with the JSON they must
  produce (generated once with a reference YAML 1.2 implementation on a dev machine, committed as JSON).
* Reimport pairs: `petstore-3.0.yaml` → `petstore-3.0-v2.yaml` (added/removed/retyped fields, renamed operationId,
  renamed path parameter, param changes, enum value added and removed, new tag, removed endpoint).
* Large: GitHub's and Stripe's public specs are **downloaded** (never vendored) by `Tools/fetch_openapi_fixtures.py`,
  pinned by commit and SHA-256, into a gitignored folder; benches skip with a warning when absent.
* Golden plans: `Tools/fixtures/openapi/golden/*.plan.json`; `VaRest.OpenApi.UpdateGolden 1` (cvar) rewrites them for
  review.

### 14.2 Tests (`VaRest.OpenApi.*`, editor context, in `Source/VaRestOpenApi/Private/Tests/`)

* `Yaml.*` — conformance, errors with exact `line:col`, alias budget (bounded time), Norway, numeric and `? ` keys,
  source text recovery, duplicate keys (warning, last wins), CRLF/CR, block scalar chomping, depth.
* `Ref.*` — local/relative, confinement (`OA-REF-OUTSIDE`, the per-import override), remote refusal, annotation
  siblings and wrappers as use-site annotations, alias loop, external file recorded as source.
* `Normalize.*` — 3.0/3.1/3.2 (and 2.0 in O1b) into the model; the petstores produce equal models modulo version notes.
* `Plan.*` — golden plans for every fixture; the type mapping table row by row; names (§5.2, §6.2) incl. `<`,
  collisions and length cuts; `(unset)` placement; presence per §6.3; recursion back-edges; envelope detection
  positive, negative (mixed API below 75 %) and error paths; error-shape unification; inline identity by semantic
  owner; auth table; `OA-OPT-DEFAULTNOTSENT`.
* `Import.*` — into `/Temp/VaRestOpenApiTests/<Test>/` (in-memory packages, never saved): struct members (friendly
  names, property-name shape, cooked authored names, GUID map), enums (display = wire, `(unset)`), API asset contents,
  settings entry; **wire round trip** per §0.1 (decode → encode → decode `Identical`; every non-default key written
  with exact spelling; `Import.OptionalEnumFirstValueSent`); default validation (`OA-APPLY-DEFAULT`); self-check +
  automatic slow path (a fault-injection hook corrupts one `VarName`); bulk vs per-call equivalence; undo
  (`Import.UndoAfterImportIsInert`); factory sniffing (`Factory.IgnoresNonOpenApiJson`, Stripe-ordered keys).
* `Reimport.*` — the v1→v2 pair with user edits in between (rename a field in the struct editor, change a default,
  edit an endpoint policy, delete an endpoint, add an environment): the merge table of §9.3 row by row; restore of a
  missing member and suppression; conflicts; declined changes not re-offered; removed field `Ignore` and not sent;
  removed param kept and not sent; `Reimport.InlineIdentityStable` (path-parameter rename keeps
  `S_UpdatePetRequest`); `Reimport.AutomatedSkipsBreaking`; `Reimport.UndoAfterReimport`; **no-op dirties
  nothing**; package-count estimate; URL source via a local test-server route.
* `Graph.*` — a Blueprint with *Break S_Pet*, *Make S_Pet* and a *Call Endpoint* node built in code (like M2's smoke
  tests), reimported to v2: compiles with zero errors for the non-breaking changes; the retyped field fails with the
  engine's pin-type error and nothing else; a removed param's linked pin still compiles.
* `Bench.VaRest.OpenApi.*` (perf filter) — the O3 gate and §13 budgets.

### 14.3 Cooked-build wire-name test (ruling R6)

Defined in O4, **executed as part of the core M7 packaged-run gate** (core §7.2): a cooked Linux host run with an
imported struct that has a struct-editor-renamed field, a sanitised key (`@type`), an `OmitWhenDefault` field, a
`NullWhenDefault` field, an `(unset)` enum and a prettified enum display name; it encodes and decodes against
`Tools/testserver/` and byte-compares with the same run in the editor. One conversion runs **before** any *Call
Endpoint* or explicit load of the API asset, asserting that the startup preload (§6.2) already registered the rules.
M7 cannot pass with this test red.

### 14.4 Restore-the-bug targets

| Break | Test that must fail |
| --- | --- |
| drop the `(unset)` enumerator | `Import.OptionalEnumFirstValueSent` |
| remove the cancelled transaction | `Import.UndoAfterImportIsInert` |
| identify members by property name instead of the GUID map | `Import.RenamedFieldKeepsWireName` |
| skip the startup preload | cooked test's pre-load conversion (§14.3) |
| touch unchanged structs on reimport | `Reimport.NoOpDirtiesNothing` |
| always take upstream in the merge | `Reimport.KeepsUserEdits` |
| infer deletion from a missing member | `Reimport.RestoresMissingMember` |
| apply Breaking changes in automated mode | `Reimport.AutomatedSkipsBreaking` |
| key inline schemas by pointer | `Reimport.InlineIdentityStable` |
| omit the removed-field `Ignore` presence | `Reimport.RemovedFieldNotSent` |
| always encode optional non-enum fields | `Import.OptionalDefaultsOmitted` |
| drop use-site annotations next to `$ref` | `Plan.RefSiblingDefault` |
| skip default validation | `Import.BadDefaultRejected` |
| remove the alias expansion budget | `Yaml.AliasBomb` (time bound) |
| resolve `yes`/`no` as booleans | `Yaml.NorwayStaysString` |
| resolve `01` without its source text | `Yaml.LeadingZeroEnum` |
| skip cycle breaking | `Import.RecursiveSchema` (struct compile error) |
| accept any JSON in `FactoryCanImport` | `Factory.IgnoresNonOpenApiJson` |
| lower the envelope threshold to 50 % | `Plan.EnvelopeNotDetectedOnMixed` |

---

## 15. Milestones

| O | Content | Depends on | Exit |
| --- | --- | --- | --- |
| O1 | Module skeleton; JSON load; `$ref` resolver (local, relative, confinement, remote refusal, annotation siblings); 3.0/3.1/3.2 normalizers; issues + locator | core M2 | `Ref`/`Normalize` green on JSON fixtures |
| O1b | **Stretch, droppable:** Swagger 2.0 normalizer | O1 | `Normalize` 2.0 green; dropping it leaves `OA-DOC-VERSION` with the hint and changes nothing else |
| OY | YAML subset parser (§2.3), **in parallel with O2–O4**; droppable to JSON-only | O1 (model only) | `Yaml` green; restore-the-bug: alias budget, Norway, leading zeros |
| O2 | Planner: type mapping, names, presence + `(unset)`, enums, composition, recursion; endpoints, params, bodies, responses, error shapes, envelope detection, auth, servers; inline identity; plan JSON + goldens | O1 | `Plan` green on all fixtures; envelope restore-the-bug |
| O3 | **Step 0: engine-only spike and gate (§13)**, then Apply: bulk struct build with validation and self-check, enums, API asset, rules + settings entry, factory + sniffing, options dialog, Message Log report, URL import, undo mechanism | O2; **core M5** with §17 Must items; M2 follow-ups N1–N4 | spike recorded and gate met or fallback applied; `Import` green incl. wire round trip and undo; petstores import with zero errors; restore-the-bug: `(unset)`, undo, GUID map, optional omission, recursion, sniffing, default validation |
| O4 | Reimport: snapshot + diff, manifest, merge, restore/suppress, preview with package count, automated deferral, removed-item handling, no-op cleanliness, file/URL sources, auto-reimport, clean-up, validator; **cooked wire-name test defined (§14.3)** | O3 | `Reimport` + `Graph` green; restore-the-bug: no-op, user edits, restore, automated, inline identity, removed field |
| O5 | Scale and automation: budgets, selection UX, commandlet, scripting library, docs (README section, "Importing an OpenAPI spec" guide incl. the optional-field rules, `Body Json`, expandable fields) | O4 | budgets met or recorded; CI drift check demo |

O1, O1b, OY and O2 need nothing from M5 and run in parallel with it. The §14.3 test runs in core M7. Each O gets its
own plan in `docs/superpowers/plans/`, written when the previous one lands.

---

## 16. Risks

* **Mutating live BP structs.** Bulk construction fills member descriptions through exported API and replicates one
  format string; reimport recompiles structs open Blueprints use. Mitigation: DataLink precedent, default validation
  before writing, a self-check after every compile with automatic per-struct fallback to the public per-call path
  (drift degrades to slowness), an equivalence canary, `Graph.*` tests, the preview's Breaking marks and package count.
* **Apply cost scales with everything loaded** (each compile walks the heap). Mitigation: the O3 step 0 spike and
  gate with a documented fallback ladder (§13); selection defaults; estimate in the dialog.
* **Undo buffer state.** The cancelled transaction plus conditional `ResetTransaction` relies on engine behaviour
  verified in source; a future engine change could re-introduce entries. Mitigation: the two undo tests; deletions are
  never inferred, so even an unexpected revert is restored by the next reimport.
* **Startup preload.** Every listed API and its types load at startup in every build. Mitigation: selection keeps
  types few; `OA-SCALE`; the validator flags missing entries; the cooked test proves rules exist before first use.
* **Optional bool/number without a schema default** cannot be sent as `false`/`0` without a per-field or per-call
  step. Mitigation: `OA-OPT-DEFAULTNOTSENT` lists them; Presence `Always`; `Body Json`.
* **Own YAML parser**: a mis-parse would import a wrong API silently. Mitigation: strict subset with errors for
  everything else, source-text preservation, conformance corpus, cross-checked JSON goldens, a fuzz smoke (mutated
  fixtures never crash or hang); its own droppable milestone.
* **Core M5 not built yet**: §17 may be negotiated. Mitigation: Must/Should split; every Should has a stated fallback;
  O1–O2 and OY independent of M5.
* **Factory conflicts** with other `.json` importers and the engine's fallback (`AssetTools.cpp:4041-4050`).
  Mitigation: exact sniff, clear error, priority switch.

---

## 17. What core keeps and what the import adds (for the M5 plan; ruling R8)

**Core keeps, unchanged:** the per-API naming policy (core §2.4), `FVaRestJsonNames::Register` for C++ structs
(M2 Task 6), environments and per-endpoint envelope overrides (core §5.1), an absolute URL in `Path` overriding the base
and `bIdempotent` (core §3.2), orphan pins kept by Call Endpoint (core §5.4), the engine-scoped Hub (core §5.2), and the
§5.4 decode carrier — **an `FInstancedStruct` decoded off the game thread and copied out by a CustomThunk getter** — for
every endpoint whose body/response/error type is a single struct or `Json` (`FVaRestJson` is a USTRUCT). The
DataValidation plugin replaces revision 0's validator hook (former R15, dropped).

**The import adds** (N = need; Must = cannot round-trip without it; Should = degrades as stated):

| # | Where | Requirement | Prio | Without it |
| --- | --- | --- | --- | --- |
| N1 | VaRestJson (M2 follow-up) | **Field rules** `FVaRestJsonFieldRule { FGuid Field; FString Json; EVaRestJsonPresence Presence {Always, OmitWhenDefault, NullWhenDefault, DecodeOnly, Ignore}; bool bNullable; bool bBase64; }` registered per `(owner, struct)` (owner-keyed, so two owners never unregister each other). Members identified with `Struct->FindPropertyGuidFromName(Property->GetFName())` (`Class.h:989-995`); the 32-hex name parse only when `ArePropertyGuidsAvailable()` is false. Semantics per §6.3. No re-registration on recompile: GUIDs survive it; M2 Task 13's cache reset suffices. | Must | — |
| N2 | VaRestJson | **Enum names** `RegisterEnum(owner, const UEnum*, {FName Enumerator; FString Json; bool bUnset})`, consulted before `GetAuthoredNameStringByIndex` (encode) and before name matching (decode). `bUnset`: the value means *no value* — an object key is omitted (or `null` under `NullWhenDefault`); an array element or map entry is skipped with a warning. | Must | display-name edits change the wire; optional enums lose their first value |
| N3 | VaRestJson | **Single-value decode/encode**: `DecodeProperty(const FProperty*, void* Dest, const FVaRestJson&, Options, Report)` and its encode twin, for the N5 carrier. | Must | — |
| N4 | VaRestJson | Non-Shipping: one warning per `UUserDefinedStruct` under a registered types folder (registered with N1) that has no rules. | Should | a missing settings entry fails silently |
| N5 | K2 + async action (**change to core §5.4**) | **Second carrier** for non-struct types: an `FInstancedPropertyBag` with one property `Value` described from the `FVaRestTypeRef` (value types `PropertyBag.h:18-40`, containers `:47-55`, map key types `:267-274`). The bag type is created once per endpoint type on the game thread (`UPropertyBag::GetOrCreateFromDescs`, `PropertyBag.h:1413`) when the request is sent, filled off the game thread with N3, and copied into the typed output pin by the CustomThunk getter. | Must | every list endpoint returns `Json` |
| N6 | asset | `UVaRestApi::JsonTypes`: `{TObjectPtr<UField> Type; rules}` + the types folder; registered (N1/N2, owner = the asset) on `PostLoad` and after edits, unregistered on `BeginDestroy`; Presence editable in the details panel. | Must | — |
| N7 | settings + Hub | `UVaRestHttpSettings::Apis` (soft list): loaded synchronously at startup by an engine-scoped subsystem (before game instances), and added to the cook through `FGameDelegates::Get().GetModifyCookDelegate()` (`GameDelegates.h:23-26`, `:96`), so a C++-only game ships them too. | Must | rules missing until something loads the asset (§6.2) |
| N8 | asset + K2 | Endpoint `Id` (FGuid, stable; Call Endpoint binds by it, name as fallback/display); `Category` (`\|`-nested, used by the action menu); `bDeprecated` + message. | Must | operationId renames orphan nodes; flat menus |
| N9 | asset + K2 + builder | `Params: TArray<FVaRestEndpointParam{FName Name; In {Path, Query, Header}; FVaRestTypeRef Type (scalar/enum, optional array); bool bRequired; FString Default; Style {Repeat, Csv, Ssv, Pipes}; FString Description; bool bRemoved}>`; one typed pin each (optional ones advanced; case-insensitive pin-name dedupe); an unlinked optional pin at its autogenerated default is not sent; `bRemoved` keeps the pin, never sends; undeclared template params stay string pins (core §5.4). Builder: repeated query keys append; the array styles. | Must | string-only path pins, no query pins |
| N10 | K2 | Advanced **`Body Json`** input on Call Endpoint; when linked, sent instead of the typed body. | Should | per-call exact bodies need *Send Request* |
| N11 | asset + K2 | **Type descriptors** `FVaRestTypeRef { Kind {None, Json, Struct, String, Bytes, Bool, Int, Int64, Double, Enum}; TObjectPtr<UObject> Type; Container {None, Array, Map} }` instead of bare `UScriptStruct*` for body, response and error; `BodyKind {Json, Form, Multipart, Binary, Text}` (Json Must; the rest Should, else a Json body + `OA-BODY-MEDIA`). | Must | — |
| N12 | asset + K2 | Endpoint `ErrorType` (N11); *On Error* gains a typed `Error Body` when the error body is JSON. | Should | *Json To Struct* on `Error.Body` |
| N13 | asset (editor-only) | `AssetImportData` (`UAssetImportData`, Instanced, VisibleAnywhere — `DataTable.h:151-160`) + `ImportState {FName Importer; int32 Version; TArray<uint8> Blob}` opaque to core; the source-file registry tag (`DataTable.cpp:347-352`). | Must | no reimport |
| N14 | client | Base URL + path = concatenation with exactly one `/` (`https://h/v1` + `/pets` → `https://h/v1/pets`), never RFC 3986 resolution. Clarifies core §3.2. | Must | every versioned base URL breaks |
| N15 | auth | `AuthMode {Required, IfAvailable, None}` (replaces `bNoAuth`); `BearerRefresh` request style `OAuth2Form` (form-encoded `grant_type=refresh_token`, optional `client_id`); per-asset redaction additions (core §4.5). | Should | `IfAvailable` → Required + issue; OAuth refresh needs a custom auth |
| N16 | asset | `NotifyEdited()` for programmatic batch edits: validation, `RefreshAssetActions` (`BlueprintActionDatabase.h:126`), reconstruct Call Endpoint nodes in loaded Blueprints — once per import. | Must | stale menus and pins until restart |

Dropped from revision 0's list: composite auth and session capture (deferred, §18), the validator hook (DataValidation
plugin), re-registration on struct recompile (GUID map).

---

## 18. Deferred to 2.x (ruling R7 and review finding 26)

To become beads when this spec is accepted:

* **C++ header export** — from the same plan: USTRUCTs with `TOptional` for optional fields (`UPROPERTY()` only; no BP
  pin type), UENUMs with N2 names, one inline helper per operation returning core's `[[nodiscard]]` builder, names and
  rules registered through `FDelayedAutoRegisterHelper(EDelayedRegisterRunPhase::ObjectSystemReady, …)`, which runs
  immediately once that phase has passed (`DelayedAutoRegister.cpp:42-46`); deterministic output; spec text only in
  `//` comments, identifiers `[A-Za-z0-9_]`, escaped literals (no code injection).
* **Remote `$ref`** fetching (2.0 refuses; bundle the spec).
* **Adopting a hand-made API asset** ("Import OpenAPI into this asset").
* **Composite auth** for AND security requirements; **generated OAuth token endpoint** with session capture.
* An opt-in **`(unknown)` enumerator** for required enums; de-duplication of identical inline schemas beyond error types;
  a per-call "always send" field list.
* **Typed Stripe-style expandable fields** (`anyOf: [string, $ref X]`), pagination helpers (`Link`/cursor), typed
  response headers, "mock from examples", per-endpoint "Send test request" (Inspector territory).
* Swagger 2.0, if O1b is dropped.
* AsyncAPI (non-goal, §0.3).

---

## 19. Decisions made without the owner

`R` = controller ruling (binding, 2026-10-20, after the review); `D` = this spec's.

| # | Decision | Why | Cost if wrong |
| --- | --- | --- | --- |
| R1 | Apply both Criticals and every Important finding: `(unset)` presence for optional/nullable enums; one cancelled transaction + conditional `ResetTransaction`; deletions never inferred | silent wrong data and undo corruption are the two ways an importer loses trust | one extra enumerator in some enums; undo history cleared after some reimports |
| R2 | Bulk struct build through exported API (DataLink precedent), default validation before writing, self-check after every compile with automatic per-struct fallback to the slow public path | fast, and engine drift degrades to slowness instead of corrupt structs | coupling to one engine format string; slow path when the check fails |
| R3 | Engine-only perf spike as O3 step 0, realistic load, gate for a ~10 MB spec, documented fallback ladder | each compile walks the heap; 120 s was a guess | the gate may force the shell lever or a lower select-all threshold |
| R4 | Identify members with the engine's cooked GUID map; rules always loaded via the settings preload list | wire names and presence hold without the API asset, in every build | every listed API loads at startup |
| R5 | Own strict YAML subset parser, own milestone parallel to O2–O4, with source text, CRLF/CR, `? ` scalar keys, duplicate keys as JSON | no usable engine parser; most hand-written specs are YAML | ≈1500 lines to maintain; 2.0 may ship JSON-only if it slips |
| R6 | Cooked-build wire-name test, executed in the core M7 packaged-run gate | the headline guarantee is a cooked-build property | M7 depends on an O4 test being ready |
| R7 | Defer C++ export, remote `$ref`, adoption, composite auth, generated OAuth endpoint; Swagger 2.0 stretch and droppable | ship the import solid | C++-first teams and Swagger-2.0-only backends wait or convert |
| R8 | Core keeps `FInstancedStruct` for struct/`Json` types; the import adds a property-bag carrier for containers and scalars; tight N-list | reconcile with core §5.4 without redesigning it | two carriers in the K2 action instead of one |
| D1 | Separate `Editor` module `VaRestOpenApi`; nothing runtime depends on it | importer code never ships in games | none |
| D2 | BP assets (UDS + UDEnum) are the output | the audience is Blueprint; no compile/restart | C++-first teams use the assets until the 2.x export |
| D3 | YAML 1.2 core schema; 1.1 booleans stay strings (coerced only where OpenAPI expects a bool); plain-scalar source text kept | no Norway problem, no lost leading zeros or `1.10` | a 1.1-only spec gets a few warnings |
| D4 | Duplicate keys: warning, last wins, JSON and YAML alike | consistent with core's JSON lookup; item-level problems do not abort | a sloppy spec imports with a warning instead of failing |
| D5 | Relative refs confined to the root's tree (override per import); remote refs refused | a vendor spec cannot read local files into committed assets; no surprise network access | split-folder specs need one checkbox; remote-split specs need bundling |
| D6 | Accept 3.0, 3.1; 3.2 on the 3.1 path; newer 3.x with a warning; numeric version values accepted | one model, version-blind planner | a future 3.x feature is skipped with an issue |
| D7 | Friendly name = JSON key whenever valid and `<`-free; else sanitised; API naming `AsIs` | wire names exact without any table for real keys; text import safe | `user_id`-style pins instead of `UserId` |
| D8 | Optional/nullable enums get `(unset)`; strings/containers omit when empty; bool/number `OmitWhenDefault` with `OA-OPT-DEFAULTNOTSENT` and `Body Json` | exact where a value can carry absence; simple read path | optional `false`/`0` without schema default needs a step |
| D9 | `NullWhenDefault` for required + nullable; `readOnly` → `DecodeOnly`; removed upstream → `Ignore`; `writeOnly` normal | "no parent" is `null`, not `0`; spec says do not send readOnly | a required nullable `0` is sent as `null` |
| D10 | Use-site annotations beside `$ref` and in single-ref `allOf` wrappers stay on the field, never on the target | defaults decide omission; targets stay shared | none known |
| D11 | Unformatted `integer` → Integer64 unless its bounds fit int32 | ids/timestamps overflow int32, and int64→int autocast truncates silently | an extra autocast node in some graphs; option to flip |
| D12 | `uuid`/`date`/`time`/`duration` → String; `date-time` → Date Time (ms precision, documented) | case-exact ids; no invented time parts; date math where safe | sub-ms timestamps need the String option |
| D13 | `byte` → byte array with per-field base64 | usable as file/texture data | needs N1's `bBase64` |
| D14 | String enums → UDEnum with display = wire; String fallback (>254, unrepresentable, option) | Switch on enum in BP; cooked- and culture-safe | unknown server values decode to the default with a warning |
| D15 | `allOf` flattened; `oneOf`/`anyOf` normalised, else Json + variants; polymorphic bases → Json | no BP inheritance or unions; never drop fields silently | designers convert variants manually |
| D16 | Recursion back-edges (members, arrays, map values) → Json, deterministic | BP structs cannot recurse | recursive data needs *Json To Struct* per level |
| D17 | Nested containers → Json | one container per BP pin | as D16 |
| D18 | New enums built before structs; reimport appends in one batch per enum with a no-resolve broadcast | per-value adds recompile every struct using the enum | relies on append-only; the spike verifies |
| D19 | Primary asset named after the file; types in `<name>_Types/`, `S_`/`E_` prefixes, optional type prefix | UE import convention; one folder per API | renaming after import is manual |
| D20 | Envelope detection: `data`/`result`/`payload`, ≥ 3 and ≥ 75 %, ≤ 6 meta keys; confirmed in the dialog | wrong unwrapping is worse than none | an unusual wrapper is configured by hand |
| D21 | Success response: 200 → 201 → lowest 2xx → `2XX` → `default` only without codes | matches how specs are written | rare specs need a manual type |
| D22 | `ErrorType` when error schemas are one schema or one canonical shape | typed errors when unambiguous, also for repeated inline shapes | mixed error shapes stay `Json` |
| D23 | One API auth from global security; AND → first scheme; per-op differences reported | core has one auth per client | multi-scheme APIs need manual setup for some endpoints |
| D24 | OAuth password/client-credentials → `BearerRefresh` (`OAuth2Form`); first token via *Send Request* + *Import Session*; browser flows → `Bearer` | covers backend-style auth without generated endpoints | an extra node for the first login |
| D25 | Cookie params/auth, mTLS, digest unsupported with issues | no cookie jar or client certs in core | those APIs need custom auth |
| D26 | Optional params → advanced pins, not sent when unlinked at default; removed params kept `bRemoved` | exact presence where the node knows it; linked orphan pins are compile errors | needs N9 |
| D27 | Identity: schema pointer for named schemas, semantic owner for inline, (struct, key) for fields, operationId then (verb, path) | survives benign upstream edits and operationId renames | an upstream field rename is remove + add |
| D28 | Three-way merge with stored base snapshots; declined changes become user edits; deletions only by explicit suppression | keeps designer edits; undo-lost members are restored | restore rows appear for deliberately deleted items until unticked once |
| D29 | Removed items kept (fields `Ignore`, params `bRemoved`, endpoints deprecated); explicit clean-up | never break graphs silently | stale items linger until cleaned |
| D30 | Type changes applied interactively and marked Breaking (declinable); automated modes defer Breaking changes | a compile error beats wrong values; no surprise retypes from auto-reimport | deferred changes wait for a human |
| D31 | No-op reimport dirties nothing; the preview shows the package count | VCS hygiene | none |
| D32 | Snapshot "mine" on the game thread, diff on the worker | control plane on the game thread; recompiles destroy properties | one extra copy of small data |
| D33 | URL sources outside the engine reimport manager (own menu action) | the manager requires files (`Editor.cpp:443`) | URL assets lack the generic *Reimport* entry |
| D34 | Fetch headers per user, never in the asset | assets are committed | each developer enters a private spec's token once |
| D35 | Factory priority 102 + exact sniff; setting (off ⇒ -1) | wins over WebAPI only for real OpenAPI files | WebAPI users flip the setting |
| D36 | Item-level errors skip the item; only document errors abort | one bad ref should not block 899 operations | partial imports need the report read |
| D37 | Selection default: all ≤ 150 operations (may drop after the spike), else none; estimate in the dialog | big APIs import only what the game uses | one extra click for mid-size APIs |
| D38 | Commandlet drift check; `-AllowBreaking` opt-in | CI catches stale API assets | none |
| D39 | Validator as a `UEditorValidatorBase` subclass | DataValidation is on by default; no core hook | projects that disabled DataValidation lose the checks |
| D40 | Own fixtures in petstore's shape; large specs downloaded and pinned | no third-party files in the repo | the fetch script must track upstream URLs |

---

## Appendix A. Issue codes

| Code | Sev. | Meaning |
| --- | --- | --- |
| `OA-DOC-ENCODING` | E | not UTF-8 |
| `OA-DOC-NOTOPENAPI` | E | no top-level `openapi`/`swagger` |
| `OA-DOC-VERSION` | E | unsupported version (incl. 2.0 if O1b is dropped) |
| `OA-DOC-VERSIONNUMBER` | W | version given as a number |
| `OA-DOC-NEWER` | W | 3.3+ read as 3.1 |
| `OA-DOC-LIMIT` | E | a §2.4 limit exceeded |
| `OA-DOC-DUPKEY` | W | duplicate key, last wins |
| `OA-YAML-SYNTAX` / `-TAB` / `-COMPLEXKEY` / `-MULTIDOC` / `-TAG` / `-ALIAS` / `-DEPTH` | E | §2.3 |
| `OA-YAML-ALIASBOMB` | E | alias expansion budget exceeded |
| `OA-YAML-NONFINITE` | E | `.inf`/`.nan` |
| `OA-YAML-BIGINT` | W | integer beyond int64 read as double |
| `OA-YAML-11BOOL` | W | `yes`/`no`/`on`/`off` where a boolean is expected |
| `OA-YAML-COERCED` | I | plain scalar used as its source text |
| `OA-REF-UNRESOLVED` | E | item skipped |
| `OA-REF-OUTSIDE` | E | reference outside the spec folder |
| `OA-REF-REMOTE` | E | remote reference refused |
| `OA-REF-LOOP` | E | `$ref` alias loop |
| `OA-REF-DYNAMIC` | W | `$id`/`$dynamicRef` unsupported → Json |
| `OA-NAME-COLLISION` | I | a name got a suffix |
| `OA-NAME-SANITISED` | I | a field's friendly name differs from its key |
| `OA-NAME-SHADOW` | W | same-named asset elsewhere |
| `OA-TYPE-UNSUPPORTED` | W | construct → Json |
| `OA-TYPE-RECURSIVE` | I | back-edge → Json |
| `OA-TYPE-UNION` / `-POLYMORPHIC` | I | → Json + variants |
| `OA-TYPE-ALLOFCONFLICT` | W | conflicting allOf member types |
| `OA-TYPE-EXTRAKEYS` | I | additionalProperties beside properties |
| `OA-TYPE-UINT64` | W | uint64 → int64 |
| `OA-TYPE-BINARYJSON` | W | binary inside JSON → String |
| `OA-ENUM-STRING` | I | enum kept as String (reason given) |
| `OA-OPT-DEFAULTNOTSENT` | I | optional bool/number fields without a default whose `false`/`0` is not sent |
| `OA-OP-DUPID` | W | duplicate operationId |
| `OA-OP-VERB` | W | method not mapped |
| `OA-OP-SERVERS` | I | operation-level servers → absolute path |
| `OA-PARAM-STYLE` / `-COOKIE` / `-CONTENT` | W | §7.3 |
| `OA-BODY-MEDIA` / `-FORMNESTED` | W | §7.4 |
| `OA-RESP-MULTI2XX` | W | several success schemas → Json |
| `OA-RESP-ERRORS` | I | no single error schema or shape |
| `OA-SRV-VARS` | I | server variables substituted |
| `OA-SRV-RELATIVE` | W | relative base URL |
| `OA-AUTH-UNSUPPORTED` / `-COOKIE` / `-AND` / `-PEROP` | W | §7.7 |
| `OA-AUTH-CLIENTSECRET` | W | client secret in a game |
| `OA-APPLY-DEFAULT` | W | a default failed validation and was dropped |
| `OA-APPLY-SLOWPATH` | W | self-check failed; struct rebuilt through the per-call API |
| `OA-APPLY-COMPILE` | E | struct still not `UDSS_UpToDate`; its endpoints use Json |
| `OA-REIMPORT-MAYBERENAME` | I | possible schema rename |
| `OA-REIMPORT-BREAKINGDEFERRED` | W | automated reimport deferred breaking changes |
| `OA-SCALE` | I | > 300 generated types |

---

## Appendix B. Review disposition (`.superpowers/sdd/openapi-spec-review.md`)

| # | Finding | Disposition |
| --- | --- | --- |
| 1 | Optional enums lose their first value (Critical) | Applied (R1): `(unset)` enumerator + N2 `bUnset`, §6.3, §6.4; round trip redefined (§0.1); `Import.OptionalEnumFirstValueSent` |
| 2 | Undo buffer (Critical) | Applied (R1): §8.5 cancelled transaction, conditional `ResetTransaction`, deletions never inferred (§9.3); two undo tests |
| 3 | Bulk build guard rails | Applied (R2): §8.1 exported API, validation, self-check + automatic fallback; compile counts corrected; §16, R2/D-table |
| 4 | Apply budget unvalidated | Applied (R3): §13 O3 step 0 spike, realistic load, gate, fallback ladder |
| 5 | Enum path cost and order | Applied: §8.3 enums first, append batch with no-resolve broadcast, D18 |
| 6 | Wire names without the asset | Applied (R4): §6.2 GUID map + always-loaded rules (N1, N6, N7), owner-keyed registration, non-Shipping warning (N4) |
| 7 | No per-call escape hatch | Applied: `Body Json` (§7.4, N10); `OA-OPT-DEFAULTNOTSENT` covers all request bodies; per-call list deferred |
| 8 | `$ref` siblings / single-ref `allOf` | Applied: §3 use-site annotations, §6.5, fixtures, `Plan.RefSiblingDefault` |
| 9 | R8 vs core §5.4 carrier | Applied (R8): §17 "core keeps" + N5 property-bag carrier, N3 single-value decode |
| 10 | Diff reads UObjects off-thread | Applied: §1 snapshot on the game thread, D32 |
| 11 | Automated reimport applies Breaking changes | Applied: §9.4 deferral, `OA-REIMPORT-BREAKINGDEFERRED`, `-AllowBreaking`, test |
| 12 | Inline identity churn | Applied: §9.1 semantic owner, §5.2 names, `Reimport.InlineIdentityStable` |
| 13 | YAML subset and schedule | Applied (R5): §2.3 source text, CRLF/CR, `? ` scalar keys, duplicate keys as JSON; milestone OY |
| 14 | No cooked-build test | Applied (R6): §14.3, in the core M7 gate |
| 15 | Scope | Applied (R7): §18 deferred list; Swagger 2.0 → O1b droppable |
| 16 | R15 unnecessary; R1/R4 refinements | Applied: validator via DataValidation (§10.3, D39); N1/N6 per finding 6 |
| 17 | Breaking list vs §9.5; removed params | Applied: §9.4 Breaking list; params kept `bRemoved` (§7.3, §9.5, N9) |
| 18 | Changed struct dirties containers and Blueprints | Applied: §8.2, preview package count (§9.4) |
| 19 | `<` breaks struct text import | Applied: pass 1 rejects `<` (§6.2); fixture key `a<b` |
| 20 | Param pin names case-insensitive | Applied: §7.3 dedupe, fixture `sort`/`Sort` |
| 21 | Round trip vs dates and number spellings | Applied: §0.1 definition; millisecond note in §6.1 |
| 22 | Relative refs read any file | Applied: §3 confinement, `OA-REF-OUTSIDE`, per-import override |
| 23 | Unknown enums, required+nullable, writeOnly | (b) and (c) applied (§6.3 `NullWhenDefault`, writeOnly); (a) `(unknown)` deferred (§18) — unknown values now land in `(unset)` for optional enums |
| 24 | Identical inline error schemas | Applied for `ErrorType` (§7.5 canonical shape); general de-duplication deferred (§18) |
| 25 | Consistency and wording | Applied: C++ bullet removed, compile counts, "off ⇒ -1", Apply codes, `AssetCreated` at commit (§8.4), map-value edges as a choice (§6.6) |
| 26 | Missing user expectations | Deferred (§18); expandable fields noted in the guide (§6.1, O5) |
