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
 * has as core but MonoGame's DESKTOPGL build still looks up by name.
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

static void *trace_getproc(const char *name)
{
	void *p = real_getproc(name);

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
