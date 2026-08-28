// Copyright (c) 2026 RidgeGlow
//
// Licensed under the Functional Source License 1.1 (MIT future). See LICENSE.

#include "GitLfsLib.h"

#include "GitSourceControlModule.h"
#include "ISourceControlModule.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/MessageLog.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

THIRD_PARTY_INCLUDES_START
#include "libgitlfs.h"
THIRD_PARTY_INCLUDES_END

namespace GitLfsLib
{
namespace
{
	// Signatures mirror libgitlfs.h. We resolve these by name instead of linking,
	// so that a missing library degrades at runtime rather than failing the build.
	typedef int (*FLockFn)(char*, char*, char**);
	typedef int (*FUnlockFn)(char*, char*, int, char**);
	typedef GitLFSPathResultList (*FLockManyFn)(char*, char**, int, char**);
	typedef GitLFSPathResultList (*FUnlockManyFn)(char*, char**, int, int, char**);
	typedef GitLFSLockList (*FLocksFn)(char*, int, int, char**);
	typedef void (*FFreeLocksFn)(GitLFSLockList);
	typedef void (*FFreePathResultsFn)(GitLFSPathResultList);
	typedef void (*FFreeErrorFn)(char*);

	struct FApi
	{
		void* Handle = nullptr;
		FLockFn Lock = nullptr;
		FUnlockFn Unlock = nullptr;
		FLockManyFn LockMany = nullptr;
		FUnlockManyFn UnlockMany = nullptr;
		FLocksFn Locks = nullptr;
		FFreeLocksFn FreeLocks = nullptr;
		FFreePathResultsFn FreePathResults = nullptr;
		FFreeErrorFn FreeError = nullptr;

		bool IsComplete() const
		{
			return Handle && Lock && Unlock && LockMany && UnlockMany && Locks
				&& FreeLocks && FreePathResults && FreeError;
		}
	};

	FApi GApi;
	bool GInitialized = false;
	FCriticalSection GInitLock;

	/**
	 * Serialises every call into the library.
	 *
	 * Source control workers run on GThreadPool and the background refresh runs
	 * on its own thread, so calls genuinely overlap. Cross-call thread safety of
	 * the library is undocumented and untested, and separate processes used to
	 * provide that isolation for free. Parallelism is not lost: it happens inside
	 * LockMany/UnlockMany, which fan out bounded by lfs.concurrenttransfers.
	 */
	FCriticalSection GCallLock;

	/** Rate limiting for the degraded-mode warning; see NoteFallbackUsed. */
	FCriticalSection GFallbackLock;
	double GLastFallbackWarningSeconds = 0.0;
	bool GFallbackWarnedOnce = false;
	constexpr double FallbackWarningIntervalSeconds = 300.0;

	const TCHAR* LibraryFileName()
	{
#if PLATFORM_WINDOWS
		return TEXT("libgitlfs-windows-amd64.dll");
#elif PLATFORM_MAC
		return TEXT("libgitlfs-darwin-universal.dylib");
#elif PLATFORM_LINUX
		return TEXT("libgitlfs-linux-amd64.so");
#else
		return nullptr;
#endif
	}

	/**
	 * Converts strings to UTF-8 buffers plus a matching array of pointers.
	 *
	 * Buffers is reserved up front so it never reallocates, and TArray move
	 * preserves the inner allocations regardless, so the pointers stay valid for
	 * as long as Buffers is alive.
	 */
	void MakeUtf8Array(const TArray<FString>& In, TArray<TArray<ANSICHAR>>& OutBuffers, TArray<char*>& OutPointers)
	{
		OutBuffers.Reserve(In.Num());
		OutPointers.Reserve(In.Num());
		for (const FString& Value : In)
		{
			FTCHARToUTF8 Converted(*Value);
			TArray<ANSICHAR>& Buffer = OutBuffers.AddDefaulted_GetRef();
			Buffer.Append(Converted.Get(), Converted.Length() + 1);
			OutPointers.Add(Buffer.GetData());
		}
	}

	/** Takes ownership of a char* the library allocated, returning it as an FString. */
	FString ConsumeError(char*& Error)
	{
		if (!Error)
		{
			return FString();
		}
		FString Result = UTF8_TO_TCHAR(Error);
		GApi.FreeError(Error);
		Error = nullptr;
		return Result;
	}

	/** Frees a returned list even if the caller returns early. */
	struct FPathResultsScope
	{
		GitLFSPathResultList List;
		explicit FPathResultsScope(const GitLFSPathResultList& In) : List(In) {}
		~FPathResultsScope() { GApi.FreePathResults(List); }
	};

	struct FLockListScope
	{
		GitLFSLockList List;
		explicit FLockListScope(const GitLFSLockList& In) : List(In) {}
		~FLockListScope() { GApi.FreeLocks(List); }
	};

	template <typename FnType>
	bool Resolve(void* Handle, const TCHAR* Name, FnType& OutFn)
	{
		OutFn = reinterpret_cast<FnType>(FPlatformProcess::GetDllExport(Handle, Name));
		if (!OutFn)
		{
			UE_LOG(LogSourceControl, Warning,
				   TEXT("Git LFS library is missing the export '%s'; falling back to running git-lfs as a process."),
				   Name);
			return false;
		}
		return true;
	}

	/**
	 * Collects a bulk result into caller arrays. Bulk calls are partial-success,
	 * so every entry must be inspected -- a path with Success == 0 carries its own
	 * error, which the previous implementation discarded entirely.
	 */
	void CollectPathResults(const GitLFSPathResultList& List, TArray<FString>& OutOk, TArray<FPathError>& OutFailures)
	{
		for (int32 Index = 0; Index < List.Count; ++Index)
		{
			const GitLFSPathResult& Result = List.Results[Index];
			const FString Path = Result.Path ? UTF8_TO_TCHAR(Result.Path) : FString();
			if (Result.Success != 0)
			{
				OutOk.Add(Path);
			}
			else
			{
				FPathError Failure;
				Failure.Path = Path;
				Failure.Error = Result.Error ? UTF8_TO_TCHAR(Result.Error) : TEXT("unknown error");
				OutFailures.Add(MoveTemp(Failure));
			}
		}
	}
}

FString GetExpectedLibraryPath()
{
	const TCHAR* FileName = LibraryFileName();
	if (!FileName)
	{
		return FString();
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries"),
						   FPlatformProcess::GetBinariesSubdirectory(), FileName);
}

void Initialize()
{
	FScopeLock Lock(&GInitLock);
	if (GInitialized)
	{
		return;
	}
	GInitialized = true;

	const FString LibraryPath = GetExpectedLibraryPath();
	if (LibraryPath.IsEmpty())
	{
		UE_LOG(LogSourceControl, Warning,
			   TEXT("Git LFS library is not available on this platform; falling back to running git-lfs as a process."));
		return;
	}

	if (!FPaths::FileExists(LibraryPath))
	{
		UE_LOG(LogSourceControl, Warning,
			   TEXT("Git LFS library was not found at '%s'. Locking will fall back to running git-lfs as a process, ")
			   TEXT("which is slower. Rebuild the plugin with network access, or download the library from ")
			   TEXT("https://github.com/RidgeGlow/git-lfs-lib/releases and place it at that path."),
			   *LibraryPath);
		return;
	}

	GApi.Handle = FPlatformProcess::GetDllHandle(*LibraryPath);
	if (!GApi.Handle)
	{
		UE_LOG(LogSourceControl, Warning,
			   TEXT("Git LFS library at '%s' could not be loaded; falling back to running git-lfs as a process."),
			   *LibraryPath);
		return;
	}

	const bool bResolved =
		Resolve(GApi.Handle, TEXT("GitLFS_Lock"), GApi.Lock) &
		Resolve(GApi.Handle, TEXT("GitLFS_Unlock"), GApi.Unlock) &
		Resolve(GApi.Handle, TEXT("GitLFS_LockMany"), GApi.LockMany) &
		Resolve(GApi.Handle, TEXT("GitLFS_UnlockMany"), GApi.UnlockMany) &
		Resolve(GApi.Handle, TEXT("GitLFS_Locks"), GApi.Locks) &
		Resolve(GApi.Handle, TEXT("GitLFS_FreeLocks"), GApi.FreeLocks) &
		Resolve(GApi.Handle, TEXT("GitLFS_FreePathResults"), GApi.FreePathResults) &
		Resolve(GApi.Handle, TEXT("GitLFS_FreeError"), GApi.FreeError);

	if (!bResolved || !GApi.IsComplete())
	{
		// Deliberately keep the handle: the Go runtime cannot be unloaded, so
		// releasing it would be worse than leaking it. Just refuse to use it.
		GApi = FApi();
		return;
	}

	UE_LOG(LogSourceControl, Log, TEXT("Git LFS library loaded from '%s'."), *LibraryPath);
}

bool IsAvailable()
{
	if (!GInitialized)
	{
		// Defensive: startup should have done this already, but an ordering bug
		// must not silently push every user onto the fallback path.
		Initialize();
	}
	return GApi.IsComplete();
}

bool LockFiles(const FString& RepositoryRoot, const TArray<FString>& Files,
			   TArray<FString>& OutLocked, TArray<FPathError>& OutFailures, FString& OutFatalError)
{
	if (!IsAvailable() || Files.Num() == 0)
	{
		return false;
	}

	FTCHARToUTF8 RootUtf8(*RepositoryRoot);
	TArray<TArray<ANSICHAR>> Buffers;
	TArray<char*> Pointers;
	MakeUtf8Array(Files, Buffers, Pointers);

	char* Error = nullptr;
	GitLFSPathResultList Raw;
	{
		FScopeLock Lock(&GCallLock);
		Raw = GApi.LockMany(const_cast<char*>(RootUtf8.Get()), Pointers.GetData(), Pointers.Num(), &Error);
	}
	FPathResultsScope Scope(Raw);

	// A whole-batch failure sets Error and yields Count == 0; per-path failures
	// leave Error null and are reported inside the list.
	OutFatalError = ConsumeError(Error);
	if (Raw.Count == 0 && !OutFatalError.IsEmpty())
	{
		return false;
	}

	CollectPathResults(Raw, OutLocked, OutFailures);
	return true;
}

bool UnlockFiles(const FString& RepositoryRoot, const TArray<FString>& Files, bool bForce,
				 TArray<FString>& OutUnlocked, TArray<FPathError>& OutFailures, FString& OutFatalError)
{
	if (!IsAvailable() || Files.Num() == 0)
	{
		return false;
	}

	FTCHARToUTF8 RootUtf8(*RepositoryRoot);
	TArray<TArray<ANSICHAR>> Buffers;
	TArray<char*> Pointers;
	MakeUtf8Array(Files, Buffers, Pointers);

	char* Error = nullptr;
	GitLFSPathResultList Raw;
	{
		FScopeLock Lock(&GCallLock);
		Raw = GApi.UnlockMany(const_cast<char*>(RootUtf8.Get()), Pointers.GetData(), Pointers.Num(),
							  bForce ? 1 : 0, &Error);
	}
	FPathResultsScope Scope(Raw);

	OutFatalError = ConsumeError(Error);
	if (Raw.Count == 0 && !OutFatalError.IsEmpty())
	{
		return false;
	}

	CollectPathResults(Raw, OutUnlocked, OutFailures);
	return true;
}

bool GetLocks(const FString& RepositoryRoot, ELockQuery Query, TMap<FString, FString>& OutLocks,
			  FString& OutError, bool bSkipIfBusy)
{
	if (!IsAvailable())
	{
		return false;
	}

	const int32 bCached = (Query == ELockQuery::Cached) ? 1 : 0;
	const int32 bLocalOnly = (Query == ELockQuery::Local) ? 1 : 0;

	FTCHARToUTF8 RootUtf8(*RepositoryRoot);

	char* Error = nullptr;
	GitLFSLockList Raw;
	{
		// The periodic background refresh passes bSkipIfBusy so it can never
		// queue behind, and thereby delay, a user-initiated operation.
		if (bSkipIfBusy)
		{
			if (!GCallLock.TryLock())
			{
				OutError = TEXT("skipped: another Git LFS operation is in progress");
				return false;
			}
		}
		else
		{
			GCallLock.Lock();
		}

		Raw = GApi.Locks(const_cast<char*>(RootUtf8.Get()), bCached, bLocalOnly, &Error);
		GCallLock.Unlock();
	}
	FLockListScope Scope(Raw);

	// An empty list means both "no locks" and "failed"; only the error tells them apart.
	OutError = ConsumeError(Error);
	if (!OutError.IsEmpty())
	{
		return false;
	}

	for (int32 Index = 0; Index < Raw.Count; ++Index)
	{
		const GitLFSLock& Lock = Raw.Locks[Index];
		if (!Lock.Path)
		{
			continue;
		}
		const FString RelativePath = UTF8_TO_TCHAR(Lock.Path);
		// OwnerName is null when the server reports no owner, which the previous
		// text parser could not distinguish from a malformed line.
		const FString Owner = Lock.OwnerName ? UTF8_TO_TCHAR(Lock.OwnerName) : FString();
		OutLocks.Add(FPaths::ConvertRelativePathToFull(RepositoryRoot, RelativePath), Owner);
	}
	return true;
}

void NoteFallbackUsed(const TCHAR* Operation, TArray<FString>& InOutMessages)
{
	const FString Message = FString::Printf(
		TEXT("Git LFS '%s' ran through the slower process fallback because the Git LFS library is not loaded. ")
		TEXT("Expected it at: %s"),
		Operation, *GetExpectedLibraryPath());

	bool bShouldSurface = false;
	{
		FScopeLock Lock(&GFallbackLock);
		const double Now = FPlatformTime::Seconds();
		if (!GFallbackWarnedOnce || (Now - GLastFallbackWarningSeconds) > FallbackWarningIntervalSeconds)
		{
			GFallbackWarnedOnce = true;
			GLastFallbackWarningSeconds = Now;
			bShouldSurface = true;
		}
	}

	// Always attach to the operation's own messages so the cause is visible where
	// the effect is; only escalate to the shared log on the rate limit, because
	// the background refresh runs every 30 seconds.
	InOutMessages.Add(Message);
	if (bShouldSurface)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("%s"), *Message);
		FMessageLog("SourceControl").Warning(FText::FromString(Message));
	}
}
}
