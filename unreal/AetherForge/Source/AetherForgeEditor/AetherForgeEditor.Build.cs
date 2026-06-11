// Copyright AetherForge. All Rights Reserved.

using UnrealBuildTool;

public class AetherForgeEditor : ModuleRules
{
	public AetherForgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// UI
				"Slate",
				"SlateCore",
				"InputCore",
				"ToolMenus",
				"WorkspaceMenuStructure",

				// Protocol / transport
				"Json",
				"JsonUtilities",
				"WebSockets",

				// Editor integration (transactions, editor world, actor labels/folders)
				"UnrealEd",

				// IPluginManager (sidecar binary discovery under the plugin dir)
				"Projects",
			}
		);
	}
}
