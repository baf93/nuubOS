/*
 * Home carousels and Game Library browsing (EPIC-001 Home, EPIC-011).
 *
 * nuubos-libraryd owns the catalog, play history, favorites, collections
 * and the application registry; this module only subscribes to its
 * snapshot, turns it into HomeCard models and sends user commands back.
 * Pixel geometry lives in home.slint.
 *
 * Covers are decoded off the UI thread, downscaled once to the card height
 * in physical pixels and given rounded corners (the software renderer
 * does not clip images to border-radius). Only covers near the visible
 * part of a library grid are kept in memory.
 */

use crate::{on_game_session, play_ui_sound, tr, tr_arg, HomeCard, HomeWindow};
use slint::{Color, ComponentHandle, Image, Model, ModelRc, SharedPixelBuffer, Rgba8Pixel, Timer, VecModel};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet, VecDeque};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const LIBRARY_SOCKET: &str = "/run/nuubos/libraryd.sock";
const EMULATION_SOCKET: &str = "/run/nuubos/emud.sock";

/* A game session runs (nuubos-emud): RetroArch is fullscreen above Home and
 * owns the controllers through the nuubOS game gamepads. */
pub static GAME_RUNNING: AtomicBool = AtomicBool::new(false);
/* Decoded covers kept in memory (LRU). */
const COVER_CACHE_LIMIT: usize = 64;
/* Grid rows above/below the visible ones whose covers are loaded. */
const GRID_COVER_MARGIN_ROWS: i32 = 1;
const SHELF_ASPECT: f32 = 1.6;
const APP_ASPECT: f32 = 1.0;
const NOTICE_MS: u64 = 2500;

const KIND_GAME: i32 = 0;
const KIND_FAVORITES: i32 = 1;
const KIND_COLLECTION: i32 = 2;
const KIND_SYSTEM: i32 = 3;
const KIND_APP: i32 = 4;

#[derive(Clone, Default)]
struct LibGame {
    id: String,
    system: String,
    title: String,
    cover: String,
    aspect: f32,
    last: i64,
    time: i64,
    available: bool,
    favorite: bool,
}

#[derive(Clone, Default)]
struct LibSystem {
    id: String,
    name: String,
    count: i64,
    aspect: f32,
    color: String,
    icon: String,
}

#[derive(Clone, Default)]
struct LibCollection {
    id: String,
    name: String,
    count: i64,
}

#[derive(Clone, Default)]
struct LibApp {
    id: String,
    name: String,
    icon: String,
}

#[derive(Clone, Default)]
struct LibSnapshot {
    user: String,
    scanning: bool,
    recent: Vec<LibGame>,
    collections: Vec<LibCollection>,
    systems: Vec<LibSystem>,
    apps: Vec<LibApp>,
}

enum CoverState {
    Loading,
    Ready(Image, f32),
    Failed,
}

#[derive(Default)]
struct LibraryState {
    snap: LibSnapshot,
    /* Open grid: scope, title, aspect and its games. */
    grid_scope: String,
    grid: Vec<LibGame>,
    covers: HashMap<String, CoverState>,
    lru: VecDeque<String>,
    notice_serial: u64,
}

thread_local! {
    static STATE: RefCell<LibraryState> = RefCell::new(LibraryState::default());
}

static COVER_QUEUE: OnceLock<Mutex<mpsc::Sender<(String, u32, f32)>>> = OnceLock::new();

fn library_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(LIBRARY_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let multi = command.starts_with("GAMES") || command == "STATUS";
    let mut reply = String::new();
    let mut reader = BufReader::new(stream);
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line)? == 0 {
            break;
        }
        let done = !multi || line.trim_end() == "end=1" || line.starts_with("ERR");
        reply.push_str(&line);
        if done {
            break;
        }
    }
    Ok(reply)
}

fn parse_game(value: &str) -> Option<LibGame> {
    let f: Vec<&str> = value.split('\t').collect();
    if f.len() < 9 {
        return None;
    }
    Some(LibGame {
        id: f[0].into(),
        system: f[1].into(),
        title: f[2].into(),
        cover: f[3].into(),
        aspect: f[4].parse::<f32>().unwrap_or(1000.0) / 1000.0,
        last: f[5].parse().unwrap_or(0),
        time: f[6].parse().unwrap_or(0),
        available: f[7] == "1",
        favorite: f[8] == "1",
    })
}

fn parse_snapshot(reply: &str) -> LibSnapshot {
    let mut s = LibSnapshot::default();
    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "user" => s.user = value.into(),
            "scanning" => s.scanning = value == "1",
            "recent" => s.recent.extend(parse_game(value)),
            "collection" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 3 {
                    s.collections.push(LibCollection {
                        id: f[0].into(),
                        name: f[1].into(),
                        count: f[2].parse().unwrap_or(0),
                    });
                }
            }
            "system" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 5 {
                    s.systems.push(LibSystem {
                        id: f[0].into(),
                        name: f[1].into(),
                        count: f[2].parse().unwrap_or(0),
                        aspect: f[3].parse::<f32>().unwrap_or(1000.0) / 1000.0,
                        color: f[4].into(),
                        icon: f.get(5).map(|v| v.to_string()).unwrap_or_default(),
                    });
                }
            }
            "app" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 3 {
                    s.apps.push(LibApp { id: f[0].into(), name: f[1].into(), icon: f[2].into() });
                }
            }
            _ => {}
        }
    }
    s
}

fn parse_games(reply: &str) -> Vec<LibGame> {
    reply
        .lines()
        .filter_map(|l| l.strip_prefix("game="))
        .filter_map(parse_game)
        .collect()
}

fn parse_color(hex: &str) -> Color {
    let v = u32::from_str_radix(hex.trim_start_matches('#'), 16).unwrap_or(0x2a3140);
    Color::from_rgb_u8((v >> 16) as u8, (v >> 8) as u8, v as u8)
}

/* ---------------------------------------------------------------- */
/* Localized detail lines                                           */
/* ---------------------------------------------------------------- */

fn count_label(ui: &HomeWindow, n: i64) -> String {
    if n == 1 {
        tr(ui, 401, "1 game")
    } else {
        tr_arg(ui, 400, "{0} games", &n.to_string())
    }
}

fn duration_label(ui: &HomeWindow, seconds: i64) -> String {
    let minutes = (seconds / 60).max(1);
    if minutes >= 60 {
        tr(ui, 407, "{0} h {1} min")
            .replace("{0}", &(minutes / 60).to_string())
            .replace("{1}", &format!("{:02}", minutes % 60))
    } else {
        tr_arg(ui, 408, "{0} min", &minutes.to_string())
    }
}

/* Local day boundaries come from the top-bar clock (statusd, local time),
 * so "today"/"yesterday" follow the user's timezone without a tz library. */
fn last_played_label(ui: &HomeWindow, last: i64) -> String {
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    let elapsed = (now - last).max(0);
    let since_midnight = ui
        .get_topbar_time()
        .split_once(':')
        .and_then(|(h, m)| Some(h.trim().parse::<i64>().ok()? * 3600 + m.trim().parse::<i64>().ok()? * 60))
        .unwrap_or(elapsed.min(86399));
    let day = if elapsed <= since_midnight {
        tr(ui, 402, "today")
    } else {
        let days = (elapsed - since_midnight + 86399) / 86400;
        if days <= 1 {
            tr(ui, 403, "yesterday")
        } else {
            tr_arg(ui, 404, "{0} days ago", &days.to_string())
        }
    };
    tr_arg(ui, 409, "Last played {0}", &day)
}

fn game_detail(ui: &HomeWindow, g: &LibGame, system_name: &str) -> String {
    let mut parts: Vec<String> = Vec::new();
    if !g.available {
        parts.push(tr(ui, 205, "Unavailable"));
    }
    parts.push(system_name.to_owned());
    if g.last > 0 {
        parts.push(last_played_label(ui, g.last));
        if g.time > 0 {
            parts.push(tr_arg(ui, 406, "Played {0}", &duration_label(ui, g.time)));
        }
    } else {
        parts.push(tr(ui, 405, "Never played"));
    }
    parts.join("  •  ")
}

/* ---------------------------------------------------------------- */
/* Cover loading                                                    */
/* ---------------------------------------------------------------- */

fn cover_key(path: &str, height: u32) -> String {
    format!("{}@{}", path, height)
}

/* Decode height in physical pixels, bucketed so a resize does not
 * re-decode everything for a few pixels. */
fn cover_target(ui: &HomeWindow, small: bool) -> u32 {
    let logical = if small { ui.get_home_cover_h() * 0.5 } else { ui.get_home_cover_h() };
    let px = (logical * ui.window().scale_factor()).max(64.0) as u32;
    px.div_ceil(32) * 32
}

/* Area-average downscale to `target` rows + anti-aliased rounded corners. */
fn shape_cover(src: &Image, target: u32, radius_px: f32) -> Option<(SharedPixelBuffer<Rgba8Pixel>, f32)> {
    let b = src.to_rgba8_premultiplied()?;
    let (w, h) = (b.width(), b.height());
    if w == 0 || h == 0 {
        return None;
    }
    let aspect = w as f32 / h as f32;
    let oh = target.min(h).max(1);
    let ow = ((w as f32 * oh as f32 / h as f32).round() as u32).max(1);
    let px = b.as_slice();
    let mut out = SharedPixelBuffer::<Rgba8Pixel>::new(ow, oh);
    let r = radius_px.max(1.0).min(ow.min(oh) as f32 / 2.0);
    for (i, o) in out.make_mut_slice().iter_mut().enumerate() {
        let (x, y) = (i as u32 % ow, i as u32 / ow);
        let (x0, x1) = (x * w / ow, ((x + 1) * w / ow).max(x * w / ow + 1).min(w));
        let (y0, y1) = (y * h / oh, ((y + 1) * h / oh).max(y * h / oh + 1).min(h));
        let mut acc = [0u32; 4];
        for sy in y0..y1 {
            for sx in x0..x1 {
                let p = px[(sy * w + sx) as usize];
                acc[0] += p.r as u32;
                acc[1] += p.g as u32;
                acc[2] += p.b as u32;
                acc[3] += p.a as u32;
            }
        }
        let cnt = ((x1 - x0) * (y1 - y0)) as f32;
        /* Distance to the nearest corner centre decides coverage. */
        let (fx, fy) = (x as f32 + 0.5, y as f32 + 0.5);
        let cx = if fx < r { r } else if fx > ow as f32 - r { ow as f32 - r } else { fx };
        let cy = if fy < r { r } else if fy > oh as f32 - r { oh as f32 - r } else { fy };
        let d = ((fx - cx).powi(2) + (fy - cy).powi(2)).sqrt();
        let cov = (r - d + 0.5).clamp(0.0, 1.0) / cnt;
        let c = |v: u32| (v as f32 * cov + 0.5) as u8;
        *o = Rgba8Pixel { r: c(acc[0]), g: c(acc[1]), b: c(acc[2]), a: c(acc[3]) };
    }
    Some((out, aspect))
}

pub fn start_cover_loader(ui: &HomeWindow) {
    let (tx, rx) = mpsc::channel::<(String, u32, f32)>();
    let _ = COVER_QUEUE.set(Mutex::new(tx));
    let weak = ui.as_weak();
    thread::spawn(move || {
        for (path, target, radius) in rx {
            let img = Image::load_from_path(Path::new(&path)).ok();
            let shaped = img.as_ref().and_then(|i| shape_cover(i, target, radius));
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                let Some(ui) = weak.upgrade() else { return };
                let key = cover_key(&path, target);
                STATE.with(|st| {
                    let mut st = st.borrow_mut();
                    let entry = match shaped {
                        Some((buf, aspect)) => CoverState::Ready(Image::from_rgba8_premultiplied(buf), aspect),
                        None => CoverState::Failed,
                    };
                    st.covers.insert(key.clone(), entry);
                    st.lru.retain(|k| k != &key);
                    st.lru.push_back(key);
                });
                cover_arrived(&ui, &path);
            });
        }
    });
}

/* Cached cover or None; queues a decode the first time a path is seen. */
fn request_cover(ui: &HomeWindow, path: &str, small: bool) -> Option<(Image, f32)> {
    if path.is_empty() {
        return None;
    }
    let target = cover_target(ui, small);
    let key = cover_key(path, target);
    let scale = ui.window().scale_factor();
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        match st.covers.get(&key) {
            Some(CoverState::Ready(img, aspect)) => {
                let out = Some((img.clone(), *aspect));
                st.lru.retain(|k| k != &key);
                st.lru.push_back(key);
                out
            }
            Some(CoverState::Loading) | Some(CoverState::Failed) => None,
            None => {
                st.covers.insert(key, CoverState::Loading);
                let radius = (6.0 * scale).max(target as f32 * 0.045);
                if let Some(q) = COVER_QUEUE.get() {
                    let _ = q.lock().unwrap().send((path.to_owned(), target, radius));
                }
                None
            }
        }
    })
}

/* Drop decoded covers beyond the cache limit, oldest first, except the
 * ones currently on screen. */
fn trim_cover_cache(keep: &HashSet<String>) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        let mut scan = st.lru.len();
        while st.lru.len() > COVER_CACHE_LIMIT && scan > 0 {
            scan -= 1;
            let Some(key) = st.lru.pop_front() else { break };
            if keep.contains(&key) {
                st.lru.push_back(key);
                continue;
            }
            st.covers.remove(&key);
        }
    });
}

/* ---------------------------------------------------------------- */
/* Models                                                           */
/* ---------------------------------------------------------------- */

fn system_lookup(snap: &LibSnapshot, id: &str) -> (String, f32, Color) {
    snap.systems
        .iter()
        .find(|s| s.id == id)
        .map(|s| (s.name.clone(), s.aspect, parse_color(&s.color)))
        .unwrap_or_else(|| (id.to_uppercase(), 1.0, parse_color("#2a3140")))
}

fn game_card(ui: &HomeWindow, snap: &LibSnapshot, g: &LibGame, load: bool) -> HomeCard {
    let (system_name, system_aspect, accent) = system_lookup(snap, &g.system);
    let cover = if load { request_cover(ui, &g.cover, false) } else { None };
    let aspect = match &cover {
        Some((_, a)) => a.clamp(0.5, 1.8),
        None => if g.aspect > 0.0 { g.aspect } else { system_aspect },
    };
    HomeCard {
        key: g.id.clone().into(),
        kind: KIND_GAME,
        title: g.title.clone().into(),
        detail: game_detail(ui, g, &system_name).into(),
        system_name: system_name.into(),
        cover_path: g.cover.clone().into(),
        has_cover: cover.is_some(),
        cover: cover.map(|(i, _)| i).unwrap_or_default(),
        aspect,
        x_units: 0.0,
        accent,
        available: g.available,
        favorite: g.favorite,
    }
}

fn with_x_units(mut cards: Vec<HomeCard>) -> Vec<HomeCard> {
    let mut units = 0.0;
    for c in cards.iter_mut() {
        c.x_units = units;
        units += c.aspect;
    }
    cards
}

fn set_model(current: ModelRc<HomeCard>, cards: Vec<HomeCard>) -> Option<ModelRc<HomeCard>> {
    /* Same length: update rows in place so Slint keeps item state. */
    if current.row_count() == cards.len() {
        for (i, c) in cards.into_iter().enumerate() {
            current.set_row_data(i, c);
        }
        return None;
    }
    Some(ModelRc::from(Rc::new(VecModel::from(cards))))
}

fn build_home(ui: &HomeWindow) {
    let snap = STATE.with(|st| st.borrow().snap.clone());

    let recent = with_x_units(snap.recent.iter().map(|g| game_card(ui, &snap, g, true)).collect());

    let mut shelf = Vec::new();
    if let Some(fav) = snap.collections.iter().find(|c| c.id == "favorites") {
        if fav.count > 0 {
            shelf.push(HomeCard {
                key: "favorites".into(),
                kind: KIND_FAVORITES,
                title: tr(ui, 399, "Favorites").into(),
                detail: count_label(ui, fav.count).into(),
                aspect: SHELF_ASPECT,
                accent: parse_color("#7a5a14"),
                available: true,
                ..Default::default()
            });
        }
    }
    for c in snap.collections.iter().filter(|c| c.id != "favorites") {
        shelf.push(HomeCard {
            key: c.id.clone().into(),
            kind: KIND_COLLECTION,
            title: c.name.clone().into(),
            detail: count_label(ui, c.count).into(),
            aspect: SHELF_ASPECT,
            accent: parse_color("#2d3f66"),
            available: true,
            ..Default::default()
        });
    }
    for s in &snap.systems {
        let icon = request_cover(ui, &s.icon, true);
        shelf.push(HomeCard {
            key: s.id.clone().into(),
            kind: KIND_SYSTEM,
            title: s.name.clone().into(),
            detail: count_label(ui, s.count).into(),
            cover_path: s.icon.clone().into(),
            has_cover: icon.is_some(),
            cover: icon.map(|(i, _)| i).unwrap_or_default(),
            aspect: SHELF_ASPECT,
            accent: parse_color(&s.color),
            available: true,
            ..Default::default()
        });
    }
    let shelf = with_x_units(shelf);

    let apps = with_x_units(
        snap.apps
            .iter()
            .map(|a| {
                let icon = request_cover(ui, &a.icon, true);
                HomeCard {
                    key: a.id.clone().into(),
                    kind: KIND_APP,
                    title: a.name.clone().into(),
                    cover_path: a.icon.clone().into(),
                    has_cover: icon.is_some(),
                    cover: icon.map(|(i, _)| i).unwrap_or_default(),
                    aspect: APP_ASPECT,
                    accent: parse_color("#1a1d22"),
                    available: true,
                    ..Default::default()
                }
            })
            .collect(),
    );

    let counts = [recent.len() as i32, shelf.len() as i32, apps.len() as i32];
    /* A newly played game becomes the first recent: focus it, and bring the
     * focus back to Recently Played when that row appears. */
    let old_recent = ui.get_home_recent();
    let old_first = old_recent.row_data(0).map(|c| c.key);
    let new_first = recent.first().map(|c| c.key.clone());
    if old_first != new_first && new_first.is_some() {
        ui.set_home_recent_index(0);
        if old_recent.row_count() == 0 {
            ui.set_home_row(0);
        }
    }
    if let Some(m) = set_model(ui.get_home_recent(), recent) {
        ui.set_home_recent(m);
    }
    if let Some(m) = set_model(ui.get_home_shelf(), shelf) {
        ui.set_home_shelf(m);
    }
    if let Some(m) = set_model(ui.get_home_apps(), apps) {
        ui.set_home_apps(m);
    }
    ui.set_home_recent_index(ui.get_home_recent_index().clamp(0, (counts[0] - 1).max(0)));
    ui.set_home_shelf_index(ui.get_home_shelf_index().clamp(0, (counts[1] - 1).max(0)));
    ui.set_home_apps_index(ui.get_home_apps_index().clamp(0, (counts[2] - 1).max(0)));
    let row = ui.get_home_row();
    if counts[row.clamp(0, 2) as usize] == 0 {
        if let Some(r) = (0..3).find(|r| counts[*r as usize] > 0) {
            ui.set_home_row(r);
        }
    }
    ui.set_library_scanning(snap.scanning);
}

/* Grid rows whose covers should be decoded now. */
fn grid_window(ui: &HomeWindow) -> (usize, usize) {
    let cols = ui.get_library_columns().max(1);
    let rows = ui.get_library_visible_rows().max(1);
    let focus_row = ui.get_library_index().max(0) / cols;
    let first = (focus_row - GRID_COVER_MARGIN_ROWS).max(0);
    let last = focus_row + rows + GRID_COVER_MARGIN_ROWS;
    ((first * cols) as usize, ((last + 1) * cols) as usize)
}

fn build_grid(ui: &HomeWindow, reset: bool) {
    let (snap, games) = STATE.with(|st| {
        let st = st.borrow();
        (st.snap.clone(), st.grid.clone())
    });
    let (lo, hi) = grid_window(ui);
    let cards: Vec<HomeCard> = games
        .iter()
        .enumerate()
        .map(|(i, g)| game_card(ui, &snap, g, i >= lo && i < hi))
        .collect();
    let n = cards.len() as i32;
    if reset {
        ui.set_library_games(ModelRc::from(Rc::new(VecModel::from(cards))));
    } else if let Some(m) = set_model(ui.get_library_games(), cards) {
        ui.set_library_games(m);
    }
    ui.set_library_index(ui.get_library_index().clamp(0, (n - 1).max(0)));
    ui.set_library_detail(count_label(ui, n as i64).into());
    keep_visible_covers(ui);
}

/* Covers on screen (Home rows + grid window) survive cache trimming. */
fn keep_visible_covers(ui: &HomeWindow) {
    let big = cover_target(ui, false);
    let small = cover_target(ui, true);
    let mut keep = HashSet::new();
    for i in 0..ui.get_home_recent().row_count() {
        if let Some(c) = ui.get_home_recent().row_data(i) {
            keep.insert(cover_key(&c.cover_path, big));
        }
    }
    for model in [ui.get_home_apps(), ui.get_home_shelf()] {
        for i in 0..model.row_count() {
            if let Some(c) = model.row_data(i) {
                keep.insert(cover_key(&c.cover_path, small));
            }
        }
    }
    if ui.get_library_open() {
        let (lo, hi) = grid_window(ui);
        let model = ui.get_library_games();
        for i in lo..hi.min(model.row_count()) {
            if let Some(c) = model.row_data(i) {
                keep.insert(cover_key(&c.cover_path, big));
            }
        }
    }
    trim_cover_cache(&keep);
}

/* A decoded cover arrived: refresh the cards that show it. Home rows are
 * rebuilt (their x-units depend on the cover shape); grid rows in place. */
fn cover_arrived(ui: &HomeWindow, path: &str) {
    let in_home = [ui.get_home_recent(), ui.get_home_shelf(), ui.get_home_apps()]
        .iter()
        .any(|m| (0..m.row_count()).any(|i| m.row_data(i).is_some_and(|c| c.cover_path == path)));
    if in_home {
        build_home(ui);
    }
    if ui.get_library_open() {
        refresh_grid_window(ui);
    }
}

/* Load covers entering the grid window, release the ones leaving it. */
fn refresh_grid_window(ui: &HomeWindow) {
    let (lo, hi) = grid_window(ui);
    let snap = STATE.with(|st| st.borrow().snap.clone());
    let model = ui.get_library_games();
    let games = STATE.with(|st| st.borrow().grid.clone());
    for (i, g) in games.iter().enumerate().take(model.row_count()) {
        let Some(cur) = model.row_data(i) else { continue };
        let inside = i >= lo && i < hi;
        if inside {
            if !cur.has_cover && !g.cover.is_empty() {
                let card = game_card(ui, &snap, g, true);
                if card.has_cover {
                    model.set_row_data(i, card);
                }
            }
        } else if cur.has_cover {
            model.set_row_data(i, game_card(ui, &snap, g, false));
        }
    }
    keep_visible_covers(ui);
}

/* ---------------------------------------------------------------- */
/* Service snapshot                                                 */
/* ---------------------------------------------------------------- */

fn apply_snapshot(ui: &HomeWindow, snap: LibSnapshot) {
    let user_changed = STATE.with(|st| {
        let mut st = st.borrow_mut();
        let changed = st.snap.user != snap.user;
        st.snap = snap;
        changed
    });
    if user_changed {
        /* Another user's Home starts at the top. */
        ui.set_home_row(0);
        ui.set_home_recent_index(0);
        ui.set_home_shelf_index(0);
        ui.set_home_apps_index(0);
        close_grid(ui);
    }
    build_home(ui);
    let scope = STATE.with(|st| st.borrow().grid_scope.clone());
    if ui.get_library_open() && !scope.is_empty() {
        fetch_grid(ui, scope, false);
    }
}

/* Rebuild detail lines after a language change. */
pub fn relocalize(ui: &HomeWindow) {
    build_home(ui);
    if ui.get_library_open() {
        build_grid(ui, false);
    }
}

/* ---------------------------------------------------------------- */
/* Game sessions (EPIC-013): launching belongs to nuubos-emud        */
/* ---------------------------------------------------------------- */

fn emulation_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(EMULATION_SOCKET)?;
    /* A launch resolves the game and starts RetroArch before replying. */
    stream.set_read_timeout(Some(Duration::from_secs(8)))?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply)?;
    Ok(reply)
}

fn launch_game(ui: &HomeWindow, card: &HomeCard) {
    if !card.available {
        show_notice(ui, tr(ui, 434, "This game is not available"));
        return;
    }
    if GAME_RUNNING.load(Ordering::SeqCst) {
        return;
    }
    let command = format!("LAUNCH\t{}", card.key);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reason = match emulation_command(&command) {
            Ok(reply) if reply.trim() == "OK" => return,
            Ok(reply) => reply.trim().strip_prefix("ERR ").unwrap_or("").to_owned(),
            Err(error) => {
                eprintln!("home: launch failed={error}");
                String::new()
            }
        };
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let text = match reason.as_str() {
                /* A double press while the game is starting. */
                "busy" => return,
                "no-core" => tr(&ui, 433, "No emulator is installed for this system"),
                "unavailable" | "game" => tr(&ui, 434, "This game is not available"),
                _ => tr(&ui, 431, "The game could not be started"),
            };
            show_notice(&ui, text);
        });
    });
}

/* Follows the session state; Home reacts to start/end in on_game_session. */
pub fn start_game_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(EMULATION_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut running = false;
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    if let Some(state) = line.strip_prefix("state=") {
                        /* "exiting" still covers Home until RetroArch is gone. */
                        running = state != "idle";
                        continue;
                    }
                    if line != "end=1" || GAME_RUNNING.swap(running, Ordering::SeqCst) == running {
                        continue;
                    }
                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            on_game_session(&ui, running);
                        }
                    });
                }
            }
        }
        /* emud restarted or not up yet: no session survives it. */
        if GAME_RUNNING.swap(false, Ordering::SeqCst) {
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

pub fn start_library_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        match UnixStream::connect(LIBRARY_SOCKET) {
            Ok(mut stream) => {
                if stream.write_all(b"SUBSCRIBE\n").is_err() {
                    thread::sleep(Duration::from_millis(500));
                    continue;
                }
                let mut block = String::new();
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    block.push_str(&line);
                    block.push('\n');
                    if line == "end=1" {
                        let snap = parse_snapshot(&block);
                        block.clear();
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                apply_snapshot(&ui, snap);
                            }
                        });
                    }
                }
            }
            Err(_) => thread::sleep(Duration::from_millis(500)),
        }
        /* libraryd restarted or not up yet. */
        thread::sleep(Duration::from_millis(500));
    });
}

/* ---------------------------------------------------------------- */
/* Grid                                                             */
/* ---------------------------------------------------------------- */

fn fetch_grid(ui: &HomeWindow, scope: String, reset: bool) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(reply) = library_command(&format!("GAMES\t{}", scope)) else { return };
        let games = parse_games(&reply);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let current = STATE.with(|st| st.borrow().grid_scope.clone());
            if current != scope {
                return;
            }
            STATE.with(|st| st.borrow_mut().grid = games);
            build_grid(&ui, reset);
        });
    });
}

fn open_grid(ui: &HomeWindow, card: &HomeCard) {
    let snap = STATE.with(|st| st.borrow().snap.clone());
    let (scope, aspect) = match card.kind {
        KIND_SYSTEM => {
            let aspect = snap.systems.iter().find(|s| s.id == card.key.as_str()).map(|s| s.aspect).unwrap_or(0.75);
            (format!("system:{}", card.key), aspect)
        }
        KIND_FAVORITES => ("favorites".to_owned(), 0.75),
        KIND_COLLECTION => (format!("collection:{}", card.key), 0.75),
        _ => return,
    };
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.grid_scope = scope.clone();
        st.grid.clear();
    });
    ui.set_library_title(card.title.clone());
    ui.set_library_detail(card.detail.clone());
    ui.set_library_aspect(aspect);
    ui.set_library_index(0);
    ui.set_library_games(ModelRc::default());
    ui.set_library_open(true);
    fetch_grid(ui, scope, true);
}

fn close_grid(ui: &HomeWindow) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.grid_scope.clear();
        st.grid.clear();
    });
    ui.set_library_open(false);
    ui.set_library_games(ModelRc::default());
    keep_visible_covers(ui);
}

/* ---------------------------------------------------------------- */
/* Input                                                            */
/* ---------------------------------------------------------------- */

fn show_notice(ui: &HomeWindow, text: String) {
    let serial = STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.notice_serial += 1;
        st.notice_serial
    });
    ui.set_home_notice(text.into());
    let weak = ui.as_weak();
    Timer::single_shot(Duration::from_millis(NOTICE_MS), move || {
        let Some(ui) = weak.upgrade() else { return };
        if STATE.with(|st| st.borrow().notice_serial) == serial {
            ui.set_home_notice("".into());
        }
    });
}

fn focused_card(ui: &HomeWindow) -> Option<HomeCard> {
    if ui.get_library_open() {
        return ui.get_library_games().row_data(ui.get_library_index().max(0) as usize);
    }
    match ui.get_home_row() {
        0 => ui.get_home_recent().row_data(ui.get_home_recent_index().max(0) as usize),
        1 => ui.get_home_shelf().row_data(ui.get_home_shelf_index().max(0) as usize),
        _ => ui.get_home_apps().row_data(ui.get_home_apps_index().max(0) as usize),
    }
}

fn toggle_favorite(ui: &HomeWindow, card: &HomeCard) {
    if card.kind != KIND_GAME {
        return;
    }
    let command = format!("FAVORITE\t{}\t{}", card.key, if card.favorite { 0 } else { 1 });
    /* Show the new state at once; the service snapshot confirms it. */
    let mut updated = card.clone();
    updated.favorite = !card.favorite;
    let model = if ui.get_library_open() { ui.get_library_games() } else { ui.get_home_recent() };
    let index = if ui.get_library_open() { ui.get_library_index() } else { ui.get_home_recent_index() };
    model.set_row_data(index.max(0) as usize, updated);
    thread::spawn(move || {
        if let Err(e) = library_command(&command) {
            eprintln!("home: favorite failed={}", e);
        }
    });
}

fn move_row(ui: &HomeWindow, delta: i32) -> bool {
    let counts = [
        ui.get_home_recent().row_count(),
        ui.get_home_shelf().row_count(),
        ui.get_home_apps().row_count(),
    ];
    let mut row = ui.get_home_row();
    loop {
        row += delta;
        if !(0..3).contains(&row) {
            return false;
        }
        if counts[row as usize] > 0 {
            ui.set_home_row(row);
            return true;
        }
    }
}

fn move_column(ui: &HomeWindow, delta: i32) -> bool {
    let (count, index) = match ui.get_home_row() {
        0 => (ui.get_home_recent().row_count(), ui.get_home_recent_index()),
        1 => (ui.get_home_shelf().row_count(), ui.get_home_shelf_index()),
        _ => (ui.get_home_apps().row_count(), ui.get_home_apps_index()),
    };
    let next = index + delta;
    if count == 0 || next < 0 || next >= count as i32 {
        return false;
    }
    match ui.get_home_row() {
        0 => ui.set_home_recent_index(next),
        1 => ui.set_home_shelf_index(next),
        _ => ui.set_home_apps_index(next),
    }
    true
}

fn move_grid(ui: &HomeWindow, delta: i32) -> bool {
    let count = ui.get_library_games().row_count() as i32;
    let next = ui.get_library_index() + delta;
    if count == 0 || next < 0 || next >= count {
        return false;
    }
    /* Left/right stay on the current grid row. */
    let cols = ui.get_library_columns().max(1);
    if delta.abs() == 1 && next / cols != ui.get_library_index() / cols {
        return false;
    }
    ui.set_library_index(next);
    refresh_grid_window(ui);
    true
}

/* Home / library navigation. Returns nothing: every press is consumed. */
pub fn handle_home_action(ui: &HomeWindow, action: &str) {
    let grid = ui.get_library_open();
    let cols = ui.get_library_columns().max(1);
    let moved = match action {
        "menu_up" => if grid { move_grid(ui, -cols) } else { move_row(ui, -1) },
        "menu_down" => if grid { move_grid(ui, cols) } else { move_row(ui, 1) },
        "menu_left" => if grid { move_grid(ui, -1) } else { move_column(ui, -1) },
        "menu_right" => if grid { move_grid(ui, 1) } else { move_column(ui, 1) },
        "menu_back" => {
            if grid {
                play_ui_sound(action);
                close_grid(ui);
            }
            return;
        }
        "menu_confirm" => {
            let Some(card) = focused_card(ui) else { return };
            play_ui_sound(action);
            match card.kind {
                KIND_FAVORITES | KIND_COLLECTION | KIND_SYSTEM => open_grid(ui, &card),
                KIND_GAME => launch_game(ui, &card),
                /* Applications need the future application session
                 * service. */
                _ => show_notice(ui, tr(ui, 411, "Launching is not available yet")),
            }
            return;
        }
        "face_north" => {
            if let Some(card) = focused_card(ui) {
                play_ui_sound("menu_confirm");
                toggle_favorite(ui, &card);
            }
            return;
        }
        _ => return,
    };
    if moved {
        play_ui_sound(action);
        ui.set_home_notice("".into());
    }
}
