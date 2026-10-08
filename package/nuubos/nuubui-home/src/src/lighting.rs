/*
 * RGB Lighting page (settings-view 17) over the Product Lighting contract
 * (org.nuubOS.Lighting1, nuubos-lightingd). The service owns effects,
 * signals and persistence; this module mirrors its snapshot, sends the
 * user's choices and previews effects/colours live on the LEDs while a
 * dropdown moves (EndPreview on Back, the choice on confirm).
 *
 * Rows: LIGHTING 0 Effect, 1 Color, 2 Brightness, 3 Speed | SYSTEM SIGNALS
 * 4 Startup & Sleep, 5 Battery, 6 Connections, 7 Games, 8 Tasks.
 */

use crate::{move_model_selection, open_settings_choice, sectioned_scroll, tr, HomeWindow};
use slint::{Color, ComponentHandle};
use std::thread;

const ROWS: i32 = 9;
const SIGNALS_START: i32 = 4;
const CATEGORIES: [&str; 5] = ["power", "battery", "connections", "games", "tasks"];
const EFFECTS: [(&str, usize, &str); 8] = [
    ("off", 54, "Off"),
    ("solid", 114, "Solid"),
    ("breathe", 115, "Breathing"),
    ("orbit", 116, "Orbit"),
    ("aurora", 117, "Aurora"),
    ("spectrum", 118, "Spectrum"),
    ("battery", 119, "Battery Level"),
    ("game", 120, "Console Colors"),
];
/* Same palette as lighting-color-index() in home.slint. */
const COLORS: [(&str, usize, &str); 11] = [
    ("theme", 749, "Theme Color"),
    ("3a86ff", 209, "Blue"),
    ("00c2e0", 750, "Cyan"),
    ("2ee6a6", 751, "Mint"),
    ("a6e22e", 752, "Lime"),
    ("ffb020", 753, "Amber"),
    ("ff6a1a", 754, "Orange"),
    ("ff2e3e", 755, "Red"),
    ("ff3ea5", 756, "Pink"),
    ("9b5cff", 757, "Violet"),
    ("ffffff", 758, "White"),
];

type State = (bool, String, String, i32, String, u32, String);

fn proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Lighting",
        "/org/nuubOS/Lighting",
        "org.nuubOS.Lighting1",
    )
}

fn swatch(hex: &str) -> Color {
    u32::from_str_radix(hex, 16)
        .map(|n| Color::from_rgb_u8((n >> 16) as u8, (n >> 8) as u8, n as u8))
        .unwrap_or(Color::from_rgb_u8(0x3a, 0x86, 0xff))
}

fn apply(ui: &HomeWindow, state: State) {
    let (supported, effect, color, brightness, speed, signals, _active) = state;
    ui.set_lighting_supported(supported);
    ui.set_lighting_swatch(swatch(&color));
    ui.set_lighting_effect(effect.into());
    ui.set_lighting_color(color.into());
    ui.set_lighting_brightness(brightness);
    ui.set_lighting_speed(speed.into());
    ui.set_lighting_signals(signals as i32);
}

pub fn refresh(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<State> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = proxy(&connection)?;
            let state: State = proxy.call("GetState", &())?;
            Ok(state)
        })();
        if let Ok(state) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply(&ui, state);
                }
            });
        }
    });
}

/* StateChanged keeps Settings and the Controllers row current. */
pub fn start_listener(ui: &HomeWindow) {
    refresh(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(connection) = zbus::blocking::Connection::system() else {
            return;
        };
        let Ok(proxy) = proxy(&connection) else {
            return;
        };
        let Ok(mut signals) = proxy.receive_signal("StateChanged") else {
            return;
        };
        for message in &mut signals {
            if let Ok(state) = message.body().deserialize::<State>() {
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply(&ui, state);
                    }
                });
            }
        }
    });
}

enum Call {
    Text(&'static str, String),
    Pair(&'static str, String, String),
    Int(&'static str, i32),
    Signal(String, bool),
    Plain(&'static str),
}

fn call(call: Call) {
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = proxy(&connection)?;
            match &call {
                Call::Text(method, value) => proxy.call(*method, &(value.as_str(),)),
                Call::Pair(method, a, b) => proxy.call(*method, &(a.as_str(), b.as_str())),
                Call::Int(method, value) => proxy.call(*method, &(*value,)),
                Call::Signal(category, enabled) => {
                    proxy.call("SetSignal", &(category.as_str(), *enabled))
                }
                Call::Plain(method) => proxy.call(*method, &()),
            }
        })();
        if let Err(error) = result {
            eprintln!("home: lighting call failed={error}");
        }
    });
}

fn open_effect_choice(ui: &HomeWindow) {
    let options = EFFECTS
        .iter()
        .map(|(value, index, fallback)| (value.to_string(), tr(ui, *index, fallback)))
        .collect();
    open_settings_choice(
        ui,
        "lighting-effect",
        &tr(ui, 126, "Effect"),
        options,
        ui.get_lighting_effect().as_str(),
    );
}

fn open_color_choice(ui: &HomeWindow) {
    let current = ui.get_lighting_color().to_string();
    let mut options: Vec<(String, String)> = COLORS
        .iter()
        .map(|(value, index, fallback)| (value.to_string(), tr(ui, *index, fallback)))
        .collect();
    if !COLORS.iter().any(|(value, _, _)| *value == current) {
        options.push((current.clone(), tr(ui, 759, "Custom")));
    }
    open_settings_choice(ui, "lighting-color", &tr(ui, 128, "Color"), options, &current);
}

/* Moving in an open lighting dropdown lights the highlighted entry. */
pub fn preview_choice(ui: &HomeWindow, context: &str, value: &str) {
    match context {
        "lighting-effect" => call(Call::Pair(
            "Preview",
            value.to_owned(),
            ui.get_lighting_color().to_string(),
        )),
        "lighting-color" => call(Call::Pair(
            "Preview",
            ui.get_lighting_effect().to_string(),
            value.to_owned(),
        )),
        _ => {}
    }
}

/* The dropdown closed without a choice. */
pub fn end_preview() {
    call(Call::Plain("EndPreview"));
}

pub fn apply_choice(context: &str, value: String) {
    match context {
        "lighting-effect" => call(Call::Text("SetEffect", value)),
        "lighting-color" => call(Call::Text("SetColor", value)),
        _ => {}
    }
}

fn speed_capable(effect: &str) -> bool {
    matches!(effect, "breathe" | "orbit" | "aurora" | "spectrum")
}

fn color_capable(effect: &str) -> bool {
    matches!(effect, "solid" | "breathe" | "orbit" | "aurora")
}

fn move_selection(ui: &HomeWindow, delta: i32) {
    let index = move_model_selection(ui.get_lighting_index(), ROWS, delta);
    ui.set_lighting_index(index);
    ui.set_lighting_scroll(sectioned_scroll(
        ui,
        index,
        ROWS,
        ui.get_lighting_scroll(),
        (SIGNALS_START, 0, 0),
    ));
}

pub fn handle_page(ui: &HomeWindow, action: &str) {
    let index = ui.get_lighting_index();
    let effect = ui.get_lighting_effect().to_string();
    match (action, index) {
        ("menu_up", _) => move_selection(ui, -1),
        ("menu_down", _) => move_selection(ui, 1),
        ("menu_confirm", 0) => open_effect_choice(ui),
        ("menu_confirm", 1) if color_capable(&effect) => open_color_choice(ui),
        ("menu_left", 2) => {
            let value = (ui.get_lighting_brightness() - 10).max(10);
            ui.set_lighting_brightness(value);
            call(Call::Int("SetBrightness", value));
        }
        ("menu_right" | "menu_confirm", 2) => {
            let value = (ui.get_lighting_brightness() + 10).min(100);
            ui.set_lighting_brightness(value);
            call(Call::Int("SetBrightness", value));
        }
        ("menu_left" | "menu_right" | "menu_confirm", 3) if speed_capable(&effect) => {
            let order = ["slow", "normal", "fast"];
            let current = order
                .iter()
                .position(|s| *s == ui.get_lighting_speed().as_str())
                .unwrap_or(1) as i32;
            let next = if action == "menu_left" { current - 1 } else { current + 1 };
            let next = order[next.rem_euclid(3) as usize];
            ui.set_lighting_speed(next.into());
            call(Call::Text("SetSpeed", next.to_owned()));
        }
        ("menu_left" | "menu_right" | "menu_confirm", k) if (SIGNALS_START..ROWS).contains(&k) => {
            let bit = 1 << (k - SIGNALS_START);
            let enabled = ui.get_lighting_signals() & bit == 0;
            ui.set_lighting_signals(ui.get_lighting_signals() ^ bit);
            call(Call::Signal(
                CATEGORIES[(k - SIGNALS_START) as usize].to_owned(),
                enabled,
            ));
        }
        _ => {}
    }
}

/* Entering the page starts from the top. */
pub fn reset_page(ui: &HomeWindow) {
    ui.set_lighting_index(0);
    ui.set_lighting_scroll(0);
}
