# The host project

Repo-internal. Not packaged with the plugin.

Development happens in a host project that is **not** part of this repository, so the repo stays
a clean plugin folder. On the main Linux workstation that host is `/w/Unreal/VaRestHost`:

```
VaRestHost/
  VaRestHost.uproject          enables the VaRest plugin
  Source/VaRestHost*.Target.cs  IncludeOrderVersion Unreal5_8, logging kept in Shipping
  Source/VaRestHost/            empty primary game module
  Config/                       minimal; Entry map
  Plugins/VaRest -> /w/Unreal/VaRest   (symlink)
```

## Why a symlink

The VaCuus host (`/w/Unreal/VaCuus/docs/dev/host-project.md`) measured the options on UE 5.8.1:

* A **clone** in `Plugins/` gives two working trees that silently diverge.
* `AdditionalPluginDirectories` must name a **parent** of the plugin directory: UBT only scans
  subfolders of the search root (`EpicGames.Build/System/EnumeratePlugins.cs:81-107`). The parent
  here, `/w/Unreal`, holds many other plugins.
* A **symlink** fails only under UBA *detours* (`ASSERT: rename: cross-process rename-while-open`).
  This workstation builds undetoured (`<bAllowUBAExecutor>false</bAllowUBAExecutor>` in
  `~/.config/Unreal Engine/UnrealBuildTool/BuildConfiguration.xml`), so the symlink builds and
  loads.

On a machine with detours on, either use `-NoUBA`, or replace the symlink with a dedicated parent
directory and `AdditionalPluginDirectories`.

## Recreating it

```bash
H=/w/Unreal/VaRestHost
mkdir -p $H/Plugins && ln -sfn /w/Unreal/VaRest $H/Plugins/VaRest
# .uproject, targets and Source/VaRestHost as listed above (copy from an existing host)
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh \
  VaRestHostEditor Linux Development -project=$H/VaRestHost.uproject
```

`Tools/run_tests.sh` reads `VAREST_ENGINE` and `VAREST_HOST`, so another layout only needs those
two variables.
