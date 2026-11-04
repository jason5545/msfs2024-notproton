// The FlyByWire A380X's systems simulation (flybywiresim/aircraft, fbw-a380x/src/wasm/systems/a380_systems,
// GPL-3.0) as a WebAssembly module of its own: the whole aircraft (electrical, hydraulic, pneumatic, fuel,
// air conditioning, flight controls' actuators, ...) built through the systems crate's SimulationTestBed and
// ticked like MSFS ticks a380_systems_wasm every frame, without MSFS. build-fbw.sh compiles it to
// wasm32-unknown-unknown, wasm2c turns it into C, and host-fbw.c times it.
use a380_systems::A380;
use std::time::Duration;
use systems::simulation::test::{ReadByName, SimulationTestBed, TestBed, WriteByName};
use uom::si::{f64::Length, f64::Velocity, length::foot, velocity::knot};

static mut BED: Option<SimulationTestBed<A380>> = None;
static mut TICK: u64 = 0;

#[no_mangle]
pub extern "C" fn bench_init() {
    let mut bed = SimulationTestBed::new(A380::new);
    // In flight like a380_systems' own hydraulic tests (in_flight): 2,500 ft, 180 kt, four engines at
    // 80 % N2, gear up, park brake off, no external power
    bed.set_on_ground(false);
    bed.set_pressure_altitude(Length::new::<foot>(2500.));
    bed.set_indicated_airspeed(Velocity::new::<knot>(180.));
    for e in 1..=4 {
        bed.write_by_name(&format!("GENERAL ENG STARTER ACTIVE:{e}"), true);
        bed.write_by_name(&format!("ENGINE_N2:{e}"), 80.);
        bed.write_by_name(&format!("ENGINE_N3:{e}"), 80.);
    }
    bed.write_by_name("GPS GROUND SPEED", 180.);
    bed.write_by_name("GEAR_LEVER_POSITION_REQUEST", false);
    bed.write_by_name("PARK_BRAKE_LEVER_POS", false);
    bed.write_by_name("EXT_PWR_AVAIL:1", false);
    bed.run_with_delta(Duration::from_millis(0));
    unsafe { BED = Some(bed) };
}

/// Runs `frames` simulation ticks of `dt_ms` each, the flight controls' commands moving every tick, and
/// returns the green hydraulic system's pressure.
#[no_mangle]
pub extern "C" fn bench_run(frames: u32, dt_ms: u32) -> f64 {
    #[allow(static_mut_refs)]
    let bed = unsafe { BED.as_mut().unwrap() };
    for _ in 0..frames {
        let t = unsafe {
            TICK += 1;
            TICK
        } as f64 * dt_ms as f64 / 1000.;
        let ail = 10. * (t * 0.7).sin();
        let elev = 5. * (t * 0.4).sin();
        bed.write_by_name("LEFT_AIL_BLUE_COMMANDED_POSITION", ail);
        bed.write_by_name("RIGHT_AIL_BLUE_COMMANDED_POSITION", -ail);
        bed.write_by_name("LEFT_ELEV_BLUE_COMMANDED_POSITION", elev);
        bed.write_by_name("RIGHT_ELEV_BLUE_COMMANDED_POSITION", elev);
        bed.run_with_delta(Duration::from_millis(dt_ms as u64));
    }
    let psi: f64 = bed.read_by_name("HYD_GREEN_SYSTEM_1_SECTION_PRESSURE");
    psi
}

// rand's getrandom has no source on wasm32-unknown-unknown; the benchmark needs repeatability, not entropy.
#[no_mangle]
unsafe extern "Rust" fn __getrandom_v03_custom(dest: *mut u8, len: usize) -> Result<(), getrandom::Error> {
    static mut STATE: u64 = 0x9E37_79B9_7F4A_7C15;
    for i in 0..len {
        STATE ^= STATE << 13;
        STATE ^= STATE >> 7;
        STATE ^= STATE << 17;
        *dest.add(i) = STATE as u8;
    }
    Ok(())
}
