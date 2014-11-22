// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	public class VaRestPlugin : ModuleRules
	{
        private string ModulePath
        {
            get { return Path.GetDirectoryName(RulesCompiler.GetModuleFilename(this.GetType().Name)); }
        }

        private string ThirdPartyPath
        {
            get { return Path.GetFullPath(Path.Combine(ModulePath, "../../ThirdParty/")); }
        }

		public VaRestPlugin(TargetInfo Target)
		{
			PrivateIncludePaths.AddRange(
				new string[] {
					"VaRestPlugin/Private",
					// ... add other private include paths required here ...
				});

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
                    "HTTP",
                    "Json"
					// ... add other public dependencies that you statically link with here ...
				});
		}
	}
}