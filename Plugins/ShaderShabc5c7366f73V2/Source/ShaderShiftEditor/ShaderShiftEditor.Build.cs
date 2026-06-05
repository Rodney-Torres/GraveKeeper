// Copyright Hyperdyne Systems LLC. All Rights Reserved.

using UnrealBuildTool;
using UnrealBuildTool.Rules;

public class ShaderShiftEditor : ModuleRules
{
	public ShaderShiftEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				// Runtime module — gives us FShaderShiftHookRegistry, FShaderShiftFileHook, etc.
				"ShaderShift",
                "ToolMenus",
                "InputCore"
            }
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"Projects",
				"RenderCore",
                "PropertyEditor",
				"RHI",
				// Used by the persist-mode workflow to check out engine shader
				// files into the default changelist for source engine builds
				// (Perforce / Plastic).  Helper degrades to a no-op when SCC
				// is disabled, so this dependency is harmless in non-SCC setups.
				"SourceControl",
            }
		);
	}
}
