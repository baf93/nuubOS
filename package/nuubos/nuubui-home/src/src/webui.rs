/*
 * Web Mode application of nuubUI (EPIC-030), on the generic rows of
 * home.slint in the shared Details shell (settings-view 67). nuubos-webd
 * owns the browser session (Cog / WPE WebKit), the per-user bookmarks,
 * history and site data; this page only starts it with an address and
 * manages those lists. While the browser runs it covers nuubUI and the
 * controller drives it through nuubos-inputd (Home ignores input, Home
 * Music stops like for a game); the Quick Menu has the WEB section.
 */

use crate::gameui::{apply_rows, arm_or, armed, armed_detail, focused_key, move_selection, nav, notice, row, show};
use crate::{handle_settings_action, navigate_settings_view, on_game_session, open_system_keyboard, play_ui_sound,
    tr, write_ui_context, HomeWindow};
use slint::{ComponentHandle, ModelRc};
use std::cell::RefCell;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

pub const KEYBOARD_ADDRESS: i32 = 25;
pub const VIEW: i32 = 67;

const WEB_SOCKET: &str = "/run/nuubos/webd.sock";

/* The Web shell owns Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);
/* The browser runs: it covers nuubUI. */
pub static RUNNING: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Default)]
struct Entry {
    url: String,
    title: String,
}

#[derive(Clone, Copy, Default, PartialEq)]
enum Page {
    #[default]
    Main,
    Bookmarks,
    History,
}

#[derive(Default)]
struct State {
    page: Page,
    bookmarks: Vec<Entry>,
    history: Vec<Entry>,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

fn request(line: &str, multi: bool) -> String {
    let Ok(mut stream) = UnixStream::connect(WEB_SOCKET) else { return String::new() };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let _ = stream.write_all(line.as_bytes());
    let _ = stream.write_all(b"\n");
    let mut reply = String::new();
    for l in BufReader::new(stream).lines() {
        let Ok(l) = l else { break };
        reply.push_str(&l);
        reply.push('\n');
        if !multi || l == "end=1" {
            break;
        }
    }
    reply
}

fn parse_entries(reply: &str, key: &str) -> Vec<Entry> {
    reply
        .lines()
        .filter_map(|l| l.strip_prefix(key))
        .map(|v| {
            let (url, title) = v.split_once('\t').unwrap_or((v, ""));
            Entry { url: url.to_owned(), title: title.to_owned() }
        })
        .collect()
}

/* Host and path without the scheme: what the user recognizes. */
fn short_url(url: &str) -> String {
    let rest = url.strip_prefix("https://").or_else(|| url.strip_prefix("http://")).unwrap_or(url);
    rest.trim_end_matches('/').to_owned()
}

fn entry_row(e: &Entry) -> crate::GenRow {
    if e.title.is_empty() {
        row(short_url(&e.url), String::new(), String::new())
    } else {
        let mut r = row(e.title.clone(), String::new(), String::new());
        r.detail = short_url(&e.url).into();
        r
    }
}

/* ---------------------------------------------------------------- */
/* Rows                                                             */
/* ---------------------------------------------------------------- */

fn build_rows(ui: &HomeWindow, st: &State) -> Vec<(String, crate::GenRow, bool)> {
    let mut rows: Vec<(String, crate::GenRow, bool)> = Vec::new();
    let armed = armed();
    match st.page {
        Page::Main => {
            rows.push(("address".into(), nav(tr(ui, 679, "Enter Address"), String::new()), true));
            rows.push(("bookmarks".into(), nav(tr(ui, 681, "Bookmarks"), st.bookmarks.len().to_string()), true));
            rows.push(("history".into(), nav(tr(ui, 682, "History"), String::new()), true));
            let mut clear = row(tr(ui, 686, "Clear History"), String::new(),
                armed_detail(&armed, "clear-history", tr(ui, 191, "Press again to confirm"), String::new()));
            clear.enabled = !st.history.is_empty();
            rows.push(("clear-history".into(), clear, !st.history.is_empty()));
            rows.push(("clear-data".into(), row(tr(ui, 687, "Clear Browsing Data"), String::new(),
                armed_detail(&armed, "clear-data", tr(ui, 191, "Press again to confirm"),
                    tr(ui, 688, "Removes cookies, site sign-ins and cached pages of this profile. Bookmarks are kept."))), true));
        }
        Page::Bookmarks => {
            if st.bookmarks.is_empty() {
                let mut r = row(tr(ui, 683, "No bookmarks yet"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for (i, e) in st.bookmarks.iter().enumerate() {
                let key = format!("bookmark:{i}");
                let mut r = entry_row(e);
                if armed == key {
                    r.detail = tr(ui, 191, "Press again to confirm").into();
                }
                rows.push((key, r, true));
            }
        }
        Page::History => {
            if st.history.is_empty() {
                let mut r = row(tr(ui, 682, "History"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for (i, e) in st.history.iter().enumerate() {
                rows.push((format!("history:{i}"), entry_row(e), true));
            }
        }
    }
    rows
}

pub(crate) fn render(ui: &HomeWindow) {
    let (rows, section) = STATE.with(|st| {
        let st = st.borrow();
        let section = match st.page {
            Page::Main => tr(ui, 678, "Web"),
            Page::Bookmarks => tr(ui, 681, "Bookmarks"),
            Page::History => tr(ui, 682, "History"),
        };
        (build_rows(ui, &st), section)
    });
    apply_rows(ui, rows, section);
    /* Remove (north face) on a bookmark. */
    let key = focused_key(ui);
    ui.set_settings_north_hint(if key.starts_with("bookmark:") { tr(ui, 644, "Remove").into() } else { "".into() });
}

/* ---------------------------------------------------------------- */
/* Loading                                                          */
/* ---------------------------------------------------------------- */

fn load(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = request("STATUS", true);
        let bookmarks = parse_entries(&reply, "bookmark=");
        let history = parse_entries(&reply, "history=");
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.bookmarks = bookmarks;
                st.history = history;
            });
            if ACTIVE.load(Ordering::SeqCst) && ui.get_settings_view() == VIEW {
                render(&ui);
            }
        });
    });
}

/* A command whose result is the refreshed lists. */
fn command_then_load(ui: &HomeWindow, line: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = request(&line, false);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            if reply.starts_with("ERR busy") {
                notice(&ui, tr(&ui, 701, "Close the browser first"));
            }
            load(&ui);
        });
    });
}

fn go_to(ui: &HomeWindow, page: Page) {
    STATE.with(|st| st.borrow_mut().page = page);
    ui.set_gen_index(0);
    ui.set_gen_scroll(0);
    render(ui);
}

/* Home → Applications → Web. */
pub fn open(ui: &HomeWindow) {
    ACTIVE.store(true, Ordering::SeqCst);
    STATE.with(|st| st.borrow_mut().page = Page::Main);
    ui.set_details_active(true);
    ui.set_details_shell_title(tr(ui, 678, "Web").into());
    ui.set_details_has_cover(false);
    ui.set_details_title(tr(ui, 678, "Web").into());
    ui.set_details_subtitle("".into());
    ui.set_details_description(tr(ui, 698,
        "D-pad moves between links, the left stick moves the pointer and the right stick scrolls.").into());
    show(ui, VIEW);
    ui.set_settings_open(true);
    write_ui_context("settings");
    load(ui);
}

pub fn leave(ui: &HomeWindow) {
    ACTIVE.store(false, Ordering::SeqCst);
    ui.set_settings_north_hint("".into());
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_details_active(false);
    ui.set_details_shell_title("".into());
    ui.set_gen_rows(ModelRc::default());
    write_ui_context("home");
}

fn open_address(ui: &HomeWindow, address: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = request(&format!("OPEN\t{address}"), false);
        if !reply.starts_with("OK") {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    notice(&ui, tr(&ui, 689, "The browser could not be started"));
                }
            });
        }
    });
}

fn entry_at(key: &str) -> Option<Entry> {
    let (kind, index) = key.split_once(':')?;
    let index: usize = index.parse().ok()?;
    STATE.with(|st| {
        let st = st.borrow();
        match kind {
            "bookmark" => st.bookmarks.get(index).cloned(),
            "history" => st.history.get(index).cloned(),
            _ => None,
        }
    })
}

fn confirm(ui: &HomeWindow, key: &str) {
    match key {
        "address" => open_system_keyboard(ui, &tr(ui, 680, "Web address or search"), KEYBOARD_ADDRESS, VIEW, "text", ""),
        "bookmarks" => go_to(ui, Page::Bookmarks),
        "history" => go_to(ui, Page::History),
        "clear-history" if arm_or(ui, "clear-history") => command_then_load(ui, "HISTORY_CLEAR".into()),
        "clear-data" if arm_or(ui, "clear-data") => command_then_load(ui, "CLEAR_DATA".into()),
        _ => {
            if let Some(e) = entry_at(key) {
                open_address(ui, e.url);
            }
        }
    }
}

/* North face on a bookmark: remove it (second press). */
fn remove_focused(ui: &HomeWindow) {
    let key = focused_key(ui);
    if !key.starts_with("bookmark:") {
        return;
    }
    if !arm_or(ui, &key) {
        return;
    }
    if let Some(e) = entry_at(&key) {
        command_then_load(ui, format!("BOOKMARK_REMOVE\t{}", e.url));
    }
}

fn back(ui: &HomeWindow) {
    let page = STATE.with(|st| st.borrow().page);
    match page {
        Page::Main => leave(ui),
        _ => go_to(ui, Page::Main),
    }
}

/* Input while the Web shell is open. */
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
        "face_north" => remove_focused(ui),
        _ => {}
    }
}

/* System keyboard result: the address to open. */
pub fn keyboard_done(ui: &HomeWindow, value: String) {
    navigate_settings_view(ui, VIEW);
    render(ui);
    let address = value.trim().to_owned();
    if !address.is_empty() {
        open_address(ui, address);
    }
}

/* Follows nuubos-webd: Home input and Home Music follow the browser like a
 * game; the lists are refreshed when it closes (new history). */
pub fn start_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(WEB_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut running = false;
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    if let Some(v) = line.strip_prefix("state=") {
                        running = v != "idle";
                    } else if line == "end=1" {
                        if RUNNING.swap(running, Ordering::SeqCst) == running {
                            continue;
                        }
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                on_game_session(&ui, running);
                                if !running && ACTIVE.load(Ordering::SeqCst) {
                                    load(&ui);
                                }
                            }
                        });
                    }
                }
            }
        }
        if RUNNING.swap(false, Ordering::SeqCst) {
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
