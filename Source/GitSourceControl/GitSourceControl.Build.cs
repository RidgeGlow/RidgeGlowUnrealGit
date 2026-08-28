// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

using UnrealBuildTool;

public class GitSourceControl : ModuleRules
{
	public GitSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Slate",
				"SlateCore",
				"InputCore",
				"DesktopWidgets",
				"EditorStyle",
				"UnrealEd",
				"SourceControl",
				"SourceControlWindows",
				"Projects"
			}
		);

		if (Target.Version.MajorVersion == 5)
		{
			PrivateDependencyModuleNames.Add("ToolMenus");
		}

		// libgitlfs replaces the bundled git-lfs binaries this plugin used to ship
		// and spawn per lock operation. GitLfsLib publishes the header and stages
		// the shared library; it never links, so this dependency cannot break a
		// build even when the library is unavailable.
		PrivateDependencyModuleNames.Add("GitLfsLib");
	}
}
