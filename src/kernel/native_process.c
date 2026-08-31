/* SPDX-License-Identifier: GPL-3.0-only */
/* General native application admission, scheduling, syscalls, and teardown. */

#include <sapote/native_process.h>

#include <sapote/abi.h>
#include <sapote/clock.h>
#include <sapote/console.h>
#include <sapote/cpu.h>
#include <sapote/fat32_fs.h>
#include <sapote/framebuffer.h>
#include <sapote/heap.h>
#include <sapote/interrupts.h>
#include <sapote/memory.h>
#include <sapote/native_fpu.h>
#include <sapote/native_handle.h>
#include <sapote/native_image.h>
#include <sapote/native_syscall.h>
#include <sapote/network.h>
#include <sapote/keyboard.h>
#include <sapote/paging.h>
#include <sapote/process.h>
#include <sapote/random.h>
#include <sapote/timer.h>
#include <sapote/tsc.h>
#include <sapote/ui.h>
#include <sapote/wall_clock.h>

#define IA32_FS_BASE UINT32_C(0xC0000100)
#define NATIVE_MAIN_STACK_GUARD PAGING_NATIVE_STACK_BASE
#define NATIVE_MAIN_STACK_BASE (NATIVE_MAIN_STACK_GUARD + PAGING_PAGE_SIZE)
#define NATIVE_MAIN_STACK_END \
    (NATIVE_MAIN_STACK_BASE + NATIVE_STACK_PAGES * PAGING_PAGE_SIZE)
#define NATIVE_COPY_CHUNK 4096U
#define NATIVE_AUX_NULL UINT64_C(0)
#define NATIVE_AUX_PAGESZ UINT64_C(6)
#define NATIVE_AUX_ENTRY UINT64_C(9)
#define NATIVE_AUX_SAPOTE_ABI UINT64_C(0x53500001)
#define NATIVE_AUX_TLS_IMAGE UINT64_C(0x53500002)
#define NATIVE_AUX_TLS_SIZE UINT64_C(0x53500003)
#define NATIVE_AUX_TLS_ALIGN UINT64_C(0x53500004)
#define NATIVE_MAIN_TLS_GUARD PAGING_NATIVE_ANON_BASE
#define NATIVE_MAIN_TLS_BASE (NATIVE_MAIN_TLS_GUARD + PAGING_PAGE_SIZE)
#define NATIVE_TLS_TCB_BYTES sizeof(uint64_t)
#define ELF_PF_X UINT32_C(1)
#define ELF_PF_W UINT32_C(2)
#define NATIVE_RFLAGS UINT64_C(0x202)
#define NATIVE_EVENT_QUEUE_CAPACITY 64U
#define NATIVE_CONSOLE_INPUT_CAPACITY 256U
#define NATIVE_SURFACE_MAX_WIDTH 1280U
#define NATIVE_SURFACE_MAX_HEIGHT 720U
#define NATIVE_SCHEDULER_MIN_SLEEP_NS UINT64_C(100000)

enum native_thread_state {
    NATIVE_THREAD_UNUSED = 0,
    NATIVE_THREAD_RUNNABLE,
    NATIVE_THREAD_SLEEP_WAIT,
    NATIVE_THREAD_JOIN_WAIT,
    NATIVE_THREAD_FUTEX_WAIT,
    NATIVE_THREAD_CONSOLE_WAIT,
    NATIVE_THREAD_HANDLE_WAIT,
    NATIVE_THREAD_EXITED,
    NATIVE_THREAD_FAULTED
};

struct native_page {
    uint64_t virtual_address;
    uintptr_t physical_address;
    uint32_t permissions;
    enum paging_process_mapping_kind kind;
    bool mapped;
};

struct native_thread {
    struct process_user_context context;
    struct native_fpu_state fpu;
    uint64_t fs_base;
    uint64_t generation;
    uint64_t stack_base;
    uint64_t stack_end;
    uint64_t wait_generation;
    uint64_t futex_address;
    uint64_t deadline_ns;
    uint64_t console_address;
    uint64_t wait_items_address;
    size_t console_length;
    size_t wait_item_count;
    struct sapote_wait_item wait_items[SAPOTE_WAIT_MAX];
    int32_t exit_status;
    enum native_thread_state state;
};

struct native_directory_resource {
    sapfs_directory_handle iterator;
    bool active;
};

struct native_window_state {
    struct sapote_event events[NATIVE_EVENT_QUEUE_CAPACITY];
    uint32_t *shadow_pixels;
    uint64_t surface_address;
    uint64_t generation;
    size_t surface_bytes;
    size_t event_count;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t ui_slot;
    uint64_t presented_pixels;
    uint64_t present_calls;
    bool allocated;
    bool visible;
    bool window_object_open;
    bool event_object_open;
    bool overflow_pending;
};

struct native_process {
    struct native_manifest manifest;
    struct native_validated_image image;
    struct paging_process_space address_space;
    struct paging_process_alias_set aliases;
    struct native_handle_table handles;
    struct native_thread threads[NATIVE_THREAD_LIMIT];
    struct native_page pages[NATIVE_PROCESS_PAGE_LIMIT];
    struct native_directory_resource directories[NATIVE_HANDLE_LIMIT];
    struct native_window_state window;
    uint8_t console_input[NATIVE_CONSOLE_INPUT_CAPACITY];
    uint64_t executable_frames[PAGING_PROCESS_ALIAS_MAX_PAGES];
    uint8_t transfer[NATIVE_COPY_CHUNK];
    uint64_t generation;
    size_t page_count;
    size_t executable_count;
    size_t thread_count;
    size_t current_thread;
    size_t console_input_head;
    size_t console_input_count;
    size_t peak_pages;
    size_t peak_handles;
    uint32_t syscall_count;
    uint32_t thread_switches;
    uint64_t context_cycles_without_fpu;
    uint64_t context_cycles_with_fpu;
    uint32_t context_transition_samples;
    int32_t exit_status;
    bool active;
    bool exiting;
    bool faulted;
};

static struct native_process processes[NATIVE_PROCESS_LIMIT];
static struct paging_process_expected_page
    validation_pages[NATIVE_PROCESS_PAGE_LIMIT];
static size_t current_process = SIZE_MAX;
static uint64_t next_process_generation = UINT64_C(1);
static uint64_t next_thread_generation = UINT64_C(1);
static uint64_t next_window_generation = UINT64_C(1);
static struct interrupt_process_gate native_gate;
static bool scheduler_active;
static size_t scheduler_process_cursor;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static size_t bounded_length(const uint8_t *text, size_t capacity)
{
    for (size_t index = 0U; index < capacity; ++index) {
        if (text[index] == 0U) {
            return index;
        }
    }
    return capacity;
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (size_t index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (result == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static void record_context_transition(
    struct native_process *process,
    uint64_t without_fpu,
    uint64_t fpu
)
{
    uint64_t with_fpu;

    if (process == NULL || !add_u64(without_fpu, fpu, &with_fpu)) {
        return;
    }
    if (!add_u64(process->context_cycles_without_fpu, without_fpu,
            &process->context_cycles_without_fpu)) {
        process->context_cycles_without_fpu = UINT64_MAX;
    }
    if (!add_u64(process->context_cycles_with_fpu, with_fpu,
            &process->context_cycles_with_fpu)) {
        process->context_cycles_with_fpu = UINT64_MAX;
    }
    if (process->context_transition_samples != UINT32_MAX) {
        ++process->context_transition_samples;
    }
}

static bool canonical_user_range(uint64_t address, size_t length)
{
    uint64_t end;

    return address != 0U && length != 0U &&
        add_u64(address, (uint64_t)length - 1U, &end) &&
        address <= UINT64_C(0x00007FFFFFFFFFFF) &&
        end <= UINT64_C(0x00007FFFFFFFFFFF);
}

static struct native_process *running_process(void)
{
    if (!scheduler_active || current_process >= NATIVE_PROCESS_LIMIT ||
        !processes[current_process].active) {
        return NULL;
    }
    return &processes[current_process];
}

static struct native_thread *running_thread(struct native_process *process)
{
    if (process == NULL || process->current_thread >= process->thread_count ||
        process->threads[process->current_thread].state ==
            NATIVE_THREAD_UNUSED) {
        return NULL;
    }
    return &process->threads[process->current_thread];
}

static size_t page_lower_bound(
    const struct native_process *process,
    uint64_t virtual_address
)
{
    size_t low = 0U;
    size_t high = process->page_count;

    while (low < high) {
        const size_t middle = low + (high - low) / 2U;

        if (process->pages[middle].virtual_address < virtual_address) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

static struct native_page *page_at(
    struct native_process *process,
    uint64_t virtual_address
)
{
    const uint64_t page_address = virtual_address & ~(PAGING_PAGE_SIZE - 1U);
    const size_t index = page_lower_bound(process, page_address);

    if (index >= process->page_count ||
        process->pages[index].virtual_address != page_address) {
        return NULL;
    }
    return &process->pages[index];
}

static bool record_page(
    struct native_process *process,
    const struct native_page *page
)
{
    size_t index;

    if (process == NULL || page == NULL ||
        process->page_count >= NATIVE_PROCESS_PAGE_LIMIT) {
        return false;
    }
    index = page_lower_bound(process, page->virtual_address);
    if (index < process->page_count &&
        process->pages[index].virtual_address == page->virtual_address) {
        return false;
    }
    for (size_t move = process->page_count; move > index; --move) {
        process->pages[move] = process->pages[move - 1U];
    }
    process->pages[index] = *page;
    ++process->page_count;
    if (process->page_count > process->peak_pages) {
        process->peak_pages = process->page_count;
    }
    return true;
}

static bool remove_page_record(
    struct native_process *process,
    uint64_t virtual_address,
    struct native_page *removed
)
{
    const size_t index = page_lower_bound(process, virtual_address);

    if (index >= process->page_count ||
        process->pages[index].virtual_address != virtual_address) {
        return false;
    }
    if (removed != NULL) {
        *removed = process->pages[index];
    }
    for (size_t move = index + 1U; move < process->page_count; ++move) {
        process->pages[move - 1U] = process->pages[move];
    }
    --process->page_count;
    zero_bytes(&process->pages[process->page_count],
        sizeof(process->pages[process->page_count]));
    return true;
}

static bool validate_user_range(
    struct native_process *process,
    uint64_t address,
    size_t length,
    bool output
)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (process == NULL || !canonical_user_range(address, length)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (page == NULL || (output &&
                (page->permissions != PAGING_WRITE ||
                    page->kind == PAGING_PROCESS_MAPPING_NATIVE_IMAGE)) ||
            paging_process_translate(&process->address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.level != 1U ||
            translation.permissions != page->permissions ||
            (translation.physical_address & ~(PAGING_PAGE_SIZE - 1U)) !=
                page->physical_address ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool validate_futex_word(
    struct native_process *process,
    uint64_t address
)
{
    const struct native_page *page = page_at(process, address);

    return (address & (sizeof(uint32_t) - 1U)) == 0U &&
        page != NULL && page->permissions == PAGING_WRITE &&
        validate_user_range(process, address, sizeof(uint32_t), false);
}

static bool copy_from_user(
    struct native_process *process,
    void *destination,
    uint64_t address,
    size_t length
)
{
    uint8_t *output = destination;
    uint64_t cursor = address;
    size_t remaining = length;

    if (destination == NULL ||
        !validate_user_range(process, address, length, false)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(&process->address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        copy_bytes(output, (const void *)(uintptr_t)translation.physical_address,
            chunk);
        output += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_to_user(
    struct native_process *process,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;
    size_t remaining = length;

    if (source == NULL || !validate_user_range(process, address, length, true)) {
        return false;
    }
    while (remaining != 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(&process->address_space, cursor,
                &translation) != PAGING_STATUS_OK) {
            return false;
        }
        copy_bytes((void *)(uintptr_t)translation.physical_address, input,
            chunk);
        input += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static int64_t handle_error(enum native_handle_status status)
{
    switch (status) {
    case NATIVE_HANDLE_OK:
        return 0;
    case NATIVE_HANDLE_FULL:
        return -SAPOTE_ENOMEM;
    case NATIVE_HANDLE_WRONG_TYPE:
        return -SAPOTE_EBADF;
    case NATIVE_HANDLE_STALE:
        return -SAPOTE_ESTALE;
    case NATIVE_HANDLE_CLOSE_FAILED:
        return -SAPOTE_EIO;
    case NATIVE_HANDLE_NULL_ARGUMENT:
    case NATIVE_HANDLE_BAD_LIMIT:
    case NATIVE_HANDLE_BAD_TYPE:
    case NATIVE_HANDLE_STATUS_COUNT:
    default:
        return -SAPOTE_EINVAL;
    }
}

static int64_t filesystem_error(enum sapfs_status status)
{
    switch (status) {
    case SAPFS_STATUS_OK:
        return 0;
    case SAPFS_STATUS_NOT_FOUND:
        return -SAPOTE_ENOENT;
    case SAPFS_STATUS_EXISTS:
        return -SAPOTE_EEXIST;
    case SAPFS_STATUS_READ_ONLY:
        return -SAPOTE_EROFS;
    case SAPFS_STATUS_ACCESS:
        return -SAPOTE_EACCES;
    case SAPFS_STATUS_NOT_DIRECTORY:
        return -SAPOTE_ENOTDIR;
    case SAPFS_STATUS_IS_DIRECTORY:
        return -SAPOTE_EISDIR;
    case SAPFS_STATUS_NOT_EMPTY:
        return -SAPOTE_ENOTEMPTY;
    case SAPFS_STATUS_BUSY:
        return -SAPOTE_EBUSY;
    case SAPFS_STATUS_NO_HANDLES:
        return -SAPOTE_ENOMEM;
    case SAPFS_STATUS_STALE_HANDLE:
        return -SAPOTE_ESTALE;
    case SAPFS_STATUS_FULL:
    case SAPFS_STATUS_DIRECTORY_FULL:
        return -SAPOTE_ENOSPC;
    case SAPFS_STATUS_NAME:
        return -SAPOTE_ENAMETOOLONG;
    case SAPFS_STATUS_PATH:
    case SAPFS_STATUS_INVALID_ARGUMENT:
    case SAPFS_STATUS_RANGE:
        return -SAPOTE_EINVAL;
    case SAPFS_STATUS_ABSENT:
    case SAPFS_STATUS_NOT_MOUNTED:
        return -SAPOTE_ENOENT;
    case SAPFS_STATUS_CORRUPT:
    case SAPFS_STATUS_IO:
    case SAPFS_STATUS_WRITEBACK:
    case SAPFS_STATUS_RESET:
    case SAPFS_STATUS_ALREADY_MOUNTED:
    case SAPFS_STATUS_COUNT:
    default:
        return -SAPOTE_EIO;
    }
}

static void window_finalize_if_unreferenced(struct native_process *process)
{
    if (process != NULL && process->window.allocated &&
        !process->window.window_object_open &&
        !process->window.event_object_open) {
        zero_bytes(&process->window, sizeof(process->window));
    }
}

static bool window_release_surface(struct native_process *process)
{
    struct native_window_state *window;
    const bool enabled = cpu_interrupts_enabled();
    bool success = true;
    size_t page_count;

    if (process == NULL || !process->window.allocated) {
        return false;
    }
    window = &process->window;
    if (enabled) {
        cpu_interrupt_disable();
    }
    if (ui_native_window_is_open(window->ui_slot) &&
        ui_native_window_close(window->ui_slot) != UI_STATUS_OK) {
        success = false;
    }
    page_count = (window->surface_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
    for (size_t page = 0U; page < page_count; ++page) {
        struct native_page removed;
        const uint64_t address = window->surface_address +
            page * PAGING_PAGE_SIZE;
        struct native_page *record = page_at(process, address);

        if (record == NULL ||
            record->kind != PAGING_PROCESS_MAPPING_NATIVE_SURFACE) {
            success = false;
            continue;
        }
        if (record->mapped &&
            paging_process_unmap_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_SURFACE, address) !=
                    PAGING_STATUS_OK) {
            success = false;
            continue;
        }
        if (!remove_page_record(process, address, &removed) ||
            frame_release(removed.physical_address) != FRAME_STATUS_OK) {
            success = false;
        }
    }
    if (window->shadow_pixels != NULL &&
        heap_free(window->shadow_pixels) != HEAP_STATUS_OK) {
        success = false;
    }
    window->shadow_pixels = NULL;
    window->surface_address = 0U;
    window->surface_bytes = 0U;
    window->visible = false;
    window->window_object_open = false;
    if (enabled) {
        cpu_interrupt_enable();
    }
    window_finalize_if_unreferenced(process);
    return success;
}

static bool close_resource(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    struct native_process *process = context;

    if (resource == NULL || process == NULL) {
        return false;
    }
    switch (type) {
    case SAPOTE_HANDLE_FILE:
        return sapfs_close((sapfs_handle)resource->words[0]) == SAPFS_STATUS_OK;
    case SAPOTE_HANDLE_DIRECTORY:
        if (resource->words[0] >= NATIVE_HANDLE_LIMIT) {
            return false;
        }
        if (sapfs_directory_close(
                process->directories[resource->words[0]].iterator) !=
                SAPFS_STATUS_OK) {
            return false;
        }
        zero_bytes(&process->directories[resource->words[0]],
            sizeof(process->directories[resource->words[0]]));
        return true;
    case SAPOTE_HANDLE_STREAM:
    case SAPOTE_HANDLE_DATAGRAM:
        return network_close(process->generation,
            (network_handle)resource->words[0]) == NETWORK_STATUS_OK;
    case SAPOTE_HANDLE_TIMER:
        return true;
    case SAPOTE_HANDLE_WINDOW:
        if (!process->window.allocated ||
            process->window.generation != resource->words[1] ||
            process->window.ui_slot != resource->words[0]) {
            return false;
        }
        return window_release_surface(process);
    case SAPOTE_HANDLE_EVENT_QUEUE:
        if (!process->window.allocated ||
            process->window.generation != resource->words[1]) {
            return false;
        }
        process->window.event_object_open = false;
        process->window.event_count = 0U;
        process->window.overflow_pending = false;
        window_finalize_if_unreferenced(process);
        return true;
    case SAPOTE_HANDLE_THREAD:
        return true;
    default:
        return false;
    }
}

static bool safe_relative_path(const char *path, size_t length)
{
    size_t component_start = 0U;

    if (path == NULL || length == 0U || length >= SAPFS_MAX_PATH ||
        path[0] == '/' || path[0] == '\\') {
        return false;
    }
    for (size_t index = 0U; index <= length; ++index) {
        const bool boundary = index == length || path[index] == '/';

        if (index < length && ((uint8_t)path[index] > UINT8_C(0x7F) ||
                path[index] == '\\' || path[index] == ':' ||
                path[index] == '\0')) {
            return false;
        }
        if (!boundary) {
            continue;
        }
        const size_t component_length = index - component_start;

        if (component_length == 0U ||
            (component_length == 2U && path[component_start] == '.' &&
                path[component_start + 1U] == '.')) {
            return false;
        }
        if (component_length == 1U && path[component_start] == '.' &&
            length != 1U) {
            return false;
        }
        component_start = index + 1U;
    }
    return true;
}

static bool path_from_user(
    struct native_process *process,
    const struct sapote_path *path,
    char output[SAPFS_MAX_PATH],
    enum sapfs_volume *volume
)
{
    char relative[SAPFS_MAX_PATH];
    size_t namespace_length;

    if (process == NULL || path == NULL || output == NULL || volume == NULL ||
        path->reserved != 0U || path->length == 0U ||
        path->length >= sizeof(relative) ||
        !copy_from_user(process, relative, path->address, path->length) ||
        !safe_relative_path(relative, path->length)) {
        return false;
    }
    relative[path->length] = '\0';
    zero_bytes(output, SAPFS_MAX_PATH);
    if (path->volume == SAPOTE_VOLUME_SYSTEM) {
        size_t resource_length;

        if ((process->manifest.capabilities & SAPOTE_CAP_SYSTEM_READ) == 0U) {
            return false;
        }
        resource_length = bounded_length(process->manifest.resource_directory,
            sizeof(process->manifest.resource_directory));
        if (resource_length == 0U) {
            copy_bytes(output, relative, path->length + 1U);
        } else if (path->length == 1U && relative[0] == '.') {
            copy_bytes(output, process->manifest.resource_directory,
                resource_length + 1U);
        } else {
            if (resource_length + 1U + path->length >= SAPFS_MAX_PATH) {
                return false;
            }
            copy_bytes(output, process->manifest.resource_directory,
                resource_length);
            output[resource_length] = '/';
            copy_bytes(output + resource_length + 1U, relative,
                path->length + 1U);
        }
        *volume = SAPFS_VOLUME_SYSTEM;
        return true;
    }
    if (path->volume != SAPOTE_VOLUME_DATA ||
        (process->manifest.capabilities &
            (SAPOTE_CAP_DATA_READ | SAPOTE_CAP_DATA_WRITE)) == 0U) {
        return false;
    }
    namespace_length = bounded_length(process->manifest.data_namespace,
        sizeof(process->manifest.data_namespace));
    if (namespace_length == 0U ||
        namespace_length + 1U + path->length >= SAPFS_MAX_PATH) {
        return false;
    }
    copy_bytes(output, process->manifest.data_namespace, namespace_length);
    if (path->length == 1U && relative[0] == '.') {
        output[namespace_length] = '\0';
    } else {
        output[namespace_length] = '/';
        copy_bytes(output + namespace_length + 1U, relative,
            path->length + 1U);
    }
    *volume = SAPFS_VOLUME_DATA;
    return true;
}

static bool read_system_file(
    const char *path,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    sapfs_handle handle;
    size_t total = 0U;

    if (path == NULL || destination == NULL || read_bytes == NULL ||
        sapfs_open(SAPFS_VOLUME_SYSTEM, path, SAPFS_ACCESS_READ, &handle) !=
            SAPFS_STATUS_OK) {
        return false;
    }
    while (total < capacity) {
        size_t completed = 0U;
        enum sapfs_status status = sapfs_read(handle, destination + total,
            capacity - total, &completed);

        if (status != SAPFS_STATUS_OK) {
            (void)sapfs_close(handle);
            return false;
        }
        total += completed;
        if (completed == 0U) {
            break;
        }
    }
    if (sapfs_close(handle) != SAPFS_STATUS_OK) {
        return false;
    }
    *read_bytes = total;
    return true;
}

static bool allocate_page(
    struct native_process *process,
    uint64_t virtual_address,
    uint32_t permissions,
    enum paging_process_mapping_kind kind,
    uintptr_t *physical_address
)
{
    struct native_page page;

    if (process == NULL || physical_address == NULL ||
        ((process->page_count + 1U) * PAGING_PAGE_SIZE) >
            process->manifest.memory_limit ||
        frame_allocate(physical_address) != FRAME_STATUS_OK) {
        return false;
    }
    zero_bytes((void *)*physical_address, PAGING_PAGE_SIZE);
    page.virtual_address = virtual_address;
    page.physical_address = *physical_address;
    page.permissions = permissions;
    page.kind = kind;
    page.mapped = false;
    if (!record_page(process, &page)) {
        (void)frame_release(*physical_address);
        *physical_address = 0U;
        return false;
    }
    return true;
}

static bool prepare_image_pages(
    struct native_process *process,
    const uint8_t *elf,
    size_t elf_length
)
{
    for (size_t segment_index = 0U;
         segment_index < process->image.segment_count; ++segment_index) {
        const struct native_elf_segment *segment =
            &process->image.segments[segment_index];
        const uint32_t permissions = (segment->flags & ELF_PF_X) != 0U ?
            PAGING_EXECUTE : ((segment->flags & ELF_PF_W) != 0U ?
                PAGING_WRITE : PAGING_READ);

        for (uint64_t virtual_address = segment->mapping_start;
             virtual_address < segment->mapping_end;
             virtual_address += PAGING_PAGE_SIZE) {
            uintptr_t physical_address;
            const uint64_t file_virtual_end = segment->virtual_address +
                segment->file_size;
            const uint64_t copy_start = virtual_address >
                segment->virtual_address ? virtual_address :
                segment->virtual_address;
            const uint64_t page_end = virtual_address + PAGING_PAGE_SIZE;
            const uint64_t copy_end = page_end < file_virtual_end ?
                page_end : file_virtual_end;

            if (!allocate_page(process, virtual_address, permissions,
                    PAGING_PROCESS_MAPPING_NATIVE_IMAGE, &physical_address)) {
                return false;
            }
            if (copy_start < copy_end) {
                const uint64_t source_offset = segment->file_offset +
                    (copy_start - segment->virtual_address);
                const size_t count = (size_t)(copy_end - copy_start);

                if (source_offset > elf_length ||
                    count > elf_length - (size_t)source_offset) {
                    return false;
                }
                copy_bytes((uint8_t *)(void *)physical_address +
                        (size_t)(copy_start - virtual_address),
                    elf + source_offset, count);
            }
            if ((permissions & PAGING_EXECUTE) != 0U) {
                if (process->executable_count >=
                        PAGING_PROCESS_ALIAS_MAX_PAGES) {
                    return false;
                }
                process->executable_frames[process->executable_count++] =
                    physical_address;
            }
        }
    }
    return process->executable_count != 0U;
}

static bool prepare_main_stack(struct native_process *process)
{
    for (size_t page = 0U; page < NATIVE_STACK_PAGES; ++page) {
        uintptr_t physical_address;

        if (!allocate_page(process,
                NATIVE_MAIN_STACK_BASE + page * PAGING_PAGE_SIZE,
                PAGING_WRITE, PAGING_PROCESS_MAPPING_NATIVE_STACK,
                &physical_address)) {
            return false;
        }
    }
    return true;
}

static bool prepared_tls_write(
    struct native_process *process,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;

    while (length != 0U) {
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (page == NULL ||
            page->kind != PAGING_PROCESS_MAPPING_NATIVE_TLS) {
            return false;
        }
        if (chunk > length) {
            chunk = length;
        }
        copy_bytes((uint8_t *)(void *)page->physical_address +
                (size_t)(cursor - page->virtual_address), input, chunk);
        cursor += chunk;
        input += chunk;
        length -= chunk;
    }
    return true;
}

/*
 * x86-64 uses ELF TLS variant II: FS names the byte immediately after the
 * initial-exec block and local-exec offsets are negative.  A kernel-owned
 * mapping kind keeps the allocator and memory-unmap service from accidentally
 * handing out or removing the initial thread's template.
 */
static bool prepare_main_tls(
    struct native_process *process,
    const uint8_t *elf,
    size_t elf_length,
    uint64_t *thread_pointer
)
{
    const struct native_elf_tls *tls = &process->image.tls;
    uint64_t aligned_size;
    uint64_t allocation_size;
    size_t page_count;

    if (thread_pointer == NULL) {
        return false;
    }
    *thread_pointer = 0U;
    if (tls->memory_size == 0U) {
        return true;
    }
    if (tls->alignment == 0U || tls->memory_size > UINT64_MAX -
            (tls->alignment - 1U) || tls->file_offset > elf_length ||
        tls->file_size > elf_length - (size_t)tls->file_offset) {
        return false;
    }
    aligned_size = (tls->memory_size + tls->alignment - 1U) &
        ~(tls->alignment - 1U);
    if (!add_u64(aligned_size, NATIVE_TLS_TCB_BYTES, &allocation_size) ||
        allocation_size > UINT64_MAX - (PAGING_PAGE_SIZE - 1U)) {
        return false;
    }
    page_count = (size_t)((allocation_size + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE);
    if (page_count == 0U || page_count > NATIVE_PROCESS_PAGE_LIMIT ||
        page_count >= (PAGING_NATIVE_ANON_END - NATIVE_MAIN_TLS_BASE) /
            PAGING_PAGE_SIZE) {
        return false;
    }
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;

        if (!allocate_page(process,
                NATIVE_MAIN_TLS_BASE + page * PAGING_PAGE_SIZE,
                PAGING_WRITE, PAGING_PROCESS_MAPPING_NATIVE_TLS,
                &physical_address)) {
            return false;
        }
    }
    *thread_pointer = NATIVE_MAIN_TLS_BASE + aligned_size;
    return (tls->file_size == 0U || prepared_tls_write(process,
            *thread_pointer - tls->memory_size,
            elf + (size_t)tls->file_offset, (size_t)tls->file_size)) &&
        prepared_tls_write(process, *thread_pointer, thread_pointer,
            sizeof(*thread_pointer));
}

static bool map_prepared_pages(struct native_process *process)
{
    if (paging_process_space_build(&process->address_space) !=
            PAGING_STATUS_OK ||
        paging_process_alias_set_narrow(&process->address_space,
            process->executable_frames, process->executable_count,
            &process->aliases) != PAGING_STATUS_OK) {
        return false;
    }
    for (size_t index = 0U; index < process->page_count; ++index) {
        const struct native_page *page = &process->pages[index];

        if (paging_process_map_user_page(&process->address_space, page->kind,
                page->virtual_address, page->physical_address,
                page->permissions) != PAGING_STATUS_OK) {
            return false;
        }
        process->pages[index].mapped = true;
        validation_pages[index].virtual_address = page->virtual_address;
        validation_pages[index].physical_address = page->physical_address;
        validation_pages[index].permissions = page->permissions;
    }
    return paging_process_validate_native(&process->address_space,
        validation_pages, process->page_count) == PAGING_STATUS_OK;
}

static bool stack_write(
    struct native_process *process,
    uint64_t address,
    const void *source,
    size_t length
)
{
    const uint8_t *input = source;
    uint64_t cursor = address;
    size_t remaining = length;

    if (address < NATIVE_MAIN_STACK_BASE ||
        length > NATIVE_MAIN_STACK_END - address) {
        return false;
    }
    while (remaining != 0U) {
        struct native_page *page = page_at(process, cursor);
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (page == NULL || page->kind != PAGING_PROCESS_MAPPING_NATIVE_STACK) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        copy_bytes((uint8_t *)(void *)page->physical_address +
                (size_t)(cursor - page->virtual_address), input, chunk);
        cursor += chunk;
        input += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool initialize_stack(
    struct native_process *process,
    struct native_thread *thread
)
{
    uint64_t argument_addresses[NATIVE_MANIFEST_ARGUMENTS + 1U];
    uint64_t environment_addresses[3];
    uint64_t cursor = NATIVE_MAIN_STACK_END;
    uint64_t vector[40];
    size_t vector_count = 0U;
    const size_t executable_length = bounded_length(
        process->manifest.executable, sizeof(process->manifest.executable));
    const size_t identifier_length = bounded_length(
        process->manifest.identifier, sizeof(process->manifest.identifier));
    const size_t namespace_length = bounded_length(
        process->manifest.data_namespace,
        sizeof(process->manifest.data_namespace));
    char environment[3][48];
    size_t environment_lengths[3];
    const size_t argc = (size_t)process->manifest.argument_count + 1U;

    zero_bytes(environment, sizeof(environment));
    copy_bytes(environment[0], "SAPOTE_ABI=1", 13U);
    copy_bytes(environment[1], "SAPOTE_APP_ID=", 14U);
    copy_bytes(environment[1] + 14U, process->manifest.identifier,
        identifier_length);
    copy_bytes(environment[2], "SAPOTE_DATA=", 12U);
    copy_bytes(environment[2] + 12U, process->manifest.data_namespace,
        namespace_length);
    environment_lengths[0] = 13U;
    environment_lengths[1] = 14U + identifier_length + 1U;
    environment_lengths[2] = 12U + namespace_length + 1U;

    for (size_t reverse = 3U; reverse > 0U; --reverse) {
        const size_t index = reverse - 1U;

        cursor -= environment_lengths[index];
        if (!stack_write(process, cursor, environment[index],
                environment_lengths[index])) {
            return false;
        }
        environment_addresses[index] = cursor;
    }
    for (size_t reverse = process->manifest.argument_count;
         reverse > 0U; --reverse) {
        const size_t index = reverse - 1U;
        const size_t length = bounded_length(process->manifest.arguments[index],
            NATIVE_MANIFEST_ARGUMENT_BYTES) + 1U;

        cursor -= length;
        if (!stack_write(process, cursor,
                process->manifest.arguments[index], length)) {
            return false;
        }
        argument_addresses[index + 1U] = cursor;
    }
    cursor -= executable_length + 1U;
    if (!stack_write(process, cursor, process->manifest.executable,
            executable_length + 1U)) {
        return false;
    }
    argument_addresses[0] = cursor;

    vector[vector_count++] = argc;
    for (size_t index = 0U; index < argc; ++index) {
        vector[vector_count++] = argument_addresses[index];
    }
    vector[vector_count++] = 0U;
    for (size_t index = 0U; index < 3U; ++index) {
        vector[vector_count++] = environment_addresses[index];
    }
    vector[vector_count++] = 0U;
    vector[vector_count++] = NATIVE_AUX_PAGESZ;
    vector[vector_count++] = PAGING_PAGE_SIZE;
    vector[vector_count++] = NATIVE_AUX_ENTRY;
    vector[vector_count++] = process->image.entry;
    vector[vector_count++] = NATIVE_AUX_SAPOTE_ABI;
    vector[vector_count++] = SAPOTE_ABI_VERSION;
    vector[vector_count++] = NATIVE_AUX_TLS_IMAGE;
    vector[vector_count++] = process->image.tls.virtual_address;
    vector[vector_count++] = NATIVE_AUX_TLS_SIZE;
    vector[vector_count++] = process->image.tls.memory_size;
    vector[vector_count++] = NATIVE_AUX_TLS_ALIGN;
    vector[vector_count++] = process->image.tls.alignment;
    vector[vector_count++] = NATIVE_AUX_NULL;
    vector[vector_count++] = 0U;
    cursor = (cursor - vector_count * sizeof(uint64_t)) & ~UINT64_C(0xF);
    if (!stack_write(process, cursor, vector,
            vector_count * sizeof(uint64_t))) {
        return false;
    }
    zero_bytes(&thread->context, sizeof(thread->context));
    thread->context.rip = process->image.entry;
    thread->context.rsp = cursor;
    thread->context.rflags = NATIVE_RFLAGS;
    thread->context.rdi = argc;
    thread->context.rsi = cursor + sizeof(uint64_t);
    thread->context.rdx = cursor + (2U + argc) * sizeof(uint64_t);
    return true;
}

static bool process_cleanup(struct native_process *process)
{
    bool success = true;
    const bool interrupts_were_enabled = cpu_interrupts_enabled();

    if (process == NULL) {
        return false;
    }
    cpu_interrupt_disable();
    if (process->address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(&process->address_space) !=
            PAGING_STATUS_OK) {
        success = false;
    }
    cpu_interrupt_enable();
    if (process->handles.initialized &&
        native_handle_close_all(&process->handles, close_resource, process) !=
            NATIVE_HANDLE_OK) {
        success = false;
    }
    network_process_terminated(process->generation);
    cpu_interrupt_disable();
    for (size_t remaining = process->page_count; remaining > 0U; --remaining) {
        const struct native_page *page = &process->pages[remaining - 1U];

        if (page->mapped &&
            process->address_space.state != PAGING_PROCESS_SPACE_INVALID &&
            process->address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
            paging_process_unmap_user_page(&process->address_space, page->kind,
                page->virtual_address) != PAGING_STATUS_OK) {
            success = false;
        }
    }
    if (process->aliases.active &&
        paging_process_alias_set_restore(&process->address_space,
            &process->aliases) != PAGING_STATUS_OK) {
        success = false;
    }
    if (process->address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        process->address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
        paging_process_space_release(&process->address_space) !=
            PAGING_STATUS_OK) {
        success = false;
    }
    for (size_t remaining = process->page_count; remaining > 0U; --remaining) {
        if (frame_release(process->pages[remaining - 1U].physical_address) !=
                FRAME_STATUS_OK) {
            success = false;
        }
    }
    zero_bytes(process, sizeof(*process));
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return success;
}

static enum native_process_status load_process(
    struct native_process *process,
    const char *manifest_path
)
{
    uint8_t manifest_bytes[NATIVE_MANIFEST_BYTES];
    struct sapfs_stat executable_stat;
    uint8_t *elf = NULL;
    char executable[SAPFS_MAX_PATH];
    char data_namespace[SAPFS_MAX_PATH];
    size_t manifest_read = 0U;
    size_t elf_read = 0U;
    size_t executable_length;
    size_t namespace_length;
    uint64_t main_thread_pointer = 0U;
    enum native_process_status result = NATIVE_PROCESS_OK;
    enum native_image_status admission_status;

    if (!read_system_file(manifest_path, manifest_bytes,
            sizeof(manifest_bytes), &manifest_read) ||
        manifest_read != sizeof(manifest_bytes)) {
        return NATIVE_PROCESS_MANIFEST_READ;
    }
    /* Parse the manifest alone only after the executable has been read below. */
    if (manifest_bytes[0] != 'S' || manifest_bytes[1] != 'A') {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    executable_length = bounded_length(manifest_bytes + 112U, 16U);
    if (executable_length == 0U || executable_length >= 16U) {
        return NATIVE_PROCESS_IMAGE_REFUSED;
    }
    zero_bytes(executable, sizeof(executable));
    copy_bytes(executable, manifest_bytes + 112U, executable_length);
    if (sapfs_stat_path(SAPFS_VOLUME_SYSTEM, executable, &executable_stat) !=
            SAPFS_STATUS_OK || executable_stat.directory ||
        executable_stat.size == 0U ||
        executable_stat.size > NATIVE_ELF_MAX_FILE_BYTES) {
        return NATIVE_PROCESS_EXECUTABLE_OPEN;
    }
    cpu_interrupt_disable();
    if (heap_allocate(executable_stat.size, (void **)&elf) != HEAP_STATUS_OK) {
        cpu_interrupt_enable();
        return NATIVE_PROCESS_MEMORY_LIMIT;
    }
    cpu_interrupt_enable();
    if (!read_system_file(executable, elf, executable_stat.size, &elf_read) ||
        elf_read != executable_stat.size) {
        result = NATIVE_PROCESS_EXECUTABLE_READ;
        goto finish;
    }
    admission_status = sapote_native_image_validate(manifest_bytes,
        sizeof(manifest_bytes), elf, elf_read, &process->manifest,
        &process->image);
    if (admission_status != NATIVE_IMAGE_OK) {
        console_write("Sapote: native admission status ");
        console_write_u64((uint64_t)admission_status);
        console_putc('\n');
        result = NATIVE_PROCESS_IMAGE_REFUSED;
        goto finish;
    }
    namespace_length = bounded_length(process->manifest.data_namespace,
        sizeof(process->manifest.data_namespace));
    zero_bytes(data_namespace, sizeof(data_namespace));
    copy_bytes(data_namespace, process->manifest.data_namespace,
        namespace_length);
    {
        const enum sapfs_status mkdir_status = sapfs_mkdir(SAPFS_VOLUME_DATA,
            data_namespace);

        if (mkdir_status != SAPFS_STATUS_OK &&
            mkdir_status != SAPFS_STATUS_EXISTS) {
            result = NATIVE_PROCESS_DATA_NAMESPACE;
            goto finish;
        }
    }
    cpu_interrupt_disable();
    if (!prepare_image_pages(process, elf, elf_read) ||
        !prepare_main_tls(process, elf, elf_read, &main_thread_pointer) ||
        !prepare_main_stack(process)) {
        result = NATIVE_PROCESS_FRAME_ALLOCATION;
        goto finish_disabled;
    }
    if (!map_prepared_pages(process)) {
        result = NATIVE_PROCESS_MAPPING;
        goto finish_disabled;
    }
    process->thread_count = 1U;
    process->current_thread = 0U;
    process->threads[0].generation = next_thread_generation++;
    if (next_thread_generation == 0U) {
        next_thread_generation = 1U;
    }
    process->threads[0].state = NATIVE_THREAD_RUNNABLE;
    process->threads[0].stack_base = NATIVE_MAIN_STACK_BASE;
    process->threads[0].stack_end = NATIVE_MAIN_STACK_END;
    process->threads[0].fs_base = main_thread_pointer;
    if (!native_fpu_state_initialize(&process->threads[0].fpu) ||
        !initialize_stack(process, &process->threads[0])) {
        result = NATIVE_PROCESS_STACK;
        goto finish_disabled;
    }
    if (native_handle_table_initialize(&process->handles,
            process->manifest.max_handles) != NATIVE_HANDLE_OK) {
        result = NATIVE_PROCESS_MEMORY_LIMIT;
        goto finish_disabled;
    }
    process->active = true;
finish_disabled:
    cpu_interrupt_enable();
finish:
    cpu_interrupt_disable();
    if (elf != NULL && heap_free(elf) != HEAP_STATUS_OK &&
        result == NATIVE_PROCESS_OK) {
        result = NATIVE_PROCESS_TEARDOWN;
    }
    cpu_interrupt_enable();
    return result;
}

enum native_process_status native_process_spawn(
    const char *manifest_path,
    uint64_t *generation
)
{
    struct native_process *process = NULL;
    enum native_process_status status;

    if (manifest_path == NULL || generation == NULL) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    *generation = 0U;
    if (scheduler_active) {
        return NATIVE_PROCESS_BUSY;
    }
    if (!native_fpu_initialize()) {
        return NATIVE_PROCESS_CPU;
    }
    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        if (!processes[index].active && processes[index].generation == 0U) {
            process = &processes[index];
            break;
        }
    }
    if (process == NULL) {
        return NATIVE_PROCESS_NO_SLOT;
    }
    zero_bytes(process, sizeof(*process));
    process->generation = next_process_generation++;
    if (next_process_generation == 0U) {
        next_process_generation = 1U;
    }
    status = load_process(process, manifest_path);
    if (status != NATIVE_PROCESS_OK) {
        if (!process_cleanup(process)) {
            return NATIVE_PROCESS_TEARDOWN;
        }
        return status;
    }
    *generation = process->generation;
    return NATIVE_PROCESS_OK;
}

static int64_t syscall_console_write(
    struct native_process *process,
    uint64_t address,
    size_t length
)
{
    size_t completed = 0U;

    if ((process->manifest.capabilities & SAPOTE_CAP_CONSOLE) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (length == 0U) {
        return 0;
    }
    if (!validate_user_range(process, address, length, false)) {
        return -SAPOTE_EFAULT;
    }
    while (completed < length) {
        size_t chunk = length - completed;

        if (chunk > sizeof(process->transfer)) {
            chunk = sizeof(process->transfer);
        }
        if (!copy_from_user(process, process->transfer, address + completed,
                chunk)) {
            return completed == 0U ? -SAPOTE_EFAULT : (int64_t)completed;
        }
        console_write_n((const char *)process->transfer, chunk);
        completed += chunk;
    }
    return (int64_t)completed;
}

static size_t console_input_copy(
    const struct native_process *process,
    uint8_t *destination,
    size_t capacity
)
{
    size_t length = process->console_input_count;

    if (length > capacity) {
        length = capacity;
    }
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = process->console_input[
            (process->console_input_head + index) %
                NATIVE_CONSOLE_INPUT_CAPACITY];
    }
    return length;
}

static void console_input_consume(
    struct native_process *process,
    size_t length
)
{
    process->console_input_head = (process->console_input_head + length) %
        NATIVE_CONSOLE_INPUT_CAPACITY;
    process->console_input_count -= length;
}

static int64_t syscall_console_read(
    struct native_process *process,
    uint64_t address,
    size_t length
)
{
    struct native_thread *thread = running_thread(process);
    size_t copied;

    if ((process->manifest.capabilities & SAPOTE_CAP_CONSOLE) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (thread == NULL) {
        return -SAPOTE_EIO;
    }
    if (length == 0U) {
        return 0;
    }
    if (length > sizeof(process->transfer)) {
        return -SAPOTE_EINVAL;
    }
    if (!validate_user_range(process, address, length, true)) {
        return -SAPOTE_EFAULT;
    }
    if (process->console_input_count == 0U) {
        thread->console_address = address;
        thread->console_length = length;
        thread->state = NATIVE_THREAD_CONSOLE_WAIT;
        return 0;
    }
    copied = console_input_copy(process, process->transfer, length);
    if (!copy_to_user(process, address, process->transfer, copied)) {
        return -SAPOTE_EFAULT;
    }
    console_input_consume(process, copied);
    return (int64_t)copied;
}

static bool anonymous_span_free(
    struct native_process *process,
    uint64_t base,
    size_t page_count,
    bool guard_before,
    bool guard_after
)
{
    const uint64_t first = guard_before ? base - PAGING_PAGE_SIZE : base;
    const size_t total = page_count + (guard_before ? 1U : 0U) +
        (guard_after ? 1U : 0U);

    if (base < PAGING_NATIVE_ANON_BASE ||
        page_count > (PAGING_NATIVE_ANON_END - base) / PAGING_PAGE_SIZE ||
        first < PAGING_NATIVE_ANON_BASE ||
        total > (PAGING_NATIVE_ANON_END - first) / PAGING_PAGE_SIZE) {
        return false;
    }
    for (size_t page = 0U; page < total; ++page) {
        if (page_at(process, first + page * PAGING_PAGE_SIZE) != NULL) {
            return false;
        }
    }
    return true;
}

static int64_t syscall_memory_unmap(
    struct native_process *process,
    uint64_t address,
    uint64_t length
);

static int64_t syscall_memory_map(
    struct native_process *process,
    uint64_t request_address,
    uint64_t response_address
)
{
    struct sapote_memory_map_request request;
    struct sapote_memory_map_response response = {
        sizeof(response), SAPOTE_ABI_VERSION, 0U, 0U
    };
    size_t page_count;
    uint64_t length;
    uint64_t base;
    bool guard_before_flag;
    bool guard_after_flag;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request)) ||
        !validate_user_range(process, response_address, sizeof(response),
            true)) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.reserved != 0U ||
        (request.flags & ~SAPOTE_MEMORY_FLAGS_V1) != 0U ||
        (request.flags & (SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE)) !=
            (SAPOTE_MEMORY_READ | SAPOTE_MEMORY_WRITE) ||
        request.length == 0U || request.length > UINT64_MAX -
            (PAGING_PAGE_SIZE - 1U)) {
        return -SAPOTE_EINVAL;
    }
    guard_before_flag =
        (request.flags & SAPOTE_MEMORY_GUARD_BEFORE) != 0U;
    guard_after_flag =
        (request.flags & SAPOTE_MEMORY_GUARD_AFTER) != 0U;
    length = (request.length + PAGING_PAGE_SIZE - 1U) &
        ~(PAGING_PAGE_SIZE - 1U);
    page_count = (size_t)(length / PAGING_PAGE_SIZE);
    if (page_count == 0U || page_count > NATIVE_PROCESS_PAGE_LIMIT ||
        process->page_count > NATIVE_PROCESS_PAGE_LIMIT - page_count ||
        (process->page_count + page_count) * PAGING_PAGE_SIZE >
            process->manifest.memory_limit) {
        return -SAPOTE_ENOMEM;
    }
    if (request.address_hint != 0U) {
        if ((request.address_hint & (PAGING_PAGE_SIZE - 1U)) != 0U) {
            return -SAPOTE_EINVAL;
        }
        base = request.address_hint;
        if (!anonymous_span_free(process, base, page_count,
                guard_before_flag, guard_after_flag)) {
            return -SAPOTE_EBUSY;
        }
    } else {
        const size_t prefix = guard_before_flag ? 1U : 0U;
        const size_t total = page_count + prefix +
            (guard_after_flag ? 1U : 0U);
        uint64_t candidate = PAGING_NATIVE_ANON_BASE +
            prefix * PAGING_PAGE_SIZE;

        base = 0U;
        while (candidate < PAGING_NATIVE_ANON_END &&
            total <= (PAGING_NATIVE_ANON_END -
                (candidate - prefix * PAGING_PAGE_SIZE)) /
                    PAGING_PAGE_SIZE) {
            if (anonymous_span_free(process, candidate, page_count,
                    guard_before_flag, guard_after_flag)) {
                base = candidate;
                break;
            }
            candidate += PAGING_PAGE_SIZE;
        }
        if (base == 0U) {
            return -SAPOTE_ENOMEM;
        }
    }
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;
        const uint64_t address = base + page * PAGING_PAGE_SIZE;

        if (!allocate_page(process, address, PAGING_WRITE,
                PAGING_PROCESS_MAPPING_NATIVE_ANON, &physical_address) ||
            paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_ANON, address,
                physical_address, PAGING_WRITE) != PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback <= page; ++rollback) {
                struct native_page removed;
                const uint64_t rollback_address = base +
                    rollback * PAGING_PAGE_SIZE;
                struct native_page *record = page_at(process,
                    rollback_address);

                if (record == NULL) {
                    continue;
                }
                if (record->mapped) {
                    (void)paging_process_unmap_user_page(
                        &process->address_space,
                        PAGING_PROCESS_MAPPING_NATIVE_ANON,
                        rollback_address);
                }
                if (remove_page_record(process, rollback_address, &removed)) {
                    (void)frame_release(removed.physical_address);
                }
            }
            return -SAPOTE_ENOMEM;
        }
        page_at(process, address)->mapped = true;
    }
    response.address = base;
    response.length = length;
    if (!copy_to_user(process, response_address, &response, sizeof(response))) {
        (void)syscall_memory_unmap(process, base, length);
        return -SAPOTE_EFAULT;
    }
    return 0;
}

static int64_t syscall_memory_unmap(
    struct native_process *process,
    uint64_t address,
    uint64_t length
)
{
    size_t page_count;

    if (length == 0U || (address & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (length & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        address < PAGING_NATIVE_ANON_BASE ||
        length > PAGING_NATIVE_ANON_END - address) {
        return -SAPOTE_EINVAL;
    }
    page_count = (size_t)(length / PAGING_PAGE_SIZE);
    for (size_t page = 0U; page < page_count; ++page) {
        const struct native_page *record = page_at(process,
            address + page * PAGING_PAGE_SIZE);

        if (record == NULL || !record->mapped ||
            record->kind != PAGING_PROCESS_MAPPING_NATIVE_ANON) {
            return -SAPOTE_EFAULT;
        }
    }
    for (size_t page = 0U; page < page_count; ++page) {
        struct native_page removed;
        const uint64_t page_address = address + page * PAGING_PAGE_SIZE;

        if (paging_process_unmap_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_ANON, page_address) !=
                PAGING_STATUS_OK ||
            !remove_page_record(process, page_address, &removed) ||
            frame_release(removed.physical_address) != FRAME_STATUS_OK) {
            process->faulted = true;
            process->exiting = true;
            return -SAPOTE_EIO;
        }
    }
    return 0;
}

static int64_t syscall_file_open(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_file_open_request request;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    char path[SAPFS_MAX_PATH];
    enum sapfs_volume volume;
    enum sapfs_access access;
    sapfs_handle file;
    sapote_handle_t handle;
    enum sapfs_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.reserved != 0U ||
        (request.flags & ~SAPOTE_OPEN_FLAGS_V1) != 0U ||
        (request.flags & (SAPOTE_OPEN_READ | SAPOTE_OPEN_WRITE)) == 0U ||
        !path_from_user(process, &request.path, path, &volume)) {
        return -SAPOTE_EINVAL;
    }
    if (volume == SAPFS_VOLUME_SYSTEM &&
        (request.flags & (SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE |
            SAPOTE_OPEN_TRUNCATE)) != 0U) {
        return -SAPOTE_EACCES;
    }
    if ((request.flags & (SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE |
            SAPOTE_OPEN_TRUNCATE)) != 0U &&
        (process->manifest.capabilities & SAPOTE_CAP_DATA_WRITE) == 0U) {
        return -SAPOTE_EACCES;
    }
    access = (request.flags & SAPOTE_OPEN_WRITE) != 0U ?
        ((request.flags & SAPOTE_OPEN_READ) != 0U ? SAPFS_ACCESS_READ_WRITE :
            SAPFS_ACCESS_WRITE) : SAPFS_ACCESS_READ;
    cpu_interrupt_enable();
    status = sapfs_stat_path(volume, path, &(struct sapfs_stat){0});
    if (status == SAPFS_STATUS_NOT_FOUND &&
        (request.flags & SAPOTE_OPEN_CREATE) != 0U) {
        status = sapfs_create(volume, path);
    }
    if (status == SAPFS_STATUS_OK &&
        (request.flags & SAPOTE_OPEN_TRUNCATE) != 0U) {
        status = sapfs_truncate(volume, path, 0U);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(volume, path, access, &file);
    }
    cpu_interrupt_disable();
    if (status != SAPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    resource.words[0] = file;
    {
        const enum native_handle_status handle_status = native_handle_install(
            &process->handles, SAPOTE_HANDLE_FILE, &resource, &handle);

        if (handle_status != NATIVE_HANDLE_OK) {
            cpu_interrupt_enable();
            (void)sapfs_close(file);
            cpu_interrupt_disable();
            return handle_error(handle_status);
        }
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_file_io(
    struct native_process *process,
    uint64_t request_address,
    bool write
)
{
    struct sapote_io_request request;
    struct native_resource *resource;
    size_t completed = 0U;
    enum native_handle_status handle_status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U) {
        return -SAPOTE_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, request.handle,
        SAPOTE_HANDLE_FILE, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (request.length == 0U) {
        return 0;
    }
    if (!validate_user_range(process, request.buffer, request.length, !write)) {
        return -SAPOTE_EFAULT;
    }
    if (write && request.offset != UINT64_MAX) {
        uint64_t position;

        if (request.offset > INT64_MAX) {
            return -SAPOTE_EINVAL;
        }
        cpu_interrupt_enable();
        const enum sapfs_status seek_status = sapfs_seek(
            (sapfs_handle)resource->words[0], (int64_t)request.offset,
            SAPFS_SEEK_START, &position);
        cpu_interrupt_disable();
        if (seek_status != SAPFS_STATUS_OK) {
            return filesystem_error(seek_status);
        }
    }
    while (completed < request.length) {
        size_t chunk = request.length - completed;
        size_t transferred = 0U;
        enum sapfs_status status;

        if (chunk > sizeof(process->transfer)) {
            chunk = sizeof(process->transfer);
        }
        if (write && !copy_from_user(process, process->transfer,
                request.buffer + completed, chunk)) {
            return completed == 0U ? -SAPOTE_EFAULT : (int64_t)completed;
        }
        cpu_interrupt_enable();
        if (write) {
            status = sapfs_write((sapfs_handle)resource->words[0],
                process->transfer, chunk, &transferred);
        } else if (request.offset != UINT64_MAX) {
            if (completed > UINT64_MAX - request.offset) {
                cpu_interrupt_disable();
                return completed == 0U ? -SAPOTE_EINVAL : (int64_t)completed;
            }
            status = sapfs_pread((sapfs_handle)resource->words[0],
                process->transfer, chunk, request.offset + completed,
                &transferred);
        } else {
            status = sapfs_read((sapfs_handle)resource->words[0],
                process->transfer, chunk, &transferred);
        }
        cpu_interrupt_disable();
        if (status != SAPFS_STATUS_OK) {
            return completed == 0U ? filesystem_error(status) :
                (int64_t)completed;
        }
        if (!write && transferred != 0U &&
            !copy_to_user(process, request.buffer + completed,
                process->transfer, transferred)) {
            return completed == 0U ? -SAPOTE_EFAULT : (int64_t)completed;
        }
        completed += transferred;
        if (transferred < chunk) {
            break;
        }
    }
    return (int64_t)completed;
}

static int64_t syscall_file_seek(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_seek_request request;
    struct native_resource *resource;
    enum sapfs_seek_origin origin;
    uint64_t position;
    enum sapfs_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.reserved != 0U ||
        request.origin > SAPOTE_SEEK_END) {
        return -SAPOTE_EINVAL;
    }
    if (native_handle_resolve(&process->handles, request.handle,
            SAPOTE_HANDLE_FILE, &resource) != NATIVE_HANDLE_OK) {
        return -SAPOTE_EBADF;
    }
    origin = request.origin == SAPOTE_SEEK_START ? SAPFS_SEEK_START :
        (request.origin == SAPOTE_SEEK_CURRENT ? SAPFS_SEEK_CURRENT :
            SAPFS_SEEK_END);
    cpu_interrupt_enable();
    status = sapfs_seek((sapfs_handle)resource->words[0], request.offset,
        origin, &position);
    cpu_interrupt_disable();
    return status == SAPFS_STATUS_OK ? (int64_t)position :
        filesystem_error(status);
}

static int64_t syscall_path_stat(
    struct native_process *process,
    uint64_t path_address,
    uint64_t output_address
)
{
    struct sapote_path path_request;
    struct sapote_path_stat output = {
        sizeof(output), SAPOTE_ABI_VERSION, 0U, 0U, 0U
    };
    struct sapfs_stat stat;
    char path[SAPFS_MAX_PATH];
    enum sapfs_volume volume;
    enum sapfs_status status;

    if (!copy_from_user(process, &path_request, path_address,
            sizeof(path_request)) ||
        !validate_user_range(process, output_address, sizeof(output), true)) {
        return -SAPOTE_EFAULT;
    }
    if (!path_from_user(process, &path_request, path, &volume)) {
        return -SAPOTE_EINVAL;
    }
    cpu_interrupt_enable();
    status = sapfs_stat_path(volume, path, &stat);
    cpu_interrupt_disable();
    if (status != SAPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    output.byte_length = stat.size;
    output.attributes = (stat.directory ? SAPOTE_PATH_DIRECTORY : 0U) |
        (stat.read_only ? SAPOTE_PATH_READ_ONLY : 0U);
    return copy_to_user(process, output_address, &output, sizeof(output)) ?
        0 : -SAPOTE_EFAULT;
}

static int64_t syscall_directory_open(
    struct native_process *process,
    uint64_t path_address
)
{
    struct sapote_path path_request;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    char path[SAPFS_MAX_PATH];
    enum sapfs_volume volume;
    enum sapfs_status status;
    sapfs_directory_handle iterator = 0U;
    sapote_handle_t handle;
    size_t slot = SIZE_MAX;

    if (!copy_from_user(process, &path_request, path_address,
            sizeof(path_request))) {
        return -SAPOTE_EFAULT;
    }
    if (!path_from_user(process, &path_request, path, &volume)) {
        return -SAPOTE_EINVAL;
    }
    for (size_t index = 0U; index < NATIVE_HANDLE_LIMIT; ++index) {
        if (!process->directories[index].active) {
            slot = index;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        return -SAPOTE_ENOMEM;
    }
    cpu_interrupt_enable();
    status = sapfs_directory_open(volume, path, &iterator);
    cpu_interrupt_disable();
    if (status != SAPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    process->directories[slot].iterator = iterator;
    process->directories[slot].active = true;
    resource.words[0] = slot;
    {
        const enum native_handle_status handle_status = native_handle_install(
            &process->handles, SAPOTE_HANDLE_DIRECTORY, &resource, &handle);

        if (handle_status != NATIVE_HANDLE_OK) {
            (void)sapfs_directory_close(iterator);
            zero_bytes(&process->directories[slot],
                sizeof(process->directories[slot]));
            return handle_error(handle_status);
        }
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_directory_read(
    struct native_process *process,
    sapote_handle_t handle,
    uint64_t output_address
)
{
    struct native_resource *resource;
    struct sapfs_list_entry entry;
    struct sapote_directory_entry output;
    struct native_directory_resource *directory;
    bool present = false;
    enum sapfs_status status;

    if (!validate_user_range(process, output_address, sizeof(output), true)) {
        return -SAPOTE_EFAULT;
    }
    {
        const enum native_handle_status handle_status = native_handle_resolve(
            &process->handles, handle, SAPOTE_HANDLE_DIRECTORY, &resource);

        if (handle_status != NATIVE_HANDLE_OK) {
            return handle_error(handle_status);
        }
    }
    if (resource->words[0] >= NATIVE_HANDLE_LIMIT) {
        return -SAPOTE_EBADF;
    }
    directory = &process->directories[resource->words[0]];
    if (!directory->active) {
        return -SAPOTE_ESTALE;
    }
    cpu_interrupt_enable();
    status = sapfs_directory_read(directory->iterator, &entry, &present);
    cpu_interrupt_disable();
    if (status != SAPFS_STATUS_OK) {
        return filesystem_error(status);
    }
    if (!present) {
        return 0;
    }
    zero_bytes(&output, sizeof(output));
    output.size = sizeof(output);
    output.version = SAPOTE_ABI_VERSION;
    output.byte_length = entry.size;
    output.attributes = entry.directory ?
        SAPOTE_PATH_DIRECTORY : 0U;
    output.name_length = (uint16_t)bounded_length(
        (const uint8_t *)entry.name, sizeof(entry.name));
    if (output.name_length > sizeof(output.name)) {
        return -SAPOTE_EIO;
    }
    copy_bytes(output.name, entry.name, output.name_length);
    if (!copy_to_user(process, output_address, &output, sizeof(output))) {
        return -SAPOTE_EFAULT;
    }
    return 1;
}

static int64_t syscall_single_path_mutation(
    struct native_process *process,
    uint64_t path_address,
    uint64_t value,
    uint64_t number
)
{
    struct sapote_path path_request;
    struct sapfs_stat stat;
    char path[SAPFS_MAX_PATH];
    enum sapfs_volume volume;
    enum sapfs_status status;

    if (!copy_from_user(process, &path_request, path_address,
            sizeof(path_request))) {
        return -SAPOTE_EFAULT;
    }
    if (!path_from_user(process, &path_request, path, &volume)) {
        return -SAPOTE_EINVAL;
    }
    if (volume != SAPFS_VOLUME_DATA ||
        (process->manifest.capabilities & SAPOTE_CAP_DATA_WRITE) == 0U) {
        return -SAPOTE_EACCES;
    }
    cpu_interrupt_enable();
    if (number == SAPOTE_SYS_PATH_MKDIR) {
        status = sapfs_mkdir(volume, path);
    } else if (number == SAPOTE_SYS_PATH_TRUNCATE) {
        status = sapfs_truncate(volume, path, value);
    } else {
        status = sapfs_stat_path(volume, path, &stat);
        if (status == SAPFS_STATUS_OK) {
            status = stat.directory ? sapfs_rmdir(volume, path) :
                sapfs_unlink(volume, path);
        }
    }
    cpu_interrupt_disable();
    return filesystem_error(status);
}

static bool replacement_backup_path(
    const char *destination,
    char backup[SAPFS_MAX_PATH]
)
{
    size_t length = bounded_length((const uint8_t *)destination,
        SAPFS_MAX_PATH);
    size_t slash = SIZE_MAX;

    if (length == SAPFS_MAX_PATH) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (destination[index] == '/') {
            slash = index;
        }
    }
    zero_bytes(backup, SAPFS_MAX_PATH);
    if (slash != SIZE_MAX) {
        if (slash + 1U + 10U >= SAPFS_MAX_PATH) {
            return false;
        }
        copy_bytes(backup, destination, slash + 1U);
        copy_bytes(backup + slash + 1U, "SAPBAK.TMP", 11U);
    } else {
        copy_bytes(backup, "SAPBAK.TMP", 11U);
    }
    return true;
}

static int64_t syscall_rename(
    struct native_process *process,
    uint64_t request_address,
    bool replace
)
{
    struct sapote_rename_request request;
    struct sapfs_stat destination_stat;
    struct sapfs_stat backup_stat;
    char source[SAPFS_MAX_PATH];
    char destination[SAPFS_MAX_PATH];
    char backup[SAPFS_MAX_PATH];
    enum sapfs_volume source_volume;
    enum sapfs_volume destination_volume;
    enum sapfs_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U ||
        !path_from_user(process, &request.source, source, &source_volume) ||
        !path_from_user(process, &request.destination, destination,
            &destination_volume)) {
        return -SAPOTE_EINVAL;
    }
    if (source_volume != SAPFS_VOLUME_DATA ||
        destination_volume != SAPFS_VOLUME_DATA ||
        (process->manifest.capabilities & SAPOTE_CAP_DATA_WRITE) == 0U) {
        return -SAPOTE_EACCES;
    }
    cpu_interrupt_enable();
    status = sapfs_rename(SAPFS_VOLUME_DATA, source, destination);
    if (status == SAPFS_STATUS_EXISTS && replace &&
        replacement_backup_path(destination, backup) &&
        sapfs_stat_path(SAPFS_VOLUME_DATA, destination, &destination_stat) ==
            SAPFS_STATUS_OK && !destination_stat.directory &&
        sapfs_stat_path(SAPFS_VOLUME_DATA, backup, &backup_stat) ==
            SAPFS_STATUS_NOT_FOUND) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, destination, backup);
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_rename(SAPFS_VOLUME_DATA, source, destination);
            if (status == SAPFS_STATUS_OK) {
                status = sapfs_unlink(SAPFS_VOLUME_DATA, backup);
            } else {
                (void)sapfs_rename(SAPFS_VOLUME_DATA, backup, destination);
            }
        }
    }
    cpu_interrupt_disable();
    return filesystem_error(status);
}

static int64_t syscall_volume_sync(
    struct native_process *process,
    uint64_t volume_number
)
{
    enum sapfs_volume volume;

    if (volume_number == SAPOTE_VOLUME_SYSTEM) {
        if ((process->manifest.capabilities & SAPOTE_CAP_SYSTEM_READ) == 0U) {
            return -SAPOTE_EACCES;
        }
        volume = SAPFS_VOLUME_SYSTEM;
    } else if (volume_number == SAPOTE_VOLUME_DATA) {
        if ((process->manifest.capabilities & SAPOTE_CAP_DATA_WRITE) == 0U) {
            return -SAPOTE_EACCES;
        }
        volume = SAPFS_VOLUME_DATA;
    } else {
        return -SAPOTE_EINVAL;
    }
    cpu_interrupt_enable();
    const enum sapfs_status status = sapfs_sync(volume);
    cpu_interrupt_disable();
    return filesystem_error(status);
}

static int64_t syscall_volume_space(
    struct native_process *process,
    uint64_t volume_number,
    uint64_t output_address
)
{
    struct sapote_volume_space output = {
        sizeof(output), SAPOTE_ABI_VERSION, 0U, 0U,
        (uint32_t)PAGING_PAGE_SIZE, 0U
    };
    enum sapfs_volume volume;
    struct sapfs_drive_info drive;

    if (!validate_user_range(process, output_address, sizeof(output), true)) {
        return -SAPOTE_EFAULT;
    }
    if (volume_number == SAPOTE_VOLUME_SYSTEM &&
        (process->manifest.capabilities & SAPOTE_CAP_SYSTEM_READ) != 0U) {
        volume = SAPFS_VOLUME_SYSTEM;
    } else if (volume_number == SAPOTE_VOLUME_DATA &&
        (process->manifest.capabilities & SAPOTE_CAP_DATA_READ) != 0U) {
        volume = SAPFS_VOLUME_DATA;
    } else {
        return -SAPOTE_EACCES;
    }
    drive = sapfs_drive(volume);
    if (!drive.mounted || !drive.healthy) {
        return -SAPOTE_EIO;
    }
    output.total_bytes = drive.total_bytes;
    output.free_bytes = drive.free_bytes;
    return copy_to_user(process, output_address, &output, sizeof(output)) ?
        0 : -SAPOTE_EFAULT;
}

static int64_t network_error(enum network_status status)
{
    switch (status) {
    case NETWORK_STATUS_OK:
        return 0;
    case NETWORK_STATUS_TIMEOUT:
        return -SAPOTE_ETIMEDOUT;
    case NETWORK_STATUS_CANCELLED:
        return -SAPOTE_ECANCELED;
    case NETWORK_STATUS_WOULD_BLOCK:
        return -SAPOTE_EAGAIN;
    case NETWORK_STATUS_NO_RESOURCES:
        return -SAPOTE_ENOMEM;
    case NETWORK_STATUS_STALE_HANDLE:
        return -SAPOTE_ESTALE;
    case NETWORK_STATUS_WRONG_OWNER:
    case NETWORK_STATUS_WRONG_MODE:
        return -SAPOTE_EBADF;
    case NETWORK_STATUS_ALREADY_BOUND:
    case NETWORK_STATUS_PORT_IN_USE:
        return -SAPOTE_EBUSY;
    case NETWORK_STATUS_CONNECTION_CLOSED:
        return -SAPOTE_EPIPE;
    case NETWORK_STATUS_RESET:
    case NETWORK_STATUS_CONNECTION_RESET:
        return -SAPOTE_EIO;
    case NETWORK_STATUS_INVALID_ARGUMENT:
    case NETWORK_STATUS_RANGE:
    case NETWORK_STATUS_NULL_ARGUMENT:
        return -SAPOTE_EINVAL;
    case NETWORK_STATUS_UNSUPPORTED:
        return -SAPOTE_ENOTSUP;
    case NETWORK_STATUS_UNAVAILABLE:
    case NETWORK_STATUS_LINK_DOWN:
    case NETWORK_STATUS_UNCONFIGURED:
    case NETWORK_STATUS_NOT_INITIALIZED:
    case NETWORK_STATUS_UNREACHABLE:
    case NETWORK_STATUS_DHCP_NAK:
    case NETWORK_STATUS_DNS_FAILURE:
    case NETWORK_STATUS_MALFORMED:
    case NETWORK_STATUS_CHECKSUM:
    case NETWORK_STATUS_FRAGMENTED:
    case NETWORK_STATUS_HTTP_FAILURE:
    case NETWORK_STATUS_FILESYSTEM:
    case NETWORK_STATUS_TOO_LARGE:
    case NETWORK_STATUS_ALREADY_INITIALIZED:
    case NETWORK_STATUS_COUNT:
    default:
        return -SAPOTE_EIO;
    }
}

static bool deadline_timeout(uint64_t deadline, uint64_t *timeout)
{
    const uint64_t now = clock_monotonic_ns();

    if (timeout == NULL || deadline <= now) {
        return false;
    }
    *timeout = deadline - now;
    if (*timeout > UINT64_C(30000000000)) {
        *timeout = UINT64_C(30000000000);
    }
    return true;
}

static enum network_status prepare_native_network(
    uint64_t deadline,
    uint64_t *timeout
)
{
    enum network_status status;

    if (!deadline_timeout(deadline, timeout)) {
        return NETWORK_STATUS_TIMEOUT;
    }
    if (network_get_state().configuration.configured) {
        return NETWORK_STATUS_OK;
    }
    status = network_start_dhcp(*timeout);
    if (status != NETWORK_STATUS_OK) {
        return status;
    }
    return deadline_timeout(deadline, timeout) ? NETWORK_STATUS_OK :
        NETWORK_STATUS_TIMEOUT;
}

static int64_t syscall_random(
    struct native_process *process,
    uint64_t address,
    size_t length
)
{
    size_t completed = 0U;

    if ((process->manifest.capabilities & SAPOTE_CAP_ENTROPY) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (length == 0U) {
        return 0;
    }
    if (!validate_user_range(process, address, length, true)) {
        return -SAPOTE_EFAULT;
    }
    while (completed < length) {
        size_t chunk = length - completed;

        if (chunk > RANDOM_MAX_REQUEST_BYTES) {
            chunk = RANDOM_MAX_REQUEST_BYTES;
        }
        if (random_bytes(process->transfer, chunk) != RANDOM_STATUS_OK ||
            !copy_to_user(process, address + completed, process->transfer,
                chunk)) {
            return completed == 0U ? -SAPOTE_EIO : (int64_t)completed;
        }
        completed += chunk;
    }
    return (int64_t)completed;
}

static int64_t syscall_time_realtime(const struct native_process *process)
{
    int64_t seconds;

    if ((process->manifest.capabilities & SAPOTE_CAP_TIME) == 0U) {
        return -SAPOTE_EACCES;
    }
    return wall_clock_read_unix_seconds(&seconds) == WALL_CLOCK_STATUS_OK ?
        seconds : -SAPOTE_EIO;
}

static int64_t syscall_timer_create(struct native_process *process)
{
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    sapote_handle_t handle;
    const enum native_handle_status status = native_handle_install(
        &process->handles, SAPOTE_HANDLE_TIMER, &resource, &handle);

    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_timer_set(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_timer_set_request request;
    struct native_resource *resource;
    enum native_handle_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U) {
        return -SAPOTE_EINVAL;
    }
    status = native_handle_resolve(&process->handles, request.handle,
        SAPOTE_HANDLE_TIMER, &resource);
    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    resource->words[0] = request.deadline_ns;
    return 0;
}

static int64_t syscall_sleep_until(
    struct native_process *process,
    uint64_t deadline
)
{
    struct native_thread *thread = running_thread(process);
    const uint64_t now = clock_monotonic_ns();

    if ((process->manifest.capabilities & SAPOTE_CAP_TIME) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (thread == NULL) {
        return -SAPOTE_EIO;
    }
    if (deadline <= now) {
        return 0;
    }
    thread->deadline_ns = deadline;
    thread->state = NATIVE_THREAD_SLEEP_WAIT;
    return 0;
}

static int64_t poll_wait_items(
    struct native_process *process,
    struct sapote_wait_item *items,
    size_t count
)
{
    struct network_poll_request network_requests[SAPOTE_WAIT_MAX];
    struct network_poll_result network_results[SAPOTE_WAIT_MAX];
    size_t network_indices[SAPOTE_WAIT_MAX];
    size_t network_count = 0U;
    size_t ready_count = 0U;

    for (size_t index = 0U; index < count; ++index) {
        struct native_resource *resource;
        const enum native_handle_status status = native_handle_resolve(
            &process->handles, items[index].handle, 0U, &resource);
        const uint8_t type = status == NATIVE_HANDLE_OK ?
            (uint8_t)((items[index].handle >> 16U) & UINT64_C(0xFF)) : 0U;

        items[index].ready = 0U;
        if (status != NATIVE_HANDLE_OK) {
            return handle_error(status);
        }
        if ((items[index].interests & ~SAPOTE_WAIT_INTERESTS_V1) != 0U ||
            items[index].interests == 0U) {
            return -SAPOTE_EINVAL;
        }
        if (type == SAPOTE_HANDLE_FILE || type == SAPOTE_HANDLE_DIRECTORY) {
            items[index].ready = items[index].interests &
                (SAPOTE_WAIT_READABLE | SAPOTE_WAIT_WRITABLE);
        } else if (type == SAPOTE_HANDLE_TIMER) {
            if (resource->words[0] != 0U &&
                clock_monotonic_ns() >= resource->words[0]) {
                items[index].ready = items[index].interests &
                    SAPOTE_WAIT_SIGNALED;
            }
        } else if (type == SAPOTE_HANDLE_EVENT_QUEUE) {
            if (process->window.allocated &&
                process->window.generation == resource->words[1] &&
                (process->window.event_count != 0U ||
                    process->window.overflow_pending)) {
                items[index].ready = items[index].interests &
                    SAPOTE_WAIT_READABLE;
            }
        } else if (type == SAPOTE_HANDLE_STREAM ||
            type == SAPOTE_HANDLE_DATAGRAM) {
            network_requests[network_count].handle = resource->words[0];
            network_requests[network_count].interests = 0U;
            if ((items[index].interests & SAPOTE_WAIT_READABLE) != 0U) {
                network_requests[network_count].interests |=
                    NETWORK_READY_READABLE;
            }
            if ((items[index].interests & SAPOTE_WAIT_WRITABLE) != 0U) {
                network_requests[network_count].interests |=
                    NETWORK_READY_WRITABLE;
            }
            network_indices[network_count++] = index;
        }
        if (items[index].ready != 0U) {
            ++ready_count;
        }
    }
    if (network_count != 0U) {
        size_t result_count = 0U;

        cpu_interrupt_enable();
        const enum network_status status = network_poll(process->generation,
            network_requests, network_count, network_results, network_count,
            &result_count, 1U);
        cpu_interrupt_disable();
        if (status != NETWORK_STATUS_OK && status != NETWORK_STATUS_TIMEOUT) {
            return network_error(status);
        }
        for (size_t result = 0U; result < result_count; ++result) {
            const size_t index = network_indices[result];

            if ((network_results[result].ready & NETWORK_READY_READABLE) != 0U) {
                items[index].ready |= SAPOTE_WAIT_READABLE;
            }
            if ((network_results[result].ready & NETWORK_READY_WRITABLE) != 0U) {
                items[index].ready |= SAPOTE_WAIT_WRITABLE;
            }
            if ((network_results[result].ready &
                    (NETWORK_READY_PEER_CLOSED | NETWORK_READY_ERROR |
                        NETWORK_READY_CANCELLED)) != 0U) {
                items[index].ready |= SAPOTE_WAIT_CLOSED;
            }
            if (items[index].ready != 0U) {
                ++ready_count;
            }
        }
    }
    return (int64_t)ready_count;
}

static int64_t syscall_wait(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_wait_request request;
    struct sapote_wait_item items[SAPOTE_WAIT_MAX];
    struct native_thread *thread = running_thread(process);
    int64_t ready;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.count == 0U || request.count > SAPOTE_WAIT_MAX) {
        return -SAPOTE_EINVAL;
    }
    if (thread == NULL || !validate_user_range(process, request.items,
            request.count * sizeof(items[0]), true) ||
        !copy_from_user(process, items, request.items,
            request.count * sizeof(items[0]))) {
        return -SAPOTE_EFAULT;
    }
    ready = poll_wait_items(process, items, request.count);
    if (ready < 0) {
        return ready;
    }
    if (ready != 0 || request.deadline_ns <= clock_monotonic_ns()) {
        if (!copy_to_user(process, request.items, items,
                request.count * sizeof(items[0]))) {
            return -SAPOTE_EFAULT;
        }
        return ready == 0 ? -SAPOTE_ETIMEDOUT : ready;
    }
    copy_bytes(thread->wait_items, items,
        request.count * sizeof(items[0]));
    thread->wait_items_address = request.items;
    thread->wait_item_count = request.count;
    thread->deadline_ns = request.deadline_ns;
    thread->state = NATIVE_THREAD_HANDLE_WAIT;
    return 0;
}

static void native_ui_event(
    uint32_t slot,
    const struct ui_native_event *source,
    void *context
)
{
    struct native_process *process = context;
    struct native_window_state *window;
    struct sapote_event event;

    if (process == NULL || source == NULL || !process->active ||
        !process->window.allocated ||
        process->window.ui_slot != slot ||
        !process->window.event_object_open) {
        return;
    }
    if ((source->type == UI_NATIVE_EVENT_KEY ||
            source->type == UI_NATIVE_EVENT_POINTER_MOVE ||
            source->type == UI_NATIVE_EVENT_POINTER_BUTTON) &&
        (process->manifest.capabilities & SAPOTE_CAP_INPUT) == 0U) {
        return;
    }
    window = &process->window;
    zero_bytes(&event, sizeof(event));
    event.size = sizeof(event);
    event.version = SAPOTE_ABI_VERSION;
    event.monotonic_ns = source->monotonic_ns;
    event.x = source->x;
    event.y = source->y;
    event.delta_x = source->delta_x;
    event.delta_y = source->delta_y;
    event.code = source->code;
    event.value = source->value;
    event.modifiers = source->modifiers;
    switch (source->type) {
    case UI_NATIVE_EVENT_KEY:
        event.type = SAPOTE_EVENT_KEY;
        break;
    case UI_NATIVE_EVENT_POINTER_MOVE:
        event.type = SAPOTE_EVENT_POINTER_MOVE;
        break;
    case UI_NATIVE_EVENT_POINTER_BUTTON:
        event.type = SAPOTE_EVENT_POINTER_BUTTON;
        break;
    case UI_NATIVE_EVENT_FOCUS:
        event.type = SAPOTE_EVENT_FOCUS;
        break;
    case UI_NATIVE_EVENT_CLOSE:
        event.type = SAPOTE_EVENT_CLOSE;
        break;
    default:
        return;
    }
    if (event.type == SAPOTE_EVENT_POINTER_MOVE && window->event_count != 0U &&
        window->events[window->event_count - 1U].type ==
            SAPOTE_EVENT_POINTER_MOVE) {
        window->events[window->event_count - 1U] = event;
        return;
    }
    if (window->event_count == NATIVE_EVENT_QUEUE_CAPACITY) {
        size_t remove = 0U;

        for (size_t index = 0U; index < window->event_count; ++index) {
            if (window->events[index].type == SAPOTE_EVENT_POINTER_MOVE) {
                remove = index;
                break;
            }
        }
        for (size_t index = remove + 1U; index < window->event_count;
             ++index) {
            window->events[index - 1U] = window->events[index];
        }
        --window->event_count;
        window->overflow_pending = true;
    }
    window->events[window->event_count++] = event;
}

static int64_t syscall_window_create(
    struct native_process *process,
    uint64_t request_address,
    uint64_t response_address
)
{
    struct sapote_window_create_request request;
    struct sapote_window_create_response response = {
        sizeof(response), SAPOTE_ABI_VERSION, SAPOTE_HANDLE_INVALID,
        SAPOTE_HANDLE_INVALID, 0U, 0U, 0U, 0U, SAPOTE_PIXEL_XRGB8888
    };
    struct native_resource window_resource = {{0U, 0U, 0U, 0U}};
    struct native_resource event_resource = {{0U, 0U, 0U, 0U}};
    struct native_window_state *window = &process->window;
    char title[SAPOTE_WINDOW_TITLE_MAX + 1U];
    uint64_t byte_length;
    size_t page_count;
    sapote_handle_t window_handle;
    sapote_handle_t event_handle;

    if ((process->manifest.capabilities & SAPOTE_CAP_WINDOW) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (!copy_from_user(process, &request, request_address,
            sizeof(request)) ||
        !validate_user_range(process, response_address, sizeof(response),
            true)) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.reserved != 0U || request.title_length == 0U ||
        request.title_length > SAPOTE_WINDOW_TITLE_MAX ||
        request.width < 64U || request.width > NATIVE_SURFACE_MAX_WIDTH ||
        request.height < 64U || request.height > NATIVE_SURFACE_MAX_HEIGHT ||
        request.pixel_format != SAPOTE_PIXEL_XRGB8888 || window->allocated ||
        !copy_from_user(process, title, request.title,
            request.title_length)) {
        return -SAPOTE_EINVAL;
    }
    for (size_t index = 0U; index < request.title_length; ++index) {
        if (title[index] < ' ' || title[index] > '~') {
            return -SAPOTE_EINVAL;
        }
    }
    title[request.title_length] = '\0';
    if (request.width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL) {
        return -SAPOTE_EINVAL;
    }
    window->stride_bytes = request.width * SURFACE_BYTES_PER_PIXEL;
    byte_length = (uint64_t)window->stride_bytes * request.height;
    if (byte_length == 0U || byte_length > SIZE_MAX ||
        byte_length > process->manifest.memory_limit ||
        process->page_count > NATIVE_PROCESS_PAGE_LIMIT -
            (size_t)((byte_length + PAGING_PAGE_SIZE - 1U) /
                PAGING_PAGE_SIZE)) {
        zero_bytes(window, sizeof(*window));
        return -SAPOTE_ENOMEM;
    }
    window->allocated = true;
    window->window_object_open = true;
    window->surface_address = PAGING_NATIVE_SURFACE_BASE;
    window->surface_bytes = (size_t)byte_length;
    window->width = request.width;
    window->height = request.height;
    window->ui_slot = (uint32_t)current_process;
    window->generation = next_window_generation++;
    if (next_window_generation == 0U) {
        next_window_generation = 1U;
    }
    if (heap_allocate(window->surface_bytes,
            (void **)&window->shadow_pixels) != HEAP_STATUS_OK) {
        zero_bytes(window, sizeof(*window));
        return -SAPOTE_ENOMEM;
    }
    zero_bytes(window->shadow_pixels, window->surface_bytes);
    page_count = (window->surface_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
    for (size_t page = 0U; page < page_count; ++page) {
        uintptr_t physical_address;
        const uint64_t address = window->surface_address +
            page * PAGING_PAGE_SIZE;

        if (!allocate_page(process, address, PAGING_WRITE,
                PAGING_PROCESS_MAPPING_NATIVE_SURFACE, &physical_address) ||
            paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_SURFACE, address,
                physical_address, PAGING_WRITE) != PAGING_STATUS_OK) {
            (void)window_release_surface(process);
            return -SAPOTE_ENOMEM;
        }
        page_at(process, address)->mapped = true;
    }
    window_resource.words[0] = window->ui_slot;
    window_resource.words[1] = window->generation;
    if (native_handle_install(&process->handles, SAPOTE_HANDLE_WINDOW,
            &window_resource, &window_handle) != NATIVE_HANDLE_OK) {
        (void)window_release_surface(process);
        return -SAPOTE_ENOMEM;
    }
    event_resource.words[0] = window->ui_slot;
    event_resource.words[1] = window->generation;
    if (native_handle_install(&process->handles, SAPOTE_HANDLE_EVENT_QUEUE,
            &event_resource, &event_handle) != NATIVE_HANDLE_OK) {
        (void)native_handle_close(&process->handles, window_handle,
            close_resource, process);
        return -SAPOTE_ENOMEM;
    }
    window->event_object_open = true;
    const enum ui_status ui_status = ui_native_window_open(window->ui_slot,
        title, window->shadow_pixels, window->width, window->height,
        window->stride_bytes, native_ui_event, process);

    if (ui_status != UI_STATUS_OK) {
        console_write("Sapote: native window open failed: ");
        console_write(ui_status_string(ui_status));
        console_write("\n");
        (void)native_handle_close(&process->handles, event_handle,
            close_resource, process);
        (void)native_handle_close(&process->handles, window_handle,
            close_resource, process);
        return -SAPOTE_EIO;
    }
    window->visible = true;
    response.window = window_handle;
    response.events = event_handle;
    response.surface_address = window->surface_address;
    response.width = window->width;
    response.height = window->height;
    response.stride_bytes = window->stride_bytes;
    if (!copy_to_user(process, response_address, &response,
            sizeof(response))) {
        (void)native_handle_close(&process->handles, event_handle,
            close_resource, process);
        (void)native_handle_close(&process->handles, window_handle,
            close_resource, process);
        return -SAPOTE_EFAULT;
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return 0;
}

static int64_t syscall_surface_present(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_present_request request;
    struct sapote_rect rectangles[SAPOTE_DAMAGE_MAX];
    struct ui_rect damage[SAPOTE_DAMAGE_MAX];
    struct native_resource *resource;
    struct native_window_state *window = &process->window;
    uint64_t pixel_count = 0U;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.rectangle_count == 0U ||
        request.rectangle_count > SAPOTE_DAMAGE_MAX ||
        !copy_from_user(process, rectangles, request.rectangles,
            request.rectangle_count * sizeof(rectangles[0]))) {
        return -SAPOTE_EINVAL;
    }
    if (native_handle_resolve(&process->handles, request.window,
            SAPOTE_HANDLE_WINDOW, &resource) != NATIVE_HANDLE_OK ||
        !window->allocated || !window->window_object_open ||
        resource->words[1] != window->generation) {
        return -SAPOTE_EBADF;
    }
    for (size_t index = 0U; index < request.rectangle_count; ++index) {
        const struct sapote_rect rectangle = rectangles[index];

        if (rectangle.width == 0U || rectangle.height == 0U ||
            rectangle.x >= window->width || rectangle.y >= window->height ||
            rectangle.width > window->width - rectangle.x ||
            rectangle.height > window->height - rectangle.y) {
            return -SAPOTE_EINVAL;
        }
        for (uint32_t row = 0U; row < rectangle.height; ++row) {
            const uint64_t address = window->surface_address +
                (uint64_t)(rectangle.y + row) * window->stride_bytes +
                (uint64_t)rectangle.x * SURFACE_BYTES_PER_PIXEL;

            if (!validate_user_range(process, address,
                    (size_t)rectangle.width * SURFACE_BYTES_PER_PIXEL,
                    false)) {
                return -SAPOTE_EFAULT;
            }
        }
        damage[index] = (struct ui_rect){ rectangle.x, rectangle.y,
            rectangle.width, rectangle.height };
        pixel_count += (uint64_t)rectangle.width * rectangle.height;
    }
    for (size_t index = 0U; index < request.rectangle_count; ++index) {
        const struct sapote_rect rectangle = rectangles[index];

        for (uint32_t row = 0U; row < rectangle.height; ++row) {
            uint32_t completed = 0U;

            while (completed < rectangle.width) {
                uint32_t pixels = rectangle.width - completed;
                const uint64_t address = window->surface_address +
                    (uint64_t)(rectangle.y + row) * window->stride_bytes +
                    (uint64_t)(rectangle.x + completed) *
                        SURFACE_BYTES_PER_PIXEL;

                if (pixels > sizeof(process->transfer) /
                        SURFACE_BYTES_PER_PIXEL) {
                    pixels = sizeof(process->transfer) /
                        SURFACE_BYTES_PER_PIXEL;
                }
                if (!copy_from_user(process, process->transfer, address,
                        (size_t)pixels * SURFACE_BYTES_PER_PIXEL)) {
                    return -SAPOTE_EFAULT;
                }
                for (uint32_t pixel = 0U; pixel < pixels; ++pixel) {
                    const size_t offset = (size_t)pixel * 4U;
                    const uint32_t xrgb =
                        (uint32_t)process->transfer[offset] |
                        (uint32_t)process->transfer[offset + 1U] << 8U |
                        (uint32_t)process->transfer[offset + 2U] << 16U |
                        (uint32_t)process->transfer[offset + 3U] << 24U;

                    window->shadow_pixels[
                        (size_t)(rectangle.y + row) * window->width +
                        rectangle.x + completed + pixel] = framebuffer_pack(
                            (uint8_t)(xrgb >> 16U),
                            (uint8_t)(xrgb >> 8U), (uint8_t)xrgb);
                }
                completed += pixels;
            }
        }
    }
    if (ui_native_window_damage(window->ui_slot, damage,
            request.rectangle_count) != UI_STATUS_OK) {
        return -SAPOTE_EIO;
    }
    ++window->present_calls;
    window->presented_pixels += pixel_count;
    return (int64_t)pixel_count;
}

static int64_t syscall_event_read(
    struct native_process *process,
    sapote_handle_t handle,
    uint64_t output_address
)
{
    struct native_resource *resource;
    struct sapote_event event;
    struct native_window_state *window = &process->window;
    enum native_handle_status status;

    if (!validate_user_range(process, output_address, sizeof(event), true)) {
        return -SAPOTE_EFAULT;
    }
    status = native_handle_resolve(&process->handles, handle,
        SAPOTE_HANDLE_EVENT_QUEUE, &resource);
    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (!window->allocated || !window->event_object_open ||
        resource->words[1] != window->generation) {
        return -SAPOTE_ESTALE;
    }
    if (window->overflow_pending) {
        zero_bytes(&event, sizeof(event));
        event.size = sizeof(event);
        event.version = SAPOTE_ABI_VERSION;
        event.type = SAPOTE_EVENT_QUEUE_OVERFLOW;
        event.monotonic_ns = clock_monotonic_ns();
        window->overflow_pending = false;
    } else {
        if (window->event_count == 0U) {
            return -SAPOTE_EAGAIN;
        }
        event = window->events[0];
        for (size_t index = 1U; index < window->event_count; ++index) {
            window->events[index - 1U] = window->events[index];
        }
        --window->event_count;
    }
    return copy_to_user(process, output_address, &event, sizeof(event)) ?
        1 : -SAPOTE_EFAULT;
}

static int64_t syscall_pointer_capture(
    struct native_process *process,
    sapote_handle_t handle,
    uint64_t capture
)
{
    struct native_resource *resource;
    enum native_handle_status status;

    if ((process->manifest.capabilities & SAPOTE_CAP_INPUT) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (capture > 1U) {
        return -SAPOTE_EINVAL;
    }
    status = native_handle_resolve(&process->handles, handle,
        SAPOTE_HANDLE_WINDOW, &resource);
    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    return ui_native_pointer_capture((uint32_t)resource->words[0],
        capture != 0U) == UI_STATUS_OK ? 0 : -SAPOTE_EBUSY;
}

static int64_t syscall_dns_resolve(
    struct native_process *process,
    uint64_t hostname_address,
    size_t hostname_length,
    uint64_t deadline
)
{
    char hostname[NETWORK_MAX_HOSTNAME + 1U];
    uint64_t timeout;
    uint32_t address;
    enum network_status status;

    if ((process->manifest.capabilities & SAPOTE_CAP_NETWORK) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (hostname_length == 0U || hostname_length > NETWORK_MAX_HOSTNAME ||
        !copy_from_user(process, hostname, hostname_address,
            hostname_length)) {
        return -SAPOTE_EFAULT;
    }
    for (size_t index = 0U; index < hostname_length; ++index) {
        if (hostname[index] == '\0' ||
            (uint8_t)hostname[index] > UINT8_C(0x7F)) {
            return -SAPOTE_EINVAL;
        }
    }
    hostname[hostname_length] = '\0';
    cpu_interrupt_enable();
    status = prepare_native_network(deadline, &timeout);
    if (status == NETWORK_STATUS_OK) {
        status = network_resolve(hostname, &address, timeout);
    }
    cpu_interrupt_disable();
    return status == NETWORK_STATUS_OK ? (int64_t)address :
        network_error(status);
}

static int64_t syscall_network_open(
    struct native_process *process,
    bool datagram
)
{
    network_handle network = 0U;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    sapote_handle_t handle;
    enum network_status status;
    enum native_handle_status handle_status;

    if ((process->manifest.capabilities & SAPOTE_CAP_NETWORK) == 0U) {
        return -SAPOTE_EACCES;
    }
    cpu_interrupt_enable();
    status = datagram ? network_udp_open(process->generation, &network) :
        network_tcp_open(process->generation, &network);
    cpu_interrupt_disable();
    if (status != NETWORK_STATUS_OK) {
        return network_error(status);
    }
    resource.words[0] = network;
    handle_status = native_handle_install(&process->handles,
        datagram ? SAPOTE_HANDLE_DATAGRAM : SAPOTE_HANDLE_STREAM,
        &resource, &handle);
    if (handle_status != NATIVE_HANDLE_OK) {
        cpu_interrupt_enable();
        (void)network_close(process->generation, network);
        cpu_interrupt_disable();
        return handle_error(handle_status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_stream_connect(
    struct native_process *process,
    sapote_handle_t handle,
    uint64_t endpoint_address,
    uint64_t deadline
)
{
    struct sapote_ipv4_endpoint endpoint;
    struct native_resource *resource;
    uint64_t timeout;
    enum native_handle_status handle_status;
    enum network_status status;

    if (!copy_from_user(process, &endpoint, endpoint_address,
            sizeof(endpoint))) {
        return -SAPOTE_EFAULT;
    }
    if (endpoint.reserved != 0U || endpoint.address == 0U ||
        endpoint.port == 0U) {
        return -SAPOTE_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, handle,
        SAPOTE_HANDLE_STREAM, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    status = prepare_native_network(deadline, &timeout);
    if (status == NETWORK_STATUS_OK) {
        status = network_tcp_connect(process->generation, resource->words[0],
            endpoint.address, endpoint.port, timeout);
    }
    cpu_interrupt_disable();
    return network_error(status);
}

static int64_t syscall_network_io(
    struct native_process *process,
    uint64_t request_address,
    bool datagram,
    bool write
)
{
    struct sapote_network_io request;
    struct native_resource *resource;
    uint64_t timeout;
    size_t transferred = 0U;
    uint32_t source = 0U;
    uint16_t port = 0U;
    enum native_handle_status handle_status;
    enum network_status status;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.endpoint.reserved != 0U || request.length == 0U ||
        request.length > (datagram ? NETWORK_MAX_UDP_DATAGRAM :
            sizeof(process->transfer)) ||
        !validate_user_range(process, request.buffer, request.length, !write) ||
        (!write && datagram && !validate_user_range(process, request_address,
            sizeof(request), true))) {
        return -SAPOTE_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, request.handle,
        datagram ? SAPOTE_HANDLE_DATAGRAM : SAPOTE_HANDLE_STREAM, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (write && !copy_from_user(process, process->transfer, request.buffer,
            request.length)) {
        return -SAPOTE_EFAULT;
    }
    cpu_interrupt_enable();
    status = prepare_native_network(request.deadline_ns, &timeout);
    if (status == NETWORK_STATUS_OK && datagram && write) {
        status = network_udp_send(process->generation, resource->words[0],
            request.endpoint.address, request.endpoint.port, process->transfer,
            request.length, timeout);
        transferred = status == NETWORK_STATUS_OK ? request.length : 0U;
    } else if (status == NETWORK_STATUS_OK && datagram) {
        status = network_udp_receive(process->generation, resource->words[0],
            &source, &port, process->transfer, request.length, &transferred,
            timeout);
    } else if (status == NETWORK_STATUS_OK && write) {
        status = network_tcp_write(process->generation, resource->words[0],
            process->transfer, request.length, &transferred, timeout);
    } else if (status == NETWORK_STATUS_OK) {
        status = network_tcp_read(process->generation, resource->words[0],
            process->transfer, request.length, &transferred, timeout);
    }
    cpu_interrupt_disable();
    if (status != NETWORK_STATUS_OK) {
        return network_error(status);
    }
    if (!write && transferred != 0U &&
        !copy_to_user(process, request.buffer, process->transfer,
            transferred)) {
        return -SAPOTE_EFAULT;
    }
    if (!write && datagram) {
        request.endpoint.address = source;
        request.endpoint.port = port;
        request.length = (uint32_t)transferred;
        if (!copy_to_user(process, request_address, &request,
                sizeof(request))) {
            return -SAPOTE_EFAULT;
        }
    }
    return (int64_t)transferred;
}

static int64_t syscall_datagram_bind(
    struct native_process *process,
    sapote_handle_t handle,
    uint16_t port
)
{
    struct native_resource *resource;
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, handle, SAPOTE_HANDLE_DATAGRAM, &resource);

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    const enum network_status status = network_udp_bind(process->generation,
        resource->words[0], port);
    cpu_interrupt_disable();
    return network_error(status);
}

static int64_t syscall_stream_shutdown(
    struct native_process *process,
    sapote_handle_t handle,
    uint32_t flags,
    uint64_t deadline
)
{
    struct native_resource *resource;
    uint64_t timeout;
    enum native_handle_status handle_status;

    if ((flags & ~(SAPOTE_SHUTDOWN_READ | SAPOTE_SHUTDOWN_WRITE)) != 0U ||
        (flags & SAPOTE_SHUTDOWN_WRITE) == 0U ||
        !deadline_timeout(deadline, &timeout)) {
        return -SAPOTE_EINVAL;
    }
    handle_status = native_handle_resolve(&process->handles, handle,
        SAPOTE_HANDLE_STREAM, &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    cpu_interrupt_enable();
    const enum network_status status = network_tcp_shutdown(
        process->generation, resource->words[0], timeout);
    cpu_interrupt_disable();
    return network_error(status);
}

static int64_t syscall_network_address(
    struct native_process *process,
    sapote_handle_t handle,
    bool peer,
    uint64_t output_address
)
{
    struct native_resource *resource;
    struct sapote_ipv4_endpoint endpoint = {0U, 0U, 0U};
    uint32_t address = 0U;
    uint16_t port = 0U;
    enum native_handle_status handle_status;
    enum network_status status;

    if (!validate_user_range(process, output_address, sizeof(endpoint), true)) {
        return -SAPOTE_EFAULT;
    }
    handle_status = native_handle_resolve(&process->handles, handle, 0U,
        &resource);
    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    const uint8_t type = (uint8_t)((handle >> 16U) & UINT64_C(0xFF));

    if (type != SAPOTE_HANDLE_STREAM && type != SAPOTE_HANDLE_DATAGRAM) {
        return -SAPOTE_EBADF;
    }
    cpu_interrupt_enable();
    status = network_address(process->generation, resource->words[0], peer,
        &address, &port);
    cpu_interrupt_disable();
    if (status != NETWORK_STATUS_OK) {
        return network_error(status);
    }
    endpoint.address = address;
    endpoint.port = port;
    return copy_to_user(process, output_address, &endpoint, sizeof(endpoint)) ?
        0 : -SAPOTE_EFAULT;
}

static int64_t syscall_cancel(
    struct native_process *process,
    sapote_handle_t handle
)
{
    struct native_resource *resource;
    enum native_handle_status handle_status = native_handle_resolve(
        &process->handles, handle, 0U, &resource);
    const uint8_t type = (uint8_t)((handle >> 16U) & UINT64_C(0xFF));

    if (handle_status != NATIVE_HANDLE_OK) {
        return handle_error(handle_status);
    }
    if (type == SAPOTE_HANDLE_TIMER) {
        resource->words[0] = 0U;
        return 0;
    }
    if (type != SAPOTE_HANDLE_STREAM && type != SAPOTE_HANDLE_DATAGRAM) {
        return -SAPOTE_ENOTSUP;
    }
    cpu_interrupt_enable();
    const enum network_status status = network_cancel(process->generation,
        resource->words[0]);
    cpu_interrupt_disable();
    return network_error(status);
}

static bool release_runtime_pages(
    struct native_process *process,
    uint64_t base,
    size_t page_count,
    enum paging_process_mapping_kind kind
)
{
    bool success = true;

    for (size_t page = 0U; page < page_count; ++page) {
        struct native_page removed;
        const uint64_t address = base + page * PAGING_PAGE_SIZE;
        struct native_page *record = page_at(process, address);

        if (record == NULL || record->kind != kind) {
            success = false;
            continue;
        }
        if (record->mapped &&
            paging_process_unmap_user_page(&process->address_space, kind,
                address) != PAGING_STATUS_OK) {
            success = false;
            continue;
        }
        if (!remove_page_record(process, address, &removed) ||
            frame_release(removed.physical_address) != FRAME_STATUS_OK) {
            success = false;
        }
    }
    return success;
}

static int64_t syscall_thread_create(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_thread_create_request request;
    struct native_thread *thread;
    struct native_resource resource = {{0U, 0U, 0U, 0U}};
    sapote_handle_t handle;
    size_t index;
    size_t stack_pages;
    uint64_t guard;
    uint64_t stack_base;

    if ((process->manifest.capabilities & SAPOTE_CAP_THREADS) == 0U) {
        return -SAPOTE_EACCES;
    }
    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.flags != 0U ||
        request.entry < process->image.mapping_start ||
        request.entry >= process->image.mapping_end ||
        page_at(process, request.entry) == NULL ||
        page_at(process, request.entry)->permissions != PAGING_EXECUTE ||
        request.stack_bytes < 4U * PAGING_PAGE_SIZE ||
        request.stack_bytes > NATIVE_STACK_PAGES * PAGING_PAGE_SIZE ||
        (request.tls_base != 0U &&
            !validate_user_range(process, request.tls_base, 1U, false))) {
        return -SAPOTE_EINVAL;
    }
    if (process->thread_count >= process->manifest.max_threads ||
        process->thread_count >= NATIVE_THREAD_LIMIT) {
        return -SAPOTE_ENOMEM;
    }
    index = process->thread_count;
    stack_pages = (request.stack_bytes + PAGING_PAGE_SIZE - 1U) /
        PAGING_PAGE_SIZE;
    guard = PAGING_NATIVE_STACK_BASE +
        index * (NATIVE_STACK_PAGES + 1U) * PAGING_PAGE_SIZE;
    stack_base = guard + PAGING_PAGE_SIZE;
    if (stack_base + stack_pages * PAGING_PAGE_SIZE > PAGING_NATIVE_STACK_END) {
        return -SAPOTE_ENOMEM;
    }
    for (size_t page = 0U; page < stack_pages; ++page) {
        uintptr_t physical_address;
        const uint64_t address = stack_base + page * PAGING_PAGE_SIZE;

        if (!allocate_page(process, address, PAGING_WRITE,
                PAGING_PROCESS_MAPPING_NATIVE_STACK, &physical_address) ||
            paging_process_map_user_page(&process->address_space,
                PAGING_PROCESS_MAPPING_NATIVE_STACK, address,
                physical_address, PAGING_WRITE) != PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback <= page; ++rollback) {
                struct native_page removed;
                const uint64_t rollback_address = stack_base +
                    rollback * PAGING_PAGE_SIZE;
                struct native_page *record = page_at(process,
                    rollback_address);

                if (record == NULL) {
                    continue;
                }
                if (record->mapped) {
                    (void)paging_process_unmap_user_page(
                        &process->address_space,
                        PAGING_PROCESS_MAPPING_NATIVE_STACK,
                        rollback_address);
                }
                if (remove_page_record(process, rollback_address, &removed)) {
                    (void)frame_release(removed.physical_address);
                }
            }
            return -SAPOTE_ENOMEM;
        }
        page_at(process, address)->mapped = true;
    }
    thread = &process->threads[index];
    zero_bytes(thread, sizeof(*thread));
    if (!native_fpu_state_initialize(&thread->fpu)) {
        (void)release_runtime_pages(process, stack_base, stack_pages,
            PAGING_PROCESS_MAPPING_NATIVE_STACK);
        return -SAPOTE_EIO;
    }
    thread->generation = next_thread_generation++;
    if (next_thread_generation == 0U) {
        next_thread_generation = 1U;
    }
    thread->fs_base = request.tls_base;
    thread->stack_base = stack_base;
    thread->stack_end = stack_base + stack_pages * PAGING_PAGE_SIZE;
    thread->state = NATIVE_THREAD_RUNNABLE;
    thread->context.rip = request.entry;
    /* A raw C entry observes the SysV post-CALL stack alignment. */
    thread->context.rsp = thread->stack_end - sizeof(uint64_t);
    thread->context.rflags = NATIVE_RFLAGS;
    thread->context.rdi = request.argument;
    resource.words[0] = index;
    resource.words[1] = thread->generation;
    {
        const enum native_handle_status status = native_handle_install(
            &process->handles, SAPOTE_HANDLE_THREAD, &resource, &handle);

        if (status != NATIVE_HANDLE_OK) {
            const int64_t error = handle_error(status);

            zero_bytes(thread, sizeof(*thread));
            if (!release_runtime_pages(process, stack_base, stack_pages,
                    PAGING_PROCESS_MAPPING_NATIVE_STACK)) {
                process->faulted = true;
                process->exit_status = -SAPOTE_EIO;
                process->exiting = true;
                return -SAPOTE_EIO;
            }
            return error;
        }
    }
    ++process->thread_count;
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)handle;
}

static int64_t syscall_thread_join(
    struct native_process *process,
    sapote_handle_t handle
)
{
    struct native_resource *resource;
    struct native_thread *current = running_thread(process);
    struct native_thread *target;
    enum native_handle_status status = native_handle_resolve(
        &process->handles, handle, SAPOTE_HANDLE_THREAD, &resource);

    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (resource->words[0] >= process->thread_count || current == NULL) {
        return -SAPOTE_EBADF;
    }
    target = &process->threads[resource->words[0]];
    if (target == current || target->generation != resource->words[1]) {
        return -SAPOTE_EINVAL;
    }
    if (target->state == NATIVE_THREAD_EXITED ||
        target->state == NATIVE_THREAD_FAULTED) {
        return target->exit_status;
    }
    current->wait_generation = target->generation;
    current->state = NATIVE_THREAD_JOIN_WAIT;
    return 0;
}

static int64_t syscall_tls_set(
    struct native_process *process,
    uint64_t tls_base
)
{
    struct native_thread *thread = running_thread(process);

    if (thread == NULL || (tls_base != 0U &&
            !validate_user_range(process, tls_base, 1U, false))) {
        return -SAPOTE_EFAULT;
    }
    thread->fs_base = tls_base;
    return 0;
}

static int64_t syscall_futex_wait(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_futex_request request;
    struct native_thread *thread = running_thread(process);
    uint32_t observed;

    if (thread == NULL || !copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.count != 0U ||
        !validate_futex_word(process, request.address) ||
        !copy_from_user(process, &observed, request.address,
            sizeof(observed))) {
        return -SAPOTE_EINVAL;
    }
    if (observed != request.expected) {
        return -SAPOTE_EAGAIN;
    }
    thread->futex_address = request.address;
    thread->deadline_ns = request.deadline_ns;
    thread->state = NATIVE_THREAD_FUTEX_WAIT;
    return 0;
}

static int64_t syscall_futex_wake(
    struct native_process *process,
    uint64_t request_address
)
{
    struct sapote_futex_request request;
    size_t woken = 0U;

    if (!copy_from_user(process, &request, request_address,
            sizeof(request))) {
        return -SAPOTE_EFAULT;
    }
    if (request.size != sizeof(request) ||
        request.version != SAPOTE_ABI_VERSION || request.expected != 0U ||
        request.deadline_ns != 0U || request.count == 0U ||
        !validate_futex_word(process, request.address)) {
        return -SAPOTE_EINVAL;
    }
    for (size_t index = 0U; index < process->thread_count &&
         woken < request.count; ++index) {
        struct native_thread *thread = &process->threads[index];

        if (thread->state == NATIVE_THREAD_FUTEX_WAIT &&
            thread->futex_address == request.address) {
            thread->state = NATIVE_THREAD_RUNNABLE;
            thread->futex_address = 0U;
            thread->deadline_ns = 0U;
            thread->context.rax = 0U;
            ++woken;
        }
    }
    return (int64_t)woken;
}

static uint64_t safe_user_rflags(uint64_t value)
{
    const uint64_t forbidden = (UINT64_C(3) << 12U) |
        (UINT64_C(1) << 17U);

    return (value | UINT64_C(2) | UINT64_C(1) << 9U) & ~forbidden;
}

static void save_syscall_context(
    struct native_thread *thread,
    const struct native_syscall_frame *frame
)
{
    thread->context.rax = frame->rax;
    thread->context.rbx = frame->rbx;
    thread->context.rcx = frame->rip;
    thread->context.rdx = frame->rdx;
    thread->context.rsi = frame->rsi;
    thread->context.rdi = frame->rdi;
    thread->context.rbp = frame->rbp;
    thread->context.r8 = frame->r8;
    thread->context.r9 = frame->r9;
    thread->context.r10 = frame->r10;
    thread->context.r11 = frame->rflags;
    thread->context.r12 = frame->r12;
    thread->context.r13 = frame->r13;
    thread->context.r14 = frame->r14;
    thread->context.r15 = frame->r15;
    thread->context.rip = frame->rip;
    thread->context.rsp = frame->rsp;
    thread->context.rflags = safe_user_rflags(frame->rflags);
}

static void save_interrupt_context(
    struct native_thread *thread,
    const struct interrupt_frame *frame
)
{
    thread->context.rax = frame->rax;
    thread->context.rbx = frame->rbx;
    thread->context.rcx = frame->rcx;
    thread->context.rdx = frame->rdx;
    thread->context.rsi = frame->rsi;
    thread->context.rdi = frame->rdi;
    thread->context.rbp = frame->rbp;
    thread->context.r8 = frame->r8;
    thread->context.r9 = frame->r9;
    thread->context.r10 = frame->r10;
    thread->context.r11 = frame->r11;
    thread->context.r12 = frame->r12;
    thread->context.r13 = frame->r13;
    thread->context.r14 = frame->r14;
    thread->context.r15 = frame->r15;
    thread->context.rip = frame->rip;
    thread->context.rsp = interrupt_frame_stack_pointer(frame);
    thread->context.rflags = safe_user_rflags(frame->rflags);
}

static void terminate_process(struct native_process *process, int32_t status)
{
    process->exit_status = status;
    process->exiting = true;
    for (size_t index = 0U; index < process->thread_count; ++index) {
        if (process->threads[index].state != NATIVE_THREAD_FAULTED) {
            process->threads[index].state = NATIVE_THREAD_EXITED;
        }
        process->threads[index].exit_status = status;
    }
}

static int64_t syscall_handle_close(
    struct native_process *process,
    sapote_handle_t handle
)
{
    enum native_handle_status status;

    cpu_interrupt_enable();
    status = native_handle_close(&process->handles, handle, close_resource,
        process);
    cpu_interrupt_disable();
    return handle_error(status);
}

static int64_t syscall_handle_duplicate(
    struct native_process *process,
    sapote_handle_t handle
)
{
    sapote_handle_t duplicate;
    const enum native_handle_status status = native_handle_duplicate(
        &process->handles, handle, &duplicate);

    if (status != NATIVE_HANDLE_OK) {
        return handle_error(status);
    }
    if (process->handles.active_handles > process->peak_handles) {
        process->peak_handles = process->handles.active_handles;
    }
    return (int64_t)duplicate;
}

static int64_t dispatch_syscall(
    struct native_process *process,
    struct native_thread *thread,
    const struct native_syscall_frame *frame
)
{
    switch (frame->rax) {
    case SAPOTE_SYS_ABI_VERSION:
        return SAPOTE_ABI_VERSION;
    case SAPOTE_SYS_EXIT:
        terminate_process(process, (int32_t)frame->rdi);
        return 0;
    case SAPOTE_SYS_CONSOLE_WRITE:
        return syscall_console_write(process, frame->rdi, (size_t)frame->rsi);
    case SAPOTE_SYS_CONSOLE_READ:
        return syscall_console_read(process, frame->rdi, (size_t)frame->rsi);
    case SAPOTE_SYS_HANDLE_CLOSE:
        return syscall_handle_close(process, frame->rdi);
    case SAPOTE_SYS_HANDLE_DUPLICATE:
        return syscall_handle_duplicate(process, frame->rdi);
    case SAPOTE_SYS_MEMORY_MAP:
        return syscall_memory_map(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_MEMORY_UNMAP:
        return syscall_memory_unmap(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_FILE_OPEN:
        return syscall_file_open(process, frame->rdi);
    case SAPOTE_SYS_FILE_READ:
        return syscall_file_io(process, frame->rdi, false);
    case SAPOTE_SYS_FILE_WRITE:
        return syscall_file_io(process, frame->rdi, true);
    case SAPOTE_SYS_FILE_SEEK:
        return syscall_file_seek(process, frame->rdi);
    case SAPOTE_SYS_PATH_STAT:
        return syscall_path_stat(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_DIRECTORY_OPEN:
        return syscall_directory_open(process, frame->rdi);
    case SAPOTE_SYS_DIRECTORY_READ:
        return syscall_directory_read(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_PATH_MKDIR:
    case SAPOTE_SYS_PATH_UNLINK:
    case SAPOTE_SYS_PATH_TRUNCATE:
        return syscall_single_path_mutation(process, frame->rdi, frame->rsi,
            frame->rax);
    case SAPOTE_SYS_PATH_RENAME:
        return syscall_rename(process, frame->rdi, false);
    case SAPOTE_SYS_PATH_REPLACE:
        return syscall_rename(process, frame->rdi, true);
    case SAPOTE_SYS_VOLUME_SYNC:
        return syscall_volume_sync(process, frame->rdi);
    case SAPOTE_SYS_VOLUME_SPACE:
        return syscall_volume_space(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_TIME_MONOTONIC:
        return (process->manifest.capabilities & SAPOTE_CAP_TIME) != 0U ?
            (int64_t)clock_monotonic_ns() : -SAPOTE_EACCES;
    case SAPOTE_SYS_TIME_REALTIME:
        return syscall_time_realtime(process);
    case SAPOTE_SYS_SLEEP_UNTIL:
        return syscall_sleep_until(process, frame->rdi);
    case SAPOTE_SYS_WAIT:
        return syscall_wait(process, frame->rdi);
    case SAPOTE_SYS_RANDOM:
        return syscall_random(process, frame->rdi, (size_t)frame->rsi);
    case SAPOTE_SYS_TIMER_CREATE:
        return (process->manifest.capabilities & SAPOTE_CAP_TIME) != 0U ?
            syscall_timer_create(process) : -SAPOTE_EACCES;
    case SAPOTE_SYS_TIMER_SET:
        return (process->manifest.capabilities & SAPOTE_CAP_TIME) != 0U ?
            syscall_timer_set(process, frame->rdi) : -SAPOTE_EACCES;
    case SAPOTE_SYS_CANCEL:
        return syscall_cancel(process, frame->rdi);
    case SAPOTE_SYS_WINDOW_CREATE:
        return syscall_window_create(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_SURFACE_PRESENT:
        return syscall_surface_present(process, frame->rdi);
    case SAPOTE_SYS_EVENT_READ:
        return syscall_event_read(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_POINTER_CAPTURE:
        return syscall_pointer_capture(process, frame->rdi, frame->rsi);
    case SAPOTE_SYS_DNS_RESOLVE:
        return syscall_dns_resolve(process, frame->rdi, (size_t)frame->rsi,
            frame->rdx);
    case SAPOTE_SYS_STREAM_OPEN:
        return syscall_network_open(process, false);
    case SAPOTE_SYS_STREAM_CONNECT:
        return syscall_stream_connect(process, frame->rdi, frame->rsi,
            frame->rdx);
    case SAPOTE_SYS_STREAM_READ:
        return syscall_network_io(process, frame->rdi, false, false);
    case SAPOTE_SYS_STREAM_WRITE:
        return syscall_network_io(process, frame->rdi, false, true);
    case SAPOTE_SYS_STREAM_SHUTDOWN:
        return syscall_stream_shutdown(process, frame->rdi,
            (uint32_t)frame->rsi, frame->rdx);
    case SAPOTE_SYS_DATAGRAM_OPEN:
        return syscall_network_open(process, true);
    case SAPOTE_SYS_DATAGRAM_BIND:
        return syscall_datagram_bind(process, frame->rdi,
            (uint16_t)frame->rsi);
    case SAPOTE_SYS_DATAGRAM_SEND:
        return syscall_network_io(process, frame->rdi, true, true);
    case SAPOTE_SYS_DATAGRAM_RECEIVE:
        return syscall_network_io(process, frame->rdi, true, false);
    case SAPOTE_SYS_NETWORK_ADDRESS:
        if (frame->rsi > 1U) {
            return -SAPOTE_EINVAL;
        }
        return syscall_network_address(process, frame->rdi,
            frame->rsi != 0U, frame->rdx);
    case SAPOTE_SYS_THREAD_CREATE:
        return syscall_thread_create(process, frame->rdi);
    case SAPOTE_SYS_THREAD_EXIT:
        thread->exit_status = (int32_t)frame->rdi;
        thread->state = NATIVE_THREAD_EXITED;
        return 0;
    case SAPOTE_SYS_THREAD_JOIN:
        return syscall_thread_join(process, frame->rdi);
    case SAPOTE_SYS_TLS_SET:
        return syscall_tls_set(process, frame->rdi);
    case SAPOTE_SYS_TLS_GET:
        return (int64_t)thread->fs_base;
    case SAPOTE_SYS_FUTEX_WAIT:
        return syscall_futex_wait(process, frame->rdi);
    case SAPOTE_SYS_FUTEX_WAKE:
        return syscall_futex_wake(process, frame->rdi);
    default:
        return -SAPOTE_ENOSYS;
    }
}

static bool current_user_position_valid(
    struct native_process *process,
    uint64_t rip,
    uint64_t rsp
)
{
    const struct native_page *instruction = page_at(process, rip);

    return instruction != NULL && instruction->permissions == PAGING_EXECUTE &&
        rsp != 0U && validate_user_range(process, rsp - 1U, 1U, false);
}

uintptr_t native_process_on_syscall(struct native_syscall_frame *frame)
{
    struct native_process *process = running_process();
    struct native_thread *thread = running_thread(process);
    uintptr_t resume_stack;
    uint64_t without_started;
    uint64_t without_cycles;
    uint64_t fpu_started;
    uint64_t fpu_cycles;
    int64_t result;

    if (frame == NULL || process == NULL || thread == NULL ||
        thread->state != NATIVE_THREAD_RUNNABLE ||
        process->address_space.state != PAGING_PROCESS_SPACE_ACTIVE ||
        !current_user_position_valid(process, frame->rip, frame->rsp)) {
        return 0U;
    }
    without_started = tsc_read();
    save_syscall_context(thread, frame);
    without_cycles = tsc_read() - without_started;
    fpu_started = tsc_read();
    if (!native_fpu_save(&thread->fpu)) {
        return 0U;
    }
    fpu_cycles = tsc_read() - fpu_started;
    without_started = tsc_read();
    thread->fs_base = cpu_read_msr(IA32_FS_BASE);
    cpu_write_msr(IA32_FS_BASE, 0U);
    if (paging_process_restore_kernel(&process->address_space) !=
            PAGING_STATUS_OK) {
        return 0U;
    }
    without_cycles += tsc_read() - without_started;
    record_context_transition(process, without_cycles, fpu_cycles);
    result = dispatch_syscall(process, thread, frame);
    thread->context.rax = (uint64_t)result;
    ++process->syscall_count;
    resume_stack = process_user_resume_stack();
    return resume_stack;
}

bool native_process_interrupt_active(void)
{
    const struct native_process *process = running_process();

    return process != NULL && process_user_boundary_active() &&
        process->address_space.state == PAGING_PROCESS_SPACE_ACTIVE;
}

static void report_user_backtrace(
    struct native_process *process,
    uint64_t frame_pointer
)
{
    for (size_t depth = 0U; depth < 8U; ++depth) {
        uint64_t words[2];

        if ((frame_pointer & (sizeof(uint64_t) - 1U)) != 0U ||
            !copy_from_user(process, words, frame_pointer, sizeof(words))) {
            break;
        }
        console_write("Sapote: native backtrace ");
        console_write_u64(depth);
        console_write(" frame ");
        console_write_hex(frame_pointer);
        console_write(" return ");
        console_write_hex(words[1]);
        console_write("\n");
        if (words[0] <= frame_pointer) {
            break;
        }
        frame_pointer = words[0];
    }
}

void native_process_on_interrupt(struct interrupt_frame *frame, void *context)
{
    struct native_process *process = running_process();
    struct native_thread *thread = running_thread(process);
    uintptr_t resume_stack = process_user_resume_stack();
    uint64_t without_started;
    uint64_t without_cycles;
    uint64_t fpu_started;
    uint64_t fpu_cycles;
    bool valid = frame != NULL && process != NULL && thread != NULL &&
        context == &native_gate && interrupt_frame_has_stack_tail(frame);

    if (valid) {
        without_started = tsc_read();
        save_interrupt_context(thread, frame);
        without_cycles = tsc_read() - without_started;
        fpu_started = tsc_read();
        valid = native_fpu_save(&thread->fpu);
        fpu_cycles = tsc_read() - fpu_started;
        without_started = tsc_read();
        thread->fs_base = cpu_read_msr(IA32_FS_BASE);
        cpu_write_msr(IA32_FS_BASE, 0U);
        if (paging_process_restore_kernel(&process->address_space) !=
                PAGING_STATUS_OK) {
            valid = false;
        }
        without_cycles += tsc_read() - without_started;
        if (valid) {
            record_context_transition(process, without_cycles, fpu_cycles);
        }
        if (frame->vector < INTERRUPT_EXCEPTION_COUNT) {
            console_write("Sapote: native thread fault vector ");
            console_write_u64(frame->vector);
            console_write(" error ");
            console_write_hex(frame->error_code);
            console_write(" rip ");
            console_write_hex(frame->rip);
            console_write(" address ");
            console_write_hex(frame->cr2);
            console_write(" rsp ");
            console_write_hex(interrupt_frame_stack_pointer(frame));
            console_write(" rax ");
            console_write_hex(frame->rax);
            console_write(" rbx ");
            console_write_hex(frame->rbx);
            console_write(" rbp ");
            console_write_hex(frame->rbp);
            console_write(" rdi ");
            console_write_hex(frame->rdi);
            console_write(" rsi ");
            console_write_hex(frame->rsi);
            console_write("\n");
            report_user_backtrace(process, frame->rbp);
            thread->state = NATIVE_THREAD_FAULTED;
            thread->exit_status = -SAPOTE_EFAULT;
            process->faulted = true;
            terminate_process(process, -SAPOTE_EFAULT);
        }
    }
    if (!valid && process != NULL) {
        process->faulted = true;
        terminate_process(process, -SAPOTE_EIO);
    }
    if (resume_stack == 0U ||
        interrupt_request_kernel_resume(frame, resume_stack) !=
            INTERRUPT_STATUS_OK) {
        console_panic("native process interrupt return failed");
    }
}

static bool process_has_runnable_thread(const struct native_process *process)
{
    for (size_t index = 0U; index < process->thread_count; ++index) {
        if (process->threads[index].state == NATIVE_THREAD_RUNNABLE) {
            return true;
        }
    }
    return false;
}

static bool process_has_live_thread(const struct native_process *process)
{
    for (size_t index = 0U; index < process->thread_count; ++index) {
        if (process->threads[index].state != NATIVE_THREAD_UNUSED &&
            process->threads[index].state != NATIVE_THREAD_EXITED &&
            process->threads[index].state != NATIVE_THREAD_FAULTED) {
            return true;
        }
    }
    return false;
}

static void update_waiting_threads(
    struct native_process *process,
    uint64_t now
)
{
    for (size_t index = 0U; index < process->thread_count; ++index) {
        struct native_thread *thread = &process->threads[index];

        if (thread->state == NATIVE_THREAD_SLEEP_WAIT &&
            now >= thread->deadline_ns) {
            thread->deadline_ns = 0U;
            thread->context.rax = 0U;
            thread->state = NATIVE_THREAD_RUNNABLE;
        } else if (thread->state == NATIVE_THREAD_FUTEX_WAIT &&
            thread->deadline_ns != 0U && now >= thread->deadline_ns) {
            thread->deadline_ns = 0U;
            thread->futex_address = 0U;
            thread->context.rax = (uint64_t)-(int64_t)SAPOTE_ETIMEDOUT;
            thread->state = NATIVE_THREAD_RUNNABLE;
        } else if (thread->state == NATIVE_THREAD_JOIN_WAIT) {
            for (size_t target = 0U; target < process->thread_count; ++target) {
                const struct native_thread *joined = &process->threads[target];

                if (joined->generation == thread->wait_generation &&
                    (joined->state == NATIVE_THREAD_EXITED ||
                        joined->state == NATIVE_THREAD_FAULTED)) {
                    thread->wait_generation = 0U;
                    thread->context.rax = (uint64_t)(int64_t)
                        joined->exit_status;
                    thread->state = NATIVE_THREAD_RUNNABLE;
                    break;
                }
            }
        } else if (thread->state == NATIVE_THREAD_HANDLE_WAIT) {
            int64_t ready = poll_wait_items(process, thread->wait_items,
                thread->wait_item_count);
            const bool timed_out = thread->deadline_ns != 0U &&
                now >= thread->deadline_ns;

            if (ready != 0 || timed_out) {
                if (ready >= 0 && !copy_to_user(process,
                        thread->wait_items_address, thread->wait_items,
                        thread->wait_item_count *
                            sizeof(thread->wait_items[0]))) {
                    ready = -SAPOTE_EFAULT;
                } else if (ready == 0) {
                    ready = -SAPOTE_ETIMEDOUT;
                }
                thread->wait_items_address = 0U;
                thread->wait_item_count = 0U;
                thread->deadline_ns = 0U;
                thread->context.rax = (uint64_t)ready;
                thread->state = NATIVE_THREAD_RUNNABLE;
            }
        } else if (thread->state == NATIVE_THREAD_CONSOLE_WAIT &&
            process->console_input_count != 0U) {
            const size_t copied = console_input_copy(process,
                process->transfer, thread->console_length);

            if (copy_to_user(process, thread->console_address,
                    process->transfer, copied)) {
                console_input_consume(process, copied);
                thread->context.rax = (uint64_t)copied;
            } else {
                thread->context.rax = (uint64_t)-(int64_t)SAPOTE_EFAULT;
            }
            thread->console_address = 0U;
            thread->console_length = 0U;
            thread->state = NATIVE_THREAD_RUNNABLE;
        }
    }
    if (!process->exiting && !process_has_live_thread(process)) {
        terminate_process(process, process->faulted ? -SAPOTE_EFAULT :
            process->exit_status);
    }
}

static bool nearest_deadline(uint64_t *deadline)
{
    bool found = false;

    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        for (size_t thread_index = 0U; thread_index < process->thread_count;
             ++thread_index) {
            const struct native_thread *thread =
                &process->threads[thread_index];

            if ((thread->state == NATIVE_THREAD_SLEEP_WAIT ||
                    thread->state == NATIVE_THREAD_FUTEX_WAIT ||
                    thread->state == NATIVE_THREAD_HANDLE_WAIT) &&
                thread->deadline_ns != 0U &&
                (!found || thread->deadline_ns < *deadline)) {
                *deadline = thread->deadline_ns;
                found = true;
            }
        }
    }
    return found;
}

static bool select_runnable_thread(void)
{
    for (size_t process_offset = 0U; process_offset < NATIVE_PROCESS_LIMIT;
         ++process_offset) {
        const size_t process_index = (scheduler_process_cursor +
            process_offset) % NATIVE_PROCESS_LIMIT;
        struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting ||
            !process_has_runnable_thread(process)) {
            continue;
        }
        for (size_t thread_offset = 1U;
             thread_offset <= process->thread_count; ++thread_offset) {
            const size_t thread_index = (process->current_thread +
                thread_offset) % process->thread_count;

            if (process->threads[thread_index].state ==
                    NATIVE_THREAD_RUNNABLE) {
                current_process = process_index;
                process->current_thread = thread_index;
                scheduler_process_cursor = (process_index + 1U) %
                    NATIVE_PROCESS_LIMIT;
                return true;
            }
        }
    }
    return false;
}

static bool any_active_process(void)
{
    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        if (processes[index].active) {
            return true;
        }
    }
    return false;
}

static size_t newest_active_process(void)
{
    uint64_t generation = 0U;
    size_t selected = SIZE_MAX;

    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        if (processes[index].active &&
            processes[index].generation > generation) {
            generation = processes[index].generation;
            selected = index;
        }
    }
    return selected;
}

static struct native_process *console_input_target(void)
{
    struct native_process *selected = NULL;

    for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
        struct native_process *process = &processes[index];

        if (process->active && !process->exiting &&
            !process->window.allocated &&
            (process->manifest.capabilities & SAPOTE_CAP_CONSOLE) != 0U &&
            (selected == NULL ||
                process->generation > selected->generation)) {
            selected = process;
        }
    }
    return selected;
}

static bool console_input_push(struct native_process *process, uint8_t value)
{
    size_t tail;

    if (process == NULL ||
        process->console_input_count == NATIVE_CONSOLE_INPUT_CAPACITY) {
        return false;
    }
    tail = (process->console_input_head + process->console_input_count) %
        NATIVE_CONSOLE_INPUT_CAPACITY;
    process->console_input[tail] = value;
    ++process->console_input_count;
    return true;
}

static bool any_console_waiter(void)
{
    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        for (size_t thread_index = 0U;
             thread_index < process->thread_count; ++thread_index) {
            if (process->threads[thread_index].state ==
                    NATIVE_THREAD_CONSOLE_WAIT) {
                return true;
            }
        }
    }
    return false;
}

static bool any_handle_waiter(void)
{
    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        for (size_t thread_index = 0U;
             thread_index < process->thread_count; ++thread_index) {
            if (process->threads[thread_index].state ==
                    NATIVE_THREAD_HANDLE_WAIT) {
                return true;
            }
        }
    }
    return false;
}

static void report_scheduler_stall(void)
{
    console_write("Sapote: native scheduler stalled\n");
    for (size_t process_index = 0U; process_index < NATIVE_PROCESS_LIMIT;
         ++process_index) {
        const struct native_process *process = &processes[process_index];

        if (!process->active || process->exiting) {
            continue;
        }
        console_write("Sapote: stalled process ");
        console_write_u64(process_index);
        console_write(" generation ");
        console_write_u64(process->generation);
        console_write(" threads ");
        console_write_u64(process->thread_count);
        console_write("\n");
        for (size_t thread_index = 0U;
             thread_index < process->thread_count; ++thread_index) {
            const struct native_thread *thread =
                &process->threads[thread_index];

            console_write("Sapote: stalled thread ");
            console_write_u64(thread_index);
            console_write(" generation ");
            console_write_u64(thread->generation);
            console_write(" state ");
            console_write_u64((uint64_t)thread->state);
            console_write(" rip ");
            console_write_hex(thread->context.rip);
            console_write(" rsp ");
            console_write_hex(thread->context.rsp);
            console_write(" wait-generation ");
            console_write_u64(thread->wait_generation);
            console_write(" futex ");
            console_write_hex(thread->futex_address);
            console_write(" deadline ");
            console_write_u64(thread->deadline_ns);
            console_write("\n");
        }
    }
}

static void capture_result(
    const struct native_process *process,
    struct native_process_result *result
)
{
    result->generation = process->generation;
    result->exit_status = process->exit_status;
    result->syscall_count = process->syscall_count;
    result->thread_switches = process->thread_switches;
    result->peak_pages = (uint32_t)process->peak_pages;
    result->peak_handles = (uint32_t)process->peak_handles;
    result->context_cycles_without_fpu =
        process->context_cycles_without_fpu;
    result->context_cycles_with_fpu = process->context_cycles_with_fpu;
    result->context_transition_samples =
        process->context_transition_samples;
    result->exited = process->exiting;
    result->faulted = process->faulted;
    result->resources_released = false;
}

static bool service_native_devices(void)
{
    struct keyboard_event event;
    bool success = true;

    cpu_interrupt_enable();
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        struct native_process *target = console_input_target();
        const bool console_event = target != NULL && event.pressed &&
            event.character != '\0';

        if (console_event) {
            /* The bounded byte stream retains older input and drops newest. */
            (void)console_input_push(target, (uint8_t)event.character);
        }
        if (!console_event && ui_handle_keyboard(&event) != UI_STATUS_OK) {
            success = false;
        }
    }
    if (ui_is_active() && (ui_process_events() != UI_STATUS_OK ||
            ui_flush() != UI_STATUS_OK)) {
        success = false;
    }
    (void)network_service();
    cpu_interrupt_disable();
    return success;
}

enum native_process_status native_process_run(struct native_process_result *result)
{
    struct native_process_result selected_result;
    bool have_result = false;
    bool cleanup_ok = true;

    if (result == NULL) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    zero_bytes(&selected_result, sizeof(selected_result));
    if (scheduler_active || !any_active_process()) {
        return NATIVE_PROCESS_BUSY;
    }
    cpu_interrupt_disable();
    if (interrupt_process_gate_arm(native_process_on_interrupt, &native_gate,
            &native_gate) != INTERRUPT_STATUS_OK) {
        cpu_interrupt_enable();
        return NATIVE_PROCESS_GATE;
    }
    if (!native_syscall_arm()) {
        (void)interrupt_process_gate_disarm(&native_gate);
        cpu_interrupt_enable();
        return NATIVE_PROCESS_SYSCALL;
    }
    scheduler_active = true;
    while (any_active_process()) {
        const uint64_t now = clock_monotonic_ns();

        if (!service_native_devices()) {
            cleanup_ok = false;
        }

        for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT; ++index) {
            if (processes[index].active && !processes[index].exiting) {
                update_waiting_threads(&processes[index], now);
            }
        }
        if (select_runnable_thread()) {
            struct native_process *process = &processes[current_process];
            struct native_thread *thread = running_thread(process);
            uint64_t without_started;
            uint64_t without_cycles;
            uint64_t fpu_started;
            uint64_t fpu_cycles;
            enum paging_status activation;

            if (interrupt_process_gate_validate(&native_gate) !=
                    INTERRUPT_STATUS_OK) {
                cleanup_ok = false;
                terminate_process(process, -SAPOTE_EIO);
            } else if (native_gate.state == INTERRUPT_PROCESS_GATE_RETURNED &&
                interrupt_process_gate_rearm(&native_gate) !=
                    INTERRUPT_STATUS_OK) {
                cleanup_ok = false;
                terminate_process(process, -SAPOTE_EIO);
            } else {
                without_started = tsc_read();
                activation = paging_process_activate(&process->address_space);
                without_cycles = tsc_read() - without_started;
                fpu_started = tsc_read();
                if (activation != PAGING_STATUS_OK ||
                    !native_fpu_restore(&thread->fpu)) {
                    cleanup_ok = false;
                    terminate_process(process, -SAPOTE_EIO);
                } else {
                    fpu_cycles = tsc_read() - fpu_started;
                    without_started = tsc_read();
                    cpu_write_msr(IA32_FS_BASE, thread->fs_base);
                    without_cycles += tsc_read() - without_started;
                    record_context_transition(process, without_cycles,
                        fpu_cycles);
                    ++process->thread_switches;
                    process_enter_user_context(&thread->context);
                }
            }
        } else {
            uint64_t deadline;

            /*
             * A handle wait is also a device wait, but its absolute timeout
             * still needs a programmed interrupt.  Service deadlines first so
             * an idle system cannot halt forever after the last device event.
             */
            if (nearest_deadline(&deadline)) {
                const uint64_t current = clock_monotonic_ns();

                if (deadline > current) {
                    uint64_t interval = deadline - current;

                    if (interval < NATIVE_SCHEDULER_MIN_SLEEP_NS) {
                        interval = NATIVE_SCHEDULER_MIN_SLEEP_NS;
                    }
                    cpu_interrupt_enable();
                    (void)timer_sleep_ns(interval);
                    cpu_interrupt_disable();
                }
            } else if (any_handle_waiter() || any_console_waiter()) {
                /* Device and UI interrupts wake an unbounded wait for polling. */
                cpu_enable_and_halt();
                cpu_interrupt_disable();
            } else {
                report_scheduler_stall();
                for (size_t index = 0U; index < NATIVE_PROCESS_LIMIT;
                     ++index) {
                    if (processes[index].active &&
                        !processes[index].exiting) {
                        processes[index].faulted = true;
                        terminate_process(&processes[index], -SAPOTE_EBUSY);
                    }
                }
            }
        }

        for (;;) {
            const size_t newest = newest_active_process();
            struct native_process_result completed;

            if (newest == SIZE_MAX || !processes[newest].exiting) {
                break;
            }
            capture_result(&processes[newest], &completed);
            if (!have_result || completed.generation >
                    selected_result.generation) {
                selected_result = completed;
                have_result = true;
            }
            if (!process_cleanup(&processes[newest])) {
                cleanup_ok = false;
                break;
            }
        }
    }
    current_process = SIZE_MAX;
    cpu_write_msr(IA32_FS_BASE, 0U);
    scheduler_active = false;
    if (!native_syscall_disarm()) {
        cleanup_ok = false;
    }
    if (interrupt_process_gate_disarm(&native_gate) != INTERRUPT_STATUS_OK) {
        cleanup_ok = false;
    }
    cpu_interrupt_enable();
    if (have_result) {
        selected_result.resources_released = native_process_resources_released();
        *result = selected_result;
    }
    return cleanup_ok && have_result ? NATIVE_PROCESS_OK :
        NATIVE_PROCESS_TEARDOWN;
}

enum native_process_status native_process_launch(
    const char *manifest_path,
    struct native_process_result *result
)
{
    uint64_t generation;
    enum native_process_status status;

    if (result == NULL) {
        return NATIVE_PROCESS_NULL_ARGUMENT;
    }
    status = native_process_spawn(manifest_path, &generation);
    if (status != NATIVE_PROCESS_OK) {
        return status;
    }
    status = native_process_run(result);
    if (status == NATIVE_PROCESS_OK && result->generation != generation) {
        return NATIVE_PROCESS_TEARDOWN;
    }
    return status;
}

bool native_process_resources_released(void)
{
    const struct paging_state paging = paging_get_state();

    return !scheduler_active && !any_active_process() &&
        !native_syscall_is_active() && !process_user_boundary_active() &&
        interrupt_process_gate_resources_released() &&
        (!paging.active ||
            (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
                paging.root_physical_address);
}

bool native_process_self_test(size_t *completed_tests)
{
    size_t handle_tests;
    size_t fpu_tests;
    const uint32_t image_tests = sapote_native_image_self_test();

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (image_tests != 12U) {
        return false;
    }
    *completed_tests += image_tests;
    if (!native_handle_self_test(&handle_tests)) {
        return false;
    }
    *completed_tests += handle_tests;
    /* Ordinary boots inspect capabilities without changing CR0/CR4 or FPU state. */
    if (!native_fpu_capability_self_test(&fpu_tests)) {
        return false;
    }
    *completed_tests += fpu_tests;
    if (!process_user_context_layout_self_test() ||
        sizeof(struct native_syscall_frame) != 144U ||
        sizeof(struct sapote_event) != 56U) {
        return false;
    }
    *completed_tests += 3U;
    return true;
}

const char *native_process_status_string(enum native_process_status status)
{
    static const char *const messages[NATIVE_PROCESS_STATUS_COUNT] = {
        "ok",
        "null native-process argument",
        "native scheduler is busy or has no process",
        "native process limit reached",
        "application manifest could not be opened",
        "application manifest could not be read exactly",
        "application executable could not be opened",
        "application executable could not be read exactly",
        "Rust admission refused the manifest or executable",
        "application Data namespace could not be prepared",
        "application memory or handle limit was refused",
        "application frame allocation failed",
        "application address space could not be built",
        "application mapping contract failed",
        "guarded initial stack could not be constructed",
        "native x87/SSE CPU contract is unavailable",
        "native interrupt return gate could not be armed",
        "native syscall boundary could not be armed",
        "native process teardown failed"
    };

    return status < NATIVE_PROCESS_STATUS_COUNT ? messages[status] :
        "unknown native-process status";
}
