//! Self-test plugin for the mock-host harness (offsets inside the harness' fake memory).
use forge_pc::*;
use std::sync::atomic::{AtomicU32, Ordering};

static POSTS: AtomicU32 = AtomicU32::new(0);

fn bump(off: u32) {
    let a = main_addr(off);
    let v = mem::read::<u32>(a).unwrap_or(0);
    mem::write(a, &(v + 1));
}

struct SelfTest {
    cfg: Option<Config>,
}

impl Plugin for SelfTest {
    fn new() -> Self {
        SelfTest { cfg: None }
    }

    fn init(&mut self) {
        bump(0x200); // init ran
        // override with call_original, a post hook that sees the final value, a RETURN, a panic
        hook_override(main_addr(0x5000), |cpu| {
            let res = call_original(main_addr(0x5000), &[cpu.r(0), cpu.r(1)], &[]);
            bump(0x204);
            cpu.set_ret(res.map_or(0xDEAD, |r| r.r0 + 1000));
        });
        hook_post(main_addr(0x5000), |cpu, _| {
            POSTS.fetch_add(1, Ordering::Relaxed);
            bump(0x208);
            mem::write(main_addr(0x20C), &cpu.r(0));
        });
        hook_pre(main_addr(0x7100), |cpu| {
            cpu.set_ret(77);
            Action::Return
        });
        hook_pre(main_addr(0x7200), |_| -> Action { panic!("selftest panic on purpose") });
        // a pointer chain in the fake memory: [0x300] -> 0x400; [0x404] -> 0x500
        mem::write(main_addr(0x300), &main_addr(0x400));
        mem::write(main_addr(0x404), &main_addr(0x500));
        let chain = mem::follow(main_addr(0x300), &[4, 8]);
        mem::write(main_addr(0x210), &chain.unwrap_or(0));
        // typed reads
        mem::write(main_addr(0x220), &[1u8, 2, 3, 4]);
        let w: u32 = mem::read(main_addr(0x220)).unwrap_or(0);
        mem::write(main_addr(0x214), &w);
        let guest = mem::Alloc::new(64, 16); // the mock has no guest heap: must be None, not a crash
        mem::write(main_addr(0x218), &(guest.is_some() as u32));
        // settings file: defaults, saved on first start, the file's values win afterwards
        self.cfg = Config::open("selftest.ini");
        if let Some(c) = &self.cfg {
            let n = c.get("main", "count", 7i32);
            let f = c.get("main", "ratio", 1.5f32);
            let b = c.get("main", "on", true);
            let t = c.get("names", "title", String::from("hello"));
            mem::write(main_addr(0x230), &n);
            mem::write(main_addr(0x234), &f.to_bits());
            mem::write(main_addr(0x238), &(b as u32));
            mem::write(main_addr(0x248), &(t.len() as u32));
            c.set("main", "count", 9);
            c.save();
        }
        mem::write(main_addr(0x23C), &(Config::open("../evil.ini").is_none() as u32));
    }

    fn update(&mut self, _dt: f32) {
        bump(0x21C);
        if let Some(c) = &self.cfg {
            if c.changed() {
                bump(0x244);
                let f = c.get("main", "ratio", 1.5f32);
                mem::write(main_addr(0x240), &f.to_bits());
            }
        }
    }

    fn unload(&mut self) {
        bump(0x224);
    }
}

plugin!(SelfTest, "RustSelf", 5);
