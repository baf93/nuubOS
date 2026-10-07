/*
 * PC game streaming (EPIC-025): the Moonlight application of nuubUI.
 *
 * nuubos-streamd owns the user's PCs, pairing, the applications a PC
 * advertises, the stream settings and the running stream; this module only
 * shows its snapshot and sends commands. Moonlight is the Settings shell with
 * its own rail (like the OOB): settings-view 40 PCs, 41 one PC, 42 Stream
 * Settings, plus the system keyboard (6) to add a PC by address.
 *
 * While a stream runs, Home is underneath the Moonlight window: its input is
 * ignored and Home Music stops, exactly as for a game (on_game_session).
 */

use crate::{
    guarded_scroll_offset, handle_settings_action, move_model_selection, navigate_settings_view,
    on_game_session, open_settings_choice, open_system_keyboard, play_ui_sound, tr, tr_arg,
    write_ui_context, HomeWindow, MoonlightHostEntry,
};
use slint::{ComponentHandle, Model, ModelRc, SharedString, VecModel};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::sync::Mutex;
use std::thread;
use std::time::Duration;

const STREAM_SOCKET: &str = "/run/nuubos/streamd.sock";
/* Keyboard purpose: the address of a PC to add. */
pub const KEYBOARD_PURPOSE_ADDRESS: i32 = 10;

/* The Moonlight pages own Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);
/* A stream runs: the Moonlight window is fullscreen above Home. */
pub static STREAM_RUNNING: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Default)]
struct Host {
    id: String,
    name: String,
    address: String,
    paired: bool,
    online: String,
    probing: bool,
}

/* nuubos-streamd snapshot (Moonlight and Steam Link). */
#[derive(Clone, Default)]
pub(crate) struct Snapshot {
    pub(crate) state: String,
    resolution: String,
    fps: i32,
    codec: String,
    bitrate: i32,
    discovering: bool,
    pairing_host: String,
    pairing_pin: String,
    apps_host: String,
    apps_state: String,
    hosts: Vec<Host>,
    apps: Vec<String>,
    pub(crate) steamlink_installed: bool,
    pub(crate) steamlink_version: String,
    pub(crate) steamlink_latest: String,
    pub(crate) steamlink_job: String,
    pub(crate) steamlink_progress: i32,
    pub(crate) steamlink_error: String,
}

pub(crate) static SNAPSHOT: Mutex<Option<Snapshot>> = Mutex::new(None);

pub(crate) fn command(line: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(STREAM_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.write_all(line.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply)?;
    Ok(reply.trim().to_owned())
}

/* Fire-and-forget: the outcome comes back in the snapshot. */
pub(crate) fn send(line: String) {
    thread::spawn(move || {
        if let Err(error) = command(&line) {
            eprintln!("home: streamd {line} failed={error}");
        }
    });
}

fn parse(text: &str) -> Snapshot {
    let mut s = Snapshot { fps: 60, ..Default::default() };
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "state" => s.state = value.into(),
            "resolution" => s.resolution = value.into(),
            "fps" => s.fps = value.parse().unwrap_or(60),
            "codec" => s.codec = value.into(),
            "bitrate" => s.bitrate = value.parse().unwrap_or(0),
            "discovering" => s.discovering = value == "1",
            "pairing_host" => s.pairing_host = value.into(),
            "pairing_pin" => s.pairing_pin = value.into(),
            "apps_host" => s.apps_host = value.into(),
            "apps_state" => s.apps_state = value.into(),
            "steamlink" => s.steamlink_installed = value == "installed",
            "steamlink_version" => s.steamlink_version = value.into(),
            "steamlink_latest" => s.steamlink_latest = value.into(),
            "steamlink_job" => s.steamlink_job = value.into(),
            "steamlink_progress" => s.steamlink_progress = value.parse().unwrap_or(-1),
            "steamlink_error" => s.steamlink_error = value.into(),
            "host" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 7 {
                    s.hosts.push(Host {
                        id: f[0].into(),
                        name: f[1].into(),
                        address: f[2].into(),
                        paired: f[3] == "1",
                        online: f[4].into(),
                        probing: f[6] == "1",
                    });
                }
            }
            "app" => {
                if let Some((_, name)) = value.split_once('\t') {
                    s.apps.push(name.into());
                }
            }
            _ => {}
        }
    }
    s
}

fn host_entry(h: &Host) -> MoonlightHostEntry {
    MoonlightHostEntry {
        id: h.id.clone().into(),
        name: h.name.clone().into(),
        address: h.address.clone().into(),
        paired: h.paired,
        online: h.online.clone().into(),
        probing: h.probing,
    }
}

fn row_count(ui: &HomeWindow) -> i32 {
    match ui.get_settings_view() {
        40 => ui.get_moonlight_hosts().row_count() as i32 + 3,
        41 if ui.get_moonlight_host().paired => ui.get_moonlight_apps().row_count() as i32 + 1,
        41 => 2,
        42 => 4,
        _ => 0,
    }
}

fn update_scroll(ui: &HomeWindow) {
    let count = row_count(ui);
    if count > 0 && ui.get_moonlight_index() >= count {
        ui.set_moonlight_index(count - 1);
    }
    ui.set_moonlight_scroll(guarded_scroll_offset(
        ui.get_moonlight_index(),
        count,
        ui.get_settings_list_visible_rows(),
        ui.get_moonlight_scroll(),
    ));
}

fn apply(ui: &HomeWindow, s: &Snapshot) {
    let hosts: Vec<MoonlightHostEntry> = s.hosts.iter().map(host_entry).collect();
    ui.set_moonlight_hosts(ModelRc::from(Rc::new(VecModel::from(hosts))));
    ui.set_moonlight_discovering(s.discovering);

    ui.set_moonlight_resolution_label(match s.resolution.as_str() {
        "720p" => "720p".into(),
        "1080p" => "1080p".into(),
        _ => tr(ui, 122, "Automatic").into(),
    });
    ui.set_moonlight_fps_label(tr_arg(ui, 460, "{0} fps", &s.fps.to_string()).into());
    ui.set_moonlight_codec_label(if s.codec == "hevc" { "HEVC".into() } else { "H.264".into() });
    ui.set_moonlight_bitrate_label(if s.bitrate > 0 {
        tr_arg(ui, 461, "{0} Mbps", &(s.bitrate / 1000).to_string()).into()
    } else {
        tr(ui, 122, "Automatic").into()
    });

    /* The open PC follows the snapshot; it disappears when removed. */
    let selected = ui.get_moonlight_host().id.to_string();
    if !selected.is_empty() {
        match s.hosts.iter().find(|h| h.id == selected) {
            Some(h) => {
                let was_paired = ui.get_moonlight_host().paired;
                ui.set_moonlight_host(host_entry(h));
                if ui.get_settings_view() == 41 && h.paired && !was_paired {
                    /* Just paired: show its applications. */
                    ui.set_moonlight_index(0);
                    send(format!("APPS\t{}", h.id));
                }
            }
            None => {
                if ui.get_settings_view() == 41 {
                    ui.set_moonlight_index(0);
                    navigate_settings_view(ui, 40);
                }
            }
        }
    }
    let pin = if !selected.is_empty() && s.pairing_host == selected { s.pairing_pin.clone() } else { String::new() };
    ui.set_moonlight_pairing_pin(pin.into());
    if s.apps_host == selected {
        let apps: Vec<SharedString> = s.apps.iter().map(|a| a.clone().into()).collect();
        ui.set_moonlight_apps(ModelRc::from(Rc::new(VecModel::from(apps))));
        ui.set_moonlight_apps_state(s.apps_state.clone().into());
    }
    update_scroll(ui);
}

/* Follows nuubos-streamd: the pages (Moonlight and Steam Link), and
 * stream start/end for Home. */
pub fn start_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(STREAM_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut block = String::new();
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    block.push_str(&line);
                    block.push('\n');
                    if line != "end=1" {
                        continue;
                    }
                    let snapshot = parse(&block);
                    block.clear();
                    /* "exiting" still covers Home until Moonlight is gone. */
                    let running = snapshot.state != "idle";
                    let changed = STREAM_RUNNING.swap(running, Ordering::SeqCst) != running;
                    *SNAPSHOT.lock().unwrap() = Some(snapshot.clone());
                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            apply(&ui, &snapshot);
                            crate::steamlink::apply(&ui, &snapshot);
                            if changed {
                                on_game_session(&ui, running);
                            }
                        }
                    });
                }
            }
        }
        /* streamd restarted or not up yet: no stream survives it. */
        if STREAM_RUNNING.swap(false, Ordering::SeqCst) {
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    on_game_session(&ui, false);
                }
            });
        }
        thread::sleep(Duration::from_millis(500));
    });
}

pub fn open(ui: &HomeWindow) {
    ACTIVE.store(true, Ordering::SeqCst);
    ui.set_moonlight_active(true);
    ui.set_moonlight_index(0);
    ui.set_moonlight_scroll(0);
    ui.set_moonlight_notice("".into());
    ui.set_moonlight_host(MoonlightHostEntry::default());
    navigate_settings_view(ui, 40);
    ui.set_settings_open(true);
    write_ui_context("moonlight");
    if let Some(s) = SNAPSHOT.lock().unwrap().clone() {
        apply(ui, &s);
    }
    /* Fresh state of the known PCs, and any new one on the network. */
    thread::spawn(|| {
        let _ = command("REFRESH");
        let _ = command("DISCOVER");
    });
}

/* Back to Home (also when the user picker takes over). */
pub fn leave(ui: &HomeWindow) {
    if !ui.get_moonlight_pairing_pin().is_empty() {
        send("PAIR_CANCEL".into());
    }
    ACTIVE.store(false, Ordering::SeqCst);
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_moonlight_active(false);
    ui.set_moonlight_host(MoonlightHostEntry::default());
    write_ui_context("home");
}

fn open_host(ui: &HomeWindow, host: MoonlightHostEntry) {
    let id = host.id.to_string();
    let paired = host.paired;
    ui.set_moonlight_host(host);
    ui.set_moonlight_apps(ModelRc::from(Rc::new(VecModel::<SharedString>::default())));
    ui.set_moonlight_apps_state(if paired { "loading".into() } else { "none".into() });
    ui.set_moonlight_pairing_pin("".into());
    ui.set_moonlight_remove_confirm(false);
    ui.set_moonlight_notice("".into());
    ui.set_moonlight_index(0);
    ui.set_moonlight_scroll(0);
    navigate_settings_view(ui, 41);
    send(if paired { format!("APPS\t{id}") } else { format!("REFRESH\t{id}") });
}

fn back_to_hosts(ui: &HomeWindow) {
    if !ui.get_moonlight_pairing_pin().is_empty() {
        send("PAIR_CANCEL".into());
    }
    /* Focus returns to the PC that was open. */
    let id = ui.get_moonlight_host().id.to_string();
    let hosts = ui.get_moonlight_hosts();
    let index = (0..hosts.row_count()).find(|&i| hosts.row_data(i).map(|h| h.id == id).unwrap_or(false));
    ui.set_moonlight_host(MoonlightHostEntry::default());
    ui.set_moonlight_remove_confirm(false);
    ui.set_moonlight_notice("".into());
    navigate_settings_view(ui, 40);
    ui.set_moonlight_index(index.map(|i| i as i32).unwrap_or(0));
    update_scroll(ui);
}

fn launch(ui: &HomeWindow, app: String) {
    let host = ui.get_moonlight_host().id.to_string();
    let weak = ui.as_weak();
    ui.set_moonlight_notice("".into());
    thread::spawn(move || {
        let reply = command(&format!("LAUNCH\t{host}\t{app}")).unwrap_or_default();
        if reply == "OK" {
            return;
        }
        let reason = reply.strip_prefix("ERR ").unwrap_or("").to_owned();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let text = match reason.as_str() {
                /* A double press while the stream is starting. */
                "busy" => return,
                "unpaired" => tr(&ui, 466, "The PC must be paired again"),
                "host" => tr(&ui, 452, "The PC could not be reached"),
                _ => tr(&ui, 464, "The stream could not be started"),
            };
            ui.set_moonlight_notice(text.into());
        });
    });
}

fn open_setting(ui: &HomeWindow, row: i32) {
    let Some(s) = SNAPSHOT.lock().unwrap().clone() else { return };
    let automatic = tr(ui, 122, "Automatic");
    match row {
        0 => open_settings_choice(
            ui,
            "moonlight-resolution",
            &tr(ui, 454, "Resolution"),
            vec![
                ("auto".into(), automatic),
                ("720p".into(), "720p".into()),
                ("1080p".into(), "1080p".into()),
            ],
            &s.resolution,
        ),
        1 => open_settings_choice(
            ui,
            "moonlight-fps",
            &tr(ui, 455, "Frame Rate"),
            vec![
                ("60".into(), tr_arg(ui, 460, "{0} fps", "60")),
                ("30".into(), tr_arg(ui, 460, "{0} fps", "30")),
            ],
            &s.fps.to_string(),
        ),
        2 => open_settings_choice(
            ui,
            "moonlight-codec",
            &tr(ui, 456, "Video Codec"),
            vec![("h264".into(), "H.264".into()), ("hevc".into(), "HEVC".into())],
            &s.codec,
        ),
        _ => {
            let mut options = vec![("0".to_owned(), automatic)];
            for mbps in [5, 10, 15, 20, 30, 40] {
                options.push(((mbps * 1000).to_string(), tr_arg(ui, 461, "{0} Mbps", &mbps.to_string())));
            }
            open_settings_choice(ui, "moonlight-bitrate", &tr(ui, 457, "Bitrate"), options, &s.bitrate.to_string());
        }
    }
}

/* A Stream Settings dropdown choice (apply_settings_choice). */
pub fn apply_choice(context: &str, value: &str) {
    let key = context.trim_start_matches("moonlight-");
    send(format!("SET\t{key}\t{value}"));
}

/* System keyboard purpose KEYBOARD_PURPOSE_ADDRESS. */
pub fn add_host(ui: &HomeWindow, value: &str) {
    let address = value.trim().to_owned();
    let valid = !address.is_empty()
        && address.len() < 64
        && !address.starts_with('-')
        && address.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | ':'));
    if !valid {
        ui.set_moonlight_notice(tr(ui, 459, "Enter a valid address").into());
        return;
    }
    ui.set_moonlight_notice("".into());
    ui.set_keyboard_value("".into());
    navigate_settings_view(ui, 40);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = command(&format!("ADD\t{address}")).unwrap_or_default();
        let id = reply.strip_prefix("OK ").map(str::to_owned);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            match id {
                /* Focus the new PC. */
                Some(id) => {
                    if let Some(s) = SNAPSHOT.lock().unwrap().clone() {
                        if let Some(i) = s.hosts.iter().position(|h| h.id == id) {
                            ui.set_moonlight_index(i as i32);
                            update_scroll(&ui);
                        }
                    }
                }
                None => ui.set_moonlight_notice(tr(&ui, 459, "Enter a valid address").into()),
            }
        });
    });
}

pub fn handle_action(ui: &HomeWindow, action: &str, settings_active: &Arc<AtomicBool>) {
    /* Dropdowns and the keyboard behave exactly as in Settings. */
    if ui.get_settings_choice_open() || ui.get_settings_view() == 6 {
        handle_settings_action(ui, action, settings_active);
        return;
    }
    let view = ui.get_settings_view();
    let index = ui.get_moonlight_index();
    let count = row_count(ui);
    play_ui_sound(action);
    match action {
        "menu_up" | "menu_down" => {
            ui.set_moonlight_index(move_model_selection(index, count, if action == "menu_up" { -1 } else { 1 }));
            ui.set_moonlight_remove_confirm(false);
            update_scroll(ui);
            return;
        }
        "menu_back" => {
            match view {
                41 | 42 => back_to_hosts(ui),
                _ => leave(ui),
            }
            return;
        }
        "menu_confirm" => {}
        _ => return,
    }
    match view {
        40 => {
            let hosts = ui.get_moonlight_hosts();
            let n = hosts.row_count() as i32;
            if index < n {
                if let Some(host) = hosts.row_data(index as usize) {
                    open_host(ui, host);
                }
            } else if index == n {
                send("DISCOVER".into());
            } else if index == n + 1 {
                ui.set_moonlight_notice("".into());
                open_system_keyboard(ui, &tr(ui, 441, "PC address"), KEYBOARD_PURPOSE_ADDRESS, 40, "text", "");
            } else {
                ui.set_moonlight_index(0);
                navigate_settings_view(ui, 42);
            }
        }
        41 => {
            let host = ui.get_moonlight_host();
            let remove_row = if host.paired { ui.get_moonlight_apps().row_count() as i32 } else { 1 };
            if index == remove_row {
                if ui.get_moonlight_remove_confirm() {
                    send(format!("REMOVE\t{}", host.id));
                    back_to_hosts(ui);
                } else {
                    ui.set_moonlight_remove_confirm(true);
                }
            } else if host.paired {
                if let Some(app) = ui.get_moonlight_apps().row_data(index as usize) {
                    launch(ui, app.to_string());
                }
            } else if ui.get_moonlight_pairing_pin().is_empty() {
                send(format!("PAIR\t{}", host.id));
            } else {
                send("PAIR_CANCEL".into());
            }
        }
        42 => open_setting(ui, index),
        _ => {}
    }
}
