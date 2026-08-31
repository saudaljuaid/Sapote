/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Sapote video backend addition for SDL 2.32.10. This file is distributed
  under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_SAPOTE

#include "../SDL_sysvideo.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "SDL_timer.h"

#include <sapote/event.h>
#include <sapote/runtime.h>
#include <sapote/window.h>

#define SAPOTE_VIDEO_DRIVER_NAME "sapote"

typedef struct SapoteVideoData
{
    sapote_handle_t wakeup_timer;
} SapoteVideoData;

typedef struct SapoteWindowData
{
    sapote_handle_t window;
    sapote_handle_t events;
    Uint32 *pixels;
    Uint32 width;
    Uint32 height;
    Uint32 pitch;
} SapoteWindowData;

static SDL_Scancode SAPOTE_TranslateScancode(Uint8 code)
{
    static const SDL_Scancode map[0x59] = {
        [0x01] = SDL_SCANCODE_ESCAPE,
        [0x02] = SDL_SCANCODE_1,
        [0x03] = SDL_SCANCODE_2,
        [0x04] = SDL_SCANCODE_3,
        [0x05] = SDL_SCANCODE_4,
        [0x06] = SDL_SCANCODE_5,
        [0x07] = SDL_SCANCODE_6,
        [0x08] = SDL_SCANCODE_7,
        [0x09] = SDL_SCANCODE_8,
        [0x0a] = SDL_SCANCODE_9,
        [0x0b] = SDL_SCANCODE_0,
        [0x0c] = SDL_SCANCODE_MINUS,
        [0x0d] = SDL_SCANCODE_EQUALS,
        [0x0e] = SDL_SCANCODE_BACKSPACE,
        [0x0f] = SDL_SCANCODE_TAB,
        [0x10] = SDL_SCANCODE_Q,
        [0x11] = SDL_SCANCODE_W,
        [0x12] = SDL_SCANCODE_E,
        [0x13] = SDL_SCANCODE_R,
        [0x14] = SDL_SCANCODE_T,
        [0x15] = SDL_SCANCODE_Y,
        [0x16] = SDL_SCANCODE_U,
        [0x17] = SDL_SCANCODE_I,
        [0x18] = SDL_SCANCODE_O,
        [0x19] = SDL_SCANCODE_P,
        [0x1a] = SDL_SCANCODE_LEFTBRACKET,
        [0x1b] = SDL_SCANCODE_RIGHTBRACKET,
        [0x1c] = SDL_SCANCODE_RETURN,
        [0x1d] = SDL_SCANCODE_LCTRL,
        [0x1e] = SDL_SCANCODE_A,
        [0x1f] = SDL_SCANCODE_S,
        [0x20] = SDL_SCANCODE_D,
        [0x21] = SDL_SCANCODE_F,
        [0x22] = SDL_SCANCODE_G,
        [0x23] = SDL_SCANCODE_H,
        [0x24] = SDL_SCANCODE_J,
        [0x25] = SDL_SCANCODE_K,
        [0x26] = SDL_SCANCODE_L,
        [0x27] = SDL_SCANCODE_SEMICOLON,
        [0x28] = SDL_SCANCODE_APOSTROPHE,
        [0x29] = SDL_SCANCODE_GRAVE,
        [0x2a] = SDL_SCANCODE_LSHIFT,
        [0x2b] = SDL_SCANCODE_BACKSLASH,
        [0x2c] = SDL_SCANCODE_Z,
        [0x2d] = SDL_SCANCODE_X,
        [0x2e] = SDL_SCANCODE_C,
        [0x2f] = SDL_SCANCODE_V,
        [0x30] = SDL_SCANCODE_B,
        [0x31] = SDL_SCANCODE_N,
        [0x32] = SDL_SCANCODE_M,
        [0x33] = SDL_SCANCODE_COMMA,
        [0x34] = SDL_SCANCODE_PERIOD,
        [0x35] = SDL_SCANCODE_SLASH,
        [0x36] = SDL_SCANCODE_RSHIFT,
        [0x37] = SDL_SCANCODE_KP_MULTIPLY,
        [0x38] = SDL_SCANCODE_LALT,
        [0x39] = SDL_SCANCODE_SPACE,
        [0x3a] = SDL_SCANCODE_CAPSLOCK,
        [0x3b] = SDL_SCANCODE_F1,
        [0x3c] = SDL_SCANCODE_F2,
        [0x3d] = SDL_SCANCODE_F3,
        [0x3e] = SDL_SCANCODE_F4,
        [0x3f] = SDL_SCANCODE_F5,
        [0x40] = SDL_SCANCODE_F6,
        [0x41] = SDL_SCANCODE_F7,
        [0x42] = SDL_SCANCODE_F8,
        [0x43] = SDL_SCANCODE_F9,
        [0x44] = SDL_SCANCODE_F10,
        [0x45] = SDL_SCANCODE_NUMLOCKCLEAR,
        [0x46] = SDL_SCANCODE_SCROLLLOCK,
        [0x47] = SDL_SCANCODE_KP_7,
        [0x48] = SDL_SCANCODE_KP_8,
        [0x49] = SDL_SCANCODE_KP_9,
        [0x4a] = SDL_SCANCODE_KP_MINUS,
        [0x4b] = SDL_SCANCODE_KP_4,
        [0x4c] = SDL_SCANCODE_KP_5,
        [0x4d] = SDL_SCANCODE_KP_6,
        [0x4e] = SDL_SCANCODE_KP_PLUS,
        [0x4f] = SDL_SCANCODE_KP_1,
        [0x50] = SDL_SCANCODE_KP_2,
        [0x51] = SDL_SCANCODE_KP_3,
        [0x52] = SDL_SCANCODE_KP_0,
        [0x53] = SDL_SCANCODE_KP_PERIOD,
        [0x57] = SDL_SCANCODE_F11,
        [0x58] = SDL_SCANCODE_F12
    };

    return code < SDL_arraysize(map) ? map[code] : SDL_SCANCODE_UNKNOWN;
}

static void SAPOTE_DispatchEvent(SDL_Window *window,
                                 const struct sapote_event *event)
{
    switch (event->type) {
    case SAPOTE_EVENT_KEY:
    {
        const SDL_Scancode scancode =
            SAPOTE_TranslateScancode((Uint8)(event->code & 0xffU));
        const Uint8 character = (Uint8)((event->code >> 8U) & 0xffU);
        const SDL_Keycode keycode = character >= 0x20U && character <= 0x7eU ?
            (SDL_Keycode)character : SDLK_UNKNOWN;

        if (scancode != SDL_SCANCODE_UNKNOWN) {
            (void)SDL_SendKeyboardKeyAndKeycode(
                event->value == SAPOTE_KEY_RELEASED ? SDL_RELEASED :
                    SDL_PRESSED,
                scancode, keycode);
        }
        if (event->value == SAPOTE_KEY_PRESSED && character >= 0x20U &&
            character <= 0x7eU) {
            char text[2] = { (char)character, '\0' };
            (void)SDL_SendKeyboardText(text);
        }
        break;
    }
    case SAPOTE_EVENT_POINTER_MOVE:
        (void)SDL_SendMouseMotion(window, 0, SDL_FALSE, event->x, event->y);
        break;
    case SAPOTE_EVENT_POINTER_BUTTON:
        if (event->code >= SDL_BUTTON_LEFT &&
            event->code <= SDL_BUTTON_RIGHT) {
            (void)SDL_SendMouseButton(window, 0,
                event->value != 0U ? SDL_PRESSED : SDL_RELEASED,
                (Uint8)event->code);
        }
        break;
    case SAPOTE_EVENT_FOCUS:
        SDL_SetKeyboardFocus(event->value != 0U ? window : NULL);
        SDL_SetMouseFocus(event->value != 0U ? window : NULL);
        break;
    case SAPOTE_EVENT_CLOSE:
        (void)SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
        break;
    case SAPOTE_EVENT_QUEUE_OVERFLOW:
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
            "Sapote input queue overflowed; coalesced motion was discarded");
        break;
    default:
        break;
    }
}

static void SAPOTE_PumpEvents(_THIS)
{
    SDL_Window *window;

    for (window = _this->windows; window != NULL; window = window->next) {
        SapoteWindowData *data = (SapoteWindowData *)window->driverdata;
        struct sapote_event event;
        long result;

        if (data == NULL) {
            continue;
        }
        while ((result = sapote_event_read(data->events, &event)) > 0) {
            SAPOTE_DispatchEvent(window, &event);
        }
        if (result < 0 && result != -SAPOTE_EAGAIN) {
            SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                "Sapote event read failed: %ld", result);
        }
    }
}

static int SAPOTE_WaitEventTimeout(_THIS, int timeout)
{
    SapoteVideoData *video = (SapoteVideoData *)_this->driverdata;
    struct sapote_wait_item items[2];
    SDL_Window *window;
    size_t count = 0;
    uint64_t deadline;
    long result;

    for (window = _this->windows; window != NULL; window = window->next) {
        SapoteWindowData *data = (SapoteWindowData *)window->driverdata;
        if (data != NULL) {
            items[count].handle = data->events;
            items[count].interests = SAPOTE_WAIT_READABLE;
            items[count].ready = 0U;
            ++count;
            break;
        }
    }
    if (video != NULL && video->wakeup_timer != 0U) {
        items[count].handle = video->wakeup_timer;
        items[count].interests = SAPOTE_WAIT_SIGNALED;
        items[count].ready = 0U;
        ++count;
    }
    if (count == 0U) {
        if (timeout > 0) {
            SDL_Delay((Uint32)timeout);
        }
        return 0;
    }
    if (timeout < 0) {
        deadline = UINT64_MAX;
    } else {
        const uint64_t now = sapote_monotonic_ns();
        const uint64_t delta = (uint64_t)(Uint32)timeout * UINT64_C(1000000);
        deadline = delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
    }
    result = sapote_wait(items, count, deadline);
    if (video != NULL && video->wakeup_timer != 0U) {
        (void)sapote_timer_set(video->wakeup_timer, 0U);
    }
    if (result == -SAPOTE_ETIMEDOUT) {
        return 0;
    }
    if (result < 0) {
        return SDL_SetError("Sapote event wait failed: %ld", result);
    }
    SAPOTE_PumpEvents(_this);
    return 1;
}

static void SAPOTE_SendWakeupEvent(_THIS, SDL_Window *window)
{
    SapoteVideoData *video = (SapoteVideoData *)_this->driverdata;
    (void)window;

    if (video != NULL && video->wakeup_timer != 0U) {
        (void)sapote_timer_set(video->wakeup_timer, sapote_monotonic_ns());
    }
}

static int SAPOTE_VideoInit(_THIS)
{
    SapoteVideoData *video = (SapoteVideoData *)_this->driverdata;
    SDL_DisplayMode mode;
    long timer;

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = 1024;
    mode.h = 768;
    mode.refresh_rate = 60;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);
    timer = sapote_timer_create();
    if (timer < 0) {
        return SDL_SetError("Sapote wake timer creation failed: %ld", timer);
    }
    video->wakeup_timer = (sapote_handle_t)timer;
    return 0;
}

static void SAPOTE_VideoQuit(_THIS)
{
    SapoteVideoData *video = (SapoteVideoData *)_this->driverdata;

    if (video != NULL && video->wakeup_timer != 0U) {
        (void)sapote_handle_close(video->wakeup_timer);
        video->wakeup_timer = 0U;
    }
}

static int SAPOTE_CreateWindow(_THIS, SDL_Window *window)
{
    struct sapote_window_create_response response;
    SapoteWindowData *data;
    char title[SAPOTE_WINDOW_TITLE_MAX + 1U];
    int status;
    (void)_this;

    if (window->w <= 0 || window->h <= 0) {
        return SDL_SetError("Sapote window dimensions must be positive");
    }
    SDL_strlcpy(title, window->title != NULL ? window->title : "SDL",
        sizeof(title));
    SDL_zero(response);
    status = sapote_window_create(title, (Uint32)window->w,
        (Uint32)window->h, &response);
    if (status != 0) {
        return SDL_SetError("Sapote window creation failed: %d", status);
    }
    if (response.width != (Uint32)window->w ||
        response.height != (Uint32)window->h ||
        response.pixel_format != SAPOTE_PIXEL_XRGB8888 ||
        response.surface_address == 0U ||
        response.stride_bytes < response.width * sizeof(Uint32)) {
        (void)sapote_handle_close(response.events);
        (void)sapote_handle_close(response.window);
        return SDL_SetError("Sapote returned an incompatible window surface");
    }
    data = (SapoteWindowData *)SDL_calloc(1, sizeof(*data));
    if (data == NULL) {
        (void)sapote_handle_close(response.events);
        (void)sapote_handle_close(response.window);
        return SDL_OutOfMemory();
    }
    data->window = response.window;
    data->events = response.events;
    data->pixels = (Uint32 *)(uintptr_t)response.surface_address;
    data->width = response.width;
    data->height = response.height;
    data->pitch = response.stride_bytes;
    window->driverdata = data;
    SDL_SetKeyboardFocus(window);
    SDL_SetMouseFocus(window);
    return 0;
}

static void SAPOTE_DestroyWindow(_THIS, SDL_Window *window)
{
    SapoteWindowData *data = (SapoteWindowData *)window->driverdata;
    (void)_this;

    if (data != NULL) {
        (void)sapote_handle_close(data->events);
        (void)sapote_handle_close(data->window);
        SDL_free(data);
        window->driverdata = NULL;
    }
}

static int SAPOTE_CreateWindowFramebuffer(_THIS, SDL_Window *window,
                                          Uint32 *format, void **pixels,
                                          int *pitch)
{
    SapoteWindowData *data = (SapoteWindowData *)window->driverdata;
    (void)_this;

    if (data == NULL || data->pitch > SDL_MAX_SINT32) {
        return SDL_SetError("Sapote window framebuffer is unavailable");
    }
    *format = SDL_PIXELFORMAT_RGB888;
    *pixels = data->pixels;
    *pitch = (int)data->pitch;
    return 0;
}

static int SAPOTE_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
                                          const SDL_Rect *rects,
                                          int numrects)
{
    SapoteWindowData *data = (SapoteWindowData *)window->driverdata;
    const SDL_Rect bounds = { 0, 0, window->w, window->h };
    struct sapote_rect damage[SAPOTE_DAMAGE_MAX];
    size_t damage_count = 0U;
    int index;
    (void)_this;

    if (data == NULL || (rects == NULL && numrects != 0) || numrects < 0) {
        return SDL_SetError("Invalid Sapote framebuffer update");
    }
    for (index = 0; index < numrects; ++index) {
        SDL_Rect clipped;
        if (SDL_IntersectRect(&bounds, &rects[index], &clipped) != SDL_TRUE) {
            continue;
        }
        damage[damage_count].x = (Uint32)clipped.x;
        damage[damage_count].y = (Uint32)clipped.y;
        damage[damage_count].width = (Uint32)clipped.w;
        damage[damage_count].height = (Uint32)clipped.h;
        ++damage_count;
        if (damage_count == SDL_arraysize(damage)) {
            const long result = sapote_surface_present(data->window, damage,
                damage_count);
            if (result < 0) {
                return SDL_SetError("Sapote partial present failed: %ld",
                    result);
            }
            damage_count = 0U;
        }
    }
    if (damage_count != 0U) {
        const long result = sapote_surface_present(data->window, damage,
            damage_count);
        if (result < 0) {
            return SDL_SetError("Sapote partial present failed: %ld", result);
        }
    }
    return 0;
}

static void SAPOTE_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    (void)_this;
    (void)window;
}

static void SAPOTE_SetWindowMouseGrab(_THIS, SDL_Window *window,
                                      SDL_bool grabbed)
{
    SapoteWindowData *data = (SapoteWindowData *)window->driverdata;
    (void)_this;

    if (data != NULL) {
        (void)sapote_pointer_capture(data->window, grabbed == SDL_TRUE);
    }
}

static int SAPOTE_CaptureMouse(SDL_Window *window)
{
    SDL_Window *target = window != NULL ? window : SDL_GetMouseFocus();
    SapoteWindowData *data = target != NULL ?
        (SapoteWindowData *)target->driverdata : NULL;
    const long result = data != NULL ?
        sapote_pointer_capture(data->window, window != NULL) : 0;

    return result < 0 ? SDL_SetError("Sapote pointer capture failed: %ld",
        result) : 0;
}

static void SAPOTE_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->driverdata);
    SDL_free(device);
}

static SDL_VideoDevice *SAPOTE_CreateDevice(void)
{
    SDL_VideoDevice *device =
        (SDL_VideoDevice *)SDL_calloc(1, sizeof(*device));
    SapoteVideoData *video;

    if (device == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    video = (SapoteVideoData *)SDL_calloc(1, sizeof(*video));
    if (video == NULL) {
        SDL_free(device);
        SDL_OutOfMemory();
        return NULL;
    }
    device->driverdata = video;
    device->VideoInit = SAPOTE_VideoInit;
    device->VideoQuit = SAPOTE_VideoQuit;
    device->CreateSDLWindow = SAPOTE_CreateWindow;
    device->DestroyWindow = SAPOTE_DestroyWindow;
    device->CreateWindowFramebuffer = SAPOTE_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SAPOTE_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SAPOTE_DestroyWindowFramebuffer;
    device->PumpEvents = SAPOTE_PumpEvents;
    device->WaitEventTimeout = SAPOTE_WaitEventTimeout;
    device->SendWakeupEvent = SAPOTE_SendWakeupEvent;
    device->SetWindowMouseGrab = SAPOTE_SetWindowMouseGrab;
    device->free = SAPOTE_DeleteDevice;
    SDL_GetMouse()->CaptureMouse = SAPOTE_CaptureMouse;
    return device;
}

VideoBootStrap SAPOTE_bootstrap = {
    SAPOTE_VIDEO_DRIVER_NAME,
    "Sapote Redwood window and input driver",
    SAPOTE_CreateDevice,
    NULL
};

#endif /* SDL_VIDEO_DRIVER_SAPOTE */
