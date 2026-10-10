/*
 * Application page (Moonlight, Steam Link): the Game Details look for an
 * application (user request 2026-10-09). moonlight.rs and steamlink.rs
 * keep owning their state and their index order (entries first, then
 * actions); this only turns that state into the page (AppPage in
 * home.slint) after every change.
 */

use crate::{tr, AppEntry, DetailAction, DetailChip, GenRow, HomeWindow};
use slint::{Model, ModelRc, SharedString, VecModel};
use std::rc::Rc;

fn action(icon: i32, label: String, primary: bool) -> DetailAction {
    DetailAction { icon, label: label.into(), primary, on: false, enabled: true, armed: false, busy: false }
}

fn chip(icon: i32, text: String, warn: bool) -> DetailChip {
    DetailChip { icon, text: text.into(), warn }
}

fn entry(icon: i32, title: &str, detail: String, value: String, good: bool, caption: String) -> AppEntry {
    AppEntry {
        title: title.into(),
        detail: detail.into(),
        value: value.into(),
        icon,
        good,
        caption: caption.into(),
    }
}

fn model<T: Clone + 'static>(v: Vec<T>) -> ModelRc<T> {
    ModelRc::from(Rc::new(VecModel::from(v)))
}

struct Page {
    kicker: String,
    title: String,
    meta: String,
    description: String,
    chips: Vec<DetailChip>,
    entries: Vec<AppEntry>,
    actions: Vec<DetailAction>,
    captions: Vec<String>,
    pin: String,
    progress: f32,
    progress_label: String,
}

impl Page {
    fn new(kicker: String, title: String) -> Self {
        Page {
            kicker,
            title,
            meta: String::new(),
            description: String::new(),
            chips: Vec::new(),
            entries: Vec::new(),
            actions: Vec::new(),
            captions: Vec::new(),
            pin: String::new(),
            progress: -1.0,
            progress_label: String::new(),
        }
    }

    fn push(&mut self, a: DetailAction, caption: String) {
        self.actions.push(a);
        self.captions.push(caption);
    }
}

fn host_state(ui: &HomeWindow, online: &str, probing: bool) -> String {
    if probing {
        tr(ui, 446, "Checking…")
    } else if online == "online" {
        tr(ui, 444, "Online")
    } else if online == "offline" {
        tr(ui, 445, "Offline")
    } else {
        String::new()
    }
}

fn moonlight_page(ui: &HomeWindow, view: i32) -> Page {
    let kicker = tr(ui, 468, "STREAM");
    if view == 41 {
        let host = ui.get_moonlight_host();
        let mut p = Page::new(kicker, host.name.to_string());
        p.meta = host.address.to_string();
        p.chips.push(if host.paired {
            chip(6, tr(ui, 442, "Paired"), false)
        } else {
            chip(13, tr(ui, 443, "Not paired"), true)
        });
        let state = host_state(ui, host.online.as_str(), host.probing);
        if !state.is_empty() {
            p.chips.push(chip(18, state, host.online == "offline"));
        }
        if host.paired {
            for app in ui.get_moonlight_apps().iter() {
                p.entries.push(entry(0, app.as_str(), String::new(), String::new(), false, String::new()));
            }
            if p.entries.is_empty() {
                p.description = match ui.get_moonlight_apps_state().as_str() {
                    "loading" => tr(ui, 451, "Loading applications…"),
                    "error" => tr(ui, 452, "The PC could not be reached"),
                    _ => tr(ui, 453, "No applications"),
                };
            }
        } else {
            let pin = ui.get_moonlight_pairing_pin().to_string();
            if pin.is_empty() {
                p.push(action(6, tr(ui, 447, "Pair"), true),
                    if host.online == "offline" { tr(ui, 452, "The PC could not be reached") } else { String::new() });
            } else {
                p.push(action(7, tr(ui, 448, "Cancel Pairing"), true), tr(ui, 475, "Waiting for the PC…"));
            }
            p.pin = pin;
        }
        let armed = ui.get_moonlight_remove_confirm();
        let mut remove = action(10, tr(ui, 450, "Remove PC"), false);
        remove.armed = armed;
        p.push(remove, if armed { tr(ui, 191, "Press again to confirm") } else { String::new() });
        return p;
    }

    let mut p = Page::new(kicker, tr(ui, 435, "Moonlight"));
    for h in ui.get_moonlight_hosts().iter() {
        let paired = if h.paired { tr(ui, 442, "Paired") } else { tr(ui, 443, "Not paired") };
        p.entries.push(entry(
            18,
            h.name.as_str(),
            format!("{} • {}", h.address, paired),
            host_state(ui, h.online.as_str(), h.probing),
            h.paired && h.online == "online",
            String::new(),
        ));
    }
    if p.entries.is_empty() {
        p.description = tr(ui, 458, "No PCs yet");
    }
    let discovering = ui.get_moonlight_discovering();
    let mut search = action(17, tr(ui, 438, "Search for PCs"), false);
    search.busy = discovering;
    p.push(search, if discovering { tr(ui, 440, "Searching for PCs…") } else { String::new() });
    p.push(action(19, tr(ui, 439, "Add PC by Address"), false), tr(ui, 441, "PC address"));
    p.push(
        action(3, tr(ui, 437, "Stream Settings"), false),
        format!(
            "{} • {} • {} • {}",
            ui.get_moonlight_resolution_label(),
            ui.get_moonlight_fps_label(),
            ui.get_moonlight_codec_label(),
            ui.get_moonlight_bitrate_label()
        ),
    );
    p
}

fn steamlink_page(ui: &HomeWindow) -> Page {
    let mut p = Page::new(tr(ui, 478, "STEAM LINK"), tr(ui, 477, "Steam Link"));
    match ui.get_steamlink_state().as_str() {
        "installing" => {
            let label = ui.get_steamlink_progress_label().to_string();
            p.progress = label.trim_end_matches('%').trim().parse::<f32>().unwrap_or(0.0) / 100.0;
            p.progress_label = format!("{} • {}", tr(ui, 481, "Downloading Steam Link"), label);
            p.description = tr(ui, 480, "Valve's application, about 35 MB. Downloading it means you accept the Steam Subscriber Agreement.");
            p.push(action(7, tr(ui, 482, "Press to cancel"), true), String::new());
        }
        "installed" => {
            p.meta = ui.get_steamlink_version_label().to_string();
            let value = ui.get_steamlink_update_value().to_string();
            if !value.is_empty() {
                p.chips.push(chip(6, value, false));
            }
            p.description = tr(ui, 484, "Choose and pair your PC in Steam Link");
            p.push(action(0, tr(ui, 483, "Open Steam Link"), true), tr(ui, 484, "Choose and pair your PC in Steam Link"));
            p.push(action(5, ui.get_steamlink_update_title().to_string(), false), ui.get_steamlink_version_label().to_string());
            let snap = crate::moonlight::SNAPSHOT.lock().unwrap().clone().unwrap_or_default();
            let mut auto = action(4, tr(ui, 852, "Automatic Updates"), false);
            auto.on = snap.steamlink_auto_update;
            p.push(auto, tr(ui, 856, "New versions install when you open Steam Link; one that does not start is undone automatically"));
            if !snap.steamlink_previous.is_empty() {
                p.push(action(9, crate::tr_arg(ui, 853, "Restore Version {0}", &snap.steamlink_previous), false), String::new());
            }
            let armed = ui.get_steamlink_remove_confirm();
            let mut remove = action(10, tr(ui, 490, "Remove Steam Link"), false);
            remove.armed = armed;
            p.push(remove, if armed { tr(ui, 191, "Press again to confirm") } else { String::new() });
        }
        _ => {
            p.description = tr(ui, 480, "Valve's application, about 35 MB. Downloading it means you accept the Steam Subscriber Agreement.");
            let detail = ui.get_steamlink_download_detail().to_string();
            if !detail.is_empty() {
                p.chips.push(chip(13, detail.clone(), true));
            }
            p.push(action(4, tr(ui, 479, "Download Steam Link"), true), detail);
        }
    }
    p
}

/* Rebuilds the page of the active application, if one is on screen. */
pub fn refresh(ui: &HomeWindow) {
    let view = ui.get_settings_view();
    let page = if ui.get_moonlight_active() && (view == 40 || view == 41) {
        moonlight_page(ui, view)
    } else if ui.get_steamlink_active() && view == 45 {
        steamlink_page(ui)
    } else {
        return;
    };
    ui.set_app_page_kicker(page.kicker.into());
    ui.set_app_page_title(page.title.into());
    ui.set_app_page_meta(page.meta.into());
    ui.set_app_page_description(page.description.into());
    ui.set_app_page_chips(model(page.chips));
    ui.set_app_page_entries(model(page.entries));
    ui.set_app_page_actions(model(page.actions));
    ui.set_app_page_captions(model(page.captions.into_iter().map(SharedString::from).collect()));
    ui.set_app_page_pin(page.pin.into());
    ui.set_app_page_progress(page.progress);
    ui.set_app_page_progress_label(page.progress_label.into());
}

/* ---------------------------------------------------------------- */
/* Files and Media: application pages drawn from their generic rows  */
/* ---------------------------------------------------------------- */

/* Icon of a Files/Media row by its key; action rows give their own. */
fn row_icon(key: &str, title: &str) -> i32 {
    let ext = |name: &str| name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default();
    let by_ext = |name: &str| match ext(name).as_str() {
        "png" | "jpg" | "jpeg" | "bmp" | "gif" | "webp" => 22,
        "mp3" | "flac" | "ogg" | "opus" | "m4a" | "aac" | "wav" => 23,
        "mkv" | "mp4" | "m4v" | "avi" | "webm" | "mov" | "ts" | "mpg" | "mpeg" => 24,
        "zip" | "7z" | "rar" | "tar" | "gz" | "xz" | "bz2" => 28,
        _ => 21,
    };
    match key {
        "shares" => 26,
        "jf" => 33,
        "info" => 13,
        k if k.starts_with("root:") => if k.contains("/userdata") || k.ends_with("/userdata") { 25 } else { 27 },
        k if k.starts_with("share:") => 32,
        k if k.starts_with("d:") || k.starts_with("jfdir:") => 20,
        k if k.starts_with("dir:") => if k.matches('/').count() <= 2 { 25 } else { 20 },
        k if k.starts_with("f:") || k.starts_with("file:") => by_ext(k),
        k if k.starts_with("jfitem:") => 24,
        k if k.starts_with("restore:") => 28,
        _ => by_ext(title),
    }
}

/* Action keys become pills after the entries (the row order puts them
 * last, so the index keeps meaning entries first, then actions). */
fn action_icon(key: &str) -> Option<i32> {
    match key {
        "paste" => Some(31),
        "mkdir" => Some(30),
        "add" => Some(19),
        "jf-signin" => Some(6),
        "jf-signout" => Some(7),
        _ => None,
    }
}

/// Draws the generic rows of Files (58, 60) or Media (65) as an
/// application page: kicker, title (location), meta, description; rows
/// with a type icon; action rows as pills.
pub fn gen_page(ui: &HomeWindow, kicker: String, title: String, meta: String, description: String) {
    let keys = crate::gameui::gen_keys();
    let rows: Vec<GenRow> = ui.get_gen_rows().iter().collect();
    let mut p = Page::new(kicker, title);
    p.meta = meta;
    p.description = description;
    for (key, r) in keys.iter().zip(rows.iter()) {
        if let Some(icon) = action_icon(key) {
            let mut a = action(icon, r.title.to_string(), false);
            a.enabled = r.enabled;
            a.armed = r.armed;
            p.push(a, if r.detail.is_empty() { r.value.to_string() } else { r.detail.to_string() });
        } else {
            let value = if r.toggle {
                if r.toggle_on { tr(ui, 198, "Connected") } else { String::new() }
            } else {
                r.value.to_string()
            };
            p.entries.push(entry(row_icon(key, r.title.as_str()), r.title.as_str(), r.detail.to_string(), value,
                r.toggle && r.toggle_on, String::new()));
        }
    }
    ui.set_app_page_gen(true);
    ui.set_app_page_kicker(p.kicker.into());
    ui.set_app_page_title(p.title.into());
    ui.set_app_page_meta(p.meta.into());
    ui.set_app_page_description(p.description.into());
    ui.set_app_page_chips(model(p.chips));
    ui.set_app_page_entries(model(p.entries));
    ui.set_app_page_actions(model(p.actions));
    ui.set_app_page_captions(model(p.captions.into_iter().map(SharedString::from).collect()));
    ui.set_app_page_pin("".into());
    ui.set_app_page_progress(-1.0);
    ui.set_app_page_progress_label("".into());
}
