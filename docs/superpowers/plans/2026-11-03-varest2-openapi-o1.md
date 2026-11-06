# OpenAPI O1 native data pipeline

This implementation follows the reviewed OpenAPI specification, sections 1–4, 10 and 15.
VaRest-brb.1 tracks implementation and verification. Source work proceeds under the owner's
feature-first instruction while M5's exact Engine/Host native exception is pending. This
document grants no build, network, filesystem-source, review or acceptance authority.

The Editor-only VaRestOpenApi module exposes one analysis entry point taking root bytes,
an absolute source identity and an optional synchronous read boundary. Load, resolve and
normalize own native data only, so the caller may run them on a worker. No editor object,
asset creation, auth header, random GUID or live type pointer enters the model.

JSON uses the existing VaRest parser with the document byte/depth limits. Missing/unsupported
versions fail explicitly. YAML remains a separate OY delivery and receives the JSON conversion
hint. Duplicate keys retain core last-wins semantics with a warning. A final SAX locator pass
runs only on documents with issues and retains positions only for requested pointers.
JSON grammar errors use OA-DOC-SYNTAX; invalid UTF-8/UTF-16/UTF-32 use OA-DOC-ENCODING.

References use canonical document/pointer identities and a per-analysis document cache.
Percent-decoded fragments use strict RFC6901. Relative paths are normalized before admission;
remote origins are refused. The injected loader must enforce physical confinement before
reading and return its canonical source identity; the resolver rechecks that identity. Root
and external bytes are never obtained through an implicit filesystem/network fallback.
Pure alias chains detect loops; recursive schemas reserve their model identity before
traversing children. Annotation siblings remain on their use site. Assertion siblings form
a real allOf wrapper without mutating the referenced target.

The normalizer accepts OpenAPI 3.0, 3.1, 3.2 and warns for newer 3.x. Swagger2 is an explicit
droppable O1b branch with a conversion diagnostic. Schema nodes preserve exact source-order
properties, type sets, defaults, examples, enum values, composition and raw constraints.
Operations merge path parameters by exact name/location, inherit security and servers only
when absent, resolve request/response/parameter/path-item references and retain selected
media schemas. Unsupported operations receive stable issues rather than disappearing.

Verification will cover JSON syntax/encoding/duplicates, pointer escaping and percent decoding,
document caching/confinement, alias and recursive graphs, annotation isolation, dialect
normalization, parameter inheritance and item-level recovery. Per-version native graphs,
compiler checks, fixture execution and independent review remain required and unexecuted.
