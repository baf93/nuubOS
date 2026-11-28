/*
 * SPDX-License-Identifier: GPL-3.0+
 *
 * Compatibility layer for Mesa 26 in the port sandbox. Loaded in every game
 * session by bash-env.sh (never in PortMaster's own UI).
 *
 * Always loaded, but only activated for mono/MonoGame processes: use
 * RTLD_DEEPBIND when Mesa's libGLESv2/libLLVM/libgallium are dlopen()ed.
 * mono-sgen ships a static LLVM and exports the same llvm::RegisterPassParser
 * symbols; without deep binding the loader resolves libLLVM's references to
 * mono's uninitialized data and crashes on the first shader compile. Applying
 * deep binding to other games breaks Mesa's own cross-library resolution.
 *
 * For MonoGame processes only (mono executable or MONOGAME_PATCH set): also
 * bypass gl4es and advertise the desktop-GL framebuffer extensions that GLES3
 * has as core but MonoGame's DESKTOPGL build still looks up by name. Its
 * glGetTexImage (used by Texture2D.GetData to recolour textures at runtime)
 * is emulated with an FBO readback for RGBA8/level-0 2D textures.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void *(*dlopen_fn)(const char *, int);
typedef void *(*dlsym_fn)(void *, const char *);
typedef void *(*getproc_fn)(const char *);
typedef char *(*getenv_fn)(const char *);
typedef const unsigned char *(*getstring_fn)(unsigned int);
typedef void (*getintegerv_fn)(unsigned int, int *);

static dlsym_fn real_dlsym;
static getenv_fn real_getenv;
static getproc_fn real_getproc;
static getstring_fn real_getstring;
static getintegerv_fn real_getintegerv;
static char exts[8192];
static int monogame;

static int is_mono_process(void)
{
	char path[256], *base;
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);

	if (n <= 0)
		return 0;
	path[n] = '\0';
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	return strncmp(base, "mono", 4) == 0;
}

__attribute__((constructor)) static void init(void)
{
	real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
	real_getenv = (getenv_fn)real_dlsym(RTLD_NEXT, "getenv");
	if (real_getenv && (real_getenv("MONOGAME_PATCH") || is_mono_process()))
		monogame = 1;
}

static const unsigned char *trace_getstring(unsigned int name)
{
	const unsigned char *s = real_getstring(name);

	if (name == 0x1F03 && s) {
		snprintf(exts, sizeof(exts),
			 "%s GL_ARB_framebuffer_object GL_EXT_framebuffer_object "
			 "GL_EXT_framebuffer_blit GL_ARB_draw_buffers "
			 "GL_ARB_texture_non_power_of_two",
			 (const char *)s);
		return (const unsigned char *)exts;
	}
	return s;
}

static void trace_getintegerv(unsigned int pname, int *data)
{
	real_getintegerv(pname, data);
	if (!data)
		return;
	if (pname == 0x8869 && *data < 16)
		*data = 16;
	if (pname == 0x8872 && *data < 32)
		*data = 32;
	if (pname == 0x8B4D && *data < 96)
		*data = 96;
}

/* glGetTexImage is not in GLES: MonoGame's DESKTOPGL build calls it from
 * Texture2D.GetData(). Emulate it for the RGBA8/level-0 case Stardew uses to
 * recolour the farmer at runtime. The FBO size is learned from the texture
 * specification calls; other formats/levels fall through to the real proc. */
#define MAXTEX 65536
static struct { unsigned short w, h; } texsz[MAXTEX];
static unsigned bound2d;

static void (*real_bindtex)(unsigned, unsigned);
static void (*real_teximage2d)(unsigned, int, int, int, int, int,
			       unsigned, unsigned, const void *);
static void (*real_texstorage2d)(unsigned, int, unsigned, int, int);
static void (*real_gettex)(unsigned, int, unsigned, unsigned, void *);

static void (*gl_genfb)(int, unsigned *);
static void (*gl_delfb)(int, const unsigned *);
static void (*gl_bindfb)(unsigned, unsigned);
static void (*gl_fbtex2d)(unsigned, unsigned, unsigned, unsigned, int);
static void (*gl_readpix)(int, int, int, int, unsigned, unsigned, void *);
static void (*gl_getint)(unsigned, int *);

static void load_gl(void)
{
	if (gl_genfb)
		return;
	gl_genfb   = (void (*)(int, unsigned *))real_getproc("glGenFramebuffers");
	gl_delfb   = (void (*)(int, const unsigned *))real_getproc("glDeleteFramebuffers");
	gl_bindfb  = (void (*)(unsigned, unsigned))real_getproc("glBindFramebuffer");
	gl_fbtex2d = (void (*)(unsigned, unsigned, unsigned, unsigned, int))real_getproc("glFramebufferTexture2D");
	gl_readpix = (void (*)(int, int, int, int, unsigned, unsigned, void *))real_getproc("glReadPixels");
	gl_getint  = (void (*)(unsigned, int *))real_getproc("glGetIntegerv");
}

static void hook_bindtex(unsigned target, unsigned id)
{
	if (target == 0x0DE1)		/* GL_TEXTURE_2D */
		bound2d = id;
	real_bindtex(target, id);
}

static void hook_teximage2d(unsigned t, int lvl, int ifmt, int w, int h,
			    int border, unsigned fmt, unsigned type,
			    const void *px)
{
	if (t == 0x0DE1 && lvl == 0 && bound2d < MAXTEX) {
		texsz[bound2d].w = w;
		texsz[bound2d].h = h;
	}
	real_teximage2d(t, lvl, ifmt, w, h, border, fmt, type, px);
}

static void hook_texstorage2d(unsigned t, int levels, unsigned ifmt,
			      int w, int h)
{
	if (t == 0x0DE1 && bound2d < MAXTEX) {
		texsz[bound2d].w = w;
		texsz[bound2d].h = h;
	}
	real_texstorage2d(t, levels, ifmt, w, h);
}

/* Limits: RGBA8/UNSIGNED_BYTE/level 0 only; the bound texture is not tracked
 * per texture unit; sizes come only from glTexImage2D/glTexStorage2D; texsz
 * is not cleared on glDeleteTextures (overwritten on reuse); ids >= MAXTEX
 * are not tracked. Any other case calls the original (likely a no-op). */
static void hook_gettex(unsigned t, int lvl, unsigned fmt, unsigned type,
			void *px)
{
	unsigned fbo;
	int prev = 0;

	load_gl();
	if (t != 0x0DE1 || lvl != 0 || bound2d >= MAXTEX ||
	    !texsz[bound2d].w || fmt != 0x1908 /* GL_RGBA */ ||
	    type != 0x1401 /* GL_UNSIGNED_BYTE */ ||
	    !gl_genfb || !gl_delfb || !gl_bindfb || !gl_fbtex2d ||
	    !gl_readpix || !gl_getint) {
		if (real_gettex)
			real_gettex(t, lvl, fmt, type, px);
		return;
	}
	gl_getint(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &prev);
	gl_genfb(1, &fbo);
	gl_bindfb(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);
	gl_fbtex2d(0x8CA8, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, 0x0DE1,
		   bound2d, 0);
	gl_readpix(0, 0, texsz[bound2d].w, texsz[bound2d].h,
		   0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
	gl_bindfb(0x8CA8, (unsigned)prev);
	gl_delfb(1, &fbo);
}

static void *trace_getproc(const char *name)
{
	void *p = real_getproc(name);

	/* These must be installed even when SDL/GLES returns NULL for the
	 * desktop-GL entry point, hence before the early return below. */
	if (strcmp(name, "glBindTexture") == 0 && p) {
		real_bindtex = (void (*)(unsigned, unsigned))p;
		return (void *)hook_bindtex;
	}
	if (strcmp(name, "glTexImage2D") == 0 && p) {
		real_teximage2d = (void (*)(unsigned, int, int, int, int, int,
					    unsigned, unsigned,
					    const void *))p;
		return (void *)hook_teximage2d;
	}
	if (strcmp(name, "glTexStorage2D") == 0 && p) {
		real_texstorage2d =
			(void (*)(unsigned, int, unsigned, int, int))p;
		return (void *)hook_texstorage2d;
	}
	if (strcmp(name, "glGetTexImage") == 0) {
		real_gettex = (void (*)(unsigned, int, unsigned, unsigned,
					void *))p;	/* may be NULL */
		return (void *)hook_gettex;
	}
	if (!p)
		return p;
	if (strcmp(name, "glGetString") == 0) {
		real_getstring = (getstring_fn)p;
		return (void *)trace_getstring;
	}
	if (strcmp(name, "glGetIntegerv") == 0) {
		real_getintegerv = (getintegerv_fn)p;
		return (void *)trace_getintegerv;
	}
	return p;
}

void *dlopen(const char *file, int flags)
{
	static dlopen_fn real;

	if (!real)
		real = (dlopen_fn)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34");
	if (file && monogame) {
		/* libgl_default.txt sets SDL_VIDEO_*_DRIVER to absolute paths
		 * inside gl4es/. Redirect the loader instead of overriding
		 * getenv(): interposing getenv broke non-mono ports. */
		if (strstr(file, "gl4es") && strstr(file, "libEGL"))
			file = "/usr/lib/libEGL.so.1";
		else if (strstr(file, "gl4es") && strstr(file, "libGL.so.1"))
			file = "/usr/lib/libGLESv2.so.2";
		else if (!strcmp(file, "libEGL.so.1"))
			file = "/usr/lib/libEGL.so.1";
		else if (!strcmp(file, "libGLESv2.so.2"))
			file = "/usr/lib/libGLESv2.so.2";
		/* Deep binding is only safe when the process really has a second
		 * LLVM (mono). Elsewhere it breaks Mesa's own cross-library
		 * symbol resolution (Dusklight segfaulted). */
		if (strstr(file, "libGLESv2") || strstr(file, "libLLVM") ||
		    strstr(file, "libgallium"))
			flags |= RTLD_DEEPBIND;
	}
	return real(file, flags);
}

void *dlsym(void *handle, const char *name)
{
	void *p = real_dlsym(handle, name);

	if (monogame && p && strcmp(name, "SDL_GL_GetProcAddress") == 0) {
		real_getproc = (getproc_fn)p;
		return (void *)trace_getproc;
	}
	return p;
}
