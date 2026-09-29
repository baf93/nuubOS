slint::include_modules!();

use slint::platform::{
    self,
    software_renderer::{MinimalSoftwareWindow, RepaintBufferType},
    Platform, PlatformError, WindowAdapter,
};
use slint::{ComponentHandle, PhysicalSize};

use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Seek, SeekFrom, Write};
use std::os::fd::AsFd;
use std::os::unix::net::UnixStream;
use std::process::{Command, Stdio};
use std::rc::Rc;
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread;
use std::time::{Duration, Instant};

use wayland_client::{
    delegate_noop,
    protocol::{
        wl_buffer, wl_compositor, wl_region, wl_registry, wl_shm, wl_shm_pool,
        wl_surface,
    },
    Connection, Dispatch, EventQueue, Proxy, QueueHandle,
};

use wayland_protocols_wlr::layer_shell::v1::client::{
    zwlr_layer_shell_v1::{self, Layer, ZwlrLayerShellV1},
    zwlr_layer_surface_v1::{
        self, Anchor, KeyboardInteractivity, ZwlrLayerSurfaceV1,
    },
};

const INPUT_SOCKET: &str = "/run/nuubos/inputd.sock";
const PIDFILE: &str = "/run/nuubos/quick-menu.pid";
const MAPPED_FILE: &str = "/run/nuubos/quick-menu-mapped";
const SURFACE_FILE: &str = "/run/nuubos/quick-menu-surface";
const STATE_FILE: &str = "/run/nuubos/quick-menu-state";
const ACTION_LOG: &str = "/run/nuubos/quick-menu-action.log";
const DISPLAYCTL: &str = "/usr/bin/nuubos-displayctl";
const STATUS_STATE: &str = "/run/nuubos/statusd.state";
const STATUS_SOCKET: &str = "/run/nuubos/statusd.sock";


const LINE_STAGE_MS: u64 = 220;
const CURTAIN_STAGE_MS: u64 = 460;
const STAGE_GAP_MS: u64 = 18;
const FRAME_INTERVAL_NS: u64 = 16_666_667;

struct QuickPlatform {
    window: Rc<MinimalSoftwareWindow>,
    started: Instant,
}

impl Platform for QuickPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        self.started.elapsed()
    }
}

struct Frame {
    buffer: wl_buffer::WlBuffer,
    _file: File,
}

struct WaylandState {
    compositor: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    layer_shell: Option<ZwlrLayerShellV1>,
    surface: Option<wl_surface::WlSurface>,
    layer_surface: Option<ZwlrLayerSurfaceV1>,
    configured_size: Option<(u32, u32)>,
    closed: bool,
    frames: Vec<Frame>,
    frame_seq: u64,
}

impl WaylandState {
    fn new() -> Self {
        Self {
            compositor: None,
            shm: None,
            layer_shell: None,
            surface: None,
            layer_surface: None,
            configured_size: None,
            closed: false,
            frames: Vec::new(),
            frame_seq: 0,
        }
    }

    fn globals_ready(&self) -> bool {
        self.compositor.is_some()
            && self.shm.is_some()
            && self.layer_shell.is_some()
    }

    fn create_overlay(
        &mut self,
        qh: &QueueHandle<Self>,
    ) -> Result<(), Box<dyn std::error::Error>> {
        /*
         * Drop any previous overlay before borrowing the globals used to
         * create the replacement. Wayland proxies are cheap cloneable
         * handles, so keep owned handles across later state mutation.
         */
        self.destroy_overlay();

        let compositor = self
            .compositor
            .as_ref()
            .cloned()
            .ok_or("wl_compositor unavailable")?;
        let layer_shell = self
            .layer_shell
            .as_ref()
            .cloned()
            .ok_or("zwlr_layer_shell_v1 unavailable")?;

        let surface = compositor.create_surface(qh, ());

        /*
         * Controller input is owned by InputService. The fullscreen layer
         * surface must not steal pointer/touch input from the application
         * underneath it.
         */
        let empty_region = compositor.create_region(qh, ());
        surface.set_input_region(Some(&empty_region));
        empty_region.destroy();

        let layer_surface = layer_shell.get_layer_surface(
            &surface,
            None,
            Layer::Overlay,
            "nuubOS Quick Menu".into(),
            qh,
            (),
        );

        layer_surface.set_size(0, 0);
        layer_surface.set_anchor(
            Anchor::Top | Anchor::Bottom | Anchor::Left | Anchor::Right,
        );
        layer_surface.set_exclusive_zone(-1);
        layer_surface.set_keyboard_interactivity(
            KeyboardInteractivity::None,
        );

        self.surface = Some(surface.clone());
        self.layer_surface = Some(layer_surface);
        self.configured_size = None;
        self.closed = false;

        /*
         * Layer-shell requires one initial commit with no buffer, followed
         * by configure/ack, before the first mapped buffer commit.
         */
        surface.commit();

        Ok(())
    }

    fn destroy_overlay(&mut self) {
        let _ = fs::remove_file(MAPPED_FILE);
        let _ = fs::remove_file(SURFACE_FILE);

        if let Some(surface) = self.surface.as_ref() {
            surface.attach(None, 0, 0);
            surface.commit();
        }

        if let Some(layer_surface) = self.layer_surface.take() {
            layer_surface.destroy();
        }

        if let Some(surface) = self.surface.take() {
            surface.destroy();
        }

        for frame in self.frames.drain(..) {
            frame.buffer.destroy();
        }

        self.configured_size = None;
        self.closed = false;
    }

    fn render_and_commit(
        &mut self,
        ui: &QuickMenuWindow,
        qh: &QueueHandle<Self>,
        conn: &Connection,
    ) -> Result<(u32, u32), Box<dyn std::error::Error>> {
        let (width, height) = self
            .configured_size
            .ok_or("layer surface not configured")?;

        if width == 0 || height == 0 {
            return Err("compositor returned zero-sized layer surface".into());
        }

        ui.window()
            .set_size(PhysicalSize::new(width, height));

        platform::update_timers_and_animations();

        let snapshot = ui.window().take_snapshot()?;

        if snapshot.width() != width || snapshot.height() != height {
            return Err(format!(
                "Slint snapshot {}x{} does not match configured {}x{}",
                snapshot.width(),
                snapshot.height(),
                width,
                height
            )
            .into());
        }

        let pixel_count = (width as usize) * (height as usize);
        let mut argb = Vec::with_capacity(pixel_count * 4);

        /*
         * wl_shm ARGB8888 on little-endian memory is B,G,R,A and channels
         * must be premultiplied by alpha.
         */
        for pixel in snapshot.as_slice() {
            let a = pixel.a as u16;
            let premul = |channel: u8| -> u8 {
                (((channel as u16) * a + 127) / 255) as u8
            };

            argb.push(premul(pixel.b));
            argb.push(premul(pixel.g));
            argb.push(premul(pixel.r));
            argb.push(pixel.a);
        }

        self.frame_seq += 1;
        let backing_path = format!(
            "/run/nuubos/quick-menu-shm-{}-{}",
            std::process::id(),
            self.frame_seq
        );

        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&backing_path)?;

        file.set_len(argb.len() as u64)?;
        file.seek(SeekFrom::Start(0))?;
        file.write_all(&argb)?;
        file.flush()?;

        /*
         * Unlink immediately: the local fd and the compositor's SCM_RIGHTS
         * copy keep the backing store alive without leaving runtime debris.
         */
        fs::remove_file(&backing_path)?;

        let shm = self.shm.as_ref().ok_or("wl_shm unavailable")?;
        let pool = shm.create_pool(
            file.as_fd(),
            argb.len() as i32,
            qh,
            (),
        );

        let buffer = pool.create_buffer(
            0,
            width as i32,
            height as i32,
            (width * 4) as i32,
            wl_shm::Format::Argb8888,
            qh,
            (),
        );

        pool.destroy();

        let surface = self
            .surface
            .as_ref()
            .ok_or("layer surface disappeared")?;

        surface.attach(Some(&buffer), 0, 0);
        surface.damage(0, 0, width as i32, height as i32);
        surface.commit();
        conn.flush()?;

        self.frames.push(Frame {
            buffer,
            _file: file,
        });

        Ok((width, height))
    }
}

impl Dispatch<wl_registry::WlRegistry, ()> for WaylandState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "wl_compositor" => {
                    state.compositor = Some(
                        registry.bind::<
                            wl_compositor::WlCompositor,
                            _,
                            _
                        >(name, version.min(4), qh, ()),
                    );
                }
                "wl_shm" => {
                    state.shm = Some(
                        registry.bind::<wl_shm::WlShm, _, _>(
                            name,
                            version.min(1),
                            qh,
                            (),
                        ),
                    );
                }
                "zwlr_layer_shell_v1" => {
                    state.layer_shell = Some(
                        registry.bind::<ZwlrLayerShellV1, _, _>(
                            name,
                            version.min(4),
                            qh,
                            (),
                        ),
                    );
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<ZwlrLayerSurfaceV1, ()> for WaylandState {
    fn event(
        state: &mut Self,
        layer_surface: &ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                layer_surface.ack_configure(serial);
                state.configured_size = Some((width, height));
            }
            zwlr_layer_surface_v1::Event::Closed => {
                state.closed = true;
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_buffer::WlBuffer, ()> for WaylandState {
    fn event(
        state: &mut Self,
        buffer: &wl_buffer::WlBuffer,
        event: wl_buffer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_buffer::Event::Release = event {
            let id = buffer.id();
            buffer.destroy();
            state.frames.retain(|frame| frame.buffer.id() != id);
        }
    }
}

delegate_noop!(WaylandState: ignore wl_compositor::WlCompositor);
delegate_noop!(WaylandState: ignore wl_region::WlRegion);
delegate_noop!(WaylandState: ignore wl_surface::WlSurface);
delegate_noop!(WaylandState: ignore wl_shm::WlShm);
delegate_noop!(WaylandState: ignore wl_shm_pool::WlShmPool);
delegate_noop!(WaylandState: ignore zwlr_layer_shell_v1::ZwlrLayerShellV1);

fn input_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(INPUT_SOCKET)?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;

    let mut reader = BufReader::new(stream);
    let mut reply = String::new();
    reader.read_line(&mut reply)?;
    Ok(reply)
}

fn set_menu_capture(enabled: bool) {
    let command = if enabled { "MENU OPEN" } else { "MENU CLOSE" };

    match input_command(command) {
        Ok(reply) => {
            eprintln!(
                "quick-menu: capture enabled={} reply={:?}",
                enabled,
                reply.trim_end()
            );
        }
        Err(error) => {
            eprintln!(
                "quick-menu: capture enabled={} failed={}",
                enabled,
                error
            );
        }
    }
}

fn append_action_log(line: &str) {
    let mut file = match OpenOptions::new()
        .create(true)
        .append(true)
        .open(ACTION_LOG)
    {
        Ok(file) => file,
        Err(_) => return,
    };

    let _ = writeln!(file, "{}", line);
    let _ = file.flush();
}

fn spawn_logged_action(
    action: &'static str,
    path: &'static str,
    lifecycle_preanimated: bool,
) {
    append_action_log(&format!("action={} phase=start", action));

    thread::spawn(move || {
        let mut command = Command::new(path);

        command
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());

        if lifecycle_preanimated {
            command.env("NUUBOS_LIFECYCLE_PREANIMATED", "1");
        }

        match command.status() {
            Ok(status) => append_action_log(&format!(
                "action={} phase=finish status={}",
                action,
                status.code().unwrap_or(-1)
            )),
            Err(error) => append_action_log(&format!(
                "action={} phase=error error={}",
                action,
                error
            )),
        }
    });
}

fn run_suspend_sync() -> Result<(), Box<dyn std::error::Error>> {
    /*
     * Sleep deliberately follows the previously-qualified Power1 model:
     * invoke the canonical platform-owned helper synchronously.
     *
     * Reboot/Poweroff are terminal actions and can remain detached. Suspend
     * is different: its completion is the resume boundary. Keeping it on the
     * Quick Menu control thread gives us one deterministic transaction:
     *
     *   confirm release
     *     -> Overlay unmap + controller capture release
     *     -> /usr/sbin/nuubos-suspend
     *     -> kernel s2idle/freeze
     *     -> resume
     *     -> return here
     */
    append_action_log("action=sleep phase=invoke-sync");

    let stdout_log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(ACTION_LOG)?;
    let stderr_log = stdout_log.try_clone()?;

    let status = Command::new("/usr/sbin/nuubos-suspend")
        .stdin(Stdio::null())
        .stdout(Stdio::from(stdout_log))
        .stderr(Stdio::from(stderr_log))
        .status()?;

    append_action_log(&format!(
        "action=sleep phase=return-sync status={}",
        status.code().unwrap_or(-1)
    ));

    if !status.success() {
        return Err(format!(
            "nuubos-suspend returned non-zero status {:?}",
            status.code()
        )
        .into());
    }

    Ok(())
}


fn binding_code(action: &str) -> Option<i32> {
    let reply = input_command(&format!("BIND GET {}", action)).ok()?;
    reply.trim().parse::<i32>().ok()
}

/*
 * Linux BTN_* semantic face positions, not Xbox/Nintendo letters:
 * 0 top, 1 right, 2 bottom, 3 left.
 */
fn face_position(code: i32) -> i32 {
    match code {
        307 => 0, // BTN_NORTH
        305 => 1, // BTN_EAST
        304 => 2, // BTN_SOUTH
        308 => 3, // BTN_WEST
        _ => -1,
    }
}

fn refresh_hint_mapping(ui: &QuickMenuWindow) {
    ui.set_confirm_face_position(
        binding_code("menu_confirm")
            .map(face_position)
            .unwrap_or(-1),
    );

    ui.set_back_face_position(
        binding_code("menu_back")
            .map(face_position)
            .unwrap_or(-1),
    );
}


#[derive(Debug, Clone, Copy)]
struct DisplaySnapshot {
    hdmi_connected: Option<bool>,
    brightness: i32,
}

#[derive(Debug)]
enum AppEvent {
    Input(LogicalEvent),
    StatusChanged,
}


#[derive(Debug, Clone)]
struct BatterySnapshot {
    percent: i32,
    state: String,
}

fn read_battery_snapshot() -> BatterySnapshot {
    let mut snapshot = BatterySnapshot {
        percent: -1,
        state: "unknown".to_owned(),
    };

    let Ok(contents) = fs::read_to_string(STATUS_STATE) else {
        return snapshot;
    };

    for line in contents.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key {
            "BATTERY_PERCENT" => {
                snapshot.percent = value.parse::<i32>().unwrap_or(-1).clamp(-1, 100);
            }
            "BATTERY_STATE" => snapshot.state = value.to_owned(),
            _ => {}
        }
    }

    snapshot
}

fn apply_battery_snapshot(ui: &QuickMenuWindow, snapshot: BatterySnapshot) {
    ui.set_battery_percent(snapshot.percent);
    ui.set_battery_state(snapshot.state.into());
    ui.set_battery_label(
        if snapshot.percent >= 0 {
            format!("{}%", snapshot.percent).into()
        } else {
            "--%".into()
        },
    );
}

fn start_status_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || {
        let Ok(stream) = UnixStream::connect(STATUS_SOCKET) else {
            eprintln!("quick-menu: status service unavailable");
            return;
        };

        let mut reader = BufReader::new(stream);
        let mut line = String::new();
        loop {
            line.clear();
            match reader.read_line(&mut line) {
                Ok(0) => break,
                Ok(_) if line.trim() == "changed" => {
                    if tx.send(AppEvent::StatusChanged).is_err() {
                        return;
                    }
                }
                Ok(_) => {}
                Err(error) => {
                    eprintln!("quick-menu: status read failed={}", error);
                    break;
                }
            }
        }
    });
}

fn read_hdmi_connected() -> Option<bool> {
    let output = match Command::new(DISPLAYCTL)
        .arg("hdmi")
        .stdin(Stdio::null())
        .output()
    {
        Ok(output) => output,
        Err(error) => {
            eprintln!("quick-menu: HDMI query failed={}", error);
            return None;
        }
    };

    if !output.status.success() {
        eprintln!(
            "quick-menu: HDMI query status={:?}: {}",
            output.status.code(),
            String::from_utf8_lossy(&output.stderr).trim()
        );
        return None;
    }

    match String::from_utf8_lossy(&output.stdout).trim() {
        "connected" => Some(true),
        "disconnected" => Some(false),
        other => {
            eprintln!("quick-menu: unexpected HDMI state={:?}", other);
            None
        }
    }
}

fn read_brightness() -> i32 {
    let output = match Command::new(DISPLAYCTL)
        .arg("brightness")
        .stdin(Stdio::null())
        .output()
    {
        Ok(output) => output,
        Err(error) => {
            eprintln!("quick-menu: brightness query failed={}", error);
            return 60;
        }
    };

    if !output.status.success() {
        eprintln!(
            "quick-menu: brightness query status={:?}",
            output.status.code()
        );
        return 60;
    }

    String::from_utf8_lossy(&output.stdout)
        .trim()
        .parse::<i32>()
        .unwrap_or(60)
        .clamp(1, 100)
}

fn read_display_snapshot() -> DisplaySnapshot {
    DisplaySnapshot {
        hdmi_connected: read_hdmi_connected(),
        brightness: read_brightness(),
    }
}

fn set_brightness_ui(ui: &QuickMenuWindow, brightness: i32) {
    let brightness = brightness.clamp(1, 100);
    ui.set_brightness_value(brightness);
    ui.set_brightness_fraction(brightness as f32 / 100.0);
    ui.set_brightness_label(format!("{}%", brightness).into());
}

fn apply_display_snapshot(ui: &QuickMenuWindow, snapshot: DisplaySnapshot) {
    let visible = snapshot.hdmi_connected == Some(false);
    let hdmi = match snapshot.hdmi_connected {
        Some(true) => "connected",
        Some(false) => "disconnected",
        None => "unknown",
    };

    ui.set_brightness_visible(visible);
    set_brightness_ui(ui, snapshot.brightness);
    append_action_log(&format!(
        "display-state hdmi={} brightness={} visible={}",
        hdmi,
        snapshot.brightness.clamp(1, 100),
        if visible { 1 } else { 0 }
    ));

    if !visible && ui.get_selected_index() == 3 {
        ui.set_selected_index(2);
    }
}

fn set_brightness_live(percent: i32) -> Result<i32, Box<dyn std::error::Error>> {
    let percent = percent.clamp(1, 100);
    let output = Command::new(DISPLAYCTL)
        .arg("brightness-live")
        .arg(percent.to_string())
        .stdin(Stdio::null())
        .output()?;

    if !output.status.success() {
        return Err(format!(
            "nuubos-displayctl brightness-live failed with status {:?}: {}",
            output.status.code(),
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }

    let applied = String::from_utf8_lossy(&output.stdout)
        .trim()
        .parse::<i32>()?
        .clamp(1, 100);

    Ok(applied)
}

fn persist_brightness(percent: i32) -> Result<i32, Box<dyn std::error::Error>> {
    let percent = percent.clamp(1, 100);
    let output = Command::new(DISPLAYCTL)
        .arg("brightness")
        .arg(percent.to_string())
        .stdin(Stdio::null())
        .output()?;

    if !output.status.success() {
        return Err(format!(
            "nuubos-displayctl brightness persist failed with status {:?}: {}",
            output.status.code(),
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }

    let applied = String::from_utf8_lossy(&output.stdout)
        .trim()
        .parse::<i32>()?
        .clamp(1, 100);

    Ok(applied)
}

fn flush_brightness(
    ui: &QuickMenuWindow,
    dirty: &mut bool,
) -> Result<(), Box<dyn std::error::Error>> {
    if !*dirty {
        return Ok(());
    }

    let requested = ui.get_brightness_value().clamp(1, 100);
    let persisted = persist_brightness(requested)?;
    set_brightness_ui(ui, persisted);
    *dirty = false;
    append_action_log(&format!(
        "brightness-commit requested={} persisted={}",
        requested, persisted
    ));
    Ok(())
}

fn first_selectable_index(ui: &QuickMenuWindow) -> i32 {
    /*
     * Visual order is CONTEXT (future selectable actions), DISPLAY/AUDIO,
     * then SYSTEM. Today Brightness is the first selectable row whenever
     * the internal panel is active; otherwise Sleep is first.
     */
    if ui.get_brightness_visible() { 3 } else { 0 }
}

fn write_menu_state(ui: &QuickMenuWindow, mapped: bool) {
    if !mapped {
        let _ = fs::remove_file(STATE_FILE);
        return;
    }

    let _ = fs::write(
        STATE_FILE,
        format!(
            "mapped=1\nselected={}\nconfirm_face={}\nback_face={}\nbrightness_visible={}\nbrightness={}\n",
            ui.get_selected_index(),
            ui.get_confirm_face_position(),
            ui.get_back_face_position(),
            if ui.get_brightness_visible() { 1 } else { 0 },
            ui.get_brightness_value()
        ),
    );
}

#[derive(Debug)]
struct LogicalEvent {
    action: String,
    pressed: bool,
}

fn start_input_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        match UnixStream::connect(INPUT_SOCKET) {
            Ok(mut stream) => {
                eprintln!("quick-menu: input service connected");

                if let Err(error) = stream.write_all(b"SUBSCRIBE QUICK_MENU\n") {
                    eprintln!(
                        "quick-menu: subscribe write failed={}",
                        error
                    );
                    thread::sleep(Duration::from_millis(250));
                    continue;
                }

                let mut reader = BufReader::new(stream);
                let mut line = String::new();

                loop {
                    line.clear();

                    match reader.read_line(&mut line) {
                        Ok(0) => {
                            eprintln!("quick-menu: input stream EOF");
                            break;
                        }
                        Ok(_) => {
                            eprintln!(
                                "quick-menu: input-line={:?}",
                                line.trim_end()
                            );

                            let fields: Vec<&str> =
                                line.split_whitespace().collect();

                            if fields.len() == 4
                                && fields[0] == "EVENT"
                                && fields[1] == "1"
                                && (fields[3] == "pressed"
                                    || fields[3] == "released")
                            {
                                if tx
                                    .send(AppEvent::Input(LogicalEvent {
                                        action: fields[2].to_owned(),
                                        pressed: fields[3] == "pressed",
                                    }))
                                    .is_err()
                                {
                                    return;
                                }
                            }
                        }
                        Err(error) => {
                            eprintln!(
                                "quick-menu: input read failed={}",
                                error
                            );
                            break;
                        }
                    }
                }
            }
            Err(error) => {
                eprintln!(
                    "quick-menu: input connect failed={}",
                    error
                );
            }
        }

        thread::sleep(Duration::from_millis(250));
    });
}

fn wait_for_configure(
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
) -> Result<(u32, u32), Box<dyn std::error::Error>> {
    while state.configured_size.is_none() && !state.closed {
        queue.blocking_dispatch(state)?;
    }

    if state.closed {
        return Err("layer surface closed before configure".into());
    }

    state
        .configured_size
        .ok_or_else(|| "missing layer configure".into())
}

fn map_overlay(
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    let _ = fs::remove_file(MAPPED_FILE);
    let _ = fs::remove_file(SURFACE_FILE);

    ui.set_lifecycle_active(false);
    ui.set_lifecycle_mode("".into());
    ui.set_line_progress(1.0);
    ui.set_curtain_progress(1.0);
    refresh_hint_mapping(ui);
    apply_display_snapshot(ui, read_display_snapshot());
    apply_battery_snapshot(ui, read_battery_snapshot());
    ui.set_selected_index(first_selectable_index(ui));

    state.create_overlay(qh)?;
    conn.flush()?;

    let (width, height) = wait_for_configure(queue, state)?;

    ui.show()?;

    let (rendered_width, rendered_height) =
        state.render_and_commit(ui, qh, conn)?;

    /*
     * A sync roundtrip after the buffer commit means the compositor has
     * processed the configure ack and the mapped-buffer commit. Only now
     * is the protocol-level mapped marker published.
     */
    queue.roundtrip(state)?;

    if state.closed {
        return Err("layer surface closed during first mapped commit".into());
    }

    fs::write(
        SURFACE_FILE,
        format!(
            "role=wlr-layer-shell\nlayer=overlay\nconfigured={}x{}\nrendered={}x{}\ncommit=confirmed-roundtrip\n",
            width,
            height,
            rendered_width,
            rendered_height
        ),
    )?;

    fs::write(MAPPED_FILE, b"mapped\n")?;
    write_menu_state(ui, true);
    set_menu_capture(true);

    eprintln!(
        "quick-menu: mapped layer=overlay size={}x{}",
        rendered_width,
        rendered_height
    );

    Ok(())
}

fn redraw_overlay(
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    state.render_and_commit(ui, qh, conn)?;
    queue.roundtrip(state)?;
    write_menu_state(ui, true);
    Ok(())
}

fn ease_in_out(progress: f32) -> f32 {
    let p = progress.clamp(0.0, 1.0);
    p * p * (3.0 - 2.0 * p)
}

fn pace_animation_frame(next_deadline: &mut Instant) {
    *next_deadline += Duration::from_nanos(FRAME_INTERVAL_NS);

    if let Some(remaining) = next_deadline.checked_duration_since(Instant::now()) {
        thread::sleep(remaining);
    } else {
        /*
         * Rendering/Wayland commit consumed the frame budget. Do not add a
         * second fixed sleep: time-based progress will naturally catch up on
         * the next frame without stretching the lifecycle animation.
         */
        *next_deadline = Instant::now();
    }
}

fn animate_shutdown(
    mode: &str,
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    ui.set_lifecycle_mode(mode.into());
    ui.set_lifecycle_active(true);
    ui.set_line_progress(1.0);
    ui.set_curtain_progress(1.0);
    redraw_overlay(ui, queue, state, qh, conn)?;

    let curtain_start = Instant::now();
    let mut curtain_deadline = curtain_start;

    loop {
        let elapsed = curtain_start.elapsed().as_millis() as u64;
        let progress = (elapsed as f32 / CURTAIN_STAGE_MS as f32).min(1.0);
        ui.set_curtain_progress(1.0 - ease_in_out(progress));
        redraw_overlay(ui, queue, state, qh, conn)?;

        if progress >= 1.0 {
            break;
        }

        pace_animation_frame(&mut curtain_deadline);
    }

    thread::sleep(Duration::from_millis(STAGE_GAP_MS));

    let line_start = Instant::now();
    let mut line_deadline = line_start;

    loop {
        let elapsed = line_start.elapsed().as_millis() as u64;
        let progress = (elapsed as f32 / LINE_STAGE_MS as f32).min(1.0);
        ui.set_line_progress(1.0 - ease_in_out(progress));
        redraw_overlay(ui, queue, state, qh, conn)?;

        if progress >= 1.0 {
            break;
        }

        pace_animation_frame(&mut line_deadline);
    }

    /*
     * Hold the exact qualified final lifecycle raster on the Overlay layer.
     * The lifecycle finalizer preserves this KMS scanout while Labwc exits.
     */
    ui.set_curtain_progress(0.0);
    ui.set_line_progress(0.0);
    redraw_overlay(ui, queue, state, qh, conn)?;

    Ok(())
}

fn unmap_overlay(
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    conn: &Connection,
) {
    let _ = fs::remove_file(MAPPED_FILE);
    let _ = fs::remove_file(SURFACE_FILE);
    let _ = fs::remove_file(STATE_FILE);

    if let Some(surface) = state.surface.as_ref() {
        surface.attach(None, 0, 0);
        surface.commit();
        let _ = conn.flush();
        let _ = queue.roundtrip(state);
    }

    state.destroy_overlay();
    let _ = ui.hide();
    set_menu_capture(false);

    eprintln!("quick-menu: unmapped");
}

fn activate_selected(
    ui: &QuickMenuWindow,
    mapped: &mut bool,
    pending_sleep: &mut bool,
    brightness_dirty: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    match ui.get_selected_index() {
        0 => {
            /*
             * Sleep is committed on RELEASE, not on press.  Remembering the
             * press is diagnostic only: the release handler below is the
             * authoritative trigger while Sleep is selected and mapped.
             *
             * This matches the previously-qualified UX invariant that the
             * physical confirm button must be up before entering s2idle,
             * without making suspend depend on a press-side state bit.
             */
            *pending_sleep = true;
            append_action_log(
                "action=sleep phase=confirm-press-observed capture=held",
            );
        }
        1 => {
            flush_brightness(ui, brightness_dirty)?;
            set_menu_capture(false);
            animate_shutdown("reboot", ui, queue, state, qh, conn)?;
            spawn_logged_action("reboot", "/sbin/reboot", true);
        }
        2 => {
            flush_brightness(ui, brightness_dirty)?;
            set_menu_capture(false);
            animate_shutdown("poweroff", ui, queue, state, qh, conn)?;
            spawn_logged_action("poweroff", "/sbin/poweroff", true);
        }
        _ => {}
    }

    Ok(())
}

fn handle_event(
    event: &LogicalEvent,
    ui: &QuickMenuWindow,
    mapped: &mut bool,
    pending_sleep: &mut bool,
    brightness_dirty: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    if !event.pressed {
        if (event.action == "menu_left" || event.action == "menu_right")
            && *mapped
        {
            flush_brightness(ui, brightness_dirty)?;
            return Ok(());
        }

        if event.action == "menu_confirm"
            && *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == 0
        {
            let press_seen = *pending_sleep;
            *pending_sleep = false;

            append_action_log(&format!(
                "action=sleep phase=confirm-release-trigger press-seen={} capture=held",
                if press_seen { 1 } else { 0 }
            ));

            /*
             * RELEASE is the authoritative Sleep trigger.  At this point the
             * physical face button is definitely up.  Tear down the Overlay
             * and controller grab, complete the Wayland roundtrip, then call
             * the canonical platform suspend helper synchronously.
             */
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;

            append_action_log(
                "action=sleep phase=overlay-unmapped-after-confirm-release",
            );

            run_suspend_sync()?;
        } else if event.action == "menu_confirm" {
            /*
             * Never carry a stale press transaction into a later menu state.
             */
            *pending_sleep = false;
        }

        return Ok(());
    }

    match event.action.as_str() {
        "quick_menu" => {
            if *mapped {
                flush_brightness(ui, brightness_dirty)?;
                unmap_overlay(ui, queue, state, conn);
                *mapped = false;
            } else {
                map_overlay(ui, queue, state, qh, conn)?;
                *mapped = true;
            }
        }
        "menu_back" if *mapped => {
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
        }
        "menu_up" if *mapped && !ui.get_lifecycle_active() => {
            let current = ui.get_selected_index();
            let last = if ui.get_brightness_visible() { 3 } else { 2 };
            ui.set_selected_index(if current <= 0 { last } else { current - 1 });
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_down" if *mapped && !ui.get_lifecycle_active() => {
            let current = ui.get_selected_index();
            let last = if ui.get_brightness_visible() { 3 } else { 2 };
            ui.set_selected_index(if current >= last { 0 } else { current + 1 });
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_left" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_brightness_visible()
            && ui.get_selected_index() == 3 =>
        {
            let before = ui.get_brightness_value();
            let requested = (before - 1).clamp(1, 100);
            if requested != before {
                let applied = set_brightness_live(requested)?;
                set_brightness_ui(ui, applied);
                *brightness_dirty = true;
                append_action_log(&format!(
                    "brightness-change action=menu_left before={} after={} phase=live",
                    before, applied
                ));
                redraw_overlay(ui, queue, state, qh, conn)?;
            }
        }
        "menu_right" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_brightness_visible()
            && ui.get_selected_index() == 3 =>
        {
            let before = ui.get_brightness_value();
            let requested = (before + 1).clamp(1, 100);
            if requested != before {
                let applied = set_brightness_live(requested)?;
                set_brightness_ui(ui, applied);
                *brightness_dirty = true;
                append_action_log(&format!(
                    "brightness-change action=menu_right before={} after={} phase=live",
                    before, applied
                ));
                redraw_overlay(ui, queue, state, qh, conn)?;
            }
        }
        "menu_confirm" if *mapped && !ui.get_lifecycle_active() => {
            activate_selected(
                ui,
                mapped,
                pending_sleep,
                brightness_dirty,
                queue,
                state,
                qh,
                conn,
            )?;
        }
        _ => {}
    }

    Ok(())
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    fs::create_dir_all("/run/nuubos")?;
    fs::write(PIDFILE, format!("{}\n", std::process::id()))?;
    let _ = fs::remove_file(MAPPED_FILE);
    let _ = fs::remove_file(SURFACE_FILE);

    let slint_window =
        MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);

    platform::set_platform(Box::new(QuickPlatform {
        window: slint_window,
        started: Instant::now(),
    }))?;

    let ui = QuickMenuWindow::new()?;

    let conn = Connection::connect_to_env()?;
    let mut queue = conn.new_event_queue::<WaylandState>();
    let qh = queue.handle();

    let mut state = WaylandState::new();
    conn.display().get_registry(&qh, ());
    queue.roundtrip(&mut state)?;

    if !state.globals_ready() {
        return Err(
            "Labwc does not advertise required wl_compositor/wl_shm/zwlr_layer_shell_v1 globals"
                .into(),
        );
    }

    eprintln!(
        "quick-menu: Wayland layer-shell globals ready"
    );

    let (tx, rx): (Sender<AppEvent>, Receiver<AppEvent>) = mpsc::channel();
    start_input_subscription(tx.clone());
    start_status_subscription(tx.clone());

    let mut mapped = false;
    let mut pending_sleep = false;
    let mut brightness_dirty = false;

    loop {
        platform::update_timers_and_animations();

        let event = match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(event) => Some(event),
            Err(mpsc::RecvTimeoutError::Timeout) => None,
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        };

        if let Some(event) = event {
            match event {
                AppEvent::Input(event) => {
                    eprintln!(
                        "quick-menu: logical-action={} state={} mapped={}",
                        event.action,
                        if event.pressed { "pressed" } else { "released" },
                        mapped
                    );

                    if let Err(error) = handle_event(
                        &event,
                        &ui,
                        &mut mapped,
                        &mut pending_sleep,
                        &mut brightness_dirty,
                        &mut queue,
                        &mut state,
                        &qh,
                        &conn,
                    ) {
                        eprintln!(
                            "quick-menu: action {} failed={}",
                            event.action,
                            error
                        );

                        if mapped {
                            let _ = flush_brightness(&ui, &mut brightness_dirty);
                            unmap_overlay(&ui, &mut queue, &mut state, &conn);
                            mapped = false;
                        }
                    }
                }
                AppEvent::StatusChanged => {
                    apply_battery_snapshot(&ui, read_battery_snapshot());
                    if mapped {
                        if let Err(error) = redraw_overlay(
                            &ui,
                            &mut queue,
                            &mut state,
                            &qh,
                            &conn,
                        ) {
                            eprintln!("quick-menu: status redraw failed={}", error);
                            let _ = flush_brightness(&ui, &mut brightness_dirty);
                            unmap_overlay(&ui, &mut queue, &mut state, &conn);
                            mapped = false;
                        }
                    }
                }
            }
        }
    }

    if mapped {
        let _ = flush_brightness(&ui, &mut brightness_dirty);
        unmap_overlay(&ui, &mut queue, &mut state, &conn);
    }

    let _ = fs::remove_file(PIDFILE);
    let _ = fs::remove_file(MAPPED_FILE);
    let _ = fs::remove_file(SURFACE_FILE);
    let _ = fs::remove_file(STATE_FILE);

    Ok(())
}

fn main() {
    eprintln!("quick-menu: start pid={}", std::process::id());

    if let Err(error) = run() {
        eprintln!("quick-menu: fatal={}", error);
        let _ = fs::remove_file(PIDFILE);
        let _ = fs::remove_file(MAPPED_FILE);
        let _ = fs::remove_file(SURFACE_FILE);
        let _ = fs::remove_file(STATE_FILE);
        std::process::exit(1);
    }
}
