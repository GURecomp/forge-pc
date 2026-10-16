# Forge PC for Rust

A Rust SDK for Forge PC plugins. Anything a C/C++ plugin can do, a Rust plugin can do; what it
adds is closures for hooks (state without globals), typed guest-memory access, and panics that
are logged instead of crashing the game.

- `forge-pc-sys`: raw bindings to `include/forge_pc.h` (same layout, checked against the header).
- `forge-pc`: the safe API and the `plugin!` macro.
- `examples/selftest`: exercises every part against a mock host (not a mod).

## A plugin

```toml
# Cargo.toml
[lib]
crate-type = ["cdylib"]
[dependencies]
forge-pc = { path = "../forge-pc" }
```

```rust
use forge_pc::*;

struct MyMod;
impl Plugin for MyMod {
    fn new() -> Self { MyMod }
    fn init(&mut self) {
        // pre hook: runs when the game enters the function (offset in "main")
        hook_pre(main_addr(0x1C358), |cpu| {
            forge_info!("r0 = {:#x}", cpu.r(0));
            Action::Continue
        });
        // override (Forge PC 0.4): your function instead of the game's, calling the original
        hook_override(main_addr(0x2A410), |cpu| {
            let r = call_original(main_addr(0x2A410), &[cpu.r(0), cpu.r(1) + 1], &[]);
            cpu.set_ret(r.map_or(0, |r| r.r0));
        });
        // a field two pointers deep:  *(*(main+0x2126498) + 0x3830) + 0xBC
        if let Some(a) = mem::follow(main_addr(0x2126498), &[0x3830, 0xBC]) {
            mem::write(a, &0u16);
        }
    }
}
plugin!(MyMod, "My mod", 2); // 2 = oldest Forge PC API minor it works with (4 for overrides)
```

Hook closures are `Fn + Send + Sync`: the game calls them from several threads at once, so
keep state in atomics or a `Mutex`. Hooks go away with the plugin; their closures are freed in
`forge_onUnload` (hot reload and mod switch-off included). Game addresses are `u32` guest
addresses, read and written through `forge_pc::mem`.

## Settings file

```rust
let cfg = Config::open("mymod.ini").unwrap();      // <mods dir>/<your mod folder>/mymod.ini
let speed = cfg.get("main", "speed", 1.5f32);        // default if the key is missing
cfg.save();                                          // first start: writes the missing keys
// in Plugin::update: `if cfg.changed() { /* hand edit: read the values again */ }`
```

## Build and install

Needs Rust (stable, `x86_64-pc-windows-msvc`).

```
cd rust
cargo build --release
```

Copy `target\release\<crate name with _>.dll` to
`<game>\mods\0100770008DD8000\<Mod name>\plugins\`. The C runtime is linked statically
(`.cargo/config.toml`), so players need nothing extra. Hot reload works like for C plugins: a
rebuilt dll is picked up in game (copy it in as one step, not while it is half written).
