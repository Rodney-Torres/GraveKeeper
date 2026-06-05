// Copyright Hyperdyne Systems LLC 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ShaderShift.h"
#include "ShaderShiftSettings.generated.h"

// ---------------------------------------------------------------------------
// Enums for Project Settings dropdowns
// ---------------------------------------------------------------------------

/** Tone mapping operator. */
UENUM()
enum class ECustomTonemapMode : uint8
{
	ACES_Filmic        UMETA(DisplayName = "ACES Filmic"),
	AgX_Punchy         UMETA(DisplayName = "AgX"),
	Reinhard           UMETA(DisplayName = "Reinhard"),
	Linear             UMETA(DisplayName = "Linear (No Tonemapper)"),
	Custom             UMETA(DisplayName = "Custom (Uncharted 2)"),
};

/** Creative look applied on top of the AgX base curve. */
UENUM()
enum class EAgxLook : uint8
{
	Default  UMETA(DisplayName = "Default (Neutral)"),
	Golden   UMETA(DisplayName = "Golden"),
	Punchy   UMETA(DisplayName = "Punchy")
};

/** Diffuse BRDF model. */
UENUM()
enum class ECustomDiffuseMode : uint8
{
	EngineDefault  UMETA(DisplayName = "Engine Default (Lambert / Chan / EON)"),
	OrenNayar      UMETA(DisplayName = "Energy-conserving Oren-Nayar"),
	DisneyBurley   UMETA(DisplayName = "Disney Burley"),
	LambertSphere  UMETA(DisplayName = "Lambert-Sphere (d'Eon 2021)"),
	Gotanda        UMETA(DisplayName = "Gotanda (tri-Ace)")
};

/** Specular BRDF model. */
UENUM()
enum class ECustomSpecMode : uint8
{
	EngineDefault  UMETA(DisplayName = "Engine Default (Single-Lobe GGX)"),
	MultiLobe      UMETA(DisplayName = "Multi-Lobe Cinematic")
};

/** Bloom kernel / look. */
UENUM()
enum class ECustomBloomMode : uint8
{
	EngineDefault       UMETA(DisplayName = "Engine Default"),
	DualKawase          UMETA(DisplayName = "Dual Kawase Blur"),
	CODScatterAsGather  UMETA(DisplayName = "COD Scatter-as-Gather (Karis 13/Tent 9)"),
	FFTDiffraction      UMETA(DisplayName = "FFT Diffraction Starburst"),
	AnamorphicStreak    UMETA(DisplayName = "Anamorphic Lens Streak"),
	SpencerOcular       UMETA(DisplayName = "Spencer Ocular PSF"),
	AnimeHardGlow       UMETA(DisplayName = "Anime / Toon Hard Glow"),
	RetroCRT            UMETA(DisplayName = "Retro / CRT-style Glow")
};

// ---------------------------------------------------------------------------
// UShaderShiftSettings - Project Settings > Plugins > ShaderShift
// ---------------------------------------------------------------------------

UCLASS(config = ShaderShift, defaultconfig, meta = (DisplayName = "ShaderShift"))
class SHADERSHIFTEDITOR_API UShaderShiftSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UShaderShiftSettings();

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override  { return TEXT("Plugins"); }
	virtual FName GetSectionName()  const override  { return TEXT("ShaderShift"); }

	virtual FText GetSectionText() const override
	{
		return NSLOCTEXT("ShaderShift", "SettingsSection", "ShaderShift");
	}

	virtual FText GetSectionDescription() const override
	{
		return NSLOCTEXT("ShaderShift", "SettingsDesc",
			"Configure engine shader overrides, BRDF, and tone mapping settings.");
	}

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	static UShaderShiftSettings* Get();

	// =================================================================
	// Tone Mapping
	// =================================================================

	/**
	 * Tone mapping operator.
	 *
	 * NOTE: All UENUM defaults below intentionally point at the engine-default
	 * value (= 0).  These are the CDO defaults the Quick Settings panel reads
	 * on a fresh install (no Saved/Config/.../ShaderShift.ini present).  They
	 * MUST stay in sync with the DefaultIntValue fallbacks in
	 * FShaderShiftModule::EarlyApplySavedSettings - the runtime module can't
	 * read the CDO at PostConfigInit, so it has its own copy of these values.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tone Mapping",
		meta = (DisplayName = "Tonemapper"))
	ECustomTonemapMode TonemapMode = ECustomTonemapMode::ACES_Filmic;

	/** AgX creative look (only relevant when an AgX tonemapper is selected) */
	UPROPERTY(config, EditAnywhere, Category = "Tone Mapping",
		meta = (DisplayName = "AgX Look"))
	EAgxLook AgxLook = EAgxLook::Default;

	// =================================================================
	// Shading Model
	// =================================================================

	/** Diffuse BRDF */
	UPROPERTY(config, EditAnywhere, Category = "Shading Model",
		meta = (DisplayName = "Diffuse BRDF"))
	ECustomDiffuseMode DiffuseMode = ECustomDiffuseMode::EngineDefault;

	/** Specular BRDF */
	UPROPERTY(config, EditAnywhere, Category = "Shading Model",
		meta = (DisplayName = "Specular BRDF"))
	ECustomSpecMode SpecularMode = ECustomSpecMode::EngineDefault;

	// =================================================================
	// Bloom
	// =================================================================

	/** Bloom kernel / look */
	UPROPERTY(config, EditAnywhere, Category = "Bloom",
		meta = (DisplayName = "Bloom Mode"))
	ECustomBloomMode BloomMode = ECustomBloomMode::EngineDefault;

	/**
	 * When enabled, the bloom setup pass applies a hue-preserving saturation
	 * boost to bright pixels so high-emissive object cores retain their colour
	 * through the tonemapper instead of trending to white - the pre-4.17
	 * Unreal bloom behaviour.
	 *
	 * Only relevant for the non-convolution bloom paths (engine default and
	 * setup-stage modes), since FFT Diffraction / Spencer Ocular are kernel
	 * replacements that go through a different normalisation pipeline.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bloom",
		meta = (DisplayName = "Preserve Emissive Color (pre-4.17 style)",
				EditCondition = "BloomMode != ECustomBloomMode::FFTDiffraction && BloomMode != ECustomBloomMode::SpencerOcular"))
	bool bPreserveEmissiveColor = false;

	// =================================================================
	// Shader Hooks (generic config-driven list)
	// =================================================================

	UPROPERTY(config, EditAnywhere, Category = "Shader Hooks",
		meta = (DisplayName = "Shader Overrides",
				TitleProperty = "DisplayName",
				ShowOnlyInnerProperties))
	TArray<FShaderShiftHookConfig> ShaderHooks;

	// =================================================================
	// Recompilation
	// =================================================================

	/**
	 * When enabled, changing any setting above will immediately write the
	 * patched shaders to disk and issue "recompileshaders changed".
	 * Disable this to batch multiple changes before recompiling.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Recompilation",
		meta = (DisplayName = "Auto-Recompile Shaders on Change"))
	bool bAutoRecompile = true;

	// =================================================================
	// Packaging
	// =================================================================

	/**
	 * When enabled, ShaderShift does NOT revert the patched engine shaders on
	 * editor shutdown, crash, or pre-exit, and the next editor/cook startup
	 * skips its stale-backup recovery sweep.  This is intended for project
	 * packaging - the cook commandlet (which is auto-detected and persists
	 * regardless of this checkbox) writes the patched shader output into the
	 * cooked content; keeping the patches on disk between cook runs makes
	 * iterative packaging more predictable.
	 *
	 * NOTE: While this is enabled, the engine shader files in your install
	 * remain modified.  If you uninstall the plugin without first turning
	 * this off and clicking "Restore Engine Defaults", the patched shaders
	 * will stay in place.  The .backup files written alongside the patched
	 * shaders allow ShaderShift to restore them when this option is later
	 * disabled.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Packaging",
		meta = (DisplayName = "Persist Shader Changes in Engine",
				ToolTip = "Keep patched engine shaders on disk across editor shutdown/crash and skip stale-backup recovery on next startup. Use when packaging the game. Cook commandlets automatically persist regardless of this setting."))
	bool bPersistShaderChangesInEngine = false;

	/**
	 * Manually recompile shaders.  Called by the "Recompile Shaders Now"
	 * button added via IDetailCustomization (CallInEditor does not work
	 * on UDeveloperSettings in the Project Settings panel).
	 */
	void RecompileShadersNow();

	// =================================================================
	// Helpers
	// =================================================================

	/**
	 * Sync the UENUM properties (TonemapMode, AgxLook, etc.) into the
	 * matching FShaderShiftDefineEntry::Value strings inside ShaderHooks.
	 * Called before applying hooks.
	 */
	void SyncEnumsToDefines();
};
