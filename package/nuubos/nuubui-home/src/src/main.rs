slint::include_modules!();

use slint::{ComponentHandle, Model, ModelRc, SharedString, Timer, VecModel};
use std::fs::{self, File};
use std::rc::Rc;
use std::io::{BufRead, BufReader, Write};
use std::net::Ipv4Addr;
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
            controller_battery: [-1; 4],
        }
    }
}

#[derive(Clone)]
struct WifiProductState {
    enabled: bool,
    state: String,
    ssid: String,
    ipv4: String,
    signal_dbm: i32,
}

impl Default for WifiProductState {
    fn default() -> Self {
        Self {
            enabled: false,
            state: "unavailable".to_owned(),
            ssid: String::new(),
            ipv4: String::new(),
            signal_dbm: 0,
        }
    }
}

#[derive(Clone)]
struct BluetoothProductState {
    present: bool,
    powered: bool,
    address: String,
    connected_count: u32,
    paired_count: u32,
}

impl Default for BluetoothProductState {
    fn default() -> Self {
        Self {
            present: false,
            powered: false,
            address: String::new(),
            connected_count: 0,
            paired_count: 0,
        }
    }
}

fn apply_wifi_product_state(ui: &HomeWindow, state: WifiProductState) {
    let status = match state.state.as_str() {
        "connected" => "Connected",
        "connecting" => "Connecting",
        "off" => "Off",
        "disconnected" => "Disconnected",
        _ => "Unavailable",
    };

    let detail = if state.state == "connected" {
        let mut parts = Vec::new();
        if !state.ssid.is_empty() {
            parts.push(state.ssid.clone());
        }
        if !state.ipv4.is_empty() {
            parts.push(state.ipv4.clone());
        }
        if state.signal_dbm != 0 {
            parts.push(format!("{} dBm", state.signal_dbm));
        }
        if parts.is_empty() {
            "Connected".to_owned()
        } else {
            parts.join("  •  ")
        }
    } else if state.state == "connecting" {
        if state.ssid.is_empty() {
            "Association in progress".to_owned()
        } else {
            format!("Connecting to {}", state.ssid)
        }
    } else if state.state == "off" || !state.enabled {
        "Wireless radio is disabled".to_owned()
    } else if state.state == "disconnected" {
        "No network connected".to_owned()
    } else {
        "Wi-Fi product service unavailable".to_owned()
    };

    ui.set_connectivity_wifi_status(status.into());
    ui.set_connectivity_wifi_detail(detail.into());
    ui.set_connectivity_wifi_active(state.state == "connected");
    ui.set_wifi_enabled(state.enabled);
    ui.set_wifi_current_ssid(state.ssid.clone().into());

    let selected_ssid = ui.get_wifi_selected_ssid().to_string();
    ui.set_wifi_selected_current(
        state.state == "connected"
            && !selected_ssid.is_empty()
            && selected_ssid == state.ssid
    );
}

fn apply_bluetooth_product_state(ui: &HomeWindow, state: BluetoothProductState) {
    let status = if !state.present {
        "Unavailable"
    } else if state.powered {
        "On"
    } else {
        "Off"
    };

    let detail = if !state.present {
        "Bluetooth adapter not available".to_owned()
    } else if !state.powered {
        "Bluetooth radio is disabled".to_owned()
    } else if state.connected_count > 0 {
        format!(
            "{} connected  •  {} paired",
            state.connected_count, state.paired_count
        )
    } else if state.paired_count > 0 {
        format!("No device connected  •  {} paired", state.paired_count)
    } else if state.address.is_empty() {
        "Ready for devices".to_owned()
    } else {
        format!("Ready  •  {}", state.address)
    };

    ui.set_connectivity_bluetooth_status(status.into());
    ui.set_connectivity_bluetooth_detail(detail.into());
    ui.set_connectivity_bluetooth_active(state.present && state.powered);
    ui.set_bluetooth_enabled(state.present && state.powered);
}

fn wifi_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Wifi",
        "/org/nuubOS/Wifi",
        "org.nuubOS.Wifi1",
    )
}

fn bluetooth_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Bluetooth",
        "/org/nuubOS/Bluetooth",
        "org.nuubOS.Bluetooth1",
    )
}

fn controllers_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Controllers",
        "/org/nuubOS/Controllers",
        "org.nuubOS.Controllers1",
    )
}

fn rumble_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Rumble",
        "/org/nuubOS/Rumble",
        "org.nuubOS.Rumble1",
    )
}

fn lighting_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Lighting",
        "/org/nuubOS/Lighting",
        "org.nuubOS.Lighting1",
    )
}

fn string_model(values: &[&str]) -> ModelRc<SharedString> {
    let values: Vec<SharedString> = values.iter().map(|value| (*value).into()).collect();
    ModelRc::from(Rc::new(VecModel::from(values)))
}

fn apply_keyboard_layout(ui: &HomeWindow) {
    let page = ui.get_keyboard_page();
    let kind = ui.get_keyboard_input_kind().to_string();
    let layout = ui.get_keyboard_layout().to_string();

    let rows: [Vec<&str>; 4] = match kind.as_str() {
        "number" => [
            vec!["1", "2", "3"],
            vec!["4", "5", "6"],
            vec!["7", "8", "9"],
            vec!["Back", "0", "Done"],
        ],
        "ipv4" => [
            vec!["1", "2", "3"],
            vec!["4", "5", "6"],
            vec!["7", "8", "9"],
            vec![".", "0", "Back", "Done"],
        ],
        "dns" => [
            vec!["1", "2", "3"],
            vec!["4", "5", "6"],
            vec!["7", "8", "9"],
            vec![".", "0", ",", "Space", "Back", "Done"],
        ],
        _ => match page {
            1 => [
                vec!["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
                vec!["!", "?", "#", "$", "%", "&", "*", "(", ")", "Back"],
                vec!["+", "=", "[", "]", "{", "}", "<", ">", "\\", "|"],
                vec!["ABC", "SYM", ".", ",", "-", "_", ":", ";", "Space", "Done"],
            ],
            2 => [
                vec!["`", "~", "^", "@", "€", "£", "¥", "¢", "°", "§"],
                vec![":", ";", "'", "\"", "\\", "/", "?", "!", "#", "Back"],
                vec!["_", "-", "+", "=", "[", "]", "{", "}", "(", ")"],
                vec!["ABC", "123", ".", ",", "<", ">", "*", "&", "Space", "Done"],
            ],
            _ if layout == "azerty" => [
                vec!["a", "z", "e", "r", "t", "y", "u", "i", "o", "p"],
                vec!["q", "s", "d", "f", "g", "h", "j", "k", "l", "m"],
                vec!["w", "x", "c", "v", "b", "n", ".", "-", "_", "Back"],
                vec!["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"],
            ],
            _ if layout == "qwertz" => [
                vec!["q", "w", "e", "r", "t", "z", "u", "i", "o", "p"],
                vec!["a", "s", "d", "f", "g", "h", "j", "k", "l", "Back"],
                vec!["y", "x", "c", "v", "b", "n", "m", ".", "-", "_"],
                vec!["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"],
            ],
            _ => [
                vec!["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
                vec!["a", "s", "d", "f", "g", "h", "j", "k", "l", "Back"],
                vec!["z", "x", "c", "v", "b", "n", "m", ".", "-", "_"],
                vec!["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"],
            ],
        },
    };

    ui.set_keyboard_row_zero(string_model(&rows[0]));
    ui.set_keyboard_row_one(string_model(&rows[1]));
    ui.set_keyboard_row_two(string_model(&rows[2]));
    ui.set_keyboard_row_three(string_model(&rows[3]));
}

fn apply_wifi_network_rows(ui: &HomeWindow, rows: Vec<(String, String, i32, bool, bool)>, saved: bool) {
    let entries: Vec<WifiNetworkEntry> = rows
        .into_iter()
        .map(|(ssid, security, signal_dbm, is_saved, current)| WifiNetworkEntry {
            ssid: ssid.into(),
            security: security.into(),
            signal_dbm,
            saved: is_saved,
            current,
        })
        .collect();
    let model = ModelRc::from(Rc::new(VecModel::from(entries)));
    if saved {
        ui.set_wifi_saved_networks(model);
        let count = ui.get_wifi_saved_networks().row_count() as i32;
        if count == 0 {
            ui.set_wifi_saved_index(0);
            ui.set_wifi_saved_scroll_offset(0);
        } else if ui.get_wifi_saved_index() >= count {
            ui.set_wifi_saved_index(count - 1);
        }
    } else {
        ui.set_wifi_networks(model);
        let count = ui.get_wifi_networks().row_count() as i32;
        if count == 0 {
            ui.set_wifi_network_index(0);
            ui.set_wifi_network_scroll_offset(0);
        } else if ui.get_wifi_network_index() >= count {
            ui.set_wifi_network_index(count - 1);
        }

        let total = 5 + count;
        if ui.get_wifi_menu_index() >= total {
            ui.set_wifi_menu_index((total - 1).max(0));
        }
        update_wifi_menu_scroll(ui);
    }
}

fn refresh_wifi_networks(ui: &HomeWindow, saved: bool) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<Vec<(String, String, i32, bool, bool)>> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call(if saved { "GetSavedNetworks" } else { "GetNetworks" }, &())
        })();
        match result {
            Ok(rows) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_wifi_network_rows(&ui, rows, saved);
                    }
                });
            }
            Err(error) => {
                eprintln!("home: Wi-Fi network list failed={error}");
            }
        }
    });
}


fn apply_bluetooth_device_rows(
    ui: &HomeWindow,
    rows: Vec<(String, String, String, bool, bool, bool, i32)>,
) {
    let mut nearby = Vec::new();
    let mut known = Vec::new();
    let selected = ui.get_bluetooth_selected_address().to_string();
    let mut selected_found = false;

    for (address, name, kind, paired, connected, trusted, rssi) in rows {
        let entry = BluetoothDeviceEntry {
            address: address.clone().into(),
            name: name.clone().into(),
            kind: kind.clone().into(),
            paired,
            connected,
            trusted,
            rssi,
        };

        if paired || connected {
            known.push(entry);
        } else {
            nearby.push(entry);
        }

        if !selected.is_empty() && selected == address {
            selected_found = true;
            ui.set_bluetooth_selected_name(name.into());
            ui.set_bluetooth_selected_kind(kind.into());
            ui.set_bluetooth_selected_paired(paired);
            ui.set_bluetooth_selected_connected(connected);
            ui.set_bluetooth_selected_trusted(trusted);
        }
    }

    if !selected.is_empty() && !selected_found {
        ui.set_bluetooth_selected_paired(false);
        ui.set_bluetooth_selected_connected(false);
        ui.set_bluetooth_selected_trusted(false);
    }

    ui.set_bluetooth_devices(ModelRc::from(Rc::new(VecModel::from(nearby))));
    ui.set_bluetooth_known_devices(ModelRc::from(Rc::new(VecModel::from(known))));

    let nearby_count = ui.get_bluetooth_devices().row_count() as i32;
    if nearby_count == 0 {
        ui.set_bluetooth_device_index(0);
        ui.set_bluetooth_device_scroll_offset(0);
    } else if ui.get_bluetooth_device_index() >= nearby_count {
        ui.set_bluetooth_device_index(nearby_count - 1);
    }

    let known_count = ui.get_bluetooth_known_devices().row_count() as i32;
    if known_count == 0 {
        ui.set_bluetooth_known_index(0);
        ui.set_bluetooth_known_scroll_offset(0);
    } else if ui.get_bluetooth_known_index() >= known_count {
        ui.set_bluetooth_known_index(known_count - 1);
    }
}

fn refresh_bluetooth_devices(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<Vec<(String, String, String, bool, bool, bool, i32)>> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("GetDevices", &())
        })();

        match result {
            Ok(rows) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_bluetooth_device_rows(&ui, rows);
                    }
                });
            }
            Err(error) => eprintln!("home: Bluetooth device list failed={error}"),
        }
    });
}

fn bluetooth_notice(ui: &HomeWindow, text: impl Into<SharedString>) {
    ui.set_bluetooth_notice(text.into());
}

fn bluetooth_set_enabled(ui: &HomeWindow, enabled: bool) {
    let weak = ui.as_weak();
    bluetooth_notice(ui, if enabled { "Enabling Bluetooth…" } else { "Disabling Bluetooth…" });
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("SetEnabled", &(enabled,))
        })();

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        bluetooth_notice(&ui, "");
                        refresh_bluetooth_devices(&ui);
                    }
                    Err(error) => bluetooth_notice(&ui, format!("Bluetooth toggle failed: {error}")),
                }
            }
        });
    });
}

fn bluetooth_start_discovery_session(ui: &HomeWindow) {
    let weak = ui.as_weak();
    ui.set_bluetooth_scanning(true);
    bluetooth_notice(ui, "");
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("StartDiscoverySession", &())
        })();

        if let Err(error) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_bluetooth_scanning(false);
                    bluetooth_notice(&ui, format!("Discovery failed: {error}"));
                }
            });
        }
    });
}

fn bluetooth_stop_discovery_session(ui: &HomeWindow) {
    ui.set_bluetooth_scanning(false);
    thread::spawn(move || {
        let _ = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("StopDiscoverySession", &())
        })();
    });
}

fn bluetooth_device_operation(ui: &HomeWindow, method: &'static str, address: String) {
    let weak = ui.as_weak();
    let progress = match method {
        "PairAndConnect" => "Pairing and connecting…",
        "Connect" => "Connecting…",
        "Disconnect" => "Disconnecting…",
        "Forget" => "Forgetting device…",
        _ => "Working…",
    };
    bluetooth_notice(ui, progress);

    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call(method, &(address.as_str(),))
        })();

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        bluetooth_notice(&ui, "");
                        refresh_bluetooth_devices(&ui);
                        if method == "Forget" {
                            ui.set_bluetooth_selected_paired(false);
                            ui.set_bluetooth_selected_connected(false);
                            navigate_settings_view(&ui, 8);
                        }
                    }
                    Err(error) => bluetooth_notice(&ui, format!("Bluetooth operation failed: {error}")),
                }
            }
        });
    });
}

fn bluetooth_pair_with_pin(ui: &HomeWindow, address: String, pin: String) {
    let weak = ui.as_weak();
    bluetooth_notice(ui, "Pairing with PIN…");

    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("PairWithPin", &(address.as_str(), pin.as_str()))
        })();

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        bluetooth_notice(&ui, "");
                        refresh_bluetooth_devices(&ui);
                    }
                    Err(error) => bluetooth_notice(&ui, format!("Bluetooth PIN pairing failed: {error}")),
                }
            }
        });
    });
}


fn apply_controller_rows(
    ui: &HomeWindow,
    rows: Vec<(String, String, String, bool, bool, i32, i32)>,
) {
    /* Top-bar controller presence comes from the typed Controllers Product
     * Service, not statusd/bluetoothctl. This guarantees an icon for every
     * connected Bluetooth gamepad even when it exposes no battery supply. */
    let bluetooth_connected = rows
        .iter()
        .filter(|(_, _, transport, connected, builtin, _, _)| {
            *connected && !*builtin && transport.eq_ignore_ascii_case("Bluetooth")
        })
        .count()
        .min(4) as i32;
    ui.set_controller_count(bluetooth_connected);

    let entries: Vec<ControllerEntry> = rows
        .into_iter()
        .map(
            |(id, name, transport, connected, builtin, preferred_player, effective_player)| {
                ControllerEntry {
                    id: id.into(),
                    name: name.into(),
                    transport: transport.into(),
                    connected,
                    builtin,
                    preferred_player,
                    effective_player,
                }
            },
        )
        .collect();

    ui.set_controllers(ModelRc::from(Rc::new(VecModel::from(entries))));
    let count = ui.get_controllers().row_count() as i32;
    if count == 0 {
        ui.set_controller_list_index(0);
        ui.set_controller_list_scroll_offset(0);
    } else if ui.get_controller_list_index() >= count {
        ui.set_controller_list_index(count - 1);
    }
    update_controller_list_scroll(ui);
}

fn apply_player_assignment_rows(
    ui: &HomeWindow,
    rows: Vec<(i32, String, String, bool)>,
) {
    let entries: Vec<PlayerAssignmentEntry> = rows
        .into_iter()
        .map(|(player, controller_id, controller_name, available)| PlayerAssignmentEntry {
            player,
            controller_id: controller_id.into(),
            controller_name: controller_name.into(),
            available,
        })
        .collect();

    ui.set_player_assignments(ModelRc::from(Rc::new(VecModel::from(entries))));
    let count = ui.get_player_assignments().row_count() as i32;
    if count > 0 && ui.get_player_assignment_index() >= count {
        ui.set_player_assignment_index(count - 1);
    }
}

fn refresh_controllers(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(
            Vec<(String, String, String, bool, bool, i32, i32)>,
            Vec<(i32, String, String, bool)>,
        )> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            let devices = proxy.call("GetDevices", &())?;
            let assignments = proxy.call("GetAssignments", &())?;
            Ok((devices, assignments))
        })();

        match result {
            Ok((devices, assignments)) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_controller_rows(&ui, devices);
                        apply_player_assignment_rows(&ui, assignments);
                    }
                });
            }
            Err(error) => eprintln!("home: Controllers refresh failed={error}"),
        }
    });
}

fn input_code_label(event_type: i32, code: i32) -> String {
    if event_type == 3 {
        return match code {
            0 => "Left Stick X".to_owned(),
            1 => "Left Stick Y".to_owned(),
            2 => "Left Trigger Axis".to_owned(),
            3 => "Right Stick X".to_owned(),
            4 => "Right Stick Y".to_owned(),
            5 => "Right Trigger Axis".to_owned(),
            9 => "Right Trigger Axis".to_owned(),
            10 => "Left Trigger Axis".to_owned(),
            16 => "D-Pad X".to_owned(),
            17 => "D-Pad Y".to_owned(),
            _ => format!("Axis {code}"),
        };
    }

    match code {
        304 => "Face South".to_owned(),
        305 => "Face East".to_owned(),
        307 => "Face North".to_owned(),
        308 => "Face West".to_owned(),
        310 => "L1".to_owned(),
        311 => "R1".to_owned(),
        312 => "L2".to_owned(),
        313 => "R2".to_owned(),
        314 => "Select".to_owned(),
        315 => "Start".to_owned(),
        316 => "Hotkey".to_owned(),
        317 => "L3".to_owned(),
        318 => "R3".to_owned(),
        544 => "D-Pad Up".to_owned(),
        545 => "D-Pad Down".to_owned(),
        546 => "D-Pad Left".to_owned(),
        547 => "D-Pad Right".to_owned(),
        _ => format!("Button {code}"),
    }
}

fn mapping_binding_label(code: i32) -> String {
    format!("BTN{code}")
}

fn logical_action_label(action: &str) -> String {
    match action {
        "menu_back" => "Face South".to_owned(),
        "menu_confirm" => "Face East".to_owned(),
        "face_north" => "Face North".to_owned(),
        "face_west" => "Face West".to_owned(),
        "menu_up" => "D-Pad Up".to_owned(),
        "menu_down" => "D-Pad Down".to_owned(),
        "menu_left" => "D-Pad Left".to_owned(),
        "menu_right" => "D-Pad Right".to_owned(),
        "l1" => "L1".to_owned(),
        "r1" => "R1".to_owned(),
        "l2" => "L2".to_owned(),
        "r2" => "R2".to_owned(),
        "l3" => "L3".to_owned(),
        "r3" => "R3".to_owned(),
        "settings" => "Start".to_owned(),
        "select" => "Select".to_owned(),
        "quick_menu" => "Hotkey".to_owned(),
        _ => action.to_owned(),
    }
}

fn lighting_color_name(r: i32, g: i32, b: i32) -> String {
    match (r, g, b) {
        (58, 134, 255) => "Blue".to_owned(),
        (171, 71, 188) => "Purple".to_owned(),
        (76, 175, 80) => "Green".to_owned(),
        (255, 152, 0) => "Orange".to_owned(),
        (244, 67, 54) => "Red".to_owned(),
        (0, 188, 212) => "Cyan".to_owned(),
        (255, 255, 255) => "White".to_owned(),
        (255, 193, 7) => "Amber".to_owned(),
        _ => "Custom".to_owned(),
    }
}

fn axis_physical_label(code: i32) -> String {
    match code {
        0 => "ABS_X".to_owned(),
        1 => "ABS_Y".to_owned(),
        2 => "ABS_Z".to_owned(),
        3 => "ABS_RX".to_owned(),
        4 => "ABS_RY".to_owned(),
        5 => "ABS_RZ".to_owned(),
        9 => "ABS_GAS".to_owned(),
        10 => "ABS_BRAKE".to_owned(),
        16 => "ABS_HAT0X".to_owned(),
        17 => "ABS_HAT0Y".to_owned(),
        _ => format!("ABS{code}"),
    }
}

fn tester_display_label(event_type: i32, code: i32, value: i32, action: &str) -> String {
    if event_type == 1 {
        let mapped = if action.is_empty() {
            "Unmapped".to_owned()
        } else {
            logical_action_label(action)
        };
        return format!("{mapped}  •  BTN{code}");
    }

    if event_type == 3 {
        let logical = input_code_label(event_type, code);
        let physical = axis_physical_label(code);
        return format!("{logical}  •  {physical} {value:+}%");
    }

    format!("Input {event_type}:{code}  •  value {value}")
}

fn set_tester_visual_state(ui: &HomeWindow, event_type: i32, code: i32, value: i32) {
    let pressed = value != 0;

    if event_type == 1 {
        match code {
            304 => ui.set_tester_face_south(pressed),
            305 => ui.set_tester_face_east(pressed),
            307 => ui.set_tester_face_north(pressed),
            308 => ui.set_tester_face_west(pressed),
            310 => ui.set_tester_l1(pressed),
            311 => ui.set_tester_r1(pressed),
            312 => ui.set_tester_l2(pressed),
            313 => ui.set_tester_r2(pressed),
            314 => ui.set_tester_select(pressed),
            315 => ui.set_tester_start(pressed),
            316 => ui.set_tester_hotkey(pressed),
            317 => ui.set_tester_l3(pressed),
            318 => ui.set_tester_r3(pressed),
            544 => ui.set_tester_dpad_up(pressed),
            545 => ui.set_tester_dpad_down(pressed),
            546 => ui.set_tester_dpad_left(pressed),
            547 => ui.set_tester_dpad_right(pressed),
            _ => {}
        }
        return;
    }

    if event_type != 3 {
        return;
    }

    /* EV_ABS values arrive already normalized by nuubos-inputd from the
     * device's actual EVIOCGABS min/max/center. */
    let normalized = value.clamp(-100, 100);
    match code {
        0 => ui.set_tester_left_x(normalized),
        1 => ui.set_tester_left_y(normalized),
        2 | 10 => ui.set_tester_left_trigger(value.clamp(0, 100)),
        3 => ui.set_tester_right_x(normalized),
        4 => ui.set_tester_right_y(normalized),
        5 | 9 => ui.set_tester_right_trigger(value.clamp(0, 100)),
        16 => {
            ui.set_tester_dpad_left(value < 0);
            ui.set_tester_dpad_right(value > 0);
        }
        17 => {
            ui.set_tester_dpad_up(value < 0);
            ui.set_tester_dpad_down(value > 0);
        }
        _ => {}
    }
}

fn reset_tester_visual_state(ui: &HomeWindow) {
    ui.set_tester_face_south(false);
    ui.set_tester_face_east(false);
    ui.set_tester_face_north(false);
    ui.set_tester_face_west(false);
    ui.set_tester_dpad_up(false);
    ui.set_tester_dpad_down(false);
    ui.set_tester_dpad_left(false);
    ui.set_tester_dpad_right(false);
    ui.set_tester_l1(false);
    ui.set_tester_r1(false);
    ui.set_tester_l2(false);
    ui.set_tester_r2(false);
    ui.set_tester_l3(false);
    ui.set_tester_r3(false);
    ui.set_tester_start(false);
    ui.set_tester_select(false);
    ui.set_tester_hotkey(false);
    ui.set_tester_left_x(0);
    ui.set_tester_left_y(0);
    ui.set_tester_right_x(0);
    ui.set_tester_right_y(0);
    ui.set_tester_left_trigger(0);
    ui.set_tester_right_trigger(0);
}

fn apply_mapping_rows(ui: &HomeWindow, rows: Vec<(String, String, i32)>) {
    let entries: Vec<ControllerMappingEntry> = rows
        .into_iter()
        .map(|(action, label, code)| ControllerMappingEntry {
            action: action.into(),
            label: label.into(),
            code,
            binding: mapping_binding_label(code).into(),
        })
        .collect();
    ui.set_controller_mapping(ModelRc::from(Rc::new(VecModel::from(entries))));
    let count = ui.get_controller_mapping().row_count() as i32 + 2;
    if count <= 2 {
        ui.set_controller_mapping_index(0);
        ui.set_controller_mapping_scroll_offset(0);
    } else if ui.get_controller_mapping_index() >= count {
        ui.set_controller_mapping_index(count - 1);
    }
    update_controller_mapping_scroll(ui);
}

fn refresh_controller_mapping(ui: &HomeWindow) {
    let id = ui.get_controller_selected_id().to_string();
    if id.is_empty() {
        return;
    }

    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(Vec<(String, String, i32)>, (i32, i32))> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            let rows: Vec<(String, String, i32)> =
                proxy.call("GetMapping", &(id.as_str(),))?;
            let deadzones: (i32, i32) =
                proxy.call("GetDeadzones", &(id.as_str(),))?;
            Ok((rows, deadzones))
        })();
        match result {
            Ok((rows, (left, right))) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_controller_left_deadzone(left);
                        ui.set_controller_right_deadzone(right);
                        apply_mapping_rows(&ui, rows);
                    }
                });
            }
            Err(error) => eprintln!("home: controller mapping refresh failed={error}"),
        }
    });
}

fn set_controller_deadzone(ui: &HomeWindow, stick: &'static str, delta: i32) {
    let id = ui.get_controller_selected_id().to_string();
    if id.is_empty() {
        return;
    }
    let current = if stick == "left" {
        ui.get_controller_left_deadzone()
    } else {
        ui.get_controller_right_deadzone()
    };
    let next = (current + delta).clamp(0, 50);
    if stick == "left" {
        ui.set_controller_left_deadzone(next);
    } else {
        ui.set_controller_right_deadzone(next);
    }

    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            proxy.call("SetDeadzone", &(id.as_str(), stick, next))
        })();
        if let Err(error) = result {
            eprintln!("home: SetDeadzone failed={error}");
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_controller_mapping(&ui);
            }
        });
    });
}

fn update_controller_list_scroll(ui: &HomeWindow) {
    let count = ui.get_controllers().row_count() as i32;
    let visible = ui.get_settings_list_visible_rows().max(1);
    let index = ui.get_controller_list_index().max(0);
    let max_offset = (count - visible).max(0);
    let mut offset = ui.get_controller_list_scroll_offset().clamp(0, max_offset);

    if count <= visible {
        offset = 0;
    } else if index < offset {
        offset = index;
    } else if index >= offset + visible {
        offset = index - visible + 1;
    }
    ui.set_controller_list_scroll_offset(offset.clamp(0, max_offset));
}

fn update_controller_mapping_scroll(ui: &HomeWindow) {
    let count = ui.get_controller_mapping().row_count() as i32 + 2;
    let visible = ui.get_settings_list_visible_rows().max(1);
    let index = ui.get_controller_mapping_index().max(0);
    let max_offset = (count - visible).max(0);
    let mut offset = ui.get_controller_mapping_scroll_offset().clamp(0, max_offset);

    if count <= visible {
        offset = 0;
    } else if index < offset {
        offset = index;
    } else if index >= offset + visible {
        offset = index - visible + 1;
    }
    ui.set_controller_mapping_scroll_offset(offset.clamp(0, max_offset));
}

fn open_controller_detail(ui: &HomeWindow, controller: ControllerEntry) {
    ui.set_controller_selected_id(controller.id);
    ui.set_controller_selected_name(controller.name);
    ui.set_controller_selected_transport(controller.transport);
    ui.set_controller_selected_preferred_player(controller.preferred_player);
    ui.set_controller_selected_effective_player(controller.effective_player);
    ui.set_controller_detail_index(0);
    navigate_settings_view(ui, 13);
}

fn update_settings_choice_scroll(ui: &HomeWindow) {
    let count = ui.get_settings_choice_options().row_count() as i32;
    let visible = 5;
    let index = ui.get_settings_choice_index().max(0);
    let max_offset = (count - visible).max(0);
    let mut offset = ui.get_settings_choice_scroll().clamp(0, max_offset);

    if count <= visible {
        offset = 0;
    } else if index < offset {
        offset = index;
    } else if index >= offset + visible {
        offset = index - visible + 1;
    }
    ui.set_settings_choice_scroll(offset.clamp(0, max_offset));
}

fn open_settings_choice(
    ui: &HomeWindow,
    context: &str,
    title: &str,
    options: Vec<(String, String)>,
    current_value: &str,
) {
    let entries: Vec<SettingsChoiceEntry> = options
        .into_iter()
        .map(|(value, label)| SettingsChoiceEntry {
            value: value.into(),
            label: label.into(),
        })
        .collect();

    let selected = entries
        .iter()
        .position(|entry| entry.value.as_str() == current_value)
        .unwrap_or(0) as i32;

    ui.set_settings_choice_context(context.into());
    ui.set_settings_choice_title(title.into());
    ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(entries))));
    ui.set_settings_choice_index(selected);
    ui.set_settings_choice_scroll(0);
    ui.set_settings_choice_open(true);
    update_settings_choice_scroll(ui);
}

fn open_player_assignment_dropdown(ui: &HomeWindow) {
    let player_index = ui.get_player_assignment_index().max(0) as usize;
    let Some(assignment) = ui.get_player_assignments().row_data(player_index) else {
        return;
    };

    let mut options = vec![(String::new(), "Automatic".to_owned())];
    for i in 0..ui.get_controllers().row_count() {
        if let Some(controller) = ui.get_controllers().row_data(i) {
            let label = if controller.connected {
                controller.name.to_string()
            } else {
                format!("{} • Unavailable", controller.name)
            };
            options.push((controller.id.to_string(), label));
        }
    }

    open_settings_choice(
        ui,
        "player-assignment",
        &format!("Player {}", assignment.player),
        options,
        assignment.controller_id.as_str(),
    );
}

fn open_lighting_mode_dropdown(ui: &HomeWindow) {
    let values = [
        ("off", "Off"),
        ("static", "Static"),
        ("breathe", "Breathe"),
        ("pulse", "Pulse"),
        ("chase", "Chase"),
        ("wave", "Wave"),
        ("rainbow", "Rainbow"),
        ("sparkle", "Sparkle"),
        ("screen", "Screen Reactive"),
    ];
    open_settings_choice(
        ui,
        "lighting-mode",
        "RGB Mode",
        values
            .iter()
            .map(|(value, label)| (value.to_string(), label.to_string()))
            .collect(),
        ui.get_lighting_mode().as_str(),
    );
}

fn open_lighting_color_dropdown(ui: &HomeWindow) {
    let values = [
        ("58,134,255", "Blue"),
        ("171,71,188", "Purple"),
        ("76,175,80", "Green"),
        ("255,152,0", "Orange"),
        ("244,67,54", "Red"),
        ("0,188,212", "Cyan"),
        ("255,255,255", "White"),
        ("255,193,7", "Amber"),
    ];
    let current = format!(
        "{},{},{}",
        ui.get_lighting_red(),
        ui.get_lighting_green(),
        ui.get_lighting_blue()
    );
    open_settings_choice(
        ui,
        "lighting-color",
        "RGB Color",
        values
            .iter()
            .map(|(value, label)| (value.to_string(), label.to_string()))
            .collect(),
        &current,
    );
}

fn apply_settings_choice(ui: &HomeWindow) {
    let index = ui.get_settings_choice_index().max(0) as usize;
    let Some(entry) = ui.get_settings_choice_options().row_data(index) else {
        return;
    };
    let context = ui.get_settings_choice_context().to_string();
    let value = entry.value.to_string();
    ui.set_settings_choice_open(false);

    match context.as_str() {
        "player-assignment" => {
            let player_index = ui.get_player_assignment_index().max(0) as usize;
            let Some(assignment) = ui.get_player_assignments().row_data(player_index) else {
                return;
            };
            let player = assignment.player;
            let name = if value.is_empty() {
                String::new()
            } else {
                let mut found = String::new();
                for i in 0..ui.get_controllers().row_count() {
                    if let Some(controller) = ui.get_controllers().row_data(i) {
                        if controller.id.as_str() == value {
                            found = controller.name.to_string();
                            break;
                        }
                    }
                }
                found
            };
            let weak = ui.as_weak();
            thread::spawn(move || {
                let result = (|| -> zbus::Result<()> {
                    let connection = zbus::blocking::Connection::system()?;
                    let proxy = controllers_proxy(&connection)?;
                    proxy.call("SetPlayerPreference", &(player, value.as_str(), name.as_str()))
                })();
                if let Err(error) = result {
                    eprintln!("home: player assignment failed={error}");
                }
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        refresh_controllers(&ui);
                    }
                });
            });
        }
        "lighting-mode" => {
            lighting_call(ui, "SetMode", LightingArgs::Mode(value));
        }
        "lighting-color" => {
            let parts: Vec<i32> = value
                .split(',')
                .filter_map(|part| part.parse::<i32>().ok())
                .collect();
            if parts.len() == 3 {
                lighting_call(ui, "SetColor", LightingArgs::Color(parts[0], parts[1], parts[2]));
            }
        }
        _ => {}
    }
}

fn set_controller_tester(ui: &HomeWindow, enabled: bool) {
    let id = ui.get_controller_selected_id().to_string();
    if enabled && id.is_empty() {
        return;
    }

    if enabled {
        ui.set_tester_action("Press any control".into());
        reset_tester_visual_state(ui);
    }

    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = controllers_proxy(&connection) {
                let _: zbus::Result<()> =
                    proxy.call("SetTester", &(enabled, if enabled { id.as_str() } else { "" }));
            }
        }
    });
}

fn begin_controller_remap(ui: &HomeWindow) {
    let id = ui.get_controller_selected_id().to_string();
    let combined_index = ui.get_controller_mapping_index().max(0);
    if combined_index < 2 {
        return;
    }
    let index = (combined_index - 2) as usize;
    let Some(mapping) = ui.get_controller_mapping().row_data(index) else {
        return;
    };
    let action = mapping.action.to_string();

    ui.set_controller_remap_waiting(true);
    ui.set_controller_remap_action(mapping.label);

    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            proxy.call("BeginRemap", &(id.as_str(), action.as_str()))
        })();
        if let Err(error) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_controller_remap_waiting(false);
                    eprintln!("home: BeginRemap failed={error}");
                }
            });
        }
    });
}

fn cancel_controller_remap(ui: &HomeWindow) {
    if !ui.get_controller_remap_waiting() {
        return;
    }
    ui.set_controller_remap_waiting(false);
    ui.set_controller_remap_action("".into());
    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = controllers_proxy(&connection) {
                let _: zbus::Result<()> = proxy.call("CancelRemap", &());
            }
        }
    });
}

fn refresh_rumble(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(bool, bool)> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = rumble_proxy(&connection)?;
            proxy.call("GetState", &())
        })();
        if let Ok((supported, enabled)) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_rumble_supported(supported);
                    ui.set_rumble_enabled(enabled);
                }
            });
        }
    });
}

fn rumble_set_enabled(ui: &HomeWindow, enabled: bool) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = rumble_proxy(&connection)?;
            proxy.call("SetEnabled", &(enabled,))
        })();
        if result.is_ok() {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_rumble_enabled(enabled);
                }
            });
        } else if let Err(error) = result {
            eprintln!("home: rumble toggle failed={error}");
        }
    });
}

fn rumble_test() {
    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = rumble_proxy(&connection) {
                let result: zbus::Result<()> = proxy.call("Test", &());
                if let Err(error) = result {
                    eprintln!("home: rumble test failed={error}");
                }
            }
        }
    });
}

fn apply_lighting_state(
    ui: &HomeWindow,
    state: (bool, String, i32, i32, i32, i32, bool, String),
) {
    let (supported, mode, brightness, red, green, blue, system_effects, active_effect) = state;
    ui.set_lighting_supported(supported);
    ui.set_lighting_mode(mode.into());
    ui.set_lighting_brightness(brightness);
    ui.set_lighting_red(red);
    ui.set_lighting_green(green);
    ui.set_lighting_blue(blue);
    ui.set_lighting_color_name(lighting_color_name(red, green, blue).into());
    ui.set_lighting_system_effects(system_effects);
    ui.set_lighting_active_effect(active_effect.into());
}

fn refresh_lighting(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(bool, String, i32, i32, i32, i32, bool, String)> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = lighting_proxy(&connection)?;
            proxy.call("GetState", &())
        })();
        if let Ok(state) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_lighting_state(&ui, state);
                }
            });
        }
    });
}

fn lighting_call(ui: &HomeWindow, method: &'static str, args: LightingArgs) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = lighting_proxy(&connection)?;
            match args {
                LightingArgs::Mode(value) => proxy.call(method, &(value.as_str(),)),
                LightingArgs::Brightness(value) => proxy.call(method, &(value,)),
                LightingArgs::Color(r, g, b) => proxy.call(method, &(r, g, b)),
                LightingArgs::Toggle(value) => proxy.call(method, &(value,)),
                LightingArgs::Effect(effect, context) => {
                    proxy.call(method, &(effect.as_str(), context.as_str()))
                }
            }
        })();
        if let Err(error) = result {
            eprintln!("home: lighting {method} failed={error}");
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_lighting(&ui);
            }
        });
    });
}

enum LightingArgs {
    Mode(String),
    Brightness(i32),
    Color(i32, i32, i32),
    Toggle(bool),
    Effect(String, String),
}

fn lighting_stop_effect(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = lighting_proxy(&connection) {
                let _: zbus::Result<()> = proxy.call("StopEffect", &());
            }
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_lighting(&ui);
            }
        });
    });
}

fn preview_lighting_effect(ui: &HomeWindow) {
    let index = ui.get_lighting_preview_index().rem_euclid(5);
    let (effect, context) = match index {
        0 => ("low-battery", ""),
        1 => ("controller-connected", ""),
        2 => ("wifi-search", ""),
        3 => ("boot", ""),
        _ => ("system", "preview-system"),
    };
    lighting_call(
        ui,
        "PlayEffect",
        LightingArgs::Effect(effect.to_owned(), context.to_owned()),
    );
}

fn start_controller_signal_listener(ui: &HomeWindow, signal_name: &'static str) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Controllers bus failed={error}");
                return;
            }
        };
        let proxy = match controllers_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Controllers proxy failed={error}");
                return;
            }
        };
        let mut signals = match proxy.receive_signal(signal_name) {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Controllers signal {signal_name} failed={error}");
                return;
            }
        };

        for message in &mut signals {
            match signal_name {
                "DevicesChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(String, String, String, bool, bool, i32, i32)>,)>();
                    if let Ok((rows,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                apply_controller_rows(&ui, rows);
                            }
                        });
                    }
                }
                "AssignmentsChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(i32, String, String, bool)>,)>();
                    if let Ok((rows,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                apply_player_assignment_rows(&ui, rows);
                            }
                        });
                    }
                }
                "MappingChanged" => {
                    let body = message.body().deserialize::<(String,)>();
                    if let Ok((id,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id {
                                    refresh_controller_mapping(&ui);
                                }
                            }
                        });
                    }
                }
                "InputEvent" => {
                    let body = message
                        .body()
                        .deserialize::<(String, String, i32, i32, i32)>();
                    if let Ok((id, action, event_type, code, value)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id {
                                    set_tester_visual_state(&ui, event_type, code, value);
                                    if event_type != 1 || value != 0 {
                                        ui.set_tester_action(
                                            tester_display_label(
                                                event_type,
                                                code,
                                                value,
                                                action.as_str(),
                                            )
                                            .into(),
                                        );
                                    }
                                }
                            }
                        });
                    }
                }
                "TesterExitRequested" => {
                    let body = message.body().deserialize::<(String,)>();
                    if let Ok((id,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id
                                    && ui.get_settings_view() == 14
                                {
                                    navigate_settings_view(
                                        &ui,
                                        ui.get_controller_tester_return_view(),
                                    );
                                }
                            }
                        });
                    }
                }
                "RemapCaptured" => {
                    let body = message.body().deserialize::<(String, String, i32)>();
                    if let Ok((id, _action, _code)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id {
                                    ui.set_controller_remap_waiting(false);
                                    ui.set_controller_remap_action("".into());
                                    refresh_controller_mapping(&ui);
                                }
                            }
                        });
                    }
                }
                _ => {}
            }
        }
    });
}

fn start_controllers_product_listener(ui: &HomeWindow) {
    refresh_controllers(ui);
    start_controller_signal_listener(ui, "DevicesChanged");
    start_controller_signal_listener(ui, "AssignmentsChanged");
    start_controller_signal_listener(ui, "MappingChanged");
    start_controller_signal_listener(ui, "InputEvent");
    start_controller_signal_listener(ui, "RemapCaptured");
    start_controller_signal_listener(ui, "TesterExitRequested");
}

fn start_rumble_listener(ui: &HomeWindow) {
    refresh_rumble(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(connection) = zbus::blocking::Connection::system() else {
            return;
        };
        let Ok(proxy) = rumble_proxy(&connection) else {
            return;
        };
        let Ok(mut signals) = proxy.receive_signal("StateChanged") else {
            return;
        };

        for message in &mut signals {
            if let Ok((supported, enabled)) = message.body().deserialize::<(bool, bool)>() {
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_rumble_supported(supported);
                        ui.set_rumble_enabled(enabled);
                    }
                });
            }
        }
    });
}

fn start_lighting_listener(ui: &HomeWindow) {
    refresh_lighting(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(connection) = zbus::blocking::Connection::system() else {
            return;
        };
        let Ok(proxy) = lighting_proxy(&connection) else {
            return;
        };
        let Ok(mut signals) = proxy.receive_signal("StateChanged") else {
            return;
        };

        for message in &mut signals {
            if let Ok(state) = message
                .body()
                .deserialize::<(bool, String, i32, i32, i32, i32, bool, String)>()
            {
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_lighting_state(&ui, state);
                    }
                });
            }
        }
    });
}

fn update_bluetooth_scroll(ui: &HomeWindow, known: bool) {
    let index = if known { ui.get_bluetooth_known_index() } else { ui.get_bluetooth_device_index() };
    let visible_rows = ui.get_settings_list_visible_rows().max(1);
    let count = if known {
        ui.get_bluetooth_known_devices().row_count() as i32
    } else {
        ui.get_bluetooth_devices().row_count() as i32
    };
    let max_offset = (count - visible_rows).max(0);
    let mut offset = if known {
        ui.get_bluetooth_known_scroll_offset()
    } else {
        ui.get_bluetooth_device_scroll_offset()
    }
    .clamp(0, max_offset);

    if count <= visible_rows {
        offset = 0;
    } else if index < offset {
        offset = index;
    } else if index >= offset + visible_rows {
        offset = index - visible_rows + 1;
    }
    offset = offset.clamp(0, max_offset);

    if known {
        ui.set_bluetooth_known_scroll_offset(offset);
    } else {
        ui.set_bluetooth_device_scroll_offset(offset);
    }
}

fn open_bluetooth_detail(ui: &HomeWindow, device: BluetoothDeviceEntry, return_view: i32) {
    ui.set_bluetooth_selected_address(device.address);
    ui.set_bluetooth_selected_name(device.name);
    ui.set_bluetooth_selected_kind(device.kind);
    ui.set_bluetooth_selected_paired(device.paired);
    ui.set_bluetooth_selected_connected(device.connected);
    ui.set_bluetooth_selected_trusted(device.trusted);
    ui.set_bluetooth_detail_index(0);
    ui.set_bluetooth_detail_return_view(return_view);
    bluetooth_notice(ui, "");
    navigate_settings_view(ui, 9);
}

fn wifi_notice(ui: &HomeWindow, text: impl Into<SharedString>) {
    ui.set_wifi_notice(text.into());
}

fn wifi_set_enabled(ui: &HomeWindow, enabled: bool) {
    let weak = ui.as_weak();
    wifi_notice(ui, if enabled { "Enabling Wi-Fi…" } else { "Disabling Wi-Fi…" });
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("SetEnabled", &(enabled,))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => wifi_notice(&ui, ""),
                    Err(error) => wifi_notice(&ui, format!("Wi-Fi toggle failed: {error}")),
                }
            }
        });
    });
}

fn wifi_start_scan_session(ui: &HomeWindow) {
    let weak = ui.as_weak();
    ui.set_wifi_scanning(true);
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("StartScanSession", &())
        })();
        if let Err(error) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_wifi_scanning(false);
                    wifi_notice(&ui, format!("Scan failed: {error}"));
                }
            });
        }
    });
}

fn wifi_stop_scan_session(ui: &HomeWindow) {
    ui.set_wifi_scanning(false);
    thread::spawn(move || {
        let _ = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("StopScanSession", &())
        })();
    });
}

fn navigate_settings_view(ui: &HomeWindow, next_view: i32) {
    let previous_view = ui.get_settings_view();

    if previous_view == 2 && next_view != 2 {
        wifi_stop_scan_session(ui);
    }
    if previous_view == 7 && next_view != 7 {
        bluetooth_stop_discovery_session(ui);
    }
    if previous_view == 14 && next_view != 14 {
        set_controller_tester(ui, false);
    }
    if previous_view == 15 && next_view != 15 {
        cancel_controller_remap(ui);
    }
    if previous_view == 17 && next_view != 17 && ui.get_lighting_preview_open() {
        ui.set_lighting_preview_open(false);
        lighting_stop_effect(ui);
    }
    if ui.get_settings_choice_open() {
        ui.set_settings_choice_open(false);
    }

    ui.set_settings_view(next_view);

    if previous_view != 2 && next_view == 2 {
        ui.set_wifi_network_index(0);
        ui.set_wifi_network_scroll_offset(0);
        wifi_notice(ui, "");
        refresh_wifi_networks(ui, false);
        wifi_start_scan_session(ui);
    }

    if previous_view != 7 && next_view == 7 {
        ui.set_bluetooth_device_index(0);
        ui.set_bluetooth_device_scroll_offset(0);
        bluetooth_notice(ui, "");
        refresh_bluetooth_devices(ui);
        bluetooth_start_discovery_session(ui);
    }
    if next_view == 10 {
        refresh_controllers(ui);
        refresh_rumble(ui);
        refresh_lighting(ui);
    }
    if next_view == 11 || next_view == 12 {
        refresh_controllers(ui);
    }
    if previous_view != 14 && next_view == 14 {
        set_controller_tester(ui, true);
    }
    if next_view == 15 {
        refresh_controller_mapping(ui);
    }
    if next_view == 16 {
        refresh_rumble(ui);
    }
    if next_view == 17 {
        refresh_lighting(ui);
    }
}

fn wifi_connect(ui: &HomeWindow, ssid: String, password: String, hidden: bool) {
    let weak = ui.as_weak();
    wifi_notice(ui, format!("Connecting to {ssid}…"));
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("Connect", &(ssid.as_str(), password.as_str(), hidden))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        wifi_notice(&ui, "");
                        refresh_wifi_networks(&ui, false);
                        refresh_wifi_networks(&ui, true);
                    }
                    Err(error) => wifi_notice(&ui, format!("Connect failed: {error}")),
                }
            }
        });
    });
}

fn wifi_disconnect(ui: &HomeWindow) {
    let weak = ui.as_weak();
    wifi_notice(ui, "Disconnecting…");
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("Disconnect", &())
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        wifi_notice(&ui, "");
                        ui.set_wifi_selected_current(false);
                        refresh_wifi_networks(&ui, false);
                        refresh_wifi_networks(&ui, true);
                    }
                    Err(error) => wifi_notice(&ui, format!("Disconnect failed: {error}")),
                }
            }
        });
    });
}

fn wifi_forget(ui: &HomeWindow, ssid: String) {
    let weak = ui.as_weak();
    wifi_notice(ui, format!("Forgetting {ssid}…"));
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("Forget", &(ssid.as_str(),))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        wifi_notice(&ui, "");
                        navigate_settings_view(&ui, 4);
                        refresh_wifi_networks(&ui, true);
                        refresh_wifi_networks(&ui, false);
                    }
                    Err(error) => wifi_notice(&ui, format!("Forget failed: {error}")),
                }
            }
        });
    });
}

fn fetch_wifi_ip_configuration(ui: &HomeWindow, ssid: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(String, String, u32, String, String, String, String, u32, String, String, String)> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("GetIpConfiguration", &(ssid.as_str(),))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok((mode, address, prefix, netmask, gateway, dns,
                        runtime_address, runtime_prefix, runtime_netmask,
                        runtime_gateway, runtime_dns)) => {
                        ui.set_wifi_ip_mode(mode.into());
                        ui.set_wifi_ip_address(address.into());
                        ui.set_wifi_ip_prefix(prefix as i32);
                        ui.set_wifi_ip_netmask(netmask.into());
                        ui.set_wifi_ip_gateway(gateway.into());
                        ui.set_wifi_ip_dns(dns.into());
                        ui.set_wifi_runtime_address(runtime_address.into());
                        ui.set_wifi_runtime_prefix(runtime_prefix as i32);
                        ui.set_wifi_runtime_netmask(runtime_netmask.into());
                        ui.set_wifi_runtime_gateway(runtime_gateway.into());
                        ui.set_wifi_runtime_dns(runtime_dns.into());
                        ui.set_wifi_ip_index(0);
                        wifi_notice(&ui, "");
                    }
                    Err(error) => wifi_notice(&ui, format!("IP configuration failed: {error}")),
                }
            }
        });
    });
}

fn save_wifi_ip_configuration(ui: &HomeWindow) {
    let ssid = ui.get_wifi_selected_ssid().to_string();
    let mode = ui.get_wifi_ip_mode().to_string();
    let address = ui.get_wifi_ip_address().to_string();
    let prefix = ui.get_wifi_ip_prefix().clamp(0, 32) as u32;
    let gateway = ui.get_wifi_ip_gateway().to_string();
    let dns = ui.get_wifi_ip_dns().to_string();
    let weak = ui.as_weak();
    wifi_notice(ui, "Applying network settings…");
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call(
                "SetIpConfiguration",
                &(ssid.as_str(), mode.as_str(), address.as_str(), prefix,
                  gateway.as_str(), dns.as_str()),
            )
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => wifi_notice(&ui, ""),
                    Err(error) => wifi_notice(&ui, format!("IP configuration failed: {error}")),
                }
            }
        });
    });
}

fn prefix_to_netmask(prefix: i32) -> String {
    if !(0..=32).contains(&prefix) {
        return String::new();
    }
    let mask = if prefix == 0 { 0u32 } else { u32::MAX << (32 - prefix) };
    format!("{}.{}.{}.{}",
            (mask >> 24) & 0xff,
            (mask >> 16) & 0xff,
            (mask >> 8) & 0xff,
            mask & 0xff)
}

fn netmask_to_prefix(value: &str) -> Option<i32> {
    let address: Ipv4Addr = value.parse().ok()?;
    let mask = u32::from(address);
    let prefix = mask.leading_ones() as i32;
    let expected = if prefix == 0 { 0u32 } else { u32::MAX << (32 - prefix) };
    if mask == expected { Some(prefix) } else { None }
}

fn open_system_keyboard(
    ui: &HomeWindow,
    title: &str,
    purpose: i32,
    return_view: i32,
    input_kind: &str,
    initial: &str,
) {
    ui.set_keyboard_title(title.into());
    ui.set_keyboard_purpose(purpose);
    ui.set_keyboard_return_view(return_view);
    ui.set_keyboard_input_kind(input_kind.into());
    ui.set_keyboard_secret(input_kind == "password");
    ui.set_keyboard_value(initial.into());
    ui.set_keyboard_page(0);
    ui.set_keyboard_index(0);
    ui.set_keyboard_shift(false);
    apply_keyboard_layout(ui);
    navigate_settings_view(ui, 6);
    ui.invoke_focus_system_keyboard();
    ui.invoke_system_keyboard_place_cursor(initial.len() as i32);
}

fn keyboard_row(ui: &HomeWindow, row: i32) -> ModelRc<SharedString> {
    match row.rem_euclid(4) {
        0 => ui.get_keyboard_row_zero(),
        1 => ui.get_keyboard_row_one(),
        2 => ui.get_keyboard_row_two(),
        _ => ui.get_keyboard_row_three(),
    }
}

fn keyboard_key(ui: &HomeWindow) -> Option<String> {
    let index = ui.get_keyboard_index().clamp(0, 39);
    let row = index / 10;
    let col = index % 10;
    keyboard_row(ui, row)
        .row_data(col as usize)
        .map(|value| value.to_string())
}

fn move_keyboard_horizontal(ui: &HomeWindow, delta: i32) {
    let index = ui.get_keyboard_index().clamp(0, 39);
    let row = index / 10;
    let model = keyboard_row(ui, row);
    let count = model.row_count() as i32;
    if count <= 0 {
        return;
    }
    let col = (index % 10).min(count - 1);
    ui.set_keyboard_index(row * 10 + (col + delta).rem_euclid(count));
}

fn move_keyboard_vertical(ui: &HomeWindow, delta: i32) {
    let index = ui.get_keyboard_index().clamp(0, 39);
    let current_row = index / 10;
    let col = index % 10;

    for step in 1..=4 {
        let row = (current_row + delta * step).rem_euclid(4);
        let count = keyboard_row(ui, row).row_count() as i32;
        if count > 0 {
            ui.set_keyboard_index(row * 10 + col.min(count - 1));
            return;
        }
    }
}

fn valid_ipv4(value: &str, allow_empty: bool) -> bool {
    (allow_empty && value.is_empty()) || value.parse::<Ipv4Addr>().is_ok()
}

fn valid_dns(value: &str) -> bool {
    if value.trim().is_empty() {
        return true;
    }
    value
        .split(|c: char| c == ',' || c.is_ascii_whitespace())
        .filter(|part| !part.is_empty())
        .all(|part| part.parse::<Ipv4Addr>().is_ok())
}

fn finish_system_keyboard(ui: &HomeWindow) {
    let purpose = ui.get_keyboard_purpose();
    let value = ui.get_keyboard_value().to_string();
    let return_view = ui.get_keyboard_return_view();

    match purpose {
        0 => {
            let ssid = ui.get_wifi_selected_ssid().to_string();
            let hidden = ui.get_wifi_pending_hidden();
            if ssid.is_empty() {
                wifi_notice(ui, "SSID is required");
                return;
            }
            if !value.is_empty() && value.len() < 8 {
                wifi_notice(ui, "Password must contain at least 8 characters");
                return;
            }
            navigate_settings_view(ui, return_view);
            ui.set_keyboard_value("".into());
            ui.set_wifi_pending_hidden(false);
            wifi_connect(ui, ssid, value, hidden);
        }
        1 => {
            if value.is_empty() {
                wifi_notice(ui, "SSID is required");
                return;
            }
            ui.set_wifi_selected_ssid(value.clone().into());
            ui.set_wifi_selected_security("Hidden".into());
            ui.set_wifi_selected_saved(false);
            ui.set_wifi_selected_current(false);
            ui.set_wifi_pending_hidden(true);
            open_system_keyboard(ui, &format!("Password • {value}"), 0, 2, "password", "");
        }
        2 => {
            if !valid_ipv4(&value, false) {
                wifi_notice(ui, "Enter a valid IPv4 address");
                return;
            }
            ui.set_wifi_ip_address(value.into());
            navigate_settings_view(&ui, 4);
        }
        3 => {
            let Ok(prefix) = value.parse::<i32>() else {
                wifi_notice(ui, "Prefix must be between 0 and 32");
                return;
            };
            if !(0..=32).contains(&prefix) {
                wifi_notice(ui, "Prefix must be between 0 and 32");
                return;
            }
            ui.set_wifi_ip_prefix(prefix);
            ui.set_wifi_ip_netmask(prefix_to_netmask(prefix).into());
            navigate_settings_view(&ui, 4);
        }
        4 => {
            if !valid_ipv4(&value, true) {
                wifi_notice(ui, "Gateway must be empty or a valid IPv4 address");
                return;
            }
            ui.set_wifi_ip_gateway(value.into());
            navigate_settings_view(&ui, 4);
        }
        5 => {
            if !valid_dns(&value) {
                wifi_notice(ui, "DNS must contain IPv4 addresses separated by comma or space");
                return;
            }
            ui.set_wifi_ip_dns(value.into());
            navigate_settings_view(&ui, 4);
        }
        6 => {
            let Some(prefix) = netmask_to_prefix(&value) else {
                wifi_notice(ui, "Enter a contiguous IPv4 subnet mask");
                return;
            };
            ui.set_wifi_ip_prefix(prefix);
            ui.set_wifi_ip_netmask(value.into());
            navigate_settings_view(&ui, 4);
        }
        7 => {
            if value.is_empty() || value.len() > 16 {
                bluetooth_notice(ui, "PIN/passkey must contain 1 to 16 characters");
                return;
            }
            let address = ui.get_bluetooth_selected_address().to_string();
            navigate_settings_view(ui, return_view);
            ui.set_keyboard_value("".into());
            bluetooth_pair_with_pin(ui, address, value);
        }
        _ => navigate_settings_view(ui, return_view),
    }
}

fn handle_system_keyboard_confirm(ui: &HomeWindow) {
    let Some(key) = keyboard_key(ui) else { return; };
    match key.as_str() {
        "Back" => {
            let mut value = ui.get_keyboard_value().to_string();
            value.pop();
            let cursor = value.len() as i32;
            ui.set_keyboard_value(value.into());
            ui.invoke_system_keyboard_place_cursor(cursor);
        }
        "Space" => {
            let mut value = ui.get_keyboard_value().to_string();
            value.push(' ');
            let cursor = value.len() as i32;
            ui.set_keyboard_value(value.into());
            ui.invoke_system_keyboard_place_cursor(cursor);
        }
        "Shift" => ui.set_keyboard_shift(!ui.get_keyboard_shift()),
        "123" => {
            ui.set_keyboard_page(1);
            ui.set_keyboard_shift(false);
            ui.set_keyboard_index(0);
            apply_keyboard_layout(ui);
        }
        "SYM" => {
            ui.set_keyboard_page(2);
            ui.set_keyboard_shift(false);
            ui.set_keyboard_index(0);
            apply_keyboard_layout(ui);
        }
        "ABC" => {
            ui.set_keyboard_page(0);
            ui.set_keyboard_shift(false);
            ui.set_keyboard_index(0);
            apply_keyboard_layout(ui);
        }
        "Done" => finish_system_keyboard(ui),
        _ => {
            let kind = ui.get_keyboard_input_kind().to_string();
            let allowed = match kind.as_str() {
                "ipv4" => key.chars().all(|c| c.is_ascii_digit() || c == '.'),
                "number" => key.chars().all(|c| c.is_ascii_digit()),
                "dns" => key.chars().all(|c| c.is_ascii_digit() || c == '.' || c == ','),
                _ => true,
            };
            if !allowed {
                return;
            }
            let mut value = ui.get_keyboard_value().to_string();
            if ui.get_keyboard_shift() && key.chars().all(|c| c.is_alphabetic()) {
                value.push_str(&key.to_uppercase());
                ui.set_keyboard_shift(false);
            } else {
                value.push_str(&key);
            }
            let cursor = value.len() as i32;
            ui.set_keyboard_value(value.into());
            ui.invoke_system_keyboard_place_cursor(cursor);
        }
    }
}

fn open_network_detail(ui: &HomeWindow, network: WifiNetworkEntry, return_view: i32) {
    let ssid = network.ssid.to_string();
    ui.set_wifi_selected_ssid(network.ssid.clone());
    ui.set_wifi_selected_security(network.security.clone());
    ui.set_wifi_selected_saved(network.saved);
    ui.set_wifi_selected_current(network.current);
    ui.set_wifi_detail_index(0);
    ui.set_wifi_detail_scroll_offset(0);
    ui.set_wifi_detail_return_view(return_view);
    navigate_settings_view(&ui, 4);
    wifi_notice(ui, "");
    fetch_wifi_ip_configuration(ui, ssid);
}

fn move_model_selection(current: i32, count: i32, delta: i32) -> i32 {
    if count <= 0 {
        return 0;
    }
    (current + delta).rem_euclid(count)
}

fn update_wifi_menu_scroll(ui: &HomeWindow) {
    let visible_rows = ui.get_settings_list_visible_rows().max(1);
    let index = ui.get_wifi_menu_index().max(0);
    let mut offset = ui.get_wifi_menu_scroll_offset().max(0);
    if index < offset {
        offset = index;
    } else if index >= offset + visible_rows {
        offset = index - visible_rows + 1;
    }
    ui.set_wifi_menu_scroll_offset(offset.max(0));
}

fn update_scroll_offset(ui: &HomeWindow, saved: bool) {
    let index = if saved { ui.get_wifi_saved_index() } else { ui.get_wifi_network_index() };
    let visible_rows = ui.get_settings_list_visible_rows().max(1);
    let count = if saved {
        ui.get_wifi_saved_networks().row_count() as i32
    } else {
        // Connect view includes Add Hidden Network at selection index 0.
        ui.get_wifi_networks().row_count() as i32 + 1
    };
    let max_offset = (count - visible_rows).max(0);
    let mut offset = if saved {
        ui.get_wifi_saved_scroll_offset()
    } else {
        ui.get_wifi_network_scroll_offset()
    }
    .clamp(0, max_offset);

    if count <= visible_rows {
        offset = 0;
    } else if index < offset {
        offset = index;
    } else if index >= offset + visible_rows {
        offset = index - visible_rows + 1;
    }
    offset = offset.clamp(0, max_offset);

    if saved {
        ui.set_wifi_saved_scroll_offset(offset);
    } else {
        ui.set_wifi_network_scroll_offset(offset);
    }
}

fn update_wifi_detail_scroll(ui: &HomeWindow) {
    const DETAIL_ROWS: i32 = 9;
    let visible_rows = ui.get_wifi_detail_visible_rows().max(1);
    let index = ui.get_wifi_detail_index().max(0);
    let max_offset = (DETAIL_ROWS - visible_rows).max(0);
    let mut offset = ui.get_wifi_detail_scroll_offset().clamp(0, max_offset);

    if DETAIL_ROWS <= visible_rows {
        offset = 0;
    } else if index < offset {
        offset = index;
    } else if index >= offset + visible_rows {
        offset = index - visible_rows + 1;
    }

    ui.set_wifi_detail_scroll_offset(offset.clamp(0, max_offset));
}

fn start_wifi_aux_signal_listener(ui: &HomeWindow, signal_name: &'static str) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Wi-Fi auxiliary bus failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let proxy = match wifi_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Wi-Fi auxiliary proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let mut signals = match proxy.receive_signal(signal_name) {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Wi-Fi {signal_name} subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        for message in &mut signals {
            match signal_name {
                "NetworksSnapshotChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(String, String, i32, bool, bool)>,)>();
                    match body {
                        Ok((rows,)) => {
                            let weak = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(ui) = weak.upgrade() {
                                    apply_wifi_network_rows(&ui, rows, false);
                                }
                            });
                        }
                        Err(error) => {
                            eprintln!("home: invalid Wi-Fi NetworksSnapshotChanged payload={error}");
                        }
                    }
                }
                "NetworksChanged" => {
                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            refresh_wifi_networks(&ui, true);
                        }
                    });
                }
                "ScanStateChanged" => {
                    let body = message.body().deserialize::<(bool,)>();
                    if let Ok((scanning,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                ui.set_wifi_scanning(scanning);
                                if !scanning {
                                    wifi_notice(&ui, "");
                                    refresh_wifi_networks(&ui, false);
                                }
                            }
                        });
                    }
                }
                "OperationFailed" => {
                    let body = message.body().deserialize::<(String, String)>();
                    if let Ok((operation, text)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                wifi_notice(&ui, format!("{operation}: {text}"));
                            }
                        });
                    }
                }
                "IpConfigurationChanged" => {
                    let body = message.body().deserialize::<(String,)>();
                    if let Ok((ssid,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_settings_view() == 4 && ui.get_wifi_selected_ssid().as_str() == ssid {
                                    fetch_wifi_ip_configuration(&ui, ssid);
                                }
                            }
                        });
                    }
                }
                _ => {}
            }
        }
        thread::sleep(Duration::from_millis(250));
    });
}

fn start_wifi_product_listener(ui: &HomeWindow) {
    start_wifi_aux_signal_listener(ui, "NetworksSnapshotChanged");
    start_wifi_aux_signal_listener(ui, "NetworksChanged");
    start_wifi_aux_signal_listener(ui, "ScanStateChanged");
    start_wifi_aux_signal_listener(ui, "OperationFailed");
    start_wifi_aux_signal_listener(ui, "IpConfigurationChanged");

    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Wi-Fi system bus connection failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let proxy = match wifi_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Wi-Fi product proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let initial_snapshot: zbus::Result<(bool, String, String, String, i32)> =
            proxy.call("GetSnapshot", &());
        if let Ok((enabled, state, ssid, ipv4, signal_dbm)) = initial_snapshot {
            let snapshot = WifiProductState { enabled, state, ssid, ipv4, signal_dbm };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_wifi_product_state(&ui, snapshot);
                }
            });
        }

        let mut signals = match proxy.receive_signal("StateChanged") {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Wi-Fi signal subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            let body = message.body();
            let snapshot = body.deserialize::<(bool, String, String, String, i32)>();
            let Ok((enabled, state, ssid, ipv4, signal_dbm)) = snapshot else {
                eprintln!("home: invalid Wi-Fi StateChanged payload");
                continue;
            };
            let snapshot = WifiProductState { enabled, state, ssid, ipv4, signal_dbm };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_wifi_product_state(&ui, snapshot);
                    refresh_wifi_networks(&ui, false);
                    refresh_wifi_networks(&ui, true);
                }
            });
        }
        thread::sleep(Duration::from_millis(250));
    });
}

fn start_bluetooth_aux_signal_listener(ui: &HomeWindow, signal_name: &'static str) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Bluetooth auxiliary bus failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let proxy = match bluetooth_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Bluetooth auxiliary proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let mut signals = match proxy.receive_signal(signal_name) {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Bluetooth {signal_name} subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            match signal_name {
                "DevicesSnapshotChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(String, String, String, bool, bool, bool, i32)>,)>();
                    match body {
                        Ok((rows,)) => {
                            let weak = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(ui) = weak.upgrade() {
                                    apply_bluetooth_device_rows(&ui, rows);
                                }
                            });
                        }
                        Err(error) => eprintln!("home: invalid Bluetooth device snapshot={error}"),
                    }
                }
                "DiscoveryStateChanged" => {
                    let body = message.body().deserialize::<(bool,)>();
                    if let Ok((discovering,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                ui.set_bluetooth_scanning(discovering);
                            }
                        });
                    }
                }
                _ => {}
            }
        }

        thread::sleep(Duration::from_millis(250));
    });
}

fn start_bluetooth_product_listener(ui: &HomeWindow) {
    start_bluetooth_aux_signal_listener(ui, "DevicesSnapshotChanged");
    start_bluetooth_aux_signal_listener(ui, "DiscoveryStateChanged");

    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Bluetooth system bus connection failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let proxy = match bluetooth_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Bluetooth product proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let initial_snapshot: zbus::Result<(bool, bool, String, u32, u32)> =
            proxy.call("GetSnapshot", &());

        match initial_snapshot {
            Ok((present, powered, address, connected_count, paired_count)) => {
                let snapshot = BluetoothProductState {
                    present,
                    powered,
                    address,
                    connected_count,
                    paired_count,
                };
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_bluetooth_product_state(&ui, snapshot);
                        refresh_bluetooth_devices(&ui);
                    }
                });
            }
            Err(error) => eprintln!("home: Bluetooth snapshot failed={error}"),
        }

        let mut signals = match proxy.receive_signal("StateChanged") {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Bluetooth signal subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            let body = message.body();
            let snapshot = body.deserialize::<(bool, bool, String, u32, u32)>();
            let Ok((present, powered, address, connected_count, paired_count)) = snapshot else {
                eprintln!("home: invalid Bluetooth StateChanged payload");
                continue;
            };
            let snapshot = BluetoothProductState {
                present,
                powered,
                address,
                connected_count,
                paired_count,
            };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_bluetooth_product_state(&ui, snapshot);
                }
            });
        }

        thread::sleep(Duration::from_millis(250));
    });
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
    ui.set_controller_one_battery(state.controller_battery[0]);
    ui.set_controller_two_battery(state.controller_battery[1]);
    ui.set_controller_three_battery(state.controller_battery[2]);
    ui.set_controller_four_battery(state.controller_battery[3]);
}

fn apply_latest_topbar_state(weak: &slint::Weak<HomeWindow>) -> bool {
    let state = load_topbar_state();
    let weak = weak.clone();
    slint::invoke_from_event_loop(move || {
        if let Some(ui) = weak.upgrade() {
            apply_topbar_state(&ui, state);
        }
    })
    .is_ok()
}

fn start_status_listener(ui: &HomeWindow) {
    apply_topbar_state(ui, load_topbar_state());

    let weak = ui.as_weak();
    thread::spawn(move || loop {
        match UnixStream::connect(STATUS_SOCKET) {
            Ok(stream) => {
                eprintln!("home: status service connected");
                let mut reader = BufReader::new(stream);
                let mut line = String::new();

                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) => {
                            eprintln!("home: status stream EOF");
                            break;
                        }
                        Ok(_) if line.trim() == "changed" => {
                            /* Same event contract as Quick Menu: statusd emits
                             * changed only after atomically publishing the new
                             * state file, then Home reloads that exact snapshot. */
                            if !apply_latest_topbar_state(&weak) {
                                return;
                            }
                        }
                        Ok(_) => {}
                        Err(error) => {
                            eprintln!("home: status read failed={error}");
                            break;
                        }
                    }
                }
            }
            Err(error) => eprintln!("home: status service unavailable={error}"),
        }

        if !apply_latest_topbar_state(&weak) {
            return;
        }
        thread::sleep(Duration::from_millis(250));
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
    ui.set_select_face_position(
        binding_code("menu_confirm")
            .map(face_position)
            .unwrap_or(-1),
    );
    ui.set_back_face_position(
        binding_code("menu_back")
            .map(face_position)
            .unwrap_or(-1),
    );
}

fn handle_settings_action(
    ui: &HomeWindow,
    action: &str,
    settings_active: &Arc<AtomicBool>,
) {
    let view = ui.get_settings_view();

    if ui.get_settings_choice_open() {
        if action == "menu_back" {
            ui.set_settings_choice_open(false);
            return;
        }

        let count = ui.get_settings_choice_options().row_count() as i32;
        match action {
            "menu_up" if count > 0 => {
                ui.set_settings_choice_index(
                    move_model_selection(ui.get_settings_choice_index(), count, -1)
                );
                update_settings_choice_scroll(ui);
            }
            "menu_down" if count > 0 => {
                ui.set_settings_choice_index(
                    move_model_selection(ui.get_settings_choice_index(), count, 1)
                );
                update_settings_choice_scroll(ui);
            }
            "menu_confirm" if count > 0 => apply_settings_choice(ui),
            _ => {}
        }
        return;
    }
    if action == "menu_back" && view == 17 && ui.get_lighting_preview_open() {
        ui.set_lighting_preview_open(false);
        lighting_stop_effect(ui);
        return;
    }

    if action == "menu_back" {
        match view {
            0 => {
                settings_active.store(false, Ordering::SeqCst);
                set_settings_capture(false);
                ui.set_settings_open(false);
            }
            1 => navigate_settings_view(ui, 0),
            2 | 3 => navigate_settings_view(ui, 1),
            4 => navigate_settings_view(ui, ui.get_wifi_detail_return_view()),
            6 => navigate_settings_view(ui, ui.get_keyboard_return_view()),
            7 | 8 => navigate_settings_view(ui, 1),
            9 => navigate_settings_view(ui, ui.get_bluetooth_detail_return_view()),
            10 => navigate_settings_view(ui, 0),
            11 | 12 | 16 | 17 => navigate_settings_view(ui, 10),
            13 => navigate_settings_view(ui, 12),
            15 => navigate_settings_view(ui, 13),
            14 => navigate_settings_view(ui, ui.get_controller_tester_return_view()),
            _ => navigate_settings_view(ui, 0),
        }
        return;
    }

    match view {
        0 => match action {
            "menu_up" => {
                let current = ui.get_settings_selected_index();
                ui.set_settings_selected_index(if current <= 0 { 5 } else { current - 1 });
            }
            "menu_down" => {
                let current = ui.get_settings_selected_index();
                ui.set_settings_selected_index(if current >= 5 { 0 } else { current + 1 });
            }
            "menu_confirm" => {
                if ui.get_settings_selected_index() == 1 {
                    ui.set_connectivity_selected_index(0);
                    navigate_settings_view(ui, 1);
                    wifi_notice(ui, "");
                    refresh_wifi_networks(ui, true);
                } else if ui.get_settings_selected_index() == 2 {
                    ui.set_controllers_menu_index(0);
                    navigate_settings_view(ui, 10);
                }
            }
            _ => {}
        },
        1 => match action {
            "menu_up" => {
                let current = ui.get_connectivity_selected_index();
                ui.set_connectivity_selected_index((current + 5).rem_euclid(6));
            }
            "menu_down" => {
                let current = ui.get_connectivity_selected_index();
                ui.set_connectivity_selected_index((current + 1).rem_euclid(6));
            }
            "menu_confirm" => match ui.get_connectivity_selected_index() {
                0 => wifi_set_enabled(ui, !ui.get_wifi_enabled()),
                1 if ui.get_wifi_enabled() => {
                    navigate_settings_view(ui, 2);
                }
                2 => {
                    ui.set_wifi_saved_index(0);
                    ui.set_wifi_saved_scroll_offset(0);
                    navigate_settings_view(ui, 3);
                    refresh_wifi_networks(ui, true);
                }
                3 => bluetooth_set_enabled(ui, !ui.get_bluetooth_enabled()),
                4 if ui.get_bluetooth_enabled() => {
                    navigate_settings_view(ui, 7);
                }
                5 if ui.get_bluetooth_enabled() => {
                    ui.set_bluetooth_known_index(0);
                    ui.set_bluetooth_known_scroll_offset(0);
                    refresh_bluetooth_devices(ui);
                    navigate_settings_view(ui, 8);
                }
                _ => {}
            },
            _ => {}
        },
        2 => {
            let network_count = ui.get_wifi_networks().row_count() as i32;
            let total = 1 + network_count;
            match action {
                "menu_up" if total > 0 => {
                    ui.set_wifi_network_index(move_model_selection(ui.get_wifi_network_index(), total, -1));
                    update_scroll_offset(ui, false);
                }
                "menu_down" if total > 0 => {
                    ui.set_wifi_network_index(move_model_selection(ui.get_wifi_network_index(), total, 1));
                    update_scroll_offset(ui, false);
                }
                "menu_confirm" => {
                    let index = ui.get_wifi_network_index();
                    if index == 0 {
                        ui.set_wifi_pending_hidden(true);
                        open_system_keyboard(ui, "Hidden Network SSID", 1, 2, "text", "");
                    } else if let Some(network) = ui.get_wifi_networks().row_data((index - 1) as usize) {
                        open_network_detail(ui, network, 2);
                    }
                }
                _ => {}
            }
        }
        3 => {
            let count = ui.get_wifi_saved_networks().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_wifi_saved_index(move_model_selection(ui.get_wifi_saved_index(), count, -1));
                    update_scroll_offset(ui, true);
                }
                "menu_down" if count > 0 => {
                    ui.set_wifi_saved_index(move_model_selection(ui.get_wifi_saved_index(), count, 1));
                    update_scroll_offset(ui, true);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(network) = ui.get_wifi_saved_networks().row_data(ui.get_wifi_saved_index() as usize) {
                        open_network_detail(ui, network, 3);
                    }
                }
                _ => {}
            }
        }
        4 => match action {
            "menu_up" => {
                ui.set_wifi_detail_index((ui.get_wifi_detail_index() + 8).rem_euclid(9));
                update_wifi_detail_scroll(ui);
            }
            "menu_down" => {
                ui.set_wifi_detail_index((ui.get_wifi_detail_index() + 1).rem_euclid(9));
                update_wifi_detail_scroll(ui);
            }
            "menu_left" | "menu_right" if ui.get_wifi_detail_index() == 2 => {
                let manual = ui.get_wifi_ip_mode().as_str() != "manual";
                if manual {
                    ui.set_wifi_ip_mode("manual".into());
                    if ui.get_wifi_ip_address().is_empty() {
                        ui.set_wifi_ip_address(ui.get_wifi_runtime_address());
                        ui.set_wifi_ip_prefix(ui.get_wifi_runtime_prefix());
                        ui.set_wifi_ip_netmask(ui.get_wifi_runtime_netmask());
                        ui.set_wifi_ip_gateway(ui.get_wifi_runtime_gateway());
                        ui.set_wifi_ip_dns(ui.get_wifi_runtime_dns());
                    }
                } else {
                    ui.set_wifi_ip_mode("automatic".into());
                }
            }
            "menu_confirm" => match ui.get_wifi_detail_index() {
                0 if ui.get_wifi_selected_current() => wifi_disconnect(ui),
                0 if matches!(ui.get_wifi_selected_security().as_str(), "Enterprise" | "WEP") => {
                    wifi_notice(ui, "This network security mode is not supported");
                }
                0 if ui.get_wifi_selected_saved() => {
                    wifi_connect(ui, ui.get_wifi_selected_ssid().to_string(), String::new(), false);
                }
                0 if ui.get_wifi_selected_security().as_str() == "Open" => {
                    wifi_connect(ui, ui.get_wifi_selected_ssid().to_string(), String::new(), false);
                }
                0 => {
                    let ssid = ui.get_wifi_selected_ssid().to_string();
                    open_system_keyboard(ui, &format!("Password • {ssid}"), 0, 4, "password", "");
                }
                1 if ui.get_wifi_selected_security().as_str() != "Open" => {
                    let ssid = ui.get_wifi_selected_ssid().to_string();
                    open_system_keyboard(ui, &format!("Password • {ssid}"), 0, 4, "password", "");
                }
                2 => {
                    let manual = ui.get_wifi_ip_mode().as_str() != "manual";
                    if manual {
                        ui.set_wifi_ip_mode("manual".into());
                        if ui.get_wifi_ip_address().is_empty() {
                            ui.set_wifi_ip_address(ui.get_wifi_runtime_address());
                            ui.set_wifi_ip_prefix(ui.get_wifi_runtime_prefix());
                            ui.set_wifi_ip_netmask(ui.get_wifi_runtime_netmask());
                            ui.set_wifi_ip_gateway(ui.get_wifi_runtime_gateway());
                            ui.set_wifi_ip_dns(ui.get_wifi_runtime_dns());
                        }
                    } else {
                        ui.set_wifi_ip_mode("automatic".into());
                    }
                }
                3 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "IPv4 Address", 2, 4, "ipv4", &ui.get_wifi_ip_address());
                }
                4 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "Subnet Mask", 6, 4, "ipv4", &ui.get_wifi_ip_netmask());
                }
                5 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "Gateway (optional)", 4, 4, "ipv4", &ui.get_wifi_ip_gateway());
                }
                6 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "DNS", 5, 4, "dns", &ui.get_wifi_ip_dns());
                }
                7 => save_wifi_ip_configuration(ui),
                8 if ui.get_wifi_selected_saved() => {
                    wifi_forget(ui, ui.get_wifi_selected_ssid().to_string());
                }
                _ => {}
            },
            _ => {}
        },
        7 => {
            let count = ui.get_bluetooth_devices().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_bluetooth_device_index(
                        move_model_selection(ui.get_bluetooth_device_index(), count, -1)
                    );
                    update_bluetooth_scroll(ui, false);
                }
                "menu_down" if count > 0 => {
                    ui.set_bluetooth_device_index(
                        move_model_selection(ui.get_bluetooth_device_index(), count, 1)
                    );
                    update_bluetooth_scroll(ui, false);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(device) =
                        ui.get_bluetooth_devices().row_data(ui.get_bluetooth_device_index() as usize)
                    {
                        open_bluetooth_detail(ui, device, 7);
                    }
                }
                _ => {}
            }
        }
        8 => {
            let count = ui.get_bluetooth_known_devices().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_bluetooth_known_index(
                        move_model_selection(ui.get_bluetooth_known_index(), count, -1)
                    );
                    update_bluetooth_scroll(ui, true);
                }
                "menu_down" if count > 0 => {
                    ui.set_bluetooth_known_index(
                        move_model_selection(ui.get_bluetooth_known_index(), count, 1)
                    );
                    update_bluetooth_scroll(ui, true);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(device) =
                        ui.get_bluetooth_known_devices().row_data(ui.get_bluetooth_known_index() as usize)
                    {
                        open_bluetooth_detail(ui, device, 8);
                    }
                }
                _ => {}
            }
        }
        9 => {
            let total = 2;
            match action {
                "menu_up" => ui.set_bluetooth_detail_index(
                    move_model_selection(ui.get_bluetooth_detail_index(), total, -1)
                ),
                "menu_down" => ui.set_bluetooth_detail_index(
                    move_model_selection(ui.get_bluetooth_detail_index(), total, 1)
                ),
                "menu_confirm" => {
                    let address = ui.get_bluetooth_selected_address().to_string();
                    if ui.get_bluetooth_detail_index() == 0 {
                        if !ui.get_bluetooth_selected_paired() {
                            bluetooth_device_operation(ui, "PairAndConnect", address);
                        } else if ui.get_bluetooth_selected_connected() {
                            bluetooth_device_operation(ui, "Disconnect", address);
                        } else {
                            bluetooth_device_operation(ui, "Connect", address);
                        }
                    } else if !ui.get_bluetooth_selected_paired() {
                        open_system_keyboard(
                            ui,
                            &format!("PIN / Passkey • {}", ui.get_bluetooth_selected_name()),
                            7,
                            9,
                            "password",
                            "",
                        );
                    } else {
                        bluetooth_device_operation(ui, "Forget", address);
                    }
                }
                _ => {}
            }
        }
        10 => match action {
            "menu_up" => ui.set_controllers_menu_index(
                move_model_selection(ui.get_controllers_menu_index(), 4, -1)
            ),
            "menu_down" => ui.set_controllers_menu_index(
                move_model_selection(ui.get_controllers_menu_index(), 4, 1)
            ),
            "menu_confirm" => match ui.get_controllers_menu_index() {
                0 => {
                    ui.set_player_assignment_index(0);
                    navigate_settings_view(ui, 11);
                }
                1 => {
                    ui.set_controller_list_index(0);
                    ui.set_controller_list_scroll_offset(0);
                    navigate_settings_view(ui, 12);
                }
                2 if ui.get_rumble_supported() => {
                    ui.set_rumble_index(0);
                    navigate_settings_view(ui, 16);
                }
                3 if ui.get_lighting_supported() => {
                    ui.set_lighting_index(0);
                    navigate_settings_view(ui, 17);
                }
                _ => {}
            },
            _ => {}
        },
        11 => {
            let count = ui.get_player_assignments().row_count() as i32;
            match action {
                "menu_up" if count > 0 => ui.set_player_assignment_index(
                    move_model_selection(ui.get_player_assignment_index(), count, -1)
                ),
                "menu_down" if count > 0 => ui.set_player_assignment_index(
                    move_model_selection(ui.get_player_assignment_index(), count, 1)
                ),
                "menu_confirm" if count > 0 => open_player_assignment_dropdown(ui),
                _ => {}
            }
        }
        12 => {
            let count = ui.get_controllers().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_controller_list_index(
                        move_model_selection(ui.get_controller_list_index(), count, -1)
                    );
                    update_controller_list_scroll(ui);
                }
                "menu_down" if count > 0 => {
                    ui.set_controller_list_index(
                        move_model_selection(ui.get_controller_list_index(), count, 1)
                    );
                    update_controller_list_scroll(ui);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(controller) =
                        ui.get_controllers().row_data(ui.get_controller_list_index() as usize)
                    {
                        open_controller_detail(ui, controller);
                    }
                }
                _ => {}
            }
        }
        13 => match action {
            "menu_up" => ui.set_controller_detail_index(
                move_model_selection(ui.get_controller_detail_index(), 2, -1)
            ),
            "menu_down" => ui.set_controller_detail_index(
                move_model_selection(ui.get_controller_detail_index(), 2, 1)
            ),
            "menu_confirm" => {
                if ui.get_controller_detail_index() == 0 {
                    ui.set_controller_mapping_index(0);
                    ui.set_controller_mapping_scroll_offset(0);
                    navigate_settings_view(ui, 15);
                } else {
                    ui.set_controller_tester_return_view(13);
                    navigate_settings_view(ui, 14);
                }
            }
            _ => {}
        },
        14 => {}
        15 => {
            let count = ui.get_controller_mapping().row_count() as i32 + 2;
            if ui.get_controller_remap_waiting() {
                return;
            }
            match action {
                "menu_up" if count > 0 => {
                    ui.set_controller_mapping_index(
                        move_model_selection(ui.get_controller_mapping_index(), count, -1)
                    );
                    update_controller_mapping_scroll(ui);
                }
                "menu_down" if count > 0 => {
                    ui.set_controller_mapping_index(
                        move_model_selection(ui.get_controller_mapping_index(), count, 1)
                    );
                    update_controller_mapping_scroll(ui);
                }
                "menu_left" if ui.get_controller_mapping_index() == 0 => {
                    set_controller_deadzone(ui, "left", -5);
                }
                "menu_right" | "menu_confirm" if ui.get_controller_mapping_index() == 0 => {
                    set_controller_deadzone(ui, "left", 5);
                }
                "menu_left" if ui.get_controller_mapping_index() == 1 => {
                    set_controller_deadzone(ui, "right", -5);
                }
                "menu_right" | "menu_confirm" if ui.get_controller_mapping_index() == 1 => {
                    set_controller_deadzone(ui, "right", 5);
                }
                "menu_confirm" if count > 2 => begin_controller_remap(ui),
                _ => {}
            }
        }
        16 => match action {
            "menu_up" => ui.set_rumble_index(
                move_model_selection(ui.get_rumble_index(), 2, -1)
            ),
            "menu_down" => ui.set_rumble_index(
                move_model_selection(ui.get_rumble_index(), 2, 1)
            ),
            "menu_left" | "menu_right" | "menu_confirm" if ui.get_rumble_index() == 0 => {
                rumble_set_enabled(ui, !ui.get_rumble_enabled());
            }
            "menu_confirm" if ui.get_rumble_index() == 1 => rumble_test(),
            _ => {}
        },
        17 => {
            if ui.get_lighting_preview_open() {
                match action {
                    "menu_left" => {
                        ui.set_lighting_preview_index(
                            (ui.get_lighting_preview_index() + 4).rem_euclid(5)
                        );
                        preview_lighting_effect(ui);
                    }
                    "menu_right" | "menu_confirm" => {
                        ui.set_lighting_preview_index(
                            (ui.get_lighting_preview_index() + 1).rem_euclid(5)
                        );
                        preview_lighting_effect(ui);
                    }
                    _ => {}
                }
            } else {
                let fixed_color = matches!(
                    ui.get_lighting_mode().as_str(),
                    "static" | "breathe" | "pulse" | "chase" | "wave"
                );
                match action {
                    "menu_up" => ui.set_lighting_index(
                        move_model_selection(ui.get_lighting_index(), 5, -1)
                    ),
                    "menu_down" => ui.set_lighting_index(
                        move_model_selection(ui.get_lighting_index(), 5, 1)
                    ),
                    "menu_confirm" if ui.get_lighting_index() == 0 => {
                        open_lighting_mode_dropdown(ui);
                    }
                    "menu_left" if ui.get_lighting_index() == 1 => {
                        let value = (ui.get_lighting_brightness() - 10).max(0);
                        lighting_call(ui, "SetBrightness", LightingArgs::Brightness(value));
                    }
                    "menu_right" | "menu_confirm" if ui.get_lighting_index() == 1 => {
                        let value = (ui.get_lighting_brightness() + 10).min(100);
                        lighting_call(ui, "SetBrightness", LightingArgs::Brightness(value));
                    }
                    "menu_confirm" if ui.get_lighting_index() == 2 && fixed_color => {
                        open_lighting_color_dropdown(ui);
                    }
                    "menu_left" | "menu_right" | "menu_confirm" if ui.get_lighting_index() == 3 => {
                        lighting_call(
                            ui,
                            "SetSystemEffects",
                            LightingArgs::Toggle(!ui.get_lighting_system_effects()),
                        );
                    }
                    "menu_confirm" if ui.get_lighting_index() == 4 => {
                        ui.set_lighting_preview_index(0);
                        ui.set_lighting_preview_open(true);
                        preview_lighting_effect(ui);
                    }
                    _ => {}
                }
            }
        },
        6 => match action {
            "menu_left" => move_keyboard_horizontal(ui, -1),
            "menu_right" => move_keyboard_horizontal(ui, 1),
            "menu_up" => move_keyboard_vertical(ui, -1),
            "menu_down" => move_keyboard_vertical(ui, 1),
            "menu_confirm" => handle_system_keyboard_confirm(ui),
            _ => {}
        },
        _ => {}
    }
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
                            let fields: Vec<&str> = line.split_whitespace().collect();
                            if fields.len() != 4 ||
                                fields[0] != "EVENT" ||
                                fields[1] != "1" ||
                                fields[3] != "pressed"
                            {
                                continue;
                            }

                            if fields[2] == "settings" {
                                if settings_active.swap(true, Ordering::SeqCst) {
                                    continue;
                                }
                                set_settings_capture(true);
                                let weak = weak.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        ui.set_settings_selected_index(0);
                                        navigate_settings_view(&ui, 0);
                                        ui.set_settings_open(true);
                                        wifi_notice(&ui, "");
                                    }
                                });
                                continue;
                            }

                            if settings_active.load(Ordering::SeqCst) && matches!(
                                fields[2],
                                "menu_up" | "menu_down" | "menu_left" | "menu_right" |
                                "menu_confirm" | "menu_back"
                            ) {
                                let action = fields[2].to_owned();
                                let weak = weak.clone();
                                let settings_active = settings_active.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        handle_settings_action(&ui, &action, &settings_active);
                                    }
                                });
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
    start_wifi_product_listener(&ui);
    start_bluetooth_product_listener(&ui);
    start_controllers_product_listener(&ui);
    start_rumble_listener(&ui);
    start_lighting_listener(&ui);
    start_lifecycle_listener(&ui);
    refresh_hint_mapping(&ui);

    let keyboard_weak = ui.as_weak();
    ui.on_system_keyboard_accepted(move || {
        if let Some(ui) = keyboard_weak.upgrade() {
            finish_system_keyboard(&ui);
        }
    });

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
