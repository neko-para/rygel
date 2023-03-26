// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "src/core/libcc/libcc.hh"
#include "disk.hh"
#include "repository.hh"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>
#else
    #include <sys/stat.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

namespace RG {

enum class ExtractFlag {
    AllowSeparators = 1 << 0,
    FlattenName = 1 << 1
};

class GetContext {
    struct EntryInfo {
        rk_ID id;
        int kind;
        const char *basename;

        int64_t mtime;
        int64_t btime;
        uint32_t mode;
        uint32_t uid;
        uint32_t gid;
        int64_t size;

        const char *filename;
    };

    rk_Disk *disk;
    bool chown;

    Async tasks;

    std::mutex fix_mutex;
    HeapArray<EntryInfo> fix_directories;
    BlockAllocator fix_alloc;

    std::atomic<int64_t> stat_len { 0 };

public:
    GetContext(rk_Disk *disk, bool chown);

    bool ExtractEntries(Span<const uint8_t> entries, unsigned int flags, const char *dest_dirname);
    int GetFile(const rk_ID &id, rk_ObjectType type, Span<const uint8_t> file_obj, const char *dest_filename);

    bool Finish();

    int64_t GetLen() const { return stat_len; }
};

GetContext::GetContext(rk_Disk *disk, bool chown)
    : disk(disk), chown(chown), tasks(disk->GetThreads())
{
}

#ifdef _WIN32

static bool ReserveFile(int fd, const char *filename, int64_t len)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);

    LARGE_INTEGER prev_pos = {};
    if (!SetFilePointerEx(h, prev_pos, &prev_pos, FILE_CURRENT)) {
        LogError("Failed to resize file '%1': %2", filename, GetWin32ErrorString());
        return false;
    }
    RG_DEFER { SetFilePointerEx(h, prev_pos, nullptr, FILE_BEGIN); };

    if (!SetFilePointerEx(h, { .QuadPart = len }, nullptr, FILE_BEGIN)) {
        LogError("Failed to resize file '%1': %2", filename, GetWin32ErrorString());
        return false;
    }
    if (!SetEndOfFile(h)) {
        LogError("Failed to resize file '%1': %2", filename, GetWin32ErrorString());
        return false;
    }

    return true;
}

static bool WriteAt(int fd, const char *filename, int64_t offset, Span<const uint8_t> buf)
{
    RG_ASSERT(buf.len < UINT32_MAX);

    HANDLE h = (HANDLE)_get_osfhandle(fd);

    while (buf.len) {
        OVERLAPPED ov = {};
        DWORD written = 0;

        ov.OffsetHigh = (uint32_t)((offset & 0xFFFFFFFF00000000ll) >> 32);
        ov.Offset = (uint32_t)(offset & 0xFFFFFFFFll);

        if (!WriteFile(h, buf.ptr, (DWORD)buf.len, &written, &ov)) {
            LogError("Failed to write to '%1': %2", filename, GetWin32ErrorString());
            return false;
        }

        offset += (Size)written;
        buf.ptr += (Size)written;
        buf.len -= (Size)written;
    }

    return true;
}

static bool CreateSymbolicLink(const char *filename, const char *target)
{
    LogWarning("Ignoring symbolic link '%1' to '%2'", filename, target);
    return true;
}

static FILETIME UnixTimeToFileTime(int64_t time)
{
    time = (time + 11644473600000ll) * 10000;

    FILETIME ft;
    ft.dwHighDateTime = (DWORD)(time >> 32);
    ft.dwLowDateTime = (DWORD)time;

    return ft;
}

static void SetFileOwner(int, const char *, uint32_t, uint32_t)
{
}

static void SetFileMetaData(int fd, const char *filename, int64_t mtime, int64_t btime, uint32_t)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);

    FILETIME mft = UnixTimeToFileTime(mtime);
    FILETIME bft = UnixTimeToFileTime(btime);

    if (!SetFileTime(h, &bft, nullptr, &mft)) {
        LogError("Failed to set modification time of '%1': %2", filename, GetWin32ErrorString());
    }
}

#else

static bool ReserveFile(int fd, const char *filename, int64_t len)
{
    if (ftruncate(fd, len) < 0) {
        LogError("Failed to reserve file '%1': %2", filename, strerror(errno));
        return false;
    }

    return true;
}

static bool WriteAt(int fd, const char *filename, int64_t offset, Span<const uint8_t> buf)
{
    while (buf.len) {
        Size written = RG_POSIX_RESTART_EINTR(pwrite(fd, buf.ptr, buf.len, (off_t)offset), < 0);

        if (written < 0) {
            LogError("Failed to write to '%1': %2", filename, strerror(errno));
            return false;
        }

        offset += written;
        buf.ptr += written;
        buf.len -= written;
    }

    return true;
}

static bool CreateSymbolicLink(const char *filename, const char *target, bool overwrite)
{
retry:
    if (symlink(target, filename) < 0) {
        if (errno == EEXIST && overwrite) {
            struct stat sb;
            if (!lstat(filename, &sb) && S_ISLNK(sb.st_mode)) {
                unlink(filename);
            }

            overwrite = false;
            goto retry;
        }

        LogError("Failed to create symbolic link '%1': %2", filename, strerror(errno));
        return false;
    }

    return true;
}

static void SetFileOwner(int fd, const char *filename, uint32_t uid, uint32_t gid)
{
    if (fchown(fd, (uid_t)uid, (gid_t)gid) < 0) {
        LogError("Failed to change owner of '%1' (ignoring)", filename);
    }
}

static void SetFileMetaData(int fd, const char *filename, int64_t mtime, int64_t, uint32_t mode)
{
    struct timespec times[2] = {};

    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = mtime / 1000;
    times[1].tv_nsec = (mtime % 1000) * 1000;

    if (futimens(fd, times) < 0) {
        LogError("Failed to set mtime of '%1' (ignoring)", filename);
    }

    if (fchmod(fd, (mode_t)mode) < 0) {
        LogError("Failed to set permissions of '%1' (ignoring)", filename);
    }
}

#endif

bool GetContext::ExtractEntries(Span<const uint8_t> entries, unsigned int flags, const char *dest_dirname)
{
    // XXX: Make sure each path does not clobber a previous one

    std::shared_ptr<BlockAllocator> temp_alloc = std::make_shared<BlockAllocator>();

    for (Size offset = 0; offset < entries.len;) {
        EntryInfo entry = {};

        // Extract entry information
        {
            rk_FileEntry *ptr = (rk_FileEntry *)(entries.ptr + offset);

            if (entries.len - offset < RG_SIZE(*ptr)) {
                LogError("Malformed entry in directory object");
                return false;
            }

            entry.id = ptr->id;
            entry.kind = ptr->kind;
            entry.basename = ptr->name;

            entry.mtime = LittleEndian(ptr->mtime);
            entry.btime = LittleEndian(ptr->btime);
            entry.mode = LittleEndian(ptr->mode);
            entry.uid = LittleEndian(ptr->uid);
            entry.gid = LittleEndian(ptr->gid);
            entry.size = LittleEndian(ptr->size);
        }

        // Skip entry for next iteration
        {
            const uint8_t *end = (const uint8_t *)memchr(entry.basename, 0, entries.end() - (const uint8_t *)entry.basename);

            if (!end) {
                LogError("Malformed entry in directory object");
                return false;
            }

            offset = end - entries.ptr + 1;
        }

        // Sanity checks
        if (entry.kind != (int8_t)rk_FileEntry::Kind::Directory &&
                entry.kind != (int8_t)rk_FileEntry::Kind::File &&
                entry.kind != (int8_t)rk_FileEntry::Kind::Link) {
            LogError("Unknown file kind 0x%1", FmtHex((unsigned int)entry.kind));
            return false;
        }
        if (!entry.basename[0] || PathContainsDotDot(entry.basename)) {
            LogError("Unsafe file name '%1'", entry.basename);
            return false;
        }
        if (PathIsAbsolute(entry.basename)) {
            LogError("Unsafe file name '%1'", entry.basename);
            return false;
        }
        if (!(flags & (int)ExtractFlag::AllowSeparators) && strpbrk(entry.basename, RG_PATH_SEPARATORS)) {
            LogError("Unsafe file name '%1'", entry.basename);
            return false;
        }

        if (flags & (int)ExtractFlag::FlattenName) {
            entry.filename = Fmt(temp_alloc.get(), "%1%/%2", dest_dirname, SplitStrReverse(entry.basename, '/')).ptr;
        } else {
            entry.filename = Fmt(temp_alloc.get(), "%1%/%2", dest_dirname, entry.basename).ptr;

            if ((flags & (int)ExtractFlag::AllowSeparators) && !EnsureDirectoryExists(entry.filename))
                return false;
        }

        tasks.Run([=, temp_alloc = temp_alloc, this] () {
            rk_ObjectType entry_type;
            HeapArray<uint8_t> entry_obj;
            if (!disk->ReadObject(entry.id, &entry_type, &entry_obj))
                return false;

            switch (entry.kind) {
                case (int8_t)rk_FileEntry::Kind::Directory: {
                    if (entry_type != rk_ObjectType::Directory) {
                        LogError("Object '%1' is not a directory", entry.id);
                        return false;
                    }

                    if (!MakeDirectory(entry.filename, false))
                        return false;
                    if (!ExtractEntries(entry_obj, 0, entry.filename))
                        return false;

                    // Add temporary hack for directory metadata
                    {
                        std::lock_guard<std::mutex> lock(fix_mutex);

                        EntryInfo *ptr = fix_directories.Append(entry);
                        ptr->filename = DuplicateString(ptr->filename, &fix_alloc).ptr;
                    }
                } break;

                case (int8_t)rk_FileEntry::Kind::File: {
                    if (entry_type != rk_ObjectType::File && entry_type != rk_ObjectType::Chunk) {
                        LogError("Object '%1' is not a file", entry.id);
                        return false;
                    }

                    int fd = GetFile(entry.id, entry_type, entry_obj, entry.filename);
                    if (fd < 0)
                        return false;
                    RG_DEFER { close(fd); };

                    // Set file metadata
                    if (chown) {
                        SetFileOwner(fd, entry.filename, entry.uid, entry.gid);
                    }
                    SetFileMetaData(fd, entry.filename, entry.mtime, entry.btime, entry.mode);
                } break;

                case (int8_t)rk_FileEntry::Kind::Link: {
                    if (entry_type != rk_ObjectType::Link) {
                        LogError("Object '%1' is not a link", entry.id);
                        return false;
                    }

                    // NUL terminate the path
                    entry_obj.Append(0);

                    if (!CreateSymbolicLink(entry.filename, (const char *)entry_obj.ptr, true))
                        return false;
                } break;

                default: { RG_UNREACHABLE(); } break;
            }

            return true;
        });
    }

    return true;
}

int GetContext::GetFile(const rk_ID &id, rk_ObjectType type, Span<const uint8_t> file_obj, const char *dest_filename)
{
    RG_ASSERT(type == rk_ObjectType::File || type == rk_ObjectType::Chunk);

    // Open destination file
    int fd = OpenDescriptor(dest_filename, (int)OpenFlag::Write);
    if (fd < 0)
        return -1;
    RG_DEFER_N(err_guard) { close(fd); };

    int64_t file_len = -1;
    switch (type) {
        case rk_ObjectType::File: {
            if (file_obj.len % RG_SIZE(rk_ChunkEntry) != RG_SIZE(int64_t)) {
                LogError("Malformed file object '%1'", id);
                return -1;
            }

            // Get file length from end of stream
            file_obj.len -= RG_SIZE(file_len);
            memcpy(&file_len, file_obj.end(), RG_SIZE(file_len));
            file_len = LittleEndian(file_len);

            if (file_len < 0) {
                LogError("Malformed file object '%1'", id);
                return -1;
            }
            if (!ReserveFile(fd, dest_filename, file_len))
                return -1;

            Async async(&tasks);

            // Write unencrypted file
            for (Size offset = 0; offset < file_obj.len; offset += RG_SIZE(rk_ChunkEntry)) {
                async.Run([=, this]() {
                    rk_ChunkEntry entry = {};

                    memcpy(&entry, file_obj.ptr + offset, RG_SIZE(entry));
                    entry.offset = LittleEndian(entry.offset);
                    entry.len = LittleEndian(entry.len);

                    rk_ObjectType type;
                    HeapArray<uint8_t> buf;
                    if (!disk->ReadObject(entry.id, &type, &buf))
                        return false;

                    if (RG_UNLIKELY(type != rk_ObjectType::Chunk)) {
                        LogError("Object '%1' is not a chunk", entry.id);
                        return false;
                    }
                    if (RG_UNLIKELY(buf.len != entry.len)) {
                        LogError("Chunk size mismatch for '%1'", entry.id);
                        return false;
                    }
                    if (!WriteAt(fd, dest_filename, entry.offset, buf)) {
                        LogError("Failed to write to '%1': %2", dest_filename, strerror(errno));
                        return false;
                    }

                    return true;
                });
            }

            if (!async.Sync())
                return -1;

            // Check actual file size
            if (file_obj.len) {
                const rk_ChunkEntry *entry = (const rk_ChunkEntry *)(file_obj.end() - RG_SIZE(rk_ChunkEntry));
                int64_t len = LittleEndian(entry->offset) + LittleEndian(entry->len);

                if (RG_UNLIKELY(len != file_len)) {
                    LogError("File size mismatch for '%1'", entry->id);
                    return -1;
                }
            }
        } break;

        case rk_ObjectType::Chunk: {
            file_len = file_obj.len;

            if (!WriteAt(fd, dest_filename, 0, file_obj)) {
                LogError("Failed to write to '%1': %2", dest_filename, strerror(errno));
                return -1;
            }
        } break;

        case rk_ObjectType::Directory:
        case rk_ObjectType::Snapshot:
        case rk_ObjectType::Link: { RG_UNREACHABLE(); } break;
    }

    if (!FlushFile(fd, dest_filename))
        return -1;

    stat_len += file_len;

    err_guard.Disable();
    return fd;
}

bool GetContext::Finish()
{
    if (!tasks.Sync())
        return false;

    for (const EntryInfo &fix: fix_directories) {
        tasks.Run([=]() {
            int fd = OpenDescriptor(fix.filename, (int)OpenFlag::Write | (int)OpenFlag::Directory);
            RG_DEFER { close(fd); };

            // Set directory metadata
            if (chown) {
                SetFileOwner(fd, fix.filename, fix.uid, fix.gid);
            }
            SetFileMetaData(fd, fix.filename, fix.mtime, fix.btime, fix.mode);

            return true;
        });
    }

    if (!tasks.Sync())
        return false;

    fix_directories.Clear();
    fix_alloc.ReleaseAll();

    return true;
}

bool rk_Get(rk_Disk *disk, const rk_ID &id, const rk_GetSettings &settings, const char *dest_path, int64_t *out_len)
{
    rk_ObjectType type;
    HeapArray<uint8_t> obj;
    if (!disk->ReadObject(id, &type, &obj))
        return false;

    GetContext get(disk, settings.chown);

    switch (type) {
        case rk_ObjectType::Chunk:
        case rk_ObjectType::File: {
            if (!settings.force) {
                if (TestFile(dest_path) && !IsDirectoryEmpty(dest_path)) {
                    LogError("File '%1' already exists", dest_path);
                    return false;
                }
            }

            int fd = get.GetFile(id, type, obj, dest_path);
            if (fd < 0)
                return false;
            close(fd);
        } break;

        case rk_ObjectType::Directory: {
            if (!settings.force && TestFile(dest_path, FileType::Directory)) {
                if (!IsDirectoryEmpty(dest_path)) {
                    LogError("Directory '%1' exists and is not empty", dest_path);
                    return false;
                }
            } else {
                if (!MakeDirectory(dest_path, !settings.force))
                    return false;
            }

            if (!get.ExtractEntries(obj, 0, dest_path))
                return false;
        } break;

        case rk_ObjectType::Snapshot: {
            if (!settings.force && TestFile(dest_path, FileType::Directory)) {
                if (!IsDirectoryEmpty(dest_path)) {
                    LogError("Directory '%1' exists and is not empty", dest_path);
                    return false;
                }
            } else {
                if (!MakeDirectory(dest_path, !settings.force))
                    return false;
            }

            // There must be at least one entry
            if (obj.len <= RG_SIZE(rk_SnapshotHeader)) {
                LogError("Malformed snapshot object '%1'", id);
                return false;
            }

            Span<uint8_t> entries = obj.Take(RG_SIZE(rk_SnapshotHeader), obj.len - RG_SIZE(rk_SnapshotHeader));
            unsigned int flags = (int)ExtractFlag::AllowSeparators | (settings.flat ? (int)ExtractFlag::FlattenName : 0);

            if (!get.ExtractEntries(entries, flags, dest_path))
                return false;
        } break;

        case rk_ObjectType::Link: {
            obj.Append(0);

            if (!CreateSymbolicLink(dest_path, (const char *)obj.ptr, settings.force))
                return false;
        } break;
    }

    if (!get.Finish())
        return false;

    if (out_len) {
        *out_len += get.GetLen();
    }
    return true;
}

bool rk_List(rk_Disk *disk, Allocator *str_alloc, HeapArray<rk_SnapshotInfo> *out_snapshots)
{
    Size prev_len = out_snapshots->len;
    RG_DEFER_N(out_guard) { out_snapshots->RemoveFrom(prev_len); };

    HeapArray<rk_ID> ids;
    if (!disk->ListTags(&ids))
        return false;

    Async async(disk->GetThreads());

    // Gather snapshot information
    {
        std::mutex mutex;

        for (const rk_ID &id: ids) {
            async.Run([=, &mutex]() {
                rk_SnapshotInfo snapshot = {};

                rk_ObjectType type;
                HeapArray<uint8_t> obj;
                if (!disk->ReadObject(id, &type, &obj))
                    return false;

                if (type != rk_ObjectType::Snapshot) {
                    LogError("Object '%1' is not a snapshot (ignoring)", id);
                    return true;
                }
                if (obj.len <= RG_SIZE(rk_SnapshotHeader)) {
                    LogError("Malformed snapshot object '%1' (ignoring)", id);
                    return true;
                }

                std::lock_guard lock(mutex);
                const rk_SnapshotHeader *header = (const rk_SnapshotHeader *)obj.ptr;

                snapshot.id = id;
                snapshot.name = header->name[0] ? DuplicateString(header->name, str_alloc).ptr : nullptr;
                snapshot.time = LittleEndian(header->time);
                snapshot.len = LittleEndian(header->len);
                snapshot.stored = LittleEndian(header->stored) + obj.len;

                out_snapshots->Append(snapshot);

                return true;
            });
        }
    }

    if (!async.Sync())
        return false;

    std::sort(out_snapshots->ptr + prev_len, out_snapshots->end(),
              [](const rk_SnapshotInfo &snapshot1, const rk_SnapshotInfo &snapshot2) { return snapshot1.time < snapshot2.time; });

    out_guard.Disable();
    return true;
}

}
