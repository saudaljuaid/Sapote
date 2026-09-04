/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Paint.  See include/sapote/paint.h for the shape of the ribbon and for
 * what Phipia does differently.
 */

#include <sapote/paint.h>

#include <sapote/cursor.h>

#include <sapote/framebuffer.h>
#include <sapote/ui_font.h>

#include "paint_glyphs.h"

/* ================================================================ METRICS
 *
 * PENDING VERIFICATION: read off a Windows 10 Paint window at 100% scaling.
 * The GROUPS and their order are Paint's; the pixel sizes are this file's
 * reading of them.
 */

#define PAINT_BORDER 1U
#define PAINT_CAPTION 30U
#define PAINT_CAPTION_BUTTON 46U
#define PAINT_QAT_SLOT 22U
#define PAINT_TABS 26U
#define PAINT_TAB_PAD 14U
#define PAINT_RIBBON 100U
#define PAINT_RIBBON_LABEL 15U
#define PAINT_STATUS 26U
#define PAINT_PAD 6U
#define PAINT_GROUP_GAP 7U

/* A tall ribbon button: a mark over one or two lines of label. */
#define PAINT_TALL_WIDTH 54U
#define PAINT_TALL_ICON 24U
/* A small one, in a stacked column. */
#define PAINT_SMALL_HEIGHT 21U
#define PAINT_SMALL_ICON 16U
/*
 * The smallest box a Lucide mark may be asked for.  The set is rasterized
 * at sixteen, twenty, twenty-four and thirty-two; asking for twelve does
 * not produce a twelve-pixel mark, it produces the sixteen drawn outside
 * the box, because nothing here resamples a glyph to fit one.
 */
#define PAINT_MARK 16U
/* The tool grid, which is three across and two down and never a row. */
#define PAINT_TOOL_CELL 26U
#define PAINT_TOOL_COLUMNS 3U
#define PAINT_TOOL_ROWS 2U
/* The shapes gallery: three rows of seven, with its scroll arrows beside. */
#define PAINT_SHAPE_CELL 22U
#define PAINT_SHAPE_COLUMNS 7U
#define PAINT_SHAPE_ROWS 3U
#define PAINT_GALLERY_ARROWS 16U   /* the smallest mark there is */
/* The colour palette: two rows of ten, and the two big colour buttons. */
#define PAINT_SWATCH 15U
#define PAINT_SWATCH_COLUMNS 10U
#define PAINT_COLOUR_BUTTON 74U
/* The canvas, and the handles Paint puts on its right and bottom edges. */
#define PAINT_SHEET_MARGIN 12U
#define PAINT_HANDLE 5U
/* The zoom slider at the right of the status bar. */
#define PAINT_ZOOM_TRACK 92U
#define PAINT_ZOOM_THUMB 7U

/* ================================================================ PALETTE
 *
 * Paint's own twenty default swatches, in the order it lays them out: ten
 * across the top row and ten under them.  These are not chosen - they are
 * the values Paint has shipped with since the ribbon arrived, and the names
 * beside them are Paint's own.
 */
struct paint_rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

#define PAINT_RGB(r, g, b) { (uint8_t)(r), (uint8_t)(g), (uint8_t)(b) }

static const struct paint_rgb swatches[PAINT_SWATCHES] = {
    PAINT_RGB(0x00U, 0x00U, 0x00U),   /* Black          */
    PAINT_RGB(0x7FU, 0x7FU, 0x7FU),   /* Grey-50%       */
    PAINT_RGB(0x88U, 0x00U, 0x15U),   /* Dark red       */
    PAINT_RGB(0xEDU, 0x1CU, 0x24U),   /* Red            */
    PAINT_RGB(0xFFU, 0x7FU, 0x27U),   /* Orange         */
    PAINT_RGB(0xFFU, 0xF2U, 0x00U),   /* Yellow         */
    PAINT_RGB(0x22U, 0xB1U, 0x4CU),   /* Green          */
    PAINT_RGB(0x00U, 0xA2U, 0xE8U),   /* Turquoise      */
    PAINT_RGB(0x3FU, 0x48U, 0xCCU),   /* Indigo         */
    PAINT_RGB(0xA3U, 0x49U, 0xA4U),   /* Purple         */
    PAINT_RGB(0xFFU, 0xFFU, 0xFFU),   /* White          */
    PAINT_RGB(0xC3U, 0xC3U, 0xC3U),   /* Grey-25%       */
    PAINT_RGB(0xB9U, 0x7AU, 0x57U),   /* Brown          */
    PAINT_RGB(0xFFU, 0xAEU, 0xC9U),   /* Rose           */
    PAINT_RGB(0xFFU, 0xC9U, 0x0EU),   /* Gold           */
    PAINT_RGB(0xEFU, 0xE4U, 0xB0U),   /* Light yellow   */
    PAINT_RGB(0xB5U, 0xE6U, 0x1DU),   /* Lime           */
    PAINT_RGB(0x99U, 0xD9U, 0xEAU),   /* Light turquoise*/
    PAINT_RGB(0x70U, 0x92U, 0xBEU),   /* Blue-grey      */
    PAINT_RGB(0xC8U, 0xBFU, 0xE7U)    /* Lavender       */
};

/* The chrome, which is Windows' rather than Paint's. */
static const struct paint_rgb caption_fill = PAINT_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct paint_rgb chrome = PAINT_RGB(0xF0U, 0xF0U, 0xF0U);
static const struct paint_rgb ribbon_fill = PAINT_RGB(0xF7U, 0xF7U, 0xF7U);
static const struct paint_rgb rule = PAINT_RGB(0xDCU, 0xDCU, 0xDCU);
static const struct paint_rgb rule_soft = PAINT_RGB(0xE8U, 0xE8U, 0xE8U);
static const struct paint_rgb ink = PAINT_RGB(0x1AU, 0x1AU, 0x1AU);
static const struct paint_rgb ink_soft = PAINT_RGB(0x5AU, 0x5AU, 0x5AU);
static const struct paint_rgb ink_faint = PAINT_RGB(0x8CU, 0x8CU, 0x8CU);
static const struct paint_rgb accent = PAINT_RGB(0x00U, 0x78U, 0xD7U);
static const struct paint_rgb on_accent = PAINT_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct paint_rgb hover_fill = PAINT_RGB(0xE8U, 0xF1U, 0xFAU);
/* Paint's workspace: the grey the sheet floats on. */
static const struct paint_rgb workspace = PAINT_RGB(0x9EU, 0x9EU, 0x9EU);
static const struct paint_rgb sheet = PAINT_RGB(0xFFU, 0xFFU, 0xFFU);
static const struct paint_rgb sheet_edge = PAINT_RGB(0x60U, 0x60U, 0x60U);
static const struct paint_rgb border_active = PAINT_RGB(0x00U, 0x78U, 0xD7U);
static const struct paint_rgb border_inactive = PAINT_RGB(0x9BU, 0x9BU,
    0x9BU);

/* ================================================================== STATE */

static struct surface *canvas;
static struct ui_rect window_rect;
static bool initialized;
static bool focused = true;
static enum paint_tool current_tool = PAINT_TOOL_PENCIL;
static size_t current_shape;
static size_t colour_one;          /* index into swatches */
static size_t colour_two = 10U;    /* White, which is Paint's default */
static uint32_t zoom_percent = 100U;
static char window_title[48] = "Untitled - Paint";
static size_t hover_tool = (size_t)-1;
static size_t hover_shape = (size_t)-1;
static size_t hover_swatch = (size_t)-1;
static const char *self_test_failure = "paint self-test has not run";

const char *paint_status_string(enum paint_status status)
{
    switch (status) {
    case PAINT_STATUS_OK:
        return "ok";
    case PAINT_STATUS_NULL_ARGUMENT:
        return "null argument";
    case PAINT_STATUS_NOT_INITIALIZED:
        return "paint not initialized";
    case PAINT_STATUS_BAD_INDEX:
        return "paint index is out of range";
    case PAINT_STATUS_UNSUPPORTED_GEOMETRY:
        return "paint geometry is unsupported";
    case PAINT_STATUS_SURFACE_FAILURE:
        return "paint surface refused a pixel";
    default:
        return "unknown paint status";
    }
}

/* ============================================================== PRIMITIVES */

static uint32_t pack_rgb(struct paint_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();

    return ((uint32_t)colour.red << format.red_position) |
        ((uint32_t)colour.green << format.green_position) |
        ((uint32_t)colour.blue << format.blue_position);
}

static struct ui_rect intersect(struct ui_rect left, struct ui_rect right)
{
    const uint32_t x0 = left.x > right.x ? left.x : right.x;
    const uint32_t y0 = left.y > right.y ? left.y : right.y;
    const uint32_t x1a = left.x + left.width;
    const uint32_t x1b = right.x + right.width;
    const uint32_t y1a = left.y + left.height;
    const uint32_t y1b = right.y + right.height;
    const uint32_t x1 = x1a < x1b ? x1a : x1b;
    const uint32_t y1 = y1a < y1b ? y1a : y1b;

    if (x1 <= x0 || y1 <= y0) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x0, y0, x1 - x0, y1 - y0 };
}

static bool holds(struct ui_rect area, struct ui_point point)
{
    return point.x >= 0 && point.y >= 0 &&
        (uint32_t)point.x >= area.x &&
        (uint32_t)point.x < area.x + area.width &&
        (uint32_t)point.y >= area.y &&
        (uint32_t)point.y < area.y + area.height;
}

static enum paint_status fill(struct ui_rect area, struct ui_rect damage,
    struct paint_rgb colour)
{
    const struct ui_rect clipped = intersect(area, damage);
    const uint32_t packed = pack_rgb(colour);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    packed) != SURFACE_STATUS_OK) {
                return PAINT_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return PAINT_STATUS_OK;
}

static enum paint_status outline(struct ui_rect area, struct ui_rect damage,
    struct paint_rgb colour)
{
    enum paint_status status = fill((struct ui_rect){ area.x, area.y,
        area.width, 1U }, damage, colour);

    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y + area.height - 1U,
            area.width, 1U }, damage, colour);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ area.x, area.y, 1U, area.height },
            damage, colour);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ area.x + area.width - 1U, area.y, 1U,
            area.height }, damage, colour);
    }
    return status;
}

static enum paint_status text_at(struct ui_rect damage, uint32_t x,
    uint32_t baseline, const char *body, struct paint_rgb colour)
{
    const struct ui_rect clip = intersect(damage, window_rect);
    struct surface_rect bounds;
    struct surface_rect region;

    if (clip.width == 0U || clip.height == 0U || body == NULL ||
            body[0] == '\0') {
        return PAINT_STATUS_OK;
    }
    bounds.x = window_rect.x;
    bounds.y = window_rect.y;
    bounds.width = window_rect.width;
    bounds.height = window_rect.height;
    region.x = clip.x;
    region.y = clip.y;
    region.width = clip.width;
    region.height = clip.height;
    (void)ui_font_draw_text_clipped(canvas, bounds, region, x, baseline, body,
        pack_rgb(colour), NULL);
    return PAINT_STATUS_OK;
}

static uint32_t width_of(const char *body)
{
    uint32_t width = 0U;

    if (body == NULL || ui_font_text_width(body, &width) !=
            UI_FONT_STATUS_OK) {
        return 0U;
    }
    return width;
}

static uint32_t centred_x(uint32_t left, uint32_t width, uint32_t text)
{
    return width > text ? left + (width - text) / 2U : left;
}

static bool names_match(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static const uint8_t *glyph_cell(const char *name, uint32_t wanted,
    uint32_t *size)
{
    for (size_t index = 0U; index < PAINT_LUCIDE_COUNT; ++index) {
        size_t choice = 0U;

        if (!names_match(paint_lucide[index].name, name)) {
            continue;
        }
        for (size_t option = 0U; option < PAINT_LUCIDE_SIZES; ++option) {
            if (paint_lucide_size[option] <= wanted) {
                choice = option;
            }
        }
        *size = paint_lucide_size[choice];
        return paint_lucide[index].alpha[choice];
    }
    return NULL;
}

static enum paint_status draw_glyph(const char *name, struct ui_rect box,
    struct ui_rect damage, struct paint_rgb colour)
{
    const struct framebuffer_state format = framebuffer_get_state();
    const uint32_t wanted = box.width < box.height ? box.width : box.height;
    const uint32_t over = pack_rgb(colour);
    uint32_t size = 0U;
    const uint8_t *cell = glyph_cell(name, wanted, &size);
    struct ui_rect placed;
    struct ui_rect clipped;

    if (cell == NULL) {
        return PAINT_STATUS_OK;
    }
    placed = (struct ui_rect){
        box.x + (box.width > size ? (box.width - size) / 2U : 0U),
        box.y + (box.height > size ? (box.height - size) / 2U : 0U),
        size, size };
    clipped = intersect(placed, damage);
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t local_y = clipped.y - placed.y + y;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint8_t coverage = cell[local_y * size +
                (clipped.x - placed.x + x)];
            uint32_t under;
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (coverage == 0U) {
                continue;
            }
            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK) {
                return PAINT_STATUS_SURFACE_FAILURE;
            }
            red = (((over >> format.red_position) & 0xFFU) * coverage +
                ((under >> format.red_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            green = (((over >> format.green_position) & 0xFFU) * coverage +
                ((under >> format.green_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            blue = (((over >> format.blue_position) & 0xFFU) * coverage +
                ((under >> format.blue_position) & 0xFFU) *
                (255U - coverage) + 127U) / 255U;
            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    (red << format.red_position) |
                    (green << format.green_position) |
                    (blue << format.blue_position)) !=
                        SURFACE_STATUS_OK) {
                return PAINT_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return PAINT_STATUS_OK;
}

/* Two hex digits of a byte, which is what the colour buttons print. */
static void hex_byte(char *out, uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    out[0] = digits[(value >> 4) & 0x0FU];
    out[1] = digits[value & 0x0FU];
}

static void hex_colour(char *out, struct paint_rgb colour)
{
    out[0] = '#';
    hex_byte(out + 1, colour.red);
    hex_byte(out + 3, colour.green);
    hex_byte(out + 5, colour.blue);
    out[7] = '\0';
}

/* ================================================================ GEOMETRY */

static struct ui_rect caption_rect(void)
{
    return (struct ui_rect){ window_rect.x + PAINT_BORDER,
        window_rect.y + PAINT_BORDER,
        window_rect.width - PAINT_BORDER * 2U, PAINT_CAPTION };
}

static struct ui_rect tabs_rect(void)
{
    const struct ui_rect caption = caption_rect();

    return (struct ui_rect){ caption.x, caption.y + caption.height,
        caption.width, PAINT_TABS };
}

static struct ui_rect ribbon_rect(void)
{
    const struct ui_rect tabs = tabs_rect();

    return (struct ui_rect){ tabs.x, tabs.y + tabs.height, tabs.width,
        PAINT_RIBBON };
}

static struct ui_rect status_rect(void)
{
    return (struct ui_rect){ window_rect.x + PAINT_BORDER,
        window_rect.y + window_rect.height - PAINT_BORDER - PAINT_STATUS,
        window_rect.width - PAINT_BORDER * 2U, PAINT_STATUS };
}

static struct ui_rect workspace_rect(void)
{
    const struct ui_rect band = ribbon_rect();
    const struct ui_rect bar = status_rect();

    return (struct ui_rect){ band.x, band.y + band.height, band.width,
        bar.y - (band.y + band.height) };
}

struct ui_rect paint_sheet_bounds(void)
{
    const struct ui_rect area = workspace_rect();
    /* Paint pins the sheet to the top-left of its workspace and grows it to
     * the right and down, which is why the handles are only on those edges. */
    const uint32_t width = area.width > PAINT_SHEET_MARGIN * 2U + 200U ?
        area.width - PAINT_SHEET_MARGIN * 2U - 120U : 200U;
    const uint32_t height = area.height > PAINT_SHEET_MARGIN * 2U + 120U ?
        area.height - PAINT_SHEET_MARGIN * 2U - 40U : 120U;

    return (struct ui_rect){ area.x + PAINT_SHEET_MARGIN,
        area.y + PAINT_SHEET_MARGIN, width, height };
}

/*
 * What the pointer means over Paint.
 *
 * The sheet takes a crosshair, which is what every raster editor since the
 * first one has put over a canvas and what Windows calls Precision Select:
 * an arrow has a hotspot in its corner and you cannot see the pixel you are
 * about to set.  The ribbon and the chrome are ordinary buttons and take
 * the ordinary arrow.
 *
 * Nothing here decides WHEN this is right - the compositor asks the window
 * under the pointer and this is Paint's answer.  See sapote/cursor.h.
 */
enum cursor_kind paint_cursor_at(struct ui_point point)
{
    if (!initialized || !holds(window_rect, point)) {
        return CURSOR_NORMAL_SELECT;
    }
    if (holds(paint_sheet_bounds(), point)) {
        return CURSOR_PRECISION_SELECT;
    }
    return CURSOR_NORMAL_SELECT;
}

struct ui_rect paint_bounds(void)
{
    return window_rect;
}

static struct ui_rect caption_button_rect(uint32_t index)
{
    const struct ui_rect caption = caption_rect();
    const uint32_t from_right = 3U - index;

    return (struct ui_rect){
        caption.x + caption.width - from_right * PAINT_CAPTION_BUTTON,
        caption.y, PAINT_CAPTION_BUTTON, caption.height };
}

/* --- the ribbon's seven groups, measured left to right --- */

/*
 * Each group's width is stated where the group is drawn, and both the
 * drawing and the hit testing walk the same list, so a group can never be
 * drawn in one place and clicked in another.
 */
enum paint_group {
    PAINT_GROUP_CLIPBOARD = 0,
    PAINT_GROUP_IMAGE,
    PAINT_GROUP_TOOLS,
    PAINT_GROUP_BRUSHES,
    PAINT_GROUP_SHAPES,
    PAINT_GROUP_SIZE,
    PAINT_GROUP_COLORS,
    PAINT_GROUP_COUNT
};

static uint32_t group_width(enum paint_group group)
{
    switch (group) {
    case PAINT_GROUP_CLIPBOARD:
        return PAINT_TALL_WIDTH + 66U;
    case PAINT_GROUP_IMAGE:
        return PAINT_TALL_WIDTH + 70U;
    case PAINT_GROUP_TOOLS:
        return PAINT_TOOL_CELL * PAINT_TOOL_COLUMNS + 8U;
    case PAINT_GROUP_BRUSHES:
        /* Wide enough for the word beneath it rather than for the button
         * alone: "Brushes" is wider than a tall button, so a group sized
         * to the button pushed its own label over the divider beside it.
         * paint_self_test() checks every group against its label. */
        return PAINT_TALL_WIDTH + 10U;
    case PAINT_GROUP_SHAPES:
        return PAINT_SHAPE_CELL * PAINT_SHAPE_COLUMNS +
            PAINT_GALLERY_ARROWS + 78U;
    case PAINT_GROUP_SIZE:
        return PAINT_TALL_WIDTH;
    case PAINT_GROUP_COLORS:
    default:
        return PAINT_COLOUR_BUTTON * 2U + 10U +
            PAINT_SWATCH * PAINT_SWATCH_COLUMNS + 10U + 58U;
    }
}

/* The word Paint prints under each group, and what each group has to be
 * wide enough to hold. */
static const char *group_label(enum paint_group group)
{
    switch (group) {
    case PAINT_GROUP_CLIPBOARD:
        return "Clipboard";
    case PAINT_GROUP_IMAGE:
        return "Image";
    case PAINT_GROUP_TOOLS:
        return "Tools";
    case PAINT_GROUP_BRUSHES:
        return "Brushes";
    case PAINT_GROUP_SHAPES:
        return "Shapes";
    case PAINT_GROUP_SIZE:
        return "Size";
    case PAINT_GROUP_COLORS:
    default:
        return "Colors";
    }
}

static uint32_t group_left(enum paint_group group)
{
    const struct ui_rect band = ribbon_rect();
    uint32_t pen = band.x + PAINT_PAD;

    for (enum paint_group scan = PAINT_GROUP_CLIPBOARD; scan < group;
         scan = (enum paint_group)(scan + 1)) {
        pen += group_width(scan) + PAINT_GROUP_GAP;
    }
    return pen;
}

static struct ui_rect tool_rect(size_t tool)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_TOOLS) + 4U;
    const uint32_t column = (uint32_t)tool % PAINT_TOOL_COLUMNS;
    const uint32_t row = (uint32_t)tool / PAINT_TOOL_COLUMNS;

    return (struct ui_rect){ left + column * PAINT_TOOL_CELL,
        band.y + 12U + row * PAINT_TOOL_CELL,
        PAINT_TOOL_CELL, PAINT_TOOL_CELL };
}

static struct ui_rect shape_rect(size_t shape)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_SHAPES) + 2U;
    const uint32_t column = (uint32_t)shape % PAINT_SHAPE_COLUMNS;
    const uint32_t row = (uint32_t)shape / PAINT_SHAPE_COLUMNS;

    return (struct ui_rect){ left + column * PAINT_SHAPE_CELL,
        band.y + 8U + row * PAINT_SHAPE_CELL,
        PAINT_SHAPE_CELL, PAINT_SHAPE_CELL };
}

static struct ui_rect swatch_rect(size_t index)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_COLORS) +
        PAINT_COLOUR_BUTTON * 2U + 10U;
    const uint32_t column = (uint32_t)index % PAINT_SWATCH_COLUMNS;
    const uint32_t row = (uint32_t)index / PAINT_SWATCH_COLUMNS;

    return (struct ui_rect){ left + column * PAINT_SWATCH,
        band.y + 20U + row * PAINT_SWATCH, PAINT_SWATCH, PAINT_SWATCH };
}

/* ================================================================== PIECES */

static const char *tool_glyph(size_t tool)
{
    switch (tool) {
    case PAINT_TOOL_PENCIL:
        return "pencil";
    case PAINT_TOOL_FILL:
        return "paint-bucket";
    case PAINT_TOOL_TEXT:
        return "type";
    case PAINT_TOOL_ERASER:
        return "eraser";
    case PAINT_TOOL_PICKER:
        return "pipette";
    case PAINT_TOOL_MAGNIFIER:
    default:
        return "zoom-in";
    }
}

/*
 * Paint's shapes gallery, in Paint's own order.
 *
 * Paint has twenty-three shapes and shows twenty-one of them in three rows
 * of seven, the last two reachable by scrolling.  This gallery carries the
 * twenty-one it shows; heart and lightning are not here, and the scroll
 * strip beside the grid therefore has nothing to scroll - it is drawn
 * because Paint draws it, and it is the one control in this window that
 * does not do the thing it is drawn as.  Carrying all twenty-three and
 * scrolling a row is the fix; until then the strip is marked as inert
 * where it is drawn rather than left to look live.
 *
 * The order below is Paint's: line and curve first, then the closed
 * shapes, then the arrows, the stars, and the callouts.
 *
 * Lucide has no mark for three of them, so those three are drawn in Lucide's
 * own idiom - a 24-unit grid, stroke 2, round caps and joins - and go through
 * the same rasterizer.  Nothing here repeats: a gallery with the square in it
 * twice is a gallery that was padded rather than filled.
 */
static const char *const shape_glyph[PAINT_SHAPES] = {
    /* row one */
    "slash",               /* Line                        */
    "spline",              /* Curve                       */
    "circle",              /* Oval                        */
    "square",              /* Rectangle                   */
    "shape-rounded-rect",  /* Rounded rectangle           */
    "octagon",             /* Polygon                     */
    "triangle",            /* Triangle                    */
    /* row two */
    "shape-right-triangle",/* Right triangle              */
    "diamond",             /* Diamond                     */
    "pentagon",            /* Pentagon                    */
    "hexagon",             /* Hexagon                     */
    "arrow-right",         /* Right arrow                 */
    "arrow-left",          /* Left arrow                  */
    "arrow-up",            /* Up arrow                    */
    /* row three */
    "arrow-down",          /* Down arrow                  */
    "sparkle",             /* Four-point star             */
    "star",                /* Five-point star             */
    "shape-star-six",      /* Six-point star              */
    "message-square",      /* Rounded rectangular callout */
    "message-circle",      /* Oval callout                */
    "cloud"                /* Cloud callout               */
    /* Paint's Heart and Lightning would follow, on a fourth row. */
};

/*
 * A tall ribbon button: a mark over one or two lines of label, with an
 * optional dropdown chevron under it - which is how Paint says a button has
 * a menu behind it.
 */
static enum paint_status draw_tall(struct ui_rect box, struct ui_rect damage,
    const char *glyph, const char *first, const char *second, bool dropdown)
{
    enum paint_status status = draw_glyph(glyph, (struct ui_rect){ box.x,
        box.y + 4U, box.width, PAINT_TALL_ICON }, damage, ink_soft);
    uint32_t baseline = box.y + PAINT_TALL_ICON + 20U;

    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, centred_x(box.x, box.width, width_of(first)),
            baseline, first, ink);
    }
    if (status == PAINT_STATUS_OK && second != NULL) {
        baseline += 13U;
        status = text_at(damage,
            centred_x(box.x, box.width, width_of(second)), baseline, second,
            ink);
    }
    if (status == PAINT_STATUS_OK && dropdown) {
        status = draw_glyph("chevron-down", (struct ui_rect){ box.x,
            baseline + 2U, box.width, PAINT_MARK }, damage, ink_soft);
    }
    return status;
}

static enum paint_status draw_small(struct ui_rect box, struct ui_rect damage,
    const char *glyph, const char *label)
{
    const enum paint_status status = draw_glyph(glyph, (struct ui_rect){
        box.x, box.y, PAINT_SMALL_ICON, box.height }, damage, ink_soft);

    if (status != PAINT_STATUS_OK) {
        return status;
    }
    return text_at(damage, box.x + PAINT_SMALL_ICON + 5U,
        box.y + box.height / 2U + 5U, label, ink);
}

/* A group's name along the bottom, and the hairline that ends it. */
static enum paint_status draw_group_label(enum paint_group group,
    struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const char *label = group_label(group);
    const uint32_t left = group_left(group);
    const uint32_t width = group_width(group);
    const enum paint_status status = text_at(damage,
        centred_x(left, width, width_of(label)),
        band.y + band.height - 4U, label, ink_faint);

    if (status != PAINT_STATUS_OK || group + 1 == PAINT_GROUP_COUNT) {
        return status;
    }
    return fill((struct ui_rect){ left + width + PAINT_GROUP_GAP / 2U,
        band.y + 4U, 1U, band.height - PAINT_RIBBON_LABEL - 4U }, damage,
        rule_soft);
}

/* --- the seven groups --- */

static enum paint_status draw_clipboard(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_CLIPBOARD);
    enum paint_status status = draw_tall((struct ui_rect){ left, band.y + 4U,
        PAINT_TALL_WIDTH, 0U }, damage, "clipboard-paste", "Paste", NULL,
        true);

    if (status == PAINT_STATUS_OK) {
        status = draw_small((struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
            band.y + 14U, 60U, PAINT_SMALL_HEIGHT }, damage, "scissors",
            "Cut");
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_small((struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
            band.y + 14U + PAINT_SMALL_HEIGHT, 60U, PAINT_SMALL_HEIGHT },
            damage, "copy", "Copy");
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_CLIPBOARD, damage);
    }
    return status;
}

static enum paint_status draw_image_group(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_IMAGE);
    enum paint_status status = draw_tall((struct ui_rect){ left, band.y + 4U,
        PAINT_TALL_WIDTH, 0U }, damage, "square", "Select", NULL,
        true);

    if (status != PAINT_STATUS_OK) {
        return status;
    }
    /* Lucide has no dashed rectangle; Paint's Select is a plain marquee, so
     * the plain square stands in and the dropdown says the rest. */
    static const char *const glyphs[3] = { "crop", "scaling", "rotate-cw" };
    static const char *const labels[3] = { "Crop", "Resize", "Rotate" };

    for (size_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        status = draw_small((struct ui_rect){ left + PAINT_TALL_WIDTH + 4U,
            band.y + 8U + (uint32_t)index * PAINT_SMALL_HEIGHT, 64U,
            PAINT_SMALL_HEIGHT }, damage, glyphs[index], labels[index]);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_IMAGE, damage);
    }
    return status;
}

static enum paint_status draw_tools(struct ui_rect damage)
{
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT &&
            status == PAINT_STATUS_OK; ++tool) {
        const struct ui_rect cell = tool_rect(tool);
        const bool chosen = tool == (size_t)current_tool;

        /*
         * Phipia marks the held tool with the ACCENT.  Paint marks it with a
         * faint pressed plate that on a ribbon this busy takes a second look
         * to find, and the tool you are holding is the one thing about this
         * window you need to know without looking.
         */
        if (chosen) {
            status = fill(cell, damage, accent);
        } else if (tool == hover_tool) {
            status = fill(cell, damage, hover_fill);
        }
        if (status == PAINT_STATUS_OK) {
            status = draw_glyph(tool_glyph(tool), cell, damage,
                chosen ? on_accent : ink_soft);
        }
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_TOOLS, damage);
    }
    return status;
}

static enum paint_status draw_brushes(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const enum paint_status status = draw_tall((struct ui_rect){
        centred_x(group_left(PAINT_GROUP_BRUSHES),
            group_width(PAINT_GROUP_BRUSHES), PAINT_TALL_WIDTH),
        band.y + 4U, PAINT_TALL_WIDTH, 0U },
        damage, "brush", "Brushes", NULL, true);

    if (status != PAINT_STATUS_OK) {
        return status;
    }
    return draw_group_label(PAINT_GROUP_BRUSHES, damage);
}

static enum paint_status draw_shapes(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t gallery_right = group_left(PAINT_GROUP_SHAPES) + 2U +
        PAINT_SHAPE_CELL * PAINT_SHAPE_COLUMNS;
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t shape = 0U; shape < PAINT_SHAPES &&
            status == PAINT_STATUS_OK; ++shape) {
        const struct ui_rect cell = shape_rect(shape);

        if (shape == current_shape) {
            status = fill(cell, damage, accent);
        } else if (shape == hover_shape) {
            status = fill(cell, damage, hover_fill);
        }
        if (status == PAINT_STATUS_OK) {
            status = draw_glyph(shape_glyph[shape], cell, damage,
                shape == current_shape ? on_accent : ink_soft);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }
    /*
     * The gallery's scroll strip.  With every shape this gallery holds
     * already on screen there is nothing for it to scroll, so it is drawn
     * the way Paint draws a scroll arrow with nothing past it: in the
     * disabled ink, not the live one.  Two boxes rather than Paint's
     * three - the third is Paint's expander, and this gallery has no
     * flyout to expand into, so it is not drawn as though it had.
     */
    static const char *const arrows[2] = { "chevron-up", "chevron-down" };

    for (size_t index = 0U; index < 2U && status == PAINT_STATUS_OK;
         ++index) {
        const struct ui_rect box = { gallery_right,
            band.y + 8U + (uint32_t)index * 22U, PAINT_GALLERY_ARROWS, 22U };

        status = outline(box, damage, rule);
        if (status == PAINT_STATUS_OK) {
            status = draw_glyph(arrows[index], box, damage, ink_faint);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    const uint32_t side = gallery_right + PAINT_GALLERY_ARROWS + 6U;

    status = draw_small((struct ui_rect){ side, band.y + 12U, 70U,
        PAINT_SMALL_HEIGHT }, damage, "square", "Outline");
    if (status == PAINT_STATUS_OK) {
        status = draw_small((struct ui_rect){ side,
            band.y + 12U + PAINT_SMALL_HEIGHT, 70U, PAINT_SMALL_HEIGHT },
            damage, "paint-bucket", "Fill");
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_SHAPES, damage);
    }
    return status;
}

/*
 * The Size button.
 *
 * Paint draws four rules of increasing weight inside it rather than an icon,
 * because the button IS the thing it chooses.  Stating that as geometry is
 * exact; there is no icon in any set that means "one, three, five and eight
 * pixels".
 */
static enum paint_status draw_size(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_SIZE);
    static const uint32_t weights[4] = { 1U, 3U, 5U, 8U };
    uint32_t pen = band.y + 10U;
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t index = 0U; index < 4U && status == PAINT_STATUS_OK;
         ++index) {
        status = fill((struct ui_rect){ left + 12U, pen,
            PAINT_TALL_WIDTH - 24U, weights[index] }, damage, ink_soft);
        pen += weights[index] + 6U;
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage,
            centred_x(left, PAINT_TALL_WIDTH, width_of("Size")),
            band.y + 60U, "Size", ink);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("chevron-down", (struct ui_rect){ left,
            band.y + 63U, PAINT_TALL_WIDTH, PAINT_MARK }, damage,
            ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_SIZE, damage);
    }
    return status;
}

static enum paint_status draw_colours(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    const uint32_t left = group_left(PAINT_GROUP_COLORS);
    static const char *const names[2] = { "Color 1", "Color 2" };
    const size_t chosen[2] = { colour_one, colour_two };
    enum paint_status status = PAINT_STATUS_OK;

    for (size_t index = 0U; index < 2U && status == PAINT_STATUS_OK;
         ++index) {
        const struct ui_rect box = {
            left + (uint32_t)index * PAINT_COLOUR_BUTTON, band.y + 6U,
            PAINT_COLOUR_BUTTON, PAINT_COLOUR_BUTTON };
        const struct ui_rect well = { box.x + 8U, box.y + 6U,
            box.width - 16U, 20U };
        char hex[8];

        status = fill(well, damage, swatches[chosen[index]]);
        if (status == PAINT_STATUS_OK) {
            status = outline(well, damage, ink_faint);
        }
        if (status == PAINT_STATUS_OK) {
            status = text_at(damage,
                centred_x(box.x, box.width, width_of(names[index])),
                box.y + 40U, names[index], ink);
        }
        if (status != PAINT_STATUS_OK) {
            return status;
        }
        /*
         * And its hex under it, which Paint does not show anywhere without
         * opening Edit colors - and which is the one number anyone opening
         * Paint to match a colour actually wants.
         */
        hex_colour(hex, swatches[chosen[index]]);
        status = text_at(damage, centred_x(box.x, box.width, width_of(hex)),
            box.y + 54U, hex, ink_faint);
    }
    for (size_t index = 0U; index < PAINT_SWATCHES &&
            status == PAINT_STATUS_OK; ++index) {
        const struct ui_rect cell = swatch_rect(index);
        const struct ui_rect well = { cell.x + 1U, cell.y + 1U,
            cell.width - 2U, cell.height - 2U };

        status = fill(well, damage, swatches[index]);
        if (status == PAINT_STATUS_OK) {
            status = outline(well, damage,
                index == hover_swatch ? accent : ink_faint);
        }
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    const uint32_t edit = left + PAINT_COLOUR_BUTTON * 2U + 10U +
        PAINT_SWATCH * PAINT_SWATCH_COLUMNS + 10U;

    status = draw_tall((struct ui_rect){ edit, band.y + 4U, 54U, 0U },
        damage, "palette", "Edit", "colors", false);
    if (status == PAINT_STATUS_OK) {
        status = draw_group_label(PAINT_GROUP_COLORS, damage);
    }
    return status;
}

static enum paint_status draw_ribbon(struct ui_rect damage)
{
    const struct ui_rect band = ribbon_rect();
    enum paint_status status = fill(band, damage, ribbon_fill);

    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ band.x, band.y + band.height - 1U,
            band.width, 1U }, damage, rule);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_clipboard(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_image_group(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_tools(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_brushes(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_shapes(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_size(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_colours(damage);
    }
    return status;
}

/* ================================================================== CHROME */

static enum paint_status draw_caption(struct ui_rect damage)
{
    const struct ui_rect caption = caption_rect();
    /*
     * Paint's Quick Access Toolbar: its own mark, then save, undo, redo and
     * the customise chevron - inside the title bar, at its left end.
     *
     * Undo and redo are the ARROWS, not the circular rotate marks.  A pair
     * of circular arrows beside each other in a title bar reads as "rotate
     * left" and "rotate right", which in a drawing program is a thing Paint
     * can actually do to your picture - so the wrong icon here is not just
     * unclear, it means something else.  Windows draws these two in blue for
     * the same reason.
     */
    static const char *const quick[] = { "save", "undo-2", "redo-2",
        "chevron-down" };
    uint32_t pen = caption.x + 6U;
    enum paint_status status = fill(caption, damage, caption_fill);

    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("palette", (struct ui_rect){ pen, caption.y,
            PAINT_QAT_SLOT, caption.height }, damage, accent);
    }
    pen += PAINT_QAT_SLOT + 2U;
    for (size_t index = 0U; index < 4U && status == PAINT_STATUS_OK;
         ++index) {
        status = draw_glyph(quick[index], (struct ui_rect){ pen, caption.y,
            PAINT_QAT_SLOT, caption.height }, damage,
            index == 3U ? ink_faint :
                (index == 0U ? ink_soft : accent));
        pen += PAINT_QAT_SLOT;
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage,
            centred_x(caption.x, caption.width, width_of(window_title)),
            caption.y + caption.height / 2U + 5U, window_title,
            focused ? ink : ink_faint);
    }
    for (uint32_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        const struct ui_rect button = caption_button_rect(index);
        const uint32_t size = 10U;
        const uint32_t bx = button.x + (button.width - size) / 2U;
        const uint32_t by = button.y + (button.height - size) / 2U;
        const struct paint_rgb mark = focused ? ink : ink_faint;

        if (index == 0U) {
            status = fill((struct ui_rect){ bx, by + size / 2U, size, 1U },
                damage, mark);
        } else if (index == 1U) {
            status = outline((struct ui_rect){ bx, by, size, size }, damage,
                mark);
        } else {
            for (uint32_t step = 0U; step < size &&
                    status == PAINT_STATUS_OK; ++step) {
                status = fill((struct ui_rect){ bx + step, by + step, 1U,
                    1U }, damage, mark);
                if (status == PAINT_STATUS_OK) {
                    status = fill((struct ui_rect){ bx + step,
                        by + size - 1U - step, 1U, 1U }, damage, mark);
                }
            }
        }
    }
    return status;
}

static enum paint_status draw_tabs(struct ui_rect damage)
{
    static const char *const tabs[] = { "File", "Home", "View" };
    const struct ui_rect strip = tabs_rect();
    uint32_t pen = strip.x + PAINT_PAD;
    enum paint_status status = fill(strip, damage, chrome);

    for (size_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        const uint32_t width = width_of(tabs[index]) + PAINT_TAB_PAD * 2U;

        if (index == 0U) {
            /* File is a solid accent button rather than a tab, because it
             * opens a menu instead of switching the ribbon. */
            status = fill((struct ui_rect){ pen, strip.y, width,
                strip.height }, damage, accent);
            if (status == PAINT_STATUS_OK) {
                status = text_at(damage, pen + PAINT_TAB_PAD,
                    strip.y + strip.height / 2U + 5U, tabs[index], on_accent);
            }
        } else {
            if (index == 1U) {
                status = fill((struct ui_rect){ pen, strip.y, width,
                    strip.height }, damage, ribbon_fill);
            }
            if (status == PAINT_STATUS_OK) {
                status = text_at(damage, pen + PAINT_TAB_PAD,
                    strip.y + strip.height / 2U + 5U, tabs[index],
                    index == 1U ? ink : ink_soft);
            }
        }
        pen += width;
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }
    /* Paint puts a help mark and the ribbon's collapse chevron at the far
     * right of the tab strip, which nothing else in the shell does. */
    status = draw_glyph("help-circle", (struct ui_rect){
        strip.x + strip.width - 48U, strip.y, 20U, strip.height }, damage,
        ink_soft);
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("chevron-up", (struct ui_rect){
            strip.x + strip.width - 24U, strip.y, 20U, strip.height },
            damage, ink_soft);
    }
    return status;
}

/*
 * The workspace: the grey Paint floats its sheet on, the sheet itself, and
 * the resize handles on its right and bottom edges only - Paint grows a
 * canvas right and down and never up or left, which is why there is no
 * handle on the other two sides.
 */
static enum paint_status draw_workspace(struct ui_rect damage)
{
    const struct ui_rect area = workspace_rect();
    const struct ui_rect page = paint_sheet_bounds();
    enum paint_status status = fill(area, damage, workspace);

    if (status == PAINT_STATUS_OK) {
        status = fill(page, damage, sheet);
    }
    if (status == PAINT_STATUS_OK) {
        status = outline(page, damage, sheet_edge);
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    const struct ui_rect handles[3] = {
        { page.x + page.width - PAINT_HANDLE / 2U,
          page.y + page.height / 2U - PAINT_HANDLE / 2U,
          PAINT_HANDLE, PAINT_HANDLE },
        { page.x + page.width / 2U - PAINT_HANDLE / 2U,
          page.y + page.height - PAINT_HANDLE / 2U,
          PAINT_HANDLE, PAINT_HANDLE },
        { page.x + page.width - PAINT_HANDLE / 2U,
          page.y + page.height - PAINT_HANDLE / 2U,
          PAINT_HANDLE, PAINT_HANDLE }
    };

    for (size_t index = 0U; index < 3U && status == PAINT_STATUS_OK;
         ++index) {
        status = fill(handles[index], damage, sheet);
        if (status == PAINT_STATUS_OK) {
            status = outline(handles[index], damage, sheet_edge);
        }
    }
    return status;
}

/* Print an unsigned number right where it is asked for; the kernel has no
 * formatter and the status bar needs three of them. */
static void number_text(char *out, uint32_t value, const char *suffix)
{
    char digits[12];
    size_t count = 0U;
    size_t at = 0U;

    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U) {
        out[at++] = digits[--count];
    }
    while (*suffix != '\0') {
        out[at++] = *suffix++;
    }
    out[at] = '\0';
}

static enum paint_status draw_status(struct ui_rect damage)
{
    const struct ui_rect bar = status_rect();
    const struct ui_rect page = paint_sheet_bounds();
    const uint32_t baseline = bar.y + bar.height / 2U + 5U;
    char text[24];
    enum paint_status status = fill(bar, damage, chrome);

    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ bar.x, bar.y, bar.width, 1U }, damage,
            rule);
    }
    /* The pointer readout, then the canvas size, then the file size - the
     * three things Paint's status bar carries, in its order. */
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("move", (struct ui_rect){ bar.x + 6U, bar.y,
            PAINT_MARK,
            bar.height }, damage, ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, bar.x + 24U, baseline, "482, 216", ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_glyph("scaling", (struct ui_rect){ bar.x + 96U, bar.y,
            PAINT_MARK, bar.height }, damage, ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        number_text(text, page.width, " x ");
        status = text_at(damage, bar.x + 114U, baseline, text, ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        number_text(text, page.height, "px");
        status = text_at(damage, bar.x + 114U + width_of("0000 x "), baseline,
            text, ink_soft);
    }
    if (status != PAINT_STATUS_OK) {
        return status;
    }

    /*
     * The zoom, at the right end: minus, a slider, plus, then the
     * percentage.  Phipia fills the used half of the track with the accent -
     * Paint draws a grey thumb on a grey track and the control reads as
     * decoration until you touch it.
     */
    const uint32_t track_x = bar.x + bar.width - PAINT_ZOOM_TRACK - 84U;
    const uint32_t middle = bar.y + bar.height / 2U;
    /*
     * Paint's zoom slider is not linear.  It has seven notches - 12, 25, 50,
     * 100, 200, 400, 800 - each a doubling of the last, so 100% sits in the
     * MIDDLE of the track rather than an eighth of the way along it.  A
     * linear slider puts the default hard against the left stop, which is
     * the giveaway that a copy measured the numbers and not the control.
     */
    static const uint32_t notches[7] = { 12U, 25U, 50U, 100U, 200U, 400U,
        800U };
    size_t notch = 0U;

    for (size_t index = 0U; index < 7U; ++index) {
        if (zoom_percent >= notches[index]) {
            notch = index;
        }
    }

    const uint32_t lit = PAINT_ZOOM_TRACK * (uint32_t)notch / 6U;

    status = text_at(damage, track_x - 16U, baseline, "-", ink_soft);
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ track_x, middle - 1U,
            PAINT_ZOOM_TRACK, 2U }, damage, rule);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){ track_x, middle - 1U, lit, 2U },
            damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = fill((struct ui_rect){
            track_x + (lit > PAINT_ZOOM_THUMB / 2U ?
                lit - PAINT_ZOOM_THUMB / 2U : 0U),
            middle - 7U, PAINT_ZOOM_THUMB, 14U }, damage, accent);
    }
    if (status == PAINT_STATUS_OK) {
        status = text_at(damage, track_x + PAINT_ZOOM_TRACK + 8U, baseline,
            "+", ink_soft);
    }
    if (status == PAINT_STATUS_OK) {
        number_text(text, zoom_percent, "%");
        status = text_at(damage, bar.x + bar.width - 8U - width_of(text),
            baseline, text, ink_soft);
    }
    return status;
}

enum paint_status paint_draw(struct ui_rect damage)
{
    enum paint_status status;

    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    status = fill(window_rect, damage,
        focused ? border_active : border_inactive);
    if (status == PAINT_STATUS_OK) {
        status = draw_caption(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_tabs(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_ribbon(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_workspace(damage);
    }
    if (status == PAINT_STATUS_OK) {
        status = draw_status(damage);
    }
    return status;
}

/* ================================================================== INPUT */

enum paint_status paint_pointer_move(struct ui_point point,
    struct ui_rect *damage)
{
    const size_t was_tool = hover_tool;
    const size_t was_shape = hover_shape;
    const size_t was_swatch = hover_swatch;

    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    hover_tool = (size_t)-1;
    hover_shape = (size_t)-1;
    hover_swatch = (size_t)-1;
    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT; ++tool) {
        if (holds(tool_rect(tool), point)) {
            hover_tool = tool;
            break;
        }
    }
    for (size_t shape = 0U; shape < PAINT_SHAPES; ++shape) {
        if (holds(shape_rect(shape), point)) {
            hover_shape = shape;
            break;
        }
    }
    for (size_t index = 0U; index < PAINT_SWATCHES; ++index) {
        if (holds(swatch_rect(index), point)) {
            hover_swatch = index;
            break;
        }
    }
    if (was_tool != hover_tool || was_shape != hover_shape ||
            was_swatch != hover_swatch) {
        *damage = ribbon_rect();
    }
    return PAINT_STATUS_OK;
}

enum paint_status paint_pointer_press(struct ui_point point,
    struct ui_rect *damage)
{
    if (damage == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return PAINT_STATUS_NOT_INITIALIZED;
    }
    *damage = (struct ui_rect){ 0U, 0U, 0U, 0U };
    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT; ++tool) {
        if (holds(tool_rect(tool), point)) {
            current_tool = (enum paint_tool)tool;
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    for (size_t shape = 0U; shape < PAINT_SHAPES; ++shape) {
        if (holds(shape_rect(shape), point)) {
            current_shape = shape;
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    for (size_t index = 0U; index < PAINT_SWATCHES; ++index) {
        if (holds(swatch_rect(index), point)) {
            /* A left click sets Color 1, which is what Paint does. */
            colour_one = index;
            *damage = ribbon_rect();
            return PAINT_STATUS_OK;
        }
    }
    return PAINT_STATUS_OK;
}

/* ============================================================== LIFECYCLE */

enum paint_status paint_set_frame(struct ui_rect frame)
{
    uint32_t needed = PAINT_PAD * 2U + PAINT_BORDER * 2U;
    const uint32_t least_height = PAINT_BORDER * 2U + PAINT_CAPTION +
        PAINT_TABS + PAINT_RIBBON + PAINT_STATUS + 80U;

    for (enum paint_group group = PAINT_GROUP_CLIPBOARD;
         group < PAINT_GROUP_COUNT;
         group = (enum paint_group)(group + 1)) {
        needed += group_width(group) + PAINT_GROUP_GAP;
    }
    if (frame.width < needed || frame.height < least_height) {
        return PAINT_STATUS_UNSUPPORTED_GEOMETRY;
    }
    window_rect = frame;
    return PAINT_STATUS_OK;
}

enum paint_status paint_initialize(struct surface *target,
    struct ui_rect frame)
{
    enum paint_status status;

    if (target == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    canvas = target;
    status = paint_set_frame(frame);
    if (status != PAINT_STATUS_OK) {
        return status;
    }
    initialized = true;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_tool(enum paint_tool tool)
{
    if ((size_t)tool >= PAINT_TOOL_COUNT) {
        return PAINT_STATUS_BAD_INDEX;
    }
    current_tool = tool;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_shape(size_t shape)
{
    if (shape >= PAINT_SHAPES) {
        return PAINT_STATUS_BAD_INDEX;
    }
    current_shape = shape;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_colours(size_t first, size_t second)
{
    if (first >= PAINT_SWATCHES || second >= PAINT_SWATCHES) {
        return PAINT_STATUS_BAD_INDEX;
    }
    colour_one = first;
    colour_two = second;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_zoom(uint32_t percent)
{
    if (percent == 0U || percent > 800U) {
        return PAINT_STATUS_BAD_INDEX;
    }
    zoom_percent = percent;
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_title(const char *title)
{
    size_t index = 0U;

    if (title == NULL) {
        return PAINT_STATUS_NULL_ARGUMENT;
    }
    while (index + 1U < sizeof(window_title) && title[index] != '\0') {
        window_title[index] = title[index];
        ++index;
    }
    window_title[index] = '\0';
    return PAINT_STATUS_OK;
}

enum paint_status paint_set_focus(bool active)
{
    focused = active;
    return PAINT_STATUS_OK;
}

/* ============================================================== SELF TEST */

bool paint_self_test(void)
{
    uint32_t size = 0U;

    /*
     * Paint's palette is Paint's, not a set of colours that look about
     * right.  These four are the ones anybody would recognise, and if the
     * table has been edited they are the first to move.
     */
    if (swatches[3].red != 0xEDU || swatches[3].green != 0x1CU ||
            swatches[3].blue != 0x24U) {
        self_test_failure = "Paint's red is not ED1C24";
        return false;
    }
    if (swatches[7].red != 0x00U || swatches[7].green != 0xA2U ||
            swatches[7].blue != 0xE8U) {
        self_test_failure = "Paint's turquoise is not 00A2E8";
        return false;
    }
    if (swatches[10].red != 0xFFU || swatches[10].green != 0xFFU ||
            swatches[10].blue != 0xFFU) {
        self_test_failure = "Paint's eleventh swatch is not white";
        return false;
    }
    if (swatches[12].red != 0xB9U || swatches[12].green != 0x7AU ||
            swatches[12].blue != 0x57U) {
        self_test_failure = "Paint's brown is not B97A57";
        return false;
    }
    /* Every tool and every shape has a mark, or a cell comes out blank. */
    for (size_t tool = 0U; tool < PAINT_TOOL_COUNT; ++tool) {
        if (glyph_cell(tool_glyph(tool), PAINT_TOOL_CELL, &size) == NULL) {
            self_test_failure = "a Paint tool has no icon";
            return false;
        }
    }
    for (size_t shape = 0U; shape < PAINT_SHAPES; ++shape) {
        if (glyph_cell(shape_glyph[shape], PAINT_SHAPE_CELL, &size) ==
                NULL) {
            self_test_failure = "a Paint shape has no icon";
            return false;
        }
    }
    /* The tool grid is three across and two down, and holds all six. */
    if (PAINT_TOOL_COLUMNS * PAINT_TOOL_ROWS != PAINT_TOOL_COUNT) {
        self_test_failure = "Paint's tool grid does not hold its six tools";
        return false;
    }
    if (PAINT_SWATCH_COLUMNS * 2U != PAINT_SWATCHES) {
        self_test_failure = "Paint's palette is not two rows of ten";
        return false;
    }
    /* The default zoom has to land in the middle of its own track, which is
     * the whole point of the notches being doublings. */
    {
        static const uint32_t notches[7] = { 12U, 25U, 50U, 100U, 200U,
            400U, 800U };
        size_t notch = 0U;

        for (size_t index = 0U; index < 7U; ++index) {
            if (100U >= notches[index]) {
                notch = index;
            }
        }
        if (notch != 3U) {
            self_test_failure = "Paint's 100% zoom is not the middle notch";
            return false;
        }
    }
    if (paint_set_tool((enum paint_tool)PAINT_TOOL_COUNT) !=
            PAINT_STATUS_BAD_INDEX ||
            paint_set_shape(PAINT_SHAPES) != PAINT_STATUS_BAD_INDEX ||
            paint_set_colours(PAINT_SWATCHES, 0U) !=
                PAINT_STATUS_BAD_INDEX ||
            paint_set_zoom(0U) != PAINT_STATUS_BAD_INDEX) {
        self_test_failure = "Paint accepted a value off the end of a table";
        return false;
    }
    /* A group narrower than the word beneath it centres that word by
     * overflowing to the right, onto the divider of the group beside it.
     * That is how the Brushes group used to read as smudged. */
    for (enum paint_group group = PAINT_GROUP_CLIPBOARD;
         group < PAINT_GROUP_COUNT; group = (enum paint_group)(group + 1)) {
        if (width_of(group_label(group)) + 6U > group_width(group)) {
            self_test_failure =
                "a Paint ribbon group is narrower than its own label";
            return false;
        }
    }
    self_test_failure = "";
    return true;
}

const char *paint_self_test_failure(void)
{
    return self_test_failure;
}
