/*
 * Text entry for Web Mode (EPIC-030) through the Wayland input method
 * protocol (zwp_input_method_v2, served by labwc/wlroots). When a text
 * field gets the focus in the browser (zwp_text_input_v3 in Cog), labwc
 * activates this input method; the Quick Menu then shows its controller
 * keyboard and commits UTF-8 text straight into the field, independent of
 * the keyboard layout. The protocol runs on its own connection and thread:
 * events become AppEvent::Ime, requests are sent from the UI thread.
 */

use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::thread;

use wayland_client::protocol::{wl_registry, wl_seat};
use wayland_client::{Connection, Dispatch, QueueHandle};

#[allow(dead_code, non_camel_case_types, non_upper_case_globals, non_snake_case, unused_imports, unused_variables,
    missing_docs, clippy::all)]
pub mod protocol {
    use wayland_client;
    use wayland_client::protocol::*;

    pub mod __interfaces {
        use wayland_client::protocol::__interfaces::*;
        wayland_scanner::generate_interfaces!("protocols/input-method-unstable-v2.xml");
    }
    use self::__interfaces::*;

    wayland_scanner::generate_client_code!("protocols/input-method-unstable-v2.xml");
}

use protocol::zwp_input_method_manager_v2::ZwpInputMethodManagerV2;
use protocol::zwp_input_method_v2::{self, ZwpInputMethodV2};

/* What the text field reported at the last `done`. */
#[derive(Debug, Clone, Default)]
pub struct ImeState {
    pub active: bool,
    /* Counts activations: a new field, not an update of the same one. */
    pub generation: u32,
    pub surrounding: String,
    pub cursor: usize,
}

pub struct Ime {
    conn: Connection,
    method: ZwpInputMethodV2,
    serial: Arc<AtomicU32>,
}

impl Ime {
    /* Inserts text at the cursor of the focused field. */
    pub fn commit_text(&self, text: &str) {
        self.method.commit_string(text.to_owned());
        self.method.commit(self.serial.load(Ordering::SeqCst));
        let _ = self.conn.flush();
    }

    /* Deletes the character before the cursor (lengths are in bytes). */
    pub fn delete_before(&self, state: &ImeState) {
        let before = state
            .surrounding
            .get(..state.cursor.min(state.surrounding.len()))
            .and_then(|text| text.chars().last())
            .map(|c| c.len_utf8())
            .unwrap_or(1);
        self.method.delete_surrounding_text(before as u32, 0);
        self.method.commit(self.serial.load(Ordering::SeqCst));
        let _ = self.conn.flush();
    }
}

struct Listener<E: From<ImeState> + Send + 'static> {
    tx: Sender<E>,
    seat: Option<wl_seat::WlSeat>,
    manager: Option<ZwpInputMethodManagerV2>,
    serial: Arc<AtomicU32>,
    pending: ImeState,
    current: ImeState,
    unavailable: bool,
}

impl<E: From<ImeState> + Send + 'static> Dispatch<wl_registry::WlRegistry, ()> for Listener<E> {
    fn event(state: &mut Self, registry: &wl_registry::WlRegistry, event: wl_registry::Event, _: &(),
        _: &Connection, qh: &QueueHandle<Self>) {
        if let wl_registry::Event::Global { name, interface, version } = event {
            match interface.as_str() {
                "wl_seat" if state.seat.is_none() => {
                    state.seat = Some(registry.bind::<wl_seat::WlSeat, _, _>(name, version.min(1), qh, ()));
                }
                "zwp_input_method_manager_v2" => {
                    state.manager = Some(registry.bind::<ZwpInputMethodManagerV2, _, _>(name, 1, qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl<E: From<ImeState> + Send + 'static> Dispatch<wl_seat::WlSeat, ()> for Listener<E> {
    fn event(_: &mut Self, _: &wl_seat::WlSeat, _: wl_seat::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
}

impl<E: From<ImeState> + Send + 'static> Dispatch<ZwpInputMethodManagerV2, ()> for Listener<E> {
    fn event(_: &mut Self, _: &ZwpInputMethodManagerV2, _: <ZwpInputMethodManagerV2 as wayland_client::Proxy>::Event,
        _: &(), _: &Connection, _: &QueueHandle<Self>) {}
}

impl<E: From<ImeState> + Send + 'static> Dispatch<ZwpInputMethodV2, ()> for Listener<E> {
    fn event(state: &mut Self, _: &ZwpInputMethodV2, event: zwp_input_method_v2::Event, _: &(), _: &Connection,
        _: &QueueHandle<Self>) {
        match event {
            zwp_input_method_v2::Event::Activate => {
                /* A new activation starts from a clean state. */
                state.pending = ImeState {
                    active: true,
                    generation: state.current.generation.wrapping_add(1),
                    ..ImeState::default()
                };
            }
            zwp_input_method_v2::Event::Deactivate => {
                state.pending.active = false;
            }
            zwp_input_method_v2::Event::SurroundingText { text, cursor, .. } => {
                state.pending.surrounding = text;
                state.pending.cursor = cursor as usize;
            }
            zwp_input_method_v2::Event::Done => {
                state.serial.fetch_add(1, Ordering::SeqCst);
                let changed = state.pending.active != state.current.active
                    || state.pending.generation != state.current.generation
                    || state.pending.surrounding != state.current.surrounding
                    || state.pending.cursor != state.current.cursor;
                state.current = state.pending.clone();
                if changed {
                    let _ = state.tx.send(E::from(state.current.clone()));
                }
            }
            zwp_input_method_v2::Event::Unavailable => {
                /* Another input method owns the seat. */
                state.unavailable = true;
            }
            _ => {}
        }
    }
}

/* Registers the input method; None when the compositor has none. */
pub fn start<E: From<ImeState> + Send + 'static>(tx: Sender<E>) -> Option<Arc<Ime>> {
    let conn = Connection::connect_to_env().ok()?;
    let mut queue = conn.new_event_queue::<Listener<E>>();
    let qh = queue.handle();
    let serial = Arc::new(AtomicU32::new(0));
    let mut listener = Listener {
        tx,
        seat: None,
        manager: None,
        serial: serial.clone(),
        pending: ImeState::default(),
        current: ImeState::default(),
        unavailable: false,
    };
    conn.display().get_registry(&qh, ());
    queue.roundtrip(&mut listener).ok()?;
    let (Some(seat), Some(manager)) = (listener.seat.clone(), listener.manager.clone()) else {
        eprintln!("quick-menu: no input method support in the compositor");
        return None;
    };
    let method = manager.get_input_method(&seat, &qh, ());
    queue.roundtrip(&mut listener).ok()?;
    if listener.unavailable {
        eprintln!("quick-menu: another input method is active");
        return None;
    }
    let ime = Arc::new(Ime { conn, method, serial });
    thread::spawn(move || loop {
        if queue.blocking_dispatch(&mut listener).is_err() || listener.unavailable {
            eprintln!("quick-menu: input method connection ended");
            let _ = listener.tx.send(E::from(ImeState::default()));
            return;
        }
    });
    Some(ime)
}
