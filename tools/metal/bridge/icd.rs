// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the HakiOS authors (Scion). See docs/metal-plan.md, "Metal on Vulkan".
//! The Vulkan driver under the runtime: an installable client driver (ICD) opened by its path, with no Khronos loader between.
//!
//! The runtime opens "a Vulkan loader" by path and takes the library's `vkGetInstanceProcAddr` (ash's `Entry::load_from`). iOS has
//! no loader, and an ICD need not export that name: the loader-ICD interface's own entry is `vk_icdGetInstanceProcAddr`, which is
//! all Mesa's drivers export (lavapipe on a Mac, panvk) and what gfxstream's guest driver for Scion (libhakios.vk.dylib) is built
//! with. So this library exports `vkGetInstanceProcAddr` itself, forwarding to the driver `select` opened, and the runtime is given
//! this library's own path as its "loader": opening it again gives the same image, whose `vkGetInstanceProcAddr` is the one below.
//!
//! The one thing it adds: where the driver has no coherent host-visible memory, which the runtime needs, coherence.rs stands in for
//! it between the two.
//!
//! What a loader would do and this does not: no layers, no dispatch table patched into the driver's dispatchable objects (every call
//! goes straight to the driver's entry points, which is how a driver is used without a loader), one driver per process.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::Mutex;

pub type VoidFunction = Option<unsafe extern "system" fn()>;
type GetInstanceProcAddr = unsafe extern "system" fn(*mut c_void, *const c_char) -> VoidFunction;
type Negotiate = unsafe extern "system" fn(*mut u32) -> i32;

/// The loader-ICD interface version asked for: 5 is what Mesa's drivers and the Khronos loader of 2020 on agree on. The answer only
/// tells this file what the driver expects of a loader, and nothing here depends on more than version 1 (`vk_icdGetInstanceProcAddr`).
const INTERFACE_VERSION: u32 = 5;

/// The driver's entry, once a driver is selected; null before.
static ENTRY: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

struct Selected {
    path: CString,
    entry_name: &'static str,
    interface: u32,
}
static SELECTED: Mutex<Option<Selected>> = Mutex::new(None);

unsafe extern "C" {
    fn dlopen(path: *const c_char, mode: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
    fn dlerror() -> *const c_char;
}
const RTLD_NOW: c_int = 0x2;
const RTLD_LOCAL: c_int = 0x4;

/// The Vulkan entry the runtime reaches (through `Entry::load_from` on this library): the selected driver's, or none.
///
/// # Safety
/// Vulkan's contract for `vkGetInstanceProcAddr`.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn vkGetInstanceProcAddr(instance: *mut c_void, name: *const c_char) -> VoidFunction {
    let entry = ENTRY.load(Ordering::Acquire);
    if entry.is_null() || name.is_null() {
        return None;
    }
    // SAFETY: ENTRY only ever holds a driver's GetInstanceProcAddr, and the driver stays loaded for the life of the process.
    let get: GetInstanceProcAddr = unsafe { std::mem::transmute(entry) };
    // a driver without coherent host memory gets it stood in for (coherence.rs); HAKIOS_VK_INCOHERENT=1 makes any driver one
    // (incoherent.rs, a test)
    let driver = unsafe { get(instance, name) };
    let name = unsafe { CStr::from_ptr(name) };
    crate::coherence::wrap(name, crate::incoherent::wrap(name, driver))
}

fn last_dl_error() -> String {
    // SAFETY: dlerror's string is valid until the next dl call on this thread.
    let text = unsafe { dlerror() };
    if text.is_null() { "unknown".into() } else { unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned() }
}

/// This library's own file: what the runtime is told to open as its loader.
fn own_path() -> Result<PathBuf, String> {
    #[repr(C)]
    struct DlInfo {
        fname: *const c_char,
        fbase: *mut c_void,
        sname: *const c_char,
        saddr: *mut c_void,
    }
    unsafe extern "C" {
        fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    }
    let mut info = DlInfo { fname: std::ptr::null(), fbase: std::ptr::null_mut(), sname: std::ptr::null(), saddr: std::ptr::null_mut() };
    // SAFETY: an address inside this image and a writable record.
    if unsafe { dladdr(vkGetInstanceProcAddr as *const c_void, &mut info) } == 0 || info.fname.is_null() {
        return Err("this library's own path (dladdr) is not known".into());
    }
    // SAFETY: dladdr's file name lives as long as the image.
    Ok(PathBuf::from(unsafe { CStr::from_ptr(info.fname) }.to_string_lossy().into_owned()))
}

/// Opens the driver at `path` (once per process: the same path again is the same driver, another one is refused, since devices of
/// the first may exist) and returns the path to give the runtime as its loader.
pub fn select(path: &CStr) -> Result<PathBuf, String> {
    let mut selected = SELECTED.lock().map_err(|_| "the driver lock is poisoned".to_string())?;
    if let Some(s) = selected.as_ref() {
        if s.path.as_c_str() != path {
            return Err(format!("a Vulkan driver is already in use ({}); {} is refused", s.path.to_string_lossy(), path.to_string_lossy()));
        }
        return own_path();
    }
    // SAFETY: a C string; the handle is kept for the life of the process (never closed).
    let handle = unsafe { dlopen(path.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
    if handle.is_null() {
        return Err(format!("the Vulkan driver {} does not open: {}", path.to_string_lossy(), last_dl_error()));
    }
    let mut entry = std::ptr::null_mut();
    let mut entry_name = "";
    for name in [c"vk_icdGetInstanceProcAddr", c"vkGetInstanceProcAddr"] {
        // SAFETY: a loaded image and a C string.
        entry = unsafe { dlsym(handle, name.as_ptr()) };
        if !entry.is_null() {
            entry_name = if name == c"vk_icdGetInstanceProcAddr" { "vk_icdGetInstanceProcAddr" } else { "vkGetInstanceProcAddr" };
            break;
        }
    }
    if entry.is_null() {
        return Err(format!("{} exports neither vk_icdGetInstanceProcAddr nor vkGetInstanceProcAddr", path.to_string_lossy()));
    }
    let mut interface = 0;
    // SAFETY: as above.
    let negotiate = unsafe { dlsym(handle, c"vk_icdNegotiateLoaderICDInterfaceVersion".as_ptr()) };
    if !negotiate.is_null() {
        let mut version = INTERFACE_VERSION;
        // SAFETY: the loader-ICD interface's negotiation call, with a writable version.
        let negotiate: Negotiate = unsafe { std::mem::transmute(negotiate) };
        if unsafe { negotiate(&mut version) } != 0 {
            return Err(format!("{} refused the loader-ICD interface (version {INTERFACE_VERSION})", path.to_string_lossy()));
        }
        interface = version;
    }
    ENTRY.store(entry, Ordering::Release);
    *selected = Some(Selected { path: path.to_owned(), entry_name, interface });
    own_path()
}

/// One line on the selected driver: its path, the entry used and the negotiated interface version (0: not negotiated).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_icd_describe(out: *mut c_char, size: usize) -> usize {
    let text = match SELECTED.lock().ok().and_then(|s| s.as_ref().map(|s| (s.path.clone(), s.entry_name, s.interface))) {
        Some((path, entry, interface)) => format!("{} ({entry}, ICD interface {interface}{})", path.to_string_lossy(),
            if crate::coherence::emulating() { "; coherent host memory emulated" } else { "" }),
        None => "no Vulkan driver selected".into(),
    };
    if !out.is_null() && size > 0 {
        let n = text.len().min(size - 1);
        // SAFETY: the caller's buffer of `size` bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), out as *mut u8, n);
            *out.add(n) = 0;
        }
    }
    text.len()
}
