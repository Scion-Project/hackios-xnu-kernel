// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the HakiOS authors (Scion).
//! Coherent host memory for a Vulkan driver that has none, between the runtime and the driver (icd.rs routes the calls here).
//!
//! metal2vulkan's runtime places Metal's `Shared`, `Managed` and `WriteCombined` memory, and its own host-visible buffers, only in
//! memory types that are `HOST_VISIBLE | HOST_COHERENT`, maps them whole (64 MiB blocks) and never calls
//! `vkFlushMappedMemoryRanges` or `vkInvalidateMappedMemoryRanges`: Metal's shared storage needs no flush. gfxstream's guest driver
//! for Scion (`libhakios.vk.dylib`) cannot map the host's memory into the guest, and keeps a copy of each mapping instead; it lists
//! host-visible types that are not coherent. Without a coherent type the runtime's first such allocation, at device creation, fails
//! (`OutOfMemory(Device)`).
//!
//! So, when the driver lists no host-visible coherent type (and `HAKIOS_VK_COHERENCE` is not `off`), this file:
//! - marks every host-visible type coherent in `vkGetPhysicalDeviceMemoryProperties[2]`;
//! - keeps, for every mapping, a copy of its bytes as of the last exchange with the driver;
//! - before every `vkQueueSubmit[2]` (and an unmap) flushes the 4 KiB chunks the CPU changed since then;
//! - after every wait that succeeds (fences, timeline semaphores, queue or device idle; a counter value that moved) invalidates every
//!   mapping and puts back the chunks the CPU changed meanwhile, which go to the driver at the next submit.
//!
//! It is right for what the runtime does (the CPU writes before a submit and reads after the wait; a chunk both sides wrote between
//! the two keeps the CPU's bytes) and slow: a comparison of all mapped memory per submit, and a transfer of all of it per wait. The
//! cheaper place for it is the runtime, which knows the ranges each command buffer reads and writes, or the driver.

use ash::vk::{self, Handle};
use std::collections::HashMap;
use std::ffi::{c_char, CStr};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Mutex;

type Void = Option<unsafe extern "system" fn()>;

const CHUNK: usize = 4096;

/// Whether the driver had no coherent host-visible type and this file stands in for one (decided at the first memory query).
static ON: AtomicBool = AtomicBool::new(false);

// The driver's own entry points this file calls, by the index of their name in NAMES.
const NAMES: [&str; 19] = [
    "vkGetPhysicalDeviceMemoryProperties",  // 0
    "vkGetPhysicalDeviceMemoryProperties2", // 1
    "vkGetPhysicalDeviceMemoryProperties2KHR",
    "vkGetDeviceProcAddr", // 3
    "vkAllocateMemory",    // 4
    "vkFreeMemory",        // 5
    "vkMapMemory",         // 6
    "vkUnmapMemory",       // 7
    "vkQueueSubmit",       // 8
    "vkQueueSubmit2",      // 9
    "vkQueueSubmit2KHR",
    "vkWaitForFences", // 11
    "vkGetFenceStatus",
    "vkQueueWaitIdle",
    "vkDeviceWaitIdle",
    "vkWaitSemaphores", // 15
    "vkWaitSemaphoresKHR",
    "vkGetSemaphoreCounterValue", // 17
    "vkGetSemaphoreCounterValueKHR",
];
static REAL: [AtomicUsize; 19] = [const { AtomicUsize::new(0) }; 19];
static FLUSH: AtomicUsize = AtomicUsize::new(0);
static INVALIDATE: AtomicUsize = AtomicUsize::new(0);

struct Mapping {
    device: u64,
    pointer: usize,
    size: usize,
    /// the bytes as of the last flush or invalidate
    known: Vec<u8>,
}

#[derive(Default)]
struct State {
    sizes: HashMap<u64, u64>,
    mappings: HashMap<u64, Mapping>,
    counters: HashMap<u64, u64>,
}
static STATE: Mutex<Option<State>> = Mutex::new(None);

fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    let mut state = STATE.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    f(state.get_or_insert_with(State::default))
}

fn real<F: Copy>(index: usize) -> F {
    let pointer = REAL[index].load(Ordering::Acquire);
    assert!(pointer != 0, "coherence: {} was never resolved", NAMES[index]);
    // SAFETY: REAL[index] only ever holds the driver's entry point for NAMES[index], of the type F the caller names.
    unsafe { std::mem::transmute_copy(&pointer) }
}

/// The function to hand out for `name`: one of this file's when it wraps that name (keeping the driver's `driver`), else `driver`.
pub fn wrap(name: &CStr, driver: Void) -> Void {
    let Some(pointer) = driver else { return None };
    let Ok(name) = name.to_str() else { return driver };
    let Some(index) = NAMES.iter().position(|n| *n == name) else { return driver };
    let _ = REAL[index].compare_exchange(0, pointer as usize, Ordering::AcqRel, Ordering::Acquire);
    // The memory queries and vkGetDeviceProcAddr are always this file's (they decide, and they hand out the rest); the others only
    // once it is on, so a driver with coherent memory runs untouched.
    if index > 3 && !ON.load(Ordering::Acquire) {
        return driver;
    }
    let ours: unsafe extern "system" fn() = unsafe {
        match index {
            0 => std::mem::transmute(memory_properties as unsafe extern "system" fn(_, _)),
            1 | 2 => std::mem::transmute(memory_properties2 as unsafe extern "system" fn(_, _)),
            3 => std::mem::transmute(get_device_proc_addr as unsafe extern "system" fn(_, _) -> _),
            4 => std::mem::transmute(allocate_memory as unsafe extern "system" fn(_, _, _, _) -> _),
            5 => std::mem::transmute(free_memory as unsafe extern "system" fn(_, _, _)),
            6 => std::mem::transmute(map_memory as unsafe extern "system" fn(_, _, _, _, _, _) -> _),
            7 => std::mem::transmute(unmap_memory as unsafe extern "system" fn(_, _)),
            8 => std::mem::transmute(queue_submit as unsafe extern "system" fn(_, _, _, _) -> _),
            9 => std::mem::transmute(queue_submit2 as unsafe extern "system" fn(_, _, _, _) -> _),
            10 => std::mem::transmute(queue_submit2_khr as unsafe extern "system" fn(_, _, _, _) -> _),
            11 => std::mem::transmute(wait_for_fences as unsafe extern "system" fn(_, _, _, _, _) -> _),
            12 => std::mem::transmute(get_fence_status as unsafe extern "system" fn(_, _) -> _),
            13 => std::mem::transmute(queue_wait_idle as unsafe extern "system" fn(_) -> _),
            14 => std::mem::transmute(device_wait_idle as unsafe extern "system" fn(_) -> _),
            15 => std::mem::transmute(wait_semaphores as unsafe extern "system" fn(_, _, _) -> _),
            16 => std::mem::transmute(wait_semaphores_khr as unsafe extern "system" fn(_, _, _) -> _),
            17 => std::mem::transmute(counter_value as unsafe extern "system" fn(_, _, _) -> _),
            _ => std::mem::transmute(counter_value_khr as unsafe extern "system" fn(_, _, _) -> _),
        }
    };
    Some(ours)
}

/// Whether this file stands in for coherent memory (`nmb_icd_describe` says so).
pub fn emulating() -> bool {
    ON.load(Ordering::Acquire)
}

fn adjust(properties: &mut vk::PhysicalDeviceMemoryProperties) {
    use vk::MemoryPropertyFlags as P;
    if std::env::var("HAKIOS_VK_COHERENCE").is_ok_and(|v| v == "off") {
        return;
    }
    let count = (properties.memory_type_count as usize).min(properties.memory_types.len());
    let types = &mut properties.memory_types[..count];
    if types.iter().any(|t| t.property_flags.contains(P::HOST_VISIBLE | P::HOST_COHERENT))
        || !types.iter().any(|t| t.property_flags.contains(P::HOST_VISIBLE))
    {
        return;
    }
    for t in types.iter_mut().filter(|t| t.property_flags.contains(P::HOST_VISIBLE)) {
        t.property_flags |= P::HOST_COHERENT;
    }
    ON.store(true, Ordering::Release);
}

unsafe extern "system" fn memory_properties(physical: vk::PhysicalDevice, out: *mut vk::PhysicalDeviceMemoryProperties) {
    let f: vk::PFN_vkGetPhysicalDeviceMemoryProperties = real(0);
    unsafe { f(physical, out) };
    if let Some(out) = unsafe { out.as_mut() } {
        adjust(out);
    }
}

unsafe extern "system" fn memory_properties2(physical: vk::PhysicalDevice, out: *mut vk::PhysicalDeviceMemoryProperties2<'static>) {
    let index = if REAL[1].load(Ordering::Acquire) != 0 { 1 } else { 2 };
    let f: vk::PFN_vkGetPhysicalDeviceMemoryProperties2 = real(index);
    unsafe { f(physical, out) };
    if let Some(out) = unsafe { out.as_mut() } {
        adjust(&mut out.memory_properties);
    }
}

unsafe extern "system" fn get_device_proc_addr(device: vk::Device, name: *const c_char) -> Void {
    let f: vk::PFN_vkGetDeviceProcAddr = real(3);
    let driver = unsafe { f(device, name) };
    if FLUSH.load(Ordering::Acquire) == 0 {
        // the two calls this file makes itself
        if let Some(p) = unsafe { f(device, c"vkFlushMappedMemoryRanges".as_ptr()) } {
            FLUSH.store(p as usize, Ordering::Release);
        }
        if let Some(p) = unsafe { f(device, c"vkInvalidateMappedMemoryRanges".as_ptr()) } {
            INVALIDATE.store(p as usize, Ordering::Release);
        }
    }
    if name.is_null() {
        return driver;
    }
    wrap(unsafe { CStr::from_ptr(name) }, driver)
}

fn ranges_call(which: &AtomicUsize, device: u64, ranges: &[vk::MappedMemoryRange<'static>]) -> vk::Result {
    let pointer = which.load(Ordering::Acquire);
    if pointer == 0 || ranges.is_empty() {
        return vk::Result::SUCCESS;
    }
    // SAFETY: FLUSH and INVALIDATE hold the driver's vkFlushMappedMemoryRanges and vkInvalidateMappedMemoryRanges.
    let f: vk::PFN_vkFlushMappedMemoryRanges = unsafe { std::mem::transmute(pointer) };
    unsafe { f(vk::Device::from_raw(device), ranges.len() as u32, ranges.as_ptr()) }
}

/// The runs of chunks where the CPU changed a mapping since the last exchange, as (start, end) in bytes.
fn changed(m: &Mapping) -> Vec<(usize, usize)> {
    // SAFETY: the mapping is live (it leaves the table before it is unmapped) and `size` bytes long.
    let now = unsafe { std::slice::from_raw_parts(m.pointer as *const u8, m.size) };
    let mut runs: Vec<(usize, usize)> = Vec::new();
    let mut at = 0;
    while at < m.size {
        let end = (at + CHUNK).min(m.size);
        if now[at..end] != m.known[at..end] {
            match runs.last_mut() {
                Some(run) if run.1 == at => run.1 = end,
                _ => runs.push((at, end)),
            }
        }
        at = end;
    }
    runs
}

fn range(memory: u64, offset: usize, size: u64) -> vk::MappedMemoryRange<'static> {
    vk::MappedMemoryRange::default().memory(vk::DeviceMemory::from_raw(memory)).offset(offset as u64).size(size)
}

/// The CPU's changes since the last exchange, to the driver.
fn flush(memory: u64, m: &mut Mapping) {
    let runs = changed(m);
    if runs.is_empty() {
        return;
    }
    let ranges: Vec<_> = runs
        .iter()
        .map(|&(start, end)| range(memory, start, if end == m.size { vk::WHOLE_SIZE } else { (end - start) as u64 }))
        .collect();
    let _ = ranges_call(&FLUSH, m.device, &ranges);   // a failure here is the submit's to report
    // SAFETY: as in `changed`.
    let now = unsafe { std::slice::from_raw_parts(m.pointer as *const u8, m.size) };
    for (start, end) in runs {
        m.known[start..end].copy_from_slice(&now[start..end]);
    }
}

fn flush_all() {
    with(|s| {
        for (memory, m) in s.mappings.iter_mut() {
            flush(*memory, m);
        }
    });
}

/// The driver's bytes into every mapping, the CPU's changes since the last exchange kept over them.
fn invalidate_all() {
    with(|s| {
        for (memory, m) in s.mappings.iter_mut() {
            let runs = changed(m);
            // SAFETY: as in `changed`, writable.
            let now = unsafe { std::slice::from_raw_parts_mut(m.pointer as *mut u8, m.size) };
            let kept: Vec<Vec<u8>> = runs.iter().map(|&(start, end)| now[start..end].to_vec()).collect();
            let _ = ranges_call(&INVALIDATE, m.device, &[range(*memory, 0, vk::WHOLE_SIZE)]);
            m.known.copy_from_slice(now);
            for (&(start, end), bytes) in runs.iter().zip(kept) {
                now[start..end].copy_from_slice(&bytes);
            }
        }
    });
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
        s.mappings.remove(&memory.as_raw());
        s.sizes.remove(&memory.as_raw());
    });
    let f: vk::PFN_vkFreeMemory = real(5);
    unsafe { f(device, memory, allocator) }
}

unsafe extern "system" fn map_memory(device: vk::Device, memory: vk::DeviceMemory, offset: u64, size: u64, flags: vk::MemoryMapFlags,
                                     out: *mut *mut std::ffi::c_void) -> vk::Result {
    let f: vk::PFN_vkMapMemory = real(6);
    let result = unsafe { f(device, memory, offset, size, flags, out) };
    if result != vk::Result::SUCCESS || out.is_null() {
        return result;
    }
    let pointer = unsafe { *out } as usize;
    with(|s| {
        let whole = s.sizes.get(&memory.as_raw()).copied().unwrap_or(0);
        let size = if size == vk::WHOLE_SIZE { whole.saturating_sub(offset) } else { size } as usize;
        if pointer != 0 && size != 0 && offset == 0 {
            // SAFETY: the driver's mapping of `size` bytes.
            let known = unsafe { std::slice::from_raw_parts(pointer as *const u8, size) }.to_vec();
            s.mappings.insert(memory.as_raw(), Mapping { device: device.as_raw(), pointer, size, known });
        }
    });
    result
}

unsafe extern "system" fn unmap_memory(device: vk::Device, memory: vk::DeviceMemory) {
    with(|s| {
        if let Some(mut m) = s.mappings.remove(&memory.as_raw()) {
            flush(memory.as_raw(), &mut m);
        }
    });
    let f: vk::PFN_vkUnmapMemory = real(7);
    unsafe { f(device, memory) }
}

unsafe extern "system" fn queue_submit(queue: vk::Queue, count: u32, submits: *const vk::SubmitInfo<'static>, fence: vk::Fence)
                                       -> vk::Result {
    flush_all();
    let f: vk::PFN_vkQueueSubmit = real(8);
    unsafe { f(queue, count, submits, fence) }
}

unsafe extern "system" fn queue_submit2(queue: vk::Queue, count: u32, submits: *const vk::SubmitInfo2<'static>, fence: vk::Fence)
                                        -> vk::Result {
    flush_all();
    let f: vk::PFN_vkQueueSubmit2 = real(9);
    unsafe { f(queue, count, submits, fence) }
}

unsafe extern "system" fn queue_submit2_khr(queue: vk::Queue, count: u32, submits: *const vk::SubmitInfo2<'static>, fence: vk::Fence)
                                            -> vk::Result {
    flush_all();
    let f: vk::PFN_vkQueueSubmit2 = real(10);
    unsafe { f(queue, count, submits, fence) }
}

fn after(result: vk::Result) -> vk::Result {
    if result == vk::Result::SUCCESS {
        invalidate_all();
    }
    result
}

unsafe extern "system" fn wait_for_fences(device: vk::Device, count: u32, fences: *const vk::Fence, all: vk::Bool32, timeout: u64)
                                          -> vk::Result {
    let f: vk::PFN_vkWaitForFences = real(11);
    after(unsafe { f(device, count, fences, all, timeout) })
}

unsafe extern "system" fn get_fence_status(device: vk::Device, fence: vk::Fence) -> vk::Result {
    let f: vk::PFN_vkGetFenceStatus = real(12);
    after(unsafe { f(device, fence) })
}

unsafe extern "system" fn queue_wait_idle(queue: vk::Queue) -> vk::Result {
    let f: vk::PFN_vkQueueWaitIdle = real(13);
    after(unsafe { f(queue) })
}

unsafe extern "system" fn device_wait_idle(device: vk::Device) -> vk::Result {
    let f: vk::PFN_vkDeviceWaitIdle = real(14);
    after(unsafe { f(device) })
}

unsafe extern "system" fn wait_semaphores(device: vk::Device, info: *const vk::SemaphoreWaitInfo<'static>, timeout: u64) -> vk::Result {
    let f: vk::PFN_vkWaitSemaphores = real(15);
    after(unsafe { f(device, info, timeout) })
}

unsafe extern "system" fn wait_semaphores_khr(device: vk::Device, info: *const vk::SemaphoreWaitInfo<'static>, timeout: u64)
                                              -> vk::Result {
    let f: vk::PFN_vkWaitSemaphores = real(16);
    after(unsafe { f(device, info, timeout) })
}

fn counter_after(index: usize, device: vk::Device, semaphore: vk::Semaphore, out: *mut u64) -> vk::Result {
    let f: vk::PFN_vkGetSemaphoreCounterValue = real(index);
    let result = unsafe { f(device, semaphore, out) };
    if result == vk::Result::SUCCESS && !out.is_null() {
        let value = unsafe { *out };
        let moved = with(|s| s.counters.insert(semaphore.as_raw(), value).is_none_or(|last| last != value));
        if moved {
            invalidate_all();
        }
    }
    result
}

unsafe extern "system" fn counter_value(device: vk::Device, semaphore: vk::Semaphore, out: *mut u64) -> vk::Result {
    counter_after(17, device, semaphore, out)
}

unsafe extern "system" fn counter_value_khr(device: vk::Device, semaphore: vk::Semaphore, out: *mut u64) -> vk::Result {
    counter_after(18, device, semaphore, out)
}
