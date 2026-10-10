slint::include_modules!();

use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType, PremultipliedRgbaColor};
use slint::platform::{Platform, WindowAdapter};
use slint::{Model, ModelRc, SharedString, VecModel};
use std::rc::Rc;

struct P(Rc<MinimalSoftwareWindow>);
impl Platform for P {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
        Ok(self.0.clone())
    }
}

fn gen(rows: &[(&str, &str, &str, bool, i32)]) -> ModelRc<GenRow> {
    /* (title, value, detail, navigates, toggle: -1 none, 0 off, 1 on) */
    let v: Vec<GenRow> = rows
        .iter()
        .map(|(t, v, d, n, tg)| GenRow {
            title: (*t).into(),
            value: (*v).into(),
            detail: (*d).into(),
            navigates: *n,
            toggle: *tg >= 0,
            toggle_on: *tg == 1,
            enabled: true,
            armed: false,
        })
        .collect();
    ModelRc::from(Rc::new(VecModel::from(v)))
}

/* Game Details page: (icon, label, primary, on, armed). */
fn actions(items: &[(i32, String, bool, bool, bool)]) -> ModelRc<DetailAction> {
    let v: Vec<DetailAction> = items
        .iter()
        .map(|(icon, label, primary, on, armed)| DetailAction {
            icon: *icon, label: label.into(), primary: *primary, on: *on, enabled: true, armed: *armed, busy: false,
        })
        .collect();
    ModelRc::from(Rc::new(VecModel::from(v)))
}

fn chips(items: &[(i32, &str, bool)]) -> ModelRc<DetailChip> {
    let v: Vec<DetailChip> = items.iter().map(|(i, t, w)| DetailChip { icon: *i, text: (*t).into(), warn: *w }).collect();
    ModelRc::from(Rc::new(VecModel::from(v)))
}

/* Synthetic art (no third-party images): a box-art-like vertical gradient
 * with a band, and a soft two-colour backdrop. */
fn art(w: u32, h: u32, top: (u8, u8, u8), bottom: (u8, u8, u8), band: bool) -> slint::Image {
    let mut b = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(w, h);
    for (i, p) in b.make_mut_slice().iter_mut().enumerate() {
        let (x, y) = (i as u32 % w, i as u32 / w);
        let t = y as f32 / h as f32;
        let mix = |a: u8, c: u8| (a as f32 * (1.0 - t) + c as f32 * t) as u8;
        let on_band = band && y > h * 6 / 100 && y < h * 22 / 100 && x > w / 10 && x < w * 9 / 10;
        *p = if on_band {
            slint::Rgba8Pixel { r: 250, g: 210, b: 60, a: 255 }
        } else {
            slint::Rgba8Pixel { r: mix(top.0, bottom.0), g: mix(top.1, bottom.1), b: mix(top.2, bottom.2), a: 255 }
        };
    }
    slint::Image::from_rgba8(b)
}

fn s(ui: &HomeWindow, i: usize) -> String {
    ui.get_i18n_strings().row_data(i).unwrap_or_default().to_string()
}

fn strings() -> ModelRc<SharedString> {
    let lang = std::env::var("LANG_FILE").unwrap_or("/i18n/en.lang".into());
    let v: Vec<SharedString> = std::fs::read_to_string(lang)
        .unwrap()
        .lines()
        .filter_map(|l| l.split_once('=').map(|(_, v)| v.into()))
        .collect();
    ModelRc::from(Rc::new(VecModel::from(v)))
}

fn shot(win: &Rc<MinimalSoftwareWindow>, w: u32, h: u32, name: &str) {
    win.set_size(slint::PhysicalSize::new(w, h));
    // Let 180 ms Home scroll animations settle.
    std::thread::sleep(std::time::Duration::from_millis(220));
    slint::platform::update_timers_and_animations();
    win.request_redraw();
    let mut buf = vec![PremultipliedRgbaColor::default(); (w * h) as usize];
    win.draw_if_needed(|r| {
        r.render(&mut buf, w as usize);
    });
    let f = std::fs::File::create(format!("/out/{name}_{w}x{h}.png")).unwrap();
    let mut e = png::Encoder::new(std::io::BufWriter::new(f), w, h);
    e.set_color(png::ColorType::Rgb);
    e.set_depth(png::BitDepth::Eight);
    let mut wr = e.write_header().unwrap();
    let raw: Vec<u8> = buf.iter().flat_map(|p| [p.red, p.green, p.blue]).collect();
    wr.write_image_data(&raw).unwrap();
}

fn quick_menu(win: &Rc<MinimalSoftwareWindow>) {
    let qm = QuickMenuWindow::new().unwrap();
    qm.show().unwrap();
    qm.set_i18n_strings(strings());
    qm.set_lifecycle_active(false);
    qm.set_battery_percent(78);
    qm.set_battery_label("78%".into());
    qm.set_clock_label("14:32".into());
    /* Battery Saver active: the glyph takes the warning colour. */
    qm.set_battery_saver(true);
    qm.set_audio_output_label("Speaker • 64%".into());
    qm.set_audio_volume_fraction(0.64);
    qm.set_brightness_visible(true);
    qm.set_brightness_label("60%".into());
    qm.set_brightness_fraction(0.6);
    qm.set_switch_user_visible(true);
    qm.set_confirm_face_position(2);
    qm.set_back_face_position(1);
    let sizes = [(640u32, 480u32), (720, 720), (1280, 720), (1920, 1080)];
    for (name, sel, music, dropdown) in [("qm_audio", 3, false, false), ("qm_music", 7, true, false), ("qm_poweroff", 2, true, false), ("qm_dropdown", 3, false, true)] {
        qm.set_selected_index(sel);
        qm.set_home_music_playing(music);
        qm.set_audio_output_dropdown_open(dropdown);
        let o: Vec<SharedString> = ["Automatic", "Bluetooth Audio", "Speaker", "HDMI"].iter().map(|s| (*s).into()).collect();
        qm.set_audio_output_options(ModelRc::from(Rc::new(VecModel::from(o))));
        qm.set_audio_output_dropdown_index(2);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }

    /* Power OSD (power key, every context): Sleep, then Restart and Power
     * Off armed. */
    qm.set_power_osd_open(true);
    qm.set_menu_panel_visible(false);
    for (name, index, confirm) in [("power_menu", 0, false), ("power_armed", 2, true)] {
        qm.set_power_osd_index(index);
        qm.set_power_osd_confirm(confirm);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_power_osd_open(false);
    qm.set_menu_panel_visible(true);

    /* Over a running game: GAME section first (Resume selected), the
     * State Slot row (dropdown), the Performance Overlay switch and Quit
     * waiting for its second press. */
    qm.set_audio_output_dropdown_open(false);
    qm.set_home_music_playing(false);
    qm.set_switch_user_visible(false);
    qm.set_game_section_visible(true);
    qm.set_game_slot_label("Slot 3".into());
    qm.set_game_overlay_on(true);
    for (name, sel, confirm) in [("qm_game", 8, -1), ("qm_game_slot", 11, -1), ("qm_game_overlay", 28, -1), ("qm_game_quit", 14, 14)] {
        qm.set_selected_index(sel);
        qm.set_game_confirm_index(confirm);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    /* State Slot dropdown (every slot, save time or Empty). */
    qm.set_selected_index(11);
    let slot_options: Vec<slint::SharedString> = (0..10)
        .map(|n| if n < 3 { format!("Slot {n} • today 21:0{n}") } else { format!("Slot {n} • Empty") }.into())
        .collect();
    qm.set_game_slot_options(ModelRc::from(Rc::new(VecModel::from(slot_options))));
    qm.set_game_slot_dropdown_index(2);
    qm.set_game_slot_dropdown_scroll(0);
    qm.set_game_slot_dropdown_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_slot_dropdown");
    }
    qm.set_game_slot_dropdown_open(false);
    /* Audio output dropdown over a game: the row is near the bottom, the
     * dropdown opens above it. */
    qm.set_selected_index(3);
    qm.set_audio_output_dropdown_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_audio_dropdown");
    }
    qm.set_audio_output_dropdown_open(false);
    qm.set_game_section_visible(false);
    qm.set_game_confirm_index(-1);
    /* PC stream (EPIC-025): STREAM section with the sampled statistics. */
    qm.set_stream_section_visible(true);
    qm.set_stream_latency_label("4 ms".into());
    qm.set_stream_decode_label("1.6 ms".into());
    qm.set_stream_video_label("640×480 • 60 fps".into());
    for (name, sel, confirm) in [("qm_stream", 15, -1), ("qm_stream_close", 17, 17)] {
        qm.set_selected_index(sel);
        qm.set_game_confirm_index(confirm);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    /* Steam Link session (EPIC-026): Resume and Quit Steam Link only. */
    qm.set_stream_steamlink(true);
    qm.set_selected_index(16);
    qm.set_game_confirm_index(-1);
    for (w, h) in sizes {
        shot(win, w, h, "qm_steamlink");
    }
    qm.set_stream_steamlink(false);
    qm.set_stream_section_visible(false);
    qm.set_game_confirm_index(-1);
    /* Web Mode (EPIC-030): WEB section (Zoom adjustable), then the
     * keyboard bottom sheet surface (270 logical px high). */
    qm.set_web_section_visible(true);
    qm.set_web_zoom_label("110%".into());
    qm.set_web_ime_active(false);
    qm.set_selected_index(24);
    for (w, h) in sizes {
        shot(win, w, h, "qm_web");
    }
    qm.set_web_section_visible(false);
    qm.set_menu_panel_visible(false);
    qm.set_surface_mode(2);
    let row = |keys: &[&str]| -> ModelRc<SharedString> {
        ModelRc::from(Rc::new(VecModel::from(keys.iter().map(|k| SharedString::from(*k)).collect::<Vec<_>>())))
    };
    qm.set_kb_row_zero(row(&["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"]));
    qm.set_kb_row_one(row(&["a", "s", "d", "f", "g", "h", "j", "k", "l", "'"]));
    qm.set_kb_row_two(row(&["⇧", "z", "x", "c", "v", "b", "n", "m", ",", "?"]));
    qm.set_kb_row_three(row(&["123", "#+=", "@", "/", "␣", "␣", ".", "-", "⌫", "Done"]));
    qm.set_kb_preview("nuubos handheld".into());
    qm.set_kb_mode_label("QWERTY".into());
    qm.set_kb_index(14);
    for w in [640u32, 720, 1280] {
        shot(win, w, 270, "qm_keyboard");
    }
    qm.set_surface_mode(0);
    qm.set_menu_panel_visible(true);
    qm.set_switch_user_visible(true);

    /* Notifications: the copy drawn over the open Quick Menu, then the
     * persistent 416x88 notification surface (NotificationWindow). */
    let s = strings();
    let t = |i: usize| s.row_data(i).unwrap_or_default();
    qm.set_selected_index(3);
    qm.set_home_music_playing(false);
    qm.set_audio_output_dropdown_open(false);
    qm.set_toast_visible(true);
    qm.set_toast_icon(2);
    qm.set_toast_severity(0);
    qm.set_toast_title(t(370));
    qm.set_toast_detail(format!("Xbox Wireless Controller • {}1", t(220)).into());
    qm.set_toast_battery(64);
    qm.set_toast_progress(-1.0);
    for (w, h) in sizes {
        shot(win, w, h, "notif_over_menu");
    }
    qm.hide().unwrap();
    drop(qm);
    let card = NotificationWindow::new().unwrap();
    card.show().unwrap();
    let pct = |i: usize, v: &str| SharedString::from(t(i).replace("{0}", v));
    let cards: [(&str, i32, i32, SharedString, SharedString, i32, f32); 9] = [
        ("music", 1, 0, t(369), "Airport Lounge — Kevin MacLeod".into(), -1, -1.0),
        ("headphones", 3, 0, t(372), "Beats Studio3 Wireless".into(), 80, -1.0),
        ("saver", 6, 0, t(376), pct(377, "20"), -1, -1.0),
        ("low", 5, 2, t(378), pct(379, "15"), -1, -1.0),
        ("critical", 5, 3, t(380), pct(381, "5"), -1, -1.0),
        ("pad_low", 2, 2, t(378), "8BitDo Ultimate 2C Wireless Controller".into(), 12, -1.0),
        ("wifi_lost", 8, 2, t(385), "CasaBaf-5G".into(), -1, -1.0),
        ("backup", 9, 0, t(97), t(91), -1, 0.42),
        ("game_saved", 10, 1, t(424), pct(423, "3"), -1, -1.0),
    ];
    for (name, icon, severity, title, detail, battery, progress) in cards {
        card.set_toast_icon(icon);
        card.set_toast_severity(severity);
        card.set_toast_title(title);
        card.set_toast_detail(detail);
        card.set_toast_battery(battery);
        card.set_toast_progress(progress);
        shot(win, 416, 88, &format!("notif_{name}"));
    }
}

fn main() {
    let win = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    slint::platform::set_platform(Box::new(P(win.clone()))).unwrap();
    if std::env::var("SCENES").as_deref() == Ok("qm") {
        quick_menu(&win);
        return;
    }
    let ui = HomeWindow::new().unwrap();
    ui.show().unwrap();
    ui.set_i18n_strings(strings());
    ui.set_curtain_progress(1.0);
    ui.set_line_progress(1.0);
    std::thread::sleep(std::time::Duration::from_millis(600));
    slint::platform::update_timers_and_animations();

    ui.set_topbar_time("14:32".into());
    ui.set_battery_percent(78);
    ui.set_battery_label("78%".into());
    ui.set_wifi_state("connected".into());
    ui.set_current_user_name("Fabio".into());
    ui.set_user_count(2);
    ui.set_ui_language_name("Italiano".into());
    ui.set_timezone("Europe/Rome".into());
    ui.set_keyboard_layout_label("Italiano".into());
    ui.set_last_time_sync_label("14:02".into());
    ui.set_manual_date_label("2026-10-05".into());
    ui.set_manual_time_label("14:32".into());
    ui.set_wifi_enabled(true);
    ui.set_bluetooth_enabled(true);
    ui.set_connectivity_wifi_active(true);
    ui.set_wifi_current_ssid("CasaBaf-5G".into());
    ui.set_system_storage_active("TF1".into());
    ui.set_system_storage_health("OK".into());
    ui.set_system_version("0.5".into());
    ui.set_system_build("2026.10".into());
    ui.set_system_device_model("Anbernic RG40XX-V".into());
    ui.set_system_kernel("Linux 7.2.8".into());
    ui.set_system_cpu_clock("1.51 GHz".into());
    ui.set_system_cpu_temperature("47 °C".into());
    ui.set_system_sleep_after_min(5);
    ui.set_battery_percent(78);
    ui.set_battery_state("discharging".into());
    ui.set_battery_health("good".into());
    ui.set_power_devices(slint::ModelRc::new(slint::VecModel::from(vec![
        PowerDevice { name: "Xbox Wireless Controller".into(), kind: "controller".into(), battery: 64 },
        PowerDevice { name: "Beats Studio3".into(), kind: "headphones".into(), battery: -1 },
    ])));
    ui.set_audio_output_label("Automatic • Speaker".into());
    let nets = vec![
        WifiNetworkEntry { ssid: "CasaBaf-5G".into(), security: "WPA2".into(), signal_dbm: -48, saved: true, current: true },
        WifiNetworkEntry { ssid: "Vodafone-A1B2C3D4E5 Extended Network Name".into(), security: "WPA2".into(), signal_dbm: -70, saved: false, current: false },
        WifiNetworkEntry { ssid: "Guest".into(), security: "Open".into(), signal_dbm: -80, saved: true, current: false },
    ];
    ui.set_wifi_networks(ModelRc::from(Rc::new(VecModel::from(nets))));

    let sizes = [(640u32, 480u32), (720, 720), (1280, 720), (1920, 1080)];
    let mut scenes: Vec<(&str, Box<dyn Fn(&HomeWindow)>)> = vec![
        ("home", Box::new(|ui| { ui.set_settings_open(false); })),
        ("general_preview", Box::new(|ui| { ui.set_settings_open(true); ui.set_settings_view(0); ui.set_settings_selected_index(0); ui.set_general_scroll_offset(0); })),
        ("general_toggle", Box::new(|ui| { ui.set_settings_view(23); ui.set_general_index(5); ui.set_general_scroll_offset(2); })),
        ("general_profile", Box::new(|ui| { ui.set_settings_view(24); ui.set_profile_index(0); })),
        ("general_users", Box::new(|ui| { ui.set_settings_view(25); ui.set_user_list_index(0); })),
        ("connectivity", Box::new(|ui| { ui.set_settings_selected_index(1); ui.set_settings_view(1); ui.set_connectivity_selected_index(4); })),
        ("connectivity_services", Box::new(|ui| {
            ui.set_connectivity_services(gen(&[(&s(ui, 600), "", "root@192.168.5.36", false, 1), (&s(ui, 602), "", "\\\\192.168.5.36\\nuubOS", false, 0),
                (&s(ui, 603), "", "http://192.168.5.36", false, 0), (&s(ui, 172), "••••-••••-••••", &s(ui, 608), false, -1),
                (&s(ui, 606), "", &s(ui, 607), false, -1), (&s(ui, 669), &s(ui, 54), &s(ui, 670), true, -1)]));
            ui.set_settings_selected_index(1); ui.set_settings_view(1); ui.set_connectivity_selected_index(9); ui.set_connectivity_scroll(6);
        })),
        ("wifi_list", Box::new(|ui| { ui.set_settings_view(2); ui.set_wifi_network_index(2); ui.set_wifi_network_scroll_offset(0); })),
        ("keyboard", Box::new(|ui| {
            /* System keyboard bottom sheet (settings-view 6) entering a Wi-Fi password. */
            let row = |keys: &[&str]| -> ModelRc<SharedString> {
                ModelRc::from(Rc::new(VecModel::from(keys.iter().map(|k| SharedString::from(*k)).collect::<Vec<_>>())))
            };
            ui.set_keyboard_return_view(2);
            ui.set_keyboard_row_zero(row(&["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"]));
            ui.set_keyboard_row_one(row(&["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"]));
            ui.set_keyboard_row_two(row(&["a", "s", "d", "f", "g", "h", "j", "k", "l", "⌫"]));
            ui.set_keyboard_row_three(row(&["⇧", "z", "x", "c", "v", "b", "n", "m", "Space", "Done"]));
            ui.set_keyboard_title(format!("{} • CasaBaf-5G", s(ui, 172)).into());
            ui.set_keyboard_value("nuubos handheld".into());
            ui.set_keyboard_index(14);
            ui.set_settings_view(6);
        })),
        ("keyboard_caps", Box::new(|ui| {
            /* Password field, Caps Lock on: the letters show their case. */
            let row = |keys: &[&str]| -> ModelRc<SharedString> {
                ModelRc::from(Rc::new(VecModel::from(keys.iter().map(|k| SharedString::from(*k)).collect::<Vec<_>>())))
            };
            ui.set_settings_open(true); ui.set_settings_selected_index(3);
            ui.set_keyboard_row_zero(row(&["Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"]));
            ui.set_keyboard_row_one(row(&["A", "S", "D", "F", "G", "H", "J", "K", "L", "Back"]));
            ui.set_keyboard_row_two(row(&["Z", "X", "C", "V", "B", "N", "M", ".", "-", "_"]));
            ui.set_keyboard_row_three(row(&["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"]));
            ui.set_keyboard_title(s(ui, 172).into());
            ui.set_keyboard_input_kind("password".into()); ui.set_keyboard_secret(true); ui.set_keyboard_page(0);
            ui.set_keyboard_shift(true); ui.set_keyboard_caps(true);
            ui.set_keyboard_value("Secret".into());
            ui.set_keyboard_index(30);
            ui.set_settings_view(6);
        })),
        ("display_audio", Box::new(|ui| { ui.set_settings_selected_index(4); ui.set_settings_view(18); ui.set_display_audio_index(2); ui.set_display_audio_scroll_offset(0); })),
        ("display_audio_sounds", Box::new(|ui| { ui.set_display_color_temperature_available(true); ui.set_display_audio_index(9); ui.set_display_audio_scroll_offset(5); })),
        ("system", Box::new(|ui| { ui.set_display_color_temperature_available(false); ui.set_settings_selected_index(6); ui.set_settings_view(19); ui.set_system_index(4); ui.set_system_scroll_offset(0); })),
        ("power", Box::new(|ui| { ui.set_settings_selected_index(5); ui.set_settings_view(69); ui.set_power_index(1); ui.set_power_scroll_offset(0); })),
        ("power_devices", Box::new(|ui| { ui.set_settings_selected_index(5); ui.set_settings_view(69); ui.set_power_index(5); ui.set_power_scroll_offset(3); })),
        ("controllers", Box::new(|ui| { ui.set_settings_selected_index(2); ui.set_settings_view(0); })),
        ("lighting", Box::new(|ui| {
            ui.set_lighting_supported(true); ui.set_lighting_effect("orbit".into()); ui.set_lighting_color("theme".into());
            ui.set_lighting_brightness(60); ui.set_lighting_speed("normal".into()); ui.set_lighting_signals(0b10111);
            ui.set_settings_selected_index(2); ui.set_settings_view(17); ui.set_lighting_index(1); ui.set_lighting_scroll(0);
        })),
        ("lighting_signals", Box::new(|ui| {
            ui.set_lighting_effect("spectrum".into()); ui.set_lighting_index(7); ui.set_lighting_scroll(4);
        })),
        ("gaming_preview", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_details_active(false);
            ui.set_settings_selected_index(3); ui.set_settings_view(0);
            ui.set_gaming_rows(gen(&[(&s(ui, 584), "alice", "", true, -1),
                (&s(ui, 767), "", &s(ui, 768), false, 0), (&s(ui, 769), "10", "", true, -1),
                (&s(ui, 770), "", &s(ui, 771), false, 1), (&s(ui, 772), "", "", true, -1),
                (&s(ui, 774), &s(ui, 54), "", true, -1), (&s(ui, 775), "", &s(ui, 776), false, 0),
                (&s(ui, 777), "", &s(ui, 778), false, 1),
                (&s(ui, 804), "", &s(ui, 805), false, -1),
                (&s(ui, 792), "ScreenScraper", &s(ui, 793), true, -1),
                (&s(ui, 565), "nuubfan", &s(ui, 567), false, -1),
                (&s(ui, 568), "", &s(ui, 569), false, -1), (&s(ui, 798), "", &s(ui, 799), false, -1),
                (&s(ui, 576), "", &s(ui, 581), true, -1)]));
            ui.set_gaming_split(8);
        })),
        ("gaming", Box::new(|ui| { ui.set_settings_selected_index(3); ui.set_settings_view(68); ui.set_gaming_index(3); })),
        ("gaming_library", Box::new(|ui| { ui.set_gaming_index(12); })),
        ("gaming_signout", Box::new(|ui| {
            ui.set_gaming_index(10);
            /* Sign Out waits for its confirming press. */
            let rows = ui.get_gaming_rows();
            if let Some(mut r) = rows.row_data(10) { r.armed = true; r.detail = s(ui, 191).into(); rows.set_row_data(10, r); }
        })),
        ("game_hotkeys", Box::new(|ui| {
            ui.set_gaming_index(0);
            ui.set_settings_view(55); ui.set_gen_index(1); ui.set_gen_scroll(0); ui.set_gen_split(0);
            ui.set_gen_section(s(ui, 772).into()); ui.set_gen_select(true);
            ui.set_gen_rows(gen(&[(&s(ui, 773), "", "", false, -1), (&s(ui, 417), &s(ui, 346), "", true, -1),
                (&s(ui, 418), &s(ui, 345), "", true, -1), (&s(ui, 779), &s(ui, 341), "", true, -1),
                (&s(ui, 780), &s(ui, 342), "", true, -1), (&s(ui, 781), &s(ui, 339), "", true, -1),
                (&s(ui, 422), &s(ui, 351), "", true, -1), (&s(ui, 592), &s(ui, 347), "", true, -1)]));
        })),
        ("perf_overlay", Box::new(|ui| {
            ui.set_settings_view(56); ui.set_gen_index(0); ui.set_gen_scroll(0);
            ui.set_gen_section(s(ui, 774).into());
            ui.set_gen_rows(gen(&[(&s(ui, 782), "", "", false, 1), (&s(ui, 783), "", "", false, 1),
                ("CPU", "", "", false, 1), ("GPU", "", "", false, 0), (&s(ui, 784), "", "", false, 1),
                (&s(ui, 785), "", "", false, 0), (&s(ui, 786), "", "", false, 1), (&s(ui, 787), "", "", false, 1)]));
        })),
        ("restore", Box::new(|ui| { ui.set_settings_view(22); })),
        ("start_game", Box::new(|ui| {
            ui.set_details_active(true); ui.set_settings_view(57); ui.set_gen_index(0); ui.set_gen_scroll(0);
            ui.set_details_title("Super Mario World".into()); ui.set_details_subtitle("".into());
            ui.set_details_description("".into());
            ui.set_gen_section("Super Mario World".into()); ui.set_gen_select(true);
            let saved = |t: &str| s(ui, 766).replace("{0}", t);
            ui.set_gen_rows(gen(&[(&s(ui, 763), &s(ui, 765), &saved("today • 21:14"), false, -1),
                (&s(ui, 765), "", &saved("today • 21:14"), false, -1),
                (&s(ui, 423).replace("{0}", "0"), "", &saved("yesterday • 18:02"), false, -1),
                (&s(ui, 423).replace("{0}", "1"), "", &saved("today • 20:40"), false, -1),
                (&s(ui, 764), "", "", false, -1)]));
        })),
        ("game_details", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_details_active(true); ui.set_settings_view(50); ui.set_gen_index(0);
            ui.set_details_title("Super Mario World".into()); ui.set_details_subtitle("Super Nintendo".into());
            ui.set_details_meta("1990  •  Platform  •  Nintendo EAD".into());
            ui.set_details_description("Mario and Luigi travel to Dinosaur Land, where Bowser has kidnapped Princess Toadstool again. With the help of Yoshi, they explore seven worlds full of secret exits, hidden switches and castles. Ride Yoshi, fly with the cape feather and discover the Star Road to reach the hardest levels. The adventure spans 96 exits and a final battle in Bowser's castle.".into());
            ui.set_details_cover(art(200, 280, (40, 90, 200), (120, 40, 160), true)); ui.set_details_has_cover(true);
            ui.set_details_backdrop(art(96, 72, (30, 110, 60), (20, 40, 110), false)); ui.set_details_has_backdrop(true);
            ui.set_details_chips(chips(&[(11, "1-2", false), (1, &s(ui, 789).replace("{0}", "90"), false),
                (12, &format!("{}  •  12 h 05 min", s(ui, 403)), false)]));
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (1, s(ui, 412), false, true, false),
                (2, s(ui, 537), false, false, false), (3, s(ui, 524), false, false, false),
                (5, s(ui, 560), false, false, false), (9, s(ui, 549), false, false, false), (10, s(ui, 544), false, false, false)]));
            ui.set_details_caption("snes/Super Mario World (USA).sfc".into()); ui.set_details_caption_armed(false);
            ui.set_details_desc_step(0); ui.set_gen_notice("".into());
        })),
        ("game_details_scrolled", Box::new(|ui| {
            ui.set_gen_index(2); ui.set_details_desc_step(1);
            ui.set_details_caption("Platformers, Favorites of the year".into());
        })),
        ("game_delete", Box::new(|ui| {
            ui.set_gen_index(6); ui.set_details_desc_step(0);
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (1, s(ui, 412), false, true, false),
                (2, s(ui, 537), false, false, false), (3, s(ui, 524), false, false, false),
                (5, s(ui, 560), false, false, false), (9, s(ui, 549), false, false, false), (10, s(ui, 544), false, false, true)]));
            ui.set_details_caption(s(ui, 546).into()); ui.set_details_caption_armed(true);
        })),
        ("game_details_bare", Box::new(|ui| {
            ui.set_gen_index(0);
            ui.set_details_title("cube".into()); ui.set_details_subtitle("PlayStation Portable".into());
            ui.set_details_meta("".into()); ui.set_details_description("".into());
            ui.set_details_has_cover(false); ui.set_details_has_backdrop(false);
            ui.set_details_chips(chips(&[(13, &s(ui, 561), true), (12, &s(ui, 405), false)]));
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (14, s(ui, 412), false, false, false),
                (2, s(ui, 537), false, false, false), (6, s(ui, 562), false, false, false), (7, s(ui, 563), false, false, false),
                (10, s(ui, 544), false, false, false)]));
            ui.set_details_caption("psp/cube.cso".into()); ui.set_details_caption_armed(false);
            ui.set_gen_notice(s(ui, 572).into());
        })),
        ("game_settings", Box::new(|ui| {
            ui.set_gen_notice("".into()); ui.set_details_has_cover(false);
            ui.set_details_active(true); ui.set_settings_view(51); ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_details_title("Super Mario World".into()); ui.set_gen_section(s(ui, 524).into());
            ui.set_details_subtitle("Nintendo Entertainment System".into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), &s(ui, 528).replace("{0}", "snes9x"), "", true, -1),
                (&s(ui, 526), "4:3", "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]));
        })),
        ("bios", Box::new(|ui| {
            ui.set_details_active(false); ui.set_settings_selected_index(3); ui.set_settings_view(52); ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_gen_section(s(ui, 760).into()); ui.set_gen_notice("".into());
            ui.set_gen_split(3); ui.set_gen_split_label(s(ui, 761).into());
            let p = s(ui, 582).replace("{0}", "0").replace("{1}", "1");
            ui.set_gen_rows(gen(&[("PlayStation", &s(ui, 66), "", true, -1), ("Sega Saturn", &s(ui, 577), &p, true, -1),
                ("Game Boy", &s(ui, 579), "", true, -1), ("Mega-CD / Sega CD", &s(ui, 577), &p, true, -1),
                ("Neo Geo", &s(ui, 578), &p, true, -1), ("Atari Lynx", &s(ui, 579), "", true, -1)]));
        })),
        ("achievements", Box::new(|ui| {
            ui.set_details_active(false); ui.set_settings_selected_index(3); ui.set_settings_view(54); ui.set_gen_index(2); ui.set_gen_scroll(0); ui.set_gen_split(0);
            ui.set_gen_section(s(ui, 584).into()); ui.set_gen_notice("".into());
            ui.set_gen_rows(gen(&[(&s(ui, 588), "alice", "", false, -1), (&s(ui, 112), "", "", false, 1),
                (&s(ui, 585), "", &s(ui, 586), false, 0), (&s(ui, 567), "", "", false, -1)]));
        })),
        ("web", Box::new(|ui| {
            ui.set_details_active(true); ui.set_details_has_cover(false); ui.set_settings_view(67); ui.set_gen_index(0); ui.set_gen_scroll(0);
            ui.set_details_shell_title(s(ui, 678).into()); ui.set_details_title(s(ui, 678).into()); ui.set_details_subtitle("".into());
            ui.set_details_description(s(ui, 698).into()); ui.set_gen_section(s(ui, 678).into()); ui.set_gen_select(true); ui.set_gen_notice("".into());
            ui.set_gen_rows(gen(&[(&s(ui, 679), "", "", true, -1), (&s(ui, 681), "3", "", true, -1), (&s(ui, 682), "", "", true, -1),
                (&s(ui, 686), "", "", false, -1), (&s(ui, 687), "", &s(ui, 688), false, -1)]));
        })),
        ("web_bookmarks", Box::new(|ui| {
            ui.set_settings_view(67); ui.set_gen_index(1); ui.set_gen_section(s(ui, 681).into());
            ui.set_settings_north_hint(s(ui, 644).into());
            ui.set_gen_rows(gen(&[("Wikipedia, the free encyclopedia", "", "en.wikipedia.org", false, -1),
                ("Libretro Docs", "", "docs.libretro.com", false, -1), ("nuubOS on GitHub", "", "github.com/baf93/nuubOS", false, -1)]));
        })),
        ("diagnostics", Box::new(|ui| {
            ui.set_details_active(false); ui.set_settings_north_hint("".into()); ui.set_settings_selected_index(5); ui.set_settings_view(27); ui.set_diag_index(0); ui.set_recovery_notice("nuubos-support-20261007-101500.tar.gz".into()); })),
        ("recovery", Box::new(|ui| { ui.set_settings_selected_index(5); ui.set_settings_view(28); ui.set_recovery_index(3); ui.set_recovery_armed(3); ui.set_recovery_stage(1); })),
        ("recovery_safe", Box::new(|ui| { ui.set_settings_selected_index(5); ui.set_safe_mode(true); ui.set_settings_view(28); ui.set_recovery_index(4); })),
        ("safe_screen", Box::new(|ui| { ui.set_recovery_notice("".into()); ui.set_recovery_armed(-1); ui.set_recovery_stage(0); ui.set_active_user_id("u".into()); ui.set_safe_index(0); ui.set_safe_screen_open(true); })),
        ("safe_screen_armed", Box::new(|ui| { ui.set_safe_index(5); ui.set_recovery_armed(5); ui.set_recovery_stage(0); })),
        ("safe_screen_notice", Box::new(|ui| { ui.set_safe_index(2); ui.set_recovery_armed(-1); ui.set_recovery_stage(0); ui.set_recovery_notice("Support bundle saved: nuubos-support-20261008-070500.tar.gz".into()); })),
        ("dropdown", Box::new(|ui| {
            ui.set_safe_screen_open(false); ui.set_recovery_notice("".into());
            ui.set_settings_view(19); ui.set_settings_choice_open(true); ui.set_settings_choice_title("Sleep After".into());
            let o: Vec<SettingsChoiceEntry> = ["Off","1 min","2 min","5 min","10 min","15 min","30 min","60 min"].iter().map(|s| SettingsChoiceEntry{ value: (*s).into(), label: (*s).into() }).collect();
            ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o)))); ui.set_settings_choice_index(2);
        })),
    ];
    /* Controller Mapping as built by nuubui-home apply_mapping_rows():
     * 17 buttons, sticks with their deadzones, Restore Default Mapping. */
    let st = strings();
    let tr = |i: usize| st.row_data(i).unwrap_or_default();
    let mut rows: Vec<ControllerMappingEntry> = (337..354)
        .map(|i| ControllerMappingEntry { control: "".into(), kind: 0, label: tr(i), value: tr(i), customized: false })
        .collect();
    rows[10].value = tr(359).replace("{0}", "Z").into();
    for (label, value, kind, control) in [
        (tr(354), tr(359).replace("{0}", "X"), 1, ""),
        (tr(355), tr(359).replace("{0}", "Y"), 1, ""),
        (tr(123), "".into(), 2, "left_deadzone"),
        (tr(356), tr(361).to_string(), 1, ""),
        (tr(357), format!("{}  •  {}", tr(359).replace("{0}", "RY"), tr(360)), 1, ""),
        (tr(124), "".into(), 2, "right_deadzone"),
        (tr(365), "".into(), 3, ""),
    ] {
        rows.push(ControllerMappingEntry { control: control.into(), kind, label, value: value.into(), customized: false });
    }
    ui.set_controller_mapping(ModelRc::from(Rc::new(VecModel::from(rows))));
    scenes.push(("mapping", Box::new(|ui| {
        ui.set_settings_selected_index(2); ui.set_settings_view(15);
        ui.set_controller_mapping_index(1); ui.set_controller_mapping_scroll_offset(0);
    })));
    scenes.push(("mapping_sticks", Box::new(|ui| {
        ui.set_settings_view(15); ui.set_controller_mapping_index(21);
        ui.set_controller_mapping_scroll_offset(18);
    })));
    scenes.push(("mapping_capture", Box::new(|ui| {
        ui.set_settings_view(15); ui.set_controller_mapping_index(18);
        ui.set_controller_mapping_scroll_offset(14);
        ui.set_controller_remap_prompt(ui.get_i18n_strings().row_data(364).unwrap_or_default());
        ui.set_controller_remap_waiting(true);
    })));
    scenes.push(("tester", Box::new(|ui| {
        ui.set_controller_remap_waiting(false);
        ui.set_settings_view(14);
        ui.set_tester_face_east(true); ui.set_tester_dpad_left(true); ui.set_tester_l1(true);
        ui.set_tester_left_trigger(40); ui.set_tester_left_x(70); ui.set_tester_left_y(-50);
        ui.set_tester_r3(true); ui.set_tester_hotkey(true);
        ui.set_tester_action("Face East  •  Button BTN_EAST".into());
    })));
    scenes.push(("user_picker", Box::new(|ui| {
        ui.set_controller_remap_waiting(false);
        ui.set_tester_face_east(false); ui.set_tester_dpad_left(false); ui.set_tester_l1(false);
        ui.set_tester_left_trigger(0); ui.set_tester_left_x(0); ui.set_tester_left_y(0);
        ui.set_tester_r3(false); ui.set_tester_hotkey(false);
        ui.set_settings_open(false);
        let users: Vec<UserProfileEntry> = ["Fabio", "Elisa", "Ospite"].iter().enumerate().map(|(i, n)| UserProfileEntry {
            id: format!("u{i}").into(), name: (*n).into(), avatar_spec: "".into(), avatar_path: "".into(),
            avatar: Default::default(), active: i == 0, default_user: i == 0 }).collect();
        ui.set_users(ModelRc::from(Rc::new(VecModel::from(users))));
        ui.set_user_picker_index(1);
        ui.set_user_picker_open(true);
    })));
    /* Initial setup (OOB): Language/Ready full-screen, steps in the shell. */
    scenes.push(("oob_language", Box::new(|ui| {
        ui.set_user_picker_open(false);
        ui.set_settings_open(true); ui.set_oob_active(true); ui.set_oob_step(0); ui.set_settings_view(30);
    })));
    scenes.push(("oob_datetime", Box::new(|ui| {
        ui.set_oob_step(1); ui.set_settings_view(30); ui.set_oob_index(1); ui.set_automatic_time(false);
    })));
    /* Manual Date & Time picker (datetime.rs), focus on Month. */
    scenes.push(("datetime_picker", Box::new(|ui| {
        let s = ui.get_i18n_strings();
        let t = |i: usize| s.row_data(i).unwrap_or_default();
        let f = |l: usize, a: SharedString, v: SharedString, b: SharedString| DateTimeField { label: t(l), above: a, value: v, below: b };
        ui.set_datetime_fields(ModelRc::new(Rc::new(VecModel::from(vec![
            f(894, "11".into(), "10".into(), "09".into()),
            f(895, t(885), t(884), t(883)),
            f(896, "2027".into(), "2026".into(), "2025".into()),
            f(897, "15".into(), "14".into(), "13".into()),
            f(898, "33".into(), "32".into(), "31".into()),
        ]))));
        ui.set_datetime_preview(format!("{} 10 {} 2026", t(892), t(884)).into());
        ui.set_datetime_field_index(1);
        ui.set_datetime_picker_open(true);
    })));
    scenes.push(("oob_wifi", Box::new(|ui| {
        ui.set_datetime_picker_open(false);
        ui.set_oob_step(2); ui.set_settings_view(31); ui.set_oob_index(2); ui.set_connectivity_wifi_active(false);
    })));
    scenes.push(("oob_users", Box::new(|ui| {
        ui.set_oob_step(3); ui.set_settings_view(32); ui.set_user_count(3); ui.set_user_login_mode("default".into());
        ui.set_default_user_name("Fabio".into()); ui.set_oob_index(5); ui.set_oob_scroll(0);
    })));
    scenes.push(("oob_users_one", Box::new(|ui| {
        let users: Vec<UserProfileEntry> = vec![UserProfileEntry { id: "u0".into(), name: "Fabio".into(), avatar_spec: "".into(),
            avatar_path: "".into(), avatar: Default::default(), active: false, default_user: true }];
        ui.set_users(ModelRc::from(Rc::new(VecModel::from(users))));
        ui.set_user_count(1); ui.set_oob_index(1);
    })));
    /* Avatar picker over the Users step: the shipped built-in avatars. */
    scenes.push(("oob_avatar_picker", Box::new(|ui| {
        let choices: Vec<AvatarChoiceEntry> = (1..=24).map(|n| AvatarChoiceEntry {
            spec: format!("builtin:{n:02}").into(), label: format!("Avatar {n}").into(), path: "".into(),
            avatar: slint::Image::load_from_path(std::path::Path::new(
                &format!("/workspace/package/nuubos/nuubos-users/src/avatars/{n:02}.png"))).unwrap_or_default(),
        }).collect();
        ui.set_avatar_choices(ModelRc::from(Rc::new(VecModel::from(choices))));
        ui.set_profile_edit_user_id("u0".into());
        ui.set_current_user_avatar(ui.get_avatar_choices().row_data(9).map(|a| a.avatar).unwrap_or_default());
        ui.set_current_user_avatar_available(true);
        ui.set_avatar_picker_index(5); ui.set_avatar_picker_scroll(0); ui.set_avatar_picker_open(true);
    })));
    scenes.push(("oob_ready", Box::new(|ui| { ui.set_avatar_picker_open(false); ui.set_oob_step(4); })));
    /* Moonlight (EPIC-025): the Settings shell with the PCs rail. */
    fn ml_host(id: &str, name: &str, address: &str, paired: bool, online: &str, probing: bool) -> MoonlightHostEntry {
        MoonlightHostEntry { id: id.into(), name: name.into(), address: address.into(), paired, online: online.into(), probing }
    }
    fn ml_hosts() -> ModelRc<MoonlightHostEntry> {
        ModelRc::from(Rc::new(VecModel::from(vec![
            ml_host("a", "GAMING-PC", "192.168.1.20", true, "online", false),
            ml_host("b", "Living Room Desktop With A Long Name", "192.168.1.31", false, "unknown", true),
        ])))
    }
    scenes.push(("moonlight_hosts", Box::new(|ui| {
        ui.set_oob_active(false);
        ui.set_settings_open(true); ui.set_moonlight_active(true); ui.set_settings_view(40);
        ui.set_moonlight_hosts(ml_hosts()); ui.set_moonlight_discovering(true);
        ui.set_moonlight_index(0); ui.set_moonlight_scroll(0);
    })));
    scenes.push(("moonlight_empty", Box::new(|ui| {
        ui.set_moonlight_hosts(ModelRc::from(Rc::new(VecModel::<MoonlightHostEntry>::default())));
        ui.set_moonlight_discovering(false); ui.set_moonlight_index(1);
    })));
    scenes.push(("moonlight_pairing", Box::new(|ui| {
        ui.set_moonlight_hosts(ml_hosts());
        ui.set_moonlight_host(ml_host("b", "Living Room Desktop", "192.168.1.31", false, "online", false));
        ui.set_settings_view(41); ui.set_moonlight_index(0); ui.set_moonlight_pairing_pin("4821".into());
    })));
    scenes.push(("moonlight_apps", Box::new(|ui| {
        ui.set_moonlight_pairing_pin("".into());
        ui.set_moonlight_host(ml_host("a", "GAMING-PC", "192.168.1.20", true, "online", false));
        let apps: Vec<SharedString> = ["Desktop", "Steam Big Picture", "Cyberpunk 2077", "Elden Ring", "Hades II", "Forza Horizon 5", "Baldur's Gate 3"]
            .iter().map(|a| SharedString::from(*a)).collect();
        ui.set_moonlight_apps(ModelRc::from(Rc::new(VecModel::from(apps))));
        ui.set_moonlight_apps_state("ready".into()); ui.set_moonlight_index(1);
    })));
    scenes.push(("moonlight_apps_loading", Box::new(|ui| {
        ui.set_moonlight_apps(ModelRc::from(Rc::new(VecModel::<SharedString>::default())));
        ui.set_moonlight_apps_state("loading".into()); ui.set_moonlight_index(0);
    })));
    scenes.push(("moonlight_settings", Box::new(|ui| {
        ui.set_settings_view(42); ui.set_moonlight_index(2);
        let s = ui.get_i18n_strings();
        ui.set_moonlight_resolution_label(s.row_data(122).unwrap_or_default());
        ui.set_moonlight_fps_label(s.row_data(460).unwrap_or_default().replace("{0}", "60").into());
        ui.set_moonlight_codec_label("H.264".into());
        ui.set_moonlight_bitrate_label(s.row_data(461).unwrap_or_default().replace("{0}", "20").into());
        ui.set_moonlight_bitrate_fraction(20000.0 / 150000.0);
        ui.set_moonlight_audio_label(s.row_data(860).unwrap_or_default());
    })));
    scenes.push(("moonlight_settings_pc", Box::new(|ui| { ui.set_moonlight_index(7); ui.set_moonlight_scroll(4); })));
    /* Steam Link page (settings-view 45, EPIC-026). */
    scenes.push(("steamlink_absent", Box::new(|ui| {
        ui.set_moonlight_active(false);
        ui.set_steamlink_active(true);
        ui.set_settings_view(45);
        ui.set_steamlink_state("absent".into());
        ui.set_steamlink_download_detail("".into());
        ui.set_steamlink_index(0);
    })));
    scenes.push(("steamlink_downloading", Box::new(|ui| {
        ui.set_steamlink_state("installing".into());
        ui.set_steamlink_progress_label("42%".into());
    })));
    scenes.push(("steamlink_installed", Box::new(|ui| {
        let s = strings();
        ui.set_steamlink_state("installed".into());
        ui.set_steamlink_version_label(format!("{} 1.3.32.316", s.row_data(485).unwrap_or_default()).into());
        ui.set_steamlink_update_title(s.row_data(487).unwrap_or_default());
        ui.set_steamlink_update_value(s.row_data(489).unwrap_or_default());
        ui.set_steamlink_index(1);
    })));
    scenes.push(("steamlink_remove", Box::new(|ui| {
        ui.set_steamlink_index(2);
        ui.set_steamlink_remove_confirm(true);
    })));
    /* Top bar with connected controllers (battery dots) and BT headphones. */
    scenes.push(("topbar_controllers_2", Box::new(|ui| {
        ui.set_moonlight_active(false);
        ui.set_steamlink_active(false);
        ui.set_oob_active(false);
        ui.set_settings_open(false);
        ui.set_audio_bluetooth_available(true);
        ui.set_topbar_controllers(ModelRc::from(Rc::new(VecModel::from(vec![
            TopbarController { player: 1, battery: 38 },
            TopbarController { player: 2, battery: -1 },
        ]))));
    })));
    scenes.push(("topbar_controllers_8", Box::new(|ui| {
        let batteries = [85, 38, 12, -1, 64, 20, 5, 100];
        ui.set_topbar_controllers(ModelRc::from(Rc::new(VecModel::from(
            (0..8).map(|i| TopbarController { player: i + 1, battery: batteries[i as usize] })
                .collect::<Vec<_>>(),
        ))));
    })));
    /* Home carousels as built by nuubui-home library.rs (x-units are the
     * running sum of aspects). Covers are synthetic gradients. */
    fn cover(w: u32, h: u32, rgb: (u8, u8, u8)) -> slint::Image {
        let mut buf = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(w, h);
        for (i, p) in buf.make_mut_slice().iter_mut().enumerate() {
            let y = (i as u32 / w) as f32 / h as f32;
            let k = 1.0 - 0.6 * y;
            *p = slint::Rgba8Pixel { r: (rgb.0 as f32 * k) as u8, g: (rgb.1 as f32 * k) as u8, b: (rgb.2 as f32 * k) as u8, a: 255 };
        }
        slint::Image::from_rgba8(buf)
    }
    fn card(kind: i32, title: &str, detail: &str, system: &str, aspect: f32, accent: u32, img: Option<slint::Image>, available: bool, _unused: bool) -> HomeCard {
        HomeCard {
            key: title.into(), kind, title: title.into(), detail: detail.into(), system_name: system.into(),
            last_played: detail.split("  •  ").nth(1).unwrap_or("").into(),
            play_time: detail.split("  •  ").nth(2).map(|t| t.trim_start_matches("Played ")).unwrap_or("").into(),
            cover_path: "".into(), has_cover: img.is_some(), cover: img.unwrap_or_default(), aspect, x_units: 0.0,
            accent: slint::Color::from_rgb_u8((accent >> 16) as u8, (accent >> 8) as u8, accent as u8),
            available, focused: false, ..Default::default()
        }
    }
    fn units(mut v: Vec<HomeCard>) -> ModelRc<HomeCard> {
        let mut u = 0.0;
        for c in v.iter_mut() { c.x_units = u; u += c.aspect; }
        ModelRc::from(Rc::new(VecModel::from(v)))
    }
    let recent = units(vec![
        card(0, "Super Mario World", "Super Nintendo  •  Last played today  •  Played 2 h 05 min", "Super Nintendo", 1.4, 0x6b5bc4, Some(cover(280, 200, (200, 60, 50))), true, true),
        card(0, "Crash Bandicoot", "PlayStation  •  Last played yesterday", "PlayStation", 1.0, 0x3d6fb8, Some(cover(240, 240, (230, 120, 30))), true, false),
        card(0, "Monster Hunter Freedom Unite", "PlayStation Portable  •  Last played 3 days ago", "PlayStation Portable", 0.57, 0x2c2f36, Some(cover(114, 200, (40, 140, 90))), true, false),
        card(0, "The Legend of Zelda: The Minish Cap", "Game Boy Advance  •  Last played 5 days ago", "Game Boy Advance", 1.0, 0x4b3fb0, None, true, false),
        card(0, "Sonic the Hedgehog 2", "Unavailable  •  Mega Drive / Genesis", "Mega Drive / Genesis", 0.7, 0x1f4fa8, None, false, false),
        card(0, "Final Fantasy VII", "PlayStation  •  Last played 12 days ago", "PlayStation", 1.0, 0x3d6fb8, Some(cover(240, 240, (60, 80, 160))), true, true),
    ]);
    /* System icons as shipped by nuubos-library. */
    let icon = |id: &str| slint::Image::load_from_path(std::path::Path::new(
        &format!("/workspace/package/nuubos/nuubos-library/src/systems/{id}.png"))).ok();
    fn previews(mut c: HomeCard, imgs: Vec<slint::Image>) -> HomeCard {
        c.previews = imgs.len() as i32;
        let mut it = imgs.into_iter();
        c.p0 = it.next().unwrap_or_default(); c.p1 = it.next().unwrap_or_default();
        c.p2 = it.next().unwrap_or_default(); c.p3 = it.next().unwrap_or_default();
        c
    }
    let fan = |a: (u8, u8, u8), b: (u8, u8, u8), c: (u8, u8, u8)| vec![cover(150, 200, a), cover(150, 200, b), cover(200, 150, c)];
    let collections = units(vec![
        previews(card(2, "RPG Classics", "12 games", "", 1.6, 0x2d3f66, None, true, false), fan((120, 40, 160), (40, 90, 170), (180, 120, 40))),
        card(2, "Couch Co-op", "3 games", "", 1.6, 0x2d3f66, None, true, false),
        card(1, "New Collection", "", "", 1.6, 0x2d3f66, None, true, false),
    ]);
    let shelf = units(vec![
        previews(card(3, "Super Nintendo", "48 games", "", 1.6, 0x6b5bc4, icon("snes"), true, false), fan((200, 60, 50), (60, 150, 90), (230, 200, 60))),
        previews(card(3, "PlayStation", "23 games", "", 1.6, 0x3d6fb8, icon("psx"), true, false), vec![cover(200, 200, (230, 120, 30))]),
        card(3, "Game Boy Advance", "61 games", "", 1.6, 0x4b3fb0, icon("gba"), true, false),
        card(3, "Mega Drive / Genesis", "1 game", "", 1.6, 0x1f4fa8, icon("megadrive"), true, false),
        card(3, "PICO-8", "3 games", "", 1.6, 0xff004d, None, true, false),
    ]);
    let snes_tile = shelf.row_data(0).unwrap_or_default();
    let apps = units(vec![
        card(4, "Moonlight", "", "", 1.0, 0x1a1d22, Some(cover(96, 96, (90, 160, 255))), true, false),
        card(4, "Steam Link", "", "", 1.0, 0x1a1d22, None, true, false),
        card(4, "Media Player", "", "", 1.0, 0x1a1d22, None, true, false),
    ]);
    let grid = units((0..14).map(|i| {
        let t = ["Chrono Trigger", "Donkey Kong Country", "EarthBound", "F-Zero", "Final Fantasy VI", "Kirby Super Star", "Mega Man X",
                 "Secret of Mana", "Star Fox", "Super Castlevania IV", "Super Mario Kart", "Super Metroid", "Yoshi's Island", "Zelda: A Link to the Past"][i];
        let img = if i % 3 == 0 { Some(cover(280, 200, (50 + (i as u8) * 12, 90, 160))) } else { None };
        /* The longest chip row (NES name, both play chips) on the focused card. */
        if i == 2 {
            return card(0, "The Legend of Zelda II: The Adventure of Link", "Nintendo Entertainment System  •  Last played 3 days ago  •  Played 12 h 40 min",
                        "Nintendo Entertainment System", 1.4, 0xb03a2e, img, true, true);
        }
        card(0, t, "Super Nintendo  •  Never played", "Super Nintendo", 1.4, 0x6b5bc4, img, true, i == 2)
    }).collect());
    /* Games copied but none played yet: the empty Recently Played card. */
    let (shelf_only, apps_only, collections_only) = (shelf.clone(), apps.clone(), collections.clone());
    let find_cards = recent.clone();
    scenes.push(("home_unplayed", Box::new(move |ui| {
        ui.set_topbar_controllers(ModelRc::default());
        ui.set_audio_bluetooth_available(false);
        ui.set_home_recent(ModelRc::default()); ui.set_home_systems(shelf_only.clone()); ui.set_home_collections(collections_only.clone()); ui.set_home_apps(apps_only.clone());
        ui.set_home_row(0); ui.set_home_empty_variant(2); ui.set_select_face_position(1);
    })));
    scenes.push(("home_recent", Box::new(move |ui| {
        ui.set_home_empty_variant(0);
        ui.set_home_recent(recent.clone()); ui.set_home_systems(shelf.clone()); ui.set_home_collections(collections.clone()); ui.set_home_apps(apps.clone());
        ui.set_home_row(0); ui.set_home_recent_index(0); ui.set_context_face_position(0); ui.set_select_face_position(1);
    })));
    scenes.push(("home_recent_end", Box::new(|ui| { ui.set_home_recent_index(5); })));
    scenes.push(("home_shelf", Box::new(|ui| { ui.set_home_row(1); ui.set_home_systems_index(0); })));
    scenes.push(("home_collections", Box::new(|ui| { ui.set_home_row(2); ui.set_home_collections_index(0); })));
    scenes.push(("home_collections_new", Box::new(|ui| { ui.set_home_collections_index(2); })));
    scenes.push(("home_sheet", Box::new(|ui| {
        ui.set_home_row(0);
        ui.set_home_sheet_title("Add to Collection • Super Mario World".into());
        ui.set_home_sheet_items(ModelRc::from(Rc::new(VecModel::from(vec![
            HomeSheetItem { label: "RPG Classics".into(), detail: "".into(), check: 1, armed: false },
            HomeSheetItem { label: "Couch Co-op".into(), detail: "".into(), check: 0, armed: false },
            HomeSheetItem { label: "New Collection".into(), detail: "".into(), check: -1, armed: false },
        ]))));
        ui.set_home_sheet_index(1); ui.set_home_sheet_open(true);
    })));
    scenes.push(("home_find", Box::new(move |ui| {
        ui.set_home_sheet_open(false);
        ui.set_find_open(true); ui.set_find_zone(1); ui.set_find_query("mario".into());
        ui.set_find_chips(ModelRc::from(Rc::new(VecModel::from(vec![
            FindChip { label: "Super Nintendo".into(), active: true },
            FindChip { label: "Decade".into(), active: false },
            FindChip { label: "Genre".into(), active: false },
            FindChip { label: "Players".into(), active: false },
            FindChip { label: "Never Played Only".into(), active: false },
            FindChip { label: "Clear Filters".into(), active: false },
        ]))));
        ui.set_find_chip_index(1);
        ui.set_find_results(find_cards.clone()); ui.set_find_status("6 games".into());
    })));
    scenes.push(("home_find_results", Box::new(|ui| { ui.set_find_zone(2); ui.set_find_result_index(1); })));
    scenes.push(("home_find_surprise", Box::new(|ui| { ui.set_find_zone(3); ui.set_find_surprise_index(0); })));
    scenes.push(("home_apps", Box::new(|ui| { ui.set_find_open(false); ui.set_home_sheet_open(false); ui.set_home_row(3); ui.set_home_apps_index(0); ui.set_library_scanning(true); })));
    scenes.push(("home_apps_notice", Box::new(|ui| {
        ui.set_library_scanning(false);
        ui.set_home_notice(ui.get_i18n_strings().row_data(411).unwrap_or_default());
    })));
    scenes.push(("home_grid", Box::new(move |ui| {
        ui.set_library_scanning(false); ui.set_home_notice("".into());
        ui.set_library_card(snes_tile.clone());
        ui.set_library_games(grid.clone()); ui.set_library_aspect(1.4); ui.set_library_title("Super Nintendo".into());
        ui.set_library_detail("14 games".into()); ui.set_library_index(2); ui.set_library_open(true);
    })));
    scenes.push(("home_grid_scrolled", Box::new(|ui| { ui.set_library_index(13); })));
    scenes.push(("home_notice", Box::new(|ui| {
        ui.set_library_open(false); ui.set_home_row(0); ui.set_home_recent_index(1);
        ui.set_home_notice(ui.get_i18n_strings().row_data(411).unwrap_or_default());
    })));
    /* Last: a theme from the built-in set (EPIC-008) on the Home screen. */
    scenes.push(("theme_ember", Box::new(|ui| {
        ui.set_home_notice("".into());
        ui.set_library_open(false);
        ui.set_home_row(0);
        let t = ui.global::<Theme>();
        let c = |v: u32| slint::Color::from_rgb_u8((v >> 16) as u8, (v >> 8) as u8, v as u8);
        t.set_accent(c(0xff7a2f)); t.set_accent_light(c(0xffa36b)); t.set_accent_muted(c(0xd46a2e));
        t.set_accent_text(c(0xffe2cf)); t.set_focus_fill(c(0x3a2214)); t.set_text(c(0xf7f4f2));
        t.set_text_secondary(c(0xb3aaa4)); t.set_text_dim(c(0x988f89)); t.set_text_faint(c(0x857c76));
        t.set_background(c(0x100d0c)); t.set_surface(c(0x2b2522)); t.set_border(c(0x3b332f));
    })));
    /* The system keyboard follows the theme too. */
    scenes.push(("keyboard_ember", Box::new(|ui| {
        ui.set_battery_state("charging".into());
        ui.set_settings_open(true); ui.set_settings_selected_index(1); ui.set_settings_view(6);
    })));
    /* Application pages (apppage.rs fills them on the device). */
    fn entries(items: &[(&str, &str, &str, bool)]) -> ModelRc<AppEntry> {
        let v: Vec<AppEntry> = items.iter().map(|(t, d, v, g)| AppEntry {
            title: (*t).into(), detail: (*d).into(), value: (*v).into(), icon: if d.is_empty() { 0 } else { 18 },
            good: *g, caption: "".into(),
        }).collect();
        ModelRc::from(Rc::new(VecModel::from(v)))
    }
    fn captions(items: &[&str]) -> ModelRc<SharedString> {
        ModelRc::from(Rc::new(VecModel::from(items.iter().map(|c| SharedString::from(*c)).collect::<Vec<_>>())))
    }
    let app_icon = cover(96, 96, (90, 160, 255));
    let app_page = move |ui: &HomeWindow, kicker: &str, title: &str, meta: &str| {
        ui.set_details_active(false); ui.set_library_open(false);
        ui.set_settings_open(true);
        ui.set_app_page_icon(app_icon.clone()); ui.set_app_page_has_icon(true);
        ui.set_app_page_kicker(kicker.into()); ui.set_app_page_title(title.into()); ui.set_app_page_meta(meta.into());
        ui.set_app_page_pin("".into()); ui.set_app_page_progress(-1.0); ui.set_app_page_description("".into());
        ui.set_app_page_chips(chips(&[]));
    };
    let ap = app_page.clone();
    scenes.push(("moonlight_pcs", Box::new(move |ui| {
        ap(ui, "STREAM", "Moonlight", "");
        ui.set_moonlight_active(true); ui.set_settings_view(40); ui.set_moonlight_index(0);
        ui.set_app_page_entries(entries(&[("GAMING-PC", "192.168.5.20 • Paired", "Online", true),
            ("Office laptop", "192.168.5.31 • Not paired", "Offline", false)]));
        ui.set_app_page_actions(actions(&[(17, "Search for PCs".into(), false, false, false),
            (19, "Add PC by Address".into(), false, false, false), (3, "Stream Settings".into(), false, false, false)]));
        ui.set_app_page_captions(captions(&["", "PC address", "Automatic • 60 fps • H.264 • Automatic"]));
    })));
    scenes.push(("moonlight_actions", Box::new(|ui| { ui.set_moonlight_index(4); })));
    let ap = app_page.clone();
    scenes.push(("moonlight_pin", Box::new(move |ui| {
        ap(ui, "STREAM", "Office laptop", "192.168.5.31");
        ui.set_settings_view(41); ui.set_moonlight_index(0);
        ui.set_app_page_chips(chips(&[(13, "Not paired", true), (18, "Online", false)]));
        ui.set_app_page_entries(entries(&[]));
        ui.set_app_page_pin("4831".into());
        ui.set_app_page_actions(actions(&[(7, "Cancel Pairing".into(), true, false, false), (10, "Remove PC".into(), false, false, false)]));
        ui.set_app_page_captions(captions(&["Waiting for the PC…", ""]));
    })));
    let ap = app_page.clone();
    scenes.push(("steamlink_absent", Box::new(move |ui| {
        ap(ui, "STEAM LINK", "Steam Link", "");
        ui.set_moonlight_active(false); ui.set_steamlink_active(true); ui.set_settings_view(45); ui.set_steamlink_index(0);
        ui.set_app_page_entries(entries(&[]));
        ui.set_app_page_description("Valve's application, about 35 MB. Downloading it means you accept the Steam Subscriber Agreement.".into());
        ui.set_app_page_actions(actions(&[(4, "Download Steam Link".into(), true, false, false)]));
        ui.set_app_page_captions(captions(&[""]));
    })));
    scenes.push(("steamlink_installing", Box::new(|ui| {
        ui.set_app_page_progress(0.42); ui.set_app_page_progress_label("Downloading Steam Link • 42%".into());
        ui.set_app_page_actions(actions(&[(7, "Press to cancel".into(), true, false, false)]));
    })));
    let ap = app_page.clone();
    scenes.push(("steamlink_installed", Box::new(move |ui| {
        ap(ui, "STEAM LINK", "Steam Link", "Version 1.3.9.300");
        ui.set_app_page_chips(chips(&[(6, "Up to date", false)]));
        ui.set_app_page_description("Choose and pair your PC in Steam Link".into());
        ui.set_app_page_actions(actions(&[(0, "Open Steam Link".into(), true, false, false),
            (5, "Check for Updates".into(), false, false, false), (10, "Remove Steam Link".into(), false, false, false)]));
        ui.set_app_page_captions(captions(&["Choose and pair your PC in Steam Link", "Version 1.3.9.300", ""]));
        ui.set_steamlink_index(1);
    })));
    scenes.push(("app_page_done", Box::new(|ui| {
        ui.set_steamlink_active(false); ui.set_moonlight_active(false); ui.set_settings_open(false); ui.set_settings_view(0);
    })));
    scenes.push(("launch", Box::new(|ui| {
        ui.set_library_open(false);
        ui.set_launch_title("Super Mario World".into());
        ui.set_launch_system("Super Nintendo".into());
        ui.set_launch_accent(slint::Color::from_rgb_u8(0x6b, 0x5b, 0xc4));
        ui.set_launch_cover(cover(280, 200, (200, 60, 50)));
        ui.set_launch_has_cover(true);
        ui.set_launch_busy(true);
        ui.set_launch_open(true);
    })));
    scenes.push(("launch_closed", Box::new(|ui| { ui.set_launch_open(false); ui.set_launch_busy(false); })));
    let only = std::env::var("ONLY").unwrap_or_default();
    for (name, f) in &scenes {
        if !only.is_empty() && !only.split(',').any(|o| name.starts_with(o)) {
            continue;
        }
        f(&ui);
        sync_focus(&ui);
        for (w, h) in sizes {
            shot(&win, w, h, name);
        }
        ui.set_settings_choice_open(false);
    }
}

/* Same as nuubui-home library.rs sync_focus: card focus lives in the models. */
fn sync_focus(ui: &HomeWindow) {
    let row = ui.get_home_row();
    for (model, active, index) in [
        (ui.get_home_recent(), row == 0, ui.get_home_recent_index()),
        (ui.get_home_systems(), row == 1, ui.get_home_systems_index()),
        (ui.get_home_collections(), row == 2, ui.get_home_collections_index()),
        (ui.get_home_apps(), row == 3, ui.get_home_apps_index()),
        (ui.get_library_games(), true, ui.get_library_index()),
    ] {
        for i in 0..model.row_count() {
            let want = active && i as i32 == index;
            if let Some(mut card) = model.row_data(i) {
                if card.focused != want {
                    card.focused = want;
                    model.set_row_data(i, card);
                }
            }
        }
    }
}
