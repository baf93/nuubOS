mod gpu;
mod ime;
mod splash;
mod splash_core;
slint::include_modules!();

use slint::platform::{
    self,
    software_renderer::{
        MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType, TargetPixel,
    },
    Platform, PlatformError, WindowAdapter, WindowEvent,
};
use slint::{
    ComponentHandle, Model, ModelRc, PhysicalSize, Rgba8Pixel, SharedPixelBuffer, SharedString,
    VecModel,
};

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
const CONTROLLERSCTL: &str = "/usr/bin/nuubos-controllersctl";
const STATUS_STATE: &str = "/run/nuubos/statusd.state";
const STATUS_SOCKET: &str = "/run/nuubos/statusd.sock";
const NOTIFY_SOCKET: &str = "/run/nuubos/notifyd.sock";
const EMULATION_SOCKET: &str = "/run/nuubos/emud.sock";
const LIBRARY_SOCKET: &str = "/run/nuubos/libraryd.sock";
const STREAM_SOCKET: &str = "/run/nuubos/streamd.sock";
const PORTS_SOCKET: &str = "/run/nuubos/portsd.sock";
/* PORT section (EPIC-058): a PortMaster port or PortMaster's interface. */
const PORT_RESUME_ROW: i32 = 44;
const PORT_QUIT_ROW: i32 = 45;
const WEB_SOCKET: &str = "/run/nuubos/webd.sock";
/* TERMINAL section (nuubos-termd): Resume, Keyboard, Quit Terminal. */
const TERM_SOCKET: &str = "/run/nuubos/termd.sock";
const TERM_RESUME_ROW: i32 = 46;
const TERM_KEYBOARD_ROW: i32 = 47;
const TERM_QUIT_ROW: i32 = 48;
/* RETROARCH section: RetroArch's own menu without a game (nuubos-emud
 * MENU, the RetroArch app): Resume, Quit RetroArch. */
const RA_RESUME_ROW: i32 = 49;
const RA_QUIT_ROW: i32 = 50;
/* The keyboard sheet types into the terminal: it reserves its height
 * (exclusive zone), so labwc shrinks the terminal above it. */
static TERMINAL_SHEET: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
const MEDIA_SOCKET: &str = "/run/nuubos/mediad.sock";
const CAST_SOCKET: &str = "/run/nuubos/castd.sock";
/* Cast Screen row (DISPLAY & AUDIO). */
const CAST_ROW: i32 = 39;
/* Media Screen Off (nuubos-displayctl): the menu turns the screen back on. */
const SCREEN_OFF_FILE: &str = "/run/nuubos/screen-off";
const REGIONAL_SOCKET: &str = "/run/nuubos/regionald.sock";
/* Web Mode keyboard sheet height (logical px, full output width). */
const KEYBOARD_SURFACE_HEIGHT: u32 = 270;


const FRAME_INTERVAL_NS: u64 = 16_666_667;
const VOLUME_OSD_TIMEOUT_MS: u64 = 1500;
/* Press Power to Sleep (Settings → Power, EPIC-045): a press released
 * before this sleeps, holding it this long opens the Power OSD. */
const POWER_HOLD_MS: u64 = 600;
const TOAST_INFO_MS: u64 = 4000;
const TOAST_ALERT_MS: u64 = 6000;
/* After a long line has scrolled to its end it stays readable this long. */
const TOAST_READ_HOLD_MS: u64 = 1500;
const TOAST_QUEUE_LIMIT: usize = 6;
/* Frame pacing while a Slint animation (marquee) is running; no frames are
 * produced otherwise. */
const ANIMATION_FRAME_MS: u64 = 33;

/* Window of one Quick Menu component: rendered on the GPU (FemtoVG on
 * OpenGL ES, frames handed to labwc as dmabufs) or, when EGL is unavailable
 * or NUUBOS_UI_RENDERER=software, by the CPU into wl_shm buffers. */
#[derive(Clone)]
enum UiWindow {
    Gpu(Rc<gpu::GpuWindow>),
    Soft(Rc<MinimalSoftwareWindow>),
}

impl UiWindow {
    fn new(egl: Option<&Rc<gpu::Egl>>) -> Self {
        if let Some(egl) = egl {
            match gpu::GpuWindow::new(egl) {
                Ok(window) => return UiWindow::Gpu(window),
                Err(error) => eprintln!("quick-menu: GPU window failed={}, using software", error),
            }
        }
        UiWindow::Soft(MinimalSoftwareWindow::new(RepaintBufferType::SwappedBuffers))
    }

    fn adapter(&self) -> Rc<dyn WindowAdapter> {
        match self {
            UiWindow::Gpu(window) => window.clone(),
            UiWindow::Soft(window) => window.clone(),
        }
    }
}

/* Hands out one window per component, in creation order:
 * QuickMenuWindow, NotificationWindow, StreamHudWindow. */
struct QuickPlatform {
    windows: RefCell<VecDeque<Rc<dyn WindowAdapter>>>,
    started: Instant,
}

impl Platform for QuickPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        let window = self.windows.borrow_mut().pop_front().ok_or_else(|| {
            PlatformError::Other("no window left for component".into())
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
/* Stream performance overlay: a bottom strip for two lines in a pill
 * (StreamHudWindow). */
const HUD_SURFACE_HEIGHT: u32 = 58;

/* Layer surface variants. Passive surfaces (volume OSD, notification) are
 * sized to their card so an update costs a few KB of rendering. The
 * notification has its own persistent surface (see Notifier). */
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum SurfaceKind {
    Full,
    Volume,
    Toast,
    Keyboard,
    StreamHud,
    /* Game closing screen: full screen, display only. */
    Closing,
}

impl SurfaceKind {
    fn slint_mode(self) -> i32 {
        match self {
            SurfaceKind::Volume => 1,
            SurfaceKind::Keyboard => 2,
            SurfaceKind::Closing => 3,
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
    slint_window: Option<UiWindow>,
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
            match kind {
                SurfaceKind::Toast => "nuubOS Notification".into(),
                SurfaceKind::StreamHud => "nuubOS Stream Overlay".into(),
                _ => "nuubOS Quick Menu".into(),
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
            SurfaceKind::StreamHud => {
                layer_surface.set_size(0, HUD_SURFACE_HEIGHT);
                layer_surface.set_anchor(Anchor::Bottom | Anchor::Left | Anchor::Right);
            }
            SurfaceKind::Keyboard => {
                /* The Web Mode keyboard: a full-width bottom sheet over the
                 * browser, which keeps its size; over the terminal it
                 * reserves its height so the prompt stays visible. */
                layer_surface.set_size(0, KEYBOARD_SURFACE_HEIGHT);
                layer_surface.set_anchor(Anchor::Bottom | Anchor::Left | Anchor::Right);
            }
            SurfaceKind::Full | SurfaceKind::Closing => {
                layer_surface.set_size(0, 0);
                layer_surface.set_anchor(
                    Anchor::Top | Anchor::Bottom | Anchor::Left | Anchor::Right,
                );
            }
        }
        layer_surface.set_exclusive_zone(
            if kind == SurfaceKind::Keyboard && TERMINAL_SHEET.load(std::sync::atomic::Ordering::SeqCst) {
                KEYBOARD_SURFACE_HEIGHT as i32
            } else {
                -1
            },
        );
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

        /* The EGL surface wraps the wl_surface: release it first. */
        if let Some(UiWindow::Gpu(window)) = self.slint_window.as_ref() {
            window.detach();
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

        if let Some(UiWindow::Gpu(gpu_window)) = self.slint_window.clone() {
            let surface = self
                .surface
                .as_ref()
                .ok_or("layer surface disappeared")?
                .clone();
            /* Double-buffered state, applied by the commit in eglSwapBuffers. */
            if let Some(viewport) = self.viewport.as_ref() {
                viewport.set_destination(logical_width as i32, logical_height as i32);
            }
            gpu_window
                .render(&surface, width, height)
                .map_err(|error| -> Box<dyn std::error::Error> { error.to_string().into() })?;
            conn.flush()?;
            return Ok((logical_width, logical_height));
        }

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

        let slint_window = match self.slint_window.as_ref() {
            Some(UiWindow::Soft(window)) => window.clone(),
            _ => return Err("Slint window unavailable".into()),
        };
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
/* No Quick Menu before a profile is in use or while nuubUI owns the whole
 * screen for a guided flow: user picker, first setup, safe mode, a
 * full-screen operation (nuubUI writes the context). The Power OSD stays
 * available everywhere but over an operation. */
fn menu_allowed() -> bool {
    let c = fs::read_to_string(UI_CONTEXT).unwrap_or_default();
    !matches!(c.trim(), "user-picker" | "oob" | "safe-mode" | "operation")
}

/* nuubUI's full-screen operation screen (system update, SD2 restore) shows
 * its own progress: no notification card over it (user request 2026-10-12). */
fn operation_screen() -> bool {
    fs::read_to_string(UI_CONTEXT).is_ok_and(|c| c.trim() == "operation")
}

/* Switch User is never offered over a session (game, port, stream, web):
 * a session's end shows it again only when no other one runs. */
fn refresh_switch_user(ui: &QuickMenuWindow) {
    ui.set_switch_user_visible(
        !ui.get_game_section_visible()
            && !ui.get_port_section_visible()
            && !ui.get_stream_section_visible()
            && !ui.get_web_section_visible()
            && !ui.get_term_section_visible()
            && !ui.get_ra_section_visible()
            && switch_user_available(),
    );
}

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

fn system_profile_label(ui: &QuickMenuWindow, profile: &str) -> String {
    match profile {
        "battery-saver" => tr(ui, 51, "Battery Saver"),
        _ => tr(ui, 52, "Auto"),
    }
}

fn refresh_system_profile(ui: &QuickMenuWindow) {
    match system_command("STATUS") {
        Ok(reply) => {
            let (requested, effective) = parse_system_profile(&reply);
            let label = if requested != effective {
                format!("{} • {}", system_profile_label(ui, &requested), system_profile_label(ui, &effective))
            } else {
                system_profile_label(ui, &requested)
            };
            ui.set_performance_profile_label(label.into());
            ui.set_battery_saver(effective == "battery-saver");
            ui.set_battery_saver_requested(requested == "battery-saver");
        }
        Err(error) => {
            eprintln!("quick-menu: System profile read failed={error}");
            ui.set_performance_profile_label(tr(ui, 241, "Unavailable").into());
        }
    }
}

/* nuubos-systemd power_key_sleep, read at each power key press. */
fn power_key_sleeps() -> bool {
    system_command("STATUS")
        .map(|reply| reply.lines().any(|line| line == "power_key_sleep=1"))
        .unwrap_or(false)
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
    usb_available: bool,
    headphones_available: bool,
    hdmi_available: bool,
    system_volume: i32,
    home_music_volume: i32,
    home_music_playing: bool,
    applications_volume: i32,
}

impl Default for AudioSnapshot {
    fn default() -> Self {
        Self {
            mode: "auto".to_owned(),
            selected: "speaker".to_owned(),
            volume: 100,
            volume_supported: true,
            bluetooth_available: false,
            usb_available: false,
            headphones_available: false,
            hdmi_available: false,
            system_volume: 60,
            home_music_volume: 60,
            home_music_playing: false,
            applications_volume: 100,
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
            "available.usb" => snapshot.usb_available = value == "1",
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
            "volume.usb" if snapshot.selected == "usb" => {
                snapshot.volume = value.parse::<i32>().unwrap_or(50);
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
            "volume.applications" => snapshot.applications_volume = value.parse::<i32>().unwrap_or(100),
            _ => {}
        }
    }

    /* The TV owns the volume on HDMI and while casting with sound. */
    snapshot.volume_supported = snapshot.selected != "hdmi" && snapshot.selected != "cast";
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
        "usb" => tr(ui, 1476, "USB Audio"),
        "analog" => tr(ui, 332, "Analog Audio"),
        "headphones" => tr(ui, 330, "Headphones"),
        "hdmi" => "HDMI".to_owned(),
        "cast" => tr(ui, 1202, "Screen Cast"),
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
    ui.set_game_volume(snapshot.applications_volume.clamp(0, 100));

    if snapshot.volume_supported {
        let volume = snapshot.volume.clamp(0, 100);
        /* 0% is audiod's hard mute (ToggleMute). */
        let level = if volume == 0 { tr(ui, 812, "Muted") } else { format!("{volume}%") };
        ui.set_audio_output_label(format!("{selected_name} • {level}").into());
        ui.set_audio_volume_label(level.into());
        ui.set_audio_volume_fraction(volume as f32 / 100.0);
        ui.set_volume_osd_detail(format!("{volume}%").into());
        ui.set_volume_osd_fraction(volume as f32 / 100.0);
        ui.set_volume_osd_muted(volume == 0);
    } else {
        ui.set_audio_output_label(format!("{selected_name} • TV").into());
        ui.set_audio_volume_label(tr(ui, 272, "TV").into());
        ui.set_audio_volume_fraction(0.0);
        ui.set_volume_osd_detail(tr(ui, 273, "Volume controlled by TV / receiver").into());
        ui.set_volume_osd_fraction(0.0);
        ui.set_volume_osd_muted(false);
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

    if snapshot.usb_available {
        options.push(("usb", tr(ui, 1476, "USB Audio")));
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
        "usb" => "usb",
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

    if snapshot.usb_available {
        modes.push("usb");
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
    change_audio_volume(ui, &format!("VOLUME ADJUST {delta}"))
}

/* Vol+/Vol-: Product Audio applies the user's Volume Button Step (GitHub #6). */
fn volume_key(ui: &QuickMenuWindow, up: bool) -> Result<AudioSnapshot, Box<dyn std::error::Error>> {
    change_audio_volume(ui, if up { "VOLUME KEY UP" } else { "VOLUME KEY DOWN" })
}

fn change_audio_volume(
    ui: &QuickMenuWindow,
    command: &str,
) -> Result<AudioSnapshot, Box<dyn std::error::Error>> {
    /* VOLUME ADJUST/KEY are atomic in Product Audio and already return a
     * complete STATUS snapshot. Do not precede every tick with another
     * STATUS IPC. */
    let reply = audio_command(command)?;
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
        "volume-change output={} command={} value={}",
        after.selected, command, after.volume
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
    /* Emulation Service session snapshot (nuubos-emud). */
    Game(GameSnapshot),
    /* What the game closing screen shows for the running game. */
    GameInfo(ClosingInfo),
    /* PC Game Streaming Service: a stream runs (nuubos-streamd). */
    Stream(StreamState),
    /* Ports Service: a port or PortMaster runs (nuubos-portsd). */
    Port(PortState),
    /* One statistics sample of the stream for the performance overlay
     * (streamd SUBSCRIBE_STATS, key=value fields). */
    StreamStats(String),
    StatusChanged,
    /* The output device changed its own volume (Bluetooth headset buttons). */
    DeviceVolume,
    Notify(Notification, Instant),
    /* Web Mode Service (nuubos-webd): browser running, zoom percent. */
    Web(bool, i32),
    /* Terminal (nuubos-termd): session running, on-screen keyboard on. */
    Terminal(bool, bool),
    /* Input method: a text field of the page has the focus or not. */
    Ime(ime::ImeState),
    /* Media music (nuubos-mediad): the MUSIC section. */
    Media(MediaState),
    /* Screen Casting Service (nuubos-castd): the Cast Screen row. */
    Cast(CastState),
}

#[derive(Debug, Clone, Default, PartialEq)]
struct MediaState {
    music: bool,
    playing: bool,
    title: String,
    artist: String,
    volume: i32,
}

impl From<ime::ImeState> for AppEvent {
    fn from(state: ime::ImeState) -> Self {
        AppEvent::Ime(state)
    }
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
const ICON_FAST_FORWARD: i32 = 12;

#[derive(Debug, Clone)]
struct ToastView {
    id: String,
    icon: i32,
    severity: i32,
    title: String,
    detail: String,
    battery: i32,
    progress: f32,
    /* A Live Notification (carries progress=, even -1): shown until its
     * completion, never timed out. */
    live: bool,
    /* nuubos-jobd id of a running job that may be cancelled (cancel=1). */
    job: String,
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

fn format_arg2(template: &str, first: &str, second: &str) -> String {
    template.replace("{0}", first).replace("{1}", second)
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
        /* Another device asked to sync (nuubos-syncd): accepted in
         * Settings → Connectivity → Syncthing. */
        "sync.request" => (
            ICON_WIFI,
            SEVERITY_INFO,
            tr(ui, 1083, "New sync request"),
            n.text("name").to_owned(),
            -1,
        ),
        "wifi.lost" => (
            ICON_WIFI,
            SEVERITY_WARNING,
            tr(ui, 385, "Wi-Fi connection lost"),
            n.text("ssid").to_owned(),
            -1,
        ),
        "media.failed" => (ICON_FAILED, SEVERITY_WARNING, tr(ui, 667, "The media could not be played"), String::new(), -1),
        "cast.started" => (
            ICON_DONE,
            SEVERITY_SUCCESS,
            format_arg(&tr(ui, 1203, "Casting to {0}"), n.text("name")),
            if n.text("delay") == "high" {
                tr(ui, 1204, "The picture reaches the TV a few seconds late")
            } else {
                String::new()
            },
            -1,
        ),
        "cast.ended" => (
            if n.text("reason") == "network" { ICON_FAILED } else { ICON_DONE },
            if n.text("reason") == "network" { SEVERITY_WARNING } else { SEVERITY_INFO },
            if n.text("reason") == "network" {
                tr(ui, 1206, "Casting connection lost")
            } else {
                tr(ui, 1205, "Casting ended")
            },
            n.text("name").to_owned(),
            -1,
        ),
        "cast.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            tr(ui, 1207, "Casting could not start"),
            match n.text("reason") {
                "unreachable" => tr(ui, 1208, "The receiver could not be reached"),
                "refused" => tr(ui, 1209, "The receiver refused the connection"),
                "unsupported" => tr(ui, 1210, "The receiver cannot show this stream"),
                _ => n.text("name").to_owned(),
            },
            -1,
        ),
        "cast.encoder.installed" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 1211, "Miracast encoder installed"), String::new(), -1),
        "cast.encoder.failed" => (
            ICON_FAILED,
            SEVERITY_WARNING,
            tr(ui, 1212, "The Miracast encoder could not be downloaded"),
            String::new(),
            -1,
        ),
        "web.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            if n.text("reason") == "crash" {
                tr(ui, 690, "The browser closed unexpectedly")
            } else {
                tr(ui, 689, "The browser could not be started")
            },
            String::new(),
            -1,
        ),
        "web.bookmark.added" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 685, "Bookmark added"), n.text("title").to_owned(), -1),
        "media.connected" => (ICON_STORAGE, SEVERITY_INFO, tr(ui, 646, "USB drive connected"), n.text("name").to_owned(), -1),
        "media.removed" => (ICON_STORAGE, SEVERITY_INFO, tr(ui, 647, "USB drive removed"), n.text("name").to_owned(), -1),
        "game.screenshot.saved" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 593, "Screenshot saved"), String::new(), -1),
        "game.screenshot.failed" => (
            ICON_FAILED,
            SEVERITY_WARNING,
            tr(ui, 594, "The screenshot could not be saved"),
            String::new(),
            -1,
        ),
        /* Background jobs (EPIC-007): one Live Notification per job. */
        "job" => {
            let title = match n.text("type") {
                "support-bundle" => tr(ui, 595, "Support bundle"),
                "file-copy" => tr(ui, 623, "Copying files"),
                "file-move" => tr(ui, 624, "Moving files"),
                "update-download" => tr(ui, 660, "Downloading update"),
                "update-install" => tr(ui, 984, "Installing update"),
                "scrape-game" | "scrape-bulk" => tr(ui, 571, "Getting metadata"),
                "core-install" => tr(ui, 1254, "Downloading emulator"),
                "cheats-update" => tr(ui, 1242, "Downloading cheats"),
                "sd2-backup" => tr(ui, 982, "Backup"),
                "sd2-restore" => tr(ui, 1017, "Restore from Backup"),
                "sd2-format" => tr(ui, 969, "Formatting SD2"),
                "sd2-prepare" => tr(ui, 973, "Creating game folders"),
                _ => return None,
            };
            let failure = match n.text("reason") {
                "not-found" => tr(ui, 573, "No metadata found for this game"),
                "quota" => tr(ui, 574, "Daily metadata limit reached, try again later"),
                "auth" => tr(ui, 575, "Check the account"),
                "network" => tr(ui, 494, "Check the network connection"),
                "unavailable" => tr(ui, 570, "The metadata service is not available in this build"),
                "space" => tr(ui, 493, "Not enough free space"),
                "conflict" => tr(ui, 943, "Restore it, or keep it aside and start a new backup"),
                "card" => tr(ui, 935, "Insert an SD2 card to back up"),
                "game" => tr(ui, 977, "Close the game first"),
                "busy" => tr(ui, 628, "In use; close what uses it first"),
                _ => tr(ui, 93, "Failed"),
            };
            let (icon, severity, detail) = match n.text("state") {
                /* A library scrape names the game it is on: a big library
                 * moves the bar by 1 % only every few dozen games. */
                "running" if n.text("type") == "scrape-bulk" && !n.text("step").is_empty() =>
                    (ICON_STORAGE, SEVERITY_INFO, n.text("step").to_owned()),
                "queued" | "running" => (ICON_STORAGE, SEVERITY_INFO, tr(ui, 91, "Running")),
                "succeeded" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 92, "Completed")),
                "cancelled" => (ICON_FAILED, SEVERITY_INFO, tr(ui, 596, "Cancelled")),
                "failed" => (ICON_FAILED, SEVERITY_ERROR, failure),
                _ => return None,
            };
            (icon, severity, title, detail, -1)
        }
        "game.state.saved" | "game.state.loaded" => (
            ICON_DONE,
            SEVERITY_SUCCESS,
            if n.event == "game.state.saved" {
                tr(ui, 424, "State saved")
            } else {
                tr(ui, 425, "State loaded")
            },
            n.int("slot")
                .map(|slot| format_arg(&tr(ui, 423, "Slot {0}"), &slot.to_string()))
                .unwrap_or_default(),
            -1,
        ),
        "game.hardcore" => (
            ICON_FAILED,
            SEVERITY_WARNING,
            tr(ui, 910, "Not available in Hardcore Mode"),
            String::new(),
            -1,
        ),
        "game.state.empty" => (
            ICON_FAILED,
            SEVERITY_WARNING,
            format_arg(
                &tr(ui, 426, "Slot {0} is empty"),
                &n.int("slot").unwrap_or(0).to_string(),
            ),
            String::new(),
            -1,
        ),
        "game.state.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            if n.text("op") == "load" {
                tr(ui, 428, "The state could not be loaded")
            } else {
                tr(ui, 427, "The state could not be saved")
            },
            n.int("slot")
                .map(|slot| format_arg(&tr(ui, 423, "Slot {0}"), &slot.to_string()))
                .unwrap_or_default(),
            -1,
        ),
        "game.slot" => (
            ICON_CONTROLLER,
            SEVERITY_INFO,
            tr(ui, 419, "State Slot"),
            format_arg(&tr(ui, 423, "Slot {0}"), &n.int("slot").unwrap_or(0).to_string()),
            -1,
        ),
        "game.screens" => (
            ICON_CONTROLLER,
            SEVERITY_INFO,
            tr(ui, 1302, "Screen Layout"),
            match n.text("layout") {
                "large" => tr(ui, 1304, "Large Top Screen"),
                "pip" => tr(ui, 1305, "Picture in Picture"),
                "single" => tr(ui, 1306, "Single Screen"),
                _ => tr(ui, 1303, "Two Screens"),
            },
            -1,
        ),
        "game.stylus" => {
            let control = match n.text("state") {
                "right" => Some(tr(ui, 1310, "Right Stick")),
                "left" => Some(tr(ui, 1311, "Left Stick")),
                "dpad" => Some(tr(ui, 1312, "D-Pad")),
                _ => None,
            };
            match control {
                Some(control) => (
                    ICON_CONTROLLER,
                    SEVERITY_INFO,
                    tr(ui, 1308, "Stylus on"),
                    format_arg(&tr(ui, 1313, "{0} moves it, L2 touches"), &control),
                    -1,
                ),
                None => (ICON_CONTROLLER, SEVERITY_INFO, tr(ui, 1309, "Stylus off"), String::new(), -1),
            }
        }
        "game.fastforward" => (
            ICON_FAST_FORWARD,
            SEVERITY_INFO,
            if n.text("state") == "on" {
                tr(ui, 429, "Fast-forward on")
            } else {
                tr(ui, 430, "Fast-forward off")
            },
            String::new(),
            -1,
        ),
        "stream.paired" => (
            ICON_DONE,
            SEVERITY_SUCCESS,
            tr(ui, 462, "PC paired"),
            n.text("name").to_owned(),
            -1,
        ),
        "stream.pair.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            tr(ui, 463, "Pairing failed"),
            n.text("name").to_owned(),
            -1,
        ),
        "stream.wake.failed" => (
            ICON_FAILED,
            SEVERITY_WARNING,
            format_arg(&tr(ui, 1188, "{0} did not wake up"), n.text("name")),
            tr(ui, 1189, "Check that Wake-on-LAN is enabled on the PC"),
            -1,
        ),
        "stream.steamlink.installed" => (
            ICON_DONE,
            SEVERITY_SUCCESS,
            tr(ui, 491, "Steam Link installed"),
            n.text("version").to_owned(),
            -1,
        ),
        "stream.steamlink.rolledback" => (
            ICON_FAILED,
            SEVERITY_WARNING,
            tr(ui, 854, "The new version did not start"),
            format_arg(&tr(ui, 855, "Version {0} restored"), n.text("version")),
            -1,
        ),
        "stream.steamlink.restored" => (
            ICON_DONE,
            SEVERITY_SUCCESS,
            tr(ui, 477, "Steam Link"),
            format_arg(&tr(ui, 855, "Version {0} restored"), n.text("version")),
            -1,
        ),
        "stream.steamlink.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            tr(ui, 492, "Steam Link could not be downloaded"),
            match n.text("reason") {
                "space" => tr(ui, 493, "Not enough free space"),
                "network" => tr(ui, 494, "Check the network connection"),
                _ => String::new(),
            },
            -1,
        ),
        "stream.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            match n.text("reason") {
                "steamlink" => tr(ui, 495, "Steam Link closed unexpectedly"),
                "connection" => tr(ui, 465, "The stream was interrupted"),
                _ => tr(ui, 464, "The stream could not be started"),
            },
            match n.text("reason") {
                "unreachable" => tr(ui, 452, "The PC could not be reached"),
                "unpaired" => tr(ui, 466, "The PC must be paired again"),
                "app" => tr(ui, 467, "The application is not available on the PC"),
                _ => n.text("name").to_owned(),
            },
            -1,
        ),
        "game.failed" => (
            ICON_FAILED,
            SEVERITY_ERROR,
            if n.text("reason") == "crash" {
                tr(ui, 432, "The game closed unexpectedly")
            } else {
                tr(ui, 431, "The game could not be started")
            },
            String::new(),
            -1,
        ),
        /* The games card in the TF2 slot (nuubos-storagectl). */
        "storage.sd2.inserted" => (ICON_STORAGE, SEVERITY_INFO, tr(ui, 932, "SD2 inserted"), n.text("name").to_owned(), -1),
        "storage.sd2.removed" => (ICON_STORAGE, SEVERITY_INFO, tr(ui, 939, "SD2 removed"),
            tr(ui, 975, "Its games come back when you insert it again"), -1),
        "storage.sd2.ejected" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 923, "SD2 ejected: safe to remove"), String::new(), -1),
        "storage.sd2.readonly" => (ICON_STORAGE, SEVERITY_WARNING, tr(ui, 944, "SD2 is read-only"), n.text("name").to_owned(), -1),
        "storage.sd2.unreadable" => (ICON_FAILED, SEVERITY_WARNING, tr(ui, 957, "SD2 can't be read"),
            tr(ui, 952, "You can format it in Settings › System › Storage"), -1),
        "storage.sd2.formatted" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 971, "SD2 formatted"), String::new(), -1),
        /* The daily update check (nuubos-update-auto). */
        "update.available" => (ICON_DONE, SEVERITY_INFO,
            tr(ui, 652, "nuubOS {0} available").replace("{0}", n.text("version")),
            tr(ui, 991, "Install it from Settings › System › Software Update"), -1),
        /* Outcome of an update, after the restart (nuubos-update-apply). */
        "update.installed" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 659, "Update installed"),
            format!("nuubOS {}", n.text("version")), -1),
        "update.rolledback" => (ICON_FAILED, SEVERITY_WARNING, tr(ui, 990, "The update could not be installed"),
            tr(ui, 658, "The previous version was restored"), -1),
        /* Diagnostic mode: the report of the last Restart / Power Off
         * (S99nuubos-diagnostics at the next start). */
        "diagnostics.bundle.saved" => (ICON_DONE, SEVERITY_SUCCESS, tr(ui, 502, "Report saved"),
            n.text("path").rsplit('/').next().unwrap_or("").to_owned(), -1),
        "game.storage_lost" => (ICON_STORAGE, SEVERITY_WARNING, tr(ui, 961, "SD2 removed: game saved and closed"), String::new(), -1),
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
        live: n.fields.contains_key("progress"),
        job: if n.event == "job" && n.text("cancel") == "1" {
            n.id.strip_prefix("job.").unwrap_or_default().to_owned()
        } else {
            String::new()
        },
    })
}

/*
 * One notification is visible at a time; later ones wait in a short queue.
 * A notification with the id of a visible or queued one replaces it in
 * place (EPIC-006 stable ids), so reconnects or progress never stack.
 * Live Notifications stay on screen until their completion (user request
 * 2026-10-09): a timed notification shows over a running one for its usual
 * time, then the oldest running one comes back.
 */
#[derive(Default)]
struct Toasts {
    current: Option<ToastView>,
    pending: VecDeque<ToastView>,
    /* Running Live Notifications, oldest first. */
    live: Vec<ToastView>,
    /* Id of the job whose Stop was confirmed, until it ends. */
    stopping: String,
    deadline: Option<Instant>,
    /* Notifications received before the last resume describe the sleep
     * transition itself (e.g. Bluetooth dropping) and are discarded. */
    resumed_at: Option<Instant>,
}

impl Toasts {
    /* Returns true when the visible notification changed. */
    fn post(&mut self, view: ToastView, update_only: bool) -> bool {
        if view.live {
            /* Progress of a running one, or a new one: always kept (an
             * UPDATE of a live one missed while asleep included). */
            match self.live.iter_mut().find(|l| l.id == view.id) {
                Some(running) => *running = view.clone(),
                None => self.live.push(view.clone()),
            }
            if self.current.as_ref().is_some_and(|c| c.id == view.id) {
                self.current = Some(view);
                self.deadline = None;
                return true;
            }
            if self.current.is_none() {
                self.show_next();
                return true;
            }
            return false;
        }
        let was_live = self.end_live(&view.id);
        if self.current.as_ref().is_some_and(|c| c.id == view.id) {
            self.deadline = Some(Instant::now() + view.timeout());
            self.current = Some(view);
            return true;
        }
        if let Some(queued) = self.pending.iter_mut().find(|q| q.id == view.id) {
            *queued = view;
            return false;
        }
        /* The completion of a live one waiting behind another card. */
        if update_only && !was_live {
            return false;
        }
        self.pending.push_back(view);
        while self.pending.len() > TOAST_QUEUE_LIMIT {
            self.pending.pop_front();
        }
        /* A running live card makes room and comes back afterwards. */
        if self.current.as_ref().map_or(true, |c| c.live) {
            self.show_next();
            return true;
        }
        false
    }

    /* The oldest running job the BACKGROUND TASK row can stop. */
    fn stoppable(&self) -> Option<&ToastView> {
        self.live.iter().find(|l| !l.job.is_empty())
    }

    /* Forgets a running Live Notification; true when it was one. */
    fn end_live(&mut self, id: &str) -> bool {
        let before = self.live.len();
        self.live.retain(|l| l.id != id);
        if self.stopping == id {
            self.stopping.clear();
        }
        self.live.len() != before
    }

    fn dismiss(&mut self, id: &str) -> bool {
        self.pending.retain(|q| q.id != id);
        self.end_live(id);
        if self.current.as_ref().is_some_and(|c| c.id == id) {
            self.show_next();
            return true;
        }
        false
    }

    /* A completion or dismissal that arrived while notifications are not
     * shown (lifecycle curtain, before a resume) still ends a live one. */
    fn finish_hidden(&mut self, id: &str) -> bool {
        if !self.end_live(id) {
            return false;
        }
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
        self.current = self.pending.pop_front().or_else(|| self.live.first().cloned());
        self.deadline = self
            .current
            .as_ref()
            .filter(|view| !view.live)
            .map(|view| Instant::now() + view.timeout());
    }

    /* Before Sleep/power off: timed cards are dropped, running jobs are kept
     * and come back with `resume`. */
    fn clear(&mut self) {
        self.current = None;
        self.pending.clear();
        self.deadline = None;
    }

    fn resume(&mut self) -> bool {
        self.resumed_at = Some(Instant::now());
        if self.current.is_none() && !self.live.is_empty() {
            self.show_next();
            return true;
        }
        false
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
/* ------------------------------------------------------------------ */
/* Themes (EPIC-008)                                                    */
/* ------------------------------------------------------------------ */

/* The active user's theme tokens (nuubos-themectl validates packages and
 * owns the selection). Built-in values for anything missing or invalid. */
const THEME_DEFAULTS: &[(&str, &str)] = &[
    ("accent", "#3a86ff"), ("accent-light", "#73a8ff"), ("accent-muted", "#5b86d6"),
    ("accent-text", "#cfe2ff"), ("focus-fill", "#18283c"), ("text", "#f5f6f8"),
    ("text-secondary", "#a8adb5"), ("text-dim", "#8e949d"), ("text-faint", "#7f858e"),
    ("text-disabled", "#555b64"), ("background", "#0d0e10"), ("surface", "#24272d"),
    ("border", "#30343b"), ("border-soft", "#2c3036"), ("warning", "#f2b84b"),
];

thread_local! {
    static THEME_TOKENS: RefCell<Vec<(String, slint::Color)>> = const { RefCell::new(Vec::new()) };
}

fn theme_color(v: &str) -> Option<slint::Color> {
    let n = u32::from_str_radix(v.trim().strip_prefix('#')?, 16).ok()?;
    (v.trim().len() == 7).then(|| slint::Color::from_rgb_u8((n >> 16) as u8, (n >> 8) as u8, n as u8))
}

fn read_theme_tokens() -> Vec<(String, slint::Color)> {
    let user = fs::read_to_string("/run/nuubos/user/active").unwrap_or_default().trim().to_owned();
    let path = std::process::Command::new("/usr/bin/nuubos-themectl")
        .args(["selected", &user])
        .output()
        .ok()
        .and_then(|o| {
            String::from_utf8_lossy(&o.stdout).lines().find_map(|l| l.strip_prefix("path=").map(str::to_owned))
        })
        .unwrap_or_default();
    let text = fs::read_to_string(format!("{path}/theme.conf")).unwrap_or_default();
    THEME_DEFAULTS
        .iter()
        .map(|(k, d)| {
            let v = text
                .lines()
                .find_map(|l| l.strip_prefix(*k).and_then(|r| r.strip_prefix('=')))
                .and_then(theme_color)
                .unwrap_or_else(|| theme_color(d).unwrap());
            ((*k).to_owned(), v)
        })
        .collect()
}

macro_rules! apply_theme {
    ($window:expr, $tokens:expr) => {{
        let t = $window.global::<Theme>();
        for (k, c) in $tokens.iter() {
            match k.as_str() {
                "accent" => t.set_accent(*c),
                "accent-light" => t.set_accent_light(*c),
                "accent-muted" => t.set_accent_muted(*c),
                "accent-text" => t.set_accent_text(*c),
                "focus-fill" => t.set_focus_fill(*c),
                "text" => t.set_text(*c),
                "text-secondary" => t.set_text_secondary(*c),
                "text-dim" => t.set_text_dim(*c),
                "text-faint" => t.set_text_faint(*c),
                "text-disabled" => t.set_text_disabled(*c),
                "background" => t.set_background(*c),
                "surface" => t.set_surface(*c),
                "border" => t.set_border(*c),
                "border-soft" => t.set_border_soft(*c),
                "warning" => t.set_warning(*c),
                _ => {}
            }
        }
    }};
}

/* Active user and the mtime of their settings.conf (which holds THEME=):
 * the tokens are read again only when one of them changed. */
fn theme_key() -> (String, Option<std::time::SystemTime>) {
    let user = fs::read_to_string("/run/nuubos/user/active").unwrap_or_default().trim().to_owned();
    let mtime = fs::metadata(format!("/state/users/{user}/settings.conf"))
        .and_then(|m| m.modified())
        .ok();
    (user, mtime)
}

thread_local! {
    static THEME_KEY: RefCell<Option<(String, Option<std::time::SystemTime>)>> = const { RefCell::new(None) };
}

/* Before any surface of this process appears (menu, Power OSD, volume OSD,
 * notification card): the user may have switched or changed Mood since,
 * and only Home is told (themectl's ui-control `mood`). The notification
 * card used to keep the Mood of the last menu open (user report
 * 2026-10-24). */
fn refresh_theme(ui: &QuickMenuWindow) {
    let key = theme_key();
    if THEME_KEY.with(|k| k.borrow().as_ref() == Some(&key)) {
        return;
    }
    let tokens = read_theme_tokens();
    apply_theme!(ui, tokens);
    THEME_TOKENS.with(|t| *t.borrow_mut() = tokens);
    THEME_KEY.with(|k| *k.borrow_mut() = Some(key));
}

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
        refresh_theme(ui);
        THEME_TOKENS.with(|t| {
            let t = t.borrow();
            if !t.is_empty() {
                apply_theme!(self.card, t);
            }
        });
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
    time: String,
}

fn read_battery_snapshot() -> BatterySnapshot {
    let mut snapshot = BatterySnapshot {
        percent: -1,
        state: "unknown".to_owned(),
        time: String::new(),
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
            "TIME" => snapshot.time = value.to_owned(),
            _ => {}
        }
    }

    snapshot
}

fn apply_battery_snapshot(ui: &QuickMenuWindow, snapshot: BatterySnapshot) {
    ui.set_battery_percent(snapshot.percent);
    ui.set_battery_state(snapshot.state.into());
    ui.set_clock_label(snapshot.time.into());
    ui.set_battery_label(
        if snapshot.percent >= 0 {
            format!("{}%", snapshot.percent).into()
        } else {
            "--%".into()
        },
    );
}

/* Product Audio pushes only volume changes made by the output device itself;
 * our own changes come back in the command reply. */
fn start_audio_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(AUDIO_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut reader = BufReader::new(stream);
                let mut line = String::new();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) if line.starts_with("volume origin=device ") => {
                            if tx.send(AppEvent::DeviceVolume).is_err() {
                                return;
                            }
                        }
                        Ok(_) => {}
                    }
                }
            }
        }
        /* Only reached while audiod is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
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

/* Running game as reported by nuubos-emud (EPIC-013). */
#[derive(Debug, Clone, Default)]
struct GameSnapshot {
    running: bool,
    /* Quit asked or RetroArch exiting: the closing screen covers it. */
    quitting: bool,
    game: String,
    paused: bool,
    slot: i32,
    slots: i32,
    /* Performance overlay (Settings -> Gaming) and the game's frame rate. */
    overlay: bool,
    overlay_items: String,
    fps: f32,
    /* GAME → RetroArch row (Settings → Gaming, hidden by default). */
    retroarch_menu: bool,
    /* RetroArch's own menu without a game runs (the RetroArch app): the
     * RETROARCH section instead of GAME; running stays false. */
    menu: bool,
    /* RetroAchievements hardcore: no save states (EPIC-017). */
    hardcore: bool,
    /* Cheats turned on; netplay (off|starting|host|client), players
     * connected, the other side's name. */
    cheats_on: i32,
    netplay: String,
    net_peers: i32,
    net_peer: String,
    /* melonDS-nuubOS: screen layout index (None for other cores) and
     * whether the stylus mode is on (Stylus / Screen Layout rows). */
    nds_layout: Option<i32>,
    nds_corner: i32,
    stylus: bool,
    /* Multi-disc game: the disc in use (0-based) and how many (0 = no
     * disc control; read by emud when the menu opens). */
    disc: i32,
    discs: i32,
}

fn parse_game_snapshot(reply: &str) -> GameSnapshot {
    let mut snapshot = GameSnapshot::default();
    for line in reply.lines() {
        match line.split_once('=') {
            Some(("state", value)) => snapshot.running = value == "running",
            Some(("quitting", value)) => snapshot.quitting = value == "1",
            Some(("game", value)) => snapshot.game = value.to_owned(),
            Some(("paused", value)) => snapshot.paused = value == "1",
            Some(("slot", value)) => snapshot.slot = value.parse().unwrap_or(0),
            Some(("slots", value)) => snapshot.slots = value.parse().unwrap_or(10),
            Some(("overlay", value)) => snapshot.overlay = value == "1",
            Some(("overlay_items", value)) => snapshot.overlay_items = value.to_owned(),
            Some(("fps", value)) => snapshot.fps = value.parse().unwrap_or(0.0),
            Some(("retroarch_menu", value)) => snapshot.retroarch_menu = value == "1",
            Some(("menu", value)) => snapshot.menu = value == "1",
            Some(("hardcore", value)) => snapshot.hardcore = value == "1",
            Some(("cheats_on", value)) => snapshot.cheats_on = value.parse().unwrap_or(0),
            Some(("netplay", value)) => snapshot.netplay = value.to_owned(),
            Some(("netplay_peers", value)) => snapshot.net_peers = value.parse().unwrap_or(0),
            Some(("netplay_peer", value)) => snapshot.net_peer = value.to_owned(),
            Some(("nds_layout", value)) => {
                snapshot.nds_layout = value.parse::<i32>().ok().filter(|v| (0..4).contains(v))
            }
            Some(("nds_corner", value)) => snapshot.nds_corner = value.parse::<i32>().ok().filter(|v| (0..4).contains(v)).unwrap_or(0),
            Some(("stylus", value)) => snapshot.stylus = value != "off",
            Some(("disc", value)) => snapshot.disc = value.parse().unwrap_or(0),
            Some(("discs", value)) => snapshot.discs = value.parse().unwrap_or(0),
            _ => {}
        }
    }
    snapshot.menu = snapshot.menu && snapshot.running;
    if snapshot.menu {
        snapshot.running = false;
    }
    snapshot
}

/* GAME → Stylus (42) and Screen Layout (43), melonDS-nuubOS only: the
 * same actions as the Stylus and Screen Layout hotkeys. The stylus moves
 * with the control inputd picks for the controller that opened the menu
 * (right stick, else left stick, else D-pad). */
const STYLUS_ROW: i32 = 42;
const LAYOUT_ROW: i32 = 43;
/* Small Screen Position, Picture in Picture only (GitHub #5). */
const CORNER_ROW: i32 = 52;
const NDS_PIP: i32 = 2;
/* GAME → Disc (51) of a multi-disc game (.m3u), where Stylus would be:
 * the disc dropdown; a choice goes back to the game, emud changes the
 * disc with the tray open for a moment (RC4 feature, EPIC-005/019). */
const DISC_ROW: i32 = 51;

fn toggle_stylus(ui: &QuickMenuWindow) {
    let control = input_command("STATUS")
        .ok()
        .and_then(|reply| {
            reply
                .lines()
                .find_map(|line| line.strip_prefix("stylus_control=").map(str::to_owned))
        })
        .filter(|c| matches!(c.as_str(), "right" | "left" | "dpad"))
        .unwrap_or_else(|| "right".to_owned());
    if let Err(error) = emulation_command(&format!("HOTKEY\tstylus {control}")) {
        eprintln!("quick-menu: game stylus failed={error}");
    }
    apply_game_snapshot(ui, &read_game_snapshot());
}

/* Picture in Picture corner (GitHub #5): emud saves it for the game and
 * applies it live. */
fn step_nds_corner(ui: &QuickMenuWindow, step: i32) {
    let corner = (ui.get_game_nds_corner() + step).rem_euclid(4);
    if let Err(error) = emulation_command(&format!("NDS_CORNER\t{corner}")) {
        eprintln!("quick-menu: game NDS_CORNER failed={error}");
    }
    apply_game_snapshot(ui, &read_game_snapshot());
}

fn open_layout_dropdown(ui: &QuickMenuWindow) {
    ui.set_game_layout_dropdown_index(ui.get_game_nds_layout());
    ui.set_game_layout_dropdown_open(true);
}

fn apply_layout_dropdown(ui: &QuickMenuWindow) {
    let layout = ui.get_game_layout_dropdown_index();
    if let Err(error) = emulation_command(&format!("NDS_LAYOUT\t{layout}")) {
        eprintln!("quick-menu: game NDS_LAYOUT failed={error}");
    }
    ui.set_game_layout_dropdown_open(false);
    apply_game_snapshot(ui, &read_game_snapshot());
}

/* State Slot dropdown: every slot with when it was saved, or Empty. */
fn open_slot_dropdown(ui: &QuickMenuWindow) {
    let reply = emulation_command("STATES").unwrap_or_default();
    let mut slots = 10;
    let mut saved: Vec<(i32, i64, String)> = Vec::new();
    for line in reply.lines() {
        if let Some(v) = line.strip_prefix("slots=") {
            slots = v.parse().unwrap_or(10);
        } else if let Some(v) = line.strip_prefix("state=") {
            let f: Vec<&str> = v.split('\t').collect();
            if let (Some(slot), Some(mtime)) = (f.first().and_then(|x| x.parse().ok()), f.get(1).and_then(|x| x.parse().ok())) {
                saved.push((slot, mtime, f.get(2).unwrap_or(&"").to_string()));
            }
        }
    }
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let options: Vec<SharedString> = (0..slots)
        .map(|n| {
            let name = format_arg(&tr(ui, 423, "Slot {0}"), &n.to_string());
            match saved.iter().find(|s| s.0 == n) {
                Some((_, mtime, hhmm)) => {
                    let days = (now - mtime).max(0) / 86400;
                    let when = match days {
                        0 => tr(ui, 402, "today"),
                        1 => tr(ui, 403, "yesterday"),
                        d => format_arg(&tr(ui, 404, "{0} days ago"), &d.to_string()),
                    };
                    format!("{name} • {when} {hhmm}").into()
                }
                None => format!("{name} • {}", tr(ui, 788, "Empty")).into(),
            }
        })
        .collect();
    let current = read_game_snapshot().slot.clamp(0, (slots - 1).max(0));
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    ui.set_game_slot_dropdown_disc(false);
    ui.set_game_slot_options(ModelRc::new(Rc::new(VecModel::from(options))));
    ui.set_game_slot_dropdown_index(current);
    ui.set_game_slot_dropdown_scroll((current - visible + 1).clamp(0, (slots - visible).max(0)));
    ui.set_game_slot_dropdown_open(true);
}

fn move_slot_dropdown(ui: &QuickMenuWindow, delta: i32) {
    let count = ui.get_game_slot_options().row_count() as i32;
    if count <= 0 {
        return;
    }
    let next = (ui.get_game_slot_dropdown_index() + delta).rem_euclid(count);
    ui.set_game_slot_dropdown_index(next);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let mut scroll = ui.get_game_slot_dropdown_scroll();
    if next < scroll {
        scroll = next;
    } else if next >= scroll + visible {
        scroll = next - visible + 1;
    }
    ui.set_game_slot_dropdown_scroll(scroll.clamp(0, (count - visible).max(0)));
}

fn apply_slot_dropdown(ui: &QuickMenuWindow) {
    let slot = ui.get_game_slot_dropdown_index();
    if let Err(error) = emulation_command(&format!("SLOT\t{slot}")) {
        eprintln!("quick-menu: game SLOT failed={error}");
    }
    ui.set_game_slot_dropdown_open(false);
    apply_game_snapshot(ui, &read_game_snapshot());
}

/* The State Slot dropdown listing the discs instead, anchored to Disc. */
fn open_disc_dropdown(ui: &QuickMenuWindow) {
    let game = read_game_snapshot();
    let discs = game.discs.max(1);
    let options: Vec<SharedString> = (1..=discs)
        .map(|n| format_arg(&tr(ui, 1400, "Disc {0}"), &n.to_string()).into())
        .collect();
    let current = game.disc.clamp(0, discs - 1);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    ui.set_game_slot_dropdown_disc(true);
    ui.set_game_slot_options(ModelRc::new(Rc::new(VecModel::from(options))));
    ui.set_game_slot_dropdown_index(current);
    ui.set_game_slot_dropdown_scroll((current - visible + 1).clamp(0, (discs - visible).max(0)));
    ui.set_game_slot_dropdown_open(true);
}

fn apply_disc_dropdown(ui: &QuickMenuWindow) {
    let disc = ui.get_game_slot_dropdown_index();
    if let Err(error) = emulation_command(&format!("DISC\t{disc}")) {
        eprintln!("quick-menu: game DISC failed={error}");
    }
    ui.set_game_slot_dropdown_open(false);
    ui.set_game_slot_dropdown_disc(false);
}

/* One line of the performance overlay from the game snapshot and
 * nuubos-systemd PERF, fields in the user's order. */
fn perf_text(ui: &QuickMenuWindow, game: &GameSnapshot) -> String {
    let perf = system_perf();
    let get = |k: &str| -> i64 {
        perf.lines()
            .find_map(|l| l.strip_prefix(k).and_then(|r| r.strip_prefix('=')))
            .and_then(|v| v.parse().ok())
            .unwrap_or(-1)
    };
    let mut parts: Vec<String> = Vec::new();
    for field in game.overlay_items.split(',') {
        let part = match field {
            "fps" => format!("{:.0} FPS", game.fps),
            "cpu" => {
                let load = get("cpu.load_pct");
                let khz = get("cpu.freq_khz");
                let mut t = String::from("CPU");
                if load >= 0 { t.push_str(&format!(" {load}%")); }
                if khz > 0 { t.push_str(&format!(" {:.1} GHz", khz as f64 / 1_000_000.0)); }
                t
            }
            "gpu" => match get("gpu.freq_hz") {
                hz if hz > 0 => format!("GPU {} MHz", hz / 1_000_000),
                _ => continue,
            },
            "ram" => match get("mem.used_mib") {
                mib if mib >= 0 => format!("RAM {mib} MB"),
                _ => continue,
            },
            "temp" => match get("temp.cpu_millic") {
                m if m > 0 => format!("{}°C", (m + 500) / 1000),
                _ => continue,
            },
            "power" => match get("battery.power_mw") {
                mw if mw >= 0 => format!("{:.1} W", mw as f64 / 1000.0),
                _ => continue,
            },
            "battery" => match get("battery.percent") {
                /* Labelled: a bare percentage reads as a load figure. */
                p if p >= 0 => format!("BAT {p}%"),
                _ => continue,
            },
            "clock" => ui.get_clock_label().to_string(),
            _ => continue,
        };
        if !part.is_empty() {
            parts.push(part);
        }
    }
    parts.join("   ")
}

fn system_perf() -> String {
    let Ok(mut stream) = UnixStream::connect(SYSTEM_SOCKET) else { return String::new() };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(300)));
    if stream.write_all(b"PERF\n").is_err() {
        return String::new();
    }
    let mut reader = BufReader::new(stream);
    let mut reply = String::new();
    loop {
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {
                let done = line.trim_end() == "end=1";
                reply.push_str(&line);
                if done {
                    break;
                }
            }
        }
    }
    reply
}

fn emulation_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(EMULATION_SOCKET)?;
    /* CHEATS may first extract the game's lists from the collection. */
    stream.set_read_timeout(Some(Duration::from_millis(if command == "CHEATS" { 5000 } else { 700 })))?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reader = BufReader::new(stream);
    let mut reply = String::new();
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line)? == 0 {
            break;
        }
        let done = (command != "STATUS" && command != "STATES" && command != "CHEATS")
            || line.trim_end() == "end=1"
            || line.starts_with("ERR");
        reply.push_str(&line);
        if done {
            break;
        }
    }
    Ok(reply)
}

/* Fire-and-forget game action: the outcome the user must see comes back as
 * a notification from the Emulation Service. */
fn game_action(command: &'static str) {
    if let Err(error) = emulation_command(command) {
        eprintln!("quick-menu: game {command} failed={error}");
    }
}

fn read_game_snapshot() -> GameSnapshot {
    emulation_command("STATUS")
        .map(|reply| parse_game_snapshot(&reply))
        .unwrap_or_default()
}

fn apply_game_snapshot(ui: &QuickMenuWindow, snapshot: &GameSnapshot) {
    let selected = ui.get_selected_index();
    if !snapshot.running && ((8..TASK_ROW).contains(&selected) || matches!(selected, STYLUS_ROW | LAYOUT_ROW | CORNER_ROW | DISC_ROW)) {
        ui.set_selected_index(3);
    }
    let nds = snapshot.running && snapshot.nds_layout.is_some();
    if !nds && matches!(ui.get_selected_index(), STYLUS_ROW | LAYOUT_ROW | CORNER_ROW) {
        ui.set_selected_index(8);
    }
    if nds && snapshot.nds_layout != Some(NDS_PIP) && ui.get_selected_index() == CORNER_ROW {
        ui.set_selected_index(LAYOUT_ROW);
    }
    if !nds {
        ui.set_game_layout_dropdown_open(false);
    }
    ui.set_game_nds(nds);
    let discs = if snapshot.running && !nds { snapshot.discs } else { 0 };
    if discs <= 1 && ui.get_selected_index() == DISC_ROW {
        ui.set_selected_index(8);
    }
    if discs <= 1 && ui.get_game_slot_dropdown_disc() {
        ui.set_game_slot_dropdown_open(false);
    }
    ui.set_game_discs(discs);
    ui.set_game_disc_label(
        format_arg2(&tr(ui, 1401, "{0} of {1}"), &(snapshot.disc + 1).to_string(), &discs.to_string()).into(),
    );
    ui.set_game_nds_layout(snapshot.nds_layout.unwrap_or(0));
    ui.set_game_nds_corner(snapshot.nds_corner);
    ui.set_game_stylus_on(nds && snapshot.stylus);
    if !snapshot.running {
        ui.set_game_confirm_index(-1);
    }
    ui.set_game_section_visible(snapshot.running);
    if !snapshot.menu && (RA_RESUME_ROW..=RA_QUIT_ROW).contains(&ui.get_selected_index()) {
        ui.set_selected_index(3);
    }
    ui.set_ra_section_visible(snapshot.menu);
    ui.set_game_hardcore(snapshot.running && snapshot.hardcore);
    if ui.get_game_hardcore() && (9..=11).contains(&ui.get_selected_index()) {
        ui.set_selected_index(8);
        ui.set_game_slot_dropdown_open(false);
    }
    ui.set_game_overlay_on(snapshot.overlay);
    ui.set_game_retroarch(snapshot.retroarch_menu);
    if !snapshot.retroarch_menu && ui.get_selected_index() == 13 {
        ui.set_selected_index(18);
    }
    ui.set_game_slot_label(
        format_arg(&tr(ui, 423, "Slot {0}"), &snapshot.slot.to_string()).into(),
    );
    ui.set_game_cheats_value(cheats_value(ui, snapshot).into());
    ui.set_game_net_value(net_value(ui, snapshot).into());
    if !snapshot.running {
        ui.set_game_sheet_open(false);
    } else if ui.get_game_sheet_open() && ui.get_game_sheet_row() == NET_ROW {
        /* The netplay state changed under the open sheet. */
        let index = ui.get_game_sheet_index();
        show_game_sheet(ui, NET_ROW, netplay_rows(ui, snapshot), index);
    }
}

/* ---------------------------------------------------------------- */
/* GAME → Cheats (40) and Netplay (41): nuubos-emud CHEATS, CHEAT_SET,  */
/* CHEAT_FILE, NETPLAY_HOST, NETPLAY_STOP. Neither in hardcore mode;    */
/* no cheats with netplay (the rows say why).                           */
/* ---------------------------------------------------------------- */

const CHEATS_ROW: i32 = 40;
const NET_ROW: i32 = 41;
/* Game sheet row kinds (game-sheet-kinds). */
const SHEET_ACTION: i32 = 0;
const SHEET_OFF: i32 = 1;
const SHEET_ON: i32 = 2;
const SHEET_INFO: i32 = 3;

#[derive(Default)]
struct GameSheet {
    /* Row keys: "info", "lists", "cheat:<k>", "file:<path>", "download",
     * "host", "stop". */
    keys: Vec<String>,
    /* Cheats: the list picker is open. */
    picking: bool,
}

thread_local! {
    static GAME_SHEET: RefCell<GameSheet> = RefCell::new(GameSheet::default());
}

fn cheats_value(ui: &QuickMenuWindow, game: &GameSnapshot) -> String {
    if game.hardcore {
        tr(ui, 910, "Not available in Hardcore Mode")
    } else if game.netplay != "off" && !game.netplay.is_empty() {
        tr(ui, 1245, "Not available during netplay")
    } else if game.cheats_on > 0 {
        format_arg(&tr(ui, 1244, "{0} on"), &game.cheats_on.to_string())
    } else {
        String::new()
    }
}

fn net_value(ui: &QuickMenuWindow, game: &GameSnapshot) -> String {
    match game.netplay.as_str() {
        _ if game.hardcore => tr(ui, 910, "Not available in Hardcore Mode"),
        "host" if game.net_peers == 0 => tr(ui, 1251, "Waiting for players…"),
        "host" => format_arg(&tr(ui, 1249, "Hosting • {0} connected"), &game.net_peers.to_string()),
        "client" => format_arg(&tr(ui, 1250, "Connected to {0}"), &game.net_peer),
        "starting" => tr(ui, 1273, "Starting netplay…"),
        _ => String::new(),
    }
}

type SheetRow = (String, String, String, i32);

fn info_row(title: String) -> SheetRow {
    ("info".into(), title, String::new(), SHEET_INFO)
}

fn netplay_rows(ui: &QuickMenuWindow, game: &GameSnapshot) -> Vec<SheetRow> {
    if game.hardcore {
        return vec![info_row(tr(ui, 910, "Not available in Hardcore Mode"))];
    }
    match game.netplay.as_str() {
        "host" | "starting" => vec![
            info_row(net_value(ui, game)),
            ("stop".into(), tr(ui, 1247, "Stop Hosting"), String::new(), SHEET_ACTION),
        ],
        "client" => vec![
            info_row(net_value(ui, game)),
            ("stop".into(), tr(ui, 1248, "Leave Game"), String::new(), SHEET_ACTION),
        ],
        _ => vec![
            info_row(tr(ui, 1271, "The game restarts from this point as a host")),
            ("host".into(), tr(ui, 1246, "Host Game"), String::new(), SHEET_ACTION),
        ],
    }
}

/* The running game's cheats (emud CHEATS), or the list picker. */
fn cheat_rows(ui: &QuickMenuWindow, picking: bool) -> Vec<SheetRow> {
    let reply = emulation_command("CHEATS").unwrap_or_default();
    let get = |k: &str| reply.lines().find_map(|l| l.strip_prefix(k).and_then(|r| r.strip_prefix('='))).unwrap_or("");
    match get("reason") {
        "hardcore" => return vec![info_row(tr(ui, 910, "Not available in Hardcore Mode"))],
        "netplay" => return vec![info_row(tr(ui, 1245, "Not available during netplay"))],
        _ => {}
    }
    let file = get("file").to_owned();
    let lists: Vec<(String, String)> = reply
        .lines()
        .filter_map(|l| l.strip_prefix("list="))
        .filter_map(|v| v.split_once('\t'))
        .map(|(p, n)| (p.to_owned(), n.to_owned()))
        .collect();
    if picking {
        return lists
            .into_iter()
            .map(|(path, name)| {
                let mark = if path == file { tr(ui, 222, "On") } else { String::new() };
                (format!("file:{path}"), name, mark, SHEET_ACTION)
            })
            .collect();
    }
    if lists.is_empty() {
        let mut rows = vec![info_row(tr(ui, 1240, "No cheats for this game"))];
        /* Without the collection, offer it (a job; its Live Notification
         * shows the download). */
        let status = Command::new("/usr/bin/nuubos-libretroctl")
            .args(["cheats", "status"])
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        if !status.lines().any(|l| l == "cheats=1") {
            rows.push(("download".into(), tr(ui, 1241, "Download Cheats"), String::new(), SHEET_ACTION));
        }
        return rows;
    }
    let current = lists.iter().find(|(p, _)| *p == file).map(|(_, n)| n.clone()).unwrap_or_default();
    let mut rows: Vec<SheetRow> = vec![(
        "lists".into(),
        tr(ui, 1243, "Cheat List"),
        current,
        if lists.len() > 1 { SHEET_ACTION } else { SHEET_INFO },
    )];
    let mut cheats: Vec<(i32, bool, String)> = reply
        .lines()
        .filter_map(|l| l.strip_prefix("cheat="))
        .filter_map(|v| {
            let f: Vec<&str> = v.splitn(3, '\t').collect();
            Some((f.first()?.parse().ok()?, *f.get(1)? == "1", f.get(2).unwrap_or(&"").to_string()))
        })
        .collect();
    cheats.sort_by_key(|c| c.0);
    for (k, on, desc) in cheats {
        let title = if desc.trim().is_empty() { format_arg(&tr(ui, 1252, "Cheat {0}"), &(k + 1).to_string()) } else { desc };
        rows.push((format!("cheat:{k}"), title, String::new(), if on { SHEET_ON } else { SHEET_OFF }));
    }
    rows
}

/* Shows rows in the sheet; the focus goes to index, else to the first
 * row that is not information. */
fn show_game_sheet(ui: &QuickMenuWindow, row: i32, rows: Vec<SheetRow>, index: i32) {
    let count = rows.len() as i32;
    let focusable = |i: i32| rows.get(i as usize).map(|r| r.3 != SHEET_INFO).unwrap_or(false);
    let index = if focusable(index) { index } else { (0..count).find(|i| focusable(*i)).unwrap_or(0) };
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let scroll = ui.get_game_sheet_scroll().clamp((index - visible + 1).max(0), index.max(0)).min((count - visible).max(0));
    GAME_SHEET.with(|g| g.borrow_mut().keys = rows.iter().map(|r| r.0.clone()).collect());
    ui.set_game_sheet_row(row);
    ui.set_game_sheet_title(if row == NET_ROW { tr(ui, 1239, "Netplay") } else { tr(ui, 1238, "Cheats") }.into());
    ui.set_game_sheet_titles(ModelRc::new(Rc::new(VecModel::from(rows.iter().map(|r| SharedString::from(r.1.as_str())).collect::<Vec<_>>()))));
    ui.set_game_sheet_values(ModelRc::new(Rc::new(VecModel::from(rows.iter().map(|r| SharedString::from(r.2.as_str())).collect::<Vec<_>>()))));
    ui.set_game_sheet_kinds(ModelRc::new(Rc::new(VecModel::from(rows.iter().map(|r| r.3).collect::<Vec<_>>()))));
    ui.set_game_sheet_index(index);
    ui.set_game_sheet_scroll(scroll);
    ui.set_game_sheet_open(count > 0);
}

fn open_game_sheet(ui: &QuickMenuWindow, row: i32) {
    GAME_SHEET.with(|g| g.borrow_mut().picking = false);
    ui.set_game_sheet_scroll(0);
    let rows = if row == NET_ROW { netplay_rows(ui, &read_game_snapshot()) } else { cheat_rows(ui, false) };
    show_game_sheet(ui, row, rows, -1);
}

fn move_game_sheet(ui: &QuickMenuWindow, delta: i32) {
    let kinds: Vec<i32> = ui.get_game_sheet_kinds().iter().collect();
    let count = kinds.len() as i32;
    if count == 0 {
        return;
    }
    let mut next = ui.get_game_sheet_index();
    for _ in 0..count {
        next = (next + delta).rem_euclid(count);
        if kinds[next as usize] != SHEET_INFO {
            break;
        }
    }
    if kinds[next as usize] == SHEET_INFO {
        return;
    }
    ui.set_game_sheet_index(next);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let mut scroll = ui.get_game_sheet_scroll();
    if next < scroll {
        scroll = next;
    } else if next >= scroll + visible {
        scroll = next - visible + 1;
    }
    ui.set_game_sheet_scroll(scroll.clamp(0, (count - visible).max(0)));
}

/* Confirm in the sheet; true when the menu must close (Host Game: the
 * game restarts). */
fn confirm_game_sheet(ui: &QuickMenuWindow) -> bool {
    let index = ui.get_game_sheet_index();
    let key = GAME_SHEET.with(|g| g.borrow().keys.get(index.max(0) as usize).cloned().unwrap_or_default());
    let kinds: Vec<i32> = ui.get_game_sheet_kinds().iter().collect();
    match key.as_str() {
        "host" => {
            game_action("NETPLAY_HOST");
            ui.set_game_sheet_open(false);
            return true;
        }
        "stop" => {
            game_action("NETPLAY_STOP");
            ui.set_game_sheet_open(false);
        }
        "download" => {
            let ok = Command::new("/usr/bin/nuubos-jobctl")
                .args(["submit", "cheats-update"])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .map(|s| s.success())
                .unwrap_or(false);
            append_action_log(&format!("cheats-update submitted={ok}"));
            ui.set_game_sheet_open(false);
        }
        "lists" => {
            GAME_SHEET.with(|g| g.borrow_mut().picking = true);
            ui.set_game_sheet_scroll(0);
            show_game_sheet(ui, CHEATS_ROW, cheat_rows(ui, true), -1);
        }
        k if k.starts_with("file:") => {
            if let Err(error) = emulation_command(&format!("CHEAT_FILE\t{}", &k[5..])) {
                eprintln!("quick-menu: cheat list failed={error}");
            }
            GAME_SHEET.with(|g| g.borrow_mut().picking = false);
            ui.set_game_sheet_scroll(0);
            show_game_sheet(ui, CHEATS_ROW, cheat_rows(ui, false), -1);
        }
        k if k.starts_with("cheat:") => {
            let on = kinds.get(index as usize) == Some(&SHEET_ON);
            if let Err(error) = emulation_command(&format!("CHEAT_SET\t{}\t{}", &k[6..], if on { 0 } else { 1 })) {
                eprintln!("quick-menu: cheat failed={error}");
            }
            show_game_sheet(ui, CHEATS_ROW, cheat_rows(ui, false), index);
            apply_game_snapshot(ui, &read_game_snapshot());
        }
        _ => {}
    }
    false
}

fn back_game_sheet(ui: &QuickMenuWindow) {
    let picking = GAME_SHEET.with(|g| std::mem::take(&mut g.borrow_mut().picking));
    if picking && ui.get_game_sheet_row() == CHEATS_ROW {
        ui.set_game_sheet_scroll(0);
        show_game_sheet(ui, CHEATS_ROW, cheat_rows(ui, false), 0);
    } else {
        ui.set_game_sheet_open(false);
    }
}

fn start_game_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(EMULATION_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut reader = BufReader::new(stream);
                let mut block = String::new();
                let mut line = String::new();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            block.push_str(&line);
                            if line.trim_end() == "end=1" {
                                let snapshot = parse_game_snapshot(&block);
                                block.clear();
                                if tx.send(AppEvent::Game(snapshot)).is_err() {
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
        /* Only reached while emud is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
}

/* Game closing screen (user request 2026-10-15): Home's loading screen
 * again while the game saves and exits. Its title, system, colour and cover
 * come from nuubos-libraryd as soon as the game runs, so the screen is ready
 * the moment the quit starts. */
#[derive(Debug, Clone)]
struct ClosingInfo {
    game: String,
    title: String,
    system: String,
    accent: u32,
    cover: Option<SharedPixelBuffer<Rgba8Pixel>>,
}

fn library_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(LIBRARY_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reply = String::new();
    for line in BufReader::new(stream).lines() {
        let line = line?;
        let done = line == "end=1" || line.starts_with("ERR");
        reply.push_str(&line);
        reply.push('\n');
        if done {
            break;
        }
    }
    Ok(reply)
}

fn fetch_closing_info(tx: Sender<AppEvent>, game: String) {
    thread::spawn(move || {
        let mut info = ClosingInfo {
            game: game.clone(),
            title: String::new(),
            system: String::new(),
            accent: 0x2a3140,
            cover: None,
        };
        let mut system_id = String::new();
        let mut cover_path = String::new();
        for line in library_command(&format!("DETAILS\t{game}")).unwrap_or_default().lines() {
            if let Some(v) = line.strip_prefix("game=") {
                let f: Vec<&str> = v.split('\t').collect();
                system_id = f.get(1).unwrap_or(&"").to_string();
                info.title = f.get(2).unwrap_or(&"").to_string();
                cover_path = f.get(3).unwrap_or(&"").to_string();
            } else if let Some(v) = line.strip_prefix("system_name=") {
                info.system = v.to_owned();
            }
        }
        if !system_id.is_empty() {
            let status = library_command("STATUS").unwrap_or_default();
            let color = status.lines().filter_map(|l| l.strip_prefix("system=")).find_map(|v| {
                let f: Vec<&str> = v.split('\t').collect();
                (f.first() == Some(&system_id.as_str())).then(|| f.get(4).copied()).flatten()
            });
            if let Some(v) = color.and_then(|c| u32::from_str_radix(c.trim_start_matches('#'), 16).ok()) {
                info.accent = v;
            }
        }
        if !cover_path.is_empty() {
            info.cover = slint::Image::load_from_path(std::path::Path::new(&cover_path))
                .ok()
                .and_then(|image| image.to_rgba8())
                .map(round_cover_corners);
        }
        let _ = tx.send(AppEvent::GameInfo(info));
    });
}

/* Rounded corners like Home's covers (radius 6 % of the height, as its
 * cover frame): an image is not clipped by a rounded parent. */
fn round_cover_corners(mut buffer: SharedPixelBuffer<Rgba8Pixel>) -> SharedPixelBuffer<Rgba8Pixel> {
    let (w, h) = (buffer.width() as f32, buffer.height() as f32);
    let r = (h * 0.06).min(w / 2.0);
    let width = buffer.width() as usize;
    for (i, px) in buffer.make_mut_slice().iter_mut().enumerate() {
        let (x, y) = ((i % width) as f32 + 0.5, (i / width) as f32 + 0.5);
        let cx = x.clamp(r, w - r);
        let cy = y.clamp(r, h - r);
        let d = ((x - cx).powi(2) + (y - cy).powi(2)).sqrt();
        let coverage = (r - d + 0.5).clamp(0.0, 1.0);
        if coverage < 1.0 {
            px.a = (px.a as f32 * coverage + 0.5) as u8;
        }
    }
    buffer
}

fn apply_closing_info(ui: &QuickMenuWindow, info: Option<&ClosingInfo>) {
    let accent = info.map(|i| i.accent).unwrap_or(0x2a3140);
    ui.set_closing_title(info.map(|i| i.title.as_str()).unwrap_or("").into());
    ui.set_closing_system(info.map(|i| i.system.as_str()).unwrap_or("").into());
    ui.set_closing_accent(slint::Color::from_rgb_u8((accent >> 16) as u8, (accent >> 8) as u8, accent as u8));
    let cover = info.and_then(|i| i.cover.clone());
    ui.set_closing_has_cover(cover.is_some());
    ui.set_closing_cover(cover.map(slint::Image::from_rgba8).unwrap_or_default());
}

/* PC stream (EPIC-025). The stream runs while the menu is open: nothing
 * pauses a PC game, so Resume only closes the menu. */
fn stream_command(command: &str, timeout: Duration) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(STREAM_SOCKET)?;
    stream.set_read_timeout(Some(timeout))?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply)?;
    Ok(reply)
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
struct StreamState {
    /* a stream or Steam Link session runs */
    running: bool,
    steamlink: bool,
    /* the user's performance overlay (Moonlight; Steam Link: Valve's
     * statistics, for the session) */
    overlay: bool,
}

fn stream_state_from(reply: &str) -> StreamState {
    let running = reply.lines().any(|line| matches!(line, "state=streaming" | "state=starting"));
    StreamState {
        running,
        steamlink: running && reply.lines().any(|line| line == "stream_client=steamlink"),
        overlay: reply.lines().any(|line| line == "overlay=1"),
    }
}

fn read_stream_state() -> StreamState {
    let Ok(mut stream) = UnixStream::connect(STREAM_SOCKET) else { return StreamState::default() };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(700)));
    if stream.write_all(b"STATUS\n").is_err() {
        return StreamState::default();
    }
    let mut reply = String::new();
    for line in BufReader::new(stream).lines() {
        let Ok(line) = line else { break };
        reply.push_str(&line);
        reply.push('\n');
        if line == "end=1" {
            break;
        }
    }
    stream_state_from(&reply)
}

fn apply_stream_running(ui: &QuickMenuWindow, stream: StreamState) {
    let (running, steamlink) = (stream.running, stream.steamlink);
    let index = ui.get_selected_index();
    let hidden = if running {
        steamlink && (17..TASK_ROW).contains(&index)
    } else {
        (15..TASK_ROW).contains(&index) || (36..=38).contains(&index)
    };
    if hidden {
        ui.set_selected_index(if running { 15 } else { 3 });
        ui.set_game_confirm_index(-1);
    }
    ui.set_stream_steamlink(steamlink);
    ui.set_stream_section_visible(running);
    ui.set_stream_overlay_on(stream.overlay);
}

/* One sample of the stream statistics when the menu opens (they only
 * exist while the stream runs; nothing is polled). */
fn refresh_stream_stats(ui: &QuickMenuWindow) {
    let reply = stream_command("STATS", Duration::from_millis(400)).unwrap_or_default();
    let Some(fields) = reply.trim().strip_prefix("OK ") else {
        ui.set_stream_latency_label("".into());
        ui.set_stream_decode_label("".into());
        ui.set_stream_video_label("".into());
        return;
    };
    let value = |key: &str| -> i64 {
        fields
            .split(' ')
            .find_map(|kv| kv.strip_prefix(key).and_then(|v| v.strip_prefix('=')))
            .and_then(|v| v.parse().ok())
            .unwrap_or(-1)
    };
    let rtt = value("rtt");
    ui.set_stream_latency_label(if rtt >= 0 { format!("{rtt} ms").into() } else { "".into() });
    let decoded = value("decoded");
    let decode_us = value("decode_us");
    ui.set_stream_decode_label(if decoded > 0 && decode_us >= 0 {
        format!("{:.1} ms", decode_us as f64 / decoded as f64 / 1000.0).into()
    } else {
        "".into()
    });
    let (width, height, fps) = (value("width"), value("height"), value("fps"));
    ui.set_stream_video_label(if width > 0 && height > 0 {
        format!("{width}×{height} • {}", format_arg(&tr(ui, 460, "{0} fps"), &fps.to_string())).into()
    } else {
        "".into()
    });
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
struct PortState {
    running: bool,
    /* PortMaster's own interface, not a port */
    portmaster: bool,
}

fn port_state_from(reply: &str) -> PortState {
    let running = reply.lines().any(|line| line == "state=running");
    PortState { running, portmaster: running && reply.lines().any(|line| line == "session=portmaster") }
}

fn port_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(PORTS_SOCKET)?;
    let _ = stream.set_read_timeout(Some(Duration::from_millis(700)));
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;
    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply)?;
    Ok(reply)
}

fn read_port_state() -> PortState {
    let Ok(mut stream) = UnixStream::connect(PORTS_SOCKET) else { return PortState::default() };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(700)));
    if stream.write_all(b"STATUS\n").is_err() {
        return PortState::default();
    }
    let mut reply = String::new();
    for line in BufReader::new(stream).lines() {
        let Ok(line) = line else { break };
        reply.push_str(&line);
        reply.push('\n');
        if line == "end=1" {
            break;
        }
    }
    port_state_from(&reply)
}

fn apply_port_running(ui: &QuickMenuWindow, port: PortState) {
    if !port.running && (PORT_RESUME_ROW..=PORT_QUIT_ROW).contains(&ui.get_selected_index()) {
        ui.set_selected_index(3);
    }
    ui.set_port_portmaster(port.portmaster);
    ui.set_port_section_visible(port.running);
}

fn start_port_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(PORTS_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut block = String::new();
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    block.push_str(&line);
                    block.push('\n');
                    if line == "end=1" {
                        let state = port_state_from(&block);
                        block.clear();
                        if tx.send(AppEvent::Port(state)).is_err() {
                            return;
                        }
                    }
                }
            }
        }
        /* Only reached while portsd is absent or restarting. */
        thread::sleep(Duration::from_secs(2));
    });
}

fn start_stream_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(STREAM_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut reader = BufReader::new(stream);
                let mut block = String::new();
                let mut line = String::new();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            block.push_str(&line);
                            if line.trim_end() == "end=1" {
                                let state = stream_state_from(&block);
                                block.clear();
                                if tx.send(AppEvent::Stream(state)).is_err() {
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
        /* Only reached while streamd is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
}


/* Statistics samples for the performance overlay: streamd pushes one a
 * second only while a stream runs with the overlay on. */
fn start_stream_stats_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(STREAM_SOCKET) {
            if stream.write_all(b"SUBSCRIBE_STATS\n").is_ok() {
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    if let Some(fields) = line.strip_prefix("stats ") {
                        if tx.send(AppEvent::StreamStats(fields.to_owned())).is_err() {
                            return;
                        }
                    }
                }
            }
        }
        /* Only reached while streamd is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
}

/* Cumulative counters of one moonlight @stats sample. */
#[derive(Clone, Copy, Default)]
struct StreamSample {
    rtt: i64,
    rtt_var: i64,
    width: i64,
    height: i64,
    decoded: i64,
    decode_us: i64,
    bytes: i64,
    host_latency: i64,
    host_frames: i64,
    lost: i64,
}

impl StreamSample {
    fn parse(fields: &str) -> Self {
        let value = |key: &str| -> i64 {
            fields
                .split(' ')
                .find_map(|kv| kv.strip_prefix(key).and_then(|v| v.strip_prefix('=')))
                .and_then(|v| v.parse().ok())
                .unwrap_or(-1)
        };
        StreamSample {
            rtt: value("rtt"),
            rtt_var: value("rtt_var"),
            width: value("width"),
            height: value("height"),
            decoded: value("decoded"),
            decode_us: value("decode_us"),
            bytes: value("bytes"),
            host_latency: value("host_latency"),
            host_frames: value("host_frames"),
            lost: value("lost"),
        }
    }
}

/*
 * Performance overlay over a Moonlight stream: its own passive surface
 * (StreamHudWindow), like the notification. Rates and averages are taken
 * over the second between two samples: clock and battery, video size,
 * received frame rate and bitrate | network round trip, decode time, host
 * processing time, frames lost on the network.
 */
struct StreamHud {
    card: StreamHudWindow,
    state: WaylandState,
    queue: EventQueue<WaylandState>,
    qh: QueueHandle<WaylandState>,
    mapped: bool,
    last: Option<(Instant, StreamSample)>,
}

impl StreamHud {
    fn sample(&mut self, ui: &QuickMenuWindow, fields: &str) {
        let now = Instant::now();
        let cur = StreamSample::parse(fields);
        /* A new stream starts its counters again. */
        let prev = self.last.filter(|(_, p)| p.decoded >= 0 && p.decoded <= cur.decoded).map(|(t, p)| (now.duration_since(t).as_secs_f64(), p));
        self.last = Some((now, cur));

        let mut one: Vec<String> = Vec::new();
        let clock = ui.get_clock_label().to_string();
        if !clock.is_empty() {
            one.push(clock);
        }
        if ui.get_battery_percent() >= 0 {
            one.push(format!("BAT {}%", ui.get_battery_percent()));
        }
        if cur.width > 0 && cur.height > 0 {
            one.push(format!("{}×{}", cur.width, cur.height));
        }
        let mut two: Vec<String> = Vec::new();
        if cur.rtt >= 0 {
            two.push(if cur.rtt_var > 0 {
                format!("{} {} ms ±{}", tr(ui, 1190, "Net"), cur.rtt, cur.rtt_var)
            } else {
                format!("{} {} ms", tr(ui, 1190, "Net"), cur.rtt)
            });
        }
        /* Over the last second; the first sample of a stream uses the
         * totals so far. */
        let (frames, decode_us, host, host_frames, lost) = match prev {
            Some((_, p)) => (
                cur.decoded - p.decoded,
                cur.decode_us - p.decode_us,
                cur.host_latency - p.host_latency,
                cur.host_frames - p.host_frames,
                cur.lost - p.lost,
            ),
            None => (cur.decoded, cur.decode_us, cur.host_latency, cur.host_frames, cur.lost),
        };
        if let Some((seconds, p)) = prev.filter(|(seconds, _)| *seconds > 0.2) {
            one.push(format_arg(&tr(ui, 460, "{0} fps"), &format!("{:.0}", frames as f64 / seconds)));
            if cur.bytes >= 0 && p.bytes >= 0 {
                let mbps = (cur.bytes - p.bytes) as f64 * 8.0 / seconds / 1_000_000.0;
                one.push(format_arg(&tr(ui, 461, "{0} Mbps"), &format!("{mbps:.1}")));
            }
        }
        if frames > 0 && decode_us >= 0 {
            two.push(format!("{} {:.1} ms", tr(ui, 1191, "Decode"), decode_us as f64 / frames as f64 / 1000.0));
        }
        if host_frames > 0 && host >= 0 {
            two.push(format!("{} {:.1} ms", tr(ui, 1192, "Host"), host as f64 / host_frames as f64 / 10.0));
        }
        if lost >= 0 && frames + lost > 0 {
            two.push(format!("{} {:.1}%", tr(ui, 1193, "Lost"), lost as f64 * 100.0 / (frames + lost) as f64));
        }
        self.card.set_line_one(one.join("   ").into());
        self.card.set_line_two(two.join("   ").into());
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

    fn unmap(&mut self, conn: &Connection) {
        if self.state.commit_unmap() {
            let _ = conn.flush();
            let _ = self.queue.roundtrip(&mut self.state);
        }
        self.state.destroy_overlay();
        let _ = self.card.hide();
        self.mapped = false;
        eprintln!("quick-menu: unmapped stream overlay surface");
    }

    /* Shown over a running Moonlight stream with the overlay on, never
     * over the menu or the lifecycle curtain; redrawn when `changed`. */
    fn sync(&mut self, conn: &Connection, want: bool, changed: bool) {
        let want = want && !self.card.get_line_one().is_empty();
        if !want {
            if self.mapped {
                self.unmap(conn);
            }
            return;
        }
        let result = if !self.mapped {
            (|| -> Result<(), Box<dyn std::error::Error>> {
                self.state.create_overlay(&self.qh, SurfaceKind::StreamHud)?;
                conn.flush()?;
                let _ = wait_for_configure(&mut self.queue, &mut self.state)?;
                self.card.show()?;
                self.redraw(conn)?;
                eprintln!("quick-menu: mapped stream overlay surface capture=0");
                Ok(())
            })()
        } else if changed {
            self.redraw(conn)
        } else {
            Ok(())
        };
        match result {
            Ok(()) => self.mapped = true,
            Err(error) => {
                eprintln!("quick-menu: stream overlay failed={error}");
                self.unmap(conn);
            }
        }
    }
}

/* Web Mode (EPIC-030). The page stays live under the menu. */
fn web_command(command: &str) {
    let command = command.to_owned();
    thread::spawn(move || {
        let result = (|| -> std::io::Result<String> {
            let mut stream = UnixStream::connect(WEB_SOCKET)?;
            stream.set_read_timeout(Some(Duration::from_secs(3)))?;
            stream.write_all(command.as_bytes())?;
            stream.write_all(b"\n")?;
            let mut reply = String::new();
            BufReader::new(stream).read_line(&mut reply)?;
            Ok(reply)
        })();
        if let Err(error) = result {
            eprintln!("quick-menu: web {command} failed={error}");
        }
    });
}

/* (browser running, zoom percent) */
fn web_state_from(reply: &str) -> (bool, i32) {
    let running = reply.lines().any(|line| line == "state=running");
    let zoom = reply
        .lines()
        .find_map(|line| line.strip_prefix("zoom="))
        .and_then(|v| v.parse().ok())
        .unwrap_or(100);
    (running, zoom)
}

fn read_web_state() -> (bool, i32) {
    let Ok(mut stream) = UnixStream::connect(WEB_SOCKET) else { return (false, 100) };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(700)));
    if stream.write_all(b"STATUS\n").is_err() {
        return (false, 100);
    }
    let mut reply = String::new();
    for line in BufReader::new(stream).lines() {
        let Ok(line) = line else { break };
        reply.push_str(&line);
        reply.push('\n');
        if line == "end=1" {
            break;
        }
    }
    web_state_from(&reply)
}

fn apply_web_state(ui: &QuickMenuWindow, running: bool, zoom: i32) {
    if !running && (20..=27).contains(&ui.get_selected_index()) {
        ui.set_selected_index(3);
    }
    ui.set_web_section_visible(running);
    ui.set_web_zoom_label(format!("{zoom}%").into());
}

/* Terminal (nuubos-termd). The shell stays live under the menu. */
fn term_command(command: &str) {
    let command = command.to_owned();
    thread::spawn(move || {
        let result = (|| -> std::io::Result<String> {
            let mut stream = UnixStream::connect(TERM_SOCKET)?;
            stream.set_read_timeout(Some(Duration::from_secs(3)))?;
            stream.write_all(command.as_bytes())?;
            stream.write_all(b"\n")?;
            let mut reply = String::new();
            BufReader::new(stream).read_line(&mut reply)?;
            Ok(reply)
        })();
        if let Err(error) = result {
            eprintln!("quick-menu: terminal {command} failed={error}");
        }
    });
}

/* (terminal running, on-screen keyboard on) */
fn term_state_from(reply: &str) -> (bool, bool) {
    (reply.lines().any(|line| line == "state=running"), reply.lines().any(|line| line == "keyboard=1"))
}

fn apply_term_state(ui: &QuickMenuWindow, running: bool, keyboard: bool) {
    if !running && (TERM_RESUME_ROW..=TERM_QUIT_ROW).contains(&ui.get_selected_index()) {
        ui.set_selected_index(3);
    }
    ui.set_term_section_visible(running);
    ui.set_term_keyboard_on(running && keyboard);
}

fn start_term_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(TERM_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut block = String::new();
                for line in BufReader::new(stream).lines() {
                    let Ok(line) = line else { break };
                    block.push_str(&line);
                    block.push('\n');
                    if line == "end=1" {
                        let state = term_state_from(&block);
                        block.clear();
                        if tx.send(AppEvent::Terminal(state.0, state.1)).is_err() {
                            return;
                        }
                    }
                }
            }
        }
        let _ = tx.send(AppEvent::Terminal(false, false));
        /* Only reached while termd is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
}

/* MUSIC (user request 2026-10-12): the Media app's music, with its own
 * controls and, over a game, the mixer (Music volume in nuubos-mediad,
 * Game volume = Applications in nuubos-audiod). */
fn media_command(command: String) {
    thread::spawn(move || {
        let Ok(mut stream) = UnixStream::connect(MEDIA_SOCKET) else { return };
        let _ = stream.set_read_timeout(Some(Duration::from_millis(700)));
        let _ = stream.write_all(command.as_bytes());
        let _ = stream.write_all(b"\n");
        let mut reply = String::new();
        let _ = BufReader::new(stream).read_line(&mut reply);
    });
}

fn media_state_from(block: &str) -> MediaState {
    let get = |k: &str| block.lines().find_map(|l| l.strip_prefix(k).and_then(|r| r.strip_prefix('='))).unwrap_or("");
    let state = get("state");
    MediaState {
        music: (state == "playing" || state == "paused") && get("kind") == "audio",
        playing: state == "playing",
        title: get("title").to_owned(),
        artist: get("artist").to_owned(),
        volume: get("volume").parse().unwrap_or(80),
    }
}

fn start_media_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(MEDIA_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut reader = BufReader::new(stream);
                let mut block = String::new();
                let mut line = String::new();
                let mut last = MediaState::default();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            block.push_str(&line);
                            if line.trim_end() == "end=1" {
                                let state = media_state_from(&block);
                                block.clear();
                                /* Position updates publish too: forward
                                 * only what the menu shows. */
                                if state != last {
                                    last = state.clone();
                                    if tx.send(AppEvent::Media(state)).is_err() {
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        let _ = tx.send(AppEvent::Media(MediaState::default()));
        /* Only reached while mediad is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
}

fn apply_media_state(ui: &QuickMenuWindow, state: &MediaState) {
    if !state.music && (31..=35).contains(&ui.get_selected_index()) {
        ui.set_selected_index(first_selectable_index_without_music(ui));
    }
    ui.set_music_section_visible(state.music);
    ui.set_music_playing(state.playing);
    ui.set_music_title(if state.artist.is_empty() { state.title.clone() } else { format!("{} • {}", state.title, state.artist) }.into());
    ui.set_music_volume(state.volume.clamp(0, 100));
}

/* Cast Screen (EPIC-029): nuubos-castd owns discovery and the session; the
 * menu shows its snapshot (SUBSCRIBE, no polling) and sends START/STOP.
 * Over a game or a PC stream only low-delay receivers are offered (product
 * decision: games get mirroring only where the latency is acceptable). */
#[derive(Debug, Clone, Default, PartialEq)]
struct CastReceiver {
    id: String,
    name: String,
    protocol: String,
    low_delay: bool,
    ready: bool,
}

#[derive(Debug, Clone, Default, PartialEq)]
struct CastState {
    available: bool,
    state: String,
    receiver_name: String,
    waiting_tv: bool,
    discovering: bool,
    receivers: Vec<CastReceiver>,
}

#[derive(Clone, PartialEq)]
enum CastEntry {
    Stop,
    Receiver(String),
    Placeholder,
}

thread_local! {
    static CAST: RefCell<(CastState, Vec<CastEntry>)> = RefCell::new((CastState::default(), Vec::new()));
}

fn cast_state_from(block: &str) -> CastState {
    let mut s = CastState { available: true, ..CastState::default() };
    let mut user = false;
    for line in block.lines() {
        let Some((k, v)) = line.split_once('=') else { continue };
        match k {
            "state" => s.state = v.to_owned(),
            "receiver_name" => s.receiver_name = v.to_owned(),
            "waiting_tv" => s.waiting_tv = v == "1",
            "discovering" => s.discovering = v == "1",
            "user" => user = v == "1",
            "rx" => {
                let f: Vec<&str> = v.split('\t').collect();
                if f.len() >= 6 {
                    s.receivers.push(CastReceiver {
                        id: f[0].to_owned(),
                        name: f[1].to_owned(),
                        protocol: f[2].to_owned(),
                        low_delay: f[3] == "low",
                        ready: f[5] == "1",
                    });
                }
            }
            _ => {}
        }
    }
    s.available = user;
    s
}

fn cast_command(command: String) {
    thread::spawn(move || {
        let reply = cast_request(&command);
        if !reply.starts_with("OK") {
            eprintln!("quick-menu: cast {} -> {}", command.replace('\t', " "), reply.trim());
        }
    });
}

fn start_cast_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(CAST_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut reader = BufReader::new(stream);
                let mut block = String::new();
                let mut line = String::new();
                let mut last = CastState::default();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            block.push_str(&line);
                            if line.trim_end() == "end=1" {
                                let state = cast_state_from(&block);
                                block.clear();
                                if state != last {
                                    last = state.clone();
                                    if tx.send(AppEvent::Cast(state)).is_err() {
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        let _ = tx.send(AppEvent::Cast(CastState::default()));
        /* Only reached while castd is absent or restarting. */
        thread::sleep(Duration::from_secs(2));
    });
}

fn cast_protocol_label(ui: &QuickMenuWindow, protocol: &str) -> String {
    match protocol {
        "cast" => tr(ui, 1198, "Google Cast"),
        "mice" => tr(ui, 1199, "Miracast"),
        _ => tr(ui, 1200, "DLNA • with delay"),
    }
}

fn cast_options(ui: &QuickMenuWindow, state: &CastState) -> Vec<(CastEntry, String)> {
    let playing = ui.get_game_section_visible() || ui.get_stream_section_visible() || ui.get_port_section_visible()
        || ui.get_ra_section_visible();
    let mut options = Vec::new();
    if state.state != "idle" && !state.state.is_empty() {
        options.push((CastEntry::Stop, tr(ui, 1195, "Stop Casting")));
    }
    for rx in &state.receivers {
        /* Miracast without Cisco's encoder: Settings offers the download. */
        if !rx.ready || (playing && !rx.low_delay) {
            continue;
        }
        options.push((CastEntry::Receiver(rx.id.clone()), format!("{} • {}", rx.name, cast_protocol_label(ui, &rx.protocol))));
    }
    if options.iter().all(|(e, _)| *e == CastEntry::Stop) {
        let text = if state.discovering {
            tr(ui, 1196, "Searching for TVs and receivers…")
        } else {
            tr(ui, 1197, "No receivers found")
        };
        options.push((CastEntry::Placeholder, text));
    }
    options
}

fn show_cast_options(ui: &QuickMenuWindow) {
    let options = CAST.with(|c| cast_options(ui, &c.borrow().0));
    let labels: Vec<SharedString> = options.iter().map(|(_, l)| l.as_str().into()).collect();
    let count = labels.len() as i32;
    let index = ui.get_cast_dropdown_index().clamp(0, (count - 1).max(0));
    ui.set_cast_options(ModelRc::new(Rc::new(VecModel::from(labels))));
    ui.set_cast_dropdown_index(index);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    ui.set_cast_dropdown_scroll(ui.get_cast_dropdown_scroll().clamp((index - visible + 1).max(0), index.min((count - visible).max(0))));
    CAST.with(|c| c.borrow_mut().1 = options.into_iter().map(|(e, _)| e).collect());
}

fn apply_cast_state(ui: &QuickMenuWindow, state: CastState) {
    let label = match state.state.as_str() {
        "connecting" if state.waiting_tv => tr(ui, 1201, "Allow the cast on the TV"),
        "connecting" => format_arg(&tr(ui, 254, "Connecting to {0}…"), &state.receiver_name),
        "casting" | "stopping" => state.receiver_name.clone(),
        _ => tr(ui, 54, "Off"),
    };
    if !state.available && ui.get_selected_index() == CAST_ROW {
        ui.set_selected_index(3);
    }
    ui.set_cast_row_visible(state.available);
    ui.set_cast_active(state.state == "casting");
    ui.set_cast_label(label.into());
    if !state.available {
        ui.set_cast_dropdown_open(false);
    }
    CAST.with(|c| c.borrow_mut().0 = state);
    if ui.get_cast_dropdown_open() {
        show_cast_options(ui);
    }
}

/* Opening the list looks for receivers again: TVs come and go. */
fn open_cast_dropdown(ui: &QuickMenuWindow) {
    cast_command("DISCOVER".into());
    ui.set_cast_dropdown_index(0);
    ui.set_cast_dropdown_scroll(0);
    show_cast_options(ui);
    ui.set_cast_dropdown_open(true);
}

fn move_cast_dropdown(ui: &QuickMenuWindow, delta: i32) {
    let count = ui.get_cast_options().row_count() as i32;
    if count <= 0 {
        return;
    }
    let next = (ui.get_cast_dropdown_index() + delta).rem_euclid(count);
    ui.set_cast_dropdown_index(next);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let mut scroll = ui.get_cast_dropdown_scroll();
    if next < scroll {
        scroll = next;
    } else if next >= scroll + visible {
        scroll = next - visible + 1;
    }
    ui.set_cast_dropdown_scroll(scroll.clamp(0, (count - visible).max(0)));
}

fn confirm_cast_dropdown(ui: &QuickMenuWindow) {
    let index = ui.get_cast_dropdown_index().max(0) as usize;
    let entry = CAST.with(|c| c.borrow().1.get(index).cloned());
    match entry {
        Some(CastEntry::Stop) => cast_command("STOP".into()),
        /* A cast already running is replaced. */
        Some(CastEntry::Receiver(id)) => {
            let busy = CAST.with(|c| {
                let s = &c.borrow().0;
                !s.state.is_empty() && s.state != "idle"
            });
            thread::spawn(move || {
                if busy {
                    let _ = cast_request("STOP");
                    for _ in 0..50 {
                        if cast_request("STATUS").lines().any(|l| l == "state=idle") {
                            break;
                        }
                        thread::sleep(Duration::from_millis(100));
                    }
                }
                let reply = cast_request(&format!("START\t{id}"));
                if !reply.starts_with("OK") {
                    eprintln!("quick-menu: cast start -> {}", reply.trim());
                }
            });
        }
        _ => return,
    }
    /* The menu stays open: the row shows the connection, then the TV. */
    ui.set_cast_dropdown_open(false);
}

fn cast_request(command: &str) -> String {
    let Ok(mut stream) = UnixStream::connect(CAST_SOCKET) else { return String::new() };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(1500)));
    let _ = stream.write_all(command.as_bytes());
    let _ = stream.write_all(b"\n");
    let mut reply = String::new();
    let mut reader = BufReader::new(stream);
    loop {
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {
                reply.push_str(&line);
                if command != "STATUS" || line.trim_end() == "end=1" {
                    break;
                }
            }
        }
    }
    reply
}

fn adjust_game_volume(ui: &QuickMenuWindow, delta: i32) {
    let v = (ui.get_game_volume() + delta).clamp(0, 100);
    ui.set_game_volume(v);
    thread::spawn(move || {
        if let Err(error) = audio_command(&format!("APPLICATIONS VOLUME SET {v}")) {
            eprintln!("quick-menu: game volume failed={error}");
        }
    });
}

fn wake_screen() {
    if fs::metadata(SCREEN_OFF_FILE).is_ok() {
        thread::spawn(|| {
            let _ = Command::new(DISPLAYCTL).args(["screen", "on"]).status();
        });
    }
}

fn start_web_subscription(tx: Sender<AppEvent>) {
    thread::spawn(move || loop {
        if let Ok(mut stream) = UnixStream::connect(WEB_SOCKET) {
            if stream.write_all(b"SUBSCRIBE\n").is_ok() {
                let mut reader = BufReader::new(stream);
                let mut block = String::new();
                let mut line = String::new();
                let mut last = (false, 0);
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            block.push_str(&line);
                            if line.trim_end() == "end=1" {
                                let state = web_state_from(&block);
                                block.clear();
                                /* Page and history changes publish too:
                                 * forward only what the menu shows. */
                                if state != last {
                                    last = state;
                                    if tx.send(AppEvent::Web(state.0, state.1)).is_err() {
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        let _ = tx.send(AppEvent::Web(false, 100));
        /* Only reached while webd is absent or restarting. */
        thread::sleep(Duration::from_secs(1));
    });
}

/* WEB rows: 20 Resume, 27 Keyboard, 21 Go Back, 22 Go Forward, 23 Reload,
 * 24 Zoom (confirm restores 100%), 25 Add Bookmark, 26 Quit Web. Back,
 * Forward, Reload and Zoom keep the menu open over the live page. */
fn activate_web_row(
    ui: &QuickMenuWindow,
    mapped: &mut bool,
    brightness_dirty: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    let close = match ui.get_selected_index() {
        20 => true,
        27 => {
            if !ui.get_web_ime_active() {
                return Ok(());
            }
            KEYBOARD_REQUESTED.store(true, std::sync::atomic::Ordering::SeqCst);
            true
        }
        21 => {
            web_command("BACK");
            false
        }
        22 => {
            web_command("FORWARD");
            false
        }
        23 => {
            web_command("RELOAD");
            false
        }
        24 => {
            web_command("ZOOM_RESET");
            false
        }
        25 => {
            web_command("BOOKMARK_ADD");
            true
        }
        26 => {
            web_command("QUIT");
            ui.set_web_section_visible(false);
            true
        }
        _ => return Ok(()),
    };
    if close {
        flush_brightness(ui, brightness_dirty)?;
        unmap_overlay(ui, queue, state, conn);
        *mapped = false;
    } else {
        redraw_overlay(ui, queue, state, qh, conn)?;
    }
    Ok(())
}

/*
 * Web Mode keyboard. Shown while the page's text field has the input
 * method focus; it takes the controller (inputd MENU OPEN) and commits
 * each key into the field. Back closes it (the field keeps the focus;
 * Keyboard in the Quick Menu shows it again).
 */
/* Keyboard row: show the keyboard once the menu has closed. */
static KEYBOARD_REQUESTED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[derive(Default)]
struct WebKeyboard {
    ime: Option<std::sync::Arc<ime::Ime>>,
    field: ime::ImeState,
    visible: bool,
    /* Closed by the user for this field: not reopened by its updates. */
    dismissed: bool,
    page: i32,
    shift: bool,
    layout: String,
    /* Typing into the terminal (nuubos-termd): terminal keys on the
     * bottom row, Ctrl held for the next key, what was typed since the
     * last Enter as the preview (foot reports no surrounding text). */
    terminal: bool,
    ctrl: bool,
    typed: String,
}

const KEY_SHIFT: &str = "⇧";
const KEY_BACKSPACE: &str = "⌫";
const KEY_SPACE: &str = "␣";
const KEY_NUMBERS: &str = "123";
const KEY_SYMBOLS: &str = "#+=";
const KEY_LETTERS: &str = "abc";
const KEY_DONE: &str = "\u{1}done";
const KEY_CTRL: &str = "\u{1}ctrl";
const KEY_ESC: &str = "\u{1}esc";
const KEY_TAB: &str = "\u{1}tab";
const KEY_ENTER: &str = "\u{1}enter";

fn keyboard_layout() -> String {
    let reply = (|| -> std::io::Result<String> {
        let mut stream = UnixStream::connect(REGIONAL_SOCKET)?;
        stream.set_read_timeout(Some(Duration::from_millis(300)))?;
        stream.write_all(b"STATUS\n")?;
        stream.shutdown(std::net::Shutdown::Write)?;
        let mut reply = String::new();
        stream.read_to_string(&mut reply)?;
        Ok(reply)
    })()
    .unwrap_or_default();
    match reply.lines().find_map(|l| l.strip_prefix("keyboard_layout=")).unwrap_or("us") {
        "fr" => "azerty".into(),
        "de" => "qwertz".into(),
        _ => "qwerty".into(),
    }
}

fn keyboard_rows(kb: &WebKeyboard) -> [Vec<&'static str>; 4] {
    let bottom = |page: &'static str| {
        if kb.terminal {
            /* Shell keys instead of Done (Back closes the keyboard). */
            if page == KEY_LETTERS {
                vec![page, KEY_SYMBOLS, KEY_CTRL, "/", KEY_SPACE, KEY_SPACE, "|", "-", KEY_BACKSPACE, KEY_ENTER]
            } else {
                vec![page, KEY_CTRL, KEY_ESC, KEY_TAB, KEY_SPACE, KEY_SPACE, ".", "-", KEY_BACKSPACE, KEY_ENTER]
            }
        } else {
            vec![page, KEY_SYMBOLS, "@", "/", KEY_SPACE, KEY_SPACE, ".", "-", KEY_BACKSPACE, KEY_DONE]
        }
    };
    match kb.page {
        1 => [
            vec!["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
            vec!["!", "?", "#", "$", "%", "&", "*", "(", ")", "'"],
            vec!["+", "=", ":", ";", ",", "_", "\"", "<", ">", "~"],
            bottom(KEY_LETTERS),
        ],
        2 => [
            vec!["`", "^", "€", "£", "¥", "°", "§", "|", "\\", "¿"],
            vec!["[", "]", "{", "}", "¡", "«", "»", "·", "¨", "´"],
            vec!["à", "è", "é", "ì", "ò", "ù", "ç", "ñ", "ä", "ö"],
            if kb.terminal {
                vec![KEY_LETTERS, KEY_NUMBERS, KEY_CTRL, KEY_ESC, KEY_SPACE, KEY_SPACE, KEY_TAB, "@", KEY_BACKSPACE, KEY_ENTER]
            } else {
                vec![KEY_LETTERS, KEY_NUMBERS, "ü", "ß", KEY_SPACE, KEY_SPACE, "ê", "â", KEY_BACKSPACE, KEY_DONE]
            },
        ],
        _ => {
            let (r0, r1, r2): (Vec<&'static str>, Vec<&'static str>, Vec<&'static str>) = match kb.layout.as_str() {
                "azerty" => (
                    vec!["a", "z", "e", "r", "t", "y", "u", "i", "o", "p"],
                    vec!["q", "s", "d", "f", "g", "h", "j", "k", "l", "m"],
                    vec![KEY_SHIFT, "w", "x", "c", "v", "b", "n", ",", "'", "?"],
                ),
                "qwertz" => (
                    vec!["q", "w", "e", "r", "t", "z", "u", "i", "o", "p"],
                    vec!["a", "s", "d", "f", "g", "h", "j", "k", "l", "'"],
                    vec![KEY_SHIFT, "y", "x", "c", "v", "b", "n", "m", ",", "?"],
                ),
                _ => (
                    vec!["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
                    vec!["a", "s", "d", "f", "g", "h", "j", "k", "l", "'"],
                    vec![KEY_SHIFT, "z", "x", "c", "v", "b", "n", "m", ",", "?"],
                ),
            };
            [r0, r1, r2, bottom(KEY_NUMBERS)]
        }
    }
}

fn key_label(ui: &QuickMenuWindow, kb: &WebKeyboard, key: &str) -> String {
    if key == KEY_DONE {
        tr(ui, 697, "Done")
    } else if key == KEY_CTRL {
        /* Key cap names, as printed on keyboards. */
        if kb.ctrl { "CTRL".into() } else { "Ctrl".into() }
    } else if key == KEY_ESC {
        "Esc".into()
    } else if key == KEY_TAB {
        "Tab".into()
    } else if key == KEY_ENTER {
        "↵".into()
    } else if kb.shift && kb.page == 0 && key.chars().count() == 1 && key.chars().all(char::is_alphabetic) {
        key.to_uppercase()
    } else {
        key.to_owned()
    }
}

fn apply_keyboard(ui: &QuickMenuWindow, kb: &WebKeyboard) {
    let rows = keyboard_rows(kb);
    let model = |row: &Vec<&'static str>| -> ModelRc<SharedString> {
        /* The space bar is two keys wide (columns 4 and 5 of the bottom
         * row): one key drawn twice as wide, keys after it one column on
         * (quick_menu.slint). */
        let labels: Vec<SharedString> = row
            .iter()
            .enumerate()
            .filter(|(i, k)| !(**k == KEY_SPACE && *i > 0 && row[*i - 1] == KEY_SPACE))
            .map(|(_, k)| key_label(ui, kb, k).into())
            .collect();
        ModelRc::from(Rc::new(VecModel::from(labels)))
    };
    ui.set_kb_row_zero(model(&rows[0]));
    ui.set_kb_row_one(model(&rows[1]));
    ui.set_kb_row_two(model(&rows[2]));
    ui.set_kb_row_three(model(&rows[3]));
    ui.set_kb_preview(if kb.terminal { kb.typed.clone() } else { kb.field.surrounding.clone() }.into());
    ui.set_kb_mode_label(match kb.page {
        1 => "123".into(),
        2 => "#+=".into(),
        _ => kb.layout.to_uppercase().into(),
    });
}

fn show_keyboard(ui: &QuickMenuWindow, kb: &mut WebKeyboard) {
    if kb.visible {
        return;
    }
    kb.visible = true;
    ui.set_kb_visible(true);
    ui.set_kb_terminal(kb.terminal);
    TERMINAL_SHEET.store(kb.terminal, std::sync::atomic::Ordering::SeqCst);
    kb.dismissed = false;
    kb.page = 0;
    kb.shift = false;
    kb.ctrl = false;
    kb.typed.clear();
    kb.layout = keyboard_layout();
    refresh_i18n(ui);
    refresh_hint_mapping(ui);
    ui.set_kb_index(0);
    apply_keyboard(ui, kb);
    set_menu_capture(true);
}

fn hide_keyboard(ui: &QuickMenuWindow, kb: &mut WebKeyboard) {
    if !kb.visible {
        return;
    }
    kb.visible = false;
    ui.set_kb_visible(false);
    set_menu_capture(false);
}

fn keyboard_move(ui: &QuickMenuWindow, kb: &WebKeyboard, dx: i32, dy: i32) {
    let rows = keyboard_rows(kb);
    let index = ui.get_kb_index();
    let row = (index / 10 + dy).rem_euclid(4);
    let keys = &rows[row as usize];
    let len = keys.len() as i32;
    let mut col = (index % 10).min(len - 1);
    if dx != 0 {
        /* The space bar is two keys wide: one step crosses it. */
        let from_space = keys[col as usize] == KEY_SPACE;
        col = (col + dx).rem_euclid(len);
        if from_space && keys[col as usize] == KEY_SPACE {
            col = (col + dx).rem_euclid(len);
        }
    }
    /* Its second half is not drawn: the focus sits on the first. */
    if col > 0 && keys[col as usize] == KEY_SPACE && keys[col as usize - 1] == KEY_SPACE {
        col -= 1;
    }
    ui.set_kb_index(row * 10 + col);
}

/* One controller action while the keyboard is shown. Returns false when
 * the keyboard closed. */
fn keyboard_action(ui: &QuickMenuWindow, kb: &mut WebKeyboard, action: &str) -> bool {
    match action {
        "menu_up" => keyboard_move(ui, kb, 0, -1),
        "menu_down" => keyboard_move(ui, kb, 0, 1),
        "menu_left" => keyboard_move(ui, kb, -1, 0),
        "menu_right" => keyboard_move(ui, kb, 1, 0),
        "menu_back" => {
            kb.dismissed = true;
            hide_keyboard(ui, kb);
            if kb.terminal {
                ui.set_term_keyboard_on(false);
                term_command("KEYBOARD\t0");
            }
            return false;
        }
        /* Start: Enter in the terminal, Done in a page's field (as the
         * system keyboard of Settings). */
        "settings" if kb.terminal => {
            terminal_type(ui, kb, "\r");
            kb.typed.clear();
            kb.ctrl = false;
            apply_keyboard(ui, kb);
        }
        "settings" => {
            kb.dismissed = true;
            hide_keyboard(ui, kb);
            return false;
        }
        "face_north" if kb.terminal => {
            terminal_type(ui, kb, "\u{7f}");
            kb.typed.pop();
            apply_keyboard(ui, kb);
        }
        "face_north" => {
            if let Some(ime) = kb.ime.as_ref() {
                ime.delete_before(&kb.field);
            }
        }
        "menu_confirm" => {
            let index = ui.get_kb_index();
            let rows = keyboard_rows(kb);
            let Some(key) = rows.get((index / 10) as usize).and_then(|r| r.get((index % 10) as usize)).copied() else {
                return true;
            };
            match key {
                KEY_DONE => {
                    kb.dismissed = true;
                    hide_keyboard(ui, kb);
                    return false;
                }
                KEY_SHIFT => kb.shift = !kb.shift,
                KEY_CTRL => kb.ctrl = !kb.ctrl,
                KEY_ESC => terminal_type(ui, kb, "\u{1b}"),
                KEY_TAB => terminal_type(ui, kb, "\t"),
                KEY_ENTER => {
                    terminal_type(ui, kb, "\r");
                    kb.typed.clear();
                }
                KEY_BACKSPACE if kb.terminal => {
                    terminal_type(ui, kb, "\u{7f}");
                    kb.typed.pop();
                }
                KEY_NUMBERS => kb.page = 1,
                KEY_SYMBOLS => kb.page = 2,
                KEY_LETTERS => kb.page = 0,
                KEY_BACKSPACE => {
                    if let Some(ime) = kb.ime.as_ref() {
                        ime.delete_before(&kb.field);
                    }
                }
                _ if kb.terminal => {
                    let text = if key == KEY_SPACE { " ".to_owned() } else { key_label(ui, kb, key) };
                    let mut chars = text.chars();
                    match (kb.ctrl, chars.next(), chars.next()) {
                        /* Ctrl + a letter (or @ [ \ ] ^ _): the control
                         * character, as a keyboard sends it. */
                        (true, Some(c), None) if c.is_ascii_alphabetic() || "@[\\]^_".contains(c) => {
                            let code = (c.to_ascii_uppercase() as u8) & 0x1f;
                            terminal_type(ui, kb, &(code as char).to_string());
                            kb.ctrl = false;
                        }
                        _ => {
                            terminal_type(ui, kb, &text);
                            kb.typed.push_str(&text);
                            kb.ctrl = false;
                        }
                    }
                    if kb.shift && kb.page == 0 && key != KEY_SPACE {
                        kb.shift = false;
                    }
                }
                _ => {
                    let text = if key == KEY_SPACE { " ".to_owned() } else { key_label(ui, kb, key) };
                    if let Some(ime) = kb.ime.as_ref() {
                        ime.commit_text(&text);
                    }
                    /* One capital, like a phone keyboard. */
                    if kb.shift && kb.page == 0 && key != KEY_SPACE {
                        kb.shift = false;
                    }
                }
            }
            apply_keyboard(ui, kb);
        }
        _ => {}
    }
    true
}

/* Text (or a control character) straight to the terminal's shell: foot
 * writes the input method's commits to the terminal as they are. Only the
 * end of what was typed stays in the preview. */
fn terminal_type(_ui: &QuickMenuWindow, kb: &mut WebKeyboard, text: &str) {
    if let Some(ime) = kb.ime.as_ref() {
        ime.commit_text(text);
    }
    if kb.typed.chars().count() > 60 {
        let cut: String = kb.typed.chars().skip(20).collect();
        kb.typed = cut;
    }
}

/* The terminal's on-screen keyboard follows termd's keyboard=, while the
 * menu is closed and the terminal has the input method. */
fn sync_terminal_keyboard(ui: &QuickMenuWindow, kb: &mut WebKeyboard, mapped: bool) -> bool {
    let want = ui.get_term_section_visible() && ui.get_term_keyboard_on() && !mapped
        && kb.field.active && !ui.get_lifecycle_active();
    if want && !kb.visible {
        kb.terminal = true;
        show_keyboard(ui, kb);
        true
    } else if !want && kb.visible && kb.terminal && !mapped {
        hide_keyboard(ui, kb);
        true
    } else {
        false
    }
}

fn selectable_menu_indices(ui: &QuickMenuWindow) -> Vec<i32> {
    let mut indices = Vec::new();
    if ui.get_game_section_visible() {
        indices.push(8);
        if !ui.get_game_hardcore() {
            indices.extend_from_slice(&[9, 10, 11]);
        }
        if ui.get_players_visible() {
            indices.push(PLAYERS_ROW);
        }
        if ui.get_game_nds() {
            indices.extend_from_slice(&[STYLUS_ROW, LAYOUT_ROW]);
            if ui.get_game_nds_layout() == NDS_PIP {
                indices.push(CORNER_ROW);
            }
        }
        if ui.get_game_discs() > 1 {
            indices.push(DISC_ROW);
        }
        indices.extend_from_slice(&[12, CHEATS_ROW, NET_ROW]);
        if ui.get_game_retroarch() {
            indices.push(13);
        }
        indices.extend_from_slice(&[18, 28, 14]);
    }
    if ui.get_port_section_visible() {
        indices.extend_from_slice(&[PORT_RESUME_ROW, PORT_QUIT_ROW]);
    }
    if ui.get_term_section_visible() {
        indices.extend_from_slice(&[TERM_RESUME_ROW, TERM_KEYBOARD_ROW, TERM_QUIT_ROW]);
    }
    if ui.get_ra_section_visible() {
        indices.extend_from_slice(&[RA_RESUME_ROW, RA_QUIT_ROW]);
    }
    if ui.get_stream_section_visible() {
        indices.extend_from_slice(if ui.get_stream_steamlink() { &[15, 36, 37, 38, 16] } else { &[15, 36, 37, 38, 16, 17] });
    }
    if ui.get_web_section_visible() {
        indices.extend_from_slice(&[20, 27, 21, 22, 23, 24, 25, 26]);
    }
    if ui.get_music_section_visible() {
        indices.extend_from_slice(&[31, 32, 33]);
        if ui.get_game_section_visible() {
            indices.push(34);
        }
        indices.push(35);
    }
    if ui.get_context_section_visible() {
        indices.push(TASK_ROW);
    }
    indices.push(3);
    if ui.get_brightness_visible() {
        indices.push(5);
    }
    if ui.get_cast_row_visible() {
        indices.push(CAST_ROW);
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

fn first_selectable_index(ui: &QuickMenuWindow) -> i32 {
    /* Resume is the safest default over a game; otherwise the single Audio
     * control, the first Display/Audio row. */
    if ui.get_game_section_visible() {
        8
    } else if ui.get_port_section_visible() {
        PORT_RESUME_ROW
    } else if ui.get_term_section_visible() {
        TERM_RESUME_ROW
    } else if ui.get_ra_section_visible() {
        RA_RESUME_ROW
    } else if ui.get_stream_section_visible() {
        15
    } else if ui.get_web_section_visible() {
        20
    } else if ui.get_music_section_visible() {
        31
    } else {
        3
    }
}

fn first_selectable_index_without_music(ui: &QuickMenuWindow) -> i32 {
    if ui.get_game_section_visible() {
        8
    } else if ui.get_port_section_visible() {
        PORT_RESUME_ROW
    } else if ui.get_term_section_visible() {
        TERM_RESUME_ROW
    } else if ui.get_ra_section_visible() {
        RA_RESUME_ROW
    } else if ui.get_stream_section_visible() {
        15
    } else if ui.get_web_section_visible() {
        20
    } else {
        3
    }
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
    ui.set_game_confirm_index(-1);
}

/* GAME → Player Assignment (EPIC-026), below State Slot while an external
 * controller is connected: the same controllersd
 * preference as Settings → Controllers & Input → Player Assignment, through
 * nuubos-controllersctl (the Quick Menu has no D-Bus client). */
const PLAYERS_ROW: i32 = 30;

#[derive(Default)]
struct PlayersState {
    /* None = the list of players; Some(n) = controllers for player n. */
    player: Option<i32>,
    /* (player, controller id, controller name, available) */
    players: Vec<(i32, String, String, bool)>,
    /* (id, name, connected) of external and built-in controllers */
    devices: Vec<(String, String, bool, bool)>,
}

thread_local! {
    static PLAYERS: RefCell<PlayersState> = RefCell::new(PlayersState::default());
}

fn controllersctl(args: &[&str]) -> Vec<Vec<String>> {
    let Ok(out) = Command::new(CONTROLLERSCTL).args(args).stdin(Stdio::null()).stderr(Stdio::null()).output() else {
        return Vec::new();
    };
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|line| line.split_once('=').filter(|(k, _)| *k != "end").map(|(_, v)| v.split('\t').map(str::to_owned).collect()))
        .collect()
}

fn refresh_players(ui: &QuickMenuWindow) {
    let devices: Vec<(String, String, bool, bool)> = controllersctl(&["devices"])
        .into_iter()
        .filter(|f| f.len() >= 5)
        .map(|f| (f[0].clone(), f[1].clone(), f[3] == "1", f[4] == "1"))
        .collect();
    let players: Vec<(i32, String, String, bool)> = controllersctl(&["players"])
        .into_iter()
        .filter(|f| f.len() >= 4)
        .map(|f| (f[0].parse().unwrap_or(0), f[1].clone(), f[2].clone(), f[3] == "1"))
        .filter(|p| p.0 > 0)
        .collect();
    let external = devices.iter().any(|(_, _, connected, builtin)| *connected && !*builtin);
    let label = players
        .iter()
        .find(|p| p.0 == 1 && !p.2.is_empty())
        .map(|p| format!("P1 • {}", p.2))
        .unwrap_or_default();
    ui.set_players_visible(external && ui.get_game_section_visible());
    ui.set_players_label(label.into());
    PLAYERS.with(|state| {
        let mut state = state.borrow_mut();
        state.players = players;
        state.devices = devices;
    });
}

fn show_players_level(ui: &QuickMenuWindow, player: Option<i32>, index: i32) {
    let options: Vec<SharedString> = PLAYERS.with(|state| {
        let mut state = state.borrow_mut();
        state.player = player;
        match player {
            None => state
                .players
                .iter()
                .map(|(n, _, name, available)| {
                    let who = if name.is_empty() { tr(ui, 122, "Automatic") } else { name.clone() };
                    let tail = if !name.is_empty() && !available { tr(ui, 221, " • unavailable") } else { String::new() };
                    format!("{}{n} • {who}{tail}", tr(ui, 220, "Player ")).into()
                })
                .collect(),
            Some(_) => std::iter::once(tr(ui, 122, "Automatic").into())
                .chain(state.devices.iter().map(|(_, name, connected, _)| {
                    if *connected { name.as_str().into() } else { format!("{name}{}", tr(ui, 221, " • unavailable")).into() }
                }))
                .collect(),
        }
    });
    let count = options.len() as i32;
    let title = match player {
        None => tr(ui, 13, "Player Assignment"),
        Some(n) => format!("{}{n}", tr(ui, 220, "Player ")),
    };
    let index = index.clamp(0, (count - 1).max(0));
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    ui.set_players_options(ModelRc::new(Rc::new(VecModel::from(options))));
    ui.set_players_dropdown_title(title.into());
    ui.set_players_dropdown_index(index);
    ui.set_players_dropdown_scroll((index - visible + 1).clamp(0, (count - visible).max(0)));
    ui.set_players_dropdown_open(count > 0);
}

fn open_players_dropdown(ui: &QuickMenuWindow) {
    refresh_players(ui);
    show_players_level(ui, None, 0);
}

fn move_players_dropdown(ui: &QuickMenuWindow, delta: i32) {
    let count = ui.get_players_options().row_count() as i32;
    if count <= 0 {
        return;
    }
    let next = (ui.get_players_dropdown_index() + delta).rem_euclid(count);
    ui.set_players_dropdown_index(next);
    let visible = ui.get_audio_output_dropdown_visible_rows().max(1);
    let mut scroll = ui.get_players_dropdown_scroll();
    if next < scroll {
        scroll = next;
    } else if next >= scroll + visible {
        scroll = next - visible + 1;
    }
    ui.set_players_dropdown_scroll(scroll.clamp(0, (count - visible).max(0)));
}

fn confirm_players_dropdown(ui: &QuickMenuWindow) {
    let index = ui.get_players_dropdown_index();
    let level = PLAYERS.with(|state| state.borrow().player);
    match level {
        None => {
            let chosen = PLAYERS.with(|state| {
                let state = state.borrow();
                state.players.get(index as usize).map(|p| {
                    /* Preselect the controller the player has now. */
                    let current = state.devices.iter().position(|d| d.0 == p.1).map(|i| i as i32 + 1).unwrap_or(0);
                    (p.0, current)
                })
            });
            if let Some((player, current)) = chosen {
                show_players_level(ui, Some(player), current);
            }
        }
        Some(player) => {
            let (id, name) = PLAYERS.with(|state| {
                let state = state.borrow();
                if index == 0 {
                    (String::new(), String::new())
                } else {
                    state.devices.get(index as usize - 1).map(|d| (d.0.clone(), d.1.clone())).unwrap_or_default()
                }
            });
            let player_arg = player.to_string();
            let ok = Command::new(CONTROLLERSCTL)
                .args(["assign", &player_arg, &id, &name])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .map(|status| status.success())
                .unwrap_or(false);
            append_action_log(&format!("player-assignment player={player} controller={id} ok={ok}"));
            refresh_players(ui);
            show_players_level(ui, None, player - 1);
        }
    }
}

fn back_players_dropdown(ui: &QuickMenuWindow) {
    match PLAYERS.with(|state| state.borrow().player) {
        Some(player) => show_players_level(ui, None, player - 1),
        None => ui.set_players_dropdown_open(false),
    }
}

/* Quick Menu row of the BACKGROUND TASK section (Stop, second press). */
const TASK_ROW: i32 = 29;

/* The section follows the running cancellable jobs (their Live
 * Notifications); hidden when none runs. */
fn apply_task_section(ui: &QuickMenuWindow, toasts: &Toasts) {
    let Some(task) = toasts.stoppable() else {
        ui.set_context_section_visible(false);
        if ui.get_selected_index() == TASK_ROW {
            ui.set_game_confirm_index(-1);
            ui.set_selected_index(first_selectable_index(ui));
        }
        return;
    };
    let title = if task.progress >= 0.0 {
        format!("{} · {}%", task.title, (task.progress * 100.0).round() as i32)
    } else {
        task.title.clone()
    };
    ui.set_context_section_visible(true);
    ui.set_context_primary_label(title.into());
    ui.set_context_primary_value(if toasts.stopping == task.id {
        tr(ui, 810, "Stopping…")
    } else {
        tr(ui, 809, "Stop")
    }.into());
}

/* Asks nuubos-jobd to cancel the job (the worker stops at a safe point;
 * its cancelled Live Notification ends the row). */
fn stop_task(toasts: &mut Toasts) {
    let Some(task) = toasts.stoppable() else { return };
    let (id, job) = (task.id.clone(), task.job.clone());
    toasts.stopping = id;
    thread::spawn(move || {
        let status = Command::new("/usr/bin/nuubos-jobctl").args(["cancel", &job])
            .stdout(Stdio::null()).stderr(Stdio::null()).status();
        if !status.is_ok_and(|s| s.success()) {
            eprintln!("quick-menu: cancel job {job} failed");
        }
    });
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

/* The performance overlay line goes to RetroArch, which draws it into the
 * game's own frame (emud OVERLAY_TEXT, RetroArch patch 0004). A layer
 * surface over the game made labwc composite every frame: ~10 % speed on
 * GPU-bound games (docs/dev/emulation.md). */
#[derive(Default)]
struct PerfOverlay {
    /* The line RetroArch currently draws ("" = none). */
    text: String,
}

impl PerfOverlay {
    fn sync(&mut self, running: bool, want: bool, text: &str) {
        if !running {
            /* A new RetroArch starts without a line. */
            self.text.clear();
            return;
        }
        let text = if want { text } else { "" };
        if text == self.text {
            return;
        }
        match emulation_command(&format!("OVERLAY_TEXT\t{text}")) {
            Ok(_) => self.text = text.to_owned(),
            Err(error) => eprintln!("quick-menu: performance overlay failed={error}"),
        }
    }
}

fn map_overlay(
    ui: &QuickMenuWindow,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    refresh_i18n(ui);
    refresh_theme(ui);
    let _ = fs::remove_file(MAPPED_FILE);
    let _ = fs::remove_file(SURFACE_FILE);

    ui.set_lifecycle_active(false);
    ui.set_lifecycle_mode("".into());
    /* Opened by the power key: the Power OSD replaces the side sheet from
     * the first frame. */
    ui.set_menu_panel_visible(!ui.get_power_osd_open());
    ui.set_surface_mode(SurfaceKind::Full.slint_mode());
    ui.set_volume_osd_visible(false);
    refresh_hint_mapping(ui);
    apply_display_snapshot(ui, read_display_snapshot());
    apply_battery_snapshot(ui, read_battery_snapshot());
    apply_audio_snapshot(ui, &read_audio_snapshot());
    refresh_system_profile(ui);
    let game = read_game_snapshot();
    apply_game_snapshot(ui, &game);
    ui.set_game_confirm_index(-1);
    /* The game waits underneath the menu (EPIC-005); every way out of the
     * menu resumes it (unmap_overlay). Switch User is not offered over a
     * game. */
    if game.running || game.menu {
        game_action("PAUSE");
    }
    let stream = read_stream_state();
    let (streaming, steamlink) = (stream.running, stream.steamlink);
    apply_stream_running(ui, stream);
    if streaming && !steamlink {
        refresh_stream_stats(ui);
    }
    /* A port waits underneath like a game (portsd freezes it). */
    let port = read_port_state();
    apply_port_running(ui, port);
    if port.running {
        if let Err(error) = port_command("PAUSE") {
            eprintln!("quick-menu: port PAUSE failed={error}");
        }
    }
    let (web, zoom) = read_web_state();
    apply_web_state(ui, web, zoom);
    ui.set_switch_user_visible(!game.running && !streaming && !port.running && !web && switch_user_available());
    refresh_players(ui);
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
    refresh_theme(ui);
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

    let want = if ui.get_closing_active() {
        Some(SurfaceKind::Closing)
    } else if ui.get_kb_visible() {
        Some(SurfaceKind::Keyboard)
    } else {
        ui.get_volume_osd_visible().then_some(SurfaceKind::Volume)
    };

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
    let kind = if mode == "reboot" { "reboot" } else { "poweroff" };
    ui.set_lifecycle_mode(mode.into());
    if let Some((logical_width, logical_height)) = state.configured_size {
        /* Physical surface size: the same scale render_and_commit uses. */
        let scale120 = if state.viewport.is_some() { state.scale120.max(120) } else { 120 };
        let width = (logical_width * scale120 + 60) / 120;
        let height = (logical_height * scale120 + 60) / 120;
        let palette = splash::load_palette();
        ui.set_splash_image(splash::image("reboot", width, height, &palette));
        if kind == "poweroff" {
            ui.set_splash_base(splash::image("poweroff", width, height, &palette));
        }
        let (x, y, w, h) = splash::track(width, height);
        let k = 120.0 / scale120 as f32;
        ui.set_splash_track_x(x * k);
        ui.set_splash_track_y(y * k);
        ui.set_splash_track_w(w * k);
        ui.set_splash_track_h(h * k);
        ui.set_splash_accent(splash::color(palette.accent));
    }
    /* The LEDs play the same shape for the same time (nuubos-lightingd);
     * nuubos-systemd's own lifecycle call later finds it already running. */
    let _ = std::process::Command::new("/usr/lib/nuubos/lifecycle-lighting")
        .arg(kind)
        .stdin(std::process::Stdio::null())
        .spawn();
    ui.set_lifecycle_active(true);

    let start = Instant::now();
    let mut deadline = start;
    loop {
        let f = splash::frame(kind, start.elapsed().as_millis() as u64);
        ui.set_splash_top_alpha(f.top_alpha);
        ui.set_splash_base_alpha(f.base_alpha);
        ui.set_splash_fill_from(f.fill_from);
        ui.set_splash_fill_to(f.fill_to);
        ui.set_splash_fill_alpha(f.fill_alpha);
        redraw_overlay(ui, queue, state, qh, conn)?;
        if f.done {
            break;
        }
        pace_animation_frame(&mut deadline);
    }

    /*
     * Hold the final lifecycle raster (identical to nuubos-splash's) on the
     * Overlay layer: the lifecycle finalizer preserves this KMS scanout
     * while Labwc exits.
     */
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
    ui.set_game_slot_dropdown_open(false);
    ui.set_game_layout_dropdown_open(false);
    ui.set_players_dropdown_open(false);
    ui.set_cast_dropdown_open(false);
    ui.set_game_sheet_open(false);
    ui.set_power_osd_open(false);
    ui.set_power_osd_confirm(false);
    set_menu_capture(false);
    if ui.get_game_section_visible() || ui.get_ra_section_visible() {
        game_action("RESUME");
    }
    if ui.get_port_section_visible() {
        if let Err(error) = port_command("RESUME") {
            eprintln!("quick-menu: port RESUME failed={error}");
        }
    }

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
    /* Restart and Power Off end the session: second press (armed row). */
    let index = ui.get_selected_index();
    if matches!(index, 1 | 2) && ui.get_game_confirm_index() != index {
        ui.set_game_confirm_index(index);
        return redraw_overlay(ui, queue, state, qh, conn);
    }
    match index {
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
        1 => end_session(true, ui, notifier, brightness_dirty, queue, state, qh, conn)?,
        2 => end_session(false, ui, notifier, brightness_dirty, queue, state, qh, conn)?,
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

/* Restart (or Power Off): sound, curtain, then the central lifecycle path. */
fn end_session(
    restart: bool,
    ui: &QuickMenuWindow,
    notifier: &mut Notifier,
    brightness_dirty: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    flush_brightness(ui, brightness_dirty)?;
    play_named_sound(if restart { "restart" } else { "poweroff" });
    thread::sleep(Duration::from_millis(if restart { 220 } else { 300 }));
    set_menu_capture(false);
    notifier.clear(ui, conn);
    ui.set_power_osd_open(false);
    animate_shutdown(if restart { "reboot" } else { "poweroff" }, ui, queue, state, qh, conn)?;
    spawn_lifecycle_action(if restart { "restart" } else { "poweroff" });
    Ok(())
}

/* Power OSD (power key, in every context: Home, Settings, games, streams,
 * Web; never over a full-screen operation). The game waits paused
 * underneath, as under the Quick Menu. */
fn open_power_osd(
    ui: &QuickMenuWindow,
    mapped: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    /* Not over a full-screen operation (system update, SD2 restore): a
     * restart or Sleep would interrupt it (user request 2026-10-12). */
    if operation_screen() {
        return Ok(());
    }
    ui.set_power_osd_index(0);
    ui.set_power_osd_confirm(false);
    ui.set_power_osd_open(true);
    ui.set_game_slot_dropdown_open(false);
    ui.set_game_layout_dropdown_open(false);
    ui.set_audio_output_dropdown_open(false);
    ui.set_cast_dropdown_open(false);
    ui.set_game_sheet_open(false);
    ui.set_game_confirm_index(-1);
    if *mapped {
        ui.set_menu_panel_visible(false);
    } else {
        map_overlay(ui, queue, state, qh, conn)?;
        *mapped = true;
    }
    redraw_overlay(ui, queue, state, qh, conn)
}

/* Power OSD navigation: Sleep on top, Restart | Power Off below (second
 * press). Sleep is committed on the confirm release (handle_event). */
fn handle_power_osd(
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
    let current = ui.get_power_osd_index();
    let select = |index: i32| {
        ui.set_power_osd_index(index);
        ui.set_power_osd_confirm(false);
    };
    match event.action.as_str() {
        "menu_back" => {
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
            return Ok(());
        }
        "menu_up" => select(0),
        "menu_down" if current == 0 => select(1),
        "menu_left" if current > 0 => select(1),
        "menu_right" if current > 0 => select(2),
        "menu_confirm" if current == 0 => {
            *pending_sleep = true;
            append_action_log("action=sleep phase=confirm-press-observed capture=held source=power-osd");
            return Ok(());
        }
        "menu_confirm" if ui.get_power_osd_confirm() => {
            return end_session(current == 1, ui, notifier, brightness_dirty, queue, state, qh, conn);
        }
        "menu_confirm" => ui.set_power_osd_confirm(true),
        _ => return Ok(()),
    }
    redraw_overlay(ui, queue, state, qh, conn)
}

/* STREAM rows: 15 Resume, 36 PC Home Button, 37 Steam Quick Access (the
 * menu closes, then the PC sees the buttons), 38 Performance Overlay (the
 * user's switch; the menu stays open), 16 Quit Stream (the application
 * keeps running on the PC), 17 Quit and Close Application (second
 * press). */
fn activate_stream_row(
    ui: &QuickMenuWindow,
    mapped: &mut bool,
    brightness_dirty: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    let index = ui.get_selected_index();
    if index == 17 && ui.get_game_confirm_index() != index {
        ui.set_game_confirm_index(index);
        return redraw_overlay(ui, queue, state, qh, conn);
    }
    if index == 38 {
        let value = if ui.get_stream_overlay_on() { "0" } else { "1" };
        match stream_command(&format!("SET\toverlay\t{value}"), Duration::from_millis(700)) {
            Ok(reply) if reply.starts_with("OK") => ui.set_stream_overlay_on(value == "1"),
            Ok(reply) => eprintln!("quick-menu: stream overlay {value} refused={}", reply.trim()),
            Err(error) => eprintln!("quick-menu: stream overlay {value} failed={error}"),
        }
        return redraw_overlay(ui, queue, state, qh, conn);
    }
    let command = match index {
        15 => None,
        16 => Some("QUIT"),
        17 => Some("QUIT\tclose"),
        36 => Some("BUTTON\thome"),
        37 => Some("BUTTON\tquick-access"),
        _ => return Ok(()),
    };
    if let Some(command) = command {
        if let Err(error) = stream_command(command, Duration::from_millis(700)) {
            eprintln!("quick-menu: stream {command} failed={error}");
        }
        if index == 16 || index == 17 {
            ui.set_stream_section_visible(false);
        }
    }
    flush_brightness(ui, brightness_dirty)?;
    unmap_overlay(ui, queue, state, conn);
    *mapped = false;
    Ok(())
}

/* GAME rows (8 Resume .. 11 State Slot, NDS 42 Stylus and 43 Screen
 * Layout, 12 Restart, 40 Cheats, 41 Netplay, 13 RetroArch Advanced, 18
 * Screenshot, 28 Performance Overlay, 14 Quit).
 * The screenshot is RetroArch's own rendering, so the menu is not in it.
 * Load State, Restart Game and Quit Game
 * lose unsaved progress and take a second press (EPIC-005 safety rules). */
fn activate_game_row(
    ui: &QuickMenuWindow,
    mapped: &mut bool,
    brightness_dirty: &mut bool,
    queue: &mut EventQueue<WaylandState>,
    state: &mut WaylandState,
    qh: &QueueHandle<WaylandState>,
    conn: &Connection,
) -> Result<(), Box<dyn std::error::Error>> {
    let index = ui.get_selected_index();
    if matches!(index, 10 | 12 | 14) && ui.get_game_confirm_index() != index {
        ui.set_game_confirm_index(index);
        return redraw_overlay(ui, queue, state, qh, conn);
    }
    let mut resume = true;
    match index {
        8 => {}
        9 => game_action("SAVE_STATE"),
        10 => game_action("LOAD_STATE"),
        11 => {
            open_slot_dropdown(ui);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        PLAYERS_ROW if ui.get_players_visible() => {
            open_players_dropdown(ui);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        DISC_ROW if ui.get_game_discs() > 1 => {
            open_disc_dropdown(ui);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        12 => game_action("RESET"),
        CHEATS_ROW | NET_ROW => {
            open_game_sheet(ui, index);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        13 if ui.get_game_retroarch() => {
            /* RetroArch's own menu takes over; it pauses the game. */
            game_action("ADVANCED");
            resume = false;
        }
        14 => {
            game_action("QUIT");
            resume = false;
        }
        18 => game_action("SCREENSHOT"),
        STYLUS_ROW if ui.get_game_nds() => {
            /* Switch, applied live; the menu stays open like row 28. */
            toggle_stylus(ui);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        LAYOUT_ROW if ui.get_game_nds() => {
            open_layout_dropdown(ui);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        CORNER_ROW if ui.get_game_nds() => {
            /* The next corner, applied live; the menu stays open. */
            step_nds_corner(ui, 1);
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        28 => {
            /* The user's Settings -> Gaming switch; emud applies it live.
             * The menu stays open (the overlay shows once it closes). */
            let value = if ui.get_game_overlay_on() { "0" } else { "1" };
            if let Err(error) = emulation_command(&format!("SET\toverlay\t{value}")) {
                eprintln!("quick-menu: game overlay {value} failed={error}");
            }
            apply_game_snapshot(ui, &read_game_snapshot());
            return redraw_overlay(ui, queue, state, qh, conn);
        }
        _ => return Ok(()),
    }
    if !resume {
        ui.set_game_section_visible(false);
    }
    flush_brightness(ui, brightness_dirty)?;
    unmap_overlay(ui, queue, state, conn);
    *mapped = false;
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

        let sleep_selected = if ui.get_power_osd_open() {
            /* The OSD opens on Sleep: only a press seen there counts. */
            ui.get_power_osd_index() == 0 && *pending_sleep
        } else {
            ui.get_selected_index() == 0
        };
        if event.action == "menu_confirm"
            && *mapped
            && !ui.get_lifecycle_active()
            && sleep_selected
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
            if notifier.toasts.resume() {
                notifier.apply(ui);
                notifier.sync(conn, ui.get_lifecycle_active());
            }
            result?;
        } else if event.action == "menu_confirm" {
            /*
             * Never carry a stale press transaction into a later menu state.
             */
            *pending_sleep = false;
        }

        return Ok(());
    }

    /* A Quick Menu key that opens nothing (user picker, setup, safe mode)
     * stays silent. */
    if *mapped || (event.action == "quick_menu" && menu_allowed()) {
        let lifecycle_confirm = event.action == "menu_confirm"
            && *mapped
            && if ui.get_power_osd_open() {
                ui.get_power_osd_index() > 0 && ui.get_power_osd_confirm()
            } else {
                (ui.get_selected_index() == 1 || ui.get_selected_index() == 2)
                    && ui.get_game_confirm_index() == ui.get_selected_index()
            };
        let slider_adjustment = *mapped
            && (event.action == "menu_left" || event.action == "menu_right")
            && (ui.get_selected_index() == 3 || ui.get_selected_index() == 24 ||
                (ui.get_selected_index() == 5 && ui.get_brightness_visible()));
        if !lifecycle_confirm && !slider_adjustment {
            play_ui_sound(event.action.as_str());
        }
    }

    /* Media Screen Off, like Sleep (user request 2026-10-13): Power turns
     * the screen back on and does nothing else; the menu does not open. */
    if fs::metadata(SCREEN_OFF_FILE).is_ok() {
        if event.action == "power" {
            wake_screen();
        }
        return Ok(());
    }
    if event.action == "power" {
        if ui.get_lifecycle_active() {
            return Ok(());
        }
        flush_brightness(ui, brightness_dirty)?;
        if *mapped && ui.get_power_osd_open() {
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
            return Ok(());
        }
        return open_power_osd(ui, mapped, queue, state, qh, conn);
    }

    if *mapped && ui.get_power_osd_open() && event.action != "quick_menu" {
        return handle_power_osd(
            event, ui, notifier, mapped, pending_sleep, brightness_dirty, queue, state, qh, conn,
        );
    }

    if *mapped && ui.get_game_layout_dropdown_open() {
        match event.action.as_str() {
            "menu_up" => ui.set_game_layout_dropdown_index((ui.get_game_layout_dropdown_index() + 3) % 4),
            "menu_down" => ui.set_game_layout_dropdown_index((ui.get_game_layout_dropdown_index() + 1) % 4),
            "menu_confirm" => apply_layout_dropdown(ui),
            "menu_back" => ui.set_game_layout_dropdown_open(false),
            _ => {}
        }
        redraw_overlay(ui, queue, state, qh, conn)?;
        return Ok(());
    }

    if *mapped && ui.get_game_slot_dropdown_open() {
        match event.action.as_str() {
            "menu_up" => move_slot_dropdown(ui, -1),
            "menu_down" => move_slot_dropdown(ui, 1),
            /* A disc choice goes back to the game: the change needs it
             * running (the tray closes after a moment of play). */
            "menu_confirm" if ui.get_game_slot_dropdown_disc() => {
                apply_disc_dropdown(ui);
                flush_brightness(ui, brightness_dirty)?;
                unmap_overlay(ui, queue, state, conn);
                *mapped = false;
                return Ok(());
            }
            "menu_confirm" => apply_slot_dropdown(ui),
            "menu_back" => ui.set_game_slot_dropdown_open(false),
            _ => {}
        }
        redraw_overlay(ui, queue, state, qh, conn)?;
        return Ok(());
    }

    if *mapped && ui.get_game_sheet_open() {
        let mut close = false;
        match event.action.as_str() {
            "menu_up" => move_game_sheet(ui, -1),
            "menu_down" => move_game_sheet(ui, 1),
            "menu_confirm" => close = confirm_game_sheet(ui),
            "menu_back" => back_game_sheet(ui),
            _ => {}
        }
        if close {
            /* Host Game: the game closes and starts again as a host. */
            ui.set_game_section_visible(false);
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
            return Ok(());
        }
        redraw_overlay(ui, queue, state, qh, conn)?;
        return Ok(());
    }

    if *mapped && ui.get_players_dropdown_open() {
        match event.action.as_str() {
            "menu_up" => move_players_dropdown(ui, -1),
            "menu_down" => move_players_dropdown(ui, 1),
            "menu_confirm" => confirm_players_dropdown(ui),
            "menu_back" => back_players_dropdown(ui),
            _ => {}
        }
        redraw_overlay(ui, queue, state, qh, conn)?;
        return Ok(());
    }

    if *mapped && ui.get_cast_dropdown_open() {
        match event.action.as_str() {
            "menu_up" => move_cast_dropdown(ui, -1),
            "menu_down" => move_cast_dropdown(ui, 1),
            "menu_confirm" => confirm_cast_dropdown(ui),
            "menu_back" => ui.set_cast_dropdown_open(false),
            _ => {}
        }
        redraw_overlay(ui, queue, state, qh, conn)?;
        return Ok(());
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
            } else if menu_allowed() {
                map_overlay(ui, queue, state, qh, conn)?;
                *mapped = true;
            }
        }
        "menu_back" if *mapped => {
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
        }
        /* Before the section arms: GAME/STREAM/WEB take every index >= 8/15/20. */
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_cast_row_visible()
            && ui.get_selected_index() == CAST_ROW =>
        {
            open_cast_dropdown(ui);
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_context_section_visible()
            && ui.get_selected_index() == TASK_ROW =>
        {
            if ui.get_game_confirm_index() != TASK_ROW {
                ui.set_game_confirm_index(TASK_ROW);
            } else {
                ui.set_game_confirm_index(-1);
                stop_task(&mut notifier.toasts);
                apply_task_section(ui, &notifier.toasts);
            }
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_confirm" | "menu_left" | "menu_right" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_music_section_visible()
            && (31..=35).contains(&ui.get_selected_index()) =>
        {
            let left = event.action == "menu_left";
            let right = event.action == "menu_right";
            match ui.get_selected_index() {
                31 if left => media_command("PREV".into()),
                31 if right => media_command("NEXT".into()),
                31 => {
                    ui.set_music_playing(!ui.get_music_playing());
                    media_command("PAUSE".into());
                }
                32 if !left && !right => media_command("NEXT".into()),
                33 if left || right => {
                    let v = (ui.get_music_volume() + if left { -5 } else { 5 }).clamp(0, 100);
                    ui.set_music_volume(v);
                    media_command(format!("VOLUME\t{v}"));
                }
                34 if left || right => adjust_game_volume(ui, if left { -5 } else { 5 }),
                35 if !left && !right => media_command("STOP".into()),
                _ => {}
            }
            redraw_overlay(ui, queue, state, qh, conn)?;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_game_section_visible()
            && ui.get_selected_index() >= 8 =>
        {
            activate_game_row(ui, mapped, brightness_dirty, queue, state, qh, conn)?;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_web_section_visible()
            && (20..=27).contains(&ui.get_selected_index()) =>
        {
            activate_web_row(ui, mapped, brightness_dirty, queue, state, qh, conn)?;
        }
        "menu_left" | "menu_right" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_web_section_visible()
            && ui.get_selected_index() == 24 =>
        {
            web_command(if event.action == "menu_left" { "ZOOM_OUT" } else { "ZOOM_IN" });
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_port_section_visible()
            && (PORT_RESUME_ROW..=PORT_QUIT_ROW).contains(&ui.get_selected_index()) =>
        {
            if ui.get_selected_index() == PORT_QUIT_ROW {
                if let Err(error) = port_command("QUIT") {
                    eprintln!("quick-menu: port QUIT failed={error}");
                }
                /* Nothing to resume: the port is ending. */
                ui.set_port_section_visible(false);
            }
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_ra_section_visible()
            && (RA_RESUME_ROW..=RA_QUIT_ROW).contains(&ui.get_selected_index()) =>
        {
            if ui.get_selected_index() == RA_QUIT_ROW {
                game_action("QUIT");
                /* Nothing to resume: RetroArch is closing. */
                ui.set_ra_section_visible(false);
            }
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_term_section_visible()
            && (TERM_RESUME_ROW..=TERM_QUIT_ROW).contains(&ui.get_selected_index()) =>
        {
            match ui.get_selected_index() {
                /* The keyboard follows termd once the menu has closed. */
                TERM_KEYBOARD_ROW => {
                    let on = !ui.get_term_keyboard_on();
                    ui.set_term_keyboard_on(on);
                    term_command(if on { "KEYBOARD\t1" } else { "KEYBOARD\t0" });
                }
                TERM_QUIT_ROW => {
                    term_command("QUIT");
                    ui.set_term_section_visible(false);
                }
                _ => {}
            }
            flush_brightness(ui, brightness_dirty)?;
            unmap_overlay(ui, queue, state, conn);
            *mapped = false;
        }
        "menu_confirm" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_stream_section_visible()
            && ui.get_selected_index() >= 15 =>
        {
            activate_stream_row(ui, mapped, brightness_dirty, queue, state, qh, conn)?;
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
        "menu_left" | "menu_right" if *mapped
            && !ui.get_lifecycle_active()
            && ui.get_selected_index() == CORNER_ROW
            && ui.get_game_nds() =>
        {
            step_nds_corner(ui, if event.action == "menu_left" { -1 } else { 1 });
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

    let conn = Connection::connect_to_env()?;

    let software = std::env::var("NUUBOS_UI_RENDERER").map(|v| v == "software").unwrap_or(false);
    let egl = if software {
        None
    } else {
        match gpu::Egl::new(&conn) {
            Ok(egl) => Some(egl),
            Err(error) => {
                eprintln!("quick-menu: EGL unavailable={}, using software", error);
                None
            }
        }
    };
    let render_window = UiWindow::new(egl.as_ref());
    let card_render_window = UiWindow::new(egl.as_ref());
    let hud_render_window = UiWindow::new(egl.as_ref());
    eprintln!(
        "quick-menu: renderer={}",
        if matches!(render_window, UiWindow::Gpu(_)) { "gpu" } else { "software" }
    );

    platform::set_platform(Box::new(QuickPlatform {
        windows: RefCell::new(VecDeque::from([
            render_window.adapter(),
            card_render_window.adapter(),
            hud_render_window.adapter(),
        ])),
        started: Instant::now(),
    }))?;

    let ui = QuickMenuWindow::new()?;
    let card = NotificationWindow::new()?;
    let hud_card = StreamHudWindow::new()?;
    refresh_theme(&ui);

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

    /* The stream overlay likewise. */
    let mut hud_queue = conn.new_event_queue::<WaylandState>();
    let hud_qh = hud_queue.handle();
    let mut hud_state = WaylandState::new(false, "hud");
    hud_state.slint_window = Some(hud_render_window);
    conn.display().get_registry(&hud_qh, ());
    hud_queue.roundtrip(&mut hud_state)?;

    if !state.globals_ready() || !notify_state.globals_ready() || !hud_state.globals_ready() {
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
    start_audio_subscription(tx.clone());
    start_game_subscription(tx.clone());
    start_stream_subscription(tx.clone());
    start_port_subscription(tx.clone());
    start_stream_stats_subscription(tx.clone());
    start_web_subscription(tx.clone());
    start_term_subscription(tx.clone());
    start_media_subscription(tx.clone());
    start_cast_subscription(tx.clone());
    let mut keyboard = WebKeyboard { ime: ime::start(tx.clone()), ..WebKeyboard::default() };
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
    let mut hud = StreamHud {
        card: hud_card,
        state: hud_state,
        queue: hud_queue,
        qh: hud_qh,
        mapped: false,
        last: None,
    };
    let mut stream = StreamState::default();
    let mut perf = PerfOverlay::default();
    /* Last game snapshot and the overlay line built from it. */
    let mut game = GameSnapshot::default();
    /* Closing screen data of the running game (fetched once per game). */
    let mut closing_info: Option<ClosingInfo> = None;
    let mut closing_game = String::new();
    let mut perf_line = String::new();
    let mut animation_deadline: Option<Instant> = None;
    let mut pending_sleep = false;
    let mut brightness_dirty = false;
    /* Press Power to Sleep: when a held power key opens the Power OSD. */
    let mut power_hold: Option<Instant> = None;

    loop {
        platform::update_timers_and_animations();
        let wake = [osd_deadline, notifier.toasts.deadline, animation_deadline, power_hold]
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
        let mut hud_dirty = false;

        /* Press Power to Sleep (user request 2026-10-26): the press only
         * starts the hold; a release before POWER_HOLD_MS sleeps, the hold
         * opens the Power OSD through the ordinary power press below. With
         * the switch off, in Media Screen Off, during the lifecycle curtain
         * or the closing screen and with the Power OSD open (power closes
         * it) the key works as before. */
        let mut power_sleep = false;
        let event = match event {
            Some(AppEvent::Input(input)) if input.action == "power" => {
                if input.pressed {
                    let plain = fs::metadata(SCREEN_OFF_FILE).is_ok()
                        || ui.get_lifecycle_active()
                        || ui.get_closing_active()
                        || (mapped && ui.get_power_osd_open())
                        || !power_key_sleeps();
                    if plain {
                        Some(AppEvent::Input(input))
                    } else {
                        power_hold = Some(Instant::now() + Duration::from_millis(POWER_HOLD_MS));
                        None
                    }
                } else if power_hold.take().is_some() {
                    power_sleep = true;
                    None
                } else {
                    Some(AppEvent::Input(input))
                }
            }
            None if power_hold.is_some_and(|deadline| Instant::now() >= deadline) => {
                power_hold = None;
                Some(AppEvent::Input(LogicalEvent { action: "power".into(), pressed: true }))
            }
            other => other,
        };

        if power_sleep {
            /* As the Quick Menu's Sleep row: every surface down first,
             * then the canonical suspend helper; the key is already up. */
            append_action_log("action=sleep phase=power-key-release");
            if keyboard.visible {
                hide_keyboard(&ui, &mut keyboard);
            }
            if passive.is_some() {
                unmap_passive(&ui, &mut queue, &mut state, &conn);
                passive = None;
                osd_deadline = None;
            }
            if mapped {
                let _ = flush_brightness(&ui, &mut brightness_dirty);
                unmap_overlay(&ui, &mut queue, &mut state, &conn);
                mapped = false;
            }
            pending_sleep = false;
            notifier.clear(&ui, &conn);
            if let Err(error) = run_suspend_sync() {
                eprintln!("quick-menu: power key sleep failed={}", error);
            }
            if notifier.toasts.resume() {
                notifier.apply(&ui);
                notifier.sync(&conn, ui.get_lifecycle_active());
            }
        }

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
                        match volume_key(&ui, event.action == "volume_up") {
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
                    } else if keyboard.visible && !mapped {
                        /* Web Mode keyboard: it owns the controller; the
                         * Quick Menu (or the power key) replaces it. */
                        if event.pressed && (event.action == "quick_menu" || event.action == "power") {
                            hide_keyboard(&ui, &mut keyboard);
                            unmap_passive(&ui, &mut queue, &mut state, &conn);
                            passive = None;
                            osd_deadline = None;
                            if event.action == "power" {
                                open_power_osd(&ui, &mut mapped, &mut queue, &mut state, &qh, &conn)?;
                            } else {
                                map_overlay(&ui, &mut queue, &mut state, &qh, &conn)?;
                                mapped = true;
                            }
                        } else if event.pressed {
                            play_ui_sound(event.action.as_str());
                            keyboard_action(&ui, &mut keyboard, &event.action);
                            dirty = true;
                        }
                    } else if event.pressed
                        && (event.action == "quick_menu" || event.action == "power")
                        && passive.is_some()
                        && !mapped
                        && !ui.get_lifecycle_active()
                        && !ui.get_closing_active()
                    {
                        /* Hotkey can promote a transient volume OSD surface into the
                         * real Quick Menu.  Destroy the non-interactive surface first
                         * so normal mapped markers/capture are established exactly once.
                         * The notification surface is not involved. */
                        unmap_passive(&ui, &mut queue, &mut state, &conn);
                        passive = None;
                        osd_deadline = None;
                        if event.action == "power" {
                            open_power_osd(&ui, &mut mapped, &mut queue, &mut state, &qh, &conn)?;
                        } else {
                            map_overlay(&ui, &mut queue, &mut state, &qh, &conn)?;
                            mapped = true;
                        }
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
                AppEvent::Media(media) => {
                    apply_media_state(&ui, &media);
                    if mapped {
                        dirty = true;
                    }
                }
                AppEvent::Cast(cast) => {
                    apply_cast_state(&ui, cast);
                    if mapped {
                        dirty = true;
                    }
                }
                AppEvent::Web(running, zoom) => {
                    apply_web_state(&ui, running, zoom);
                    if !running && keyboard.visible && !keyboard.terminal {
                        hide_keyboard(&ui, &mut keyboard);
                    }
                    if mapped && !running {
                        refresh_switch_user(&ui);
                    }
                    dirty = true;
                }
                AppEvent::Terminal(running, on) => {
                    apply_term_state(&ui, running, on);
                    if !running {
                        keyboard.terminal = false;
                    }
                    if mapped && !running {
                        refresh_switch_user(&ui);
                    }
                    dirty = true;
                }
                AppEvent::Ime(field) => {
                    if field.generation != keyboard.field.generation {
                        keyboard.dismissed = false;
                    }
                    let active = field.active;
                    keyboard.field = field;
                    ui.set_web_ime_active(active);
                    if ui.get_term_section_visible() {
                        if !active {
                            hide_keyboard(&ui, &mut keyboard);
                        }
                    } else if !active {
                        hide_keyboard(&ui, &mut keyboard);
                    } else if keyboard.visible {
                        apply_keyboard(&ui, &keyboard);
                    } else if ui.get_web_section_visible() && !mapped && !keyboard.dismissed
                        && !ui.get_lifecycle_active()
                    {
                        show_keyboard(&ui, &mut keyboard);
                    }
                    dirty = true;
                }
                AppEvent::Port(port) => {
                    apply_port_running(&ui, port);
                    if mapped {
                        if !port.running {
                            refresh_switch_user(&ui);
                        }
                        dirty = true;
                    }
                }
                AppEvent::Stream(state) => {
                    let running = state.running;
                    apply_stream_running(&ui, state);
                    if !running {
                        hud.last = None;
                        hud.card.set_line_one("".into());
                        hud.card.set_line_two("".into());
                    }
                    stream = state;
                    if mapped {
                        if !running {
                            refresh_switch_user(&ui);
                        }
                        dirty = true;
                    }
                }
                AppEvent::GameInfo(info) => {
                    if info.game == closing_game {
                        if ui.get_closing_active() {
                            apply_closing_info(&ui, Some(&info));
                            dirty = true;
                        }
                        closing_info = Some(info);
                    }
                }
                AppEvent::StreamStats(fields) => {
                    hud.sample(&ui, &fields);
                    hud_dirty = true;
                }
                AppEvent::Game(snapshot) => {
                    apply_game_snapshot(&ui, &snapshot);
                    if snapshot.running && !snapshot.game.is_empty() && snapshot.game != closing_game {
                        closing_game = snapshot.game.clone();
                        closing_info = None;
                        fetch_closing_info(tx.clone(), closing_game.clone());
                    }
                    if snapshot.quitting != ui.get_closing_active() {
                        if snapshot.quitting {
                            apply_closing_info(&ui, closing_info.as_ref());
                        } else {
                            apply_closing_info(&ui, None);
                        }
                        ui.set_closing_active(snapshot.quitting);
                        dirty = true;
                    }
                    if !snapshot.running && !snapshot.quitting {
                        /* Session over: drop the decoded cover. */
                        closing_info = None;
                        closing_game.clear();
                    }
                    perf_line = if snapshot.running && snapshot.overlay && !snapshot.paused && !mapped {
                        perf_text(&ui, &snapshot)
                    } else {
                        String::new()
                    };
                    game = snapshot.clone();
                    if mapped {
                        if !snapshot.running {
                            refresh_switch_user(&ui);
                        }
                        dirty = true;
                    }
                }
                AppEvent::DeviceVolume => {
                    /* Same feedback as the physical volume keys. */
                    if !ui.get_lifecycle_active() {
                        apply_audio_snapshot(&ui, &read_audio_snapshot());
                        ui.set_volume_osd_visible(true);
                        osd_deadline = Some(
                            Instant::now() + Duration::from_millis(VOLUME_OSD_TIMEOUT_MS),
                        );
                        dirty = true;
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
                    let hidden = ui.get_lifecycle_active() || operation_screen();
                    /* A card shown when the operation screen opened goes. */
                    if hidden && notifier.mapped {
                        toast_dirty = true;
                    }
                    if !stale && !hidden {
                        let changed = if notification.verb == NotifyVerb::Dismiss {
                            notifier.toasts.dismiss(&notification.id)
                        } else {
                            refresh_i18n(&ui);
                            match toast_view(&ui, &notification) {
                                Some(view) => {
                                    /* Warnings/errors and screenshots have a cue
                                     * (Notification Sounds); ordinary cards are
                                     * silent (user request 2026-10-10: the chime
                                     * was annoying), progress updates too. */
                                    if notification.verb == NotifyVerb::Post && !view.live {
                                        if notification.event == "game.screenshot.saved" {
                                            play_named_sound("screenshot");
                                        } else if view.severity >= SEVERITY_WARNING {
                                            play_named_sound("error");
                                        }
                                    }
                                    notifier
                                        .toasts
                                        .post(view, notification.verb == NotifyVerb::Update)
                                }
                                None => false,
                            }
                        };
                        if changed {
                            notifier.apply(&ui);
                            toast_dirty = true;
                        }
                    } else if notification.verb == NotifyVerb::Dismiss
                        || !notification.fields.contains_key("progress")
                    {
                        if notifier.toasts.finish_hidden(&notification.id) {
                            notifier.apply(&ui);
                            toast_dirty = true;
                        }
                    }
                    apply_task_section(&ui, &notifier.toasts);
                    if mapped {
                        dirty = true;
                    }
                }
            }
        }

        /* Terminal keyboard: shown and hidden with termd's keyboard=. */
        if sync_terminal_keyboard(&ui, &mut keyboard, mapped) {
            dirty = true;
        }

        /* Keyboard row of the menu: shown once the menu has closed. */
        if !mapped && KEYBOARD_REQUESTED.swap(false, std::sync::atomic::Ordering::SeqCst)
            && ui.get_web_ime_active() && !keyboard.visible
        {
            show_keyboard(&ui, &mut keyboard);
            dirty = true;
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
            notifier.sync(&conn, ui.get_lifecycle_active() || operation_screen());
        }

        /* Stream performance overlay, same rule. */
        hud.sync(
            &conn,
            stream.running && !stream.steamlink && stream.overlay && !mapped
                && !ui.get_lifecycle_active() && !operation_screen(),
            hud_dirty,
        );

        /* Performance overlay: over a running game, never over the menu
         * or the lifecycle curtain. */
        perf.sync(
            game.running,
            game.overlay && !game.paused && !mapped && !ui.get_lifecycle_active(),
            &perf_line,
        );

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
    if hud.mapped {
        hud.unmap(&conn);
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
