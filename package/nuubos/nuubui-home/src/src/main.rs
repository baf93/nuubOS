slint::include_modules!();

use slint::{ComponentHandle, Timer};
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::thread;
use std::time::Duration;

const UI_READY: &str = "/run/nuubos/ui-ready";
const UI_CONTROL: &str = "/run/nuubos/ui-control";

const LINE_STAGE_MS: u64 = 220;
const CURTAIN_STAGE_MS: u64 = 460;
const STAGE_GAP_MS: u64 = 18;

fn mark_ready() {
    let _ = fs::create_dir_all("/run/nuubos");
    let _ = fs::write(UI_READY, b"ready\n");
}

fn clear_ready() {
    let _ = fs::remove_file(UI_READY);
}

fn start_shutdown_transition(ui: &HomeWindow, mode: String) {
    ui.set_transition_mode(mode.into());

    /*
     * Stage 1:
     * two full-width blue edges enter from top/bottom while the matching
     * lifecycle raster halves close over Home.
     */
    ui.set_curtain_progress(0.0);

    let weak = ui.as_weak();
    Timer::single_shot(
        Duration::from_millis(CURTAIN_STAGE_MS + STAGE_GAP_MS),
        move || {
            if let Some(ui) = weak.upgrade() {
                /*
                 * Stage 2:
                 * the two coincident full-width edges become one line and
                 * contract to the normal branded separator width.
                 */
                ui.set_line_progress(0.0);
            }
        },
    );
}

fn start_boot_transition(ui: &HomeWindow) {
    ui.set_transition_mode("boot".into());

    /*
     * Stage 1:
     * the existing branded separator line expands to full screen width.
     */
    ui.set_line_progress(1.0);

    let weak = ui.as_weak();
    Timer::single_shot(
        Duration::from_millis(LINE_STAGE_MS + STAGE_GAP_MS),
        move || {
            if let Some(ui) = weak.upgrade() {
                /*
                 * Stage 2:
                 * the full-width line splits into two edges; the splash halves
                 * travel to top/bottom and reveal Home between them.
                 */
                ui.set_curtain_progress(1.0);
            }
        },
    );
}

fn start_lifecycle_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();

    thread::spawn(move || loop {
        match File::open(UI_CONTROL) {
            Ok(file) => {
                let reader = BufReader::new(file);

                for line in reader.lines() {
                    let Ok(line) = line else {
                        break;
                    };

                    let mode = line.trim().to_owned();
                    if mode != "reboot" && mode != "poweroff" {
                        continue;
                    }

                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            start_shutdown_transition(&ui, mode);
                        }
                    });
                }
            }
            Err(_) => thread::sleep(Duration::from_millis(50)),
        }
    });
}

fn main() -> Result<(), slint::PlatformError> {
    let ui = HomeWindow::new()?;
    ui.window().set_fullscreen(true);

    start_lifecycle_listener(&ui);

    /*
     * Keep the framebuffer/Slint boot raster stable until Home is ready, then
     * run the two-stage blue-line transition.
     */
    let weak = ui.as_weak();
    Timer::single_shot(Duration::from_millis(220), move || {
        if let Some(ui) = weak.upgrade() {
            mark_ready();
            start_boot_transition(&ui);
        }
    });

    let result = ui.run();

    clear_ready();
    result
}
