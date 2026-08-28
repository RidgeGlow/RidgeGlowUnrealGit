// Copyright (c) 2026 RidgeGlow
//
// Licensed under the Functional Source License 1.1 (MIT future). See LICENSE.

#pragma once

#include "CoreMinimal.h"

/**
 * Thin wrapper over libgitlfs, the Git LFS locking surface exposed as a C shared
 * library by https://github.com/RidgeGlow/git-lfs-lib.
 *
 * This replaces spawning a bundled git-lfs binary once per lock operation. The
 * library is loaded at runtime with FPlatformProcess::GetDllHandle and is never
 * linked, so its absence is a runtime condition rather than a build failure --
 * see Source/ThirdParty/GitLfsLib/GitLfsLib.Build.cs.
 *
 * When IsAvailable() is false, callers fall back to invoking git-lfs as a
 * process. That fallback is degraded mode, not a supported steady state: it is
 * reported loudly and never entered silently.
 */
namespace GitLfsLib
{
	/** Mirrors the cached/localOnly pair of git lfs locks. */
	enum class ELockQuery : uint8
	{
		/** Query the remote server and refresh the cache. */
		Remote,
		/** Read the last known remote state from the cache. */
		Cached,
		/** Report only locks held locally, without touching the network. */
		Local
	};

	/** One path that failed inside a bulk operation. */
	struct FPathError
	{
		FString Path;
		FString Error;
	};

	/**
	 * Loads the library and resolves its entry points. Safe to call more than
	 * once; only the first call does work.
	 *
	 * Call this from module startup rather than letting the first operation
	 * trigger it: cgo attaches the calling thread to the Go runtime on first
	 * use, which is better done once from a known thread than from whichever
	 * pool worker happens to arrive first.
	 */
	void Initialize();

	/** True once the library and all of its entry points have been resolved. */
	bool IsAvailable();

	/** Locations searched for the library, in order. */
	TArray<FString> GetLibrarySearchPaths();

	/**
	 * The library's path if it is present, otherwise the location it should be
	 * placed at. Useful in diagnostics and UI.
	 */
	FString GetExpectedLibraryPath();

	/**
	 * Locks the given files. Paths may be absolute or relative to the repository
	 * root; the library treats a relative path as repository-relative.
	 *
	 * Bulk calls are partial-success: a true return means the batch ran, not
	 * that every path succeeded. Inspect OutFailures either way.
	 *
	 * @return false only if the batch could not start at all, in which case
	 *         OutFatalError explains why.
	 */
	bool LockFiles(const FString& RepositoryRoot, const TArray<FString>& Files,
				   TArray<FString>& OutLocked, TArray<FPathError>& OutFailures, FString& OutFatalError);

	/** Unlocks the given files. See LockFiles for the return contract. */
	bool UnlockFiles(const FString& RepositoryRoot, const TArray<FString>& Files, bool bForce,
					 TArray<FString>& OutUnlocked, TArray<FPathError>& OutFailures, FString& OutFatalError);

	/**
	 * Retrieves locks for the repository, keyed by absolute local filename with
	 * the owning user as the value -- the same shape the state cache wants.
	 *
	 * @param bSkipIfBusy  If true, give up immediately when another operation
	 *                     holds the library rather than waiting. Used by the
	 *                     periodic background refresh so it can never stall a
	 *                     user-initiated checkout.
	 */
	bool GetLocks(const FString& RepositoryRoot, ELockQuery Query, TMap<FString, FString>& OutLocks,
				  FString& OutError, bool bSkipIfBusy = false);

	/**
	 * Records that an operation went through the process fallback, and appends a
	 * warning to InOutMessages so it reaches the revision control message log.
	 *
	 * Rate limited: the background refresh runs every 30 seconds and would
	 * otherwise bury the log in identical warnings.
	 */
	void NoteFallbackUsed(const TCHAR* Operation, TArray<FString>& InOutMessages);
}
