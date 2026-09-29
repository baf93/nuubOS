slint::include_modules!();

use slint::{ComponentHandle, Timer};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::thread;
use std::time::Duration;

const UI_READY: &str = "/run/nuubos/ui-ready";
const UI_CONTROL: &str = "/run/nuubos/ui-control";
const STATUS_STATE: &str = "/run/nuubos/statusd.state";
const STATUS_SOCKET: &str = "/run/nuubos/statusd.sock";
const INPUT_SOCKET: &str = "/run/nuubos/inputd.sock";

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


fn input_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(INPUT_SOCKET)?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;

    let mut reader = BufReader::new(stream);
    let mut reply = String::new();
    reader.read_line(&mut reply)?;
    Ok(reply)
}

fn set_settings_capture(enabled: bool) {
    let command = if enabled {
        "SETTINGS OPEN"
    } else {
        "SETTINGS CLOSE"
    };

    if let Err(error) = input_command(command) {
        eprintln!(
            "home: settings capture enabled={} failed={}",
            enabled, error
        );
    }
}

fn binding_code(action: &str) -> Option<i32> {
    let reply = input_command(&format!("BIND GET {}", action)).ok()?;
    reply.trim().parse::<i32>().ok()
}

/*
 * Linux BTN_* semantic face positions, not Xbox/Nintendo letters:
 * 0 top, 1 right, 2 bottom, 3 left.
 */
fn face_position(code: i32) -> i32 {
    match code {
        307 => 0, // BTN_NORTH
        305 => 1, // BTN_EAST
        304 => 2, // BTN_SOUTH
        308 => 3, // BTN_WEST
        _ => -1,
    }
}

fn refresh_hint_mapping(ui: &HomeWindow) {
    ui.set_back_face_position(
        binding_code("menu_back")
            .map(face_position)
            .unwrap_or(-1),
    );
}

fn start_input_listener(
    ui: &HomeWindow,
    settings_active: Arc<AtomicBool>,
) {
    let weak = ui.as_weak();

    thread::spawn(move || loop {
        match UnixStream::connect(INPUT_SOCKET) {
            Ok(mut stream) => {
                if let Err(error) = stream.write_all(b"SUBSCRIBE HOME\n") {
                    eprintln!("home: input subscribe failed={}", error);
                    thread::sleep(Duration::from_millis(250));
                    continue;
                }

                if settings_active.load(Ordering::SeqCst) {
                    set_settings_capture(true);
                }

                let mut reader = BufReader::new(stream);
                let mut line = String::new();

                loop {
                    line.clear();

                    match reader.read_line(&mut line) {
                        Ok(0) => break,
                        Ok(_) => {
                            let fields: Vec<&str> =
                                line.split_whitespace().collect();

                            if fields.len() != 4 ||
                                fields[0] != "EVENT" ||
                                fields[1] != "1" ||
                                fields[3] != "pressed"
                            {
                                continue;
                            }

                            match fields[2] {
                                "settings" => {
                                    if settings_active.swap(
                                        true,
                                        Ordering::SeqCst,
                                    ) {
                                        continue;
                                    }

                                    set_settings_capture(true);

                                    let weak = weak.clone();
                                    let _ = slint::invoke_from_event_loop(
                                        move || {
                                            if let Some(ui) = weak.upgrade() {
                                                ui.set_settings_selected_index(0);
                                                ui.set_settings_open(true);
                                            }
                                        },
                                    );
                                }
                                "menu_back"
                                    if settings_active.load(
                                        Ordering::SeqCst,
                                    ) =>
                                {
                                    settings_active.store(
                                        false,
                                        Ordering::SeqCst,
                                    );
                                    set_settings_capture(false);

                                    let weak = weak.clone();
                                    let _ = slint::invoke_from_event_loop(
                                        move || {
                                            if let Some(ui) = weak.upgrade() {
                                                ui.set_settings_open(false);
                                            }
                                        },
                                    );
                                }
                                "menu_up"
                                    if settings_active.load(
                                        Ordering::SeqCst,
                                    ) =>
                                {
                                    let weak = weak.clone();
                                    let _ = slint::invoke_from_event_loop(
                                        move || {
                                            if let Some(ui) = weak.upgrade() {
                                                let current =
                                                    ui.get_settings_selected_index();
                                                ui.set_settings_selected_index(
                                                    if current <= 0 {
                                                        5
                                                    } else {
                                                        current - 1
                                                    },
                                                );
                                            }
                                        },
                                    );
                                }
                                "menu_down"
                                    if settings_active.load(
                                        Ordering::SeqCst,
                                    ) =>
                                {
                                    let weak = weak.clone();
                                    let _ = slint::invoke_from_event_loop(
                                        move || {
                                            if let Some(ui) = weak.upgrade() {
                                                let current =
                                                    ui.get_settings_selected_index();
                                                ui.set_settings_selected_index(
                                                    if current >= 5 {
                                                        0
                                                    } else {
                                                        current + 1
                                                    },
                                                );
                                            }
                                        },
                                    );
                                }
                                _ => {}
                            }
                        }
                        Err(error) => {
                            eprintln!("home: input read failed={}", error);
                            break;
                        }
                    }
                }
            }
            Err(error) => {
                eprintln!("home: input connect failed={}", error);
            }
        }

        thread::sleep(Duration::from_millis(250));
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
    refresh_hint_mapping(&ui);

    let settings_active = Arc::new(AtomicBool::new(false));
    start_input_listener(&ui, settings_active.clone());

    let weak = ui.as_weak();
    Timer::single_shot(Duration::from_millis(220), move || {
        if let Some(ui) = weak.upgrade() {
            mark_ready();
            start_boot_transition(&ui);
        }
    });

    let result = ui.run();

    if settings_active.load(Ordering::SeqCst) {
        set_settings_capture(false);
    }

    clear_ready();
    result
}
