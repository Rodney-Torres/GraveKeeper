// Copyright Hyperdyne Systems LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Containers/Map.h"
#include "Containers/Array.h"
#include "RHI.h"

DECLARE_LOG_CATEGORY_EXTERN(LogShaderOverrideInMemory, Log, All);

/**
 * Represents an in-memory shader file that can be served to the shader compiler
 */
struct FInMemoryShader
{
	/** The shader filename (e.g., "PostProcessToneMap.usf") */
	FString Filename;

	/** The virtual path this shader overrides (e.g., "/Engine/Private/PostProcessToneMap.usf") */
	FString VirtualPath;

	/** The shader source code */
	FString SourceCode;

	/** UTF8 encoded source for file interface */
	TArray<uint8> SourceBytes;

	/** Timestamp for cache invalidation */
	FDateTime Timestamp;

	/** Whether this shader is active */
	bool bActive;

	FInMemoryShader()
		: Timestamp(FDateTime::UtcNow())
		, bActive(true)
	{
	}

	FInMemoryShader(const FString& InFilename, const FString& InSourceCode)
		: Filename(InFilename)
		, SourceCode(InSourceCode)
		, Timestamp(FDateTime::UtcNow())
		, bActive(true)
	{
		// Convert to UTF8 bytes for file interface
		FTCHARToUTF8 UTF8Converter(*SourceCode);
		SourceBytes.Append((uint8*)UTF8Converter.Get(), UTF8Converter.Length());
	}
};

/**
 * Singleton registry that holds all in-memory shader overrides.
 * Thread-safe access to shader data.
 */
class FInMemoryShaderRegistry
{
public:
	/** Get the singleton instance */
	static FInMemoryShaderRegistry& Get();

	/** Register a shader override from source code */
	bool RegisterShader(const FString& Filename, const FString& SourceCode);

	/** Register a shader override from file (loads into memory) */
	bool RegisterShaderFromFile(const FString& Filename, const FString& FilePath);

	/** Unregister a shader override */
	bool UnregisterShader(const FString& Filename);

	/** Check if a shader is registered */
	bool HasShader(const FString& Filename) const;

	/** Get shader source code */
	bool GetShaderSource(const FString& Filename, FString& OutSource) const;

	/** Get shader as byte array */
	bool GetShaderBytes(const FString& Filename, TArray<uint8>& OutBytes) const;

	/** Get all registered shader filenames */
	void GetRegisteredShaders(TArray<FString>& OutFilenames) const;

	/** Clear all registered shaders */
	void ClearAll();

	/** Get the number of registered shaders */
	int32 GetShaderCount() const;

private:
	/** Private constructor for singleton */
	FInMemoryShaderRegistry() = default;

	/** Map of filename to shader data */
	TMap<FString, TSharedPtr<FInMemoryShader>> Shaders;

	/** Critical section for thread safety */
	mutable FCriticalSection RegistryLock;
};

/**
 * Custom platform file wrapper that intercepts shader file reads.
 * When a .usf file is requested that's in our registry, we return
 * the in-memory version instead of reading from disk.
 */
class FShaderOverridePlatformFile : public IPlatformFile
{
public:
	FShaderOverridePlatformFile(IPlatformFile* InInnerFile);
	virtual ~FShaderOverridePlatformFile();

	// IPlatformFile interface
	virtual bool ShouldBeUsed(IPlatformFile* Inner, const TCHAR* CmdLine) const override;
	virtual bool Initialize(IPlatformFile* Inner, const TCHAR* CmdLine) override;
	virtual IPlatformFile* GetLowerLevel() override { return InnerFile; }
	virtual void SetLowerLevel(IPlatformFile* NewLowerLevel) override { InnerFile = NewLowerLevel; }
	virtual const TCHAR* GetName() const override { return TEXT("ShaderOverridePlatformFile"); }

	virtual bool FileExists(const TCHAR* Filename) override;
	virtual int64 FileSize(const TCHAR* Filename) override;
	virtual bool DeleteFile(const TCHAR* Filename) override;
	virtual bool IsReadOnly(const TCHAR* Filename) override;
	virtual bool MoveFile(const TCHAR* To, const TCHAR* From) override;
	virtual bool SetReadOnly(const TCHAR* Filename, bool bIsReadOnly) override;
	/** Return the modification time of a file. Returns FDateTime::MinValue() on failure **/
	virtual FDateTime GetTimeStamp(const TCHAR* Filename) override;
	/** Sets the modification time of a file **/
	virtual void SetTimeStamp(const TCHAR* Filename, FDateTime DateTime) override;
	virtual FDateTime GetAccessTimeStamp(const TCHAR* Filename) override;
	virtual FString GetFilenameOnDisk(const TCHAR* Filename) override;

	virtual IFileHandle* OpenRead(const TCHAR* Filename, bool bAllowWrite) override;
	virtual IFileHandle* OpenWrite(const TCHAR* Filename, bool bAppend, bool bAllowRead) override;

	virtual bool DirectoryExists(const TCHAR* Directory) override;
	virtual bool CreateDirectory(const TCHAR* Directory) override;
	virtual bool DeleteDirectory(const TCHAR* Directory) override;

	virtual FFileStatData GetStatData(const TCHAR* FilenameOrDirectory) override;

	virtual void FindFiles(TArray<FString>& FoundFiles, const TCHAR* Directory, const TCHAR* FileExtension) override;
	virtual void FindFilesRecursively(TArray<FString>& FoundFiles, const TCHAR* Directory, const TCHAR* FileExtension) override;

	virtual bool IterateDirectory(const TCHAR* Directory, FDirectoryVisitor& Visitor) override;
	virtual bool IterateDirectoryRecursively(const TCHAR* Directory, FDirectoryVisitor& Visitor) override;
	virtual bool IterateDirectoryStat(const TCHAR* Directory, FDirectoryStatVisitor& Visitor) override;
	virtual bool IterateDirectoryStatRecursively(const TCHAR* Directory, FDirectoryStatVisitor& Visitor) override;

private:
	/** Check if this file should be intercepted and served from memory */
	bool ShouldInterceptFile(const TCHAR* Filename) const;

	/** Extract the shader filename from a full path */
	FString ExtractShaderFilename(const FString& Path) const;

	/** The inner (lower level) platform file */
	IPlatformFile* InnerFile;

	/** Whether this wrapper has been initialized */
	bool bInitialized;
};

/**
 * Memory-based file handle for serving in-memory shader content
 */
class FMemoryFileHandle : public IFileHandle
{
public:
	FMemoryFileHandle(const TArray<uint8>& InData);
	virtual ~FMemoryFileHandle();

	virtual int64 Tell() override;
	virtual bool Seek(int64 NewPosition) override;
	virtual bool SeekFromEnd(int64 NewPositionRelativeToEnd = 0) override;
	virtual bool Read(uint8* Destination, int64 BytesToRead) override;
	virtual bool ReadAt(uint8* Destination, int64 BytesToRead, int64 Offset) override;
	virtual bool Write(const uint8* Source, int64 BytesToWrite) override;
	virtual bool Truncate(int64 NewSize) override;
	virtual int64 Size() override;
	virtual bool Flush(const bool bFullFlush = false) override;
	//virtual bool Close() override;
	//virtual bool IsOpen() const override { return bIsOpen; }

private:
	/** Reference to the source data */
	TArray<uint8> Data;

	/** Current read position */
	int64 Position;

	/** Whether the handle is open */
	bool bIsOpen;
};

/**
 * Module interface for the In-Memory Shader Override Plugin
 */
class FShaderOverridePlugin_InMemoryModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Register a custom shader to override an engine shader
	 * @param ShaderName - Name of the shader file (e.g., "PostProcessToneMap.usf")
	 * @param ShaderSource - The HLSL source code
	 * @return True if registration was successful
	 */
	static bool RegisterShaderOverride(const FString& ShaderName, const FString& ShaderSource);

	/**
	 * Register a shader from a file in the plugin
	 * @param ShaderName - Name of the shader file
	 * @param ShaderFilePath - Path to the shader file
	 * @return True if registration was successful
	 */
	static bool RegisterShaderOverrideFromFile(const FString& ShaderName, const FString& ShaderFilePath);

	/**
	 * Unregister a shader override
	 * @param ShaderName - Name of the shader to unregister
	 * @return True if unregistration was successful
	 */
	static bool UnregisterShaderOverride(const FString& ShaderName);

	/**
	 * Get the shader registry
	 */
	static FInMemoryShaderRegistry& GetRegistry();

private:
	/** Initialize the shader source directory mappings */
	void InitializeShaderSourceMappings();

	/** Install the platform file wrapper */
	bool InstallPlatformFileWrapper();

	/** Remove the platform file wrapper */
	void RemovePlatformFileWrapper();

	/** Load shader overrides from configuration */
	void LoadShaderOverridesFromConfig();

	/** Load a shader file from disk into memory */
	bool LoadShaderFile(const FString& FilePath, FString& OutSource);

private:
	/** The custom platform file wrapper */
	FShaderOverridePlatformFile* PlatformFileWrapper;

	/** Path to the plugin directory */
	FString PluginDirectory;

	/** Path to the plugin's shaders directory */
	FString ShadersDirectory;

	/** Whether the platform file wrapper is installed */
	bool bWrapperInstalled;
};
