/*
 * GPU rendering for the Quick Menu layer surfaces.
 *
 * Each Slint window (menu, notification card) owns a FemtoVG renderer on its
 * own OpenGL ES context (Mesa/Panfrost through EGL). Frames reach labwc as
 * dmabufs via wl_egl_window, so the compositor imports them instead of
 * copying wl_shm memory, and nothing is rasterized on the CPU.
 *
 * Layer surfaces come and go with the menu, the volume OSD and
 * notifications; the GL context and FemtoVG canvas (glyph atlas, textures)
 * persist across them. While no surface exists the context is current
 * without a surface (EGL_KHR_surfaceless_context).
 */

use std::cell::{Cell, RefCell};
use std::ffi::{c_void, CStr};
use std::num::NonZeroU32;
use std::ptr::NonNull;
use std::rc::{Rc, Weak};

use glutin::api::egl::{
    config::Config, context::PossiblyCurrentContext, display::Display, surface::Surface,
};
use glutin::config::{ConfigSurfaceTypes, ConfigTemplateBuilder, GlConfig};
use glutin::context::{ContextApi, ContextAttributesBuilder, Version};
use glutin::prelude::*;
use glutin::surface::{SurfaceAttributesBuilder, SwapInterval, WindowSurface};
use raw_window_handle::{
    RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle, WaylandWindowHandle,
};
use slint::platform::femtovg_renderer::{FemtoVGRenderer, OpenGLInterface};
use slint::platform::{Renderer, WindowAdapter, WindowEvent};
use slint::{PhysicalSize, Window, WindowSize};
use wayland_client::{protocol::wl_surface::WlSurface, Connection, Proxy};

type BoxError = Box<dyn std::error::Error + Send + Sync>;

/* One EGL display for the process' Wayland connection. */
pub struct Egl {
    display: Display,
    config: Config,
}

impl Egl {
    pub fn new(conn: &Connection) -> Result<Rc<Self>, BoxError> {
        let display_ptr = NonNull::new(conn.backend().display_ptr() as *mut c_void)
            .ok_or("no wl_display pointer")?;
        let raw = RawDisplayHandle::Wayland(WaylandDisplayHandle::new(display_ptr));
        let display = unsafe { Display::new(raw)? };

        /* Premultiplied ARGB: the overlays are translucent. */
        let template = ConfigTemplateBuilder::new()
            .with_alpha_size(8)
            .with_surface_type(ConfigSurfaceTypes::WINDOW)
            .with_api(glutin::config::Api::GLES2)
            .build();
        let config = unsafe { display.find_configs(template)? }
            .filter(|c| c.alpha_size() == 8)
            .min_by_key(|c| c.num_samples())
            .ok_or("no EGL config with alpha")?;

        Ok(Rc::new(Self { display, config }))
    }
}

/* The GL context of one window and the EGL surface of its current layer
 * surface, if any. */
struct GlTarget {
    egl: Rc<Egl>,
    context: PossiblyCurrentContext,
    surface: RefCell<Option<Surface<WindowSurface>>>,
    surface_size: Cell<(u32, u32)>,
}

impl GlTarget {
    fn make_current(&self) -> Result<(), BoxError> {
        match self.surface.borrow().as_ref() {
            Some(surface) => self.context.make_current(surface)?,
            None => self.context.make_current_surfaceless()?,
        }
        Ok(())
    }
}

struct GlHandle(Rc<GlTarget>);

unsafe impl OpenGLInterface for GlHandle {
    fn ensure_current(&self) -> Result<(), BoxError> {
        if !self.0.context.is_current() {
            self.0.make_current()?;
        }
        Ok(())
    }

    fn swap_buffers(&self) -> Result<(), BoxError> {
        if let Some(surface) = self.0.surface.borrow().as_ref() {
            surface.swap_buffers(&self.0.context)?;
        }
        Ok(())
    }

    fn resize(&self, width: NonZeroU32, height: NonZeroU32) -> Result<(), BoxError> {
        if let Some(surface) = self.0.surface.borrow().as_ref() {
            surface.resize(&self.0.context, width, height);
            self.0.surface_size.set((width.get(), height.get()));
        }
        Ok(())
    }

    fn get_proc_address(&self, name: &CStr) -> *const c_void {
        self.0.egl.display.get_proc_address(name)
    }
}

/* Slint window adapter rendering with FemtoVG into an EGL window surface. */
pub struct GpuWindow {
    window: Window,
    renderer: FemtoVGRenderer,
    target: Rc<GlTarget>,
    needs_redraw: Cell<bool>,
    size: Cell<PhysicalSize>,
}

impl GpuWindow {
    pub fn new(egl: &Rc<Egl>) -> Result<Rc<Self>, BoxError> {
        let attributes = ContextAttributesBuilder::new()
            .with_context_api(ContextApi::Gles(Some(Version::new(2, 0))))
            .build(None);
        let context = unsafe { egl.display.create_context(&egl.config, &attributes)? }
            .make_current_surfaceless()?;
        let target = Rc::new(GlTarget {
            egl: egl.clone(),
            context,
            surface: RefCell::new(None),
            surface_size: Cell::new((0, 0)),
        });
        let renderer = FemtoVGRenderer::new(GlHandle(target.clone()))?;
        Ok(Rc::new_cyclic(|weak: &Weak<Self>| Self {
            window: Window::new(weak.clone()),
            renderer,
            target,
            needs_redraw: Cell::new(false),
            size: Cell::new(PhysicalSize::default()),
        }))
    }

    /* Bind the window to a (new) layer surface. */
    fn attach(&self, surface: &WlSurface, width: u32, height: u32) -> Result<(), BoxError> {
        let surface_ptr = NonNull::new(surface.id().as_ptr() as *mut c_void)
            .ok_or("no wl_surface pointer")?;
        let raw = RawWindowHandle::Wayland(WaylandWindowHandle::new(surface_ptr));
        let attributes = SurfaceAttributesBuilder::<WindowSurface>::new().build(
            raw,
            NonZeroU32::new(width).ok_or("zero width")?,
            NonZeroU32::new(height).ok_or("zero height")?,
        );
        let egl_surface =
            unsafe { self.target.egl.display.create_window_surface(&self.target.egl.config, &attributes)? };
        self.target.context.make_current(&egl_surface)?;
        /* The Quick Menu paces its own frames: never block on frame callbacks. */
        egl_surface.set_swap_interval(&self.target.context, SwapInterval::DontWait)?;
        *self.target.surface.borrow_mut() = Some(egl_surface);
        self.target.surface_size.set((width, height));
        Ok(())
    }

    /* Release the EGL surface (and its wl_egl_window) before the wl_surface
     * it wraps is destroyed. */
    pub fn detach(&self) {
        if self.target.surface.borrow_mut().take().is_some() {
            let _ = self.target.context.make_current_surfaceless();
        }
    }

    pub fn attached(&self) -> bool {
        self.target.surface.borrow().is_some()
    }

    /* Render one frame into `surface` and present it (eglSwapBuffers attaches
     * the dmabuf, damages the whole surface and commits). */
    pub fn render(&self, surface: &WlSurface, width: u32, height: u32) -> Result<(), BoxError> {
        if !self.attached() {
            self.attach(surface, width, height)?;
        } else if self.target.surface_size.get() != (width, height) {
            if let (Some(w), Some(h)) = (NonZeroU32::new(width), NonZeroU32::new(height)) {
                GlHandle(self.target.clone()).resize(w, h)?;
            }
        }
        self.needs_redraw.set(false);
        self.renderer.render()?;
        Ok(())
    }
}

impl WindowAdapter for GpuWindow {
    fn window(&self) -> &Window {
        &self.window
    }

    fn renderer(&self) -> &dyn Renderer {
        &self.renderer
    }

    fn size(&self) -> PhysicalSize {
        self.size.get()
    }

    fn set_size(&self, size: WindowSize) {
        let scale = self.window.scale_factor();
        self.size.set(size.to_physical(scale));
        self.window.dispatch_event(WindowEvent::Resized { size: size.to_logical(scale) });
    }

    fn request_redraw(&self) {
        self.needs_redraw.set(true);
    }
}
