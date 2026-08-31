/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/boot_ledger.h>
#include <sapote/camera.h>
#include <sapote/clock.h>
#include <sapote/console.h>
#include <sapote/cpu.h>
#include <sapote/dock3d.h>
#include <sapote/framebuffer.h>
#include <sapote/fat32_fs.h>
#include <sapote/heap.h>
#include <sapote/logo.h>
#include <sapote/memory.h>
#include <sapote/network.h>
#include <sapote/pci.h>
#include <sapote/pointer.h>
#include <sapote/screen.h>
#include <sapote/studio_icon.h>
#include <sapote/thread.h>
#include <sapote/timer.h>
#include <sapote/ui.h>
#include <sapote/ui_anim.h>
#include <sapote/ui_font.h>
#include <sapote/wallpaper.h>

#define UI_MIN_WIDTH 800U
#define UI_MIN_HEIGHT 600U
#define UI_MAX_WIDTH 1920U
#define UI_MAX_HEIGHT 1200U
#define UI_LOGO_WIDTH 280U
#define UI_LOGO_HEIGHT 278U
#define UI_LOGO_PIXELS (UI_LOGO_WIDTH * UI_LOGO_HEIGHT)
#define UI_LOGO_BITMAP_SCALE 1U
#define UI_CURSOR_SOURCE_WIDTH 27U
#define UI_CURSOR_SOURCE_HEIGHT 37U
#define UI_MENU_HEIGHT 24U
#define UI_MENU_TELEMETRY_MAX_WIDTH 384U
#define UI_WORKSPACE_MENU_WIDTH 132U
#define UI_WORKSPACE_MENU_HEIGHT 202U
#define UI_HERO_MAX_WIDTH 640U
#define UI_HERO_HEIGHT 388U
#define UI_HERO_TITLE_HEIGHT 24U
#define UI_DOCK_WIDTH 652U
#define UI_DOCK_ITEM_WIDTH 70U
#define UI_DOCK_ITEM_HEIGHT 98U
#define UI_DOCK_GAP 8U
#define UI_DOCK_PADDING 18U
#define UI_DOCK_HEIGHT 110U
#define UI_FONT_ASCENT 15U
#define UI_FONT_DESCENT 4U
#define UI_PANEL_TITLE_HEIGHT 26U
#define UI_STUDIO_MAX_CLIPS 6U
#define UI_STUDIO_PATH_BYTES 64U
#define UI_STUDIO_PROJECT_BYTES 424U
#define UI_STUDIO_PREVIEW_WIDTH 320U
#define UI_STUDIO_PREVIEW_HEIGHT 180U
#define UI_STUDIO_BMP_HEADER_BYTES 54U
#define UI_STUDIO_BMP_MAX_WIDTH 1920U
#define UI_STUDIO_BMP_MAX_HEIGHT 1080U
#define UI_CAMERA_CAPTURE_WIDTH 320U
#define UI_CAMERA_CAPTURE_HEIGHT 180U
#define UI_CAMERA_BMP_HEADER_BYTES 54U
#define UI_SETTINGS_CATEGORY_COUNT 12U
#define UI_WALLPAPER_COUNT 14U
#define UI_CAMERA_SCENE_WIDTH 704U
#define UI_CAMERA_SCENE_HEIGHT 424U
#define UI_SPRING_FRAME_NS UINT64_C(16000000)
#define UI_REDRAW_DIAGNOSTIC_TILE 16U
#define UI_REDRAW_DIAGNOSTIC_COLUMNS \
    ((UI_MAX_WIDTH + UI_REDRAW_DIAGNOSTIC_TILE - 1U) / \
        UI_REDRAW_DIAGNOSTIC_TILE)
#define UI_REDRAW_DIAGNOSTIC_ROWS \
    ((UI_MAX_HEIGHT + UI_REDRAW_DIAGNOSTIC_TILE - 1U) / \
        UI_REDRAW_DIAGNOSTIC_TILE)
#define UI_LAUNCHER_QUERY_BYTES 24U
#define UI_LAUNCHER_APPS_PER_PAGE 6U
#define UI_LAUNCHER_MAX_PAGES 4U
#define UI_APPLICATION_MANIFEST_BYTES 13U
#define UI_STORE_QUERY_BYTES 32U
#define UI_STORE_NAV_COUNT 13U
#define UI_STORE_ICON_COLUMNS 8U
#define UI_STORE_ICON_SIZE 32U
#define UI_STORE_ICON_SHEET_WIDTH 256U
#define UI_STORE_ICON_SHEET_HEIGHT 64U

static const char label_files[] = "Files";
static const char label_terminal[] = "Terminal";
static const char label_notes[] = "Notes";
static const char label_studio[] = "SapStudio";
static const char label_camera[] = "Camera";
static const char label_canvas[] = "Canvas";
static const char label_store[] = "Store";
static const char label_settings[] = "Settings";

static struct ui_state state;
static struct surface *canvas;
static struct ui_rect panel_home;
static struct ui_rect panel_windows[UI_PANEL_COUNT];
static struct ui_rect panel_restore[UI_PANEL_COUNT];
static struct ui_rect panel_origins[UI_PANEL_COUNT];
static bool panel_origin_valid[UI_PANEL_COUNT];
static bool panel_open[UI_PANEL_COUNT];
static bool panel_minimized[UI_PANEL_COUNT];
static bool panel_maximized[UI_PANEL_COUNT];
static enum ui_panel_id panel_order[UI_PANEL_COUNT - 1U];
static size_t panel_order_count;
static uint8_t panel_cascade;
static struct ui_event event_queue[UI_EVENT_QUEUE_CAPACITY];
static size_t event_count;
static uint32_t logo_pixels[UI_LOGO_PIXELS];
static uint8_t logo_alpha[UI_LOGO_PIXELS];
static uint8_t logo_red_shift;
static uint8_t logo_green_shift;
static uint8_t logo_blue_shift;
static uint32_t studio_icon_pixels[80U * 80U];
static uint8_t studio_icon_alpha[80U * 80U];
static uint32_t studio_icon_width;
static uint32_t studio_icon_height;
static uint32_t settings_icon_pixels[80U * 80U];
static uint8_t settings_icon_alpha[80U * 80U];
static uint32_t settings_icon_width;
static uint32_t settings_icon_height;
static uint32_t files_icon_pixels[80U * 80U];
static uint8_t files_icon_alpha[80U * 80U];
static uint32_t files_icon_width;
static uint32_t files_icon_height;
static uint32_t terminal_icon_pixels[80U * 80U];
static uint8_t terminal_icon_alpha[80U * 80U];
static uint32_t terminal_icon_width;
static uint32_t terminal_icon_height;
static uint32_t camera_icon_pixels[80U * 80U];
static uint8_t camera_icon_alpha[80U * 80U];
static uint32_t camera_icon_width;
static uint32_t camera_icon_height;
static uint32_t canvas_icon_pixels[80U * 80U];
static uint8_t canvas_icon_alpha[80U * 80U];
static uint32_t canvas_icon_width;
static uint32_t canvas_icon_height;
static uint32_t store_icon_pixels[80U * 80U];
static uint8_t store_icon_alpha[80U * 80U];
static uint32_t store_icon_width;
static uint32_t store_icon_height;
static uint32_t store_ui_icon_pixels[
    UI_STORE_ICON_SHEET_WIDTH * UI_STORE_ICON_SHEET_HEIGHT
];
static uint8_t store_ui_icon_alpha[
    UI_STORE_ICON_SHEET_WIDTH * UI_STORE_ICON_SHEET_HEIGHT
];
static uint32_t store_ui_icon_width;
static uint32_t store_ui_icon_height;
static uint32_t settings_category_icon_pixels[256U * 192U];
static uint8_t settings_category_icon_alpha[256U * 192U];
static uint32_t settings_category_icon_width;
static uint32_t settings_category_icon_height;
static uint32_t wallpaper_pixels[1024U * 768U];
static uint32_t camera_scene_pixels[
    UI_CAMERA_SCENE_WIDTH * UI_CAMERA_SCENE_HEIGHT
];
static struct sapfs_list_entry file_entries[12U];
static size_t file_entry_count;
static char file_directory[SAPFS_MAX_PATH + 1U] = ".";
static char note_path[SAPFS_MAX_PATH + 1U] = "NOTES.TXT";
static char note_buffer[1536U];
static size_t note_length;
static bool note_dirty;
static bool note_savable = true;
static bool terminal_welcomed;
static char app_status[80U] = "data volume ready";
static uint32_t studio_clip_durations[UI_STUDIO_MAX_CLIPS];
static char studio_clip_paths[UI_STUDIO_MAX_CLIPS][UI_STUDIO_PATH_BYTES + 1U];
static uint32_t studio_preview_pixels[
    UI_STUDIO_PREVIEW_WIDTH * UI_STUDIO_PREVIEW_HEIGHT
];
static uint8_t studio_bmp_row[UI_STUDIO_BMP_MAX_WIDTH * 3U + 4U];
static uint32_t studio_preview_width;
static uint32_t studio_preview_height;
static bool studio_preview_loaded;
static uint32_t studio_playhead;
static uint8_t studio_clip_count;
static uint8_t studio_selected_clip = UINT8_MAX;
static bool studio_dirty;
static char studio_status[64U] = "Project ready";
static int8_t settings_page = -1;
static bool dock_dark;
static bool dock_magnification = true;
static bool dock_reflections = true;
static bool dock_labels = true;
static bool menu_glass = true;
static bool launcher_open;
static bool launcher_search_focused;
static char launcher_query[UI_LAUNCHER_QUERY_BYTES];
static size_t launcher_query_length;
static size_t launcher_page;
static char application_launch_path[UI_APPLICATION_MANIFEST_BYTES];
static uint8_t store_section;
static bool store_search_focused;
static char store_query[UI_STORE_QUERY_BYTES];
static size_t store_query_length;
static bool window_high_contrast;
static bool keyboard_focus_wrap = true;
static bool keyboard_focus_indicator = true;
static bool cursor_large;
static bool cursor_dark = true;
static bool window_motion = true;
static bool window_shadows = true;
static bool window_bevels = true;
static bool window_title_gradient = true;
static uint8_t desktop_wallpaper;
static uint32_t camera_capture_count;
static bool camera_frame_available;
static uint64_t camera_seen_generation;
static char camera_status[64U] = "No camera connected";
static uint8_t camera_bmp_row[UI_CAMERA_CAPTURE_WIDTH * 3U];
static uint32_t camera_preview_row[UI_MAX_WIDTH];
static uint32_t settings_wallpaper_thumbnail_pixels[128U * 72U];
static uint32_t dock_backdrop_pixels[UI_MAX_WIDTH * 48U];
static uint64_t redraw_tile_hashes[
    UI_REDRAW_DIAGNOSTIC_COLUMNS * UI_REDRAW_DIAGNOSTIC_ROWS
];
enum ui_anim_pending {
    UI_ANIM_PENDING_NONE = 0,
    UI_ANIM_PENDING_OPEN,
    UI_ANIM_PENDING_CLOSE
};

static struct ui_anim panel_anim;
static enum ui_panel_id panel_anim_panel;
static struct ui_rect panel_anim_origin;
static struct ui_rect panel_anim_frame;
static enum ui_anim_pending panel_anim_pending;
static bool panel_anim_driver;
static struct ui_rect pending_native_origin;
static bool pending_native_origin_valid;
static volatile uint64_t motion_timer_id;
static volatile bool dock_spring_active;
static uint64_t dock_spring_last_ns;
static struct dock3d_state dock_model;
static bool panel_drag_active;
static enum ui_panel_id panel_drag_panel;
static struct ui_point panel_drag_anchor;
static struct ui_rect panel_drag_origin;
static const char *self_test_failure = "Sapote Redwood UI self-test not run";
static const char *event_queue_failure = "UI event queue self-test not run";
static const char *installed_proof_failure =
    "Sapote Redwood installed proof not run";

struct ui_native_window_record {
    char title[UI_NATIVE_TITLE_BYTES];
    const uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    ui_native_event_fn event_handler;
    void *context;
    bool active;
    bool pointer_capture;
};

static struct ui_native_window_record
    native_windows[UI_NATIVE_WINDOW_COUNT];

static enum ui_status draw_button(
    struct ui_rect button,
    struct ui_rect damage,
    const char *label
);
static enum ui_status draw_circle(
    uint32_t center_x,
    uint32_t center_y,
    uint32_t radius,
    struct ui_rect damage,
    uint32_t pixel
);
static struct ui_rect camera_preview_rect(void);
static enum ui_status render_region(struct ui_rect damage, bool full_draw);

static bool native_panel_slot(enum ui_panel_id panel, uint32_t *slot)
{
    if (panel < UI_PANEL_NATIVE_0 || panel > UI_PANEL_NATIVE_3) {
        return false;
    }
    if (slot != NULL) {
        *slot = (uint32_t)(panel - UI_PANEL_NATIVE_0);
    }
    return true;
}

static void native_event_emit(
    enum ui_panel_id panel,
    const struct ui_native_event *event
)
{
    uint32_t slot;

    if (event == NULL || !native_panel_slot(panel, &slot) ||
        !native_windows[slot].active ||
        native_windows[slot].event_handler == NULL) {
        return;
    }
    native_windows[slot].event_handler(slot, event,
        native_windows[slot].context);
}

static void native_focus_emit(enum ui_panel_id panel, bool focused)
{
    const struct ui_native_event event = {
        .type = UI_NATIVE_EVENT_FOCUS,
        .monotonic_ns = clock_monotonic_ns(),
        .value = focused ? 1U : 0U
    };

    native_event_emit(panel, &event);
}

static void motion_timer_wake(uint64_t deadline_ns, void *context);

static bool motion_schedule_wake(uint64_t now)
{
    uint64_t identifier = 0U;
    const uint64_t deadline = now + UI_ANIM_FRAME_NS;

    if (deadline < now || timer_arm(deadline, motion_timer_wake,
            NULL, &identifier) != TIMER_STATUS_OK) {
        return false;
    }
    motion_timer_id = identifier;
    return true;
}

static void motion_timer_wake(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;
    (void)context;
    motion_timer_id = 0U;
    if (ui_anim_running(&panel_anim) || dock_spring_active) {
        /* The monotonic clock owns progress.  This timer only wakes the idle
         * shell so the next due frame is presented; missing one never changes
         * the final pose or leaves a resource behind. */
        (void)motion_schedule_wake(clock_monotonic_ns());
    }
}

static const uint32_t cursor_outer[UI_CURSOR_SOURCE_HEIGHT] = {
    UINT32_C(0xE0000000), UINT32_C(0xF0000000), UINT32_C(0xFC000000),
    UINT32_C(0xFE000000), UINT32_C(0xFF000000), UINT32_C(0xFF800000),
    UINT32_C(0xFFE00000), UINT32_C(0xFFF00000), UINT32_C(0xFFF80000),
    UINT32_C(0xFFFE0000), UINT32_C(0xFFFF0000), UINT32_C(0xFFFF8000),
    UINT32_C(0xFFFFE000), UINT32_C(0xFFFFF000), UINT32_C(0xFFFFF800),
    UINT32_C(0xFFFFFC00), UINT32_C(0xFFFFFF00), UINT32_C(0xFFFFFF80),
    UINT32_C(0xFFFFFFC0), UINT32_C(0xFFFFFFC0), UINT32_C(0xFFFFFFC0),
    UINT32_C(0xFFFE0000), UINT32_C(0xFFFF0000), UINT32_C(0xFFFF0000),
    UINT32_C(0xFFFF8000), UINT32_C(0xFDFF8000), UINT32_C(0xF9FFC000),
    UINT32_C(0xF0FFE000), UINT32_C(0xE0FFE000), UINT32_C(0xC07FF000),
    UINT32_C(0x007FF000), UINT32_C(0x003FF800), UINT32_C(0x001FF800),
    UINT32_C(0x001FF800), UINT32_C(0x000FF000), UINT32_C(0x000FC000),
    UINT32_C(0x00070000)
};
static const uint32_t cursor_inner[UI_CURSOR_SOURCE_HEIGHT] = {
    UINT32_C(0x80000000), UINT32_C(0xC0000000), UINT32_C(0xE0000000),
    UINT32_C(0xF8000000), UINT32_C(0xFC000000), UINT32_C(0xFE000000),
    UINT32_C(0xFF000000), UINT32_C(0xFFC00000), UINT32_C(0xFFE00000),
    UINT32_C(0xFFF00000), UINT32_C(0xFFFC0000), UINT32_C(0xFFFE0000),
    UINT32_C(0xFFFF0000), UINT32_C(0xFFFFC000), UINT32_C(0xFFFFE000),
    UINT32_C(0xFFFFF000), UINT32_C(0xFFFFF800), UINT32_C(0xFFFFFE00),
    UINT32_C(0xFFFFFF00), UINT32_C(0xFFFFFF80), UINT32_C(0xFFF80000),
    UINT32_C(0xFFFC0000), UINT32_C(0xFFFC0000), UINT32_C(0xFDFE0000),
    UINT32_C(0xF8FE0000), UINT32_C(0xF0FF0000), UINT32_C(0xE07F0000),
    UINT32_C(0xC07F8000), UINT32_C(0x803FC000), UINT32_C(0x003FC000),
    UINT32_C(0x001FE000), UINT32_C(0x000FE000), UINT32_C(0x000FF000),
    UINT32_C(0x0007E000), UINT32_C(0x00078000), UINT32_C(0x00020000),
    UINT32_C(0x00000000)
};

static bool add_u32(uint32_t left, uint32_t right, uint32_t *sum)
{
    if (sum == NULL || left > UINT32_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static bool rect_end(struct ui_rect rectangle, uint32_t *right, uint32_t *bottom)
{
    return rectangle.width != 0U && rectangle.height != 0U &&
        add_u32(rectangle.x, rectangle.width, right) &&
        add_u32(rectangle.y, rectangle.height, bottom);
}

static struct ui_rect drop_shadow_bounds(
    struct ui_rect rectangle,
    uint32_t offset
)
{
    uint32_t width;
    uint32_t height;

    if (!add_u32(rectangle.width, offset, &width) ||
        !add_u32(rectangle.height, offset, &height)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ rectangle.x, rectangle.y, width, height };
}

static struct ui_rect drop_shadow_draw_rect(
    struct ui_rect rectangle,
    uint32_t offset
)
{
    uint32_t x;
    uint32_t y;

    if (!add_u32(rectangle.x, offset, &x) ||
        !add_u32(rectangle.y, offset, &y)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x, y, rectangle.width, rectangle.height };
}

static bool rect_contains_point(struct ui_rect rectangle, struct ui_point point)
{
    uint32_t right;
    uint32_t bottom;

    return point.x >= 0 && point.y >= 0 &&
        rect_end(rectangle, &right, &bottom) &&
        (uint32_t)point.x >= rectangle.x && (uint32_t)point.x < right &&
        (uint32_t)point.y >= rectangle.y && (uint32_t)point.y < bottom;
}

static bool rects_intersect(struct ui_rect left, struct ui_rect right)
{
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    return rect_end(left, &left_right, &left_bottom) &&
        rect_end(right, &right_right, &right_bottom) &&
        left.x < right_right && right.x < left_right &&
        left.y < right_bottom && right.y < left_bottom;
}

static struct ui_rect rect_intersection(struct ui_rect left, struct ui_rect right)
{
    struct ui_rect result = { 0U, 0U, 0U, 0U };
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    if (!rects_intersect(left, right) ||
        !rect_end(left, &left_right, &left_bottom) ||
        !rect_end(right, &right_right, &right_bottom)) {
        return result;
    }
    result.x = left.x > right.x ? left.x : right.x;
    result.y = left.y > right.y ? left.y : right.y;
    const uint32_t end_x = left_right < right_right ? left_right : right_right;
    const uint32_t end_y = left_bottom < right_bottom ? left_bottom : right_bottom;
    result.width = end_x - result.x;
    result.height = end_y - result.y;
    return result;
}

static struct ui_rect rect_union(struct ui_rect left, struct ui_rect right)
{
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    if (left.width == 0U || left.height == 0U) {
        return right;
    }
    if (right.width == 0U || right.height == 0U) {
        return left;
    }
    if (!rect_end(left, &left_right, &left_bottom) ||
        !rect_end(right, &right_right, &right_bottom)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    const uint32_t start_x = left.x < right.x ? left.x : right.x;
    const uint32_t start_y = left.y < right.y ? left.y : right.y;
    const uint32_t end_x = left_right > right_right ? left_right : right_right;
    const uint32_t end_y = left_bottom > right_bottom ? left_bottom : right_bottom;
    return (struct ui_rect){ start_x, start_y, end_x - start_x, end_y - start_y };
}

static struct surface_rect surface_rect_of(struct ui_rect rectangle)
{
    return (struct surface_rect){
        rectangle.x, rectangle.y, rectangle.width, rectangle.height
    };
}

static uint32_t dock_fixed_pixel(int32_t value)
{
    return value <= 0 ? 0U : dock3d_round_pixel(value);
}

static void dock_sync_layout(void)
{
    const uint32_t surface_width = state.layout.surface.width;
    const uint32_t surface_height = state.layout.surface.height;
    const int32_t resting_cell = dock_model.icon + dock_model.gap;
    const int32_t resting_width = resting_cell *
        (int32_t)UI_DOCK_ITEM_COUNT;
    const int32_t resting_x =
        (int32_t)(surface_width * DOCK3D_ONE / 2U) - resting_width / 2;
    const uint32_t stable_hit_top = dock_fixed_pixel(dock_model.baseline -
        (int32_t)((int64_t)dock_model.icon * dock_model.magnification /
            DOCK3D_ONE));
    uint32_t visual_top = surface_height;

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct dock3d_item_state *model = &dock_model.items[index];
        const int32_t base_size = (int32_t)(((int64_t)dock_model.icon *
            model->scale) / DOCK3D_ONE);
        const int32_t width_factor = DOCK3D_ONE + (int32_t)(
            (int64_t)model->press * 3277 / DOCK3D_ONE);
        const int32_t height_factor = DOCK3D_ONE - (int32_t)(
            (int64_t)model->press * 5243 / DOCK3D_ONE);
        const int32_t width_fixed = (int32_t)(
            (int64_t)base_size * width_factor / DOCK3D_ONE);
        const int32_t height_fixed = (int32_t)(
            (int64_t)base_size * height_factor / DOCK3D_ONE);
        const int32_t x_fixed = model->center_x - width_fixed / 2;
        const int32_t y_fixed = dock_model.baseline - height_fixed +
            dock3d_bounce_offset(&dock_model, index);
        const int32_t hit_x_fixed = resting_x +
            (int32_t)index * resting_cell;
        const int32_t hit_right_fixed = hit_x_fixed + resting_cell;
        uint32_t icon_x = dock_fixed_pixel(x_fixed);
        uint32_t icon_y = dock_fixed_pixel(y_fixed);
        uint32_t icon_width = dock_fixed_pixel(width_fixed);
        uint32_t icon_height = dock_fixed_pixel(height_fixed);
        uint32_t hit_x = dock_fixed_pixel(hit_x_fixed);
        uint32_t hit_right = dock_fixed_pixel(hit_right_fixed);
        uint32_t hit_width = hit_right > hit_x ? hit_right - hit_x : 1U;

        if (icon_x >= surface_width) {
            icon_x = surface_width - 1U;
        }
        if (icon_y >= surface_height) {
            icon_y = surface_height - 1U;
        }
        if (icon_width > surface_width - icon_x) {
            icon_width = surface_width - icon_x;
        }
        if (icon_height > surface_height - icon_y) {
            icon_height = surface_height - icon_y;
        }
        if (hit_x >= surface_width) {
            hit_x = surface_width - 1U;
        }
        if (hit_right > surface_width) {
            hit_right = surface_width;
        }
        hit_width = hit_right > hit_x ? hit_right - hit_x : 1U;
        if (hit_width > surface_width - hit_x) {
            hit_width = surface_width - hit_x;
        }
        state.layout.dock_items[index].icon_bounds = (struct ui_rect){
            icon_x, icon_y, icon_width, icon_height
        };
        state.layout.dock_items[index].bounds = (struct ui_rect){
            hit_x, stable_hit_top, hit_width,
            surface_height - stable_hit_top
        };
        if (icon_y < visual_top) {
            visual_top = icon_y;
        }
    }

    const uint32_t panel_x = dock_fixed_pixel(dock_model.panel_x);
    const uint32_t panel_y = dock_fixed_pixel(dock_model.panel_y);
    uint32_t panel_width = dock_fixed_pixel(dock_model.panel_width);
    const uint32_t padding = dock_fixed_pixel(dock_model.icon) / 2U + 8U;
    uint32_t dock_left = panel_x > padding ? panel_x - padding : 0U;
    uint32_t dock_right = panel_x + panel_width + padding;
    uint32_t dock_top = visual_top > 62U ? visual_top - 62U : 0U;

    if (dock_right > surface_width) {
        dock_right = surface_width;
    }
    if (panel_width > surface_width - panel_x) {
        panel_width = surface_width - panel_x;
    }
    if (panel_y < dock_top) {
        dock_top = panel_y;
    }
    state.layout.dock = (struct ui_rect){
        dock_left, dock_top, dock_right - dock_left,
        surface_height - dock_top
    };
}

static struct ui_rect dock_bounds_for(
    const struct ui_layout *layout,
    enum ui_element_id element
)
{
    if (layout == NULL || element < UI_ELEMENT_DOCK_FILES ||
        element > UI_ELEMENT_DOCK_SETTINGS) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    const struct ui_rect item =
        layout->dock_items[(size_t)element - 1U].bounds;
    const uint32_t left = item.x >= 8U ? item.x - 8U : 0U;
    const uint32_t top = item.y >= 26U ? item.y - 26U : 0U;
    uint32_t right = item.x + item.width + 8U;
    const uint32_t bottom = layout->dock.y + layout->dock.height;

    if (right > layout->surface.width) {
        right = layout->surface.width;
    }
    /* Eight pixels cover the largest hover expansion.  On the two outer
     * items that also reaches the corresponding tapered shelf edge, avoiding
     * stale strips without redrawing all four icons for every hover packet. */
    return (struct ui_rect){ left, top, right - left, bottom - top };
}

static struct ui_rect dock_visual_bounds(const struct ui_layout *layout)
{
    if (layout == NULL) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    const uint32_t top = layout->dock.y > 12U ? layout->dock.y - 12U : 0U;

    return (struct ui_rect){ 0U, top, layout->surface.width,
        layout->surface.height - top };
}

static enum ui_panel_id panel_for_element(enum ui_element_id element)
{
    if (element < UI_ELEMENT_DOCK_FILES ||
        element > UI_ELEMENT_DOCK_SETTINGS) {
        return UI_PANEL_NONE;
    }
    return state.layout.dock_items[(size_t)element - 1U].panel;
}

static bool dock_index_for_panel(enum ui_panel_id panel, size_t *index)
{
    if (index == NULL || panel <= UI_PANEL_NONE || panel >= UI_PANEL_COUNT) {
        return false;
    }
    for (size_t candidate = 0U; candidate < UI_DOCK_ITEM_COUNT;
         ++candidate) {
        if (state.layout.dock_items[candidate].panel == panel) {
            *index = candidate;
            return true;
        }
    }
    return false;
}

static struct ui_rect default_panel_origin(void)
{
    const uint32_t width = 58U;
    const uint32_t height = 58U;
    const uint32_t x = state.layout.dock.x +
        (state.layout.dock.width > width ?
            (state.layout.dock.width - width) / 2U : 0U);
    const uint32_t y = state.layout.dock.y + 18U;

    return (struct ui_rect){ x, y, width, height };
}

static struct ui_rect origin_for_panel(enum ui_panel_id panel, bool opening)
{
    size_t dock_index;

    if (dock_index_for_panel(panel, &dock_index)) {
        return state.layout.dock_items[dock_index].icon_bounds;
    }
    if (opening && pending_native_origin_valid) {
        pending_native_origin_valid = false;
        return pending_native_origin;
    }
    if (panel > UI_PANEL_NONE && panel < UI_PANEL_COUNT &&
            panel_origin_valid[panel]) {
        return panel_origins[panel];
    }
    return default_panel_origin();
}

static void install_panel_geometry(enum ui_panel_id panel)
{
    const struct ui_rect window = panel > UI_PANEL_NONE &&
        panel < UI_PANEL_COUNT ? panel_windows[panel] : panel_home;

    state.layout.panel = window;
    state.layout.panel_client = (struct ui_rect){
        window.x + 10U, window.y + 38U,
        window.width - 20U, window.height - 48U
    };
    state.layout.panel_title_baseline = window.y + 22U;
    state.layout.panel_text_baseline = state.layout.panel_client.y +
        UI_FONT_ASCENT;
}

static void remove_panel_from_order(enum ui_panel_id panel)
{
    for (size_t index = 0U; index < panel_order_count; ++index) {
        if (panel_order[index] != panel) {
            continue;
        }
        for (size_t next = index + 1U; next < panel_order_count; ++next) {
            panel_order[next - 1U] = panel_order[next];
        }
        --panel_order_count;
        return;
    }
}

static void bring_panel_to_front(enum ui_panel_id panel)
{
    remove_panel_from_order(panel);
    if (panel_order_count < UI_PANEL_COUNT - 1U) {
        panel_order[panel_order_count++] = panel;
    }
}

static enum ui_panel_id front_panel(void)
{
    return panel_order_count == 0U ? UI_PANEL_NONE :
        panel_order[panel_order_count - 1U];
}

static enum ui_panel_id panel_at_point(struct ui_point point)
{
    for (size_t index = panel_order_count; index > 0U; --index) {
        const enum ui_panel_id panel = panel_order[index - 1U];

        if (panel_open[panel] &&
                rect_contains_point(panel_windows[panel], point)) {
            return panel;
        }
    }
    return UI_PANEL_NONE;
}

static bool baseline_fits(struct ui_rect rectangle, uint32_t baseline)
{
    uint32_t bottom;
    uint32_t right;

    return rect_end(rectangle, &right, &bottom) &&
        baseline >= rectangle.y + UI_FONT_ASCENT &&
        baseline <= UINT32_MAX - UI_FONT_DESCENT &&
        baseline + UI_FONT_DESCENT <= bottom;
}

enum ui_status ui_layout_build(
    uint32_t width,
    uint32_t height,
    struct ui_layout *layout
)
{
    uint32_t panel_width;
    uint32_t panel_height;

    if (layout == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (width < UI_MIN_WIDTH || height < UI_MIN_HEIGHT ||
        width > UI_MAX_WIDTH || height > UI_MAX_HEIGHT) {
        return UI_STATUS_UNSUPPORTED_GEOMETRY;
    }
    *layout = (struct ui_layout){ 0 };
    layout->surface = (struct ui_rect){ 0U, 0U, width, height };
    layout->menu_bar = (struct ui_rect){ 0U, 0U, width, UI_MENU_HEIGHT };
    layout->workspace_bar = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->menu_baseline = 17U;
    layout->hero_window = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->logo = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->wordmark = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->motto = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->version_label = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->dock = (struct ui_rect){
        (width - UI_DOCK_WIDTH) / 2U, height - UI_DOCK_HEIGHT,
        UI_DOCK_WIDTH, UI_DOCK_HEIGHT
    };

    static const enum ui_element_id ids[UI_DOCK_ITEM_COUNT] = {
        UI_ELEMENT_DOCK_FILES, UI_ELEMENT_DOCK_TERMINAL,
        UI_ELEMENT_DOCK_NOTES, UI_ELEMENT_DOCK_STUDIO,
        UI_ELEMENT_DOCK_CAMERA, UI_ELEMENT_DOCK_CANVAS,
        UI_ELEMENT_DOCK_STORE,
        UI_ELEMENT_DOCK_SETTINGS
    };
    static const char *const labels[UI_DOCK_ITEM_COUNT] = {
        label_files, label_terminal, label_notes, label_studio,
        label_camera, label_canvas, label_store, label_settings
    };
    static const enum ui_action actions[UI_DOCK_ITEM_COUNT] = {
        UI_ACTION_OPEN_FILES, UI_ACTION_OPEN_TERMINAL, UI_ACTION_OPEN_NOTES,
        UI_ACTION_OPEN_STUDIO, UI_ACTION_OPEN_CAMERA, UI_ACTION_OPEN_CANVAS,
        UI_ACTION_OPEN_STORE,
        UI_ACTION_OPEN_SETTINGS
    };
    static const enum ui_panel_id panels[UI_DOCK_ITEM_COUNT] = {
        UI_PANEL_FILES, UI_PANEL_TERMINAL, UI_PANEL_NOTES, UI_PANEL_STUDIO,
        UI_PANEL_CAMERA, UI_PANEL_NONE, UI_PANEL_STORE, UI_PANEL_SETTINGS
    };

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const uint32_t x = layout->dock.x + UI_DOCK_PADDING +
            (uint32_t)index * (UI_DOCK_ITEM_WIDTH + UI_DOCK_GAP);

        layout->dock_items[index] = (struct ui_dock_item){
            .id = ids[index],
            .label = labels[index],
            .bounds = { x, layout->dock.y + 1U,
                UI_DOCK_ITEM_WIDTH, UI_DOCK_ITEM_HEIGHT },
            .icon_bounds = { x + 6U, layout->dock.y + 18U, 58U, 58U },
            .action = actions[index],
            .panel = panels[index]
        };
    }
    layout->dock_label_baseline = layout->dock.y + 16U;

    panel_width = width - 80U < 860U ? width - 80U : 860U;
    panel_height = layout->dock.y - 56U;
    layout->panel = (struct ui_rect){
        (width - panel_width) / 2U, 40U,
        panel_width, panel_height
    };
    layout->panel_client = (struct ui_rect){
        layout->panel.x + 10U, layout->panel.y + 38U,
        panel_width - 20U, panel_height - 48U
    };
    layout->panel_title_baseline = layout->panel.y + 22U;
    layout->panel_text_baseline = layout->panel_client.y + UI_FONT_ASCENT;

    return ui_layout_validate(layout);
}

static enum ui_status validate_rect(
    struct ui_rect rectangle,
    struct ui_rect surface
)
{
    uint32_t right;
    uint32_t bottom;
    uint32_t surface_right;
    uint32_t surface_bottom;

    if (!rect_end(rectangle, &right, &bottom)) {
        return UI_STATUS_RECTANGLE_OVERFLOW;
    }
    if (!rect_end(surface, &surface_right, &surface_bottom)) {
        return UI_STATUS_RECTANGLE_OVERFLOW;
    }
    if (rectangle.x < surface.x || rectangle.y < surface.y ||
        right > surface_right || bottom > surface_bottom) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    return UI_STATUS_OK;
}

enum ui_status ui_layout_validate(const struct ui_layout *layout)
{
    if (layout == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (layout->surface.width < UI_MIN_WIDTH ||
        layout->surface.height < UI_MIN_HEIGHT) {
        return UI_STATUS_UNSUPPORTED_GEOMETRY;
    }

    const struct ui_rect rectangles[] = {
        layout->surface, layout->menu_bar, layout->dock, layout->panel,
        layout->panel_client,
        drop_shadow_bounds(layout->panel, 6U)
    };
    for (size_t index = 0U; index < sizeof(rectangles) / sizeof(rectangles[0]);
         ++index) {
        const enum ui_status status =
            validate_rect(rectangles[index], layout->surface);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    if (layout->panel_client.width == 0U ||
        layout->panel_client.height == 0U) {
        return UI_STATUS_EMPTY_PANEL_CLIENT;
    }

    bool seen[UI_ELEMENT_COUNT] = { false };
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct ui_dock_item *item = &layout->dock_items[index];

        if (item->id <= UI_ELEMENT_NONE || item->id >= UI_ELEMENT_COUNT ||
            item->action <= UI_ACTION_NONE || item->action >= UI_ACTION_COUNT ||
            item->panel >= UI_PANEL_COUNT ||
            (item->panel == UI_PANEL_NONE &&
                item->action != UI_ACTION_OPEN_CANVAS) ||
            item->label == NULL) {
            return UI_STATUS_BAD_ELEMENT;
        }
        if (seen[item->id]) {
            return UI_STATUS_DUPLICATE_ELEMENT_ID;
        }
        seen[item->id] = true;
        /*
         * A magnified 3D Dock icon deliberately grows beyond its stable hit
         * lane.  Both rectangles must remain inside the Dock envelope, but
         * the artwork is not required to remain inside the (non-overlapping)
         * input lane.  Requiring that containment made the installed proof
         * reject every legitimate live hover frame.
         */
        if (validate_rect(item->bounds, layout->dock) != UI_STATUS_OK ||
            validate_rect(item->icon_bounds, layout->dock) != UI_STATUS_OK) {
            return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
        }
        for (size_t other = index + 1U; other < UI_DOCK_ITEM_COUNT; ++other) {
            if (rects_intersect(item->bounds,
                    layout->dock_items[other].bounds)) {
                return UI_STATUS_DOCK_OVERLAP;
            }
        }
    }
    for (enum ui_element_id id = UI_ELEMENT_DOCK_FILES;
         id <= UI_ELEMENT_DOCK_SETTINGS; id = (enum ui_element_id)(id + 1)) {
        if (!seen[id]) {
            return UI_STATUS_BAD_ELEMENT;
        }
    }

    if (!baseline_fits(layout->menu_bar, layout->menu_baseline) ||
        !baseline_fits(layout->panel, layout->panel_title_baseline) ||
        !baseline_fits(layout->panel_client, layout->panel_text_baseline)) {
        return UI_STATUS_TEXT_BASELINE_OUT_OF_BOUNDS;
    }
    if (UI_CURSOR_HOTSPOT_X >= UI_CURSOR_WIDTH ||
        UI_CURSOR_HOTSPOT_Y >= UI_CURSOR_HEIGHT) {
        return UI_STATUS_BAD_CURSOR_HOTSPOT;
    }
    return UI_STATUS_OK;
}

enum ui_status ui_hit_test(
    const struct ui_layout *layout,
    struct ui_point point,
    enum ui_element_id *element
)
{
    enum ui_element_id hit = UI_ELEMENT_NONE;

    if (layout == NULL || element == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (!rect_contains_point(layout->dock_items[index].bounds, point)) {
            continue;
        }
        if (hit != UI_ELEMENT_NONE) {
            return UI_STATUS_HIT_TEST_AMBIGUOUS;
        }
        hit = layout->dock_items[index].id;
    }
    *element = hit;
    return UI_STATUS_OK;
}

static void install_theme(struct ui_theme *theme)
{
    theme->white = framebuffer_pack(0xF8U, 0xFAU, 0xF8U);
    theme->ink = framebuffer_pack(0x18U, 0x21U, 0x24U);
    theme->desktop_dark = framebuffer_pack(0x07U, 0x16U, 0x22U);
    theme->desktop_light = framebuffer_pack(0x1CU, 0x4BU, 0x5AU);
    theme->title_active = framebuffer_pack(0x1CU, 0x29U, 0x2DU);
    theme->title_inactive = framebuffer_pack(0x91U, 0x9DU, 0xA2U);
    theme->accent_teal = framebuffer_pack(0x68U, 0xA9U, 0xC5U);
    theme->accent_gold = framebuffer_pack(0xE6U, 0xC4U, 0x62U);
    theme->accent_green = framebuffer_pack(0x8EU, 0xADU, 0x89U);
    theme->accent_red = framebuffer_pack(0xD9U, 0x55U, 0x4FU);
    theme->accent_violet = framebuffer_pack(0x94U, 0x7BU, 0xB4U);
    theme->shadow = framebuffer_pack(0x05U, 0x0CU, 0x12U);
    theme->window_face = framebuffer_pack(0xD9U, 0xDFU, 0xE0U);
}

static enum ui_status fill_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t pixel
)
{
    const struct ui_rect clipped = rect_intersection(rectangle, damage);

    if (clipped.width == 0U || clipped.height == 0U) {
        return UI_STATUS_OK;
    }
    return surface_fill_rect(canvas, surface_rect_of(clipped), pixel) ==
        SURFACE_STATUS_OK ? UI_STATUS_OK : UI_STATUS_SURFACE_FAILURE;
}

static enum ui_status draw_wallpaper(struct ui_rect damage)
{
    const struct ui_rect clipped = rect_intersection(state.layout.surface,
        damage);

    if (clipped.width == 0U || clipped.height == 0U) {
        return UI_STATUS_OK;
    }
    if (state.layout.surface.width == 1024U &&
            state.layout.surface.height == 768U) {
        const uint32_t *source = &wallpaper_pixels[
            (size_t)clipped.y * 1024U + clipped.x
        ];
        return surface_blit(canvas, clipped.x, clipped.y, source,
            clipped.width, clipped.height,
            1024U * SURFACE_BYTES_PER_PIXEL) == SURFACE_STATUS_OK ?
                UI_STATUS_OK : UI_STATUS_SURFACE_FAILURE;
    }

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t target_y = clipped.y + y;
        const uint32_t source_y = target_y * 768U /
            state.layout.surface.height;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t target_x = clipped.x + x;
            const uint32_t source_x = target_x * 1024U /
                state.layout.surface.width;

            if (surface_pixel(canvas, target_x, target_y,
                    wallpaper_pixels[source_y * 1024U + source_x]) !=
                SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static uint8_t packed_channel(uint32_t pixel, uint8_t shift)
{
    return (uint8_t)((pixel >> shift) & 0xFFU);
}

static uint32_t blend_packed(uint32_t under, uint32_t over, uint8_t alpha)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    const uint32_t inverse = 255U - alpha;
    const uint8_t red = (uint8_t)((
        (uint32_t)packed_channel(over, framebuffer.red_position) * alpha +
        (uint32_t)packed_channel(under, framebuffer.red_position) * inverse +
        127U) / 255U);
    const uint8_t green = (uint8_t)((
        (uint32_t)packed_channel(over, framebuffer.green_position) * alpha +
        (uint32_t)packed_channel(under, framebuffer.green_position) * inverse +
        127U) / 255U);
    const uint8_t blue = (uint8_t)((
        (uint32_t)packed_channel(over, framebuffer.blue_position) * alpha +
        (uint32_t)packed_channel(under, framebuffer.blue_position) * inverse +
        127U) / 255U);

    return framebuffer_pack(red, green, blue);
}

static bool search_stroke_sample(
    int64_t x,
    int64_t y,
    int64_t size
)
{
    /*
     * Rasterize Lucide's pinned 24-unit search.svg at four samples per pixel.
     * Coordinates use eighth-pixel units, preserving its two-unit round stroke
     * without a PNG decoder or a large bitmap in the kernel.
     */
    const int64_t center = 11 * size / 3;
    const int64_t radius = 8 * size / 3;
    const int64_t half_stroke = size / 3;
    const int64_t circle_x = x - center;
    const int64_t circle_y = y - center;
    const int64_t distance_squared = circle_x * circle_x +
        circle_y * circle_y;
    const int64_t inner = radius - half_stroke;
    const int64_t outer = radius + half_stroke;
    const int64_t line_start = 50 * size / 9;
    const int64_t line_end = 7 * size;
    const int64_t line_dx = line_end - line_start;
    const int64_t line_dy = line_dx;
    const int64_t sample_dx = x - line_start;
    const int64_t sample_dy = y - line_start;
    const int64_t dot = sample_dx * line_dx + sample_dy * line_dy;
    const int64_t line_length_squared = line_dx * line_dx +
        line_dy * line_dy;
    bool on_handle;

    if (distance_squared >= inner * inner &&
            distance_squared <= outer * outer) {
        return true;
    }
    if (dot <= 0) {
        on_handle = sample_dx * sample_dx + sample_dy * sample_dy <=
            half_stroke * half_stroke;
    } else if (dot >= line_length_squared) {
        const int64_t end_dx = x - line_end;
        const int64_t end_dy = y - line_end;

        on_handle = end_dx * end_dx + end_dy * end_dy <=
            half_stroke * half_stroke;
    } else {
        const int64_t cross = sample_dx * line_dy - sample_dy * line_dx;

        on_handle = cross * cross <= half_stroke * half_stroke *
            line_length_squared;
    }
    return on_handle;
}

static enum ui_status draw_search_icon(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t colour
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);
    const uint32_t size = bounds.width < bounds.height ? bounds.width :
        bounds.height;

    if (size < 8U || size > 24U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t local_y = clipped.y - bounds.y + y;
            uint32_t covered = 0U;
            uint32_t under;

            for (uint32_t sample_y = 0U; sample_y < 4U; ++sample_y) {
                for (uint32_t sample_x = 0U; sample_x < 4U; ++sample_x) {
                    const int64_t sample_position_x =
                        (int64_t)local_x * 8 + (int64_t)sample_x * 2 + 1;
                    const int64_t sample_position_y =
                        (int64_t)local_y * 8 + (int64_t)sample_y * 2 + 1;

                    if (search_stroke_sample(sample_position_x,
                            sample_position_y, size)) {
                        ++covered;
                    }
                }
            }
            if (covered == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend_packed(under, colour,
                        (uint8_t)((covered * UINT8_MAX + 8U) / 16U))) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status translucent_fill(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t over,
    uint8_t alpha
)
{
    const struct ui_rect clipped = rect_intersection(rectangle, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            uint32_t under;

            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend_packed(under, over, alpha)) != SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status translucent_capsule_fill(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t colour,
    uint8_t alpha
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);
    const int64_t radius = (int64_t)bounds.height * 4;
    const int64_t left_center = radius;
    const int64_t right_center = (int64_t)bounds.width * 8 - radius;
    const int64_t vertical_center = radius;

    if (bounds.height < 8U || bounds.width < bounds.height) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t local_y = clipped.y - bounds.y + y;
            uint32_t covered = 0U;
            uint32_t under;

            for (uint32_t sample_y = 0U; sample_y < 4U; ++sample_y) {
                for (uint32_t sample_x = 0U; sample_x < 4U; ++sample_x) {
                    const int64_t sample_position_x =
                        (int64_t)local_x * 8 + (int64_t)sample_x * 2 + 1;
                    const int64_t sample_position_y =
                        (int64_t)local_y * 8 + (int64_t)sample_y * 2 + 1;
                    int64_t horizontal_distance = 0;
                    const int64_t vertical_distance = sample_position_y -
                        vertical_center;

                    if (sample_position_x < left_center) {
                        horizontal_distance = sample_position_x -
                            left_center;
                    } else if (sample_position_x > right_center) {
                        horizontal_distance = sample_position_x -
                            right_center;
                    }
                    if (horizontal_distance * horizontal_distance +
                            vertical_distance * vertical_distance <=
                            radius * radius) {
                        ++covered;
                    }
                }
            }
            if (covered == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend_packed(under, colour, (uint8_t)(
                        ((uint32_t)alpha * covered + 8U) / 16U))) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status stroke_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t thickness,
    uint32_t pixel
)
{
    if (rectangle.width < thickness * 2U ||
        rectangle.height < thickness * 2U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    const struct ui_rect edges[4] = {
        { rectangle.x, rectangle.y, rectangle.width, thickness },
        { rectangle.x, rectangle.y + rectangle.height - thickness,
            rectangle.width, thickness },
        { rectangle.x, rectangle.y, thickness, rectangle.height },
        { rectangle.x + rectangle.width - thickness, rectangle.y,
            thickness, rectangle.height }
    };
    for (size_t index = 0U; index < 4U; ++index) {
        const enum ui_status status = fill_clipped(edges[index], damage, pixel);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status bevel_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    bool raised
)
{
    if (rectangle.width < 2U || rectangle.height < 2U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    const uint32_t upper = raised ? state.theme.white :
        state.theme.ink;
    const uint32_t lower = raised ? state.theme.ink :
        state.theme.white;
    const struct ui_rect upper_edges[2] = {
        { rectangle.x, rectangle.y, rectangle.width, 1U },
        { rectangle.x, rectangle.y, 1U, rectangle.height }
    };
    const struct ui_rect lower_edges[2] = {
        { rectangle.x, rectangle.y + rectangle.height - 1U,
            rectangle.width, 1U },
        { rectangle.x + rectangle.width - 1U, rectangle.y,
            1U, rectangle.height }
    };

    for (size_t index = 0U; index < 2U; ++index) {
        enum ui_status status = fill_clipped(upper_edges[index], damage,
            upper);

        if (status != UI_STATUS_OK) {
            return status;
        }
        status = fill_clipped(lower_edges[index], damage, lower);
        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_text(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t pixel
)
{
    const struct ui_rect clip = rect_intersection(bounds, damage);
    size_t glyphs = 0U;

    if (clip.width == 0U || clip.height == 0U) {
        return UI_STATUS_OK;
    }
    if (ui_font_draw_text_clipped(canvas, surface_rect_of(bounds),
            surface_rect_of(clip), x, baseline, text, pixel, &glyphs) !=
        UI_FONT_STATUS_OK) {
        return UI_STATUS_FONT_FAILURE;
    }
    state.renders.glyphs += glyphs;
    return UI_STATUS_OK;
}

static enum ui_status draw_alpha_subimage(
    struct ui_rect bounds,
    struct ui_rect damage,
    const uint32_t *pixels,
    const uint8_t *alpha_pixels,
    uint32_t source_width,
    uint32_t source_height,
    struct ui_rect source_bounds
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);
    uint32_t source_right;
    uint32_t source_bottom;

    if (pixels == NULL || alpha_pixels == NULL || source_width == 0U ||
        source_height == 0U || bounds.width == 0U || bounds.height == 0U ||
        !rect_end(source_bounds, &source_right, &source_bottom) ||
        source_right > source_width || source_bottom > source_height) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - bounds.y + y;
        const uint64_t denominator_y = bounds.height > 1U ?
            (uint64_t)bounds.height - 1U : 1U;
        const uint64_t scaled_y = (uint64_t)local_y *
            (source_bounds.height > 1U ? source_bounds.height - 1U : 0U);
        const uint32_t source_y = source_bounds.y +
            (uint32_t)(scaled_y / denominator_y);
        const uint32_t next_y = source_y + 1U < source_bottom ?
            source_y + 1U : source_y;
        const uint64_t fraction_y = scaled_y % denominator_y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint64_t denominator_x = bounds.width > 1U ?
                (uint64_t)bounds.width - 1U : 1U;
            const uint64_t scaled_x = (uint64_t)local_x *
                (source_bounds.width > 1U ? source_bounds.width - 1U : 0U);
            const uint32_t source_x = source_bounds.x +
                (uint32_t)(scaled_x / denominator_x);
            const uint32_t next_x = source_x + 1U < source_right ?
                source_x + 1U : source_x;
            const uint64_t fraction_x = scaled_x % denominator_x;
            const size_t samples[4U] = {
                (size_t)source_y * source_width + source_x,
                (size_t)source_y * source_width + next_x,
                (size_t)next_y * source_width + source_x,
                (size_t)next_y * source_width + next_x
            };
            const uint64_t weights[4U] = {
                (denominator_x - fraction_x) *
                    (denominator_y - fraction_y),
                fraction_x * (denominator_y - fraction_y),
                (denominator_x - fraction_x) * fraction_y,
                fraction_x * fraction_y
            };
            const uint64_t denominator = denominator_x * denominator_y;
            uint64_t alpha_sum = 0U;
            uint8_t alpha;
            uint32_t pixel = 0U;

            for (size_t sample = 0U; sample < 4U; ++sample) {
                alpha_sum += (uint64_t)alpha_pixels[samples[sample]] *
                    weights[sample];
            }
            alpha = (uint8_t)((alpha_sum + denominator / 2U) /
                denominator);

            if (alpha == 0U) {
                continue;
            }
            static const uint8_t channels = 3U;
            const uint8_t shifts[3U] = { logo_red_shift,
                logo_green_shift, logo_blue_shift };
            uint32_t background;

            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &background) != SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
            for (uint8_t channel = 0U; channel < channels; ++channel) {
                const uint8_t shift = shifts[channel];
                const uint32_t background_value =
                    (background >> shift) & UINT32_C(0xFF);
                uint64_t premultiplied_sum = 0U;

                for (size_t sample = 0U; sample < 4U; ++sample) {
                    const uint32_t foreground_value =
                        (pixels[samples[sample]] >> shift) & UINT32_C(0xFF);
                    premultiplied_sum += (uint64_t)foreground_value *
                        alpha_pixels[samples[sample]] * weights[sample];
                }
                const uint32_t premultiplied = (uint32_t)(
                    (premultiplied_sum + denominator / 2U) / denominator);
                const uint32_t value = (premultiplied +
                    background_value * (UINT8_MAX - alpha) + 127U) /
                        UINT8_MAX;
                pixel |= (value > UINT8_MAX ? UINT8_MAX : value) << shift;
            }
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y, pixel) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_alpha_image(
    struct ui_rect bounds,
    struct ui_rect damage,
    const uint32_t *pixels,
    const uint8_t *alpha_pixels,
    uint32_t source_width,
    uint32_t source_height
)
{
    return draw_alpha_subimage(bounds, damage, pixels, alpha_pixels,
        source_width, source_height,
        (struct ui_rect){ 0U, 0U, source_width, source_height });
}

static enum ui_status draw_logo_color(
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    return draw_alpha_image(bounds, damage, logo_pixels, logo_alpha,
        UI_LOGO_WIDTH, UI_LOGO_HEIGHT);
}

static enum ui_status gradient_rect(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint8_t top_red,
    uint8_t top_green,
    uint8_t top_blue,
    uint8_t bottom_red,
    uint8_t bottom_green,
    uint8_t bottom_blue
)
{
    enum ui_status status = UI_STATUS_OK;
    const uint32_t denominator = bounds.height > 1U ? bounds.height - 1U : 1U;

    for (uint32_t row = 0U; row < bounds.height && status == UI_STATUS_OK;
         ++row) {
        const uint32_t inverse = denominator - row;
        const uint8_t red = (uint8_t)(((uint32_t)top_red * inverse +
            (uint32_t)bottom_red * row) / denominator);
        const uint8_t green = (uint8_t)(((uint32_t)top_green * inverse +
            (uint32_t)bottom_green * row) / denominator);
        const uint8_t blue = (uint8_t)(((uint32_t)top_blue * inverse +
            (uint32_t)bottom_blue * row) / denominator);

        status = fill_clipped((struct ui_rect){ bounds.x, bounds.y + row,
            bounds.width, 1U }, damage, framebuffer_pack(red, green, blue));
    }
    return status;
}

static enum ui_status draw_icon(
    enum ui_element_id id,
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t pixel
)
{
    (void)pixel;
    enum ui_status status = UI_STATUS_OK;

    if (id == UI_ELEMENT_DOCK_FILES || id == UI_ELEMENT_DOCK_TERMINAL) {
        const bool files = id == UI_ELEMENT_DOCK_FILES;
        const uint32_t *icon_pixels = files ?
            files_icon_pixels : terminal_icon_pixels;
        const uint8_t *icon_alpha = files ?
            files_icon_alpha : terminal_icon_alpha;
        const uint32_t source_width = files ?
            files_icon_width : terminal_icon_width;
        const uint32_t source_height = files ?
            files_icon_height : terminal_icon_height;
        uint32_t mark_width = bounds.width;
        uint32_t mark_height = mark_width * source_height / source_width;

        if (mark_height > bounds.height) {
            mark_height = bounds.height;
            mark_width = mark_height * source_width / source_height;
        }
        status = draw_alpha_image((struct ui_rect){
            bounds.x + (bounds.width - mark_width) / 2U,
            bounds.y + (bounds.height - mark_height) / 2U,
            mark_width, mark_height
        }, damage, icon_pixels, icon_alpha, source_width, source_height);
    } else if (id == UI_ELEMENT_DOCK_NOTES) {
        const struct ui_rect page = { bounds.x + 7U, bounds.y + 4U,
            bounds.width - 14U, bounds.height - 8U };

        if (status == UI_STATUS_OK) {
            status = gradient_rect(page, damage, 0xFFU, 0xFFU, 0xF8U,
                0xE4U, 0xE2U, 0xD6U);
        }
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(page, damage, 1U,
                framebuffer_pack(0xA0U, 0xA0U, 0x98U));
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ page.x, page.y,
                page.width, 13U }, damage,
                framebuffer_pack(0x72U, 0x75U, 0x76U));
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ page.x + 13U,
                page.y + 15U, 2U, page.height - 18U }, damage,
                framebuffer_pack(0xE9U, 0x78U, 0x78U));
        }
        for (uint32_t row = page.y + 22U;
             row + 1U < page.y + page.height && status == UI_STATUS_OK;
             row += 8U) {
            status = fill_clipped((struct ui_rect){ page.x + 4U, row,
                page.width - 8U, 1U }, damage,
                framebuffer_pack(0xB8U, 0xD1U, 0xDCU));
        }
    } else if (id == UI_ELEMENT_DOCK_STUDIO) {
        uint32_t mark_width = bounds.width;
        uint32_t mark_height = mark_width * studio_icon_height /
            studio_icon_width;

        if (mark_height > bounds.height) {
            mark_height = bounds.height;
            mark_width = mark_height * studio_icon_width /
                studio_icon_height;
        }
        status = draw_alpha_image((struct ui_rect){
            bounds.x + (bounds.width - mark_width) / 2U,
            bounds.y + (bounds.height - mark_height) / 2U,
            mark_width, mark_height
        }, damage, studio_icon_pixels, studio_icon_alpha,
            studio_icon_width, studio_icon_height);
    } else if (id == UI_ELEMENT_DOCK_CAMERA ||
            id == UI_ELEMENT_DOCK_CANVAS ||
            id == UI_ELEMENT_DOCK_STORE ||
            id == UI_ELEMENT_DOCK_SETTINGS) {
        const bool camera = id == UI_ELEMENT_DOCK_CAMERA;
        const bool drawing = id == UI_ELEMENT_DOCK_CANVAS;
        const bool store = id == UI_ELEMENT_DOCK_STORE;
        const uint32_t *icon_pixels = camera ? camera_icon_pixels :
            (drawing ? canvas_icon_pixels :
                (store ? store_icon_pixels : settings_icon_pixels));
        const uint8_t *icon_alpha = camera ? camera_icon_alpha :
            (drawing ? canvas_icon_alpha :
                (store ? store_icon_alpha : settings_icon_alpha));
        const uint32_t source_width = camera ? camera_icon_width :
            (drawing ? canvas_icon_width :
                (store ? store_icon_width : settings_icon_width));
        const uint32_t source_height = camera ? camera_icon_height :
            (drawing ? canvas_icon_height :
                (store ? store_icon_height : settings_icon_height));
        uint32_t mark_width = bounds.width;
        uint32_t mark_height = mark_width * source_height / source_width;

        if (mark_height > bounds.height) {
            mark_height = bounds.height;
            mark_width = mark_height * source_width / source_height;
        }
        status = draw_alpha_image((struct ui_rect){
            bounds.x + (bounds.width - mark_width) / 2U,
            bounds.y + (bounds.height - mark_height) / 2U,
            mark_width, mark_height
        }, damage, icon_pixels, icon_alpha, source_width, source_height);
    } else {
        status = UI_STATUS_BAD_ELEMENT;
    }
    return status;
}

static uint32_t centered_text_x(struct ui_rect bounds, const char *text)
{
    uint32_t width = 0U;

    if (ui_font_text_width(text, &width) != UI_FONT_STATUS_OK ||
        width >= bounds.width) {
        return bounds.x;
    }
    return bounds.x + (bounds.width - width) / 2U;
}

enum window_control_icon {
    WINDOW_CONTROL_CLOSE = 0,
    WINDOW_CONTROL_MAXIMIZE,
    WINDOW_CONTROL_MINIMIZE
};

static bool sample_near_segment(
    int64_t x,
    int64_t y,
    int64_t start_x,
    int64_t start_y,
    int64_t end_x,
    int64_t end_y,
    int64_t half_stroke
)
{
    const int64_t line_x = end_x - start_x;
    const int64_t line_y = end_y - start_y;
    const int64_t sample_x = x - start_x;
    const int64_t sample_y = y - start_y;
    const int64_t dot = sample_x * line_x + sample_y * line_y;
    const int64_t length_squared = line_x * line_x + line_y * line_y;

    if (dot <= 0) {
        return sample_x * sample_x + sample_y * sample_y <=
            half_stroke * half_stroke;
    }
    if (dot >= length_squared) {
        const int64_t distance_x = x - end_x;
        const int64_t distance_y = y - end_y;

        return distance_x * distance_x + distance_y * distance_y <=
            half_stroke * half_stroke;
    }
    {
        const int64_t cross = sample_x * line_y - sample_y * line_x;

        return cross * cross <= half_stroke * half_stroke *
            length_squared;
    }
}

static bool sample_in_rounded_rectangle(
    int64_t x,
    int64_t y,
    int64_t left,
    int64_t top,
    int64_t right,
    int64_t bottom,
    int64_t radius
)
{
    int64_t distance_x = 0;
    int64_t distance_y = 0;

    if (x < left || x > right || y < top || y > bottom) {
        return false;
    }
    if (x < left + radius) {
        distance_x = left + radius - x;
    } else if (x > right - radius) {
        distance_x = x - (right - radius);
    }
    if (y < top + radius) {
        distance_y = top + radius - y;
    } else if (y > bottom - radius) {
        distance_y = y - (bottom - radius);
    }
    return distance_x * distance_x + distance_y * distance_y <=
        radius * radius;
}

static bool window_control_icon_sample(
    enum window_control_icon icon,
    int64_t x,
    int64_t y
)
{
    const int64_t half_stroke = 5;

    if (icon == WINDOW_CONTROL_CLOSE) {
        return sample_near_segment(x, y, 48, 48, 96, 96,
                half_stroke) ||
            sample_near_segment(x, y, 96, 48, 48, 96, half_stroke);
    }
    if (icon == WINDOW_CONTROL_MINIMIZE) {
        return sample_near_segment(x, y, 44, 72, 100, 72, half_stroke);
    }
    {
        const int64_t left = 36;
        const int64_t top = 36;
        const int64_t right = 108;
        const int64_t bottom = 108;
        const int64_t radius = 12;
        const bool in_outer = sample_in_rounded_rectangle(x, y,
            left - half_stroke, top - half_stroke,
            right + half_stroke, bottom + half_stroke,
            radius + half_stroke);
        const bool in_inner = sample_in_rounded_rectangle(x, y,
            left + half_stroke, top + half_stroke,
            right - half_stroke, bottom - half_stroke,
            radius > half_stroke ? radius - half_stroke : 0);

        return in_outer && !in_inner;
    }
}

static enum ui_status draw_window_control_disc(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t border,
    uint32_t fill
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);
    const int64_t center_x = (int64_t)bounds.width * 4;
    const int64_t center_y = (int64_t)bounds.height * 4;
    const uint32_t diameter = bounds.width < bounds.height ? bounds.width :
        bounds.height;
    const int64_t outer_radius = (int64_t)diameter * 4 - 6;
    const int64_t inner_radius = outer_radius - 8;

    if (diameter < 12U || diameter > 24U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t local_y = clipped.y - bounds.y + y;
            uint32_t outer_covered = 0U;
            uint32_t inner_covered = 0U;
            uint32_t under;

            for (uint32_t sample_y = 0U; sample_y < 4U; ++sample_y) {
                for (uint32_t sample_x = 0U; sample_x < 4U; ++sample_x) {
                    const int64_t sample_position_x =
                        (int64_t)local_x * 8 + (int64_t)sample_x * 2 + 1;
                    const int64_t sample_position_y =
                        (int64_t)local_y * 8 + (int64_t)sample_y * 2 + 1;
                    const int64_t distance_x = sample_position_x - center_x;
                    const int64_t distance_y = sample_position_y - center_y;
                    const int64_t distance_squared =
                        distance_x * distance_x + distance_y * distance_y;

                    if (distance_squared <= outer_radius * outer_radius) {
                        ++outer_covered;
                    }
                    if (distance_squared <= inner_radius * inner_radius) {
                        ++inner_covered;
                    }
                }
            }
            if (outer_covered == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
            under = blend_packed(under, border,
                (uint8_t)((outer_covered * UINT8_MAX + 8U) / 16U));
            under = blend_packed(under, fill,
                (uint8_t)((inner_covered * UINT8_MAX + 8U) / 16U));
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y, under) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_window_control_icon(
    struct ui_rect bounds,
    struct ui_rect damage,
    enum window_control_icon icon,
    uint32_t colour
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);

    if (bounds.width != 18U || bounds.height != 18U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - bounds.x + x;
            const uint32_t local_y = clipped.y - bounds.y + y;
            uint32_t covered = 0U;
            uint32_t under;

            for (uint32_t sample_y = 0U; sample_y < 4U; ++sample_y) {
                for (uint32_t sample_x = 0U; sample_x < 4U; ++sample_x) {
                    if (window_control_icon_sample(icon,
                            (int64_t)local_x * 8 +
                                (int64_t)sample_x * 2 + 1,
                            (int64_t)local_y * 8 +
                                (int64_t)sample_y * 2 + 1)) {
                        ++covered;
                    }
                }
            }
            if (covered == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend_packed(under, colour,
                        (uint8_t)((covered * UINT8_MAX + 8U) / 16U))) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_window_title(
    struct ui_rect title,
    struct ui_rect damage,
    uint32_t baseline,
    const char *label,
    bool active
)
{
    const uint32_t label_x = centered_text_x(title, label);
    uint32_t label_width;
    enum ui_status status = ui_font_text_width(label, &label_width) ==
        UI_FONT_STATUS_OK ? UI_STATUS_OK : UI_STATUS_FONT_FAILURE;

    if (status == UI_STATUS_OK && window_title_gradient && active) {
        status = gradient_rect(title, damage, 0xF4U, 0xF7U, 0xF7U,
            0x99U, 0xA5U, 0xAAU);
    } else if (status == UI_STATUS_OK && window_title_gradient) {
        status = gradient_rect(title, damage, 0xE4U, 0xE7U, 0xE8U,
            0xAFU, 0xB6U, 0xB9U);
    } else if (status == UI_STATUS_OK) {
        status = fill_clipped(title, damage,
            framebuffer_pack(0xC6U, 0xCCU, 0xCEU));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            title.x, title.y + title.height - 1U, title.width, 1U
        }, damage, state.theme.ink);
    }
    {
        const struct ui_rect close = {
            title.x + 6U, title.y + 4U, 18U, 18U
        };
        const struct ui_rect maximize = {
            title.x + 28U, title.y + 4U, 18U, 18U
        };
        const struct ui_rect minimize = {
            title.x + 50U, title.y + 4U, 18U, 18U
        };
        const uint32_t close_mark = active ? state.theme.white :
            framebuffer_pack(0x78U, 0x2CU, 0x2AU);
        const uint32_t maximize_mark = active ? state.theme.white :
            framebuffer_pack(0x4EU, 0x35U, 0x62U);
        const uint32_t minimize_mark = framebuffer_pack(
            active ? 0x5CU : 0x78U, active ? 0x63U : 0x7EU,
            active ? 0x68U : 0x82U);

        const uint32_t close_fill = framebuffer_pack(
            active ? 0xF2U : 0xD9U, active ? 0x5FU : 0x6CU,
            active ? 0x57U : 0x67U);
        const uint32_t maximize_fill = framebuffer_pack(
            active ? 0xA6U : 0x9BU, active ? 0x7DU : 0x86U,
            active ? 0xC8U : 0xA9U);
        const uint32_t minimize_fill = framebuffer_pack(
            active ? 0xE8U : 0xD9U, active ? 0xEBU : 0xDDU,
            active ? 0xEDU : 0xDFU);

        status = draw_window_control_disc(close, damage,
            close_fill, close_fill);
        if (status == UI_STATUS_OK) {
            status = draw_window_control_icon(close, damage,
                WINDOW_CONTROL_CLOSE, close_mark);
        }
        if (status == UI_STATUS_OK) {
            status = draw_window_control_disc(maximize, damage,
                maximize_fill, maximize_fill);
        }
        if (status == UI_STATUS_OK) {
            status = draw_window_control_icon(maximize, damage,
                WINDOW_CONTROL_MAXIMIZE, maximize_mark);
        }
        if (status == UI_STATUS_OK) {
            status = draw_window_control_disc(minimize, damage,
                minimize_fill, minimize_fill);
        }
        if (status == UI_STATUS_OK) {
            status = draw_window_control_icon(minimize, damage,
                WINDOW_CONTROL_MINIMIZE, minimize_mark);
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(title, damage, label_x, baseline, label,
            state.theme.ink);
    }
    return status;
}

static bool panel_is_active_for(enum ui_element_id element)
{
    const enum ui_panel_id panel = panel_for_element(element);

    return panel > UI_PANEL_NONE && panel < UI_PANEL_COUNT &&
        panel_open[panel];
}

static enum ui_status draw_dock_radial(
    uint32_t center_x,
    uint32_t center_y,
    uint32_t radius_x,
    uint32_t radius_y,
    struct ui_rect damage,
    uint32_t pixel,
    uint8_t maximum_alpha
)
{
    if (radius_x == 0U || radius_y == 0U) {
        return UI_STATUS_OK;
    }
    const int32_t left = (int32_t)center_x - (int32_t)radius_x;
    const int32_t top = (int32_t)center_y - (int32_t)radius_y;
    const uint64_t radius_x_squared = (uint64_t)radius_x * radius_x;
    const uint64_t radius_y_squared = (uint64_t)radius_y * radius_y;

    for (uint32_t row = 0U; row <= radius_y * 2U; ++row) {
        const int32_t y = top + (int32_t)row;

        if (y < 0 || (uint32_t)y >= state.layout.surface.height) {
            continue;
        }
        for (uint32_t column = 0U; column <= radius_x * 2U; ++column) {
            const int32_t x = left + (int32_t)column;
            const int64_t dx = (int64_t)x - center_x;
            const int64_t dy = (int64_t)y - center_y;
            const uint64_t distance = (uint64_t)(dx * dx) *
                radius_y_squared + (uint64_t)(dy * dy) * radius_x_squared;
            const uint64_t limit = radius_x_squared * radius_y_squared;

            if (x < 0 || (uint32_t)x >= state.layout.surface.width ||
                    distance >= limit ||
                    !rect_contains_point(damage, (struct ui_point){ x, y })) {
                continue;
            }
            const uint8_t alpha = (uint8_t)(
                (uint64_t)maximum_alpha * (limit - distance) / limit);
            uint32_t under;

            if (surface_read_pixel(canvas, (uint32_t)x, (uint32_t)y,
                    &under) != SURFACE_STATUS_OK ||
                    surface_pixel(canvas, (uint32_t)x, (uint32_t)y,
                        blend_packed(under, pixel, alpha)) !=
                        SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static uint8_t dock_icon_alpha_at(
    enum ui_element_id id,
    uint32_t local_x,
    uint32_t local_y,
    uint32_t width,
    uint32_t height
)
{
    const uint8_t *alpha = NULL;
    uint32_t source_width = 0U;
    uint32_t source_height = 0U;

    if (id == UI_ELEMENT_DOCK_FILES) {
        alpha = files_icon_alpha;
        source_width = files_icon_width;
        source_height = files_icon_height;
    } else if (id == UI_ELEMENT_DOCK_TERMINAL) {
        alpha = terminal_icon_alpha;
        source_width = terminal_icon_width;
        source_height = terminal_icon_height;
    } else if (id == UI_ELEMENT_DOCK_STUDIO) {
        alpha = studio_icon_alpha;
        source_width = studio_icon_width;
        source_height = studio_icon_height;
    } else if (id == UI_ELEMENT_DOCK_CAMERA) {
        alpha = camera_icon_alpha;
        source_width = camera_icon_width;
        source_height = camera_icon_height;
    } else if (id == UI_ELEMENT_DOCK_CANVAS) {
        alpha = canvas_icon_alpha;
        source_width = canvas_icon_width;
        source_height = canvas_icon_height;
    } else if (id == UI_ELEMENT_DOCK_STORE) {
        alpha = store_icon_alpha;
        source_width = store_icon_width;
        source_height = store_icon_height;
    } else if (id == UI_ELEMENT_DOCK_SETTINGS) {
        alpha = settings_icon_alpha;
        source_width = settings_icon_width;
        source_height = settings_icon_height;
    } else if (id == UI_ELEMENT_DOCK_NOTES) {
        return local_x >= 7U && local_x + 7U < width && local_y >= 4U &&
            local_y + 4U < height ? UINT8_MAX : 0U;
    }
    if (alpha == NULL || width == 0U || height == 0U ||
            source_width == 0U || source_height == 0U) {
        return 0U;
    }
    uint32_t source_x = local_x * source_width / width;
    uint32_t source_y = local_y * source_height / height;
    if (source_x >= source_width) {
        source_x = source_width - 1U;
    }
    if (source_y >= source_height) {
        source_y = source_height - 1U;
    }
    return alpha[(size_t)source_y * source_width + source_x];
}

static enum ui_status draw_dock_reflection(
    const struct ui_dock_item *item,
    struct ui_rect icon,
    struct ui_rect damage
)
{
    const uint32_t baseline = dock_fixed_pixel(dock_model.baseline);
    const uint32_t panel_height = dock_fixed_pixel(dock_model.panel_height);
    const uint32_t dock_center = dock_fixed_pixel(dock_model.center_x);
    const uint32_t icon_bottom = icon.y + icon.height;
    const uint32_t lift = baseline > icon_bottom ? baseline - icon_bottom : 0U;

    if (!dock_reflections || panel_height <= lift + 1U ||
            icon.width == 0U || icon.height == 0U) {
        return UI_STATUS_OK;
    }
    uint32_t reflection_height = icon.height * 66U / 100U;
    if (reflection_height > panel_height - lift) {
        reflection_height = panel_height - lift;
    }
    for (uint32_t row = 0U; row < reflection_height; ++row) {
        const uint32_t source_local_y = icon.height - 1U -
            row * icon.height / reflection_height;
        const uint32_t source_y = icon.y + source_local_y;
        const uint32_t destination_y = baseline + lift + row;
        const uint32_t depth = destination_y > baseline ?
            destination_y - baseline : 0U;
        const uint32_t flare = 1000U +
            55U * depth / (panel_height == 0U ? 1U : panel_height);
        const uint32_t fade = 117U * (reflection_height - row) /
            reflection_height;

        if (destination_y >= state.layout.surface.height) {
            break;
        }
        for (uint32_t column = 0U; column < icon.width; ++column) {
            const uint8_t artwork_alpha = dock_icon_alpha_at(item->id,
                column, source_local_y, icon.width, icon.height);
            if (artwork_alpha == 0U) {
                continue;
            }
            const uint32_t source_x = icon.x + column;
            const int64_t offset = (int64_t)source_x - dock_center;
            const int64_t warped = (int64_t)dock_center +
                offset * flare / 1000;
            if (warped < 0 || warped >= state.layout.surface.width ||
                    !rect_contains_point(damage, (struct ui_point){
                        (int32_t)warped, (int32_t)destination_y })) {
                continue;
            }
            uint32_t source;
            uint32_t under;
            const uint8_t alpha = (uint8_t)(
                (uint32_t)fade * artwork_alpha / UINT8_MAX);
            if (surface_read_pixel(canvas, source_x, source_y, &source) !=
                    SURFACE_STATUS_OK ||
                    surface_read_pixel(canvas, (uint32_t)warped,
                        destination_y, &under) != SURFACE_STATUS_OK ||
                    surface_pixel(canvas, (uint32_t)warped, destination_y,
                        blend_packed(under, source, alpha)) !=
                        SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_dock_tooltip(
    size_t item_index,
    const struct ui_dock_item *item,
    struct ui_rect icon,
    struct ui_rect damage
)
{
    if (!dock_labels || dock_model.tooltip_item != (int)item_index ||
            dock_model.tooltip <= 655) {
        return UI_STATUS_OK;
    }
    uint32_t label_width;
    if (ui_font_text_width(item->label, &label_width) != UI_FONT_STATUS_OK) {
        return UI_STATUS_FONT_FAILURE;
    }
    const uint32_t padding = 10U;
    const uint32_t bubble_width = label_width + padding * 2U;
    const uint32_t bubble_height = 24U;
    uint32_t center = icon.x + icon.width / 2U;
    uint32_t bubble_x = center > bubble_width / 2U ?
        center - bubble_width / 2U : 6U;
    const uint32_t slide = (uint32_t)(
        (int64_t)(DOCK3D_ONE - dock_model.tooltip) * 6 / DOCK3D_ONE);
    uint32_t bubble_y = icon.y > bubble_height + 14U + slide ?
        icon.y - bubble_height - 14U - slide : 2U;
    if (bubble_x + bubble_width > state.layout.surface.width - 6U) {
        bubble_x = state.layout.surface.width - 6U - bubble_width;
    }
    const uint8_t alpha = (uint8_t)(
        (int64_t)dock_model.tooltip * 247 / DOCK3D_ONE);
    enum ui_status status = UI_STATUS_OK;
    for (uint32_t row = 0U; row < bubble_height &&
            status == UI_STATUS_OK; ++row) {
        const uint32_t edge = row < 4U ? 4U - row :
            (row + 4U >= bubble_height ? row + 4U - bubble_height + 1U : 0U);
        status = translucent_fill((struct ui_rect){
            bubble_x + edge, bubble_y + row,
            bubble_width - edge * 2U, 1U
        }, damage, framebuffer_pack(0xF2U, 0xF2U, 0xF5U), alpha);
    }
    if (status == UI_STATUS_OK) {
        for (uint32_t row = 0U; row < 7U; ++row) {
            const uint32_t half = 6U - row;
            status = translucent_fill((struct ui_rect){
                center - half, bubble_y + bubble_height + row,
                half * 2U + 1U, 1U
            }, damage, framebuffer_pack(0xF2U, 0xF2U, 0xF5U), alpha);
            if (status != UI_STATUS_OK) {
                break;
            }
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text((struct ui_rect){ bubble_x, bubble_y,
            bubble_width, bubble_height }, damage, bubble_x + padding,
            bubble_y + 17U, item->label, framebuffer_pack(0x17U, 0x17U,
                0x1AU));
    }
    return status;
}

static enum ui_status draw_dock_item(
    const struct ui_dock_item *item,
    struct ui_rect damage
)
{
    const bool active = panel_is_active_for(item->id);
    const size_t item_index = (size_t)(item - state.layout.dock_items);
    struct ui_rect icon = item->icon_bounds;
    const uint32_t center_x = icon.x + icon.width / 2U;
    const uint32_t baseline = dock_fixed_pixel(dock_model.baseline);
    const uint32_t panel_height = dock_fixed_pixel(dock_model.panel_height);
    const uint32_t icon_bottom = icon.y + icon.height;
    const uint32_t lift = baseline > icon_bottom ? baseline - icon_bottom : 0U;
    const uint8_t shadow_alpha = (uint8_t)(115U *
        (lift < icon.height ? icon.height - lift : 0U) /
        (icon.height == 0U ? 1U : icon.height));
    enum ui_status status = draw_dock_radial(center_x,
        baseline + panel_height * 11U / 100U,
        icon.width * 46U / 100U, panel_height * 30U / 100U,
        damage, state.theme.shadow, shadow_alpha);

    if (status == UI_STATUS_OK) {
        status = draw_icon(item->id, icon, damage,
        state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_dock_reflection(item, icon, damage);
    }
    if (status == UI_STATUS_OK && active) {
        const uint32_t light_y = baseline + panel_height * 34U / 100U;
        const uint32_t radius = icon.width * 52U / 1000U + 1U;
        status = draw_dock_radial(center_x, light_y, radius * 4U,
            radius * 3U, damage, framebuffer_pack(0xD8U, 0xF2U, 0xFFU),
            180U);
        if (status == UI_STATUS_OK) {
            status = draw_circle(center_x, light_y, radius, damage,
                framebuffer_pack(0xEEU, 0xF8U, 0xFFU));
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_dock_tooltip(item_index, item, icon, damage);
    }
    return status;
}

static size_t append_text(char *buffer, size_t capacity, size_t at, const char *text)
{
    if (buffer == NULL || text == NULL || capacity == 0U) {
        return at;
    }
    for (size_t index = 0U; text[index] != '\0' && at + 1U < capacity;
         ++index) {
        buffer[at++] = text[index];
    }
    buffer[at] = '\0';
    return at;
}

static size_t append_u64(char *buffer, size_t capacity, size_t at, uint64_t value)
{
    char reversed[24];
    size_t count = 0U;

    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(reversed));
    while (count > 0U && at + 1U < capacity) {
        buffer[at++] = reversed[--count];
    }
    buffer[at] = '\0';
    return at;
}

static bool strings_equal(const char *left, const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[index] != '\0' && left[index] == right[index]) {
        ++index;
    }
    return left[index] == right[index];
}

static bool copy_string(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    if (destination == NULL || source == NULL || capacity == 0U) {
        return false;
    }
    while (source[index] != '\0') {
        if (index + 1U >= capacity) {
            destination[0] = '\0';
            return false;
        }
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return true;
}

static void set_app_status(const char *prefix, enum sapfs_status status)
{
    size_t at = append_text(app_status, sizeof(app_status), 0U, prefix);

    if (status != SAPFS_STATUS_OK) {
        at = append_text(app_status, sizeof(app_status), at, ": ");
        (void)append_text(app_status, sizeof(app_status), at,
            sapfs_status_string(status));
    }
}

static bool entry_path(const char *name, char *path)
{
    size_t at = 0U;

    if (!strings_equal(file_directory, ".")) {
        at = append_text(path, SAPFS_MAX_PATH + 1U, at, file_directory);
        at = append_text(path, SAPFS_MAX_PATH + 1U, at, "/");
    }
    at = append_text(path, SAPFS_MAX_PATH + 1U, at, name);
    return at != 0U && at <= SAPFS_MAX_PATH;
}

static bool file_is_internal(const char *name)
{
    static const char *const internal[] = {
        "SAPSTUDI.SAP", "STUTEMP.SAP", "STUBACK.SAP",
        "STUOUT.BMP", "OUTBACK.BMP"
    };

    if (!strings_equal(file_directory, ".")) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(internal) / sizeof(internal[0]);
         ++index) {
        if (strings_equal(name, internal[index])) {
            return true;
        }
    }
    return false;
}

static enum sapfs_status files_refresh(void)
{
    file_entry_count = 0U;
    const enum sapfs_status status = sapfs_list(SAPFS_VOLUME_DATA,
        file_directory, file_entries,
        sizeof(file_entries) / sizeof(file_entries[0]), &file_entry_count);

    if (status == SAPFS_STATUS_OK) {
        size_t visible = 0U;

        for (size_t index = 0U; index < file_entry_count; ++index) {
            if (!file_is_internal(file_entries[index].name)) {
                file_entries[visible++] = file_entries[index];
            }
        }
        file_entry_count = visible;
    }

    set_app_status(status == SAPFS_STATUS_OK ?
        "data volume / fat32 / synchronized view" : "Files", status);
    return status;
}

static void files_up(void)
{
    size_t length = 0U;
    size_t slash = SIZE_MAX;

    if (strings_equal(file_directory, ".")) {
        set_app_status("already at data root", SAPFS_STATUS_OK);
        return;
    }
    while (file_directory[length] != '\0') {
        if (file_directory[length] == '/') {
            slash = length;
        }
        ++length;
    }
    if (slash == SIZE_MAX) {
        (void)copy_string(file_directory, sizeof(file_directory), ".");
    } else {
        file_directory[slash] = '\0';
    }
    (void)files_refresh();
}

static void files_create(bool directory)
{
    char name[13U];
    char path[SAPFS_MAX_PATH + 1U];
    enum sapfs_status status = SAPFS_STATUS_FULL;

    for (uint32_t number = 1U; number <= 9U; ++number) {
        size_t at = append_text(name, sizeof(name), 0U,
            directory ? "FOLDER" : "NEW");

        if (at + 1U >= sizeof(name)) {
            status = SAPFS_STATUS_PATH;
            break;
        }
        name[at] = (char)('0' + number);
        ++at;
        name[at] = '\0';
        if (!directory) {
            (void)append_text(name, sizeof(name), at, ".TXT");
        }
        if (!entry_path(name, path)) {
            status = SAPFS_STATUS_PATH;
            break;
        }
        struct sapfs_stat stat;
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &stat);
        if (status != SAPFS_STATUS_NOT_FOUND) {
            continue;
        }
        status = directory ? sapfs_mkdir(SAPFS_VOLUME_DATA, path) :
            sapfs_create(SAPFS_VOLUME_DATA, path);
        if (status == SAPFS_STATUS_OK) {
            set_app_status(directory ? "created folder" : "created file",
                SAPFS_STATUS_OK);
            (void)files_refresh();
            return;
        }
        break;
    }
    set_app_status(directory ? "new folder" : "new file", status);
}

static enum sapfs_status note_load(void)
{
    struct sapfs_stat stat;
    sapfs_handle handle = 0U;
    size_t read_bytes = 0U;
    enum sapfs_status status = sapfs_stat_path(SAPFS_VOLUME_DATA,
        note_path, &stat);

    note_length = 0U;
    note_buffer[0] = '\0';
    note_dirty = false;
    note_savable = false;
    if (status == SAPFS_STATUS_NOT_FOUND) {
        note_savable = true;
        set_app_status("new note / Ctrl+S to save", SAPFS_STATUS_OK);
        return SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK && stat.directory) {
        status = SAPFS_STATUS_IS_DIRECTORY;
    }
    if (status == SAPFS_STATUS_OK &&
        stat.size > (uint32_t)(sizeof(note_buffer) - 1U)) {
        set_app_status("note exceeds editor capacity", SAPFS_STATUS_RANGE);
        return SAPFS_STATUS_RANGE;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, note_path,
            SAPFS_ACCESS_READ, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_read(handle, (uint8_t *)note_buffer,
            sizeof(note_buffer) - 1U, &read_bytes);
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK && read_bytes != stat.size) {
        status = SAPFS_STATUS_IO;
    }
    if (status == SAPFS_STATUS_OK) {
        note_length = read_bytes;
        note_buffer[note_length] = '\0';
        note_savable = true;
        set_app_status("note loaded from data volume", SAPFS_STATUS_OK);
    } else {
        set_app_status("open note", status);
    }
    return status;
}

static bool note_sibling_path(const char *leaf, char *path)
{
    size_t slash = SIZE_MAX;
    size_t source = 0U;
    size_t destination = 0U;

    if (leaf == NULL || path == NULL) {
        return false;
    }
    while (note_path[source] != '\0') {
        if (note_path[source] == '/') {
            slash = source;
        }
        ++source;
    }
    if (slash != SIZE_MAX) {
        for (size_t index = 0U; index <= slash; ++index) {
            if (destination + 1U >= SAPFS_MAX_PATH + 1U) {
                path[0] = '\0';
                return false;
            }
            path[destination++] = note_path[index];
        }
    }
    for (size_t index = 0U; leaf[index] != '\0'; ++index) {
        if (destination + 1U >= SAPFS_MAX_PATH + 1U) {
            path[0] = '\0';
            return false;
        }
        path[destination++] = leaf[index];
    }
    path[destination] = '\0';
    return destination != 0U && !strings_equal(path, note_path);
}

static enum sapfs_status note_replacement_paths(
    char *temporary,
    char *backup
)
{
    char temporary_leaf[] = "SNTMP1.TMP";
    char backup_leaf[] = "SNBAK1.BAK";

    for (uint32_t number = 1U; number <= 9U; ++number) {
        struct sapfs_stat stat;
        enum sapfs_status status;

        temporary_leaf[5] = (char)('0' + number);
        backup_leaf[5] = (char)('0' + number);
        if (!note_sibling_path(temporary_leaf, temporary) ||
            !note_sibling_path(backup_leaf, backup)) {
            return SAPFS_STATUS_PATH;
        }
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, temporary, &stat);
        if (status == SAPFS_STATUS_OK) {
            continue;
        }
        if (status != SAPFS_STATUS_NOT_FOUND) {
            return status;
        }
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, backup, &stat);
        if (status == SAPFS_STATUS_NOT_FOUND) {
            return SAPFS_STATUS_OK;
        }
        if (status != SAPFS_STATUS_OK) {
            return status;
        }
    }
    return SAPFS_STATUS_DIRECTORY_FULL;
}

static void note_remove_temporary(const char *path)
{
    if (sapfs_unlink(SAPFS_VOLUME_DATA, path) == SAPFS_STATUS_OK) {
        (void)sapfs_sync(SAPFS_VOLUME_DATA);
    }
}

static enum sapfs_status note_write_temporary(const char *path)
{
    sapfs_handle handle;
    size_t written = 0U;
    enum sapfs_status status = sapfs_create(SAPFS_VOLUME_DATA, path);

    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, path,
            SAPFS_ACCESS_WRITE, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_write(handle, (const uint8_t *)note_buffer,
            note_length, &written);
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK && written != note_length) {
        status = SAPFS_STATUS_WRITEBACK;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status != SAPFS_STATUS_OK) {
        note_remove_temporary(path);
    }
    return status;
}

static enum sapfs_status note_restore_original(
    const char *temporary,
    const char *backup,
    bool replacement_visible
)
{
    enum sapfs_status status = SAPFS_STATUS_OK;

    if (replacement_visible) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, note_path, temporary);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, backup, note_path);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status == SAPFS_STATUS_OK && replacement_visible) {
        note_remove_temporary(temporary);
    }
    return status;
}

static enum sapfs_status note_save(void)
{
    struct sapfs_stat stat;
    char temporary[SAPFS_MAX_PATH + 1U];
    char backup[SAPFS_MAX_PATH + 1U];
    bool original_exists = false;
    bool original_backed_up = false;
    enum sapfs_status status;

    if (!note_savable) {
        status = SAPFS_STATUS_RANGE;
    } else {
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, note_path, &stat);
        if (status == SAPFS_STATUS_OK) {
            original_exists = true;
        } else if (status == SAPFS_STATUS_NOT_FOUND) {
            status = SAPFS_STATUS_OK;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = note_replacement_paths(temporary, backup);
    }
    if (status == SAPFS_STATUS_OK) {
        status = note_write_temporary(temporary);
    }
    if (status == SAPFS_STATUS_OK && original_exists) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, note_path, backup);
        if (status == SAPFS_STATUS_OK) {
            original_backed_up = true;
            status = sapfs_sync(SAPFS_VOLUME_DATA);
        }
        if (status != SAPFS_STATUS_OK) {
            if (original_backed_up) {
                const enum sapfs_status restore = note_restore_original(
                    temporary, backup, false);

                if (restore != SAPFS_STATUS_OK) {
                    status = restore;
                } else {
                    note_remove_temporary(temporary);
                }
            } else {
                note_remove_temporary(temporary);
            }
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, temporary, note_path);
        if (status != SAPFS_STATUS_OK && original_exists) {
            const enum sapfs_status restore = note_restore_original(
                temporary, backup, false);

            if (restore != SAPFS_STATUS_OK) {
                status = restore;
            } else {
                note_remove_temporary(temporary);
            }
        } else if (status != SAPFS_STATUS_OK) {
            note_remove_temporary(temporary);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
        if (status != SAPFS_STATUS_OK && original_exists) {
            const enum sapfs_status restore = note_restore_original(
                temporary, backup, true);

            if (restore != SAPFS_STATUS_OK) {
                status = restore;
            }
        }
    }
    if (status == SAPFS_STATUS_OK && original_exists) {
        status = sapfs_unlink(SAPFS_VOLUME_DATA, backup);
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_sync(SAPFS_VOLUME_DATA);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        note_dirty = false;
        set_app_status("note saved", SAPFS_STATUS_OK);
    } else {
        set_app_status("save note / original retained", status);
    }
    return status;
}

static void studio_set_status(const char *message)
{
    if (!copy_string(studio_status, sizeof(studio_status), message)) {
        (void)copy_string(studio_status, sizeof(studio_status), "Studio status");
    }
}

static void studio_reset(bool dirty)
{
    for (size_t index = 0U; index < UI_STUDIO_MAX_CLIPS; ++index) {
        studio_clip_durations[index] = 0U;
        studio_clip_paths[index][0] = '\0';
    }
    studio_preview_width = 0U;
    studio_preview_height = 0U;
    studio_preview_loaded = false;
    studio_clip_count = 0U;
    studio_selected_clip = UINT8_MAX;
    studio_playhead = 0U;
    studio_dirty = dirty;
    studio_set_status(dirty ? "New project" : "Project ready");
}

static void studio_store_u32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
    bytes[offset + 2U] = (uint8_t)(value >> 16U);
    bytes[offset + 3U] = (uint8_t)(value >> 24U);
}

static void studio_store_u16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
}

static uint16_t studio_load_u16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)((uint16_t)bytes[offset] |
        (uint16_t)((uint16_t)bytes[offset + 1U] << 8U));
}

static uint32_t studio_load_u32(const uint8_t *bytes, size_t offset)
{
    return (uint32_t)bytes[offset] |
        (uint32_t)bytes[offset + 1U] << 8U |
        (uint32_t)bytes[offset + 2U] << 16U |
        (uint32_t)bytes[offset + 3U] << 24U;
}

static void studio_encode_project(uint8_t *bytes)
{
    static const uint8_t magic[8U] = {
        'S', 'A', 'P', 'S', 'T', 'U', '2', 0U
    };

    for (size_t index = 0U; index < UI_STUDIO_PROJECT_BYTES; ++index) {
        bytes[index] = 0U;
    }
    for (size_t index = 0U; index < sizeof(magic); ++index) {
        bytes[index] = magic[index];
    }
    bytes[8U] = studio_clip_count;
    bytes[9U] = studio_selected_clip;
    studio_store_u32(bytes, 12U, studio_playhead);
    for (size_t index = 0U; index < UI_STUDIO_MAX_CLIPS; ++index) {
        const size_t record = 16U + index * (4U + UI_STUDIO_PATH_BYTES);

        studio_store_u32(bytes, record, studio_clip_durations[index]);
        for (size_t at = 0U; at < UI_STUDIO_PATH_BYTES &&
             studio_clip_paths[index][at] != '\0'; ++at) {
            bytes[record + 4U + at] =
                (uint8_t)studio_clip_paths[index][at];
        }
    }
}

static bool studio_decode_project(const uint8_t *bytes)
{
    static const uint8_t magic[8U] = {
        'S', 'A', 'P', 'S', 'T', 'U', '2', 0U
    };

    for (size_t index = 0U; index < sizeof(magic); ++index) {
        if (bytes[index] != magic[index]) {
            return false;
        }
    }
    if (bytes[8U] > UI_STUDIO_MAX_CLIPS ||
        (bytes[9U] != UINT8_MAX && bytes[9U] >= bytes[8U]) ||
        studio_load_u32(bytes, 12U) > 1000U) {
        return false;
    }
    studio_clip_count = bytes[8U];
    studio_selected_clip = bytes[9U];
    studio_playhead = studio_load_u32(bytes, 12U);
    for (size_t index = 0U; index < UI_STUDIO_MAX_CLIPS; ++index) {
        const size_t record = 16U + index * (4U + UI_STUDIO_PATH_BYTES);
        const uint32_t duration = studio_load_u32(bytes, record);
        size_t path_length = 0U;

        if ((index < studio_clip_count && (duration == 0U || duration > 1000U)) ||
            (index >= studio_clip_count && duration != 0U)) {
            return false;
        }
        while (path_length < UI_STUDIO_PATH_BYTES &&
               bytes[record + 4U + path_length] != 0U) {
            const uint8_t character = bytes[record + 4U + path_length];

            if (character < 0x20U || character > 0x7EU ||
                character == '\\') {
                return false;
            }
            studio_clip_paths[index][path_length] = (char)character;
            ++path_length;
        }
        studio_clip_paths[index][path_length] = '\0';
        if ((index < studio_clip_count && path_length == 0U) ||
            (index >= studio_clip_count && path_length != 0U)) {
            return false;
        }
        studio_clip_durations[index] = duration;
    }
    return true;
}

static enum sapfs_status studio_load_preview(const char *path);

static enum sapfs_status studio_remove_if_present(const char *path)
{
    struct sapfs_stat stat;
    enum sapfs_status status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &stat);

    if (status == SAPFS_STATUS_NOT_FOUND) {
        return SAPFS_STATUS_OK;
    }
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    return stat.directory ? SAPFS_STATUS_IS_DIRECTORY :
        sapfs_unlink(SAPFS_VOLUME_DATA, path);
}

static enum sapfs_status studio_regular_presence(
    const char *path,
    bool *present
)
{
    struct sapfs_stat stat;
    enum sapfs_status status;

    if (present == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &stat);
    if (status == SAPFS_STATUS_NOT_FOUND) {
        *present = false;
        return SAPFS_STATUS_OK;
    }
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (stat.directory) {
        return SAPFS_STATUS_IS_DIRECTORY;
    }
    *present = true;
    return SAPFS_STATUS_OK;
}

static enum sapfs_status studio_recover_project(void)
{
    static const char project[] = "SAPSTUDI.SAP";
    static const char scratch[] = "STUTEMP.SAP";
    static const char backup[] = "STUBACK.SAP";
    bool primary = false;
    bool saved = false;
    bool staged = false;
    bool changed = false;
    enum sapfs_status status = studio_regular_presence(project, &primary);

    if (status == SAPFS_STATUS_OK) {
        status = studio_regular_presence(backup, &saved);
    }
    if (status == SAPFS_STATUS_OK) {
        status = studio_regular_presence(scratch, &staged);
    }
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (!primary && saved) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, backup, project);
        changed = status == SAPFS_STATUS_OK;
    } else if (!primary && !saved && staged) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, scratch, project);
        changed = status == SAPFS_STATUS_OK;
        staged = status != SAPFS_STATUS_OK;
    } else if (primary && saved) {
        status = studio_remove_if_present(backup);
        changed = status == SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK && staged) {
        status = studio_remove_if_present(scratch);
        changed = changed || status == SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK && changed) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    return status;
}

static enum sapfs_status studio_load(void)
{
    static const char project[] = "SAPSTUDI.SAP";
    uint8_t bytes[UI_STUDIO_PROJECT_BYTES];
    struct sapfs_stat stat;
    sapfs_handle handle;
    size_t read_bytes = 0U;
    enum sapfs_status status = studio_recover_project();

    studio_reset(false);
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, project, &stat);
    }
    if (status == SAPFS_STATUS_NOT_FOUND) {
        studio_set_status("New project / ready to edit");
        return SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK &&
        (stat.directory || stat.size != UI_STUDIO_PROJECT_BYTES)) {
        status = SAPFS_STATUS_CORRUPT;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, project,
            SAPFS_ACCESS_READ, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_read(handle, bytes, sizeof(bytes), &read_bytes);
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK &&
        (read_bytes != sizeof(bytes) || !studio_decode_project(bytes))) {
        status = SAPFS_STATUS_CORRUPT;
    }
    if (status == SAPFS_STATUS_OK) {
        studio_dirty = false;
        if (studio_selected_clip != UINT8_MAX &&
            studio_load_preview(studio_clip_paths[studio_selected_clip]) !=
                SAPFS_STATUS_OK) {
            studio_set_status("Project opened / source offline");
        } else {
            studio_set_status("Project opened");
        }
    } else {
        studio_reset(false);
        studio_set_status("Project unavailable");
    }
    return status;
}

static enum sapfs_status studio_write_scratch(const uint8_t *bytes)
{
    static const char scratch[] = "STUTEMP.SAP";
    sapfs_handle handle;
    size_t written = 0U;
    enum sapfs_status status = sapfs_create(SAPFS_VOLUME_DATA, scratch);

    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, scratch,
            SAPFS_ACCESS_WRITE, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_write(handle, bytes, UI_STUDIO_PROJECT_BYTES, &written);
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK && written != UI_STUDIO_PROJECT_BYTES) {
        status = SAPFS_STATUS_WRITEBACK;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status != SAPFS_STATUS_OK) {
        (void)studio_remove_if_present(scratch);
    }
    return status;
}

static enum sapfs_status studio_save(void)
{
    static const char project[] = "SAPSTUDI.SAP";
    static const char scratch[] = "STUTEMP.SAP";
    static const char backup[] = "STUBACK.SAP";
    uint8_t bytes[UI_STUDIO_PROJECT_BYTES];
    struct sapfs_stat stat;
    bool original_exists = false;
    bool backed_up = false;
    bool replacement_visible = false;
    enum sapfs_status status = studio_recover_project();

    studio_encode_project(bytes);
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, project, &stat);
        if (status == SAPFS_STATUS_OK) {
            original_exists = true;
        } else if (status == SAPFS_STATUS_NOT_FOUND) {
            status = SAPFS_STATUS_OK;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = studio_write_scratch(bytes);
    }
    if (status == SAPFS_STATUS_OK && original_exists) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, project, backup);
        backed_up = status == SAPFS_STATUS_OK;
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_sync(SAPFS_VOLUME_DATA);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, scratch, project);
        replacement_visible = status == SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status != SAPFS_STATUS_OK && backed_up) {
        if (replacement_visible) {
            (void)sapfs_rename(SAPFS_VOLUME_DATA, project, scratch);
        }
        const enum sapfs_status restore = sapfs_rename(SAPFS_VOLUME_DATA,
            backup, project);

        if (restore == SAPFS_STATUS_OK) {
            (void)sapfs_sync(SAPFS_VOLUME_DATA);
            (void)studio_remove_if_present(scratch);
        } else {
            status = restore;
        }
    } else if (status != SAPFS_STATUS_OK) {
        (void)studio_remove_if_present(scratch);
    }
    if (status == SAPFS_STATUS_OK && original_exists) {
        status = studio_remove_if_present(backup);
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_sync(SAPFS_VOLUME_DATA);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        studio_dirty = false;
        studio_set_status("Project saved");
    } else {
        studio_set_status("Save failed / project retained");
    }
    return status;
}

static bool studio_file_is_bmp(const char *name)
{
    size_t length = 0U;

    if (name == NULL) {
        return false;
    }
    while (name[length] != '\0') {
        ++length;
    }
    if (length < 5U || name[length - 4U] != '.') {
        return false;
    }
    const char b = name[length - 3U];
    const char m = name[length - 2U];
    const char p = name[length - 1U];

    return (b == 'B' || b == 'b') && (m == 'M' || m == 'm') &&
        (p == 'P' || p == 'p');
}

static bool studio_path_used(const char *path)
{
    for (size_t index = 0U; index < studio_clip_count; ++index) {
        if (strings_equal(studio_clip_paths[index], path)) {
            return true;
        }
    }
    return false;
}

static enum sapfs_status studio_load_preview(const char *path)
{
    uint8_t header[UI_STUDIO_BMP_HEADER_BYTES];
    struct sapfs_stat stat;
    sapfs_handle handle = 0U;
    size_t read_bytes = 0U;
    enum sapfs_status status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &stat);
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t pixel_offset = 0U;
    uint32_t row_stride = 0U;
    bool top_down = false;

    studio_preview_loaded = false;
    if (status == SAPFS_STATUS_OK &&
        (stat.directory || stat.size < sizeof(header))) {
        status = SAPFS_STATUS_CORRUPT;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, path, SAPFS_ACCESS_READ,
            &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_read(handle, header, sizeof(header), &read_bytes);
    }
    if (status == SAPFS_STATUS_OK && read_bytes != sizeof(header)) {
        status = SAPFS_STATUS_CORRUPT;
    }
    if (status == SAPFS_STATUS_OK) {
        const int32_t signed_width = (int32_t)studio_load_u32(header, 18U);
        const int32_t signed_height = (int32_t)studio_load_u32(header, 22U);

        pixel_offset = studio_load_u32(header, 10U);
        if (header[0U] != 'B' || header[1U] != 'M' ||
            studio_load_u32(header, 14U) < 40U ||
            studio_load_u16(header, 26U) != 1U ||
            studio_load_u16(header, 28U) != 24U ||
            studio_load_u32(header, 30U) != 0U ||
            signed_width <= 0 || signed_height == 0 ||
            signed_height == INT32_MIN) {
            status = SAPFS_STATUS_CORRUPT;
        } else {
            width = (uint32_t)signed_width;
            top_down = signed_height < 0;
            height = (uint32_t)(top_down ? -signed_height : signed_height);
        }
    }
    if (status == SAPFS_STATUS_OK &&
        (width > UI_STUDIO_BMP_MAX_WIDTH ||
            height > UI_STUDIO_BMP_MAX_HEIGHT ||
            pixel_offset < sizeof(header))) {
        status = SAPFS_STATUS_RANGE;
    }
    if (status == SAPFS_STATUS_OK) {
        const uint64_t row_bytes = (uint64_t)width * 3U;
        const uint64_t stride = (row_bytes + 3U) & ~UINT64_C(3);
        const uint64_t required = (uint64_t)pixel_offset +
            stride * height;

        if (stride > sizeof(studio_bmp_row) || required > stat.size) {
            status = SAPFS_STATUS_RANGE;
        } else {
            row_stride = (uint32_t)stride;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        if ((uint64_t)width * UI_STUDIO_PREVIEW_HEIGHT <=
            (uint64_t)height * UI_STUDIO_PREVIEW_WIDTH) {
            studio_preview_height = UI_STUDIO_PREVIEW_HEIGHT;
            studio_preview_width = (uint32_t)((uint64_t)width *
                UI_STUDIO_PREVIEW_HEIGHT / height);
        } else {
            studio_preview_width = UI_STUDIO_PREVIEW_WIDTH;
            studio_preview_height = (uint32_t)((uint64_t)height *
                UI_STUDIO_PREVIEW_WIDTH / width);
        }
        if (studio_preview_width == 0U || studio_preview_height == 0U) {
            status = SAPFS_STATUS_RANGE;
        }
    }
    for (size_t index = 0U;
         index < UI_STUDIO_PREVIEW_WIDTH * UI_STUDIO_PREVIEW_HEIGHT; ++index) {
        studio_preview_pixels[index] = framebuffer_pack(0U, 0U, 0U);
    }
    for (uint32_t y = 0U; y < studio_preview_height &&
         status == SAPFS_STATUS_OK; ++y) {
        const uint32_t source_y = (uint32_t)((uint64_t)y * height /
            studio_preview_height);
        const uint32_t stored_y = top_down ? source_y :
            height - 1U - source_y;
        const uint64_t row_offset = (uint64_t)pixel_offset +
            (uint64_t)stored_y * row_stride;
        uint64_t position = 0U;

        status = sapfs_seek(handle, (int64_t)row_offset, SAPFS_SEEK_START,
            &position);
        if (status == SAPFS_STATUS_OK && position != row_offset) {
            status = SAPFS_STATUS_RANGE;
        }
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_read(handle, studio_bmp_row, row_stride,
                &read_bytes);
        }
        if (status == SAPFS_STATUS_OK && read_bytes != row_stride) {
            status = SAPFS_STATUS_CORRUPT;
        }
        for (uint32_t x = 0U; x < studio_preview_width &&
             status == SAPFS_STATUS_OK; ++x) {
            const uint32_t source_x = (uint32_t)((uint64_t)x * width /
                studio_preview_width);
            const size_t source = (size_t)source_x * 3U;

            studio_preview_pixels[(size_t)y * UI_STUDIO_PREVIEW_WIDTH + x] =
                framebuffer_pack(studio_bmp_row[source + 2U],
                    studio_bmp_row[source + 1U], studio_bmp_row[source]);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        studio_preview_loaded = true;
    }
    if (handle != 0U) {
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
            studio_preview_loaded = false;
        }
    }
    return status;
}

static void studio_import_clip(void)
{
    struct sapfs_list_entry entries[12U];
    size_t count = 0U;
    enum sapfs_status status;

    if (studio_clip_count >= UI_STUDIO_MAX_CLIPS) {
        studio_set_status("Timeline is full");
        return;
    }
    status = sapfs_list(SAPFS_VOLUME_DATA, file_directory, entries,
        sizeof(entries) / sizeof(entries[0]), &count);
    if (status != SAPFS_STATUS_OK) {
        studio_set_status("Import failed / data unavailable");
        return;
    }
    for (size_t index = 0U; index < count; ++index) {
        char path[SAPFS_MAX_PATH + 1U];

        if (entries[index].directory ||
            !studio_file_is_bmp(entries[index].name) ||
            !entry_path(entries[index].name, path) ||
            studio_path_used(path)) {
            continue;
        }
        size_t path_length = 0U;

        while (path[path_length] != '\0') {
            ++path_length;
        }
        if (path_length > UI_STUDIO_PATH_BYTES) {
            studio_set_status("Import path exceeds Studio bound");
            return;
        }
        status = studio_load_preview(path);
        if (status != SAPFS_STATUS_OK) {
            studio_set_status("BMP rejected / 24-bit RGB required");
            return;
        }
        if (!copy_string(studio_clip_paths[studio_clip_count],
                sizeof(studio_clip_paths[studio_clip_count]), path)) {
            studio_set_status("Import path exceeds Studio bound");
            return;
        }
        studio_clip_durations[studio_clip_count] = 180U;
        studio_selected_clip = studio_clip_count;
        ++studio_clip_count;
        studio_dirty = true;
        studio_set_status("BMP imported / ready to edit");
        return;
    }
    studio_set_status("No new BMP in current data folder");
}

static void studio_trim_clip(void)
{
    if (studio_selected_clip == UINT8_MAX ||
        studio_selected_clip >= studio_clip_count) {
        studio_set_status("Select a clip to trim");
        return;
    }
    if (studio_clip_durations[studio_selected_clip] <= 24U) {
        studio_set_status("Clip reached one-second minimum");
        return;
    }
    studio_clip_durations[studio_selected_clip] -= 24U;
    studio_dirty = true;
    studio_set_status("Trimmed one second");
}

static enum sapfs_status studio_write_all(
    sapfs_handle handle,
    const uint8_t *bytes,
    size_t count
)
{
    size_t written = 0U;
    const enum sapfs_status status = sapfs_write(handle, bytes, count,
        &written);

    return status == SAPFS_STATUS_OK && written != count ?
        SAPFS_STATUS_WRITEBACK : status;
}

static enum sapfs_status studio_write_export_scratch(void)
{
    static const char scratch[] = "STUOUT.BMP";
    uint8_t header[UI_STUDIO_BMP_HEADER_BYTES] = { 0U };
    const uint32_t row_stride =
        (studio_preview_width * 3U + 3U) & ~UINT32_C(3);
    const uint32_t file_bytes = UI_STUDIO_BMP_HEADER_BYTES +
        row_stride * studio_preview_height;
    sapfs_handle handle = 0U;
    enum sapfs_status status = studio_remove_if_present(scratch);

    header[0U] = 'B';
    header[1U] = 'M';
    studio_store_u32(header, 2U, file_bytes);
    studio_store_u32(header, 10U, UI_STUDIO_BMP_HEADER_BYTES);
    studio_store_u32(header, 14U, 40U);
    studio_store_u32(header, 18U, studio_preview_width);
    studio_store_u32(header, 22U, studio_preview_height);
    studio_store_u16(header, 26U, 1U);
    studio_store_u16(header, 28U, 24U);
    studio_store_u32(header, 34U,
        row_stride * studio_preview_height);
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_create(SAPFS_VOLUME_DATA, scratch);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, scratch,
            SAPFS_ACCESS_WRITE, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = studio_write_all(handle, header, sizeof(header));
    }
    for (uint32_t row = 0U; row < studio_preview_height &&
         status == SAPFS_STATUS_OK; ++row) {
        const uint32_t source_y = studio_preview_height - 1U - row;

        for (uint32_t x = 0U; x < row_stride; ++x) {
            studio_bmp_row[x] = 0U;
        }
        for (uint32_t x = 0U; x < studio_preview_width; ++x) {
            const uint32_t pixel = studio_preview_pixels[
                (size_t)source_y * UI_STUDIO_PREVIEW_WIDTH + x
            ];
            const size_t destination = (size_t)x * 3U;

            studio_bmp_row[destination] =
                (uint8_t)(pixel >> logo_blue_shift);
            studio_bmp_row[destination + 1U] =
                (uint8_t)(pixel >> logo_green_shift);
            studio_bmp_row[destination + 2U] =
                (uint8_t)(pixel >> logo_red_shift);
        }
        status = studio_write_all(handle, studio_bmp_row, row_stride);
    }
    if (handle != 0U) {
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status != SAPFS_STATUS_OK) {
        (void)studio_remove_if_present(scratch);
    }
    return status;
}

static enum sapfs_status studio_recover_export(void)
{
    static const char output[] = "EXPORT.BMP";
    static const char scratch[] = "STUOUT.BMP";
    static const char backup[] = "OUTBACK.BMP";
    bool output_exists = false;
    bool scratch_exists = false;
    bool backup_exists = false;
    bool changed = false;
    enum sapfs_status status = studio_regular_presence(output,
        &output_exists);

    if (status == SAPFS_STATUS_OK) {
        status = studio_regular_presence(scratch, &scratch_exists);
    }
    if (status == SAPFS_STATUS_OK) {
        status = studio_regular_presence(backup, &backup_exists);
    }
    if (status == SAPFS_STATUS_OK && !output_exists && backup_exists) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, backup, output);
        changed = status == SAPFS_STATUS_OK;
        backup_exists = status != SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK && output_exists && backup_exists) {
        status = studio_remove_if_present(backup);
        changed = status == SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK && scratch_exists) {
        status = studio_remove_if_present(scratch);
        changed = changed || status == SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK && changed) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    return status;
}

static enum sapfs_status studio_export(void)
{
    static const char output[] = "EXPORT.BMP";
    static const char scratch[] = "STUOUT.BMP";
    static const char backup[] = "OUTBACK.BMP";
    struct sapfs_stat stat;
    bool original_exists = false;
    bool backed_up = false;
    bool replacement_visible = false;
    enum sapfs_status status;

    if (!studio_preview_loaded) {
        studio_set_status("Import a BMP before export");
        return SAPFS_STATUS_NOT_FOUND;
    }
    status = studio_recover_export();
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, output, &stat);
        if (status == SAPFS_STATUS_OK) {
            original_exists = !stat.directory;
            status = stat.directory ? SAPFS_STATUS_IS_DIRECTORY :
                SAPFS_STATUS_OK;
        } else if (status == SAPFS_STATUS_NOT_FOUND) {
            status = SAPFS_STATUS_OK;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = studio_write_export_scratch();
    }
    if (status == SAPFS_STATUS_OK && original_exists) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, output, backup);
        backed_up = status == SAPFS_STATUS_OK;
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_sync(SAPFS_VOLUME_DATA);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, scratch, output);
        replacement_visible = status == SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status != SAPFS_STATUS_OK && backed_up) {
        if (replacement_visible) {
            (void)sapfs_rename(SAPFS_VOLUME_DATA, output, scratch);
        }
        const enum sapfs_status restore = sapfs_rename(SAPFS_VOLUME_DATA,
            backup, output);

        if (restore == SAPFS_STATUS_OK) {
            (void)sapfs_sync(SAPFS_VOLUME_DATA);
            (void)studio_remove_if_present(scratch);
        } else {
            status = restore;
        }
    } else if (status != SAPFS_STATUS_OK) {
        (void)studio_remove_if_present(scratch);
    }
    if (status == SAPFS_STATUS_OK && original_exists) {
        status = studio_remove_if_present(backup);
        if (status == SAPFS_STATUS_OK) {
            status = sapfs_sync(SAPFS_VOLUME_DATA);
        }
    }
    if (status == SAPFS_STATUS_OK) {
        studio_set_status("EXPORT.BMP written to data");
        (void)files_refresh();
    } else {
        studio_set_status("Export failed / previous output retained");
    }
    return status;
}

static struct ui_rect settings_back_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;

    return (struct ui_rect){ client.x + 10U, client.y + 8U, 76U, 26U };
}

static struct ui_rect settings_category_rect(size_t index)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t cell_width = (client.width - 64U) / 4U;

    return (struct ui_rect){ client.x + 32U + (uint32_t)(index % 4U) * cell_width,
        client.y + 58U + (uint32_t)(index / 4U) * 132U,
        cell_width - 16U, 106U };
}

static struct ui_rect settings_appearance_rect(bool dark)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t width = 240U;
    const uint32_t gap = 44U;
    const uint32_t total = width * 2U + gap;

    return (struct ui_rect){ client.x + (client.width - total) / 2U +
        (dark ? width + gap : 0U), client.y + 96U, width, 188U };
}

static struct ui_rect settings_wallpaper_rect(size_t index)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t gap = 8U;
    const uint32_t width = (client.width - 32U - gap * 6U) / 7U;

    return (struct ui_rect){ client.x + 16U + (uint32_t)(index % 7U) *
        (width + gap), client.y + 62U + (uint32_t)(index / 7U) * 104U,
        width, 92U };
}

static struct ui_rect settings_option_rect(size_t index)
{
    const struct ui_rect client = state.layout.panel_client;

    return (struct ui_rect){ client.x + 76U,
        client.y + 86U + (uint32_t)index * 66U,
        client.width - 152U, 52U };
}

static size_t settings_interactive_option_count(size_t page)
{
    if (page == 2U || page == 10U) {
        return 3U;
    }
    if (page == 3U || page == 4U || page == 5U) {
        return 2U;
    }
    if (page == 6U) {
        return 1U;
    }
    return 0U;
}

static enum ui_status draw_settings_row(
    size_t index,
    struct ui_rect damage,
    const char *label,
    const char *value,
    bool interactive,
    bool enabled
)
{
    const struct ui_rect row = settings_option_rect(index);
    const struct ui_rect value_box = { row.x + row.width - 128U,
        row.y + 11U, 108U, 30U };
    enum ui_status status = gradient_rect(row, damage,
        0xFFU, 0xFFU, 0xFFU, 0xE4U, 0xE7U, 0xE8U);

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(row, damage, 1U,
            framebuffer_pack(0xA0U, 0xA5U, 0xA7U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(row, damage, row.x + 17U, row.y + 33U,
            label, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = interactive ?
            gradient_rect(value_box, damage,
                enabled ? 0x8DU : 0xEEU,
                enabled ? 0xBFU : 0xEFU,
                enabled ? 0xD3U : 0xEFU,
                enabled ? 0x4DU : 0xC8U,
                enabled ? 0x8EU : 0xCCU,
                enabled ? 0xAAU : 0xCEU) :
            fill_clipped(value_box, damage,
                framebuffer_pack(0xF4U, 0xF5U, 0xF5U));
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(value_box, damage, 1U,
            framebuffer_pack(0x8BU, 0x91U, 0x93U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(value_box, damage,
            centered_text_x(value_box, value), value_box.y + 20U,
            value, state.theme.ink);
    }
    return status;
}

static enum ui_status draw_settings_control_page(
    size_t page,
    struct ui_rect damage
)
{
    enum ui_status status = UI_STATUS_OK;

    if (page == 2U) {
        status = draw_settings_row(0U, damage, "Magnification",
            dock_magnification ? "On" : "Off", true, dock_magnification);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Reflections",
                dock_reflections ? "On" : "Off", true, dock_reflections);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(2U, damage, "Hover labels",
                dock_labels ? "On" : "Off", true, dock_labels);
        }
        return status;
    }
    if (page == 3U) {
        status = draw_settings_row(0U, damage, "Menu bar glass",
            menu_glass ? "Glass" : "Solid", true, menu_glass);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Window contrast",
                window_high_contrast ? "High" : "Standard", true,
                window_high_contrast);
        }
        return status;
    }
    if (page == 4U) {
        status = draw_settings_row(0U, damage, "Dock focus wrapping",
            keyboard_focus_wrap ? "On" : "Off", true,
            keyboard_focus_wrap);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Keyboard focus label",
                keyboard_focus_indicator ? "On" : "Off", true,
                keyboard_focus_indicator);
        }
        return status;
    }
    if (page == 5U) {
        status = draw_settings_row(0U, damage, "Cursor size",
            cursor_large ? "Large" : "Classic", true, cursor_large);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Cursor contrast",
                cursor_dark ? "Dark" : "Light", true, cursor_dark);
        }
        return status;
    }
    if (page == 6U) {
        status = draw_settings_row(0U, damage, "Fluid window motion",
            window_motion ? "Full" : "Reduced", true, window_motion);
        return status;
    }
    if (page == 7U) {
        const struct network_state network = network_get_state();
        char address[32] = "Not configured";
        const char *source = "None";

        if (network.configuration.configured) {
            size_t at = 0U;
            const uint32_t ipv4 = network.configuration.address;
            address[0] = '\0';
            at = append_u64(address, sizeof(address), at, ipv4 & 0xFFU);
            at = append_text(address, sizeof(address), at, ".");
            at = append_u64(address, sizeof(address), at,
                (ipv4 >> 8U) & 0xFFU);
            at = append_text(address, sizeof(address), at, ".");
            at = append_u64(address, sizeof(address), at,
                (ipv4 >> 16U) & 0xFFU);
            at = append_text(address, sizeof(address), at, ".");
            (void)append_u64(address, sizeof(address), at,
                (ipv4 >> 24U) & 0xFFU);
        }
        if (network.configuration.source == NETWORK_CONFIGURATION_DHCP) {
            source = "DHCP";
        } else if (network.configuration.source ==
                NETWORK_CONFIGURATION_STATIC) {
            source = "Static";
        }
        status = draw_settings_row(0U, damage, "virtio-net link",
            network.active ? "Active" : "Unavailable", false,
            network.active);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Configuration",
                source, false, false);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(2U, damage, "IPv4 address",
                address, false, false);
        }
        return status;
    }
    if (page == 8U) {
        const struct sapfs_drive_info system = sapfs_drive(SAPFS_VOLUME_SYSTEM);
        const struct sapfs_drive_info data = sapfs_drive(SAPFS_VOLUME_DATA);
        char data_free[32] = "Unavailable";

        if (data.present) {
            size_t at = 0U;
            data_free[0] = '\0';
            at = append_u64(data_free, sizeof(data_free), at,
                data.free_bytes / (1024U * 1024U));
            (void)append_text(data_free, sizeof(data_free), at, " MiB free");
        }
        status = draw_settings_row(0U, damage, "System volume",
            system.mounted ? "Mounted / read-only" : "Unavailable",
            false, false);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Data volume",
                data.mounted ? "Mounted / writable" : "Unavailable",
                false, false);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(2U, damage, "Capacity",
                data_free, false, false);
        }
        return status;
    }
    if (page == 9U) {
        const struct camera_frame_info camera = camera_get_info();
        char geometry[32] = "No signal";

        if (camera.connected) {
            size_t at = append_u64(geometry, sizeof(geometry), 0U,
                camera.width);
            at = append_text(geometry, sizeof(geometry), at, " x ");
            (void)append_u64(geometry, sizeof(geometry), at, camera.height);
        }
        status = draw_settings_row(0U, damage, "Camera device",
            camera.connected ? "Connected" : "Not connected", false,
            camera.connected);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Live frame",
                geometry, false, false);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(2U, damage, "Capture pipeline",
                "RGB888 / BMP", false, false);
        }
        return status;
    }
    if (page == 10U) {
        status = draw_settings_row(0U, damage, "Window shadows",
            window_shadows ? "On" : "Off", true, window_shadows);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Window bevels",
                window_bevels ? "On" : "Off", true, window_bevels);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(2U, damage, "Title gradients",
                window_title_gradient ? "On" : "Off", true,
                window_title_gradient);
        }
        return status;
    }
    if (page == 11U) {
        status = draw_settings_row(0U, damage, "Kernel",
            "Sapote 2.2.0 dev", false, false);
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(1U, damage, "Interface",
                "Sapote Redwood", false, false);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_row(2U, damage, "Rendering",
                "RGB565 / Inter", false, false);
        }
        return status;
    }
    return UI_STATUS_BAD_ELEMENT;
}

static enum ui_status draw_settings_category_icon(
    size_t index,
    struct ui_rect icon,
    struct ui_rect damage,
    const uint8_t colour[3U]
)
{
    (void)colour;
    if (index >= UI_SETTINGS_CATEGORY_COUNT) {
        return UI_STATUS_BAD_ELEMENT;
    }
    return draw_alpha_subimage(icon, damage, settings_category_icon_pixels,
        settings_category_icon_alpha, settings_category_icon_width,
        settings_category_icon_height,
        (struct ui_rect){ (uint32_t)(index % 4U) * 64U,
            (uint32_t)(index / 4U) * 64U, 64U, 64U });
#if 0
    enum ui_status status = gradient_rect(icon, damage,
        0xFAU, 0xFAU, 0xFAU, colour[0U], colour[1U], colour[2U]);
    const uint32_t white = state.theme.white;
    const uint32_t ink = state.theme.ink;

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(icon, damage, 1U,
            framebuffer_pack(0x7AU, 0x7FU, 0x82U));
    }
    if (index == 0U) {
        static const uint8_t swatches[4U][3U] = {
            { 0xD9U, 0x55U, 0x4FU }, { 0xE6U, 0xC4U, 0x62U },
            { 0x68U, 0xA9U, 0xC5U }, { 0x94U, 0x7BU, 0xB4U }
        };
        for (size_t swatch = 0U; swatch < 4U && status == UI_STATUS_OK;
             ++swatch) {
            status = fill_clipped((struct ui_rect){ icon.x + 7U +
                (uint32_t)(swatch % 2U) * 21U, icon.y + 7U +
                (uint32_t)(swatch / 2U) * 21U, 18U, 18U }, damage,
                framebuffer_pack(swatches[swatch][0U], swatches[swatch][1U],
                    swatches[swatch][2U]));
        }
    } else if (index == 1U || index == 3U) {
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 8U, icon.y + 8U,
                38U, 27U }, damage, ink);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 11U,
                icon.y + 11U, 32U, 21U }, damage,
                index == 1U ? framebuffer_pack(0x41U, 0x86U, 0xA2U) : white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 25U,
                icon.y + 35U, 4U, 8U }, damage, ink);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 17U,
                icon.y + 43U, 20U, 3U }, damage, ink);
        }
    } else if (index == 2U) {
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 7U,
                icon.y + 38U, 40U, 5U }, damage, ink);
        }
        for (uint32_t item = 0U; item < 4U && status == UI_STATUS_OK; ++item) {
            status = fill_clipped((struct ui_rect){ icon.x + 10U + item * 10U,
                icon.y + 22U, 7U, 15U }, damage,
                item == 2U ? state.theme.accent_red : white);
        }
    } else if (index == 4U) {
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 6U,
                icon.y + 13U, 42U, 29U }, damage, ink);
        }
        for (uint32_t row = 0U; row < 3U && status == UI_STATUS_OK; ++row) {
            for (uint32_t key = 0U; key < 6U && status == UI_STATUS_OK; ++key) {
                status = fill_clipped((struct ui_rect){ icon.x + 9U + key * 6U,
                    icon.y + 16U + row * 8U, 4U, 5U }, damage, white);
            }
        }
    } else if (index == 5U) {
        for (uint32_t row = 0U; row < 28U && status == UI_STATUS_OK; ++row) {
            const uint32_t width = row < 20U ? row / 2U + 2U : 5U;
            status = fill_clipped((struct ui_rect){ icon.x + 15U,
                icon.y + 8U + row, width, 1U }, damage, white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 24U,
                icon.y + 29U, 4U, 14U }, damage, ink);
        }
    } else if (index == 6U) {
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 8U,
                icon.y + 21U, 10U, 14U }, damage, white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 18U,
                icon.y + 16U, 9U, 24U }, damage, ink);
        }
        for (uint32_t wave = 0U; wave < 3U && status == UI_STATUS_OK; ++wave) {
            status = fill_clipped((struct ui_rect){ icon.x + 31U + wave * 5U,
                icon.y + 19U - wave * 3U, 2U, 18U + wave * 6U },
                damage, white);
        }
    } else if (index == 7U) {
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 12U,
                icon.y + 26U, 30U, 3U }, damage, white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 26U,
                icon.y + 12U, 3U, 30U }, damage, white);
        }
        const struct ui_rect nodes[3U] = {
            { icon.x + 7U, icon.y + 21U, 11U, 11U },
            { icon.x + 23U, icon.y + 7U, 11U, 11U },
            { icon.x + 36U, icon.y + 34U, 11U, 11U }
        };
        for (size_t node = 0U; node < 3U && status == UI_STATUS_OK; ++node) {
            status = fill_clipped(nodes[node], damage, ink);
        }
    } else if (index == 8U) {
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 8U,
                icon.y + 10U, 38U, 34U }, damage, ink);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 12U,
                icon.y + 14U, 30U, 22U }, damage, white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 35U,
                icon.y + 39U, 6U, 3U }, damage, state.theme.accent_green);
        }
    } else if (index == 9U) {
        if (status == UI_STATUS_OK) {
            status = draw_circle(icon.x + 27U, icon.y + 27U, 19U,
                damage, ink);
        }
        if (status == UI_STATUS_OK) {
            status = draw_circle(icon.x + 27U, icon.y + 27U, 16U,
                damage, white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 26U,
                icon.y + 15U, 3U, 13U }, damage, ink);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 27U,
                icon.y + 26U, 10U, 3U }, damage, ink);
        }
    } else if (index == 10U) {
        if (status == UI_STATUS_OK) {
            status = draw_circle(icon.x + 27U, icon.y + 28U, 18U,
                damage, white);
        }
        if (status == UI_STATUS_OK) {
            status = draw_circle(icon.x + 27U, icon.y + 28U, 14U,
                damage, ink);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ icon.x + 25U,
                icon.y + 6U, 5U, 23U }, damage, white);
        }
    } else if (index == 11U && status == UI_STATUS_OK) {
        status = draw_circle(icon.x + 27U, icon.y + 27U, 19U,
            damage, white);
        if (status == UI_STATUS_OK) {
            status = draw_text(icon, damage, centered_text_x(icon, "i"),
                icon.y + 35U, "i", ink);
        }
    }
    return status;
#endif
}

static enum ui_status draw_settings_preview(
    struct ui_rect card,
    struct ui_rect damage,
    bool dark
)
{
    const struct ui_rect screen = { card.x + 18U, card.y + 18U,
        card.width - 36U, 122U };
    enum ui_status status = gradient_rect(card, damage,
        0xF8U, 0xF8U, 0xF8U, 0xCFU, 0xD1U, 0xD2U);

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(card, damage, dock_dark == dark ? 3U : 1U,
            dock_dark == dark ? state.theme.accent_teal :
                framebuffer_pack(0x7BU, 0x80U, 0x82U));
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect(screen, damage, 0x3AU, 0x75U, 0x91U,
            0x09U, 0x22U, 0x38U);
    }
    if (status == UI_STATUS_OK) {
        status = translucent_fill((struct ui_rect){ screen.x + 28U,
            screen.y + screen.height - 22U, screen.width - 56U, 18U },
            damage, dark ? framebuffer_pack(0x18U, 0x1AU, 0x1CU) :
                framebuffer_pack(0xD7U, 0xDCU, 0xDEU), 230U);
    }
    for (uint32_t icon = 0U; icon < 5U && status == UI_STATUS_OK; ++icon) {
        status = fill_clipped((struct ui_rect){ screen.x + 40U + icon * 25U,
            screen.y + screen.height - 19U, 15U, 13U }, damage,
            icon == 3U ? state.theme.accent_red : state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(card, damage, centered_text_x(card,
            dark ? "Dark" : "Light"), card.y + 170U,
            dark ? "Dark" : "Light", state.theme.ink);
    }
    return status;
}

static enum ui_status draw_settings_wallpaper_choice(
    size_t index,
    struct ui_rect damage,
    const char *label
)
{
    const struct ui_rect tile = settings_wallpaper_rect(index);
    const struct ui_rect preview = { tile.x + 3U, tile.y + 3U,
        tile.width - 6U, 62U };
    const struct ui_rect clipped = rect_intersection(preview, damage);
    enum ui_status status = UI_STATUS_OK;

    if (index >= UI_WALLPAPER_COUNT || preview.width > 128U ||
        preview.height > 72U) {
        return UI_STATUS_BAD_ELEMENT;
    }
    if (clipped.width != 0U && clipped.height != 0U) {
        if (sapote_wallpaper_decode((uint32_t)index,
                settings_wallpaper_thumbnail_pixels, 128U * 72U,
                preview.width, preview.height, logo_red_shift,
                logo_green_shift, logo_blue_shift) != WALLPAPER_STATUS_OK) {
            return UI_STATUS_WALLPAPER_FAILURE;
        }
        for (uint32_t y = 0U; y < clipped.height &&
             status == UI_STATUS_OK; ++y) {
            for (uint32_t x = 0U; x < clipped.width; ++x) {
                const uint32_t source_x = clipped.x - preview.x + x;
                const uint32_t source_y = clipped.y - preview.y + y;
                if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                        settings_wallpaper_thumbnail_pixels[source_y *
                            preview.width + source_x]) != SURFACE_STATUS_OK) {
                    status = UI_STATUS_SURFACE_FAILURE;
                    break;
                }
            }
        }
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(preview, damage,
            desktop_wallpaper == index ? 3U : 1U,
            desktop_wallpaper == index ? state.theme.accent_teal :
                framebuffer_pack(0x78U, 0x7FU, 0x82U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(tile, damage, centered_text_x(tile, label),
            tile.y + 84U, label, state.theme.ink);
    }
    return status;
}

static enum ui_status draw_settings_app(struct ui_rect damage)
{
    static const char *const categories[UI_SETTINGS_CATEGORY_COUNT] = {
        "Appearance", "Desktop", "Dock", "Displays",
        "Keyboard", "Pointer", "Performance", "Network",
        "Storage", "Camera", "Windows", "About"
    };
    static const uint8_t colours[UI_SETTINGS_CATEGORY_COUNT][3U] = {
        { 0x68U, 0xA9U, 0xC5U }, { 0x58U, 0x8CU, 0xB7U },
        { 0x9AU, 0xA0U, 0xA4U }, { 0x6EU, 0x8CU, 0xB2U },
        { 0xD0U, 0xD1U, 0xC8U }, { 0xE7U, 0xE7U, 0xE2U },
        { 0x94U, 0x7BU, 0xB4U }, { 0x64U, 0xA3U, 0x83U },
        { 0xD1U, 0xACU, 0x58U }, { 0xB9U, 0x8BU, 0x52U },
        { 0x7DU, 0x8FU, 0xA2U }, { 0xD9U, 0x55U, 0x4FU }
    };
    const struct ui_rect client = state.layout.panel_client;
    enum ui_status status = fill_clipped(client, damage,
        framebuffer_pack(0xEEU, 0xEFU, 0xEFU));

    if (settings_page < 0) {
        if (status == UI_STATUS_OK) {
            status = draw_text(client, damage,
                centered_text_x(client, "Settings"), client.y + 29U,
                "Settings", state.theme.ink);
        }
        for (size_t index = 0U; index < UI_SETTINGS_CATEGORY_COUNT &&
             status == UI_STATUS_OK; ++index) {
            const struct ui_rect tile = settings_category_rect(index);
            const struct ui_rect icon = { tile.x + (tile.width - 54U) / 2U,
                tile.y + 4U, 54U, 54U };

            status = draw_settings_category_icon(index, icon, damage,
                colours[index]);
            if (status == UI_STATUS_OK) {
                status = draw_text(tile, damage,
                    centered_text_x(tile, categories[index]), tile.y + 82U,
                    categories[index], state.theme.ink);
            }
        }
        return status;
    }

    const size_t page = (size_t)settings_page;
    if (status == UI_STATUS_OK) {
        status = draw_button(settings_back_rect(), damage, "< All");
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(client, damage,
            centered_text_x(client, categories[page]), client.y + 31U,
            categories[page], state.theme.ink);
    }
    if (page == 0U) {
        if (status == UI_STATUS_OK) {
            status = draw_settings_preview(settings_appearance_rect(false),
                damage, false);
        }
        if (status == UI_STATUS_OK) {
            status = draw_settings_preview(settings_appearance_rect(true),
                damage, true);
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(client, damage, client.x + 56U,
                client.y + 330U, "Appearance changes the 3D dock colour only.",
                state.theme.ink);
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(client, damage, client.x + 56U,
                client.y + 358U,
                "Geometry, reflections and magnification stay identical.",
                state.theme.title_inactive);
        }
        return status;
    }

    if (page == 1U) {
        static const char *const wallpapers[UI_WALLPAPER_COUNT] = {
            "Nebula", "Stars", "Milky Way", "Reflection", "Falls",
            "River", "Dunes", "Aurora", "Fjord", "Golden Mist",
            "Yosemite", "Alpine", "Tropics", "Ocean"
        };
        if (status == UI_STATUS_OK) {
            status = draw_text(client, damage, client.x + 18U,
                client.y + 52U, "Choose a wallpaper", state.theme.ink);
        }
        for (size_t index = 0U; index < UI_WALLPAPER_COUNT &&
             status == UI_STATUS_OK; ++index) {
            status = draw_settings_wallpaper_choice(index, damage,
                wallpapers[index]);
        }
        return status;
    }

    return status == UI_STATUS_OK ?
        draw_settings_control_page(page, damage) : status;
}

static bool select_desktop_wallpaper(uint8_t index)
{
    if (index >= UI_WALLPAPER_COUNT ||
        sapote_wallpaper_decode(index, wallpaper_pixels, 1024U * 768U,
            1024U, 768U, logo_red_shift, logo_green_shift,
            logo_blue_shift) != WALLPAPER_STATUS_OK) {
        return false;
    }
    desktop_wallpaper = index;
    return true;
}

static bool camera_refresh_frame(void)
{
    const struct camera_frame_info current = camera_get_info();
    struct camera_frame_info snapshot;

    if (!current.connected) {
        camera_frame_available = false;
        camera_seen_generation = current.generation;
        (void)copy_string(camera_status, sizeof(camera_status),
            "No camera connected");
        return false;
    }
    if (camera_frame_available &&
            current.generation == camera_seen_generation) {
        return true;
    }
    if (camera_snapshot(camera_scene_pixels,
            UI_CAMERA_SCENE_WIDTH * UI_CAMERA_SCENE_HEIGHT,
            UI_CAMERA_SCENE_WIDTH, UI_CAMERA_SCENE_HEIGHT,
            logo_red_shift, logo_green_shift, logo_blue_shift,
            &snapshot) != CAMERA_STATUS_OK) {
        camera_frame_available = false;
        (void)copy_string(camera_status, sizeof(camera_status),
            "Camera frame unavailable");
        return false;
    }
    camera_frame_available = true;
    camera_seen_generation = snapshot.generation;
    (void)copy_string(camera_status, sizeof(camera_status),
        "Camera ready");
    return true;
}

static uint32_t camera_frame_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    if (!camera_frame_available || width == 0U || height == 0U) {
        return framebuffer_pack(0x16U, 0x19U, 0x1CU);
    }
    const uint32_t source_x = x * UI_CAMERA_SCENE_WIDTH / width;
    const uint32_t source_y = y * UI_CAMERA_SCENE_HEIGHT / height;

    return camera_scene_pixels[(size_t)source_y * UI_CAMERA_SCENE_WIDTH +
        source_x];
}

static struct ui_rect camera_preview_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;

    return (struct ui_rect){ client.x + 12U, client.y + 12U,
        client.width - 24U, client.height - 102U };
}

static struct ui_rect camera_capture_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;

    return (struct ui_rect){ client.x + client.width / 2U - 27U,
        client.y + client.height - 78U, 54U, 54U };
}

static struct ui_rect camera_controls_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;
    const struct ui_rect preview = camera_preview_rect();

    return (struct ui_rect){ client.x, preview.y + preview.height + 4U,
        client.width,
        client.y + client.height - preview.y - preview.height - 4U };
}

static enum sapfs_status camera_capture(void)
{
    static const char scratch[] = "CAMTEMP.BMP";
    char output[] = "PHOTO00.BMP";
    uint8_t header[UI_CAMERA_BMP_HEADER_BYTES] = { 0U };
    const uint32_t row_stride = UI_CAMERA_CAPTURE_WIDTH * 3U;
    const uint32_t file_bytes = UI_CAMERA_BMP_HEADER_BYTES +
        row_stride * UI_CAMERA_CAPTURE_HEIGHT;
    sapfs_handle handle = 0U;
    enum sapfs_status status = SAPFS_STATUS_FULL;
    bool output_found = false;
    struct camera_frame_info frame;

    if (camera_snapshot(camera_scene_pixels,
            UI_CAMERA_SCENE_WIDTH * UI_CAMERA_SCENE_HEIGHT,
            UI_CAMERA_CAPTURE_WIDTH, UI_CAMERA_CAPTURE_HEIGHT,
            logo_red_shift, logo_green_shift, logo_blue_shift,
            &frame) != CAMERA_STATUS_OK) {
        camera_frame_available = false;
        (void)copy_string(camera_status, sizeof(camera_status),
            "Connect a camera before taking a photo");
        return SAPFS_STATUS_NOT_FOUND;
    }

    for (uint32_t number = 0U; number < 100U; ++number) {
        struct sapfs_stat stat;
        output[5U] = (char)('0' + number / 10U);
        output[6U] = (char)('0' + number % 10U);
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, output, &stat);
        if (status == SAPFS_STATUS_NOT_FOUND) {
            status = SAPFS_STATUS_OK;
            output_found = true;
            break;
        }
    }
    if (!output_found || status != SAPFS_STATUS_OK) {
        (void)copy_string(camera_status, sizeof(camera_status),
            "Photo library is full");
        return status;
    }
    header[0U] = 'B'; header[1U] = 'M';
    studio_store_u32(header, 2U, file_bytes);
    studio_store_u32(header, 10U, UI_CAMERA_BMP_HEADER_BYTES);
    studio_store_u32(header, 14U, 40U);
    studio_store_u32(header, 18U, UI_CAMERA_CAPTURE_WIDTH);
    studio_store_u32(header, 22U, UI_CAMERA_CAPTURE_HEIGHT);
    studio_store_u16(header, 26U, 1U);
    studio_store_u16(header, 28U, 24U);
    studio_store_u32(header, 34U, row_stride * UI_CAMERA_CAPTURE_HEIGHT);
    status = studio_remove_if_present(scratch);
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_create(SAPFS_VOLUME_DATA, scratch);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, scratch,
            SAPFS_ACCESS_WRITE, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = studio_write_all(handle, header, sizeof(header));
    }
    for (uint32_t row = 0U; row < UI_CAMERA_CAPTURE_HEIGHT &&
         status == SAPFS_STATUS_OK; ++row) {
        const uint32_t source_y = UI_CAMERA_CAPTURE_HEIGHT - 1U - row;
        for (uint32_t x = 0U; x < UI_CAMERA_CAPTURE_WIDTH; ++x) {
            const uint32_t pixel = camera_scene_pixels[
                (size_t)source_y * UI_CAMERA_CAPTURE_WIDTH + x];
            const size_t destination = (size_t)x * 3U;
            camera_bmp_row[destination] =
                (uint8_t)(pixel >> logo_blue_shift);
            camera_bmp_row[destination + 1U] =
                (uint8_t)(pixel >> logo_green_shift);
            camera_bmp_row[destination + 2U] =
                (uint8_t)(pixel >> logo_red_shift);
        }
        status = studio_write_all(handle, camera_bmp_row, row_stride);
    }
    if (handle != 0U) {
        const enum sapfs_status close_status = sapfs_close(handle);
        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_rename(SAPFS_VOLUME_DATA, scratch, output);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status == SAPFS_STATUS_OK) {
        ++camera_capture_count;
        size_t at = append_text(camera_status, sizeof(camera_status), 0U,
            "Saved ");
        (void)append_text(camera_status, sizeof(camera_status), at, output);
        (void)files_refresh();
    } else {
        (void)studio_remove_if_present(scratch);
        (void)copy_string(camera_status, sizeof(camera_status),
            "Capture failed / no partial photo retained");
    }
    camera_frame_available = false;
    return status;
}

static enum ui_status draw_circle(
    uint32_t center_x,
    uint32_t center_y,
    uint32_t radius,
    struct ui_rect damage,
    uint32_t pixel
)
{
    enum ui_status status = UI_STATUS_OK;
    const uint64_t radius_squared = (uint64_t)radius * radius;

    for (int32_t y = -(int32_t)radius; y <= (int32_t)radius &&
         status == UI_STATUS_OK; ++y) {
        for (int32_t x = -(int32_t)radius; x <= (int32_t)radius; ++x) {
            if ((uint64_t)(x * x + y * y) > radius_squared) {
                continue;
            }
            status = fill_clipped((struct ui_rect){
                (uint32_t)((int32_t)center_x + x),
                (uint32_t)((int32_t)center_y + y), 1U, 1U
            }, damage, pixel);
            if (status != UI_STATUS_OK) {
                break;
            }
        }
    }
    return status;
}

static enum ui_status draw_camera_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const struct ui_rect preview = camera_preview_rect();
    const struct ui_rect clipped = rect_intersection(preview, damage);
    const struct ui_rect controls = camera_controls_rect();
    const struct ui_rect capture = camera_capture_rect();
    enum ui_status status = fill_clipped(client, damage,
        framebuffer_pack(0xD7U, 0xD9U, 0xDAU));

    (void)camera_refresh_frame();

    for (uint32_t y = 0U; y < clipped.height && status == UI_STATUS_OK; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t local_x = clipped.x - preview.x + x;
            const uint32_t local_y = clipped.y - preview.y + y;
            camera_preview_row[x] = camera_frame_pixel(local_x, local_y,
                preview.width, preview.height);
        }
        if (surface_blit(canvas, clipped.x, clipped.y + y,
                camera_preview_row, clipped.width, 1U,
                clipped.width * SURFACE_BYTES_PER_PIXEL) !=
                    SURFACE_STATUS_OK) {
            status = UI_STATUS_SURFACE_FAILURE;
        }
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(preview, damage, 1U,
            framebuffer_pack(0x4CU, 0x51U, 0x54U));
    }
    if (status == UI_STATUS_OK && !camera_frame_available) {
        status = draw_text(preview, damage,
            centered_text_x(preview, "No camera connected"),
            preview.y + preview.height / 2U,
            "No camera connected", framebuffer_pack(0xD4U, 0xD8U, 0xDAU));
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect(controls, damage, 0xF7U, 0xF7U, 0xF7U,
            0xC1U, 0xC4U, 0xC6U);
    }
    if (status == UI_STATUS_OK) {
        status = draw_circle(capture.x + capture.width / 2U,
            capture.y + capture.height / 2U, 26U, damage,
            framebuffer_pack(0xF8U, 0xF8U, 0xF8U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_circle(capture.x + capture.width / 2U,
            capture.y + capture.height / 2U, 20U, damage,
            state.theme.accent_red);
    }
    if (status == UI_STATUS_OK) {
        const uint32_t center_x = capture.x + capture.width / 2U;
        const uint32_t center_y = capture.y + capture.height / 2U;
        status = fill_clipped((struct ui_rect){ center_x - 11U,
            center_y - 7U, 22U, 15U }, damage, state.theme.white);
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ center_x - 6U,
                center_y - 10U, 9U, 4U }, damage, state.theme.white);
        }
        if (status == UI_STATUS_OK) {
            status = draw_circle(center_x, center_y, 5U, damage,
                state.theme.accent_red);
        }
        if (status == UI_STATUS_OK) {
            status = draw_circle(center_x, center_y, 2U, damage,
                state.theme.white);
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(controls, damage, controls.x + 18U,
            controls.y + 36U, camera_status, state.theme.ink);
    }
    return status;
}

static struct ui_rect file_entry_rect(size_t index)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t content_x = client.x + 190U;
    const uint32_t content_width = client.width - 190U;
    const uint32_t cell_width = content_width / 4U;

    return (struct ui_rect){ content_x + (uint32_t)(index % 4U) * cell_width,
        client.y + 54U + (uint32_t)(index / 4U) * 112U,
        cell_width, 104U };
}

static enum ui_status draw_button(
    struct ui_rect button,
    struct ui_rect damage,
    const char *label
)
{
    enum ui_status status = gradient_rect(button, damage,
        0xF8U, 0xFAU, 0xFAU, 0xA8U, 0xB2U, 0xB5U);

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(button, damage, 1U,
            framebuffer_pack(0x64U, 0x70U, 0x74U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(button, damage, centered_text_x(button, label),
            button.y + 18U, label, state.theme.ink);
    }
    return status;
}

static enum ui_status draw_files_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const struct ui_rect toolbar = { client.x, client.y, client.width, 46U };
    const struct ui_rect sidebar = { client.x, client.y + 46U, 190U,
        client.height - 70U };
    const struct ui_rect content = { client.x + 190U, client.y + 46U,
        client.width - 190U, client.height - 70U };
    const struct ui_rect status_bar = { client.x,
        client.y + client.height - 24U, client.width, 24U };
    enum ui_status status = gradient_rect(toolbar, damage,
        0xF4U, 0xF5U, 0xF5U, 0xA9U, 0xADU, 0xAFU);

    if (status == UI_STATUS_OK) {
        status = fill_clipped(sidebar, damage,
            framebuffer_pack(0xD9U, 0xDDU, 0xDFU));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(content, damage, state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect(status_bar, damage,
            0xE8U, 0xEAU, 0xEAU, 0xB5U, 0xB8U, 0xBAU);
    }
    const struct ui_rect buttons[] = {
        { toolbar.x + 10U, toolbar.y + 8U, 32U, 26U },
        { toolbar.x + 44U, toolbar.y + 8U, 32U, 26U },
        { toolbar.x + 88U, toolbar.y + 8U, 32U, 26U },
        { toolbar.x + 120U, toolbar.y + 8U, 32U, 26U },
        { toolbar.x + 152U, toolbar.y + 8U, 32U, 26U },
        { toolbar.x + 184U, toolbar.y + 8U, 32U, 26U },
        { toolbar.x + 226U, toolbar.y + 8U, 38U, 26U }
    };
    static const char *const labels[] = {
        "<", ">", "[]", "##", "=", "|||", "*"
    };

    for (size_t index = 0U; index < 7U && status == UI_STATUS_OK; ++index) {
        status = draw_button(buttons[index], damage, labels[index]);
    }
    if (status == UI_STATUS_OK) {
        const struct ui_rect search = { toolbar.x + toolbar.width - 166U,
            toolbar.y + 8U, 156U, 26U };
        status = fill_clipped(search, damage, state.theme.white);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(search, damage, 1U,
                framebuffer_pack(0x76U, 0x7CU, 0x80U));
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(search, damage, search.x + 10U,
                search.y + 18U, "Search", state.theme.title_inactive);
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(toolbar, damage,
            centered_text_x(toolbar, strings_equal(file_directory, ".") ?
                "Data Home" : file_directory), toolbar.y + 26U,
            strings_equal(file_directory, ".") ? "Data Home" : file_directory,
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 22U, "DEVICES", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect((struct ui_rect){ sidebar.x + 4U,
            sidebar.y + 29U, sidebar.width - 8U, 28U }, damage,
            0x70U, 0xA7U, 0xCCU, 0x2FU, 0x6FU, 0x9CU);
    }
    if (status == UI_STATUS_OK) {
        status = draw_logo_color((struct ui_rect){ sidebar.x + 14U,
            sidebar.y + 33U, 22U, 20U }, damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 46U,
            sidebar.y + 49U, "data", state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 88U, "SHARED", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 116U, "Sapote", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 154U, "PLACES", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 182U, "Desktop", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 208U, "Documents", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 234U, "Downloads", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 260U, "Applications", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 286U, "Notes", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 312U, "Pictures", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 352U, "SEARCH FOR", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 380U, "Today", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 406U, "Yesterday", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 432U, "All Files", state.theme.ink);
    }
    for (size_t index = 0U; index < file_entry_count &&
         index < 12U && status == UI_STATUS_OK; ++index) {
        const struct ui_rect tile = file_entry_rect(index);
        const struct sapfs_list_entry *entry = &file_entries[index];
        const struct ui_rect icon = { tile.x + (tile.width - 50U) / 2U,
            tile.y + 4U, 50U, 48U };

        if (entry->directory) {
            status = gradient_rect((struct ui_rect){ icon.x + 5U, icon.y,
                24U, 16U }, damage, 0xD3U, 0xE8U, 0xF2U,
                0x76U, 0xADU, 0xC8U);
            if (status == UI_STATUS_OK) {
                status = gradient_rect((struct ui_rect){ icon.x, icon.y + 10U,
                    icon.width, icon.height - 10U }, damage,
                    0xB8U, 0xDCU, 0xEDU, 0x58U, 0x91U, 0xB0U);
            }
        } else {
            status = gradient_rect((struct ui_rect){ icon.x + 7U, icon.y,
                icon.width - 14U, icon.height }, damage,
                0xFFU, 0xFFU, 0xF9U, 0xD8U, 0xDEU, 0xDDU);
            for (uint32_t line = 0U; line < 4U && status == UI_STATUS_OK;
                 ++line) {
                status = fill_clipped((struct ui_rect){ icon.x + 14U,
                    icon.y + 12U + line * 7U, icon.width - 27U, 2U },
                    damage, state.theme.accent_teal);
            }
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(tile, damage,
                centered_text_x(tile, entry->name), tile.y + 72U,
                entry->name, state.theme.ink);
        }
        if (status == UI_STATUS_OK && !entry->directory) {
            char size[32U];
            size_t size_at = append_u64(size, sizeof(size), 0U, entry->size);
            (void)append_text(size, sizeof(size), size_at, " bytes");
            status = draw_text(tile, damage, centered_text_x(tile, size),
                tile.y + 92U, size, state.theme.title_inactive);
        }
    }
    if (status == UI_STATUS_OK) {
        const struct sapfs_drive_info drive = sapfs_drive(SAPFS_VOLUME_DATA);
        char capacity[64U];
        size_t at = append_u64(capacity, sizeof(capacity), 0U,
            file_entry_count);

        at = append_text(capacity, sizeof(capacity), at,
            file_entry_count == 1U ? " item, " : " items, ");
        at = append_u64(capacity, sizeof(capacity), at,
            drive.free_bytes / (1024U * 1024U));
        (void)append_text(capacity, sizeof(capacity), at, " MB available");
        status = draw_text(status_bar, damage,
            centered_text_x(status_bar, capacity), status_bar.y + 17U,
            capacity, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(status_bar, damage, status_bar.x + 8U,
            status_bar.y + 17U, "data / fat32", state.theme.title_inactive);
    }
    return status;
}

static enum ui_status draw_notes_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t sidebar_width = 154U;
    const struct ui_rect sidebar = { client.x, client.y, sidebar_width,
        client.height };
    const struct ui_rect paper = { client.x + sidebar_width + 1U, client.y,
        client.width - sidebar_width - 1U, client.height };
    const struct ui_rect search = { sidebar.x + 8U, sidebar.y + 8U,
        sidebar.width - 16U, 24U };
    const struct ui_rect selected = { sidebar.x + 5U, sidebar.y + 40U,
        sidebar.width - 10U, 34U };
    enum ui_status status = gradient_rect(sidebar, damage,
        0xF3U, 0xF3U, 0xF1U, 0xD5U, 0xD6U, 0xD3U);

    if (status == UI_STATUS_OK) {
        status = fill_clipped(paper, damage,
            framebuffer_pack(0xFFU, 0xFDU, 0xC9U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ paper.x, paper.y, 1U,
            paper.height }, damage, framebuffer_pack(0x8AU, 0x8AU, 0x82U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(search, damage, state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(search, damage, 1U,
            framebuffer_pack(0x9AU, 0x9EU, 0x9CU));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(search, damage, search.x + 8U, search.y + 17U,
            "Search", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect(selected, damage,
            0xFFU, 0xFFU, 0xFFU, 0xE7U, 0xE8U, 0xE5U);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(selected, damage, selected.x + 6U,
            selected.y + 17U, note_path, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(paper, damage, paper.x + 18U, paper.y + 25U,
            note_path, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ paper.x + 12U, paper.y + 34U,
            paper.width - 24U, 1U }, damage,
            framebuffer_pack(0xD8U, 0xCFU, 0x92U));
    }
    for (uint32_t y = paper.y + 60U; y < paper.y + paper.height - 28U &&
         status == UI_STATUS_OK; y += 22U) {
        status = fill_clipped((struct ui_rect){ paper.x + 10U, y,
            paper.width - 20U, 1U }, damage,
            framebuffer_pack(0xD6U, 0xD1U, 0xA7U));
    }
    size_t source = 0U;
    uint32_t line = 0U;
    while (source < note_length && line < 18U && status == UI_STATUS_OK) {
        char text[82U];
        size_t count = 0U;

        while (source < note_length && note_buffer[source] != '\n' &&
               count + 1U < sizeof(text)) {
            text[count++] = note_buffer[source++];
        }
        if (source < note_length && note_buffer[source] == '\n') {
            ++source;
        }
        text[count] = '\0';
        status = draw_text(paper, damage, paper.x + 18U,
            paper.y + 56U + line * 22U, text, state.theme.ink);
        ++line;
    }
    return status;
}

static struct ui_rect studio_toolbar_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;

    return (struct ui_rect){ client.x, client.y, client.width, 38U };
}

static struct ui_rect studio_button_rect(size_t index)
{
    const struct ui_rect toolbar = studio_toolbar_rect();
    static const uint32_t widths[] = { 64U, 68U, 68U, 60U, 68U };
    uint32_t x = toolbar.x + 10U;

    for (size_t before = 0U; before < index && before < 5U; ++before) {
        x += widths[before] + 6U;
    }
    return index < 5U ?
        (struct ui_rect){ x, toolbar.y + 7U, widths[index], 24U } :
        (struct ui_rect){ 0U, 0U, 0U, 0U };
}

static struct ui_rect studio_timeline_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t upper_height = (client.height - 38U) * 56U / 100U;

    return (struct ui_rect){ client.x, client.y + 38U + upper_height,
        client.width, client.height - 38U - upper_height };
}

static enum ui_status draw_studio_button(
    struct ui_rect button,
    struct ui_rect damage,
    const char *label
)
{
    enum ui_status status = gradient_rect(button, damage,
        0x78U, 0x7DU, 0x82U, 0x34U, 0x38U, 0x3CU);

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(button, damage, 1U,
            framebuffer_pack(0x0DU, 0x0FU, 0x11U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(button, damage, centered_text_x(button, label),
            button.y + 17U, label, state.theme.white);
    }
    return status;
}

static enum ui_status draw_wallpaper_preview(
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t target_y = clipped.y + y;
        const uint32_t source_y = (target_y - bounds.y) * 768U /
            bounds.height;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t target_x = clipped.x + x;
            const uint32_t source_x = (target_x - bounds.x) * 1024U /
                bounds.width;

            if (surface_pixel(canvas, target_x, target_y,
                    wallpaper_pixels[(size_t)source_y * 1024U + source_x]) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_studio_preview(
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    if (!studio_preview_loaded) {
        return draw_wallpaper_preview(bounds, damage);
    }
    const struct ui_rect clipped = rect_intersection(bounds, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t target_y = clipped.y + y;
        const uint32_t source_y = (target_y - bounds.y) *
            studio_preview_height / bounds.height;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t target_x = clipped.x + x;
            const uint32_t source_x = (target_x - bounds.x) *
                studio_preview_width / bounds.width;

            if (surface_pixel(canvas, target_x, target_y,
                    studio_preview_pixels[(size_t)source_y *
                        UI_STUDIO_PREVIEW_WIDTH + source_x]) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static void studio_short_label(const char *path, char *label, size_t capacity)
{
    const char *name = path;
    size_t length = 0U;

    for (size_t index = 0U; path[index] != '\0'; ++index) {
        if (path[index] == '/') {
            name = &path[index + 1U];
        }
    }
    while (name[length] != '\0' && length + 1U < capacity) {
        label[length] = name[length];
        ++length;
    }
    label[length] = '\0';
}

static enum ui_status draw_studio_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const struct ui_rect toolbar = studio_toolbar_rect();
    const struct ui_rect timeline = studio_timeline_rect();
    const struct ui_rect upper = { client.x, toolbar.y + toolbar.height,
        client.width, timeline.y - toolbar.y - toolbar.height };
    const uint32_t side_width = client.width / 5U;
    const struct ui_rect browser = { upper.x, upper.y, side_width,
        upper.height };
    const struct ui_rect inspector = { upper.x + upper.width - side_width,
        upper.y, side_width, upper.height };
    const struct ui_rect viewer = { browser.x + browser.width, upper.y,
        upper.width - browser.width - inspector.width, upper.height };
    const uint32_t preview_width = viewer.width > 36U ? viewer.width - 36U :
        viewer.width;
    const uint32_t preview_height = preview_width * 9U / 16U <
        viewer.height - 58U ? preview_width * 9U / 16U : viewer.height - 58U;
    const struct ui_rect preview = {
        viewer.x + (viewer.width - preview_width) / 2U,
        viewer.y + 26U + (viewer.height - 48U - preview_height) / 2U,
        preview_width, preview_height
    };
    static const char *const button_labels[] = {
        "New", "Import", "Trim 1s", "Save", "Export"
    };
    enum ui_status status = gradient_rect(toolbar, damage,
        0x68U, 0x6DU, 0x71U, 0x25U, 0x29U, 0x2CU);

    for (size_t index = 0U; index < 5U && status == UI_STATUS_OK; ++index) {
        status = draw_studio_button(studio_button_rect(index), damage,
            button_labels[index]);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(toolbar, damage, toolbar.x + toolbar.width - 198U,
            toolbar.y + 25U, studio_dirty ? "Project - Edited" :
            "Project", state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(browser, damage,
            framebuffer_pack(0x32U, 0x35U, 0x38U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(viewer, damage,
            framebuffer_pack(0x12U, 0x14U, 0x16U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(inspector, damage,
            framebuffer_pack(0x37U, 0x3AU, 0x3DU));
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(browser, damage, 1U,
            framebuffer_pack(0x0CU, 0x0DU, 0x0FU));
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(inspector, damage, 1U,
            framebuffer_pack(0x0CU, 0x0DU, 0x0FU));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(browser, damage, browser.x + 10U,
            browser.y + 22U, "LIBRARIES", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect((struct ui_rect){ browser.x + 5U,
            browser.y + 30U, browser.width - 10U, 28U }, damage,
            0x65U, 0x83U, 0x96U, 0x35U, 0x55U, 0x68U);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(browser, damage, browser.x + 16U,
            browser.y + 49U, "SapStudio Library", state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(browser, damage, browser.x + 10U,
            browser.y + 82U, "IMPORTED MEDIA", state.theme.title_inactive);
    }
    for (size_t index = 0U; index < studio_clip_count &&
         status == UI_STATUS_OK; ++index) {
        char label[18U];

        studio_short_label(studio_clip_paths[index], label, sizeof(label));
        status = draw_text(browser, damage, browser.x + 14U,
            browser.y + 108U + (uint32_t)index * 23U, label,
            index == studio_selected_clip ? state.theme.accent_teal :
                state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(viewer, damage, viewer.x + 12U,
            viewer.y + 20U, studio_preview_loaded ?
            "VIEWER / 24-BIT BMP" : "VIEWER / NO MEDIA",
            state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = draw_studio_preview(preview, damage);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(preview, damage, 1U,
            framebuffer_pack(0x78U, 0x7CU, 0x80U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(viewer, damage,
            centered_text_x(viewer, "00:00:00:00"),
            viewer.y + viewer.height - 10U, "00:00:00:00",
            state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(inspector, damage, inspector.x + 10U,
            inspector.y + 22U, "INSPECTOR", state.theme.title_inactive);
    }
    static const char *const inspector_labels[] = {
        "Transform", "Position   0  0", "Scale      100%",
        "Opacity    100%", "Audio", "Volume       0 dB"
    };
    for (size_t index = 0U; index < 6U && status == UI_STATUS_OK; ++index) {
        const uint32_t color = index == 0U || index == 4U ?
            state.theme.white : state.theme.title_inactive;

        status = draw_text(inspector, damage, inspector.x + 12U,
            inspector.y + 52U + (uint32_t)index * 27U,
            inspector_labels[index], color);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(timeline, damage,
            framebuffer_pack(0x20U, 0x23U, 0x26U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ timeline.x, timeline.y,
            timeline.width, 28U }, damage,
            framebuffer_pack(0x3CU, 0x40U, 0x44U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(timeline, damage, timeline.x + 10U,
            timeline.y + 19U, studio_status, state.theme.white);
    }
    for (uint32_t tick = 0U; tick <= 10U && status == UI_STATUS_OK; ++tick) {
        const uint32_t x = timeline.x + 54U +
            tick * (timeline.width - 68U) / 10U;

        status = fill_clipped((struct ui_rect){ x, timeline.y + 28U,
            1U, 8U }, damage, state.theme.title_inactive);
    }
    static const char *const track_labels[] = { "V1", "V2", "A1" };
    for (size_t track = 0U; track < 3U && status == UI_STATUS_OK; ++track) {
        const uint32_t y = timeline.y + 40U + (uint32_t)track * 30U;

        status = draw_text(timeline, damage, timeline.x + 9U, y + 18U,
            track_labels[track], state.theme.title_inactive);
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ timeline.x + 38U, y,
                timeline.width - 48U, 26U }, damage,
                framebuffer_pack(0x2BU, 0x2FU, 0x32U));
        }
    }
    uint32_t clip_x = timeline.x + 42U;
    for (size_t index = 0U; index < studio_clip_count &&
         status == UI_STATUS_OK; ++index) {
        uint32_t available;
        uint32_t clip_width = 92U + studio_clip_durations[index] / 12U;

        if (clip_x >= timeline.x + timeline.width - 12U) {
            break;
        }
        available = timeline.x + timeline.width - 12U - clip_x;
        if (clip_width > available) {
            clip_width = available;
        }
        if (clip_width == 0U) {
            break;
        }
        const struct ui_rect clip = { clip_x, timeline.y + 42U,
            clip_width, 22U };
        status = gradient_rect(clip, damage,
            index == studio_selected_clip ? 0x96U : 0x68U,
            index == studio_selected_clip ? 0xBCU : 0x96U,
            index == studio_selected_clip ? 0xD0U : 0xAEU,
            0x32U, 0x62U, 0x7CU);
        if (status == UI_STATUS_OK) {
            char label[14U];

            studio_short_label(studio_clip_paths[index], label,
                sizeof(label));
            status = draw_text(clip, damage, clip.x + 6U, clip.y + 16U,
                label, state.theme.white);
        }
        clip_x += clip_width + 5U;
    }
    if (status == UI_STATUS_OK) {
        const uint32_t playhead_x = timeline.x + 38U +
            studio_playhead * (timeline.width - 48U) / 1000U;

        status = fill_clipped((struct ui_rect){ playhead_x,
            timeline.y + 28U, 2U, timeline.height - 32U }, damage,
            state.theme.accent_red);
    }
    return status;
}

static const char *store_nav_label(size_t index)
{
    static const char *const labels[UI_STORE_NAV_COUNT] = {
        "Home", "Installed", "Updates", "All Applications",
        "Accessibility", "Creative", "Development", "Games",
        "Internet", "System", "Utilities", "Settings", "About"
    };

    return index < UI_STORE_NAV_COUNT ? labels[index] : "Store";
}

static uint8_t store_nav_icon(size_t index)
{
    static const uint8_t icons[UI_STORE_NAV_COUNT] = {
        0U, 1U, 2U, 5U, 6U, 9U, 7U, 8U,
        10U, 13U, 14U, 3U, 4U
    };

    return index < UI_STORE_NAV_COUNT ? icons[index] : 15U;
}

static uint32_t store_sidebar_width(void)
{
    return state.layout.panel_client.width >= 700U ? 202U : 176U;
}

static struct ui_rect store_search_rect(void)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t width = store_sidebar_width();

    return (struct ui_rect){ client.x + 12U, client.y + 11U,
        width - 24U, 30U };
}

static struct ui_rect store_nav_rect(size_t index)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t width = store_sidebar_width();
    uint32_t y;

    if (index < 3U) {
        y = client.y + 50U + (uint32_t)index * 24U;
    } else if (index < 11U) {
        y = client.y + 137U + (uint32_t)(index - 3U) * 23U;
    } else {
        y = client.y + client.height - 54U +
            (uint32_t)(index - 11U) * 25U;
    }
    return (struct ui_rect){ client.x + 9U, y, width - 18U, 22U };
}

static enum ui_status draw_store_ui_icon(
    uint8_t index,
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    if (index >= 16U) {
        return UI_STATUS_BAD_ELEMENT;
    }
    return draw_alpha_subimage(bounds, damage, store_ui_icon_pixels,
        store_ui_icon_alpha, store_ui_icon_width, store_ui_icon_height,
        (struct ui_rect){
            (uint32_t)(index % UI_STORE_ICON_COLUMNS) * UI_STORE_ICON_SIZE,
            (uint32_t)(index / UI_STORE_ICON_COLUMNS) * UI_STORE_ICON_SIZE,
            UI_STORE_ICON_SIZE, UI_STORE_ICON_SIZE
        });
}

static enum ui_status draw_store_info_row(
    struct ui_rect content,
    struct ui_rect damage,
    uint32_t row,
    const char *label,
    const char *value
)
{
    const struct ui_rect bounds = {
        content.x + 28U, content.y + 74U + row * 58U,
        content.width - 56U, 48U
    };
    enum ui_status status = fill_clipped(bounds, damage,
        framebuffer_pack(0xF6U, 0xF7U, 0xF8U));

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(bounds, damage, 1U,
            framebuffer_pack(0xD6U, 0xDAU, 0xDEU));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(bounds, damage, bounds.x + 14U,
            bounds.y + 20U, label, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        uint32_t value_width = 0U;
        if (ui_font_text_width(value, &value_width) != UI_FONT_STATUS_OK) {
            return UI_STATUS_FONT_FAILURE;
        }
        status = draw_text(bounds, damage,
            value_width + 14U < bounds.width ?
                bounds.x + bounds.width - value_width - 14U :
                bounds.x + 14U,
            bounds.y + 20U, value,
            framebuffer_pack(0x64U, 0x6AU, 0x70U));
    }
    return status;
}

static enum ui_status draw_store_empty(
    struct ui_rect content,
    struct ui_rect damage,
    const char *title,
    const char *detail
)
{
    const uint32_t top = content.y + 198U;
    const uint32_t height = content.height > 230U ?
        content.height - 218U : 96U;
    const struct ui_rect card = {
        content.x + 28U, top, content.width - 56U, height
    };
    enum ui_status status = fill_clipped(card, damage,
        framebuffer_pack(0xFAU, 0xFAU, 0xFBU));

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(card, damage, 1U,
            framebuffer_pack(0xD9U, 0xDDU, 0xE1U));
    }
    if (status == UI_STATUS_OK && card.height >= 88U) {
        status = draw_store_ui_icon(15U, (struct ui_rect){
            card.x + (card.width - 42U) / 2U, card.y + 13U, 42U, 42U
        }, damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(card, damage, centered_text_x(card, title),
            card.y + (card.height >= 88U ? 72U : 32U), title,
            state.theme.ink);
    }
    if (status == UI_STATUS_OK && card.height >= 112U) {
        status = draw_text(card, damage, centered_text_x(card, detail),
            card.y + 96U, detail,
            framebuffer_pack(0x6CU, 0x73U, 0x79U));
    }
    return status;
}

static enum ui_status draw_store_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t sidebar_width = store_sidebar_width();
    const struct ui_rect sidebar = {
        client.x, client.y, sidebar_width, client.height
    };
    const struct ui_rect content = {
        client.x + sidebar_width + 1U, client.y,
        client.width - sidebar_width - 1U, client.height
    };
    const struct ui_rect search = store_search_rect();
    const char *const section = store_nav_label(store_section);
    enum ui_status status = fill_clipped(client, damage,
        framebuffer_pack(0xFFU, 0xFFU, 0xFFU));

    if (status == UI_STATUS_OK) {
        status = fill_clipped(sidebar, damage,
            framebuffer_pack(0xF1U, 0xF3U, 0xF5U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            sidebar.x + sidebar.width - 1U, sidebar.y, 1U, sidebar.height
        }, damage, framebuffer_pack(0xD0U, 0xD4U, 0xD8U));
    }
    if (status == UI_STATUS_OK) {
        status = translucent_capsule_fill(search, damage,
            store_search_focused ? state.theme.accent_violet :
                framebuffer_pack(0xC6U, 0xCBU, 0xCFU), 255U);
    }
    if (status == UI_STATUS_OK) {
        status = translucent_capsule_fill((struct ui_rect){
            search.x + 1U, search.y + 1U,
            search.width - 2U, search.height - 2U
        }, damage, framebuffer_pack(0xFFU, 0xFFU, 0xFFU), 255U);
    }
    if (status == UI_STATUS_OK) {
        status = draw_search_icon((struct ui_rect){
            search.x + 8U, search.y + 6U, 18U, 18U
        }, damage, framebuffer_pack(0x64U, 0x6AU, 0x70U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(search, damage, search.x + 32U, search.y + 21U,
            store_query_length == 0U ? "Search" : store_query,
            store_query_length == 0U ?
                framebuffer_pack(0x8AU, 0x91U, 0x98U) : state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            sidebar.x + 14U, sidebar.y + 128U,
            sidebar.width - 28U, 1U
        }, damage, framebuffer_pack(0xD6U, 0xDAU, 0xDEU));
    }
    for (size_t index = 0U; index < UI_STORE_NAV_COUNT &&
            status == UI_STATUS_OK; ++index) {
        const struct ui_rect row = store_nav_rect(index);

        if (index == store_section) {
            status = translucent_capsule_fill(row, damage,
                framebuffer_pack(0xE8U, 0xE0U, 0xF1U), 255U);
        }
        if (status == UI_STATUS_OK) {
            status = draw_store_ui_icon(store_nav_icon(index),
                (struct ui_rect){ row.x + 5U, row.y + 2U, 18U, 18U },
                damage);
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(row, damage, row.x + 29U, row.y + 17U,
                store_nav_label(index), state.theme.ink);
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(content, damage, content.x + 28U,
            content.y + 34U,
            store_query_length == 0U ? section : "Search",
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(content, damage, content.x + 28U,
            content.y + 57U, "Applications for Sapote Redwood",
            framebuffer_pack(0x6DU, 0x73U, 0x79U));
    }

    if (status != UI_STATUS_OK) {
        return status;
    }
    if (store_query_length != 0U) {
        return draw_store_empty(content, damage,
            "No applications found",
            "No packages have been published to the catalog yet.");
    }
    if (store_section == 11U) {
        status = draw_store_info_row(content, damage, 0U,
            "Catalog source", "System volume");
        if (status == UI_STATUS_OK) {
            status = draw_store_info_row(content, damage, 1U,
                "Application data", "Data/Applications");
        }
        if (status == UI_STATUS_OK) {
            status = draw_store_info_row(content, damage, 2U,
                "Package checks", "Manifest + SHA-256");
        }
        return status;
    }
    if (store_section == 12U) {
        status = draw_store_info_row(content, damage, 0U,
            "Application", "Sapote Store");
        if (status == UI_STATUS_OK) {
            status = draw_store_info_row(content, damage, 1U,
                "Platform", "Sapote native ABI v1");
        }
        if (status == UI_STATUS_OK) {
            status = draw_store_info_row(content, damage, 2U,
                "Catalog status", "No packages published");
        }
        return status;
    }

    const struct ui_rect hero = {
        content.x + 28U, content.y + 72U, content.width - 56U, 108U
    };
    status = gradient_rect(hero, damage, 0xF3U, 0xEDU, 0xFAU,
        0xE2U, 0xD4U, 0xEFU);
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(hero, damage, 1U,
            framebuffer_pack(0xB5U, 0x9CU, 0xC9U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_alpha_image((struct ui_rect){
            hero.x + 18U, hero.y + 14U, 80U, 80U
        }, damage, store_icon_pixels, store_icon_alpha,
            store_icon_width, store_icon_height);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(hero, damage, hero.x + 116U, hero.y + 39U,
            "Sapote Store", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(hero, damage, hero.x + 116U, hero.y + 65U,
            "Native software. One trusted catalog.",
            framebuffer_pack(0x5FU, 0x57U, 0x68U));
    }
    if (status != UI_STATUS_OK) {
        return status;
    }
    if (store_section == 1U) {
        return draw_store_empty(content, damage,
            "No Store applications installed",
            "System applications remain available in the Dock.");
    }
    if (store_section == 2U) {
        return draw_store_empty(content, damage,
            "No updates available",
            "There are no published packages to check.");
    }
    if (store_section >= 3U && store_section <= 10U) {
        return draw_store_empty(content, damage,
            "No applications in this category",
            "The catalog is ready for its first published package.");
    }
    return draw_store_empty(content, damage,
        "The catalog is ready",
        "Applications will appear here after they are published.");
}

static void begin_dock_spring(void)
{
    if (!dock_spring_active) {
        dock_spring_last_ns = clock_monotonic_ns();
    }
    dock_spring_active = true;
    if (timer_is_started() && motion_timer_id == 0U) {
        (void)motion_schedule_wake(dock_spring_last_ns);
    }
}

static enum ui_status draw_one_panel(struct ui_rect damage, bool focused)
{
    enum ui_status status;
    uint32_t native_slot;

    if (state.active_panel == UI_PANEL_NONE) {
        return UI_STATUS_OK;
    }
    status = window_shadows ?
        fill_clipped(drop_shadow_draw_rect(state.layout.panel, 6U),
            damage, state.theme.shadow) : UI_STATUS_OK;
    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.panel, damage,
            state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(state.layout.panel, damage, 1U,
            state.theme.ink);
    }
    if (status == UI_STATUS_OK && window_bevels) {
        status = bevel_clipped((struct ui_rect){ state.layout.panel.x + 1U,
            state.layout.panel.y + 1U, state.layout.panel.width - 2U,
            state.layout.panel.height - 2U }, damage, true);
    }
    if (status == UI_STATUS_OK) {
        status = draw_window_title((struct ui_rect){
            state.layout.panel.x + 4U, state.layout.panel.y + 4U,
            state.layout.panel.width - 8U, UI_PANEL_TITLE_HEIGHT
        }, damage, state.layout.panel_title_baseline,
            ui_panel_name(state.active_panel), focused);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.panel_client, damage,
            state.active_panel == UI_PANEL_TERMINAL ||
            state.active_panel == UI_PANEL_STUDIO ?
            framebuffer_pack(0x08U, 0x10U, 0x12U) :
            (window_high_contrast ? framebuffer_pack(0xFFU, 0xFFU, 0xFFU) :
                state.theme.white));
    }
    if (status == UI_STATUS_OK && window_bevels) {
        status = bevel_clipped(state.layout.panel_client, damage, false);
    }
    if (status != UI_STATUS_OK) {
        return status;
    }

    if (native_panel_slot(state.active_panel, &native_slot)) {
        const struct ui_native_window_record *native =
            &native_windows[native_slot];
        const struct ui_rect clipped = rect_intersection(
            state.layout.panel_client, damage);

        if (!native->active || native->pixels == NULL ||
            native->width == 0U || native->height == 0U ||
            state.layout.panel_client.width == 0U ||
            state.layout.panel_client.height == 0U) {
            return UI_STATUS_BAD_PANEL;
        }
        if (clipped.width == 0U || clipped.height == 0U) {
            return UI_STATUS_OK;
        }
        if (native->width == state.layout.panel_client.width &&
                native->height == state.layout.panel_client.height) {
            const uint32_t source_x = clipped.x -
                state.layout.panel_client.x;
            const uint32_t source_y = clipped.y -
                state.layout.panel_client.y;
            const uint32_t *source = native->pixels +
                (size_t)source_y * (native->stride_bytes /
                    SURFACE_BYTES_PER_PIXEL) + source_x;

            return surface_blit(canvas, clipped.x, clipped.y, source,
                clipped.width, clipped.height, native->stride_bytes) ==
                    SURFACE_STATUS_OK ? UI_STATUS_OK :
                        UI_STATUS_SURFACE_FAILURE;
        }
        const size_t stride_pixels = native->stride_bytes /
            SURFACE_BYTES_PER_PIXEL;
        for (uint32_t y = 0U; y < clipped.height; ++y) {
            const uint32_t destination_y = clipped.y + y -
                state.layout.panel_client.y;
            uint32_t source_y = (uint32_t)((uint64_t)destination_y *
                native->height / state.layout.panel_client.height);

            if (source_y >= native->height) {
                source_y = native->height - 1U;
            }
            for (uint32_t x = 0U; x < clipped.width; ++x) {
                const uint32_t destination_x = clipped.x + x -
                    state.layout.panel_client.x;
                uint32_t source_x = (uint32_t)((uint64_t)destination_x *
                    native->width / state.layout.panel_client.width);

                if (source_x >= native->width) {
                    source_x = native->width - 1U;
                }
                if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                        native->pixels[(size_t)source_y * stride_pixels +
                            source_x]) != SURFACE_STATUS_OK) {
                    return UI_STATUS_SURFACE_FAILURE;
                }
            }
        }
        return UI_STATUS_OK;
    }

    if (state.active_panel == UI_PANEL_FILES) {
        return draw_files_app(damage);
    }
    if (state.active_panel == UI_PANEL_TERMINAL) {
        const struct ui_rect clip =
            rect_intersection(state.layout.panel_client, damage);

        if (clip.width != 0U && clip.height != 0U &&
            screen_redraw_region(surface_rect_of(clip)) != SCREEN_STATUS_OK) {
            return UI_STATUS_SCREEN_FAILURE;
        }
        return UI_STATUS_OK;
    }
    if (state.active_panel == UI_PANEL_NOTES) {
        return draw_notes_app(damage);
    }
    if (state.active_panel == UI_PANEL_STUDIO) {
        return draw_studio_app(damage);
    }
    if (state.active_panel == UI_PANEL_CAMERA) {
        return draw_camera_app(damage);
    }
    if (state.active_panel == UI_PANEL_STORE) {
        return draw_store_app(damage);
    }
    if (state.active_panel == UI_PANEL_SETTINGS) {
        return draw_settings_app(damage);
    }
    return UI_STATUS_BAD_PANEL;
}

/*
 * Capture the finished window once, then let ui_anim.c warp that immutable
 * picture.  Opening draws the new front window into the cached surface before
 * taking the copy.  Closing reuses the last fully presented pixels, so the
 * panel may already be absent from the live stacking order.
 */
static enum ui_status panel_anim_start(void)
{
    const enum ui_anim_pending pending = panel_anim_pending;
    const bool opening = pending == UI_ANIM_PENDING_OPEN;

    panel_anim_pending = UI_ANIM_PENDING_NONE;
    if (pending == UI_ANIM_PENDING_NONE || canvas == NULL) {
        return UI_STATUS_OK;
    }
    if (opening) {
        const enum ui_panel_id saved_panel = state.active_panel;

        state.active_panel = panel_anim_panel;
        install_panel_geometry(panel_anim_panel);
        const enum ui_status status = draw_one_panel(panel_anim_frame, true);
        state.active_panel = saved_panel;
        install_panel_geometry(saved_panel);
        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    if (ui_anim_begin(&panel_anim, canvas, panel_anim_frame,
            panel_anim_origin, opening, clock_monotonic_ns(),
            opening ? UI_ANIM_DEFAULT_OPEN_NS : UI_ANIM_DEFAULT_CLOSE_NS) !=
                UI_ANIM_STATUS_OK) {
        ui_anim_end(&panel_anim);
        return UI_STATUS_OK;
    }
    if (timer_is_started() && motion_timer_id == 0U) {
        (void)motion_schedule_wake(clock_monotonic_ns());
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_animated_panel(struct ui_rect damage)
{
    const struct ui_rect clip = rect_intersection(
        ui_anim_bounds(&panel_anim), damage);

    if (clip.width == 0U || clip.height == 0U) {
        return UI_STATUS_OK;
    }
    return ui_anim_draw(&panel_anim, canvas, clip) == UI_ANIM_STATUS_OK ?
        UI_STATUS_OK : UI_STATUS_SURFACE_FAILURE;
}

static enum ui_status draw_panel(struct ui_rect damage)
{
    const enum ui_panel_id focused = state.active_panel;
    const struct ui_rect saved_panel = state.layout.panel;
    const struct ui_rect saved_client = state.layout.panel_client;
    const uint32_t saved_title_baseline = state.layout.panel_title_baseline;
    const uint32_t saved_text_baseline = state.layout.panel_text_baseline;
    enum ui_status status = UI_STATUS_OK;

    for (size_t index = 0U; index < panel_order_count &&
         status == UI_STATUS_OK; ++index) {
        const enum ui_panel_id panel = panel_order[index];

        if (!panel_open[panel] ||
                (ui_anim_running(&panel_anim) && panel == panel_anim_panel)) {
            continue;
        }
        state.active_panel = panel;
        install_panel_geometry(panel);
        status = draw_one_panel(damage, panel == focused);
    }
    state.active_panel = focused;
    state.layout.panel = saved_panel;
    state.layout.panel_client = saved_client;
    state.layout.panel_title_baseline = saved_title_baseline;
    state.layout.panel_text_baseline = saved_text_baseline;
    return status;
}

static uint32_t cursor_draw_width(void)
{
    return cursor_large ? 24U : UI_CURSOR_WIDTH;
}

static uint32_t cursor_draw_height(void)
{
    return cursor_large ? 33U : UI_CURSOR_HEIGHT;
}

static struct ui_rect cursor_rect_for(struct ui_point point)
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    if (point.x < 0 || point.y < 0 || state.layout.surface.width == 0U ||
        state.layout.surface.height == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    x = (uint32_t)point.x;
    y = (uint32_t)point.y;
    if (x >= state.layout.surface.width) {
        x = state.layout.surface.width - 1U;
    }
    if (y >= state.layout.surface.height) {
        y = state.layout.surface.height - 1U;
    }
    width = cursor_draw_width();
    height = cursor_draw_height();
    if (width > state.layout.surface.width - x) {
        width = state.layout.surface.width - x;
    }
    if (height > state.layout.surface.height - y) {
        height = state.layout.surface.height - y;
    }
    return (struct ui_rect){ x, y, width, height };
}

static struct ui_rect cursor_damage_rect_for(struct ui_point point)
{
    const struct ui_rect cursor = cursor_rect_for(point);

    if (cursor.width == 0U || cursor.height == 0U) {
        return cursor;
    }
    const uint32_t left = cursor.x >= 3U ? cursor.x - 3U : 0U;
    const uint32_t top = cursor.y >= 3U ? cursor.y - 3U : 0U;
    uint32_t right = cursor.x + cursor.width + 3U;
    uint32_t bottom = cursor.y + cursor.height + 3U;

    if (right > state.layout.surface.width) {
        right = state.layout.surface.width;
    }
    if (bottom > state.layout.surface.height) {
        bottom = state.layout.surface.height;
    }
    return (struct ui_rect){ left, top, right - left, bottom - top };
}

static bool cursor_mask_contains(
    const uint32_t *mask,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    const uint32_t source_x = x * UI_CURSOR_SOURCE_WIDTH / width;
    const uint32_t source_y = y * UI_CURSOR_SOURCE_HEIGHT / height;

    return (mask[source_y] & (UINT32_C(0x80000000) >> source_x)) != 0U;
}

static enum ui_status draw_cursor(struct ui_rect damage)
{
    const struct ui_rect cursor = cursor_rect_for(state.pointer);

    if (!state.pointer_present || !rects_intersect(cursor, damage)) {
        return UI_STATUS_OK;
    }
    for (uint32_t y = 0U; y < cursor.height; ++y) {
        for (uint32_t x = 0U; x < cursor.width; ++x) {
            uint32_t pixel;

            if (!cursor_mask_contains(cursor_outer, x, y,
                    cursor.width, cursor.height)) {
                continue;
            }
            pixel = cursor_mask_contains(cursor_inner, x, y,
                cursor.width, cursor.height) ?
                (cursor_dark ? state.theme.ink : state.theme.white) :
                (cursor_dark ? state.theme.white : state.theme.ink);
            if (surface_pixel(canvas, cursor.x + x, cursor.y + y, pixel) !=
                SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_desktop_pattern(struct ui_rect damage)
{
    return draw_wallpaper(damage);
}

static enum ui_status draw_dock_shelf(struct ui_rect damage)
{
    const uint32_t surface_width = state.layout.surface.width;
    const uint32_t surface_height = state.layout.surface.height;
    const uint32_t panel_x = dock_fixed_pixel(dock_model.panel_x);
    const uint32_t panel_y = dock_fixed_pixel(dock_model.panel_y);
    const uint32_t panel_width = dock_fixed_pixel(dock_model.panel_width);
    uint32_t panel_height = dock_fixed_pixel(dock_model.panel_height);
    const uint32_t center_x = dock_fixed_pixel(dock_model.center_x);

    if (panel_height == 0U || panel_y >= surface_height ||
            panel_width == 0U) {
        return UI_STATUS_OK;
    }
    if (panel_height > 48U) {
        panel_height = 48U;
    }
    if (!rects_intersect((struct ui_rect){ 0U,
            panel_y > 18U ? panel_y - 18U : 0U,
            surface_width, surface_height -
                (panel_y > 18U ? panel_y - 18U : 0U) }, damage)) {
        return UI_STATUS_OK;
    }

    /* Snapshot the real composited desktop before laying down glass.  A
     * compact nine-tap blur sampled from this cache is the freestanding
     * counterpart of upstream 3d-dock's three-pass Cairo frost surface. */
    for (uint32_t row = 0U; row < panel_height; ++row) {
        const uint32_t source_y = panel_y + row < surface_height ?
            panel_y + row : surface_height - 1U;
        for (uint32_t x = 0U; x < surface_width; ++x) {
            if (surface_read_pixel(canvas, x, source_y,
                    &dock_backdrop_pixels[(size_t)row * surface_width + x]) !=
                    SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }

    for (uint32_t row = 0U; row < panel_height; ++row) {
        const uint32_t flare_total = panel_width * 55U * row /
            (1000U * (panel_height == 0U ? 1U : panel_height));
        const uint32_t left = panel_x > flare_total / 2U ?
            panel_x - flare_total / 2U : 0U;
        uint32_t right = panel_x + panel_width + flare_total / 2U;
        if (right > surface_width) {
            right = surface_width;
        }
        for (uint32_t x = left; x < right; ++x) {
            const uint32_t y = panel_y + row;
            if (y >= surface_height || !rect_contains_point(damage,
                    (struct ui_point){ (int32_t)x, (int32_t)y })) {
                continue;
            }
            uint32_t sums[3U] = { 0U, 0U, 0U };
            uint32_t samples = 0U;
            for (int32_t sample_y = -2; sample_y <= 2; sample_y += 2) {
                int32_t cached_y = (int32_t)row + sample_y;
                if (cached_y < 0) {
                    cached_y = 0;
                } else if ((uint32_t)cached_y >= panel_height) {
                    cached_y = (int32_t)panel_height - 1;
                }
                for (int32_t sample_x = -3; sample_x <= 3;
                        sample_x += 3) {
                    int32_t cached_x = (int32_t)x + sample_x;
                    if (cached_x < 0) {
                        cached_x = 0;
                    } else if ((uint32_t)cached_x >= surface_width) {
                        cached_x = (int32_t)surface_width - 1;
                    }
                    const uint32_t pixel = dock_backdrop_pixels[
                        (size_t)cached_y * surface_width + (uint32_t)cached_x];
                    sums[0U] += (pixel >> logo_red_shift) & 0xFFU;
                    sums[1U] += (pixel >> logo_green_shift) & 0xFFU;
                    sums[2U] += (pixel >> logo_blue_shift) & 0xFFU;
                    samples += 1U;
                }
            }
            uint32_t frosted = framebuffer_pack((uint8_t)(sums[0U] / samples),
                (uint8_t)(sums[1U] / samples),
                (uint8_t)(sums[2U] / samples));
            const uint8_t wash_alpha = dock_dark ?
                (uint8_t)(68U - row * 20U / panel_height) :
                (uint8_t)(107U - row * 56U / panel_height);
            const uint32_t wash = dock_dark ?
                framebuffer_pack(0x18U, 0x1AU, 0x1EU) :
                framebuffer_pack(0xF7U, 0xF8U, 0xFFU);
            frosted = blend_packed(frosted, wash, wash_alpha);
            frosted = blend_packed(frosted,
                framebuffer_pack(0U, 0U, 0U),
                (uint8_t)(41U * row / panel_height));
            if (surface_pixel(canvas, x, y, frosted) != SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }

    /* Soft back-edge glow, then the hard centre-hot specular line. */
    const uint32_t glow_height = dock_fixed_pixel(dock_model.icon) * 26U /
        100U;
    for (uint32_t row = 0U; row < glow_height; ++row) {
        const uint32_t y = panel_y > glow_height - row ?
            panel_y - glow_height + row : 0U;
        const uint8_t vertical_alpha = (uint8_t)(28U * (row + 1U) /
            (glow_height == 0U ? 1U : glow_height));
        for (uint32_t x = panel_x; x < panel_x + panel_width &&
                x < surface_width; ++x) {
            const uint32_t distance = x > center_x ? x - center_x :
                center_x - x;
            const uint32_t half = panel_width / 2U;
            const uint8_t edge = distance < half ?
                (uint8_t)(UINT8_MAX * (half - distance) /
                    (half == 0U ? 1U : half)) : 0U;
            if (rect_contains_point(damage, (struct ui_point){
                    (int32_t)x, (int32_t)y })) {
                uint32_t under;
                if (surface_read_pixel(canvas, x, y, &under) !=
                        SURFACE_STATUS_OK ||
                        surface_pixel(canvas, x, y, blend_packed(under,
                            state.theme.white,
                            (uint8_t)((uint32_t)vertical_alpha * edge /
                                UINT8_MAX))) != SURFACE_STATUS_OK) {
                    return UI_STATUS_SURFACE_FAILURE;
                }
            }
        }
    }
    for (uint32_t x = panel_x; x < panel_x + panel_width &&
            x < surface_width; ++x) {
        const uint32_t distance = x > center_x ? x - center_x :
            center_x - x;
        const uint32_t half = panel_width / 2U;
        const uint8_t alpha = distance < half ?
            (uint8_t)(26U + 198U * (half - distance) /
                (half == 0U ? 1U : half)) : 26U;
        if (rect_contains_point(damage, (struct ui_point){
                (int32_t)x, (int32_t)panel_y })) {
            uint32_t under;
            if (surface_read_pixel(canvas, x, panel_y, &under) !=
                    SURFACE_STATUS_OK ||
                    surface_pixel(canvas, x, panel_y, blend_packed(under,
                        state.theme.white, alpha)) != SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static struct ui_rect menu_search_rect(void)
{
    return (struct ui_rect){
        state.layout.surface.width - 31U, 2U, 24U, 20U
    };
}

static struct ui_rect launcher_bounds(void)
{
    uint32_t width = state.layout.surface.width > 700U ? 620U :
        state.layout.surface.width - 80U;
    uint32_t height = state.layout.surface.height > 630U ? 440U :
        state.layout.surface.height - 160U;

    if (width > state.layout.surface.width - 40U) {
        width = state.layout.surface.width - 40U;
    }
    if (height > state.layout.dock.y - 44U) {
        height = state.layout.dock.y - 44U;
    }
    return (struct ui_rect){
        (state.layout.surface.width - width) / 2U, 42U, width, height
    };
}

static struct ui_rect launcher_search_rect(void)
{
    const struct ui_rect panel = launcher_bounds();
    const uint32_t width = panel.width > 180U ? panel.width - 160U :
        panel.width;

    return (struct ui_rect){
        panel.x + (panel.width - width) / 2U, panel.y + 42U,
        width, 30U
    };
}

static struct ui_rect launcher_app_rect(size_t slot)
{
    const struct ui_rect panel = launcher_bounds();
    const uint32_t content_width = panel.width - 64U;
    const uint32_t cell_width = content_width / 3U;
    const uint32_t row = (uint32_t)(slot / 3U);
    const uint32_t column = (uint32_t)(slot % 3U);

    return (struct ui_rect){
        panel.x + 32U + column * cell_width,
        panel.y + 92U + row * 124U,
        cell_width, 108U
    };
}

static bool launcher_character_equal(char left, char right)
{
    if (left >= 'A' && left <= 'Z') {
        left = (char)(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
        right = (char)(right - 'A' + 'a');
    }
    return left == right;
}

static bool launcher_label_matches(const char *label)
{
    if (launcher_query_length == 0U) {
        return true;
    }
    for (size_t start = 0U; label[start] != '\0'; ++start) {
        size_t query = 0U;

        while (query < launcher_query_length && label[start + query] != '\0' &&
                launcher_character_equal(label[start + query],
                    launcher_query[query])) {
            ++query;
        }
        if (query == launcher_query_length) {
            return true;
        }
    }
    return false;
}

static size_t launcher_match_count(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (launcher_label_matches(state.layout.dock_items[index].label)) {
            ++count;
        }
    }
    return count;
}

static bool launcher_dock_index_at(size_t visible_index, size_t *dock_index)
{
    size_t visible = 0U;

    if (dock_index == NULL) {
        return false;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (!launcher_label_matches(state.layout.dock_items[index].label)) {
            continue;
        }
        if (visible == visible_index) {
            *dock_index = index;
            return true;
        }
        ++visible;
    }
    return false;
}

static size_t launcher_page_count(void)
{
    const size_t matches = launcher_match_count();
    size_t pages = (matches + UI_LAUNCHER_APPS_PER_PAGE - 1U) /
        UI_LAUNCHER_APPS_PER_PAGE;

    if (pages == 0U) {
        pages = 1U;
    }
    return pages > UI_LAUNCHER_MAX_PAGES ? UI_LAUNCHER_MAX_PAGES : pages;
}

static struct ui_rect launcher_page_rect(size_t page)
{
    const struct ui_rect panel = launcher_bounds();
    const size_t pages = launcher_page_count();
    const uint32_t spacing = 18U;
    const uint32_t row_width = (uint32_t)pages * spacing;

    return (struct ui_rect){
        panel.x + (panel.width - row_width) / 2U +
            (uint32_t)page * spacing,
        panel.y + panel.height - 28U, 12U, 12U
    };
}

static enum ui_status draw_launcher(struct ui_rect damage)
{
    if (!launcher_open) {
        return UI_STATUS_OK;
    }
    const struct ui_rect panel = launcher_bounds();
    const struct ui_rect search = launcher_search_rect();
    const size_t matches = launcher_match_count();
    const size_t pages = launcher_page_count();
    enum ui_status status = translucent_fill((struct ui_rect){
        0U, UI_MENU_HEIGHT, state.layout.surface.width,
        state.layout.surface.height - UI_MENU_HEIGHT
    }, damage, framebuffer_pack(0x03U, 0x0AU, 0x12U), 72U);

    for (uint32_t row = 0U; row < panel.height &&
            status == UI_STATUS_OK; ++row) {
        const uint32_t edge = row < 14U ? 14U - row :
            (row + 14U >= panel.height ?
                row + 14U - panel.height + 1U : 0U);

        status = translucent_fill((struct ui_rect){
            panel.x + edge, panel.y + row,
            panel.width - edge * 2U, 1U
        }, damage, framebuffer_pack(0x16U, 0x22U, 0x31U), 224U);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(panel, damage, panel.x + 28U, panel.y + 27U,
            "Applications", framebuffer_pack(0xF7U, 0xF9U, 0xFCU));
    }
    if (status == UI_STATUS_OK) {
        status = translucent_capsule_fill(search, damage,
            framebuffer_pack(0xFAU, 0xFBU, 0xFCU), 236U);
    }
    if (status == UI_STATUS_OK) {
        status = draw_search_icon((struct ui_rect){
            search.x + 9U, search.y + 6U, 18U, 18U
        }, damage, framebuffer_pack(0x5EU, 0x66U, 0x70U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(search, damage, search.x + 35U, search.y + 21U,
            launcher_query_length == 0U ? "Search applications" :
                launcher_query,
            launcher_query_length == 0U ? state.theme.title_inactive :
                state.theme.ink);
    }
    for (size_t slot = 0U; slot < UI_LAUNCHER_APPS_PER_PAGE &&
            status == UI_STATUS_OK; ++slot) {
        const size_t visible = launcher_page * UI_LAUNCHER_APPS_PER_PAGE +
            slot;
        size_t dock_index;

        if (visible >= matches ||
                !launcher_dock_index_at(visible, &dock_index)) {
            continue;
        }
        const struct ui_rect cell = launcher_app_rect(slot);
        const struct ui_rect icon = {
            cell.x + (cell.width - 66U) / 2U, cell.y + 2U, 66U, 66U
        };
        const struct ui_dock_item *item =
            &state.layout.dock_items[dock_index];

        status = draw_icon(item->id, icon, damage, state.theme.white);
        if (status == UI_STATUS_OK) {
            status = draw_text(cell, damage,
                centered_text_x(cell, item->label), cell.y + 91U,
                item->label, framebuffer_pack(0xF7U, 0xF9U, 0xFCU));
        }
    }
    if (matches == 0U && status == UI_STATUS_OK) {
        status = draw_text(panel, damage, centered_text_x(panel,
            "No applications found"), panel.y + 145U,
            "No applications found", framebuffer_pack(0xC8U, 0xD0U,
                0xD9U));
    }
    for (size_t page = 0U; page < pages && status == UI_STATUS_OK; ++page) {
        const struct ui_rect dot = launcher_page_rect(page);
        status = draw_circle(dot.x + dot.width / 2U,
            dot.y + dot.height / 2U, page == launcher_page ? 5U : 3U,
            damage, page == launcher_page ?
                framebuffer_pack(0xF6U, 0xF8U, 0xFBU) :
                framebuffer_pack(0x7DU, 0x89U, 0x98U));
    }
    return status;
}

static enum ui_status draw_menu_brand(struct ui_rect damage)
{
    const char *const application = state.active_panel == UI_PANEL_NONE ?
        "S.now" : ui_panel_name(state.active_panel);
    enum ui_status status = draw_logo_color((struct ui_rect){ 8U, 3U,
        18U, 17U }, damage);

    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 34U,
            state.layout.menu_baseline, application, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 142U,
            state.layout.menu_baseline, "File", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 184U,
            state.layout.menu_baseline, "Edit", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 228U,
            state.layout.menu_baseline, "View", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 274U,
            state.layout.menu_baseline, "Go", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 306U,
            state.layout.menu_baseline, "Window", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 378U,
            state.layout.menu_baseline, "Help", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        const struct ui_rect search = menu_search_rect();

        status = draw_search_icon((struct ui_rect){
            search.x + 3U, search.y + 1U, 18U, 18U
        }, damage, framebuffer_pack(0x4FU, 0x55U, 0x59U));
    }
    return status;
}

static enum ui_status render_region(struct ui_rect damage, bool full)
{
    enum ui_status status;

    if (panel_anim_pending != UI_ANIM_PENDING_NONE) {
        status = panel_anim_start();
        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    status = draw_desktop_pattern(damage);

    if (status == UI_STATUS_OK) {
        status = menu_glass ?
            gradient_rect(state.layout.menu_bar, damage,
                0xF8U, 0xFAU, 0xFBU, 0xA9U, 0xB3U, 0xB8U) :
            fill_clipped(state.layout.menu_bar, damage,
                framebuffer_pack(0xD2U, 0xD6U, 0xD8U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            state.layout.menu_bar.x,
            state.layout.menu_bar.y + state.layout.menu_bar.height - 1U,
            state.layout.menu_bar.width, 1U
        }, damage, framebuffer_pack(0x65U, 0x6BU, 0x6EU));
    }
    if (status == UI_STATUS_OK) {
        status = draw_menu_brand(damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_panel(damage);
    }
    if (status == UI_STATUS_OK && ui_anim_running(&panel_anim)) {
        status = draw_animated_panel(damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_launcher(damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_dock_shelf(damage);
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT &&
         status == UI_STATUS_OK; ++index) {
        status = draw_dock_item(&state.layout.dock_items[index], damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_cursor(damage);
    }
    if (status != UI_STATUS_OK) {
        return status;
    }
    if (surface_present(canvas) != SURFACE_STATUS_OK) {
        return UI_STATUS_SURFACE_FAILURE;
    }
    if (full) {
        state.renders.full_draws += 1U;
    } else {
        state.renders.damaged_draws += 1U;
    }
    state.renders.damage_rectangles += 1U;
    state.renders.pixels_copied += canvas->last_present_pixels;
    return UI_STATUS_OK;
}

static uint64_t surface_hash(void)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    const uint32_t telemetry_x = canvas->width >
        UI_MENU_TELEMETRY_MAX_WIDTH ?
        canvas->width - UI_MENU_TELEMETRY_MAX_WIDTH : 0U;

    for (uint32_t y = 0U; y < canvas->height; ++y) {
        for (uint32_t x = 0U; x < canvas->width; ++x) {
            /*
             * The right menu strip contains live uptime telemetry.  A minute
             * boundary between an interaction frame and the installed proof
             * is expected to change those glyphs and is not stale damage.
             * Keep the stable hash over every other desktop pixel, including
             * the brand, menus, windows, cursor, wallpaper, and complete Dock.
             */
            if (y < state.layout.menu_bar.height && x >= telemetry_x) {
                continue;
            }
            hash ^= canvas->pixels[(size_t)y * canvas->width + x];
            hash *= UINT64_C(0x100000001B3);
        }
    }
    return hash;
}

static uint64_t surface_tile_hash(uint32_t tile_x, uint32_t tile_y)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    const uint32_t telemetry_x = canvas->width >
        UI_MENU_TELEMETRY_MAX_WIDTH ?
        canvas->width - UI_MENU_TELEMETRY_MAX_WIDTH : 0U;
    uint32_t right = tile_x + UI_REDRAW_DIAGNOSTIC_TILE;
    uint32_t bottom = tile_y + UI_REDRAW_DIAGNOSTIC_TILE;

    if (right > canvas->width) {
        right = canvas->width;
    }
    if (bottom > canvas->height) {
        bottom = canvas->height;
    }
    for (uint32_t y = tile_y; y < bottom; ++y) {
        for (uint32_t x = tile_x; x < right; ++x) {
            if (y < state.layout.menu_bar.height && x >= telemetry_x) {
                continue;
            }
            hash ^= canvas->pixels[(size_t)y * canvas->width + x];
            hash *= UINT64_C(0x100000001B3);
        }
    }
    return hash;
}

static void snapshot_redraw_tiles(void)
{
    const uint32_t columns = (canvas->width +
        UI_REDRAW_DIAGNOSTIC_TILE - 1U) / UI_REDRAW_DIAGNOSTIC_TILE;
    const uint32_t rows = (canvas->height +
        UI_REDRAW_DIAGNOSTIC_TILE - 1U) / UI_REDRAW_DIAGNOSTIC_TILE;

    for (uint32_t row = 0U; row < rows; ++row) {
        for (uint32_t column = 0U; column < columns; ++column) {
            redraw_tile_hashes[(size_t)row * columns + column] =
                surface_tile_hash(column * UI_REDRAW_DIAGNOSTIC_TILE,
                    row * UI_REDRAW_DIAGNOSTIC_TILE);
        }
    }
}

static void report_redraw_tile_mismatch(void)
{
    const uint32_t columns = (canvas->width +
        UI_REDRAW_DIAGNOSTIC_TILE - 1U) / UI_REDRAW_DIAGNOSTIC_TILE;
    const uint32_t rows = (canvas->height +
        UI_REDRAW_DIAGNOSTIC_TILE - 1U) / UI_REDRAW_DIAGNOSTIC_TILE;
    uint32_t minimum_column = columns;
    uint32_t minimum_row = rows;
    uint32_t maximum_column = 0U;
    uint32_t maximum_row = 0U;
    uint32_t changed = 0U;

    for (uint32_t row = 0U; row < rows; ++row) {
        for (uint32_t column = 0U; column < columns; ++column) {
            if (redraw_tile_hashes[(size_t)row * columns + column] ==
                    surface_tile_hash(column * UI_REDRAW_DIAGNOSTIC_TILE,
                        row * UI_REDRAW_DIAGNOSTIC_TILE)) {
                continue;
            }
            if (column < minimum_column) {
                minimum_column = column;
            }
            if (column > maximum_column) {
                maximum_column = column;
            }
            if (row < minimum_row) {
                minimum_row = row;
            }
            if (row > maximum_row) {
                maximum_row = row;
            }
            ++changed;
        }
    }
    console_serial_write("RW REDRAW changed tiles ");
    console_serial_write_u64(changed);
    if (changed != 0U) {
        console_serial_write(" bounds ");
        console_serial_write_u64((uint64_t)minimum_column *
            UI_REDRAW_DIAGNOSTIC_TILE);
        console_serial_write(",");
        console_serial_write_u64((uint64_t)minimum_row *
            UI_REDRAW_DIAGNOSTIC_TILE);
        console_serial_write(" ");
        console_serial_write_u64((uint64_t)(maximum_column -
            minimum_column + 1U) * UI_REDRAW_DIAGNOSTIC_TILE);
        console_serial_write("x");
        console_serial_write_u64((uint64_t)(maximum_row - minimum_row + 1U) *
            UI_REDRAW_DIAGNOSTIC_TILE);
    }
    console_serial_write("\n");
}

enum ui_status ui_construct(bool pointer_present)
{
    uint32_t logo_width;
    uint32_t logo_height;
    uint32_t wallpaper_width;
    uint32_t wallpaper_height;
    uint32_t wallpaper_frames;
    const struct framebuffer_state framebuffer = framebuffer_get_state();

    if (state.initialized) {
        return UI_STATUS_ALREADY_INITIALIZED;
    }
    if (!framebuffer_is_active() || !screen_is_active() ||
        !ui_font_is_verified()) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    canvas = screen_surface();
    if (canvas == NULL) {
        return UI_STATUS_SURFACE_FAILURE;
    }
    enum ui_status status = ui_layout_build(framebuffer.width,
        framebuffer.height, &state.layout);

    if (status != UI_STATUS_OK) {
        canvas = NULL;
        return status;
    }
    panel_home = state.layout.panel;
    if (sapote_logo_geometry(&logo_width, &logo_height) != LOGO_STATUS_OK ||
        logo_width != UI_LOGO_WIDTH || logo_height != UI_LOGO_HEIGHT ||
        sapote_logo_decode(logo_pixels, UI_LOGO_PIXELS,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_logo_decode_alpha(logo_alpha, UI_LOGO_PIXELS) !=
            LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_LOGO_FAILURE;
    }
    logo_red_shift = framebuffer.red_position;
    logo_green_shift = framebuffer.green_position;
    logo_blue_shift = framebuffer.blue_position;
    if (sapote_studio_icon_geometry(&studio_icon_width,
            &studio_icon_height) != LOGO_STATUS_OK ||
        studio_icon_width == 0U || studio_icon_width > 80U ||
        studio_icon_height == 0U || studio_icon_height > 80U ||
        sapote_studio_icon_decode(studio_icon_pixels,
            (size_t)studio_icon_width * studio_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_studio_icon_decode_alpha(studio_icon_alpha,
            (size_t)studio_icon_width * studio_icon_height) != LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_STUDIO_ICON_FAILURE;
    }
    if (sapote_settings_icon_geometry(&settings_icon_width,
            &settings_icon_height) != LOGO_STATUS_OK ||
        settings_icon_width == 0U || settings_icon_width > 80U ||
        settings_icon_height == 0U || settings_icon_height > 80U ||
        sapote_settings_icon_decode(settings_icon_pixels,
            (size_t)settings_icon_width * settings_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_settings_icon_decode_alpha(settings_icon_alpha,
            (size_t)settings_icon_width * settings_icon_height) !=
                LOGO_STATUS_OK ||
        sapote_camera_icon_geometry(&camera_icon_width,
            &camera_icon_height) != LOGO_STATUS_OK ||
        camera_icon_width == 0U || camera_icon_width > 80U ||
        camera_icon_height == 0U || camera_icon_height > 80U ||
        sapote_camera_icon_decode(camera_icon_pixels,
            (size_t)camera_icon_width * camera_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_camera_icon_decode_alpha(camera_icon_alpha,
            (size_t)camera_icon_width * camera_icon_height) !=
                LOGO_STATUS_OK ||
        sapote_canvas_icon_geometry(&canvas_icon_width,
            &canvas_icon_height) != LOGO_STATUS_OK ||
        canvas_icon_width == 0U || canvas_icon_width > 80U ||
        canvas_icon_height == 0U || canvas_icon_height > 80U ||
        sapote_canvas_icon_decode(canvas_icon_pixels,
            (size_t)canvas_icon_width * canvas_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_canvas_icon_decode_alpha(canvas_icon_alpha,
            (size_t)canvas_icon_width * canvas_icon_height) !=
                LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_APP_ICON_FAILURE;
    }
    if (sapote_files_icon_geometry(&files_icon_width,
            &files_icon_height) != LOGO_STATUS_OK ||
        files_icon_width == 0U || files_icon_width > 80U ||
        files_icon_height == 0U || files_icon_height > 80U ||
        sapote_files_icon_decode(files_icon_pixels,
            (size_t)files_icon_width * files_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_files_icon_decode_alpha(files_icon_alpha,
            (size_t)files_icon_width * files_icon_height) != LOGO_STATUS_OK ||
        sapote_terminal_icon_geometry(&terminal_icon_width,
            &terminal_icon_height) != LOGO_STATUS_OK ||
        terminal_icon_width == 0U || terminal_icon_width > 80U ||
        terminal_icon_height == 0U || terminal_icon_height > 80U ||
        sapote_terminal_icon_decode(terminal_icon_pixels,
            (size_t)terminal_icon_width * terminal_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_terminal_icon_decode_alpha(terminal_icon_alpha,
            (size_t)terminal_icon_width * terminal_icon_height) !=
                LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_APP_ICON_FAILURE;
    }
    if (sapote_store_icon_geometry(&store_icon_width,
            &store_icon_height) != LOGO_STATUS_OK ||
        store_icon_width == 0U || store_icon_width > 80U ||
        store_icon_height == 0U || store_icon_height > 80U ||
        sapote_store_icon_decode(store_icon_pixels,
            (size_t)store_icon_width * store_icon_height,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_store_icon_decode_alpha(store_icon_alpha,
            (size_t)store_icon_width * store_icon_height) !=
                LOGO_STATUS_OK ||
        sapote_store_ui_icons_geometry(&store_ui_icon_width,
            &store_ui_icon_height) != LOGO_STATUS_OK ||
        store_ui_icon_width != UI_STORE_ICON_SHEET_WIDTH ||
        store_ui_icon_height != UI_STORE_ICON_SHEET_HEIGHT ||
        sapote_store_ui_icons_decode(store_ui_icon_pixels,
            UI_STORE_ICON_SHEET_WIDTH * UI_STORE_ICON_SHEET_HEIGHT,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, 0U) != LOGO_STATUS_OK ||
        sapote_store_ui_icons_decode_alpha(store_ui_icon_alpha,
            UI_STORE_ICON_SHEET_WIDTH * UI_STORE_ICON_SHEET_HEIGHT) !=
                LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_APP_ICON_FAILURE;
    }
    if (sapote_settings_category_icons_geometry(
            &settings_category_icon_width,
            &settings_category_icon_height) != LOGO_STATUS_OK ||
        settings_category_icon_width != 256U ||
        settings_category_icon_height != 192U ||
        sapote_settings_category_icons_decode(settings_category_icon_pixels,
            256U * 192U, framebuffer.red_position,
            framebuffer.green_position, framebuffer.blue_position, 0U) !=
                LOGO_STATUS_OK ||
        sapote_settings_category_icons_decode_alpha(
            settings_category_icon_alpha, 256U * 192U) != LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_APP_ICON_FAILURE;
    }
    if (sapote_wallpaper_geometry(&wallpaper_width, &wallpaper_height,
            &wallpaper_frames) != WALLPAPER_STATUS_OK ||
        wallpaper_width != 1024U || wallpaper_height != 768U ||
        wallpaper_frames != UI_WALLPAPER_COUNT ||
        sapote_wallpaper_decode(0U, wallpaper_pixels, 1024U * 768U,
            1024U, 768U,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position) != WALLPAPER_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_WALLPAPER_FAILURE;
    }

    install_theme(&state.theme);
    state.pointer_present = pointer_present;
    state.focus = UI_ELEMENT_DOCK_FILES;
    state.hover = UI_ELEMENT_NONE;
    state.pressed = UI_ELEMENT_NONE;
    state.active_panel = UI_PANEL_NONE;
    panel_order_count = 0U;
    panel_cascade = 0U;
    for (size_t index = 0U; index < UI_PANEL_COUNT; ++index) {
        panel_open[index] = false;
        panel_minimized[index] = false;
        panel_maximized[index] = false;
        panel_windows[index] = panel_home;
        panel_restore[index] = panel_home;
        panel_origins[index] = (struct ui_rect){ 0U, 0U, 0U, 0U };
        panel_origin_valid[index] = false;
    }
    for (size_t index = 0U; index < UI_NATIVE_WINDOW_COUNT; ++index) {
        native_windows[index] = (struct ui_native_window_record){ 0 };
    }
    install_panel_geometry(UI_PANEL_NONE);
    state.ledger_pass = false;
    terminal_welcomed = false;
    settings_page = -1;
    dock_dark = false;
    launcher_open = false;
    launcher_search_focused = false;
    launcher_query_length = 0U;
    launcher_query[0] = '\0';
    launcher_page = 0U;
    application_launch_path[0] = '\0';
    store_section = 0U;
    store_search_focused = false;
    store_query_length = 0U;
    store_query[0] = '\0';
    desktop_wallpaper = 0U;
    camera_capture_count = 0U;
    camera_frame_available = false;
    camera_seen_generation = 0U;
    camera_initialize();
    ui_anim_reset(&panel_anim);
    panel_anim_panel = UI_PANEL_NONE;
    panel_anim_origin = (struct ui_rect){ 0U, 0U, 0U, 0U };
    panel_anim_frame = (struct ui_rect){ 0U, 0U, 0U, 0U };
    panel_anim_pending = UI_ANIM_PENDING_NONE;
    panel_anim_driver = false;
    pending_native_origin = (struct ui_rect){ 0U, 0U, 0U, 0U };
    pending_native_origin_valid = false;
    motion_timer_id = 0U;
    dock_spring_active = false;
    dock_spring_last_ns = 0U;
    panel_drag_active = false;
    panel_drag_panel = UI_PANEL_NONE;
    panel_drag_anchor = (struct ui_point){ 0, 0 };
    panel_drag_origin = panel_home;
    event_count = 0U;

    if (pointer_present) {
        if (pointer_set_bounds(framebuffer.width, framebuffer.height) !=
            POINTER_STATUS_OK) {
            canvas = NULL;
            return UI_STATUS_BAD_CURSOR_HOTSPOT;
        }
        const struct pointer_state pointer = pointer_get_state();
        state.pointer = (struct ui_point){
            (int32_t)pointer.x, (int32_t)pointer.y
        };
    } else {
        state.pointer = (struct ui_point){ 0, 0 };
    }
    dock3d_initialize(&dock_model, 58U, framebuffer.width,
        framebuffer.height);
    dock3d_set_pointer(&dock_model, state.pointer.x, state.pointer.y,
        pointer_present, dock_magnification);
    dock_sync_layout();

    if (screen_set_palette(0x08U, 0x10U, 0x12U,
            0x9DU, 0xD7U, 0xA3U) != SCREEN_STATUS_OK ||
        screen_set_deferred_present(true) != SCREEN_STATUS_OK ||
        screen_set_visible(false) != SCREEN_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_SCREEN_FAILURE;
    }
    state.initialized = true;
    return UI_STATUS_OK;
}

enum ui_status ui_activate(void)
{
    if (!state.initialized) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    if (state.active) {
        return UI_STATUS_ALREADY_INITIALIZED;
    }
    state.active = true;
    const enum ui_status status = render_region(state.layout.surface, true);

    if (status != UI_STATUS_OK) {
        state.active = false;
        return status;
    }
    state.stable_render_hash = surface_hash();
    return UI_STATUS_OK;
}

enum ui_status ui_terminal_draw_logo(void)
{
    const uint32_t width = 148U;
    const uint32_t height = width * UI_LOGO_HEIGHT / UI_LOGO_WIDTH;

    if (!state.initialized || canvas == NULL) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    return screen_draw_image(logo_pixels, logo_alpha,
        UI_LOGO_WIDTH, UI_LOGO_HEIGHT, width, height, 9U) == SCREEN_STATUS_OK ?
        UI_STATUS_OK : UI_STATUS_SCREEN_FAILURE;
}

void ui_animation_attach(void)
{
    panel_anim_driver = true;
}

bool ui_animation_active(void)
{
    return state.active &&
        (panel_anim_pending != UI_ANIM_PENDING_NONE ||
            ui_anim_running(&panel_anim));
}

bool ui_is_active(void)
{
    return state.active;
}

const struct ui_state *ui_get_state(void)
{
    return &state;
}

static enum ui_status publish_unlocked(const struct ui_event *event)
{
    if (event == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (event->type <= UI_EVENT_NONE || event->type >= UI_EVENT_TYPE_COUNT) {
        return UI_STATUS_BAD_EVENT;
    }

    state.events.accepted += 1U;
    if (event->type == UI_EVENT_POINTER_MOVEMENT && event_count > 0U &&
        event_queue[event_count - 1U].type == UI_EVENT_POINTER_MOVEMENT) {
        event_queue[event_count - 1U] = *event;
        state.events.coalesced += 1U;
        return UI_STATUS_OK;
    }
    if (event_count == UI_EVENT_QUEUE_CAPACITY) {
        if (event->type == UI_EVENT_POINTER_MOVEMENT) {
            state.events.dropped += 1U;
            return UI_STATUS_EVENT_QUEUE_FULL;
        }
        size_t movement = event_count;
        for (size_t index = 0U; index < event_count; ++index) {
            if (event_queue[index].type == UI_EVENT_POINTER_MOVEMENT) {
                movement = index;
                break;
            }
        }
        if (movement == event_count) {
            state.events.dropped += 1U;
            return UI_STATUS_EVENT_QUEUE_FULL;
        }
        for (size_t index = movement + 1U; index < event_count; ++index) {
            event_queue[index - 1U] = event_queue[index];
        }
        event_count -= 1U;
        state.events.dropped += 1U;
    }
    event_queue[event_count++] = *event;
    return UI_STATUS_OK;
}

enum ui_status ui_event_publish(const struct ui_event *event)
{
    const bool enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }
    const enum ui_status status = publish_unlocked(event);
    if (enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

static bool pop_event(struct ui_event *event)
{
    const bool enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }
    if (event_count == 0U) {
        if (enabled) {
            cpu_interrupt_enable();
        }
        return false;
    }
    *event = event_queue[0];
    for (size_t index = 1U; index < event_count; ++index) {
        event_queue[index - 1U] = event_queue[index];
    }
    event_count -= 1U;
    state.events.drained += 1U;
    if (enabled) {
        cpu_interrupt_enable();
    }
    return true;
}

enum ui_status ui_handle_keyboard(const struct keyboard_event *event)
{
    struct ui_event ui_event = { 0 };
    uint32_t native_slot;

    if (event == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (!state.active) {
        return UI_STATUS_OK;
    }
    if (launcher_open) {
        if (!event->pressed) {
            return UI_STATUS_OK;
        }
        if (event->scancode == 0x01U) {
            ui_event.type = UI_EVENT_PANEL_CLOSE;
        } else if (event->scancode == 0x1CU) {
            ui_event.type = UI_EVENT_KEYBOARD_ACTIVATION;
        } else if (event->scancode == 0x0EU) {
            ui_event.type = UI_EVENT_TEXT_INPUT;
            ui_event.character = '\b';
        } else if (!event->control && event->character >= ' ' &&
                event->character <= '~') {
            ui_event.type = UI_EVENT_TEXT_INPUT;
            ui_event.character = event->character;
        } else {
            return UI_STATUS_OK;
        }
        return ui_event_publish(&ui_event);
    }
    if (native_panel_slot(state.active_panel, &native_slot) &&
        native_windows[native_slot].active) {
        const struct ui_native_event native_event = {
            .type = UI_NATIVE_EVENT_KEY,
            .monotonic_ns = clock_monotonic_ns(),
            .code = (uint32_t)event->scancode |
                ((uint32_t)(uint8_t)event->character << 8U),
            .value = event->pressed ? 1U : 0U,
            .modifiers = (event->shift ? 1U : 0U) |
                (event->control ? 2U : 0U)
        };

        native_event_emit(state.active_panel, &native_event);
        return UI_STATUS_OK;
    }
    if (!event->pressed) {
        return UI_STATUS_OK;
    }
    if (state.active_panel == UI_PANEL_STORE && store_search_focused &&
            event->scancode != 0x01U && !event->control &&
            (event->scancode == 0x0EU ||
                (event->character >= ' ' && event->character <= '~'))) {
        ui_event.type = UI_EVENT_TEXT_INPUT;
        ui_event.character = event->scancode == 0x0EU ? '\b' :
            event->character;
    } else if ((state.active_panel == UI_PANEL_NOTES ||
            state.active_panel == UI_PANEL_STUDIO) &&
        event->scancode != 0x01U) {
        ui_event.type = UI_EVENT_TEXT_INPUT;
        ui_event.control = event->control;
        if (event->control && (event->character == 's' ||
                event->character == 'S')) {
            ui_event.character = 's';
        } else if (state.active_panel == UI_PANEL_STUDIO) {
            return UI_STATUS_OK;
        } else if (event->scancode == 0x0EU) {
            ui_event.character = '\b';
        } else if (event->scancode == 0x1CU) {
            ui_event.character = '\n';
        } else if (!event->control && event->character >= ' ' &&
                event->character <= '~') {
            ui_event.character = event->character;
        } else {
            return UI_STATUS_OK;
        }
    } else if (event->scancode == 0x0FU) {
        ui_event.type = event->shift ? UI_EVENT_KEYBOARD_FOCUS_PREVIOUS :
            UI_EVENT_KEYBOARD_FOCUS_NEXT;
    } else if (event->scancode == 0x1CU) {
        ui_event.type = UI_EVENT_KEYBOARD_ACTIVATION;
    } else if (event->scancode == 0x01U) {
        ui_event.type = UI_EVENT_PANEL_CLOSE;
    } else {
        return UI_STATUS_OK;
    }
    return ui_event_publish(&ui_event);
}

static enum ui_element_id next_focus(enum ui_element_id current, bool previous)
{
    if (current < UI_ELEMENT_DOCK_FILES || current > UI_ELEMENT_DOCK_SETTINGS) {
        return UI_ELEMENT_DOCK_FILES;
    }
    if (previous) {
        return current == UI_ELEMENT_DOCK_FILES ?
            (keyboard_focus_wrap ? UI_ELEMENT_DOCK_SETTINGS : current) :
            (enum ui_element_id)(current - 1);
    }
    return current == UI_ELEMENT_DOCK_SETTINGS ?
        (keyboard_focus_wrap ? UI_ELEMENT_DOCK_FILES : current) :
        (enum ui_element_id)(current + 1);
}

static enum ui_status set_panel(
    enum ui_panel_id panel,
    struct ui_rect *damage
)
{
    const enum ui_panel_id old_panel = state.active_panel;
    bool opening = false;
    bool restoring = false;

    if (panel < UI_PANEL_NONE || panel >= UI_PANEL_COUNT || damage == NULL) {
        return UI_STATUS_BAD_PANEL;
    }
    panel_drag_active = false;
    if (old_panel == panel) {
        return UI_STATUS_OK;
    }
    ui_anim_end(&panel_anim);
    panel_anim_pending = UI_ANIM_PENDING_NONE;
    panel_anim_panel = UI_PANEL_NONE;
    if (!dock_spring_active && motion_timer_id != 0U) {
        (void)timer_cancel((uint64_t)motion_timer_id);
        motion_timer_id = 0U;
    }
    if (panel == UI_PANEL_NONE &&
        native_panel_slot(old_panel, NULL)) {
        const struct ui_native_event close_event = {
            .type = UI_NATIVE_EVENT_CLOSE,
            .monotonic_ns = clock_monotonic_ns()
        };

        native_event_emit(old_panel, &close_event);
    }
    native_focus_emit(old_panel, false);
    if (panel == UI_PANEL_NONE && old_panel == UI_PANEL_NOTES && note_dirty &&
            note_save() != SAPFS_STATUS_OK) {
        *damage = rect_union(*damage, state.layout.panel);
        return UI_STATUS_OK;
    }
    if (panel == UI_PANEL_NONE && old_panel == UI_PANEL_STUDIO &&
            studio_dirty && studio_save() != SAPFS_STATUS_OK) {
        *damage = rect_union(*damage, state.layout.panel);
        return UI_STATUS_OK;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (state.layout.dock_items[index].panel == old_panel ||
            state.layout.dock_items[index].panel == panel) {
            *damage = rect_union(*damage,
                dock_bounds_for(&state.layout,
                    state.layout.dock_items[index].id));
        }
    }
    if (panel == UI_PANEL_NONE) {
        if (old_panel != UI_PANEL_NONE) {
            if (panel_anim_driver && window_motion) {
                panel_anim_panel = old_panel;
                panel_anim_frame = panel_windows[old_panel];
                panel_anim_origin = origin_for_panel(old_panel, false);
                panel_anim_pending = UI_ANIM_PENDING_CLOSE;
            }
            panel_open[old_panel] = false;
            panel_minimized[old_panel] = false;
            panel_maximized[old_panel] = false;
            panel_origin_valid[old_panel] = false;
            remove_panel_from_order(old_panel);
        }
        state.active_panel = front_panel();
        install_panel_geometry(state.active_panel);
    } else {
        opening = !panel_open[panel];
        restoring = panel_minimized[panel];
        if (opening) {
            uint32_t native_slot;

            if (!native_panel_slot(panel, &native_slot)) {
                const uint32_t offset_x = (uint32_t)panel_cascade * 14U;
                const uint32_t offset_y = (uint32_t)panel_cascade * 11U;

                panel_windows[panel] = (struct ui_rect){
                    panel_home.x + offset_x, panel_home.y + offset_y,
                    panel_home.width, panel_home.height
                };
                panel_cascade = (uint8_t)((panel_cascade + 1U) %
                    (UI_PANEL_COUNT - 1U));
            }
            panel_restore[panel] = panel_windows[panel];
            panel_maximized[panel] = false;
            panel_open[panel] = true;
        }
        panel_minimized[panel] = false;
        bring_panel_to_front(panel);
        state.active_panel = panel;
        install_panel_geometry(panel);
        if ((opening || restoring) && panel_anim_driver && window_motion) {
            panel_anim_panel = panel;
            panel_anim_frame = panel_windows[panel];
            panel_anim_origin = origin_for_panel(panel, opening);
            panel_origins[panel] = panel_anim_origin;
            panel_origin_valid[panel] = true;
            panel_anim_pending = UI_ANIM_PENDING_OPEN;
        }
    }
    native_focus_emit(state.active_panel, true);
    state.renders.panel_transitions += 1U;
    *damage = rect_union(*damage, state.layout.surface);

    if (dock_spring_active && motion_timer_id == 0U &&
            timer_is_started()) {
        (void)motion_schedule_wake(clock_monotonic_ns());
    }

    if (opening && panel == UI_PANEL_FILES) {
        (void)files_refresh();
    } else if (opening && panel == UI_PANEL_NOTES) {
        (void)note_load();
    } else if (opening && panel == UI_PANEL_STUDIO) {
        (void)studio_load();
    }

    if (panel_open[UI_PANEL_TERMINAL]) {
        const struct ui_rect terminal = panel_windows[UI_PANEL_TERMINAL];
        const struct ui_rect terminal_client = {
            terminal.x + 10U, terminal.y + 38U,
            terminal.width - 20U, terminal.height - 48U
        };

        if (screen_set_viewport(surface_rect_of(terminal_client), true) !=
                SCREEN_STATUS_OK) {
            return UI_STATUS_SCREEN_FAILURE;
        }
        if (opening && panel == UI_PANEL_TERMINAL && !terminal_welcomed) {
            if (screen_clear() != SCREEN_STATUS_OK ||
                screen_write("Sapote terminal\n"
                    "Type help for commands. Type fetch for system identity.\n"
                    "\nsap> ") != SCREEN_STATUS_OK) {
                return UI_STATUS_SCREEN_FAILURE;
            }
            console_serial_write("Sapote: Redwood Terminal opened\n");
            terminal_welcomed = true;
        }
    } else if (screen_set_visible(false) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    return UI_STATUS_OK;
}

static struct ui_rect maximized_panel_geometry(void)
{
    const uint32_t x = 8U;
    const uint32_t y = UI_MENU_HEIGHT + 8U;
    const uint32_t bottom = state.layout.surface.height > 98U ?
        state.layout.surface.height - 98U : state.layout.surface.height;

    return (struct ui_rect){ x, y,
        state.layout.surface.width - x * 2U,
        bottom > y ? bottom - y : 1U };
}

static enum ui_status toggle_panel_maximized(struct ui_rect *damage)
{
    const enum ui_panel_id panel = state.active_panel;

    if (damage == NULL || panel <= UI_PANEL_NONE || panel >= UI_PANEL_COUNT) {
        return UI_STATUS_BAD_PANEL;
    }
    if (panel_maximized[panel]) {
        panel_windows[panel] = panel_restore[panel];
        panel_maximized[panel] = false;
    } else {
        panel_restore[panel] = panel_windows[panel];
        panel_windows[panel] = maximized_panel_geometry();
        panel_maximized[panel] = true;
    }
    install_panel_geometry(panel);
    panel_drag_active = false;
    ui_anim_end(&panel_anim);
    panel_anim_pending = UI_ANIM_PENDING_NONE;
    panel_anim_panel = UI_PANEL_NONE;
    if (panel == UI_PANEL_TERMINAL &&
            screen_set_viewport(surface_rect_of(state.layout.panel_client),
                true) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    state.renders.panel_transitions += 1U;
    *damage = state.layout.surface;
    return UI_STATUS_OK;
}

static enum ui_status minimize_active_panel(struct ui_rect *damage)
{
    const enum ui_panel_id panel = state.active_panel;

    if (damage == NULL || panel <= UI_PANEL_NONE || panel >= UI_PANEL_COUNT ||
            !panel_open[panel]) {
        return UI_STATUS_BAD_PANEL;
    }
    ui_anim_end(&panel_anim);
    panel_anim_pending = UI_ANIM_PENDING_NONE;
    panel_anim_panel = UI_PANEL_NONE;
    if (panel_anim_driver && window_motion) {
        panel_anim_panel = panel;
        panel_anim_frame = panel_windows[panel];
        panel_anim_origin = origin_for_panel(panel, false);
        panel_anim_pending = UI_ANIM_PENDING_CLOSE;
    }
    native_focus_emit(panel, false);
    panel_minimized[panel] = true;
    remove_panel_from_order(panel);
    state.active_panel = front_panel();
    install_panel_geometry(state.active_panel);
    native_focus_emit(state.active_panel, true);
    panel_drag_active = false;
    if (panel == UI_PANEL_TERMINAL &&
            screen_set_visible(false) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    if (state.active_panel == UI_PANEL_TERMINAL &&
            screen_set_viewport(surface_rect_of(state.layout.panel_client),
                true) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    state.renders.panel_transitions += 1U;
    *damage = state.layout.surface;
    return UI_STATUS_OK;
}

static enum ui_element_id active_hit(struct ui_point point)
{
    if (rect_contains_point(menu_search_rect(), point)) {
        return UI_ELEMENT_MENU_SEARCH;
    }
    if (launcher_open) {
        const struct ui_rect panel = launcher_bounds();

        if (rect_contains_point(launcher_search_rect(), point)) {
            return UI_ELEMENT_LAUNCHER_SEARCH;
        }
        for (size_t slot = 0U; slot < UI_LAUNCHER_APPS_PER_PAGE; ++slot) {
            size_t dock_index;
            const size_t visible = launcher_page *
                UI_LAUNCHER_APPS_PER_PAGE + slot;

            if (launcher_dock_index_at(visible, &dock_index) &&
                    rect_contains_point(launcher_app_rect(slot), point)) {
                return (enum ui_element_id)(UI_ELEMENT_LAUNCHER_APP_0 +
                    dock_index);
            }
        }
        const size_t pages = launcher_page_count();
        for (size_t page = 0U; page < pages; ++page) {
            if (rect_contains_point(launcher_page_rect(page), point)) {
                return (enum ui_element_id)(UI_ELEMENT_LAUNCHER_PAGE_0 +
                    page);
            }
        }
        return rect_contains_point(panel, point) ? UI_ELEMENT_NONE :
            UI_ELEMENT_LAUNCHER_DISMISS;
    }
    const int dock_hit = dock3d_hit(&dock_model, point.x, point.y);
    enum ui_element_id hit = dock_hit >= 0 ?
        (enum ui_element_id)(UI_ELEMENT_DOCK_FILES + dock_hit) :
        UI_ELEMENT_NONE;

    if (hit != UI_ELEMENT_NONE) {
        return hit;
    }
    if (state.active_panel == UI_PANEL_NONE) {
        return UI_ELEMENT_NONE;
    }
    const struct ui_rect window_controls[3U] = {
        { state.layout.panel.x + 10U, state.layout.panel.y + 8U, 18U, 18U },
        { state.layout.panel.x + 32U, state.layout.panel.y + 8U, 18U, 18U },
        { state.layout.panel.x + 54U, state.layout.panel.y + 8U, 18U, 18U }
    };
    static const enum ui_element_id window_control_ids[3U] = {
        UI_ELEMENT_WINDOW_CLOSE, UI_ELEMENT_WINDOW_MAXIMIZE,
        UI_ELEMENT_WINDOW_MINIMIZE
    };
    for (size_t index = 0U; index < 3U; ++index) {
        if (rect_contains_point(window_controls[index], point)) {
            return window_control_ids[index];
        }
    }
    const struct ui_rect client = state.layout.panel_client;
    if (state.active_panel == UI_PANEL_FILES) {
        const struct ui_rect buttons[] = {
            { client.x + 10U, client.y + 8U, 32U, 26U },
            { client.x + 226U, client.y + 8U, 38U, 26U }
        };
        static const enum ui_element_id ids[] = {
            UI_ELEMENT_FILES_UP, UI_ELEMENT_FILES_REFRESH
        };
        for (size_t index = 0U; index < 2U; ++index) {
            if (rect_contains_point(buttons[index], point)) {
                return ids[index];
            }
        }
        if (rect_contains_point((struct ui_rect){ client.x + 4U,
                client.y + 75U, 182U, 28U }, point)) {
            return UI_ELEMENT_FILES_ROOT;
        }
        for (size_t index = 0U; index < file_entry_count && index < 12U;
             ++index) {
            if (rect_contains_point(file_entry_rect(index), point)) {
                return (enum ui_element_id)(UI_ELEMENT_FILES_ENTRY_0 + index);
            }
        }
    } else if (state.active_panel == UI_PANEL_STUDIO) {
        static const enum ui_element_id ids[] = {
            UI_ELEMENT_STUDIO_NEW, UI_ELEMENT_STUDIO_IMPORT,
            UI_ELEMENT_STUDIO_TRIM, UI_ELEMENT_STUDIO_SAVE,
            UI_ELEMENT_STUDIO_EXPORT
        };

        for (size_t index = 0U; index < 5U; ++index) {
            if (rect_contains_point(studio_button_rect(index), point)) {
                return ids[index];
            }
        }
        if (rect_contains_point(studio_timeline_rect(), point)) {
            return UI_ELEMENT_STUDIO_TIMELINE;
        }
    } else if (state.active_panel == UI_PANEL_STORE) {
        if (rect_contains_point(store_search_rect(), point)) {
            return UI_ELEMENT_STORE_SEARCH;
        }
        for (size_t index = 0U; index < UI_STORE_NAV_COUNT; ++index) {
            if (rect_contains_point(store_nav_rect(index), point)) {
                return (enum ui_element_id)(UI_ELEMENT_STORE_NAV_0 + index);
            }
        }
    } else if (state.active_panel == UI_PANEL_SETTINGS) {
        if (settings_page < 0) {
            for (size_t index = 0U; index < UI_SETTINGS_CATEGORY_COUNT;
                 ++index) {
                if (rect_contains_point(settings_category_rect(index), point)) {
                    return (enum ui_element_id)(
                        UI_ELEMENT_SETTINGS_CATEGORY_0 + index);
                }
            }
        } else {
            if (rect_contains_point(settings_back_rect(), point)) {
                return UI_ELEMENT_SETTINGS_BACK;
            }
            if (settings_page == 0 &&
                    rect_contains_point(settings_appearance_rect(false), point)) {
                return UI_ELEMENT_SETTINGS_APPEARANCE_LIGHT;
            }
            if (settings_page == 0 &&
                    rect_contains_point(settings_appearance_rect(true), point)) {
                return UI_ELEMENT_SETTINGS_APPEARANCE_DARK;
            }
            if (settings_page == 1) {
                for (size_t index = 0U; index < UI_WALLPAPER_COUNT; ++index) {
                    if (rect_contains_point(settings_wallpaper_rect(index),
                            point)) {
                        return (enum ui_element_id)(
                            UI_ELEMENT_SETTINGS_WALLPAPER_0 + index);
                    }
                }
            }
            const size_t option_count = settings_interactive_option_count(
                (size_t)settings_page);
            for (size_t index = 0U; index < option_count; ++index) {
                if (rect_contains_point(settings_option_rect(index), point)) {
                    return (enum ui_element_id)(
                        UI_ELEMENT_SETTINGS_OPTION_0 + index);
                }
            }
        }
    } else if (state.active_panel == UI_PANEL_CAMERA) {
        if (rect_contains_point(camera_capture_rect(), point)) {
            return UI_ELEMENT_CAMERA_CAPTURE;
        }
    }
    return UI_ELEMENT_NONE;
}

static bool panel_title_contains(struct ui_rect panel, struct ui_point point)
{
    if (panel.width <= 82U || panel.height <= UI_PANEL_TITLE_HEIGHT + 4U) {
        return false;
    }
    return rect_contains_point((struct ui_rect){
        panel.x + 76U, panel.y + 4U,
        panel.width - 80U, UI_PANEL_TITLE_HEIGHT
    }, point);
}

static enum ui_status drag_panel_to(
    struct ui_point point,
    struct ui_rect *damage
)
{
    if (!panel_drag_active || panel_drag_panel <= UI_PANEL_NONE ||
            panel_drag_panel >= UI_PANEL_COUNT || damage == NULL) {
        return UI_STATUS_BAD_PANEL;
    }
    if (panel_maximized[panel_drag_panel]) {
        return UI_STATUS_BAD_PANEL;
    }
    const struct ui_rect old_window = panel_windows[panel_drag_panel];
    const int64_t delta_x = (int64_t)point.x -
        (int64_t)panel_drag_anchor.x;
    const int64_t delta_y = (int64_t)point.y -
        (int64_t)panel_drag_anchor.y;
    int64_t next_x = (int64_t)panel_drag_origin.x + delta_x;
    int64_t next_y = (int64_t)panel_drag_origin.y + delta_y;
    const int64_t maximum_x = state.layout.surface.width > old_window.width ?
        (int64_t)(state.layout.surface.width - old_window.width) : 0;
    int64_t maximum_y = state.layout.surface.height > old_window.height ?
        (int64_t)(state.layout.surface.height - old_window.height) :
        (int64_t)UI_MENU_HEIGHT;

    if (maximum_y < (int64_t)UI_MENU_HEIGHT) {
        maximum_y = (int64_t)UI_MENU_HEIGHT;
    }
    if (next_x < 0) {
        next_x = 0;
    } else if (next_x > maximum_x) {
        next_x = maximum_x;
    }
    if (next_y < (int64_t)UI_MENU_HEIGHT) {
        next_y = (int64_t)UI_MENU_HEIGHT;
    } else if (next_y > maximum_y) {
        next_y = maximum_y;
    }
    panel_windows[panel_drag_panel].x = (uint32_t)next_x;
    panel_windows[panel_drag_panel].y = (uint32_t)next_y;
    install_panel_geometry(panel_drag_panel);
    *damage = rect_union(*damage, rect_union(
        drop_shadow_draw_rect(old_window, 6U),
        drop_shadow_draw_rect(panel_windows[panel_drag_panel], 6U)));

    if (panel_drag_panel == UI_PANEL_TERMINAL &&
            screen_set_viewport(surface_rect_of(state.layout.panel_client),
                true) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    return UI_STATUS_OK;
}

static enum ui_status activate_element(
    enum ui_element_id element,
    struct ui_rect *damage
)
{
    if (element == UI_ELEMENT_MENU_SEARCH) {
        launcher_open = !launcher_open;
        launcher_search_focused = launcher_open;
        if (!launcher_open) {
            launcher_page = 0U;
        }
        *damage = state.layout.surface;
        return UI_STATUS_OK;
    }
    if (element == UI_ELEMENT_LAUNCHER_SEARCH && launcher_open) {
        launcher_search_focused = true;
        *damage = rect_union(*damage, launcher_search_rect());
        return UI_STATUS_OK;
    }
    if (element == UI_ELEMENT_LAUNCHER_DISMISS && launcher_open) {
        launcher_open = false;
        launcher_search_focused = false;
        *damage = state.layout.surface;
        return UI_STATUS_OK;
    }
    if (element >= UI_ELEMENT_LAUNCHER_PAGE_0 &&
            element <= UI_ELEMENT_LAUNCHER_PAGE_3 && launcher_open) {
        const size_t page = (size_t)(element - UI_ELEMENT_LAUNCHER_PAGE_0);

        if (page >= launcher_page_count()) {
            return UI_STATUS_BAD_ELEMENT;
        }
        launcher_page = page;
        *damage = rect_union(*damage, launcher_bounds());
        return UI_STATUS_OK;
    }
    if (element >= UI_ELEMENT_LAUNCHER_APP_0 &&
            element <= UI_ELEMENT_LAUNCHER_APP_7 && launcher_open) {
        const size_t dock_index = (size_t)(element -
            UI_ELEMENT_LAUNCHER_APP_0);

        if (dock_index >= UI_DOCK_ITEM_COUNT ||
                !launcher_label_matches(
                    state.layout.dock_items[dock_index].label)) {
            return UI_STATUS_BAD_ELEMENT;
        }
        launcher_open = false;
        launcher_search_focused = false;
        launcher_page = 0U;
        *damage = state.layout.surface;
        dock3d_launch(&dock_model, dock_index);
        begin_dock_spring();
        if (state.layout.dock_items[dock_index].action ==
                UI_ACTION_OPEN_CANVAS) {
            pending_native_origin =
                state.layout.dock_items[dock_index].icon_bounds;
            pending_native_origin_valid = true;
            return copy_string(application_launch_path,
                sizeof(application_launch_path), "CANVAS.MAN") ?
                    UI_STATUS_OK : UI_STATUS_BAD_ELEMENT;
        }
        return set_panel(state.layout.dock_items[dock_index].panel, damage);
    }
    if (element >= UI_ELEMENT_DOCK_FILES &&
        element <= UI_ELEMENT_DOCK_SETTINGS) {
        const size_t dock_index = (size_t)(element - UI_ELEMENT_DOCK_FILES);

        dock3d_launch(&dock_model,
            dock_index);
        begin_dock_spring();
        *damage = rect_union(*damage, dock_visual_bounds(&state.layout));
        if (state.layout.dock_items[dock_index].action ==
                UI_ACTION_OPEN_CANVAS) {
            pending_native_origin =
                state.layout.dock_items[dock_index].icon_bounds;
            pending_native_origin_valid = true;
            return copy_string(application_launch_path,
                sizeof(application_launch_path), "CANVAS.MAN") ?
                    UI_STATUS_OK : UI_STATUS_BAD_ELEMENT;
        }
        return set_panel(panel_for_element(element), damage);
    }
    if (element == UI_ELEMENT_WINDOW_CLOSE) {
        return set_panel(UI_PANEL_NONE, damage);
    }
    if (element == UI_ELEMENT_WINDOW_MAXIMIZE) {
        return toggle_panel_maximized(damage);
    }
    if (element == UI_ELEMENT_WINDOW_MINIMIZE) {
        return minimize_active_panel(damage);
    }
    if (element == UI_ELEMENT_STORE_SEARCH &&
            state.active_panel == UI_PANEL_STORE) {
        store_search_focused = true;
        *damage = rect_union(*damage, store_search_rect());
        return UI_STATUS_OK;
    }
    if (element >= UI_ELEMENT_STORE_NAV_0 &&
            element <= UI_ELEMENT_STORE_NAV_12 &&
            state.active_panel == UI_PANEL_STORE) {
        store_section = (uint8_t)(element - UI_ELEMENT_STORE_NAV_0);
        store_search_focused = false;
        *damage = rect_union(*damage, state.layout.panel_client);
        return UI_STATUS_OK;
    }
    if (element == UI_ELEMENT_FILES_UP) {
        files_up();
    } else if (element == UI_ELEMENT_FILES_NEW_FILE) {
        files_create(false);
    } else if (element == UI_ELEMENT_FILES_NEW_FOLDER) {
        files_create(true);
    } else if (element == UI_ELEMENT_FILES_REFRESH) {
        (void)files_refresh();
    } else if (element == UI_ELEMENT_FILES_SYNC) {
        set_app_status("sync", sapfs_sync(SAPFS_VOLUME_DATA));
    } else if (element == UI_ELEMENT_FILES_ROOT) {
        if (!copy_string(file_directory, sizeof(file_directory), ".")) {
            return UI_STATUS_FILESYSTEM_FAILURE;
        }
        (void)files_refresh();
    } else if (element >= UI_ELEMENT_FILES_ENTRY_0 &&
            element <= UI_ELEMENT_FILES_ENTRY_11) {
        const size_t index = (size_t)(element - UI_ELEMENT_FILES_ENTRY_0);
        char path[SAPFS_MAX_PATH + 1U];

        if (index >= file_entry_count ||
            !entry_path(file_entries[index].name, path)) {
            return UI_STATUS_BAD_ELEMENT;
        }
        if (file_entries[index].directory) {
            if (!copy_string(file_directory, sizeof(file_directory), path)) {
                return UI_STATUS_FILESYSTEM_FAILURE;
            }
            (void)files_refresh();
        } else {
            if (!copy_string(note_path, sizeof(note_path), path)) {
                return UI_STATUS_FILESYSTEM_FAILURE;
            }
            (void)note_load();
            return set_panel(UI_PANEL_NOTES, damage);
        }
    } else if (element == UI_ELEMENT_STUDIO_NEW) {
        studio_reset(true);
    } else if (element == UI_ELEMENT_STUDIO_IMPORT) {
        studio_import_clip();
    } else if (element == UI_ELEMENT_STUDIO_TRIM) {
        studio_trim_clip();
    } else if (element == UI_ELEMENT_STUDIO_SAVE) {
        (void)studio_save();
    } else if (element == UI_ELEMENT_STUDIO_EXPORT) {
        (void)studio_export();
    } else if (element == UI_ELEMENT_STUDIO_TIMELINE) {
        const struct ui_rect timeline = studio_timeline_rect();
        const uint32_t left = timeline.x + 38U;
        const uint32_t right = timeline.x + timeline.width - 10U;
        uint32_t pointer_x = state.pointer.x < 0 ? left :
            (uint32_t)state.pointer.x;

        if (pointer_x < left) {
            pointer_x = left;
        } else if (pointer_x > right) {
            pointer_x = right;
        }
        studio_playhead = (pointer_x - left) * 1000U / (right - left);
        studio_dirty = true;
        uint32_t clip_x = timeline.x + 42U;

        for (size_t index = 0U; index < studio_clip_count; ++index) {
            const uint32_t clip_width = 92U +
                studio_clip_durations[index] / 12U;

            if (pointer_x >= clip_x && pointer_x < clip_x + clip_width) {
                studio_selected_clip = (uint8_t)index;
                if (studio_load_preview(studio_clip_paths[index]) ==
                    SAPFS_STATUS_OK) {
                    studio_set_status("Clip selected / playhead moved");
                } else {
                    studio_set_status("Source offline / playhead moved");
                }
                break;
            }
            clip_x += clip_width + 5U;
        }
        if (studio_selected_clip == UINT8_MAX) {
            studio_set_status("Playhead moved");
        }
    } else if (element == UI_ELEMENT_SETTINGS_BACK) {
        settings_page = -1;
    } else if (element >= UI_ELEMENT_SETTINGS_CATEGORY_0 &&
            element <= UI_ELEMENT_SETTINGS_CATEGORY_11) {
        settings_page = (int8_t)(element - UI_ELEMENT_SETTINGS_CATEGORY_0);
    } else if (element == UI_ELEMENT_SETTINGS_APPEARANCE_LIGHT ||
            element == UI_ELEMENT_SETTINGS_APPEARANCE_DARK) {
        dock_dark = element == UI_ELEMENT_SETTINGS_APPEARANCE_DARK;
        *damage = rect_union(*damage, dock_visual_bounds(&state.layout));
    } else if (element >= UI_ELEMENT_SETTINGS_OPTION_0 &&
            element <= UI_ELEMENT_SETTINGS_OPTION_2) {
        const size_t option = (size_t)(
            element - UI_ELEMENT_SETTINGS_OPTION_0);
        if (settings_page == 2) {
            if (option == 0U) {
                dock_magnification = !dock_magnification;
                begin_dock_spring();
            } else if (option == 1U) {
                dock_reflections = !dock_reflections;
            } else {
                dock_labels = !dock_labels;
            }
            *damage = rect_union(*damage, dock_visual_bounds(&state.layout));
        } else if (settings_page == 3 && option < 2U) {
            if (option == 0U) {
                menu_glass = !menu_glass;
                *damage = rect_union(*damage, state.layout.menu_bar);
            } else {
                window_high_contrast = !window_high_contrast;
            }
        } else if (settings_page == 4 && option < 2U) {
            if (option == 0U) {
                keyboard_focus_wrap = !keyboard_focus_wrap;
            } else {
                keyboard_focus_indicator = !keyboard_focus_indicator;
                *damage = rect_union(*damage,
                    dock_visual_bounds(&state.layout));
            }
        } else if (settings_page == 5 && option < 2U) {
            const struct ui_rect old_cursor =
                cursor_damage_rect_for(state.pointer);
            if (option == 0U) {
                cursor_large = !cursor_large;
            } else {
                cursor_dark = !cursor_dark;
            }
            *damage = rect_union(*damage, old_cursor);
            *damage = rect_union(*damage,
                cursor_damage_rect_for(state.pointer));
        } else if (settings_page == 6 && option < 1U) {
            window_motion = !window_motion;
        } else if (settings_page == 10) {
            if (option == 0U) {
                window_shadows = !window_shadows;
            } else if (option == 1U) {
                window_bevels = !window_bevels;
            } else {
                window_title_gradient = !window_title_gradient;
            }
        } else {
            return UI_STATUS_BAD_ELEMENT;
        }
    } else if (element >= UI_ELEMENT_SETTINGS_WALLPAPER_0 &&
            element <= UI_ELEMENT_SETTINGS_WALLPAPER_13) {
        const uint8_t index = (uint8_t)(
            element - UI_ELEMENT_SETTINGS_WALLPAPER_0);
        if (!select_desktop_wallpaper(index)) {
            return UI_STATUS_WALLPAPER_FAILURE;
        }
        *damage = state.layout.surface;
    } else if (element == UI_ELEMENT_CAMERA_CAPTURE) {
        (void)camera_capture();
        return UI_STATUS_OK;
    } else {
        return UI_STATUS_BAD_ELEMENT;
    }
    *damage = rect_union(*damage, state.layout.panel);
    return UI_STATUS_OK;
}

static void note_input(char character, bool control)
{
    if (!note_savable) {
        set_app_status("note is read-only in this editor",
            SAPFS_STATUS_RANGE);
        return;
    }
    if (control && (character == 's' || character == 'S')) {
        (void)note_save();
        return;
    }
    if (character == '\b') {
        if (note_length != 0U) {
            --note_length;
            note_buffer[note_length] = '\0';
            note_dirty = true;
        }
        return;
    }
    if ((character == '\n' || (character >= ' ' && character <= '~')) &&
        note_length + 1U < sizeof(note_buffer)) {
        note_buffer[note_length++] = character;
        note_buffer[note_length] = '\0';
        note_dirty = true;
        set_app_status("editing in memory", SAPFS_STATUS_OK);
    } else if (note_length + 1U >= sizeof(note_buffer)) {
        set_app_status("note capacity reached", SAPFS_STATUS_RANGE);
    }
}

static void native_pointer_emit(
    enum ui_native_event_type type,
    struct ui_point point,
    struct ui_point previous,
    enum ui_pointer_button button,
    bool pressed
)
{
    uint32_t slot;

    if (!native_panel_slot(state.active_panel, &slot) ||
        !native_windows[slot].active ||
        (!native_windows[slot].pointer_capture &&
            !rect_contains_point(state.layout.panel_client, point))) {
        return;
    }
    const struct ui_native_window_record *native = &native_windows[slot];
    const int64_t local_x = ((int64_t)point.x -
        (int64_t)state.layout.panel_client.x) * native->width /
        state.layout.panel_client.width;
    const int64_t local_y = ((int64_t)point.y -
        (int64_t)state.layout.panel_client.y) * native->height /
        state.layout.panel_client.height;
    const int64_t previous_x = ((int64_t)previous.x -
        (int64_t)state.layout.panel_client.x) * native->width /
        state.layout.panel_client.width;
    const int64_t previous_y = ((int64_t)previous.y -
        (int64_t)state.layout.panel_client.y) * native->height /
        state.layout.panel_client.height;
    const struct ui_native_event event = {
        .type = type,
        .monotonic_ns = clock_monotonic_ns(),
        .x = (int32_t)local_x,
        .y = (int32_t)local_y,
        .delta_x = (int32_t)(local_x - previous_x),
        .delta_y = (int32_t)(local_y - previous_y),
        .code = (uint32_t)button,
        .value = pressed ? 1U : 0U
    };

    native_event_emit(state.active_panel, &event);
}

static enum ui_status apply_event(
    const struct ui_event *event,
    struct ui_rect *damage
)
{
    enum ui_element_id hit = UI_ELEMENT_NONE;

    if (event == NULL || damage == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (ui_animation_active() &&
            event->type != UI_EVENT_POINTER_MOVEMENT) {
        state.pressed = UI_ELEMENT_NONE;
        return UI_STATUS_OK;
    }
    if (event->type == UI_EVENT_POINTER_MOVEMENT) {
        const struct ui_rect old_cursor = cursor_damage_rect_for(state.pointer);
        const enum ui_element_id old_hover = state.hover;
        const struct ui_point old_pointer = state.pointer;

        state.pointer = event->point;
        dock3d_set_pointer(&dock_model, state.pointer.x, state.pointer.y,
            state.pointer_present, dock_magnification);
        dock_sync_layout();
        const struct ui_rect new_cursor = cursor_damage_rect_for(state.pointer);
        *damage = rect_union(*damage, rect_union(old_cursor, new_cursor));
        if (panel_drag_active) {
            state.hover = UI_ELEMENT_NONE;
            state.renders.cursor_moves += 1U;
            if (old_hover >= UI_ELEMENT_DOCK_FILES &&
                    old_hover <= UI_ELEMENT_DOCK_SETTINGS) {
                *damage = rect_union(*damage,
                    dock_visual_bounds(&state.layout));
                begin_dock_spring();
            }
            return drag_panel_to(state.pointer, damage);
        }
        const int dock_hit = dock3d_hit(&dock_model,
            state.pointer.x, state.pointer.y);
        hit = dock_hit >= 0 ?
            (enum ui_element_id)(UI_ELEMENT_DOCK_FILES + dock_hit) :
            UI_ELEMENT_NONE;
        state.hover = hit;
        state.renders.cursor_moves += 1U;
        if (dock_model.pointer_in ||
                (old_hover >= UI_ELEMENT_DOCK_FILES &&
                    old_hover <= UI_ELEMENT_DOCK_SETTINGS)) {
            *damage = rect_union(*damage,
                dock_visual_bounds(&state.layout));
            begin_dock_spring();
        }
        if (old_hover != hit) {
            /* A hover changes the enlarged icon, both neighbours, its label,
             * and the reflection.  Invalidate the complete visual envelope so
             * no cursor or magnification fragments can survive a fast move. */
            *damage = rect_union(*damage,
                dock_visual_bounds(&state.layout));
            begin_dock_spring();
            state.renders.dock_state_changes += 1U;
        }
        native_pointer_emit(UI_NATIVE_EVENT_POINTER_MOVE, state.pointer,
            old_pointer, UI_POINTER_BUTTON_NONE, false);
    } else if (event->type == UI_EVENT_POINTER_BUTTON_PRESS &&
        event->button == UI_POINTER_BUTTON_LEFT) {
        enum ui_element_id dock_hit = UI_ELEMENT_NONE;

        const int dock_index = dock3d_hit(&dock_model,
            event->point.x, event->point.y);
        dock_hit = dock_index >= 0 ?
            (enum ui_element_id)(UI_ELEMENT_DOCK_FILES + dock_index) :
            UI_ELEMENT_NONE;
        if (dock_hit == UI_ELEMENT_NONE && !launcher_open) {
            const enum ui_panel_id clicked = panel_at_point(event->point);

            if (clicked != UI_PANEL_NONE && clicked != state.active_panel) {
                const enum ui_status focus_status = set_panel(clicked,
                    damage);

                if (focus_status != UI_STATUS_OK) {
                    return focus_status;
                }
            }
        }
        hit = active_hit(event->point);
        if (hit == UI_ELEMENT_NONE && state.active_panel != UI_PANEL_NONE &&
                !ui_animation_active() &&
                !panel_maximized[state.active_panel] &&
                panel_title_contains(state.layout.panel, event->point)) {
            panel_drag_active = true;
            panel_drag_panel = state.active_panel;
            panel_drag_anchor = event->point;
            panel_drag_origin = panel_windows[state.active_panel];
        }
        state.pressed = hit;
        native_pointer_emit(UI_NATIVE_EVENT_POINTER_BUTTON, event->point,
            event->point, event->button, true);
        if (hit >= UI_ELEMENT_DOCK_FILES &&
                hit <= UI_ELEMENT_DOCK_SETTINGS) {
            *damage = rect_union(*damage,
                dock_bounds_for(&state.layout, hit));
        }
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_POINTER_BUTTON_RELEASE &&
        event->button == UI_POINTER_BUTTON_LEFT) {
        const enum ui_element_id pressed = state.pressed;

        if (panel_drag_active) {
            panel_drag_active = false;
            panel_drag_panel = UI_PANEL_NONE;
            state.pressed = UI_ELEMENT_NONE;
            state.renders.dock_state_changes += 1U;
            return UI_STATUS_OK;
        }
        hit = active_hit(event->point);
        state.pressed = UI_ELEMENT_NONE;
        native_pointer_emit(UI_NATIVE_EVENT_POINTER_BUTTON, event->point,
            event->point, event->button, false);
        if (pressed >= UI_ELEMENT_DOCK_FILES &&
                pressed <= UI_ELEMENT_DOCK_SETTINGS) {
            *damage = rect_union(*damage,
                dock_bounds_for(&state.layout, pressed));
        }
        if (pressed != UI_ELEMENT_NONE && pressed == hit) {
            const enum ui_status status = activate_element(pressed, damage);

            if (status != UI_STATUS_OK) {
                return status;
            }
        }
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_KEYBOARD_FOCUS_NEXT ||
        event->type == UI_EVENT_KEYBOARD_FOCUS_PREVIOUS) {
        const enum ui_element_id old_focus = state.focus;

        state.focus = next_focus(state.focus,
            event->type == UI_EVENT_KEYBOARD_FOCUS_PREVIOUS);
        *damage = rect_union(*damage,
            dock_bounds_for(&state.layout, old_focus));
        *damage = rect_union(*damage,
            dock_bounds_for(&state.layout, state.focus));
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_KEYBOARD_ACTIVATION) {
        if (launcher_open) {
            size_t dock_index;
            const size_t visible = launcher_page *
                UI_LAUNCHER_APPS_PER_PAGE;

            if (launcher_dock_index_at(visible, &dock_index)) {
                return activate_element((enum ui_element_id)(
                    UI_ELEMENT_LAUNCHER_APP_0 + dock_index), damage);
            }
            return UI_STATUS_OK;
        }
        return set_panel(panel_for_element(state.focus), damage);
    } else if (event->type == UI_EVENT_PANEL_CLOSE) {
        if (launcher_open) {
            launcher_open = false;
            launcher_search_focused = false;
            *damage = state.layout.surface;
            return UI_STATUS_OK;
        }
        if (state.active_panel != UI_PANEL_NONE) {
            return set_panel(UI_PANEL_NONE, damage);
        }
    } else if (event->type == UI_EVENT_TEXT_INPUT && launcher_open &&
            launcher_search_focused) {
        if (event->character == '\b') {
            if (launcher_query_length != 0U) {
                --launcher_query_length;
                launcher_query[launcher_query_length] = '\0';
            }
        } else if (event->character >= ' ' && event->character <= '~' &&
                launcher_query_length + 1U < sizeof(launcher_query)) {
            launcher_query[launcher_query_length++] = event->character;
            launcher_query[launcher_query_length] = '\0';
        }
        launcher_page = 0U;
        *damage = rect_union(*damage, launcher_bounds());
    } else if (event->type == UI_EVENT_TEXT_INPUT &&
            state.active_panel == UI_PANEL_STORE && store_search_focused) {
        if (event->character == '\b') {
            if (store_query_length != 0U) {
                --store_query_length;
                store_query[store_query_length] = '\0';
            }
        } else if (event->character >= ' ' && event->character <= '~' &&
                store_query_length + 1U < sizeof(store_query)) {
            store_query[store_query_length++] = event->character;
            store_query[store_query_length] = '\0';
        }
        *damage = rect_union(*damage, state.layout.panel_client);
    } else if (event->type == UI_EVENT_TEXT_INPUT &&
        state.active_panel == UI_PANEL_NOTES) {
        note_input(event->character, event->control);
        *damage = rect_union(*damage, state.layout.panel);
    } else if (event->type == UI_EVENT_TEXT_INPUT &&
        state.active_panel == UI_PANEL_STUDIO && event->control &&
        (event->character == 's' || event->character == 'S')) {
        (void)studio_save();
        *damage = rect_union(*damage, state.layout.panel);
    } else if (event->type == UI_EVENT_REDRAW_REQUEST) {
        *damage = state.layout.surface;
    } else if (event->type != UI_EVENT_POINTER_BUTTON_PRESS &&
        event->type != UI_EVENT_POINTER_BUTTON_RELEASE) {
        return UI_STATUS_BAD_EVENT;
    }
    return UI_STATUS_OK;
}

enum ui_status ui_process_events(void)
{
    struct ui_event event;
    struct ui_rect damage = { 0U, 0U, 0U, 0U };

    if (!state.active) {
        return UI_STATUS_NOT_ACTIVE;
    }
    while (pop_event(&event)) {
        const enum ui_status status = apply_event(&event, &damage);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    /* Pointer input does not advance the genie warp.  Keep ordinary pointer
     * and Dock damage local; ui_flush() owns time-based animation frames. */
    if (damage.width == 0U || damage.height == 0U) {
        return UI_STATUS_OK;
    }
    return render_region(damage,
        damage.x == 0U && damage.y == 0U &&
        damage.width == state.layout.surface.width &&
        damage.height == state.layout.surface.height);
}

enum ui_status ui_flush(void)
{
    struct ui_rect damage = { 0U, 0U, 0U, 0U };
    const struct boot_ledger *ledger;

    if (!state.active) {
        return UI_STATUS_NOT_ACTIVE;
    }
    if (ui_anim_running(&panel_anim)) {
        const struct ui_rect bounds = ui_anim_bounds(&panel_anim);

        (void)ui_anim_advance(&panel_anim, clock_monotonic_ns());
        if (!ui_anim_running(&panel_anim)) {
            ui_anim_end(&panel_anim);
            panel_anim_panel = UI_PANEL_NONE;
            if (!dock_spring_active && motion_timer_id != 0U) {
                (void)timer_cancel((uint64_t)motion_timer_id);
                motion_timer_id = 0U;
            }
        }
        damage = rect_union(damage, bounds);
    }
    if (dock_spring_active) {
        const uint64_t now = clock_monotonic_ns();

        if (now - dock_spring_last_ns >= UI_SPRING_FRAME_NS) {
            uint64_t frames = (now - dock_spring_last_ns) /
                UI_SPRING_FRAME_NS;
            const struct ui_rect old_dock =
                dock_visual_bounds(&state.layout);

            if (frames > 8U) {
                frames = 8U;
            }
            dock3d_advance(&dock_model, (uint32_t)frames,
                dock_magnification);
            dock_sync_layout();
            dock_spring_last_ns = now;
            if (!dock3d_animating(&dock_model)) {
                dock_spring_active = false;
                if (!ui_anim_running(&panel_anim) && motion_timer_id != 0U) {
                    (void)timer_cancel((uint64_t)motion_timer_id);
                    motion_timer_id = 0U;
                }
            }
            damage = rect_union(damage, rect_union(old_dock,
                dock_visual_bounds(&state.layout)));
        }
    }
    if (panel_open[UI_PANEL_CAMERA] &&
            camera_get_info().generation != camera_seen_generation) {
        const enum ui_panel_id focused = state.active_panel;

        install_panel_geometry(UI_PANEL_CAMERA);
        damage = rect_union(damage, camera_preview_rect());
        install_panel_geometry(focused);
    }
    ledger = boot_ledger_installed();
    const bool pass = ledger != NULL && ledger->executed && !ledger->degraded &&
        boot_ledger_fingerprint_valid(ledger);
    if (state.ledger_pass != pass) {
        state.ledger_pass = pass;
    }
    if (damage.width != 0U && damage.height != 0U) {
        return render_region(damage, false);
    }
    if (canvas->damage.pending) {
        const struct surface_rect pending = canvas->damage.rectangle;
        const struct ui_rect pending_ui = {
            pending.x, pending.y, pending.width, pending.height
        };
        const enum ui_status status = draw_cursor(pending_ui);

        if (status != UI_STATUS_OK ||
            surface_present(canvas) != SURFACE_STATUS_OK) {
            return UI_STATUS_SURFACE_FAILURE;
        }
        state.renders.damaged_draws += 1U;
        state.renders.damage_rectangles += 1U;
        state.renders.pixels_copied += canvas->last_present_pixels;
    }
    return UI_STATUS_OK;
}

static struct ui_rect native_window_geometry(
    uint32_t slot,
    uint32_t width,
    uint32_t height
)
{
    const uint32_t outer_width = width + 20U;
    const uint32_t outer_height = height + 48U;
    const uint32_t column = slot % 2U;
    const uint32_t row = slot / 2U;
    const uint64_t paired_width = (uint64_t)outer_width * 2U + 24U;
    const uint64_t paired_height = (uint64_t)outer_height * 2U + 24U;
    const uint32_t horizontal_room = state.layout.surface.width > 16U ?
        state.layout.surface.width - 16U : 0U;
    const uint32_t vertical_room = state.layout.dock.y > panel_home.y ?
        state.layout.dock.y - panel_home.y : 0U;
    struct ui_rect result = {
        panel_home.x, panel_home.y, outer_width, outer_height
    };

    if (paired_width <= horizontal_room) {
        result.x = (state.layout.surface.width - (uint32_t)paired_width) / 2U +
            column * (outer_width + 24U);
    } else if (column != 0U) {
        result.x += 36U;
    }
    if (row != 0U) {
        if (paired_height <= vertical_room) {
            result.y += outer_height + 24U;
        } else {
            result.y += row * 36U;
        }
    }
    if (result.x + result.width > state.layout.surface.width) {
        result.x = state.layout.surface.width - result.width;
    }
    if (result.y + result.height > state.layout.surface.height) {
        result.y = state.layout.surface.height - result.height;
    }
    return result;
}

enum ui_status ui_native_window_open(
    uint32_t slot,
    const char *title,
    const uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    ui_native_event_fn event_handler,
    void *context
)
{
    struct ui_native_window_record *native;
    struct ui_rect damage = { 0U, 0U, 0U, 0U };
    enum ui_panel_id panel;
    size_t title_length = 0U;

    if (title == NULL || pixels == NULL || event_handler == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (!state.active) {
        return UI_STATUS_NOT_ACTIVE;
    }
    if (slot >= UI_NATIVE_WINDOW_COUNT) {
        return UI_STATUS_BAD_PANEL;
    }
    native = &native_windows[slot];
    if (native->active) {
        return UI_STATUS_ALREADY_INITIALIZED;
    }
    while (title_length < UI_NATIVE_TITLE_BYTES &&
        title[title_length] != '\0') {
        ++title_length;
    }
    if (title_length == 0U || title_length >= UI_NATIVE_TITLE_BYTES ||
        width < 64U || height < 64U ||
        width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL ||
        stride_bytes < width * SURFACE_BYTES_PER_PIXEL ||
        stride_bytes % SURFACE_BYTES_PER_PIXEL != 0U ||
        width + 20U > state.layout.surface.width ||
        height + 48U > state.layout.surface.height - UI_MENU_HEIGHT) {
        return UI_STATUS_UNSUPPORTED_GEOMETRY;
    }
    for (size_t index = 0U; index < title_length; ++index) {
        native->title[index] = title[index];
    }
    native->title[title_length] = '\0';
    native->pixels = pixels;
    native->width = width;
    native->height = height;
    native->stride_bytes = stride_bytes;
    native->event_handler = event_handler;
    native->context = context;
    native->active = true;
    panel = (enum ui_panel_id)(UI_PANEL_NATIVE_0 + slot);
    panel_windows[panel] = native_window_geometry(slot, width, height);
    panel_restore[panel] = panel_windows[panel];
    panel_minimized[panel] = false;
    panel_maximized[panel] = false;
    panel_open[panel] = false;
    if (set_panel(panel, &damage) != UI_STATUS_OK) {
        panel_open[panel] = false;
        *native = (struct ui_native_window_record){ 0 };
        return UI_STATUS_BAD_PANEL;
    }
    return render_region(damage, true);
}

enum ui_status ui_native_window_close(uint32_t slot)
{
    struct ui_rect damage;
    enum ui_panel_id panel;

    if (slot >= UI_NATIVE_WINDOW_COUNT) {
        return UI_STATUS_BAD_PANEL;
    }
    if (!native_windows[slot].active) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    panel = (enum ui_panel_id)(UI_PANEL_NATIVE_0 + slot);
    damage = drop_shadow_draw_rect(panel_windows[panel], 6U);
    ui_anim_end(&panel_anim);
    panel_anim_pending = UI_ANIM_PENDING_NONE;
    panel_anim_panel = UI_PANEL_NONE;
    if (state.active_panel == panel && panel_anim_driver && window_motion) {
        panel_anim_panel = panel;
        panel_anim_frame = panel_windows[panel];
        panel_anim_origin = origin_for_panel(panel, false);
        panel_anim_pending = UI_ANIM_PENDING_CLOSE;
    }
    if (state.active_panel == panel) {
        native_focus_emit(panel, false);
    }
    panel_open[panel] = false;
    panel_minimized[panel] = false;
    panel_maximized[panel] = false;
    panel_origin_valid[panel] = false;
    remove_panel_from_order(panel);
    native_windows[slot] = (struct ui_native_window_record){ 0 };
    if (state.active_panel == panel) {
        state.active_panel = front_panel();
        install_panel_geometry(state.active_panel);
        native_focus_emit(state.active_panel, true);
        if (state.active_panel != UI_PANEL_NONE) {
            damage = rect_union(damage,
                drop_shadow_draw_rect(panel_windows[state.active_panel], 6U));
        }
    }
    state.renders.panel_transitions += 1U;
    return render_region(damage, false);
}

enum ui_status ui_native_window_damage(
    uint32_t slot,
    const struct ui_rect *rectangles,
    size_t rectangle_count
)
{
    enum ui_panel_id panel;
    struct ui_native_window_record *native;

    if (rectangles == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (slot >= UI_NATIVE_WINDOW_COUNT || rectangle_count == 0U ||
        rectangle_count > 8U) {
        return UI_STATUS_RECTANGLE_OVERFLOW;
    }
    native = &native_windows[slot];
    if (!native->active) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < rectangle_count; ++index) {
        const struct ui_rect rectangle = rectangles[index];

        if (rectangle.width == 0U || rectangle.height == 0U ||
            rectangle.x >= native->width || rectangle.y >= native->height ||
            rectangle.width > native->width - rectangle.x ||
            rectangle.height > native->height - rectangle.y) {
            return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
        }
    }
    panel = (enum ui_panel_id)(UI_PANEL_NATIVE_0 + slot);
    if (!panel_open[panel]) {
        return UI_STATUS_OK;
    }
    const struct ui_rect client = {
        panel_windows[panel].x + 10U, panel_windows[panel].y + 38U,
        panel_windows[panel].width - 20U,
        panel_windows[panel].height - 48U
    };
    for (size_t index = 0U; index < rectangle_count; ++index) {
        const struct ui_rect source = rectangles[index];
        struct ui_rect damage;

        if (client.width == native->width && client.height == native->height) {
            damage = (struct ui_rect){
                client.x + source.x, client.y + source.y,
                source.width, source.height
            };
        } else {
            const uint64_t left = (uint64_t)source.x *
                client.width / native->width;
            const uint64_t top = (uint64_t)source.y *
                client.height / native->height;
            const uint64_t right = ((uint64_t)(source.x + source.width) *
                client.width + native->width - 1U) /
                native->width;
            const uint64_t bottom = ((uint64_t)(source.y + source.height) *
                client.height + native->height - 1U) /
                native->height;

            damage = (struct ui_rect){
                client.x + (uint32_t)left, client.y + (uint32_t)top,
                (uint32_t)(right - left), (uint32_t)(bottom - top)
            };
        }
        const enum ui_status status = render_region(damage, false);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

enum ui_status ui_native_pointer_capture(uint32_t slot, bool capture)
{
    if (slot >= UI_NATIVE_WINDOW_COUNT) {
        return UI_STATUS_BAD_PANEL;
    }
    if (!native_windows[slot].active) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    if (capture && state.active_panel !=
            (enum ui_panel_id)(UI_PANEL_NATIVE_0 + slot)) {
        return UI_STATUS_NOT_ACTIVE;
    }
    native_windows[slot].pointer_capture = capture;
    return UI_STATUS_OK;
}

bool ui_native_window_is_open(uint32_t slot)
{
    return slot < UI_NATIVE_WINDOW_COUNT && native_windows[slot].active;
}

bool ui_application_launch_dequeue(char *manifest_path, size_t capacity)
{
    size_t length = 0U;
    const bool enabled = cpu_interrupts_enabled();

    if (manifest_path == NULL || capacity == 0U) {
        return false;
    }
    cpu_interrupt_disable();
    while (length < sizeof(application_launch_path) &&
            application_launch_path[length] != '\0') {
        ++length;
    }
    if (length == 0U || length == sizeof(application_launch_path) ||
            length + 1U > capacity) {
        if (enabled) {
            cpu_interrupt_enable();
        }
        return false;
    }
    for (size_t index = 0U; index <= length; ++index) {
        manifest_path[index] = application_launch_path[index];
    }
    application_launch_path[0] = '\0';
    if (enabled) {
        cpu_interrupt_enable();
    }
    return true;
}

static uint64_t synthetic_render_hash(bool active)
{
    uint32_t pixels[64U * 32U];
    const uint32_t ink = UINT32_C(0x00101012);
    const uint32_t desktop_dark = UINT32_C(0x00595976);
    const uint32_t desktop_light = UINT32_C(0x00666684);
    const uint32_t title_active = UINT32_C(0x0018181C);
    const uint32_t accent_gold = UINT32_C(0x00C4A44E);
    const uint32_t window_face = UINT32_C(0x00D7D6CE);

    for (size_t index = 0U; index < sizeof(pixels) / sizeof(pixels[0]); ++index) {
        pixels[index] = desktop_dark;
    }
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 64U; ++x) {
            pixels[y * 64U + x] = title_active;
        }
    }
    for (uint32_t y = 8U; y < 32U; y += 2U) {
        for (uint32_t x = 0U; x < 64U; ++x) {
            pixels[y * 64U + x] = desktop_light;
        }
    }
    for (uint32_t y = 18U; y < 30U; ++y) {
        for (uint32_t x = 8U; x < 56U; ++x) {
            const bool edge = y == 18U || y == 29U || x == 8U || x == 55U;
            pixels[y * 64U + x] = edge ? ink : window_face;
        }
    }
    for (uint32_t y = 22U; y < 28U; ++y) {
        for (uint32_t x = 12U; x < 22U; ++x) {
            pixels[y * 64U + x] = active ? title_active : accent_gold;
        }
    }
    for (uint32_t y = 0U; y < UI_CURSOR_HEIGHT && y + 4U < 32U; ++y) {
        for (uint32_t x = 0U; x < UI_CURSOR_WIDTH && x + 44U < 64U; ++x) {
            if (cursor_mask_contains(cursor_outer, x, y,
                    UI_CURSOR_WIDTH, UI_CURSOR_HEIGHT)) {
                pixels[(y + 4U) * 64U + x + 44U] = ink;
            }
        }
    }

    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (size_t index = 0U; index < sizeof(pixels) / sizeof(pixels[0]); ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(0x100000001B3);
    }
    return hash;
}

static bool event_queue_self_test(void)
{
    const struct ui_event_counters saved = state.events;
    struct ui_event saved_queue[UI_EVENT_QUEUE_CAPACITY];
    const size_t saved_count = event_count;

    event_queue_failure = "UI event queue self-test passed";
    for (size_t index = 0U; index < saved_count; ++index) {
        saved_queue[index] = event_queue[index];
    }
    state.events = (struct ui_event_counters){ 0 };
    event_count = 0U;

    struct ui_event move = {
        .type = UI_EVENT_POINTER_MOVEMENT,
        .point = { 1, 1 },
        .button = UI_POINTER_BUTTON_NONE
    };
    if (publish_unlocked(&move) != UI_STATUS_OK) {
        event_queue_failure = "pointer movement event was refused by an empty queue";
        return false;
    }
    move.point.x = 2;
    if (publish_unlocked(&move) != UI_STATUS_OK || event_count != 1U ||
        state.events.coalesced != 1U) {
        event_queue_failure = "pointer movement did not coalesce in the fixed event queue";
        return false;
    }
    const struct ui_event activate = {
        .type = UI_EVENT_KEYBOARD_ACTIVATION,
        .point = { 0, 0 },
        .button = UI_POINTER_BUTTON_NONE
    };
    while (event_count < UI_EVENT_QUEUE_CAPACITY) {
        if (publish_unlocked(&activate) != UI_STATUS_OK) {
            event_queue_failure = "keyboard activation did not fill the event queue exactly";
            return false;
        }
    }
    const struct ui_event button = {
        .type = UI_EVENT_POINTER_BUTTON_PRESS,
        .point = { 2, 2 },
        .button = UI_POINTER_BUTTON_LEFT
    };
    if (publish_unlocked(&button) != UI_STATUS_OK ||
        event_queue[event_count - 1U].type != UI_EVENT_POINTER_BUTTON_PRESS ||
        state.events.dropped != 1U) {
        event_queue_failure = "button transition did not evict movement from the full event queue";
        return false;
    }

    event_count = saved_count;
    for (size_t index = 0U; index < saved_count; ++index) {
        event_queue[index] = saved_queue[index];
    }
    state.events = saved;
    return true;
}

bool ui_self_test(void)
{
    struct ui_layout layout;
    enum ui_element_id hit;
    enum ui_status status;

    self_test_failure = "Sapote Redwood UI self-test passed";
    if (!ui_font_self_test()) {
        self_test_failure = "UI font suite rejected its valid fixture";
        return false;
    }
    status = ui_layout_build(800U, 600U, &layout);
    if (status == UI_STATUS_OK) {
        status = ui_layout_build(1024U, 768U, &layout);
    }
    if (status == UI_STATUS_OK) {
        status = ui_layout_build(1280U, 720U, &layout);
    }
    if (status != UI_STATUS_OK) {
        self_test_failure = ui_status_string(status);
        return false;
    }
    if (ui_layout_build(799U, 600U, &layout) !=
            UI_STATUS_UNSUPPORTED_GEOMETRY) {
        self_test_failure = "UI layout geometry acceptance is inconsistent";
        return false;
    }
    if (ui_layout_build(1280U, 720U, &layout) != UI_STATUS_OK) {
        self_test_failure = "UI layout valid fixture could not be restored";
        return false;
    }

    struct ui_layout damaged = layout;
    damaged.dock_items[1].id = damaged.dock_items[0].id;
    if (ui_layout_validate(&damaged) != UI_STATUS_DUPLICATE_ELEMENT_ID) {
        self_test_failure = "duplicate dock element ID was accepted";
        return false;
    }
    damaged = layout;
    damaged.dock_items[1].bounds = damaged.dock_items[0].bounds;
    if (ui_layout_validate(&damaged) != UI_STATUS_DOCK_OVERLAP) {
        self_test_failure = "overlapping dock elements were accepted";
        return false;
    }
    damaged = layout;
    damaged.panel.x = UINT32_MAX - 3U;
    damaged.panel.width = 8U;
    if (ui_layout_validate(&damaged) != UI_STATUS_RECTANGLE_OVERFLOW) {
        self_test_failure = "overflowing UI rectangle was accepted";
        return false;
    }

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct ui_rect bounds = layout.dock_items[index].bounds;
        const struct ui_point inside = {
            (int32_t)bounds.x, (int32_t)bounds.y
        };
        const struct ui_point right_edge = {
            (int32_t)(bounds.x + bounds.width), (int32_t)bounds.y
        };
        const struct ui_point bottom_edge = {
            (int32_t)bounds.x, (int32_t)(bounds.y + bounds.height)
        };

        if (ui_hit_test(&layout, inside, &hit) != UI_STATUS_OK ||
            hit != layout.dock_items[index].id ||
            ui_hit_test(&layout, right_edge, &hit) != UI_STATUS_OK ||
            hit == layout.dock_items[index].id ||
            ui_hit_test(&layout, bottom_edge, &hit) != UI_STATUS_OK ||
            hit == layout.dock_items[index].id) {
            self_test_failure = "half-open dock hit-test edge is inconsistent";
            return false;
        }
    }

    const struct ui_rect first = { 0U, 0U, UI_CURSOR_WIDTH, UI_CURSOR_HEIGHT };
    const struct ui_rect last = { 100U, 100U, UI_CURSOR_WIDTH, UI_CURSOR_HEIGHT };
    const struct ui_rect both = rect_union(first, last);
    if (both.x != 0U || both.y != 0U || both.width != 118U ||
        both.height != 125U || UI_CURSOR_HOTSPOT_X >= UI_CURSOR_WIDTH ||
        UI_CURSOR_HOTSPOT_Y >= UI_CURSOR_HEIGHT) {
        self_test_failure = "cursor damage union or hotspot is invalid";
        return false;
    }
    if (next_focus(UI_ELEMENT_DOCK_FILES, true) !=
            UI_ELEMENT_DOCK_SETTINGS ||
        next_focus(UI_ELEMENT_DOCK_SETTINGS, false) !=
            UI_ELEMENT_DOCK_FILES) {
        self_test_failure = "keyboard focus wrap is invalid";
        return false;
    }
    if (!ui_anim_self_test()) {
        self_test_failure = ui_anim_self_test_failure();
        return false;
    }
    if (!dock3d_self_test()) {
        self_test_failure = dock3d_self_test_failure();
        return false;
    }
    if (!camera_self_test()) {
        self_test_failure = "bounded camera frame broker is invalid";
        return false;
    }
    if (!event_queue_self_test()) {
        self_test_failure = event_queue_failure;
        return false;
    }

    const uint64_t stable = synthetic_render_hash(false);
    if (stable != UINT64_C(0xCD65C2C6A1DC2975) ||
        stable == synthetic_render_hash(true)) {
        self_test_failure = "synthetic Sapote Redwood render hash is invalid";
        return false;
    }
    return true;
}

const char *ui_self_test_failure(void)
{
    return self_test_failure;
}

enum ui_status ui_verify_installed(struct ui_proof *proof)
{
    static const enum ui_element_id ids[UI_DOCK_ITEM_COUNT] = {
        UI_ELEMENT_DOCK_FILES, UI_ELEMENT_DOCK_TERMINAL,
        UI_ELEMENT_DOCK_NOTES, UI_ELEMENT_DOCK_STUDIO,
        UI_ELEMENT_DOCK_CAMERA, UI_ELEMENT_DOCK_CANVAS,
        UI_ELEMENT_DOCK_STORE,
        UI_ELEMENT_DOCK_SETTINGS
    };
    static const enum ui_action actions[UI_DOCK_ITEM_COUNT] = {
        UI_ACTION_OPEN_FILES, UI_ACTION_OPEN_TERMINAL,
        UI_ACTION_OPEN_NOTES, UI_ACTION_OPEN_STUDIO,
        UI_ACTION_OPEN_CAMERA, UI_ACTION_OPEN_CANVAS,
        UI_ACTION_OPEN_STORE,
        UI_ACTION_OPEN_SETTINGS
    };
    static const enum ui_panel_id panels[UI_DOCK_ITEM_COUNT] = {
        UI_PANEL_FILES, UI_PANEL_TERMINAL, UI_PANEL_NOTES, UI_PANEL_STUDIO,
        UI_PANEL_CAMERA, UI_PANEL_NONE, UI_PANEL_STORE, UI_PANEL_SETTINGS
    };

    if (proof == NULL) {
        installed_proof_failure =
            "Sapote Redwood installed proof received no result buffer";
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (!state.active || canvas == NULL || !ui_font_is_verified() ||
        state.focus <= UI_ELEMENT_NONE || state.focus >= UI_ELEMENT_COUNT ||
        state.hover >= UI_ELEMENT_COUNT || state.pressed >= UI_ELEMENT_COUNT ||
        state.active_panel >= UI_PANEL_COUNT) {
        installed_proof_failure =
            "Sapote Redwood installed UI state is invalid";
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    if (ui_layout_validate(&state.layout) != UI_STATUS_OK) {
        installed_proof_failure =
            "Sapote Redwood live Dock layout is invalid";
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    if (state.pointer_present) {
        const struct pointer_state pointer = pointer_get_state();

        if (!pointer.present || pointer.bound_width != canvas->width ||
            pointer.bound_height != canvas->height || state.pointer.x < 0 ||
            state.pointer.y < 0 || (uint32_t)state.pointer.x >= canvas->width ||
            (uint32_t)state.pointer.y >= canvas->height) {
            installed_proof_failure =
                "Sapote Redwood pointer installation is invalid";
            return UI_STATUS_INSTALLED_PROOF_FAILURE;
        }
    }
    if (state.theme.white != framebuffer_pack(0xF8U, 0xFAU, 0xF8U) ||
        state.theme.ink != framebuffer_pack(0x18U, 0x21U, 0x24U) ||
        state.theme.desktop_dark != framebuffer_pack(0x07U, 0x16U, 0x22U) ||
        state.theme.desktop_light != framebuffer_pack(0x1CU, 0x4BU, 0x5AU) ||
        state.theme.title_active != framebuffer_pack(0x1CU, 0x29U, 0x2DU) ||
        state.theme.title_inactive != framebuffer_pack(0x91U, 0x9DU, 0xA2U) ||
        state.theme.accent_teal != framebuffer_pack(0x68U, 0xA9U, 0xC5U) ||
        state.theme.accent_gold != framebuffer_pack(0xE6U, 0xC4U, 0x62U) ||
        state.theme.accent_green != framebuffer_pack(0x8EU, 0xADU, 0x89U) ||
        state.theme.accent_red != framebuffer_pack(0xD9U, 0x55U, 0x4FU) ||
        state.theme.accent_violet != framebuffer_pack(0x94U, 0x7BU, 0xB4U) ||
        state.theme.shadow != framebuffer_pack(0x05U, 0x0CU, 0x12U) ||
        state.theme.window_face != framebuffer_pack(0xD9U, 0xDFU, 0xE0U)) {
        installed_proof_failure =
            "Sapote Redwood theme installation is invalid";
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (state.layout.dock_items[index].id != ids[index] ||
            state.layout.dock_items[index].action != actions[index] ||
            state.layout.dock_items[index].panel != panels[index]) {
            installed_proof_failure =
                "Sapote Redwood Dock action metadata is invalid";
            return UI_STATUS_INSTALLED_PROOF_FAILURE;
        }
    }

    const uint64_t first_hash = surface_hash();
    snapshot_redraw_tiles();
    if (render_region(state.layout.surface, true) != UI_STATUS_OK) {
        installed_proof_failure =
            "Sapote Redwood full redraw failed";
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    const uint64_t second_hash = surface_hash();
    if (first_hash != second_hash) {
        report_redraw_tile_mismatch();
        installed_proof_failure =
            "Sapote Redwood full redraw is not idempotent";
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    state.stable_render_hash = second_hash;

    const struct boot_ledger *ledger = boot_ledger_installed();
    *proof = (struct ui_proof){
        .width = canvas->width,
        .height = canvas->height,
        .dock_items = UI_DOCK_ITEM_COUNT,
        .events = state.events.drained,
        .panels = state.renders.panel_transitions,
        .cursor_moves = state.renders.cursor_moves,
        .damage_rectangles = state.renders.damage_rectangles,
        .glyphs = state.renders.glyphs,
        .ledger_fingerprint = ledger == NULL ? 0U : ledger->fingerprint,
        .render_hash = second_hash
    };
    installed_proof_failure = "Sapote Redwood installed proof passed";
    return UI_STATUS_OK;
}

const char *ui_installed_proof_failure(void)
{
    return installed_proof_failure;
}

const char *ui_panel_name(enum ui_panel_id panel)
{
    static const char *const names[] = {
        "None", "Files", "Terminal", "Notes", "SapStudio",
        "Camera", "Store", "Settings"
    };
    uint32_t slot;

    if (native_panel_slot(panel, &slot)) {
        return native_windows[slot].active ? native_windows[slot].title :
            "Native application";
    }

    if ((size_t)panel >= sizeof(names) / sizeof(names[0])) {
        return "Unknown panel";
    }
    return names[panel];
}

const char *ui_element_name(enum ui_element_id element)
{
    if (element == UI_ELEMENT_NONE) {
        return "none";
    }
    if (element == UI_ELEMENT_DOCK_FILES) {
        return "Files";
    }
    if (element == UI_ELEMENT_DOCK_TERMINAL) {
        return "Terminal";
    }
    if (element == UI_ELEMENT_DOCK_NOTES) {
        return "Notes";
    }
    if (element == UI_ELEMENT_DOCK_STUDIO) {
        return "SapStudio";
    }
    if (element == UI_ELEMENT_DOCK_CAMERA) {
        return "Camera";
    }
    if (element == UI_ELEMENT_DOCK_CANVAS) {
        return "Canvas";
    }
    if (element == UI_ELEMENT_DOCK_STORE) {
        return "Store";
    }
    if (element == UI_ELEMENT_DOCK_SETTINGS) {
        return "Settings";
    }
    return element < UI_ELEMENT_COUNT ? "application control" :
        "unknown element";
}

const char *ui_status_string(enum ui_status status)
{
    static const char *const messages[] = {
        "ok",
        "null UI argument",
        "Sapote Redwood is already initialized",
        "Sapote Redwood is not initialized",
        "Sapote Redwood is not active",
        "unsupported Sapote Redwood framebuffer geometry",
        "UI rectangle arithmetic overflowed",
        "UI rectangle lies outside its surface",
        "duplicate UI element identifier",
        "dock item rectangles overlap",
        "panel client rectangle is empty",
        "UI text baseline lies outside its box",
        "software cursor hotspot is invalid",
        "UI hit test selected more than one element",
        "UI event queue is full",
        "UI event type is invalid",
        "UI element is invalid",
        "UI panel is invalid",
        "UI font rendering failed",
        "cached surface rendering failed",
        "canonical logo rendering failed",
        "SapStudio icon rendering failed",
        "application icon rendering failed",
        "desktop wallpaper rendering failed",
        "application filesystem operation failed",
        "terminal viewport rendering failed",
        "installed Sapote Redwood proof failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        (size_t)UI_STATUS_INSTALLED_PROOF_FAILURE + 1U,
        "UI status messages are out of sync");
    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown UI status";
    }
    return messages[status];
}
