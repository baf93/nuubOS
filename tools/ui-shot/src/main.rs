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

    /* Over a running game: GAME section first (Resume selected), the
     * State Slot row (adjustable) and Quit waiting for its second press. */
    qm.set_audio_output_dropdown_open(false);
    qm.set_home_music_playing(false);
    qm.set_switch_user_visible(false);
    qm.set_game_section_visible(true);
    qm.set_game_slot_label("Slot 3".into());
    for (name, sel, confirm) in [("qm_game", 8, -1), ("qm_game_slot", 11, -1), ("qm_game_quit", 14, 14)] {
        qm.set_selected_index(sel);
        qm.set_game_confirm_index(confirm);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_game_section_visible(false);
    qm.set_game_confirm_index(-1);
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
    ui.set_system_screensaver_after_min(2);
    ui.set_system_sleep_after_min(5);
    ui.set_system_poweroff_after_min(30);
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
        ("wifi_list", Box::new(|ui| { ui.set_settings_view(2); ui.set_wifi_network_index(2); ui.set_wifi_network_scroll_offset(0); })),
        ("display_audio", Box::new(|ui| { ui.set_settings_selected_index(3); ui.set_settings_view(18); ui.set_display_audio_index(5); ui.set_display_audio_scroll_offset(1); })),
        ("system", Box::new(|ui| { ui.set_settings_selected_index(4); ui.set_settings_view(19); ui.set_system_index(4); ui.set_system_scroll_offset(0); })),
        ("controllers", Box::new(|ui| { ui.set_settings_selected_index(2); ui.set_settings_view(0); })),
        ("restore", Box::new(|ui| { ui.set_settings_view(22); })),
        ("dropdown", Box::new(|ui| {
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
    scenes.push(("user_picker", Box::new(|ui| {
        ui.set_controller_remap_waiting(false);
        ui.set_settings_open(false);
        let users: Vec<UserProfileEntry> = ["Fabio", "Elisa", "Ospite"].iter().enumerate().map(|(i, n)| UserProfileEntry {
            id: format!("u{i}").into(), name: (*n).into(), avatar_spec: "".into(), avatar_path: "".into(),
            avatar: Default::default(), active: i == 0, default_user: i == 0 }).collect();
        ui.set_users(ModelRc::from(Rc::new(VecModel::from(users))));
        ui.set_user_picker_index(1);
        ui.set_user_picker_open(true);
    })));
    /* Initial setup (OOB): Welcome/Ready full-screen, steps in the shell. */
    scenes.push(("oob_welcome", Box::new(|ui| {
        ui.set_user_picker_open(false);
        ui.set_settings_open(true); ui.set_oob_active(true); ui.set_oob_step(0); ui.set_settings_view(30);
    })));
    scenes.push(("oob_datetime", Box::new(|ui| {
        ui.set_oob_step(1); ui.set_settings_view(30); ui.set_oob_index(1); ui.set_automatic_time(false);
    })));
    scenes.push(("oob_wifi", Box::new(|ui| {
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
    scenes.push(("oob_ready", Box::new(|ui| { ui.set_oob_step(4); })));
    /* Top bar with connected controllers (battery dots) and BT headphones. */
    scenes.push(("topbar_controllers_2", Box::new(|ui| {
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
    fn card(kind: i32, title: &str, detail: &str, system: &str, aspect: f32, accent: u32, img: Option<slint::Image>, available: bool, favorite: bool) -> HomeCard {
        HomeCard {
            key: title.into(), kind, title: title.into(), detail: detail.into(), system_name: system.into(),
            cover_path: "".into(), has_cover: img.is_some(), cover: img.unwrap_or_default(), aspect, x_units: 0.0,
            accent: slint::Color::from_rgb_u8((accent >> 16) as u8, (accent >> 8) as u8, accent as u8),
            available, favorite,
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
    let shelf = units(vec![
        card(1, "Favorites", "4 games", "", 1.6, 0x7a5a14, None, true, false),
        card(2, "RPG Classics", "12 games", "", 1.6, 0x2d3f66, None, true, false),
        card(3, "Super Nintendo", "48 games", "", 1.6, 0x6b5bc4, icon("snes"), true, false),
        card(3, "PlayStation", "23 games", "", 1.6, 0x3d6fb8, icon("psx"), true, false),
        card(3, "Game Boy Advance", "61 games", "", 1.6, 0x4b3fb0, icon("gba"), true, false),
        card(3, "Mega Drive / Genesis", "1 game", "", 1.6, 0x1f4fa8, icon("megadrive"), true, false),
        card(3, "PICO-8", "3 games", "", 1.6, 0xff004d, None, true, false),
    ]);
    let apps = units(vec![
        card(4, "Moonlight", "", "", 1.0, 0x1a1d22, Some(cover(96, 96, (90, 160, 255))), true, false),
        card(4, "Steam Link", "", "", 1.0, 0x1a1d22, None, true, false),
        card(4, "Media Player", "", "", 1.0, 0x1a1d22, None, true, false),
    ]);
    let grid = units((0..14).map(|i| {
        let t = ["Chrono Trigger", "Donkey Kong Country", "EarthBound", "F-Zero", "Final Fantasy VI", "Kirby Super Star", "Mega Man X",
                 "Secret of Mana", "Star Fox", "Super Castlevania IV", "Super Mario Kart", "Super Metroid", "Yoshi's Island", "Zelda: A Link to the Past"][i];
        let img = if i % 3 == 0 { Some(cover(280, 200, (50 + (i as u8) * 12, 90, 160))) } else { None };
        card(0, t, "Super Nintendo  •  Never played", "Super Nintendo", 1.4, 0x6b5bc4, img, true, i == 2)
    }).collect());
    scenes.push(("home_recent", Box::new(move |ui| {
        ui.set_topbar_controllers(ModelRc::default());
        ui.set_audio_bluetooth_available(false);
        ui.set_home_recent(recent.clone()); ui.set_home_shelf(shelf.clone()); ui.set_home_apps(apps.clone());
        ui.set_home_row(0); ui.set_home_recent_index(0); ui.set_context_face_position(0); ui.set_select_face_position(1);
    })));
    scenes.push(("home_recent_end", Box::new(|ui| { ui.set_home_recent_index(5); })));
    scenes.push(("home_shelf", Box::new(|ui| { ui.set_home_row(1); ui.set_home_shelf_index(2); })));
    scenes.push(("home_apps", Box::new(|ui| { ui.set_home_row(2); ui.set_home_apps_index(0); ui.set_library_scanning(true); })));
    scenes.push(("home_grid", Box::new(move |ui| {
        ui.set_library_scanning(false);
        ui.set_library_games(grid.clone()); ui.set_library_aspect(1.4); ui.set_library_title("Super Nintendo".into());
        ui.set_library_detail("14 games".into()); ui.set_library_index(2); ui.set_library_open(true);
    })));
    scenes.push(("home_grid_scrolled", Box::new(|ui| { ui.set_library_index(12); })));
    scenes.push(("home_notice", Box::new(|ui| {
        ui.set_library_open(false); ui.set_home_row(0); ui.set_home_recent_index(1);
        ui.set_home_notice(ui.get_i18n_strings().row_data(411).unwrap_or_default());
    })));
    for (name, f) in &scenes {
        f(&ui);
        for (w, h) in sizes {
            shot(&win, w, h, name);
        }
        ui.set_settings_choice_open(false);
    }
}
