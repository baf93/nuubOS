/*
 * Game pages of nuubUI, all built from the owning services' data on the
 * generic rows of home.slint (GenRow):
 *
 *   50 Game Details (EPIC-016)      nuubos-libraryd DETAILS + nuubos-emud
 *   51 Game Settings (EPIC-018)     nuubos-emud GAME_SETTINGS / GAME_SET,
 *                                   Get More Emulators (nuubos-libretroctl
 *                                   cores, job core-install)
 *   52/53 BIOS Files (EPIC-033)     nuubos-biosctl
 *   54 RetroAchievements (EPIC-017) nuubos-achievementsctl
 *   55 Game Hotkeys, 56 Performance Overlay   nuubos-emud SETTINGS / SET
 *   Start Game (a Home bottom sheet) nuubos-emud STATES, LAUNCH <mode>
 *   63 Software Update (EPIC-048)   nuubos-updatectl, nuubos-update-apply
 *   66 Syncthing (EPIC-034)         syncui.rs (nuubos-syncd)
 *
 * Settings → Gaming (68) itself is built here too (gaming-rows): the
 * RetroAchievements account, the ScreenScraper account and bulk scraping
 * (EPIC-015, nuubos-scraper, nuubos-jobd), BIOS Files and the libretro
 * cheat collection (job cheats-update).
 *
 * The optional network services (Remote Services EPIC-037/039 through
 * nuubos-remotectl) are also rows of Settings → Connectivity,
 * section SERVICES (connectivity-services), built here from the same
 * replies.
 *
 * 50 is the console game page GameDetailsPage (details-page: hero cover,
 * blurred backdrop, metadata chips, description, a row of actions), 51
 * a Settings shell of its own (details-active), both opened from Home; 52-54 are Settings pages. This module keeps
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
    /* A game in a folder of its own: Delete Game removes the folder. */
    folder: bool,
    /* Ports: whether the user let this port read Music and Videos
     * (nuubos-portsd MEDIA_ACCESS). */
    media_access: bool,
    members: Vec<Member>,
    /* The user's tags of the game (libraryd field 13). */
    tags: Vec<String>,
    meta: Vec<(String, String)>,
    /* Search System (GitHub #29): (id, name) of every system and the ids
     * the metadata source can search; the source and its Cover Region. */
    all_systems: Vec<(String, String)>,
    search_systems: Vec<String>,
    source: String,
    source_region: String,
    /* nuubos-emud */
    core: String,
    core_default: String,
    core_override: String,
    cores: Vec<String>,
    aspect: String,
    filter: String,
    filter_default: String,
    /* Emulation accuracy (melonDS-nuubOS only): override and default. */
    accuracy_supported: bool,
    accuracy: String,
    accuracy_default: String,
    /* Nintendo 64 accessory: "" = Rumble Pak (default), "memory". */
    pak_supported: bool,
    pak: String,
    /* 3D render resolution: "" = Auto, "1x", "2x"; the default is "auto"
     * (by the output), "1x", "2x" or "custom" (the user's core option). */
    resolution_supported: bool,
    resolution: String,
    resolution_default: String,
    /* Frameskip: "" = the user's core option, "off", "auto", "max"; the
     * default is one of those or "custom". */
    frameskip_supported: bool,
    frameskip: String,
    frameskip_default: String,
    /* System Settings (GitHub #12): the system's own values a game
     * inherits ("" = none), and for a system the games with their own. */
    system_core: String,
    system_aspect: String,
    system_filter: String,
    system_accuracy: String,
    system_pak: String,
    system_resolution: String,
    system_frameskip: String,
    games: i32,
    /* Controls (user request 2026-10-29): the system's pad buttons
     * (control, label: "" = the control's name, "#n" = string n) and the
     * controls set, own and the system's (control, target). */
    pad: Vec<(String, String)>,
    remap: Vec<(String, String)>,
    system_remap: Vec<(String, String)>,
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
    /* System Settings (GitHub #12): the system's page, on its own (77,
     * from Settings → Gaming → System Settings, 76) or inside the Details
     * shell from a game's settings (51 with sys_scope). */
    sys: Details,
    sys_scope: bool,
    /* The Controls sub-page of Game Settings (51) or a system's page (77). */
    controls: bool,
    /* Advanced Search (GitHub #29) on page 51: search name, system and
     * Cover Region of the game. */
    search: bool,
    /* Page 76: (id, name, effective emulator) of the systems with games. */
    systems: Vec<(String, String, String)>,
    /* The row page 76 lands on once loaded (back from a system). */
    systems_focus: String,
    bios: Vec<BiosSystem>,
    bios_files: Vec<BiosFile>,
    bios_system: String,
    ra_user: String,
    ra_enabled: bool,
    ra_hardcore: bool,
    /* Offline Achievements (RAOfflineProxy, GitHub #18): casual only. */
    ra_offline: bool,
    ra_name_pending: String,
    ss_user: String,
    ss_available: bool,
    /* Metadata source (nuubos-scraper provider) and the end of the user's
     * TheGamesDB key. */
    ss_provider: String,
    /* Cover Region (GitHub #10): auto | us | eu | jp | wor. */
    ss_region: String,
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
    remote: Vec<(String, String)>,
    remote_password: String,
    update: Vec<(String, String)>,
    update_result: String,
    /* The update job running from this page ("download" | "install"), its
     * progress 0..100: its row becomes a progress bar. */
    update_job: String,
    update_progress: i32,
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
    /* Get More Emulators: the buildbot list is being fetched; the core
     * being downloaded (job core-install) and its progress. */
    cores_loading: bool,
    core_job: String,
    core_progress: i32,
    /* The libretro cheat collection (nuubos-libretroctl cheats status):
     * downloaded, when; its update job runs, its progress. */
    cheats_present: bool,
    cheats_updated: i64,
    cheats_job: bool,
    cheats_progress: i32,
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

pub(crate) fn emulation(line: &str, multi: bool) -> String {
    request(EMULATION_SOCKET, line, multi, 3).unwrap_or_default()
}

/* Puts `items` into the model behind `current`: in place when the row
 * count is unchanged, writing only the rows that differ, else a new model
 * (returned for the caller to set). A new model re-instantiates every row
 * (layout, text shaping, animations restarted): Game Details rebuilt all its
 * buttons and chips on each left/right, ~50 ms of UI thread per press on the
 * H700 (2026-10-15). */
pub(crate) fn sync_model<T: Clone + PartialEq + 'static>(current: ModelRc<T>, items: Vec<T>) -> Option<ModelRc<T>> {
    if current.row_count() == items.len() && current.as_any().is::<VecModel<T>>() {
        for (i, item) in items.into_iter().enumerate() {
            if current.row_data(i).as_ref() != Some(&item) {
                current.set_row_data(i, item);
            }
        }
        return None;
    }
    Some(ModelRc::from(Rc::new(VecModel::from(items))))
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
                    d.tags = f.get(12).map(|t| t.split(',').filter(|t| !t.is_empty()).map(String::from).collect())
                        .unwrap_or_default();
                }
            }
            "system_name" => d.system_name = value.into(),
            "sessions" => d.sessions = value.parse().unwrap_or(0),
            "folder" => d.folder = value == "1",
            /* Games-card games are "sd2:<path>" (libraryd): shown as SD2 › <path>. */
            "path" => d.path = match value.strip_prefix("sd2:") {
                Some(rest) => format!("SD2 › {rest}"),
                None => value.into(),
            },
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
            "accuracy_supported" => d.accuracy_supported = value == "1",
            "accuracy" => d.accuracy = value.into(),
            "accuracy_default" => d.accuracy_default = value.into(),
            "pak_supported" => d.pak_supported = value == "1",
            "pak" => d.pak = value.into(),
            "resolution_supported" => d.resolution_supported = value == "1",
            "resolution" => d.resolution = value.into(),
            "resolution_default" => d.resolution_default = value.into(),
            "frameskip_supported" => d.frameskip_supported = value == "1",
            "frameskip" => d.frameskip = value.into(),
            "frameskip_default" => d.frameskip_default = value.into(),
            "system_core" => d.system_core = value.into(),
            "system_aspect" => d.system_aspect = value.into(),
            "system_filter" => d.system_filter = value.into(),
            "system_accuracy" => d.system_accuracy = value.into(),
            "system_pak" => d.system_pak = value.into(),
            "system_resolution" => d.system_resolution = value.into(),
            "system_frameskip" => d.system_frameskip = value.into(),
            "games" => d.games = value.parse().unwrap_or(0),
            "pad" => d.pad = pairs_list(value),
            "remap" => d.remap = pairs_list(value),
            "system_remap" => d.system_remap = pairs_list(value),
            _ => {}
        }
    }
}

/* "a:b,c:d" -> [(a, b), (c, d)] */
fn pairs_list(value: &str) -> Vec<(String, String)> {
    value.split(',').filter_map(|p| p.split_once(':')).map(|(a, b)| (a.to_owned(), b.to_owned())).collect()
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
const HOTKEYS: [(&str, usize, &str); 9] = [
    ("save_state", 417, "Save State"),
    ("load_state", 418, "Load State"),
    ("slot_next", 779, "Next Slot"),
    ("slot_prev", 780, "Previous Slot"),
    ("fast_forward", 781, "Fast Forward"),
    ("quit", 422, "Quit Game"),
    ("screenshot", 592, "Screenshot"),
    /* Nintendo DS (melonDS-nuubOS) */
    ("screen_layout", 1302, "Screen Layout"),
    ("stylus", 1307, "Stylus"),
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

pub(crate) fn days_ago(ui: &HomeWindow, last: i64) -> String {
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
        "square" => tr(ui, 1413, "Square Pixels"),
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

/* What an unset key gives: the system's value when it has one, else the
 * default. */
fn inherited_label(ui: &HomeWindow, system_value: &str, system_label: String, default: String) -> String {
    if system_value.is_empty() { default_label(ui, &default) } else { tr_arg(ui, 1448, "System ({0})", &system_label) }
}

pub const SYSTEMS_VIEW: i32 = 76;
pub const SYSTEM_VIEW: i32 = 77;

/* The page shows a system's settings, not a game's. */
fn system_scope(ui: &HomeWindow) -> bool {
    let view = ui.get_settings_view();
    view == SYSTEM_VIEW || (view == 51 && STATE.with(|st| st.borrow().sys_scope))
}

/* The details the Game/System Settings rows and dropdowns work on. */
fn scope_details(ui: &HomeWindow) -> Details {
    let system = system_scope(ui);
    STATE.with(|st| { let st = st.borrow(); if system { st.sys.clone() } else { st.details.clone() } })
}

/* Rows of Game Settings (a game) or System Settings (system). */
/* Advanced Search (GitHub #29): how the metadata source looks the game
 * up. A port is another system's game (Daytona USA for PSP is a
 * Dreamcast game); each row stores its choice and runs Update. */
fn search_rows(ui: &HomeWindow, st: &State, d: &Details, rows: &mut Vec<(String, GenRow, bool)>) {
    let on = !st.busy && d.available;
    let searched = if d.meta("search").is_empty() { d.title.clone() } else { d.meta("search").to_owned() };
    let mut name = row(tr(ui, 800, "Search Name"), searched,
        tr(ui, 1474, "Change it if the game is not found or the match is wrong."));
    name.navigates = true;
    name.enabled = on;
    rows.push(("search-name".into(), name, on));
    let mut system = row(tr(ui, 1469, "Search System"), search_system_name(d),
        tr(ui, 1470, "Change it for a port of a game from another system."));
    system.navigates = true;
    system.enabled = on;
    rows.push(("search-system".into(), system, on));
    if d.source != "thegamesdb" {
        /* The region alone as the value (640x480: room for the title). */
        let own = d.meta("search_region");
        let mut region = if own.is_empty() {
            row(tr(ui, 1419, "Cover Region"), region_label(ui, &d.source_region),
                tr_arg(ui, 1471, "As in Settings ({0})", &region_label(ui, &d.source_region)))
        } else {
            row(tr(ui, 1419, "Cover Region"), region_label(ui, own), String::new())
        };
        region.navigates = true;
        region.enabled = on;
        rows.push(("search-region".into(), region, on));
    }
}

fn settings_rows(ui: &HomeWindow, st: &State, d: &Details, system: bool, rows: &mut Vec<(String, GenRow, bool)>) {
    if !system && !d.cores.is_empty() {
        let mut all = nav(tr_arg(ui, 1452, "{0} Settings", &d.system_name), String::new());
        all.detail = tr(ui, 1453, "Settings shared by every game of this system").into();
        rows.push(("sys-open".into(), all, true));
    }
    let core = if !d.core_override.is_empty() { d.core_override.clone() }
        else { inherited_label(ui, &d.system_core, d.system_core.clone(), d.core_default.clone()) };
    rows.push(("core".into(), nav(tr(ui, 525, "Emulator"), core), d.cores.len() > 1));
    /* Other libretro emulators for the system, from the buildbot. */
    if !system {
        if st.core_job.is_empty() {
            let value = if st.cores_loading { tr(ui, 1272, "Looking for emulators…") } else { String::new() };
            rows.push(("core-get".into(), nav(tr(ui, 1253, "Get More Emulators"), value), !st.cores_loading));
        } else {
            rows.push(("core-get".into(), progress_row(tr(ui, 1254, "Downloading emulator"), st.core_progress), false));
        }
    }
    let aspect = if !d.aspect.is_empty() { aspect_label(ui, &d.aspect) }
        else { inherited_label(ui, &d.system_aspect, aspect_label(ui, &d.system_aspect), tr(ui, 531, "Integer Scaling")) };
    rows.push(("aspect".into(), nav(tr(ui, 526, "Aspect Ratio"), aspect), true));
    let filter = if !d.filter.is_empty() { filter_label(ui, &d.filter) }
        else { inherited_label(ui, &d.system_filter, filter_label(ui, &d.system_filter), filter_label(ui, &d.filter_default)) };
    rows.push(("filter".into(), nav(tr(ui, 527, "Video Filter"), filter), true));
    if d.accuracy_supported {
        let accuracy = if !d.accuracy.is_empty() { accuracy_label(ui, &d.accuracy) }
            else { inherited_label(ui, &d.system_accuracy, accuracy_label(ui, &d.system_accuracy), accuracy_label(ui, &d.accuracy_default)) };
        let mut r = nav(tr(ui, 1314, "Emulation Accuracy"), accuracy);
        /* Flycast (translucent sorting, default Accurate) or melonDS-nuubOS. */
        r.detail = if matches!(d.system.as_str(), "dreamcast" | "naomi" | "atomiswave") {
            tr(ui, 1383, "Fast can draw smoke and glass wrong")
        } else {
            tr(ui, 1317, "Choose Accurate if the game misbehaves")
        }
        .into();
        rows.push(("accuracy".into(), r, true));
    }
    if d.pak_supported {
        let value = if !d.pak.is_empty() { pak_label(ui, &d.pak) }
            else if !d.system_pak.is_empty() { tr_arg(ui, 1448, "System ({0})", &pak_label(ui, &d.system_pak)) }
            else { pak_label(ui, "") };
        let mut r = nav(tr(ui, 1339, "Controller Accessory"), value);
        r.detail = tr(ui, 1342, "One fits at a time: choose Controller Pak if the game saves on it").into();
        rows.push(("pak".into(), r, true));
    }
    if d.resolution_supported {
        let value = if !d.resolution.is_empty() { resolution_label(ui, &d.resolution) }
            else { inherited_label(ui, &d.system_resolution, resolution_label(ui, &d.system_resolution), resolution_label(ui, &d.resolution_default)) };
        let mut r = nav(tr(ui, 454, "Resolution"), value);
        r.detail = if d.resolution_default == "auto" {
            tr(ui, 1381, "Auto: 1x on the console, 2x on a TV")
        } else {
            tr(ui, 1382, "2x is sharper but can slow the game down")
        }
        .into();
        rows.push(("resolution".into(), r, true));
    }
    if d.frameskip_supported {
        let value = if !d.frameskip.is_empty() { frameskip_label(ui, &d.frameskip) }
            else { inherited_label(ui, &d.system_frameskip, frameskip_label(ui, &d.system_frameskip), frameskip_label(ui, &d.frameskip_default)) };
        let mut r = nav(tr(ui, 1464, "Frameskip"), value);
        r.detail = tr(ui, 1466, "Auto skips frames only when the game slows down").into();
        rows.push(("frameskip".into(), r, true));
    }
    /* Emulated games only (ports and applications have no game pad). */
    if !d.cores.is_empty() {
        let custom = tr(ui, 1463, "Custom");
        let value = if !d.remap.is_empty() { custom }
            else if !d.system_remap.is_empty() { tr_arg(ui, 1448, "System ({0})", &custom) }
            else { String::new() };
        let mut r = nav(tr(ui, 1456, "Controls"), value);
        r.detail = tr(ui, 1457, "Choose what each button does in the game").into();
        rows.push(("controls".into(), r, true));
    }
    if system {
        if d.games > 0 {
            rows.push(("sys-games".into(), row(tr(ui, 1450, "Apply to All Games"), String::new(),
                armed_detail(&st.armed, "sys-games", tr(ui, 191, "Press again to confirm"),
                    tr_arg(ui, 1449, "{0} games use their own settings", &d.games.to_string()))), true));
        }
        rows.push(("sys-reset".into(), row(tr(ui, 1451, "Reset System Settings"), String::new(),
            armed_detail(&st.armed, "sys-reset", tr(ui, 598, "Press again to reset"),
                tr(ui, 536, "Changes apply the next time the game starts"))), true));
    } else {
        rows.push(("reset".into(), row(tr(ui, 535, "Reset Game Settings"), String::new(),
            armed_detail(&st.armed, "reset", tr(ui, 598, "Press again to reset"),
                tr(ui, 536, "Changes apply the next time the game starts"))), true));
    }
}

/* Controls: the D-pad is every system's. */
const DPAD: [&str; 4] = ["menu_up", "menu_down", "menu_left", "menu_right"];

/* The name on the system's pad of the game control `target`; the D-pad
 * as arrows (row values stay short: "Default (D-Pad Up)" did not fit). */
fn pad_label(ui: &HomeWindow, d: &Details, target: &str) -> String {
    if target == "none" {
        return tr(ui, 173, "None");
    }
    if let Some(i) = DPAD.iter().position(|c| *c == target) {
        return ["↑", "↓", "←", "→"][i].to_owned();
    }
    match d.pad.iter().find(|(c, _)| c == target).map(|(_, l)| l.as_str()) {
        Some(l) if l.starts_with('#') => tr(ui, l[1..].parse().unwrap_or(0), ""),
        Some(l) if !l.is_empty() => l.to_owned(),
        _ => crate::control_label(ui, target),
    }
}

/* What a control does untouched: itself, when the system's pad has it. */
fn pad_default(ui: &HomeWindow, d: &Details, control: &str) -> String {
    if DPAD.contains(&control) || d.pad.iter().any(|(c, _)| c == control) {
        pad_label(ui, d, control)
    } else {
        tr(ui, 173, "None")
    }
}

fn remap_get<'a>(list: &'a [(String, String)], control: &str) -> &'a str {
    list.iter().find(|(c, _)| c == control).map(|(_, t)| t.as_str()).unwrap_or("")
}

/* The value of a control's row: its own target, else the system's, else
 * itself. */
fn remap_value(ui: &HomeWindow, d: &Details, control: &str) -> String {
    let own = remap_get(&d.remap, control);
    if !own.is_empty() {
        return pad_label(ui, d, own);
    }
    let sys = remap_get(&d.system_remap, control);
    inherited_label(ui, sys, pad_label(ui, d, sys), pad_default(ui, d, control))
}

/* The Controls page: each control of the console and the button of the
 * system's pad it presses. */
fn controls_rows(ui: &HomeWindow, st: &State, d: &Details, rows: &mut Vec<(String, GenRow, bool)>) {
    for control in HOTKEY_CONTROLS {
        rows.push((format!("ctl:{control}"), nav(crate::control_label(ui, control), remap_value(ui, d, control)), true));
    }
    rows.push(("ctl-reset".into(), row(tr(ui, 1458, "Reset Controls"), String::new(),
        armed_detail(&st.armed, "ctl-reset", tr(ui, 598, "Press again to reset"),
            tr(ui, 536, "Changes apply the next time the game starts"))), !d.remap.is_empty()));
}

fn open_remap_choice(ui: &HomeWindow, control: &str) {
    let d = scope_details(ui);
    let sys = remap_get(&d.system_remap, control);
    let mut options = vec![(String::new(), inherited_label(ui, sys, pad_label(ui, &d, sys), pad_default(ui, &d, control)))];
    for (c, _) in &d.pad {
        options.push((c.clone(), pad_label(ui, &d, c)));
    }
    for c in DPAD {
        options.push((c.to_owned(), crate::control_label(ui, c)));
    }
    options.push(("none".into(), tr(ui, 173, "None")));
    open_settings_choice(ui, &format!("game-remap:{control}"), &crate::control_label(ui, control), options,
        remap_get(&d.remap, control));
}

/* Into or out of the Controls sub-page. */
fn show_controls(ui: &HomeWindow, on: bool) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.controls = on;
        st.armed.clear();
    });
    ui.set_gen_index(0);
    ui.set_gen_scroll(0);
    render(ui);
    if !on {
        focus(ui, "controls");
        render(ui);
    }
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
        51 | SYSTEM_VIEW if st.controls => {
            controls_rows(ui, st, if view == SYSTEM_VIEW || st.sys_scope { &st.sys } else { d }, &mut rows)
        }
        51 if st.search => search_rows(ui, st, d, &mut rows),
        51 if st.sys_scope => settings_rows(ui, st, &st.sys, true, &mut rows),
        51 => settings_rows(ui, st, d, false, &mut rows),
        SYSTEM_VIEW => settings_rows(ui, st, &st.sys, true, &mut rows),
        SYSTEMS_VIEW => {
            if st.systems.is_empty() {
                let mut r = row(tr(ui, 398, "No games yet"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for (id, name, core) in &st.systems {
                rows.push((format!("sys:{id}"), nav(name.clone(), core.clone()), true));
            }
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
                let mut offline = toggle(tr(ui, 1445, "Offline Achievements"), st.ra_offline && !st.ra_hardcore,
                    if st.ra_hardcore { tr(ui, 1447, "Casual only: off in Hardcore Mode") }
                    else { tr(ui, 1446, "Sent when you are back online") });
                offline.enabled = !st.ra_hardcore;
                rows.push(("ra-offline".into(), offline, !st.ra_hardcore));
                rows.push(("ra-signout".into(), row(tr(ui, 567, "Sign Out"), String::new(),
                    armed_detail(&st.armed, "ra-signout", tr(ui, 191, "Press again to confirm"), String::new())), true));
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
                let channel = get("channel");
                let mut channel_row = nav(tr(ui, 985, "Update Channel"), channel_name(ui, &channel));
                if channel != "stable" {
                    channel_row.detail = tr(ui, 989, "RC and Nightly get new versions first and may have bugs").into();
                }
                rows.push(("update-channel".into(), channel_row, !st.busy));
                rows.push(("update-auto".into(), toggle(tr(ui, 992, "Check Automatically"), get("auto") == "1",
                    tr(ui, 993, "Once a day, when connected to Wi-Fi")), true));
                let check_value = if st.busy { tr(ui, 488, "Checking…") }
                    else if available { tr_arg(ui, 652, "nuubOS {0} available", &get("version")) }
                    else if !st.update_result.is_empty() { st.update_result.clone() }
                    else { String::new() };
                rows.push(("update-check".into(), row(tr(ui, 487, "Check for Updates"), check_value, String::new()), !st.busy));
                if st.update_job == "download" {
                    rows.push(("update-busy".into(), progress_row(tr(ui, 660, "Downloading update"), st.update_progress), false));
                } else if available && get("downloaded") != "1" {
                    rows.push(("update-download".into(), row(tr(ui, 653, "Download"), String::new(), String::new()), !st.busy));
                }
                if st.update_job == "install" {
                    rows.push(("update-busy".into(), progress_row(tr(ui, 984, "Installing update"), st.update_progress), false));
                } else if available && get("downloaded") == "1" {
                    let detail = if get("battery") == "low" { tr(ui, 1012, "Charge the battery above 30% to install") }
                        else if get("battery") == "absent" { tr(ui, 1353, "No battery: keep the power cable connected until the update is complete") }
                        else { tr(ui, 657, "Your games, saves and settings are kept") };
                    rows.push(("update-install".into(), row(tr(ui, 654, "Install and Restart"), String::new(),
                        armed_detail(&st.armed, "update-install", tr(ui, 191, "Press again to confirm"), detail)), !st.busy));
                }
                /* What's New: the release notes, on their own full screen. */
                if available && !release_notes(&get("notes")).is_empty() {
                    rows.push(("update-notes".into(), nav(tr(ui, 1005, "What's New"), String::new()), true));
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
    let tags: Vec<String> = d.tags.iter().map(|t| crate::library::tag_label(ui, t)).collect();
    out.push(("tags".into(), detail_action(35, tr(ui, 1230, "Tags"), false),
        if tags.is_empty() { tr(ui, 1237, "Mark your progress, or a game that does not work") } else { tags.join(", ") }));
    if !d.cores.is_empty() {
        let core = if d.core.is_empty() { String::new() } else { format!("{}: {}", tr(ui, 525, "Emulator"), d.core) };
        out.push(("settings".into(), detail_action(3, tr(ui, 524, "Game Settings"), false), core));
    }
    if d.system == "ports" {
        /* A port's details come from PortMaster: no metadata source, no
         * search name. Music and Videos are the user's to open to it. */
        let mut media = detail_action(23, tr(ui, 1376, "Music & Videos Access"), false);
        media.on = d.media_access;
        out.push(("media".into(), media, if d.media_access {
            tr(ui, 1378, "On: this port can read your music and videos, not change them")
        } else {
            tr(ui, 1377, "Off: this port cannot see your music and videos")
        }));
    } else if d.meta("match") == "review" {
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
    if d.system != "ports" {
        /* Search name, system and Cover Region on their own page (user
         * 2026-10-30: three buttons crowded the page). */
        let mut search = detail_action(17, tr(ui, 1473, "Advanced Search"), false);
        search.enabled = !st.busy && d.available;
        let searched = if d.meta("search").is_empty() { d.title.clone() } else { d.meta("search").to_owned() };
        out.push(("search".into(), search, tr_arg(ui, 801, "Searched as \"{0}\". Change it if the game is not found or the match is wrong.", &searched)));
    }
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
    /* An unavailable game can be deleted too: libraryd then removes only
     * its library entry, cover and metadata (user request 2026-10-20). */
    let mut delete = detail_action(10, tr(ui, 544, "Delete Game"), false);
    delete.armed = st.armed == "delete";
    out.push(("delete".into(), delete, armed_detail(&st.armed, "delete",
        tr(ui, 546, "Press again to delete permanently"),
        if d.available && d.folder {
            tr(ui, 1475, "Permanently deletes the game's folder and everything in it for every user. Saves are kept.")
        } else if d.available {
            tr(ui, 545, "Permanently deletes the game file for every user. Saves are kept.")
        } else {
            tr(ui, 1318, "Removes the game, its cover and details from the library for every user. Saves are kept.")
        })));
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
    for t in &d.tags {
        chips.push(detail_chip(if t == "broken" { 13 } else { 35 }, crate::library::tag_label(ui, t), t == "broken"));
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
    if let Some(m) = sync_model(ui.get_details_actions(), actions) {
        ui.set_details_actions(m);
    }
    if let Some(m) = sync_model(ui.get_details_chips(), chips) {
        ui.set_details_chips(m);
    }
    ui.set_details_meta(meta.into());
    ui.set_details_caption(caption.into());
    ui.set_details_caption_armed(armed);
    ui.set_gen_index(index);
    ui.set_gen_select(select);
    ui.set_gen_select_label("".into());
}

/* Backdrop: a small, box-blurred copy of the cover,
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
    /* Before Files: Backup Content (59) sits inside its range. */
    if view == crate::storageui::STORAGE_VIEW || view == crate::storageui::CONTENT_VIEW
        || view == crate::storageui::DATA_VIEW {
        crate::storageui::render(ui);
        return;
    }
    if (58..=62).contains(&view) || view == crate::filesui::RECOVERY_VIEW {
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
    if view == crate::syncui::VIEW {
        crate::syncui::render_page(ui);
        return;
    }
    if view == crate::tsui::VIEW {
        crate::tsui::render(ui);
        return;
    }
    if !(50..=56).contains(&view) && view != 63 && view != SYSTEMS_VIEW && view != SYSTEM_VIEW {
        return;
    }
    ui.set_settings_search_page(view == 51 && STATE.with(|st| st.borrow().search));
    let rows = STATE.with(|st| build_rows(ui, &st.borrow(), view));
    let section = match view {
        50 => STATE.with(|st| st.borrow().details.system_name.clone()),
        51 | SYSTEM_VIEW if STATE.with(|st| st.borrow().controls) => tr(ui, 1456, "Controls"),
        51 if STATE.with(|st| st.borrow().search) => tr(ui, 1473, "Advanced Search"),
        51 if STATE.with(|st| st.borrow().sys_scope) =>
            STATE.with(|st| tr_arg(ui, 1452, "{0} Settings", &st.borrow().sys.system_name)),
        51 => tr(ui, 524, "Game Settings"),
        SYSTEMS_VIEW => tr(ui, 1454, "System Settings"),
        SYSTEM_VIEW => STATE.with(|st| st.borrow().sys.system_name.clone()),
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
    apply_rows_sections(ui, rows, section, (split, split_label), (0, String::new()), &[]);
}

/* The same with up to three sections: the second from row `split.0`, the
 * third from `split2.0` (0 = none); `scopes` = the USER/DEVICE chip of each
 * section when they differ (empty = the page's own). */
pub(crate) fn apply_rows_sections(ui: &HomeWindow, rows: Vec<(String, GenRow, bool)>, section: String,
    first: (i32, String), second: (i32, String), scopes: &[String]) {
    apply_rows_sections3(ui, rows, section, first, second, (0, String::new()), scopes);
}

/* Up to four sections: the top one and three inline headers (Syncthing:
 * DEVICES, WHAT TO SYNC, AVAILABLE FOLDERS). */
pub(crate) fn apply_rows_sections3(ui: &HomeWindow, rows: Vec<(String, GenRow, bool)>, section: String,
    (split, split_label): (i32, String), (split2, split2_label): (i32, String),
    (split3, split3_label): (i32, String), scopes: &[String]) {
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
    if let Some(m) = sync_model(ui.get_gen_rows(), rows) {
        ui.set_gen_rows(m);
    }
    ui.set_gen_index(index);
    ui.set_gen_select(select);
    ui.set_gen_select_label("".into());
    ui.set_gen_split(split);
    ui.set_gen_split_label(split_label.into());
    ui.set_gen_split2(split2);
    ui.set_gen_split2_label(split2_label.into());
    ui.set_gen_split3(split3);
    ui.set_gen_split3_label(split3_label.into());
    let scopes: Vec<slint::SharedString> = scopes.iter().map(|s| s.into()).collect();
    if let Some(m) = sync_model(ui.get_gen_scopes(), scopes) {
        ui.set_gen_scopes(m);
    }
    ui.set_gen_scroll(if split > 0 {
        sectioned_scroll(ui, index, count, ui.get_gen_scroll(), (split, split2, split3))
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

/* Focus the row with this key (back on the row that opened a page). */
pub(crate) fn focus(ui: &HomeWindow, key: &str) {
    if let Some(i) = STATE.with(|st| st.borrow().keys.iter().position(|k| k == key)) {
        ui.set_gen_index(i as i32);
    }
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
        if d.system == "ports" {
            d.media_access = crate::library::ports_command(&format!("MEDIA_ACCESS\t{}", port_script(&d)))
                .is_ok_and(|r| r.trim() == "OK 1");
        }
        if d.system != "ports" {
            let ss = run_tool("/usr/bin/nuubos-scraper", &["provider", "status"], None);
            d.source = kv(&ss, "provider").to_owned();
            d.source_region = kv(&ss, "region").to_owned();
            d.search_systems = kv(&ss, "systems").split(',').filter(|s| !s.is_empty()).map(str::to_owned).collect();
            d.all_systems = library("SYSTEMS", true).lines().filter_map(|l| l.strip_prefix("system="))
                .filter_map(|l| l.split_once('\t')).map(|(i, n)| (i.to_owned(), n.to_owned())).collect();
        }
        let bios = run_tool("/usr/bin/nuubos-biosctl", &["status", &d.system], None);
        d.bios_missing = parse_bios(&bios).0.iter().any(|s| s.state == "missing" || s.state == "invalid");
        let load = |p: &str| if p.is_empty() { None } else { Image::load_from_path(std::path::Path::new(p)).ok() };
        let cover_src = load(&d.cover);
        let cover = cover_src.as_ref().and_then(|i| crate::library::shape_cover(i, target, radius)).map(|c| c.0);
        let backdrop = cover_src.as_ref().and_then(blurred);
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
        st.search = false;
        st.busy = false;
    });
    /* The Home card's cover stands in until the hero is decoded. */
    ui.set_details_has_cover(cover.is_some());
    ui.set_details_cover(cover.unwrap_or_default());
    ui.set_details_has_backdrop(false);
    ui.set_details_backdrop(Image::default());
    load_details_then(ui, id.to_owned(), true);
}

/* ---------------------------------------------------------------- */
/* Start Game sheet (Confirm held on a game card)                   */
/* ---------------------------------------------------------------- */

thread_local! {
    /* The game and its save states, newest first: (slot, mtime);
     * slot -1 = the automatic state. */
    static START: RefCell<(String, Vec<(i32, i64)>)> = RefCell::new((String::new(), Vec::new()));
    /* The held card while emud lists its states (images stay on this thread). */
    static START_CARD: RefCell<Option<crate::HomeCard>> = RefCell::new(None);
}

/* Opens once emud has listed the states, so the sheet rises complete. */
pub fn open_start_sheet(ui: &HomeWindow, card: &crate::HomeCard) {
    let weak = ui.as_weak();
    let id = card.key.to_string();
    START_CARD.with(|c| *c.borrow_mut() = Some(card.clone()));
    thread::spawn(move || {
        let reply = emulation(&format!("STATES\t{id}"), true);
        let hardcore = reply.lines().any(|l| l == "hardcore=1");
        let mut states: Vec<(i32, i64, String)> = reply
            .lines()
            .filter_map(|l| l.strip_prefix("state="))
            .filter_map(|v| {
                let f: Vec<&str> = v.split('\t').collect();
                Some((f.first()?.parse().ok()?, f.get(1)?.parse().ok()?, f.get(2).unwrap_or(&"").to_string()))
            })
            .collect();
        states.sort_by(|a, b| b.1.cmp(&a.1));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let Some(card) = START_CARD.with(|c| c.borrow_mut().take()).filter(|c| c.key.as_str() == id) else { return };
            if ui.get_settings_open() || crate::library::GAME_RUNNING.load(Ordering::SeqCst) {
                return;
            }
            let items: Vec<crate::StartSheetItem> = states
                .iter()
                .enumerate()
                .map(|(i, (slot, mtime, hhmm))| crate::StartSheetItem {
                    label: if *slot < 0 { tr(&ui, 765, "Automatic Save") } else { tr_arg(&ui, 423, "Slot {0}", &slot.to_string()) }.into(),
                    detail: tr_arg(&ui, 766, "Saved {0}", &saved_when(&ui, *mtime, hhmm)).into(),
                    tile: if *slot < 0 { String::new() } else { slot.to_string() }.into(),
                    latest: i == 0,
                })
                .collect();
            START.with(|s| *s.borrow_mut() = (id, states.iter().map(|s| (s.0, s.1)).collect()));
            ui.set_start_sheet_title(card.title.clone());
            ui.set_start_sheet_system(card.system_name.clone());
            ui.set_start_sheet_has_cover(card.has_cover);
            ui.set_start_sheet_cover(if card.has_cover { card.cover.clone() } else { Image::default() });
            ui.set_start_sheet_items(ModelRc::from(Rc::new(VecModel::from(items))));
            ui.set_start_sheet_hardcore(hardcore);
            ui.set_start_sheet_index(0);
            ui.set_start_sheet_scroll(0);
            ui.set_start_sheet_open(true);
        });
    });
}

pub fn close_start_sheet(ui: &HomeWindow) {
    ui.set_start_sheet_open(false);
    ui.set_start_sheet_items(ModelRc::default());
    ui.set_start_sheet_cover(Image::default());
}

/* Home input while the sheet is open: row 0 starts from the beginning,
 * row n the n-th newest state. */
pub fn start_sheet_action(ui: &HomeWindow, action: &str) {
    let count = START.with(|s| s.borrow().1.len()) as i32 + 1;
    match action {
        "menu_up" | "menu_down" => {
            play_ui_sound(action);
            let next = (ui.get_start_sheet_index() + if action == "menu_up" { -1 } else { 1 }).rem_euclid(count);
            ui.set_start_sheet_index(next);
            /* Saves scroll under the fixed first row. */
            let visible = ui.get_start_sheet_visible_rows().max(1);
            let save = (next - 1).max(0);
            let scroll = ui.get_start_sheet_scroll();
            ui.set_start_sheet_scroll(if save < scroll { save } else if save >= scroll + visible { save - visible + 1 } else { scroll });
        }
        "menu_back" => {
            play_ui_sound(action);
            close_start_sheet(ui);
        }
        "menu_confirm" => {
            play_ui_sound(action);
            let index = ui.get_start_sheet_index();
            let (id, states) = START.with(|s| s.borrow().clone());
            let mode = match index {
                0 => "new".to_owned(),
                n => match states.get(n as usize - 1) {
                    Some((slot, _)) => format!("slot:{slot}"),
                    None => return,
                },
            };
            close_start_sheet(ui);
            crate::library::launch_with_mode(ui, &id, true, &mode);
        }
        _ => {}
    }
}

pub fn leave_details(ui: &HomeWindow) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.sys_scope = false;
        st.controls = false;
    });
    ui.set_settings_system_scope(false);
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

/* Each tag On/Off; a status tag replaces the other one (libraryd). */
fn open_tags(ui: &HomeWindow) {
    let d = STATE.with(|st| st.borrow().details.clone());
    let options: Vec<(String, String)> = crate::library::TAGS
        .iter()
        .map(|t| {
            let on = d.tags.iter().any(|x| x == t);
            (t.to_string(), format!("{}  •  {}", crate::library::tag_label(ui, t), if on { tr(ui, 222, "On") } else { tr(ui, 54, "Off") }))
        })
        .collect();
    open_settings_choice(ui, "game-tag", &tr(ui, 1230, "Tags"), options, "");
}

fn open_core_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    let mut options = vec![(String::new(), inherited_label(ui, &d.system_core, d.system_core.clone(), d.core_default.clone()))];
    options.extend(d.cores.iter().map(|c| (c.clone(), c.clone())));
    open_settings_choice(ui, "game-core", &tr(ui, 525, "Emulator"), options, &d.core_override);
}

fn open_aspect_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    let mut options = vec![(String::new(),
        inherited_label(ui, &d.system_aspect, aspect_label(ui, &d.system_aspect), tr(ui, 531, "Integer Scaling")))];
    for v in ["core", "4:3", "16:9", "full", "integer", "square"] {
        options.push((v.to_owned(), aspect_label(ui, v)));
    }
    open_settings_choice(ui, "game-aspect", &tr(ui, 526, "Aspect Ratio"), options, &d.aspect);
}

fn accuracy_label(ui: &HomeWindow, v: &str) -> String {
    if v == "accurate" { tr(ui, 1316, "Accurate") } else { tr(ui, 1315, "Fast") }
}

fn pak_label(ui: &HomeWindow, v: &str) -> String {
    if v == "memory" { tr(ui, 1341, "Controller Pak") } else { tr(ui, 1340, "Rumble Pak") }
}

fn resolution_label(ui: &HomeWindow, v: &str) -> String {
    match v {
        "auto" => tr(ui, 52, "Auto"),
        "2x" => tr(ui, 1380, "2x"),
        "custom" => tr(ui, 315, "Custom"),
        _ => tr(ui, 1379, "1x"),
    }
}

fn open_resolution_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    let mut options = vec![(String::new(), inherited_label(ui, &d.system_resolution,
        resolution_label(ui, &d.system_resolution), resolution_label(ui, &d.resolution_default)))];
    for v in ["1x", "2x"] {
        options.push((v.to_owned(), resolution_label(ui, v)));
    }
    open_settings_choice(ui, "game-resolution", &tr(ui, 454, "Resolution"), options, &d.resolution);
}

fn frameskip_label(ui: &HomeWindow, v: &str) -> String {
    match v {
        "auto" => tr(ui, 52, "Auto"),
        "max" => tr(ui, 1465, "Auto (up to 3)"),
        "custom" => tr(ui, 315, "Custom"),
        _ => tr(ui, 54, "Off"),
    }
}

fn open_frameskip_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    let mut options = vec![(String::new(), inherited_label(ui, &d.system_frameskip,
        frameskip_label(ui, &d.system_frameskip), frameskip_label(ui, &d.frameskip_default)))];
    for v in ["off", "auto", "max"] {
        options.push((v.to_owned(), frameskip_label(ui, v)));
    }
    open_settings_choice(ui, "game-frameskip", &tr(ui, 1464, "Frameskip"), options, &d.frameskip);
}

/* Nintendo 64: Rumble Pak (the default, stored empty) or Controller Pak. */
fn open_pak_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    /* Under a system set to Controller Pak a game can still choose Rumble. */
    let options = if d.system_pak.is_empty() {
        vec![(String::new(), pak_label(ui, "")), ("memory".to_owned(), pak_label(ui, "memory"))]
    } else {
        vec![(String::new(), tr_arg(ui, 1448, "System ({0})", &pak_label(ui, &d.system_pak))),
             ("rumble".to_owned(), pak_label(ui, "")), ("memory".to_owned(), pak_label(ui, "memory"))]
    };
    open_settings_choice(ui, "game-pak", &tr(ui, 1339, "Controller Accessory"), options, &d.pak);
}

fn open_accuracy_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    let mut options = vec![(String::new(), inherited_label(ui, &d.system_accuracy,
        accuracy_label(ui, &d.system_accuracy), accuracy_label(ui, &d.accuracy_default)))];
    for v in ["fast", "accurate"] {
        options.push((v.to_owned(), accuracy_label(ui, v)));
    }
    open_settings_choice(ui, "game-accuracy", &tr(ui, 1314, "Emulation Accuracy"), options, &d.accuracy);
}

fn open_filter_choice(ui: &HomeWindow) {
    let d = scope_details(ui);
    let mut options = vec![(String::new(), inherited_label(ui, &d.system_filter,
        filter_label(ui, &d.system_filter), filter_label(ui, &d.filter_default)))];
    for v in ["sharp", "smooth", "pixel"] {
        options.push((v.to_owned(), filter_label(ui, v)));
    }
    open_settings_choice(ui, "game-filter", &tr(ui, 527, "Video Filter"), options, &d.filter);
}

fn game_set(ui: &HomeWindow, key: &str, value: String) {
    if system_scope(ui) {
        let system = STATE.with(|st| st.borrow().sys.system.clone());
        system_command(ui, format!("SYSTEM_SET\t{system}\t{key}\t{value}"));
        return;
    }
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

/* System Settings: the system's values (and its games with their own). */
fn load_system(ui: &HomeWindow, system: String, name: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut d = Details { system: system.clone(), system_name: name, ..Default::default() };
        parse_game_settings(&emulation(&format!("SYSTEM_SETTINGS\t{system}"), true), &mut d);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().sys = d);
            render(&ui);
        });
    });
}

fn system_command(ui: &HomeWindow, line: String) {
    let (system, name) = STATE.with(|st| { let st = st.borrow(); (st.sys.system.clone(), st.sys.system_name.clone()) });
    let weak = ui.as_weak();
    thread::spawn(move || {
        let _ = emulation(&line, false);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                load_system(&ui, system, name);
            }
        });
    });
}

/* Settings → Gaming → System Settings: the systems with games and the
 * emulator each one uses. */
pub fn open_systems(ui: &HomeWindow) {
    set_back(ui, GAMING_VIEW);
    STATE.with(|st| st.borrow_mut().systems.clear());
    show(ui, SYSTEMS_VIEW);
    let names = crate::library::system_names();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut list = Vec::new();
        for (id, name) in names {
            let mut d = Details::default();
            parse_game_settings(&emulation(&format!("SYSTEM_SETTINGS\t{id}"), true), &mut d);
            /* Ports and applications have no emulator to set. */
            if !d.cores.is_empty() {
                list.push((id, name, d.core));
            }
        }
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().systems = list);
            render(&ui);
            let target = STATE.with(|st| std::mem::take(&mut st.borrow_mut().systems_focus));
            if !target.is_empty() {
                focus(&ui, &target);
                render(&ui);
            }
        });
    });
}

/* Confirm on a Game/System Settings row; false when the key is not one. */
fn settings_confirm(ui: &HomeWindow, key: &str) -> bool {
    match key {
        "core" => open_core_choice(ui),
        "aspect" => open_aspect_choice(ui),
        "filter" => open_filter_choice(ui),
        "accuracy" => open_accuracy_choice(ui),
        "pak" => open_pak_choice(ui),
        "resolution" => open_resolution_choice(ui),
        "frameskip" => open_frameskip_choice(ui),
        "controls" => show_controls(ui, true),
        k if k.starts_with("ctl:") => open_remap_choice(ui, &k[4..]),
        "ctl-reset" => {
            if arm_or(ui, "ctl-reset") {
                game_set(ui, "remap", String::new());
            }
        }
        "sys-games" => {
            if arm_or(ui, "sys-games") {
                let system = STATE.with(|st| st.borrow().sys.system.clone());
                system_command(ui, format!("SYSTEM_RESET_GAMES\t{system}"));
            } else {
                render(ui);
            }
        }
        "sys-reset" => {
            if arm_or(ui, "sys-reset") {
                let system = STATE.with(|st| st.borrow().sys.system.clone());
                system_command(ui, format!("SYSTEM_RESET\t{system}"));
            } else {
                render(ui);
            }
        }
        _ => return false,
    }
    true
}

/* A choice made in a dropdown opened by this module. */
pub fn apply_choice(ui: &HomeWindow, context: &str, value: String) {
    match context {
        "game-core" => game_set(ui, "core", value),
        "game-core-get" => install_core(ui, value),
        "game-aspect" => game_set(ui, "aspect", value),
        "game-filter" => game_set(ui, "filter", value),
        "game-accuracy" => game_set(ui, "accuracy", value),
        "game-pak" => game_set(ui, "pak", value),
        "game-resolution" => game_set(ui, "resolution", value),
        "game-frameskip" => game_set(ui, "frameskip", value),
        c if c.starts_with("game-remap:") => game_set(ui, &format!("remap_{}", &c[11..]), value),
        "emu-slots" => emu_set(ui, "slots", value),
        "emu-position" => emu_set(ui, "position", value),
        "game-fill" => start_bulk(ui, value),
        "update-channel" => {
            STATE.with(|st| st.borrow_mut().update_result.clear());
            tool_then(ui, "/usr/bin/nuubos-updatectl", vec!["channel".into(), value], None, |ui, _| refresh_update(ui));
        }
        "game-source" => tool_then(ui, "/usr/bin/nuubos-scraper", vec!["provider".into(), "set".into(), value],
            None, |ui, _| refresh_gaming(ui)),
        "game-region" => tool_then(ui, "/usr/bin/nuubos-scraper", vec!["region".into(), "set".into(), value],
            None, |ui, _| refresh_gaming(ui)),
        /* Store the game's choice, then look it up again (as Search Name). */
        "game-search-system" | "game-search-region" => {
            let id = STATE.with(|st| st.borrow().details.id.clone());
            let verb = if context == "game-search-system" { "search-system" } else { "search-region" };
            tool_then(ui, "/usr/bin/nuubos-scraper", vec![verb.into(), id, value], None, |ui, out| {
                if out.starts_with("OK") {
                    let id = STATE.with(|st| st.borrow().details.id.clone());
                    scrape_game(ui, id, true);
                } else {
                    play_ui_sound("error");
                }
            });
        }
        c if c.starts_with("emu-hotkey:") => emu_set(ui, &format!("hotkey_{}", &c[11..]), value),
        "game-tag" => {
            let (id, on) = STATE.with(|st| {
                let st = st.borrow();
                (st.details.id.clone(), st.details.tags.iter().any(|t| *t == value))
            });
            let verb = if on { "TAG_CLEAR" } else { "TAG_SET" };
            details_command(ui, format!("{verb}\t{id}\t{value}"));
        }
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

/* The port's script name, as nuubos-portsd knows it. */
fn port_script(d: &Details) -> String {
    d.path.rsplit('/').next().unwrap_or_default().to_owned()
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
        "tags" => open_tags(ui),
        "settings" => {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.sys_scope = false;
                st.controls = false;
                st.search = false;
            });
            ui.set_settings_system_scope(false);
            show(ui, 51);
        }
        "bios" => {
            STATE.with(|st| st.borrow_mut().bios_system = d.system.clone());
            leave_details(ui);
            open_bios(ui);
        }
        "metadata" => scrape_game(ui, d.id.clone(), !d.meta("match").is_empty()),
        "media" => {
            let on = !d.media_access;
            let ok = crate::library::ports_command(&format!("MEDIA_ACCESS\t{}\t{}", port_script(&d), if on { 1 } else { 0 }))
                .is_ok_and(|r| r.trim() == "OK");
            if ok {
                STATE.with(|st| st.borrow_mut().details.media_access = on);
                play_ui_sound("select");
            } else {
                play_ui_sound("error");
            }
            render(ui);
        }
        "search" => {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.search = true;
                st.controls = false;
                st.sys_scope = false;
            });
            ui.set_settings_system_scope(false);
            show(ui, 51);
        }
        "search-name" => {
            let current = if d.meta("search").is_empty() { d.title.clone() } else { d.meta("search").to_owned() };
            open_system_keyboard(ui, &tr(ui, 800, "Search Name"), KEYBOARD_SEARCH, 51, "text", &current);
        }
        "search-system" => {
            let mut options: Vec<(String, String)> = d.all_systems.iter()
                .filter(|(id, _)| *id != d.system && d.search_systems.contains(id))
                .map(|(id, name)| (id.clone(), name.clone())).collect();
            options.sort_by_key(|(_, name)| name.to_lowercase());
            options.insert(0, ("default".into(), tr_arg(ui, 1472, "Same as the game ({0})", &d.system_name)));
            let current = if d.meta("search_system").is_empty() { "default" } else { d.meta("search_system") };
            open_settings_choice(ui, "game-search-system", &tr(ui, 1469, "Search System"), options, current);
        }
        "search-region" => {
            let mut options = vec![("default".to_owned(), tr_arg(ui, 1471, "As in Settings ({0})", &region_label(ui, &d.source_region)))];
            options.extend(REGIONS.iter().map(|id| (id.to_string(), region_label(ui, id))));
            let current = if d.meta("search_region").is_empty() { "default" } else { d.meta("search_region") };
            open_settings_choice(ui, "game-search-region", &tr(ui, 1419, "Cover Region"), options, current);
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
        "sys-open" => {
            /* The game's system, inside the shell; Back returns here. */
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.sys_scope = true;
                st.armed.clear();
            });
            ui.set_settings_system_scope(true);
            ui.set_gen_index(0);
            ui.set_gen_scroll(0);
            load_system(ui, d.system.clone(), d.system_name.clone());
        }
        k if settings_confirm(ui, k) => {}
        "core-get" if !STATE.with(|st| st.borrow().cores_loading) && STATE.with(|st| st.borrow().core_job.is_empty()) => {
            open_core_offer(ui)
        }
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
            if ui.get_settings_view() == 51 && STATE.with(|st| st.borrow().controls) {
                show_controls(ui, false);
            } else if ui.get_settings_view() == 51 && STATE.with(|st| st.borrow().search) {
                STATE.with(|st| st.borrow_mut().search = false);
                show(ui, 50);
                focus(ui, "search");
                render(ui);
            } else if ui.get_settings_view() == 51 && STATE.with(|st| st.borrow().sys_scope) {
                /* Back from the system to the game, its values reloaded. */
                STATE.with(|st| st.borrow_mut().sys_scope = false);
                ui.set_settings_system_scope(false);
                reload(ui);
                show(ui, 51);
                if let Some(i) = STATE.with(|st| st.borrow().keys.iter().position(|k| k == "sys-open")) {
                    ui.set_gen_index(i as i32);
                    render(ui);
                }
            } else if ui.get_settings_view() == 51 {
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
        "thegamesdb" => tr(ui, 794, "Covers and descriptions in English. A free private API key gives more requests."),
        "libretro" => tr(ui, 795, "Covers only. No account needed."),
        _ => tr(ui, 793, "Covers, descriptions and ratings in your language. A free ScreenScraper account gives more requests."),
    }
}

/* The system a game is searched as: its own, or the one the user chose
 * (GitHub #29). */
fn search_system_name(d: &Details) -> String {
    let id = d.meta("search_system");
    if id.is_empty() {
        return d.system_name.clone();
    }
    d.all_systems.iter().find(|(i, _)| i == id).map(|(_, n)| n.clone()).unwrap_or_else(|| id.to_owned())
}

/* Cover Regions (nuubos-scraper region ids). */
const REGIONS: [&str; 5] = ["auto", "us", "eu", "jp", "wor"];

fn region_label(ui: &HomeWindow, id: &str) -> String {
    match id {
        "us" => tr(ui, 1421, "USA"),
        "eu" => tr(ui, 1422, "Europe"),
        "jp" => tr(ui, 1423, "Japan"),
        "wor" => tr(ui, 1424, "World"),
        _ => tr(ui, 122, "Automatic"),
    }
}

fn position_label(ui: &HomeWindow, v: &str) -> String {
    match v {
        "top" => tr(ui, 1410, "Top"),
        "bottom" => tr(ui, 1412, "Bottom"),
        _ => tr(ui, 1411, "Center"),
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
    /* Where a picture lower than the screen sits (GitHub #13). */
    rows.push(("position".into(), GenRow { navigates: true, ..row(tr(ui, 1408, "Picture Position"),
        position_label(ui, &emu_get(st, "position")), tr(ui, 1409, "Where the game sits when it does not fill the screen")) }, true));
    rows.push(("bezels".into(), toggle(tr(ui, 775, "Bezels"), on("bezels"),
        tr(ui, 776, "Frames the game when it does not fill the screen")), true));
    rows.push(("crt".into(), toggle(tr(ui, 777, "CRT Effect"), on("crt"),
        tr(ui, 778, "Scanlines for consoles played on TVs")), true));
    /* The Quick Menu's GAME → RetroArch row (hidden by default). */
    rows.push(("retroarch_menu".into(), toggle(tr(ui, 1391, "RetroArch in the Quick Menu"), on("retroarch_menu"),
        tr(ui, 1392, "Opens RetroArch's own menu during a game")), true));
    /* Per system (GitHub #12): emulator and picture for all its games. */
    let mut systems = nav(tr(ui, 1454, "System Settings"), String::new());
    systems.detail = tr(ui, 1455, "Emulator and picture for all the games of a system").into();
    rows.push(("systems".into(), systems, true));
    /* LIBRARY (DEVICE): the library scan, the metadata source, its optional
     * account or key, then the two scraping runs. */
    let scanning = ui.get_library_scanning();
    rows.push(("scan".into(), row(tr(ui, 804, "Scan for New Games"),
        if scanning { tr(ui, 410, "Searching for games…") } else { String::new() },
        tr(ui, 805, "Finds games copied to the SD card since the last scan")), !scanning));
    /* Device-wide, owned by libraryd (user request 2026-10-20). */
    rows.push(("unavailable".into(), toggle(tr(ui, 1319, "Show Unavailable Games"),
        crate::library::SHOW_UNAVAILABLE.load(std::sync::atomic::Ordering::Relaxed),
        tr(ui, 1320, "Games whose file is missing or on an SD2 card that is not inserted")), true));
    rows.push(("wrap-rows".into(), toggle(tr(ui, 1467, "Wrap Around Rows"),
        crate::library::WRAP_ROWS.load(std::sync::atomic::Ordering::Relaxed),
        tr(ui, 1468, "On Home, going past the last item returns to the first")), true));
    let mut source = nav(tr(ui, 792, "Metadata Source"), source_name(&st.ss_provider).to_owned());
    source.detail = source_detail(ui, &st.ss_provider).into();
    rows.push(("ss-source".into(), source, true));
    /* TheGamesDB has one cover a game. */
    if st.ss_provider != "thegamesdb" {
        let mut region = nav(tr(ui, 1419, "Cover Region"), region_label(ui, &st.ss_region));
        region.detail = tr(ui, 1420, "Box art from this region when there is one. Fill Missing Data changes the covers already downloaded.").into();
        rows.push(("ss-region".into(), region, true));
    }
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
    /* The libretro cheat collection (Quick Menu → Cheats uses it). */
    if st.cheats_job {
        rows.push(("cheats".into(), progress_row(tr(ui, 1242, "Downloading cheats"), st.cheats_progress), false));
    } else {
        let value = if st.cheats_present {
            tr_arg(ui, 1259, "Updated {0}", &days_ago(ui, st.cheats_updated))
        } else {
            tr(ui, 1258, "Not downloaded")
        };
        rows.push(("cheats".into(), row(tr(ui, 1257, "Cheat Collection"), value,
            tr(ui, 1260, "About 37 MB from libretro, with cheats for thousands of games")), true));
    }
    /* SYNCTHING (USER): its page and, once on, the setup guide. */
    let (value, detail, enabled) = crate::syncui::summary(ui);
    let mut sync = nav(tr(ui, 669, "Syncthing"), value);
    sync.detail = detail.into();
    rows.push(("sync".into(), sync, true));
    if enabled {
        let mut add = nav(tr(ui, 1028, "Add a Device"), String::new());
        add.detail = tr(ui, 1029, "Another console, a computer or a phone").into();
        rows.push(("sync-add".into(), add, true));
    }
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
        /* LIBRARY (DEVICE) starts after the user's rows, SYNCTHING (USER)
         * after the library. */
        let split = rows.iter().position(|r| r.0 == "scan")
            .unwrap_or(rows.len()) as i32;
        ui.set_gaming_split(split);
        ui.set_gaming_split2(rows.iter().position(|r| r.0 == "sync").unwrap_or(0) as i32);
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
    if let Some(m) = sync_model(ui.get_gaming_rows(), rows) {
        ui.set_gaming_rows(m);
    }
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
        let cheats = run_tool("/usr/bin/nuubos-libretroctl", &["cheats", "status"], None);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.emu = emu;
                st.cheats_present = kv(&cheats, "cheats") == "1";
                st.cheats_updated = kv(&cheats, "updated").parse().unwrap_or(0);
                st.ra_user = kv(&ra, "user").to_owned();
                st.ra_enabled = kv(&ra, "enabled") == "1";
                st.ra_hardcore = kv(&ra, "hardcore") == "1";
                st.ra_offline = kv(&ra, "offline") != "0";
                st.ss_user = kv(&ss, "user").to_owned();
                st.ss_available = kv(&ss, "available") == "1";
                st.ss_provider = kv(&ss, "provider").to_owned();
                st.ss_region = kv(&ss, "region").to_owned();
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
                "sync" => open_sync(ui),
                "sync-add" => crate::syncui::open_guide(ui),
                "bios" => open_bios(ui),
                "systems" => open_systems(ui),
                "cheats" if !STATE.with(|st| st.borrow().cheats_job) => update_cheats(ui),
                "boot_screens" | "slot_rotation" | "bezels" | "crt" | "retroarch_menu" => {
                    let on = STATE.with(|st| emu_get(&st.borrow(), &key) == "1");
                    emu_set(ui, &key, if on { "0" } else { "1" }.to_owned());
                }
                "slots" => {
                    let current = STATE.with(|st| emu_get(&st.borrow(), "slots"));
                    let options = ["3", "5", "10", "20", "50", "99"].iter().map(|n| (n.to_string(), n.to_string())).collect();
                    open_settings_choice(ui, "emu-slots", &tr(ui, 769, "Save State Slots"), options, &current);
                }
                "position" => {
                    let current = STATE.with(|st| emu_get(&st.borrow(), "position"));
                    let options = ["top", "center", "bottom"].iter().map(|v| (v.to_string(), position_label(ui, v))).collect();
                    open_settings_choice(ui, "emu-position", &tr(ui, 1408, "Picture Position"), options, &current);
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
                "unavailable" => {
                    let on = !crate::library::SHOW_UNAVAILABLE.load(std::sync::atomic::Ordering::Relaxed);
                    crate::library::SHOW_UNAVAILABLE.store(on, std::sync::atomic::Ordering::Relaxed);
                    render_gaming(ui);
                    thread::spawn(move || { let _ = library(&format!("SHOW_UNAVAILABLE\t{}", if on { 1 } else { 0 }), false); });
                }
                "wrap-rows" => {
                    let on = !crate::library::WRAP_ROWS.load(std::sync::atomic::Ordering::Relaxed);
                    crate::library::WRAP_ROWS.store(on, std::sync::atomic::Ordering::Relaxed);
                    render_gaming(ui);
                    thread::spawn(move || { let _ = library(&format!("WRAP_ROWS\t{}", if on { 1 } else { 0 }), false); });
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
                "ss-region" => {
                    let current = STATE.with(|st| st.borrow().ss_region.clone());
                    let options = REGIONS.iter().map(|id| (id.to_string(), region_label(ui, id))).collect();
                    open_settings_choice(ui, "game-region", &tr(ui, 1419, "Cover Region"), options, &current);
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

/* The libretro cheat collection: a job with its progress on the row. */
fn update_cheats(ui: &HomeWindow) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.cheats_job = true;
        st.cheats_progress = 0;
    });
    render_gaming(ui);
    job_with_progress(ui, vec!["cheats-update".into()], |ui, p| {
        STATE.with(|st| st.borrow_mut().cheats_progress = p);
        if ui.get_settings_view() == GAMING_VIEW {
            render_gaming(ui);
        }
    }, |ui, out| {
        STATE.with(|st| st.borrow_mut().cheats_job = false);
        if out.starts_with("ERR") {
            notice(ui, job_failure(ui, &out));
        }
        refresh_gaming(ui);
    });
}

/* "ERR <state> <reason>" of a download job, for the user. */
fn job_failure(ui: &HomeWindow, out: &str) -> String {
    match out.rsplit(' ').next().unwrap_or("") {
        "network" => tr(ui, 494, "Check the network connection"),
        "space" => tr(ui, 493, "Not enough free space"),
        "cancelled" => tr(ui, 596, "Cancelled"),
        _ => tr(ui, 93, "Failed"),
    }
}

/* Get More Emulators: the buildbot's cores for the game's system that are
 * not on the console (nuubos-libretroctl), as a dropdown. */
fn open_core_offer(ui: &HomeWindow) {
    let system = STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.cores_loading = true;
        st.details.system.clone()
    });
    render(ui);
    tool_then(ui, "/usr/bin/nuubos-libretroctl", vec!["cores".into(), system], None, |ui, out| {
        STATE.with(|st| st.borrow_mut().cores_loading = false);
        render(ui);
        if ui.get_settings_view() != 51 {
            return;
        }
        if !out.contains("end=1") {
            notice(ui, tr(ui, 494, "Check the network connection"));
            return;
        }
        let options: Vec<(String, String)> = out
            .lines()
            .filter_map(|l| l.strip_prefix("core="))
            .filter_map(|v| {
                let f: Vec<&str> = v.split('\t').collect();
                Some((f.first()?.to_string(), f.get(1).unwrap_or(f.first()?).to_string()))
            })
            .collect();
        if options.is_empty() {
            notice(ui, tr(ui, 1255, "No other emulators for this system"));
            return;
        }
        open_settings_choice(ui, "game-core-get", &tr(ui, 1253, "Get More Emulators"), options, "");
    });
}

/* Downloads the chosen core (job core-install, progress on its row), then
 * makes it the game's emulator. */
fn install_core(ui: &HomeWindow, core: String) {
    notice(ui, String::new());
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.core_job = core.clone();
        st.core_progress = 0;
    });
    render(ui);
    job_with_progress(ui, vec!["core-install".into(), core], |ui, p| {
        STATE.with(|st| st.borrow_mut().core_progress = p);
        render(ui);
    }, |ui, out| {
        let core = STATE.with(|st| std::mem::take(&mut st.borrow_mut().core_job));
        if out.starts_with("ERR") {
            notice(ui, job_failure(ui, &out));
            render(ui);
            return;
        }
        notice(ui, tr(ui, 1256, "Emulator installed"));
        game_set(ui, "core", core);
    });
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
                st.ra_offline = kv(&out, "offline") != "0";
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
        if address.is_empty() { String::new() } else { format!("https://{address}") }), true));
    /* Tailscale: its own page (nuubos-tsd). */
    let (ts_value, ts_detail) = crate::tsui::summary(ui);
    rows.push(("tailscale".into(), GenRow { navigates: true, ..row(tr(ui, 1328, "Tailscale"), ts_value, ts_detail) }, true));
    rows.push(("password".into(), row(tr(ui, 172, "Password"),
        if st.remote_password.is_empty() { "••••-••••-••••".into() } else { st.remote_password.clone() },
        if st.remote_password.is_empty() { tr(ui, 608, "Press to show") }
        else { tr(ui, 604, "Only on networks you trust. Never exposed to the Internet.") }), true));
    rows.push(("regen".into(), row(tr(ui, 606, "Regenerate Password"), String::new(),
        armed_detail(&st.armed, "regen", tr(ui, 191, "Press again to confirm"),
            tr(ui, 607, "The old password stops working"))), true));
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
    if let Some(m) = sync_model(ui.get_connectivity_services(), rows) {
        ui.set_connectivity_services(m);
    }
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
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.remote = remote;
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
        "tailscale" => open_tailscale(ui),
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
        _ => {}
    }
}

pub fn open_update(ui: &HomeWindow) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        /* A download or installation still running keeps its bar. */
        if st.update_job.is_empty() {
            st.busy = false;
        }
        st.update_result.clear();
    });
    set_back(ui, 19);
    show(ui, 63);
    refresh_update(ui);
}

/* Release notes from the manifest (one line, "\n" = new line, "\\" = \):
 * the non-empty lines, bullets dropped. */
fn release_notes(escaped: &str) -> Vec<String> {
    let mut text = String::new();
    let mut chars = escaped.chars();
    while let Some(c) = chars.next() {
        match (c, chars.clone().next()) {
            ('\\', Some('n')) => { text.push('\n'); chars.next(); }
            ('\\', Some('\\')) => { text.push('\\'); chars.next(); }
            _ => text.push(c),
        }
    }
    text.lines()
        .map(|l| l.trim().trim_start_matches(['-', '*', '•']).trim().to_owned())
        .filter(|l| !l.is_empty())
        .collect()
}

fn channel_name(ui: &HomeWindow, id: &str) -> String {
    match id {
        "rc" => tr(ui, 987, "RC"),
        "nightly" => tr(ui, 988, "Nightly"),
        _ => tr(ui, 986, "Stable"),
    }
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
        /* An installation needs the battery above 30 %. */
        if let Some(b) = apply.lines().find_map(|l| l.strip_prefix("battery=")) {
            pairs.push(("battery".into(), b.to_owned()));
        }
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().update = pairs);
            render(&ui);
        });
    });
}

pub fn open_sync(ui: &HomeWindow) {
    set_back(ui, GAMING_VIEW);
    show(ui, crate::syncui::VIEW);
}

/* Connectivity → SERVICES → Tailscale. */
pub fn open_tailscale(ui: &HomeWindow) {
    set_back(ui, 1);
    show(ui, crate::tsui::VIEW);
}

/* A row whose operation is running: a bar in place of its detail line,
 * the percentage as its value. */
fn progress_row(title: String, percent: i32) -> GenRow {
    GenRow {
        progress_shown: true,
        progress: percent.clamp(0, 100) as f32 / 100.0,
        ..row(title, format!("{}%", percent.clamp(0, 100)), String::new())
    }
}

/* Install and Restart (or Try Again): the job backs the console up to SD2,
 * writes the inactive system slot and restarts into it (EPIC-048), on the
 * full-screen operation screen (user request 2026-10-12: no game or other
 * action meanwhile); the restart takes over from the screen. */
pub fn start_install(ui: &HomeWindow) {
    start_update_job(ui, "install");
    crate::storageui::show_op_screen(ui, crate::storageui::OP_UPDATE);
    job_with_progress(ui, vec!["update-install".into()], |ui, p| {
        update_job_progress(ui, p);
        ui.set_op_progress(p);
    }, |ui, out| {
        if out.starts_with("ERR") {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.busy = false;
                st.update_job.clear();
            });
            crate::storageui::op_failed(ui);
            if out.contains("battery") {
                notice(ui, tr(ui, 1012, "Charge the battery above 30% to install"));
            }
        } else {
            ui.set_op_progress(100);
        }
        refresh_update(ui);
    });
}

fn start_update_job(ui: &HomeWindow, job: &str) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.busy = true;
        st.update_job = job.to_owned();
        st.update_progress = 0;
    });
    render(ui);
}

fn update_job_progress(ui: &HomeWindow, percent: i32) {
    STATE.with(|st| st.borrow_mut().update_progress = percent);
    render(ui);
}

/* Run a job and follow its progress (`jobctl run --progress`: jobd's own
 * events, no polling): `progress` gets 0..100 on each change, `done` the
 * job's final line (its result, or "ERR <state> <reason>"). */
pub(crate) fn job_with_progress(ui: &HomeWindow, args: Vec<String>, progress: fn(&HomeWindow, i32),
                                done: fn(&HomeWindow, String)) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut result = String::new();
        let mut all = vec!["run".to_owned(), "--progress".to_owned()];
        all.extend(args);
        if let Ok(mut child) = Command::new("/usr/bin/nuubos-jobctl").args(&all)
            .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn()
        {
            if let Some(out) = child.stdout.take() {
                for line in BufReader::new(out).lines().map_while(Result::ok) {
                    match line.strip_prefix("@progress ").and_then(|p| p.trim().parse::<i32>().ok()) {
                        Some(p) if p >= 0 => {
                            let weak = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(ui) = weak.upgrade() { progress(&ui, p.min(100)); }
                            });
                        }
                        Some(_) => {}
                        None => result = line.trim().to_owned(),
                    }
                }
            }
            let _ = child.wait();
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() { done(&ui, result); }
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
        (SYSTEMS_VIEW, k) if k.starts_with("sys:") => {
            let id = k[4..].to_owned();
            let name = STATE.with(|st| st.borrow().systems.iter().find(|s| s.0 == id).map(|s| s.1.clone()))
                .unwrap_or_default();
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.sys = Details { system: id.clone(), system_name: name.clone(), ..Default::default() };
                st.controls = false;
            });
            show(ui, SYSTEM_VIEW);
            load_system(ui, id, name);
        }
        (SYSTEM_VIEW, k) => {
            settings_confirm(ui, k);
        }
        (54, "ra-signin") => {
            open_system_keyboard(ui, &tr(ui, 285, "Username"), KEYBOARD_RA_NAME, 54, "text", "");
        }
        (54, "ra-enabled") | (54, "ra-hardcore") => {
            let (field, on) = STATE.with(|st| {
                let st = st.borrow();
                if key == "ra-enabled" { ("enabled", !st.ra_enabled) } else { ("hardcore", !st.ra_hardcore) }
            });
            /* Turning Hardcore on first explains what it takes away. */
            if field == "hardcore" && on {
                ui.set_settings_dialog_title(tr(ui, 907, "Turn On Hardcore Mode?").into());
                ui.set_settings_dialog_body(tr(ui, 908, "Achievements are earned as on the original console, with no help. While Hardcore Mode is on, save states and fast-forward are off: every game starts from the beginning and only the game's own saves are kept. RetroArch also blocks cheats, rewind and slow motion. You can turn it off at any time.").into());
                ui.set_settings_dialog_confirm(tr(ui, 909, "Turn On").into());
                ui.set_settings_dialog_index(0);
                ui.set_settings_dialog_open(true);
                return;
            }
            set_achievements(ui, field, on);
        }
        (54, "ra-offline") => {
            let on = STATE.with(|st| !st.borrow().ra_offline);
            set_achievements(ui, "offline", on);
        }
        (54, "ra-signout") => {
            if arm_or(ui, "ra-signout") {
                tool_then(ui, "/usr/bin/nuubos-achievementsctl", vec!["logout".into(), active_user()],
                    None, |ui, _| refresh_achievements(ui));
            }
        }
        (66, k) => crate::syncui::page_confirm(ui, k),
        (crate::tsui::VIEW, k) => crate::tsui::page_confirm(ui, k),
        (63, "update-notes") => {
            let (version, notes) = STATE.with(|st| {
                let st = st.borrow();
                let get = |k: &str| st.update.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
                (get("version"), get("notes"))
            });
            let text = release_notes(&notes).iter().map(|l| format!("•  {l}")).collect::<Vec<_>>().join("\n\n");
            ui.set_notes_title(format!("{} • nuubOS {version}", tr(ui, 1005, "What's New")).into());
            ui.set_notes_text(text.into());
            ui.set_notes_scroll(0.0);
            ui.set_notes_open(true);
        }
        (63, "update-channel") => {
            let current = STATE.with(|st| st.borrow().update.iter().find(|(k, _)| k == "channel").map(|(_, v)| v.clone()))
                .unwrap_or_default();
            let options = ["stable", "rc", "nightly"].iter().map(|id| (id.to_string(), channel_name(ui, id))).collect();
            open_settings_choice(ui, "update-channel", &tr(ui, 985, "Update Channel"), options, &current);
        }
        (63, "update-auto") => {
            let on = STATE.with(|st| st.borrow().update.iter().any(|(k, v)| k == "auto" && v == "1"));
            tool_then(ui, "/usr/bin/nuubos-updatectl", vec!["auto".into(), if on { "off".into() } else { "on".into() }], None,
                |ui, _| refresh_update(ui));
        }
        (63, "update-check") => {
            STATE.with(|st| st.borrow_mut().busy = true);
            render(ui);
            tool_then(ui, "/usr/bin/nuubos-updatectl", vec!["check".into()], None, |ui, out| {
                /* "OK none": nothing published on the channel yet. */
                let text = if out.starts_with("OK current") || out.starts_with("OK none") { tr(ui, 489, "Up to date") }
                    else if out.contains("server") { tr(ui, 1013, "The update service cannot be reached. Try again later") }
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
            start_update_job(ui, "download");
            job_with_progress(ui, vec!["update-download".into()], update_job_progress, |ui, out| {
                STATE.with(|st| {
                    let mut st = st.borrow_mut();
                    st.busy = false;
                    st.update_job.clear();
                });
                if out.starts_with("ERR") {
                    notice(ui, if out.contains("checksum") || out.contains("signature") {
                        tr(ui, 656, "The update could not be verified") }
                        else if out.contains("space") { tr(ui, 493, "Not enough free space") }
                        else if out.contains("server") { tr(ui, 1013, "The update service cannot be reached. Try again later") }
                        else { tr(ui, 93, "Failed") });
                }
                refresh_update(ui);
            });
        }
        (63, "update-install") if STATE.with(|st| st.borrow().update.iter().any(|(k, v)| k == "battery" && v == "low")) => {
            notice(ui, tr(ui, 1012, "Charge the battery above 30% to install"));
            refresh_update(ui);
        }
        (63, "update-install") => {
            if arm_or(ui, "update-install") {
                start_install(ui);
            }
        }
        _ => {}
    }
}

/* Input on Settings pages 52-62 (from handle_settings_action). */
pub fn handle_page(ui: &HomeWindow, view: i32, action: &str) {
    if view == crate::storageui::STORAGE_VIEW || view == crate::storageui::CONTENT_VIEW
        || view == crate::storageui::DATA_VIEW {
        crate::storageui::handle_page(ui, view, action);
        return;
    }
    if (58..=62).contains(&view) || view == crate::filesui::RECOVERY_VIEW {
        crate::filesui::handle_page(ui, view, action);
        return;
    }
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => {
            STATE.with(|st| st.borrow_mut().bios_system.clear());
            match view {
                53 => show(ui, 52),
                SYSTEM_VIEW if STATE.with(|st| st.borrow().controls) => show_controls(ui, false),
                SYSTEM_VIEW => {
                    /* Back on the system's row, its emulator refreshed. */
                    let id = STATE.with(|st| st.borrow().sys.system.clone());
                    open_systems(ui);
                    STATE.with(|st| st.borrow_mut().systems_focus = format!("sys:{id}"));
                }
                _ => {
                    let back = STATE.with(|st| st.borrow().back);
                    navigate_settings_view(ui, if back > 0 { back } else { 19 });
                    if back == 1 {
                        /* The SERVICES rows. */
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

fn set_achievements(ui: &HomeWindow, field: &str, on: bool) {
    tool_then(ui, "/usr/bin/nuubos-achievementsctl",
        vec!["set".into(), active_user(), field.into(), if on { "1".into() } else { "0".into() }],
        None, |ui, _| refresh_achievements(ui));
}

/* The release notes screen: Up/Down scroll a few lines, Left/Right a page,
 * Back or A closes. */
pub fn notes_action(ui: &HomeWindow, action: &str) {
    let line = 60.0;
    let page = (ui.get_notes_page() - line).max(line);
    let max = ui.get_notes_scroll_max();
    let scroll = ui.get_notes_scroll().min(max);
    let next = match action {
        "menu_up" => scroll - line,
        "menu_down" => scroll + line,
        "menu_left" => scroll - page,
        "menu_right" => scroll + page,
        "menu_back" | "menu_confirm" => {
            ui.set_notes_open(false);
            return;
        }
        _ => return,
    };
    ui.set_notes_scroll(next.clamp(0.0, max));
}

/* Settings confirmation dialog (only Turn On Hardcore Mode for now):
 * left/right/up/down move between Cancel and the action, Back cancels. */
pub fn dialog_action(ui: &HomeWindow, action: &str) {
    match action {
        "menu_left" | "menu_right" | "menu_up" | "menu_down" => {
            ui.set_settings_dialog_index(1 - ui.get_settings_dialog_index());
        }
        "menu_back" => ui.set_settings_dialog_open(false),
        "menu_confirm" => {
            ui.set_settings_dialog_open(false);
            if ui.get_settings_dialog_index() == 1 {
                set_achievements(ui, "hardcore", true);
            }
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
            navigate_settings_view(ui, 51);
            render(ui);
            focus(ui, "search-name");
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
