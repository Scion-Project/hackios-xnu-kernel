// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the HakiOS authors (Scion).
//! A test: any Vulkan driver made to behave as one without coherent host memory, as gfxstream's guest driver for Scion does
//! (it keeps a copy of each mapping in the guest and moves bytes only at a flush or an invalidate). `HAKIOS_VK_INCOHERENT=1`.
//!
//! The memory types lose `HOST_COHERENT`; a mapping hands out a copy of the driver's (taken when it is mapped), and only
//! `vkFlushMappedMemoryRanges` copies the copy's bytes to the driver's mapping, only `vkInvalidateMappedMemoryRanges` the other
//! way. `VK_EXT_host_image_copy` is hidden too (gfxstream's driver has none), so a texture's CPU copies go through mapped
//! staging buffers, as they will there. So on a driver that works (lavapipe on a Mac) every missing flush or invalidate is a wrong number, which is how
//! coherence.rs and the runtime's own flushing (tools/metal/m2v/) are checked without the guest's driver.

use ash::vk::{self, Handle};
use std::collections::HashMap;
use std::ffi::{c_char, CStr};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};

type Void = Option<unsafe extern "system" fn()>;

const NAMES: [&str; 11] = [
    "vkGetPhysicalDeviceMemoryProperties",  // 0
    "vkGetPhysicalDeviceMemoryProperties2", // 1
    "vkGetPhysicalDeviceMemoryProperties2KHR",
    "vkGetDeviceProcAddr",            // 3
    "vkAllocateMemory",               // 4
    "vkFreeMemory",                   // 5
    "vkMapMemory",                    // 6
    "vkUnmapMemory",                  // 7
    "vkFlushMappedMemoryRanges",      // 8
    "vkInvalidateMappedMemoryRanges", // 9
    "vkEnumerateDeviceExtensionProperties", // 10
];
static REAL: [AtomicUsize; 11] = [const { AtomicUsize::new(0) }; 11];
/// Bytes moved: by flushes, by invalidates; and the calls of each (`moved`).
static MOVED: [std::sync::atomic::AtomicU64; 4] = [const { std::sync::atomic::AtomicU64::new(0) }; 4];

/// What the test moved so far: bytes flushed, flush calls, bytes invalidated, invalidate calls.
pub fn moved() -> [u64; 4] {
    std::array::from_fn(|i| MOVED[i].load(Ordering::Relaxed))
}

struct Shadow {
    /// the driver's own mapping, and the copy handed out in its place
    driver: usize,
    copy: Vec<u8>,
}

#[derive(Default)]
struct State {
    sizes: HashMap<u64, u64>,
    shadows: HashMap<u64, Shadow>,
}
static STATE: Mutex<Option<State>> = Mutex::new(None);

fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    let mut state = STATE.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    f(state.get_or_insert_with(State::default))
}

/// Whether the test is on (`HAKIOS_VK_INCOHERENT=1`), read once.
pub fn on() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("HAKIOS_VK_INCOHERENT").is_ok_and(|v| v == "1"))
}

fn real<F: Copy>(index: usize) -> F {
    let pointer = REAL[index].load(Ordering::Acquire);
    assert!(pointer != 0, "incoherent: {} was never resolved", NAMES[index]);
    // SAFETY: REAL[index] only ever holds the driver's entry point for NAMES[index], of the type F the caller names.
    unsafe { std::mem::transmute_copy(&pointer) }
}

/// The function to hand out for `name` while the test is on: this file's where it has one, else the driver's.
pub fn wrap(name: &CStr, driver: Void) -> Void {
    if !on() {
        return driver;
    }
    let Some(pointer) = driver else { return None };
    let Ok(name) = name.to_str() else { return driver };
    let Some(index) = NAMES.iter().position(|n| *n == name) else { return driver };
    let _ = REAL[index].compare_exchange(0, pointer as usize, Ordering::AcqRel, Ordering::Acquire);
    let ours: unsafe extern "system" fn() = unsafe {
        match index {
            0 => std::mem::transmute(memory_properties as unsafe extern "system" fn(_, _)),
            1 | 2 => std::mem::transmute(memory_properties2 as unsafe extern "system" fn(_, _)),
            3 => std::mem::transmute(get_device_proc_addr as unsafe extern "system" fn(_, _) -> _),
            4 => std::mem::transmute(allocate_memory as unsafe extern "system" fn(_, _, _, _) -> _),
            5 => std::mem::transmute(free_memory as unsafe extern "system" fn(_, _, _)),
            6 => std::mem::transmute(map_memory as unsafe extern "system" fn(_, _, _, _, _, _) -> _),
            7 => std::mem::transmute(unmap_memory as unsafe extern "system" fn(_, _)),
            8 => std::mem::transmute(flush_ranges as unsafe extern "system" fn(_, _, _) -> _),
            9 => std::mem::transmute(invalidate_ranges as unsafe extern "system" fn(_, _, _) -> _),
            _ => std::mem::transmute(device_extensions as unsafe extern "system" fn(_, _, _, _) -> _),
        }
    };
    Some(ours)
}

fn strip(properties: &mut vk::PhysicalDeviceMemoryProperties) {
    let count = (properties.memory_type_count as usize).min(properties.memory_types.len());
    for t in &mut properties.memory_types[..count] {
        t.property_flags &= !vk::MemoryPropertyFlags::HOST_COHERENT;
    }
}

unsafe extern "system" fn memory_properties(physical: vk::PhysicalDevice, out: *mut vk::PhysicalDeviceMemoryProperties) {
    let f: vk::PFN_vkGetPhysicalDeviceMemoryProperties = real(0);
    unsafe { f(physical, out) };
    if let Some(out) = unsafe { out.as_mut() } {
        strip(out);
    }
}

unsafe extern "system" fn memory_properties2(physical: vk::PhysicalDevice, out: *mut vk::PhysicalDeviceMemoryProperties2<'static>) {
    let index = if REAL[1].load(Ordering::Acquire) != 0 { 1 } else { 2 };
    let f: vk::PFN_vkGetPhysicalDeviceMemoryProperties2 = real(index);
    unsafe { f(physical, out) };
    if let Some(out) = unsafe { out.as_mut() } {
        strip(&mut out.memory_properties);
    }
}

unsafe extern "system" fn get_device_proc_addr(device: vk::Device, name: *const c_char) -> Void {
    let f: vk::PFN_vkGetDeviceProcAddr = real(3);
    let driver = unsafe { f(device, name) };
    if name.is_null() {
        return driver;
    }
    wrap(unsafe { CStr::from_ptr(name) }, driver)
}

unsafe extern "system" fn allocate_memory(device: vk::Device, info: *const vk::MemoryAllocateInfo<'static>,
                                          allocator: *const vk::AllocationCallbacks<'static>, out: *mut vk::DeviceMemory) -> vk::Result {
    let f: vk::PFN_vkAllocateMemory = real(4);
    let result = unsafe { f(device, info, allocator, out) };
    if result == vk::Result::SUCCESS {
        let (memory, size) = unsafe { ((*out).as_raw(), (*info).allocation_size) };
        with(|s| s.sizes.insert(memory, size));
    }
    result
}

unsafe extern "system" fn free_memory(device: vk::Device, memory: vk::DeviceMemory, allocator: *const vk::AllocationCallbacks<'static>) {
    with(|s| {
        s.shadows.remove(&memory.as_raw());
        s.sizes.remove(&memory.as_raw());
    });
    let f: vk::PFN_vkFreeMemory = real(5);
    unsafe { f(device, memory, allocator) }
}

unsafe extern "system" fn map_memory(device: vk::Device, memory: vk::DeviceMemory, offset: u64, size: u64, flags: vk::MemoryMapFlags,
                                     out: *mut *mut std::ffi::c_void) -> vk::Result {
    let f: vk::PFN_vkMapMemory = real(6);
    let result = unsafe { f(device, memory, offset, size, flags, out) };
    if result != vk::Result::SUCCESS || out.is_null() || offset != 0 {
        return result;
    }
    let driver = unsafe { *out } as usize;
    with(|s| {
        let whole = s.sizes.get(&memory.as_raw()).copied().unwrap_or(0);
        let size = if size == vk::WHOLE_SIZE { whole } else { size } as usize;
        if driver != 0 && size != 0 {
            // SAFETY: the driver's mapping of `size` bytes.
            let copy = unsafe { std::slice::from_raw_parts(driver as *const u8, size) }.to_vec();
            let shadow = s.shadows.entry(memory.as_raw()).or_insert(Shadow { driver, copy });
            // SAFETY: the copy is the caller's in place of the driver's mapping until the unmap.
            unsafe { *out = shadow.copy.as_mut_ptr().cast() };
        }
    });
    result
}

unsafe extern "system" fn unmap_memory(device: vk::Device, memory: vk::DeviceMemory) {
    with(|s| s.shadows.remove(&memory.as_raw()));
    let f: vk::PFN_vkUnmapMemory = real(7);
    unsafe { f(device, memory) }
}

/// Copies between each range's copy and the driver's mapping, `to_driver` or back.
fn exchange(count: u32, ranges: *const vk::MappedMemoryRange<'static>, to_driver: bool) {
    if ranges.is_null() {
        return;
    }
    // SAFETY: Vulkan's contract: `count` ranges.
    let ranges = unsafe { std::slice::from_raw_parts(ranges, count as usize) };
    let at = if to_driver { 0 } else { 2 };
    MOVED[at + 1].fetch_add(1, Ordering::Relaxed);
    with(|s| {
        for r in ranges {
            let Some(shadow) = s.shadows.get_mut(&r.memory.as_raw()) else { continue };
            let size = shadow.copy.len();
            let start = (r.offset as usize).min(size);
            let end = if r.size == vk::WHOLE_SIZE { size } else { (start + r.size as usize).min(size) };
            // SAFETY: the driver's mapping is as long as the copy.
            let driver = unsafe { std::slice::from_raw_parts_mut(shadow.driver as *mut u8, size) };
            MOVED[at].fetch_add((end - start) as u64, Ordering::Relaxed);
            if to_driver {
                driver[start..end].copy_from_slice(&shadow.copy[start..end]);
            } else {
                shadow.copy[start..end].copy_from_slice(&driver[start..end]);
            }
        }
    });
}

unsafe extern "system" fn flush_ranges(device: vk::Device, count: u32, ranges: *const vk::MappedMemoryRange<'static>) -> vk::Result {
    exchange(count, ranges, true);
    let f: vk::PFN_vkFlushMappedMemoryRanges = real(8);
    unsafe { f(device, count, ranges) }
}

unsafe extern "system" fn invalidate_ranges(device: vk::Device, count: u32, ranges: *const vk::MappedMemoryRange<'static>)
                                            -> vk::Result {
    let f: vk::PFN_vkInvalidateMappedMemoryRanges = real(9);
    let result = unsafe { f(device, count, ranges) };
    exchange(count, ranges, false);
    result
}

/// The driver's device extensions but `VK_EXT_host_image_copy`.
unsafe extern "system" fn device_extensions(physical: vk::PhysicalDevice, layer: *const c_char, count: *mut u32,
                                            out: *mut vk::ExtensionProperties) -> vk::Result {
    let f: vk::PFN_vkEnumerateDeviceExtensionProperties = real(10);
    let mut n = 0u32;
    let result = unsafe { f(physical, layer, &mut n, std::ptr::null_mut()) };
    if result != vk::Result::SUCCESS || count.is_null() {
        return result;
    }
    let mut all = vec![vk::ExtensionProperties::default(); n as usize];
    let result = unsafe { f(physical, layer, &mut n, all.as_mut_ptr()) };
    if result != vk::Result::SUCCESS && result != vk::Result::INCOMPLETE {
        return result;
    }
    all.truncate(n as usize);
    all.retain(|e| e.extension_name_as_c_str().map_or(true, |name| name != c"VK_EXT_host_image_copy"));
    unsafe {
        if out.is_null() {
            *count = all.len() as u32;
            return vk::Result::SUCCESS;
        }
        let room = (*count as usize).min(all.len());
        std::ptr::copy_nonoverlapping(all.as_ptr(), out, room);
        *count = room as u32;
        if room < all.len() { vk::Result::INCOMPLETE } else { vk::Result::SUCCESS }
    }
}
