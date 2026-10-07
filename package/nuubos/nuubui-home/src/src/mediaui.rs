/*
 * Media application of nuubUI (EPIC-027 Jellyfin client, EPIC-028 local
 * video and music), on the generic rows of home.slint in the shared shell
 * (settings-view 65). nuubos-mediad owns playback (mpv, Cedrus decoding,
 * per-user resume); nuubos-jellyfinctl owns the server account; folders
 * are listed by nuubos-filesctl. While something plays, the controller's
 * actions are player commands (route_input_action → handle_player).
 */

use crate::gameui::{apply_rows, arm_or, armed, armed_detail, focused_key, move_selection, nav, notice, row,
    run_tool, show, tool_then};
use crate::{handle_settings_action, navigate_settings_view, on_game_session, open_system_keyboard,
    play_ui_sound, tr, write_ui_context, GenRow, HomeWindow};
use slint::{ComponentHandle, ModelRc};
use std::cell::RefCell;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

pub const KEYBOARD_SERVER: i32 = 22;
pub const KEYBOARD_JF_USER: i32 = 23;
pub const KEYBOARD_JF_PASSWORD: i32 = 24;

const MEDIA_SOCKET: &str = "/run/nuubos/mediad.sock";
const FILESCTL: &str = "/usr/bin/nuubos-filesctl";
const JELLYFIN: &str = "/usr/bin/nuubos-jellyfinctl";

/* The Media shell owns Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);
/* Something plays: controller actions drive the player. */
pub static PLAYING: AtomicBool = AtomicBool::new(false);
static PLAYING_VIDEO: AtomicBool = AtomicBool::new(false);
/* Jellyfin item being played, for the stop position report. */
static JELLYFIN_ITEM: Mutex<String> = Mutex::new(String::new());

const VIDEO_EXTS: &[&str] = &["mkv", "mp4", "m4v", "avi", "webm", "mov", "ts", "mpg", "mpeg"];
const AUDIO_EXTS: &[&str] = &["mp3", "flac", "ogg", "opus", "m4a", "aac", "wav"];

#[derive(Clone, Default)]
struct Entry {
    key: String,
    title: String,
    detail: String,
    folder: bool,
}

#[derive(Default)]
struct State {
    /* "" = locations; "/path" = local folder; "jf:" = Jellyfin views;
     * "jf:<id>" = a Jellyfin folder. Parents for Back. */
    cwd: String,
    stack: Vec<String>,
    entries: Vec<Entry>,
    jf_name: String,
    jf_signed: bool,
    draft: (String, String),
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

fn ext_of(name: &str) -> String {
    name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default()
}

fn media_kind(name: &str) -> Option<bool> {
    let e = ext_of(name);
    if VIDEO_EXTS.contains(&e.as_str()) {
        Some(true)
    } else if AUDIO_EXTS.contains(&e.as_str()) {
        Some(false)
    } else {
        None
    }
}

fn duration(seconds: f64) -> String {
    let s = seconds.max(0.0) as i64;
    if s >= 3600 {
        format!("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60)
    } else {
        format!("{}:{:02}", s / 60, s % 60)
    }
}

fn player(line: &str) -> String {
    let Ok(mut stream) = UnixStream::connect(MEDIA_SOCKET) else { return String::new() };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(3)));
    let _ = stream.write_all(line.as_bytes());
    let _ = stream.write_all(b"\n");
    let mut reply = String::new();
    let _ = BufReader::new(stream).read_line(&mut reply);
    reply
}

fn send(line: String) {
    thread::spawn(move || {
        let _ = player(&line);
    });
}

/* ---------------------------------------------------------------- */
/* Rows                                                             */
/* ---------------------------------------------------------------- */

fn build_rows(ui: &HomeWindow, st: &State) -> Vec<(String, GenRow, bool)> {
    let mut rows: Vec<(String, GenRow, bool)> = Vec::new();
    if st.cwd == "jf:" && !st.jf_signed {
        rows.push(("jf-signin".into(), row(tr(ui, 587, "Sign In"), tr(ui, 566, "Not signed in"), String::new()), true));
        return rows;
    }
    if st.cwd == "jf:" {
        rows.push(("jf-signout".into(), row(tr(ui, 567, "Sign Out"), st.jf_name.clone(),
            armed_detail(&armed(), "jf-signout", tr(ui, 191, "Press again to confirm"), String::new())), true));
    }
    if st.entries.is_empty() && !st.cwd.is_empty() {
        let mut r = row(tr(ui, 621, "Empty folder"), String::new(), String::new());
        r.enabled = false;
        rows.push(("info".into(), r, false));
    }
    for e in &st.entries {
        let mut r = if e.folder { nav(e.title.clone(), String::new()) } else { row(e.title.clone(), String::new(), String::new()) };
        r.detail = e.detail.clone().into();
        rows.push((e.key.clone(), r, true));
    }
    rows
}

pub(crate) fn render(ui: &HomeWindow) {
    let (rows, section, title) = STATE.with(|st| {
        let st = st.borrow();
        let section = if st.cwd.is_empty() { tr(ui, 665, "Media") }
            else if st.cwd.starts_with("jf:") { "Jellyfin".to_owned() }
            else { st.cwd.rsplit('/').next().unwrap_or("").to_owned() };
        (build_rows(ui, &st), section.clone(), section)
    });
    apply_rows(ui, rows, section);
    ui.set_details_title(title.into());
    ui.set_details_subtitle("".into());
    ui.set_details_description(STATE.with(|st| st.borrow().cwd.clone()).into());
}

/* ---------------------------------------------------------------- */
/* Loading                                                          */
/* ---------------------------------------------------------------- */

fn load(ui: &HomeWindow) {
    let cwd = STATE.with(|st| st.borrow().cwd.clone());
    let user = ui.get_active_user_id().to_string();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut entries: Vec<Entry> = Vec::new();
        let mut jf: Option<(bool, String)> = None;
        let mut error = false;
        if cwd.is_empty() {
            for l in run_tool(FILESCTL, &["roots"], None).lines() {
                let f: Vec<&str> = l.strip_prefix("root=").unwrap_or("").split('\t').collect();
                if f.len() >= 2 {
                    let title = if f[0] == "userdata" { String::new() } else { f[1].rsplit('/').next().unwrap_or("").to_owned() };
                    entries.push(Entry { key: format!("dir:{}", f[1]), title, detail: String::new(), folder: true });
                }
            }
            entries.push(Entry { key: "jf".into(), title: "Jellyfin".into(), detail: String::new(), folder: true });
        } else if cwd == "jf:" {
            let st = run_tool(JELLYFIN, &["status", &user], None);
            let signed = st.lines().any(|l| l == "signed=1");
            let name = st.lines().find_map(|l| l.strip_prefix("name=")).unwrap_or("").to_owned();
            jf = Some((signed, name));
            if signed {
                let out = run_tool(JELLYFIN, &["views", &user], None);
                error = out.starts_with("ERR");
                entries = jellyfin_entries(&out);
            }
        } else if let Some(parent) = cwd.strip_prefix("jf:") {
            let out = run_tool(JELLYFIN, &["items", &user, parent], None);
            error = out.starts_with("ERR");
            entries = jellyfin_entries(&out);
        } else {
            let mut list: Vec<(bool, String)> = run_tool(FILESCTL, &["list", &cwd], None)
                .lines()
                .filter_map(|l| l.strip_prefix("entry="))
                .filter_map(|v| {
                    let f: Vec<&str> = v.split('\t').collect();
                    (f.len() >= 4).then(|| (f[0] == "d", f[3].to_owned()))
                })
                .filter(|(dir, name)| *dir || media_kind(name).is_some())
                .collect();
            list.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.to_lowercase().cmp(&b.1.to_lowercase())));
            entries = list
                .into_iter()
                .map(|(dir, name)| Entry {
                    key: format!("{}:{}/{}", if dir { "dir" } else { "file" }, cwd, name),
                    title: name,
                    detail: String::new(),
                    folder: dir,
                })
                .collect();
        }
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                for e in entries.iter_mut() {
                    if e.key.starts_with("dir:") && e.title.is_empty() {
                        e.title = tr(&ui, 610, "Internal Storage");
                    }
                }
                st.entries = entries;
                if let Some((signed, name)) = jf {
                    st.jf_signed = signed;
                    st.jf_name = name;
                }
            });
            if error {
                notice(&ui, tr(&ui, 668, "The server could not be reached"));
            }
            render(&ui);
        });
    });
}

fn jellyfin_entries(out: &str) -> Vec<Entry> {
    out.lines()
        .filter_map(|l| l.strip_prefix("item="))
        .filter_map(|v| {
            let f: Vec<&str> = v.split('\t').collect();
            if f.len() < 6 {
                return None;
            }
            let folder = f[3] == "1";
            let resume: f64 = f[4].parse().unwrap_or(0.0);
            let length: f64 = f[5].parse().unwrap_or(0.0);
            let detail = if !folder && length > 0.0 {
                if resume > 0.0 { format!("{} / {}", duration(resume), duration(length)) } else { duration(length) }
            } else {
                String::new()
            };
            Some(Entry {
                key: format!("{}:{}:{}", if folder { "jfdir" } else { "jfitem" }, f[0], resume),
                title: f[1].to_owned(),
                detail,
                folder,
            })
        })
        .collect()
}

fn enter(ui: &HomeWindow, path: String) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        let cwd = std::mem::take(&mut st.cwd);
        st.stack.push(cwd);
        st.cwd = path;
        st.entries.clear();
    });
    ui.set_gen_index(0);
    ui.set_gen_scroll(0);
    render(ui);
    load(ui);
}

/* Home → Applications → Media. */
pub fn open(ui: &HomeWindow) {
    ACTIVE.store(true, Ordering::SeqCst);
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.cwd.clear();
        st.stack.clear();
    });
    ui.set_details_active(true);
    ui.set_details_shell_title(tr(ui, 665, "Media").into());
    ui.set_details_has_cover(false);
    show(ui, 65);
    ui.set_settings_open(true);
    write_ui_context("settings");
    load(ui);
}

fn leave(ui: &HomeWindow) {
    ACTIVE.store(false, Ordering::SeqCst);
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_details_active(false);
    ui.set_details_shell_title("".into());
    ui.set_gen_rows(ModelRc::default());
    write_ui_context("home");
}

fn play(ui: &HomeWindow, key: &str) {
    let user = ui.get_active_user_id().to_string();
    if let Some(path) = key.strip_prefix("file:") {
        *JELLYFIN_ITEM.lock().unwrap() = String::new();
        let line = match media_kind(path) {
            Some(false) => {
                let (dir, name) = path.rsplit_once('/').unwrap_or(("", path));
                format!("PLAYDIR\t{dir}\t{name}")
            }
            _ => format!("PLAY\t{path}"),
        };
        start(ui, line);
    } else if let Some(rest) = key.strip_prefix("jfitem:") {
        let (id, resume) = rest.split_once(':').unwrap_or((rest, "0"));
        let id = id.to_owned();
        let resume = resume.to_owned();
        let weak = ui.as_weak();
        thread::spawn(move || {
            let url = run_tool(JELLYFIN, &["stream", &user, &id], None).trim().to_owned();
            let _ = slint::invoke_from_event_loop(move || {
                let Some(ui) = weak.upgrade() else { return };
                if !url.starts_with("http") {
                    notice(&ui, tr(&ui, 668, "The server could not be reached"));
                    return;
                }
                *JELLYFIN_ITEM.lock().unwrap() = id;
                start(&ui, format!("PLAY\t{url}\t{resume}"));
            });
        });
    }
}

fn start(ui: &HomeWindow, line: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = player(&line);
        if !reply.starts_with("OK") {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    notice(&ui, tr(&ui, 667, "The media could not be played"));
                }
            });
        }
    });
}

fn confirm(ui: &HomeWindow, key: &str) {
    if let Some(path) = key.strip_prefix("dir:") {
        enter(ui, path.to_owned());
    } else if key == "jf" {
        enter(ui, "jf:".into());
    } else if let Some(rest) = key.strip_prefix("jfdir:") {
        let id = rest.split(':').next().unwrap_or("").to_owned();
        enter(ui, format!("jf:{id}"));
    } else if key.starts_with("file:") || key.starts_with("jfitem:") {
        play(ui, key);
    } else if key == "jf-signin" {
        STATE.with(|st| st.borrow_mut().draft = Default::default());
        open_system_keyboard(ui, &tr(ui, 666, "Server address (http://server:8096)"), KEYBOARD_SERVER, 65, "text", "http://");
    } else if key == "jf-signout" && arm_or(ui, "jf-signout") {
        tool_then(ui, JELLYFIN, vec!["logout".into(), ui.get_active_user_id().to_string()], None, |ui, _| load(ui));
    }
}

fn back(ui: &HomeWindow) {
    let parent = STATE.with(|st| st.borrow_mut().stack.pop());
    match parent {
        Some(p) => {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.cwd = p;
                st.entries.clear();
            });
            ui.set_gen_index(0);
            render(ui);
            load(ui);
        }
        None => leave(ui),
    }
}

/* Input while the Media shell is open. */
pub fn handle_action(ui: &HomeWindow, action: &str, settings_active: &Arc<AtomicBool>) {
    if ui.get_settings_choice_open() || ui.get_settings_view() == 6 {
        handle_settings_action(ui, action, settings_active);
        return;
    }
    play_ui_sound(action);
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => back(ui),
        "menu_confirm" => {
            let key = focused_key(ui);
            confirm(ui, &key);
        }
        _ => {}
    }
}

/* Controller actions while playing (the player window covers nuubUI). */
pub fn handle_player(action: &str) {
    let video = PLAYING_VIDEO.load(Ordering::SeqCst);
    let line = match action {
        "menu_confirm" => "PAUSE",
        "menu_left" => "SEEK\t-10",
        "menu_right" => "SEEK\t10",
        "menu_up" => if video { "SEEK\t60" } else { "NEXT" },
        "menu_down" => if video { "SEEK\t-60" } else { "PREV" },
        "menu_back" => "STOP",
        "face_north" => "SUB",
        "face_west" => "AUDIO",
        _ => return,
    };
    send(line.to_owned());
}

/* System keyboard results (Jellyfin sign-in). */
pub fn keyboard_done(ui: &HomeWindow, purpose: i32, value: String) {
    match purpose {
        KEYBOARD_SERVER => {
            let v = value.trim().trim_end_matches('/').to_owned();
            if v.len() <= 8 {
                navigate_settings_view(ui, 65);
                render(ui);
                return;
            }
            STATE.with(|st| st.borrow_mut().draft.0 = v);
            open_system_keyboard(ui, &tr(ui, 285, "Username"), KEYBOARD_JF_USER, 65, "text", "");
        }
        KEYBOARD_JF_USER => {
            STATE.with(|st| st.borrow_mut().draft.1 = value.trim().to_owned());
            open_system_keyboard(ui, &tr(ui, 172, "Password"), KEYBOARD_JF_PASSWORD, 65, "password", "");
        }
        KEYBOARD_JF_PASSWORD => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, 65);
            let (server, name) = STATE.with(|st| std::mem::take(&mut st.borrow_mut().draft));
            let user = ui.get_active_user_id().to_string();
            tool_then(ui, JELLYFIN, vec!["login".into(), user, server, name], Some(value), |ui, out| {
                if out.starts_with("ERR auth") {
                    notice(ui, tr(ui, 589, "Sign-in failed"));
                } else if out.starts_with("ERR") {
                    notice(ui, tr(ui, 668, "The server could not be reached"));
                }
                load(ui);
            });
        }
        _ => {}
    }
}

/* Follows nuubos-mediad: Home input and Home Music follow playback like a
 * game; a stopped Jellyfin item reports its position. */
pub fn start_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(MEDIA_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut playing = false;
                let mut video = false;
                let mut last_position = String::new();
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    if let Some(v) = line.strip_prefix("state=") {
                        playing = v == "playing" || v == "paused" || v == "stopping";
                    } else if let Some(v) = line.strip_prefix("kind=") {
                        video = v == "video";
                    } else if let Some(v) = line.strip_prefix("last_position=") {
                        last_position = v.to_owned();
                    } else if line == "end=1" {
                        PLAYING_VIDEO.store(video, Ordering::SeqCst);
                        if PLAYING.swap(playing, Ordering::SeqCst) == playing {
                            continue;
                        }
                        if !playing {
                            let item = std::mem::take(&mut *JELLYFIN_ITEM.lock().unwrap());
                            if !item.is_empty() && last_position.parse::<f64>().unwrap_or(-1.0) >= 0.0 {
                                let pos = last_position.clone();
                                let user = std::fs::read_to_string("/run/nuubos/user/active").unwrap_or_default().trim().to_owned();
                                thread::spawn(move || {
                                    let _ = run_tool(JELLYFIN, &["progress", &user, &item, &pos], None);
                                });
                            }
                        }
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                on_game_session(&ui, playing);
                                if !playing && ACTIVE.load(Ordering::SeqCst) {
                                    /* Resume positions changed. */
                                    load(&ui);
                                }
                            }
                        });
                    }
                }
            }
        }
        if PLAYING.swap(false, Ordering::SeqCst) {
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
