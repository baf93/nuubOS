/*
 * Home carousels and Game Library browsing (EPIC-001 Home, EPIC-011).
 *
 * nuubos-libraryd owns the catalog, play history, collections, Find and
 * the application registry; this module only subscribes to its
 * snapshot, turns it into HomeCard models and sends user commands back.
 * Pixel geometry lives in home.slint.
 *
 * Covers are decoded off the UI thread, downscaled once to the card height
 * in physical pixels and given rounded corners (the software renderer
 * does not clip images to border-radius). Only covers near the visible
 * part of a library grid are kept in memory.
 */

use crate::{on_game_session, play_ui_sound, tr, tr_arg, HomeCard, HomeSheetItem, HomeWindow};
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
const KIND_NEW_COLLECTION: i32 = 1;
const KIND_COLLECTION: i32 = 2;
const KIND_SYSTEM: i32 = 3;
const KIND_APP: i32 = 4;
pub(crate) const KIND_RESULTS: i32 = 5;

/* Home rows. */
const ROW_RECENT: i32 = 0;
const ROW_SYSTEMS: i32 = 1;
const ROW_COLLECTIONS: i32 = 2;
const ROW_APPS: i32 = 3;

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
}

#[derive(Clone, Default)]
struct LibSystem {
    id: String,
    name: String,
    count: i64,
    aspect: f32,
    color: String,
    icon: String,
    previews: Vec<String>,
}

#[derive(Clone, Default)]
pub(crate) struct LibCollection {
    pub id: String,
    pub name: String,
    pub count: i64,
    previews: Vec<String>,
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
    /* Games added or available again since the last requested scan. */
    scan_new: usize,
    /* Favorites of an older nuubOS still to become a collection. */
    favorites_pending: bool,
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
    /* Find view: the results strip and its search. */
    find: Vec<LibGame>,
    find_spec: String,
    find_more: String,
    covers: HashMap<String, CoverState>,
    lru: VecDeque<String>,
    notice_serial: u64,
}

thread_local! {
    static STATE: RefCell<LibraryState> = RefCell::new(LibraryState::default());
}

static COVER_QUEUE: OnceLock<Mutex<mpsc::Sender<(String, u32, f32)>>> = OnceLock::new();
/* Cover keys still wanted by the UI: a grid cover that leaves the grid
 * window before its turn is dropped from this set and the loader skips it,
 * so fast scrolling through a big system does not decode a backlog of
 * covers nobody sees any more. */
static COVER_WANTED: Mutex<Option<HashSet<String>>> = Mutex::new(None);

fn cover_wanted(key: &str, wanted: bool) {
    let mut set = COVER_WANTED.lock().unwrap();
    let set = set.get_or_insert_with(HashSet::new);
    if wanted {
        set.insert(key.to_owned());
    } else {
        set.remove(key);
    }
}

fn cover_still_wanted(key: &str) -> bool {
    COVER_WANTED.lock().unwrap().as_ref().is_some_and(|s| s.contains(key))
}

fn library_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(LIBRARY_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let multi = command.starts_with("GAMES") || command.starts_with("SEARCH") || command == "FILTERS"
        || command.starts_with("DETAILS") || command == "STATUS";
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
    })
}

fn parse_snapshot(reply: &str) -> LibSnapshot {
    let mut s = LibSnapshot::default();
    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "user" => s.user = value.into(),
            "scanning" => s.scanning = value == "1",
            "scan_new" => s.scan_new = value.parse().unwrap_or(0),
            "recent" => s.recent.extend(parse_game(value)),
            "favorites_pending" => s.favorites_pending = value != "0",
            "collection" => {
                let f: Vec<&str> = value.split('\t').collect();
                if f.len() >= 3 && f[0] != "favorites" {
                    s.collections.push(LibCollection {
                        id: f[0].into(),
                        name: f[1].into(),
                        count: f[2].parse().unwrap_or(0),
                        previews: f[3..].iter().filter(|p| !p.is_empty()).map(|p| p.to_string()).collect(),
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
                        previews: f.iter().skip(6).filter(|p| !p.is_empty()).map(|p| p.to_string()).collect(),
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
fn capitalized(text: &str) -> String {
    let mut chars = text.chars();
    match chars.next() {
        Some(first) => first.to_uppercase().chain(chars).collect(),
        None => String::new(),
    }
}

fn last_played_label(ui: &HomeWindow, last: i64) -> String {
    tr_arg(ui, 409, "Last played {0}", &last_played_day(ui, last))
}

fn last_played_day(ui: &HomeWindow, last: i64) -> String {
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
    day
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

/* Area-average downscale to `target` rows + anti-aliased rounded corners
 * (radius 0: square corners). */
pub(crate) fn shape_cover(src: &Image, target: u32, radius_px: f32) -> Option<(SharedPixelBuffer<Rgba8Pixel>, f32)> {
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
        let cov = if radius_px <= 0.0 { 1.0 } else { (r - d + 0.5).clamp(0.0, 1.0) } / cnt;
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
            let key = cover_key(&path, target);
            if !cover_still_wanted(&key) {
                continue;
            }
            let img = Image::load_from_path(Path::new(&path)).ok();
            let shaped = img.as_ref().and_then(|i| shape_cover(i, target, radius));
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                let Some(ui) = weak.upgrade() else { return };
                let accepted = STATE.with(|st| {
                    let mut st = st.borrow_mut();
                    /* Cancelled while decoding: the UI no longer wants it. */
                    if !matches!(st.covers.get(&key), Some(CoverState::Loading)) {
                        return false;
                    }
                    cover_wanted(&key, false);
                    let entry = match shaped {
                        Some((buf, aspect)) => CoverState::Ready(Image::from_rgba8_premultiplied(buf), aspect),
                        None => CoverState::Failed,
                    };
                    st.covers.insert(key.clone(), entry);
                    st.lru.retain(|k| k != &key);
                    st.lru.push_back(key);
                    true
                });
                if accepted {
                    cover_arrived(&ui, &path);
                }
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
                cover_wanted(&key, true);
                st.covers.insert(key, CoverState::Loading);
                let radius = (12.0 * scale).max(target as f32 * 0.06);
                if let Some(q) = COVER_QUEUE.get() {
                    let _ = q.lock().unwrap().send((path.to_owned(), target, radius));
                }
                None
            }
        }
    })
}

/* Forget a grid cover still queued or decoding (it left the grid window);
 * a later request queues it again. */
fn cancel_cover(ui: &HomeWindow, path: &str) {
    if path.is_empty() {
        return;
    }
    let key = cover_key(path, cover_target(ui, false));
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        if matches!(st.covers.get(&key), Some(CoverState::Loading)) {
            st.covers.remove(&key);
            cover_wanted(&key, false);
        }
    });
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
        /* The chip says only when ("Today"), user request 2026-10-10. */
        last_played: if g.last > 0 { capitalized(&last_played_day(ui, g.last)) } else { tr(ui, 405, "Never played") }.into(),
        play_time: if g.last > 0 && g.time > 0 { duration_label(ui, g.time) } else { String::new() }.into(),
        system_name: system_name.into(),
        cover_path: g.cover.clone().into(),
        has_cover: cover.is_some(),
        cover: cover.map(|(i, _)| i).unwrap_or_default(),
        aspect,
        x_units: 0.0,
        accent,
        available: g.available,
        focused: false,
        ..Default::default()
    }
}

/* Cover previews of a system or collection card (small decode). */
fn with_previews(ui: &HomeWindow, mut card: HomeCard, paths: &[String]) -> HomeCard {
    let mut images: Vec<Image> = Vec::new();
    for p in paths.iter().take(4) {
        if let Some((img, _)) = request_cover(ui, p, true) {
            images.push(img);
        }
    }
    card.previews = images.len() as i32;
    let mut it = images.into_iter();
    card.p0 = it.next().unwrap_or_default();
    card.p1 = it.next().unwrap_or_default();
    card.p2 = it.next().unwrap_or_default();
    card.p3 = it.next().unwrap_or_default();
    card
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

    let systems = with_x_units(
        snap.systems
            .iter()
            .map(|s| {
                let icon = request_cover(ui, &s.icon, true);
                with_previews(ui, HomeCard {
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
                }, &s.previews)
            })
            .collect(),
    );
    let mut collections: Vec<HomeCard> = snap
        .collections
        .iter()
        .map(|c| {
            with_previews(ui, HomeCard {
                key: c.id.clone().into(),
                kind: KIND_COLLECTION,
                title: c.name.clone().into(),
                detail: count_label(ui, c.count).into(),
                aspect: SHELF_ASPECT,
                accent: parse_color("#2d3f66"),
                available: true,
                ..Default::default()
            }, &c.previews)
        })
        .collect();
    /* The row always ends with New Collection: never empty. */
    if !snap.user.is_empty() {
        collections.push(HomeCard {
            key: "new".into(),
            kind: KIND_NEW_COLLECTION,
            title: tr(ui, 538, "New Collection").into(),
            aspect: SHELF_ASPECT,
            available: true,
            ..Default::default()
        });
    }
    let collections = with_x_units(collections);

    let apps = with_x_units(
        snap.apps
            .iter()
            .map(|a| {
                let icon = request_cover(ui, &a.icon, true);
                HomeCard {
                    key: a.id.clone().into(),
                    kind: KIND_APP,
                    /* Built-in nuubOS applications have localized names. */
                    title: match a.id.as_str() {
                        "files" => tr(ui, 609, "Files").into(),
                        "media" => tr(ui, 665, "Media").into(),
                        "web" => tr(ui, 678, "Web").into(),
                        _ => a.name.clone().into(),
                    },
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

    let counts = [recent.len() as i32, systems.len() as i32, collections.len() as i32, apps.len() as i32];
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
    if let Some(m) = set_model(ui.get_home_systems(), systems) {
        ui.set_home_systems(m);
    }
    if let Some(m) = set_model(ui.get_home_collections(), collections) {
        ui.set_home_collections(m);
    }
    if let Some(m) = set_model(ui.get_home_apps(), apps) {
        ui.set_home_apps(m);
    }
    ui.set_home_recent_index(ui.get_home_recent_index().clamp(0, (counts[0] - 1).max(0)));
    ui.set_home_systems_index(ui.get_home_systems_index().clamp(0, (counts[1] - 1).max(0)));
    ui.set_home_collections_index(ui.get_home_collections_index().clamp(0, (counts[2] - 1).max(0)));
    ui.set_home_apps_index(ui.get_home_apps_index().clamp(0, (counts[3] - 1).max(0)));
    ui.set_library_scanning(snap.scanning);
    sync_focus(ui);
}

/* Card focus lives in the models (HomeCard.focused) and changes only on
 * the cards it moves between: Slint marks every binding on a shared index
 * dirty when it changes, and with it every cached card layer that read it
 * (18-22 layer re-renders per row change, a missed frame on each move). */
fn sync_focus(ui: &HomeWindow) {
    let row = ui.get_home_row();
    let models = [
        (ui.get_home_recent(), row == ROW_RECENT, ui.get_home_recent_index()),
        (ui.get_home_systems(), row == ROW_SYSTEMS, ui.get_home_systems_index()),
        (ui.get_home_collections(), row == ROW_COLLECTIONS, ui.get_home_collections_index()),
        (ui.get_home_apps(), row == ROW_APPS, ui.get_home_apps_index()),
        (ui.get_library_games(), true, ui.get_library_index()),
        (ui.get_find_results(), ui.get_find_zone() == 2, ui.get_find_result_index()),
    ];
    for (model, active, index) in models {
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

/* A new playful message for empty sections, per user session. */
fn pick_empty_variant(ui: &HomeWindow) {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.subsec_nanos())
        .unwrap_or(0);
    ui.set_home_empty_variant((nanos % 12) as i32);
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
    sync_focus(ui);
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
    for model in [ui.get_home_apps(), ui.get_home_systems(), ui.get_home_collections()] {
        for i in 0..model.row_count() {
            if let Some(c) = model.row_data(i) {
                keep.insert(cover_key(&c.cover_path, small));
            }
        }
    }
    /* Showcase previews (paths from the snapshot). */
    STATE.with(|st| {
        let st = st.borrow();
        for p in st.snap.systems.iter().flat_map(|s| s.previews.iter())
            .chain(st.snap.collections.iter().flat_map(|c| c.previews.iter()))
        {
            keep.insert(cover_key(p, small));
        }
    });
    STATE.with(|st| {
        for g in st.borrow().find.iter() {
            keep.insert(cover_key(&g.cover, big));
        }
    });
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
    let in_home = [ui.get_home_recent(), ui.get_home_systems(), ui.get_home_collections(), ui.get_home_apps()]
        .iter()
        .any(|m| (0..m.row_count()).any(|i| m.row_data(i).is_some_and(|c| c.cover_path == path)))
        || STATE.with(|st| {
            let st = st.borrow();
            st.snap.systems.iter().any(|s| s.previews.iter().any(|p| p == path))
                || st.snap.collections.iter().any(|c| c.previews.iter().any(|p| p == path))
        });
    if in_home {
        build_home(ui);
    }
    if ui.get_library_open() {
        refresh_grid_window(ui);
    }
    if ui.get_find_open() && STATE.with(|st| st.borrow().find.iter().any(|g| g.cover == path)) {
        build_find(ui);
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
        } else {
            cancel_cover(ui, &g.cover);
        }
    }
    sync_focus(ui);
    keep_visible_covers(ui);
}

/* ---------------------------------------------------------------- */
/* Service snapshot                                                 */
/* ---------------------------------------------------------------- */

fn apply_snapshot(ui: &HomeWindow, snap: LibSnapshot) {
    let scan = (snap.scanning, snap.scan_new);
    if snap.favorites_pending {
        /* Favorites are gone as a concept (user request 2026-10-09): an
         * older list becomes a collection named in the user's language. */
        let command = format!("MIGRATE_FAVORITES\t{}", tr(ui, 399, "Favorites").replace(['\t', '\n', '|'], " "));
        thread::spawn(move || {
            let _ = library_command(&command);
        });
    }
    let user_changed = STATE.with(|st| {
        let mut st = st.borrow_mut();
        let changed = st.snap.user != snap.user;
        st.snap = snap;
        changed
    });
    if user_changed {
        /* Another user's Home starts at the top, on Recently Played
         * even while it is empty (user request 2026-10-08). */
        ui.set_home_row(0);
        ui.set_home_recent_index(0);
        ui.set_home_systems_index(0);
        ui.set_home_collections_index(0);
        ui.set_home_apps_index(0);
        ui.set_home_sheet_open(false);
        ui.set_find_open(false);
        close_grid(ui);
        pick_empty_variant(ui);
    }
    build_home(ui);
    crate::findui::library_changed(ui);
    crate::gameui::library_scan_state(ui, scan.0, scan.1);
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
    launch_by_id(ui, card.key.as_str(), card.available);
}

/* Confirm held on a game card: Start Game (states, fresh start) after
 * LONG_PRESS_MS; released before, the game resumes (EPIC-019). */
const LONG_PRESS_MS: u64 = 600;

thread_local! {
    static GAME_PRESS: RefCell<Option<(slint::Timer, HomeCard)>> = RefCell::new(None);
}

fn press_game(ui: &HomeWindow, card: &HomeCard) {
    let timer = slint::Timer::default();
    let weak = ui.as_weak();
    let held = card.clone();
    timer.start(slint::TimerMode::SingleShot, Duration::from_millis(LONG_PRESS_MS), move || {
        GAME_PRESS.with(|p| p.borrow_mut().take());
        let Some(ui) = weak.upgrade() else { return };
        if !held.available {
            play_ui_sound("error");
            show_notice(&ui, tr(&ui, 434, "This game is not available"));
            return;
        }
        play_ui_sound("menu_confirm");
        let cover = held.has_cover.then(|| held.cover.clone());
        crate::gameui::open_launch_menu(&ui, held.key.as_str(), held.title.as_str(), cover);
    });
    GAME_PRESS.with(|p| *p.borrow_mut() = Some((timer, card.clone())));
}

pub fn release_confirm(ui: &HomeWindow) {
    if let Some((timer, card)) = GAME_PRESS.with(|p| p.borrow_mut().take()) {
        timer.stop();
        launch_game(ui, &card);
    }
}

/* Also Play in Game Details. */
pub fn launch_by_id(ui: &HomeWindow, id: &str, available: bool) {
    launch_with_mode(ui, id, available, "resume");
}

const LAUNCH_SCREEN_TIMEOUT_MS: u64 = 60_000;

thread_local! {
    /* Closes the loading screen if no session ever starts. */
    static LAUNCH_TIMER: Timer = Timer::default();
}

/* Loading screen for the game being launched, from the card Home shows
 * for it (Recently Played, the open grid, else a title-only screen). */
fn show_launch_screen(ui: &HomeWindow, id: &str) {
    let card = [ui.get_home_recent(), ui.get_library_games()]
        .into_iter()
        .find_map(|m| (0..m.row_count()).filter_map(|i| m.row_data(i)).find(|c| c.key.as_str() == id));
    let snap = STATE.with(|st| st.borrow().snap.clone());
    match card {
        Some(c) => {
            ui.set_launch_title(c.title.clone());
            ui.set_launch_system(c.system_name.clone());
            ui.set_launch_accent(c.accent);
            ui.set_launch_has_cover(c.has_cover);
            ui.set_launch_cover(c.cover.clone());
        }
        None => {
            let game = snap.recent.iter().find(|g| g.id == id);
            ui.set_launch_title(game.map(|g| g.title.clone()).unwrap_or_default().into());
            ui.set_launch_system(Default::default());
            ui.set_launch_has_cover(false);
        }
    }
    ui.set_launch_busy(true);
    ui.set_launch_open(true);
    let weak = ui.as_weak();
    LAUNCH_TIMER.with(|t| {
        t.start(slint::TimerMode::SingleShot, Duration::from_millis(LAUNCH_SCREEN_TIMEOUT_MS), move || {
            if let Some(ui) = weak.upgrade() {
                if !GAME_RUNNING.load(Ordering::SeqCst) {
                    hide_launch_screen(&ui);
                }
            }
        })
    });
}

fn hide_launch_screen(ui: &HomeWindow) {
    LAUNCH_TIMER.with(|t| t.stop());
    ui.set_launch_busy(false);
    ui.set_launch_open(false);
    ui.set_launch_cover(Image::default());
}

/* Session start: the game covers Home, stop animating under it; end:
 * back to Home. */
fn launch_session(ui: &HomeWindow, running: bool) {
    if running {
        LAUNCH_TIMER.with(|t| t.stop());
        ui.set_launch_busy(false);
    } else if ui.get_launch_open() {
        hide_launch_screen(ui);
    }
}

/* mode: resume | new | slot:N (nuubos-emud LAUNCH). */
pub fn launch_with_mode(ui: &HomeWindow, id: &str, available: bool, mode: &str) {
    if !available {
        play_ui_sound("error");
        show_notice(ui, tr(ui, 434, "This game is not available"));
        return;
    }
    if GAME_RUNNING.load(Ordering::SeqCst) {
        return;
    }
    let command = format!("LAUNCH\t{}\t{}", id, mode);
    play_ui_sound("launch");
    show_launch_screen(ui, id);
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
            if reason != "busy" {
                hide_launch_screen(&ui);
            }
            let text = match reason.as_str() {
                /* A double press while the game is starting. */
                "busy" => return,
                "no-core" => tr(&ui, 433, "No emulator is installed for this system"),
                "unavailable" | "game" => tr(&ui, 434, "This game is not available"),
                _ => tr(&ui, 431, "The game could not be started"),
            };
            play_ui_sound("error");
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
                            launch_session(&ui, running);
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
                    launch_session(&ui, false);
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
        let command = match scope.strip_prefix("search:") {
            Some(spec) => format!("SEARCH\t{spec}"),
            None => format!("GAMES\t{scope}"),
        };
        let Ok(reply) = library_command(&command) else { return };
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

pub(crate) fn open_grid(ui: &HomeWindow, card: &HomeCard) {
    let snap = STATE.with(|st| st.borrow().snap.clone());
    let (scope, aspect) = match card.kind {
        KIND_SYSTEM => {
            let aspect = snap.systems.iter().find(|s| s.id == card.key.as_str()).map(|s| s.aspect).unwrap_or(0.75);
            (format!("system:{}", card.key), aspect)
        }
        KIND_COLLECTION => (format!("collection:{}", card.key), 0.75),
        KIND_RESULTS => (format!("search:{}", card.key), 0.75),
        _ => return,
    };
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.grid_scope = scope.clone();
        st.grid.clear();
    });
    ui.set_library_title(card.title.clone());
    ui.set_library_detail(card.detail.clone());
    ui.set_library_card(card.clone());
    ui.set_library_aspect(aspect);
    ui.set_library_index(0);
    ui.set_library_games(ModelRc::default());
    ui.set_library_open(true);
    fetch_grid(ui, scope, true);
}

/* The application page shows the icon of the tile it was opened from. */
fn app_page_icon(ui: &HomeWindow, card: &HomeCard) {
    ui.set_app_page_icon(card.cover.clone());
    ui.set_app_page_has_icon(card.has_cover);
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

/* Systems with at least one available game for the active user (the
 * libraryd snapshot), e.g. for the BIOS Files sections. */
pub fn system_ids() -> Vec<String> {
    STATE.with(|st| st.borrow().snap.systems.iter().filter(|s| s.count > 0).map(|s| s.id.clone()).collect())
}

/* (id, name) of the systems with games, for the Find page. */
pub fn system_names() -> Vec<(String, String)> {
    STATE.with(|st| st.borrow().snap.systems.iter().filter(|s| s.count > 0).map(|s| (s.id.clone(), s.name.clone())).collect())
}

/* Home notice for other modules (e.g. after Delete Game). */
pub fn notice(ui: &HomeWindow, text: String) {
    show_notice(ui, text);
}

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
        ROW_RECENT => ui.get_home_recent().row_data(ui.get_home_recent_index().max(0) as usize),
        ROW_SYSTEMS => ui.get_home_systems().row_data(ui.get_home_systems_index().max(0) as usize),
        ROW_COLLECTIONS => ui.get_home_collections().row_data(ui.get_home_collections_index().max(0) as usize),
        _ => ui.get_home_apps().row_data(ui.get_home_apps_index().max(0) as usize),
    }
}

/* Every section is reachable, empty ones included (empty-state card). */
fn move_row(ui: &HomeWindow, delta: i32) -> bool {
    let row = ui.get_home_row() + delta;
    if !(ROW_RECENT..=ROW_APPS).contains(&row) {
        return false;
    }
    ui.set_home_row(row);
    sync_focus(ui);
    true
}

fn move_column(ui: &HomeWindow, delta: i32) -> bool {
    let (count, index) = match ui.get_home_row() {
        ROW_RECENT => (ui.get_home_recent().row_count(), ui.get_home_recent_index()),
        ROW_SYSTEMS => (ui.get_home_systems().row_count(), ui.get_home_systems_index()),
        ROW_COLLECTIONS => (ui.get_home_collections().row_count(), ui.get_home_collections_index()),
        _ => (ui.get_home_apps().row_count(), ui.get_home_apps_index()),
    };
    let next = index + delta;
    if count == 0 || next < 0 || next >= count as i32 {
        return false;
    }
    match ui.get_home_row() {
        ROW_RECENT => ui.set_home_recent_index(next),
        ROW_SYSTEMS => ui.set_home_systems_index(next),
        ROW_COLLECTIONS => ui.set_home_collections_index(next),
        _ => ui.set_home_apps_index(next),
    }
    sync_focus(ui);
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
    if ui.get_home_sheet_open() {
        sheet_action(ui, action);
        return;
    }
    if ui.get_find_open() && !ui.get_library_open() {
        crate::findui::find_action(ui, action);
        return;
    }
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
                KIND_COLLECTION | KIND_SYSTEM => open_grid(ui, &card),
                KIND_NEW_COLLECTION => crate::findui::new_collection(ui, None),
                KIND_GAME => press_game(ui, &card),
                /* Built-in nuubUI applications (EPIC-025, EPIC-026). */
                _ if card.key.as_str() == "moonlight" => {
                    app_page_icon(ui, &card);
                    crate::moonlight::open(ui)
                }
                _ if card.key.as_str() == "steamlink" => {
                    app_page_icon(ui, &card);
                    crate::steamlink::open(ui)
                }
                _ if card.key.as_str() == "files" => {
                    app_page_icon(ui, &card);
                    crate::filesui::open(ui)
                }
                _ if card.key.as_str() == "media" => {
                    app_page_icon(ui, &card);
                    crate::mediaui::open(ui)
                }
                _ if card.key.as_str() == "web" => crate::webui::open(ui),
                /* Other applications need the future application session
                 * service. */
                _ => show_notice(ui, tr(ui, 411, "Launching is not available yet")),
            }
            return;
        }
        /* Game Details (EPIC-016, North since 2026-10-10; A still
         * launches at once); on a collection: Manage. */
        "face_north" => {
            match focused_card(ui) {
                Some(card) if card.kind == KIND_GAME => {
                    play_ui_sound("menu_confirm");
                    let cover = card.has_cover.then(|| card.cover.clone());
                    crate::gameui::open_details(ui, card.key.as_str(), card.title.as_str(), cover);
                }
                Some(card) if card.kind == KIND_COLLECTION => {
                    play_ui_sound("menu_confirm");
                    open_manage_sheet(ui, &card);
                }
                _ => {}
            }
            return;
        }
        /* Find a Game (EPIC-011 browsing); on a system or collection
         * page, limited to it. A results grid goes back to its Find. */
        "select" => {
            play_ui_sound("menu_confirm");
            let card = ui.get_library_card();
            if !grid {
                crate::findui::open(ui);
            } else if card.kind == KIND_SYSTEM || card.kind == KIND_COLLECTION {
                close_grid(ui);
                crate::findui::open_scoped(ui, card.clone(), card.kind == KIND_SYSTEM);
            } else {
                close_grid(ui);
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

/* ---------------------------------------------------------------- */
/* Home sheet: Manage a collection (Rename, Delete)                 */
/* ---------------------------------------------------------------- */

#[derive(Clone, Default)]
enum Sheet {
    #[default]
    None,
    /* Collection id + name; rows = Rename, Delete. */
    Manage(String, String),
    /* Find filter: context, (value, label) options, current value. */
    Choice(String, String, Vec<(String, String)>, String),
}

thread_local! {
    static SHEET: RefCell<(Sheet, i32)> = RefCell::new((Sheet::None, -1));
}

pub(crate) fn collections() -> Vec<LibCollection> {
    STATE.with(|st| st.borrow().snap.collections.clone())
}

fn show_sheet(ui: &HomeWindow, title: String, items: Vec<HomeSheetItem>, index: i32) {
    let count = items.len() as i32;
    ui.set_home_sheet_title(title.into());
    ui.set_home_sheet_items(ModelRc::from(Rc::new(VecModel::from(items))));
    let index = index.clamp(0, (count - 1).max(0));
    ui.set_home_sheet_index(index);
    let visible = ui.get_home_sheet_visible_rows().max(1);
    ui.set_home_sheet_scroll((index - visible + 1).clamp(0, (count - visible).max(0)));
    ui.set_home_sheet_open(true);
}

fn render_sheet(ui: &HomeWindow) {
    let (sheet, armed) = SHEET.with(|s| s.borrow().clone());
    let index = ui.get_home_sheet_index();
    match sheet {
        Sheet::Manage(_, name) => {
            let items = vec![
                HomeSheetItem { label: tr(ui, 843, "Rename Collection").into(), check: -1, ..Default::default() },
                HomeSheetItem {
                    label: tr(ui, 839, "Delete Collection").into(),
                    detail: if armed == 1 { tr(ui, 290, "Press again to delete") } else { tr(ui, 850, "Games stay in your library; only the collection goes away") }.into(),
                    check: -1,
                    armed: armed == 1,
                },
            ];
            show_sheet(ui, name, items, index);
        }
        Sheet::Choice(_, title, options, current) => {
            let items = options
                .iter()
                .map(|(v, l)| HomeSheetItem { label: l.clone().into(), check: if *v == current { 1 } else { -1 }, ..Default::default() })
                .collect();
            show_sheet(ui, title, items, index);
        }
        Sheet::None => ui.set_home_sheet_open(false),
    }
}

pub(crate) fn open_choice_sheet(ui: &HomeWindow, context: String, title: String, options: Vec<(String, String)>, current: String) {
    let index = options.iter().position(|(v, _)| *v == current).unwrap_or(0) as i32;
    SHEET.with(|s| *s.borrow_mut() = (Sheet::Choice(context, title, options, current), -1));
    ui.set_home_sheet_index(index);
    render_sheet(ui);
}

fn open_manage_sheet(ui: &HomeWindow, card: &HomeCard) {
    SHEET.with(|s| *s.borrow_mut() = (Sheet::Manage(card.key.to_string(), card.title.to_string()), -1));
    ui.set_home_sheet_index(0);
    render_sheet(ui);
}

pub(crate) fn close_sheet(ui: &HomeWindow) {
    SHEET.with(|s| *s.borrow_mut() = (Sheet::None, -1));
    ui.set_home_sheet_open(false);
}

fn sheet_action(ui: &HomeWindow, action: &str) {
    let count = ui.get_home_sheet_items().row_count() as i32;
    match action {
        "menu_up" | "menu_down" if count > 0 => {
            play_ui_sound(action);
            let next = (ui.get_home_sheet_index() + if action == "menu_up" { -1 } else { 1 }).rem_euclid(count);
            ui.set_home_sheet_index(next);
            SHEET.with(|s| s.borrow_mut().1 = -1);
            render_sheet(ui);
        }
        "menu_back" => {
            play_ui_sound(action);
            close_sheet(ui);
        }
        "menu_confirm" => {
            play_ui_sound(action);
            let index = ui.get_home_sheet_index();
            let (sheet, armed) = SHEET.with(|s| s.borrow().clone());
            match sheet {
                Sheet::Manage(cid, name) => match index {
                    0 => {
                        close_sheet(ui);
                        crate::findui::rename_collection(ui, cid, name);
                    }
                    _ if armed == 1 => {
                        close_sheet(ui);
                        show_notice(ui, tr(ui, 848, "Collection deleted"));
                        thread::spawn(move || {
                            let _ = library_command(&format!("COLLECTION_DELETE\t{cid}"));
                        });
                    }
                    _ => {
                        SHEET.with(|s| s.borrow_mut().1 = 1);
                        render_sheet(ui);
                    }
                },
                Sheet::Choice(context, _, options, _) => {
                    close_sheet(ui);
                    if let Some((value, _)) = options.get(index as usize) {
                        crate::findui::apply_choice(ui, &context, value.clone());
                    }
                }
                Sheet::None => close_sheet(ui),
            }
        }
        _ => {}
    }
}

/* Find results / a picked game, opened from the Find page. */
pub(crate) fn open_results(ui: &HomeWindow, spec: String, title: String) {
    let card = HomeCard {
        key: spec.into(),
        kind: KIND_RESULTS,
        title: title.into(),
        available: true,
        ..Default::default()
    };
    open_grid(ui, &card);
}

/* ---------------------------------------------------------------- */
/* Find view results                                                */
/* ---------------------------------------------------------------- */

/* Covers in the strip: the first ones (the rest is in the grid). */
const FIND_STRIP: usize = 40;

pub(crate) fn set_find_results(ui: &HomeWindow, reply: &str, spec: &str, more: String) {
    let games = parse_games(reply);
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.find = games.into_iter().take(FIND_STRIP).collect();
        st.find_spec = spec.to_owned();
        st.find_more = if reply.lines().filter(|l| l.starts_with("game=")).count() > 0 { more } else { String::new() };
    });
    ui.set_find_result_index(0);
    build_find(ui);
}

fn build_find(ui: &HomeWindow) {
    let (snap, games, more, spec) = STATE.with(|st| {
        let st = st.borrow();
        (st.snap.clone(), st.find.clone(), st.find_more.clone(), st.find_spec.clone())
    });
    let mut cards: Vec<HomeCard> = games.iter().map(|g| game_card(ui, &snap, g, true)).collect();
    /* The last card opens every result in the library grid. */
    if !more.is_empty() {
        cards.push(HomeCard {
            key: spec.into(),
            kind: KIND_RESULTS,
            title: more.into(),
            aspect: 0.75,
            available: true,
            ..Default::default()
        });
    }
    let n = cards.len() as i32;
    let cards = with_x_units(cards);
    if let Some(m) = set_model(ui.get_find_results(), cards) {
        ui.set_find_results(m);
    }
    ui.set_find_result_index(ui.get_find_result_index().clamp(0, (n - 1).max(0)));
    sync_focus(ui);
}

pub(crate) fn sync_find_focus(ui: &HomeWindow) {
    sync_focus(ui);
}

fn find_card(ui: &HomeWindow) -> Option<HomeCard> {
    ui.get_find_results().row_data(ui.get_find_result_index().max(0) as usize)
}

/* A on a result: play it; on the last card: every result in the grid. */
pub(crate) fn find_confirm(ui: &HomeWindow) {
    let Some(card) = find_card(ui) else { return };
    if card.kind == KIND_RESULTS {
        open_results(ui, card.key.to_string(), tr(ui, 837, "Results"));
    } else {
        launch_by_id(ui, card.key.as_str(), card.available);
    }
}

pub(crate) fn find_details(ui: &HomeWindow) {
    if let Some(card) = find_card(ui).filter(|c| c.kind == KIND_GAME) {
        let cover = card.has_cover.then(|| card.cover.clone());
        crate::gameui::open_details(ui, card.key.as_str(), card.title.as_str(), cover);
    }
}

pub(crate) fn library_request(command: &str) -> String {
    library_command(command).unwrap_or_default()
}
