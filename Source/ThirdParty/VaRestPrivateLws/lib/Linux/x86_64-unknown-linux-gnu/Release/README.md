# Bundled private LWS archive

This directory carries the exact existing recipe007/output006 archive. It is a
plugin dependency, rather than a build-cache artifact. Stock Unreal BuildPlugin
copies Source and deletes the copied Intermediate tree; the rules therefore
resolve this library from ModuleDirectory/lib.

provenance.json records the complete 32-member identity and original recipe.
The modified library sources, selected headers/configuration, license and notice
are supplied in this module. The archive was copied without modification or a
new compiler invocation. It retains the original Linux SDK and provider binding;
this relocation does not establish compatibility with another engine/toolchain.

The existing rules still select only Linux x64 Editor Development, modular and
non-merged source builds and refuse the existing precompiled cases. Ordinary
Game targets keep their previous dependency selection. Portable packaged/editor
and Game qualification, other platforms, native-owner lifecycle and release
acceptance remain open. No Engine, Launch or consuming-game source is required
or changed by this relocation.
