#ifndef ATLAS_WINDOWS_COMPAT_H
#define ATLAS_WINDOWS_COMPAT_H
/* Internal Win32 adaptation; the public C API and storage codec are unchanged. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <unistd.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

typedef SRWLOCK pthread_mutex_t;
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr; InitializeSRWLock(m); return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }

static inline wchar_t *adb_win_path(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!count) { errno = EINVAL; return NULL; }
    wchar_t *wide = malloc((size_t)count * sizeof(wchar_t));
    if (!wide) { errno = ENOMEM; return NULL; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide); errno = EINVAL; return NULL;
    }
    return wide;
}
static inline int adb_win_open(const char *path, int flags, ...) {
    wchar_t *wide = adb_win_path(path); if (!wide) return -1;
    /* Always binary: CRLF translation would corrupt the WAL and snapshot. */
    int fd = _wopen(wide, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    free(wide); return fd;
}
static inline int adb_win_mkdir(const char *path, int mode) {
    (void)mode; wchar_t *wide = adb_win_path(path); if (!wide) return -1;
    int rc = _wmkdir(wide); free(wide); return rc;
}
static inline int adb_win_stat(const char *path, struct _stat64 *info) {
    wchar_t *wide = adb_win_path(path); if (!wide) return -1;
    int rc = _wstat64(wide, info); free(wide); return rc;
}
static inline int adb_win_unlink(const char *path) {
    wchar_t *wide = adb_win_path(path); if (!wide) return -1;
    int rc = _wunlink(wide); free(wide); return rc;
}
static inline int adb_win_rename(const char *from, const char *to) {
    wchar_t *a = adb_win_path(from), *b = adb_win_path(to);
    if (!a || !b) { free(a); free(b); return -1; }
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a); free(b); if (!ok) { errno = EIO; return -1; } return 0;
}
static inline int adb_win_ftruncate(int fd, long long size) {
    int rc = _chsize_s(fd, size); if (rc) { errno = rc; return -1; } return 0;
}
#ifndef LOCK_EX
#define LOCK_EX 2
#define LOCK_NB 4
#endif
static inline int adb_win_flock(int fd, int operation) {
    (void)operation;
    OVERLAPPED overlapped = {0};
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &overlapped)) { errno = EACCES; return -1; }
    return 0; /* Closing the descriptor releases the OS lock. */
}
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#define open adb_win_open
#define mkdir adb_win_mkdir
#define stat _stat64
/* Function-like macro leaves "struct stat" mapped to struct _stat64. */
#define _stat64(...) adb_win_stat(__VA_ARGS__)
#define fstat _fstat64
#undef lseek
#define lseek _lseeki64
#define fsync _commit
#undef ftruncate
#define ftruncate adb_win_ftruncate
#define flock adb_win_flock
#define rename adb_win_rename
#define unlink adb_win_unlink
#endif
