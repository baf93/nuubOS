/*
 * Themes (EPIC-008): the active user's theme package sets the Theme
 * tokens of home.slint. nuubos-themectl owns packages, validation and the
 * per-user selection; this module only reads the selected package's color
 * tokens. Anything invalid keeps the built-in values; safe mode always uses
 * them (EPIC-050). The theme dropdown previews each theme live and restores
 * the selected one when it is closed without a choice.
 */

use crate::{open_settings_choice, tr, HomeWindow, Theme};
use slint::{Color, ComponentHandle};
use std::process::Command;
use std::thread;

const THEMECTL: &str = "/usr/bin/nuubos-themectl";

/* Built-in nuubOS values, in token order. */
const DEFAULTS: &[(&str, &str)] = &[
    ("accent", "#3a86ff"), ("accent-light", "#73a8ff"), ("accent-muted", "#5b86d6"),
    ("accent-text", "#cfe2ff"), ("focus-fill", "#18283c"), ("text", "#f5f6f8"),
    ("text-secondary", "#a8adb5"), ("text-dim", "#8e949d"), ("text-faint", "#7f858e"),
    ("text-disabled", "#555b64"), ("background", "#0d0e10"), ("surface", "#24272d"),
    ("border", "#30343b"), ("border-soft", "#2c3036"), ("warning", "#f2b84b"),
];

fn parse_color(v: &str) -> Option<Color> {
    let hex = v.strip_prefix('#')?;
    if hex.len() != 6 {
        return None;
    }
    let n = u32::from_str_radix(hex, 16).ok()?;
    Some(Color::from_rgb_u8((n >> 16) as u8, (n >> 8) as u8, n as u8))
}

/* Token values of a theme folder: defaults overridden by valid entries. */
pub fn load(path: &str) -> Vec<(String, Color)> {
    let text = std::fs::read_to_string(format!("{path}/theme.conf")).unwrap_or_default();
    DEFAULTS
        .iter()
        .map(|(k, d)| {
            let v = text
                .lines()
                .find_map(|l| l.strip_prefix(*k).and_then(|r| r.strip_prefix('=')))
                .and_then(|v| parse_color(v.trim()))
                .unwrap_or_else(|| parse_color(d).unwrap());
            ((*k).to_owned(), v)
        })
        .collect()
}

pub fn apply(ui: &HomeWindow, tokens: &[(String, Color)]) {
    let t = ui.global::<Theme>();
    for (k, c) in tokens {
        match k.as_str() {
            "accent" => t.set_accent(*c),
            "accent-light" => t.set_accent_light(*c),
            "accent-muted" => t.set_accent_muted(*c),
            "accent-text" => t.set_accent_text(*c),
            "focus-fill" => t.set_focus_fill(*c),
            "text" => t.set_text(*c),
            "text-secondary" => t.set_text_secondary(*c),
            "text-dim" => t.set_text_dim(*c),
            "text-faint" => t.set_text_faint(*c),
            "text-disabled" => t.set_text_disabled(*c),
            "background" => t.set_background(*c),
            "surface" => t.set_surface(*c),
            "border" => t.set_border(*c),
            "border-soft" => t.set_border_soft(*c),
            "warning" => t.set_warning(*c),
            _ => {}
        }
    }
}

fn tool(args: &[&str]) -> String {
    Command::new(THEMECTL)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default()
}

fn safe_mode() -> bool {
    std::env::var("NUUBOS_SAFE_MODE").as_deref() == Ok("1")
}

/* The active user's selected theme (startup, user switch, closed preview). */
pub fn apply_active(ui: &HomeWindow) {
    let user = ui.get_active_user_id().to_string();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let path = if user.is_empty() || safe_mode() {
            String::new()
        } else {
            let out = tool(&["selected", &user]);
            out.lines().find_map(|l| l.strip_prefix("path=")).unwrap_or("").to_owned()
        };
        let tokens = load(&path);
        let name = current_name(&path);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                apply(&ui, &tokens);
                ui.set_theme_name(name.into());
            }
        });
    });
}

/* General → Theme: installed themes; moving through them previews. */
pub fn open_choice(ui: &HomeWindow) {
    let user = ui.get_active_user_id().to_string();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let list = tool(&["list"]);
        let current = tool(&["selected", &user]);
        let current = current.lines().find_map(|l| l.strip_prefix("id=")).unwrap_or("nuubos").to_owned();
        let options: Vec<(String, String)> = list
            .lines()
            .filter_map(|l| l.strip_prefix("theme="))
            .filter_map(|v| {
                let f: Vec<&str> = v.split('\t').collect();
                (f.len() >= 6).then(|| (f[5].to_owned(), format!("{}  •  {}", f[1], f[2])))
            })
            .collect();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let current_path = options
                .iter()
                .map(|(p, _)| p.clone())
                .find(|p| p.ends_with(&format!("/{current}")))
                .unwrap_or_default();
            open_settings_choice(&ui, "theme", &tr(&ui, 194, "Theme"), options, &current_path);
        });
    });
}

/* Dropdown focus moved: show that theme on the real UI. */
pub fn preview(ui: &HomeWindow, path: &str) {
    if !safe_mode() {
        apply(ui, &load(path));
    }
}

/* Confirmed: the theme folder's id becomes the user's selection. */
pub fn select(ui: &HomeWindow, path: String) {
    let user = ui.get_active_user_id().to_string();
    let id = path.rsplit('/').next().unwrap_or("").to_owned();
    let weak = ui.as_weak();
    thread::spawn(move || {
        let _ = tool(&["select", &user, &id]);
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                apply_active(&ui);
            }
        });
    });
}

/* The selected theme's name for the General row. */
pub fn current_name(path: &str) -> String {
    std::fs::read_to_string(format!("{path}/theme.conf"))
        .ok()
        .and_then(|t| t.lines().find_map(|l| l.strip_prefix("NAME=").map(str::to_owned)))
        .unwrap_or_else(|| "nuubOS".into())
}
