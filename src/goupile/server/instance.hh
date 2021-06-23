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
// along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "../../core/libcc/libcc.hh"
#include "../../core/libnet/libnet.hh"
#include "../../core/libsqlite/libsqlite.hh"
#include "session.hh"

namespace RG {

extern const int InstanceVersion;

enum SyncMode {
    Online,
    Mirror,
    Offline
};
static const char *const SyncModeNames[] = {
    "Online",
    "Mirror",
    "Offline"
};

class InstanceHolder {
    mutable std::atomic_int refcount {0};

    // Strictly managed by DomainHolder
    int64_t generation;

public:
    int64_t unique = -1;

    Span<const char> key = {};
    const char *filename = nullptr;
    sq_Database db;
    const char *title = nullptr;

    InstanceHolder *master = nullptr;
    HeapArray<InstanceHolder *> slaves;

    struct {
        const char *name = nullptr;
        bool use_offline = false;
        SyncMode sync_mode = SyncMode::Online;
        int max_file_size = (int)Megabytes(16);
        const char *shared_key = nullptr;
        const char *token_key = nullptr;
        uint8_t token_skey[32];
        uint8_t token_pkey[32];
        const char *backup_key = nullptr;
        int64_t auto_userid = 0;
        const char *auto_key = nullptr;
    } config;

    // Lazily initialized
    bool auto_init = false;
    RetainPtr<SessionInfo> auto_session;

    BlockAllocator str_alloc;

    RG_HASHTABLE_HANDLER(InstanceHolder, key);

    ~InstanceHolder();

    bool Checkpoint();

    void Ref() const { master->refcount++; }
    void Unref() const { master->refcount--; }

private:
    InstanceHolder() {};

    bool Open(int64_t unique, InstanceHolder *master, const char *key, const char *filename);

    friend class DomainHolder;
};

bool MigrateInstance(sq_Database *db);
bool MigrateInstance(const char *filename);

}
