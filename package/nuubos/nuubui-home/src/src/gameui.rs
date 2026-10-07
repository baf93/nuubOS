/*
 * Game pages of nuubUI, all built from the owning services' data on the
 * generic rows of home.slint (GenRow):
 *
 *   50 Game Details (EPIC-016)      nuubos-libraryd DETAILS + nuubos-emud
 *   51 Game Settings (EPIC-018)     nuubos-emud GAME_SETTINGS / GAME_SET
 *   52/53 BIOS Files (EPIC-033)     nuubos-biosctl
 *   54 RetroAchievements (EPIC-017) nuubos-achievementsctl
 *   55 Game Metadata (EPIC-015)     nuubos-scraper (account), nuubos-jobd
 *   56 Hidden Games (EPIC-011)      nuubos-libraryd GAMES hidden / HIDE
 *   57 Remote Services (EPIC-037/039) nuubos-remotectl
 *   63 Software Update (EPIC-048)   nuubos-updatectl, nuubos-update-apply
 *   66 Syncthing (EPIC-034)         nuubos-syncctl (per user)
 *   64 Online Services (EPIC-055)   every optional network service, its
 *                                   state and the page that controls it
 *
 * 50 and 51 are a Settings shell of their own (details-active), opened from
 * Home with the Details action; 52-56 are Settings pages. This module keeps
 * no product state: every row reflects the last service reply and every
 * action is a service request. Long work (scraping) is a nuubos-jobd job
 * whose progress is the job's Live Notification.
 */

use crate::{
    guarded_scroll_offset, handle_settings_action, move_model_selection, navigate_settings_view,
    open_settings_choice, open_system_keyboard, play_ui_sound, tr, tr_arg, write_ui_context,
    GenRow, HomeWindow,
};
use slint::{ComponentHandle, Image, ModelRc, VecModel};
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
    available: bool,
    favorite: bool,
    hidden: bool,
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
    ss_name_pending: String,
    hidden: Vec<(String, String, String)>,
    remote: Vec<(String, String)>,
    remote_password: String,
    update: Vec<(String, String)>,
    update_result: String,
    online: Vec<(String, String)>,
    sync: Vec<(String, String)>,
    /* Row keys of the page on screen, in GenRow order. */
    keys: Vec<String>,
    actionable: Vec<bool>,
    /* The destructive row waiting for its confirming press. */
    armed: String,
    busy: bool,
    /* Where Back goes from pages 52-57 and 63-64. */
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
                    d.last = f[5].parse().unwrap_or(0);
                    d.time = f[6].parse().unwrap_or(0);
                    d.available = f[7] == "1";
                    d.favorite = f[8] == "1";
                }
            }
            "system_name" => d.system_name = value.into(),
            "sessions" => d.sessions = value.parse().unwrap_or(0),
            "path" => d.path = value.into(),
            "hidden" => d.hidden = value == "1",
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
        50 => {
            if d.available {
                rows.push(("play".into(), row(tr(ui, 413, "Play"), String::new(), String::new()), true));
            }
            rows.push(("favorite".into(), toggle(tr(ui, 412, "Favorite"), d.favorite, String::new()), true));
            let in_count = d.members.iter().filter(|m| m.member).count();
            let names: Vec<&str> = d.members.iter().filter(|m| m.member).map(|m| m.name.as_str()).collect();
            let mut collections = nav(tr(ui, 537, "Collections"), if in_count > 0 { in_count.to_string() } else { String::new() });
            collections.detail = names.join(", ").into();
            rows.push(("collections".into(), collections, true));
            if !d.cores.is_empty() {
                rows.push(("settings".into(), nav(tr(ui, 524, "Game Settings"), String::new()), true));
            }
            match d.meta("match") {
                "review" => {
                    let mut r = row(tr(ui, 562, "Accept Match"), d.meta("title").to_owned(), tr(ui, 561, "Check this match"));
                    r.enabled = !st.busy;
                    rows.push(("accept".into(), r, !st.busy));
                    rows.push(("reject".into(), row(tr(ui, 563, "Reject Match"), String::new(), String::new()), !st.busy));
                }
                "" => rows.push(("metadata".into(), row(tr(ui, 559, "Get Metadata"),
                    if st.busy { tr(ui, 571, "Getting metadata") } else { String::new() }, String::new()), !st.busy && d.available)),
                _ => rows.push(("metadata".into(), row(tr(ui, 560, "Update Metadata"),
                    if st.busy { tr(ui, 571, "Getting metadata") } else { String::new() }, String::new()), !st.busy && d.available)),
            }
            if !d.available {
                rows.push(("info".into(), row(tr(ui, 205, "Unavailable"), String::new(), d.path.clone()), false));
            } else if d.bios_missing {
                rows.push(("bios".into(), nav(tr(ui, 583, "BIOS missing for this system"), String::new()), true));
            }
            if d.last > 0 {
                rows.push(("info".into(), row(tr(ui, 551, "Last Played"), days_ago(ui, d.last), String::new()), false));
                rows.push(("info".into(), row(tr(ui, 552, "Time Played"), duration(ui, d.time), String::new()), false));
                rows.push(("info".into(), row(tr(ui, 550, "Sessions"), d.sessions.to_string(), String::new()), false));
            } else {
                rows.push(("info".into(), row(tr(ui, 551, "Last Played"), tr(ui, 405, "Never played"), String::new()), false));
            }
            for (key, idx, fallback) in [
                ("developer", 553, "Developer"),
                ("publisher", 554, "Publisher"),
                ("release", 555, "Release Date"),
                ("genre", 556, "Genre"),
                ("players", 557, "Players"),
            ] {
                let v = d.meta(key);
                if !v.is_empty() {
                    rows.push(("info".into(), row(tr(ui, idx, fallback), v.to_owned(), String::new()), false));
                }
            }
            if !d.core.is_empty() {
                rows.push(("info".into(), row(tr(ui, 525, "Emulator"), d.core.clone(), String::new()), false));
            }
            rows.push(("info".into(), row(tr(ui, 558, "File"), String::new(), d.path.clone()), false));
            rows.push(("hide".into(), toggle(tr(ui, 540, "Hide Game"), d.hidden, tr(ui, 543, "Only for you; the game stays available to other users")), true));
            if d.last > 0 {
                rows.push(("stats".into(), row(tr(ui, 549, "Reset Play Statistics"), String::new(),
                    armed_detail(&st.armed, "stats", tr(ui, 598, "Press again to reset"), String::new())), true));
            }
            if d.available {
                rows.push(("delete".into(), row(tr(ui, 544, "Delete Game"), String::new(),
                    armed_detail(&st.armed, "delete", tr(ui, 546, "Press again to delete permanently"),
                        tr(ui, 545, "Permanently deletes the game file for every user. Saves are kept."))), true));
            }
        }
        51 => {
            let core = if d.core_override.is_empty() { default_label(ui, &d.core_default) } else { d.core_override.clone() };
            rows.push(("core".into(), nav(tr(ui, 525, "Emulator"), core), d.cores.len() > 1));
            let aspect = if d.aspect.is_empty() { default_label(ui, &tr(ui, 529, "As the game")) } else { aspect_label(ui, &d.aspect) };
            rows.push(("aspect".into(), nav(tr(ui, 526, "Aspect Ratio"), aspect), true));
            let filter = if d.filter.is_empty() { default_label(ui, &tr(ui, 532, "Sharp")) } else { filter_label(ui, &d.filter) };
            rows.push(("filter".into(), nav(tr(ui, 527, "Video Filter"), filter), true));
            rows.push(("reset".into(), row(tr(ui, 535, "Reset Game Settings"), String::new(),
                armed_detail(&st.armed, "reset", tr(ui, 598, "Press again to reset"),
                    tr(ui, 536, "Changes apply the next time the game starts"))), true));
        }
        52 => {
            for s in &st.bios {
                let mut r = nav(s.name.clone(), bios_state_label(ui, &s.state));
                r.detail = tr(ui, 582, "{0} of {1} present")
                    .replace("{0}", &s.present.to_string())
                    .replace("{1}", &s.total.to_string())
                    .into();
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
        55 => {
            rows.push(("info".into(), row(tr(ui, 564, "Game Metadata"),
                if st.ss_available { tr(ui, 67, "Available") } else { tr(ui, 205, "Unavailable") },
                if st.ss_available { tr(ui, 569, "Uses the Internet only when you ask. Games without metadata keep their file name.") }
                else { tr(ui, 570, "The metadata service is not available in this build") }), false));
            if st.ss_user.is_empty() {
                rows.push(("ss-signin".into(), row(tr(ui, 565, "ScreenScraper Account"), tr(ui, 566, "Not signed in"), String::new()), st.ss_available));
            } else {
                rows.push(("ss-signout".into(), row(tr(ui, 565, "ScreenScraper Account"), st.ss_user.clone(),
                    armed_detail(&st.armed, "ss-signout", tr(ui, 191, "Press again to confirm"), tr(ui, 567, "Sign Out"))), true));
            }
            rows.push(("bulk".into(), row(tr(ui, 568, "Get Metadata for All Games"),
                if st.busy { tr(ui, 571, "Getting metadata") } else { String::new() }, String::new()), st.ss_available && !st.busy));
        }
        56 => {
            if st.hidden.is_empty() {
                let mut r = row(tr(ui, 542, "No hidden games"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for (id, title, system) in &st.hidden {
                rows.push((format!("unhide:{id}"), toggle(title.clone(), true, system.clone()), true));
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
        64 => {
            let get = |k: &str| st.online.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
            rows.push(("info".into(), row(tr(ui, 661, "Online Services"), String::new(),
                tr(ui, 662, "nuubOS works fully offline. These services use the network only when you enable or start them.")), false));
            let meta = if get("ss_provider") != "available" { tr(ui, 205, "Unavailable") }
                else if get("ss_user").is_empty() { tr(ui, 566, "Not signed in") } else { get("ss_user") };
            rows.push(("open-meta".into(), nav(tr(ui, 564, "Game Metadata"), meta), true));
            let ra = if get("ra_user").is_empty() { tr(ui, 566, "Not signed in") } else { get("ra_user") };
            rows.push(("open-ra".into(), nav(tr(ui, 584, "RetroAchievements"), ra), true));
            rows.push(("open-update".into(), nav(tr(ui, 651, "Software Update"), tr(ui, 663, "Only when you check")), true));
            let remote: Vec<&str> = [("ssh", "SSH"), ("smb", "SMB"), ("web", "Web")]
                .iter()
                .filter(|(k, _)| get(k) == "1")
                .map(|(_, n)| *n)
                .collect();
            rows.push(("open-remote".into(), nav(tr(ui, 599, "Remote Services"),
                if remote.is_empty() { tr(ui, 54, "Off") } else { remote.join(", ") }), true));
            let shares = get("shares");
            rows.push(("info".into(), row(tr(ui, 611, "Network Shares"),
                if shares.is_empty() || shares == "0" { tr(ui, 664, "Not in use") } else { shares }, String::new()), false));
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
        57 => {
            let get = |k: &str| st.remote.iter().find(|(key, _)| key == k).map(|(_, v)| v.clone()).unwrap_or_default();
            let address = get("address");
            rows.push(("info".into(), row(tr(ui, 599, "Remote Services"), String::new(),
                tr(ui, 604, "Only on networks you trust. Never exposed to the Internet.")), false));
            rows.push(("svc:ssh".into(), toggle(tr(ui, 600, "SSH, SCP and SFTP"), get("ssh") == "1",
                tr(ui, 601, "Sign in as root with the device password")), true));
            rows.push(("svc:smb".into(), toggle(tr(ui, 602, "File Sharing (SMB)"), get("smb") == "1",
                if address.is_empty() { String::new() } else { format!("\\\\{address}\\nuubOS") }), true));
            rows.push(("svc:web".into(), toggle(tr(ui, 603, "Web Administration"), get("web") == "1",
                if address.is_empty() { String::new() } else { format!("http://{address}") }), true));
            rows.push(("info".into(), row(tr(ui, 605, "Address"),
                if address.is_empty() { tr(ui, 159, "Not connected") } else { address.clone() }, String::new()), false));
            rows.push(("info".into(), row(tr(ui, 285, "Username"), get("user"), String::new()), false));
            rows.push(("password".into(), row(tr(ui, 172, "Password"),
                if st.remote_password.is_empty() { "••••-••••-••••".into() } else { st.remote_password.clone() },
                if st.remote_password.is_empty() { tr(ui, 608, "Press to show") } else { String::new() }), true));
            rows.push(("regen".into(), row(tr(ui, 606, "Regenerate Password"), String::new(),
                armed_detail(&st.armed, "regen", tr(ui, 191, "Press again to confirm"),
                    tr(ui, 607, "The old password stops working"))), true));
        }
        _ => {}
    }
    rows
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
    if !(50..=57).contains(&view) && !(63..=64).contains(&view) && view != 66 {
        return;
    }
    let rows = STATE.with(|st| build_rows(ui, &st.borrow(), view));
    let section = match view {
        50 => STATE.with(|st| st.borrow().details.system_name.clone()),
        51 => tr(ui, 524, "Game Settings"),
        52 => tr(ui, 576, "BIOS Files"),
        53 => STATE.with(|st| {
            let st = st.borrow();
            st.bios.iter().find(|s| s.id == st.bios_system).map(|s| s.name.clone()).unwrap_or_default()
        }),
        54 => tr(ui, 584, "RetroAchievements"),
        55 => tr(ui, 564, "Game Metadata"),
        57 | 63 | 64 => tr(ui, 8, "System"),
        66 => tr(ui, 669, "Syncthing"),
        _ => tr(ui, 541, "Hidden Games"),
    };
    apply_rows(ui, rows, section);
}

/* Show rows on the generic page: (key, row, actionable). */
pub(crate) fn apply_rows(ui: &HomeWindow, rows: Vec<(String, GenRow, bool)>, section: String) {
    let count = rows.len() as i32;
    let index = ui.get_gen_index().clamp(0, (count - 1).max(0));
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.keys = rows.iter().map(|r| r.0.clone()).collect();
        st.actionable = rows.iter().map(|r| r.2).collect();
    });
    let select = rows.get(index as usize).map(|r| r.2).unwrap_or(false);
    ui.set_gen_rows(ModelRc::from(Rc::new(VecModel::from(rows.into_iter().map(|r| r.1).collect::<Vec<_>>()))));
    ui.set_gen_index(index);
    ui.set_gen_select(select);
    ui.set_gen_scroll(guarded_scroll_offset(index, count, ui.get_settings_list_visible_rows(), ui.get_gen_scroll()));
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

pub(crate) fn focused_key(ui: &HomeWindow) -> String {
    let i = ui.get_gen_index().max(0) as usize;
    STATE.with(|st| st.borrow().keys.get(i).cloned().unwrap_or_default())
}

/* ---------------------------------------------------------------- */
/* Game Details shell                                               */
/* ---------------------------------------------------------------- */

/* Fetch everything the Details page aggregates, off the UI thread. */
fn load_details(ui: &HomeWindow, id: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut d = Details::default();
        parse_details(&library(&format!("DETAILS\t{id}"), true), &mut d);
        if d.id.is_empty() {
            return;
        }
        parse_game_settings(&emulation(&format!("GAME_SETTINGS\t{}\t{}", d.id, d.system), true), &mut d);
        let bios = run_tool("/usr/bin/nuubos-biosctl", &["status", &d.system], None);
        d.bios_missing = parse_bios(&bios).0.iter().any(|s| s.state == "missing" || s.state == "invalid");
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            if !ACTIVE.load(Ordering::SeqCst) {
                return;
            }
            let title = if d.meta("title").is_empty() { d.title.clone() } else { d.meta("title").to_owned() };
            ui.set_details_title(title.into());
            ui.set_details_subtitle(d.system_name.clone().into());
            ui.set_details_description(d.meta("description").replace("\\n", "\n").into());
            STATE.with(|st| st.borrow_mut().details = d);
            render(&ui);
        });
    });
}

/* Home: the Details action on a game card. */
pub fn open_details(ui: &HomeWindow, id: &str, title: &str, cover: Option<Image>) {
    ACTIVE.store(true, Ordering::SeqCst);
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.details = Details { id: id.to_owned(), title: title.to_owned(), available: true, ..Default::default() };
        st.busy = false;
    });
    ui.set_details_active(true);
    ui.set_details_title(title.into());
    ui.set_details_subtitle("".into());
    ui.set_details_description("".into());
    ui.set_details_has_cover(cover.is_some());
    ui.set_details_cover(cover.unwrap_or_default());
    show(ui, 50);
    ui.set_settings_open(true);
    write_ui_context("settings");
    load_details(ui, id.to_owned());
}

pub fn leave_details(ui: &HomeWindow) {
    ACTIVE.store(false, Ordering::SeqCst);
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_details_active(false);
    ui.set_details_cover(Image::default());
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
        "unavailable" => tr(ui, 570, "The metadata service is not available in this build"),
        "network" => tr(ui, 494, "Check the network connection"),
        "cancelled" => tr(ui, 596, "Cancelled"),
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
    let mut options = vec![(String::new(), default_label(ui, &tr(ui, 529, "As the game")))];
    for v in ["core", "4:3", "16:9", "full", "integer"] {
        options.push((v.to_owned(), aspect_label(ui, v)));
    }
    open_settings_choice(ui, "game-aspect", &tr(ui, 526, "Aspect Ratio"), options, &d.aspect);
}

fn open_filter_choice(ui: &HomeWindow) {
    let d = STATE.with(|st| st.borrow().details.clone());
    let mut options = vec![(String::new(), default_label(ui, &tr(ui, 532, "Sharp")))];
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
        "favorite" => details_command(ui, format!("FAVORITE\t{}\t{}", d.id, if d.favorite { 0 } else { 1 })),
        "hide" => details_command(ui, format!("HIDE\t{}\t{}", d.id, if d.hidden { 0 } else { 1 })),
        "collections" => open_collections(ui),
        "settings" => show(ui, 51),
        "bios" => {
            STATE.with(|st| st.borrow_mut().bios_system = d.system.clone());
            leave_details(ui);
            open_bios(ui);
        }
        "metadata" => scrape_game(ui, d.id.clone(), !d.meta("match").is_empty()),
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
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => {
            if ui.get_settings_view() == 51 {
                show(ui, 50);
                render(ui);
            } else {
                leave_details(ui);
            }
        }
        "menu_confirm" => {
            let key = focused_key(ui);
            details_confirm(ui, &key);
        }
        _ => {}
    }
}

/* ---------------------------------------------------------------- */
/* Settings pages 52-57, 63-64                                      */
/* ---------------------------------------------------------------- */

/* Settings → Gaming, the home of pages 52-56. */
pub const GAMING_VIEW: i32 = 68;

/* Pages opened from Online Services go back there, the others to their
 * category page. */
fn set_back(ui: &HomeWindow, category: i32) {
    let back = if ui.get_settings_view() == 64 { 64 } else { category };
    STATE.with(|st| st.borrow_mut().back = back);
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
    notice(ui, tr(ui, 581, "Copy BIOS files to the bios folder on the SD card. nuubOS never includes them."));
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

pub fn open_metadata(ui: &HomeWindow) {
    set_back(ui, GAMING_VIEW);
    show(ui, 55);
    refresh_metadata(ui);
}

fn refresh_metadata(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let out = run_tool("/usr/bin/nuubos-scraper", &["account", "status"], None);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.ss_user = kv(&out, "user").to_owned();
                st.ss_available = kv(&out, "provider") == "available";
            });
            render(&ui);
        });
    });
}

pub fn open_remote(ui: &HomeWindow) {
    set_back(ui, 19);
    STATE.with(|st| st.borrow_mut().remote_password.clear());
    show(ui, 57);
    refresh_remote(ui);
}

fn refresh_remote(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let out = run_tool("/usr/sbin/nuubos-remotectl", &["status"], None);
        let pairs: Vec<(String, String)> = out
            .lines()
            .filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_owned(), v.to_owned())))
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().remote = pairs);
            render(&ui);
        });
    });
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

pub fn open_online(ui: &HomeWindow) {
    set_back(ui, 19);
    show(ui, 64);
    let weak = ui.as_weak();
    let user = ui.get_active_user_id().to_string();
    thread::spawn(move || {
        let mut pairs: Vec<(String, String)> = Vec::new();
        let ss = run_tool("/usr/bin/nuubos-scraper", &["account", "status"], None);
        pairs.push(("ss_user".into(), kv(&ss, "user").to_owned()));
        pairs.push(("ss_provider".into(), kv(&ss, "provider").to_owned()));
        if !user.is_empty() {
            let ra = run_tool("/usr/bin/nuubos-achievementsctl", &["status", &user], None);
            pairs.push(("ra_user".into(), kv(&ra, "user").to_owned()));
        }
        let remote = run_tool("/usr/sbin/nuubos-remotectl", &["status"], None);
        for k in ["ssh", "smb", "web"] {
            pairs.push((k.into(), kv(&remote, k).to_owned()));
        }
        let shares = run_tool("/usr/sbin/nuubos-sharesctl", &["list"], None);
        pairs.push(("shares".into(), shares.lines().filter(|l| l.starts_with("share=")).count().to_string()));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().online = pairs);
            render(&ui);
        });
    });
}

pub fn open_sync(ui: &HomeWindow) {
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

pub fn open_hidden(ui: &HomeWindow) {
    set_back(ui, GAMING_VIEW);
    show(ui, 56);
    refresh_hidden(ui);
}

fn refresh_hidden(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = library("GAMES\thidden", true);
        let games: Vec<(String, String, String)> = reply
            .lines()
            .filter_map(|l| l.strip_prefix("game="))
            .filter_map(|v| {
                let f: Vec<&str> = v.split('\t').collect();
                (f.len() >= 3).then(|| (f[0].to_owned(), f[2].to_owned(), f[1].to_owned()))
            })
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().hidden = games);
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
        (55, "ss-signin") => {
            open_system_keyboard(ui, &tr(ui, 285, "Username"), KEYBOARD_SS_NAME, 55, "text", "");
        }
        (55, "ss-signout") => {
            if arm_or(ui, "ss-signout") {
                tool_then(ui, "/usr/bin/nuubos-scraper", vec!["account".into(), "clear".into()],
                    None, |ui, _| refresh_metadata(ui));
            }
        }
        (55, "bulk") => {
            STATE.with(|st| st.borrow_mut().busy = true);
            render(ui);
            tool_then(ui, "/usr/bin/nuubos-jobctl", vec!["run".into(), "scrape-bulk".into(), "all".into()],
                None, |ui, out| {
                    STATE.with(|st| st.borrow_mut().busy = false);
                    let text = scrape_outcome(ui, out.trim());
                    notice(ui, text);
                    render(ui);
                });
        }
        (57, k) if k.starts_with("svc:") => {
            let svc = k[4..].to_owned();
            let on = STATE.with(|st| st.borrow().remote.iter().any(|(key, v)| *key == svc && v == "1"));
            tool_then(ui, "/usr/sbin/nuubos-remotectl", vec!["set".into(), svc, if on { "0".into() } else { "1".into() }],
                None, |ui, _| refresh_remote(ui));
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
        (64, "open-meta") => open_metadata(ui),
        (64, "open-ra") => open_achievements(ui),
        (64, "open-update") => open_update(ui),
        (64, "open-remote") => open_remote(ui),
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
        (57, "password") => {
            /* The local console is the trusted place to read it (EPIC-039). */
            tool_then(ui, "/usr/sbin/nuubos-remotectl", vec!["credential".into(), "show".into()], None, |ui, out| {
                STATE.with(|st| st.borrow_mut().remote_password = out.trim().to_owned());
                render(ui);
            });
        }
        (57, "regen") => {
            if arm_or(ui, "regen") {
                tool_then(ui, "/usr/sbin/nuubos-remotectl", vec!["credential".into(), "regenerate".into()], None, |ui, _| {
                    STATE.with(|st| st.borrow_mut().remote_password.clear());
                    refresh_remote(ui);
                });
            }
        }
        (56, k) if k.starts_with("unhide:") => {
            let id = k[7..].to_owned();
            let weak = ui.as_weak();
            thread::spawn(move || {
                let _ = library(&format!("HIDE\t{id}\t0"), false);
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        refresh_hidden(&ui);
                    }
                });
            });
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
                66 => navigate_settings_view(ui, 24),
                _ => {
                    let back = STATE.with(|st| st.borrow().back);
                    navigate_settings_view(ui, if back > 0 { back } else { 19 });
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
    let value = value.trim().to_owned();
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
                navigate_settings_view(ui, if purpose == KEYBOARD_RA_NAME { 54 } else { 55 });
                render(ui);
                return;
            }
            let ra = purpose == KEYBOARD_RA_NAME;
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                if ra { st.ra_name_pending = value.clone() } else { st.ss_name_pending = value.clone() }
            });
            open_system_keyboard(ui, &tr(ui, 172, "Password"),
                if ra { KEYBOARD_RA_PASSWORD } else { KEYBOARD_SS_PASSWORD }, if ra { 54 } else { 55 }, "password", "");
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
        KEYBOARD_SS_PASSWORD => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, 55);
            let name = STATE.with(|st| std::mem::take(&mut st.borrow_mut().ss_name_pending));
            if value.is_empty() || name.is_empty() {
                render(ui);
                return;
            }
            tool_then(ui, "/usr/bin/nuubos-scraper", vec!["account".into(), "set".into(), name], Some(value),
                |ui, _| refresh_metadata(ui));
        }
        _ => {}
    }
}

/* Re-render after a language change. */
pub fn relocalize(ui: &HomeWindow) {
    render(ui);
}
