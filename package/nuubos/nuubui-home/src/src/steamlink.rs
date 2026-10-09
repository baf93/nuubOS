/*
 * Steam Link (EPIC-026): the nuubUI page of Valve's Steam Link application.
 *
 * nuubos-streamd owns everything: downloading Valve's build on the user's
 * request (it is never part of the image), updating and removing it, and
 * running it as a session like a Moonlight stream. This page only shows the
 * streamd snapshot (moonlight::SNAPSHOT, shared listener) and sends
 * commands. Valve's own interface handles the PCs, pairing and streams.
 *
 * Settings shell like Moonlight, settings-view 45. Rows: absent → Download;
 * downloading → progress (A cancels); installed → Open, Update / Check for
 * Updates, Remove (second press).
 */

use crate::moonlight::{command, send, Snapshot, SNAPSHOT};
use crate::{
    move_model_selection, navigate_settings_view, play_ui_sound, tr, tr_arg, write_ui_context,
    HomeWindow,
};
use slint::ComponentHandle;
use std::cmp::Ordering as CmpOrdering;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

/* The Steam Link page owns Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);

/* "1.3.32.316" > "1.3.9.300", numerically per component. */
fn compare_versions(a: &str, b: &str) -> CmpOrdering {
    let parse = |v: &str| v.split('.').map(|p| p.parse::<u64>().unwrap_or(0)).collect::<Vec<_>>();
    parse(a).cmp(&parse(b))
}

fn update_available(s: &Snapshot) -> bool {
    !s.steamlink_latest.is_empty()
        && compare_versions(&s.steamlink_latest, &s.steamlink_version) == CmpOrdering::Greater
}

fn row_count(ui: &HomeWindow) -> i32 {
    if ui.get_steamlink_state() == "installed" {
        3
    } else {
        1
    }
}

pub fn apply(ui: &HomeWindow, s: &Snapshot) {
    let state = if s.steamlink_job == "installing" {
        "installing"
    } else if s.steamlink_installed {
        "installed"
    } else {
        "absent"
    };
    ui.set_steamlink_state(state.into());
    ui.set_steamlink_progress_label(format!("{}%", s.steamlink_progress.max(0)).into());
    ui.set_steamlink_download_detail(match s.steamlink_error.as_str() {
        "" => "".into(),
        "space" => tr(ui, 493, "Not enough free space").into(),
        "network" => tr(ui, 494, "Check the network connection").into(),
        _ => tr(ui, 492, "Steam Link could not be downloaded").into(),
    });
    ui.set_steamlink_version_label(format!("{} {}", tr(ui, 485, "Version"), s.steamlink_version).into());
    ui.set_steamlink_update_title(if update_available(s) {
        tr_arg(ui, 486, "Update to {0}", &s.steamlink_latest).into()
    } else {
        tr(ui, 487, "Check for Updates").into()
    });
    ui.set_steamlink_update_value(if s.steamlink_job == "checking" {
        tr(ui, 488, "Checking…").into()
    } else if !s.steamlink_latest.is_empty() && !update_available(s) {
        tr(ui, 489, "Up to date").into()
    } else {
        "".into()
    });
    let count = row_count(ui);
    if ui.get_steamlink_index() >= count {
        ui.set_steamlink_index(count - 1);
        ui.set_steamlink_remove_confirm(false);
    }
    crate::apppage::refresh(ui);
}

pub fn open(ui: &HomeWindow) {
    ACTIVE.store(true, Ordering::SeqCst);
    ui.set_steamlink_active(true);
    ui.set_steamlink_index(0);
    ui.set_steamlink_notice("".into());
    ui.set_steamlink_remove_confirm(false);
    navigate_settings_view(ui, 45);
    ui.set_settings_open(true);
    write_ui_context("steamlink");
    let snapshot = SNAPSHOT.lock().unwrap().clone();
    if let Some(s) = snapshot {
        apply(ui, &s);
        /* One request to Valve: is there a newer build? */
        if s.steamlink_installed && s.steamlink_job == "idle" {
            send("STEAMLINK_CHECK".into());
        }
    }
}

/* Back to Home (also when the user picker takes over). */
pub fn leave(ui: &HomeWindow) {
    ACTIVE.store(false, Ordering::SeqCst);
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_steamlink_active(false);
    write_ui_context("home");
}

fn launch(ui: &HomeWindow) {
    let weak = ui.as_weak();
    ui.set_steamlink_notice("".into());
    thread::spawn(move || {
        let reply = command("STEAMLINK_LAUNCH").unwrap_or_default();
        if reply == "OK" || reply == "ERR busy" {
            /* Started (Home hides under it), or a double press. */
            return;
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_steamlink_notice(tr(&ui, 497, "Steam Link could not be started").into());
            }
        });
    });
}

pub fn handle_action(ui: &HomeWindow, action: &str) {
    let index = ui.get_steamlink_index();
    let count = row_count(ui);
    play_ui_sound(action);
    match action {
        "menu_up" | "menu_down" | "menu_left" | "menu_right" => {
            let back = action == "menu_up" || action == "menu_left";
            ui.set_steamlink_index(move_model_selection(index, count, if back { -1 } else { 1 }));
            ui.set_steamlink_remove_confirm(false);
            return;
        }
        "menu_back" => {
            leave(ui);
            return;
        }
        "menu_confirm" => {}
        _ => return,
    }
    let Some(s) = SNAPSHOT.lock().unwrap().clone() else { return };
    ui.set_steamlink_notice("".into());
    match (ui.get_steamlink_state().as_str(), index) {
        ("absent", _) => send("STEAMLINK_INSTALL".into()),
        ("installing", _) => send("STEAMLINK_CANCEL".into()),
        (_, 0) => launch(ui),
        (_, 1) if update_available(&s) => send("STEAMLINK_INSTALL".into()),
        (_, 1) => send("STEAMLINK_CHECK".into()),
        (_, 2) if ui.get_steamlink_remove_confirm() => {
            ui.set_steamlink_remove_confirm(false);
            ui.set_steamlink_index(0);
            send("STEAMLINK_REMOVE".into());
        }
        (_, 2) => ui.set_steamlink_remove_confirm(true),
        _ => {}
    }
}
