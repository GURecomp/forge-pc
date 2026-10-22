//! Safe Rust SDK for Forge PC plugins.
//!
//! A plugin is a `cdylib` crate. Implement [`Plugin`] and put [`plugin!`] at the crate root:
//!
//! ```ignore
//! use forge_pc::*;
//!
//! struct MyMod;
//! impl Plugin for MyMod {
//!     fn new() -> Self { MyMod }
//!     fn init(&mut self) {
//!         let target = main_addr(0x1C358);
//!         hook_pre(target, |cpu| {
//!             forge_info!("called with r0 = {:#x}", cpu.r(0));
//!             Action::Continue
//!         });
//!     }
//! }
//! plugin!(MyMod, "My mod", 2);
//! ```
//!
//! Game addresses are 32-bit guest addresses (`u32`), never host pointers: use [`mem`].
//! Hook closures run on the game's threads, concurrently: they are `Fn + Send + Sync`, keep
//! state in atomics / `Mutex`. Hooks are removed by Forge when the plugin unloads; their
//! closures are freed right after, in `forge_onUnload`.

pub use forge_pc_sys as sys;

use core::ffi::{c_char, c_void};
use std::ffi::CString;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Mutex;
use std::sync::atomic::{AtomicPtr, Ordering};

/// Minor version of the Forge PC API this crate was written against.
pub const API_MINOR: u16 = sys::FORGE_PC_VERSION_MINOR;

// --- the API table ------------------------------------------------------------------------------

static API: AtomicPtr<sys::ForgeApi> = AtomicPtr::new(core::ptr::null_mut());
static NAME: Mutex<Option<CString>> = Mutex::new(None);

#[doc(hidden)]
pub fn __set_api(api: *const sys::ForgeApi) {
    API.store(api as *mut _, Ordering::Release);
}

/// The loader's API table (valid once Forge called `forge_setApi`, i.e. in every callback).
pub fn api() -> &'static sys::ForgeApi {
    let p = API.load(Ordering::Acquire);
    assert!(!p.is_null(), "forge API used before forge_setApi");
    unsafe { &*p }
}

fn api_covers(end_of_entry: usize) -> bool {
    api().size as usize >= end_of_entry
}

// --- logging ------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Level {
    Debug = sys::FORGE_LOG_DEBUG,
    Info = sys::FORGE_LOG_INFO,
    Warn = sys::FORGE_LOG_WARN,
    Error = sys::FORGE_LOG_ERROR,
}

fn cstring(s: &str) -> CString {
    CString::new(s.replace('\0', " ")).unwrap_or_default()
}

/// Writes a line to forge.log (and the game log) under the plugin's name.
pub fn log(level: Level, msg: &str) {
    let name = NAME.lock().ok().and_then(|n| n.clone()).unwrap_or_else(|| cstring("rust plugin"));
    let msg = cstring(msg);
    unsafe { (api().log)(level as i32, name.as_ptr(), msg.as_ptr()) }
}

#[macro_export]
macro_rules! forge_debug { ($($a:tt)*) => { $crate::log($crate::Level::Debug, &format!($($a)*)) } }
#[macro_export]
macro_rules! forge_info { ($($a:tt)*) => { $crate::log($crate::Level::Info, &format!($($a)*)) } }
#[macro_export]
macro_rules! forge_warn { ($($a:tt)*) => { $crate::log($crate::Level::Warn, &format!($($a)*)) } }
#[macro_export]
macro_rules! forge_error { ($($a:tt)*) => { $crate::log($crate::Level::Error, &format!($($a)*)) } }

// --- guest memory -------------------------------------------------------------------------------

/// Plain-old-data: any bit pattern is valid. Implement it for your own `#[repr(C)]` structs.
///
/// # Safety
/// `Self` must have no padding-sensitive invariants, pointers or niches.
pub unsafe trait Pod: Copy + 'static {}
macro_rules! pod { ($($t:ty),*) => { $(unsafe impl Pod for $t {})* } }
pod!(u8, u16, u32, u64, i8, i16, i32, i64, f32, f64);
unsafe impl<T: Pod, const N: usize> Pod for [T; N] {}

pub mod mem {
    use super::*;

    /// Base address of a game module ("main", "sdk", "subsdk0", "rtld"); 0 if unknown.
    pub fn module_base(name: &str) -> u32 {
        let n = cstring(name);
        unsafe { (api().mem_getModuleBase)(n.as_ptr()) }
    }

    pub fn is_valid(addr: u32, size: u32) -> bool {
        unsafe { (api().mem_isValid)(addr, size) }
    }

    pub fn read<T: Pod>(addr: u32) -> Option<T> {
        let mut v = core::mem::MaybeUninit::<T>::zeroed();
        let ok = unsafe {
            (api().mem_read)(addr, v.as_mut_ptr() as *mut c_void, core::mem::size_of::<T>() as u32)
        };
        ok.then(|| unsafe { v.assume_init() })
    }

    pub fn write<T: Pod>(addr: u32, value: &T) -> bool {
        unsafe {
            (api().mem_write)(addr, value as *const T as *const c_void, core::mem::size_of::<T>() as u32)
        }
    }

    pub fn read_bytes(addr: u32, len: usize) -> Option<Vec<u8>> {
        let mut v = vec![0u8; len];
        let ok = unsafe { (api().mem_read)(addr, v.as_mut_ptr() as *mut c_void, len as u32) };
        ok.then_some(v)
    }

    pub fn write_bytes(addr: u32, data: &[u8]) -> bool {
        unsafe { (api().mem_write)(addr, data.as_ptr() as *const c_void, data.len() as u32) }
    }

    /// Reads a NUL-terminated string (at most `max` bytes).
    pub fn read_cstr(addr: u32, max: usize) -> Option<String> {
        let mut out = Vec::new();
        for i in 0..max as u32 {
            let b: u8 = read(addr + i)?;
            if b == 0 {
                break;
            }
            out.push(b);
        }
        Some(String::from_utf8_lossy(&out).into_owned())
    }

    /// Follows a pointer chain: for each offset, `p = read_u32(p)` (None if unreadable or 0), then
    /// `p += offset`. `follow(a, &[0x3830, 0xBC])` is "`*(*a + 0x3830) + 0xBC`", the address of a
    /// field two pointers deep (the shape of most Atmosphere cheat codes).
    pub fn follow(base: u32, offsets: &[u32]) -> Option<u32> {
        let mut p = base;
        for &o in offsets {
            p = read::<u32>(p)?;
            if p == 0 {
                return None;
            }
            p = p.wrapping_add(o);
        }
        Some(p)
    }

    /// A zeroed block of guest memory of your own (game code can read and write it too). Freed
    /// when dropped: drop it in `Plugin::unload` at the latest.
    pub struct Alloc(u32);
    impl Alloc {
        pub fn new(size: u32, align: u32) -> Option<Alloc> {
            if !super::api_covers(sys::API_SIZE_0_2) {
                return None;
            }
            let a = unsafe { (api().mem_alloc)(size, align) };
            (a != 0).then_some(Alloc(a))
        }
        pub fn addr(&self) -> u32 {
            self.0
        }
    }
    impl Drop for Alloc {
        fn drop(&mut self) {
            unsafe { (api().mem_free)(self.0) }
        }
    }
}

/// `main` module base + offset (offsets in the notes and in cheat codes are relative to main).
pub fn main_addr(offset: u32) -> u32 {
    mem::module_base("main").wrapping_add(offset)
}

// --- cpu state, hooks ---------------------------------------------------------------------------

/// The guest CPU at a hook: arguments in r0-r3 (then the stack), float arguments in s0-s15.
#[repr(transparent)]
pub struct Cpu(sys::ForgeCpu);

impl Cpu {
    pub fn r(&self, i: usize) -> u32 {
        self.0.r[i]
    }
    pub fn set_r(&mut self, i: usize, v: u32) {
        self.0.r[i] = v;
    }
    pub fn sp(&self) -> u32 {
        self.0.r[13]
    }
    pub fn lr(&self) -> u32 {
        self.0.r[14]
    }
    /// The integer / pointer return value (r0).
    pub fn set_ret(&mut self, v: u32) {
        self.0.r[0] = v;
    }
    pub fn s(&self, i: usize) -> f32 {
        unsafe { self.0.vfp.s[i] }
    }
    pub fn set_s(&mut self, i: usize, v: f32) {
        unsafe { self.0.vfp.s[i] = v }
    }
    pub fn d(&self, i: usize) -> f64 {
        unsafe { self.0.vfp.d[i] }
    }
    pub fn set_d(&mut self, i: usize, v: f64) {
        unsafe { self.0.vfp.d[i] = v }
    }
    pub fn raw(&mut self) -> &mut sys::ForgeCpu {
        &mut self.0
    }
}

/// What a pre hook decides.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    /// Run the function (and the other hooks).
    Continue,
    /// Skip the function and return to its caller now: set the return value first.
    Return,
    /// Forge PC 0.7: continue at `cpu.raw().r[15]` with the registers as the hook left them.
    /// Post hooks don't run; hooks on the target address do. Jumping to the hooked address
    /// itself is treated as `Continue`. The plugin must ask for minor 7.
    Jump,
}

/// What a post hook is told about the call it follows.
#[derive(Clone, Copy, Debug)]
pub struct PostArgs {
    /// r0-r3 at entry.
    pub r: [u32; 4],
    /// The caller's return address.
    pub lr: u32,
    /// SP at entry (the 5th and later arguments are at `sp`, `sp + 4`, ...).
    pub sp: u32,
}

/// A registered hook. Forge removes it when the plugin unloads; dropping the handle does not.
#[derive(Clone, Copy)]
pub struct Hook(*mut sys::ForgeHook);
unsafe impl Send for Hook {}
unsafe impl Sync for Hook {}
impl Hook {
    pub fn set_enabled(&self, enabled: bool) {
        unsafe { (api().hook_enable)(self.0, enabled) }
    }
}

struct Keep {
    ptr: *mut c_void,
    free: unsafe fn(*mut c_void),
}
unsafe impl Send for Keep {}
static KEEP: Mutex<Vec<Keep>> = Mutex::new(Vec::new());

unsafe fn free_box<F>(p: *mut c_void) {
    drop(unsafe { Box::from_raw(p as *mut F) });
}

/// Moves the closure to the heap for the loader to call back with; freed by `__release_all`.
fn leak<F: 'static>(f: F) -> *mut c_void {
    let p = Box::into_raw(Box::new(f)) as *mut c_void;
    KEEP.lock().unwrap_or_else(|e| e.into_inner()).push(Keep { ptr: p, free: free_box::<F> });
    p
}

#[doc(hidden)]
pub fn __release_all() {
    let all = std::mem::take(&mut *KEEP.lock().unwrap_or_else(|e| e.into_inner()));
    for k in all {
        unsafe { (k.free)(k.ptr) }
    }
}

unsafe extern "C" fn pre_tramp<F: Fn(&mut Cpu) -> Action + Send + Sync + 'static>(
    cpu: *mut sys::ForgeCpu,
    user: *mut c_void,
) -> i32 {
    let f = unsafe { &*(user as *const F) };
    let cpu = unsafe { &mut *(cpu as *mut Cpu) };
    match catch_unwind(AssertUnwindSafe(|| f(cpu))) {
        Ok(Action::Return) => sys::FORGE_HOOK_RETURN,
        Ok(Action::Jump) => sys::FORGE_HOOK_JUMP,
        _ => sys::FORGE_HOOK_CONTINUE,
    }
}

unsafe extern "C" fn post_tramp<F: Fn(&mut Cpu, &PostArgs) + Send + Sync + 'static>(
    cpu: *mut sys::ForgeCpu,
    args: *const u32,
    user: *mut c_void,
) {
    let f = unsafe { &*(user as *const F) };
    let cpu = unsafe { &mut *(cpu as *mut Cpu) };
    let a = unsafe { core::slice::from_raw_parts(args, 6) };
    let args = PostArgs { r: [a[0], a[1], a[2], a[3]], lr: a[4], sp: a[5] };
    let _ = catch_unwind(AssertUnwindSafe(|| f(cpu, &args)));
}

unsafe extern "C" fn override_tramp<F: Fn(&mut Cpu) + Send + Sync + 'static>(
    cpu: *mut sys::ForgeCpu,
    user: *mut c_void,
) {
    let f = unsafe { &*(user as *const F) };
    let cpu = unsafe { &mut *(cpu as *mut Cpu) };
    let _ = catch_unwind(AssertUnwindSafe(|| f(cpu)));
}

fn made(h: *mut sys::ForgeHook, target: u32, what: &str) -> Option<Hook> {
    if h.is_null() {
        forge_error!("{what} at {target:#x}: not the start of a recompiled block");
        None
    } else {
        Some(Hook(h))
    }
}

/// Runs `f` when the game enters the function at `target` (a guest address: [`main_addr`]).
pub fn hook_pre<F>(target: u32, f: F) -> Option<Hook>
where
    F: Fn(&mut Cpu) -> Action + Send + Sync + 'static,
{
    let user = leak(f);
    let h = unsafe { (api().hook_create)(target, Some(pre_tramp::<F>), None, user) };
    made(h, target, "hook")
}

/// Runs `f` when the function at `target` returns (it may change the return value).
pub fn hook_post<F>(target: u32, f: F) -> Option<Hook>
where
    F: Fn(&mut Cpu, &PostArgs) + Send + Sync + 'static,
{
    let user = leak(f);
    let h = unsafe { (api().hook_create)(target, None, Some(post_tramp::<F>), user) };
    made(h, target, "hook")
}

/// Replaces the function at `target` (Forge PC 0.4). Call [`call_original`] to run the game's
/// function (or the next override) from inside `f`; leave the return value in the cpu.
pub fn hook_override<F>(target: u32, f: F) -> Option<Hook>
where
    F: Fn(&mut Cpu) + Send + Sync + 'static,
{
    if !api_covers(sys::API_SIZE_0_4) {
        forge_error!("hook_override needs Forge PC 0.4 or newer");
        return None;
    }
    let user = leak(f);
    let h = unsafe { (api().hook_override)(target, override_tramp::<F>, user) };
    made(h, target, "override")
}

// --- calling guest code -------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, Default)]
pub struct CallResult {
    pub r0: u32,
    pub r1: u32,
    pub s0: f32,
    pub d0: f64,
}

fn do_call(
    f: unsafe extern "C" fn(u32, *const u32, u32, *const f32, u32, *mut sys::ForgeCallResult) -> bool,
    function: u32,
    args: &[u32],
    fargs: &[f32],
) -> Option<CallResult> {
    let mut out = sys::ForgeCallResult::default();
    let ok = unsafe {
        f(function, args.as_ptr(), args.len() as u32, fargs.as_ptr(), fargs.len() as u32, &mut out)
    };
    ok.then_some(CallResult { r0: out.r0, r1: out.r1, s0: out.s0, d0: out.d0 })
}

/// Calls a guest function (game threads only; fails if it makes a system call or waits).
pub fn call(function: u32, args: &[u32], fargs: &[f32]) -> Option<CallResult> {
    do_call(api().call, function, args, fargs)
}

/// From an override: calls the function it replaced (or the next override down). Forge PC 0.4.
pub fn call_original(function: u32, args: &[u32], fargs: &[f32]) -> Option<CallResult> {
    if !api_covers(sys::API_SIZE_0_4) {
        return None;
    }
    do_call(api().call_original, function, args, fargs)
}

// --- the rest of the API ------------------------------------------------------------------------

/// An MT Framework singleton by DTI class name ("sPlayer", "sQuest", ...); None if unknown.
pub fn singleton(name: &str) -> Option<u32> {
    let n = cstring(name);
    let a = unsafe { (api().singleton_getByName)(n.as_ptr()) };
    (a != 0).then_some(a)
}

/// First match of a byte pattern ("E9 2D ?? 40") in a module, at or after `from`.
pub fn pattern_find(module: &str, pattern: &str, from: u32) -> Option<u32> {
    let (m, p) = (cstring(module), cstring(pattern));
    let a = unsafe { (api().pattern_find)(m.as_ptr(), p.as_ptr(), from) };
    (a != 0).then_some(a)
}

/// Size in pixels of the picture the game is shown in (Forge PC 0.3).
pub fn screen_size() -> Option<(u32, u32)> {
    if !api_covers(sys::API_SIZE_0_3) {
        return None;
    }
    let (mut w, mut h) = (0u32, 0u32);
    unsafe { (api().screen_size)(&mut w, &mut h) }.then_some((w, h))
}

/// True if `addr` is the start of a recompiled block, i.e. a place a hook can be put (Forge PC
/// 0.7). Logs nothing. `None` if the loader is older than 0.7.
pub fn is_block(addr: u32) -> Option<bool> {
    if !api_covers(sys::API_SIZE_0_7) {
        return None;
    }
    Some(unsafe { (api().is_block)(addr) })
}

/// `<game>/mods/<title id>`.
pub fn mods_dir() -> String {
    unsafe { std::ffi::CStr::from_ptr((api().mods_dir)()) }.to_string_lossy().into_owned()
}

pub fn menu_is_open() -> bool {
    unsafe { (api().menu_isOpen)() }
}

// --- settings files (Forge PC 0.5) ---------------------------------------------------------------

/// A value type a [`Config`] can hold.
pub trait ConfigValue: Sized {
    fn get(cfg: &Config, section: &str, key: &str, default: Self) -> Self;
}

impl ConfigValue for i32 {
    fn get(c: &Config, s: &str, k: &str, d: i32) -> i32 {
        let (s, k) = (cstring(s), cstring(k));
        unsafe { (api().config_int)(c.0, s.as_ptr(), k.as_ptr(), d) }
    }
}
impl ConfigValue for f32 {
    fn get(c: &Config, s: &str, k: &str, d: f32) -> f32 {
        let (s, k) = (cstring(s), cstring(k));
        unsafe { (api().config_float)(c.0, s.as_ptr(), k.as_ptr(), d) }
    }
}
impl ConfigValue for bool {
    fn get(c: &Config, s: &str, k: &str, d: bool) -> bool {
        let (s, k) = (cstring(s), cstring(k));
        unsafe { (api().config_bool)(c.0, s.as_ptr(), k.as_ptr(), d) }
    }
}
impl ConfigValue for String {
    fn get(c: &Config, s: &str, k: &str, d: String) -> String {
        let (s, k, d) = (cstring(s), cstring(k), cstring(&d));
        let p = unsafe { (api().config_string)(c.0, s.as_ptr(), k.as_ptr(), d.as_ptr()) };
        if p.is_null() {
            return String::new();
        }
        unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}

/// A settings file of your mod: `<mods dir>/<your mod folder>/<name>` (ini: `[section]` and
/// `key = value`). Every `get` takes a default; [`Config::save`] writes the keys the file does
/// not have yet, so the first start produces a complete file. Poll [`Config::changed`] from
/// `Plugin::update` to pick up hand edits while the game runs.
///
/// ```ignore
/// let cfg = Config::open("mymod.ini")?;
/// let speed = cfg.get("main", "speed", 1.5f32);
/// cfg.save();
/// ```
pub struct Config(*mut sys::ForgeConfig);
unsafe impl Send for Config {}
unsafe impl Sync for Config {}

impl Config {
    /// Opens (or prepares) a settings file; None before Forge PC 0.5 or for a bad name.
    pub fn open(name: &str) -> Option<Config> {
        if !api_covers(sys::API_SIZE_0_5) {
            forge_error!("config files need Forge PC 0.5 or newer");
            return None;
        }
        let n = cstring(name);
        let p = unsafe { (api().config_open)(n.as_ptr()) };
        (!p.is_null()).then_some(Config(p))
    }

    pub fn get<T: ConfigValue>(&self, section: &str, key: &str, default: T) -> T {
        T::get(self, section, key, default)
    }

    /// Sets a value (written by the next [`Config::save`]).
    pub fn set(&self, section: &str, key: &str, value: impl ToString) -> bool {
        let (s, k, v) = (cstring(section), cstring(key), cstring(&value.to_string()));
        unsafe { (api().config_set)(self.0, s.as_ptr(), k.as_ptr(), v.as_ptr()) }
    }

    /// Writes new keys (defaults you asked for) and changed ones; comments and the rest stay.
    pub fn save(&self) -> bool {
        unsafe { (api().config_save)(self.0) }
    }

    /// True once after the file changed on disk; the values were re-read.
    pub fn changed(&self) -> bool {
        unsafe { (api().config_changed)(self.0) }
    }
}

// --- the plugin entry points --------------------------------------------------------------------

/// Implement this and put [`plugin!`] at the crate root. `init` and `update` run on the game's
/// main thread; `unload` runs at exit and before a hot reload (stop your threads, free guest
/// memory, put back game memory you changed that the game won't rewrite).
pub trait Plugin: Send + 'static {
    fn new() -> Self
    where
        Self: Sized;
    fn init(&mut self) {}
    fn update(&mut self, _dt: f32) {}
    fn unload(&mut self) {}
    /// Forge PC 0.6: once per presented frame, on the presentation thread. Record raw Vulkan into
    /// `p.vk_command_buffer`; the image is in TRANSFER_DST_OPTIMAL on entry and must be again on
    /// return (see `ForgePresent` in forge_pc.h). Set `WANTS_PRESENT` to receive it; the
    /// plugin must then ask for minor 6.
    fn present(&mut self, _p: &sys::ForgePresent) {}
    const WANTS_PRESENT: bool = false;
}

#[doc(hidden)]
pub fn __on_load(info: *mut sys::ForgePluginInfo, name: &str, min_minor: u16) {
    let info = unsafe { &mut *info };
    info.required_ver = sys::ForgeVersion { major: sys::FORGE_PC_VERSION_MAJOR, minor: min_minor, patch: 0 };
    info.name = [0; 64];
    for (i, b) in name.bytes().take(63).enumerate() {
        info.name[i] = b as c_char;
    }
    *NAME.lock().unwrap_or_else(|e| e.into_inner()) = Some(cstring(name));
    // a panic in any callback ends up in forge.log, with its location
    std::panic::set_hook(Box::new(|p| {
        let loc = p.location().map(|l| format!(" at {}:{}", l.file(), l.line())).unwrap_or_default();
        let msg = if let Some(s) = p.payload().downcast_ref::<&str>() {
            (*s).to_string()
        } else if let Some(s) = p.payload().downcast_ref::<String>() {
            s.clone()
        } else {
            "panic".to_string()
        };
        log(Level::Error, &format!("PANIC: {msg}{loc}"));
    }));
}

/// Runs an entry point; a panic is logged (by the panic hook) and does not cross into the game.
#[doc(hidden)]
pub fn __guard(f: impl FnOnce()) {
    let _ = catch_unwind(AssertUnwindSafe(f));
}

/// Generates the exports Forge looks for. `plugin!(Type, "Name", min_minor)`: `min_minor` is the
/// oldest Forge PC API minor version your plugin works with (2 without 0.3 / 0.4 features, 4
/// with `hook_override`); an older loader then refuses the plugin cleanly.
#[macro_export]
macro_rules! plugin {
    ($ty:ty, $name:expr, $min_minor:expr) => {
        static __FORGE_INSTANCE: ::std::sync::Mutex<Option<$ty>> = ::std::sync::Mutex::new(None);

        #[unsafe(no_mangle)]
        pub extern "C" fn forge_setApi(api: *const $crate::sys::ForgeApi) {
            $crate::__set_api(api);
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn forge_onLoad(info: *mut $crate::sys::ForgePluginInfo) {
            $crate::__on_load(info, $name, $min_minor);
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn forge_onInit() {
            $crate::__guard(|| {
                let mut p = <$ty as $crate::Plugin>::new();
                $crate::Plugin::init(&mut p);
                *__FORGE_INSTANCE.lock().unwrap_or_else(|e| e.into_inner()) = Some(p);
            });
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn forge_onUpdate(dt: f32) {
            $crate::__guard(|| {
                if let Some(p) = __FORGE_INSTANCE.lock().unwrap_or_else(|e| e.into_inner()).as_mut() {
                    $crate::Plugin::update(p, dt);
                }
            });
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn forge_onPresent(p: *const $crate::sys::ForgePresent) {
            if !<$ty as $crate::Plugin>::WANTS_PRESENT || p.is_null() {
                return;
            }
            $crate::__guard(|| {
                if let Some(plugin) = __FORGE_INSTANCE.lock().unwrap_or_else(|e| e.into_inner()).as_mut() {
                    $crate::Plugin::present(plugin, unsafe { &*p });
                }
            });
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn forge_onUnload() {
            $crate::__guard(|| {
                let p = __FORGE_INSTANCE.lock().unwrap_or_else(|e| e.into_inner()).take();
                if let Some(mut p) = p {
                    $crate::Plugin::unload(&mut p);
                }
            });
            // Forge dropped this plugin's hooks and waited for its callbacks: nothing calls
            // the closures any more
            $crate::__release_all();
        }
    };
}
