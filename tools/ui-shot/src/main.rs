slint::include_modules!();

#[path = "/workspace/package/nuubos/nuubui-home/src/src/qr.rs"]
mod qr;

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

fn scopes(labels: &[String]) -> ModelRc<SharedString> {
    let v: Vec<SharedString> = labels.iter().map(|l| l.into()).collect();
    ModelRc::from(Rc::new(VecModel::from(v)))
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
            progress_shown: false,
            progress: 0.0,
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

/* Output scale of each target screen (nuubos-wl-output-apply): the CubeXX
 * 720x720 and RG34XX 720x480 panels are denser than the RG40XX-V; 1.125
 * gives the UI the 640 px width it is laid out for (640x640 / 640x427
 * logical) with larger text. */
fn output_scale(w: u32, h: u32) -> f32 {
    if w == 720 && (h == 720 || h == 480) { 1.125 } else { 1.0 }
}

fn shot(win: &Rc<MinimalSoftwareWindow>, w: u32, h: u32, name: &str) {
    win.dispatch_event(slint::platform::WindowEvent::ScaleFactorChanged { scale_factor: output_scale(w, h) });
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
    let sizes = [(640u32, 480u32), (720, 720), (720, 480), (1280, 720), (1920, 1080)];
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
    /* The RetroArch row, shown with the user's Settings → Gaming switch. */
    qm.set_game_retroarch(true);
    qm.set_game_confirm_index(-1);
    for (name, sel) in [("qm_game_retroarch", 13), ("qm_game_retroarch_quit", 14)] {
        qm.set_selected_index(sel);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_game_retroarch(false);
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
    /* NDS game: Stylus (42, on) and Screen Layout (43) before Restart Game,
     * then the layout dropdown. */
    qm.set_game_nds(true);
    qm.set_game_confirm_index(-1);
    qm.set_game_stylus_on(true);
    qm.set_game_nds_layout(1);
    for (name, sel) in [("qm_game_nds_stylus", 42), ("qm_game_nds_layout", 43), ("qm_game_nds_restart", 12)] {
        qm.set_selected_index(sel);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_selected_index(43);
    qm.set_game_layout_dropdown_index(2);
    qm.set_game_layout_dropdown_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_nds_layout_dropdown");
    }
    qm.set_game_layout_dropdown_open(false);
    /* Picture in Picture: Small Screen Position (52) under Screen Layout
     * (GitHub #5). */
    qm.set_game_nds_layout(2);
    qm.set_game_nds_corner(3);
    qm.set_selected_index(52);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_nds_corner");
    }
    qm.set_game_nds_corner(0);
    qm.set_game_nds_layout(1);
    qm.set_game_nds(false);
    /* Multi-disc game: Disc (51) where Stylus would be, then its dropdown. */
    qm.set_game_discs(3);
    qm.set_game_disc_label("2 of 3".into());
    qm.set_selected_index(51);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_disc");
    }
    let disc_options: Vec<slint::SharedString> = (1..=3).map(|n| format!("Disc {n}").into()).collect();
    qm.set_game_slot_options(ModelRc::from(Rc::new(VecModel::from(disc_options))));
    qm.set_game_slot_dropdown_disc(true);
    qm.set_game_slot_dropdown_index(1);
    qm.set_game_slot_dropdown_scroll(0);
    qm.set_game_slot_dropdown_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_disc_dropdown");
    }
    qm.set_game_slot_dropdown_open(false);
    qm.set_game_slot_dropdown_disc(false);
    qm.set_game_discs(0);
    qm.set_game_stylus_on(false);
    /* Cheats (40) and Netplay (41) with their sheets. */
    let model = |v: Vec<&str>| ModelRc::from(Rc::new(VecModel::from(v.into_iter().map(SharedString::from).collect::<Vec<_>>())));
    let kinds = |v: Vec<i32>| ModelRc::from(Rc::new(VecModel::from(v)));
    qm.set_game_cheats_value("2 on".into());
    qm.set_game_net_value("Hosting • 1 connected".into());
    qm.set_selected_index(40);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_cheats_row");
    }
    qm.set_game_sheet_row(40);
    qm.set_game_sheet_title("Cheats".into());
    qm.set_game_sheet_titles(model(vec!["Cheat List", "Infinite Maximum Coins", "Powerup Always In Level",
        "In-level music mod", "Spare Item Type (In-Level)", "Spare Item Type (On Map)", "Invincibility", "Moon Jump"]));
    qm.set_game_sheet_values(model(vec!["Tobu Tobu Girl Deluxe", "", "", "", "", "", "", ""]));
    qm.set_game_sheet_kinds(kinds(vec![0, 2, 1, 1, 2, 1, 1, 1]));
    qm.set_game_sheet_index(1);
    qm.set_game_sheet_scroll(0);
    qm.set_game_sheet_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_cheats");
    }
    qm.set_selected_index(41);
    qm.set_game_sheet_row(41);
    qm.set_game_sheet_title("Netplay".into());
    qm.set_game_sheet_titles(model(vec!["Hosting • 1 connected", "Stop Hosting"]));
    qm.set_game_sheet_values(model(vec!["", ""]));
    qm.set_game_sheet_kinds(kinds(vec![3, 0]));
    qm.set_game_sheet_index(1);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_netplay");
    }
    qm.set_game_sheet_open(false);
    qm.set_game_net_value("".into());
    qm.set_game_cheats_value("".into());
    /* Hardcore session: no state rows. */
    qm.set_game_hardcore(true);
    qm.set_game_cheats_value("Not available in Hardcore Mode".into());
    qm.set_game_net_value("Not available in Hardcore Mode".into());
    for (name, sel) in [("qm_game_hardcore", 8), ("qm_game_hardcore_quit", 14), ("qm_game_hardcore_cheats", 40)] {
        qm.set_selected_index(sel);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_game_sheet_row(40);
    qm.set_game_sheet_title("Cheats".into());
    qm.set_game_sheet_titles(model(vec!["Not available in Hardcore Mode"]));
    qm.set_game_sheet_values(model(vec![""]));
    qm.set_game_sheet_kinds(kinds(vec![3]));
    qm.set_game_sheet_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_hardcore_cheats_sheet");
    }
    qm.set_game_sheet_open(false);
    qm.set_game_cheats_value("".into());
    qm.set_game_net_value("".into());
    qm.set_game_hardcore(false);
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
    for (name, sel, confirm, overlay) in [
        ("qm_stream", 15, -1, false),
        ("qm_stream_overlay", 38, -1, true),
        ("qm_stream_close", 17, 17, false),
    ] {
        qm.set_stream_overlay_on(overlay);
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
    /* PortMaster port (EPIC-058): Resume and Quit Game; PortMaster's own
     * interface: Quit PortMaster. */
    qm.set_port_section_visible(true);
    for (name, sel, portmaster) in [("qm_port", 44, false), ("qm_portmaster", 45, true)] {
        qm.set_port_portmaster(portmaster);
        qm.set_selected_index(sel);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_port_portmaster(false);
    qm.set_port_section_visible(false);
    /* RetroArch's own menu without a game (the RetroArch app). */
    qm.set_ra_section_visible(true);
    for (name, sel) in [("qm_retroarch", 49), ("qm_retroarch_quit", 50)] {
        qm.set_selected_index(sel);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_ra_section_visible(false);
    /* Game closing screen (surface-mode 3): with a cover (a synthetic
     * 3:4 gradient), then without. */
    let mut cover = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(300, 400);
    let cw = cover.width();
    for (i, px) in cover.make_mut_slice().iter_mut().enumerate() {
        let (x, y) = (i as u32 % cw, i as u32 / cw);
        *px = slint::Rgba8Pixel { r: (40 + x / 3) as u8, g: (60 + y / 4) as u8, b: 150, a: 255 };
    }
    qm.set_menu_panel_visible(false);
    qm.set_surface_mode(3);
    qm.set_closing_title("Adventure Island II - Aliens in Paradise".into());
    qm.set_closing_system("Game Boy".into());
    qm.set_closing_accent(slint::Color::from_rgb_u8(0x8b, 0x95, 0x6d));
    qm.set_closing_has_cover(true);
    qm.set_closing_cover(slint::Image::from_rgba8(cover));
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_closing");
    }
    qm.set_closing_has_cover(false);
    qm.set_closing_cover(slint::Image::default());
    for (w, h) in sizes {
        shot(win, w, h, "qm_game_closing_nocover");
    }
    qm.set_surface_mode(0);
    qm.set_menu_panel_visible(true);
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
    /* MUSIC (2026-10-12): the user's music over Home, then over a game
     * with the mixer (Music / Game volume). */
    qm.set_music_section_visible(true);
    qm.set_music_playing(true);
    qm.set_music_title("Airport Lounge • Kevin MacLeod".into());
    qm.set_music_volume(65);
    qm.set_game_volume(80);
    for (name, game, sel) in [("qm_music_home", false, 31), ("qm_music_game", true, 34)] {
        qm.set_game_section_visible(game);
        qm.set_selected_index(sel);
        for (w, h) in sizes {
            shot(win, w, h, name);
        }
    }
    qm.set_game_section_visible(false);
    qm.set_music_section_visible(false);
    /* Cast Screen (EPIC-029): the row while casting, the receiver
     * dropdown over Home, then over a game (low-delay receivers only). */
    qm.set_cast_row_visible(true);
    qm.set_cast_label("Living Room TV".into());
    qm.set_selected_index(39);
    for (w, h) in sizes {
        shot(win, w, h, "qm_cast");
    }
    let opts = |items: &[&str]| -> ModelRc<SharedString> {
        ModelRc::from(Rc::new(VecModel::from(items.iter().map(|k| SharedString::from(*k)).collect::<Vec<_>>())))
    };
    qm.set_cast_options(opts(&[
        "Stop Casting",
        "Living Room TV • Google Cast",
        "Office PC • Miracast",
        "Bedroom TV • DLNA • with delay",
    ]));
    qm.set_cast_dropdown_index(1);
    qm.set_cast_dropdown_open(true);
    for (w, h) in sizes {
        shot(win, w, h, "qm_cast_dropdown");
    }
    qm.set_game_section_visible(true);
    qm.set_cast_label("Off".into());
    qm.set_cast_options(opts(&["Living Room TV • Google Cast", "Office PC • Miracast"]));
    qm.set_cast_dropdown_index(0);
    for (w, h) in sizes {
        shot(win, w, h, "qm_cast_game");
    }
    qm.set_cast_dropdown_open(false);
    qm.set_cast_row_visible(false);
    qm.set_game_section_visible(false);
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
        ("wifi_lost", 8, 2, t(385), "MyHome-5G".into(), -1, -1.0),
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
    card.hide().unwrap();
    drop(card);
    /* Stream performance overlay: the bottom strip surface (58 px). */
    let hud = StreamHudWindow::new().unwrap();
    hud.show().unwrap();
    hud.set_line_one("14:32   BAT 78%   1280×720   60 fps   18.4 Mbps".into());
    hud.set_line_two(format!("{} 4 ms ±1   {} 5.1 ms   {} 3.2 ms   {} 0.0%", t(1190), t(1191), t(1192), t(1193)).into());
    for w in [640u32, 720, 1280] {
        shot(win, w, 58, "stream_overlay");
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
    ui.set_lifecycle_on(false);
    std::thread::sleep(std::time::Duration::from_millis(600));
    slint::platform::update_timers_and_animations();

    ui.set_topbar_time("14:32".into());
    ui.set_battery_percent(78);
    ui.set_battery_label("78%".into());
    ui.set_wifi_state("connected".into());
    ui.set_current_user_name("Player 1".into());
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
    ui.set_wifi_current_ssid("MyHome-5G".into());
    ui.set_system_storage_summary("SD1 • SD2".into());
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
        WifiNetworkEntry { ssid: "MyHome-5G".into(), security: "WPA2".into(), signal_dbm: -48, saved: true, current: true },
        WifiNetworkEntry { ssid: "Vodafone-A1B2C3D4E5 Extended Network Name".into(), security: "WPA2".into(), signal_dbm: -70, saved: false, current: false },
        WifiNetworkEntry { ssid: "Guest".into(), security: "Open".into(), signal_dbm: -80, saved: true, current: false },
    ];
    ui.set_wifi_networks(ModelRc::from(Rc::new(VecModel::from(nets))));

    let sizes = [(640u32, 480u32), (720, 720), (720, 480), (1280, 720), (1920, 1080)];
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
            ui.set_keyboard_title(format!("{} • MyHome-5G", s(ui, 172)).into());
            ui.set_keyboard_value("nuubos handheld".into());
            ui.set_keyboard_index(14);
            ui.set_keyboard_input_kind("password".into()); ui.set_keyboard_page(0);
            ui.set_launch_open(false); ui.set_settings_open(true); ui.set_settings_selected_index(1);
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
        /* Home Music off (user request 2026-10-28): the toggle focused, the volume dimmed. */
        ("display_audio_music_off", Box::new(|ui| { ui.set_home_music_enabled(false); ui.set_display_audio_index(5); })),
        /* Volume Button Step (GitHub #6) focused. */
        ("display_audio_volume_step", Box::new(|ui| { ui.set_settings_open(true); ui.set_settings_selected_index(4); ui.set_settings_view(18); ui.set_volume_step(5); ui.set_display_audio_index(3); ui.set_display_audio_scroll_offset(0); })),
        ("display_audio_sounds", Box::new(|ui| { ui.set_home_music_enabled(true); ui.set_display_color_temperature_available(true); ui.set_display_audio_index(11); ui.set_display_audio_scroll_offset(7); })),
        ("display_audio_cast", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_settings_selected_index(4); ui.set_settings_view(18);
            ui.set_display_color_temperature_available(false);
            ui.set_cast_available(true); ui.set_cast_audio(true); ui.set_cast_latency("low".into());
            ui.set_cast_h264("installed".into()); ui.set_cast_encoder_value(s(ui, 1220).into());
            ui.set_display_audio_index(13); ui.set_display_audio_scroll_offset(10);
        })),
        ("display_audio_cast_download", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_settings_selected_index(4); ui.set_settings_view(18);
            ui.set_cast_available(true); ui.set_cast_latency("normal".into());
            ui.set_cast_h264("installing".into()); ui.set_cast_encoder_value(s(ui, 1221).replace("{0}", "42").into());
            ui.set_display_audio_index(13); ui.set_display_audio_scroll_offset(10);
        })),
        ("system", Box::new(|ui| { ui.set_display_color_temperature_available(false); ui.set_settings_selected_index(6); ui.set_settings_view(19); ui.set_system_index(5); ui.set_system_scroll_offset(0); })),
        ("power", Box::new(|ui| { ui.set_settings_open(true); ui.set_settings_selected_index(5); ui.set_settings_view(69); ui.set_power_index(3); ui.set_power_scroll_offset(0); })),
        ("power_quick", Box::new(|ui| { ui.set_settings_open(true); ui.set_settings_selected_index(5); ui.set_settings_view(69); ui.set_system_hypersleep(false); ui.set_power_index(3); ui.set_power_scroll_offset(0); })),
        ("rumble", Box::new(|ui| { ui.set_settings_open(true); ui.set_settings_selected_index(2); ui.set_rumble_supported(true); ui.set_rumble_mode("light".into()); ui.set_settings_view(16); ui.set_rumble_index(0); })),
        ("power_key", Box::new(|ui| { ui.set_settings_open(true); ui.set_system_hypersleep(true); ui.set_system_power_key_sleep(true); ui.set_settings_selected_index(5); ui.set_settings_view(69); ui.set_power_index(5); ui.set_power_scroll_offset(2); })),
        ("power_devices", Box::new(|ui| { ui.set_settings_open(true); ui.set_system_hypersleep(true); ui.set_settings_selected_index(5); ui.set_settings_view(69); ui.set_power_index(8); ui.set_power_scroll_offset(6); })),
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
                (&s(ui, 1391), "", &s(ui, 1392), false, 0),
                (&s(ui, 804), "", &s(ui, 805), false, -1),
                (&s(ui, 792), "ScreenScraper", &s(ui, 793), true, -1),
                (&s(ui, 1419), &s(ui, 1422), &s(ui, 1420), true, -1),
                (&s(ui, 565), "nuubfan", &s(ui, 567), false, -1),
                (&s(ui, 568), "", &s(ui, 569), false, -1), (&s(ui, 798), "", &s(ui, 799), false, -1),
                (&s(ui, 576), "", &s(ui, 581), true, -1),
                (&s(ui, 669), &s(ui, 222), &s(ui, 489), true, -1),
                (&s(ui, 1028), "", &s(ui, 1029), true, -1)]));
            ui.set_gaming_split(9);
            ui.set_gaming_split2(15);
        })),
        ("gaming", Box::new(|ui| { ui.set_settings_selected_index(3); ui.set_settings_view(68); ui.set_gaming_index(3); })),
        ("gaming_retroarch", Box::new(|ui| { ui.set_gaming_index(8); })),
        ("gaming_library", Box::new(|ui| { ui.set_gaming_index(14); })),
        /* Cover Region (GitHub #10). */
        ("gaming_region", Box::new(|ui| { ui.set_gaming_index(11); })),
        ("gaming_syncthing", Box::new(|ui| { ui.set_gaming_index(17); })),
        ("gaming_signout", Box::new(|ui| {
            ui.set_gaming_index(12);
            /* Sign Out waits for its confirming press. */
            let rows = ui.get_gaming_rows();
            if let Some(mut r) = rows.row_data(12) { r.armed = true; r.detail = s(ui, 191).into(); rows.set_row_data(12, r); }
        })),
        ("game_hotkeys", Box::new(|ui| {
            ui.set_gaming_index(0);
            ui.set_settings_view(55); ui.set_gen_index(1); ui.set_gen_scroll(0); ui.set_gen_split(0);
            ui.set_gen_section(s(ui, 772).into()); ui.set_gen_select(true);
            ui.set_gen_rows(gen(&[(&s(ui, 773), "", "", false, -1), (&s(ui, 417), &s(ui, 346), "", true, -1),
                (&s(ui, 418), &s(ui, 345), "", true, -1), (&s(ui, 779), &s(ui, 341), "", true, -1),
                (&s(ui, 780), &s(ui, 342), "", true, -1), (&s(ui, 781), &s(ui, 339), "", true, -1),
                (&s(ui, 422), &s(ui, 351), "", true, -1), (&s(ui, 592), &s(ui, 347), "", true, -1),
                (&s(ui, 1302), &s(ui, 352), "", true, -1), (&s(ui, 1307), &s(ui, 348), "", true, -1)]));
        })),
        ("game_hotkeys_end", Box::new(|ui| {
            ui.set_gen_index(9); ui.set_gen_scroll(5);
        })),
        ("perf_overlay", Box::new(|ui| {
            ui.set_settings_view(56); ui.set_gen_index(0); ui.set_gen_scroll(0);
            ui.set_gen_section(s(ui, 774).into());
            ui.set_gen_rows(gen(&[(&s(ui, 782), "", "", false, 1), (&s(ui, 783), "", "", false, 1),
                ("CPU", "", "", false, 1), ("GPU", "", "", false, 0), (&s(ui, 784), "", "", false, 1),
                (&s(ui, 785), "", "", false, 0), (&s(ui, 786), "", "", false, 1), (&s(ui, 787), "", "", false, 1)]));
        })),
        ("restore", Box::new(|ui| { ui.set_settings_view(22); })),
        ("game_details", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_details_active(true); ui.set_settings_view(50); ui.set_gen_index(0);
            ui.set_details_title("Tobu Tobu Girl Deluxe".into()); ui.set_details_subtitle("Game Boy Color".into());
            ui.set_details_meta("Platform  •  Tangram Games".into());
            ui.set_details_description("An arcade platformer for the Game Boy Color: bounce off enemies, dash through the clouds and climb as high as you can. A chiptune jukebox and high scores to chase. Free, open-source homebrew: code MIT, graphics and music CC BY 4.0.".into());
            ui.set_details_cover(slint::Image::load_from_path(std::path::Path::new("/shot/covers/tobu-tobu-girl-deluxe.png")).unwrap_or_default()); ui.set_details_has_cover(true);
            ui.set_details_backdrop(art(96, 72, (30, 110, 60), (20, 40, 110), false)); ui.set_details_has_backdrop(true);
            ui.set_details_chips(chips(&[(35, &s(ui, 1233), false), (11, "1", false),
                (1, &s(ui, 789).replace("{0}", "90"), false), (12, &format!("{}  •  12 h 05 min", s(ui, 403)), false)]));
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (1, s(ui, 412), false, true, false),
                (2, s(ui, 537), false, false, false), (35, s(ui, 1230), false, false, false), (3, s(ui, 524), false, false, false),
                (5, s(ui, 560), false, false, false), (9, s(ui, 549), false, false, false), (10, s(ui, 544), false, false, false)]));
            ui.set_details_caption("gbc/Tobu Tobu Girl Deluxe.gbc".into()); ui.set_details_caption_armed(false);
            ui.set_details_desc_step(0); ui.set_gen_notice("".into());
        })),
        ("game_details_scrolled", Box::new(|ui| {
            ui.set_gen_index(2); ui.set_details_desc_step(1);
            ui.set_details_caption("Platformers, Favorites of the year".into());
        })),
        ("game_details_tags", Box::new(|ui| {
            ui.set_gen_index(3); ui.set_details_desc_step(0);
            ui.set_details_caption(format!("{}, {}", s(ui, 1233), s(ui, 1236)).into());
        })),
        ("game_delete", Box::new(|ui| {
            ui.set_gen_index(6); ui.set_details_desc_step(0);
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (1, s(ui, 412), false, true, false),
                (2, s(ui, 537), false, false, false), (3, s(ui, 524), false, false, false),
                (5, s(ui, 560), false, false, false), (9, s(ui, 549), false, false, false), (10, s(ui, 544), false, false, true)]));
            ui.set_details_caption(s(ui, 546).into()); ui.set_details_caption_armed(true);
        })),
        ("game_details_advanced_search", Box::new(|ui| {
            /* GitHub #29: one Advanced Search action for the search name,
             * system and Cover Region (page 51, game_settings_search). */
            ui.set_gen_index(6); ui.set_details_desc_step(0);
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (1, s(ui, 412), false, true, false),
                (2, s(ui, 537), false, false, false), (35, s(ui, 1230), false, false, false), (3, s(ui, 524), false, false, false),
                (5, s(ui, 560), false, false, false), (17, s(ui, 1473), false, false, false),
                (9, s(ui, 549), false, false, false), (10, s(ui, 544), false, false, false)]));
            ui.set_details_caption(s(ui, 801).replace("{0}", "Daytona USA").into()); ui.set_details_caption_armed(false);
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
        ("game_details_port", Box::new(|ui| {
            ui.set_gen_index(3); ui.set_gen_notice("".into());
            ui.set_details_title("A Meta Data Game".into()); ui.set_details_subtitle("Ports".into());
            ui.set_details_meta("2020  •  Luis Zuno".into());
            ui.set_details_description("Embark on a short yet captivating journey of self-discovery.".into());
            ui.set_details_actions(actions(&[(0, s(ui, 413), true, false, false), (2, s(ui, 537), false, false, false),
                (35, s(ui, 1230), false, false, false), (23, s(ui, 1376), false, true, false),
                (9, s(ui, 549), false, false, false), (10, s(ui, 544), false, false, false)]));
            ui.set_details_caption(s(ui, 1378).into()); ui.set_details_caption_armed(false);
        })),
        ("game_settings", Box::new(|ui| {
            ui.set_gen_notice("".into()); ui.set_details_has_cover(false);
            ui.set_details_active(true); ui.set_settings_view(51); ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_details_title("Tobu Tobu Girl Deluxe".into()); ui.set_gen_section(s(ui, 524).into());
            ui.set_details_subtitle("Nintendo Entertainment System".into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), &s(ui, 528).replace("{0}", "snes9x"), "", true, -1),
                (&s(ui, 1253), "", "", true, -1),
                (&s(ui, 526), "4:3", "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]));
        })),
        ("game_settings_search", Box::new(|ui| {
            ui.set_gen_index(1); ui.set_gen_section(s(ui, 1473).into()); ui.set_settings_search_page(true);
            ui.set_details_title("Daytona USA".into()); ui.set_details_subtitle("PlayStation Portable".into());
            ui.set_gen_rows(gen(&[(&s(ui, 800), "Daytona USA", &s(ui, 1474), true, -1),
                (&s(ui, 1469), "Dreamcast", &s(ui, 1470), true, -1),
                (&s(ui, 1419), &s(ui, 122), &s(ui, 1471).replace("{0}", &s(ui, 122)), true, -1)]));
        })),
        ("game_settings_nds", Box::new(|ui| {
            ui.set_gen_index(4); ui.set_gen_section(s(ui, 524).into()); ui.set_settings_search_page(false);
            ui.set_details_title("Mario Kart DS".into());
            ui.set_details_subtitle("Nintendo DS".into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), &s(ui, 528).replace("{0}", "melonds_nuubos"), "", true, -1),
                (&s(ui, 1253), "", "", true, -1),
                (&s(ui, 526), &s(ui, 528).replace("{0}", &s(ui, 531)), "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 1314), &s(ui, 528).replace("{0}", &s(ui, 1315)), &s(ui, 1317), true, -1),
                (&s(ui, 1464), &s(ui, 1465), &s(ui, 1466), true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]));
        })),
        ("game_settings_psx", Box::new(|ui| {
            ui.set_gen_index(4);
            ui.set_details_title("Ape Escape".into());
            ui.set_details_subtitle("PlayStation".into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), &s(ui, 528).replace("{0}", "pcsx_rearmed"), "", true, -1),
                (&s(ui, 1253), "", "", true, -1),
                (&s(ui, 526), &s(ui, 528).replace("{0}", &s(ui, 531)), "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 454), &s(ui, 528).replace("{0}", &s(ui, 52)), &s(ui, 1381), true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]));
        })),
        /* System Settings (GitHub #12): a game inheriting its system, then
         * the system's page inside the shell. */
        ("game_settings_system", Box::new(|ui| {
            ui.set_gen_index(0);
            let sys = s(ui, 1448);
            ui.set_gen_rows(gen(&[(&s(ui, 1452).replace("{0}", "PlayStation"), "", &s(ui, 1453), true, -1),
                (&s(ui, 525), &sys.replace("{0}", "swanstation"), "", true, -1),
                (&s(ui, 1253), "", "", true, -1),
                (&s(ui, 526), &sys.replace("{0}", "4:3"), "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 454), &sys.replace("{0}", &s(ui, 1380)), &s(ui, 1381), true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]));
        })),
        ("game_settings_system_page", Box::new(|ui| {
            ui.set_gen_index(5); ui.set_settings_system_scope(true);
            ui.set_gen_section(s(ui, 1452).replace("{0}", "PlayStation").into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), "swanstation", "", true, -1),
                (&s(ui, 526), "4:3", "", true, -1), (&s(ui, 527), &s(ui, 528).replace("{0}", &s(ui, 532)), "", true, -1),
                (&s(ui, 454), &s(ui, 1380), &s(ui, 1381), true, -1),
                (&s(ui, 1450), "", &s(ui, 1449).replace("{0}", "3"), false, -1),
                (&s(ui, 1451), "", &s(ui, 536), false, -1)]));
        })),
        /* Controls (user request 2026-10-29): a NES game whose top and
         * left buttons press A and B, the B button following its system. */
        ("game_controls", Box::new(|ui| {
            ui.set_gen_index(2); ui.set_settings_system_scope(false);
            ui.set_gen_section(s(ui, 1456).into());
            let sys = s(ui, 1448);
            let none = s(ui, 173);
            ui.set_gen_rows(gen(&[(&s(ui, 337), &s(ui, 528).replace("{0}", "B"), "", true, -1),
                (&s(ui, 338), &s(ui, 528).replace("{0}", "A"), "", true, -1),
                (&s(ui, 339), "A", "", true, -1), (&s(ui, 340), &sys.replace("{0}", "B"), "", true, -1),
                (&s(ui, 341), &s(ui, 528).replace("{0}", "↑"), "", true, -1),
                (&s(ui, 342), &s(ui, 528).replace("{0}", "↓"), "", true, -1),
                (&s(ui, 345), &s(ui, 528).replace("{0}", &none), "", true, -1),
                (&s(ui, 1458), "", &s(ui, 536), false, -1)]));
        })),
        ("game_controls_choice", Box::new(|ui| {
            ui.set_settings_choice_title(s(ui, 339).into());
            let labels = [s(ui, 528).replace("{0}", &s(ui, 173)), "A".into(), "B".into(), s(ui, 352),
                s(ui, 351), s(ui, 341), s(ui, 342), s(ui, 343), s(ui, 344), s(ui, 173)];
            let o: Vec<SettingsChoiceEntry> = labels.iter()
                .map(|l| SettingsChoiceEntry { value: "".into(), label: l.as_str().into() }).collect();
            ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o))));
            ui.set_settings_choice_index(1);
            ui.set_settings_choice_open(true);
        })),
        ("game_controls_psx", Box::new(|ui| {
            ui.set_settings_choice_open(false);
            ui.set_gen_index(0);
            let d = s(ui, 528);
            ui.set_gen_rows(gen(&[(&s(ui, 337), &d.replace("{0}", &s(ui, 1459)), "", true, -1),
                (&s(ui, 338), &d.replace("{0}", &s(ui, 1460)), "", true, -1),
                (&s(ui, 339), &d.replace("{0}", &s(ui, 1461)), "", true, -1),
                (&s(ui, 340), &d.replace("{0}", &s(ui, 1462)), "", true, -1),
                (&s(ui, 351), &d.replace("{0}", &s(ui, 351)), "", true, -1)]));
        })),
        ("game_settings_dreamcast", Box::new(|ui| {
            ui.set_gen_section(s(ui, 524).into()); ui.set_settings_system_scope(false);
            ui.set_gen_index(4);
            ui.set_details_title("Dead or Alive 2".into());
            ui.set_details_subtitle("Dreamcast".into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), &s(ui, 528).replace("{0}", "flycast_nuubos"), "", true, -1),
                (&s(ui, 1253), "", "", true, -1),
                (&s(ui, 526), &s(ui, 528).replace("{0}", &s(ui, 531)), "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 1314), &s(ui, 528).replace("{0}", &s(ui, 1316)), &s(ui, 1383), true, -1),
                (&s(ui, 454), &s(ui, 528).replace("{0}", &s(ui, 1379)), &s(ui, 1382), true, -1),
                (&s(ui, 1464), &s(ui, 528).replace("{0}", &s(ui, 1465)), &s(ui, 1466), true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]));
        })),
        ("game_settings_core_download", Box::new(|ui| {
            ui.set_gen_index(1);
            let rows = gen(&[(&s(ui, 525), "bsnes", "", true, -1),
                (&s(ui, 1254), "42%", "", false, -1),
                (&s(ui, 526), "4:3", "", true, -1), (&s(ui, 527), &s(ui, 534), "", true, -1),
                (&s(ui, 535), "", &s(ui, 536), false, -1)]);
            let mut r = rows.row_data(1).unwrap();
            r.progress_shown = true;
            r.progress = 0.42;
            rows.set_row_data(1, r);
            ui.set_gen_rows(rows);
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
        /* Settings → Gaming → System Settings (76) and a system (77). */
        ("systems_list", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_gen_select(true); ui.set_details_active(false);
            ui.set_settings_selected_index(3); ui.set_settings_view(76); ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_gen_split(0); ui.set_gen_notice("".into());
            ui.set_gen_section(s(ui, 1454).into());
            ui.set_gen_rows(gen(&[("Game Boy Advance", "mgba", "", true, -1), ("PlayStation", "swanstation", "", true, -1),
                ("Nintendo 64", "parallel_n64_nuubos", "", true, -1), ("Dreamcast", "flycast_nuubos", "", true, -1),
                ("Nintendo DS", "melonds_nuubos", "", true, -1)]));
        })),
        ("system_settings", Box::new(|ui| {
            ui.set_settings_view(77); ui.set_gen_index(3);
            ui.set_gen_section("Nintendo 64".into());
            ui.set_gen_rows(gen(&[(&s(ui, 525), &s(ui, 528).replace("{0}", "parallel_n64_nuubos"), "", true, -1),
                (&s(ui, 526), &s(ui, 528).replace("{0}", &s(ui, 531)), "", true, -1),
                (&s(ui, 527), &s(ui, 528).replace("{0}", &s(ui, 532)), "", true, -1),
                (&s(ui, 1339), &s(ui, 1341), &s(ui, 1342), true, -1),
                (&s(ui, 454), &s(ui, 528).replace("{0}", &s(ui, 1379)), &s(ui, 1382), true, -1),
                (&s(ui, 1451), "", &s(ui, 536), false, -1)]));
        })),
        ("achievements", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_gen_select(true); ui.set_details_active(false); ui.set_settings_selected_index(3); ui.set_settings_view(54); ui.set_gen_index(2); ui.set_gen_scroll(0); ui.set_gen_split(0);
            ui.set_gen_section(s(ui, 584).into()); ui.set_gen_notice("".into());
            ui.set_gen_rows(gen(&[(&s(ui, 588), "alice", "", false, -1), (&s(ui, 112), "", "", false, 1),
                (&s(ui, 585), "", &s(ui, 586), false, 0), (&s(ui, 1445), "", &s(ui, 1446), false, 1),
                (&s(ui, 567), "", "", false, -1)]));
        })),
        ("achievements_hardcore", Box::new(|ui| {
            ui.set_settings_dialog_title(s(ui, 907).into()); ui.set_settings_dialog_body(s(ui, 908).into());
            ui.set_settings_dialog_confirm(s(ui, 909).into()); ui.set_settings_dialog_index(1); ui.set_settings_dialog_open(true);
        })),
        ("web", Box::new(|ui| {
            ui.set_settings_dialog_open(false);
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
        ("safe_screen", Box::new(|ui| { ui.set_recovery_notice("".into()); ui.set_recovery_armed(-1); ui.set_recovery_stage(0); ui.set_active_user_id("u".into()); ui.set_safe_index(0); ui.set_safe_screen_open(true); })),
        ("safe_screen_armed", Box::new(|ui| { ui.set_safe_index(5); ui.set_recovery_armed(5); ui.set_recovery_stage(0); })),
        ("safe_screen_notice", Box::new(|ui| { ui.set_safe_index(2); ui.set_recovery_armed(-1); ui.set_recovery_stage(0); ui.set_recovery_notice("Support bundle saved: nuubos-support-20261008-070500.tar.gz".into()); })),
        /* SD2 restore screen and release notes viewer (2026-10-12). */
        ("restore_running", Box::new(|ui| {
            ui.set_safe_screen_open(false); ui.set_op_oob(true); ui.set_op_failed(false); ui.set_op_progress(42);
            ui.set_op_index(0); ui.set_op_screen_open(true); })),
        ("restore_failed", Box::new(|ui| { ui.set_op_failed(true); ui.set_op_index(1); })),
        ("update_screen", Box::new(|ui| { ui.set_op_kind(1); ui.set_op_oob(false); ui.set_op_failed(false); ui.set_op_progress(63); ui.set_op_index(0); })),
        ("update_screen_failed", Box::new(|ui| { ui.set_op_failed(true); ui.set_op_index(0); })),
        ("notes_view", Box::new(|ui| {
            ui.set_op_screen_open(false); ui.set_op_kind(0);
            ui.set_notes_title(format!("{} • nuubOS 0.6", s(ui, 1005)).into());
            ui.set_notes_text(["OTA updates with two system slots and an automatic return to the previous version when the new one does not start",
                "Update channels (Stable, RC, Nightly) and a daily check with a notification", "Full-screen SD2 restore with progress",
                "Fix: Wi-Fi gets its address by itself after every boot", "Fix: CPU stability under load (1416 MHz maximum)",
                "Release notes on their own screen, scrolled with the D-pad", "Faster library scan on large SD2 cards",
                "New themes: Lagoon and Ember", "Smaller fixes and translations"].iter().map(|l| format!("•  {l}")).collect::<Vec<_>>().join("\n\n").into());
            ui.set_notes_scroll(0.0); ui.set_notes_open(true); })),
        ("notes_view_scrolled", Box::new(|ui| { ui.set_notes_scroll(2000.0); })),
        /* Software Update while the installation runs: the row is its bar. */
        ("update_installing", Box::new(|ui| {
            ui.set_notes_open(false);
            ui.set_settings_open(true); ui.set_details_active(false);
            ui.set_settings_selected_index(6); ui.set_settings_view(63); ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_gen_section(s(ui, 732).into()); ui.set_gen_select(true); ui.set_gen_split(0); ui.set_gen_split2(0);
            let mut rows: Vec<GenRow> = gen(&[("Version", "0.5 • 3ddd1b61d1b1", "", false, -1), (&s(ui, 985), &s(ui, 988), &s(ui, 989), true, -1),
                (&s(ui, 992), "", &s(ui, 993), false, 1), (&s(ui, 487), "nuubOS 0.6", "", false, -1),
                (&s(ui, 984), "42%", "", false, -1), (&s(ui, 1005), "", "", true, -1)]).iter().collect();
            rows[4].progress_shown = true; rows[4].progress = 0.42;
            ui.set_gen_rows(ModelRc::from(Rc::new(VecModel::from(rows))));
        })),
        /* Settings → System → Storage (storageui.rs page 64), Backup and
         * Recovery (filesui.rs pages 62 and 28, rows from storageui.rs),
         * Backup Content 59. */
        ("storage", Box::new(|ui| {
            ui.set_safe_screen_open(false); ui.set_recovery_notice("".into());
            ui.set_notes_open(false); ui.set_op_screen_open(false);
            ui.set_settings_open(true); ui.set_details_active(false);
            ui.set_settings_selected_index(6); ui.set_settings_view(64); ui.set_gen_index(2); ui.set_gen_scroll(0);
            ui.set_gen_section(s(ui, 19).to_uppercase().into()); ui.set_gen_select(true);
            ui.set_gen_split(0); ui.set_gen_split2(0);
            ui.set_gen_rows(gen(&[
                (&s(ui, 912), &s(ui, 914).replace("{0}", "41.1 GB").replace("{1}", "60.2 GB"), &s(ui, 913), false, -1),
                (&s(ui, 64), &s(ui, 914).replace("{0}", "214.3 GB").replace("{1}", "255.8 GB"), "SANDISK", false, -1),
                (&s(ui, 924), "", "", false, -1),
                (&s(ui, 926), "", &s(ui, 927), false, -1),
                (&s(ui, 928), "", &s(ui, 929), false, -1),
                (&s(ui, 1435), &s(ui, 54), &s(ui, 1439), true, -1),
                (&s(ui, 625), "", "", true, -1)]));
        })),
        ("storage_format_armed", Box::new(|ui| {
            ui.set_gen_index(4); ui.set_gen_scroll(0);
            let rows = ui.get_gen_rows();
            if let Some(mut r) = rows.row_data(4) { r.armed = true; r.detail = s(ui, 930).into(); rows.set_row_data(4, r); }
        })),
        ("storage_empty", Box::new(|ui| {
            ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_gen_rows(gen(&[
                (&s(ui, 912), &s(ui, 914).replace("{0}", "41.1 GB").replace("{1}", "60.2 GB"), &s(ui, 913), false, -1),
                (&s(ui, 64), &s(ui, 916), "", false, -1),
                (&s(ui, 625), "", "", true, -1)]));
        })),
        /* Data on SD2 (storageui.rs page 75, GitHub #24). */
        ("storage_data", Box::new(|ui| {
            ui.set_settings_open(true); ui.set_details_active(false); ui.set_settings_selected_index(6);
            ui.set_gen_select(true); ui.set_gen_split(0); ui.set_gen_split2(0);
            ui.set_settings_view(75); ui.set_gen_index(2); ui.set_gen_scroll(0);
            ui.set_gen_section(s(ui, 1435).to_uppercase().into());
            ui.set_gen_rows(gen(&[
                (&s(ui, 576), &s(ui, 912), "", false, -1), (&s(ui, 947), &s(ui, 64), "", false, -1),
                (&s(ui, 948), &s(ui, 64), "", false, -1), (&s(ui, 951), &s(ui, 912), "", false, -1),
                (&s(ui, 953), &s(ui, 912), &s(ui, 954), false, -1)]));
        })),
        ("storage_data_moving", Box::new(|ui| {
            let rows = ui.get_gen_rows();
            for i in 0..rows.row_count() {
                if let Some(mut r) = rows.row_data(i) { r.enabled = false; rows.set_row_data(i, r); }
            }
            if let Some(mut r) = rows.row_data(4) {
                r.value = s(ui, 1436).into(); r.progress_shown = true; r.progress = 0.37; rows.set_row_data(4, r);
            }
        })),
        ("storage_data_offline", Box::new(|ui| {
            ui.set_gen_rows(gen(&[
                (&s(ui, 576), &s(ui, 912), "", false, -1), (&s(ui, 947), &s(ui, 64), &s(ui, 1437), false, -1),
                (&s(ui, 948), &s(ui, 64), &s(ui, 1437), false, -1), (&s(ui, 951), &s(ui, 912), "", false, -1),
                (&s(ui, 953), &s(ui, 912), &s(ui, 954), false, -1)]));
            ui.set_gen_notice(s(ui, 1438).replace("{0}", "3").into());
        })),
        ("storage_data_forget", Box::new(|ui| {
            ui.set_gen_notice("".into());
            ui.set_settings_choice_open(true); ui.set_settings_choice_title(s(ui, 1440).into());
            let o = vec![SettingsChoiceEntry { value: "forget".into(), label: s(ui, 1441).into() },
                SettingsChoiceEntry { value: "cancel".into(), label: s(ui, 619).into() }];
            ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o)))); ui.set_settings_choice_index(1);
        })),
        /* Backup (filesui.rs page 62): the console backup only (storageui.rs). */
        ("backup", Box::new(|ui| {
            ui.set_settings_choice_open(false);
            ui.set_settings_view(62); ui.set_gen_index(1); ui.set_gen_scroll(0);
            ui.set_gen_scopes(scopes(&[s(ui, 313)]));
            ui.set_gen_section(s(ui, 1024).into());
            ui.set_gen_split(0); ui.set_gen_split2(0);
            ui.set_gen_rows(gen(&[
                (&s(ui, 69), "", &s(ui, 933).replace("{0}", &s(ui, 402)), false, 1),
                (&s(ui, 1015), &s(ui, 64), "", true, -1),
                (&s(ui, 936), &s(ui, 937), "", true, -1),
                (&s(ui, 88), "", "", false, -1)]));
        })),
        ("backup_sd1", Box::new(|ui| {
            ui.set_gen_rows(gen(&[
                (&s(ui, 69), "", &s(ui, 933).replace("{0}", &s(ui, 402)), false, 1),
                (&s(ui, 1015), &s(ui, 912), &s(ui, 1022), true, -1),
                (&s(ui, 936), &s(ui, 937), "", true, -1),
                (&s(ui, 88), "", "", false, -1)]));
        })),
        ("backup_dest", Box::new(|ui| {
            ui.set_settings_choice_open(true); ui.set_settings_choice_title(s(ui, 1015).into());
            let o: Vec<SettingsChoiceEntry> = [(64, "sd2"), (912, "sd1"), (1016, "usb")].iter()
                .map(|(i, v)| SettingsChoiceEntry { value: (*v).into(), label: s(ui, *i).into() }).collect();
            ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o)))); ui.set_settings_choice_index(1);
        })),
        /* Recovery (filesui.rs page 28): RESTORE (storageui.rs) | REPAIR &
         * RESET (main.rs recovery_rows). */
        ("recovery", Box::new(|ui| {
            ui.set_settings_choice_open(false);
            ui.set_settings_view(28); ui.set_gen_index(0); ui.set_gen_scroll(0);
            ui.set_gen_scopes(scopes(&[s(ui, 313), s(ui, 313)]));
            ui.set_gen_section(s(ui, 1014).into());
            ui.set_gen_split(1); ui.set_gen_split_label(s(ui, 983).into()); ui.set_gen_split2(0);
            ui.set_gen_rows(gen(&[
                (&s(ui, 1017), "", &s(ui, 1020), true, -1),
                (&s(ui, 505), "", &s(ui, 512), false, -1),
                (&s(ui, 508), "", &s(ui, 513), false, -1),
                (&s(ui, 1396), "", &s(ui, 1397), false, -1),
                (&s(ui, 510), "", &s(ui, 514), false, -1),
                (&s(ui, 511), "", &s(ui, 515), false, -1)]));
        })),
        ("recovery_retroarch", Box::new(|ui| {
            ui.set_gen_index(3); ui.set_gen_scroll(1);
            let rows = ui.get_gen_rows();
            if let Some(mut r) = rows.row_data(3) { r.armed = true; r.detail = s(ui, 191).into(); rows.set_row_data(3, r); }
        })),
        ("recovery_scope", Box::new(|ui| {
            ui.set_settings_choice_open(true); ui.set_settings_choice_title(s(ui, 1019).into());
            let o = vec![SettingsChoiceEntry { value: "all".into(), label: s(ui, 1018).into() },
                SettingsChoiceEntry { value: "a".into(), label: "Player 1".into() },
                SettingsChoiceEntry { value: "b".into(), label: "Player 2".into() }];
            ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o)))); ui.set_settings_choice_index(0);
        })),
        ("recovery_reset_armed", Box::new(|ui| {
            ui.set_settings_choice_open(false);
            ui.set_gen_index(5); ui.set_gen_scroll(2);
            let rows = ui.get_gen_rows();
            if let Some(mut r) = rows.row_data(5) { r.armed = true; r.detail = s(ui, 517).into(); rows.set_row_data(5, r); }
        })),
        ("recovery_none", Box::new(|ui| {
            ui.set_gen_index(1); ui.set_gen_scroll(0);
            let mut rows: Vec<GenRow> = gen(&[
                (&s(ui, 634), "", "", false, -1),
                (&s(ui, 505), "", &s(ui, 512), false, -1),
                (&s(ui, 508), "", &s(ui, 513), false, -1),
                (&s(ui, 510), "", &s(ui, 514), false, -1),
                (&s(ui, 511), "", &s(ui, 515), false, -1)]).iter().collect();
            rows[0].enabled = false;
            ui.set_gen_rows(ModelRc::from(Rc::new(VecModel::from(rows))));
        })),
        ("storage_content", Box::new(|ui| {
            ui.set_gen_select_label("".into()); ui.set_settings_north_hint("".into()); ui.set_gen_scopes(scopes(&[]));
            ui.set_settings_view(59); ui.set_gen_index(6); ui.set_gen_scroll(0); ui.set_gen_split(0); ui.set_gen_split2(0);
            ui.set_gen_section(s(ui, 936).to_uppercase().into());
            ui.set_gen_rows(gen(&[
                (&s(ui, 945), "", &s(ui, 946), false, 1),
                (&s(ui, 947), "", "", false, 1), (&s(ui, 948), "", "", false, 1),
                (&s(ui, 949), "", &s(ui, 950), false, 1), (&s(ui, 951), "", "", false, 1),
                (&s(ui, 576), "", "", false, 1), (&s(ui, 953), "", &s(ui, 954), false, 0)]));
        })),
        ("storage_prompt", Box::new(|ui| {
            ui.set_settings_view(64); ui.set_gen_index(0); ui.set_gen_split(0);
            ui.set_gen_rows(gen(&[
                (&s(ui, 912), &s(ui, 914).replace("{0}", "41.1 GB").replace("{1}", "60.2 GB"), &s(ui, 913), false, -1),
                (&s(ui, 64), &s(ui, 914).replace("{0}", "63.8 GB").replace("{1}", "63.9 GB"), "SD64", false, -1),
                (&s(ui, 924), "", "", false, -1), (&s(ui, 926), "", &s(ui, 927), false, -1),
                (&s(ui, 928), "", &s(ui, 929), false, -1), (&s(ui, 625), "", "", true, -1)]));
            ui.set_settings_choice_open(true);
            ui.set_settings_choice_title(format!("{} • SD64", s(ui, 955)).into());
            let o = vec![SettingsChoiceEntry { value: "prepare".into(), label: s(ui, 926).into() },
                SettingsChoiceEntry { value: "keep".into(), label: s(ui, 956).into() }];
            ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o)))); ui.set_settings_choice_index(0);
        })),
        ("dropdown", Box::new(|ui| {
            ui.set_settings_choice_open(false);
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
        let users: Vec<UserProfileEntry> = [("Player 1", 2), ("Player 2", 6), ("Guest", 11)].iter().enumerate().map(|(i, (n, a))| UserProfileEntry {
            id: format!("u{i}").into(), name: (*n).into(), avatar_spec: format!("builtin:{a:02}").into(), avatar_path: "".into(),
            avatar: slint::Image::load_from_path(std::path::Path::new(
                &format!("/workspace/package/nuubos/nuubos-users/src/avatars/{a:02}.png"))).unwrap_or_default(),
            active: i == 0, default_user: i == 0 }).collect();
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
        ui.set_default_user_name("Player 1".into()); ui.set_oob_index(5); ui.set_oob_scroll(0);
    })));
    scenes.push(("oob_users_one", Box::new(|ui| {
        let users: Vec<UserProfileEntry> = vec![UserProfileEntry { id: "u0".into(), name: "Player 1".into(), avatar_spec: "".into(),
            avatar_path: "".into(), avatar: Default::default(), active: false, default_user: true }];
        ui.set_users(ModelRc::from(Rc::new(VecModel::from(users))));
        ui.set_user_count(1); ui.set_oob_index(1);
    })));
    /* Avatar picker over the Users step: the shipped built-in avatars. */
    scenes.push(("oob_avatar_picker", Box::new(|ui| {
        let choices: Vec<AvatarChoiceEntry> = (1..=20).map(|n| AvatarChoiceEntry {
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
        MoonlightHostEntry { id: id.into(), name: name.into(), address: address.into(), paired, online: online.into(), probing, wol: false, waking: false }
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
    /* Covers of free homebrew games (tools/ui-shot/covers, README there). */
    fn hbo(name: &str) -> Option<slint::Image> {
        slint::Image::load_from_path(std::path::Path::new(&format!("/shot/covers/{name}.png"))).ok()
    }
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
    /* Game tags on the cover (library.rs tag_label/tag_color). */
    fn tagged(mut c: HomeCard, t1: &str, c1: u32, t2: &str, c2: u32) -> HomeCard {
        let col = |v: u32| slint::Color::from_rgb_u8((v >> 16) as u8, (v >> 8) as u8, v as u8);
        c.tag1 = t1.into(); c.tag1_color = col(c1); c.tag2 = t2.into(); c.tag2_color = col(c2);
        c
    }
    fn units(mut v: Vec<HomeCard>) -> ModelRc<HomeCard> {
        let mut u = 0.0;
        for c in v.iter_mut() { c.x_units = u; u += c.aspect; }
        ModelRc::from(Rc::new(VecModel::from(v)))
    }
    let recent = units(vec![
        tagged(card(0, "Tobu Tobu Girl Deluxe", "Game Boy Color  •  Last played today  •  Played 2 h 05 min", "Game Boy Color", 1.11, 0x8b4fd8, hbo("tobu-tobu-girl-deluxe"), true, true),
            "Playing", 0x4f9cf9, "", 0),
        tagged(card(0, "µCity", "Game Boy Color  •  Last played yesterday", "Game Boy Color", 1.11, 0x8b4fd8, hbo("ucity"), true, false),
            "Completed", 0x3ec27a, "", 0),
        tagged(card(0, "Concentration Room", "Nintendo Entertainment System  •  Last played 3 days ago", "Nintendo Entertainment System", 1.29, 0xb03a2e, hbo("concentration-room"), true, false),
            "100%", 0xf2c94c, "", 0),
        card(0, "Libbet and the Magic Floor", "Game Boy  •  Last played 5 days ago", "Game Boy", 1.11, 0x6f7f5a, hbo("libbet"), true, false),
        tagged(card(0, "Thwaite", "Unavailable  •  Nintendo Entertainment System", "Nintendo Entertainment System", 1.14, 0xb03a2e, hbo("thwaite"), false, false),
            "Backlog", 0x8e9aaf, "", 0),
        card(0, "Tobu Tobu Girl", "Game Boy  •  Last played 12 days ago", "Game Boy", 1.11, 0x6f7f5a, hbo("tobu-tobu-girl"), true, true),
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
    let collections = units(vec![
        previews(card(2, "Homebrew Favorites", "7 games", "", 1.6, 0x2d3f66, None, true, false), vec![hbo("tobu-tobu-girl-deluxe").unwrap_or_default(), hbo("ucity").unwrap_or_default(), hbo("concentration-room").unwrap_or_default()]),
        card(2, "Couch Co-op", "3 games", "", 1.6, 0x2d3f66, None, true, false),
        card(1, "New Collection", "", "", 1.6, 0x2d3f66, None, true, false),
    ]);
    let hbi = |n: &str| hbo(n).unwrap_or_default();
    let shelf = units(vec![
        previews(card(3, "Game Boy Color", "2 games", "", 1.6, 0x8b4fd8, icon("gbc"), true, false), vec![hbi("tobu-tobu-girl-deluxe"), hbi("ucity")]),
        previews(card(3, "Game Boy", "2 games", "", 1.6, 0x6f7f5a, icon("gb"), true, false), vec![hbi("libbet"), hbi("tobu-tobu-girl")]),
        previews(card(3, "Nintendo Entertainment System", "10 games", "", 1.6, 0xb03a2e, icon("nes"), true, false), vec![hbi("concentration-room"), hbi("thwaite-title"), hbi("thwaite")]),
        card(3, "Mega Drive / Genesis", "1 game", "", 1.6, 0x1f4fa8, icon("megadrive"), true, false),
        card(3, "PICO-8", "3 games", "", 1.6, 0xff004d, None, true, false),
    ]);
    let apps = units(vec![
        card(4, "Moonlight", "", "", 1.0, 0x1a1d22, Some(cover(96, 96, (90, 160, 255))), true, false),
        card(4, "Steam Link", "", "", 1.0, 0x1a1d22, None, true, false),
        card(4, "Media Player", "", "", 1.0, 0x1a1d22, None, true, false),
    ]);
    let grid = units((0..7).map(|i| {
        let (t, sys, c, acc) = [("Tobu Tobu Girl Deluxe", "Game Boy Color", "tobu-tobu-girl-deluxe", 0x8b4fd8),
            ("Concentration Room", "Nintendo Entertainment System", "concentration-room", 0xb03a2e),
            ("µCity", "Game Boy Color", "ucity", 0x8b4fd8),
            ("Libbet and the Magic Floor", "Game Boy", "libbet", 0x6f7f5a),
            ("Thwaite", "Nintendo Entertainment System", "thwaite-title", 0xb03a2e),
            ("Tobu Tobu Girl", "Game Boy", "tobu-tobu-girl", 0x6f7f5a),
            ("Thwaite (Practice)", "Nintendo Entertainment System", "thwaite", 0xb03a2e)][i];
        /* The longest chip row (NES name, both play chips) on the focused card. */
        if i == 2 {
            return card(0, t, "Game Boy Color  •  Last played 3 days ago  •  Played 12 h 40 min", sys, 1.11, acc, hbo(c), true, true);
        }
        let card = card(0, t, &format!("{sys}  •  Never played"), sys, 1.11, acc, hbo(c), true, false);
        match i { 1 => tagged(card, "Completed", 0x3ec27a, "", 0), 3 => tagged(card, "Backlog", 0x8e9aaf, "", 0), _ => card }
    }).collect());
    let favorites_tile = previews(card(2, "Homebrew Favorites", "7 games", "", 1.6, 0x2d3f66, None, true, false),
        vec![hbi("tobu-tobu-girl-deluxe"), hbi("ucity"), hbi("concentration-room")]);
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
        ui.set_home_sheet_title("Add to Collection • Tobu Tobu Girl Deluxe".into());
        ui.set_home_sheet_items(ModelRc::from(Rc::new(VecModel::from(vec![
            HomeSheetItem { label: "RPG Classics".into(), detail: "".into(), check: 1, armed: false },
            HomeSheetItem { label: "Couch Co-op".into(), detail: "".into(), check: 0, armed: false },
            HomeSheetItem { label: "New Collection".into(), detail: "".into(), check: -1, armed: false },
        ]))));
        ui.set_home_sheet_index(1); ui.set_home_sheet_open(true);
    })));
    scenes.push(("start_game", Box::new(|ui| {
        ui.set_home_sheet_open(false);
        ui.set_start_sheet_title("Tobu Tobu Girl Deluxe".into()); ui.set_start_sheet_system("Game Boy Color".into());
        ui.set_start_sheet_cover(art(72, 96, (40, 90, 200), (120, 40, 160), true)); ui.set_start_sheet_has_cover(true);
        let saved = |t: &str| s(ui, 766).replace("{0}", t);
        let item = |label: String, when: &str, tile: &str, latest: bool| StartSheetItem {
            label: label.into(), detail: saved(when).into(), tile: tile.into(), latest };
        ui.set_start_sheet_items(ModelRc::from(Rc::new(VecModel::from(vec![
            item(s(ui, 423).replace("{0}", "3"), "today • 21:14", "3", true),
            item(s(ui, 765), "today • 20:58", "", false),
            item(s(ui, 423).replace("{0}", "2"), "today • 20:40", "2", false),
            item(s(ui, 423).replace("{0}", "1"), "yesterday • 18:02", "1", false),
            item(s(ui, 423).replace("{0}", "0"), "3 days ago • 22:31", "0", false),
        ]))));
        ui.set_start_sheet_index(1); ui.set_start_sheet_scroll(0); ui.set_start_sheet_open(true);
    })));
    scenes.push(("start_game_empty", Box::new(|ui| {
        ui.set_start_sheet_title("Tobu Tobu Girl Deluxe".into()); ui.set_start_sheet_system("Game Boy Color".into());
        ui.set_start_sheet_has_cover(false);
        ui.set_start_sheet_items(ModelRc::default());
        ui.set_start_sheet_index(0); ui.set_start_sheet_scroll(0); ui.set_start_sheet_open(true);
    })));
    scenes.push(("start_game_hardcore", Box::new(|ui| {
        ui.set_start_sheet_hardcore(true);
    })));
    scenes.push(("home_find", Box::new(move |ui| {
        ui.set_start_sheet_hardcore(false);
        ui.set_home_sheet_open(false);
        ui.set_start_sheet_open(false);
        ui.set_find_open(true); ui.set_find_zone(1); ui.set_find_query("mario".into());
        ui.set_find_chips(ModelRc::from(Rc::new(VecModel::from(vec![
            FindChip { label: "Game Boy Color".into(), active: true },
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
        ui.set_library_card(favorites_tile.clone());
        ui.set_library_games(grid.clone()); ui.set_library_aspect(1.11); ui.set_library_title("Homebrew Favorites".into());
        ui.set_library_detail("7 games".into()); ui.set_library_index(2); ui.set_library_open(true);
    })));
    scenes.push(("home_grid_scrolled", Box::new(|ui| { ui.set_library_index(6); })));
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
    scenes.push(("moonlight_wake", Box::new(move |ui| {
        ap(ui, "STREAM", "GAMING-PC", "192.168.5.20");
        ui.set_app_page_pin("".into());
        ui.set_settings_view(41); ui.set_moonlight_index(0);
        ui.set_app_page_chips(chips(&[(6, "Paired", false), (18, "Offline", true)]));
        ui.set_app_page_entries(entries(&[]));
        ui.set_app_page_description("The PC could not be reached".into());
        ui.set_app_page_actions(actions(&[(34, "Wake PC".into(), true, false, false), (10, "Remove PC".into(), false, false, false)]));
        ui.set_app_page_captions(captions(&["Sends a Wake-on-LAN signal to the PC", ""]));
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
    /* PortMaster (EPIC-058, settings-view 74). */
    let ap = app_page.clone();
    scenes.push(("portmaster_installed", Box::new(move |ui| {
        ap(ui, "PORTMASTER", "PortMaster", &format!("{} 2026.06.23-0015", s(ui, 485)));
        ui.set_steamlink_active(false); ui.set_moonlight_active(false); ui.set_netplay_active(false);
        ui.set_portmaster_active(true); ui.set_settings_view(74); ui.set_gen_index(0); ui.set_gen_notice("".into());
        ui.set_app_page_chips(chips(&[(6, &s(ui, 489), false)]));
        ui.set_app_page_entries(entries(&[]));
        ui.set_app_page_description(s(ui, 1367).into());
        ui.set_app_page_actions(actions(&[(0, s(ui, 1362), true, false, false), (5, s(ui, 487), false, false, false),
            (4, s(ui, 852), false, true, false), (27, s(ui, 1430), false, false, false), (10, s(ui, 1364), false, false, false)]));
        ui.set_app_page_captions(captions(&[&s(ui, 1363), "", &s(ui, 1368), &s(ui, 1431), &s(ui, 1365)]));
    })));
    /* Ports on SD2 (GitHub #21) focused. */
    scenes.push(("portmaster_location", Box::new(|ui| { ui.set_gen_index(3); })));
    let ap = app_page.clone();
    scenes.push(("portmaster_absent", Box::new(move |ui| {
        ap(ui, "PORTMASTER", "PortMaster", "");
        ui.set_portmaster_active(true); ui.set_settings_view(74); ui.set_gen_index(0);
        ui.set_app_page_chips(chips(&[(13, &s(ui, 494), true)]));
        ui.set_app_page_entries(entries(&[]));
        ui.set_app_page_description(s(ui, 1367).into());
        ui.set_app_page_actions(actions(&[(4, s(ui, 1359), true, false, false)]));
        ui.set_app_page_captions(captions(&[&s(ui, 494)]));
    })));
    /* Ports on SD2 being moved (GitHub #21). */
    scenes.push(("portmaster_moving", Box::new(|ui| {
        ui.set_app_page_chips(chips(&[]));
        ui.set_app_page_progress(0.37);
        ui.set_app_page_progress_label(format!("{} • 37%", s(ui, 1432)).into());
        ui.set_app_page_actions(actions(&[(7, s(ui, 482), true, false, false)]));
        ui.set_app_page_captions(captions(&[""]));
        ui.set_gen_index(0);
    })));
    scenes.push(("portmaster_moved", Box::new(|ui| {
        ui.set_app_page_progress(-1.0); ui.set_app_page_progress_label("".into());
        ui.set_app_page_actions(actions(&[(0, s(ui, 1362), true, false, false), (5, s(ui, 487), false, false, false),
            (4, s(ui, 852), false, true, false), (27, s(ui, 1430), false, true, false), (10, s(ui, 1364), false, false, false)]));
        ui.set_app_page_captions(captions(&[&s(ui, 1363), "", &s(ui, 1368), &s(ui, 1433).replace("{0}", "2"), &s(ui, 1365)]));
        ui.set_gen_index(3);
    })));
    /* Ports on SD2 with the card out: Insert SD2. */
    scenes.push(("portmaster_sd2_out", Box::new(|ui| {
        ui.set_app_page_chips(chips(&[(13, &s(ui, 81), true)]));
        ui.set_app_page_actions(actions(&[(4, s(ui, 1359), true, false, false), (27, s(ui, 1430), false, true, false)]));
        ui.set_app_page_captions(captions(&["", &s(ui, 1431)]));
        ui.set_gen_index(1);
    })));
    scenes.push(("portmaster_leave", Box::new(|ui| {
        ui.set_portmaster_active(false);
    })));
    let ap = app_page.clone();
    scenes.push(("netplay", Box::new(move |ui| {
        ap(ui, "NETPLAY", "Netplay", "");
        ui.set_steamlink_active(false); ui.set_moonlight_active(false);
        ui.set_netplay_active(true); ui.set_settings_view(72); ui.set_gen_index(0); ui.set_gen_notice("".into());
        ui.set_app_page_description(s(ui, 1262).into());
        let v: Vec<AppEntry> = vec![
            AppEntry { title: "Concentration Room".into(), detail: "Hosted by Player 2 • Nintendo Entertainment System".into(), value: s(ui, 1269).into(),
                icon: 11, good: true, caption: "Hosted by Player 2 • Nintendo Entertainment System • 192.168.5.40:55435".into() },
            AppEntry { title: "Super Tilt Bro.".into(), detail: "Hosted by Living Room PC".into(), value: s(ui, 1266).into(),
                icon: 11, good: false, caption: "".into() },
            AppEntry { title: "Bomberman '94".into(), detail: "Hosted by Bob • PC Engine".into(),
                value: s(ui, 1267).replace("{0}", "Beetle PCE").into(), icon: 11, good: false, caption: "".into() },
        ];
        ui.set_app_page_entries(ModelRc::from(Rc::new(VecModel::from(v))));
        ui.set_app_page_actions(actions(&[(17, s(ui, 1263), false, false, false)]));
        ui.set_app_page_captions(captions(&[""]));
    })));
    let ap = app_page.clone();
    scenes.push(("netplay_empty", Box::new(move |ui| {
        ap(ui, "NETPLAY", "Netplay", "");
        ui.set_netplay_active(true); ui.set_settings_view(72); ui.set_gen_index(0);
        ui.set_app_page_description(s(ui, 1265).into());
        ui.set_app_page_entries(entries(&[]));
        ui.set_app_page_actions(actions(&[(17, s(ui, 1263), true, false, false)]));
        ui.set_app_page_captions(captions(&[""]));
    })));
    scenes.push(("app_page_done", Box::new(|ui| {
        ui.set_netplay_active(false);
        ui.set_steamlink_active(false); ui.set_moonlight_active(false); ui.set_settings_open(false); ui.set_settings_view(0);
    })));
    scenes.push(("launch", Box::new(|ui| {
        ui.set_library_open(false);
        ui.set_launch_title("Tobu Tobu Girl Deluxe".into());
        ui.set_launch_system("Game Boy Color".into());
        ui.set_launch_accent(slint::Color::from_rgb_u8(0x6b, 0x5b, 0xc4));
        ui.set_launch_cover(slint::Image::load_from_path(std::path::Path::new("/shot/covers/tobu-tobu-girl-deluxe.png")).unwrap_or_default());
        ui.set_launch_has_cover(true);
        ui.set_launch_busy(true);
        ui.set_launch_open(true);
    })));
    scenes.push(("launch_closed", Box::new(|ui| { ui.set_launch_open(false); ui.set_launch_busy(false); })));
    /* Syncthing (2026-10-12): page 66 and the setup guide. */
    const SID: &str = "3ULBXAD-LZEBDFS-BDH7UDD-ST45XXC-FCF2M4O-Q62OCH5-LFBD5PZ-5EXXOAP";
    scenes.push(("sync_page", Box::new(|ui| {
        ui.set_launch_open(false);
        ui.set_settings_open(true); ui.set_details_active(false);
        ui.set_settings_selected_index(3); ui.set_settings_view(66); ui.set_gen_index(3); ui.set_gen_scroll(0);
        ui.set_gen_section("SYNCTHING".into()); ui.set_gen_select(true);
        ui.set_gen_split(3); ui.set_gen_split_label(s(ui, 1030).into());
        ui.set_gen_split2(6); ui.set_gen_split2_label(s(ui, 1031).into());
        ui.set_gen_rows(gen(&[
            (&s(ui, 669), "", &s(ui, 323), false, 1),
            (&s(ui, 1028), "", &s(ui, 1029), true, -1),
            (&s(ui, 1037), "Player 1 · RG CubeXX", &s(ui, 1038).replace("{0}", "3ULBXAD"), false, -1),
            ("Player 2 · RG40XX-V", "", &s(ui, 1032), false, -1),
            ("DESKTOP-NAS", &s(ui, 1039).replace("{0}", "42"), "192.168.1.20", false, -1),
            ("Pixel 8", &s(ui, 159), "", false, -1),
            (&s(ui, 673), "", &s(ui, 1324).replace("{0}", "retro-saves"), false, 1),
            (&s(ui, 674), "", "", false, 0),
            (&s(ui, 675), "", "", false, 0),
            (&s(ui, 1089), "", &s(ui, 1090), false, 1),
            (&s(ui, 1091), "", &s(ui, 1092), false, 1),
            (&s(ui, 1035), "", &s(ui, 1036), false, 1),
            (&s(ui, 1087), "", &s(ui, 1088), false, 0),
            ("RetroArch Saves", "retro-saves", &s(ui, 1325).replace("{0}", "DESKTOP-NAS"), false, -1),
            ("Screenshots", "x7k2p-shots", &s(ui, 1325).replace("{0}", "Pixel 8"), false, -1)]));
        ui.set_gen_split3(13);
        ui.set_gen_split3_label(s(ui, 1343).into());
    })));
    scenes.push(("sync_page_bottom", Box::new(|ui| { ui.set_gen_index(6); ui.set_gen_scroll(3);
    })));
    /* AVAILABLE FOLDERS (2026-10-21) and what one syncs as. */
    scenes.push(("sync_page_offers", Box::new(|ui| { ui.set_gen_index(13); ui.set_gen_scroll(9);
    })));
    scenes.push(("sync_page_offer_choice", Box::new(|ui| {
        let o: Vec<SettingsChoiceEntry> = [(674, "states"), (675, "screenshots"), (1034, "decline")].iter()
            .map(|(i, v)| SettingsChoiceEntry { value: (*v).into(), label: s(ui, *i).into() }).collect();
        ui.set_settings_choice_title(s(ui, 1344).replace("{0}", "RetroArch Saves").into());
        ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o)))); ui.set_settings_choice_index(0);
        ui.set_settings_choice_open(true);
    })));
    scenes.push(("sync_page_choice_closed", Box::new(|ui| { ui.set_settings_choice_open(false);
    })));
    scenes.push(("sync_guide_intro", Box::new(|ui| {
        ui.set_sync_guide_title(s(ui, 1040).into());
        ui.set_sync_guide_body(format!("{}\n\n{}", s(ui, 1041), s(ui, 1042)).into());
        ui.set_sync_guide_code("".into()); ui.set_sync_guide_id("".into()); ui.set_sync_guide_show_qr(false);
        ui.set_sync_guide_waiting("".into()); ui.set_sync_guide_error("".into());
        ui.set_sync_guide_items(gen(&[(&s(ui, 1043), "", &s(ui, 1044), true, -1), (&s(ui, 1045), "", &s(ui, 1046), true, -1),
            (&s(ui, 1047), "", &s(ui, 1048), true, -1)]));
        ui.set_sync_guide_index(1); ui.set_sync_guide_step(0); ui.set_sync_guide_open(true);
    })));
    scenes.push(("sync_guide_folders", Box::new(|ui| {
        ui.set_sync_guide_title(s(ui, 1049).into()); ui.set_sync_guide_body(format!("{} {}", s(ui, 1050), s(ui, 1093)).into());
        ui.set_sync_guide_items(gen(&[(&s(ui, 673), "", "", false, 1), (&s(ui, 674), "", "", false, 0),
            (&s(ui, 675), "", "", false, 0), (&s(ui, 1089), "", &s(ui, 1090), false, 0),
            (&s(ui, 1091), "", &s(ui, 1092), false, 1), (&s(ui, 1414), "", &s(ui, 1415), false, 0),
            (&s(ui, 1416), "", &s(ui, 1417), false, 0), (&s(ui, 391), "", "", false, -1)]));
        ui.set_sync_guide_index(7); ui.set_sync_guide_step(1);
    })));
    scenes.push(("sync_guide_console", Box::new(|ui| {
        let path = [s(ui, 3), s(ui, 702), s(ui, 669), s(ui, 1028), s(ui, 1043)].join(" → ");
        ui.set_sync_guide_title(s(ui, 1051).into());
        ui.set_sync_guide_body(format!("1. {}\n2. {}\n3. {}\n\n{}", s(ui, 1052), s(ui, 1053), s(ui, 1054).replace("{0}", &path),
            s(ui, 1055).replace("{0}", "Player 1 · RG CubeXX")).into());
        ui.set_sync_guide_code("3ULBXAD".into());
        ui.set_sync_guide_items(gen(&[(&s(ui, 391), "", "", false, -1)]));
        ui.set_sync_guide_index(0); ui.set_sync_guide_step(2);
    })));
    scenes.push(("sync_guide_pick", Box::new(|ui| {
        ui.set_sync_guide_title(s(ui, 1056).into()); ui.set_sync_guide_body(s(ui, 1057).into()); ui.set_sync_guide_code("".into());
        ui.set_sync_guide_waiting(s(ui, 1058).into());
        ui.set_sync_guide_items(gen(&[("Player 2 · RG40XX-V", "192.168.1.31", &s(ui, 1032), false, -1),
            (&s(ui, 1059).replace("{0}", "GBA3TN7"), "192.168.1.30", "", false, -1),
            (&s(ui, 1059).replace("{0}", "MZ2KQ4P"), "192.168.1.44", "", false, -1)]));
        ui.set_sync_guide_index(1); ui.set_sync_guide_step(3);
    })));
    scenes.push(("sync_guide_computer", Box::new(|ui| {
        ui.set_sync_guide_title(s(ui, 1067).into()); ui.set_sync_guide_body(s(ui, 1068).replace("{0}", "3ULBXAD").into());
        ui.set_sync_guide_id("3ULBXAD-LZEBDFS-BDH7UDD-ST45XXC-\nFCF2M4O-Q62OCH5-LFBD5PZ-5EXXOAP".into());
        ui.set_sync_guide_waiting(s(ui, 1069).into());
        ui.set_sync_guide_items(gen(&[("DESKTOP-NAS", "192.168.1.20", &s(ui, 1032), false, -1)]));
        ui.set_sync_guide_index(0); ui.set_sync_guide_step(3);
    })));
    scenes.push(("sync_guide_phone", Box::new(|ui| {
        let (size, dark) = qr::encode(SID).unwrap();
        let side = size + 8;
        let mut px = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(side as u32, side as u32);
        for (i, p) in px.make_mut_slice().iter_mut().enumerate() {
            let (x, y) = (i % side, i / side);
            let on = x >= 4 && y >= 4 && x < size + 4 && y < size + 4 && dark[(y - 4) * size + (x - 4)];
            let v = if on { 0 } else { 255 };
            *p = slint::Rgba8Pixel { r: v, g: v, b: v, a: 255 };
        }
        ui.set_sync_guide_qr(slint::Image::from_rgba8(px));
        ui.set_sync_guide_title(s(ui, 1076).into()); ui.set_sync_guide_body(s(ui, 1077).into());
        ui.set_sync_guide_id("".into()); ui.set_sync_guide_code("3ULBXAD".into()); ui.set_sync_guide_show_qr(true);
        ui.set_sync_guide_waiting(s(ui, 1078).into());
        ui.set_sync_guide_items(gen(&[]));
        ui.set_sync_guide_index(-1); ui.set_sync_guide_step(3);
    })));
    scenes.push(("sync_guide_done", Box::new(|ui| {
        ui.set_sync_guide_show_qr(false); ui.set_sync_guide_code("".into()); ui.set_sync_guide_waiting("".into());
        ui.set_sync_guide_title(s(ui, 1080).into()); ui.set_sync_guide_body(s(ui, 1081).replace("{0}", "Player 2 · RG40XX-V").into());
        ui.set_sync_guide_items(gen(&[(&s(ui, 697), "", "", false, -1)]));
        ui.set_sync_guide_index(0); ui.set_sync_guide_step(5);
    })));
    scenes.push(("sync_closed", Box::new(|ui| { ui.set_sync_guide_open(false); ui.set_settings_open(false); })));
    /* Tailscale (2026-10-21): page 73 and the sign-in screen. */
    scenes.push(("ts_page_download", Box::new(|ui| {
        ui.set_launch_open(false);
        ui.set_settings_open(true); ui.set_details_active(false);
        ui.set_settings_selected_index(1); ui.set_settings_view(73); ui.set_gen_index(0); ui.set_gen_scroll(0);
        ui.set_gen_section(s(ui, 1328).to_uppercase().into()); ui.set_gen_select(true);
        ui.set_gen_split(0); ui.set_gen_split2(0);
        ui.set_gen_rows(gen(&[(&s(ui, 653), "", &s(ui, 1330), true, -1)]));
    })));
    scenes.push(("ts_page_running", Box::new(|ui| {
        let version = format!("{} 1.104.2", s(ui, 485));
        ui.set_gen_rows(gen(&[
            (&s(ui, 1328), "", &s(ui, 198), false, 1),
            ("nuubos-rg-cubexx.tail1234.ts.net", "100.101.102.103", &s(ui, 1336), false, -1),
            (&s(ui, 567), "", "mario@example.com", false, -1),
            (&s(ui, 487), &version, &s(ui, 489), false, -1),
            (&s(ui, 644), "", &s(ui, 1335), false, -1)]));
    })));
    scenes.push(("ts_signin", Box::new(|ui| {
        let (size, dark) = qr::encode("https://login.tailscale.com/a/1b2a93e301d978").unwrap();
        let side = size + 8;
        let mut px = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(side as u32, side as u32);
        for (i, p) in px.make_mut_slice().iter_mut().enumerate() {
            let (x, y) = (i % side, i / side);
            let on = x >= 4 && y >= 4 && x < size + 4 && y < size + 4 && dark[(y - 4) * size + (x - 4)];
            let v = if on { 0 } else { 255 };
            *p = slint::Rgba8Pixel { r: v, g: v, b: v, a: 255 };
        }
        ui.set_sync_guide_qr(slint::Image::from_rgba8(px));
        ui.set_sync_guide_title(s(ui, 587).into()); ui.set_sync_guide_body(s(ui, 1332).into());
        ui.set_sync_guide_id("https://login.tailscale.com/a/1b2a93e301d978".into()); ui.set_sync_guide_code("".into());
        ui.set_sync_guide_show_qr(true); ui.set_sync_guide_waiting("".into()); ui.set_sync_guide_error("".into());
        ui.set_sync_guide_items(gen(&[]));
        ui.set_sync_guide_index(-1); ui.set_sync_guide_step(-1); ui.set_sync_guide_open(true);
    })));
    scenes.push(("ts_closed", Box::new(|ui| { ui.set_sync_guide_open(false); ui.set_settings_open(false); })));
    /* Media (2026-10-12): shelves, grid, album, Now Playing. */
    fn mtile(title: &str, sub: &str, kind: i32, art: Option<slint::Image>, progress: f32, badge: &str, tint: (u8, u8, u8)) -> MediaTile {
        MediaTile { title: title.into(), subtitle: sub.into(), has_art: art.is_some(), art: art.unwrap_or_default(), kind, progress,
            badge: badge.into(), tint: slint::Color::from_rgb_u8(tint.0, tint.1, tint.2) }
    }
    fn hints(items: &[(&str, i32, &str)]) -> ModelRc<AppHint> {
        ModelRc::from(Rc::new(VecModel::from(items.iter().map(|(k, r, l)| AppHint { kind: (*k).into(), role: *r, label: (*l).into() }).collect::<Vec<_>>())))
    }
    fn tracks(items: &[(&str, &str, &str, bool)]) -> ModelRc<MediaTrack> {
        ModelRc::from(Rc::new(VecModel::from(items.iter().map(|(n, t, d, c)| MediaTrack { number: (*n).into(), title: (*t).into(), detail: (*d).into(), current: *c }).collect::<Vec<_>>())))
    }
    let media_base = |ui: &HomeWindow| {
        ui.set_details_active(true); ui.set_library_open(false); ui.set_settings_choice_open(false);
        ui.set_details_shell_title("Media".into()); ui.set_settings_view(65); ui.set_settings_open(true);
        ui.set_moonlight_active(false); ui.set_steamlink_active(false); ui.set_files_open(false); ui.set_media_open(true);
        ui.set_app_kicker("MEDIA".into());
        ui.set_select_face_position(2); ui.set_back_face_position(1); ui.set_context_face_position(0); ui.set_details_face_position(3);
        ui.set_media_notice("".into());
    };
    let mb = media_base.clone();
    scenes.push(("media_home", Box::new(move |ui| {
        mb(ui);
        let shelf = |title: &str, tiles: Vec<MediaTile>, shape: i32| MediaShelf { title: title.into(), tiles: ModelRc::from(Rc::new(VecModel::from(tiles))), shape };
        let cont = vec![mtile("Big Buck Bunny", "Resume from 4:12", 0, Some(cover(320, 180, (230, 140, 60))), 0.42, "", (0, 0, 0)),
            mtile("Sintel", "Resume from 31:05", 0, None, 0.7, "", (140, 90, 230)), mtile("Tears of Steel", "", 0, None, 0.1, "", (60, 180, 140))];
        let videos = vec![mtile("Cosmos Laundromat", "", 0, Some(cover(200, 300, (60, 120, 220))), -1.0, "", (0, 0, 0)),
            mtile("Agent 327", "3 videos", 1, None, -1.0, "3", (220, 90, 120)), mtile("Spring", "", 0, Some(cover(200, 300, (90, 200, 120))), -1.0, "", (0, 0, 0)),
            mtile("Caminandes Llamigos", "", 0, None, -1.0, "", (240, 150, 60)), mtile("Elephants Dream", "", 0, None, -1.0, "", (60, 160, 200)),
            mtile("Coffee Run", "", 0, None, -1.0, "", (200, 160, 40))];
        let music = vec![mtile("Shuffle All", "", 4, None, -1.0, "", (58, 134, 255)),
            mtile("Jazz Lounge", "Kevin MacLeod • 8 tracks", 2, Some(cover(240, 240, (200, 60, 160))), -1.0, "", (0, 0, 0)),
            mtile("Cinematic Moods", "Kevin MacLeod • 12 tracks", 2, None, -1.0, "", (210, 170, 60)),
            mtile("In Rainbows", "Radiohead • 10 tracks", 2, Some(cover(240, 240, (240, 120, 40))), -1.0, "", (0, 0, 0)),
            mtile("Songs", "8 tracks", 2, None, -1.0, "", (80, 170, 140))];
        let lists = vec![mtile("Road Trip", "24 tracks", 2, None, -1.0, "", (140, 90, 230)), mtile("New Playlist", "", 4, None, -1.0, "", (58, 134, 255))];
        let browse = vec![mtile("Internal Storage", "56.5 GB free", 6, None, -1.0, "", (100, 110, 140)), mtile("SD2", "60.2 GB free", 6, None, -1.0, "", (100, 110, 140)),
            mtile("NAS", "Network Share", 6, None, -1.0, "", (100, 110, 140)), mtile("Jellyfin", "Signed in as eli", 3, None, -1.0, "", (110, 80, 200))];
        let _ = (music, lists);
        let shelves = vec![shelf("Continue Watching", cont, 2), shelf("Videos", videos, 0), shelf("Browse", browse, 1)];
        ui.set_media_shelves(ModelRc::from(Rc::new(VecModel::from(shelves))));
        ui.set_media_shelf_indices(ModelRc::from(Rc::new(VecModel::from(vec![0, 2, 0]))));
        ui.set_media_section(1);
        ui.set_media_shelf_index(0);
        ui.set_media_screen(0);
        ui.set_media_hero_title("Big Buck Bunny".into()); ui.set_media_hero_subtitle("Resume from 4:12".into());
        ui.set_media_backdrop(cover(24, 14, (230, 140, 60))); ui.set_media_has_backdrop(true);
        ui.set_media_np_active(false);
        ui.set_app_hints(hints(&[("face", 0, "Play"), ("face", 1, "Back"), ("face", 3, "Options")]));
    })));
    scenes.push(("media_home_music", Box::new(|ui| {
        let shelf = |title: &str, tiles: Vec<MediaTile>, shape: i32| MediaShelf { title: title.into(), tiles: ModelRc::from(Rc::new(VecModel::from(tiles))), shape };
        let albums = vec![mtile("Shuffle All", "", 9, None, -1.0, "", (58, 134, 255)),
            mtile("Jazz Lounge", "Kevin MacLeod • 8 tracks", 2, Some(cover(240, 240, (200, 60, 160))), -1.0, "", (0, 0, 0)),
            mtile("Cinematic Moods", "Kevin MacLeod • 12 tracks", 2, None, -1.0, "", (210, 170, 60)),
            mtile("In Rainbows", "Radiohead • 10 tracks", 2, Some(cover(240, 240, (240, 120, 40))), -1.0, "", (0, 0, 0))];
        let lists = vec![mtile("Road Trip", "24 tracks", 2, None, -1.0, "", (140, 90, 230)), mtile("New Playlist", "", 4, None, -1.0, "", (58, 134, 255))];
        ui.set_media_shelves(ModelRc::from(Rc::new(VecModel::from(vec![shelf("Albums", albums, 1), shelf("Playlists", lists, 1)]))));
        ui.set_media_shelf_indices(ModelRc::from(Rc::new(VecModel::from(vec![1, 0]))));
        ui.set_media_section(0);
        ui.set_media_shelf_index(0);
        ui.set_media_hero_title("Jazz Lounge".into()); ui.set_media_hero_subtitle("Kevin MacLeod • 8 tracks".into());
        ui.set_media_backdrop(cover(24, 24, (200, 60, 160)));
        ui.set_media_np_active(true); ui.set_media_np_playing(true);
        ui.set_media_np_title("Airport Lounge".into()); ui.set_media_np_artist("Kevin MacLeod".into());
        ui.set_media_np_has_art(true); ui.set_media_np_art(cover(64, 64, (200, 60, 160)));
        ui.set_media_np_position(83.0); ui.set_media_np_duration(320.0);
        ui.set_app_hints(hints(&[("face", 0, "Open"), ("face", 1, "Back"), ("face", 3, "Options"), ("face", 2, "Now Playing")]));
    })));
    scenes.push(("media_grid", Box::new(|ui| {
        let t: Vec<MediaTile> = (0..14).map(|i| mtile(&format!("Episode {}", i + 1), "", 0, if i % 3 == 0 { Some(cover(200, 300, (60 + i * 10, 120, 200))) } else { None },
            if i == 2 { 0.5 } else { -1.0 }, "", ((40 + i * 15) as u8, 120, 200))).collect();
        ui.set_media_grid_tiles(ModelRc::from(Rc::new(VecModel::from(t))));
        ui.set_media_grid_title("AGENT 327".into()); ui.set_media_grid_shape(0); ui.set_media_grid_index(6);
        ui.set_media_hero_title("Episode 7".into()); ui.set_media_hero_subtitle("Agent 327 • 3:52".into());
        ui.set_media_screen(1);
    })));
    scenes.push(("media_album", Box::new(|ui| {
        ui.set_media_album_title("Jazz Lounge".into()); ui.set_media_album_artist("Kevin MacLeod".into()); ui.set_media_album_meta("8 tracks".into());
        ui.set_media_album_art(cover(300, 300, (200, 60, 160))); ui.set_media_album_has_art(true);
        ui.set_media_album_tracks(tracks(&[("1", "Airport Lounge", "5:20", true), ("2", "Backbay Lounge", "3:27", false), ("3", "Bossa Antigua", "4:58", false),
            ("4", "Cool Vibes", "3:44", false), ("5", "Jazz Brunch", "3:31", false), ("6", "Laid Back Guitars", "1:44", false),
            ("7", "Lobby Time", "3:57", false), ("8", "Smooth Lovin", "3:21", false)]));
        ui.set_media_album_index(3); ui.set_media_screen(2);
        ui.set_media_backdrop(cover(24, 24, (200, 60, 160)));
        ui.set_app_hints(hints(&[("face", 0, "Play"), ("face", 1, "Back"), ("face", 3, "Options"), ("face", 2, "Now Playing")]));
    })));
    scenes.push(("media_album_buttons", Box::new(|ui| { ui.set_media_album_index(-2); ui.set_media_album_has_art(false); })));
    scenes.push(("media_np", Box::new(|ui| {
        ui.set_media_screen(3);
        ui.set_media_np_album("Jazz Lounge • FLAC • 24-bit • 96 kHz".into());
        ui.set_media_np_elapsed("1:23".into()); ui.set_media_np_total("5:20".into());
        ui.set_media_np_shuffle(true); ui.set_media_np_repeat(2);
        ui.set_media_np_queue(tracks(&[("1", "Airport Lounge", "", true), ("2", "Backbay Lounge", "", false), ("3", "Bossa Antigua", "", false),
            ("4", "Cool Vibes", "", false), ("5", "Jazz Brunch", "", false), ("6", "Laid Back Guitars", "", false)]));
        ui.set_media_np_zone(1); ui.set_media_np_control(2); ui.set_media_np_queue_index(0);
        ui.set_app_hints(hints(&[("face", 0, "Select"), ("face", 1, "Back"), ("face", 3, "Screen Off")]));
    })));
    scenes.push(("media_np_paused", Box::new(|ui| { ui.set_media_np_zone(1); ui.set_media_np_has_art(false); ui.set_media_np_playing(false); })));
    scenes.push(("media_empty", Box::new(|ui| {
        ui.set_media_screen(0); ui.set_media_np_active(false); ui.set_media_has_backdrop(false);
        ui.set_media_shelves(ModelRc::default()); ui.set_media_hero_title("".into()); ui.set_media_hero_subtitle("".into());
        ui.set_media_empty_text(ui.get_i18n_strings().row_data(1121).unwrap_or_default());
    })));
    /* Settings tab (2026-10-14): rows like Moonlight's Stream Settings. */
    scenes.push(("media_settings", Box::new(|ui| {
        ui.set_media_shelves(ModelRc::default());
        ui.set_media_section(2); ui.set_media_screen(0);
        ui.set_media_has_backdrop(false); ui.set_media_np_active(false);
        ui.set_lighting_supported(true);
        ui.set_media_rgb_follow(true); ui.set_media_rgb_keep(false); ui.set_media_settings_index(0);
        ui.set_app_hints(hints(&[("face", 0, "Select"), ("face", 1, "Back")]));
    })));
    /* Files (2026-10-12): two-pane commander. */
    fn frow(name: &str, detail: &str, icon: i32, marked: bool, usage: f32) -> FileRow {
        FileRow { name: name.into(), detail: detail.into(), icon, marked, dim: false, usage }
    }
    scenes.push(("files_commander", Box::new(|ui| {
        ui.set_media_open(false); ui.set_files_open(true); ui.set_app_kicker("FILES".into()); ui.set_details_shell_title("Files".into()); ui.set_settings_view(58);
        ui.set_details_active(true); ui.set_settings_open(true);
        ui.set_select_face_position(2); ui.set_back_face_position(1); ui.set_context_face_position(0); ui.set_details_face_position(3);
        let rows = |v: Vec<FileRow>| ModelRc::from(Rc::new(VecModel::from(v)));
        ui.set_files_left(FilePane { title: "Places".into(), path: "".into(), places: true, loading: false, index: 0, rows: rows(vec![
            frow("Internal Storage", "56.5 GB free / 58.1 GB", 6, false, 0.03), frow("SD2", "60.2 GB free / 62.5 GB", 7, false, 0.04),
            frow("KINGSTON", "3.1 GB free / 15.9 GB", 8, false, 0.81), frow("NAS", "//nas/media • Connect to open it", 9, false, -1.0),
            frow("Add Share", "", 10, false, -1.0)]) });
        ui.set_files_right(FilePane { title: "Internal Storage".into(), path: "/roms/nes".into(), places: false, loading: false, index: 3, rows: rows(vec![
            frow("..", "", 11, false, -1.0), frow("Hacks", "", 0, false, -1.0), frow("Concentration Room.nes", "40 KB", 12, true, -1.0),
            frow("Lawn Mower.nes", "24 KB", 12, true, -1.0), frow("RHDE - Furniture Fight.nes", "256 KB", 12, false, -1.0),
            frow("Russian Roulette.nes", "40 KB", 12, false, -1.0), frow("Super Tilt Bro.nes", "512 KB", 12, false, -1.0),
            frow("Thwaite.nes", "40 KB", 12, false, -1.0), frow("cover.png", "312 KB", 2, false, -1.0), frow("notes.txt", "2 KB", 1, false, -1.0),
            frow("Zap Ruder.nes", "40 KB", 12, false, -1.0), frow("robotfindskitten.nes", "24 KB", 12, false, -1.0)]) });
        ui.set_files_active(1);
        ui.set_files_status("Lawn Mower.nes • 24 KB • 12 October 2026 14:02".into());
        ui.set_files_status_right("2 selected • 56.5 GB free".into());
        ui.set_files_progress(-1.0); ui.set_files_notice("".into()); ui.set_files_sort_label("Sort: Name".into());
        ui.set_app_hints(hints(&[("face", 1, "Back"), ("face", 2, "Mark"), ("face", 3, "Actions"), ("select", 0, "Sort")]));
    })));
    scenes.push(("files_copying", Box::new(|ui| {
        ui.set_files_active(0);
        ui.set_files_progress(0.46); ui.set_files_progress_label("Copying Concentration Room.nes (1/2)".into());
    })));
    scenes.push(("files_actions", Box::new(|ui| {
        ui.set_files_progress(-1.0); ui.set_files_active(1);
        let o: Vec<SettingsChoiceEntry> = [1139, 1140, 1275, 618, 523, 612, 1144]
            .iter().map(|&k| SettingsChoiceEntry { value: k.to_string().into(), label: s(ui, k).into() }).collect();
        ui.set_settings_choice_title("2 selected".into());
        ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o))));
        ui.set_settings_choice_index(2); ui.set_settings_choice_scroll(0); ui.set_settings_choice_open(true);
    })));
    /* Add to Library (2026-10-14): the one question of a PS1 .m3u + .chd set, then the job. */
    scenes.push(("files_import_ask", Box::new(|ui| {
        let o: Vec<SettingsChoiceEntry> = ["Dreamcast", "Neo Geo CD", "PC Engine", "PC Engine CD", "PlayStation", "Sega CD", "Sega Saturn"]
            .iter().map(|l| SettingsChoiceEntry { value: (*l).into(), label: (*l).into() }).collect();
        ui.set_settings_choice_title(s(ui, 1276).replace("{0}", "Homebrew Disc Set.m3u (+3)").into());
        ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(o))));
        ui.set_settings_choice_index(4); ui.set_settings_choice_scroll(0); ui.set_settings_choice_open(true);
    })));
    scenes.push(("files_importing", Box::new(|ui| {
        ui.set_files_progress(0.37); ui.set_files_progress_label(s(ui, 1277).replace("{0}", "Homebrew Disc Set").into());
    })));
    scenes.push(("files_closed", Box::new(|ui| { ui.set_files_open(false); ui.set_settings_open(false); ui.set_app_hints(ModelRc::default()); })));
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
