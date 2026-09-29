slint::include_modules!();

use slint::{ComponentHandle, Timer};
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::os::unix::net::UnixStream;
use std::thread;
use std::time::Duration;

const UI_READY: &str = "/run/nuubos/ui-ready";
const UI_CONTROL: &str = "/run/nuubos/ui-control";
const STATUS_STATE: &str = "/run/nuubos/statusd.state";
const STATUS_SOCKET: &str = "/run/nuubos/statusd.sock";

const LINE_STAGE_MS: u64 = 220;
const CURTAIN_STAGE_MS: u64 = 460;
const STAGE_GAP_MS: u64 = 18;

#[derive(Clone)]
struct TopbarState {
    time: String,
    user_name: String,
    wifi_state: String,
    battery_percent: i32,
    battery_state: String,
    controller_count: i32,
    controller_battery: [i32; 4],
}

impl Default for TopbarState {
    fn default() -> Self {
        Self {
            time: "--:--".to_owned(),
            user_name: "Player".to_owned(),
            wifi_state: "disconnected".to_owned(),
            battery_percent: -1,
            battery_state: "unknown".to_owned(),
            controller_count: 0,
            controller_battery: [-1; 4],
        }
    }
}

fn load_topbar_state() -> TopbarState {
    let mut state = TopbarState::default();
    let Ok(contents) = fs::read_to_string(STATUS_STATE) else {
        return state;
    };

    for line in contents.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };

        match key {
            "TIME" => state.time = value.to_owned(),
            "USER_NAME" => state.user_name = value.to_owned(),
            "WIFI_STATE" => state.wifi_state = value.to_owned(),
            "BATTERY_PERCENT" => {
                state.battery_percent = value.parse::<i32>().unwrap_or(-1);
            }
            "BATTERY_STATE" => state.battery_state = value.to_owned(),
            "CONTROLLER_COUNT" => {
                state.controller_count = value.parse::<i32>().unwrap_or(0).clamp(0, 4);
            }
            "CONTROLLER_1_BATTERY" => {
                state.controller_battery[0] = value.parse::<i32>().unwrap_or(-1);
            }
            "CONTROLLER_2_BATTERY" => {
                state.controller_battery[1] = value.parse::<i32>().unwrap_or(-1);
            }
            "CONTROLLER_3_BATTERY" => {
                state.controller_battery[2] = value.parse::<i32>().unwrap_or(-1);
            }
            "CONTROLLER_4_BATTERY" => {
                state.controller_battery[3] = value.parse::<i32>().unwrap_or(-1);
            }
            _ => {}
        }
    }

    state
}

fn apply_topbar_state(ui: &HomeWindow, state: TopbarState) {
    ui.set_topbar_time(state.time.into());
    ui.set_current_user_name(state.user_name.into());
    ui.set_wifi_state(state.wifi_state.into());
    ui.set_battery_percent(state.battery_percent);
    ui.set_battery_state(state.battery_state.into());
    ui.set_battery_label(if state.battery_percent >= 0 { format!("{}%", state.battery_percent).into() } else { "--%".into() });
    ui.set_controller_count(state.controller_count);
    ui.set_controller_one_battery(state.controller_battery[0]);
    ui.set_controller_two_battery(state.controller_battery[1]);
    ui.set_controller_three_battery(state.controller_battery[2]);
    ui.set_controller_four_battery(state.controller_battery[3]);
}

fn start_status_listener(ui: &HomeWindow) {
    apply_topbar_state(ui, load_topbar_state());

    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(stream) = UnixStream::connect(STATUS_SOCKET) else {
            return;
        };
        let reader = BufReader::new(stream);

        for line in reader.lines() {
            let Ok(line) = line else {
                break;
            };
            if line.trim() != "changed" {
                continue;
            }

            let state = load_topbar_state();
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_topbar_state(&ui, state);
                }
            });
        }
    });
}

fn mark_ready() {
    let _ = fs::create_dir_all("/run/nuubos");
    let _ = fs::write(UI_READY, b"ready\n");
}

fn clear_ready() {
    let _ = fs::remove_file(UI_READY);
}

fn start_shutdown_transition(ui: &HomeWindow, mode: String) {
    ui.set_transition_mode(mode.into());
    ui.set_curtain_progress(0.0);

    let weak = ui.as_weak();
    Timer::single_shot(
        Duration::from_millis(CURTAIN_STAGE_MS + STAGE_GAP_MS),
        move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_line_progress(0.0);
            }
        },
    );
}

fn start_boot_transition(ui: &HomeWindow) {
    ui.set_transition_mode("boot".into());
    ui.set_line_progress(1.0);

    let weak = ui.as_weak();
    Timer::single_shot(
        Duration::from_millis(LINE_STAGE_MS + STAGE_GAP_MS),
        move || {
            if let Some(ui) = weak.upgrade() {
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

    start_status_listener(&ui);
    start_lifecycle_listener(&ui);

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
