// SPDX-License-Identifier: GPL-3.0-only
//! Where C calls Rust.
//!
//! Every function here is `extern "C"` and every one of them is the boundary
//! at which Rust's guarantees stop: a caller that passes a bad pointer or a
//! wrong length gets exactly the same undefined behaviour it would get from a
//! C callee. So the boundary is kept as small as it can be - raw pointers turn
//! into slices immediately, once, and everything past that point is safe Rust.
//!
//! Unsafe blocks appear only where validated C pointers become Rust slices or
//! where results are written back through validated C pointers. Each one states
//! the condition the caller has to meet.

use crate::elf64;
use crate::ext4;
use crate::fat16;
use crate::fat32;
use crate::font;
use crate::linux_elf64;
use crate::linux_fat16;
use crate::logo::{self, Format, Status};
use crate::native_image;
use crate::nvbios;
use crate::wallpaper;
use crate::ui_font;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::ptr::null_mut;

const HEAP_ALIGNMENT: usize = 16;
const HEAP_STATUS_OK: i32 = 0;

struct KernelAllocator;

// SAFETY: `heap_allocate` returns distinct 16-byte-aligned allocations and
// `heap_free` accepts exactly those pointers. Sapote serializes kernel entry
// today; the C allocator owns its own integrity checks as threading expands.
unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe extern "C" {
            fn heap_allocate(size: u64, pointer: *mut *mut c_void) -> i32;
        }

        if layout.size() == 0 || layout.align() > HEAP_ALIGNMENT {
            return null_mut();
        }
        let mut pointer = null_mut();
        // SAFETY: `pointer` is writable and the size is non-zero. The C heap
        // writes either one owned allocation or null and returns a status.
        let status = unsafe {
            heap_allocate(layout.size() as u64, &mut pointer)
        };
        if status == HEAP_STATUS_OK {
            pointer.cast()
        } else {
            null_mut()
        }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, _layout: Layout) {
        unsafe extern "C" {
            fn heap_free(pointer: *mut c_void) -> i32;
        }

        // SAFETY: `GlobalAlloc` requires the caller to pass a live pointer
        // returned by this allocator. Treat allocator corruption as fatal.
        if unsafe { heap_free(pointer.cast()) } != HEAP_STATUS_OK {
            panic();
        }
    }
}

#[global_allocator]
static KERNEL_ALLOCATOR: KernelAllocator = KernelAllocator;

/// Read one checked byte range through the active C-owned ext4 probe session.
pub(crate) fn ext4_block_read(
    context: usize,
    start_byte: u64,
    destination: &mut [u8],
) -> bool {
    unsafe extern "C" {
        fn sapote_ext4_block_read(
            context: usize,
            start_byte: u64,
            destination: *mut u8,
            length: usize,
        ) -> i32;
    }

    // SAFETY: the slice supplies a live writable region for its exact length;
    // C authenticates the context and checks the media bounds.
    unsafe {
        sapote_ext4_block_read(
            context,
            start_byte,
            destination.as_mut_ptr(),
            destination.len(),
        ) == 0
    }
}

const _: () = {
    assert!(fat32::Status::Count as i32 == 37);
    assert!(core::mem::size_of::<fat32::Geometry>() == 96);
    assert!(core::mem::align_of::<fat32::Geometry>() == 8);
    assert!(core::mem::offset_of!(fat32::Geometry, total_sectors) == 16);
    assert!(core::mem::offset_of!(fat32::Geometry, cluster_count) == 48);
    assert!(core::mem::offset_of!(fat32::Geometry, volume_label) == 76);
    assert!(core::mem::offset_of!(fat32::Geometry, valid) == 88);
    assert!(core::mem::size_of::<fat32::FsInfo>() == 16);
    assert!(core::mem::size_of::<fat32::DirectoryEntry>() == 28);
    assert!(core::mem::offset_of!(fat32::DirectoryEntry, first_cluster) == 16);
    assert!(core::mem::size_of::<fat32::Name>() == 16);

    assert!(core::mem::size_of::<linux_fat16::Chain>() == 5136);
    assert!(core::mem::align_of::<linux_fat16::Chain>() == 8);
    assert!(core::mem::offset_of!(linux_fat16::Chain, clusters) == 0);
    assert!(core::mem::offset_of!(linux_fat16::Chain, lbas) == 1024);
    assert!(core::mem::offset_of!(linux_fat16::Chain, cluster_count) == 5120);
    assert!(core::mem::offset_of!(linux_fat16::Chain, file_bytes) == 5124);
    assert!(core::mem::offset_of!(linux_fat16::Chain, final_cluster_bytes) == 5128);
    assert!(core::mem::offset_of!(linux_fat16::Chain, valid) == 5132);

    assert!(core::mem::size_of::<nvbios::Image>() == 24);
    assert!(core::mem::align_of::<nvbios::Image>() == 4);
    assert!(core::mem::offset_of!(nvbios::Image, image_bytes) == 0);
    assert!(core::mem::offset_of!(nvbios::Image, pcir_offset) == 4);
    assert!(core::mem::offset_of!(nvbios::Image, bit_offset) == 8);
    assert!(core::mem::offset_of!(nvbios::Image, vendor_id) == 12);
    assert!(core::mem::offset_of!(nvbios::Image, device_id) == 14);
    assert!(core::mem::offset_of!(nvbios::Image, class_code) == 16);
    assert!(core::mem::offset_of!(nvbios::Image, subclass) == 17);
    assert!(core::mem::offset_of!(nvbios::Image, programming_interface) == 18);
    assert!(core::mem::offset_of!(nvbios::Image, code_type) == 19);
    assert!(core::mem::offset_of!(nvbios::Image, bit_tokens) == 20);
    assert!(core::mem::offset_of!(nvbios::Image, bit_token_bytes) == 21);
    assert!(core::mem::offset_of!(nvbios::Image, last_image) == 22);

    assert!(core::mem::size_of::<linux_fat16::Payload>() == 40);
    assert!(core::mem::align_of::<linux_fat16::Payload>() == 4);
    assert!(core::mem::offset_of!(linux_fat16::Payload, sha256) == 0);
    assert!(core::mem::offset_of!(linux_fat16::Payload, byte_count) == 32);
    assert!(core::mem::offset_of!(linux_fat16::Payload, deterministic) == 36);

    assert!(core::mem::size_of::<linux_elf64::Segment>() == 56);
    assert!(core::mem::align_of::<linux_elf64::Segment>() == 8);
    assert!(core::mem::offset_of!(linux_elf64::Segment, file_offset) == 0);
    assert!(core::mem::offset_of!(linux_elf64::Segment, virtual_address) == 8);
    assert!(core::mem::offset_of!(linux_elf64::Segment, file_size) == 16);
    assert!(core::mem::offset_of!(linux_elf64::Segment, memory_size) == 24);
    assert!(core::mem::offset_of!(linux_elf64::Segment, mapping_start) == 32);
    assert!(core::mem::offset_of!(linux_elf64::Segment, mapping_end) == 40);
    assert!(core::mem::offset_of!(linux_elf64::Segment, flags) == 48);
    assert!(core::mem::offset_of!(linux_elf64::Segment, reserved) == 52);

    assert!(core::mem::size_of::<linux_elf64::ValidatedImage>() == 248);
    assert!(core::mem::align_of::<linux_elf64::ValidatedImage>() == 8);
    assert!(core::mem::offset_of!(linux_elf64::ValidatedImage, valid) == 0);
    assert!(core::mem::offset_of!(linux_elf64::ValidatedImage, program_header_count) == 4);
    assert!(core::mem::offset_of!(linux_elf64::ValidatedImage, segment_count) == 8);
    assert!(core::mem::offset_of!(linux_elf64::ValidatedImage, non_load_count) == 12);
    assert!(core::mem::offset_of!(linux_elf64::ValidatedImage, entry) == 16);
    assert!(core::mem::offset_of!(linux_elf64::ValidatedImage, segments) == 24);

    assert!(native_image::Status::DigestMismatch as i32 == 30);
    assert!(core::mem::size_of::<native_image::Manifest>() == 432);
    assert!(core::mem::align_of::<native_image::Manifest>() == 8);
    assert!(core::mem::offset_of!(native_image::Manifest, capabilities) == 8);
    assert!(core::mem::offset_of!(native_image::Manifest, name) == 32);
    assert!(core::mem::offset_of!(native_image::Manifest, arguments) == 176);
    assert!(core::mem::size_of::<native_image::Tls>() == 40);
    assert!(core::mem::size_of::<native_image::Segment>() == 56);
    assert!(core::mem::size_of::<native_image::ValidatedImage>() == 976);
    assert!(core::mem::offset_of!(native_image::ValidatedImage, tls) == 40);
    assert!(core::mem::offset_of!(native_image::ValidatedImage, segments) == 80);
};

/// Stop in C's console panic path if a compiler-inserted check ever fires.
pub(crate) fn panic() -> ! {
    unsafe extern "C" {
        fn console_panic(message: *const u8) -> !;
    }

    // SAFETY: this is a static NUL-terminated string and the C function never
    // returns. Keeping this declaration here preserves the one unsafe module.
    unsafe { console_panic(c"Rust panicked".as_ptr() as *const u8) }
}

/// Validate the ext4 volume behind a temporary native block-I/O context.
///
/// # Safety
/// `identity` must point to writable storage for one [`ext4::Identity`]. The
/// context must remain valid for the duration of this synchronous call.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_ext4_probe(
    context: usize,
    identity: *mut ext4::Identity,
) -> i32 {
    if context == 0 || identity.is_null() {
        return 1;
    }
    match ext4::probe(context) {
        Ok(value) => {
            // SAFETY: the checked pointer is the function contract.
            unsafe { *identity = value };
            0
        }
        Err(ext4::ProbeError::Io) => 3,
        Err(ext4::ProbeError::Invalid) => 4,
    }
}

/// The run-length image, produced by `tools/make-logo-asset.py` at build time.
/// The Makefile points `SAPOTE_LOGO_BLOB` at it; there is no committed copy.
static LOGO: &[u8] = include_bytes!(env!("SAPOTE_LOGO_BLOB"));
static STUDIO_ICON: &[u8] = include_bytes!(env!("SAPOTE_STUDIO_ICON_BLOB"));
static SETTINGS_ICON: &[u8] = include_bytes!(env!("SAPOTE_SETTINGS_ICON_BLOB"));
static FILES_ICON: &[u8] = include_bytes!(env!("SAPOTE_FILES_ICON_BLOB"));
static TERMINAL_ICON: &[u8] = include_bytes!(env!("SAPOTE_TERMINAL_ICON_BLOB"));
static CAMERA_ICON: &[u8] = include_bytes!(env!("SAPOTE_CAMERA_ICON_BLOB"));
static CANVAS_ICON: &[u8] = include_bytes!(env!("SAPOTE_CANVAS_ICON_BLOB"));
static STORE_ICON: &[u8] = include_bytes!(env!("SAPOTE_STORE_ICON_BLOB"));
static STORE_UI_ICONS: &[u8] =
    include_bytes!(env!("SAPOTE_STORE_UI_ICONS_BLOB"));
static SETTINGS_CATEGORY_ICONS: &[u8] =
    include_bytes!(env!("SAPOTE_SETTINGS_CATEGORY_ICONS_BLOB"));

/// The deterministic RGB565 wallpaper collection built from committed PNGs.
static WALLPAPER: &[u8] = include_bytes!(env!("SAPOTE_WALLPAPER_BLOB"));

/// Run the SPW3 decoder's production-asset and bounded refusal checks.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_wallpaper_self_test() -> i32 {
    i32::from(wallpaper::self_test(WALLPAPER))
}

/// Return the exact byte length of the built-in SPW3 collection.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_wallpaper_size() -> usize {
    WALLPAPER.len()
}

/// Copy the checked dimensions out through the C ABI.
///
/// # Safety
/// Both pointers must address writable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_wallpaper_geometry(
    width: *mut u32,
    height: *mut u32,
    frames: *mut u32,
) -> i32 {
    if width.is_null() || height.is_null() || frames.is_null() {
        return wallpaper::Status::NullArgument as i32;
    }
    match wallpaper::geometry(WALLPAPER) {
        Ok(geometry) => {
            // SAFETY: non-null writable pointers are the function contract.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
                *frames = geometry.frames;
            }
            wallpaper::Status::Ok as i32
        }
        Err(status) => status as i32,
    }
}

/// Decode the wallpaper into packed pixels owned by the caller.
///
/// # Safety
/// `out` must point to `out_pixels` writable, aligned, non-aliased `u32`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_wallpaper_decode(
    frame: u32,
    out: *mut u32,
    out_pixels: usize,
    out_width: u32,
    out_height: u32,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
) -> i32 {
    if out.is_null() {
        return wallpaper::Status::NullArgument as i32;
    }
    // SAFETY: the checked C contract above is exactly this slice's contract.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };
    let format = wallpaper::Format { red_shift, green_shift, blue_shift };
    match wallpaper::decode(
        WALLPAPER, frame, pixels, out_width, out_height, &format
    ) {
        Ok(_) => wallpaper::Status::Ok as i32,
        Err(status) => status as i32,
    }
}

fn status_code(status: Status) -> i32 {
    status as i32
}

/// Run the decoder's own tests. Returns 1 when they all pass.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_logo_self_test() -> i32 {
    i32::from(logo::self_test())
}

/// How many bytes the built-in image occupies.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_logo_size() -> usize {
    LOGO.len()
}

/// Read the built-in image's declared size without decoding it.
///
/// # Safety
///
/// `width` and `height` must both be non-null and point at writable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_logo_geometry(width: *mut u32, height: *mut u32) -> i32 {
    if width.is_null() || height.is_null() {
        return status_code(Status::NullArgument);
    }

    match logo::geometry(LOGO) {
        Ok(geometry) => {
            // SAFETY: both pointers were checked non-null just above, and the
            // caller's contract is that each addresses a writable u32.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
            }
            status_code(Status::Ok)
        }
        Err(status) => status_code(status),
    }
}

/// Decode the built-in image into `out`, one packed pixel per element.
///
/// `out` is filled row by row and must hold exactly `out_pixels` writable
/// `u32`s. The channel shifts and background come from the framebuffer, so
/// nothing here assumes a byte order.
///
/// # Safety
///
/// `out` must point at `out_pixels` writable, aligned `u32`s, and must not
/// alias anything else live for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_logo_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }

    // SAFETY: the caller's contract is exactly the requirement of
    // from_raw_parts_mut - out_pixels writable, aligned, non-aliased u32s -
    // and the null case was refused above. This is the only place in the crate
    // where a pointer becomes a slice; everything below it is bounds checked.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };

    let format = Format {
        red_shift,
        green_shift,
        blue_shift,
        background,
    };

    match logo::decode(LOGO, pixels, &format) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

/// Decode the built-in image's alpha channel without flattening it.
///
/// # Safety
///
/// `out` must point at `out_pixels` writable, non-aliased bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_logo_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }

    // SAFETY: the caller supplies the writable extent and the null case was
    // refused above. The decoder performs all subsequent bounds checks.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };
    match logo::decode_alpha(LOGO, pixels) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

/// Read the built-in SapStudio icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_studio_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    if width.is_null() || height.is_null() {
        return status_code(Status::NullArgument);
    }
    match logo::geometry(STUDIO_ICON) {
        Ok(geometry) => {
            // SAFETY: both writable pointers were checked above.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
            }
            status_code(Status::Ok)
        }
        Err(status) => status_code(status),
    }
}

/// Decode the built-in SapStudio icon over the supplied background.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_studio_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }
    // SAFETY: the caller supplies the writable extent and null was refused.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };
    let format = Format {
        red_shift,
        green_shift,
        blue_shift,
        background,
    };
    match logo::decode(STUDIO_ICON, pixels, &format) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

/// Decode the built-in SapStudio icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_studio_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }
    // SAFETY: the caller supplies the writable extent and null was refused.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };
    match logo::decode_alpha(STUDIO_ICON, pixels) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

unsafe fn app_icon_geometry(icon: &[u8], width: *mut u32, height: *mut u32) -> i32 {
    if width.is_null() || height.is_null() {
        return status_code(Status::NullArgument);
    }
    match logo::geometry(icon) {
        Ok(geometry) => {
            // SAFETY: both writable pointers were checked above.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
            }
            status_code(Status::Ok)
        }
        Err(status) => status_code(status),
    }
}

unsafe fn app_icon_decode(
    icon: &[u8],
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }
    // SAFETY: the caller supplies the writable extent and null was refused.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };
    let format = Format { red_shift, green_shift, blue_shift, background };
    match logo::decode(icon, pixels, &format) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

unsafe fn app_icon_decode_alpha(icon: &[u8], out: *mut u8, out_pixels: usize) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }
    // SAFETY: the caller supplies the writable extent and null was refused.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };
    match logo::decode_alpha(icon, pixels) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

/// Read the exact classic Settings icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_settings_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(SETTINGS_ICON, width, height) }
}

/// Decode the exact classic Settings icon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_settings_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(SETTINGS_ICON, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the exact classic Settings icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_settings_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(SETTINGS_ICON, out, out_pixels) }
}

/// Read the checked Files icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_files_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(FILES_ICON, width, height) }
}

/// Decode the checked Files icon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_files_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(FILES_ICON, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the checked Files icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_files_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(FILES_ICON, out, out_pixels) }
}

/// Read the checked Terminal icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_terminal_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(TERMINAL_ICON, width, height) }
}

/// Decode the checked Terminal icon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_terminal_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(TERMINAL_ICON, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the checked Terminal icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_terminal_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(TERMINAL_ICON, out, out_pixels) }
}

/// Read the checked Settings category sprite geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_settings_category_icons_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(SETTINGS_CATEGORY_ICONS, width, height) }
}

/// Decode the Settings category sprite.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_settings_category_icons_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(SETTINGS_CATEGORY_ICONS, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the Settings category sprite alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_settings_category_icons_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(SETTINGS_CATEGORY_ICONS, out, out_pixels) }
}

/// Read the exact classic Camera icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_camera_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(CAMERA_ICON, width, height) }
}

/// Decode the exact classic Camera icon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_camera_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(CAMERA_ICON, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the exact classic Camera icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_camera_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(CAMERA_ICON, out, out_pixels) }
}

/// Read the checked Canvas application icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_canvas_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(CANVAS_ICON, width, height) }
}

/// Decode the checked Canvas application icon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_canvas_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(CANVAS_ICON, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the checked Canvas application icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_canvas_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(CANVAS_ICON, out, out_pixels) }
}

/// Read the checked Sapote Store application icon geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_store_icon_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(STORE_ICON, width, height) }
}

/// Decode the checked Sapote Store application icon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_store_icon_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(STORE_ICON, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the checked Sapote Store application icon alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_store_icon_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(STORE_ICON, out, out_pixels) }
}

/// Read the checked monochrome Lucide Store sprite geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_store_ui_icons_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_geometry(STORE_UI_ICONS, width, height) }
}

/// Decode the checked monochrome Lucide Store sprite.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_store_ui_icons_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe {
        app_icon_decode(STORE_UI_ICONS, out, out_pixels, red_shift,
            green_shift, blue_shift, background)
    }
}

/// Decode the checked monochrome Lucide Store sprite alpha channel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_store_ui_icons_decode_alpha(
    out: *mut u8,
    out_pixels: usize,
) -> i32 {
    // SAFETY: forwarded unchanged to the checked pointer boundary.
    unsafe { app_icon_decode_alpha(STORE_UI_ICONS, out, out_pixels) }
}

/// The packed glyph table, produced by `tools/make-font-asset.py` at build
/// time. The Makefile points `SAPOTE_FONT_BLOB` at it; there is no committed
/// copy of the blob, only the ASCII art it is built from.
static FONT: &[u8] = include_bytes!(env!("SAPOTE_FONT_BLOB"));

fn font_status_code(status: font::Status) -> i32 {
    status as i32
}

/// Run the font reader's own tests. Returns 1 when they all pass.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_font_self_test() -> i32 {
    i32::from(font::self_test())
}

/// How many bytes the built-in glyph table occupies.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_font_size() -> usize {
    FONT.len()
}

/// Read the glyph table's cell size and covered range without copying a glyph.
///
/// # Safety
///
/// Each pointer must be non-null and address a writable `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_font_geometry(
    width: *mut u32,
    height: *mut u32,
    first: *mut u32,
    count: *mut u32,
) -> i32 {
    if width.is_null() || height.is_null() || first.is_null() || count.is_null() {
        return font_status_code(font::Status::NullArgument);
    }

    match font::geometry(FONT) {
        Ok(geometry) => {
            // SAFETY: all four pointers were checked non-null just above, and
            // the caller's contract is that each addresses a writable u32.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
                *first = geometry.first;
                *count = geometry.count;
            }
            font_status_code(font::Status::Ok)
        }
        Err(status) => font_status_code(status),
    }
}

/// Copy one glyph's rows into `out`, one byte per row, leftmost pixel in the
/// most significant bit. Writes `height` bytes and no more.
///
/// # Safety
///
/// `out` must point at `out_len` writable bytes and must not alias anything
/// else live for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_font_glyph(code: u32, out: *mut u8, out_len: usize) -> i32 {
    if out.is_null() {
        return font_status_code(font::Status::NullArgument);
    }

    // SAFETY: the caller's contract is exactly the requirement of
    // from_raw_parts_mut - out_len writable, non-aliased bytes - and the null
    // case was refused above. Everything past this line is bounds checked.
    let rows = unsafe { core::slice::from_raw_parts_mut(out, out_len) };

    match font::glyph(FONT, code, rows) {
        Ok(_) => font_status_code(font::Status::Ok),
        Err(status) => font_status_code(status),
    }
}

/// Build-packed antialiased Inter glyphs. No TrueType parser enters the kernel.
static UI_FONT: &[u8] = include_bytes!(env!("SAPOTE_UI_FONT_BLOB"));

fn ui_font_status_code(status: ui_font::Status) -> i32 {
    status as i32
}

/// Run the SUF2 parser's synthetic acceptance and refusal tests.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_ui_font_self_test() -> i32 {
    i32::from(ui_font::self_test())
}

/// Return the byte length of the built-in SUF2 asset.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_ui_font_size() -> usize {
    UI_FONT.len()
}

/// Return a stable FNV-1a fingerprint of the exact built-in bytes.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_ui_font_fingerprint() -> u64 {
    ui_font::fingerprint(UI_FONT)
}

/// Validate the built-in asset and copy all declared metrics.
///
/// # Safety
///
/// `metrics` must be non-null and point to one writable `ui_font::Geometry`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_ui_font_geometry(metrics: *mut ui_font::Geometry) -> i32 {
    if metrics.is_null() {
        return ui_font_status_code(ui_font::Status::NullArgument);
    }
    match ui_font::geometry(UI_FONT) {
        Ok(value) => {
            // SAFETY: the pointer was checked above; the caller owns one
            // writable value with the same repr(C) layout.
            unsafe { *metrics = value };
            ui_font_status_code(ui_font::Status::Ok)
        }
        Err(status) => ui_font_status_code(status),
    }
}

/// Copy one glyph alpha bitmap from SUF2 into a caller-owned buffer.
///
/// # Safety
///
/// `out` must address `out_len` writable, non-aliased bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_ui_font_glyph(code: u32, out: *mut u8, out_len: usize) -> i32 {
    if out.is_null() {
        return ui_font_status_code(ui_font::Status::NullArgument);
    }
    // SAFETY: this is the caller's contract, and the null case was refused.
    let bytes = unsafe { core::slice::from_raw_parts_mut(out, out_len) };
    match ui_font::glyph(UI_FONT, code, bytes) {
        Ok(_) => ui_font_status_code(ui_font::Status::Ok),
        Err(status) => ui_font_status_code(status),
    }
}

/// Copy one glyph's proportional advance through the C ABI.
///
/// # Safety
/// `out` must address one writable `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_ui_font_glyph_advance(code: u32, out: *mut u32) -> i32 {
    if out.is_null() {
        return ui_font_status_code(ui_font::Status::NullArgument);
    }
    match ui_font::advance(UI_FONT, code) {
        Ok(value) => {
            // SAFETY: the non-null pointer names one caller-owned value.
            unsafe { *out = value };
            ui_font_status_code(ui_font::Status::Ok)
        }
        Err(status) => ui_font_status_code(status),
    }
}

fn fat32_status_code(status: fat32::Status) -> i32 {
    status as i32
}

/// Parse and validate one CPU-owned FAT32 boot sector.
///
/// # Safety
///
/// `block` must address `block_len` readable bytes and `out` one writable
/// geometry value. The ranges must not overlap.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_parse_bpb(
    block: *const u8,
    block_len: usize,
    namespace_blocks: u64,
    namespace_block_bytes: u32,
    out: *mut fat32::Geometry,
) -> i32 {
    if out.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Geometry and null was refused.
    unsafe { *out = fat32::Geometry::invalid() };
    if block.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(block, block_len) };
    match fat32::parse_bpb(bytes, namespace_blocks, namespace_block_bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat32_status_code(fat32::Status::Ok)
        }
        Err(status) => fat32_status_code(status),
    }
}

/// Parse checked FSInfo hints without treating either hint as authoritative.
///
/// # Safety
///
/// `block` must address `block_len` readable bytes, `geometry` one readable
/// value, and `out` one writable result. No input may overlap the output.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_parse_fsinfo(
    block: *const u8,
    block_len: usize,
    geometry: *const fat32::Geometry,
    out: *mut fat32::FsInfo,
) -> i32 {
    if out.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable result and null was refused.
    unsafe { *out = fat32::FsInfo::invalid() };
    if block.is_null() || geometry.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises both readable inputs and non-overlap.
    let (bytes, checked_geometry) = unsafe {
        (core::slice::from_raw_parts(block, block_len), *geometry)
    };
    match fat32::parse_fsinfo(bytes, &checked_geometry) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat32_status_code(fat32::Status::Ok)
        }
        Err(status) => fat32_status_code(status),
    }
}

/// Compare mirrored FAT sectors and validate their reserved entries.
///
/// # Safety
///
/// Each block pointer must address its stated readable length and `geometry`
/// must address one readable value.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_validate_fat_pair(
    first: *const u8,
    first_len: usize,
    second: *const u8,
    second_len: usize,
    geometry: *const fat32::Geometry,
) -> i32 {
    if first.is_null() || second.is_null() || geometry.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises all three readable ranges.
    let (first_bytes, second_bytes, checked_geometry) = unsafe {
        (
            core::slice::from_raw_parts(first, first_len),
            core::slice::from_raw_parts(second, second_len),
            *geometry,
        )
    };
    match fat32::validate_fat_pair(first_bytes, second_bytes, &checked_geometry) {
        Ok(()) => fat32_status_code(fat32::Status::Ok),
        Err(status) => fat32_status_code(status),
    }
}

/// Classify one masked FAT32 link value.
///
/// # Safety
///
/// `geometry` must address one readable value and `out` one writable `u32`.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_classify_cluster(
    value: u32,
    geometry: *const fat32::Geometry,
    out: *mut u32,
) -> i32 {
    if out.is_null() || geometry.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the output pointer was checked and the caller owns it.
    unsafe { *out = 0 };
    // SAFETY: the caller promises one readable Geometry.
    let checked_geometry = unsafe { *geometry };
    match fat32::classify_cluster(value, &checked_geometry) {
        Ok(cluster) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = cluster };
            fat32_status_code(fat32::Status::Ok)
        }
        Err(status) => fat32_status_code(status),
    }
}

/// Canonicalize one bounded ASCII 8.3 path component.
///
/// # Safety
///
/// `component` must address `component_len` readable bytes and `out` one
/// writable result. The ranges must not overlap.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_parse_component(
    component: *const u8,
    component_len: usize,
    out: *mut fat32::Name,
) -> i32 {
    if out.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Name and null was refused.
    unsafe { *out = fat32::Name::invalid() };
    if component.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range.
    let bytes = unsafe { core::slice::from_raw_parts(component, component_len) };
    match fat32::parse_component(bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat32_status_code(fat32::Status::Ok)
        }
        Err(status) => fat32_status_code(status),
    }
}

/// Validate a complete relative FAT32 path and return its component count.
///
/// # Safety
///
/// `path` must address `path_len` readable bytes and `component_count` one
/// writable `u32`. The ranges must not overlap.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_validate_path(
    path: *const u8,
    path_len: usize,
    component_count: *mut u32,
) -> i32 {
    if component_count.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable value and null was refused.
    unsafe { *component_count = 0 };
    if path.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range.
    let bytes = unsafe { core::slice::from_raw_parts(path, path_len) };
    match fat32::validate_path(bytes) {
        Ok(count) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *component_count = count };
            fat32_status_code(fat32::Status::Ok)
        }
        Err(status) => fat32_status_code(status),
    }
}

/// Parse one ordinary, deleted, end, or refused FAT32 directory entry.
///
/// # Safety
///
/// `entry` must address `entry_len` readable bytes and `out` one writable
/// result. The ranges must not overlap.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn sapote_fat32_parse_directory_entry(
    entry: *const u8,
    entry_len: usize,
    out: *mut fat32::DirectoryEntry,
) -> i32 {
    if out.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable result and null was refused.
    unsafe { *out = fat32::DirectoryEntry::invalid() };
    if entry.is_null() {
        return fat32_status_code(fat32::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range.
    let bytes = unsafe { core::slice::from_raw_parts(entry, entry_len) };
    match fat32::parse_directory_entry(bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat32_status_code(fat32::Status::Ok)
        }
        Err(status) => fat32_status_code(status),
    }
}

fn fat16_status_code(status: fat16::Status) -> i32 {
    status as i32
}

/// Validate a CPU-owned BPB block and copy pointer-free checked geometry.
///
/// # Safety
///
/// `block` must address `block_len` readable, non-aliased bytes and `out` must
/// address one writable `fat16::Geometry`. The two ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_parse_bpb(
    block: *const u8,
    block_len: usize,
    namespace_blocks: u64,
    namespace_block_bytes: u32,
    out: *mut fat16::Geometry,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Geometry and null was refused.
    unsafe { *out = fat16::Geometry::invalid() };
    if block.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(block, block_len) };
    match fat16::parse_bpb(bytes, namespace_blocks, namespace_block_bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Validate and copy an exact canonical raw 8.3 query.
///
/// # Safety
///
/// `name` must address `name_len` readable bytes and `out` one writable query;
/// the ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_make_query(
    name: *const u8,
    name_len: usize,
    out: *mut fat16::RootQuery,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootQuery and null was refused.
    unsafe { *out = fat16::RootQuery::invalid() };
    if name.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(name, name_len) };
    match fat16::make_query(bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Locate one validated root entry inside one CPU-owned root block.
///
/// # Safety
///
/// `block` must address `block_len` readable bytes; `geometry` and `query`
/// must each address one readable value; `out` must address one writable root
/// entry. No input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_find_root(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    query: *const fat16::RootQuery,
    destination_bytes: u32,
    out: *mut fat16::RootEntry,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootEntry and null was refused.
    unsafe { *out = fat16::RootEntry::invalid() };
    if block.is_null() || geometry.is_null() || query.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range and two readable values;
    // all null cases were refused and the output is non-aliased by contract.
    let (bytes, checked_geometry, checked_query) = unsafe {
        (
            core::slice::from_raw_parts(block, block_len),
            *geometry,
            *query,
        )
    };
    match fat16::find_root(bytes, &checked_geometry, &checked_query, destination_bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Validate FAT16 reserved entries and capture cluster two's EOC by value.
///
/// # Safety
///
/// `block` must address `block_len` readable bytes; `geometry` must address one
/// readable value; `out` must address one writable FAT result. No input may
/// overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_parse_fat(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    out: *mut fat16::FatState,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable FatState and null was refused.
    unsafe { *out = fat16::FatState::invalid() };
    if block.is_null() || geometry.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range and one readable value;
    // all null cases were refused and the output is non-aliased by contract.
    let (bytes, checked_geometry) =
        unsafe { (core::slice::from_raw_parts(block, block_len), *geometry) };
    match fat16::parse_fat(bytes, &checked_geometry) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Join validated geometry, root, and FAT values into one checked extent.
///
/// # Safety
///
/// Each input must address one readable value and `out` one writable extent;
/// no input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_validate_extent(
    geometry: *const fat16::Geometry,
    entry: *const fat16::RootEntry,
    fat: *const fat16::FatState,
    out: *mut fat16::Extent,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Extent and null was refused.
    unsafe { *out = fat16::Extent::invalid() };
    if geometry.is_null() || entry.is_null() || fat.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises three readable non-aliased values and every
    // null case was refused above.
    let (checked_geometry, checked_entry, checked_fat) = unsafe { (*geometry, *entry, *fat) };
    match fat16::validate_extent(&checked_geometry, &checked_entry, &checked_fat) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Validate deterministic file bytes and return their SHA-256 by value.
///
/// # Safety
///
/// `data` must address `data_len` readable bytes and `out` one writable
/// `fat16::Payload`; the ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_validate_payload(
    data: *const u8,
    data_len: usize,
    out: *mut fat16::Payload,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Payload and null was refused.
    unsafe { *out = fat16::Payload::invalid() };
    if data.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
    match fat16::validate_payload(bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

fn linux_fat16_status_code(status: linux_fat16::Status) -> i32 {
    status as i32
}

/// Run the pointer-free BusyBox FAT-chain invariant controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_fat16_self_test() -> u32 {
    linux_fat16::self_test()
}

/// Construct the one canonical raw 8.3 BusyBox query by value.
///
/// # Safety
///
/// `out` must address one writable root query.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_make_query(out: *mut fat16::RootQuery) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootQuery and null was refused.
    unsafe { *out = linux_fat16::make_query() };
    linux_fat16_status_code(linux_fat16::Status::Ok)
}

/// Locate the one bounded BusyBox root entry in a CPU-owned root block.
///
/// # Safety
///
/// Inputs must address their complete readable values, `out` must address one
/// writable root entry, and no input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_find_root(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    query: *const fat16::RootQuery,
    destination_bytes: u32,
    out: *mut fat16::RootEntry,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootEntry and null was refused.
    unsafe { *out = fat16::RootEntry::invalid() };
    if block.is_null() || geometry.is_null() || query.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises the readable, non-aliased values above.
    let (bytes, checked_geometry, checked_query) = unsafe {
        (
            core::slice::from_raw_parts(block, block_len),
            *geometry,
            *query,
        )
    };
    match linux_fat16::find_root(bytes, &checked_geometry, &checked_query, destination_bytes) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the exact bounded FAT chain and return pointer-free cluster/LBA
/// values.
///
/// # Safety
///
/// Inputs must address their complete readable values, `out` must address one
/// writable chain, and no input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_build_chain(
    fat_block: *const u8,
    fat_len: usize,
    geometry: *const fat16::Geometry,
    entry: *const fat16::RootEntry,
    out: *mut linux_fat16::Chain,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Chain and null was refused.
    unsafe { *out = linux_fat16::Chain::invalid() };
    if fat_block.is_null() || geometry.is_null() || entry.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises the readable, non-aliased values above.
    let (bytes, checked_geometry, checked_entry) = unsafe {
        (
            core::slice::from_raw_parts(fat_block, fat_len),
            *geometry,
            *entry,
        )
    };
    match linux_fat16::build_chain(bytes, &checked_geometry, &checked_entry) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the complete CPU-owned BusyBox bytes and return their SHA-256.
///
/// # Safety
///
/// `data` must address `data_len` readable bytes, `out` one writable payload,
/// and the two ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_validate_payload(
    data: *const u8,
    data_len: usize,
    out: *mut linux_fat16::Payload,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Payload and null was refused.
    unsafe { *out = linux_fat16::Payload::invalid() };
    if data.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
    match linux_fat16::validate_payload(bytes) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Run the pointer-free uname BusyBox FAT-chain invariant controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_uname_fat16_self_test() -> u32 {
    linux_fat16::self_test_uname()
}

/// Construct the canonical uname fixture root query by value.
///
/// # Safety
///
/// `out` must address one writable root query.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_uname_fat16_make_query(out: *mut fat16::RootQuery) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootQuery and null was refused.
    unsafe { *out = linux_fat16::make_uname_query() };
    linux_fat16_status_code(linux_fat16::Status::Ok)
}

/// Locate the bounded uname BusyBox root entry in a CPU-owned block.
///
/// # Safety
///
/// Inputs must name their complete readable values and `out` one writable
/// non-overlapping root entry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_uname_fat16_find_root(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    query: *const fat16::RootQuery,
    destination_bytes: u32,
    out: *mut fat16::RootEntry,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootEntry and null was refused.
    unsafe { *out = fat16::RootEntry::invalid() };
    if block.is_null() || geometry.is_null() || query.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises the readable, non-aliased values above.
    let (bytes, checked_geometry, checked_query) = unsafe {
        (
            core::slice::from_raw_parts(block, block_len),
            *geometry,
            *query,
        )
    };
    match linux_fat16::find_uname_root(bytes, &checked_geometry, &checked_query, destination_bytes)
    {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the exact bounded uname FAT chain.
///
/// # Safety
///
/// Inputs must name complete readable values and `out` one writable,
/// non-overlapping chain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_uname_fat16_build_chain(
    fat_block: *const u8,
    fat_len: usize,
    geometry: *const fat16::Geometry,
    entry: *const fat16::RootEntry,
    out: *mut linux_fat16::Chain,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Chain and null was refused.
    unsafe { *out = linux_fat16::Chain::invalid() };
    if fat_block.is_null() || geometry.is_null() || entry.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises the readable, non-aliased values above.
    let (bytes, checked_geometry, checked_entry) = unsafe {
        (
            core::slice::from_raw_parts(fat_block, fat_len),
            *geometry,
            *entry,
        )
    };
    match linux_fat16::build_uname_chain(bytes, &checked_geometry, &checked_entry) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the complete CPU-owned uname BusyBox bytes and SHA-256.
///
/// # Safety
///
/// `data` must name `data_len` readable bytes and `out` one writable,
/// non-overlapping payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_uname_fat16_validate_payload(
    data: *const u8,
    data_len: usize,
    out: *mut linux_fat16::Payload,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Payload and null was refused.
    unsafe { *out = linux_fat16::Payload::invalid() };
    if data.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
    match linux_fat16::validate_uname_payload(bytes) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Run the pointer-free measured cat FAT16 controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_cat_fat16_self_test() -> u32 {
    linux_fat16::self_test_cat()
}

/// Build the exact CATBOX root query.
///
/// # Safety
///
/// `out` must point to one writable `fat16::RootQuery`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_cat_fat16_make_query(out: *mut fat16::RootQuery) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable query and null was refused.
    unsafe { *out = linux_fat16::make_cat_query() };
    linux_fat16_status_code(linux_fat16::Status::Ok)
}

/// Select only the exact measured CATBOX root entry.
///
/// # Safety
///
/// The input pointers must name their complete readable values and `out` one
/// writable, non-overlapping root entry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_cat_fat16_find_root(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    query: *const fat16::RootQuery,
    destination_bytes: u32,
    out: *mut fat16::RootEntry,
) -> i32 {
    if block.is_null() || geometry.is_null() || query.is_null() || out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises these complete non-overlapping values.
    let (bytes, checked_geometry, checked_query) = unsafe {
        (
            core::slice::from_raw_parts(block, block_len),
            *geometry,
            *query,
        )
    };
    match linux_fat16::find_cat_root(bytes, &checked_geometry, &checked_query, destination_bytes) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the exact CATBOX FAT chain.
///
/// # Safety
///
/// Inputs must name complete readable values and `out` one writable chain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_cat_fat16_build_chain(
    fat_block: *const u8,
    fat_len: usize,
    geometry: *const fat16::Geometry,
    entry: *const fat16::RootEntry,
    out: *mut linux_fat16::Chain,
) -> i32 {
    if fat_block.is_null() || geometry.is_null() || entry.is_null() || out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises these complete readable values.
    let (bytes, checked_geometry, checked_entry) = unsafe {
        (
            core::slice::from_raw_parts(fat_block, fat_len),
            *geometry,
            *entry,
        )
    };
    match linux_fat16::build_cat_chain(bytes, &checked_geometry, &checked_entry) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the complete CPU-owned cat BusyBox bytes and SHA-256.
///
/// # Safety
///
/// `data` must name `data_len` readable bytes and `out` one writable payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_cat_fat16_validate_payload(
    data: *const u8,
    data_len: usize,
    out: *mut linux_fat16::Payload,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable payload and null was refused.
    unsafe { *out = linux_fat16::Payload::invalid() };
    if data.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this complete readable range.
    let bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
    match linux_fat16::validate_cat_payload(bytes) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

fn linux_elf64_status_code(status: linux_elf64::Status) -> i32 {
    status as i32
}

fn native_image_status_code(status: native_image::Status) -> i32 {
    status as i32
}

/// Run the allocation-free native manifest, ELF, and SHA-256 invariants.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_native_image_self_test() -> u32 {
    native_image::self_test()
}

/// Validate one CPU-owned manifest and executable as a single admission unit.
///
/// # Safety
///
/// Both inputs must name their complete readable lengths. `manifest_out` and
/// `image_out` must each name one writable, non-overlapping result, and neither
/// output may overlap either input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_native_image_validate(
    manifest_bytes: *const u8,
    manifest_length: usize,
    elf_bytes: *const u8,
    elf_length: usize,
    manifest_out: *mut native_image::Manifest,
    image_out: *mut native_image::ValidatedImage,
) -> i32 {
    if manifest_out.is_null() || image_out.is_null() {
        return native_image_status_code(native_image::Status::NullArgument);
    }
    // SAFETY: the caller promises both writable results and null was refused.
    unsafe {
        *manifest_out = native_image::Manifest::invalid();
        *image_out = native_image::ValidatedImage::invalid();
    }
    if manifest_bytes.is_null() || elf_bytes.is_null() {
        return native_image_status_code(native_image::Status::NullArgument);
    }
    // SAFETY: the caller promises both complete readable ranges.
    let manifest = unsafe {
        core::slice::from_raw_parts(manifest_bytes, manifest_length)
    };
    // SAFETY: as above, for the complete executable range.
    let elf = unsafe { core::slice::from_raw_parts(elf_bytes, elf_length) };
    match native_image::validate(manifest, elf) {
        Ok((manifest_value, image_value)) => {
            // SAFETY: both validated outputs still name one writable value.
            unsafe {
                *manifest_out = manifest_value;
                *image_out = image_value;
            }
            native_image_status_code(native_image::Status::Ok)
        }
        Err(status) => native_image_status_code(status),
    }
}

/// Run the pointer-free measured BusyBox ELF conjunction controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_elf64_self_test() -> u32 {
    linux_elf64::self_test()
}

/// Parse one complete CPU-owned BusyBox ELF into pointer-free segment facts.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` one
/// writable validated image. The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut linux_elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable result and null was refused.
    unsafe { *out = linux_elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this complete readable range.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match linux_elf64::parse(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            linux_elf64_status_code(linux_elf64::Status::Ok)
        }
        Err(status) => linux_elf64_status_code(status),
    }
}

/// Run the pointer-free measured uname BusyBox ELF conjunction controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_uname_elf64_self_test() -> u32 {
    linux_elf64::self_test_uname()
}

/// Parse the complete CPU-owned uname BusyBox ELF into segment facts.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` one
/// writable validated image. The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_uname_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut linux_elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable result and null was refused.
    unsafe { *out = linux_elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this complete readable range.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match linux_elf64::parse_uname(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            linux_elf64_status_code(linux_elf64::Status::Ok)
        }
        Err(status) => linux_elf64_status_code(status),
    }
}

/// Run the pointer-free measured cat BusyBox ELF conjunction controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_cat_elf64_self_test() -> u32 {
    linux_elf64::self_test_cat()
}

/// Parse the complete CPU-owned cat BusyBox ELF into segment facts.
///
/// # Safety
///
/// `input` must address `input_len` readable bytes and `out` one writable,
/// non-overlapping validated image.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_cat_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut linux_elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable result and null was refused.
    unsafe { *out = linux_elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this complete readable range.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match linux_elf64::parse_cat(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            linux_elf64_status_code(linux_elf64::Status::Ok)
        }
        Err(status) => linux_elf64_status_code(status),
    }
}

fn elf64_status_code(status: elf64::Status) -> i32 {
    status as i32
}

/// Run all host-independent ELF64 parser mutation families.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_elf64_self_test() -> u32 {
    elf64::self_test()
}

/// Parse one CPU-owned ELF file into pointer-free validated facts.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` must
/// address one writable `elf64::ValidatedImage`.  The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return elf64_status_code(elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable output and null was refused.
    unsafe { *out = elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return elf64_status_code(elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this one readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match elf64::parse(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            elf64_status_code(elf64::Status::Ok)
        }
        Err(status) => elf64_status_code(status),
    }
}

/// Run all host-independent multiprocess ELF64 parser mutation families.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_multiprocess_elf64_self_test() -> u32 {
    elf64::self_test_multiprocess()
}

/// Run every VBIOS parser control and report how many passed.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_nvbios_self_test() -> u32 {
    nvbios::self_test() as u32
}

/// How many controls a complete VBIOS parser self-test runs.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_nvbios_controls() -> u32 {
    nvbios::ROBUSTNESS_CONTROLS as u32
}

/// Copy the synthesised reference VBIOS image out for the caller to compare.
///
/// The kernel keeps its own copy of these bytes; writing Rust's copy through
/// this boundary is how the two are held to being the same image rather than
/// two images that merely parse.
///
/// # Safety
///
/// `out` must address `capacity` writable, non-aliased bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_nvbios_reference(
    out: *mut u8,
    capacity: usize,
) -> usize {
    let reference = nvbios::reference();

    if out.is_null() || capacity < reference.len() {
        return 0;
    }
    // SAFETY: the caller promises this writable range and null was refused.
    let destination = unsafe {
        core::slice::from_raw_parts_mut(out, reference.len())
    };
    destination.copy_from_slice(&reference);
    reference.len()
}

/// Validate one VBIOS image read out of an NVIDIA PROM window.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` must
/// address one writable `nvbios::Image`.  The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_nvbios_parse(
    input: *const u8,
    input_len: usize,
    out: *mut nvbios::Image,
) -> i32 {
    if out.is_null() {
        return nvbios::Status::Length as i32;
    }
    // SAFETY: the caller promises one writable output and null was refused.
    unsafe { *out = nvbios::Image::default() };
    if input.is_null() {
        return nvbios::Status::Length as i32;
    }
    // SAFETY: the caller promises this one readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match nvbios::parse(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            nvbios::Status::Ok as i32
        }
        Err(status) => status as i32,
    }
}

/// Parse one CPU-owned multiprocess ELF file into pointer-free facts.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` must
/// address one writable `elf64::ValidatedImage`.  The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_multiprocess_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return elf64_status_code(elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable output and null was refused.
    unsafe { *out = elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return elf64_status_code(elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this one readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match elf64::parse_multiprocess(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            elf64_status_code(elf64::Status::Ok)
        }
        Err(status) => elf64_status_code(status),
    }
}
