#ifndef LIBGITLFS_H
#define LIBGITLFS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* Id;
    char* Path;
    char* LockedAt;
    char* OwnerName;
} GitLFSLock;

typedef struct {
    GitLFSLock* Locks;
    int Count;
} GitLFSLockList;

/**
 * The outcome of one path within a bulk operation.
 * Bulk calls are partial-success: inspect every element.
 */
typedef struct {
    char* Path;    /* the requested path, echoed back */
    int Success;   /* 1 on success, 0 on failure */
    char* Error;   /* NULL unless Success == 0 */
} GitLFSPathResult;

typedef struct {
    GitLFSPathResult* Results;
    int Count;
} GitLFSPathResultList;

/**
 * Lock a single file.
 * Returns 0 on success. On error, returns non-zero and allocates errorMsg if provided.
 */
extern int GitLFS_Lock(char* repoPath, char* filePath, char** errorMsg);

/**
 * Unlock a single file.
 * Returns 0 on success. On error, returns non-zero and allocates errorMsg if provided.
 */
extern int GitLFS_Unlock(char* repoPath, char* filePath, int force, char** errorMsg);

/**
 * Lock many files in one call, concurrently, over a single shared client.
 *
 * This is the bulk entry point: locking a directory's worth of files should go
 * through here rather than looping over GitLFS_Lock, which would repeat the
 * full client/config/cache setup per file and serialise the requests.
 *
 * errorMsg is set only for a whole-batch failure (e.g. the repository could not
 * be opened), in which case Count is 0. Otherwise every path gets an entry in
 * the returned list and individual failures are reported per path.
 *
 * The returned list MUST be freed with GitLFS_FreePathResults.
 */
extern GitLFSPathResultList GitLFS_LockMany(char* repoPath, char** filePaths, int count, char** errorMsg);

/**
 * Unlock many files in one call, concurrently. See GitLFS_LockMany.
 * The returned list MUST be freed with GitLFS_FreePathResults.
 */
extern GitLFSPathResultList GitLFS_UnlockMany(char* repoPath, char** filePaths, int count, int force, char** errorMsg);

/**
 * Retrieve locks for the given repository.
 *
 *   cached=0, localOnly=0  query the remote server (and refresh the cache)
 *   cached=1, localOnly=0  read the last known remote state from the cache
 *   cached=0, localOnly=1  report only locks held locally, without the network
 *
 * Returns a GitLFSLockList. If Count is 0 and errorMsg is non-NULL, an error occurred.
 * The returned list MUST be freed with GitLFS_FreeLocks.
 */
extern GitLFSLockList GitLFS_Locks(char* repoPath, int cached, int localOnly, char** errorMsg);

/**
 * Frees the memory allocated by GitLFS_Locks.
 */
extern void GitLFS_FreeLocks(GitLFSLockList list);

/**
 * Frees the memory allocated by GitLFS_LockMany / GitLFS_UnlockMany.
 */
extern void GitLFS_FreePathResults(GitLFSPathResultList list);

/**
 * Frees the memory allocated for errorMsg by other GitLFS API functions.
 */
extern void GitLFS_FreeError(char* errMsg);

#ifdef __cplusplus
}
#endif

#endif // LIBGITLFS_H
