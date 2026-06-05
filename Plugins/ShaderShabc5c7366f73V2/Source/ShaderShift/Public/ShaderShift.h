// Copyright Hyperdyne Systems LLC 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Containers/Map.h"
#include "Containers/Array.h"
#include "ShaderShift.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogShaderShift, Log, All);

// ---------------------------------------------------------------------------
// EShaderHookState (internal)
// ---------------------------------------------------------------------------

enum class EShaderHookState : uint8
{
	Unknown,
	Applied,
	Outdated,
	NotApplied,
	Error
};

// ---------------------------------------------------------------------------
// EShaderApplyResult - returned by Apply() to distinguish outcomes
// ---------------------------------------------------------------------------

/**
 * Distinguishes between three outcomes of an Apply() call so callers can
 * decide whether a shader recompile is actually needed:
 *
 *   WroteDisk    - file was different; new content was written → recompile needed
 *   AlreadyCurrent - file already contained the expected content → no recompile needed
 *   Failed       - an I/O or other error occurred
 */
enum class EShaderApplyResult : uint8
{
	WroteDisk,
	AlreadyCurrent,
	Failed
};

// ---------------------------------------------------------------------------
// FShaderShiftDefineEntry - one configurable #define inside a shader
// ---------------------------------------------------------------------------

/**
 * Describes a single configurable #define that exists inside a shader template.
 * At apply-time the system finds the #define line in the template source and
 * replaces its value with whatever the user set in Project Settings.
 *
 * For well-known modes (tonemapper, diffuse, specular) the settings panel
 * exposes typed UENUM dropdowns directly on UShaderShiftSettings which are
 * synced back into the defines array.  Additional / user-added shader hooks
 * use this struct's generic Value text field.
 */
USTRUCT()
struct SHADERSHIFT_API FShaderShiftDefineEntry
{
	GENERATED_BODY()

	/** The exact #define name - hidden from the UI */
	UPROPERTY(config)
	FString DefineName;

	/** User-friendly label shown in the array header */
	UPROPERTY(VisibleAnywhere, Category = "Define",
		meta = (DisplayName = "Name", NoResetToDefault))
	FString DisplayName;

	/** Help text shown as a tooltip */
	UPROPERTY(VisibleAnywhere, Category = "Define",
		meta = (DisplayName = "Info", MultiLine = true, NoResetToDefault))
	FString Description;

	/** Current value - written into the shader verbatim */
	UPROPERTY(EditAnywhere, Category = "Define",
		meta = (DisplayName = "Value"))
	FString Value;

	/** Default value (used for reset) */
	UPROPERTY(VisibleAnywhere, Category = "Define",
		meta = (DisplayName = "Default", NoResetToDefault))
	FString DefaultValue;
};

// ---------------------------------------------------------------------------
// FShaderShiftHookConfig - per-shader-file configuration block
// ---------------------------------------------------------------------------

USTRUCT()
struct SHADERSHIFT_API FShaderShiftHookConfig
{
	GENERATED_BODY()

	/** Inline enable/disable toggle */
	UPROPERTY(EditAnywhere, Category = "Shader Hook",
		meta = (InlineEditConditionToggle))
	bool bEnabled = true;

	/** Display name (read-only) */
	UPROPERTY(VisibleAnywhere, Category = "Shader Hook",
		meta = (EditCondition = "bEnabled", DisplayName = "Shader Hook",
				NoResetToDefault))
	FString DisplayName;

	/** Virtual engine shader path - read-only */
	UPROPERTY(VisibleAnywhere, Category = "Shader Hook",
		meta = (DisplayName = "Engine Shader Path", NoResetToDefault))
	FString VirtualPath;

	/** Template filename - usually not needed, tucked into Advanced */
	UPROPERTY(VisibleAnywhere, Category = "Shader Hook", AdvancedDisplay,
		meta = (DisplayName = "Template File", NoResetToDefault))
	FString TemplateFilename;

	/**
	 * When true, changing this hook's defines triggers "recompileshaders global"
	 * (rebuilds every global shader).  When false, only "recompileshaders changed"
	 * is issued, which is much faster because it only recompiles shaders whose
	 * source file actually changed.
	 *
	 * Set to true for hooks that touch shading-model files (diffuse/specular BRDF)
	 * because those affect material shaders across the entire project.
	 * Post-process-only hooks (tonemapper, LUT) can leave this false.
	 *
	 * Populated from the plugin config ("RequiresGlobalRecompile=true/false").
	 */
	UPROPERTY(VisibleAnywhere, Category = "Shader Hook", AdvancedDisplay,
		meta = (DisplayName = "Requires Global Recompile", NoResetToDefault))
	bool bRequiresGlobalRecompile = false;

	/** Generic defines - hidden when disabled */
	UPROPERTY(EditAnywhere, Category = "Shader Hook",
		meta = (DisplayName = "Settings",
				TitleProperty = "DisplayName",
				EditCondition = "bEnabled",
				EditConditionHides))
	TArray<FShaderShiftDefineEntry> Defines;
};

// ---------------------------------------------------------------------------
// FShaderShiftFileHook - one engine-shader override (disk patching)
// ---------------------------------------------------------------------------

struct SHADERSHIFT_API FShaderShiftFileHook
{
	FGuid HookGuid;
	FString VirtualPath;
	FString Filename;
	FString TemplateSource;
	FString DisplayName;
	mutable EShaderHookState CachedState;

	/**
	 * Whether this hook is enabled. Populated by the Editor module from
	 * UShaderShiftSettings before calling ApplyAllEnabledHooks().
	 */
	bool bEnabled = true;

	/**
	 * The resolved define values to use on the next Apply() call.
	 * Populated by the Editor module from UShaderShiftSettings before
	 * calling ApplyAllEnabledHooks(). Empty = use template defaults.
	 */
	TArray<FShaderShiftDefineEntry> PendingDefines;

	/**
	 * Whether this hook requires "recompileshaders global" (expensive) or
	 * can get by with just "recompileshaders changed" (fast).
	 * Populated from FShaderShiftHookConfig::bRequiresGlobalRecompile.
	 */
	bool bRequiresGlobalRecompile = false;

	FShaderShiftFileHook()
		: CachedState(EShaderHookState::Unknown)
	{
	}

	FShaderShiftFileHook(
		const FGuid& InGuid,
		const FString& InVirtualPath,
		const FString& InTemplateSource,
		const FString& InDisplayName = FString());

	FString GetPhysicalPath() const;
	FString GetBackupPath() const;
	FString GetGuidMarker() const;

	static FString BuildFinalSource(
		const FString& TemplateSource,
		const TArray<FShaderShiftDefineEntry>& Defines);

	EShaderHookState EvaluateState(const TArray<FShaderShiftDefineEntry>& Defines) const;

	/**
	 * Write this hook to disk if needed.
	 * Returns WroteDisk if the file was actually changed (recompile required),
	 * AlreadyCurrent if the on-disk content already matched (no recompile needed),
	 * or Failed on any I/O error.
	 */
	EShaderApplyResult Apply(const TArray<FShaderShiftDefineEntry>& Defines);
	bool Revert();
	bool IsApplied() const;
};

// ---------------------------------------------------------------------------
// FShaderShiftHookRegistry
// ---------------------------------------------------------------------------

class SHADERSHIFT_API FShaderShiftHookRegistry
{
public:
	static FShaderShiftHookRegistry& Get();

	bool RegisterHook(const FShaderShiftFileHook& Hook);
	bool RegisterHookFromFile(
		const FString& VirtualPath,
		const FString& SourceFilePath,
		const FString& DisplayName = FString());

	bool UnregisterHook(const FGuid& HookGuid);
	bool UnregisterHookByPath(const FString& VirtualPath);
	bool HasHook(const FString& VirtualPath) const;

	const FShaderShiftFileHook* FindHook(const FString& VirtualPath) const;
	FShaderShiftFileHook* FindHookMutable(const FString& VirtualPath);
	void GetAllHooks(TArray<const FShaderShiftFileHook*>& OutHooks) const;

	/**
	 * Apply all enabled hooks.
	 * OutDiskWriteCount is set to the number of hooks that actually wrote new
	 * content to disk (i.e. whose output differed from what was already there).
	 * bOutNeedsGlobalRecompile is set to true if any hook that wrote to disk
	 * has bRequiresGlobalRecompile set.
	 * Returns total number of hooks that succeeded (written + already-current).
	 */
	int32 ApplyAllEnabledHooks(int32& OutDiskWriteCount, bool& bOutNeedsGlobalRecompile);

	/** Convenience overload that discards the disk-write count and global flag. */
	int32 ApplyAllEnabledHooks();

	int32 RevertAllHooks();
	bool ApplyHook(const FString& VirtualPath);
	bool RevertHook(const FString& VirtualPath);
	int32 GetHookCount() const;
	void ClearAll();

	/**
	 * Flush the shader file cache and issue "recompileshaders changed".
	 * Only call this when at least one shader file was actually written to disk.
	 */
	static void TriggerShaderRecompile();
	static void RevertStaleHooks();

private:
	FShaderShiftHookRegistry() = default;

	TMap<FString, FShaderShiftFileHook> Hooks;
	mutable FCriticalSection RegistryLock;
};

// ---------------------------------------------------------------------------
// Module interface
// ---------------------------------------------------------------------------

class SHADERSHIFT_API FShaderShiftModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static bool RegisterShaderOverride(const FString& VirtualPath, const FString& ShaderSource);
	static bool RegisterShaderOverrideFromFile(const FString& VirtualPath, const FString& ShaderFilePath);
	static bool UnregisterShaderOverride(const FString& VirtualPath);
	static FShaderShiftHookRegistry& GetRegistry();

	/**
	 * Update the in-memory cache of the persist-changes flag.
	 *
	 * Called by the editor module when the user toggles the
	 * "Persist Shader Changes in Engine" setting (either in Project Settings
	 * or in the Quick Panel checkbox).  The runtime module needs its own copy
	 * of this flag because it reads it during ShutdownModule and from the
	 * crash/pre-exit delegate lambdas - contexts where the editor module's
	 * UShaderShiftSettings UObject may already be torn down.
	 */
	static void SetPersistShaderChanges(bool bInPersist);

	/**
	 * Returns true if the patched engine shaders should be left on disk
	 * across editor shutdown / crash / pre-exit, AND if startup stale-backup
	 * recovery should be skipped.
	 *
	 * Returns true if EITHER:
	 *   - The user has the "Persist Shader Changes in Engine" setting enabled, or
	 *   - The current process is a cook commandlet (auto-persist for packaging).
	 *
	 * The cook check exists so the cook commandlet's clean shutdown does not
	 * revert the freshly-patched shaders out from under a packaging pipeline.
	 */
	static bool ShouldPersistShaderChanges();

private:
	void LoadShaderHookConfigs();

	/**
	 * Read saved user settings directly from the ShaderShift.ini config file
	 * (the same file UShaderShiftSettings writes to) without going through the
	 * UObject/CDO system, which is not available at PostConfigInit loading phase.
	 *
	 * Resolves the UENUM property values into define strings, stamps them onto
	 * the registered hooks, and applies them to disk so the engine compiles the
	 * patched shaders during its normal startup - avoiding a costly post-startup
	 * recompile.
	 */
	void EarlyApplySavedSettings();

	void SyncSettingsWithHooks();
	void RegisterCrashHandlers();
	void UnregisterCrashHandlers();

	FString PluginDirectory;
	FString ShadersDirectory;

	FDelegateHandle OnShutdownAfterErrorHandle;
	FDelegateHandle OnEnginePreExitHandle;

	/**
	 * Cached "Persist Shader Changes in Engine" flag.
	 *
	 * Populated at StartupModule from the user's saved ShaderShift.ini before
	 * RevertStaleHooks runs, and refreshed by SetPersistShaderChanges whenever
	 * the editor module observes the setting toggle.  Read by ShouldPersistShaderChanges
	 * - which OR's it with cook-commandlet auto-detection.
	 */
	static bool bPersistShaderChanges;
};
