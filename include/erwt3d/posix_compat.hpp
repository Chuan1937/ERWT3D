#pragma once
#ifdef _WIN32

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <basetsd.h>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <direct.h>

typedef SSIZE_T ssize_t;
typedef int mode_t;

constexpr int PROT_READ  = 0x1;
constexpr int PROT_WRITE = 0x2;
constexpr int MAP_PRIVATE = 0x02;
constexpr int MAP_SHARED  = 0x01;
#define MAP_FAILED ((void*)-1)
constexpr int MADV_SEQUENTIAL = 2;
constexpr int MS_ASYNC = 1;
constexpr int POSIX_FADV_SEQUENTIAL = 2;
constexpr int POSIX_FADV_WILLNEED   = 3;
constexpr int POSIX_FADV_DONTNEED   = 4;

inline int io_open(const char* path, int flags) {
    return _open(path, flags | _O_BINARY);
}
inline int io_open(const char* path, int flags, int mode) {
    return _open(path, flags | _O_BINARY, mode);
}
inline int io_close(int fd) { return _close(fd); }
inline ssize_t io_read(int fd, void* buf, size_t count) {
    return static_cast<ssize_t>(_read(fd, buf, static_cast<unsigned>(count)));
}

inline ssize_t pread(int fd, void* buf, size_t count, int64_t offset) {
    if (count == 0) return 0;
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    uint8_t* dst = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > 0x40000000ULL) chunk = 0x40000000ULL;
        OVERLAPPED ov{};
        uint64_t curOff = static_cast<uint64_t>(offset) + done;
        ov.Offset = static_cast<DWORD>(curOff & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(curOff >> 32);
        DWORD rd = 0;
        if (!ReadFile(h, dst + done, static_cast<DWORD>(chunk), &rd, &ov)) {
            if (done == 0 && GetLastError() == ERROR_HANDLE_EOF) return 0;
            errno = EIO; return -1;
        }
        if (rd == 0) break;
        done += rd;
    }
    return static_cast<ssize_t>(done);
}

inline ssize_t pwrite(int fd, const void* buf, size_t count, int64_t offset) {
    if (count == 0) return 0;
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > 0x40000000ULL) chunk = 0x40000000ULL;
        OVERLAPPED ov{};
        uint64_t curOff = static_cast<uint64_t>(offset) + done;
        ov.Offset = static_cast<DWORD>(curOff & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(curOff >> 32);
        DWORD wr = 0;
        if (!WriteFile(h, src + done, static_cast<DWORD>(chunk), &wr, &ov)) {
            errno = EIO; return -1;
        }
        if (wr == 0) { errno = EIO; return -1; }
        done += wr;
    }
    return static_cast<ssize_t>(done);
}

inline int ftruncate(int fd, int64_t length) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    LARGE_INTEGER li;
    li.QuadPart = length;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) { errno = EIO; return -1; }
    if (!SetEndOfFile(h)) { errno = EIO; return -1; }
    return 0;
}

inline int fsync(int fd) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    return FlushFileBuffers(h) ? 0 : -1;
}
inline int fdatasync(int fd) { return fsync(fd); }

inline int posix_fallocate(int fd, int64_t offset, int64_t len) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { errno = EIO; return -1; }
    int64_t needed = offset + len;
    if (needed > sz.QuadPart) {
        LARGE_INTEGER li; li.QuadPart = needed;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) { errno = EIO; return -1; }
        if (!SetEndOfFile(h)) { errno = EIO; return -1; }
    }
    return 0;
}

inline void* mmap(void*, size_t length, int prot, int flags, int fd, int64_t offset) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE || length == 0) return MAP_FAILED;
    DWORD protect = PAGE_READONLY;
    DWORD access  = FILE_MAP_READ;
    if (prot & PROT_WRITE) {
        protect = (flags & MAP_SHARED) ? PAGE_READWRITE : PAGE_WRITECOPY;
        access  = (flags & MAP_SHARED) ? FILE_MAP_WRITE : FILE_MAP_COPY;
    }
    HANDLE mapping = CreateFileMappingW(h, nullptr, protect, 0, 0, nullptr);
    if (!mapping) return MAP_FAILED;
    DWORD offHi = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
    DWORD offLo = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    void* ptr = MapViewOfFile(mapping, access, offHi, offLo, length);
    CloseHandle(mapping);
    return ptr ? ptr : MAP_FAILED;
}
inline int munmap(void* addr, size_t) { return UnmapViewOfFile(addr) ? 0 : -1; }
inline int madvise(void*, size_t, int) { return 0; }
inline int msync(void* addr, size_t len, int) { return FlushViewOfFile(addr, len) ? 0 : -1; }

inline int posix_fadvise(int, int64_t, int64_t, int) { return 0; }
inline void readahead(int, int64_t, size_t) {}

inline int unlink(const char* p) { return _unlink(p); }
inline int mkdir(const char* p, mode_t) { return _mkdir(p); }

inline int erwt3d_fstat(int fd, struct _stat64* st) {
    return _fstat64(fd, st);
}
#define fstat erwt3d_fstat

inline int erwt3d_stat(const char* path, struct _stat64* st) {
    return _stat64(path, st);
}
#define stat erwt3d_stat

inline uint64_t win32_get_available_physical_memory_bytes() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) return ms.ullAvailPhys;
    return 0;
}

#else

inline int io_open(const char* path, int flags) {
    return open(path, flags);
}
inline int io_open(const char* path, int flags, int mode) {
    return open(path, flags, mode);
}
inline int io_close(int fd) { return close(fd); }
inline ssize_t io_read(int fd, void* buf, size_t count) {
    return read(fd, buf, count);
}

#endif // _WIN32
