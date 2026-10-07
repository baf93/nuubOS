/*
 * Files application and storage pages of nuubUI, on the generic rows of
 * home.slint (see gameui.rs):
 *
 *   58 Files (EPIC-031)            nuubos-filesctl; copy/move are jobs
 *   60 Network Shares (EPIC-036)   nuubos-sharesctl
 *   61 Removable Media (EPIC-032)  nuubos-mediactl (Settings > Storage)
 *   62 Backup & Restore (EPIC-035) nuubos-backupctl jobs (Settings > Profile)
 *
 * 58 and 60 are the Files shell (the Details shell with "Files" as title,
 * its rail shows the location); 61 and 62 are Settings pages. No file
 * policy lives here: paths are checked by nuubos-filesctl.
 */

use crate::gameui::{
    apply_rows, arm_or, armed, armed_detail, focused_key, move_selection, nav, notice, row, run_tool,
    show, tool_then, toggle,
};
use crate::{handle_settings_action, navigate_settings_view, open_settings_choice, open_system_keyboard,
    play_ui_sound, tr, tr_arg, write_ui_context, GenRow, HomeWindow};
use slint::{ComponentHandle, ModelRc};
use std::cell::RefCell;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;

pub const KEYBOARD_FOLDER: i32 = 16;
pub const KEYBOARD_RENAME: i32 = 17;
pub const KEYBOARD_SHARE_NAME: i32 = 18;
pub const KEYBOARD_SHARE_URL: i32 = 19;
pub const KEYBOARD_SHARE_USER: i32 = 20;
pub const KEYBOARD_SHARE_PASSWORD: i32 = 21;

const FILESCTL: &str = "/usr/bin/nuubos-filesctl";
const MEDIACTL: &str = "/usr/sbin/nuubos-mediactl";
const SHARESCTL: &str = "/usr/sbin/nuubos-sharesctl";
const BACKUPCTL: &str = "/usr/bin/nuubos-backupctl";
const JOBCTL: &str = "/usr/bin/nuubos-jobctl";
const INTERNAL_BACKUPS: &str = "/userdata/backups";

/* The Files shell owns Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Default)]
struct Entry {
    dir: bool,
    size: i64,
    name: String,
}

#[derive(Clone, Default)]
struct Root {
    kind: String,
    path: String,
    free: i64,
}

#[derive(Default)]
struct State {
    /* Files: "" = the list of locations. */
    cwd: String,
    roots: Vec<Root>,
    entries: Vec<Entry>,
    clipboard: Option<(String, bool)>,
    focused: String,
    busy: bool,
    shares: Vec<(String, String, String, bool)>,
    share_draft: (String, String, String),
    volumes: Vec<(String, String, i64, i64)>,
    backups: Vec<(String, String, String, i64)>,
    with_screenshots: bool,
    with_appdata: bool,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

fn human(bytes: i64) -> String {
    let b = bytes as f64;
    if b >= 1e9 {
        format!("{:.1} GB", b / 1e9)
    } else if b >= 1e6 {
        format!("{:.1} MB", b / 1e6)
    } else if b >= 1e3 {
        format!("{:.0} KB", b / 1e3)
    } else {
        format!("{} B", bytes)
    }
}

fn root_label(ui: &HomeWindow, r: &Root) -> String {
    match r.kind.as_str() {
        "userdata" => tr(ui, 610, "Internal Storage"),
        _ => r.path.rsplit('/').next().unwrap_or(&r.path).to_owned(),
    }
}

fn parent(path: &str) -> String {
    match path.rfind('/') {
        Some(0) | None => String::new(),
        Some(i) => path[..i].to_owned(),
    }
}

/* ---------------------------------------------------------------- */
/* Rows                                                             */
/* ---------------------------------------------------------------- */

fn build_rows(ui: &HomeWindow, st: &State, view: i32) -> Vec<(String, GenRow, bool)> {
    let mut rows: Vec<(String, GenRow, bool)> = Vec::new();
    let armed = armed();
    match view {
        58 if st.cwd.is_empty() => {
            for r in &st.roots {
                rows.push((format!("root:{}", r.path), nav(root_label(ui, r), tr_arg(ui, 622, "{0} free", &human(r.free))), true));
            }
            rows.push(("shares".into(), nav(tr(ui, 611, "Network Shares"), String::new()), true));
        }
        58 => {
            if let Some((src, mv)) = &st.clipboard {
                let name = src.rsplit('/').next().unwrap_or(src).to_owned();
                let mut r = row(tr(ui, 614, "Paste Here"), name, if *mv { tr(ui, 616, "Move") } else { tr(ui, 615, "Copy") });
                r.enabled = !st.busy;
                rows.push(("paste".into(), r, !st.busy));
            }
            rows.push(("mkdir".into(), row(tr(ui, 612, "New Folder"), String::new(), String::new()), true));
            if st.entries.is_empty() {
                let mut r = row(tr(ui, 621, "Empty folder"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for e in &st.entries {
                let key = format!("{}:{}", if e.dir { "d" } else { "f" }, e.name);
                if e.dir {
                    rows.push((key, nav(e.name.clone(), String::new()), true));
                } else {
                    rows.push((key, row(e.name.clone(), human(e.size), String::new()), true));
                }
            }
        }
        60 => {
            for (name, url, _user, mounted) in &st.shares {
                rows.push((format!("share:{name}"), toggle(name.clone(), *mounted, url.clone()), true));
            }
            rows.push(("add".into(), row(tr(ui, 641, "Add Share"), String::new(), String::new()), true));
        }
        61 => {
            if st.volumes.is_empty() {
                let mut r = row(tr(ui, 629, "No removable media"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for (name, _path, total, free) in &st.volumes {
                let key = format!("vol:{name}");
                rows.push((key.clone(), row(name.clone(), tr_arg(ui, 622, "{0} free", &human(*free)),
                    armed_detail(&armed, &key, tr(ui, 191, "Press again to confirm"),
                        format!("{} • {}", tr(ui, 626, "Eject"), human(*total)))), true));
            }
        }
        62 => {
            rows.push(("shots".into(), toggle(tr(ui, 632, "Include Screenshots"), st.with_screenshots, String::new()), true));
            rows.push(("appdata".into(), toggle(tr(ui, 633, "Include App Data"), st.with_appdata, String::new()), true));
            let mut b = row(tr(ui, 631, "Back Up Now"), if st.busy { tr(ui, 91, "Running") } else { String::new() }, String::new());
            b.enabled = !st.busy;
            rows.push(("backup".into(), b, !st.busy));
            if st.backups.is_empty() {
                let mut r = row(tr(ui, 634, "No backups found"), String::new(), String::new());
                r.enabled = false;
                rows.push(("info".into(), r, false));
            }
            for (path, user, created, size) in &st.backups {
                let key = format!("restore:{path}");
                let title = format!("{} • {}", user, created.replace('T', " ").trim_end_matches('Z'));
                rows.push((key.clone(), row(title, human(*size),
                    armed_detail(&armed, &key, tr(ui, 191, "Press again to confirm"),
                        tr(ui, 636, "Replaces this profile's saves, save states and settings. ROMs and BIOS are not affected."))),
                    !st.busy));
            }
        }
        _ => {}
    }
    rows
}

pub(crate) fn render(ui: &HomeWindow) {
    let view = ui.get_settings_view();
    let (rows, section) = STATE.with(|st| {
        let st = st.borrow();
        let section = match view {
            58 if st.cwd.is_empty() => tr(ui, 609, "Files"),
            58 => st.cwd.rsplit('/').next().unwrap_or("").to_owned(),
            60 => tr(ui, 611, "Network Shares"),
            61 => tr(ui, 19, "Storage"),
            _ => tr(ui, 630, "Backup & Restore"),
        };
        (build_rows(ui, &st, view), section)
    });
    /* Keep the focus on the same entry after a refresh. */
    let focus = STATE.with(|st| std::mem::take(&mut st.borrow_mut().focused));
    if !focus.is_empty() {
        if let Some(i) = rows.iter().position(|r| r.0 == focus) {
            ui.set_gen_index(i as i32);
        }
    }
    apply_rows(ui, rows, section);
    if ACTIVE.load(Ordering::SeqCst) {
        STATE.with(|st| {
            let st = st.borrow();
            let (title, sub) = if st.cwd.is_empty() {
                (tr(ui, 609, "Files"), String::new())
            } else {
                let root = st.roots.iter().find(|r| st.cwd.starts_with(&r.path));
                (root.map(|r| root_label(ui, r)).unwrap_or_default(),
                 root.map(|r| tr_arg(ui, 622, "{0} free", &human(r.free))).unwrap_or_default())
            };
            ui.set_details_title(title.into());
            ui.set_details_subtitle(sub.into());
            ui.set_details_description(st.cwd.clone().into());
        });
    }
}

/* ---------------------------------------------------------------- */
/* Files shell                                                      */
/* ---------------------------------------------------------------- */

fn parse_kv_lines<'a>(out: &'a str, key: &str) -> Vec<Vec<&'a str>> {
    out.lines()
        .filter_map(|l| l.strip_prefix(key).and_then(|r| r.strip_prefix('=')))
        .map(|v| v.split('\t').collect())
        .collect()
}

fn refresh_files(ui: &HomeWindow) {
    let cwd = STATE.with(|st| st.borrow().cwd.clone());
    let weak = ui.as_weak();
    thread::spawn(move || {
        let roots: Vec<Root> = parse_kv_lines(&run_tool(FILESCTL, &["roots"], None), "root")
            .into_iter()
            .filter(|f| f.len() >= 5)
            .map(|f| Root { kind: f[0].into(), path: f[1].into(), free: f[3].parse().unwrap_or(0) })
            .collect();
        let mut entries: Vec<Entry> = if cwd.is_empty() {
            Vec::new()
        } else {
            parse_kv_lines(&run_tool(FILESCTL, &["list", &cwd], None), "entry")
                .into_iter()
                .filter(|f| f.len() >= 4)
                .map(|f| Entry { dir: f[0] == "d", size: f[1].parse().unwrap_or(0), name: f[3].into() })
                .collect()
        };
        entries.sort_by(|a, b| b.dir.cmp(&a.dir).then(a.name.to_lowercase().cmp(&b.name.to_lowercase())));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                /* A removed volume or share: back to the locations. */
                if !st.cwd.is_empty() && !roots.iter().any(|r| st.cwd.starts_with(&r.path)) {
                    st.cwd.clear();
                }
                st.roots = roots;
                st.entries = entries;
            });
            render(&ui);
        });
    });
}

fn enter(ui: &HomeWindow, path: String) {
    STATE.with(|st| st.borrow_mut().cwd = path);
    ui.set_gen_index(0);
    ui.set_gen_scroll(0);
    refresh_files(ui);
}

/* Home → Applications → Files. */
pub fn open(ui: &HomeWindow) {
    ACTIVE.store(true, Ordering::SeqCst);
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.cwd.clear();
        st.busy = false;
    });
    ui.set_details_active(true);
    ui.set_details_shell_title(tr(ui, 609, "Files").into());
    ui.set_details_has_cover(false);
    show(ui, 58);
    ui.set_settings_open(true);
    write_ui_context("settings");
    refresh_files(ui);
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

fn selected_path(key: &str) -> Option<(String, bool)> {
    let (kind, name) = key.split_once(':')?;
    let cwd = STATE.with(|st| st.borrow().cwd.clone());
    Some((format!("{cwd}/{name}"), kind == "d"))
}

fn file_actions(ui: &HomeWindow, dir: bool) {
    let mut options = Vec::new();
    if dir {
        options.push(("open".to_owned(), tr(ui, 212, "Open")));
    }
    options.push(("copy".into(), tr(ui, 615, "Copy")));
    options.push(("move".into(), tr(ui, 616, "Move")));
    options.push(("rename".into(), tr(ui, 617, "Rename")));
    options.push(("delete".into(), tr(ui, 618, "Delete")));
    /* Theme packages (EPIC-008): a folder with theme.conf or an archive;
     * nuubos-themectl decides whether it is valid. */
    if let Some((path, _)) = selected_path(&focused_key(ui)) {
        if (dir && std::path::Path::new(&format!("{path}/theme.conf")).exists()) || (!dir && path.ends_with(".tar")) {
            options.push(("theme".into(), tr(ui, 648, "Install Theme")));
        }
    }
    let title = focused_key(ui).split_once(':').map(|(_, n)| n.to_owned()).unwrap_or_default();
    open_settings_choice(ui, "files-action", &title, options, "");
}

fn paste(ui: &HomeWindow) {
    let (clip, cwd) = STATE.with(|st| (st.borrow().clipboard.clone(), st.borrow().cwd.clone()));
    let Some((src, mv)) = clip else { return };
    STATE.with(|st| st.borrow_mut().busy = true);
    render(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let out = run_tool(JOBCTL, &["run", if mv { "file-move" } else { "file-copy" }, &src, &cwd], None);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.busy = false;
                st.clipboard = None;
            });
            if out.starts_with("ERR") {
                notice(&ui, if out.contains("space") { tr(&ui, 493, "Not enough free space") } else { tr(&ui, 93, "Failed") });
            }
            refresh_files(&ui);
        });
    });
}

fn files_confirm(ui: &HomeWindow, key: &str) {
    if let Some(path) = key.strip_prefix("root:") {
        enter(ui, path.to_owned());
        return;
    }
    match key {
        "shares" => open_shares(ui),
        "paste" => paste(ui),
        "mkdir" => open_system_keyboard(ui, &tr(ui, 613, "Folder name"), KEYBOARD_FOLDER, 58, "text", ""),
        k if k.starts_with("d:") || k.starts_with("f:") => file_actions(ui, k.starts_with("d:")),
        _ => {}
    }
}

fn files_back(ui: &HomeWindow) {
    let view = ui.get_settings_view();
    if view == 60 {
        show(ui, 58);
        refresh_files(ui);
        return;
    }
    let (cwd, at_root) = STATE.with(|st| {
        let st = st.borrow();
        (st.cwd.clone(), st.roots.iter().any(|r| r.path == st.cwd))
    });
    if cwd.is_empty() {
        leave(ui);
    } else {
        let name = cwd.rsplit('/').next().unwrap_or("").to_owned();
        STATE.with(|st| st.borrow_mut().focused = if at_root { String::new() } else { format!("d:{name}") });
        let up = if at_root { String::new() } else { parent(&cwd) };
        STATE.with(|st| st.borrow_mut().cwd = up);
        if at_root {
            STATE.with(|st| st.borrow_mut().focused = format!("root:{cwd}"));
        }
        refresh_files(ui);
    }
}

/* Input while the Files shell is open (58, 60 and their keyboard and
 * dropdowns). */
pub fn handle_action(ui: &HomeWindow, action: &str, settings_active: &Arc<AtomicBool>) {
    if ui.get_settings_choice_open() || ui.get_settings_view() == 6 {
        handle_settings_action(ui, action, settings_active);
        return;
    }
    play_ui_sound(action);
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => files_back(ui),
        "menu_confirm" => {
            let key = focused_key(ui);
            if ui.get_settings_view() == 60 {
                shares_confirm(ui, &key);
            } else {
                files_confirm(ui, &key);
            }
        }
        _ => {}
    }
}

fn file_command(ui: &HomeWindow, args: Vec<String>) {
    tool_then(ui, FILESCTL, args, None, |ui, out| {
        if out.starts_with("ERR") {
            notice(ui, tr(ui, 93, "Failed"));
        }
        refresh_files(ui);
    });
}

/* ---------------------------------------------------------------- */
/* Network shares                                                   */
/* ---------------------------------------------------------------- */

fn open_shares(ui: &HomeWindow) {
    show(ui, 60);
    refresh_shares(ui);
}

fn refresh_shares(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let shares: Vec<(String, String, String, bool)> = parse_kv_lines(&run_tool(SHARESCTL, &["list"], None), "share")
            .into_iter()
            .filter(|f| f.len() >= 4)
            .map(|f| (f[0].into(), f[1].into(), f[2].into(), f[3] == "1"))
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().shares = shares);
            render(&ui);
        });
    });
}

fn shares_confirm(ui: &HomeWindow, key: &str) {
    if key == "add" {
        STATE.with(|st| st.borrow_mut().share_draft = Default::default());
        open_system_keyboard(ui, &tr(ui, 642, "Share name"), KEYBOARD_SHARE_NAME, 60, "text", "");
    } else if let Some(name) = key.strip_prefix("share:") {
        let mounted = STATE.with(|st| st.borrow().shares.iter().any(|s| s.0 == name && s.3));
        let options = vec![
            (if mounted { "umount" } else { "mount" }.to_owned(),
             if mounted { tr(ui, 169, "Disconnect") } else { tr(ui, 158, "Connect") }),
            ("remove".to_owned(), tr(ui, 644, "Remove")),
        ];
        open_settings_choice(ui, "share-action", name, options, "");
    }
}

/* ---------------------------------------------------------------- */
/* Removable media (Settings > Storage)                             */
/* ---------------------------------------------------------------- */

pub fn open_media(ui: &HomeWindow) {
    show(ui, 61);
    refresh_media(ui);
}

fn refresh_media(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let volumes: Vec<(String, String, i64, i64)> = parse_kv_lines(&run_tool(MEDIACTL, &["list"], None), "volume")
            .into_iter()
            .filter(|f| f.len() >= 6)
            .map(|f| (f[0].into(), f[2].into(), f[3].parse().unwrap_or(0), f[4].parse().unwrap_or(0)))
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().volumes = volumes);
            render(&ui);
        });
    });
}

/* ---------------------------------------------------------------- */
/* Backup & Restore (Settings > Profile)                            */
/* ---------------------------------------------------------------- */

pub fn open_backup(ui: &HomeWindow) {
    STATE.with(|st| st.borrow_mut().busy = false);
    show(ui, 62);
    refresh_backups(ui);
}

fn refresh_backups(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut dirs = vec![INTERNAL_BACKUPS.to_owned()];
        for f in parse_kv_lines(&run_tool(MEDIACTL, &["list"], None), "volume") {
            if f.len() >= 3 {
                dirs.push(format!("{}/nuubos-backups", f[2]));
            }
        }
        let refs: Vec<&str> = std::iter::once("list").chain(dirs.iter().map(String::as_str)).collect();
        let mut backups: Vec<(String, String, String, i64)> = parse_kv_lines(&run_tool(BACKUPCTL, &refs, None), "backup")
            .into_iter()
            .filter(|f| f.len() >= 4)
            .map(|f| (f[0].into(), f[1].into(), f[2].into(), f[3].parse().unwrap_or(0)))
            .collect();
        backups.sort_by(|a, b| b.2.cmp(&a.2));
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().backups = backups);
            render(&ui);
        });
    });
}

fn backup_destinations(ui: &HomeWindow) {
    let volumes = STATE.with(|st| st.borrow().volumes.clone());
    let mut options = vec![(INTERNAL_BACKUPS.to_owned(), tr(ui, 610, "Internal Storage"))];
    for (name, path, _, _) in volumes {
        options.push((format!("{path}/nuubos-backups"), name));
    }
    open_settings_choice(ui, "backup-destination", &tr(ui, 631, "Back Up Now"), options, INTERNAL_BACKUPS);
}

fn start_backup(ui: &HomeWindow, dest: String) {
    let user = ui.get_active_user_id().to_string();
    let (shots, appdata) = STATE.with(|st| (st.borrow().with_screenshots, st.borrow().with_appdata));
    STATE.with(|st| st.borrow_mut().busy = true);
    render(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let _ = std::fs::create_dir_all(&dest);
        let mut args = vec!["run".to_owned(), "backup-user".into(), user, dest];
        if shots {
            args.push("screenshots".into());
        }
        if appdata {
            args.push("appdata".into());
        }
        let refs: Vec<&str> = args.iter().map(String::as_str).collect();
        let ok = !run_tool(JOBCTL, &refs, None).starts_with("ERR");
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().busy = false);
            notice(&ui, if ok { tr(&ui, 637, "Backup created") } else { tr(&ui, 639, "The backup could not be created") });
            refresh_backups(&ui);
        });
    });
}

fn start_restore(ui: &HomeWindow, file: String) {
    let user = ui.get_active_user_id().to_string();
    STATE.with(|st| st.borrow_mut().busy = true);
    render(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let out = run_tool(JOBCTL, &["run", "restore-user", &file, &user], None);
        let restart = out.trim() == "restart-required";
        let ok = !out.starts_with("ERR");
        if restart {
            /* Every service reloads the restored settings: central path. */
            let _ = run_tool("/usr/bin/nuubos-systemctl", &["restart"], None);
        }
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| st.borrow_mut().busy = false);
            notice(&ui, if ok { tr(&ui, 638, "Restore complete") } else { tr(&ui, 640, "The backup could not be restored") });
            render(&ui);
        });
    });
}

/* Input on Settings pages 61 and 62 (gameui::handle_page). */
pub fn handle_page(ui: &HomeWindow, view: i32, action: &str) {
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => navigate_settings_view(ui, if view == 61 { 20 } else { 24 }),
        "menu_confirm" => {
            let key = focused_key(ui);
            match (view, key.as_str()) {
                (61, k) if k.starts_with("vol:") => {
                    if arm_or(ui, k) {
                        tool_then(ui, MEDIACTL, vec!["eject".into(), k[4..].to_owned()], None, |ui, out| {
                            notice(ui, if out.starts_with("OK") { tr(ui, 627, "Safe to remove") }
                                else { tr(ui, 628, "In use; close what uses it first") });
                            refresh_media(ui);
                        });
                    }
                }
                (62, "shots") => {
                    STATE.with(|st| { let mut st = st.borrow_mut(); st.with_screenshots = !st.with_screenshots; });
                    render(ui);
                }
                (62, "appdata") => {
                    STATE.with(|st| { let mut st = st.borrow_mut(); st.with_appdata = !st.with_appdata; });
                    render(ui);
                }
                (62, "backup") => {
                    /* Fresh volume list for the destination choice. */
                    tool_then(ui, MEDIACTL, vec!["list".into()], None, |ui, out| {
                        let volumes = parse_kv_lines(&out, "volume")
                            .into_iter()
                            .filter(|f| f.len() >= 6)
                            .map(|f| (f[0].to_owned(), f[2].to_owned(), f[3].parse().unwrap_or(0), f[4].parse().unwrap_or(0)))
                            .collect();
                        STATE.with(|st| st.borrow_mut().volumes = volumes);
                        backup_destinations(ui);
                    });
                }
                (62, k) if k.starts_with("restore:") => {
                    if arm_or(ui, k) {
                        start_restore(ui, k[8..].to_owned());
                    }
                }
                _ => {}
            }
        }
        _ => {}
    }
}

/* Dropdown choices opened by this module. */
pub fn apply_choice(ui: &HomeWindow, context: &str, value: String) {
    match context {
        "files-action" => {
            let key = focused_key(ui);
            let Some((path, _dir)) = selected_path(&key) else { return };
            let name = path.rsplit('/').next().unwrap_or("").to_owned();
            match value.as_str() {
                "open" => enter(ui, path),
                "copy" | "move" => {
                    STATE.with(|st| st.borrow_mut().clipboard = Some((path, value == "move")));
                    render(ui);
                }
                "rename" => open_system_keyboard(ui, &tr(ui, 617, "Rename"), KEYBOARD_RENAME, 58, "text", &name),
                "theme" => {
                    tool_then(ui, "/usr/bin/nuubos-themectl", vec!["install".into(), path], None, |ui, out| {
                        notice(ui, if out.starts_with("OK") { tr(ui, 649, "Theme installed") }
                            else { tr(ui, 650, "This is not a valid nuubOS theme") });
                        refresh_files(ui);
                    });
                }
                "delete" => {
                    let options = vec![("delete".to_owned(), tr(ui, 618, "Delete")), ("cancel".to_owned(), tr(ui, 619, "Cancel"))];
                    open_settings_choice(ui, "files-delete", &tr_arg(ui, 620, "Delete \"{0}\"?", &name), options, "cancel");
                }
                _ => {}
            }
        }
        "files-delete" if value == "delete" => {
            if let Some((path, _)) = selected_path(&focused_key(ui)) {
                file_command(ui, vec!["delete".into(), path]);
            }
        }
        "share-action" => {
            let key = focused_key(ui);
            let Some(name) = key.strip_prefix("share:").map(str::to_owned) else { return };
            tool_then(ui, SHARESCTL, vec![value.clone(), name], None, |ui, out| {
                if out.contains("unreachable") {
                    notice(ui, tr(ui, 645, "The share could not be reached"));
                }
                refresh_shares(ui);
            });
        }
        "backup-destination" => start_backup(ui, value),
        _ => {}
    }
}

/* System keyboard results for this module's purposes. */
pub fn keyboard_done(ui: &HomeWindow, purpose: i32, value: String) {
    let trimmed = value.trim().to_owned();
    match purpose {
        KEYBOARD_FOLDER | KEYBOARD_RENAME => {
            navigate_settings_view(ui, 58);
            render(ui);
            if trimmed.is_empty() {
                return;
            }
            let cwd = STATE.with(|st| st.borrow().cwd.clone());
            if purpose == KEYBOARD_FOLDER {
                STATE.with(|st| st.borrow_mut().focused = format!("d:{trimmed}"));
                file_command(ui, vec!["mkdir".into(), format!("{cwd}/{trimmed}")]);
            } else if let Some((path, dir)) = selected_path(&focused_key(ui)) {
                STATE.with(|st| st.borrow_mut().focused = format!("{}:{trimmed}", if dir { "d" } else { "f" }));
                file_command(ui, vec!["rename".into(), path, trimmed]);
            }
        }
        KEYBOARD_SHARE_NAME => {
            if trimmed.is_empty() {
                navigate_settings_view(ui, 60);
                render(ui);
                return;
            }
            STATE.with(|st| st.borrow_mut().share_draft.0 = trimmed);
            open_system_keyboard(ui, &tr(ui, 643, "Share address (//server/share)"), KEYBOARD_SHARE_URL, 60, "text", "//");
        }
        KEYBOARD_SHARE_URL => {
            STATE.with(|st| st.borrow_mut().share_draft.1 = trimmed);
            open_system_keyboard(ui, &tr(ui, 285, "Username"), KEYBOARD_SHARE_USER, 60, "text", "");
        }
        KEYBOARD_SHARE_USER => {
            STATE.with(|st| st.borrow_mut().share_draft.2 = trimmed);
            open_system_keyboard(ui, &tr(ui, 172, "Password"), KEYBOARD_SHARE_PASSWORD, 60, "password", "");
        }
        KEYBOARD_SHARE_PASSWORD => {
            ui.set_keyboard_value("".into());
            navigate_settings_view(ui, 60);
            let (name, url, user) = STATE.with(|st| std::mem::take(&mut st.borrow_mut().share_draft));
            let mut args = vec!["add".to_owned(), name.clone(), url];
            if !user.is_empty() {
                args.push(user);
            }
            /* The password goes on stdin, never on the command line. */
            tool_then(ui, SHARESCTL, args, Some(value), |ui, out| {
                if out.starts_with("ERR") {
                    notice(ui, tr(ui, 93, "Failed"));
                }
                refresh_shares(ui);
            });
        }
        _ => {}
    }
}
