/*
 * Game pages of nuubUI, all built from the owning services' data on the
 * generic rows of home.slint (GenRow):
 *
 *   50 Game Details (EPIC-016)      nuubos-libraryd DETAILS + nuubos-emud
 *   51 Game Settings (EPIC-018)     nuubos-emud GAME_SETTINGS / GAME_SET
 *   52/53 BIOS Files (EPIC-033)     nuubos-biosctl
 *   54 RetroAchievements (EPIC-017) nuubos-achievementsctl
 *   55 Game Hotkeys, 56 Performance Overlay   nuubos-emud SETTINGS / SET
 *   57 Start Game (Details shell)   nuubos-emud STATES, LAUNCH <mode>
 *   63 Software Update (EPIC-048)   nuubos-updatectl, nuubos-update-apply
 *   66 Syncthing (EPIC-034)         nuubos-syncctl (per user)
 *
 * Settings → Gaming (68) itself is built here too (gaming-rows): the
 * RetroAchievements account, the ScreenScraper account and bulk scraping
 * (EPIC-015, nuubos-scraper, nuubos-jobd) and BIOS Files.
 *
 * The optional network services (Remote Services EPIC-037/039 through
 * nuubos-remotectl, Syncthing) are also rows of Settings → Connectivity,
 * section SERVICES (connectivity-services), built here from the same
 * replies.
 *
 * 50 is the console game page GameDetailsPage (details-page: hero cover,
 * blurred backdrop, metadata chips, description, a row of actions), 51
 * and 57 a Settings shell of their own (details-active), all opened from
 * Home; 52-54 are Settings pages. This module keeps
 * no product state: every row reflects the last service reply and every
 * action is a service request. Long work (scraping) is a nuubos-jobd job
 * whose progress is the job's Live Notification.
 */

use crate::{
    guarded_scroll_offset, handle_settings_action, move_model_selection, navigate_settings_view, sectioned_scroll,
    open_settings_choice, open_system_keyboard, play_ui_sound, tr, tr_arg, write_ui_context,
    DetailAction, DetailChip, GenRow, HomeWindow,
};
use slint::{ComponentHandle, Image, Model, ModelRc, Rgba8Pixel, SharedPixelBuffer, VecModel};
use std::cell::RefCell;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::UnixStream;
use std::process::{Command, Stdio};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const LIBRARY_SOCKET: &str = "/run/nuubos/libraryd.sock";
const EMULATION_SOCKET: &str = "/run/nuubos/emud.sock";

/* System keyboard purposes owned by this module. */
pub const KEYBOARD_COLLECTION: i32 = 11;
pub const KEYBOARD_RA_NAME: i32 = 12;
pub const KEYBOARD_RA_PASSWORD: i32 = 13;
pub const KEYBOARD_SS_NAME: i32 = 14;
pub const KEYBOARD_SS_PASSWORD: i32 = 15;
pub const KEYBOARD_TGDB_KEY: i32 = 26;
pub const KEYBOARD_SEARCH: i32 = 27;

/* The Details shell owns Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Default)]
struct Member {
    id: String,
    name: String,
    member: bool,
}

#[derive(Clone, Default)]
struct Details {
    id: String,
    system: String,
    system_name: String,
    title: String,
    /* Cover art path (libraryd). */
    cover: String,
    available: bool,
    last: i64,
    time: i64,
    sessions: i64,
    path: String,
    members: Vec<Member>,
    meta: Vec<(String, String)>,
    /* nuubos-emud */
    core: String,
    core_default: String,
    core_override: String,
    cores: Vec<String>,
    aspect: String,
    filter: String,
    filter_default: String,
    /* nuubos-biosctl: missing required firmware for the system. */
    bios_missing: bool,
}

impl Details {
    fn meta(&self, key: &str) -> &str {
        self.meta.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str()).unwrap_or("")
    }
}

#[derive(Clone, Default)]
struct BiosSystem {
    id: String,
    name: String,
    state: String,
    present: i64,
    total: i64,
}

#[derive(Clone, Default)]
struct BiosFile {
    system: String,
    path: String,
    required: bool,
    state: String,
    desc: String,
}

#[derive(Default)]
struct State {
    details: Details,
    bios: Vec<BiosSystem>,
    bios_files: Vec<BiosFile>,
    bios_system: String,
    ra_user: String,
    ra_enabled: bool,
    ra_hardcore: bool,
    ra_name_pending: String,
    ss_user: String,
    ss_available: bool,
    /* Metadata source (nuubos-scraper provider) and the end of the user's
     * TheGamesDB key. */
    ss_provider: String,
    ss_apikey: String,
    ss_name_pending: String,
    /* A bulk scrape job is running. */
    ss_busy: bool,
    /* Scan for New Games: 1 = requested, 2 = libraryd reported it running. */
    lib_scan: u8,
    /* Row keys of Settings → Gaming, in order. */
    gaming_keys: Vec<String>,
    /* The user's emulation settings (nuubos-emud SETTINGS). */
    emu: Vec<(String, String)>,
    /* Start Game (57): the game's save states (slot, mtime, local hh:mm;
     * slot -1 = the automatic state). */
    states: Vec<(i32, i64, String)>,
    remote: Vec<(String, String)>,
    remote_password: String,
    update: Vec<(String, String)>,
    update_result: String,
    sync: Vec<(String, String)>,
    /* Row keys of Connectivity's SERVICES section, in order. */
    service_keys: Vec<String>,
    /* Row keys of the page on screen, in GenRow order. */
    keys: Vec<String>,
    actionable: Vec<bool>,
    /* The destructive row waiting for its confirming press. */
    armed: String,
    busy: bool,
    /* Where Back goes from pages 52-54, 63 and 66. */
    back: i32,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

/* ---------------------------------------------------------------- */
/* Service requests                                                 */
/* ---------------------------------------------------------------- */

fn request(socket: &str, line: &str, multi: bool, timeout: u64) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(socket)?;
    stream.set_read_timeout(Some(Duration::from_secs(timeout)))?;
    stream.write_all(line.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reply = String::new();
    let mut reader = BufReader::new(stream);
    loop {
        let mut l = String::new();
        if reader.read_line(&mut l)? == 0 {
            break;
        }
        let done = !multi || l.trim_end() == "end=1" || l.starts_with("ERR");
        reply.push_str(&l);
        if done {
            break;
        }
    }
    Ok(reply)
}

fn library(line: &str, multi: bool) -> String {
    request(LIBRARY_SOCKET, line, multi, 3).unwrap_or_default()
}

fn emulation(line: &str, multi: bool) -> String {
    request(EMULATION_SOCKET, line, multi, 3).unwrap_or_default()
}

/* A product CLI (the service contract of these features), optionally fed
 * one secret line on stdin so it never appears in a process list. */
pub(crate) fn run_tool(program: &str, args: &[&str], stdin_line: Option<String>) -> String {
    let mut cmd = Command::new(program);
    cmd.args(args).stdout(Stdio::piped()).stderr(Stdio::null());
    cmd.stdin(if stdin_line.is_some() { Stdio::piped() } else { Stdio::null() });
    let Ok(mut child) = cmd.spawn() else { return String::new() };
    if let (Some(line), Some(mut input)) = (stdin_line, child.stdin.take()) {
        let _ = input.write_all(line.as_bytes());
        let _ = input.write_all(b"\n");
    }
    let mut out = String::new();
    if let Some(mut stdout) = child.stdout.take() {
        let _ = stdout.read_to_string(&mut out);
    }
    let _ = child.wait();
    out
}

fn active_user() -> String {
    std::fs::read_to_string("/run/nuubos/user/active").map(|s| s.trim().to_owned()).unwrap_or_default()
}

/* ---------------------------------------------------------------- */
/* Parsing                                                          */
/* ---------------------------------------------------------------- */

fn parse_details(reply: &str, d: &mut Details) {
    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "game" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 9 {
                    d.id = f[0].into();
                    d.system = f[1].into();
                    d.title = f[2].into();
                    d.cover = f[3].into();
                    d.last = f[5].parse().unwrap_or(0);
                    d.time = f[6].parse().unwrap_or(0);
                    d.available = f[7] == "1";
                }
            }
            "system_name" => d.system_name = value.into(),
            "sessions" => d.sessions = value.parse().unwrap_or(0),
            "path" => d.path = value.into(),
            "member" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 3 {
                    d.members.push(Member { id: f[0].into(), name: f[1].into(), member: f[2] == "1" });
                }
            }
            k if k.starts_with("meta_") => d.meta.push((k[5..].to_owned(), value.to_owned())),
            _ => {}
        }
    }
}

fn parse_game_settings(reply: &str, d: &mut Details) {
    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "core" => d.core = value.into(),
            "core_default" => d.core_default = value.into(),
            "core_override" => d.core_override = value.into(),
            "cores" => d.cores = value.split(',').filter(|c| !c.is_empty()).map(str::to_owned).collect(),
            "aspect" => d.aspect = value.into(),
            "filter" => d.filter = value.into(),
            "filter_default" => d.filter_default = value.into(),
            _ => {}
        }
    }
}

fn parse_bios(reply: &str) -> (Vec<BiosSystem>, Vec<BiosFile>) {
    let mut systems = Vec::new();
    let mut files = Vec::new();
    for line in reply.lines() {
        if let Some(v) = line.strip_prefix("system=") {
            let f: Vec<&str> = v.split('\t').collect();
            if f.len() >= 6 {
                systems.push(BiosSystem {
                    id: f[0].into(),
                    name: f[1].into(),
                    state: f[3].into(),
                    present: f[4].parse().unwrap_or(0),
                    total: f[5].parse().unwrap_or(0),
                });
            }
        } else if let Some(v) = line.strip_prefix("file=") {
            let f: Vec<&str> = v.split('\t').collect();
            if f.len() >= 5 {
                files.push(BiosFile {
                    system: f[0].into(),
                    path: f[1].into(),
                    required: f[2] == "1",
                    state: f[3].into(),
                    desc: f[4].into(),
                });
            }
        }
    }
    (systems, files)
}

fn kv<'a>(reply: &'a str, key: &str) -> &'a str {
    reply
        .lines()
        .find_map(|l| l.strip_prefix(key).and_then(|r| r.strip_prefix('=')))
        .unwrap_or("")
}

fn pairs(reply: &str) -> Vec<(String, String)> {
    reply
        .lines()
        .filter_map(|l| l.split_once('='))
        .filter(|(k, _)| *k != "end")
        .map(|(k, v)| (k.to_owned(), v.to_owned()))
        .collect()
}

fn emu_get(st: &State, key: &str) -> String {
    st.emu.iter().find(|(k, _)| k == key).map(|(_, v)| v.clone()).unwrap_or_default()
}

/* Game hotkey actions in page order, with their labels. */
const HOTKEYS: [(&str, usize, &str); 7] = [
    ("save_state", 417, "Save State"),
    ("load_state", 418, "Load State"),
    ("slot_next", 779, "Next Slot"),
    ("slot_prev", 780, "Previous Slot"),
    ("fast_forward", 781, "Fast Forward"),
    ("quit", 422, "Quit Game"),
    ("screenshot", 592, "Screenshot"),
];

/* Controls a hotkey can use (Quick Menu button + this control). */
const HOTKEY_CONTROLS: [&str; 16] = [
    "menu_confirm", "menu_back", "face_north", "face_west", "menu_up", "menu_down", "menu_left",
    "menu_right", "l1", "r1", "l2", "r2", "l3", "r3", "settings", "select",
];

/* Performance overlay fields, in overlay order. */
const OVERLAY_FIELDS: [(&str, usize, &str); 8] = [
    ("fps", 783, "Frame Rate"),
    ("cpu", 0, "CPU"),
    ("gpu", 0, "GPU"),
    ("ram", 0, "RAM"),
    ("temp", 784, "Temperature"),
    ("power", 785, "Power Draw"),
    ("battery", 786, "Battery"),
    ("clock", 787, "Clock"),
];

/* "today • 15:32" for a state saved at mtime (local hh:mm from emud). */
fn saved_when(ui: &HomeWindow, mtime: i64, hhmm: &str) -> String {
    format!("{} • {}", days_ago(ui, mtime), hhmm)
}

/* ---------------------------------------------------------------- */
/* Labels                                                           */
/* ---------------------------------------------------------------- */

pub(crate) fn row(title: String, value: String, detail: String) -> GenRow {
    GenRow { title: title.into(), value: value.into(), detail: detail.into(), enabled: true, ..Default::default() }
}

pub(crate) fn nav(title: String, value: String) -> GenRow {
    GenRow { navigates: true, ..row(title, value, String::new()) }
}

pub(crate) fn toggle(title: String, on: bool, detail: String) -> GenRow {
    GenRow { toggle: true, toggle_on: on, ..row(title, String::new(), detail) }
}

fn duration(ui: &HomeWindow, seconds: i64) -> String {
    let minutes = (seconds / 60).max(1);
    if minutes >= 60 {
        tr(ui, 407, "{0} h {1} min")
            .replace("{0}", &(minutes / 60).to_string())
            .replace("{1}", &format!("{:02}", minutes % 60))
    } else {
        tr_arg(ui, 408, "{0} min", &minutes.to_string())
    }
}

fn days_ago(ui: &HomeWindow, last: i64) -> String {
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    let days = (now - last).max(0) / 86400;
    match days {
        0 => tr(ui, 402, "today"),
        1 => tr(ui, 403, "yesterday"),
        n => tr_arg(ui, 404, "{0} days ago", &n.to_string()),
    }
}

fn aspect_label(ui: &HomeWindow, v: &str) -> String {
    match v {
        "core" => tr(ui, 529, "As the game"),
        "4:3" => "4:3".into(),
        "16:9" => "16:9".into(),
        "full" => tr(ui, 530, "Full Screen"),
        "integer" => tr(ui, 531, "Integer Scaling"),
        _ => String::new(),
    }
}

fn filter_label(ui: &HomeWindow, v: &str) -> String {
    match v {
        "sharp" => tr(ui, 532, "Sharp"),
        "smooth" => tr(ui, 533, "Smooth"),
        "pixel" => tr(ui, 534, "Pixel Perfect"),
        "crt" => tr(ui, 777, "CRT Effect"),
        _ => String::new(),
    }
}

fn bios_state_label(ui: &HomeWindow, v: &str) -> String {
    match v {
        "ready" | "ok" => tr(ui, 66, "Ready"),
        "missing" => tr(ui, 577, "Missing"),
        "invalid" => tr(ui, 578, "Invalid"),
        "optional" => tr(ui, 579, "Optional"),
        "unverified" => tr(ui, 580, "Not verifiable"),
        other => other.to_owned(),
    }
}

fn default_label(ui: &HomeWindow, inherited: &str) -> String {
    tr_arg(ui, 528, "Default ({0})", inherited)
}

/* ---------------------------------------------------------------- */
/* Pages                                                            */
/* ---------------------------------------------------------------- */

pub(crate) fn armed_detail(armed: &str, key: &str, prompt: String, normal: String) -> String {
    if armed == key { prompt } else { normal }
}

/* Rows of the page on screen: (key, row, actionable). */
fn build_rows(ui: &HomeWindow, st: &State, view: i32) -> Vec<(String, GenRow, bool)> {
    let mut rows: Vec<(String, GenRow, bool)> = Vec::new();
    let d = &st.details;
    match view {
        51 => {
            let core = if d.core_override.is_empty() { default_label(ui, &d.core_default) } else { d.core_override.clone() };
            rows.push(("core".into(), nav(tr(ui, 525, "Emulator"), core), d.cores.len() > 1));
            let aspect = if d.aspect.is_empty() { default_label(ui, &tr(ui, 531, "Integer Scaling")) } else { aspect_label(ui, &d.aspect) };
            rows.push(("aspect".into(), nav(tr(ui, 526, "Aspect Ratio"), aspect), true));
            let filter = if d.filter.is_empty() { default_label(ui, &filter_label(ui, &d.filter_default)) } else { filter_label(ui, &d.filter) };
            rows.push(("filter".into(), nav(tr(ui, 527, "Video Filter"), filter), true));
            rows.push(("reset".into(), row(tr(ui, 535, "Reset Game Settings"), String::new(),
                armed_detail(&st.armed, "reset", tr(ui, 598, "Press again to reset"),
                    tr(ui, 536, "Changes apply the next time the game starts"))), true));
        }
        52 => {
            /* YOUR SYSTEMS (at least one game in the library) first, then
             * the other supported systems (bios_split). */
            let mine = crate::library::system_ids();
            let (own, other): (Vec<&BiosSystem>, Vec<&BiosSystem>) = st.bios.iter().partition(|s| mine.contains(&s.id));
            if own.is_empty() {
                let mut r = row(tr(ui, 398, "No games yet"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for s in own.into_iter().chain(other) {
                let mut r = nav(s.name.clone(), bios_state_label(ui, &s.state));
                /* Only a system that cannot start says how far it is. */
                if s.state == "missing" || s.state == "invalid" {
                    r.detail = tr(ui, 582, "{0} of {1} present")
                        .replace("{0}", &s.present.to_string())
                        .replace("{1}", &s.total.to_string())
                        .into();
                }
                rows.push((format!("sys:{}", s.id), r, true));
            }
        }
        53 => {
            for f in st.bios_files.iter().filter(|f| f.system == st.bios_system) {
                let mut detail = f.desc.clone();
                if !f.required {
                    detail = format!("{} • {}", tr(ui, 579, "Optional"), detail);
                }
                rows.push(("info".into(), row(f.path.clone(), bios_state_label(ui, &f.state), detail), false));
            }
        }
        54 => {
            if st.ra_user.is_empty() {
                rows.push(("ra-signin".into(), row(tr(ui, 587, "Sign In"), tr(ui, 566, "Not signed in"),
                    tr(ui, 591, "Your password is used once and never stored")), !st.busy));
            } else {
                rows.push(("info".into(), row(tr(ui, 588, "Signed in"), st.ra_user.clone(), String::new()), false));
                rows.push(("ra-enabled".into(), toggle(tr(ui, 112, "Enabled"), st.ra_enabled, String::new()), true));
                rows.push(("ra-hardcore".into(), toggle(tr(ui, 585, "Hardcore Mode"), st.ra_hardcore,
                    tr(ui, 586, "No save states or fast-forward; achievements count double")), true));
                rows.push(("ra-signout".into(), row(tr(ui, 567, "Sign Out"), String::new(),
                    armed_detail(&st.armed, "ra-signout", tr(ui, 191, "Press again to confirm"), String::new())), true));
            }
        }
        66 => {
            let get = |k: &str| st.sync.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
            let enabled = get("enabled") == "1";
            rows.push(("sync-enabled".into(), toggle(tr(ui, 669, "Syncthing"), enabled,
                if get("failed") == "1" { tr(ui, 677, "Syncthing stopped unexpectedly") }
                else { tr(ui, 670, "Keeps your saves in sync with your other devices. It is not a backup.") }), true));
            if enabled && !get("device_id").is_empty() {
                rows.push(("info".into(), row(tr(ui, 671, "This Console's ID"), String::new(),
                    format!("{}  •  {}", get("device_id"), tr(ui, 672, "Add it on your other device; it then appears here to accept"))), false));
                for (kind, idx, fallback) in [("saves", 673, "Saves"), ("states", 674, "Save States"), ("screenshots", 675, "Screenshots")] {
                    let on = st.sync.iter().any(|(k, v)| k == "folder" && v == &format!("{kind}\t1"));
                    rows.push((format!("sync-folder:{kind}"), toggle(tr(ui, idx, fallback), on, String::new()), true));
                }
                for (k, v) in &st.sync {
                    let f: Vec<&str> = v.split('\t').collect();
                    if k == "pending" && f.len() >= 2 {
                        rows.push((format!("sync-accept:{}", f[0]), row(if f[1].is_empty() { f[0].to_owned() } else { f[1].to_owned() },
                            String::new(), tr(ui, 676, "Wants to connect • press to accept")), true));
                    } else if k == "device" && f.len() >= 3 {
                        let key = format!("sync-remove:{}", f[0]);
                        rows.push((key.clone(), row(if f[1].is_empty() { f[0].to_owned() } else { f[1].to_owned() },
                            if f[2] == "1" { tr(ui, 198, "Connected") } else { tr(ui, 159, "Not connected") },
                            armed_detail(&st.armed, &key, tr(ui, 191, "Press again to confirm"), String::new())), true));
                    }
                }
            }
        }
        63 => {
            let get = |k: &str| st.update.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
            let build = get("build");
            rows.push(("info".into(), row(tr(ui, 485, "Version"),
                if build.is_empty() { get("current") } else { format!("{} • {}", get("current"), build) }, String::new()), false));
            if get("provider") != "available" {
                rows.push(("info".into(), row(tr(ui, 655, "Updates are not available in this build"), String::new(), String::new()), false));
            } else {
                let available = get("available") == "1";
                let check_value = if st.busy { tr(ui, 488, "Checking…") }
                    else if available { tr_arg(ui, 652, "nuubOS {0} available", &get("version")) }
                    else if !st.update_result.is_empty() { st.update_result.clone() }
                    else { String::new() };
                rows.push(("update-check".into(), row(tr(ui, 487, "Check for Updates"), check_value, String::new()), !st.busy));
                if available && get("downloaded") != "1" {
                    rows.push(("update-download".into(), row(tr(ui, 653, "Download"), String::new(), get("notes")), !st.busy));
                }
                if available && get("downloaded") == "1" {
                    rows.push(("update-install".into(), row(tr(ui, 654, "Install and Restart"), String::new(),
                        armed_detail(&st.armed, "update-install", tr(ui, 191, "Press again to confirm"),
                            tr(ui, 657, "Your games, saves and settings are kept"))), true));
                }
            }
        }
        55 => {
            let mut hint = row(tr(ui, 773, "Hold Hotkey and press a button"), String::new(), String::new());
            hint.enabled = false;
            rows.push(("info".into(), hint, false));
            for (action, idx, fallback) in HOTKEYS {
                let control = emu_get(st, &format!("hotkey_{action}"));
                rows.push((format!("hk:{action}"), nav(tr(ui, idx, fallback), crate::control_label(ui, &control)), true));
            }
        }
        56 => {
            let on = emu_get(st, "overlay") == "1";
            rows.push(("ov-on".into(), toggle(tr(ui, 782, "Show Overlay"), on, String::new()), true));
            let items = emu_get(st, "overlay_items");
            for (field, idx, fallback) in OVERLAY_FIELDS {
                let label = if idx == 0 { fallback.to_owned() } else { tr(ui, idx, fallback) };
                let shown = items.split(',').any(|f| f == field);
                let mut r = toggle(label, shown, String::new());
                r.enabled = on;
                rows.push((format!("ov:{field}"), r, on));
            }
        }
        57 => {
            /* Continue = the most recent state; then every state; then a
             * fresh start. */
            if let Some(newest) = st.states.iter().max_by_key(|s| s.1) {
                let what = if newest.0 < 0 { tr(ui, 765, "Automatic Save") } else { tr_arg(ui, 423, "Slot {0}", &newest.0.to_string()) };
                rows.push(("start:resume".into(), row(tr(ui, 763, "Continue"), what,
                    tr_arg(ui, 766, "Saved {0}", &saved_when(ui, newest.1, &newest.2))), true));
            }
            for (slot, mtime, hhmm) in &st.states {
                let title = if *slot < 0 { tr(ui, 765, "Automatic Save") } else { tr_arg(ui, 423, "Slot {0}", &slot.to_string()) };
                rows.push((format!("start:slot:{slot}"), row(title, String::new(),
                    tr_arg(ui, 766, "Saved {0}", &saved_when(ui, *mtime, hhmm))), true));
            }
            rows.push(("start:new".into(), row(tr(ui, 764, "Start from the Beginning"), String::new(), String::new()), true));
        }
        _ => {}
    }
    rows
}

/* ---------------------------------------------------------------- */
/* Game Details page (50)                                           */
/* ---------------------------------------------------------------- */

fn detail_action(icon: i32, label: String, primary: bool) -> DetailAction {
    DetailAction { icon, label: label.into(), primary, on: false, enabled: true, armed: false, busy: false }
}

fn detail_chip(icon: i32, text: String, warn: bool) -> DetailChip {
    DetailChip { icon, text: text.into(), warn }
}

/* The page's actions in order: (key, button, line under the actions). */
fn details_actions(ui: &HomeWindow, st: &State) -> Vec<(String, DetailAction, String)> {
    let d = &st.details;
    let mut out = Vec::new();
    if d.available {
        out.push(("play".into(), detail_action(0, tr(ui, 413, "Play"), true), String::new()));
    }
    let names: Vec<&str> = d.members.iter().filter(|m| m.member).map(|m| m.name.as_str()).collect();
    out.push(("collections".into(), detail_action(2, tr(ui, 537, "Collections"), false), names.join(", ")));
    if !d.cores.is_empty() {
        let core = if d.core.is_empty() { String::new() } else { format!("{}: {}", tr(ui, 525, "Emulator"), d.core) };
        out.push(("settings".into(), detail_action(3, tr(ui, 524, "Game Settings"), false), core));
    }
    if d.meta("match") == "review" {
        let mut accept = detail_action(6, tr(ui, 562, "Accept Match"), false);
        accept.enabled = !st.busy;
        out.push(("accept".into(), accept, format!("{}: {}", tr(ui, 561, "Check this match"), d.meta("title"))));
        let mut reject = detail_action(7, tr(ui, 563, "Reject Match"), false);
        reject.enabled = !st.busy;
        out.push(("reject".into(), reject, String::new()));
    } else {
        let mut get = if d.meta("match").is_empty() {
            detail_action(4, tr(ui, 559, "Get Metadata"), false)
        } else {
            detail_action(5, tr(ui, 560, "Update Metadata"), false)
        };
        get.enabled = !st.busy && d.available;
        get.busy = st.busy;
        let caption = if st.busy {
            tr(ui, 571, "Getting metadata")
        } else if !d.meta("provider").is_empty() {
            tr_arg(ui, 802, "Source: {0}", source_name(d.meta("provider")))
        } else {
            String::new()
        };
        out.push(("metadata".into(), get, caption));
    }
    let mut search = detail_action(17, tr(ui, 800, "Search Name"), false);
    search.enabled = !st.busy && d.available;
    let searched = if d.meta("search").is_empty() { d.title.clone() } else { d.meta("search").to_owned() };
    out.push(("search".into(), search, tr_arg(ui, 801, "Searched as \"{0}\". Change it if the game is not found or the match is wrong.", &searched)));
    if d.available && d.bios_missing {
        out.push(("bios".into(), detail_action(8, tr(ui, 576, "BIOS Files"), false),
            tr(ui, 583, "BIOS missing for this system")));
    }
    if d.last > 0 {
        let mut reset = detail_action(9, tr(ui, 549, "Reset Play Statistics"), false);
        reset.armed = st.armed == "stats";
        out.push(("stats".into(), reset,
            armed_detail(&st.armed, "stats", tr(ui, 598, "Press again to reset"), String::new())));
    }
    if d.available {
        let mut delete = detail_action(10, tr(ui, 544, "Delete Game"), false);
        delete.armed = st.armed == "delete";
        out.push(("delete".into(), delete, armed_detail(&st.armed, "delete",
            tr(ui, 546, "Press again to delete permanently"),
            tr(ui, 545, "Permanently deletes the game file for every user. Saves are kept."))));
    }
    out
}

/* Warnings first, then players, rating and play time. */
fn details_chips(ui: &HomeWindow, d: &Details) -> Vec<DetailChip> {
    let mut chips = Vec::new();
    if !d.available {
        chips.push(detail_chip(13, tr(ui, 205, "Unavailable"), true));
    }
    if d.meta("match") == "review" {
        chips.push(detail_chip(13, tr(ui, 561, "Check this match"), true));
    }
    if !d.meta("players").is_empty() {
        chips.push(detail_chip(11, d.meta("players").to_owned(), false));
    }
    if let Ok(rating) = d.meta("rating").parse::<u32>() {
        chips.push(detail_chip(1, tr_arg(ui, 789, "{0}%", &rating.min(100).to_string()), false));
    }
    let played = if d.last > 0 {
        format!("{}  •  {}", days_ago(ui, d.last), duration(ui, d.time))
    } else {
        tr(ui, 405, "Never played")
    };
    chips.push(detail_chip(12, played, false));
    chips
}

/* Year, genre and developer (else publisher) on one line. */
fn details_meta(d: &Details) -> String {
    let mut parts: Vec<String> = Vec::new();
    let year: String = d.meta("release").chars().take(4).collect();
    if year.len() == 4 && year.chars().all(|c| c.is_ascii_digit()) {
        parts.push(year);
    }
    for key in ["genre", if d.meta("developer").is_empty() { "publisher" } else { "developer" }] {
        if !d.meta(key).is_empty() {
            parts.push(d.meta(key).to_owned());
        }
    }
    parts.join("  •  ")
}

fn render_details(ui: &HomeWindow) {
    let (items, chips, meta, path) = STATE.with(|st| {
        let st = st.borrow();
        (details_actions(ui, &st), details_chips(ui, &st.details), details_meta(&st.details), st.details.path.clone())
    });
    let count = items.len() as i32;
    let index = ui.get_gen_index().clamp(0, (count - 1).max(0));
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.keys = items.iter().map(|i| i.0.clone()).collect();
        st.actionable = items.iter().map(|i| i.1.enabled).collect();
    });
    /* The focused action explains itself; otherwise the game's file. */
    let (caption, armed, select) = items
        .get(index as usize)
        .map(|(_, a, c)| (if c.is_empty() { path.clone() } else { c.clone() }, a.armed, a.enabled))
        .unwrap_or_default();
    let actions: Vec<DetailAction> = items.into_iter().map(|i| i.1).collect();
    ui.set_details_actions(ModelRc::from(Rc::new(VecModel::from(actions))));
    ui.set_details_chips(ModelRc::from(Rc::new(VecModel::from(chips))));
    ui.set_details_meta(meta.into());
    ui.set_details_caption(caption.into());
    ui.set_details_caption_armed(armed);
    ui.set_gen_index(index);
    ui.set_gen_select(select);
    ui.set_gen_select_label("".into());
}

/* Backdrop: a small, box-blurred copy of the screenshot (or the cover),
 * stretched under the page; cheap to decode and to draw. */
fn blurred(src: &Image) -> Option<SharedPixelBuffer<Rgba8Pixel>> {
    let (mut buf, _) = crate::library::shape_cover(src, 72, 0.0)?;
    let (w, h) = (buf.width() as usize, buf.height() as usize);
    let px = buf.make_mut_slice();
    let mut tmp = px.to_vec();
    const R: usize = 3;
    for _ in 0..2 {
        for horizontal in [true, false] {
            let (outer, inner) = if horizontal { (h, w) } else { (w, h) };
            for o in 0..outer {
                let at = |i: usize| if horizontal { o * w + i } else { i * w + o };
                for i in 0..inner {
                    let (lo, hi) = (i.saturating_sub(R), (i + R).min(inner - 1));
                    let mut acc = [0u32; 4];
                    for j in lo..=hi {
                        let p = px[at(j)];
                        acc[0] += p.r as u32;
                        acc[1] += p.g as u32;
                        acc[2] += p.b as u32;
                        acc[3] += p.a as u32;
                    }
                    let n = (hi - lo + 1) as u32;
                    tmp[at(i)] = Rgba8Pixel {
                        r: (acc[0] / n) as u8, g: (acc[1] / n) as u8, b: (acc[2] / n) as u8, a: (acc[3] / n) as u8,
                    };
                }
            }
            px.copy_from_slice(&tmp);
        }
    }
    Some(buf)
}

/* Left/right: actions; up/down: the description. */
fn details_move(ui: &HomeWindow, action: &str) {
    match action {
        "menu_left" | "menu_right" => {
            let count = STATE.with(|st| st.borrow().keys.len()) as i32;
            let index = ui.get_gen_index() + if action == "menu_left" { -1 } else { 1 };
            if count == 0 || !(0..count).contains(&index) {
                return;
            }
            ui.set_gen_index(index);
            STATE.with(|st| st.borrow_mut().armed.clear());
            render(ui);
        }
        _ => {
            let step = ui.get_details_desc_step() + if action == "menu_up" { -1 } else { 1 };
            ui.set_details_desc_step(step.clamp(0, ui.get_details_desc_steps()));
        }
    }
}

pub(crate) fn render(ui: &HomeWindow) {
    let view = ui.get_settings_view();
    if (58..=62).contains(&view) {
        crate::filesui::render(ui);
        return;
    }
    if view == 65 {
        crate::mediaui::render(ui);
        return;
    }
    if view == crate::webui::VIEW {
        crate::webui::render(ui);
        return;
    }
    if view == crate::findui::COLLECTIONS_VIEW {
        crate::findui::render(ui);
        return;
    }
    if view == 1 || (view == 0 && ui.get_settings_selected_index() == 1) {
        render_services(ui);
        return;
    }
    if view == GAMING_VIEW || (view == 0 && ui.get_settings_selected_index() == 3) {
        render_gaming(ui);
        return;
    }
    if view == 50 {
        render_details(ui);
        return;
    }
    if !(50..=57).contains(&view) && view != 63 && view != 66 {
        return;
    }
    let rows = STATE.with(|st| build_rows(ui, &st.borrow(), view));
    let section = match view {
        50 => STATE.with(|st| st.borrow().details.system_name.clone()),
        51 => tr(ui, 524, "Game Settings"),
        52 => {
            /* First row of OTHER SYSTEMS; none when all are the user's. */
            let mine = crate::library::system_ids();
            let split = rows.iter().position(|r| r.0.starts_with("sys:") && !mine.contains(&r.0[4..].to_owned()))
                .unwrap_or(0) as i32;
            apply_rows_split(ui, rows, tr(ui, 760, "YOUR SYSTEMS"), split, tr(ui, 761, "OTHER SYSTEMS"));
            return;
        }
        53 => STATE.with(|st| {
            let st = st.borrow();
            st.bios.iter().find(|s| s.id == st.bios_system).map(|s| s.name.clone()).unwrap_or_default()
        }),
        54 => tr(ui, 584, "RetroAchievements"),
        55 => tr(ui, 772, "Game Hotkeys"),
        56 => tr(ui, 774, "Performance Overlay"),
        57 => STATE.with(|st| st.borrow().details.title.clone()),
        63 => tr(ui, 732, "STORAGE & SOFTWARE"),
        _ => tr(ui, 727, "SERVICES"),
    };
    apply_rows(ui, rows, section);
}

/* Show rows on the generic page: (key, row, actionable). */
pub(crate) fn apply_rows(ui: &HomeWindow, rows: Vec<(String, GenRow, bool)>, section: String) {
    apply_rows_split(ui, rows, section, 0, String::new());
}

/* The same with a second section from row `split` (0 = none). */
pub(crate) fn apply_rows_split(ui: &HomeWindow, rows: Vec<(String, GenRow, bool)>, section: String, split: i32,
    split_label: String) {
    let count = rows.len() as i32;
    let index = ui.get_gen_index().clamp(0, (count - 1).max(0));
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.keys = rows.iter().map(|r| r.0.clone()).collect();
        st.actionable = rows.iter().map(|r| r.2).collect();
    });
    let select = rows.get(index as usize).map(|r| r.2).unwrap_or(false);
    /* The row waiting for its confirming press shows the armed style. */
    let armed = armed();
    let rows: Vec<GenRow> = rows
        .into_iter()
        .map(|(key, mut row, _)| {
            row.armed = !armed.is_empty() && key == armed;
            row
        })
        .collect();
    ui.set_gen_rows(ModelRc::from(Rc::new(VecModel::from(rows))));
    ui.set_gen_index(index);
    ui.set_gen_select(select);
    ui.set_gen_select_label("".into());
    ui.set_gen_split(split);
    ui.set_gen_split_label(split_label.into());
    ui.set_gen_scroll(if split > 0 {
        sectioned_scroll(ui, index, count, ui.get_gen_scroll(), (split, 0, 0))
    } else {
        guarded_scroll_offset(index, count, ui.get_settings_list_visible_rows(), ui.get_gen_scroll())
    });
    ui.set_gen_section(section.into());
}

pub(crate) fn show(ui: &HomeWindow, view: i32) {
    STATE.with(|st| st.borrow_mut().armed.clear());
    ui.set_gen_index(0);
    ui.set_gen_scroll(0);
    ui.set_gen_notice("".into());
    navigate_settings_view(ui, view);
    render(ui);
}

/* Keys of the generic rows on screen, in order. */
pub(crate) fn gen_keys() -> Vec<String> {
    STATE.with(|st| st.borrow().keys.clone())
}

pub(crate) fn focused_key(ui: &HomeWindow) -> String {
    let i = ui.get_gen_index().max(0) as usize;
    STATE.with(|st| st.borrow().keys.get(i).cloned().unwrap_or_default())
}

/* ---------------------------------------------------------------- */
/* Game Details shell                                               */
/* ---------------------------------------------------------------- */

/* Fetch everything the Details page aggregates, off the UI thread. */
/* open: the page appears only once everything is ready (user request
 * 2026-10-10: it built itself piece by piece). */
fn load_details(ui: &HomeWindow, id: String) {
    load_details_then(ui, id, false);
}

fn load_details_then(ui: &HomeWindow, id: String, open: bool) {
    let weak = ui.as_weak();
    /* The hero cover is decoded at its on-screen height (physical px). */
    let scale = ui.window().scale_factor();
    let target = ((ui.window().size().height as f32 * 0.62) as u32).max(64).div_ceil(32) * 32;
    let radius = (12.0 * scale).max(target as f32 * 0.04);
    thread::spawn(move || {
        let mut d = Details::default();
        parse_details(&library(&format!("DETAILS\t{id}"), true), &mut d);
        if d.id.is_empty() {
            return;
        }
        parse_game_settings(&emulation(&format!("GAME_SETTINGS\t{}\t{}", d.id, d.system), true), &mut d);
        let bios = run_tool("/usr/bin/nuubos-biosctl", &["status", &d.system], None);
        d.bios_missing = parse_bios(&bios).0.iter().any(|s| s.state == "missing" || s.state == "invalid");
        let load = |p: &str| if p.is_empty() { None } else { Image::load_from_path(std::path::Path::new(p)).ok() };
        let cover_src = load(&d.cover);
        let cover = cover_src.as_ref().and_then(|i| crate::library::shape_cover(i, target, radius)).map(|c| c.0);
        let backdrop = load(d.meta("screenshot")).or(cover_src).as_ref().and_then(blurred);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            if (!open && !ACTIVE.load(Ordering::SeqCst)) || STATE.with(|st| st.borrow().details.id != d.id) {
                return;
            }
            if open {
                /* A newer request (another game, or the user moved on to a
                 * game or Settings meanwhile) wins. */
                if ui.get_settings_open() || crate::library::GAME_RUNNING.load(Ordering::SeqCst) {
                    return;
                }
                ACTIVE.store(true, Ordering::SeqCst);
                ui.set_details_active(true);
                ui.set_details_meta("".into());
                ui.set_details_desc_step(0);
            }
            let title = if d.meta("title").is_empty() { d.title.clone() } else { d.meta("title").to_owned() };
            ui.set_details_title(title.into());
            ui.set_details_subtitle(d.system_name.clone().into());
            ui.set_details_description(d.meta("description").replace("\\n", "\n").into());
            if let Some(c) = cover {
                ui.set_details_cover(Image::from_rgba8_premultiplied(c));
                ui.set_details_has_cover(true);
            }
            ui.set_details_has_backdrop(backdrop.is_some());
            ui.set_details_backdrop(backdrop.map(Image::from_rgba8_premultiplied).unwrap_or_default());
            STATE.with(|st| st.borrow_mut().details = d);
            if open {
                show(&ui, 50);
                ui.set_settings_open(true);
                write_ui_context("settings");
            }
            render(&ui);
        });
    });
}

/* Home: the Details action on a game card. */
pub fn open_details(ui: &HomeWindow, id: &str, title: &str, cover: Option<Image>) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.details = Details { id: id.to_owned(), title: title.to_owned(), available: true, ..Default::default() };
        st.busy = false;
    });
    /* The Home card's cover stands in until the hero is decoded. */
    ui.set_details_has_cover(cover.is_some());
    ui.set_details_cover(cover.unwrap_or_default());
    ui.set_details_has_backdrop(false);
    ui.set_details_backdrop(Image::default());
    load_details_then(ui, id.to_owned(), true);
}

/* Home: a long press on a game card. The rows come from emud STATES. */
pub fn open_launch_menu(ui: &HomeWindow, id: &str, title: &str, cover: Option<Image>) {
    ACTIVE.store(true, Ordering::SeqCst);
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.details = Details { id: id.to_owned(), title: title.to_owned(), available: true, ..Default::default() };
        st.states.clear();
        st.busy = false;
    });
    ui.set_details_active(true);
    ui.set_details_title(title.into());
    ui.set_details_subtitle("".into());
    ui.set_details_description("".into());
    ui.set_details_has_cover(cover.is_some());
    ui.set_details_cover(cover.unwrap_or_default());
    show(ui, 57);
    ui.set_settings_open(true);
    write_ui_context("settings");
    let weak = ui.as_weak();
    let id = id.to_owned();
    thread::spawn(move || {
        let reply = emulation(&format!("STATES\t{id}"), true);
        let states: Vec<(i32, i64, String)> = reply
            .lines()
            .filter_map(|l| l.strip_prefix("state="))
            .filter_map(|v| {
                let f: Vec<&str> = v.split('\t').collect();
                Some((f.first()?.parse().ok()?, f.get(1)?.parse().ok()?, f.get(2).unwrap_or(&"").to_string()))
            })
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            if !ACTIVE.load(Ordering::SeqCst) || ui.get_settings_view() != 57 {
                return;
            }
            STATE.with(|st| st.borrow_mut().states = states);
            render(&ui);
        });
    });
}

pub fn leave_details(ui: &HomeWindow) {
    ACTIVE.store(false, Ordering::SeqCst);
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_details_active(false);
    ui.set_details_cover(Image::default());
    ui.set_details_backdrop(Image::default());
    ui.set_details_has_backdrop(false);
    ui.set_details_actions(ModelRc::default());
    ui.set_details_chips(ModelRc::default());
    ui.set_gen_rows(ModelRc::default());
    write_ui_context("home");
}

fn reload(ui: &HomeWindow) {
    let id = STATE.with(|st| st.borrow().details.id.clone());
    load_details(ui, id);
}

pub(crate) fn notice(ui: &HomeWindow, text: String) {
    ui.set_gen_notice(text.into());
}

fn details_command(ui: &HomeWindow, line: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let _ = library(&line, false);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                reload(&ui);
            }
        });
    });
}

/* Scraping one game is a job; its Live Notification shows progress. */
fn scrape_game(ui: &HomeWindow, id: String, force: bool) {
    STATE.with(|st| st.borrow_mut().busy = true);
    render(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut args = vec!["run", "scrape-game", id.as_str()];
        if force {
            args.push("force");
        }
        let out = run_tool("/usr/bin/nuubos-jobctl", &args, None);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().busy = false);
            let text = scrape_outcome(&ui, out.trim());
            notice(&ui, text);
            reload(&ui);
        });
    });
}

fn scrape_outcome(ui: &HomeWindow, out: &str) -> String {
    if out == "scraped" || out == "review" || out.starts_with(|c: char| c.is_ascii_digit()) {
        return tr(ui, 572, "Metadata updated");
    }
    let reason = out.rsplit(' ').next().unwrap_or("");
    match reason {
        "not-found" => tr(ui, 573, "No metadata found for this game"),
        "quota" => tr(ui, 574, "Daily metadata limit reached, try again later"),
        "auth" => tr(ui, 575, "Check the account"),
        "busy" | "service" => tr(ui, 790, "The metadata service is busy, try again later"),
        "unavailable" => tr(ui, 570, "The metadata service is not available in this build"),
        "network" => tr(ui, 494, "Check the network connection"),
        "cancelled" => tr(ui, 596, "Cancelled"),
        "complete" | "matched" => tr(ui, 572, "Metadata updated"),
        _ => tr(ui, 93, "Failed"),
    }
}

fn open_collections(ui: &HomeWindow) {
    let d = STATE.with(|st| st.borrow().details.clone());
    let mut options: Vec<(String, String)> = d
        .members
        .iter()
        .map(|m| (m.id.clone(), format!("{}  •  {}", m.name, if m.member { tr(ui, 222, "On") } else { tr(ui, 54, "Off") })))
        .collect();
    options.push(("new".into(), tr(ui, 538, "New Collection")));
    open_settings_choice(ui, "game-collection", &tr(ui, 537, "Collections"), options, "");
}

fn open_core_choice(ui: &HomeWindow) {
    let d = STATE.with(|st| st.borrow().details.clone());
    let mut options = vec![(String::new(), default_label(ui, &d.core_default))];
    options.extend(d.cores.iter().map(|c| (c.clone(), c.clone())));
    open_settings_choice(ui, "game-core", &tr(ui, 525, "Emulator"), options, &d.core_override);
}

fn open_aspect_choice(ui: &HomeWindow) {
    let d = STATE.with(|st| st.borrow().details.clone());
    let mut options = vec![(String::new(), default_label(ui, &tr(ui, 531, "Integer Scaling")))];
    for v in ["core", "4:3", "16:9", "full", "integer"] {
        options.push((v.to_owned(), aspect_label(ui, v)));
    }
    open_settings_choice(ui, "game-aspect", &tr(ui, 526, "Aspect Ratio"), options, &d.aspect);
}

fn open_filter_choice(ui: &HomeWindow) {
    let d = STATE.with(|st| st.borrow().details.clone());
    let mut options = vec![(String::new(), default_label(ui, &filter_label(ui, &d.filter_default)))];
    for v in ["sharp", "smooth", "pixel"] {
        options.push((v.to_owned(), filter_label(ui, v)));
    }
    open_settings_choice(ui, "game-filter", &tr(ui, 527, "Video Filter"), options, &d.filter);
}

fn game_set(ui: &HomeWindow, key: &str, value: String) {
    let (id, system) = STATE.with(|st| {
        let st = st.borrow();
        (st.details.id.clone(), st.details.system.clone())
    });
    let line = format!("GAME_SET\t{id}\t{system}\t{key}\t{value}");
    let weak = ui.as_weak();
    thread::spawn(move || {
        let _ = emulation(&line, false);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                reload(&ui);
            }
        });
    });
}

/* A choice made in a dropdown opened by this module. */
pub fn apply_choice(ui: &HomeWindow, context: &str, value: String) {
    match context {
        "game-core" => game_set(ui, "core", value),
        "game-aspect" => game_set(ui, "aspect", value),
        "game-filter" => game_set(ui, "filter", value),
        "emu-slots" => emu_set(ui, "slots", value),
        "game-fill" => start_bulk(ui, value),
        "game-source" => tool_then(ui, "/usr/bin/nuubos-scraper", vec!["provider".into(), "set".into(), value],
            None, |ui, _| refresh_gaming(ui)),
        c if c.starts_with("emu-hotkey:") => emu_set(ui, &format!("hotkey_{}", &c[11..]), value),
        "game-collection" => {
            if value == "new" {
                open_system_keyboard(ui, &tr(ui, 539, "Collection name"), KEYBOARD_COLLECTION, 50, "text", "");
                return;
            }
            let (id, member) = STATE.with(|st| {
                let st = st.borrow();
                (st.details.id.clone(), st.details.members.iter().any(|m| m.id == value && m.member))
            });
            let verb = if member { "COLLECTION_REMOVE" } else { "COLLECTION_ADD" };
            details_command(ui, format!("{verb}\t{value}\t{id}"));
        }
        _ => {}
    }
}

/* The armed (waiting for its confirming press) row key. */
pub(crate) fn armed() -> String {
    STATE.with(|st| st.borrow().armed.clone())
}

pub(crate) fn arm_or(ui: &HomeWindow, key: &str) -> bool {
    let armed = STATE.with(|st| st.borrow().armed == key);
    if armed {
        STATE.with(|st| st.borrow_mut().armed.clear());
        return true;
    }
    STATE.with(|st| st.borrow_mut().armed = key.to_owned());
    render(ui);
    false
}

fn details_confirm(ui: &HomeWindow, key: &str) {
    let d = STATE.with(|st| st.borrow().details.clone());
    match key {
        "play" => {
            let id = d.id.clone();
            leave_details(ui);
            crate::library::launch_by_id(ui, &id, d.available);
        }
        "collections" => open_collections(ui),
        "settings" => show(ui, 51),
        "bios" => {
            STATE.with(|st| st.borrow_mut().bios_system = d.system.clone());
            leave_details(ui);
            open_bios(ui);
        }
        "metadata" => scrape_game(ui, d.id.clone(), !d.meta("match").is_empty()),
        "search" => {
            let current = if d.meta("search").is_empty() { d.title.clone() } else { d.meta("search").to_owned() };
            open_system_keyboard(ui, &tr(ui, 800, "Search Name"), KEYBOARD_SEARCH, 50, "text", &current);
        }
        "accept" | "reject" => {
            let verb = key.to_owned();
            let id = d.id.clone();
            STATE.with(|st| st.borrow_mut().busy = true);
            render(ui);
            let weak = ui.as_weak();
            thread::spawn(move || {
                let _ = run_tool("/usr/bin/nuubos-scraper", &[&verb, &id], None);
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        STATE.with(|st| st.borrow_mut().busy = false);
                        reload(&ui);
                    }
                });
            });
        }
        "stats" => {
            if arm_or(ui, "stats") {
                details_command(ui, format!("STATS_RESET\t{}", d.id));
            }
        }
        "delete" => {
            if !arm_or(ui, "delete") {
                return;
            }
            let id = d.id.clone();
            let weak = ui.as_weak();
            thread::spawn(move || {
                let ok = library(&format!("DELETE\t{id}\tCONFIRM"), false).starts_with("OK");
                let _ = slint::invoke_from_event_loop(move || {
                    let Some(ui) = weak.upgrade() else { return };
                    if ok {
                        leave_details(&ui);
                        crate::library::notice(&ui, tr(&ui, 547, "Game deleted"));
                    } else {
                        notice(&ui, tr(&ui, 548, "The game could not be deleted"));
                        render(&ui);
                    }
                });
            });
        }
        k if k.starts_with("start:") => {
            let id = d.id.clone();
            let mode = k[6..].to_owned();
            leave_details(ui);
            crate::library::launch_with_mode(ui, &id, true, &mode);
        }
        "core" => open_core_choice(ui),
        "aspect" => open_aspect_choice(ui),
        "filter" => open_filter_choice(ui),
        "reset" => {
            if arm_or(ui, "reset") {
                let line = format!("GAME_RESET\t{}", d.id);
                let weak = ui.as_weak();
                thread::spawn(move || {
                    let _ = emulation(&line, false);
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            reload(&ui);
                        }
                    });
                });
            }
        }
        _ => {}
    }
}

pub(crate) fn move_selection(ui: &HomeWindow, action: &str) {
    let count = STATE.with(|st| st.borrow().keys.len()) as i32;
    if count == 0 {
        return;
    }
    ui.set_gen_index(move_model_selection(ui.get_gen_index(), count, if action == "menu_up" { -1 } else { 1 }));
    STATE.with(|st| st.borrow_mut().armed.clear());
    render(ui);
}

/* Input while the Details shell is open (views 50, 51 and its keyboard
 * and dropdowns). */
pub fn handle_details_action(ui: &HomeWindow, action: &str, settings_active: &Arc<AtomicBool>) {
    if ui.get_settings_choice_open() || ui.get_settings_view() == 6 {
        handle_settings_action(ui, action, settings_active);
        return;
    }
    play_ui_sound(action);
    let page = ui.get_settings_view() == 50;
    match action {
        "menu_up" | "menu_down" | "menu_left" | "menu_right" if page => details_move(ui, action),
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => {
            if ui.get_settings_view() == 51 {
                show(ui, 50);
                /* Back on the action that opened Game Settings. */
                if let Some(i) = STATE.with(|st| st.borrow().keys.iter().position(|k| k == "settings")) {
                    ui.set_gen_index(i as i32);
                    render(ui);
                }
            } else {
                leave_details(ui);
            }
        }
        "menu_confirm" => {
            let i = ui.get_gen_index().max(0) as usize;
            if page && !STATE.with(|st| st.borrow().actionable.get(i).copied().unwrap_or(false)) {
                return;
            }
            let key = focused_key(ui);
            details_confirm(ui, &key);
        }
        _ => {}
    }
}

/* ---------------------------------------------------------------- */
/* Settings → Gaming, pages 52-54, 63, 66                           */
/* ---------------------------------------------------------------- */

/* Settings → Gaming: RetroAchievements (USER), then LIBRARY (DEVICE) with
 * the ScreenScraper account and bulk scraping on the page itself and BIOS
 * Files (user request 2026-10-08: no Game Metadata sub-page, no Hidden
 * Games). */
pub const GAMING_VIEW: i32 = 68;

/* Metadata sources (nuubos-scraper providers), product names. */
const SOURCES: [&str; 3] = ["screenscraper", "thegamesdb", "libretro"];

fn source_name(id: &str) -> &'static str {
    match id {
        "thegamesdb" => "TheGamesDB",
        "libretro" => "libretro",
        _ => "ScreenScraper",
    }
}

fn source_detail(ui: &HomeWindow, id: &str) -> String {
    match id {
        "thegamesdb" => tr(ui, 794, "Covers, screenshots and descriptions in English. A free private API key gives more requests."),
        "libretro" => tr(ui, 795, "Covers and screenshots only. No account needed."),
        _ => tr(ui, 793, "Covers, screenshots, descriptions and ratings in your language. A free ScreenScraper account gives more requests."),
    }
}

fn gaming_rows(ui: &HomeWindow, st: &State) -> Vec<(String, GenRow, bool)> {
    let mut rows = Vec::new();
    let on = |k: &str| emu_get(st, k) == "1";
    rows.push(("ra".into(), nav(tr(ui, 584, "RetroAchievements"),
        if st.ra_user.is_empty() { tr(ui, 566, "Not signed in") } else { st.ra_user.clone() }), true));
    /* The user's emulation settings (nuubos-emud). */
    rows.push(("boot_screens".into(), toggle(tr(ui, 767, "Console Boot Screens"), on("boot_screens"),
        tr(ui, 768, "Shows the console's start-up logo when its BIOS is available")), true));
    rows.push(("slots".into(), nav(tr(ui, 769, "Save State Slots"), emu_get(st, "slots")), true));
    rows.push(("slot_rotation".into(), toggle(tr(ui, 770, "Rotate Save Slots"), on("slot_rotation"),
        tr(ui, 771, "Each save goes to the next slot, then starts again from slot 0")), true));
    rows.push(("hotkeys".into(), nav(tr(ui, 772, "Game Hotkeys"), String::new()), true));
    rows.push(("overlay".into(), nav(tr(ui, 774, "Performance Overlay"),
        if on("overlay") { tr(ui, 222, "On") } else { tr(ui, 54, "Off") }), true));
    rows.push(("bezels".into(), toggle(tr(ui, 775, "Bezels"), on("bezels"),
        tr(ui, 776, "Frames the game when it does not fill the screen")), true));
    rows.push(("crt".into(), toggle(tr(ui, 777, "CRT Effect"), on("crt"),
        tr(ui, 778, "Scanlines for consoles played on TVs")), true));
    /* LIBRARY (DEVICE): the library scan, the metadata source, its optional
     * account or key, then the two scraping runs. */
    let scanning = ui.get_library_scanning();
    rows.push(("scan".into(), row(tr(ui, 804, "Scan for New Games"),
        if scanning { tr(ui, 410, "Searching for games…") } else { String::new() },
        tr(ui, 805, "Finds games copied to the SD card since the last scan")), !scanning));
    let mut source = nav(tr(ui, 792, "Metadata Source"), source_name(&st.ss_provider).to_owned());
    source.detail = source_detail(ui, &st.ss_provider).into();
    rows.push(("ss-source".into(), source, true));
    match st.ss_provider.as_str() {
        "screenscraper" => if st.ss_user.is_empty() {
            rows.push(("ss-signin".into(), row(tr(ui, 565, "ScreenScraper Account"), tr(ui, 566, "Not signed in"),
                tr(ui, 803, "Optional: more requests per day")), true));
        } else {
            rows.push(("ss-signout".into(), row(tr(ui, 565, "ScreenScraper Account"), st.ss_user.clone(),
                armed_detail(&st.armed, "ss-signout", tr(ui, 191, "Press again to confirm"), tr(ui, 567, "Sign Out"))), true));
        },
        "thegamesdb" => rows.push(("tgdb-key".into(), row(tr(ui, 796, "TheGamesDB API Key"),
            if st.ss_apikey.is_empty() { tr(ui, 797, "Not set") } else { format!("•••• {}", st.ss_apikey) },
            tr(ui, 803, "Optional: more requests per day")), true)),
        _ => {}
    }
    let busy = if st.ss_busy { tr(ui, 571, "Getting metadata") } else { String::new() };
    let mut bulk = row(tr(ui, 568, "Get Metadata for All Games"), busy.clone(),
        if st.ss_available {
            tr(ui, 569, "Uses the Internet only when you ask. Games without metadata keep their file name.")
        } else {
            tr(ui, 570, "The metadata service is not available")
        });
    bulk.enabled = st.ss_available;
    rows.push(("bulk".into(), bulk, st.ss_available && !st.ss_busy));
    let mut fill = row(tr(ui, 798, "Fill Missing Data"), String::new(),
        tr(ui, 799, "Adds only what is missing, such as covers or descriptions. What is already there is kept."));
    fill.enabled = st.ss_available;
    rows.push(("fill".into(), fill, st.ss_available && !st.ss_busy));
    /* Text in the user's language (ScreenScraper/TheGamesDB keep one per
     * language; libretro has no text). */
    if st.ss_provider != "libretro" {
        let mut lang = row(tr(ui, 868, "Metadata in Your Language"), String::new(),
            tr(ui, 869, "Gets descriptions and genres in your language; the languages already downloaded stay saved"));
        lang.enabled = st.ss_available;
        rows.push(("lang".into(), lang, st.ss_available && !st.ss_busy));
    }
    let mut bios = nav(tr(ui, 576, "BIOS Files"), String::new());
    bios.detail = tr(ui, 581, "Copy BIOS files to the bios folder on the SD card. nuubOS never includes them.").into();
    rows.push(("bios".into(), bios, true));
    rows
}

/* A library scrape job (all | missing[:seconds] | lang). */
fn start_bulk(ui: &HomeWindow, scope: String) {
    STATE.with(|st| st.borrow_mut().ss_busy = true);
    render_gaming(ui);
    tool_then(ui, "/usr/bin/nuubos-jobctl", vec!["run".into(), "scrape-bulk".into(), scope],
        None, |ui, out| {
            STATE.with(|st| st.borrow_mut().ss_busy = false);
            let text = scrape_outcome(ui, out.trim());
            notice(ui, text);
            render_gaming(ui);
        });
}

fn render_gaming(ui: &HomeWindow) {
    let (rows, select) = STATE.with(|st| {
        let mut st = st.borrow_mut();
        let rows = gaming_rows(ui, &st);
        st.gaming_keys = rows.iter().map(|r| r.0.clone()).collect();
        /* LIBRARY (DEVICE) starts after the user's rows. */
        let split = rows.iter().position(|r| r.0 == "scan")
            .unwrap_or(rows.len()) as i32;
        ui.set_gaming_split(split);
        let select = rows.get(ui.get_gaming_index().max(0) as usize).map(|r| r.2).unwrap_or(false);
        let armed = st.armed.clone();
        let rows = rows.into_iter()
            .map(|(key, mut row, _)| {
                row.armed = !armed.is_empty() && key == armed;
                row
            })
            .collect::<Vec<GenRow>>();
        (rows, select)
    });
    ui.set_gaming_rows(ModelRc::from(Rc::new(VecModel::from(rows))));
    ui.set_gaming_select(select);
}

/* Entering or previewing Gaming: both accounts from their services. */
pub fn refresh_gaming(ui: &HomeWindow) {
    STATE.with(|st| st.borrow_mut().armed.clear());
    ui.set_gen_notice("".into());
    render_gaming(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let user = active_user();
        let ra = if user.is_empty() { String::new() } else { run_tool("/usr/bin/nuubos-achievementsctl", &["status", &user], None) };
        let ss = run_tool("/usr/bin/nuubos-scraper", &["account", "status"], None);
        let emu = pairs(&emulation("SETTINGS", true));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.emu = emu;
                st.ra_user = kv(&ra, "user").to_owned();
                st.ra_enabled = kv(&ra, "enabled") == "1";
                st.ra_hardcore = kv(&ra, "hardcore") == "1";
                st.ss_user = kv(&ss, "user").to_owned();
                st.ss_available = kv(&ss, "available") == "1";
                st.ss_provider = kv(&ss, "provider").to_owned();
                st.ss_apikey = kv(&ss, "apikey").to_owned();
            });
            render_gaming(&ui);
        });
    });
}

/* Input on Settings → Gaming (from handle_settings_action). */
pub fn handle_gaming(ui: &HomeWindow, action: &str) {
    match action {
        "menu_up" | "menu_down" => {
            let count = STATE.with(|st| st.borrow().gaming_keys.len()).max(1) as i32;
            ui.set_gaming_index(move_model_selection(ui.get_gaming_index(), count, if action == "menu_up" { -1 } else { 1 }));
            /* Moving off an armed row cancels the pending confirmation. */
            STATE.with(|st| st.borrow_mut().armed.clear());
            render_gaming(ui);
        }
        "menu_confirm" => {
            let key = STATE.with(|st| st.borrow().gaming_keys.get(ui.get_gaming_index().max(0) as usize).cloned())
                .unwrap_or_default();
            if key != "ss-signout" {
                STATE.with(|st| st.borrow_mut().armed.clear());
            }
            match key.as_str() {
                "ra" => open_achievements(ui),
                "bios" => open_bios(ui),
                "boot_screens" | "slot_rotation" | "bezels" | "crt" => {
                    let on = STATE.with(|st| emu_get(&st.borrow(), &key) == "1");
                    emu_set(ui, &key, if on { "0" } else { "1" }.to_owned());
                }
                "slots" => {
                    let current = STATE.with(|st| emu_get(&st.borrow(), "slots"));
                    let options = ["3", "5", "10", "20", "50", "99"].iter().map(|n| (n.to_string(), n.to_string())).collect();
                    open_settings_choice(ui, "emu-slots", &tr(ui, 769, "Save State Slots"), options, &current);
                }
                "hotkeys" => {
                    set_back(ui, GAMING_VIEW);
                    show(ui, 55);
                }
                "overlay" => {
                    set_back(ui, GAMING_VIEW);
                    show(ui, 56);
                }
                "ss-signin" => {
                    open_system_keyboard(ui, &tr(ui, 285, "Username"), KEYBOARD_SS_NAME, GAMING_VIEW, "text", "");
                }
                "ss-signout" => {
                    if arm_or(ui, "ss-signout") {
                        tool_then(ui, "/usr/bin/nuubos-scraper", vec!["account".into(), "clear".into()],
                            None, |ui, _| refresh_gaming(ui));
                    }
                }
                /* Fill Missing Data: optionally skip the games tried
                 * recently (user request 2026-10-10). */
                "fill" if !STATE.with(|st| st.borrow().ss_busy) => {
                    let options = vec![
                        ("missing".to_owned(), tr(ui, 870, "Every game with missing data")),
                        ("missing:3600".to_owned(), tr(ui, 871, "Not tried in the last hour")),
                        ("missing:86400".to_owned(), tr(ui, 872, "Not tried in the last day")),
                        ("missing:604800".to_owned(), tr(ui, 873, "Not tried in the last week")),
                        ("missing:2592000".to_owned(), tr(ui, 874, "Not tried in the last month")),
                    ];
                    open_settings_choice(ui, "game-fill", &tr(ui, 798, "Fill Missing Data"), options, "");
                }
                "bulk" | "lang" if !STATE.with(|st| st.borrow().ss_busy) => {
                    start_bulk(ui, if key == "lang" { "lang" } else { "all" }.to_owned());
                }
                "scan" if !ui.get_library_scanning() => {
                    STATE.with(|st| st.borrow_mut().lib_scan = 1);
                    thread::spawn(|| { let _ = library("SCAN", false); });
                }
                "ss-source" => {
                    let current = STATE.with(|st| st.borrow().ss_provider.clone());
                    let options = SOURCES.iter().map(|id| (id.to_string(), source_name(id).to_owned())).collect();
                    open_settings_choice(ui, "game-source", &tr(ui, 792, "Metadata Source"), options, &current);
                }
                "tgdb-key" => {
                    open_system_keyboard(ui, &tr(ui, 796, "TheGamesDB API Key"), KEYBOARD_TGDB_KEY, GAMING_VIEW, "text", "");
                }
                _ => {}
            }
        }
        _ => {}
    }
}

/* Every libraryd snapshot: the Scan row follows `scanning`, and the end of a
 * scan the user asked for reports how many games it found. */
pub fn library_scan_state(ui: &HomeWindow, scanning: bool, found: usize) {
    let done = STATE.with(|st| {
        let mut st = st.borrow_mut();
        match (st.lib_scan, scanning) {
            (1, true) => st.lib_scan = 2,
            (2, false) => { st.lib_scan = 0; return true; }
            _ => {}
        }
        false
    });
    if ui.get_settings_view() != GAMING_VIEW {
        return;
    }
    if done {
        notice(ui, if found == 0 { tr(ui, 807, "No new games found") }
            else { tr_arg(ui, 806, "New games found: {0}", &found.to_string()) });
    }
    render_gaming(ui);
}

/* SET one emulation setting, then show the stored values again. */
fn emu_set(ui: &HomeWindow, key: &str, value: String) {
    let line = format!("SET\t{key}\t{value}");
    let weak = ui.as_weak();
    thread::spawn(move || {
        let _ = emulation(&line, false);
        let emu = pairs(&emulation("SETTINGS", true));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().emu = emu);
            if ui.get_settings_view() == GAMING_VIEW {
                render_gaming(&ui);
            } else {
                render(&ui);
            }
        });
    });
}

/* Back from these pages returns to their category page. */
fn set_back(_ui: &HomeWindow, category: i32) {
    STATE.with(|st| st.borrow_mut().back = category);
}

pub fn open_bios(ui: &HomeWindow) {
    set_back(ui, GAMING_VIEW);
    if !ui.get_settings_open() {
        ui.set_settings_selected_index(3);
        ui.set_settings_open(true);
        write_ui_context("settings");
    }
    let target = STATE.with(|st| st.borrow().bios_system.clone());
    show(ui, 52);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let (systems, files) = parse_bios(&run_tool("/usr/bin/nuubos-biosctl", &["status"], None));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let open_system = STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.bios = systems;
                st.bios_files = files;
                !target.is_empty() && st.bios.iter().any(|s| s.id == target)
            });
            if open_system {
                show(&ui, 53);
            } else {
                render(&ui);
            }
        });
    });
}

pub fn open_achievements(ui: &HomeWindow) {
    set_back(ui, GAMING_VIEW);
    show(ui, 54);
    refresh_achievements(ui);
}

fn refresh_achievements(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let user = active_user();
        let out = if user.is_empty() { String::new() } else { run_tool("/usr/bin/nuubos-achievementsctl", &["status", &user], None) };
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.ra_user = kv(&out, "user").to_owned();
                st.ra_enabled = kv(&out, "enabled") == "1";
                st.ra_hardcore = kv(&out, "hardcore") == "1";
                st.busy = false;
            });
            render(&ui);
        });
    });
}

/* ---------------------------------------------------------------- */
/* Connectivity → SERVICES                                          */
/* ---------------------------------------------------------------- */

/* First row of the SERVICES section on the Connectivity page. */
pub const SERVICES_FIRST: i32 = 6;

fn service_rows(ui: &HomeWindow, st: &State) -> Vec<(String, GenRow, bool)> {
    let get = |k: &str| st.remote.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
    let address = get("address");
    let mut rows = Vec::new();
    rows.push(("svc:ssh".into(), toggle(tr(ui, 600, "SSH, SCP and SFTP"), get("ssh") == "1",
        if address.is_empty() { tr(ui, 601, "Sign in as root with the device password") } else { format!("root@{address}") }), true));
    rows.push(("svc:smb".into(), toggle(tr(ui, 602, "File Sharing (SMB)"), get("smb") == "1",
        if address.is_empty() { String::new() } else { format!("\\\\{address}\\nuubOS") }), true));
    rows.push(("svc:web".into(), toggle(tr(ui, 603, "Web Administration"), get("web") == "1",
        if address.is_empty() { String::new() } else { format!("http://{address}") }), true));
    rows.push(("password".into(), row(tr(ui, 172, "Password"),
        if st.remote_password.is_empty() { "••••-••••-••••".into() } else { st.remote_password.clone() },
        if st.remote_password.is_empty() { tr(ui, 608, "Press to show") }
        else { tr(ui, 604, "Only on networks you trust. Never exposed to the Internet.") }), true));
    rows.push(("regen".into(), row(tr(ui, 606, "Regenerate Password"), String::new(),
        armed_detail(&st.armed, "regen", tr(ui, 191, "Press again to confirm"),
            tr(ui, 607, "The old password stops working"))), true));
    let sync = |k: &str| st.sync.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
    rows.push(("open-sync".into(), nav(tr(ui, 669, "Syncthing"),
        if sync("enabled") == "1" { tr(ui, 222, "On") } else { tr(ui, 54, "Off") }),
        true));
    if let Some(r) = rows.last_mut() {
        r.1.detail = if sync("failed") == "1" { tr(ui, 677, "Syncthing stopped unexpectedly") }
            else { tr(ui, 670, "Keeps your saves in sync with your other devices. It is not a backup.") }.into();
    }
    rows
}

fn render_services(ui: &HomeWindow) {
    let rows = STATE.with(|st| {
        let mut st = st.borrow_mut();
        let rows = service_rows(ui, &st);
        st.service_keys = rows.iter().map(|r| r.0.clone()).collect();
        let armed = st.armed.clone();
        rows.into_iter()
            .map(|(key, mut row, _)| {
                row.armed = !armed.is_empty() && key == armed;
                row
            })
            .collect::<Vec<GenRow>>()
    });
    ui.set_connectivity_services(ModelRc::from(Rc::new(VecModel::from(rows))));
}

/* Number of SERVICES rows (Connectivity's row count is SERVICES_FIRST + this). */
pub fn service_count() -> i32 {
    STATE.with(|st| st.borrow().service_keys.len()) as i32
}

/* Entering or previewing Connectivity: the password is hidden again. */
pub fn reset_services(ui: &HomeWindow) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.remote_password.clear();
        st.armed.clear();
    });
    refresh_services(ui);
}

pub fn refresh_services(ui: &HomeWindow) {
    if ui.get_connectivity_services().row_count() == 0 {
        render_services(ui);
    }
    let weak = ui.as_weak();
    thread::spawn(move || {
        let pairs = |out: String| -> Vec<(String, String)> {
            out.lines().filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_owned(), v.to_owned()))).collect()
        };
        let remote = pairs(run_tool("/usr/sbin/nuubos-remotectl", &["status"], None));
        let sync = pairs(run_tool("/usr/bin/nuubos-syncctl", &["status"], None));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.remote = remote;
                st.sync = sync;
            });
            render_services(&ui);
        });
    });
}

/* Moving off an armed row cancels the pending confirmation. */
pub fn services_disarm(ui: &HomeWindow) {
    if STATE.with(|st| std::mem::take(&mut st.borrow_mut().armed)).is_empty() {
        return;
    }
    render_services(ui);
}

pub fn services_confirm(ui: &HomeWindow, index: i32) {
    let key = STATE.with(|st| st.borrow().service_keys.get((index - SERVICES_FIRST).max(0) as usize).cloned())
        .unwrap_or_default();
    if key != "regen" {
        STATE.with(|st| st.borrow_mut().armed.clear());
    }
    match key.as_str() {
        k if k.starts_with("svc:") => {
            let svc = k[4..].to_owned();
            let on = STATE.with(|st| st.borrow().remote.iter().any(|(key, v)| *key == svc && v == "1"));
            tool_then(ui, "/usr/sbin/nuubos-remotectl", vec!["set".into(), svc, if on { "0".into() } else { "1".into() }],
                None, |ui, _| refresh_services(ui));
        }
        "password" => {
            /* The local console is the trusted place to read it (EPIC-039). */
            tool_then(ui, "/usr/sbin/nuubos-remotectl", vec!["credential".into(), "show".into()], None, |ui, out| {
                STATE.with(|st| st.borrow_mut().remote_password = out.trim().to_owned());
                render_services(ui);
            });
        }
        "regen" => {
            if arm_or(ui, "regen") {
                render_services(ui);
                tool_then(ui, "/usr/sbin/nuubos-remotectl", vec!["credential".into(), "regenerate".into()], None, |ui, _| {
                    STATE.with(|st| st.borrow_mut().remote_password.clear());
                    refresh_services(ui);
                });
            }
        }
        "open-sync" => open_sync(ui),
        _ => {}
    }
}

pub fn open_update(ui: &HomeWindow) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.busy = false;
        st.update_result.clear();
    });
    set_back(ui, 19);
    show(ui, 63);
    refresh_update(ui);
}

fn refresh_update(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut pairs: Vec<(String, String)> = run_tool("/usr/bin/nuubos-updatectl", &["status"], None)
            .lines()
            .filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_owned(), v.to_owned())))
            .collect();
        /* The last installation outcome (rolled back, installed...). */
        let apply = run_tool("/usr/sbin/nuubos-update-apply", &["status"], None);
        if let Some(r) = apply.lines().find_map(|l| l.strip_prefix("result=")) {
            pairs.push(("result".into(), r.to_owned()));
        }
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let result = pairs.iter().find(|(k, _)| k == "result").map(|(_, v)| v.clone()).unwrap_or_default();
            if result == "rolled-back" {
                notice(&ui, tr(&ui, 658, "The previous version was restored"));
            }
            STATE.with(|st| st.borrow_mut().update = pairs);
            render(&ui);
        });
    });
}

pub fn open_sync(ui: &HomeWindow) {
    set_back(ui, 1);
    show(ui, 66);
    refresh_sync(ui);
}

fn refresh_sync(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let pairs: Vec<(String, String)> = run_tool("/usr/bin/nuubos-syncctl", &["status"], None)
            .lines()
            .filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_owned(), v.to_owned())))
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().sync = pairs);
            render(&ui);
        });
    });
}

pub(crate) fn tool_then(ui: &HomeWindow, program: &'static str, args: Vec<String>, secret: Option<String>,
             done: fn(&HomeWindow, String)) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let refs: Vec<&str> = args.iter().map(String::as_str).collect();
        let out = run_tool(program, &refs, secret);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                done(&ui, out);
            }
        });
    });
}

fn page_confirm(ui: &HomeWindow, view: i32, key: &str) {
    match (view, key) {
        (55, k) if k.starts_with("hk:") => {
            let action = k[3..].to_owned();
            let (label, current) = STATE.with(|st| {
                let st = st.borrow();
                let label = HOTKEYS.iter().find(|h| h.0 == action).map(|h| tr(ui, h.1, h.2)).unwrap_or_default();
                (label, emu_get(&st, &format!("hotkey_{action}")))
            });
            let options = HOTKEY_CONTROLS.iter().map(|c| (c.to_string(), crate::control_label(ui, c))).collect();
            open_settings_choice(ui, &format!("emu-hotkey:{action}"), &label, options, &current);
        }
        (56, "ov-on") => {
            let on = STATE.with(|st| emu_get(&st.borrow(), "overlay") == "1");
            emu_set(ui, "overlay", if on { "0" } else { "1" }.to_owned());
        }
        (56, k) if k.starts_with("ov:") => {
            let field = &k[3..];
            let items = STATE.with(|st| emu_get(&st.borrow(), "overlay_items"));
            let mut list: Vec<&str> = items.split(',').filter(|f| !f.is_empty()).collect();
            if list.contains(&field) {
                list.retain(|f| *f != field);
            } else {
                list.push(field);
            }
            /* Kept in overlay order. */
            let ordered: Vec<&str> = OVERLAY_FIELDS.iter().map(|f| f.0).filter(|f| list.contains(f)).collect();
            emu_set(ui, "overlay_items", ordered.join(","));
        }
        (52, k) if k.starts_with("sys:") => {
            STATE.with(|st| st.borrow_mut().bios_system = k[4..].to_owned());
            show(ui, 53);
        }
        (54, "ra-signin") => {
            open_system_keyboard(ui, &tr(ui, 285, "Username"), KEYBOARD_RA_NAME, 54, "text", "");
        }
        (54, "ra-enabled") | (54, "ra-hardcore") => {
            let (field, on) = STATE.with(|st| {
                let st = st.borrow();
                if key == "ra-enabled" { ("enabled", !st.ra_enabled) } else { ("hardcore", !st.ra_hardcore) }
            });
            tool_then(ui, "/usr/bin/nuubos-achievementsctl",
                vec!["set".into(), active_user(), field.into(), if on { "1".into() } else { "0".into() }],
                None, |ui, _| refresh_achievements(ui));
        }
        (54, "ra-signout") => {
            if arm_or(ui, "ra-signout") {
                tool_then(ui, "/usr/bin/nuubos-achievementsctl", vec!["logout".into(), active_user()],
                    None, |ui, _| refresh_achievements(ui));
            }
        }
        (66, "sync-enabled") => {
            let on = STATE.with(|st| st.borrow().sync.iter().any(|(k, v)| k == "enabled" && v == "1"));
            tool_then(ui, "/usr/bin/nuubos-syncctl", vec![if on { "disable".into() } else { "enable".into() }], None, |ui, _| {
                /* Syncthing needs a moment to create its identity: the
                 * page refreshes when the state arrives. */
                refresh_sync(ui);
                let weak = ui.as_weak();
                slint::Timer::single_shot(std::time::Duration::from_secs(3), move || {
                    if let Some(ui) = weak.upgrade() { refresh_sync(&ui); }
                });
            });
        }
        (66, k) if k.starts_with("sync-folder:") => {
            let kind = k[12..].to_owned();
            let on = STATE.with(|st| st.borrow().sync.iter().any(|(key, v)| key == "folder" && v == &format!("{kind}\t1")));
            tool_then(ui, "/usr/bin/nuubos-syncctl", vec!["folder".into(), kind, if on { "off".into() } else { "on".into() }], None,
                |ui, _| refresh_sync(ui));
        }
        (66, k) if k.starts_with("sync-accept:") => {
            tool_then(ui, "/usr/bin/nuubos-syncctl", vec!["accept".into(), k[12..].to_owned()], None, |ui, _| refresh_sync(ui));
        }
        (66, k) if k.starts_with("sync-remove:") => {
            if arm_or(ui, k) {
                tool_then(ui, "/usr/bin/nuubos-syncctl", vec!["remove".into(), k[12..].to_owned()], None, |ui, _| refresh_sync(ui));
            }
        }
        (63, "update-check") => {
            STATE.with(|st| st.borrow_mut().busy = true);
            render(ui);
            tool_then(ui, "/usr/bin/nuubos-updatectl", vec!["check".into()], None, |ui, out| {
                let text = if out.starts_with("OK current") { tr(ui, 489, "Up to date") }
                    else if out.contains("signature") { tr(ui, 656, "The update could not be verified") }
                    else if out.contains("network") { tr(ui, 494, "Check the network connection") }
                    else if out.contains("unavailable") { tr(ui, 655, "Updates are not available in this build") }
                    else { String::new() };
                STATE.with(|st| {
                    let mut st = st.borrow_mut();
                    st.busy = false;
                    st.update_result = text;
                });
                refresh_update(ui);
            });
        }
        (63, "update-download") => {
            STATE.with(|st| st.borrow_mut().busy = true);
            render(ui);
            tool_then(ui, "/usr/bin/nuubos-jobctl", vec!["run".into(), "update-download".into()], None, |ui, out| {
                STATE.with(|st| st.borrow_mut().busy = false);
                if out.starts_with("ERR") {
                    notice(ui, if out.contains("checksum") || out.contains("signature") {
                        tr(ui, 656, "The update could not be verified") } else { tr(ui, 93, "Failed") });
                }
                refresh_update(ui);
            });
        }
        (63, "update-install") => {
            if arm_or(ui, "update-install") {
                tool_then(ui, "/usr/sbin/nuubos-update-apply", vec!["install".into()], None, |ui, out| {
                    if out.starts_with("OK") {
                        /* Installed by the lifecycle restart (pre-power hook). */
                        thread::spawn(|| { let _ = run_tool("/usr/bin/nuubos-systemctl", &["restart"], None); });
                    } else {
                        notice(ui, tr(ui, 656, "The update could not be verified"));
                        refresh_update(ui);
                    }
                });
            }
        }
        _ => {}
    }
}

/* Input on Settings pages 52-62 (from handle_settings_action). */
pub fn handle_page(ui: &HomeWindow, view: i32, action: &str) {
    if (58..=62).contains(&view) {
        crate::filesui::handle_page(ui, view, action);
        return;
    }
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => {
            STATE.with(|st| st.borrow_mut().bios_system.clear());
            match view {
                53 => show(ui, 52),
                _ => {
                    let back = STATE.with(|st| st.borrow().back);
                    navigate_settings_view(ui, if back > 0 { back } else { 19 });
                    if back == 1 {
                        /* Syncthing's state on the SERVICES row. */
                        refresh_services(ui);
                    } else if back == GAMING_VIEW {
                        /* The RetroAchievements account on its row. */
                        refresh_gaming(ui);
                    }
                }
            }
        }
        "menu_confirm" => {
            let key = focused_key(ui);
            page_confirm(ui, view, &key);
        }
        _ => {}
    }
}

/* System keyboard results for this module's purposes. */
pub fn keyboard_done(ui: &HomeWindow, purpose: i32, value: String) {
    /* Passwords are taken exactly as typed (spaces included). */
    let value = if matches!(purpose, KEYBOARD_RA_PASSWORD | KEYBOARD_SS_PASSWORD) { value } else { value.trim().to_owned() };
    match purpose {
        KEYBOARD_COLLECTION => {
            navigate_settings_view(ui, 50);
            render(ui);
            if value.is_empty() {
                return;
            }
            let id = STATE.with(|st| st.borrow().details.id.clone());
            let weak = ui.as_weak();
            thread::spawn(move || {
                let reply = library(&format!("COLLECTION_CREATE\t{value}"), false);
                if let Some(cid) = reply.trim().strip_prefix("OK ") {
                    let _ = library(&format!("COLLECTION_ADD\t{cid}\t{id}"), false);
                }
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        reload(&ui);
                    }
                });
            });
        }
        KEYBOARD_RA_NAME | KEYBOARD_SS_NAME => {
            if value.is_empty() {
                navigate_settings_view(ui, if purpose == KEYBOARD_RA_NAME { 54 } else { GAMING_VIEW });
                render(ui);
                return;
            }
            let ra = purpose == KEYBOARD_RA_NAME;
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                if ra { st.ra_name_pending = value.clone() } else { st.ss_name_pending = value.clone() }
            });
            open_system_keyboard(ui, &tr(ui, 172, "Password"),
                if ra { KEYBOARD_RA_PASSWORD } else { KEYBOARD_SS_PASSWORD }, if ra { 54 } else { GAMING_VIEW }, "password", "");
        }
        KEYBOARD_RA_PASSWORD => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, 54);
            let name = STATE.with(|st| std::mem::take(&mut st.borrow_mut().ra_name_pending));
            if value.is_empty() || name.is_empty() {
                render(ui);
                return;
            }
            STATE.with(|st| st.borrow_mut().busy = true);
            notice(ui, tr(ui, 590, "Signing in…"));
            render(ui);
            tool_then(ui, "/usr/bin/nuubos-achievementsctl", vec!["login".into(), active_user(), name], Some(value),
                |ui, out| {
                    let ok = out.starts_with("OK");
                    notice(ui, if ok { tr(ui, 588, "Signed in") } else if out.contains("network") {
                        tr(ui, 494, "Check the network connection") } else { tr(ui, 589, "Sign-in failed") });
                    refresh_achievements(ui);
                });
        }
        KEYBOARD_TGDB_KEY => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, GAMING_VIEW);
            if value.is_empty() {
                render_gaming(ui);
                return;
            }
            tool_then(ui, "/usr/bin/nuubos-scraper", vec!["apikey".into(), "set".into()], Some(value),
                |ui, out| {
                    refresh_gaming(ui);
                    notice(ui, if out.starts_with("OK") { tr(ui, 199, "Saved") } else { tr(ui, 93, "Failed") });
                });
        }
        KEYBOARD_SEARCH => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, 50);
            render(ui);
            /* Store the name, then look the game up with it. */
            let id = STATE.with(|st| st.borrow().details.id.clone());
            let weak = ui.as_weak();
            thread::spawn(move || {
                let _ = run_tool("/usr/bin/nuubos-scraper", &["search", &id], Some(value));
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        scrape_game(&ui, id, true);
                    }
                });
            });
        }
        KEYBOARD_SS_PASSWORD => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, GAMING_VIEW);
            let name = STATE.with(|st| std::mem::take(&mut st.borrow_mut().ss_name_pending));
            if value.is_empty() || name.is_empty() {
                render(ui);
                return;
            }
            notice(ui, tr(ui, 590, "Signing in…"));
            render_gaming(ui);
            tool_then(ui, "/usr/bin/nuubos-scraper", vec!["account".into(), "set".into(), name], Some(value),
                |ui, out| {
                    refresh_gaming(ui);
                    notice(ui, if out.starts_with("OK") { tr(ui, 588, "Signed in") } else if out.contains("network") {
                        tr(ui, 494, "Check the network connection") } else { tr(ui, 589, "Sign-in failed") });
                });
        }
        _ => {}
    }
}

/* Re-render after a language change. */
pub fn relocalize(ui: &HomeWindow) {
    render(ui);
}
