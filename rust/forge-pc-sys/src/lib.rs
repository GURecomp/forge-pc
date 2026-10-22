//! Raw bindings to `include/forge_pc.h` (Forge PC plugin API). Layout must match the header:
//! `ForgeApi` only ever grows at the end. Use the `forge-pc` crate unless you need this.
#![allow(non_camel_case_types, non_snake_case, clippy::missing_safety_doc)]

use core::ffi::{c_char, c_int, c_void};

pub const FORGE_PC_VERSION_MAJOR: u16 = 0;
pub const FORGE_PC_VERSION_MINOR: u16 = 7;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ForgeVersion {
    pub major: u16,
    pub minor: u16,
    pub patch: u16,
}

#[repr(C)]
pub struct ForgePluginInfo {
    pub required_ver: ForgeVersion,
    pub name: [c_char; 64],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union ForgeVfp {
    pub s: [f32; 64],
    pub d: [f64; 32],
    pub sw: [u32; 64],
    pub dw: [u64; 32],
}

/// Guest CPU state at a hook (r[13] = sp, r[14] = lr, r[15] = pc).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ForgeCpu {
    pub r: [u32; 16],
    pub n: u32,
    pub z: u32,
    pub c: u32,
    pub v: u32,
    pub q: u32,
    pub ge: u32,
    pub thumb: u32,
    pub fpscr: u32,
    pub vfp: ForgeVfp,
}

/// One presented frame (Forge PC 0.6), passed to `forge_onPresent`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ForgePresent {
    pub size: u32,
    pub vk_api_version: u32,
    pub vk_instance: *mut c_void,
    pub vk_get_instance_proc: *mut c_void,
    pub vk_physical_device: *mut c_void,
    pub vk_device: *mut c_void,
    pub vk_queue: *mut c_void,
    pub vk_queue_family: u32,
    pub vk_command_buffer: *mut c_void,
    pub vk_image: u64,
    pub vk_format: u32,
    pub width: u32,
    pub height: u32,
    pub image_count: u32,
    pub image_index: u32,
    pub lock_queue: unsafe extern "C" fn(),
    pub unlock_queue: unsafe extern "C" fn(),
}

pub const FORGE_HOOK_CONTINUE: c_int = 0;
pub const FORGE_HOOK_RETURN: c_int = 1;
/// 0.7: continue at `cpu.r[15]` with the registers as the hook left them.
pub const FORGE_HOOK_JUMP: c_int = 2;

pub type ForgeHookPre = unsafe extern "C" fn(cpu: *mut ForgeCpu, user: *mut c_void) -> c_int;
pub type ForgeHookPost =
    unsafe extern "C" fn(cpu: *mut ForgeCpu, args: *const u32, user: *mut c_void);
pub type ForgeOverride = unsafe extern "C" fn(cpu: *mut ForgeCpu, user: *mut c_void);

/// Opaque.
#[repr(C)]
pub struct ForgeHook {
    _private: [u8; 0],
}

/// Opaque: a plugin's settings file (0.5).
#[repr(C)]
pub struct ForgeConfig {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ForgeCallResult {
    pub r0: u32,
    pub r1: u32,
    pub s0: f32,
    pub d0: f64,
}

pub const FORGE_LOG_DEBUG: c_int = 0;
pub const FORGE_LOG_INFO: c_int = 1;
pub const FORGE_LOG_WARN: c_int = 2;
pub const FORGE_LOG_ERROR: c_int = 3;

type CallFn = unsafe extern "C" fn(
    function: u32,
    args: *const u32,
    count: u32,
    fargs: *const f32,
    fcount: u32,
    out: *mut ForgeCallResult,
) -> bool;

#[repr(C)]
pub struct ForgeApi {
    pub version: ForgeVersion,
    pub size: u32,

    pub log: unsafe extern "C" fn(level: c_int, plugin: *const c_char, message: *const c_char),
    pub log_getLevel: unsafe extern "C" fn() -> c_int,

    pub mem_getModuleBase: unsafe extern "C" fn(name: *const c_char) -> u32,
    pub mem_getModuleSize: unsafe extern "C" fn(name: *const c_char) -> u32,

    pub mem_isValid: unsafe extern "C" fn(addr: u32, size: u32) -> bool,
    pub mem_read: unsafe extern "C" fn(addr: u32, out: *mut c_void, size: u32) -> bool,
    pub mem_write: unsafe extern "C" fn(addr: u32, data: *const c_void, size: u32) -> bool,
    pub mem_ptr: unsafe extern "C" fn(addr: u32, size: u32) -> *mut c_void,

    pub hook_create: unsafe extern "C" fn(
        target: u32,
        pre: Option<ForgeHookPre>,
        post: Option<ForgeHookPost>,
        user: *mut c_void,
    ) -> *mut ForgeHook,
    pub hook_enable: unsafe extern "C" fn(hook: *mut ForgeHook, enabled: bool),

    pub call: CallFn,

    pub singleton_getByName: unsafe extern "C" fn(name: *const c_char) -> u32,
    pub singleton_getById: unsafe extern "C" fn(id: u32) -> u32,
    pub singleton_getAll: unsafe extern "C" fn(out: *mut u32, max: u32) -> u32,
    pub dti_of: unsafe extern "C" fn(object: u32) -> u32,
    pub dti_name: unsafe extern "C" fn(dti: u32) -> *const c_char,
    pub dti_makeId: unsafe extern "C" fn(name: *const c_char) -> u32,

    pub pattern_find:
        unsafe extern "C" fn(module: *const c_char, pattern: *const c_char, from: u32) -> u32,

    pub mods_dir: unsafe extern "C" fn() -> *const c_char,

    pub imgui_version: *const c_char,
    pub menu_isOpen: unsafe extern "C" fn() -> bool,

    // 0.2
    pub mem_alloc: unsafe extern "C" fn(size: u32, align: u32) -> u32,
    pub mem_free: unsafe extern "C" fn(addr: u32),

    // 0.3
    pub screen_size: unsafe extern "C" fn(width: *mut u32, height: *mut u32) -> bool,

    // 0.4
    pub call_original: CallFn,
    pub hook_override: unsafe extern "C" fn(
        target: u32,
        f: ForgeOverride,
        user: *mut c_void,
    ) -> *mut ForgeHook,

    // 0.5
    pub config_open: unsafe extern "C" fn(name: *const c_char) -> *mut ForgeConfig,
    pub config_int: unsafe extern "C" fn(
        cfg: *mut ForgeConfig,
        section: *const c_char,
        key: *const c_char,
        def: i32,
    ) -> i32,
    pub config_float: unsafe extern "C" fn(
        cfg: *mut ForgeConfig,
        section: *const c_char,
        key: *const c_char,
        def: f32,
    ) -> f32,
    pub config_bool: unsafe extern "C" fn(
        cfg: *mut ForgeConfig,
        section: *const c_char,
        key: *const c_char,
        def: bool,
    ) -> bool,
    pub config_string: unsafe extern "C" fn(
        cfg: *mut ForgeConfig,
        section: *const c_char,
        key: *const c_char,
        def: *const c_char,
    ) -> *const c_char,
    pub config_set: unsafe extern "C" fn(
        cfg: *mut ForgeConfig,
        section: *const c_char,
        key: *const c_char,
        value: *const c_char,
    ) -> bool,
    pub config_save: unsafe extern "C" fn(cfg: *mut ForgeConfig) -> bool,
    pub config_changed: unsafe extern "C" fn(cfg: *mut ForgeConfig) -> bool,
    // --- 0.7 ---
    pub is_block: unsafe extern "C" fn(addr: u32) -> bool,
}

// ForgeApi is a table of function pointers the loader keeps alive for the whole process.
unsafe impl Sync for ForgeApi {}
unsafe impl Send for ForgeApi {}

// Offsets of the version fields' ends: the `size` of the loader's table says which entries exist.
pub const API_SIZE_0_2: usize = core::mem::offset_of!(ForgeApi, mem_free) + core::mem::size_of::<usize>();
pub const API_SIZE_0_3: usize = core::mem::offset_of!(ForgeApi, screen_size) + core::mem::size_of::<usize>();
pub const API_SIZE_0_4: usize = core::mem::offset_of!(ForgeApi, hook_override) + core::mem::size_of::<usize>();
pub const API_SIZE_0_5: usize = core::mem::offset_of!(ForgeApi, config_changed) + core::mem::size_of::<usize>();
pub const API_SIZE_0_7: usize = core::mem::offset_of!(ForgeApi, is_block) + core::mem::size_of::<usize>();

const _: () = assert!(core::mem::size_of::<ForgeCpu>() == 352);
const _: () = assert!(core::mem::size_of::<ForgePluginInfo>() == 70);
const _: () = assert!(core::mem::size_of::<ForgePresent>() == 112);
