// Copyright Hyperdyne Systems LLC. All Rights Reserved.

#include "ShaderOverridePlugin_InMemory.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PathViews.h"
#include "ShaderCore.h"
#include "GlobalShader.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogShaderOverrideInMemory);

#define LOCTEXT_NAMESPACE "FShaderOverridePlugin_InMemoryModule"

//-----------------------------------------------------------------------------
// FInMemoryShaderRegistry Implementation
//-----------------------------------------------------------------------------

FInMemoryShaderRegistry& FInMemoryShaderRegistry::Get()
{
	static FInMemoryShaderRegistry Instance;
	return Instance;
}

bool FInMemoryShaderRegistry::RegisterShader(const FString& Filename, const FString& SourceCode)
{
	FScopeLock Lock(&RegistryLock);

	if (Filename.IsEmpty() || SourceCode.IsEmpty())
	{
		UE_LOG(LogShaderOverrideInMemory, Warning, TEXT("Cannot register shader with empty filename or source"));
		return false;
	}

	TSharedPtr<FInMemoryShader> Shader = MakeShareable(new FInMemoryShader(Filename, SourceCode));
	Shader->VirtualPath = FString::Printf(TEXT("/Engine/Private/%s"), *Filename);

	Shaders.Add(Filename, Shader);

	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("Registered in-memory shader: %s (%d bytes)"),
		*Filename, Shader->SourceBytes.Num());

	return true;
}

bool FInMemoryShaderRegistry::RegisterShaderFromFile(const FString& Filename, const FString& FilePath)
{
	FString SourceCode;
	if (!FFileHelper::LoadFileToString(SourceCode, *FilePath))
	{
		UE_LOG(LogShaderOverrideInMemory, Error, TEXT("Failed to load shader file: %s"), *FilePath);
		return false;
	}

	return RegisterShader(Filename, SourceCode);
}

bool FInMemoryShaderRegistry::UnregisterShader(const FString& Filename)
{
	FScopeLock Lock(&RegistryLock);

	if (Shaders.Remove(Filename) > 0)
	{
		UE_LOG(LogShaderOverrideInMemory, Log, TEXT("Unregistered in-memory shader: %s"), *Filename);
		return true;
	}

	return false;
}

bool FInMemoryShaderRegistry::HasShader(const FString& Filename) const
{
	FScopeLock Lock(&RegistryLock);
	return Shaders.Contains(Filename);
}

bool FInMemoryShaderRegistry::GetShaderSource(const FString& Filename, FString& OutSource) const
{
	FScopeLock Lock(&RegistryLock);

	const TSharedPtr<FInMemoryShader>* Found = Shaders.Find(Filename);
	if (Found && (*Found)->bActive)
	{
		OutSource = (*Found)->SourceCode;
		return true;
	}

	return false;
}

bool FInMemoryShaderRegistry::GetShaderBytes(const FString& Filename, TArray<uint8>& OutBytes) const
{
	FScopeLock Lock(&RegistryLock);

	const TSharedPtr<FInMemoryShader>* Found = Shaders.Find(Filename);
	if (Found && (*Found)->bActive)
	{
		OutBytes = (*Found)->SourceBytes;
		return true;
	}

	return false;
}

void FInMemoryShaderRegistry::GetRegisteredShaders(TArray<FString>& OutFilenames) const
{
	FScopeLock Lock(&RegistryLock);

	for (const auto& Pair : Shaders)
	{
		OutFilenames.Add(Pair.Key);
	}
}

void FInMemoryShaderRegistry::ClearAll()
{
	FScopeLock Lock(&RegistryLock);
	Shaders.Empty();
	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("Cleared all in-memory shaders"));
}

int32 FInMemoryShaderRegistry::GetShaderCount() const
{
	FScopeLock Lock(&RegistryLock);
	return Shaders.Num();
}

//-----------------------------------------------------------------------------
// FMemoryFileHandle Implementation
//-----------------------------------------------------------------------------

FMemoryFileHandle::FMemoryFileHandle(const TArray<uint8>& InData)
	: Data(InData)
	, Position(0)
	, bIsOpen(true)
{
}

FMemoryFileHandle::~FMemoryFileHandle()
{
	//Close();
}

int64 FMemoryFileHandle::Tell()
{
	return Position;
}

bool FMemoryFileHandle::Seek(int64 NewPosition)
{
	if (NewPosition < 0 || NewPosition > Data.Num())
	{
		return false;
	}
	Position = NewPosition;
	return true;
}

bool FMemoryFileHandle::SeekFromEnd(int64 NewPositionRelativeToEnd)
{
	int64 NewPosition = Data.Num() + NewPositionRelativeToEnd;
	return Seek(NewPosition);
}

bool FMemoryFileHandle::Read(uint8* Destination, int64 BytesToRead)
{
	if (!bIsOpen)
	{
		return false;
	}

	int64 BytesAvailable = Data.Num() - Position;
	int64 BytesToCopy = FMath::Min(BytesToRead, BytesAvailable);

	if (BytesToCopy <= 0)
	{
		return false;
	}

	FMemory::Memcpy(Destination, Data.GetData() + Position, BytesToCopy);
	Position += BytesToCopy;

	return BytesToCopy == BytesToRead;
}

bool FMemoryFileHandle::ReadAt(uint8* Destination, int64 BytesToRead, int64 Offset)
{
	return false;
}

bool FMemoryFileHandle::Write(const uint8* Source, int64 BytesToWrite)
{
	// Memory shader files are read-only
	return false;
}

bool FMemoryFileHandle::Truncate(int64 NewSize)
{
	return false;
}

int64 FMemoryFileHandle::Size()
{
	return Data.Num();
}

bool FMemoryFileHandle::Flush(const bool bFullFlush)
{
	// No-op for memory files
	return true;
}

/*
bool FMemoryFileHandle::Close()
{
	//bIsOpen = false;
	return true;
}
*/
//-----------------------------------------------------------------------------
// FShaderOverridePlatformFile Implementation
//-----------------------------------------------------------------------------

FShaderOverridePlatformFile::FShaderOverridePlatformFile(IPlatformFile* InInnerFile)
	: InnerFile(InInnerFile)
	, bInitialized(false)
{
}

FShaderOverridePlatformFile::~FShaderOverridePlatformFile()
{
}

bool FShaderOverridePlatformFile::ShouldBeUsed(IPlatformFile* Inner, const TCHAR* CmdLine) const
{
	// We want to be used for all shader file operations
	return true;
}

bool FShaderOverridePlatformFile::Initialize(IPlatformFile* Inner, const TCHAR* CmdLine)
{
	InnerFile = Inner;
	bInitialized = true;

	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("ShaderOverridePlatformFile initialized"));

	return true;
}

bool FShaderOverridePlatformFile::ShouldInterceptFile(const TCHAR* Filename) const
{
	if (!Filename)
	{
		return false;
	}

	FString Path(Filename);

	// Check if this is a .usf or .ush file
	if (!Path.EndsWith(TEXT(".usf")) && !Path.EndsWith(TEXT(".ush")))
	{
		return false;
	}

	// Extract the filename and check if it's in our registry
	FString ShaderName = ExtractShaderFilename(Path);

	return FInMemoryShaderRegistry::Get().HasShader(ShaderName);
}

FString FShaderOverridePlatformFile::ExtractShaderFilename(const FString& Path) const
{
	// Extract just the filename from a full path
	FString Filename = FPaths::GetCleanFilename(Path);
	return Filename;
}

bool FShaderOverridePlatformFile::FileExists(const TCHAR* Filename)
{
	if (ShouldInterceptFile(Filename))
	{
		return true; // Claim it exists - we'll serve from memory
	}
	return InnerFile->FileExists(Filename);
}

int64 FShaderOverridePlatformFile::FileSize(const TCHAR* Filename)
{
	if (ShouldInterceptFile(Filename))
	{
		FString ShaderName = ExtractShaderFilename(Filename);
		TArray<uint8> Bytes;
		if (FInMemoryShaderRegistry::Get().GetShaderBytes(ShaderName, Bytes))
		{
			return Bytes.Num();
		}
	}
	return InnerFile->FileSize(Filename);
}

bool FShaderOverridePlatformFile::DeleteFile(const TCHAR* Filename)
{
	if (ShouldInterceptFile(Filename))
	{
		return false; // In-memory files cannot be deleted
	}
	return InnerFile->DeleteFile(Filename);
}

bool FShaderOverridePlatformFile::IsReadOnly(const TCHAR* Filename)
{
	if (ShouldInterceptFile(Filename))
	{
		return true; // In-memory files are read-only
	}
	return InnerFile->IsReadOnly(Filename);
}

bool FShaderOverridePlatformFile::MoveFile(const TCHAR* To, const TCHAR* From)
{
	// Don't allow moving in-memory files
	if (ShouldInterceptFile(From) || ShouldInterceptFile(To))
	{
		return false;
	}
	return InnerFile->MoveFile(To, From);
}

bool FShaderOverridePlatformFile::SetReadOnly(const TCHAR* Filename, bool bIsReadOnly)
{
	if (ShouldInterceptFile(Filename))
	{
		return false; // Cannot change read-only status
	}
	return InnerFile->SetReadOnly(Filename, bIsReadOnly);
}

FDateTime FShaderOverridePlatformFile::GetTimeStamp(const TCHAR* Filename)
{
	return FDateTime();
}

void FShaderOverridePlatformFile::SetTimeStamp(const TCHAR* Filename, FDateTime DateTime)
{
}

FDateTime FShaderOverridePlatformFile::GetAccessTimeStamp(const TCHAR* Filename)
{
	if (ShouldInterceptFile(Filename))
	{
		return FDateTime::UtcNow();
	}
	return InnerFile->GetAccessTimeStamp(Filename);
}

FString FShaderOverridePlatformFile::GetFilenameOnDisk(const TCHAR* Filename)
{
	if (ShouldInterceptFile(Filename))
	{
		return FString(Filename); // Return the virtual filename
	}
	return InnerFile->GetFilenameOnDisk(Filename);
}

IFileHandle* FShaderOverridePlatformFile::OpenRead(const TCHAR* Filename, bool bAllowWrite)
{
	if (ShouldInterceptFile(Filename))
	{
		FString ShaderName = ExtractShaderFilename(Filename);
		TArray<uint8> Bytes;

		if (FInMemoryShaderRegistry::Get().GetShaderBytes(ShaderName, Bytes))
		{
			UE_LOG(LogShaderOverrideInMemory, Verbose, TEXT("Serving in-memory shader: %s"), *ShaderName);
			return new FMemoryFileHandle(Bytes);
		}
	}

	return InnerFile->OpenRead(Filename, bAllowWrite);
}

IFileHandle* FShaderOverridePlatformFile::OpenWrite(const TCHAR* Filename, bool bAppend, bool bAllowRead)
{
	// Don't allow writing to in-memory files
	if (ShouldInterceptFile(Filename))
	{
		return nullptr;
	}
	return InnerFile->OpenWrite(Filename, bAppend, bAllowRead);
}

bool FShaderOverridePlatformFile::DirectoryExists(const TCHAR* Directory)
{
	return InnerFile->DirectoryExists(Directory);
}

bool FShaderOverridePlatformFile::CreateDirectory(const TCHAR* Directory)
{
	return InnerFile->CreateDirectory(Directory);
}

bool FShaderOverridePlatformFile::DeleteDirectory(const TCHAR* Directory)
{
	return InnerFile->DeleteDirectory(Directory);
}

FFileStatData FShaderOverridePlatformFile::GetStatData(const TCHAR* FilenameOrDirectory)
{
	if (ShouldInterceptFile(FilenameOrDirectory))
	{
		FString ShaderName = ExtractShaderFilename(FilenameOrDirectory);
		TArray<uint8> Bytes;

		if (FInMemoryShaderRegistry::Get().GetShaderBytes(ShaderName, Bytes))
		{
			return FFileStatData(
				FDateTime::UtcNow(),
				FDateTime::UtcNow(),
				Bytes.Num(),
				true,  // bIsFile
				true,  // bIsReadOnly
				false  // bIsValid
			);
		}
	}
	return InnerFile->GetStatData(FilenameOrDirectory);
}

void FShaderOverridePlatformFile::FindFiles(TArray<FString>& FoundFiles, const TCHAR* Directory, const TCHAR* FileExtension)
{
	InnerFile->FindFiles(FoundFiles, Directory, FileExtension);
}

void FShaderOverridePlatformFile::FindFilesRecursively(TArray<FString>& FoundFiles, const TCHAR* Directory, const TCHAR* FileExtension)
{
	InnerFile->FindFilesRecursively(FoundFiles, Directory, FileExtension);
}

bool FShaderOverridePlatformFile::IterateDirectory(const TCHAR* Directory, FDirectoryVisitor& Visitor)
{
	return InnerFile->IterateDirectory(Directory, Visitor);
}

bool FShaderOverridePlatformFile::IterateDirectoryRecursively(const TCHAR* Directory, FDirectoryVisitor& Visitor)
{
	return InnerFile->IterateDirectoryRecursively(Directory, Visitor);
}

bool FShaderOverridePlatformFile::IterateDirectoryStat(const TCHAR* Directory, FDirectoryStatVisitor& Visitor)
{
	return InnerFile->IterateDirectoryStat(Directory, Visitor);
}

bool FShaderOverridePlatformFile::IterateDirectoryStatRecursively(const TCHAR* Directory, FDirectoryStatVisitor& Visitor)
{
	return InnerFile->IterateDirectoryStatRecursively(Directory, Visitor);
}

//-----------------------------------------------------------------------------
// FShaderOverridePlugin_InMemoryModule Implementation
//-----------------------------------------------------------------------------

void FShaderOverridePlugin_InMemoryModule::StartupModule()
{
	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("ShaderOverridePlugin_InMemory starting up..."));

	// Get plugin directory
	//PluginDirectory = FPaths::GetPath(FModuleManager::Get().GetModuleFilename("ShaderOverridePlugin_InMemory"));
	//if (PluginDirectory.IsEmpty())
	{
		PluginDirectory = FPaths::ProjectPluginsDir() / TEXT("ShaderOverridePlugin_InMemory");
	}

	ShadersDirectory = FPaths::Combine(PluginDirectory, TEXT("Shaders"));

	// Initialize shader source directory mappings
	InitializeShaderSourceMappings();

	// Install the platform file wrapper to intercept shader reads
	InstallPlatformFileWrapper();

	// Load shader overrides from config
	LoadShaderOverridesFromConfig();

	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("ShaderOverridePlugin_InMemory startup complete"));
	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("  Registered %d in-memory shaders"),
		FInMemoryShaderRegistry::Get().GetShaderCount());
}

void FShaderOverridePlugin_InMemoryModule::ShutdownModule()
{
	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("ShaderOverridePlugin_InMemory shutting down..."));

	// Remove the platform file wrapper
	RemovePlatformFileWrapper();

	// Clear all registered shaders
	FInMemoryShaderRegistry::Get().ClearAll();

	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("ShaderOverridePlugin_InMemory shutdown complete"));
}

void FShaderOverridePlugin_InMemoryModule::InitializeShaderSourceMappings()
{
	// Add our plugin's shader directory to the engine's shader source mappings
	// This allows #include directives to find our custom shaders
	FString PluginVirtualPath = TEXT("/Plugin/ShaderOverridePlugin_InMemory");

	// Register our shader directory
	AddShaderSourceDirectoryMapping(PluginVirtualPath, ShadersDirectory);

	UE_LOG(LogShaderOverrideInMemory, Log, TEXT("Added shader source mapping: %s -> %s"),
		*PluginVirtualPath, *ShadersDirectory);
}

bool FShaderOverridePlugin_InMemoryModule::InstallPlatformFileWrapper()
{
	FPlatformFileManager& PlatformFileManager = FPlatformFileManager::Get();
	IPlatformFile* CurrentFile = &PlatformFileManager.GetPlatformFile();

	// Check if our wrapper is already installed
	if (CurrentFile->GetName() == FString(TEXT("ShaderOverridePlatformFile")))
	{
		UE_LOG(LogShaderOverrideInMemory, Warning, TEXT("Platform file wrapper already installed"));
		return true;
	}

	// Create and install our wrapper
	PlatformFileWrapper = new FShaderOverridePlatformFile(CurrentFile);

	if (PlatformFileWrapper->Initialize(CurrentFile, nullptr))
	{
		PlatformFileManager.SetPlatformFile(*PlatformFileWrapper);
		bWrapperInstalled = true;

		UE_LOG(LogShaderOverrideInMemory, Log, TEXT("Installed shader override platform file wrapper"));
		return true;
	}

	UE_LOG(LogShaderOverrideInMemory, Error, TEXT("Failed to install platform file wrapper"));
	delete PlatformFileWrapper;
	PlatformFileWrapper = nullptr;

	return false;
}

void FShaderOverridePlugin_InMemoryModule::RemovePlatformFileWrapper()
{
	if (!bWrapperInstalled || !PlatformFileWrapper)
	{
		return;
	}

	FPlatformFileManager& PlatformFileManager = FPlatformFileManager::Get();
	IPlatformFile* CurrentFile = &PlatformFileManager.GetPlatformFile();

	// Check if our wrapper is the current platform file
	if (CurrentFile == PlatformFileWrapper)
	{
		// Restore the lower level file
		IPlatformFile* LowerLevel = PlatformFileWrapper->GetLowerLevel();
		PlatformFileManager.SetPlatformFile(*LowerLevel);

		UE_LOG(LogShaderOverrideInMemory, Log, TEXT("Removed shader override platform file wrapper"));
	}

	// Note: We don't delete PlatformFileWrapper here as it may still be in use
	// It will be cleaned up when the module is unloaded

	bWrapperInstalled = false;
}

void FShaderOverridePlugin_InMemoryModule::LoadShaderOverridesFromConfig()
{
	// Get config file path
	FString ConfigPath = FPaths::Combine(PluginDirectory, TEXT("Config/ShaderOverride.ini"));

	TArray<FString> ShadersToLoad;

	if (GConfig)
	{
		GConfig->GetArray(TEXT("ShaderOverride"), TEXT("Shaders"), ShadersToLoad, ConfigPath);
	}

	// If no config, use defaults
	if (ShadersToLoad.Num() == 0)
	{
		UE_LOG(LogShaderOverrideInMemory, Log, TEXT("No config found, using default shader list"));
		ShadersToLoad.Add(TEXT("PostProcessToneMap.usf"));
		ShadersToLoad.Add(TEXT("PostProcessCombineLUTs.usf"));
	}

	// Load each shader
	for (const FString& ShaderName : ShadersToLoad)
	{
		FString ShaderPath = FPaths::Combine(ShadersDirectory, ShaderName);

		if (FPaths::FileExists(ShaderPath))
		{
			RegisterShaderOverrideFromFile(ShaderName, ShaderPath);
		}
		else
		{
			UE_LOG(LogShaderOverrideInMemory, Warning, TEXT("Shader file not found: %s"), *ShaderPath);
		}
	}
}

bool FShaderOverridePlugin_InMemoryModule::LoadShaderFile(const FString& FilePath, FString& OutSource)
{
	return FFileHelper::LoadFileToString(OutSource, *FilePath);
}

bool FShaderOverridePlugin_InMemoryModule::RegisterShaderOverride(const FString& ShaderName, const FString& ShaderSource)
{
	return FInMemoryShaderRegistry::Get().RegisterShader(ShaderName, ShaderSource);
}

bool FShaderOverridePlugin_InMemoryModule::RegisterShaderOverrideFromFile(const FString& ShaderName, const FString& ShaderFilePath)
{
	return FInMemoryShaderRegistry::Get().RegisterShaderFromFile(ShaderName, ShaderFilePath);
}

bool FShaderOverridePlugin_InMemoryModule::UnregisterShaderOverride(const FString& ShaderName)
{
	return FInMemoryShaderRegistry::Get().UnregisterShader(ShaderName);
}

FInMemoryShaderRegistry& FShaderOverridePlugin_InMemoryModule::GetRegistry()
{
	return FInMemoryShaderRegistry::Get();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FShaderOverridePlugin_InMemoryModule, ShaderOverridePlugin_InMemory)