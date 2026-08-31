/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Sapote's bounded VFS object layer. The public sapfs_* names are retained as
 * the native ABI v1 compatibility surface, while concrete filesystem handles
 * and path rules remain behind a backend contract.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/fat32_backend.h>
#include <sapote/fat32_fs.h>
#include <sapote/ext4_fs.h>
#include <sapote/vfs_backend.h>

#define VFS_MAX_VNODES 128U
#define VFS_VNODE_BUCKETS 64U
#define VFS_MAX_OPEN_FILES SAPFS_MAX_HANDLES
#define VFS_MAX_DIRECTORY_ITERATORS 32U
#define VFS_NO_INDEX UINT16_MAX

struct vfs_mount_state {
    const struct vfs_backend_ops *backend;
    uint64_t generation;
    size_t references;
    enum sapfs_volume volume;
    bool active;
};

struct vfs_vnode_state {
    struct sapfs_stat stat;
    uint64_t generation;
    uint64_t mount_generation;
    size_t references;
    uint16_t next_bucket;
    enum sapfs_volume volume;
    char path[SAPFS_MAX_PATH];
    bool active;
};

struct vfs_open_file_state {
    const struct vfs_backend_ops *backend;
    uint64_t generation;
    uint64_t vnode_generation;
    sapfs_handle backend_handle;
    uint16_t vnode_index;
    bool active;
};

struct vfs_directory_state {
    struct sapfs_list_entry entries[SAPFS_MAX_LIST_ENTRIES];
    const struct vfs_backend_ops *backend;
    sapfs_handle backend_handle;
    uint64_t generation;
    uint64_t vnode_generation;
    size_t count;
    size_t cursor;
    uint16_t vnode_index;
    bool streaming;
    bool active;
};

static struct vfs_mount_state mounts[SAPFS_VOLUME_COUNT];
static struct vfs_vnode_state vnodes[VFS_MAX_VNODES];
static struct vfs_open_file_state open_files[VFS_MAX_OPEN_FILES];
static struct vfs_directory_state directories[VFS_MAX_DIRECTORY_ITERATORS];
static uint16_t vnode_buckets[VFS_VNODE_BUCKETS];
static uint64_t next_mount_generation = UINT64_C(1);
static uint64_t next_vnode_generation = UINT64_C(1);
static uint64_t next_open_generation = UINT64_C(1);
static uint64_t next_directory_generation = UINT64_C(1);

static const struct vfs_backend_ops fat32_backend_ops = {
    .mount = fat32_backend_mount,
    .unmount = fat32_backend_unmount,
    .sync = fat32_backend_sync,
    .drive = fat32_backend_drive,
    .completion_count = fat32_backend_completion_count,
    .open = fat32_backend_open,
    .close = fat32_backend_close,
    .read = fat32_backend_read,
    .pread = fat32_backend_pread,
    .write = fat32_backend_write,
    .seek = fat32_backend_seek,
    .stat_path = fat32_backend_stat_path,
    .list = fat32_backend_list,
    .directory_open = NULL,
    .directory_read = NULL,
    .directory_close = NULL,
    .create = fat32_backend_create,
    .truncate = fat32_backend_truncate,
    .mkdir = fat32_backend_mkdir,
    .rename = fat32_backend_rename,
    .unlink = fat32_backend_unlink,
    .rmdir = fat32_backend_rmdir,
    .link = fat32_backend_link,
    .case_sensitive = false,
};

static const struct vfs_backend_ops ext4_backend_ops = {
    .mount = ext4_backend_mount,
    .unmount = ext4_backend_unmount,
    .sync = ext4_backend_sync,
    .drive = ext4_backend_drive,
    .completion_count = ext4_backend_completion_count,
    .open = ext4_backend_open,
    .close = ext4_backend_close,
    .read = ext4_backend_read,
    .pread = ext4_backend_pread,
    .write = ext4_backend_write,
    .seek = ext4_backend_seek,
    .stat_path = ext4_backend_stat_path,
    .list = ext4_backend_list,
    .directory_open = ext4_backend_directory_open,
    .directory_read = ext4_backend_directory_read,
    .directory_close = ext4_backend_directory_close,
    .create = ext4_backend_create,
    .truncate = ext4_backend_truncate,
    .mkdir = ext4_backend_mkdir,
    .rename = ext4_backend_rename,
    .unlink = ext4_backend_unlink,
    .rmdir = ext4_backend_rmdir,
    .link = ext4_backend_link,
    .case_sensitive = true,
};

static const struct vfs_backend_ops *volume_backends[SAPFS_VOLUME_COUNT];

_Static_assert(VFS_MAX_OPEN_FILES <= UINT8_MAX,
    "VFS open-file index no longer fits encoded handle");
_Static_assert(VFS_MAX_DIRECTORY_ITERATORS <= UINT8_MAX,
    "VFS directory index no longer fits encoded handle");
_Static_assert(VFS_VNODE_BUCKETS != 0U &&
    (VFS_VNODE_BUCKETS & (VFS_VNODE_BUCKETS - 1U)) == 0U,
    "VFS vnode bucket count must be a power of two");

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *to = destination;
    const uint8_t *from = source;

    for (size_t index = 0U; index < length; ++index) {
        to[index] = from[index];
    }
}

static bool valid_volume(enum sapfs_volume volume)
{
    return volume >= SAPFS_VOLUME_SYSTEM && volume < SAPFS_VOLUME_COUNT;
}

static size_t text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return SAPFS_MAX_PATH;
    }
    while (length < SAPFS_MAX_PATH && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool text_equal(const char *left, const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (index < SAPFS_MAX_PATH && left[index] == right[index]) {
        if (left[index] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

static uint64_t next_generation(uint64_t *counter, uint64_t maximum)
{
    uint64_t result = *counter;

    ++*counter;
    if (result == 0U || result > maximum) {
        result = 1U;
        *counter = 2U;
    }
    return result;
}

static enum sapfs_status canonicalize_path(
    const char *path,
    bool case_sensitive,
    char canonical[SAPFS_MAX_PATH]
)
{
    size_t component_starts[SAPFS_MAX_DEPTH];
    const size_t length = text_length(path);
    size_t component_count = 0U;
    size_t used = 0U;
    size_t index = 0U;

    if (canonical == NULL || length == 0U || length >= SAPFS_MAX_PATH ||
        path[0] == '/' || path[0] == '\\') {
        return SAPFS_STATUS_PATH;
    }
    zero_bytes(canonical, SAPFS_MAX_PATH);
    while (index < length) {
        size_t start = index;
        size_t component_length;

        while (index < length && path[index] != '/') {
            const uint8_t byte = (uint8_t)path[index];

            if (byte > UINT8_C(0x7F) || path[index] == '\\' ||
                path[index] == ':') {
                return SAPFS_STATUS_PATH;
            }
            ++index;
        }
        component_length = index - start;
        if (component_length == 0U) {
            return SAPFS_STATUS_PATH;
        }
        if (component_length == 1U && path[start] == '.') {
            /* A mount-relative dot is the retained current vnode. */
        } else if (component_length == 2U && path[start] == '.' &&
                path[start + 1U] == '.') {
            if (component_count == 0U) {
                return SAPFS_STATUS_PATH;
            }
            used = component_starts[--component_count];
            if (used != 0U) {
                --used;
            }
            canonical[used] = '\0';
        } else {
            if (component_count >= SAPFS_MAX_DEPTH ||
                used + component_length + (used == 0U ? 0U : 1U) >=
                    SAPFS_MAX_PATH) {
                return SAPFS_STATUS_PATH;
            }
            if (used != 0U) {
                canonical[used++] = '/';
            }
            component_starts[component_count++] = used;
            for (size_t offset = 0U; offset < component_length; ++offset) {
                char byte = path[start + offset];

                if (!case_sensitive && byte >= 'a' && byte <= 'z') {
                    byte = (char)(byte - 'a' + 'A');
                }
                canonical[used++] = byte;
            }
            canonical[used] = '\0';
        }
        if (index < length) {
            ++index;
            if (index == length) {
                return SAPFS_STATUS_PATH;
            }
        }
    }
    if (used == 0U) {
        canonical[0] = '.';
        canonical[1] = '\0';
    }
    return SAPFS_STATUS_OK;
}

static size_t vnode_bucket(enum sapfs_volume volume, const char *path,
    uint64_t object_id)
{
    uint64_t hash = UINT64_C(1469598103934665603) ^ (uint64_t)volume;

    if (object_id != 0U) {
        for (size_t byte = 0U; byte < sizeof(object_id); ++byte) {
            hash ^= (uint8_t)(object_id >> (byte * 8U));
            hash *= UINT64_C(1099511628211);
        }
    } else for (size_t index = 0U; path[index] != '\0'; ++index) {
        hash ^= (uint8_t)path[index];
        hash *= UINT64_C(1099511628211);
    }
    return (size_t)(hash & (VFS_VNODE_BUCKETS - 1U));
}

static void vnode_remove_from_bucket(size_t vnode_index)
{
    struct vfs_vnode_state *vnode = &vnodes[vnode_index];
    const size_t bucket = vnode_bucket(vnode->volume, vnode->path,
        vnode->stat.object_id);
    uint16_t *link = &vnode_buckets[bucket];

    for (size_t steps = 0U; steps < VFS_MAX_VNODES &&
         *link != VFS_NO_INDEX; ++steps) {
        if (*link == vnode_index) {
            *link = vnodes[*link].next_bucket;
            return;
        }
        link = &vnodes[*link].next_bucket;
    }
}

static enum sapfs_status vnode_retain(
    enum sapfs_volume volume,
    const char *canonical,
    const struct sapfs_stat *stat,
    size_t *vnode_index
)
{
    size_t bucket;
    uint16_t current;
    size_t free_index = VFS_MAX_VNODES;

    if (stat == NULL || vnode_index == NULL || !valid_volume(volume) ||
        !mounts[volume].active) {
        return SAPFS_STATUS_NOT_MOUNTED;
    }
    bucket = vnode_bucket(volume, canonical, stat->object_id);
    current = vnode_buckets[bucket];
    for (size_t steps = 0U; steps < VFS_MAX_VNODES &&
         current != VFS_NO_INDEX; ++steps) {
        struct vfs_vnode_state *vnode = &vnodes[current];

        if (vnode->active && vnode->volume == volume &&
            vnode->mount_generation == mounts[volume].generation &&
            ((stat->object_id != 0U &&
                vnode->stat.object_id == stat->object_id) ||
             (stat->object_id == 0U && text_equal(vnode->path, canonical)))) {
            if (vnode->references == SIZE_MAX) {
                return SAPFS_STATUS_BUSY;
            }
            ++vnode->references;
            vnode->stat = *stat;
            *vnode_index = current;
            return SAPFS_STATUS_OK;
        }
        current = vnode->next_bucket;
    }
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) {
        if (!vnodes[index].active) {
            free_index = index;
            break;
        }
    }
    if (free_index == VFS_MAX_VNODES || mounts[volume].references == SIZE_MAX) {
        return SAPFS_STATUS_NO_HANDLES;
    }
    zero_bytes(&vnodes[free_index], sizeof(vnodes[free_index]));
    vnodes[free_index].stat = *stat;
    vnodes[free_index].generation = next_generation(
        &next_vnode_generation, UINT64_MAX);
    vnodes[free_index].mount_generation = mounts[volume].generation;
    vnodes[free_index].references = 1U;
    vnodes[free_index].next_bucket = vnode_buckets[bucket];
    vnodes[free_index].volume = volume;
    copy_bytes(vnodes[free_index].path, canonical,
        text_length(canonical) + 1U);
    vnodes[free_index].active = true;
    vnode_buckets[bucket] = (uint16_t)free_index;
    ++mounts[volume].references;
    *vnode_index = free_index;
    return SAPFS_STATUS_OK;
}

static void vnode_release(size_t vnode_index, uint64_t generation)
{
    struct vfs_vnode_state *vnode;

    if (vnode_index >= VFS_MAX_VNODES) {
        return;
    }
    vnode = &vnodes[vnode_index];
    if (!vnode->active || vnode->generation != generation ||
        vnode->references == 0U) {
        return;
    }
    --vnode->references;
    if (vnode->references != 0U) {
        return;
    }
    vnode_remove_from_bucket(vnode_index);
    if (valid_volume(vnode->volume) &&
        mounts[vnode->volume].references != 0U) {
        --mounts[vnode->volume].references;
    }
    zero_bytes(vnode, sizeof(*vnode));
}

static enum sapfs_status resolve_path(
    enum sapfs_volume volume,
    const char *path,
    char canonical[SAPFS_MAX_PATH],
    size_t *vnode_index
)
{
    char partial[SAPFS_MAX_PATH];
    struct sapfs_stat stat;
    enum sapfs_status status;
    size_t length;

    if (!valid_volume(volume) || canonical == NULL || vnode_index == NULL ||
        !mounts[volume].active) {
        return SAPFS_STATUS_NOT_MOUNTED;
    }
    status = canonicalize_path(path, mounts[volume].backend->case_sensitive,
        canonical);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (canonical[0] == '.' && canonical[1] == '\0') {
        status = mounts[volume].backend->stat_path(volume, canonical, &stat);
        return status == SAPFS_STATUS_OK ?
            vnode_retain(volume, canonical, &stat, vnode_index) : status;
    }
    zero_bytes(partial, sizeof(partial));
    length = text_length(canonical);
    for (size_t index = 0U; index <= length; ++index) {
        if (index != length && canonical[index] != '/') {
            partial[index] = canonical[index];
            continue;
        }
        partial[index] = '\0';
        status = mounts[volume].backend->stat_path(volume, partial, &stat);
        if (status != SAPFS_STATUS_OK) {
            return status;
        }
        if (index != length && !stat.directory) {
            return SAPFS_STATUS_NOT_DIRECTORY;
        }
        if (index != length) {
            partial[index] = '/';
        }
    }
    return vnode_retain(volume, canonical, &stat, vnode_index);
}

static enum sapfs_status resolve_parent(
    enum sapfs_volume volume,
    const char *path,
    char canonical[SAPFS_MAX_PATH]
)
{
    char parent[SAPFS_MAX_PATH];
    char resolved_parent[SAPFS_MAX_PATH];
    size_t vnode_index;
    size_t split = SIZE_MAX;
    enum sapfs_status status = valid_volume(volume) && mounts[volume].active ?
        canonicalize_path(path, mounts[volume].backend->case_sensitive,
            canonical) : SAPFS_STATUS_NOT_MOUNTED;

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (canonical[0] == '.' && canonical[1] == '\0') {
        return SAPFS_STATUS_ACCESS;
    }
    for (size_t index = 0U; canonical[index] != '\0'; ++index) {
        if (canonical[index] == '/') {
            split = index;
        }
    }
    zero_bytes(parent, sizeof(parent));
    if (split == SIZE_MAX) {
        parent[0] = '.';
        parent[1] = '\0';
    } else {
        copy_bytes(parent, canonical, split);
        parent[split] = '\0';
    }
    status = resolve_path(volume, parent, resolved_parent, &vnode_index);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (!vnodes[vnode_index].stat.directory) {
        status = SAPFS_STATUS_NOT_DIRECTORY;
    }
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status;
}

static void install_mount(
    enum sapfs_volume volume,
    const struct vfs_backend_ops *backend
)
{
    struct sapfs_drive_info drive;

    if (backend == NULL) {
        return;
    }
    drive = backend->drive(volume);
    if (!drive.mounted) {
        return;
    }
    mounts[volume].backend = backend;
    mounts[volume].generation = next_generation(
        &next_mount_generation, UINT64_MAX);
    mounts[volume].volume = volume;
    mounts[volume].active = true;
}

static sapfs_handle encode_handle(size_t index, uint64_t generation)
{
    return generation << 8U | (uint64_t)(index + 1U);
}

static enum sapfs_status open_file_state(
    sapfs_handle handle,
    struct vfs_open_file_state **state
)
{
    const uint64_t encoded_index = handle & UINT64_C(0xFF);
    const uint64_t generation = handle >> 8U;
    size_t index;

    if (state == NULL || encoded_index == 0U ||
        encoded_index > VFS_MAX_OPEN_FILES || generation == 0U) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!open_files[index].active ||
        open_files[index].generation != generation) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    *state = &open_files[index];
    return SAPFS_STATUS_OK;
}

static enum sapfs_status directory_state(
    sapfs_directory_handle handle,
    struct vfs_directory_state **state
)
{
    const uint64_t encoded_index = handle & UINT64_C(0xFF);
    const uint64_t generation = handle >> 8U;
    size_t index;

    if (state == NULL || encoded_index == 0U ||
        encoded_index > VFS_MAX_DIRECTORY_ITERATORS || generation == 0U) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!directories[index].active ||
        directories[index].generation != generation) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    *state = &directories[index];
    return SAPFS_STATUS_OK;
}

bool sapfs_self_test(size_t *completed_tests)
{
    return fat32_backend_self_test(completed_tests);
}

void sapfs_initialize(void)
{
    zero_bytes(mounts, sizeof(mounts));
    zero_bytes(vnodes, sizeof(vnodes));
    zero_bytes(open_files, sizeof(open_files));
    zero_bytes(directories, sizeof(directories));
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) {
        vnode_buckets[index] = VFS_NO_INDEX;
    }
    fat32_backend_initialize();
    ext4_backend_initialize();
    for (enum sapfs_volume volume = SAPFS_VOLUME_SYSTEM;
         volume < SAPFS_VOLUME_COUNT; ++volume) {
        if (fat32_backend_drive(volume).mounted) {
            volume_backends[volume] = &fat32_backend_ops;
        } else if (ext4_backend_mount(volume) == SAPFS_STATUS_OK) {
            volume_backends[volume] = &ext4_backend_ops;
        } else {
            volume_backends[volume] = &fat32_backend_ops;
        }
        install_mount(volume, volume_backends[volume]);
    }
}

enum sapfs_status sapfs_mount(enum sapfs_volume volume)
{
    const struct vfs_backend_ops *backend;
    enum sapfs_status status;

    if (!valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    if (mounts[volume].active) {
        return SAPFS_STATUS_ALREADY_MOUNTED;
    }
    backend = volume_backends[volume];
    if (backend == NULL) {
        return SAPFS_STATUS_NOT_MOUNTED;
    }
    status = backend->mount(volume);
    if (status == SAPFS_STATUS_OK) {
        install_mount(volume, backend);
    }
    return status;
}

enum sapfs_status sapfs_unmount(enum sapfs_volume volume)
{
    enum sapfs_status status;

    if (!valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    if (!mounts[volume].active) {
        return SAPFS_STATUS_NOT_MOUNTED;
    }
    if (mounts[volume].references != 0U) {
        return SAPFS_STATUS_BUSY;
    }
    status = mounts[volume].backend->unmount(volume);
    if (status == SAPFS_STATUS_OK) {
        zero_bytes(&mounts[volume], sizeof(mounts[volume]));
    }
    return status;
}

enum sapfs_status sapfs_sync(enum sapfs_volume volume)
{
    if (!valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    return mounts[volume].active ? mounts[volume].backend->sync(volume) :
        SAPFS_STATUS_NOT_MOUNTED;
}

struct sapfs_drive_info sapfs_drive(enum sapfs_volume volume)
{
    const struct sapfs_drive_info absent = {0};
    const struct vfs_backend_ops *backend;

    if (!valid_volume(volume)) {
        return absent;
    }
    backend = mounts[volume].active ? mounts[volume].backend :
        volume_backends[volume];
    return backend != NULL ? backend->drive(volume) : absent;
}

uint64_t sapfs_completion_count(enum sapfs_volume volume)
{
    const struct vfs_backend_ops *backend;

    if (!valid_volume(volume)) {
        return 0U;
    }
    backend = mounts[volume].active ? mounts[volume].backend :
        volume_backends[volume];
    return backend != NULL ? backend->completion_count(volume) : 0U;
}

enum sapfs_status sapfs_open(
    enum sapfs_volume volume,
    const char *path,
    enum sapfs_access access,
    sapfs_handle *handle
)
{
    char canonical[SAPFS_MAX_PATH];
    sapfs_handle backend_handle = 0U;
    size_t vnode_index;
    size_t slot = VFS_MAX_OPEN_FILES;
    enum sapfs_status status;

    if (handle == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    status = resolve_path(volume, path, canonical, &vnode_index);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (vnodes[vnode_index].stat.directory) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return SAPFS_STATUS_IS_DIRECTORY;
    }
    for (size_t index = 0U; index < VFS_MAX_OPEN_FILES; ++index) {
        if (!open_files[index].active) {
            slot = index;
            break;
        }
    }
    if (slot == VFS_MAX_OPEN_FILES) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return SAPFS_STATUS_NO_HANDLES;
    }
    status = mounts[volume].backend->open(
        volume, canonical, access, &backend_handle);
    if (status != SAPFS_STATUS_OK) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return status;
    }
    zero_bytes(&open_files[slot], sizeof(open_files[slot]));
    open_files[slot].generation = next_generation(
        &next_open_generation, UINT64_MAX >> 8U);
    open_files[slot].backend = mounts[volume].backend;
    open_files[slot].vnode_generation = vnodes[vnode_index].generation;
    open_files[slot].backend_handle = backend_handle;
    open_files[slot].vnode_index = (uint16_t)vnode_index;
    open_files[slot].active = true;
    *handle = encode_handle(slot, open_files[slot].generation);
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_close(sapfs_handle handle)
{
    struct vfs_open_file_state *state;
    sapfs_handle backend_handle;
    const struct vfs_backend_ops *backend;
    uint64_t vnode_generation;
    uint16_t vnode_index;
    enum sapfs_status status = open_file_state(handle, &state);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    backend_handle = state->backend_handle;
    backend = state->backend;
    vnode_generation = state->vnode_generation;
    vnode_index = state->vnode_index;
    state->active = false;
    state->backend_handle = 0U;
    status = backend->close(backend_handle);
    vnode_release(vnode_index, vnode_generation);
    return status;
}

enum sapfs_status sapfs_read(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    struct vfs_open_file_state *state;
    enum sapfs_status status = open_file_state(handle, &state);

    return status == SAPFS_STATUS_OK ? state->backend->read(
        state->backend_handle, destination, capacity, read_bytes) : status;
}

enum sapfs_status sapfs_pread(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
)
{
    struct vfs_open_file_state *state;
    enum sapfs_status status = open_file_state(handle, &state);

    return status == SAPFS_STATUS_OK ? state->backend->pread(
        state->backend_handle, destination, capacity, offset, read_bytes) :
        status;
}

enum sapfs_status sapfs_write(
    sapfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
)
{
    struct vfs_open_file_state *state;
    enum sapfs_status status = open_file_state(handle, &state);

    return status == SAPFS_STATUS_OK ? state->backend->write(
        state->backend_handle, source, source_bytes, written_bytes) : status;
}

enum sapfs_status sapfs_seek(
    sapfs_handle handle,
    int64_t offset,
    enum sapfs_seek_origin origin,
    uint64_t *position
)
{
    struct vfs_open_file_state *state;
    enum sapfs_status status = open_file_state(handle, &state);

    return status == SAPFS_STATUS_OK ? state->backend->seek(
        state->backend_handle, offset, origin, position) : status;
}

enum sapfs_status sapfs_stat_path(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_stat *stat
)
{
    char canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status;

    if (stat == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(stat, sizeof(*stat));
    status = resolve_path(volume, path, canonical, &vnode_index);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    *stat = vnodes[vnode_index].stat;
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_list(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
)
{
    char canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (!vnodes[vnode_index].stat.directory) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return SAPFS_STATUS_NOT_DIRECTORY;
    }
    status = mounts[volume].backend->list(volume, canonical, entries, capacity,
        entry_count);
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status;
}

enum sapfs_status sapfs_directory_open(
    enum sapfs_volume volume,
    const char *path,
    sapfs_directory_handle *handle
)
{
    char canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    size_t slot = VFS_MAX_DIRECTORY_ITERATORS;
    enum sapfs_status status;

    if (handle == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    status = resolve_path(volume, path, canonical, &vnode_index);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (!vnodes[vnode_index].stat.directory) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return SAPFS_STATUS_NOT_DIRECTORY;
    }
    for (size_t index = 0U; index < VFS_MAX_DIRECTORY_ITERATORS; ++index) {
        if (!directories[index].active) {
            slot = index;
            break;
        }
    }
    if (slot == VFS_MAX_DIRECTORY_ITERATORS) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return SAPFS_STATUS_NO_HANDLES;
    }
    zero_bytes(&directories[slot], sizeof(directories[slot]));
    directories[slot].backend = mounts[volume].backend;
    directories[slot].streaming =
        mounts[volume].backend->directory_open != NULL &&
        mounts[volume].backend->directory_read != NULL &&
        mounts[volume].backend->directory_close != NULL;
    if (directories[slot].streaming) {
        status = mounts[volume].backend->directory_open(volume, canonical,
            &directories[slot].backend_handle);
    } else {
        status = mounts[volume].backend->list(volume, canonical,
            directories[slot].entries, SAPFS_MAX_LIST_ENTRIES,
            &directories[slot].count);
    }
    if (status != SAPFS_STATUS_OK) {
        vnode_release(vnode_index, vnodes[vnode_index].generation);
        return status;
    }
    directories[slot].generation = next_generation(
        &next_directory_generation, UINT64_MAX >> 8U);
    directories[slot].vnode_generation = vnodes[vnode_index].generation;
    directories[slot].vnode_index = (uint16_t)vnode_index;
    directories[slot].active = true;
    *handle = encode_handle(slot, directories[slot].generation);
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_directory_read(
    sapfs_directory_handle handle,
    struct sapfs_list_entry *entry,
    bool *present
)
{
    struct vfs_directory_state *state;
    enum sapfs_status status;

    if (entry == NULL || present == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(entry, sizeof(*entry));
    *present = false;
    status = directory_state(handle, &state);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (state->streaming) {
        return state->backend->directory_read(state->backend_handle, entry,
            present);
    }
    if (state->cursor == state->count) {
        return SAPFS_STATUS_OK;
    }
    if (state->cursor > state->count) {
        return SAPFS_STATUS_CORRUPT;
    }
    *entry = state->entries[state->cursor++];
    *present = true;
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_directory_close(sapfs_directory_handle handle)
{
    struct vfs_directory_state *state;
    uint64_t vnode_generation;
    uint16_t vnode_index;
    enum sapfs_status status = directory_state(handle, &state);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    vnode_generation = state->vnode_generation;
    vnode_index = state->vnode_index;
    if (state->streaming) {
        status = state->backend->directory_close(state->backend_handle);
    }
    state->active = false;
    vnode_release(vnode_index, vnode_generation);
    return status;
}

enum sapfs_status sapfs_create(enum sapfs_volume volume, const char *path)
{
    char canonical[SAPFS_MAX_PATH];
    enum sapfs_status status = resolve_parent(volume, path, canonical);

    return status == SAPFS_STATUS_OK ?
        mounts[volume].backend->create(volume, canonical) : status;
}

enum sapfs_status sapfs_truncate(
    enum sapfs_volume volume,
    const char *path,
    uint64_t size
)
{
    char canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (vnodes[vnode_index].stat.directory) {
        status = SAPFS_STATUS_IS_DIRECTORY;
    }
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status == SAPFS_STATUS_OK ?
        mounts[volume].backend->truncate(volume, canonical, size) : status;
}

enum sapfs_status sapfs_mkdir(enum sapfs_volume volume, const char *path)
{
    char canonical[SAPFS_MAX_PATH];
    enum sapfs_status status = resolve_parent(volume, path, canonical);

    return status == SAPFS_STATUS_OK ?
        mounts[volume].backend->mkdir(volume, canonical) : status;
}

enum sapfs_status sapfs_rename(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
)
{
    char source_canonical[SAPFS_MAX_PATH];
    char destination_canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status = resolve_path(
        volume, source, source_canonical, &vnode_index);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    status = resolve_parent(volume, destination, destination_canonical);
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status == SAPFS_STATUS_OK ? mounts[volume].backend->rename(volume,
        source_canonical, destination_canonical) : status;
}

enum sapfs_status sapfs_unlink(enum sapfs_volume volume, const char *path)
{
    char canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (vnodes[vnode_index].stat.directory) {
        status = SAPFS_STATUS_IS_DIRECTORY;
    }
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status == SAPFS_STATUS_OK ?
        mounts[volume].backend->unlink(volume, canonical) : status;
}

enum sapfs_status sapfs_rmdir(enum sapfs_volume volume, const char *path)
{
    char canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (!vnodes[vnode_index].stat.directory) {
        status = SAPFS_STATUS_NOT_DIRECTORY;
    } else if (vnodes[vnode_index].references != 1U) {
        status = SAPFS_STATUS_BUSY;
    }
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status == SAPFS_STATUS_OK ?
        mounts[volume].backend->rmdir(volume, canonical) : status;
}

enum sapfs_status sapfs_link(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
)
{
    char source_canonical[SAPFS_MAX_PATH];
    char destination_canonical[SAPFS_MAX_PATH];
    size_t vnode_index;
    enum sapfs_status status = resolve_path(
        volume, source, source_canonical, &vnode_index);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    status = resolve_parent(volume, destination, destination_canonical);
    vnode_release(vnode_index, vnodes[vnode_index].generation);
    return status == SAPFS_STATUS_OK ? mounts[volume].backend->link(volume,
        source_canonical, destination_canonical) : status;
}
