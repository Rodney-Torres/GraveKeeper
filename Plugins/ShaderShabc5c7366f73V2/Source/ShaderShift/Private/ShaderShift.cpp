// Copyright Hyperdyne Systems LLC 2026. All Rights Reserved.

#include "ShaderShift.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PathViews.h"
#include "ShaderCore.h"
#include "Misc/CoreDelegates.h"
#include "Interfaces/IPluginManager.h"

DEFINE_LOG_CATEGORY(LogShaderShift);

#define LOCTEXT_NAMESPACE "FShaderShiftModule"

// Static persist flag - populated from saved config at StartupModule and
// updated in-place by the editor module when the user toggles the setting.
bool FShaderShiftModule::bPersistShaderChanges = false;

namespace
{
	/**
	 * Resolve the on-disk path to the user's saved ShaderShift.ini.
	 *
	 * Mirrors the resolution order in FShaderShiftModule::EarlyApplySavedSettings
	 * (platform-specific path → GeneratedConfigDir → bare Config dir) so the
	 * persist flag and the rest of the saved settings are always read from the
	 * same file.  Returns an empty FString if no saved ini exists yet.
	 */
	FString ResolveSavedShaderShiftConfigPath()
	{
		const FString PlatformConfigDir = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Config"),
			FPlatformProperties::PlatformName(), TEXT("ShaderShift.ini"));
		const FString GeneratedPath = FPaths::Combine(
			FPaths::GeneratedConfigDir(), TEXT("ShaderShift.ini"));
		const FString BareConfigPath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("ShaderShift.ini"));

		if (FPaths::FileExists(PlatformConfigDir)) return PlatformConfigDir;
		if (FPaths::FileExists(GeneratedPath))     return GeneratedPath;
		if (FPaths::FileExists(BareConfigPath))    return BareConfigPath;
		return FString();
	}

	/**
	 * Read just the bPersistShaderChangesInEngine value from the user's saved
	 * ShaderShift.ini, returning the default (false) if the file or key is
	 * absent.  Called early in StartupModule, before the editor module's
	 * UDeveloperSettings CDO is available.
	 */
	bool ReadPersistFlagFromSavedConfig()
	{
		const FString Path = ResolveSavedShaderShiftConfigPath();
		if (Path.IsEmpty())
		{
			return false;
		}

		FConfigFile Config;
		Config.Read(Path);

		const FConfigSection* Section = Config.FindSection(
			TEXT("/Script/ShaderShiftEditor.ShaderShiftSettings"));
		if (!Section)
		{
			return false;
		}

		if (const FConfigValue* Val = Section->Find(TEXT("bPersistShaderChangesInEngine")))
		{
			return Val->GetValue().ToBool();
		}
		return false;
	}

	/**
	 * Detect whether this process is the UE cook commandlet.
	 *
	 * We can't use ::IsRunningCookCommandlet() at PostConfigInit because the
	 * Engine module hasn't loaded yet; FCommandLine is always available.
	 * Recognises both legacy "Cook" and "CookByTheBook" invocations as well as
	 * the bare "CookCommandlet" name some toolchains pass.
	 */
	bool IsCookCommandletProcess()
	{
		const FString CmdLine(FCommandLine::Get());
		return CmdLine.Contains(TEXT("-run=Cook"),            ESearchCase::IgnoreCase)
			|| CmdLine.Contains(TEXT("-run=CookByTheBook"),   ESearchCase::IgnoreCase)
			|| CmdLine.Contains(TEXT("CookCommandlet"),       ESearchCase::IgnoreCase);
	}

	/**
	 * Universal pre-write fallback: clear the read-only attribute on a file
	 * if it has one set.
	 *
	 * Engine shader files in a source-engine workspace are typically marked
	 * read-only by Perforce until checked out.  The editor module's persist-
	 * mode SCC integration (CheckOutPatchedShadersIfPersist) handles the
	 * checkout when the user has opted into persist mode, but we still need
	 * a safety net for the cases:
	 *
	 *   - SCC is disabled in the project but the engine files happen to be
	 *     read-only (e.g., a manual chmod, or a shipped engine drop).
	 *   - Persist is OFF, so the editor module never tried to check anything
	 *     out.  In a non-persist workflow we still need writes to succeed -
	 *     the patches just won't survive shutdown, but they should apply
	 *     during the session.
	 *   - SCC checkout failed for some reason (login, network) but the user
	 *     still wants the session to continue.
	 *
	 * Honours the user's intent - they installed this plugin specifically to
	 * override engine shaders - by clearing the read-only bit so the write
	 * goes through.  No-op when the file is already writable.
	 */
	void EnsureFileWritable(const FString& Path)
	{
		// IFileManager has IsReadOnly but not SetReadOnly in 5.4-5.6 - the
		// SetReadOnly mutator only exists on the lower-level IPlatformFile
		// interface.  Reach for it via FPlatformFileManager so this compiles
		// across every supported engine version.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.IsReadOnly(*Path))
		{
			const bool bOk = PlatformFile.SetReadOnly(*Path, false);
			UE_LOG(LogShaderShift, Verbose,
				TEXT("Cleared read-only flag on %s (success: %s)"),
				*Path, bOk ? TEXT("yes") : TEXT("no"));
		}
	}
}

void FShaderShiftModule::SetPersistShaderChanges(bool bInPersist)
{
	if (bPersistShaderChanges != bInPersist)
	{
		UE_LOG(LogShaderShift, Log,
			TEXT("Persist Shader Changes in Engine: %s -> %s"),
			bPersistShaderChanges ? TEXT("ON") : TEXT("OFF"),
			bInPersist            ? TEXT("ON") : TEXT("OFF"));
	}
	bPersistShaderChanges = bInPersist;
}

bool FShaderShiftModule::ShouldPersistShaderChanges()
{
	return bPersistShaderChanges || IsCookCommandletProcess();
}

// ============================================================================
// FShaderShiftFileHook
// ============================================================================

FShaderShiftFileHook::FShaderShiftFileHook(
	const FGuid& InGuid,
	const FString& InVirtualPath,
	const FString& InTemplateSource,
	const FString& InDisplayName)
	: HookGuid(InGuid)
	, VirtualPath(InVirtualPath)
	, TemplateSource(InTemplateSource)
	, DisplayName(InDisplayName)
	, CachedState(EShaderHookState::Unknown)
{
	Filename = FPaths::GetCleanFilename(VirtualPath);

	if (DisplayName.IsEmpty())
	{
		DisplayName = Filename;
	}
}

FString FShaderShiftFileHook::GetPhysicalPath() const
{
	FString Relative = VirtualPath;
	if (Relative.RemoveFromStart(TEXT("/Engine/")))
	{
		return FPaths::Combine(FPaths::EngineDir(), TEXT("Shaders"), Relative);
	}

	UE_LOG(LogShaderShift, Warning,
		TEXT("Unexpected virtual path format: %s"), *VirtualPath);
	return FPaths::Combine(FPaths::EngineDir(), TEXT("Shaders"), TEXT("Private"), Filename);
}

FString FShaderShiftFileHook::GetBackupPath() const
{
	return GetPhysicalPath() + TEXT(".") + HookGuid.ToString(EGuidFormats::DigitsWithHyphens) + TEXT(".backup");
}

FString FShaderShiftFileHook::GetGuidMarker() const
{
	return FString::Printf(TEXT("// SHADER_HOOK: %s"),
		*HookGuid.ToString(EGuidFormats::DigitsWithHyphens));
}

// ---------------------------------------------------------------------------
// BuildFinalSource  - substitute #define values in the template
// ---------------------------------------------------------------------------

FString FShaderShiftFileHook::BuildFinalSource(
	const FString& InTemplate,
	const TArray<FShaderShiftDefineEntry>& Defines)
{
	FString Result = InTemplate;

	for (const FShaderShiftDefineEntry& Def : Defines)
	{
		if (Def.DefineName.IsEmpty() || Def.Value.IsEmpty())
		{
			continue;
		}

		const FString DefineToken = FString::Printf(TEXT("#define %s "), *Def.DefineName);
		const FString DefineTokenTab = FString::Printf(TEXT("#define\t%s "), *Def.DefineName);

		TArray<FString> Lines;
		Result.ParseIntoArray(Lines, TEXT("\n"), false);

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			FString& Line = Lines[i];
			FString Trimmed = Line;
			Trimmed.TrimStartInline();

			int32 DefIdx = INDEX_NONE;

			if (Trimmed.Contains(DefineToken))
			{
				DefIdx = Line.Find(DefineToken);
			}
			else if (Trimmed.Contains(DefineTokenTab))
			{
				DefIdx = Line.Find(DefineTokenTab);
			}
			else
			{
				continue;
			}

			if (DefIdx == INDEX_NONE)
			{
				continue;
			}

			int32 ValueStart = DefIdx + DefineToken.Len();
			if (!Line.Mid(DefIdx, DefineToken.Len()).Contains(DefineToken))
			{
				ValueStart = DefIdx + DefineTokenTab.Len();
			}

			FString Prefix = Line.Left(ValueStart);
			FString Rest = Line.Mid(ValueStart);

			FString TrailingComment;
			int32 CommentIdx = Rest.Find(TEXT("//"));
			if (CommentIdx != INDEX_NONE)
			{
				TrailingComment = Rest.Mid(CommentIdx);
			}

			if (TrailingComment.IsEmpty())
			{
				Line = Prefix + Def.Value;
			}
			else
			{
				Line = Prefix + Def.Value + TEXT("  ") + TrailingComment;
			}

			UE_LOG(LogShaderShift, Verbose,
				TEXT("  Substituted %s = %s"), *Def.DefineName, *Def.Value);
		}

		Result = FString::Join(Lines, TEXT("\n"));
	}

	return Result;
}

EShaderHookState FShaderShiftFileHook::EvaluateState(
	const TArray<FShaderShiftDefineEntry>& Defines) const
{
	const FString PhysicalPath = GetPhysicalPath();

	FString CurrentContent;
	if (!FFileHelper::LoadFileToString(CurrentContent, *PhysicalPath))
	{
		CachedState = EShaderHookState::Error;
		return CachedState;
	}

	const FString Marker = GetGuidMarker();

	if (!CurrentContent.Contains(Marker))
	{
		CachedState = EShaderHookState::NotApplied;
		return CachedState;
	}

	FString FinalSource = BuildFinalSource(TemplateSource, Defines);
	FString ExpectedContent = Marker + TEXT("\r\n") + FinalSource;

	CachedState = (CurrentContent == ExpectedContent)
		? EShaderHookState::Applied
		: EShaderHookState::Outdated;

	return CachedState;
}

EShaderApplyResult FShaderShiftFileHook::Apply(const TArray<FShaderShiftDefineEntry>& Defines)
{
	const FString PhysicalPath = GetPhysicalPath();
	const FString BackupPath = GetBackupPath();
	const FString Marker = GetGuidMarker();

	const FString FinalSource = BuildFinalSource(TemplateSource, Defines);
	const FString ExpectedContent = Marker + TEXT("\r\n") + FinalSource;

	FString OriginalContent;
	if (!FFileHelper::LoadFileToString(OriginalContent, *PhysicalPath))
	{
		UE_LOG(LogShaderShift, Error,
			TEXT("Failed to read engine shader: %s"), *PhysicalPath);
		return EShaderApplyResult::Failed;
	}

	// On-disk content already matches what we'd write - nothing to do.
	if (OriginalContent == ExpectedContent)
	{
		CachedState = EShaderHookState::Applied;
		UE_LOG(LogShaderShift, Verbose,
			TEXT("Hook '%s' already current - skipping disk write"), *DisplayName);
		return EShaderApplyResult::AlreadyCurrent;
	}

	UE_LOG(LogShaderShift, Log,
		TEXT("Hook '%s' content differs - on-disk %d chars vs expected %d chars"),
		*DisplayName, OriginalContent.Len(), ExpectedContent.Len());

	// First application: back up the original file.
	// If our marker is already present the backup must exist from the first apply.
	if (!OriginalContent.Contains(Marker))
	{
		if (!FPaths::FileExists(BackupPath))
		{
			if (!FFileHelper::SaveStringToFile(OriginalContent, *BackupPath, FFileHelper::EEncodingOptions::ForceUTF8))
			{
				UE_LOG(LogShaderShift, Error,
					TEXT("Failed to back up original shader: %s"), *BackupPath);
				return EShaderApplyResult::Failed;
			}
			UE_LOG(LogShaderShift, Log,
				TEXT("Backed up original: %s"), *PhysicalPath);
		}
	}

	// Universal fallback: strip the read-only attribute before writing so the
	// patch applies regardless of SCC checkout status or persist setting.
	// Necessary for source-engine builds where engine shaders are P4-tracked
	// and read-only by default; harmless on launcher engines where shader
	// files are already writable.
	EnsureFileWritable(PhysicalPath);

	if (!FFileHelper::SaveStringToFile(ExpectedContent, *PhysicalPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		UE_LOG(LogShaderShift, Error,
			TEXT("Failed to write shader override: %s"), *PhysicalPath);
		return EShaderApplyResult::Failed;
	}

	CachedState = EShaderHookState::Applied;

	UE_LOG(LogShaderShift, Log,
		TEXT("Applied shader hook '%s' -> %s"), *DisplayName, *VirtualPath);

	return EShaderApplyResult::WroteDisk;
}

bool FShaderShiftFileHook::Revert()
{
	const FString PhysicalPath = GetPhysicalPath();
	const FString BackupPath = GetBackupPath();

	if (!FPaths::FileExists(BackupPath))
	{
		UE_LOG(LogShaderShift, Warning,
			TEXT("No backup found for '%s' - cannot revert"), *DisplayName);
		return false;
	}

	FString BackupContent;
	if (!FFileHelper::LoadFileToString(BackupContent, *BackupPath))
	{
		UE_LOG(LogShaderShift, Error,
			TEXT("Failed to read backup: %s"), *BackupPath);
		return false;
	}

	// Same universal read-only strip as Apply().  If the user re-checked the
	// file into source control between sessions (P4 makes it read-only again
	// on revert/sync), Revert still needs to write the original content back.
	EnsureFileWritable(PhysicalPath);

	if (!FFileHelper::SaveStringToFile(BackupContent, *PhysicalPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		UE_LOG(LogShaderShift, Error,
			TEXT("Failed to restore shader: %s"), *PhysicalPath);
		return false;
	}

	IFileManager::Get().Delete(*BackupPath);
	CachedState = EShaderHookState::NotApplied;

	UE_LOG(LogShaderShift, Log,
		TEXT("Reverted shader hook '%s' - restored: %s"), *DisplayName, *PhysicalPath);

	return true;
}

bool FShaderShiftFileHook::IsApplied() const
{
	if (CachedState == EShaderHookState::Unknown)
	{
		TArray<FShaderShiftDefineEntry> Empty;
		EvaluateState(Empty);
	}

	return CachedState == EShaderHookState::Applied
		|| CachedState == EShaderHookState::Outdated;
}

// ============================================================================
// FShaderShiftHookRegistry
// ============================================================================

FShaderShiftHookRegistry& FShaderShiftHookRegistry::Get()
{
	static FShaderShiftHookRegistry Instance;
	return Instance;
}

bool FShaderShiftHookRegistry::RegisterHook(const FShaderShiftFileHook& Hook)
{
	FScopeLock Lock(&RegistryLock);

	if (!Hook.HookGuid.IsValid() || Hook.VirtualPath.IsEmpty() || Hook.TemplateSource.IsEmpty())
	{
		UE_LOG(LogShaderShift, Warning,
			TEXT("Cannot register hook - invalid data (path: %s)"), *Hook.VirtualPath);
		return false;
	}

	Hooks.Add(Hook.VirtualPath, Hook);

	UE_LOG(LogShaderShift, Log,
		TEXT("Registered hook: '%s' -> %s"), *Hook.DisplayName, *Hook.VirtualPath);

	return true;
}

bool FShaderShiftHookRegistry::RegisterHookFromFile(
	const FString& VirtualPath,
	const FString& SourceFilePath,
	const FString& DisplayName)
{
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *SourceFilePath))
	{
		UE_LOG(LogShaderShift, Error,
			TEXT("Failed to load template: %s"), *SourceFilePath);
		return false;
	}

	const uint32 H1 = GetTypeHash(VirtualPath);
	const uint32 H2 = GetTypeHash(SourceFilePath);
	const FGuid Guid(H1, H2, 0x53484452 /* "SHDR" */, 0x48595045 /* "HYPE" */);

	FShaderShiftFileHook Hook(Guid, VirtualPath, Source, DisplayName);
	return RegisterHook(Hook);
}

bool FShaderShiftHookRegistry::UnregisterHook(const FGuid& HookGuid)
{
	FScopeLock Lock(&RegistryLock);

	for (auto It = Hooks.CreateIterator(); It; ++It)
	{
		if (It->Value.HookGuid == HookGuid)
		{
			It.RemoveCurrent();
			return true;
		}
	}
	return false;
}

bool FShaderShiftHookRegistry::UnregisterHookByPath(const FString& VirtualPath)
{
	FScopeLock Lock(&RegistryLock);
	return Hooks.Remove(VirtualPath) > 0;
}

bool FShaderShiftHookRegistry::HasHook(const FString& VirtualPath) const
{
	FScopeLock Lock(&RegistryLock);
	return Hooks.Contains(VirtualPath);
}

const FShaderShiftFileHook* FShaderShiftHookRegistry::FindHook(const FString& VirtualPath) const
{
	FScopeLock Lock(&RegistryLock);
	return Hooks.Find(VirtualPath);
}

FShaderShiftFileHook* FShaderShiftHookRegistry::FindHookMutable(const FString& VirtualPath)
{
	FScopeLock Lock(&RegistryLock);
	return Hooks.Find(VirtualPath);
}

void FShaderShiftHookRegistry::GetAllHooks(TArray<const FShaderShiftFileHook*>& OutHooks) const
{
	FScopeLock Lock(&RegistryLock);
	OutHooks.Reset(Hooks.Num());
	for (const auto& Pair : Hooks)
	{
		OutHooks.Add(&Pair.Value);
	}
}

int32 FShaderShiftHookRegistry::ApplyAllEnabledHooks(int32& OutDiskWriteCount, bool& bOutNeedsGlobalRecompile)
{
	FScopeLock Lock(&RegistryLock);

	int32 SuccessCount        = 0;
	OutDiskWriteCount         = 0;
	bOutNeedsGlobalRecompile  = false;

	for (auto& Pair : Hooks)
	{
		FShaderShiftFileHook& Hook = Pair.Value;

		if (!Hook.bEnabled)
		{
			if (Hook.IsApplied())
			{
				Hook.Revert();
			}
			continue;
		}

		const EShaderApplyResult Result = Hook.Apply(Hook.PendingDefines);

		switch (Result)
		{
			case EShaderApplyResult::WroteDisk:
				SuccessCount++;
				OutDiskWriteCount++;
				if (Hook.bRequiresGlobalRecompile)
				{
					bOutNeedsGlobalRecompile = true;
				}
				break;

			case EShaderApplyResult::AlreadyCurrent:
				SuccessCount++;
				break;

			case EShaderApplyResult::Failed:
				UE_LOG(LogShaderShift, Error,
					TEXT("Failed to apply hook: %s"), *Hook.DisplayName);
				break;
		}
	}

	return SuccessCount;
}

int32 FShaderShiftHookRegistry::ApplyAllEnabledHooks()
{
	int32 Dummy = 0;
	bool bDummyGlobal = false;
	return ApplyAllEnabledHooks(Dummy, bDummyGlobal);
}

int32 FShaderShiftHookRegistry::RevertAllHooks()
{
	FScopeLock Lock(&RegistryLock);

	int32 Count = 0;
	for (auto& Pair : Hooks)
	{
		if (Pair.Value.IsApplied())
		{
			if (Pair.Value.Revert())
			{
				Count++;
			}
		}
	}
	return Count;
}

bool FShaderShiftHookRegistry::ApplyHook(const FString& VirtualPath)
{
	FScopeLock Lock(&RegistryLock);

	FShaderShiftFileHook* Hook = Hooks.Find(VirtualPath);
	if (!Hook)
	{
		return false;
	}

	const EShaderApplyResult Result = Hook->Apply(Hook->PendingDefines);
	return Result != EShaderApplyResult::Failed;
}

bool FShaderShiftHookRegistry::RevertHook(const FString& VirtualPath)
{
	FScopeLock Lock(&RegistryLock);
	FShaderShiftFileHook* Hook = Hooks.Find(VirtualPath);
	return Hook ? Hook->Revert() : false;
}

int32 FShaderShiftHookRegistry::GetHookCount() const
{
	FScopeLock Lock(&RegistryLock);
	return Hooks.Num();
}

void FShaderShiftHookRegistry::ClearAll()
{
	FScopeLock Lock(&RegistryLock);
	Hooks.Empty();
}

void FShaderShiftHookRegistry::TriggerShaderRecompile()
{
#if WITH_EDITOR
	FlushShaderFileCache();

	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("recompileshaders changed"));

		UE_LOG(LogShaderShift, Log,
			TEXT("Issued 'recompileshaders changed' with cache flush"));
		UE_LOG(LogShaderShift, Verbose,
			TEXT("  Tip: Shader mode changes require full recompilation of dependent shaders."));
		UE_LOG(LogShaderShift, Verbose,
			TEXT("       This may take 10-60 seconds depending on project size."));
	}
#endif
}

void FShaderShiftHookRegistry::RevertStaleHooks()
{
	const TArray<FString> SearchDirs = {
		FPaths::Combine(FPaths::EngineDir(), TEXT("Shaders"), TEXT("Private")),
		FPaths::Combine(FPaths::EngineDir(), TEXT("Shaders"), TEXT("Public"))
	};

	int32 RestoredCount = 0;

	for (const FString& SearchDir : SearchDirs)
	{
		TArray<FString> BackupFiles;
		IFileManager::Get().FindFilesRecursive(
			BackupFiles, *SearchDir, TEXT("*.backup"), true, false);

		for (const FString& BackupPath : BackupFiles)
		{
			FString WithoutBackup = BackupPath;
			if (!WithoutBackup.RemoveFromEnd(TEXT(".backup")))
			{
				continue;
			}

			int32 GuidDotIdx = INDEX_NONE;
			if (WithoutBackup.Len() > 37)
			{
				int32 CandidateIdx = WithoutBackup.Len() - 36 - 1;
				if (CandidateIdx >= 0 && WithoutBackup[CandidateIdx] == TEXT('.'))
				{
					FString MaybeGuid = WithoutBackup.Mid(CandidateIdx + 1);
					FGuid Parsed;
					if (FGuid::Parse(MaybeGuid, Parsed))
					{
						GuidDotIdx = CandidateIdx;
					}
				}
			}

			FString OriginalPath;
			if (GuidDotIdx != INDEX_NONE)
			{
				OriginalPath = WithoutBackup.Left(GuidDotIdx);
			}
			else
			{
				UE_LOG(LogShaderShift, Warning,
					TEXT("Found backup file but cannot determine original path: %s"),
					*BackupPath);
				continue;
			}

			FString BackupContent;
			if (!FFileHelper::LoadFileToString(BackupContent, *BackupPath))
			{
				UE_LOG(LogShaderShift, Error,
					TEXT("Failed to read stale backup: %s"), *BackupPath);
				continue;
			}

			// Same read-only strip as Apply/Revert - if a previous unclean
			// shutdown left the file patched and it has since been re-checked
			// into P4 (or simply marked read-only), the recovery write would
			// otherwise fail and leave the user's engine in a patched state.
			EnsureFileWritable(OriginalPath);

			if (!FFileHelper::SaveStringToFile(BackupContent, *OriginalPath, FFileHelper::EEncodingOptions::ForceUTF8))
			{
				UE_LOG(LogShaderShift, Error,
					TEXT("Failed to restore shader from stale backup: %s -> %s"),
					*BackupPath, *OriginalPath);
				continue;
			}

			IFileManager::Get().Delete(*BackupPath);
			RestoredCount++;

			UE_LOG(LogShaderShift, Warning,
				TEXT("Recovered stale shader override - restored: %s"), *OriginalPath);
		}
	}

	if (RestoredCount > 0)
	{
		UE_LOG(LogShaderShift, Warning,
			TEXT("Recovered %d stale shader overrides from a previous unclean shutdown"),
			RestoredCount);
	}
}

// ============================================================================
// FShaderShiftModule
// ============================================================================

void FShaderShiftModule::StartupModule()
{
	UE_LOG(LogShaderShift, Log, TEXT("ShaderShift starting up..."));

	// Resolve the plugin base directory via the plugin manager so we work in
	// every install scenario:
	//   - <ProjectFolder>/Plugins/ShaderShift               (project plugin)
	//   - <EngineInstall>/Engine/Plugins/Marketplace/...    (Fab / engine plugin)
	//   - any other Engine/Plugins location
	//
	// IPluginManager has finished plugin discovery before any plugin module
	// loads, so this is safe at PostConfigInit. The hardcoded
	// FPaths::ProjectPluginsDir() fallback only ever applied for project
	// installs and silently broke engine-installed plugins shipped via Fab -
	// LoadShaderHookConfigs would not find Config/ShaderOverride.ini and the
	// Shaders/ template files because they live next to the engine .uplugin,
	// not in the project's Plugins folder.
	TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("ShaderShift"));

	if (Plugin.IsValid())
	{
		PluginDirectory = Plugin->GetBaseDir();
	}
	else
	{
		// Fallback only - should not happen because the plugin manager already
		// discovered us in order to load this module. Kept for diagnostics.
		PluginDirectory = FPaths::ProjectPluginsDir() / TEXT("ShaderShift");
		UE_LOG(LogShaderShift, Warning,
			TEXT("IPluginManager could not locate 'ShaderShift'; falling back to: %s"),
			*PluginDirectory);
	}

	ShadersDirectory = FPaths::Combine(PluginDirectory, TEXT("Shaders"));

	UE_LOG(LogShaderShift, Log,
		TEXT("Plugin base directory resolved to: %s"), *PluginDirectory);

	// Read the persist flag from the user's saved config *before* the
	// stale-backup sweep so a previous session that intentionally left
	// shaders patched is not undone here.  Cook commandlets always persist
	// regardless of the user setting (handled inside ShouldPersistShaderChanges).
	bPersistShaderChanges = ReadPersistFlagFromSavedConfig();
	const bool bShouldPersist = ShouldPersistShaderChanges();

	if (bShouldPersist)
	{
		UE_LOG(LogShaderShift, Log,
			TEXT("Persist mode active (user setting: %s, cook commandlet: %s) - "
				 "skipping stale-backup recovery"),
			bPersistShaderChanges            ? TEXT("ON")  : TEXT("OFF"),
			IsCookCommandletProcess()        ? TEXT("YES") : TEXT("NO"));
	}
	else
	{
		FShaderShiftHookRegistry::RevertStaleHooks();
	}

	LoadShaderHookConfigs();

	// Read saved user settings from ShaderShift.ini and apply patched shaders
	// to disk *before* the engine compiles them.  This avoids a costly
	// post-startup recompile - the engine will compile the already-patched
	// files during its normal startup shader compilation.
	EarlyApplySavedSettings();

	// Crash and pre-exit handlers are always registered; they consult the
	// runtime persist flag (which may toggle mid-session) before deciding
	// whether to revert.  This way a user can flip the checkbox during a
	// session and the most recent value wins on shutdown.
	RegisterCrashHandlers();

	UE_LOG(LogShaderShift, Log,
		TEXT("Runtime startup complete - %d hooks registered (Editor module will apply)"),
		FShaderShiftHookRegistry::Get().GetHookCount());
}

void FShaderShiftModule::ShutdownModule()
{
	UE_LOG(LogShaderShift, Log, TEXT("ShaderShift shutting down..."));

	UnregisterCrashHandlers();

	if (ShouldPersistShaderChanges())
	{
		UE_LOG(LogShaderShift, Log,
			TEXT("  Persist mode active - leaving patched shaders on disk "
				 "(user setting: %s, cook commandlet: %s)"),
			bPersistShaderChanges     ? TEXT("ON")  : TEXT("OFF"),
			IsCookCommandletProcess() ? TEXT("YES") : TEXT("NO"));
	}
	else
	{
		const int32 Reverted = FShaderShiftHookRegistry::Get().RevertAllHooks();
		UE_LOG(LogShaderShift, Log, TEXT("  Reverted %d hooks"), Reverted);
	}

	// Always clear the in-memory registry - it is process-local state, not
	// related to the on-disk shader files.
	FShaderShiftHookRegistry::Get().ClearAll();
}

void FShaderShiftModule::RegisterCrashHandlers()
{
	OnShutdownAfterErrorHandle = FCoreDelegates::OnShutdownAfterError.AddLambda([]()
	{
		if (FShaderShiftModule::ShouldPersistShaderChanges())
		{
			UE_LOG(LogShaderShift, Warning,
				TEXT("Crash detected - persist mode active, leaving shaders patched"));
			return;
		}
		UE_LOG(LogShaderShift, Warning,
			TEXT("Crash detected - reverting shader hooks"));
		FShaderShiftHookRegistry::Get().RevertAllHooks();
	});

	OnEnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([]()
	{
		if (FShaderShiftModule::ShouldPersistShaderChanges())
		{
			UE_LOG(LogShaderShift, Log,
				TEXT("PreExit - persist mode active, leaving shaders patched"));
			return;
		}
		FShaderShiftHookRegistry::Get().RevertAllHooks();
	});

	UE_LOG(LogShaderShift, Verbose,
		TEXT("Registered crash and pre-exit handlers"));
}

void FShaderShiftModule::UnregisterCrashHandlers()
{
	if (OnShutdownAfterErrorHandle.IsValid())
	{
		FCoreDelegates::OnShutdownAfterError.Remove(OnShutdownAfterErrorHandle);
		OnShutdownAfterErrorHandle.Reset();
	}

	if (OnEnginePreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(OnEnginePreExitHandle);
		OnEnginePreExitHandle.Reset();
	}
}

void FShaderShiftModule::LoadShaderHookConfigs()
{
	const FString ConfigPath = FPaths::Combine(PluginDirectory, TEXT("Config/ShaderOverride.ini"));

	if (!FPaths::FileExists(ConfigPath))
	{
		UE_LOG(LogShaderShift, Warning,
			TEXT("Plugin config not found: %s"), *ConfigPath);
		return;
	}

	FConfigFile PluginConfig;
	PluginConfig.Read(ConfigPath);

	const FConfigSection* ShadersSection = PluginConfig.FindSection(TEXT("Shaders"));
	if (!ShadersSection)
	{
		UE_LOG(LogShaderShift, Warning, TEXT("No [Shaders] section in %s"), *ConfigPath);
		return;
	}

	TArray<FString> ShaderEntries;
	for (auto It = ShadersSection->CreateConstIterator(); It; ++It)
	{
		if (It->Key == TEXT("+Shader"))
		{
			ShaderEntries.Add(It->Value.GetValue());
		}
	}

	for (const FString& Entry : ShaderEntries)
	{
		FString VirtualPath, TemplateFilename, HookDisplayName;

		FParse::Value(*Entry, TEXT("VirtualPath="), VirtualPath);
		FParse::Value(*Entry, TEXT("TemplateFilename="), TemplateFilename);
		FParse::Value(*Entry, TEXT("DisplayName="), HookDisplayName);

		VirtualPath = VirtualPath.TrimQuotes();
		TemplateFilename = TemplateFilename.TrimQuotes();
		HookDisplayName = HookDisplayName.TrimQuotes();

		if (VirtualPath.IsEmpty() || TemplateFilename.IsEmpty())
		{
			UE_LOG(LogShaderShift, Warning,
				TEXT("Incomplete shader entry: %s"), *Entry);
			continue;
		}

		const FString TemplatePath = FPaths::Combine(ShadersDirectory, TemplateFilename);
		if (!FPaths::FileExists(TemplatePath))
		{
			UE_LOG(LogShaderShift, Warning,
				TEXT("Template file not found: %s"), *TemplatePath);
			continue;
		}

		FShaderShiftHookRegistry::Get().RegisterHookFromFile(
			VirtualPath, TemplatePath, HookDisplayName);
	}

	UE_LOG(LogShaderShift, Log,
		TEXT("Registered %d shader hooks from plugin config"),
		ShaderEntries.Num());
}

// ---------------------------------------------------------------------------
// EarlyApplySavedSettings
//
// Reads the saved user config (ShaderShift.ini) directly from disk - without
// the UObject system - and applies patched shaders before the engine's shader
// compiler runs.  This means the engine compiles the patched versions on its
// normal startup pass, avoiding a 1-2 minute post-startup recompile for BRDF
// changes.
//
// The config file is the same one UShaderShiftSettings (config = ShaderShift)
// writes to via SaveConfig().  The section name follows UE's convention:
//   [/Script/ShaderShiftEditor.ShaderShiftSettings]
//
// We read:
//   - The UENUM integer values (TonemapMode, AgxLook, DiffuseMode, SpecularMode)
//   - The per-hook ShaderHooks array entries (bEnabled, Defines)
//
// For the UENUM properties we map them to their corresponding define names
// exactly as UShaderShiftSettings::SyncEnumsToDefines() does, but without
// needing the CDO.
// ---------------------------------------------------------------------------

void FShaderShiftModule::EarlyApplySavedSettings()
{
	const FString SectionName =
		TEXT("/Script/ShaderShiftEditor.ShaderShiftSettings");

	// -------------------------------------------------------------------
	// 1. Load the user's saved ShaderShift.ini directly - NO hierarchy merge.
	//
	//    FConfigCacheIni::LoadLocalIniFile with bIsBaseIniName=true performs
	//    the full UE config hierarchy merge (Base → Default → Platform →
	//    Saved), which means CDO defaults from DefaultShaderShift.ini
	//    override the user's actual saved values.  Instead we read the
	//    user's Saved/Config/<Platform>/ShaderShift.ini directly.
	// -------------------------------------------------------------------

	// Build the config path explicitly.  FPaths::GeneratedConfigDir() may not
	// include the platform subdirectory this early in startup.  We construct
	// the path the same way UE does: Saved/Config/{PlatformName}/ShaderShift.ini
	// where PlatformName is e.g. "WindowsEditor" in editor builds.
	const FString PlatformConfigDir = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Config"),
		FPlatformProperties::PlatformName(), TEXT("ShaderShift.ini"));

	// Also try the GeneratedConfigDir path and the bare Config path as fallbacks.
	const FString GeneratedPath = FPaths::Combine(
		FPaths::GeneratedConfigDir(), TEXT("ShaderShift.ini"));
	const FString BareConfigPath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("ShaderShift.ini"));

	FString ResolvedConfigPath;
	if (FPaths::FileExists(PlatformConfigDir))
	{
		ResolvedConfigPath = PlatformConfigDir;
	}
	else if (FPaths::FileExists(GeneratedPath))
	{
		ResolvedConfigPath = GeneratedPath;
	}
	else if (FPaths::FileExists(BareConfigPath))
	{
		ResolvedConfigPath = BareConfigPath;
	}
	else
	{
		UE_LOG(LogShaderShift, Log,
			TEXT("EarlyApplySavedSettings - ShaderShift.ini not found (tried '%s'), skipping"),
			*PlatformConfigDir);
		return;
	}

	UE_LOG(LogShaderShift, Log,
		TEXT("EarlyApplySavedSettings - reading config from: %s"), *ResolvedConfigPath);

	FConfigFile SavedConfig;
	SavedConfig.Read(ResolvedConfigPath);

	const FConfigSection* SettingsSection = SavedConfig.FindSection(*SectionName);
	if (!SettingsSection)
	{
		UE_LOG(LogShaderShift, Log,
			TEXT("EarlyApplySavedSettings - section '%s' not found in config, skipping"),
			*SectionName);
		return;
	}

	// -------------------------------------------------------------------
	// 2. Read UENUM values and build the define map.
	//
	//    UE only writes properties whose values differ from CDO defaults.
	//    For properties not present in the ini, we use the CDO defaults
	//    which must match UShaderShiftSettings defaults exactly.
	// -------------------------------------------------------------------

	TMap<FString, FString> EnumDefines;

	// CDO default enum integer values - must match UShaderShiftSettings defaults.
	// These are used when a key is absent from the ini (meaning the user has
	// the default value and UE didn't write it).
	//
	// Name-to-integer maps for each enum - UE config stores the enum entry
	// name (e.g., "Golden"), not the integer.  We can't use FindObject<UEnum>
	// here because the ShaderShiftEditor module (where the enums are declared)
	// hasn't loaded yet at PostConfigInit.  Since the enum values are stable
	// and defined by the plugin itself, a static lookup table is reliable.

	struct FEnumMapping
	{
		const TCHAR*            ConfigKey;
		FString                 DefineName;
		int32                   DefaultIntValue;
		TMap<FString, int32>    NameToInt;
	};

	TArray<FEnumMapping> Mappings;

	// IMPORTANT: DefaultIntValue must stay in sync with the in-class
	// initializers on UShaderShiftSettings (ShaderShiftSettings.h).  The
	// editor module CDO is not available at PostConfigInit, so this code
	// has its own copy of those defaults.  Mismatched values cause the
	// shaders patched at PostConfigInit to disagree with what the panel
	// shows after the editor module loads - which forces an unnecessary
	// post-startup recompile and can flicker the viewport.

	// ECustomTonemapMode - engine default = ACES_Filmic (0)
	{
		FEnumMapping M;
		M.ConfigKey       = TEXT("TonemapMode");
		M.DefineName      = TEXT("CUSTOM_TONEMAP_MODE");
		M.DefaultIntValue = 0; // ACES_Filmic - engine default
		M.NameToInt.Add(TEXT("ACES_Filmic"),   0);
		M.NameToInt.Add(TEXT("AgX_Punchy"),     1);
		M.NameToInt.Add(TEXT("Reinhard"),       2);
		M.NameToInt.Add(TEXT("Linear"),         3);
		M.NameToInt.Add(TEXT("Custom"),         4);
		Mappings.Add(MoveTemp(M));
	}

	// EAgxLook - neutral default
	{
		FEnumMapping M;
		M.ConfigKey       = TEXT("AgxLook");
		M.DefineName      = TEXT("AGX_LOOK");
		M.DefaultIntValue = 0; // Default - neutral
		M.NameToInt.Add(TEXT("Default"), 0);
		M.NameToInt.Add(TEXT("Golden"),  1);
		M.NameToInt.Add(TEXT("Punchy"),  2);
		Mappings.Add(MoveTemp(M));
	}

	// ECustomDiffuseMode - engine default
	{
		FEnumMapping M;
		M.ConfigKey       = TEXT("DiffuseMode");
		M.DefineName      = TEXT("CUSTOM_DIFFUSE_MODE");
		M.DefaultIntValue = 0; // EngineDefault
		M.NameToInt.Add(TEXT("EngineDefault"), 0);
		M.NameToInt.Add(TEXT("OrenNayar"),     1);
		M.NameToInt.Add(TEXT("DisneyBurley"),  2);
		M.NameToInt.Add(TEXT("LambertSphere"), 3);
		M.NameToInt.Add(TEXT("Gotanda"),       4);
		Mappings.Add(MoveTemp(M));
	}

	// ECustomSpecMode - engine default (single-lobe GGX)
	{
		FEnumMapping M;
		M.ConfigKey       = TEXT("SpecularMode");
		M.DefineName      = TEXT("CUSTOM_SPEC_MODE");
		M.DefaultIntValue = 0; // EngineDefault
		M.NameToInt.Add(TEXT("EngineDefault"), 0);
		M.NameToInt.Add(TEXT("MultiLobe"),     1);
		Mappings.Add(MoveTemp(M));
	}

	// ECustomBloomMode
	{
		FEnumMapping M;
		M.ConfigKey       = TEXT("BloomMode");
		M.DefineName      = TEXT("CUSTOM_BLOOM_MODE");
		M.DefaultIntValue = 0; // EngineDefault
		M.NameToInt.Add(TEXT("EngineDefault"),      0);
		M.NameToInt.Add(TEXT("DualKawase"),         1);
		M.NameToInt.Add(TEXT("CODScatterAsGather"), 2);
		M.NameToInt.Add(TEXT("FFTDiffraction"),     3);
		M.NameToInt.Add(TEXT("AnamorphicStreak"),   4);
		M.NameToInt.Add(TEXT("SpencerOcular"),      5);
		M.NameToInt.Add(TEXT("AnimeHardGlow"),      6);
		M.NameToInt.Add(TEXT("RetroCRT"),           7);
		Mappings.Add(MoveTemp(M));
	}

	for (const FEnumMapping& Mapping : Mappings)
	{
		const FConfigValue* FoundValue = SettingsSection->Find(Mapping.ConfigKey);

		int32 IntValue = Mapping.DefaultIntValue;

		if (FoundValue)
		{
			const FString& RawValue = FoundValue->GetValue();

			if (RawValue.IsNumeric())
			{
				IntValue = FCString::Atoi(*RawValue);
			}
			else if (const int32* Found = Mapping.NameToInt.Find(RawValue))
			{
				IntValue = *Found;
			}
			else
			{
				UE_LOG(LogShaderShift, Warning,
					TEXT("  Early config: unknown value '%s' for %s - using default %d"),
					*RawValue, Mapping.ConfigKey, Mapping.DefaultIntValue);
				IntValue = Mapping.DefaultIntValue;
			}

			UE_LOG(LogShaderShift, Log,
				TEXT("  Early config: %s = %d (raw: '%s')"),
				*Mapping.DefineName, IntValue, *RawValue);
		}
		else
		{
			UE_LOG(LogShaderShift, Log,
				TEXT("  Early config: %s not in ini - using CDO default %d"),
				*Mapping.DefineName, IntValue);
		}

		EnumDefines.Add(Mapping.DefineName, FString::FromInt(IntValue));
	}

	// Boolean toggles - same idea as the enum mapping above but parsed as
	// "True"/"False" since UPROPERTY(config, bool) serialises that way.
	{
		struct FBoolMapping
		{
			const TCHAR* ConfigKey;
			const TCHAR* DefineName;
			bool         DefaultValue;
		};

		const FBoolMapping BoolMappings[] =
		{
			{ TEXT("bPreserveEmissiveColor"), TEXT("CUSTOM_BLOOM_PRESERVE_COLOR"), false },
		};

		for (const FBoolMapping& B : BoolMappings)
		{
			bool bValue = B.DefaultValue;

			if (const FConfigValue* FoundValue = SettingsSection->Find(B.ConfigKey))
			{
				const FString& Raw = FoundValue->GetValue();
				bValue = Raw.ToBool();

				UE_LOG(LogShaderShift, Log,
					TEXT("  Early config: %s = %s (raw: '%s')"),
					B.DefineName, bValue ? TEXT("1") : TEXT("0"), *Raw);
			}
			else
			{
				UE_LOG(LogShaderShift, Log,
					TEXT("  Early config: %s not in ini - using CDO default %s"),
					B.DefineName, bValue ? TEXT("1") : TEXT("0"));
			}

			EnumDefines.Add(B.DefineName, bValue ? TEXT("1") : TEXT("0"));
		}
	}

	// -------------------------------------------------------------------
	// 2. Read per-hook defines from the plugin config (ShaderOverride.ini)
	//    to get the define metadata (names, defaults).
	// -------------------------------------------------------------------

	const FString PluginConfigPath =
		FPaths::Combine(PluginDirectory, TEXT("Config/ShaderOverride.ini"));

	FConfigFile PluginConfig;
	if (FPaths::FileExists(PluginConfigPath))
	{
		PluginConfig.Read(PluginConfigPath);
	}

	// -------------------------------------------------------------------
	// 3. For each registered hook, build defines and apply to disk.
	// -------------------------------------------------------------------

	FShaderShiftHookRegistry& Registry = FShaderShiftHookRegistry::Get();

	TArray<const FShaderShiftFileHook*> AllHooks;
	Registry.GetAllHooks(AllHooks);

	for (const FShaderShiftFileHook* ConstHook : AllHooks)
	{
		FShaderShiftFileHook* Hook = Registry.FindHookMutable(ConstHook->VirtualPath);
		if (!Hook)
		{
			continue;
		}

		// Read defines for this hook's template from the plugin config.
		const FConfigSection* DefineSection =
			PluginConfig.FindSection(Hook->Filename);

		TArray<FShaderShiftDefineEntry> Defines;

		if (DefineSection)
		{
			for (auto It = DefineSection->CreateConstIterator(); It; ++It)
			{
				if (It->Key != TEXT("+Define"))
				{
					continue;
				}

				const FString& DefEntry = It->Value.GetValue();

				FShaderShiftDefineEntry Def;
				FString DefName, DefDefault;

				FParse::Value(*DefEntry, TEXT("DefineName="),   DefName);
				FParse::Value(*DefEntry, TEXT("DefaultValue="), DefDefault);

				Def.DefineName   = DefName.TrimQuotes();
				Def.DefaultValue = DefDefault.TrimQuotes();
				Def.Value        = Def.DefaultValue;

				if (!Def.DefineName.IsEmpty())
				{
					Defines.Add(Def);
				}
			}
		}

		// Override define values with the saved UENUM settings.
		for (FShaderShiftDefineEntry& Def : Defines)
		{
			if (const FString* EnumVal = EnumDefines.Find(Def.DefineName))
			{
				Def.Value = *EnumVal;
			}
		}

		// Stamp onto the hook and apply.
		Hook->bEnabled       = true;  // assume enabled for early apply
		Hook->PendingDefines = Defines;
	}

	int32 DiskWriteCount = 0;
	bool bNeedsGlobal = false;
	const int32 Applied = Registry.ApplyAllEnabledHooks(DiskWriteCount, bNeedsGlobal);

	UE_LOG(LogShaderShift, Log,
		TEXT("EarlyApplySavedSettings - %d hooks applied, %d wrote to disk (before engine shader compile)"),
		Applied, DiskWriteCount);
}

void FShaderShiftModule::SyncSettingsWithHooks()
{
	// Intentionally empty in the Runtime module.
}

bool FShaderShiftModule::RegisterShaderOverride(
	const FString& VirtualPath, const FString& ShaderSource)
{
	const uint32 H = GetTypeHash(VirtualPath);
	const FGuid Guid(H, GetTypeHash(ShaderSource.Left(64)), 0x53484452, 0x48595045);

	FShaderShiftFileHook Hook(Guid, VirtualPath, ShaderSource);
	if (!FShaderShiftHookRegistry::Get().RegisterHook(Hook))
	{
		return false;
	}

	return FShaderShiftHookRegistry::Get().ApplyHook(VirtualPath);
}

bool FShaderShiftModule::RegisterShaderOverrideFromFile(
	const FString& VirtualPath, const FString& ShaderFilePath)
{
	if (!FShaderShiftHookRegistry::Get().RegisterHookFromFile(VirtualPath, ShaderFilePath))
	{
		return false;
	}
	return FShaderShiftHookRegistry::Get().ApplyHook(VirtualPath);
}

bool FShaderShiftModule::UnregisterShaderOverride(
	const FString& VirtualPath)
{
	FShaderShiftHookRegistry::Get().RevertHook(VirtualPath);
	return FShaderShiftHookRegistry::Get().UnregisterHookByPath(VirtualPath);
}

FShaderShiftHookRegistry& FShaderShiftModule::GetRegistry()
{
	return FShaderShiftHookRegistry::Get();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FShaderShiftModule, ShaderShift)
