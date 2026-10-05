mod splash;
slint::include_modules!();

use slint::platform::{
    self,
    software_renderer::{
        MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType, TargetPixel,
    },
    Platform, PlatformError, WindowAdapter, WindowEvent,
};
use slint::{ComponentHandle, Model, ModelRc, PhysicalSize, SharedString, VecModel};

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Write};
use std::os::fd::AsFd;
use std::os::unix::fs::FileExt;
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
    Connection, Dispatch, EventQueue, QueueHandle,
};

use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1,
    wp_fractional_scale_v1::{self, WpFractionalScaleV1},
};
use wayland_protocols::wp::viewporter::client::{
    wp_viewport::WpViewport, wp_viewporter::WpViewporter,
};
use wayland_protocols_wlr::layer_shell::v1::client::{
    zwlr_layer_shell_v1::{self, Layer, ZwlrLayerShellV1},
    zwlr_layer_surface_v1::{
        self, Anchor, KeyboardInteractivity, ZwlrLayerSurfaceV1,
    },
};

const INPUT_SOCKET: &str = "/run/nuubos/inputd.sock";
const AUDIO_SOCKET: &str = "/run/nuubos/audiod.sock";
const SYSTEM_SOCKET: &str = "/run/nuubos/systemd.sock";
const USERS_SOCKET: &str = "/run/nuubos/usersd.sock";
const UI_CONTEXT: &str = "/run/nuubos/ui-context";
const LOCALIZATION_SOCKET: &str = "/run/nuubos/localizationd.sock";
const I18N_DIR: &str = "/usr/share/nuubos/i18n";
const PIDFILE: &str = "/run/nuubos/quick-menu.pid";
const MAPPED_FILE: &str = "/run/nuubos/quick-menu-mapped";
const SURFACE_FILE: &str = "/run/nuubos/quick-menu-surface";
const STATE_FILE: &str = "/run/nuubos/quick-menu-state";
const ACTION_LOG: &str = "/run/nuubos/quick-menu-action.log";
const DISPLAYCTL: &str = "/usr/bin/nuubos-displayctl";
const STATUS_STATE: &str = "/run/nuubos/statusd.state";
const STATUS_SOCKET: &str = "/run/nuubos/statusd.sock";
const NOTIFY_SOCKET: &str = "/run/nuubos/notifyd.sock";


const LINE_STAGE_MS: u64 = 220;
const CURTAIN_STAGE_MS: u64 = 460;
const STAGE_GAP_MS: u64 = 18;
const FRAME_INTERVAL_NS: u64 = 16_666_667;
const VOLUME_OSD_TIMEOUT_MS: u64 = 1500;
const TOAST_INFO_MS: u64 = 4000;
const TOAST_ALERT_MS: u64 = 6000;
/* After a long line has scrolled to its end it stays readable this long. */
const TOAST_READ_HOLD_MS: u64 = 1500;
const TOAST_QUEUE_LIMIT: usize = 6;
/* Frame pacing while a Slint animation (marquee) is running; no frames are
 * produced otherwise. */
const ANIMATION_FRAME_MS: u64 = 33;

/* Hands out one software window per component, in creation order:
 * QuickMenuWindow first, then NotificationWindow. */
struct QuickPlatform {
    windows: RefCell<VecDeque<Rc<MinimalSoftwareWindow>>>,
    started: Instant,
}

impl Platform for QuickPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        let window = self.windows.borrow_mut().pop_front().ok_or_else(|| {
            PlatformError::Other("no software window left for component".into())
        })?;
        Ok(window)
    }

    fn duration_since_start(&self) -> Duration {
        self.started.elapsed()
    }
}

/* wl_shm ARGB8888 on little-endian memory: B,G,R,A, premultiplied. Slint
 * renders straight into this layout, so no per-frame conversion pass. */
#[repr(C)]
#[derive(Copy, Clone, Default)]
struct BgraPixel {
    b: u8,
    g: u8,
    r: u8,
    a: u8,
}

impl TargetPixel for BgraPixel {
    fn blend(&mut self, color: PremultipliedRgbaColor) {
        let a = (u8::MAX - color.alpha) as u16;
        self.r = (self.r as u16 * a / 255) as u8 + color.red;
        self.g = (self.g as u16 * a / 255) as u8 + color.green;
        self.b = (self.b as u16 * a / 255) as u8 + color.blue;
        self.a = (self.a as u16 + color.alpha as u16
            - (self.a as u16 * color.alpha as u16) / 255) as u8;
    }

    fn from_rgb(r: u8, g: u8, b: u8) -> Self {
        Self { b, g, r, a: 255 }
    }

    fn background() -> Self {
        Self::default()
    }
}

/* One persistent wl_shm buffer. Two of them are swapped so Slint only
 * repaints what changed since that buffer was last presented. */
struct ShmBuffer {
    file: File,
    buffer: wl_buffer::WlBuffer,
    pixels: Vec<BgraPixel>,
    width: u32,
    height: u32,
}

const OSD_SURFACE_WIDTH: u32 = 376;
const OSD_SURFACE_HEIGHT: u32 = 98;
const OSD_SURFACE_MARGIN_BOTTOM: i32 = 52;
/* The notification card (400x72 logical) plus an 8 px margin, anchored
 * top-right: the card lands 14 px from the top and from the right edge,
 * matching its position on full-screen surfaces (quick_menu.slint). */
const TOAST_SURFACE_WIDTH: u32 = 416;
const TOAST_SURFACE_HEIGHT: u32 = 88;
const TOAST_SURFACE_MARGIN: i32 = 6;

/* Layer surface variants. Passive surfaces (volume OSD, notification) are
 * sized to their card so an update costs a few KB of rendering. The
 * notification has its own persistent surface (see Notifier). */
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum SurfaceKind {
    Full,
    Volume,
    Toast,
}

impl SurfaceKind {
    fn slint_mode(self) -> i32 {
        match self {
            SurfaceKind::Volume => 1,
            _ => 0,
        }
    }
}

struct WaylandState {
    /* Only the Quick Menu surface publishes the mapped/surface markers. */
    publishes_markers: bool,
    shm_tag: &'static str,
    compositor: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    layer_shell: Option<ZwlrLayerShellV1>,
    surface: Option<wl_surface::WlSurface>,
    layer_surface: Option<ZwlrLayerSurfaceV1>,
    configured_size: Option<(u32, u32)>,
    closed: bool,
    /* Set once a null buffer has been committed: wlroots resets the layer
     * surface on unmap and forgets the configures it already sent, so
     * acking one of those afterwards is a fatal protocol error. */
    unmapping: bool,
    buffers: Vec<ShmBuffer>,
    next_buffer: usize,
    full_frames: u8,
    frame_seq: u64,
    slint_window: Option<Rc<MinimalSoftwareWindow>>,
    /* Fractional output scale (wp_fractional_scale_v1, value * 120). The
     * buffer is rendered at physical resolution and mapped back to the
     * logical surface size with wp_viewport, so HDMI scaling stays crisp. */
    fractional_manager: Option<WpFractionalScaleManagerV1>,
    viewporter: Option<WpViewporter>,
    fractional: Option<WpFractionalScaleV1>,
    viewport: Option<WpViewport>,
    scale120: u32,
    rendered_scale120: u32,
}

impl WaylandState {
    fn new(publishes_markers: bool, shm_tag: &'static str) -> Self {
        Self {
            publishes_markers,
            shm_tag,
            compositor: None,
            shm: None,
            layer_shell: None,
            surface: None,
            layer_surface: None,
            configured_size: None,
            closed: false,
            unmapping: false,
            buffers: Vec::new(),
            next_buffer: 0,
            full_frames: 2,
            frame_seq: 0,
            slint_window: None,
            fractional_manager: None,
            viewporter: None,
            fractional: None,
            viewport: None,
            scale120: 120,
            rendered_scale120: 120,
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
        kind: SurfaceKind,
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
            if kind == SurfaceKind::Toast {
                "nuubOS Notification".into()
            } else {
                "nuubOS Quick Menu".into()
            },
            qh,
            (),
        );

        match kind {
            SurfaceKind::Volume => {
                /* The volume OSD needs only its own card: a small surface keeps
                 * each volume step to a few KB of rendering and compositing. */
                layer_surface.set_size(OSD_SURFACE_WIDTH, OSD_SURFACE_HEIGHT);
                layer_surface.set_anchor(Anchor::Bottom);
                layer_surface.set_margin(0, 0, OSD_SURFACE_MARGIN_BOTTOM, 0);
            }
            SurfaceKind::Toast => {
                layer_surface.set_size(TOAST_SURFACE_WIDTH, TOAST_SURFACE_HEIGHT);
                layer_surface.set_anchor(Anchor::Top | Anchor::Right);
                layer_surface.set_margin(TOAST_SURFACE_MARGIN, TOAST_SURFACE_MARGIN, 0, 0);
            }
            SurfaceKind::Full => {
                layer_surface.set_size(0, 0);
                layer_surface.set_anchor(
                    Anchor::Top | Anchor::Bottom | Anchor::Left | Anchor::Right,
                );
            }
        }
        layer_surface.set_exclusive_zone(-1);
        layer_surface.set_keyboard_interactivity(
            KeyboardInteractivity::None,
        );

        if let (Some(manager), Some(viewporter)) =
            (self.fractional_manager.as_ref(), self.viewporter.as_ref())
        {
            self.fractional = Some(manager.get_fractional_scale(&surface, qh, ()));
            self.viewport = Some(viewporter.get_viewport(&surface, qh, ()));
        }

        self.surface = Some(surface.clone());
        self.layer_surface = Some(layer_surface);
        self.configured_size = None;
        self.closed = false;
        self.unmapping = false;

        /*
         * Layer-shell requires one initial commit with no buffer, followed
         * by configure/ack, before the first mapped buffer commit.
         */
        surface.commit();

        Ok(())
    }

    fn destroy_overlay(&mut self) {
        if self.publishes_markers {
            let _ = fs::remove_file(MAPPED_FILE);
            let _ = fs::remove_file(SURFACE_FILE);
        }

        if !self.unmapping {
            self.commit_unmap();
        }

        if let Some(viewport) = self.viewport.take() {
            viewport.destroy();
        }
        if let Some(fractional) = self.fractional.take() {
            fractional.destroy();
        }

        if let Some(layer_surface) = self.layer_surface.take() {
            layer_surface.destroy();
        }

        if let Some(surface) = self.surface.take() {
            surface.destroy();
        }

        for shm_buffer in self.buffers.drain(..) {
            shm_buffer.buffer.destroy();
        }
        self.next_buffer = 0;
        self.full_frames = 2;

        self.configured_size = None;
        self.closed = false;
        self.unmapping = false;
    }

    /* Commit a null buffer (unmap). Configures still in flight are then
     * ignored instead of acked. Returns whether a surface existed. */
    fn commit_unmap(&mut self) -> bool {
        match self.surface.as_ref() {
            Some(surface) => {
                self.unmapping = true;
                surface.attach(None, 0, 0);
                surface.commit();
                true
            }
            None => false,
        }
    }

    fn create_shm_buffer(
        &mut self,
        width: u32,
        height: u32,
        qh: &QueueHandle<Self>,
    ) -> Result<ShmBuffer, Box<dyn std::error::Error>> {
        let bytes = (width as usize) * (height as usize) * 4;
        self.frame_seq += 1;
        let backing_path = format!(
            "/run/nuubos/quick-menu-shm-{}-{}-{}",
            self.shm_tag,
            std::process::id(),
            self.frame_seq
        );

        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&backing_path)?;
        file.set_len(bytes as u64)?;

        /*
         * Unlink immediately: the local fd and the compositor's SCM_RIGHTS
         * copy keep the backing store alive without leaving runtime debris.
         */
        fs::remove_file(&backing_path)?;

        let shm = self.shm.as_ref().ok_or("wl_shm unavailable")?;
        let pool = shm.create_pool(file.as_fd(), bytes as i32, qh, ());
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

        Ok(ShmBuffer {
            file,
            buffer,
            pixels: vec![BgraPixel::default(); (width as usize) * (height as usize)],
            width,
            height,
        })
    }

    fn render_and_commit(
        &mut self,
        window: &slint::Window,
        qh: &QueueHandle<Self>,
        conn: &Connection,
    ) -> Result<(u32, u32), Box<dyn std::error::Error>> {
        let (logical_width, logical_height) = self
            .configured_size
            .ok_or("layer surface not configured")?;

        if logical_width == 0 || logical_height == 0 {
            return Err("compositor returned zero-sized layer surface".into());
        }

        let scale120 = if self.viewport.is_some() { self.scale120.max(120) } else { 120 };
        let width = (logical_width * scale120 + 60) / 120;
        let height = (logical_height * scale120 + 60) / 120;
        if scale120 != self.rendered_scale120 {
            window.dispatch_event(WindowEvent::ScaleFactorChanged {
                scale_factor: scale120 as f32 / 120.0,
            });
            self.rendered_scale120 = scale120;
        }

        window.set_size(PhysicalSize::new(width, height));

        platform::update_timers_and_animations();

        if self
            .buffers
            .first()
            .map(|b| b.width != width || b.height != height)
            .unwrap_or(true)
        {
            for shm_buffer in self.buffers.drain(..) {
                shm_buffer.buffer.destroy();
            }
            for _ in 0..2 {
                let shm_buffer = self.create_shm_buffer(width, height, qh)?;
                self.buffers.push(shm_buffer);
            }
            self.next_buffer = 0;
            self.full_frames = 2;
        }

        let slint_window = self
            .slint_window
            .as_ref()
            .ok_or("Slint window unavailable")?
            .clone();
        let index = self.next_buffer;
        let full = self.full_frames > 0;
        let stride = width as usize;
        let mut dirty: Vec<(i32, i32, i32, i32)> = Vec::new();
        {
            let target = &mut self.buffers[index];
            slint_window.request_redraw();
            slint_window.draw_if_needed(|renderer| {
                renderer.set_repaint_buffer_type(if full {
                    RepaintBufferType::NewBuffer
                } else {
                    RepaintBufferType::SwappedBuffers
                });
                let region = renderer.render(&mut target.pixels, stride);
                for (origin, size) in region.iter() {
                    dirty.push((origin.x, origin.y, size.width as i32, size.height as i32));
                }
            });
        }
        if full {
            self.full_frames -= 1;
            dirty = vec![(0, 0, width as i32, height as i32)];
        }

        /* Publish only the repainted rows of the swapped buffer. */
        {
            let target = &self.buffers[index];
            for &(x, y, w, h) in &dirty {
                let x = x.clamp(0, width as i32) as usize;
                let y = y.clamp(0, height as i32) as usize;
                let w = (w.max(0) as usize).min(width as usize - x);
                let h = (h.max(0) as usize).min(height as usize - y);
                for row in y..y + h {
                    let start = row * stride + x;
                    let pixels = &target.pixels[start..start + w];
                    let bytes = unsafe {
                        std::slice::from_raw_parts(pixels.as_ptr() as *const u8, w * 4)
                    };
                    target.file.write_all_at(bytes, (start * 4) as u64)?;
                }
            }
        }

        let surface = self
            .surface
            .as_ref()
            .ok_or("layer surface disappeared")?;

        if let Some(viewport) = self.viewport.as_ref() {
            viewport.set_destination(logical_width as i32, logical_height as i32);
        }
        surface.attach(Some(&self.buffers[index].buffer), 0, 0);
        for &(x, y, w, h) in &dirty {
            surface.damage_buffer(x, y, w, h);
        }
        surface.commit();
        conn.flush()?;

        self.next_buffer = (index + 1) % self.buffers.len();

        Ok((logical_width, logical_height))
    }

    /* The preferred scale arrives once the surface enters an output, i.e.
     * after the first commit. Re-render when it differs from the last frame. */
    fn needs_rescale(&self) -> bool {
        self.viewport.is_some() && self.scale120.max(120) != self.rendered_scale120
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
                "wp_fractional_scale_manager_v1" => {
                    state.fractional_manager = Some(
                        registry.bind::<WpFractionalScaleManagerV1, _, _>(name, 1, qh, ()),
                    );
                }
                "wp_viewporter" => {
                    state.viewporter = Some(
                        registry.bind::<WpViewporter, _, _>(name, 1, qh, ()),
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
                if state.unmapping {
                    return;
                }
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
        /* Persistent buffers are reused; with wl_shm the compositor copies
         * on commit, so release needs no bookkeeping here. */
        let _ = (state, buffer, event);
    }
}

delegate_noop!(WaylandState: ignore wl_compositor::WlCompositor);
delegate_noop!(WaylandState: ignore wl_region::WlRegion);
delegate_noop!(WaylandState: ignore wl_surface::WlSurface);
delegate_noop!(WaylandState: ignore wl_shm::WlShm);
delegate_noop!(WaylandState: ignore wl_shm_pool::WlShmPool);
delegate_noop!(WaylandState: ignore zwlr_layer_shell_v1::ZwlrLayerShellV1);
delegate_noop!(WaylandState: ignore WpFractionalScaleManagerV1);
delegate_noop!(WaylandState: ignore WpViewporter);
delegate_noop!(WaylandState: ignore WpViewport);

impl Dispatch<WpFractionalScaleV1, ()> for WaylandState {
    fn event(
        state: &mut Self,
        _: &WpFractionalScaleV1,
        event: wp_fractional_scale_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wp_fractional_scale_v1::Event::PreferredScale { scale } = event {
            state.scale120 = scale;
        }
    }
}

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

fn users_command(command:&str)->std::io::Result<String>{let mut s=UnixStream::connect(USERS_SOCKET)?;s.set_read_timeout(Some(Duration::from_millis(700)))?;s.write_all(command.as_bytes())?;if !command.ends_with('\n'){s.write_all(b"\n")?;}s.shutdown(std::net::Shutdown::Write)?;let mut r=String::new();let _=s.read_to_string(&mut r);Ok(r)}
fn switch_user_available()->bool{let c=fs::read_to_string(UI_CONTEXT).unwrap_or_default();if !matches!(c.trim(),"home"|"settings"){return false;}let Ok(r)=users_command("STATUS") else{return false;};r.lines().find_map(|l|l.strip_prefix("count=")).and_then(|v|v.parse::<i32>().ok()).unwrap_or(0)>1}

fn lifecycle_action(action: &'static str) -> Result<(), Box<dyn std::error::Error>> {
    let command = match action {
        "sleep" => "ACTION SLEEP",
        "restart" => "ACTION RESTART",
        "poweroff" => "ACTION POWEROFF",
        _ => return Err("invalid lifecycle action".into()),
    };

    append_action_log(&format!("action={} phase=product-api", action));
    let mut stream = UnixStream::connect(SYSTEM_SOCKET)?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    stream.shutdown(std::net::Shutdown::Write)?;

    /* Sleep intentionally blocks until resume. Restart/Power Off are terminal.
     * There is no client-side platform helper or /sbin action here: the
     * Product System lifecycle service is the sole owner of all hooks/policy. */
    let mut reply = String::new();
    let _ = stream.read_to_string(&mut reply);
    if reply.starts_with("ERR") {
        return Err(reply.trim().to_owned().into());
    }
    Ok(())
}

fn spawn_lifecycle_action(action: &'static str) {
    thread::spawn(move || {
        if let Err(error) = lifecycle_action(action) {
            append_action_log(&format!("action={} phase=error error={}", action, error));
        }
    });
}

fn run_suspend_sync() -> Result<(), Box<dyn std::error::Error>> {
    lifecycle_action("sleep")
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




fn localization_language() -> String {
    let result = (|| -> std::io::Result<String> {
        let mut stream = UnixStream::connect(LOCALIZATION_SOCKET)?;
        stream.write_all(b"STATUS\n")?;
        stream.shutdown(std::net::Shutdown::Write)?;
        let mut reply = String::new();
        stream.read_to_string(&mut reply)?;
        Ok(reply)
    })();
    if let Ok(reply) = result {
        for line in reply.lines() {
            if let Some(value) = line.strip_prefix("language=") {
                return value.trim().to_owned();
            }
        }
    }
    "en".to_owned()
}

fn load_i18n_catalog(code: &str) -> Vec<SharedString> {
    fn load(path: &str) -> Vec<Option<String>> {
        let Ok(file) = File::open(path) else { return Vec::new(); };
        let mut values: Vec<Option<String>> = Vec::new();
        for line in BufReader::new(file).lines() {
            let Ok(line) = line else { continue; };
            if line.trim().is_empty() || line.starts_with('#') { continue; }
            let Some((key, value)) = line.split_once('=') else { continue; };
            let Ok(index) = key.trim().parse::<usize>() else { continue; };
            if values.len() <= index { values.resize(index + 1, None); }
            if !value.is_empty() { values[index] = Some(value.to_owned()); }
        }
        values
    }

    let english = load(&format!("{}/en.lang", I18N_DIR));
    let selected = if code == "en" {
        Vec::new()
    } else {
        load(&format!("{}/{}.lang", I18N_DIR, code))
    };
    let len = english.len().max(selected.len());
    (0..len)
        .map(|index| {
            selected.get(index).and_then(|value| value.clone())
                .or_else(|| english.get(index).and_then(|value| value.clone()))
                .unwrap_or_default()
                .into()
        })
        .collect()
}

thread_local! {
    static I18N_LOADED: RefCell<String> = const { RefCell::new(String::new()) };
}

/* Reloads the catalog only when the active user's language changed. Called
 * when the Quick Menu opens and before a notification is localized. */
fn refresh_i18n(ui: &QuickMenuWindow) {
    let code = localization_language();
    if I18N_LOADED.with(|loaded| *loaded.borrow() == code) {
        return;
    }
    let values = load_i18n_catalog(&code);
    if !values.is_empty() {
        ui.set_i18n_strings(ModelRc::from(Rc::new(VecModel::from(values))));
        I18N_LOADED.with(|loaded| *loaded.borrow_mut() = code);
    }
}

fn tr(ui: &QuickMenuWindow, index: usize, fallback: &str) -> String {
    ui.get_i18n_strings()
        .row_data(index)
        .map(|value| value.to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| fallback.to_owned())
}

fn system_command(command: &str) -> Result<String, Box<dyn std::error::Error>> {
    let mut stream = UnixStream::connect(SYSTEM_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_millis(300)))?;
    stream.set_write_timeout(Some(Duration::from_millis(300)))?;
    stream.write_all(command.as_bytes())?;
    if !command.ends_with('\n') {
        stream.write_all(b"\n")?;
    }
    stream.shutdown(std::net::Shutdown::Write)?;
    let mut reply = String::new();
    stream.read_to_string(&mut reply)?;
    Ok(reply)
}

fn parse_system_profile(reply: &str) -> (String, String) {
    let mut requested = "auto".to_owned();
    let mut effective = "auto".to_owned();
    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key {
            "profile.requested" => requested = value.to_owned(),
            "profile.effective" => effective = value.to_owned(),
            _ => {}
        }
    }
    (requested, effective)
}

fn system_profile_label(profile: &str) -> &'static str {
    match profile {
        "battery-saver" => "Battery Saver",
        _ => "Auto",
    }
}

fn refresh_system_profile(ui: &QuickMenuWindow) {
    match system_command("STATUS") {
        Ok(reply) => {
            let (requested, effective) = parse_system_profile(&reply);
            let label = if requested != effective {
                format!(
                    "{} • {} active",
                    system_profile_label(&requested),
                    system_profile_label(&effective)
                )
            } else {
                system_profile_label(&requested).to_owned()
            };
            ui.set_performance_profile_label(label.into());
        }
        Err(error) => {
            eprintln!("quick-menu: System profile read failed={error}");
            ui.set_performance_profile_label(tr(ui, 241, "Unavailable").into());
        }
    }
}

fn cycle_system_profile(ui: &QuickMenuWindow, delta: i32) {
    let Ok(reply) = system_command("STATUS") else {
        return;
    };
    let (requested, _) = parse_system_profile(&reply);
    let profiles = ["auto", "battery-saver"];
    let current = profiles.iter().position(|p| *p == requested).unwrap_or(0) as i32;
    let next = (current + delta).rem_euclid(profiles.len() as i32) as usize;
    let command = format!("SET PROFILE {}", profiles[next]);
    if let Err(error) = system_command(&command) {
        eprintln!("quick-menu: profile change failed={error}");
    }
    refresh_system_profile(ui);
}

#[derive(Debug, Clone)]
struct AudioSnapshot {
    mode: String,
    selected: String,
    volume: i32,
    volume_supported: bool,
    bluetooth_available: bool,
    headphones_available: bool,
    hdmi_available: bool,
    system_volume: i32,
    home_music_volume: i32,
    home_music_playing: bool,
}

impl Default for AudioSnapshot {
    fn default() -> Self {
        Self {
            mode: "auto".to_owned(),
            selected: "speaker".to_owned(),
            volume: 100,
            volume_supported: true,
            bluetooth_available: false,
            headphones_available: false,
            hdmi_available: false,
            system_volume: 60,
            home_music_volume: 60,
            home_music_playing: false,
        }
    }
}

fn audio_command(command: &str) -> Result<String, Box<dyn std::error::Error>> {
    let mut stream = UnixStream::connect(AUDIO_SOCKET)?;
    /* The overlay render/input thread must never block indefinitely on a
     * product service. A stale audiod used to freeze Quick Menu on open. */
    stream.set_read_timeout(Some(Duration::from_millis(300)))?;
    stream.set_write_timeout(Some(Duration::from_millis(300)))?;
    stream.write_all(command.as_bytes())?;
    stream.shutdown(std::net::Shutdown::Write)?;
    let mut reply = String::new();
    stream.read_to_string(&mut reply)?;
    Ok(reply)
}

fn parse_audio_snapshot(reply: &str) -> AudioSnapshot {
    let mut snapshot = AudioSnapshot::default();

    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key {
            "mode" => snapshot.mode = value.to_owned(),
            "selected" => snapshot.selected = value.to_owned(),
            "available.bluetooth" => snapshot.bluetooth_available = value == "1",
            "available.headphones" => snapshot.headphones_available = value == "1",
            "available.hdmi" => snapshot.hdmi_available = value == "1",
            "volume.speaker" if snapshot.selected == "speaker" => {
                snapshot.volume = value.parse::<i32>().unwrap_or(100);
            }
            "volume.headphones" if snapshot.selected == "headphones" => {
                snapshot.volume = value.parse::<i32>().unwrap_or(100);
            }
            "volume.bluetooth" if snapshot.selected == "bluetooth" => {
                snapshot.volume = value.parse::<i32>().unwrap_or(100);
            }
            "volume.hdmi" if snapshot.selected == "hdmi" => {
                snapshot.volume_supported = value != "unsupported";
                snapshot.volume = -1;
            }
            "volume.system" => {
                snapshot.system_volume = value.parse::<i32>().unwrap_or(60);
            }
            "volume.home_music" => {
                snapshot.home_music_volume = value.parse::<i32>().unwrap_or(60);
            }
            "home_music.playing" => snapshot.home_music_playing = value == "1",
            _ => {}
        }
    }

    snapshot.volume_supported = snapshot.selected != "hdmi";
    snapshot
}

fn read_audio_snapshot() -> AudioSnapshot {
    let Ok(reply) = audio_command("STATUS") else {
        return AudioSnapshot::default();
    };
    parse_audio_snapshot(&reply)
}

fn audio_output_name(ui: &QuickMenuWindow, output: &str) -> String {
    match output {
        "bluetooth" => tr(ui, 331, "Bluetooth Audio"),
        "analog" => tr(ui, 332, "Analog Audio"),
        "headphones" => tr(ui, 330, "Headphones"),
        "hdmi" => "HDMI".to_owned(),
        "speaker" => tr(ui, 240, "Speaker"),
        _ => tr(ui, 235, "Audio"),
    }
}

fn apply_audio_snapshot(ui: &QuickMenuWindow, snapshot: &AudioSnapshot) {
    let selected_name = audio_output_name(ui, snapshot.selected.as_str());

    ui.set_volume_osd_title(selected_name.as_str().into());
    ui.set_volume_osd_supported(snapshot.volume_supported);
    ui.set_audio_volume_visible(snapshot.volume_supported);
    ui.set_system_sounds_label(
        format!("{}%", snapshot.system_volume.clamp(0, 100)).into(),
    );
    ui.set_system_sounds_fraction(
        snapshot.system_volume.clamp(0, 100) as f32 / 100.0,
    );
    ui.set_home_music_playing(snapshot.home_music_playing);

    if snapshot.volume_supported {
        let volume = snapshot.volume.clamp(0, 100);
        ui.set_audio_output_label(format!("{selected_name} • {volume}%").into());
        ui.set_audio_volume_label(format!("{volume}%").into());
        ui.set_audio_volume_fraction(volume as f32 / 100.0);
        ui.set_volume_osd_detail(format!("{volume}%").into());
        ui.set_volume_osd_fraction(volume as f32 / 100.0);
    } else {
        ui.set_audio_output_label(format!("{selected_name} • TV").into());
        ui.set_audio_volume_label(tr(ui, 272, "TV").into());
        ui.set_audio_volume_fraction(0.0);
        ui.set_volume_osd_detail(tr(ui, 273, "Volume controlled by TV / receiver").into());
        ui.set_volume_osd_fraction(0.0);
    }
}

fn audio_dropdown_options(ui: &QuickMenuWindow, snapshot: &AudioSnapshot) -> Vec<(&'static str, String)> {
    /* Automatic is a first-class Product Audio mode, not a physical sink.
     * Keep it visible even when the currently selected physical output is one
     * of the explicit routes below. */
    let mut options = vec![("auto", tr(ui, 122, "Automatic"))];

    if snapshot.bluetooth_available {
        options.push(("bluetooth", tr(ui, 331, "Bluetooth Audio")));
    }

    if snapshot.headphones_available {
        options.push(("analog", tr(ui, 330, "Headphones")));
    } else {
        options.push(("analog", tr(ui, 240, "Speaker")));
    }

    if snapshot.hdmi_available {
        options.push(("hdmi", "HDMI".to_owned()));
    }

    options
}

fn open_audio_dropdown(ui: &QuickMenuWindow) {
    let snapshot = read_audio_snapshot();
    let options = audio_dropdown_options(ui, &snapshot);
    let labels: Vec<SharedString> = options.iter().map(|(_, label)| label.as_str().into()).collect();
    /* Selection reflects the requested policy mode. In Automatic, selected is
     * intentionally the resolved physical output and therefore cannot be used
     * to decide which dropdown row is active. */
    let selected_mode = match snapshot.mode.as_str() {
        "auto" => "auto",
        "bluetooth" => "bluetooth",
        "hdmi" => "hdmi",
        "analog" | "speaker" | "headphones" => "analog",
        _ => "auto",
    };
    let selected = options
        .iter()
        .position(|(value, _)| *value == selected_mode)
        .unwrap_or(0) as i32;
    ui.set_audio_output_options(ModelRc::new(Rc::new(VecModel::from(labels))));
    ui.set_audio_output_dropdown_index(selected);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let count = options.len() as i32;
    let scroll = if selected >= visible { selected - visible + 1 } else { 0 };
    ui.set_audio_output_dropdown_scroll(scroll.min((count - visible).max(0)));
    ui.set_audio_output_dropdown_open(true);
}

fn apply_audio_dropdown(ui: &QuickMenuWindow) {
    let snapshot = read_audio_snapshot();
    let options = audio_dropdown_options(ui, &snapshot);
    if options.is_empty() {
        return;
    }

    let index = ui
        .get_audio_output_dropdown_index()
        .clamp(0, options.len() as i32 - 1) as usize;
    let requested = options[index].0;

    /* Output changes must never tear down the Quick Menu. A transient Product
     * Audio timeout/error leaves the dropdown open so the user can retry or go
     * Back; only a successful switch closes the dropdown, not the overlay. */
    match audio_command(&format!("OUTPUT SET {requested}")) {
        Ok(reply) if reply.starts_with("OK") => {
            ui.set_audio_output_dropdown_open(false);
            apply_audio_snapshot(ui, &read_audio_snapshot());
            append_action_log(&format!("audio-output mode={requested}"));
        }
        Ok(reply) => {
            eprintln!(
                "quick-menu: audio output change failed mode={} reply={}",
                requested,
                reply.trim()
            );
        }
        Err(error) => {
            eprintln!(
                "quick-menu: audio output change failed mode={} error={}",
                requested,
                error
            );
        }
    }
}

fn move_audio_dropdown(ui: &QuickMenuWindow, delta: i32) {
    let count = ui.get_audio_output_options().row_count() as i32;
    if count <= 0 {
        return;
    }

    let current = ui.get_audio_output_dropdown_index();
    let next = (current + delta).rem_euclid(count);
    ui.set_audio_output_dropdown_index(next);

    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let mut scroll = ui.get_audio_output_dropdown_scroll();
    if next < scroll {
        scroll = next;
    } else if next >= scroll + visible {
        scroll = next - visible + 1;
    }
    ui.set_audio_output_dropdown_scroll(scroll.clamp(0, (count - visible).max(0)));
}

fn cycle_audio_output(
    ui: &QuickMenuWindow,
    direction: i32,
) -> Result<(), Box<dyn std::error::Error>> {
    let snapshot = read_audio_snapshot();
    let mut modes: Vec<&str> = vec!["auto"];

    if snapshot.bluetooth_available {
        modes.push("bluetooth");
    }

    /* The codec exposes one physical analog route.  Jack insertion chooses
     * headphones vs speaker, so Quick Menu switches to "analog" rather than
     * pretending those destinations can be forced independently. */
    modes.push("analog");

    if snapshot.hdmi_available {
        modes.push("hdmi");
    }

    let current = modes
        .iter()
        .position(|mode_name| *mode_name == snapshot.mode)
        .unwrap_or(0) as i32;
    let next = (current + direction).rem_euclid(modes.len() as i32) as usize;

    let reply = audio_command(&format!("OUTPUT SET {}", modes[next]))?;
    if !reply.starts_with("OK") {
        return Err(format!("audio output change failed: {}", reply.trim()).into());
    }

    let updated = read_audio_snapshot();
    apply_audio_snapshot(ui, &updated);
    append_action_log(&format!(
        "audio-output mode={} selected={}",
        updated.mode, updated.selected
    ));
    Ok(())
}

fn adjust_audio_volume(
    ui: &QuickMenuWindow,
    delta: i32,
) -> Result<AudioSnapshot, Box<dyn std::error::Error>> {
    /* VOLUME ADJUST is atomic in Product Audio and already returns a complete
     * STATUS snapshot. Do not precede every 1% tick with another STATUS IPC. */
    let reply = audio_command(&format!("VOLUME ADJUST {delta}"))?;
    if reply.starts_with("unsupported") {
        let snapshot = read_audio_snapshot();
        apply_audio_snapshot(ui, &snapshot);
        return Ok(snapshot);
    }
    if reply.starts_with("ERR") {
        return Err(format!("volume adjust failed: {}", reply.trim()).into());
    }

    let after = parse_audio_snapshot(&reply);
    apply_audio_snapshot(ui, &after);
    append_action_log(&format!(
        "volume-change output={} delta={} value={}",
        after.selected, delta, after.volume
    ));
    Ok(after)
}


fn adjust_system_sounds_volume(
    ui: &QuickMenuWindow,
    delta: i32,
) -> Result<(), Box<dyn std::error::Error>> {
    let snapshot = read_audio_snapshot();
    let requested = (snapshot.system_volume + delta).clamp(0, 100);
    let reply = audio_command(&format!("SYSTEM VOLUME SET {requested}"))?;
    if reply.starts_with("ERR") {
        return Err(format!("system sounds volume failed: {}", reply.trim()).into());
    }
    apply_audio_snapshot(ui, &read_audio_snapshot());
    Ok(())
}

fn next_home_music() -> Result<(), Box<dyn std::error::Error>> {
    let reply = audio_command("MUSIC NEXT")?;
    if reply.starts_with("ERR") {
        return Err(format!("home music next failed: {}", reply.trim()).into());
    }
    Ok(())
}

fn play_named_sound(cue: &str) {
    let cue = cue.to_owned();
    thread::spawn(move || {
        let _ = audio_command(&format!("SFX PLAY {cue}"));
    });
}

fn play_ui_sound(action: &str) {
    let cue = match action {
        "menu_confirm" => Some("select"),
        "menu_back" => Some("back"),
        "menu_up" | "menu_down" | "menu_left" | "menu_right" => Some("navigation"),
        "quick_menu" => Some("quick-settings"),
        _ => None,
    };

    if let Some(cue) = cue {
        play_named_sound(cue);
    }
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
    Notify(Notification, Instant),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NotifyVerb {
    Post,
    Update,
    Dismiss,
}

/* One message of the notifyd contract (nuubos/notify.h). */
#[derive(Debug, Clone)]
struct Notification {
    verb: NotifyVerb,
    id: String,
    event: String,
    fields: HashMap<String, String>,
}

impl Notification {
    fn text(&self, key: &str) -> &str {
        self.fields.get(key).map(String::as_str).unwrap_or("")
    }

    fn int(&self, key: &str) -> Option<i32> {
        self.fields.get(key).and_then(|v| v.parse::<i32>().ok())
    }
}

fn percent_decode(raw: &[u8]) -> String {
    let mut out = Vec::with_capacity(raw.len());
    let mut i = 0;
    while i < raw.len() {
        if raw[i] == b'%' && i + 2 < raw.len() {
            let hex = std::str::from_utf8(&raw[i + 1..i + 3]).ok();
            if let Some(byte) = hex.and_then(|h| u8::from_str_radix(h, 16).ok()) {
                out.push(byte);
                i += 3;
                continue;
            }
        }
        out.push(raw[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn parse_notification(line: &[u8]) -> Option<Notification> {
    let mut tokens = line
        .split(|b| *b == b' ' || *b == b'\n' || *b == b'\r')
        .filter(|t| !t.is_empty());
    let verb = match tokens.next()? {
        b"POST" => NotifyVerb::Post,
        b"UPDATE" => NotifyVerb::Update,
        b"DISMISS" => NotifyVerb::Dismiss,
        _ => return None,
    };
    let mut fields = HashMap::new();
    for token in tokens {
        let eq = token.iter().position(|b| *b == b'=')?;
        let key = String::from_utf8_lossy(&token[..eq]).into_owned();
        fields.insert(key, percent_decode(&token[eq + 1..]));
    }
    let id = fields.remove("id")?;
    let event = fields.remove("event").unwrap_or_default();
    Some(Notification { verb, id, event, fields })
}

fn start_notify_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(NOTIFY_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                eprintln!("quick-menu: notification router connected");
                let mut reader = BufReader::new(stream);
                let mut line = Vec::new();
                loop {
                    line.clear();
                    match reader.read_until(b'\n', &mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            if let Some(notification) = parse_notification(&line) {
                                if tx
                                    .send(AppEvent::Notify(notification, Instant::now()))
                                    .is_err()
                                {
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
        /* Only reached while notifyd is absent or restarting; live
         * notifications are replayed by the router on resubscribe. */
        thread::sleep(Duration::from_secs(1));
    });
}

const SEVERITY_INFO: i32 = 0;
const SEVERITY_SUCCESS: i32 = 1;
const SEVERITY_WARNING: i32 = 2;
const SEVERITY_ERROR: i32 = 3;

/* Icon ids understood by NotificationCard in quick_menu.slint. */
const ICON_MUSIC: i32 = 1;
const ICON_CONTROLLER: i32 = 2;
const ICON_HEADPHONES: i32 = 3;
const ICON_SPEAKER: i32 = 4;
const ICON_BATTERY: i32 = 5;
const ICON_SAVER: i32 = 6;
const ICON_CHARGER: i32 = 7;
const ICON_WIFI: i32 = 8;
const ICON_STORAGE: i32 = 9;
const ICON_DONE: i32 = 10;
const ICON_FAILED: i32 = 11;

#[derive(Debug, Clone)]
struct ToastView {
    id: String,
    icon: i32,
    severity: i32,
    title: String,
    detail: String,
    battery: i32,
    progress: f32,
}

impl ToastView {
    fn timeout(&self) -> Duration {
        Duration::from_millis(if self.severity >= SEVERITY_WARNING {
            TOAST_ALERT_MS
        } else {
            TOAST_INFO_MS
        })
    }
}

fn format_arg(template: &str, value: &str) -> String {
    template.replace("{0}", value)
}

fn join_detail(parts: &[&str], separator: &str) -> String {
    parts
        .iter()
        .filter(|part| !part.is_empty())
        .copied()
        .collect::<Vec<_>>()
        .join(separator)
}

/*
 * Presentation of each typed event: localized title/detail, icon and
 * severity. Producers only send raw values; unknown events are ignored so a
 * newer service never breaks an older renderer.
 */
fn toast_view(ui: &QuickMenuWindow, n: &Notification) -> Option<ToastView> {
    let percent = n.int("percent").filter(|p| (0..=100).contains(p));
    let percent_text = percent.map(|p| p.to_string()).unwrap_or_default();
    let speaker = n.text("kind") == "speaker";
    let (icon, severity, title, detail, battery) = match n.event.as_str() {
        "music.track" => (
            ICON_MUSIC,
            SEVERITY_INFO,
            tr(ui, 369, "Now Playing"),
            join_detail(&[n.text("title"), n.text("artist")], " — "),
            -1,
        ),
        "controller.connected" => {
            let player = n
                .int("player")
                .filter(|p| *p > 0)
                .map(|p| format!("{}{}", tr(ui, 220, "Player "), p))
                .unwrap_or_default();
            (
                ICON_CONTROLLER,
                SEVERITY_INFO,
                tr(ui, 370, "Controller connected"),
                join_detail(&[n.text("name"), &player], " • "),
                n.int("battery").unwrap_or(-1),
            )
        }
        "controller.disconnected" => (
            ICON_CONTROLLER,
            SEVERITY_INFO,
            tr(ui, 371, "Controller disconnected"),
            n.text("name").to_owned(),
            -1,
        ),
        "headphones.connected" => (
            if speaker { ICON_SPEAKER } else { ICON_HEADPHONES },
            SEVERITY_INFO,
            if speaker {
                tr(ui, 374, "Bluetooth audio connected")
            } else {
                tr(ui, 372, "Headphones connected")
            },
            n.text("name").to_owned(),
            n.int("battery").unwrap_or(-1),
        ),
        "headphones.disconnected" => (
            if speaker { ICON_SPEAKER } else { ICON_HEADPHONES },
            SEVERITY_INFO,
            if speaker {
                tr(ui, 375, "Bluetooth audio disconnected")
            } else {
                tr(ui, 373, "Headphones disconnected")
            },
            n.text("name").to_owned(),
            -1,
        ),
        "accessory.battery.low" => (
            match n.text("kind") {
                "controller" => ICON_CONTROLLER,
                "headphones" => ICON_HEADPHONES,
                _ => ICON_BATTERY,
            },
            SEVERITY_WARNING,
            tr(ui, 378, "Low battery"),
            n.text("name").to_owned(),
            percent.unwrap_or(-1),
        ),
        "battery.saver" => (
            ICON_SAVER,
            SEVERITY_INFO,
            tr(ui, 376, "Battery Saver on"),
            if percent.is_some() {
                format_arg(&tr(ui, 377, "Battery at {0}%"), &percent_text)
            } else {
                String::new()
            },
            -1,
        ),
        "battery.low" => (
            ICON_BATTERY,
            SEVERITY_WARNING,
            tr(ui, 378, "Low battery"),
            format_arg(&tr(ui, 379, "{0}% remaining"), &percent_text),
            -1,
        ),
        "battery.critical" => (
            ICON_BATTERY,
            SEVERITY_ERROR,
            tr(ui, 380, "Battery critically low"),
            format_arg(
                &tr(ui, 381, "{0}% remaining • connect the charger"),
                &percent_text,
            ),
            -1,
        ),
        "battery.charging" => (
            ICON_CHARGER,
            SEVERITY_SUCCESS,
            tr(ui, 382, "Charging"),
            if percent.is_some() { format!("{percent_text}%") } else { String::new() },
            -1,
        ),
        "battery.full" => (
            ICON_CHARGER,
            SEVERITY_SUCCESS,
            tr(ui, 383, "Fully charged"),
            String::new(),
            -1,
        ),
        "wifi.connected" => (
            ICON_WIFI,
            SEVERITY_SUCCESS,
            tr(ui, 384, "Wi-Fi connected"),
            n.text("ssid").to_owned(),
            -1,
        ),
        "wifi.lost" => (
            ICON_WIFI,
            SEVERITY_WARNING,
            tr(ui, 385, "Wi-Fi connection lost"),
            n.text("ssid").to_owned(),
            -1,
        ),
        "storage.job" => {
            let title = match n.text("action") {
                "backup" => tr(ui, 97, "Backup to SD1"),
                "move-back" => tr(ui, 96, "Move to SD1"),
                "adopt" => tr(ui, 95, "Move to SD2"),
                _ => return None,
            };
            let (icon, severity, detail) = match n.text("state") {
                "running" => (ICON_STORAGE, SEVERITY_INFO, tr(ui, 91, "Running")),
                "succeeded" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 92, "Completed")),
                "failed" => (ICON_FAILED, SEVERITY_ERROR, tr(ui, 93, "Failed")),
                _ => return None,
            };
            (icon, severity, title, detail, -1)
        }
        _ => return None,
    };

    let progress = n
        .int("progress")
        .filter(|p| (0..=100).contains(p))
        .map(|p| p as f32 / 100.0)
        .unwrap_or(-1.0);

    Some(ToastView {
        id: n.id.clone(),
        icon,
        severity,
        title,
        detail,
        battery: battery.clamp(-1, 100),
        progress,
    })
}

/*
 * One notification is visible at a time; later ones wait in a short queue.
 * A notification with the id of a visible or queued one replaces it in
 * place (EPIC-006 stable ids), so reconnects or progress never stack.
 */
#[derive(Default)]
struct Toasts {
    current: Option<ToastView>,
    pending: VecDeque<ToastView>,
    deadline: Option<Instant>,
    /* Notifications received before the last resume describe the sleep
     * transition itself (e.g. Bluetooth dropping) and are discarded. */
    resumed_at: Option<Instant>,
}

impl Toasts {
    /* Returns true when the visible notification changed. */
    fn post(&mut self, view: ToastView, update_only: bool) -> bool {
        if self.current.as_ref().is_some_and(|c| c.id == view.id) {
            self.deadline = Some(Instant::now() + view.timeout());
            self.current = Some(view);
            return true;
        }
        if let Some(queued) = self.pending.iter_mut().find(|q| q.id == view.id) {
            *queued = view;
            return false;
        }
        if update_only {
            return false;
        }
        self.pending.push_back(view);
        while self.pending.len() > TOAST_QUEUE_LIMIT {
            self.pending.pop_front();
        }
        if self.current.is_none() {
            self.show_next();
            return true;
        }
        false
    }

    fn dismiss(&mut self, id: &str) -> bool {
        self.pending.retain(|q| q.id != id);
        if self.current.as_ref().is_some_and(|c| c.id == id) {
            self.show_next();
            return true;
        }
        false
    }

    fn expire(&mut self, now: Instant) -> bool {
        if self.deadline.is_some_and(|deadline| now >= deadline) {
            self.show_next();
            return true;
        }
        false
    }

    fn show_next(&mut self) {
        self.current = self.pending.pop_front();
        self.deadline = self
            .current
            .as_ref()
            .map(|view| Instant::now() + view.timeout());
    }

    fn clear(&mut self) {
        self.current = None;
        self.pending.clear();
        self.deadline = None;
    }

    /* Same content in the notification surface and in the Quick Menu copy. */
    fn apply(&mut self, ui: &QuickMenuWindow, card: &NotificationWindow) {
        ui.set_toast_visible(self.current.is_some());
        let Some(view) = self.current.as_ref() else {
            /* The next card, even with the same text, starts fresh. */
            ui.set_toast_title("".into());
            ui.set_toast_detail("".into());
            card.set_toast_title("".into());
            card.set_toast_detail("".into());
            return;
        };
        let text_changed = card.get_toast_title().as_str() != view.title
            || card.get_toast_detail().as_str() != view.detail;
        if text_changed {
            /* New text scrolls from its beginning: blank the labels and
             * evaluate the marquee position so it snaps back to 0 before
             * the new text animates from there. */
            ui.set_toast_title("".into());
            ui.set_toast_detail("".into());
            card.set_toast_title("".into());
            card.set_toast_detail("".into());
            let _ = ui.get_toast_marquee_offset();
            let _ = card.get_toast_marquee_offset();
        }
        ui.set_toast_icon(view.icon);
        ui.set_toast_severity(view.severity);
        ui.set_toast_title(view.title.as_str().into());
        ui.set_toast_detail(view.detail.as_str().into());
        ui.set_toast_battery(view.battery);
        ui.set_toast_progress(view.progress);
        card.set_toast_icon(view.icon);
        card.set_toast_severity(view.severity);
        card.set_toast_title(view.title.as_str().into());
        card.set_toast_detail(view.detail.as_str().into());
        card.set_toast_battery(view.battery);
        card.set_toast_progress(view.progress);
        if text_changed {
            /* Long text extends the card's lifetime so it can be read at the
             * marquee's constant speed. */
            let scroll_ms = card.get_toast_scroll_time().max(0) as u64;
            if scroll_ms > 0 {
                let readable = Instant::now()
                    + Duration::from_millis(scroll_ms + TOAST_READ_HOLD_MS);
                self.deadline = self.deadline.map(|deadline| deadline.max(readable));
            }
        }
    }
}

/*
 * The persistent notification surface. It has its own Slint window and its
 * own event queue, and is mapped/unmapped only by the notification's own
 * lifetime, never by the Quick Menu opening or closing.
 */
struct Notifier {
    toasts: Toasts,
    card: NotificationWindow,
    state: WaylandState,
    queue: EventQueue<WaylandState>,
    qh: QueueHandle<WaylandState>,
    mapped: bool,
}

impl Notifier {
    fn apply(&mut self, ui: &QuickMenuWindow) {
        self.toasts.apply(ui, &self.card);
    }

    fn redraw(&mut self, conn: &Connection) -> Result<(), Box<dyn std::error::Error>> {
        self.state.render_and_commit(self.card.window(), &self.qh, conn)?;
        self.queue.roundtrip(&mut self.state)?;
        if self.state.needs_rescale() {
            self.state.render_and_commit(self.card.window(), &self.qh, conn)?;
            self.queue.roundtrip(&mut self.state)?;
        }
        Ok(())
    }

    fn map(&mut self, conn: &Connection) -> Result<(), Box<dyn std::error::Error>> {
        self.state.create_overlay(&self.qh, SurfaceKind::Toast)?;
        conn.flush()?;
        let _ = wait_for_configure(&mut self.queue, &mut self.state)?;
        self.card.show()?;
        self.redraw(conn)?;
        eprintln!("quick-menu: mapped notification surface capture=0");
        Ok(())
    }

    fn unmap(&mut self, conn: &Connection) {
        if self.state.commit_unmap() {
            let _ = conn.flush();
            let _ = self.queue.roundtrip(&mut self.state);
        }
        self.state.destroy_overlay();
        let _ = self.card.hide();
        self.mapped = false;
        eprintln!("quick-menu: unmapped notification surface");
    }

    /* Bring the surface in line with the visible notification. Hidden while
     * the lifecycle curtain is shown so it never covers it. */
    fn sync(&mut self, conn: &Connection, suppressed: bool) {
        let want = self.toasts.current.is_some() && !suppressed;
        let result = if want && !self.mapped {
            let result = self.map(conn);
            self.mapped = result.is_ok();
            result
        } else if want {
            self.redraw(conn)
        } else {
            if self.mapped {
                self.unmap(conn);
            }
            Ok(())
        };
        if let Err(error) = result {
            eprintln!("quick-menu: notification surface failed={}", error);
            self.unmap(conn);
        }
    }

    fn clear(&mut self, ui: &QuickMenuWindow, conn: &Connection) {
        self.toasts.clear();
        self.apply(ui);
        self.sync(conn, true);
    }
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

    if !visible && ui.get_selected_index() == 5 {
        ui.set_selected_index(3);
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

fn selectable_menu_indices(ui: &QuickMenuWindow) -> Vec<i32> {
    let mut indices = vec![3];
    if ui.get_brightness_visible() {
        indices.push(5);
    }
    if ui.get_home_music_playing() {
        indices.push(7);
    }
    if ui.get_switch_user_visible() {
        indices.push(6);
    }
    indices.extend_from_slice(&[4, 0, 1, 2]);
    indices
}

fn first_selectable_index(_ui: &QuickMenuWindow) -> i32 {
    /* The single Audio control is always the first Display/Audio row. */
    3
}

fn move_menu_selection(ui: &QuickMenuWindow, delta: i32) {
    let indices = selectable_menu_indices(ui);
    if indices.is_empty() {
        return;
    }

    let current = ui.get_selected_index();
    let position = indices.iter().position(|value| *value == current).unwrap_or(0) as i32;
    let next = (position + delta).rem_euclid(indices.len() as i32) as usize;
    ui.set_selected_index(indices[next]);
}

fn write_menu_state(ui: &QuickMenuWindow, mapped: bool) {
    if !mapped {
        let _ = fs::remove_file(STATE_FILE);
        return;
    }

    let _ = fs::write(
        STATE_FILE,
        format!(
            "mapped=1\nselected={}\nconfirm_face={}\nback_face={}\naudio_output={}\nbrightness_visible={}\nbrightness={}\n",
            ui.get_selected_index(),
            ui.get_confirm_face_position(),
            ui.get_back_face_position(),
            ui.get_audio_output_label(),
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
    refresh_i18n(ui);
    let _ = fs::remove_file(MAPPED_FILE);
    let _ = fs::remove_file(SURFACE_FILE);

    ui.set_lifecycle_active(false);
    ui.set_lifecycle_mode("".into());
    ui.set_line_progress(1.0);
    ui.set_curtain_progress(1.0);
    ui.set_menu_panel_visible(true);
    ui.set_surface_mode(SurfaceKind::Full.slint_mode());
    ui.set_volume_osd_visible(false);
    refresh_hint_mapping(ui);
    apply_display_snapshot(ui, read_display_snapshot());
    apply_battery_snapshot(ui, read_battery_snapshot());
    apply_audio_snapshot(ui, &read_audio_snapshot());
    refresh_system_profile(ui);
    ui.set_switch_user_visible(switch_user_available());
    ui.set_selected_index(first_selectable_index(ui));

    state.create_overlay(qh, SurfaceKind::Full)?;
    conn.flush()?;

    /* Focus scrolling is a pure Slint binding of the configured size. */
    let (width, height) = wait_for_configure(queue, state)?;

    ui.show()?;

    let (rendered_width, rendered_height) =
        state.render_and_commit(ui.window(), qh, conn)?;

    /*
     * A sync roundtrip after the buffer commit means the compositor has
     * processed the configure ack and the mapped-buffer commit. Only now
     * is the protocol-level mapped marker published.
     */
    queue.roundtrip(state)?;
    if state.needs_rescale() {
        state.render_and_commit(ui.window(), qh, conn)?;
        queue.roundtrip(state)?;
    }

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
    state.render_and_commit(ui.window(), qh, conn)?;
    queue.roundtrip(state)?;
    if state.needs_rescale() {
        state.render_and_commit(ui.window(), qh, conn)?;
        queue.roundtrip(state)?;
    }
    write_menu_state(ui, true);
    Ok(())
}


fn map_passive(
    ui: &QuickMenuWindow,
    kind: SurfaceKind,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    /*
     * Volume OSD and notifications use the already-qualified layer-shell
     * overlay process, but never publish Quick Menu mapped markers and never
     * grab controller input. Presentation only: the owning services hold
     * the policy.
     */
    ui.set_lifecycle_active(false);
    ui.set_menu_panel_visible(false);
    ui.set_surface_mode(kind.slint_mode());

    state.create_overlay(qh, kind)?;
    conn.flush()?;
    let _ = wait_for_configure(queue, state)?;
    ui.show()?;
    redraw_surface(ui, queue, state, qh, conn)?;

    eprintln!("quick-menu: mapped passive surface={:?} capture=0", kind);
    Ok(())
}

fn redraw_surface(
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    state.render_and_commit(ui.window(), qh, conn)?;
    queue.roundtrip(state)?;
    if state.needs_rescale() {
        state.render_and_commit(ui.window(), qh, conn)?;
        queue.roundtrip(state)?;
    }
    Ok(())
}

fn unmap_passive(
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    conn: &Connection,
) {
    if state.commit_unmap() {
        let _ = conn.flush();
        let _ = queue.roundtrip(state);
    }

    state.destroy_overlay();
    let _ = ui.hide();
    eprintln!("quick-menu: unmapped passive surface");
}

/*
 * Bring the main window's surfaces in line with what is visible. With the
 * Quick Menu open everything is drawn in its full-screen surface; otherwise
 * the volume OSD surface is created, redrawn or removed. (Notifications have
 * their own surface: Notifier.) Errors are returned only for the Quick Menu
 * surface; a failed passive surface is dropped and retried on the next
 * change.
 */
fn present(
    ui: &QuickMenuWindow,
    mapped: bool,
    passive: &mut Option<SurfaceKind>,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    if mapped {
        return redraw_overlay(ui, queue, state, qh, conn);
    }

    let want = ui.get_volume_osd_visible().then_some(SurfaceKind::Volume);

    if want == *passive {
        if passive.is_some() {
            if let Err(error) = redraw_surface(ui, queue, state, qh, conn) {
                eprintln!("quick-menu: passive redraw failed={}", error);
                unmap_passive(ui, queue, state, conn);
                *passive = None;
            }
        }
        return Ok(());
    }

    if passive.is_some() {
        unmap_passive(ui, queue, state, conn);
        *passive = None;
    }
    if let Some(kind) = want {
        match map_passive(ui, kind, queue, state, qh, conn) {
            Ok(()) => *passive = Some(kind),
            Err(error) => {
                eprintln!("quick-menu: passive surface {:?} failed={}", kind, error);
                unmap_passive(ui, queue, state, conn);
            }
        }
    }
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
    if let Some((logical_width, logical_height)) = state.configured_size {
        /* Physical surface size: the same scale render_and_commit uses. */
        let scale120 = if state.viewport.is_some() { state.scale120.max(120) } else { 120 };
        let width = (logical_width * scale120 + 60) / 120;
        let height = (logical_height * scale120 + 60) / 120;
        let (split, line_fraction, line_px) = splash::geometry(width, height);
        ui.set_splash_image(splash::render(mode, width, height));
        ui.set_splash_split(split);
        ui.set_splash_line_fraction(line_fraction);
        ui.set_splash_line_height(line_px as f32 * 120.0 / scale120 as f32);
    }
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

    if state.commit_unmap() {
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
    notifier: &mut Notifier,
    _mapped: &mut bool,
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
            play_named_sound("restart");
            thread::sleep(Duration::from_millis(220));
            set_menu_capture(false);
            notifier.clear(ui, conn);
            animate_shutdown("reboot", ui, queue, state, qh, conn)?;
            spawn_lifecycle_action("restart");
        }
        2 => {
            flush_brightness(ui, brightness_dirty)?;
            play_named_sound("poweroff");
            thread::sleep(Duration::from_millis(300));
            set_menu_capture(false);
            notifier.clear(ui, conn);
            animate_shutdown("poweroff", ui, queue, state, qh, conn)?;
            spawn_lifecycle_action("poweroff");
        }
        6 if ui.get_switch_user_visible() => { flush_brightness(ui, brightness_dirty)?; let _=users_command("REQUEST_SWITCH"); unmap_overlay(ui,queue,state,conn); *_mapped=false; }
        7 if ui.get_home_music_playing() => {
            /* Skip keeps the Quick Menu open. AudioService switches tracks
             * asynchronously; a slow track change must never be treated as a
             * failed action (which unmaps the overlay). */
            flush_brightness(ui, brightness_dirty)?;
            thread::spawn(|| {
                if let Err(error) = next_home_music() {
                    eprintln!("quick-menu: skip track failed={error}");
                }
            });
        }
        _ => {}
    }

    Ok(())
}

fn handle_event(
    event: &LogicalEvent,
    ui: &QuickMenuWindow,
    notifier: &mut Notifier,
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
            notifier.clear(ui, conn);
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;

            append_action_log(
                "action=sleep phase=overlay-unmapped-after-confirm-release",
            );

            let result = run_suspend_sync();
            notifier.toasts.resumed_at = Some(Instant::now());
            result?;
        } else if event.action == "menu_confirm" {
            /*
             * Never carry a stale press transaction into a later menu state.
             */
            *pending_sleep = false;
        }

        return Ok(());
    }

    if *mapped || event.action == "quick_menu" {
        let lifecycle_confirm = event.action == "menu_confirm"
            && *mapped
            && (ui.get_selected_index() == 1 || ui.get_selected_index() == 2);
        let slider_adjustment = *mapped
            && (event.action == "menu_left" || event.action == "menu_right")
            && (ui.get_selected_index() == 3 ||
                (ui.get_selected_index() == 5 && ui.get_brightness_visible()));
        if !lifecycle_confirm && !slider_adjustment {
            play_ui_sound(event.action.as_str());
        }
    }

    if *mapped && ui.get_audio_output_dropdown_open() {
        match event.action.as_str() {
            "menu_up" => move_audio_dropdown(ui, -1),
            "menu_down" => move_audio_dropdown(ui, 1),
            "menu_confirm" => apply_audio_dropdown(ui),
            "menu_back" => ui.set_audio_output_dropdown_open(false),
            _ => {}
        }
        redraw_overlay(ui, queue, state, qh, conn)?;
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
            move_menu_selection(ui, -1);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_down" if *mapped && !ui.get_lifecycle_active() => {
            move_menu_selection(ui, 1);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_left" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == 4 =>
        {
            cycle_system_profile(ui, -1);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_right" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == 4 =>
        {
            cycle_system_profile(ui, 1);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_left" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == 3
            && ui.get_audio_volume_visible() =>
        {
            if let Err(error) = adjust_audio_volume(ui, -1) {
                eprintln!("quick-menu: in-menu volume down failed={}", error);
            }
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_left" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_brightness_visible()
            && ui.get_selected_index() == 5 =>
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
            && ui.get_selected_index() == 3
            && ui.get_audio_volume_visible() =>
        {
            if let Err(error) = adjust_audio_volume(ui, 1) {
                eprintln!("quick-menu: in-menu volume up failed={}", error);
            }
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_right" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_brightness_visible()
            && ui.get_selected_index() == 5 =>
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
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == 4 =>
        {
            cycle_system_profile(ui, 1);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == 3 =>
        {
            open_audio_dropdown(ui);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && (ui.get_selected_index() <= 2 || ui.get_selected_index() == 6 || (ui.get_selected_index() == 7 && ui.get_home_music_playing())) =>
        {
            activate_selected(
                ui,
                notifier,
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
        MinimalSoftwareWindow::new(RepaintBufferType::SwappedBuffers);
    let render_window = slint_window.clone();
    let card_window =
        MinimalSoftwareWindow::new(RepaintBufferType::SwappedBuffers);
    let card_render_window = card_window.clone();

    platform::set_platform(Box::new(QuickPlatform {
        windows: RefCell::new(VecDeque::from([slint_window, card_window])),
        started: Instant::now(),
    }))?;

    let ui = QuickMenuWindow::new()?;
    let card = NotificationWindow::new()?;

    let conn = Connection::connect_to_env()?;
    let mut queue = conn.new_event_queue::<WaylandState>();
    let qh = queue.handle();

    let mut state = WaylandState::new(true, "menu");
    state.slint_window = Some(render_window);
    conn.display().get_registry(&qh, ());
    queue.roundtrip(&mut state)?;

    /* The notification surface lives on its own queue with its own globals,
     * so its configure/frame traffic never interleaves with the menu's. */
    let mut notify_queue = conn.new_event_queue::<WaylandState>();
    let notify_qh = notify_queue.handle();
    let mut notify_state = WaylandState::new(false, "notify");
    notify_state.slint_window = Some(card_render_window);
    conn.display().get_registry(&notify_qh, ());
    notify_queue.roundtrip(&mut notify_state)?;

    if !state.globals_ready() || !notify_state.globals_ready() {
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
    start_notify_subscription(tx.clone());
    refresh_i18n(&ui);

    let mut mapped = false;
    /* Volume OSD surface while the Quick Menu is closed. */
    let mut passive: Option<SurfaceKind> = None;
    let mut osd_deadline: Option<Instant> = None;
    let mut notifier = Notifier {
        toasts: Toasts::default(),
        card,
        state: notify_state,
        queue: notify_queue,
        qh: notify_qh,
        mapped: false,
    };
    let mut animation_deadline: Option<Instant> = None;
    let mut pending_sleep = false;
    let mut brightness_dirty = false;

    loop {
        platform::update_timers_and_animations();
        let wake = [osd_deadline, notifier.toasts.deadline, animation_deadline]
            .into_iter()
            .flatten()
            .min();
        let event = if let Some(deadline) = wake {
            let wait = deadline.saturating_duration_since(Instant::now());
            match rx.recv_timeout(wait) {
                Ok(e) => Some(e),
                Err(mpsc::RecvTimeoutError::Timeout) => None,
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        } else {
            match rx.recv() {
                Ok(e) => Some(e),
                Err(_) => break,
            }
        };

        /* Set when something visible changed and the surfaces must follow:
         * `dirty` for the Quick Menu / volume OSD, `toast_dirty` for the
         * notification surface. */
        let mut dirty = false;
        let mut toast_dirty = false;

        if let Some(event) = event {
            match event {
                AppEvent::Input(event) => {
                    eprintln!(
                        "quick-menu: logical-action={} state={} mapped={} passive={:?}",
                        event.action,
                        if event.pressed { "pressed" } else { "released" },
                        mapped,
                        passive
                    );

                    /* Physical Vol+/Vol- are global semantic actions. AudioService
                     * decides whether the active route supports volume. HDMI never
                     * receives a mixer write; its OSD explains that the TV/receiver
                     * owns volume instead. */
                    if event.pressed
                        && (event.action == "volume_up" || event.action == "volume_down")
                    {
                        let delta = if event.action == "volume_up" { 1 } else { -1 };
                        match adjust_audio_volume(&ui, delta) {
                            Ok(_) => {
                                ui.set_volume_osd_visible(true);
                                osd_deadline = Some(
                                    Instant::now()
                                        + Duration::from_millis(VOLUME_OSD_TIMEOUT_MS),
                                );
                                dirty = true;
                            }
                            Err(error) => {
                                eprintln!("quick-menu: volume action failed={}", error);
                            }
                        }
                    } else if event.pressed
                        && event.action == "quick_menu"
                        && passive.is_some()
                        && !mapped
                    {
                        /* Hotkey can promote a transient volume OSD surface into the
                         * real Quick Menu.  Destroy the non-interactive surface first
                         * so normal mapped markers/capture are established exactly once.
                         * The notification surface is not involved. */
                        unmap_passive(&ui, &mut queue, &mut state, &conn);
                        passive = None;
                        osd_deadline = None;
                        map_overlay(&ui, &mut queue, &mut state, &qh, &conn)?;
                        mapped = true;
                    } else if passive.is_some() && !mapped {
                        /* Passive surfaces are display-only.  Do not let ordinary
                         * menu navigation affect an invisible Quick Menu. */
                    } else {
                        let was_mapped = mapped;
                        if let Err(error) = handle_event(
                            &event,
                            &ui,
                            &mut notifier,
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
                        /* A volume OSD still showing when the menu closes moves to
                         * its own surface. */
                        if was_mapped && !mapped {
                            dirty = true;
                        }
                    }
                }
                AppEvent::StatusChanged => {
                    apply_battery_snapshot(&ui, read_battery_snapshot());
                    if mapped {
                        dirty = true;
                    }
                }
                AppEvent::Notify(notification, received) => {
                    let stale = notifier
                        .toasts
                        .resumed_at
                        .is_some_and(|resume| received < resume);
                    if !stale && !ui.get_lifecycle_active() {
                        let changed = if notification.verb == NotifyVerb::Dismiss {
                            notifier.toasts.dismiss(&notification.id)
                        } else {
                            refresh_i18n(&ui);
                            match toast_view(&ui, &notification) {
                                Some(view) => notifier
                                    .toasts
                                    .post(view, notification.verb == NotifyVerb::Update),
                                None => false,
                            }
                        };
                        if changed {
                            notifier.apply(&ui);
                            toast_dirty = true;
                        }
                    }
                }
            }
        }

        let now = Instant::now();
        if osd_deadline.is_some_and(|deadline| now >= deadline) {
            ui.set_volume_osd_visible(false);
            osd_deadline = None;
            dirty = true;
        }
        if notifier.toasts.expire(now) {
            notifier.apply(&ui);
            toast_dirty = true;
        }
        if animation_deadline.is_some_and(|deadline| now >= deadline) {
            animation_deadline = None;
            dirty = mapped || passive.is_some();
            toast_dirty = notifier.mapped;
        }
        /* The open Quick Menu draws its own copy of the notification. */
        if toast_dirty && mapped {
            dirty = true;
        }

        if dirty {
            if let Err(error) = present(
                &ui, mapped, &mut passive, &mut queue, &mut state, &qh, &conn,
            ) {
                eprintln!("quick-menu: redraw failed={}", error);
                if mapped {
                    let _ = flush_brightness(&ui, &mut brightness_dirty);
                    unmap_overlay(&ui, &mut queue, &mut state, &conn);
                    mapped = false;
                    let _ = present(
                        &ui, mapped, &mut passive, &mut queue, &mut state, &qh, &conn,
                    );
                }
            }
        }

        if toast_dirty {
            notifier.sync(&conn, ui.get_lifecycle_active());
        }

        /* Keep producing frames only while an animation (marquee) runs. */
        if animation_deadline.is_none()
            && (mapped || passive.is_some() || notifier.mapped)
            && (ui.window().has_active_animations()
                || notifier.card.window().has_active_animations())
        {
            animation_deadline =
                Some(Instant::now() + Duration::from_millis(ANIMATION_FRAME_MS));
        }
    }

    if mapped {
        let _ = flush_brightness(&ui, &mut brightness_dirty);
        unmap_overlay(&ui, &mut queue, &mut state, &conn);
    } else if passive.is_some() {
        unmap_passive(&ui, &mut queue, &mut state, &conn);
    }
    if notifier.mapped {
        notifier.unmap(&conn);
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
