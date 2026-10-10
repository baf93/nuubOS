/*
 * Find a Game (Select on Home, EPIC-011 browsing) and the Collections page.
 *
 * Find is a Home view (find-view in home.slint, user request 2026-10-10):
 * a search bar (title words, system keyboard over Home), filter chips
 * (system, decade, genre, players, never played, clear), the results as a
 * live strip of covers (A plays, North opens Game Details, the last card
 * opens them all in the library grid) and two Surprise Me buttons (random
 * game, random unplayed game → its Game Details). Choices open in the Home
 * sheet. nuubos-libraryd does the work: SEARCH with the filters and random
 * pick, FILTERS for what the chips can offer.
 *
 * The Collections page (settings-view 71, generic rows of the Details
 * shell) lists the collections: New, Rename (confirm), Delete (North,
 * second press); Home's Manage sheet reaches its keyboard flows directly.
 */

use crate::gameui::{apply_rows_split, arm_or, armed, armed_detail, focused_key, move_selection, nav, notice, show};
use crate::{handle_settings_action, navigate_settings_view, open_system_keyboard, play_ui_sound,
    tr, tr_arg, write_ui_context, HomeWindow};
use slint::{ComponentHandle, Model, ModelRc};
use std::cell::RefCell;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;

pub const COLLECTIONS_VIEW: i32 = 71;
pub const KEYBOARD_TITLE: i32 = 28;
pub const KEYBOARD_NEW_COLLECTION: i32 = 29;
pub const KEYBOARD_RENAME_COLLECTION: i32 = 30;

/* The Collections shell owns Home input. */
pub static ACTIVE: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Default)]
struct Filters {
    title: String,
    system: String,
    /* Collection id and name (Select on a collection page). */
    collection: String,
    collection_name: String,
    decade: i32,
    genre: String,
    players: i32,
    unplayed: bool,
}

#[derive(Default)]
struct State {
    filters: Filters,
    /* Matches of the current filters (None while counting). */
    count: Option<usize>,
    /* FILTERS reply: decades (first year, count), genres (name, count). */
    decades: Vec<(i32, usize)>,
    genres: Vec<(String, usize)>,
    most_players: i32,
    /* Keyboard flows started from Home return to Home. */
    from_home: bool,
    /* Opened from a system/collection page: Back returns to it. */
    return_grid: Option<crate::HomeCard>,
    /* New Collection from Add to Collection: the game to add. */
    pending_game: Option<String>,
    rename: Option<String>,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

fn clean(value: &str) -> String {
    value.replace(['|', '\t', '\n', '\r'], " ").trim().to_owned()
}

fn spec(f: &Filters) -> String {
    let mut parts: Vec<String> = Vec::new();
    if !f.title.is_empty() {
        parts.push(format!("q={}", clean(&f.title)));
    }
    if !f.system.is_empty() {
        parts.push(format!("system={}", f.system));
    }
    if !f.collection.is_empty() {
        parts.push(format!("collection={}", f.collection));
    }
    if f.decade > 0 {
        parts.push(format!("year={}-{}", f.decade, f.decade + 9));
    }
    if !f.genre.is_empty() {
        parts.push(format!("genre={}", clean(&f.genre)));
    }
    if f.players > 1 {
        parts.push(format!("players={}", f.players));
    }
    if f.unplayed {
        parts.push("unplayed=1".to_owned());
    }
    parts.join("|")
}

fn system_name(id: &str) -> String {
    crate::library::system_names().into_iter().find(|(i, _)| i == id).map(|(_, n)| n).unwrap_or_default()
}

/* ---------------------------------------------------------------- */
/* Rows                                                             */
/* ---------------------------------------------------------------- */

fn collection_rows(ui: &HomeWindow) -> Vec<(String, crate::GenRow, bool)> {
    let armed = armed();
    let mut rows: Vec<(String, crate::GenRow, bool)> = vec![("new".into(), nav(tr(ui, 538, "New Collection"), String::new()), true)];
    for c in crate::library::collections() {
        let key = format!("c:{}", c.id);
        let mut r = nav(c.name.clone(), tr_arg(ui, 400, "{0} games", &c.count.to_string()));
        r.detail = armed_detail(&armed, &key, tr(ui, 290, "Press again to delete"), String::new()).into();
        rows.push((key, r, true));
    }
    rows
}

pub(crate) fn render(ui: &HomeWindow) {
    if ui.get_settings_view() == COLLECTIONS_VIEW {
        apply_rows_split(ui, collection_rows(ui), tr(ui, 537, "Collections"), 0, String::new());
        let key = focused_key(ui);
        ui.set_settings_north_hint(if key.starts_with("c:") { tr(ui, 618, "Delete").into() } else { "".into() });
    }
}

/* ---------------------------------------------------------------- */
/* Find view                                                        */
/* ---------------------------------------------------------------- */

const ZONE_SEARCH: i32 = 0;
const ZONE_CHIPS: i32 = 1;
const ZONE_RESULTS: i32 = 2;
const ZONE_SURPRISE: i32 = 3;

fn any_filter(f: &Filters) -> bool {
    !f.system.is_empty() || !f.collection.is_empty() || f.decade > 0 || !f.genre.is_empty() || f.players > 1 || f.unplayed
}

/* Chip keys in order (the labels follow). */
fn chip_keys(f: &Filters) -> Vec<&'static str> {
    let mut keys = vec!["system", "decade", "genre", "players", "unplayed"];
    if !f.collection.is_empty() {
        keys.insert(0, "collection");
    }
    if any_filter(f) || !f.title.is_empty() {
        keys.push("clear");
    }
    keys
}

fn render_find(ui: &HomeWindow) {
    let chips: Vec<crate::FindChip> = STATE.with(|st| {
        let st = st.borrow();
        let f = &st.filters;
        chip_keys(f)
            .iter()
            .map(|k| {
                let (label, active) = match *k {
                    "collection" => (f.collection_name.clone(), true),
                    "system" if !f.system.is_empty() => (system_name(&f.system), true),
                    "system" => (tr(ui, 8, "System"), false),
                    "decade" if f.decade > 0 => (format!("{}–{}", f.decade, f.decade + 9), true),
                    "decade" => (tr(ui, 832, "Decade"), false),
                    "genre" if !f.genre.is_empty() => (f.genre.clone(), true),
                    "genre" => (tr(ui, 556, "Genre"), false),
                    "players" if f.players > 1 => (format!("{} {}+", tr(ui, 557, "Players"), f.players), true),
                    "players" => (tr(ui, 557, "Players"), false),
                    "unplayed" => (tr(ui, 834, "Never Played Only"), f.unplayed),
                    _ => (tr(ui, 844, "Clear Filters"), false),
                };
                crate::FindChip { label: label.into(), active }
            })
            .collect()
    });
    let count = chips.len() as i32;
    ui.set_find_chips(slint::ModelRc::from(std::rc::Rc::new(slint::VecModel::from(chips))));
    ui.set_find_chip_index(ui.get_find_chip_index().clamp(0, count - 1));
    let (query, status) = STATE.with(|st| {
        let st = st.borrow();
        let status = match st.count {
            None => tr(ui, 488, "Checking…"),
            Some(0) => tr(ui, 836, "No games match these filters"),
            Some(1) => tr(ui, 401, "1 game"),
            Some(n) => tr_arg(ui, 400, "{0} games", &n.to_string()),
        };
        (st.filters.title.clone(), status)
    });
    ui.set_find_query(query.into());
    ui.set_find_status(status.into());
}

/* ---------------------------------------------------------------- */
/* Service                                                          */
/* ---------------------------------------------------------------- */

/* Runs the search: the count and the results strip follow. */
fn recount(ui: &HomeWindow) {
    let spec = STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.count = None;
        spec(&st.filters)
    });
    render_find(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = crate::library::library_request(&format!("SEARCH\t{spec}"));
        let n = reply.lines().filter(|l| l.starts_with("game=")).count();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            let current = STATE.with(|st| spec_of(&st.borrow()));
            if current != spec || !ui.get_find_open() {
                return;
            }
            STATE.with(|st| st.borrow_mut().count = Some(n));
            crate::library::set_find_results(&ui, &reply, &spec, tr_arg(&ui, 835, "Show Results ({0})", &n.to_string()));
            if n == 0 && ui.get_find_zone() == ZONE_RESULTS {
                ui.set_find_zone(ZONE_CHIPS);
            }
            render_find(&ui);
        });
    });
}

fn spec_of(st: &State) -> String {
    spec(&st.filters)
}

fn load_filters(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = crate::library::library_request("FILTERS");
        let mut decades = Vec::new();
        let mut genres = Vec::new();
        let mut most = 0;
        for line in reply.lines() {
            if let Some(v) = line.strip_prefix("decade=") {
                let (y, n) = v.split_once('\t').unwrap_or((v, "0"));
                decades.push((y.parse().unwrap_or(0), n.parse().unwrap_or(0)));
            } else if let Some(v) = line.strip_prefix("genre=") {
                let (g, n) = v.split_once('\t').unwrap_or((v, "0"));
                genres.push((g.to_owned(), n.parse().unwrap_or(0)));
            } else if let Some(v) = line.strip_prefix("players=") {
                most = v.parse().unwrap_or(0);
            }
        }
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.decades = decades;
                st.genres = genres;
                st.most_players = most;
            });
            if ui.get_find_open() {
                render_find(&ui);
            }
        });
    });
}

fn random_pick(ui: &HomeWindow, unplayed: bool) {
    let mut spec = STATE.with(|st| spec_of(&st.borrow()));
    if unplayed && !spec.contains("unplayed=1") {
        spec.push_str(if spec.is_empty() { "unplayed=1" } else { "|unplayed=1" });
    }
    spec.push_str(if spec.is_empty() { "random=1" } else { "|random=1" });
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = crate::library::library_request(&format!("SEARCH\t{spec}"));
        let game = reply.lines().find_map(|l| l.strip_prefix("game=")).map(|v| {
            let f: Vec<&str> = v.split('\t').collect();
            (f.first().unwrap_or(&"").to_string(), f.get(2).unwrap_or(&"").to_string())
        });
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            match game {
                Some((id, title)) if !id.is_empty() => crate::gameui::open_details(&ui, &id, &title, None),
                _ => {
                    play_ui_sound("error");
                    crate::library::notice(&ui, tr(&ui, 836, "No games match these filters"));
                }
            }
        });
    });
}

/* ---------------------------------------------------------------- */
/* Shell                                                            */
/* ---------------------------------------------------------------- */

fn enter(ui: &HomeWindow, view: i32, title: String) {
    ACTIVE.store(true, Ordering::SeqCst);
    ui.set_details_active(true);
    ui.set_details_shell_title(title.clone().into());
    ui.set_details_has_cover(false);
    ui.set_details_title(title.into());
    ui.set_details_subtitle("".into());
    ui.set_details_description("".into());
    show(ui, view);
    ui.set_settings_open(true);
    write_ui_context("settings");
}

/* Select on Home. The filters of the last search stay for the session. */
pub fn open(ui: &HomeWindow) {
    STATE.with(|st| st.borrow_mut().return_grid = None);
    show_find(ui);
}

/* Select on a system or collection page (user request 2026-10-10): a new
 * search already limited to it; Back returns to the page. */
pub fn open_scoped(ui: &HomeWindow, card: crate::HomeCard, system: bool) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.filters = Filters::default();
        if system {
            st.filters.system = card.key.to_string();
        } else {
            st.filters.collection = card.key.to_string();
            st.filters.collection_name = card.title.to_string();
        }
        st.return_grid = Some(card);
    });
    ui.set_find_chip_index(0);
    show_find(ui);
}

fn show_find(ui: &HomeWindow) {
    crate::library::close_sheet(ui);
    ui.set_find_open(true);
    ui.set_find_zone(ZONE_SEARCH);
    ui.set_find_result_index(0);
    load_filters(ui);
    recount(ui);
}

pub fn close(ui: &HomeWindow) {
    ui.set_find_open(false);
    crate::library::set_find_results(ui, "", "", String::new());
    if let Some(card) = STATE.with(|st| st.borrow_mut().return_grid.take()) {
        crate::library::open_grid(ui, &card);
    }
}

fn move_zone(ui: &HomeWindow, delta: i32) -> bool {
    let has_results = ui.get_find_results().row_count() > 0;
    let mut zone = ui.get_find_zone() + delta;
    if zone == ZONE_RESULTS && !has_results {
        zone += delta;
    }
    if !(ZONE_SEARCH..=ZONE_SURPRISE).contains(&zone) {
        return false;
    }
    ui.set_find_zone(zone);
    crate::library::sync_find_focus(ui);
    true
}

fn chip_confirm(ui: &HomeWindow) {
    let key = STATE.with(|st| chip_keys(&st.borrow().filters).get(ui.get_find_chip_index().max(0) as usize).copied())
        .unwrap_or("");
    let any = tr(ui, 833, "Any");
    let (options, current, title): (Vec<(String, String)>, String, String) = STATE.with(|st| {
        let st = st.borrow();
        match key {
            "system" => {
                let mut o = vec![(String::new(), any.clone())];
                o.extend(crate::library::system_names());
                (o, st.filters.system.clone(), tr(ui, 8, "System"))
            }
            "decade" => {
                let mut o = vec![("0".to_owned(), any.clone())];
                o.extend(st.decades.iter().map(|(y, n)| (y.to_string(), format!("{}–{} • {}", y, y + 9, n))));
                (o, st.filters.decade.to_string(), tr(ui, 832, "Decade"))
            }
            "genre" => {
                let mut o = vec![(String::new(), any.clone())];
                o.extend(st.genres.iter().map(|(g, n)| (g.clone(), format!("{g} • {n}"))));
                (o, st.filters.genre.clone(), tr(ui, 556, "Genre"))
            }
            "players" => {
                let mut o = vec![("0".to_owned(), any.clone())];
                o.extend((2..=st.most_players.clamp(2, 8)).map(|n| (n.to_string(), format!("{n}+"))));
                (o, st.filters.players.to_string(), tr(ui, 557, "Players"))
            }
            _ => (Vec::new(), String::new(), String::new()),
        }
    });
    match key {
        /* The collection scope has no other choices: confirming drops it. */
        "collection" => {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.filters.collection.clear();
                st.filters.collection_name.clear();
            });
            recount(ui);
        }
        "unplayed" => {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.filters.unplayed = !st.filters.unplayed;
            });
            recount(ui);
        }
        "clear" => {
            STATE.with(|st| st.borrow_mut().filters = Filters::default());
            ui.set_find_chip_index(0);
            recount(ui);
        }
        "" => {}
        _ => crate::library::open_choice_sheet(ui, format!("find-{key}"), title, options, current),
    }
}

/* Input while the Find view is open (the Home sheet and the system
 * keyboard first). */
pub fn find_action(ui: &HomeWindow, action: &str) {
    if ui.get_settings_view() == 6 {
        match action {
            "menu_left" => crate::move_keyboard_horizontal(ui, -1),
            "menu_right" => crate::move_keyboard_horizontal(ui, 1),
            "menu_up" => crate::move_keyboard_vertical(ui, -1),
            "menu_down" => crate::move_keyboard_vertical(ui, 1),
            "menu_confirm" => crate::handle_system_keyboard_confirm(ui),
            "face_north" => crate::keyboard_toggle_shift(ui),
            "face_west" => crate::keyboard_backspace(ui),
            "menu_back" => navigate_settings_view(ui, 0),
            _ => {}
        }
        return;
    }
    let moved = match action {
        "menu_up" => move_zone(ui, -1),
        "menu_down" => move_zone(ui, 1),
        "menu_left" | "menu_right" => {
            let delta = if action == "menu_left" { -1 } else { 1 };
            match ui.get_find_zone() {
                ZONE_CHIPS => {
                    let n = ui.get_find_chips().row_count() as i32;
                    let next = ui.get_find_chip_index() + delta;
                    (0..n).contains(&next) && { ui.set_find_chip_index(next); true }
                }
                ZONE_RESULTS => {
                    let n = ui.get_find_results().row_count() as i32;
                    let next = ui.get_find_result_index() + delta;
                    (0..n).contains(&next) && {
                        ui.set_find_result_index(next);
                        crate::library::sync_find_focus(ui);
                        true
                    }
                }
                ZONE_SURPRISE => {
                    let next = ui.get_find_surprise_index() + delta;
                    (0..2).contains(&next) && { ui.set_find_surprise_index(next); true }
                }
                _ => false,
            }
        }
        "menu_back" => {
            play_ui_sound(action);
            close(ui);
            return;
        }
        "menu_confirm" => {
            play_ui_sound(action);
            match ui.get_find_zone() {
                ZONE_SEARCH => {
                    let current = STATE.with(|st| st.borrow().filters.title.clone());
                    open_system_keyboard(ui, &tr(ui, 831, "Title"), KEYBOARD_TITLE, 0, "text", &current);
                }
                ZONE_CHIPS => chip_confirm(ui),
                ZONE_RESULTS => crate::library::find_confirm(ui),
                _ => random_pick(ui, ui.get_find_surprise_index() == 1),
            }
            return;
        }
        "face_north" if ui.get_find_zone() == ZONE_RESULTS => {
            play_ui_sound("menu_confirm");
            crate::library::find_details(ui);
            return;
        }
        _ => return,
    };
    if moved {
        play_ui_sound(action);
    }
}

pub fn leave(ui: &HomeWindow) {
    ACTIVE.store(false, Ordering::SeqCst);
    ui.set_settings_north_hint("".into());
    navigate_settings_view(ui, 0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_details_active(false);
    ui.set_details_shell_title("".into());
    ui.set_gen_rows(ModelRc::default());
    write_ui_context("home");
}

/* New Collection: from the Home card (None) or from Add to Collection
 * (the game to add). Straight to the keyboard; back to Home after. */
pub fn new_collection(ui: &HomeWindow, game: Option<(String, String)>) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.from_home = true;
        st.pending_game = game.map(|g| g.0);
    });
    enter(ui, COLLECTIONS_VIEW, tr(ui, 537, "Collections"));
    open_system_keyboard(ui, &tr(ui, 539, "Collection name"), KEYBOARD_NEW_COLLECTION, COLLECTIONS_VIEW, "text", "");
}

pub fn rename_collection(ui: &HomeWindow, id: String, name: String) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        st.from_home = true;
        st.rename = Some(id);
    });
    enter(ui, COLLECTIONS_VIEW, tr(ui, 537, "Collections"));
    open_system_keyboard(ui, &tr(ui, 539, "Collection name"), KEYBOARD_RENAME_COLLECTION, COLLECTIONS_VIEW, "text", &name);
}

fn back(ui: &HomeWindow) {
    leave(ui);
}

fn confirm(ui: &HomeWindow, key: &str) {
    match key {
        "new" => {
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.from_home = false;
                st.pending_game = None;
            });
            open_system_keyboard(ui, &tr(ui, 539, "Collection name"), KEYBOARD_NEW_COLLECTION, COLLECTIONS_VIEW, "text", "");
        }
        k if k.starts_with("c:") => {
            let id = k[2..].to_owned();
            let name = crate::library::collections().into_iter().find(|c| c.id == id).map(|c| c.name).unwrap_or_default();
            STATE.with(|st| {
                let mut st = st.borrow_mut();
                st.from_home = false;
                st.rename = Some(id);
            });
            open_system_keyboard(ui, &tr(ui, 539, "Collection name"), KEYBOARD_RENAME_COLLECTION, COLLECTIONS_VIEW, "text", &name);
        }
        _ => {}
    }
}

/* A filter chosen in the Home sheet. */
pub fn apply_choice(ui: &HomeWindow, context: &str, value: String) {
    STATE.with(|st| {
        let mut st = st.borrow_mut();
        match context {
            "find-system" => st.filters.system = value,
            "find-decade" => st.filters.decade = value.parse().unwrap_or(0),
            "find-genre" => st.filters.genre = value,
            "find-players" => st.filters.players = value.parse().unwrap_or(0),
            _ => {}
        }
    });
    recount(ui);
}

/* Input while the Find / Collections shell is open. */
pub fn handle_action(ui: &HomeWindow, action: &str, settings_active: &Arc<AtomicBool>) {
    if ui.get_settings_choice_open() || ui.get_settings_view() == 6 {
        handle_settings_action(ui, action, settings_active);
        return;
    }
    play_ui_sound(action);
    match action {
        "menu_up" | "menu_down" => move_selection(ui, action),
        "menu_back" => back(ui),
        "menu_confirm" => {
            let key = focused_key(ui);
            confirm(ui, &key);
        }
        /* Collections page: delete (second press). */
        "face_north" if ui.get_settings_view() == COLLECTIONS_VIEW => {
            let key = focused_key(ui);
            if key.starts_with("c:") && arm_or(ui, &key) {
                let id = key[2..].to_owned();
                command_then_render(ui, format!("COLLECTION_DELETE\t{id}"), Some(tr(ui, 848, "Collection deleted")));
            }
        }
        _ => {}
    }
}

fn command_then_render(ui: &HomeWindow, line: String, done: Option<String>) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let reply = crate::library::library_request(&line);
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            if ACTIVE.load(Ordering::SeqCst) {
                /* The library snapshot follows; render with what we know. */
                if let Some(text) = done.filter(|_| !reply.starts_with("ERR")) {
                    notice(&ui, text);
                }
                render(&ui);
            }
        });
    });
}

/* The library snapshot changed (a collection created/renamed/deleted). */
pub fn library_changed(ui: &HomeWindow) {
    if ACTIVE.load(Ordering::SeqCst) && ui.get_settings_view() == COLLECTIONS_VIEW {
        render(ui);
    }
}

/* System keyboard results. */
pub fn keyboard_done(ui: &HomeWindow, purpose: i32, value: String) {
    let name = clean(&value);
    match purpose {
        KEYBOARD_TITLE => {
            navigate_settings_view(ui, 0);
            STATE.with(|st| st.borrow_mut().filters.title = name);
            recount(ui);
        }
        KEYBOARD_NEW_COLLECTION | KEYBOARD_RENAME_COLLECTION => {
            let (from_home, game, rename) = STATE.with(|st| {
                let mut st = st.borrow_mut();
                (st.from_home, st.pending_game.take(), st.rename.take())
            });
            if from_home {
                leave(ui);
            } else {
                navigate_settings_view(ui, COLLECTIONS_VIEW);
                render(ui);
            }
            if name.is_empty() {
                return;
            }
            let weak = ui.as_weak();
            thread::spawn(move || {
                if purpose == KEYBOARD_RENAME_COLLECTION {
                    if let Some(id) = rename {
                        let _ = crate::library::library_request(&format!("COLLECTION_RENAME\t{id}\t{name}"));
                    }
                } else {
                    let reply = crate::library::library_request(&format!("COLLECTION_CREATE\t{name}"));
                    if let (Some(cid), Some(game)) = (reply.trim().strip_prefix("OK "), game) {
                        let _ = crate::library::library_request(&format!("COLLECTION_ADD\t{cid}\t{game}"));
                        let label = name.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                crate::library::notice(&ui, tr_arg(&ui, 846, "Added to {0}", &label));
                            }
                        });
                    }
                }
            });
        }
        _ => {}
    }
}

/* Keyboard Back: a flow started from Home goes back to Home. */
pub fn keyboard_cancelled(ui: &HomeWindow) -> bool {
    let from_home = STATE.with(|st| {
        let mut st = st.borrow_mut();
        let f = st.from_home;
        st.from_home = false;
        st.pending_game = None;
        st.rename = None;
        f
    });
    if from_home {
        leave(ui);
    }
    from_home
}
