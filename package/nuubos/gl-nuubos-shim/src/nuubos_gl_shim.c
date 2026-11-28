/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * nuubOS thin shim: Desktop OpenGL -> OpenGL ES 3.x interposer.
 *
 *   LD_PRELOAD=libgl_nuubos_shim.so <game>
 *
 * PortMaster titles and MonoGame ports call desktop GL (GL 1.x/2.1 entry
 * points, fixed-function state and GLSL 1.10), while the H700 only offers
 * Mesa GLES 3.1 on Panfrost. This library sits between them: it intercepts
 * GL/EGL entry points, rewrites shaders and draws, and emulates the legacy
 * state that GLES removed. Programs already using GLES are left alone.
 *
 * Layout of the implementation (spec v2.1 section numbering):
 *   Module A  identity strings and advertised versions/extensions
 *   Module B  double -> float at API boundaries and in uniforms
 *   Module C  default-framebuffer / draw-buffer emulation
 *   Module D  glMapBuffer -> glMapBufferRange
 *   Module E  textures: 1D, BGRA swizzle, readback and CPU DXT decode
 *   Module F  legacy primitives (quads) and draw conversion
 *   Module G  shader-token rewriting; G.2 fixed-function via UBO + shaders
 *   Module H  reconstructed legacy state: matrix stacks, attrib stacks,
 *             immediate mode and display lists
 *   G.3       opt-in geometry/tessellation emulation via compute + transform
 *             feedback; only a closed subset is supported
 *
 * Design rules: one source compiled for both aarch64 and armhf, no GL/EGL
 * headers (the ABI types and enum values are declared locally), no absolute
 * library paths, and every real driver symbol resolved lazily so the shim can
 * be preloaded into any process. Per-context state lives in ShimCtx, indexed
 * by EGLContext and cached in TLS; transient conversion buffers are also TLS
 * so no lock is taken on the hot path.
 *
 * Environment: NUUBOS_SHIM_DEBUG, NUUBOS_SHIM_DESKTOP_GL=0|1,
 * NUUBOS_SHIM_GL_VERSION, NUUBOS_SHIM_BGRA_MODE=ext,
 * NUUBOS_SHIM_FF_SHADERS=0, NUUBOS_SHIM_GEOM_EMU=1,
 * NUUBOS_SHIM_TESS_EMU=1, NUUBOS_SHIM_GEOM_CAP_MB, NUUBOS_SHIM_TESS_MAX,
 * NUUBOS_ALLOW_UNCAP and MONOGAME_PATCH. Desktop identity wrappers are
 * opt-in; G.3 requires both its opt-in flag and compute/TF/SSBO support.
 *
 * Single-source implementation of
 * docs/dev/especificacion_thin_shim_gl_nuubos_v2_1.txt; the follow-up notes
 * are in docs/dev/thin-shim-gl-implementacion.md.
 */

#define _GNU_SOURCE 1
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>

/* ========================================================================== */
/* Section 0: local types and constants (Appendix A, reconstructed locally)   */
/* ========================================================================== */

typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef ptrdiff_t      GLsizeiptr;
typedef ptrdiff_t      GLintptr;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef float          GLfloat;
typedef double         GLdouble;
typedef float          GLclampf;
typedef char           GLchar;
typedef unsigned char  GLubyte;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef unsigned short GLushort;
typedef unsigned short GLhalf;
typedef int64_t        GLint64;
typedef uint64_t       GLuint64;
typedef struct __GLsync *GLsync;
typedef void*          EGLDisplay;
typedef void*          EGLContext;
typedef void*          EGLSurface;
typedef void*          EGLConfig;
typedef int32_t        EGLint;
typedef unsigned int   EGLBoolean;
typedef unsigned int   EGLenum;

typedef void (*shim_glfn)(void);

/* GL core enums used by the shim. Values are part of the GL ABI, so they
   are declared locally instead of including a GL header: the same source is
   built for two architectures and must not depend on headers installed for
   only one of them (spec Appendix A). Only constants referenced by the
   interposer are listed; unknown values are passed through to the driver. */
enum {
    GL_FALSE = 0, GL_TRUE = 1,
    GL_POINTS = 0x0000, GL_LINES = 0x0001, GL_LINE_LOOP = 0x0002,
    GL_LINE_STRIP = 0x0003, GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005, GL_TRIANGLE_FAN = 0x0006,
    GL_QUADS = 0x0007, GL_QUAD_STRIP = 0x0008, GL_POLYGON = 0x0009,
    GL_PATCHES = 0x000E,
    GL_NONE = 0x0000,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02,
    GL_EXTENSIONS = 0x1F03,
    GL_CURRENT_COLOR = 0x0B00, GL_CURRENT_NORMAL = 0x0B02,
    GL_CURRENT_TEXTURE_COORDS = 0x0B03, GL_POINT_SIZE = 0x0B11,
    GL_MATRIX_MODE = 0x0BA0, GL_MODELVIEW_MATRIX = 0x0BA6,
    GL_PROJECTION_MATRIX = 0x0BA7, GL_TEXTURE_MATRIX = 0x0BA8,
    GL_LIGHTING = 0x0B50, GL_COLOR_MATERIAL = 0x0B57,
    GL_FOG = 0x0B60, GL_FOG_MODE = 0x0B65, GL_FOG_DENSITY = 0x0B62,
    GL_FOG_START = 0x0B63, GL_FOG_END = 0x0B64, GL_FOG_COLOR = 0x0B66,
    GL_LIGHT_MODEL_AMBIENT = 0x0B53, GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0B51,
    GL_LIGHT_MODEL_TWO_SIDE = 0x0B52,
    GL_TEXTURE_1D = 0x0DE0, GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE = 0x1702, GL_TEXTURE_ENV = 0x2300,
    GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201,
    GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_BLEND = 0xBE2,
    GL_REPLACE = 0x1E01, GL_ADD = 0x0104,
    GL_FRONT_LEFT = 0x0400, GL_FRONT_RIGHT = 0x0401,
    GL_BACK_LEFT = 0x0402, GL_BACK_RIGHT = 0x0403,
    GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_FRONT_AND_BACK = 0x0408,
    GL_LIGHT0 = 0x4000,
    GL_AMBIENT = 0x1200, GL_DIFFUSE = 0x1201, GL_SPECULAR = 0x1202,
    GL_POSITION = 0x1203, GL_SPOT_DIRECTION = 0x1204,
    GL_SPOT_EXPONENT = 0x1205, GL_SPOT_CUTOFF = 0x1206,
    GL_CONSTANT_ATTENUATION = 0x1207, GL_LINEAR_ATTENUATION = 0x1208,
    GL_QUADRATIC_ATTENUATION = 0x1209,
    GL_EMISSION = 0x1600, GL_SHININESS = 0x1601,
    GL_AMBIENT_AND_DIFFUSE = 0x1602,
    GL_CLIP_PLANE0 = 0x3000, GL_CLIP_DISTANCE0 = 0x3000,
    GL_FLAT = 0x1D00, GL_SMOOTH = 0x1D01,
    GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_EQUAL = 0x0202,
    GL_LEQUAL = 0x0203, GL_GREATER = 0x0204, GL_NOTEQUAL = 0x0205,
    GL_GEQUAL = 0x0206, GL_ALWAYS = 0x0207,
    GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71, GL_SCISSOR_TEST = 0x0C11,
    GL_STENCIL_TEST = 0x0B90, GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_TEXTURE_2D_BIT = 0x0001,
    GL_POINTS_BIT = 0x0000,
    GL_RED = 0x1903, GL_ALPHA = 0x1906, GL_RGB = 0x1907,
    GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909,
    GL_LUMINANCE_ALPHA = 0x190A, GL_DEPTH_COMPONENT = 0x1902,
    GL_BGR = 0x80E0, GL_BGRA = 0x80E1,
    GL_UNSIGNED_BYTE = 0x1401, GL_BYTE = 0x1400,
    GL_UNSIGNED_SHORT = 0x1403, GL_SHORT = 0x1402,
    GL_UNSIGNED_INT = 0x1405, GL_INT = 0x1404,
    GL_FLOAT = 0x1406, GL_DOUBLE = 0x140A, GL_HALF_FLOAT = 0x140B,
    GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367,
    GL_LUMINANCE8 = 0x8040, GL_LUMINANCE8_ALPHA8 = 0x8045,
    GL_ALPHA8 = 0x803C,
    GL_RGBA8 = 0x8058, GL_SRGB8_ALPHA8 = 0x8C43,
    GL_RGBA32F = 0x8814, GL_RGB32F = 0x8815,
    GL_RGBA16F = 0x881A, GL_RGB16F = 0x881B,
    GL_RG = 0x8227, GL_RG32F = 0x8230, GL_RG16F = 0x822F,
    GL_R32F = 0x822E, GL_R16F = 0x822D,
    GL_DEPTH_COMPONENT32F = 0x8CAC,
    GL_TEXTURE_BINDING_2D = 0x8069,
    GL_TEXTURE_WIDTH = 0x1000, GL_TEXTURE_HEIGHT = 0x1001,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_TEXTURE_WRAP_R = 0x8072,
    GL_CLAMP = 0x2900, GL_CLAMP_TO_EDGE = 0x812F,
    GL_CLAMP_TO_BORDER = 0x812D,
    GL_TEXTURE_LOD_BIAS = 0x8501, GL_DEPTH_TEXTURE_MODE = 0x884B,
    GL_TEXTURE_PRIORITY = 0x8066, GL_GENERATE_MIPMAP = 0x8191,
    GL_TEXTURE0 = 0x84C0, GL_ACTIVE_TEXTURE = 0x84E0,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
    GL_BUFFER_SIZE = 0x8764,
    GL_READ_ONLY = 0x88B8, GL_WRITE_ONLY = 0x88B9,
    GL_READ_WRITE = 0x88BA,
    GL_MAP_READ_BIT = 0x0001, GL_MAP_WRITE_BIT = 0x0002,
    GL_MAP_INVALIDATE_BUFFER_BIT = 0x0008,
    GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_UNPACK_BUFFER = 0x88EC,
    GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
    GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
    GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_SKIP_ROWS = 0x0CF3,
    GL_UNPACK_SKIP_PIXELS = 0x0CF4, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_SKIP_ROWS = 0x0D03,
    GL_PACK_SKIP_PIXELS = 0x0D04, GL_PACK_ALIGNMENT = 0x0D05,
    GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
    GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0,
    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2,
    GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,
    GL_COMPRESSED_SRGB_S3TC_DXT1_EXT = 0x8C4C,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT = 0x8C4D,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT = 0x8C4E,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F,
    GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501,
    GL_INVALID_OPERATION = 0x0502, GL_CONTEXT_LOST = 0x9242,
    GL_SHADER_TYPE = 0x8B4F, GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30, GL_GEOMETRY_SHADER = 0x8DD9,
    GL_TESS_CONTROL_SHADER = 0x8E88, GL_TESS_EVALUATION_SHADER = 0x8E87,
    GL_COMPUTE_SHADER = 0x91B9,
    GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_COMPILE = 0x1300, GL_COMPILE_AND_EXECUTE = 0x1301,
    GL_INFO_LOG_LENGTH = 0x8B84, GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_MAJOR_VERSION = 0x821B, GL_MINOR_VERSION = 0x821C,
    GL_NUM_EXTENSIONS = 0x821D,
    GL_MAX_VERTEX_ATTRIBS = 0x8869,
    GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872,
    GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D,
    GL_CONTEXT_PROFILE_MASK = 0x9126,
    GL_CONTEXT_COMPATIBILITY_PROFILE_BIT = 0x00000002,
    GL_UNIFORM_BUFFER = 0x8A11, GL_UNIFORM_BUFFER_BINDING = 0x8A28,
    GL_MAX_UNIFORM_BUFFER_BINDINGS = 0x8A2F,
    GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30,
    GL_INVALID_INDEX = 0xFFFFFFFF,
    GL_SHADER_STORAGE_BUFFER = 0x90D2,
    GL_SHADER_STORAGE_BUFFER_BINDING = 0x90D3,
    GL_TRANSFORM_FEEDBACK_BUFFER = 0x8C8E,
    GL_INTERLEAVED_ATTRIBS = 0x8C8C,
    GL_RASTERIZER_DISCARD = 0x8C89,
    GL_ALL_BARRIER_BITS = 0xFFFFFFFF,
    GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT = 0x00000001,
    GL_ELEMENT_ARRAY_BARRIER_BIT = 0x00000002,
    GL_SECONDARY_COLOR_ARRAY = 0x845E,
    GL_FOG_COORD_ARRAY = 0x8457,
    GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075,
    GL_COLOR_ARRAY = 0x8076, GL_INDEX_ARRAY = 0x8077,
    GL_TEXTURE_COORD_ARRAY = 0x8078, GL_EDGE_FLAG_ARRAY = 0x8079,
    GL_COLOR_MATERIAL_BIT = 0x00000200,
    GL_MAX_LIGHTS = 0x0B31, GL_MAX_TEXTURE_UNITS = 0x84E2,
    GL_MAX_TEXTURE_COORDS = 0x8871, GL_MAX_CLIP_PLANES = 0x0D32,
    GL_MAX_VARYING_VECTORS = 0x8DFC,
    GL_STREAM_DRAW = 0x88E0, GL_DYNAMIC_DRAW = 0x88E8,
    GL_STATIC_DRAW = 0x88E4,
    GL_PATCH_VERTICES = 0x8E72,
    GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS = 0x90EB,
    GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE,
    GL_NO_ERROR = 0,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_TRANSFORM_FEEDBACK_BUFFER_BINDING = 0x8C8F
};

/* EGL enums. As with the GL constants above, only the values needed to
   rewrite context/config attributes are defined here; the real EGL library
   owns the implementation and all other calls are forwarded unchanged. */
enum {
    EGL_FALSE = 0, EGL_TRUE = 1, EGL_NONE = 0x3038,
    EGL_OPENGL_ES_API = 0x30A0, EGL_OPENGL_API = 0x30A2,
    EGL_RENDERABLE_TYPE = 0x3040,
    EGL_OPENGL_BIT = 0x0008, EGL_OPENGL_ES_BIT = 0x0001,
    EGL_OPENGL_ES2_BIT = 0x0004, EGL_OPENGL_ES3_BIT = 0x0040,
    EGL_CONTEXT_CLIENT_VERSION = 0x3098,
    EGL_CONTEXT_MAJOR_VERSION = 0x3098,
    EGL_CONTEXT_MINOR_VERSION = 0x30FB,
    EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD,
    EGL_CONTEXT_OPENGL_DEBUG = 0x31B0,
    EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE = 0x31B1
};

#define EGL_NO_CONTEXT  ((EGLContext)0)
#define EGL_NO_DISPLAY  ((EGLDisplay)0)
#define EGL_NO_SURFACE  ((EGLSurface)0)

/* G.2 FF block: C mirror of the std140 UBO uploaded to generated shaders.
   Field order and explicit padding are deliberate: the static assertions
   below pin the offsets that the GLSL declarations rely on, so changing any
   member without updating both sides silently breaks fixed-function state.
   Matrices are stored column-major, matching GL's memory layout. */
typedef struct ShimFFLight {
    float ambient[4], diffuse[4], specular[4], position[4], halfVector[4];
    float spotDirection[3], spotExponent;
    float spotCutoff, spotCosCutoff, constantAttenuation, linearAttenuation;
    float quadraticAttenuation, _pad[3];
} ShimFFLight;

typedef struct ShimFFMaterial {
    float emission[4], ambient[4], diffuse[4], specular[4];
    float shininess, _pad[3];
} ShimFFMaterial;

typedef struct ShimFFLightProducts {
    float ambient[4], diffuse[4], specular[4];
} ShimFFLightProducts;

typedef struct ShimFFFog {
    float color[4];
    float density, start, end, scale;
} ShimFFFog;

typedef struct ShimFFBlock {
    float mv[16], proj[16], mvp[16];
    float mv_inv[16], proj_inv[16], mvp_inv[16];
    float mv_t[16], proj_t[16], mvp_t[16];
    float mv_it[16], proj_it[16], mvp_it[16];
    float normal_matrix[12];
    float normal_scale, _pad0[3];
    float tex_matrix[8][16];
    ShimFFLight light[8];
    ShimFFMaterial front_mat, back_mat;
    float lm_ambient[4];
    float front_scene[4], back_scene[4];
    ShimFFLightProducts front_lp[8], back_lp[8];
    ShimFFFog fog;
    float clip_plane[8][4];
    float texenv_color[8][4];
    int32_t clip_enable_mask, _pad1[3];
    /* Module H (reconstructed): legacy state that G.2 must expose to shaders.
       GLES has no glAlphaFunc/glPointSize/glTexEnv, so the generated fragment
       shader reads these vectors from the same UBO and discards/emulates
       accordingly. Keep them last so the existing offsets above stay valid. */
    float alpha_params[4];       /* func, ref, enabled, 0 */
    float point_params[4];       /* glPointSize, shade model, 0, 0 */
    float texenv_mode[8][4];     /* GL_TEXTURE_ENV_MODE per unit in [u][0] */
} ShimFFBlock;

_Static_assert(sizeof(ShimFFLight) == 128, "ShimFFLight size");
_Static_assert(sizeof(ShimFFMaterial) == 80, "ShimFFMaterial size");
_Static_assert(sizeof(ShimFFLightProducts) == 48, "ShimFFLightProducts size");
_Static_assert(sizeof(ShimFFFog) == 32, "ShimFFFog size");
_Static_assert(sizeof(ShimFFBlock) == 3808, "ShimFFBlock size");
_Static_assert(offsetof(ShimFFBlock, normal_matrix) == 768, "normal_matrix offset");
_Static_assert(offsetof(ShimFFBlock, tex_matrix) == 832, "tex_matrix offset");
_Static_assert(offsetof(ShimFFBlock, light) == 1344, "light offset");
_Static_assert(offsetof(ShimFFBlock, front_mat) == 2368, "front_mat offset");
_Static_assert(offsetof(ShimFFBlock, front_lp) == 2576, "front_lp offset");
_Static_assert(offsetof(ShimFFBlock, fog) == 3344, "fog offset");
_Static_assert(offsetof(ShimFFBlock, clip_plane) == 3376, "clip_plane offset");
_Static_assert(offsetof(ShimFFBlock, clip_enable_mask) == 3632, "clip_enable_mask offset");
_Static_assert(offsetof(ShimFFBlock, alpha_params) == 3648, "alpha_params offset");
_Static_assert(offsetof(ShimFFBlock, point_params) == 3664, "point_params offset");
_Static_assert(offsetof(ShimFFBlock, texenv_mode) == 3680, "texenv_mode offset");

#define FFD_MV        (1u << 0)
#define FFD_PROJ      (1u << 1)
#define FFD_TEX       (1u << 2)
#define FFD_LIGHT     (1u << 3)
#define FFD_MAT       (1u << 4)
#define FFD_FOG       (1u << 5)
#define FFD_CLIP      (1u << 6)
#define FFD_TEXENV    (1u << 7)
/* Dirty bits: ff_sync() only rebuilds/reuploads the affected part of the
   UBO. FFD_MISC covers alpha test, glPointSize and glShadeModel. */
#define FFD_MISC      (1u << 8)   /* alpha test, glPointSize, glShadeModel */
#define FFD_ALL       ((1u << 9) - 1u)

#define SHIM_FF_LOC_MAX 13
#define SHIM_FF_LOC(K) ((int)(ctx->features.max_vertex_attribs - 1 - (K)))

/* Legacy client array state, shared by Module H and G.2.6. This mirrors
   glVertexPointer/glColorPointer/... and the bound VBO: GLES 3 has no client
   arrays, so ff_feed_user_program() injects equivalent generic attributes
   into a private VAO just before each draw. */
typedef struct ShimArray {
    int enabled;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const void *ptr;
    GLuint vbo;
} ShimArray;

enum {
    SHIM_ARR_VERTEX = 0,
    SHIM_ARR_NORMAL = 1,
    SHIM_ARR_COLOR = 2,
    SHIM_ARR_TEX0 = 3,
    SHIM_ARR_TEX7 = 10,
    SHIM_ARR_SECONDARY = 11,
    SHIM_ARR_FOGCOORD = 12,
    SHIM_ARR_COUNT = 13
};

typedef struct ShimCtxFeatures {
    int ready;
    int es_major, es_minor;
    int has_bgra8888, has_s3tc, has_base_vertex, software_renderer;
    int has_color_buffer_float, has_border_clamp, has_clip_cull_distance;
    int max_vertex_attribs, max_ubo_bindings, max_ubo_block_size;
    int has_compute, has_tf, has_ssbo;
    int max_compute_invocations, ssbo_max_mb;
    int geom_emu, tess_emu;
    unsigned geom_cap_bytes, tess_max;
} ShimCtxFeatures;

/* One G.3 emulation bundle: the virtual GS/TCS/TES objects, the real
   compute/VS/FS programs built from them, scratch buffers for transform
   feedback and SSBO output, and a flattened view of the app's attributes.
   Entries are kept in a per-context linked list so shaders can be compiled
   before they are linked into a program. */
typedef struct ShimEmuProg {
    struct ShimEmuProg *next;
    GLuint prog;
    GLuint p1, p2, p3, p2a, p2b;
    GLuint gs, tcs, tes, fs, vs;
    GLuint vao, vbo, ibo, tf_buf, ssbo[4];
    GLuint tf_obj, fs_id, vs_id;
    int patch_vertices, point_mode, cw, max_idx;
    float levels[6];
    int kind;                  /* 1 = GS, 2 = TCS, 3 = TES, 4 = TCS+TES */
    int out_prim, max_vertices, max_idx_per_prim;
    int stride, out_stride, point_size_off;
    int nattrs;
    int attr_size[16], attr_type[16], attr_off[16];
    char attr_name[16][64];
    int link_ok;
    char *log;
    struct ShimEmuUniform *uniforms;
    int vs_attached, fs_attached;
} ShimEmuProg;

/* H.1: partial snapshot for glPushAttrib/glPopAttrib. Stores the legacy
   state consumed by G.2; GL_HINT/GL_LINE/GL_POLYGON have no GLES
   representation and stay documented no-ops. */
typedef struct ShimAttribState {
    int matrix_mode, mv_top, proj_top, tex_top[8], client_active_tex, active_tex;
    float current_color[4], current_normal[3], current_texcoord[8][4];
    float secondary_color[4], fog_coord, point_size;
    int point_size_set;
    int tex2d_enabled[8], texenv_mode[8];
    float texenv_color[8][4];
    int alpha_test, alpha_func, lighting_enabled, fog_enabled, shade_model;
    int color_material, color_material_face, color_material_mode;
    float alpha_ref;
    uint32_t clip_enable_mask;
    int fog_mode, arrays_enabled[13];
} ShimAttribState;

/* Module H.4: display lists. Records a compact command sequence and
   replays it by calling the same shim wrappers. */
typedef enum {
    DL_BEGIN, DL_END, DL_VERTEX4, DL_COLOR4, DL_NORMAL3, DL_TEXCOORD4,
    DL_MATRIX_MODE, DL_LOAD_IDENTITY, DL_PUSH_MATRIX, DL_POP_MATRIX,
    DL_MATRIX_LOAD, DL_MATRIX_MULT, DL_ENABLE, DL_DISABLE,
    DL_BIND_TEXTURE, DL_ACTIVE_TEXTURE, DL_ALPHA_FUNC, DL_TEXENV_MODE,
    DL_TEXENV_COLOR, DL_CALL_LIST
} ShimDLOp;

typedef struct {
    ShimDLOp op;
    int i[2];
    float f[16];
} ShimDLCmd;

typedef struct {
    ShimDLCmd *cmds;
    size_t n, cap;
    int generated;
} ShimDL;

typedef struct ShimCtx {
    struct ShimCtx *next;
    EGLContext egl;
    int destroyed;

    ShimCtxFeatures features;

    /* Module H fixed-function client state. Everything here is per
       EGLContext: GL state is not shared between contexts even when their
       objects are. The stacks have the fixed GL 1.x depths (32 matrix,
       16 attrib), so no dynamic allocation is needed on the hot path. */
    int matrix_mode;
    float mv_stack[32][16], proj_stack[32][16], tex_stack[8][32][16];
    int mv_top, proj_top, tex_top[8];
    float current_color[4], current_normal[3], current_texcoord[8][4];
    float secondary_color[4], fog_coord;
    float point_size;
    int point_size_set;
    ShimArray arrays[SHIM_ARR_COUNT];
    int client_active_tex;
    int active_tex;                 /* glActiveTexture, 0..7 */
    int legacy_va_enabled;
    int tex2d_enabled[8], texenv_mode[8];
    float texenv_color[8][4];
    int alpha_test, alpha_func;
    int tex_matrix_touched;
    int lighting_enabled, fog_enabled;
    float alpha_ref;
    int shade_model;
    int color_material, color_material_face, color_material_mode;
    GLuint imm_vao, imm_vbo, imm_ibo, imm_prog;
    GLuint client_vbo[13];
    int client_cap[13];
    int imm_vbo_cap, imm_ibo_cap;
    GLuint tmp_ibo;
    int tmp_ibo_cap;
    GLuint quad_ibo;
    int quad_ibo_cap;
    int quad_abs_first, quad_abs_count, quad_abs_mode;
    int gen_mipmap_ids[64], gen_mipmap_pos;
    GLuint pending_error;
    int list_base;
    ShimDL *dl_active;
    int dl_recursion;
    /* list to execute once when COMPILE_AND_EXECUTE closes */
    int dl_exec;
    ShimDL **dl_table;       /* index = list name - 1 */
    size_t dl_table_n;
    ShimAttribState attrib[16];
    int attrib_top;

    /* v2.1 G.2 fixed-function uniforms and UBO ring. Four buffers are
       rotated so a frame still in flight keeps reading stable data while
       the next frame's state is uploaded; ff_ubo_binding uses the highest
       binding point to avoid colliding with the app's own UBOs. */
    ShimFFBlock ff;
    uint32_t ff_dirty;
    int ff_ready, ff_ubo_cur, ff_ubo_binding;
    GLuint ff_ubo[4];
    GLuint cur_prog;
    uint32_t clip_enable_mask;
    int fog_mode;
    float light_spot_cutoff[8];
    int g_ff_variants;

    /* v2.1 G.3 emulation state: current emulation program plus the shared
       TF/output buffers. Present only when emulation was requested and the
       driver passed phase 0; see Module G.3 below. */
    int emu_cur;
    GLuint emu_tf_buf, emu_out_vbo, emu_out_ibo;
    unsigned emu_tf_cap, emu_out_vbo_cap, emu_out_ibo_cap;
    GLuint emu_vao;
    int emu_patch_vertices;
    ShimEmuProg *emu_progs;
} ShimCtx;

/* ========================================================================== */
/* Section 3.0: global state, lazy real pointers, helpers                     */
/* ========================================================================== */

/* X-macro: every libGLESv2 symbol is resolved lazily, once. The same list
   generates (a) the real_* function-pointer declarations, (b) the loader in
   ensure_gl_ptrs(), and (c) the lookup entries in shim_lookup_gl(), so a
   function cannot be added in one place and forgotten in another. The
   entries from spec lines 169-182 are returned unwrapped: the shim must not
   put a desktop wrapper in front of a real GLES entry point. */
#define GLFUNCS(X) \
    X(GLenum, glGetError, (void)) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(const GLubyte *, glGetStringi, (GLenum, GLuint)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glGetIntegeri_v, (GLenum, GLuint, GLint *)) \
    X(void, glGetFloatv, (GLenum, GLfloat *)) \
    X(void, glGetBooleanv, (GLenum, GLboolean *)) \
    X(GLboolean, glIsEnabled, (GLenum)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glClearDepthf, (GLclampf)) \
    X(void, glDepthRangef, (GLclampf, GLclampf)) \
    X(void, glDrawBuffers, (GLsizei, const GLenum *)) \
    X(void, glReadBuffer, (GLenum)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    X(GLboolean, glUnmapBuffer, (GLenum)) \
    X(void, glGetBufferParameteriv, (GLenum, GLenum, GLint *)) \
    X(void, glGetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
    X(void, glTexParameteriv, (GLenum, GLenum, const GLint *)) \
    X(void, glTexParameterfv, (GLenum, GLenum, const GLfloat *)) \
    X(void, glGenerateMipmap, (GLenum)) \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void, glCompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, glGetTexLevelParameteriv, (GLenum, GLint, GLenum, GLint *)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glDrawRangeElements, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void *)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glVertexAttrib2f, (GLuint, GLfloat, GLfloat)) \
    X(void, glGetVertexAttribfv, (GLuint, GLenum, GLfloat *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glDetachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *)) \
    X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar *)) \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    X(void, glGetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glBlendEquation, (GLenum)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glCullFace, (GLenum)) \
    X(void, glFrontFace, (GLenum)) \
    X(void, glLineWidth, (GLfloat)) \
    X(void, glPolygonOffset, (GLfloat, GLfloat)) \
    X(void, glStencilFunc, (GLenum, GLint, GLuint)) \
    X(void, glStencilOp, (GLenum, GLenum, GLenum)) \
    X(void, glStencilMask, (GLuint)) \
    /* Guaranteed minimum list from spec 169-182: return the real pointer unwrapped. */ \
    X(void, glClear, (GLbitfield)) \
    X(void, glFlush, (void)) \
    X(void, glFinish, (void)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, glGetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint *)) \
    X(void, glGetRenderbufferParameteriv, (GLenum, GLenum, GLint *)) \
    X(GLboolean, glIsFramebuffer, (GLuint)) \
    X(GLboolean, glIsRenderbuffer, (GLuint)) \
    X(void, glBindSampler, (GLuint, GLuint)) \
    X(void, glGenSamplers, (GLsizei, GLuint *)) \
    X(void, glDeleteSamplers, (GLsizei, const GLuint *)) \
    X(void, glSamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, glSamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(void, glGetSamplerParameteriv, (GLuint, GLenum, GLint *)) \
    X(void, glGenQueries, (GLsizei, GLuint *)) \
    X(void, glDeleteQueries, (GLsizei, const GLuint *)) \
    X(void, glBeginQuery, (GLenum, GLuint)) \
    X(void, glEndQuery, (GLenum)) \
    X(void, glGetQueryiv, (GLenum, GLenum, GLint *)) \
    X(void, glGetQueryObjectiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetQueryObjectuiv, (GLuint, GLenum, GLuint *)) \
    X(GLboolean, glIsQuery, (GLuint)) \
    X(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei)) \
    X(void, glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void *, GLsizei)) \
    X(void, glVertexAttribDivisor, (GLuint, GLuint)) \
    X(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
    X(void, glMultiDrawArrays, (GLenum, const GLint *, const GLsizei *, GLsizei)) \
    X(void, glProgramUniform1ui, (GLuint, GLint, GLuint)) \
    X(void, glProgramUniform1i, (GLuint, GLint, GLint)) \
    X(void, glProgramUniform1f, (GLuint, GLint, GLfloat)) \
    X(void, glDispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, glMemoryBarrier, (GLbitfield)) \
    X(void, glTransformFeedbackVaryings, (GLuint, GLsizei, const GLchar *const *, GLenum)) \
    X(void, glBeginTransformFeedback, (GLenum)) \
    X(void, glEndTransformFeedback, (void)) \
    X(void, glGenTransformFeedbacks, (GLsizei, GLuint *)) \
    X(void, glBindTransformFeedback, (GLenum, GLuint)) \
    X(void, glDeleteTransformFeedbacks, (GLsizei, const GLuint *)) \
    X(void, glBindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
    X(void, glUniform1ui, (GLint, GLuint)) \
    X(void, glProgramUniform1fv, (GLuint, GLint, GLsizei, const GLfloat *)) \
    X(void, glProgramUniform2fv, (GLuint, GLint, GLsizei, const GLfloat *)) \
    X(void, glProgramUniform3fv, (GLuint, GLint, GLsizei, const GLfloat *)) \
    X(void, glProgramUniform4fv, (GLuint, GLint, GLsizei, const GLfloat *)) \
    X(void, glProgramUniform1iv, (GLuint, GLint, GLsizei, const GLint *)) \
    X(void, glProgramUniform2iv, (GLuint, GLint, GLsizei, const GLint *)) \
    X(void, glProgramUniform3iv, (GLuint, GLint, GLsizei, const GLint *)) \
    X(void, glProgramUniform4iv, (GLuint, GLint, GLsizei, const GLint *)) \
    X(void, glProgramUniform1uiv, (GLuint, GLint, GLsizei, const GLuint *)) \
    X(void, glProgramUniformMatrix2fv, (GLuint, GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glProgramUniformMatrix3fv, (GLuint, GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glProgramUniformMatrix4fv, (GLuint, GLint, GLsizei, GLboolean, const GLfloat *))

/* Forward declarations of wrappers used by the symbol tables. */
static void *shim_SDL_GL_GetProcAddress(const char *);
static void *shim_eglGetProcAddress(const char *);
static EGLBoolean shim_egl_swap_interval(EGLDisplay, EGLint);
static EGLBoolean shim_eglBindAPI(EGLenum);
static EGLBoolean shim_eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLContext shim_eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static void init_ctx_defaults(ShimCtx *c);
static EGLBoolean shim_eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean shim_eglDestroyContext(EGLDisplay, EGLContext);

static void shim_glDrawArrays(GLenum, GLint, GLsizei);
static void shim_glDrawElements(GLenum, GLsizei, GLenum, const void *);
static void shim_glDrawRangeElements(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void shim_emu_draw_arrays(GLenum, GLint, GLsizei);
static void shim_emu_draw_elements(GLenum, GLsizei, GLenum, const void *);
struct ShimEmuProg;
static struct ShimEmuProg *shim_emu_prog_find(ShimCtx *, GLuint, int);
static void shim_glEnable(GLenum);
static void shim_glDisable(GLenum);
static GLboolean shim_glIsEnabled(GLenum);
static void shim_glUseProgram(GLuint);
static void shim_glLinkProgram(GLuint);
static void shim_glBindTexture(GLenum, GLuint);
static void shim_glTexParameteri(GLenum, GLenum, GLint);
static void shim_glTexParameterf(GLenum, GLenum, GLfloat);
static void shim_glTexParameteriv(GLenum, GLenum, const GLint *);
static void shim_glTexParameterfv(GLenum, GLenum, const GLfloat *);
static void shim_glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void shim_glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void shim_glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void shim_glCompressedTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
static void shim_glTexStorage2D(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void shim_glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
static const GLubyte *shim_glGetString(GLenum);
static const GLubyte *shim_glGetStringi(GLenum, GLuint);
static void shim_glGetIntegerv(GLenum, GLint *);
static void shim_glGetFloatv(GLenum, GLfloat *);
static void shim_glDrawBuffers(GLsizei, const GLenum *);

static pthread_mutex_t g_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ext_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_prog_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_emu_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static pthread_once_t g_gl_once = PTHREAD_ONCE_INIT;

static ShimCtx *g_ctx_registry;
static char **g_ext_tokens;
static int g_ext_count;
static char *g_ext_string;
static int g_debug;
static int g_isolate_llvm;
static int g_allow_uncap;
static int g_desktop_mode;          /* 0 passive, 1 desktop */
static int g_desktop_policy;        /* 0 forced off, 1 forced on, otherwise auto */
static int g_ff_shaders = 1;
static int g_ff_need_variants;
static int g_ff_shader_count;
struct ShimProgEntry { GLuint prog; uint32_t mask; struct ShimProgEntry *next; };
static struct ShimProgEntry *g_prog_ff;
/* Track real VS/FS: G.3 virtual shaders are attached later, and their
   ShimEmuProg is invisible if only glAttachShader is inspected. */
struct ShimProgAttach {
    GLuint prog, vs, fs;
    struct ShimProgAttach *next;
};
static struct ShimProgAttach *g_prog_attach;
static char g_ver_str[128];
static char g_glsl_str[32];
static int g_ver_ready;
static int g_ver_major = 3;
static int g_ver_minor = 1;
static int g_ver_forced;
static int g_bgra_ext_mode;
static int g_ff_enabled = 1;
static int g_geom_requested;
static int g_tess_requested;
static unsigned g_geom_cap_mb = 32;
static unsigned g_tess_max = 16;

static __thread ShimCtx *tls_ctx_ptr;
static __thread EGLContext tls_ctx_handle;
static __thread uint8_t *tls_scratch;
static __thread size_t tls_scratch_cap;
static pthread_key_t g_scratch_key;
static pthread_once_t g_scratch_once = PTHREAD_ONCE_INIT;

static void *real_dlsym_fn = NULL;
static void *real_dlopen_fn = NULL;

#define DECL_REAL(ret, name, args) static ret (*real_##name) args;
GLFUNCS(DECL_REAL)
#undef DECL_REAL

static void *real_sdl_getproc;
static void *real_egl_getproc;
static void *real_egl_swap_interval;
static void *real_egl_bind_api;
static void *real_egl_choose_config;
static void *real_egl_create_context;
static void *real_egl_make_current;
static void *real_egl_destroy_context;

/* Desktop mode is switched on lazily by the first desktop-GL request
   (dlopen of libGL, eglBindAPI(EGL_OPENGL_API), a 3.2+ context, or an
   explicit NUUBOS_SHIM_DESKTOP_GL=1). It is atomic because the preload is
   used from multiple threads, including SDL's loader thread. */
static inline int desktop_mode(void)
{
    return __atomic_load_n(&g_desktop_mode, __ATOMIC_RELAXED) != 0;
}

/* g_desktop_policy==0 means the app explicitly opted out of the identity
   wrappers; the gl4es dlopen redirect still happens in every mode. */
static inline void enter_desktop_mode(void)
{
    if (g_desktop_policy == 0)
        return;
    __atomic_store_n(&g_desktop_mode, 1, __ATOMIC_RELAXED);
}

/* Diagnostics are silent unless NUUBOS_SHIM_DEBUG is set; no trace string
   is ever shown to the player. */
static void shim_trace(const char *fmt, ...)
{
    va_list ap;
    if (!g_debug)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000L);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void scratch_destructor(void *p)
{
    free(p);
}

static void scratch_key_init(void)
{
    pthread_key_create(&g_scratch_key, scratch_destructor);
}

static void ensure_scratch_key(void)
{
    /* Called from several SDL/Mesa threads: pthread_once also removes the
       unsynchronised g_scratch_key_ready check. */
    pthread_once(&g_scratch_once, scratch_key_init);
}

/* Reusable per-thread scratch for widening GLdouble arguments to GLfloat.
   A pthread key frees the buffer at thread exit; the TLS pointer alone is
   not enough because detached SDL threads would otherwise leak it. */
static uint8_t *scratch(size_t need)
{
    ensure_scratch_key();
    if (need > tls_scratch_cap) {
        size_t cap = 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) return NULL;
            cap <<= 1;
        }
        uint8_t *p = realloc(tls_scratch, cap);
        if (!p)
            return NULL;
        tls_scratch = p;
        tls_scratch_cap = cap;
        pthread_setspecific(g_scratch_key, p);
    }
    return tls_scratch;
}

/* Mark part of the fixed-function UBO stale. Cheap enough to call from
   every legacy state setter: ff_sync() later collapses all bits set during
   a frame into one upload. */
static inline void shim_ff_touch(ShimCtx *c, uint32_t bit)
{
    if (c && __atomic_load_n(&g_ff_shader_count, __ATOMIC_RELAXED))
        c->ff_dirty |= bit;
}

/* Dynamic string buffer used by shader translation and the extension/version
   strings. Always NUL-terminated; sb_* return 0 on allocation failure so the
   caller can fall back to the unmodified shader instead of crashing. */
typedef struct StrBuf {
    char *data;
    size_t len, cap;
} StrBuf;

static int sb_reserve(StrBuf *s, size_t extra)
{
    if (s->len + extra + 1 <= s->cap)
        return 1;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->len + extra + 1) {
        if (cap > SIZE_MAX / 2)
            return 0;
        cap <<= 1;
    }
    char *p = realloc(s->data, cap);
    if (!p)
        return 0;
    s->data = p;
    s->cap = cap;
    return 1;
}

static int sb_addn(StrBuf *s, const char *p, size_t n)
{
    if (!sb_reserve(s, n))
        return 0;
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = 0;
    return 1;
}

static int sb_add(StrBuf *s, const char *p)
{
    return sb_addn(s, p, strlen(p));
}

static int sb_printf(StrBuf *s, const char *fmt, ...)
{
    va_list ap, cp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !sb_reserve(s, (size_t)n)) {
        va_end(cp);
        return 0;
    }
    vsnprintf(s->data + s->len, s->cap - s->len, fmt, cp);
    va_end(cp);
    s->len += (size_t)n;
    return 1;
}

#define sb_addf(sb, ...) sb_printf((sb), __VA_ARGS__)

/* Insert code just before the final brace of main() in the already
   translated shader, so legacy state is applied after the output is written
   regardless of where the app's statements are placed. */
static void shim_inject_before_last_brace(StrBuf *s, const char *text)
{
    if (!s || !s->data || !text || !*text) return;
    size_t i = s->len;
    while (i > 0 && (s->data[i-1] == '\n' || s->data[i-1] == ' ' ||
                     s->data[i-1] == '\t' || s->data[i-1] == '\r')) i--;
    if (!i || s->data[i-1] != '}') return;
    i--;  /* the returned index points just AFTER the brace */
    size_t n = strlen(text);
    if (!sb_reserve(s, n)) return;
    memmove(s->data + i + n, s->data + i, s->len - i + 1);
    memcpy(s->data + i, text, n);
    s->len += n;
}

/* gl_TexCoord[n] is an output varying in GLSL ES, but desktop GL drivers
   initialise it from gl_MultiTexCoord[n]. Do it on entry to main() so the
   G.2/H translation never leaves the coordinate at zero. */
static void shim_inject_after_main_brace(StrBuf *s, const char *text)
{
    if (!s || !s->data || !text || !*text) return;
    char *p = strstr(s->data, "main");
    if (!p) return;
    p = strchr(p, '{');
    if (!p) return;
    size_t i = (size_t)(p + 1 - s->data);
    size_t n = strlen(text);
    if (!sb_reserve(s, n)) return;
    memmove(s->data + i + n, s->data + i, s->len - i + 1);
    memcpy(s->data + i, text, n);
    s->len += n;
}

static void identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(float out[16], const float a[16], const float b[16])
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = v;
        }
    memcpy(out, t, sizeof(t));
}

static int mat4_inverse(float out[16], const float m[16])
{
    float a[16], inv[16];
    memcpy(a, m, sizeof(a));
    identity(inv);
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int row = col + 1; row < 4; row++)
            if (fabsf(a[col * 4 + row]) > fabsf(a[col * 4 + pivot]))
                pivot = row;
        if (fabsf(a[col * 4 + pivot]) < 1e-12f) {
            identity(out);
            return 0;
        }
        if (pivot != col) {
            for (int k = 0; k < 4; k++) {
                float tmp = a[k * 4 + col];
                a[k * 4 + col] = a[k * 4 + pivot];
                a[k * 4 + pivot] = tmp;
                tmp = inv[k * 4 + col];
                inv[k * 4 + col] = inv[k * 4 + pivot];
                inv[k * 4 + pivot] = tmp;
            }
        }
        float d = a[col * 4 + col];
        for (int k = 0; k < 4; k++) {
            a[col * 4 + k] /= d;
            inv[col * 4 + k] /= d;
        }
        for (int c = 0; c < 4; c++) {
            if (c == col)
                continue;
            float f = a[col * 4 + c];
            for (int k = 0; k < 4; k++) {
                a[c * 4 + k] -= f * a[col * 4 + k];
                inv[c * 4 + k] -= f * inv[col * 4 + k];
            }
        }
    }
    memcpy(out, inv, sizeof(inv));
    return 1;
}

static void mat4_transpose(float out[16], const float m[16])
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = m[r * 4 + c];
}

static void normal_matrix_from_mv(float out[12], const float mv[16])
{
    float inv[16], t[16];
    if (!mat4_inverse(inv, mv))
        identity(inv);
    mat4_transpose(t, inv);
    for (int c = 0; c < 3; c++)
        memcpy(out + c * 4, t + c * 4, 3 * sizeof(float));
}

/* -------------------------------------------------------------------------- */
/* 2.1-2.5: constructor, dlopen/dlsym/EGL hooks, symbol lookup                */
/* -------------------------------------------------------------------------- */

static void *shim_lookup_gl(const char *name);
static void *shim_lookup_egl(const char *name);
static int ensure_egl_real(const char *name, void **slot);
static void ensure_gl_symbols(void);
static void shim_probe_emu(ShimCtx *c);   /* G.3.0 phase 0 */

/* mono-sgen/dotnet ship their own static LLVM. Without RTLD_DEEPBIND,
   Mesa's libLLVM references resolve to mono's uninitialised copies and the
   first shader compile crashes. Apply deep binding only to Mesa libraries
   and only for detected Mono processes. */
static int shim_dl_flags(const char *file, int flags)
{
    if (g_isolate_llvm && file) {
        if (strstr(file, "libGLESv2") || strstr(file, "libLLVM") ||
            strstr(file, "libgallium") || strstr(file, "libEGL"))
            flags |= RTLD_DEEPBIND;
    }
    return flags;
}

/* One-time process setup via pthread_once: resolve the real dlsym/dlopen
   with versioned symbols (mono exports its own dlsym), read the environment,
   and force S3TC decompression for DXT textures. The script's late exports
   are intentionally ignored: the shim's policy is fixed before game code
   runs. */
static void shim_init(void)
{
    static const char *versions[] = { "GLIBC_2.34", "GLIBC_2.17", "GLIBC_2.4" };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        real_dlsym_fn = dlvsym(RTLD_NEXT, "dlsym", versions[i]);
        if (real_dlsym_fn)
            break;
    }
    if (!real_dlsym_fn) {
        /* A1: never fall back to dlsym(RTLD_NEXT,"dlsym"): that resolves to us. */
        fprintf(stderr, "libgl_nuubos_shim: no real dlsym found; aborting\n");
        abort();
    }
    real_dlopen_fn = ((void *(*)(void *, const char *))real_dlsym_fn)(RTLD_NEXT, "dlopen");
    if (!real_dlopen_fn) {
        fprintf(stderr, "libgl_nuubos_shim: no real dlopen found; aborting\n");
        abort();
    }

    /* Spec v2.1 step 3.0.2 requires overwrite=0. Forcing this globally
       regressed Dusklight/Citra on 2026-11-26, and detect_features() already
       reports native S3TC on the H700; keep the documented behaviour. */
    setenv("force_s3tc_enable", "true", 0);

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char *base = strrchr(exe, '/');
        base = base ? base + 1 : exe;
        if (strncmp(base, "mono", 4) == 0 || strncmp(base, "dotnet", 6) == 0)
            g_isolate_llvm = 1;
    }
    if (getenv("MONOGAME_PATCH"))
        g_isolate_llvm = 1;
    g_debug = getenv("NUUBOS_SHIM_DEBUG") != NULL;
    g_allow_uncap = getenv("NUUBOS_ALLOW_UNCAP") != NULL;

    const char *pol = getenv("NUUBOS_SHIM_DESKTOP_GL");
    if (pol && strcmp(pol, "0") == 0)
        g_desktop_policy = 0;
    else if (pol && strcmp(pol, "1") == 0) {
        g_desktop_policy = 1;
        g_desktop_mode = 1;
    } else
        g_desktop_policy = -1;

    /* PortMaster's libgl_*.txt exports LIBGL_FB/LIBGL_ES and points SDL at a
       bundled gl4es, whose desktop GL path drew a black window on nuubOS.
       The shim exists precisely to redirect that dlopen to Mesa GLES; do NOT
       go passive here. NUUBOS_SHIM_DESKTOP_GL=0 only disables the desktop
       identity wrappers; the dlopen redirect stays active in every mode. */

    const char *ver = getenv("NUUBOS_SHIM_GL_VERSION");
    if (ver && sscanf(ver, "%d.%d", &g_ver_major, &g_ver_minor) == 2) {
        if (g_ver_major < 3 || g_ver_major > 3 || g_ver_minor < 0 || g_ver_minor > 3) {
            g_ver_major = 3;
            g_ver_minor = 1;
        } else {
            g_ver_forced = 1;
        }
    }
    g_bgra_ext_mode = getenv("NUUBOS_SHIM_BGRA_MODE") &&
                      strcmp(getenv("NUUBOS_SHIM_BGRA_MODE"), "ext") == 0;
    g_ff_enabled = !(getenv("NUUBOS_SHIM_FF_SHADERS") &&
                     strcmp(getenv("NUUBOS_SHIM_FF_SHADERS"), "0") == 0);
    g_ff_shaders = g_ff_enabled;
    g_geom_requested = getenv("NUUBOS_SHIM_GEOM_EMU") &&
                       strcmp(getenv("NUUBOS_SHIM_GEOM_EMU"), "1") == 0;
    g_tess_requested = getenv("NUUBOS_SHIM_TESS_EMU") &&
                       strcmp(getenv("NUUBOS_SHIM_TESS_EMU"), "1") == 0;
    if (g_tess_requested)
        g_geom_requested = 1;
    const char *cap = getenv("NUUBOS_SHIM_GEOM_CAP_MB");
    g_geom_cap_mb = cap ? (unsigned)atoi(cap) : 32;
    if (g_geom_cap_mb == 0)
        g_geom_cap_mb = 32;
    const char *tmax = getenv("NUUBOS_SHIM_TESS_MAX");
    g_tess_max = tmax ? (unsigned)atoi(tmax) : 16;
    if (g_tess_max < 1 || g_tess_max > 64)
        g_tess_max = 16;

    ensure_scratch_key();
    shim_trace("shim_init: pid=%ld desktop=%d isolate=%d ff=%d bgra=%s geom=%d tess=%d\n",
               (long)getpid(), g_desktop_mode, g_isolate_llvm,
               g_ff_enabled, g_bgra_ext_mode ? "ext" : "swizzle",
               g_geom_requested, g_tess_requested);
}

static void shim_ensure_init(void)
{
    pthread_once(&g_init_once, shim_init);
}

/* Process teardown: free the context registry and cached strings. GL
   objects are intentionally not deleted here (see shim_dtor). */
static void shim_fini(void)
{
    pthread_mutex_lock(&g_ctx_mutex);
    ShimCtx *c = g_ctx_registry;
    while (c) {
        ShimCtx *next = c->next;
        free(c);
        c = next;
    }
    g_ctx_registry = NULL;
    pthread_mutex_unlock(&g_ctx_mutex);
    pthread_mutex_lock(&g_ext_mutex);
    if (g_ext_tokens) {
        for (int i = 0; i < g_ext_count; i++)
            free(g_ext_tokens[i]);
        free(g_ext_tokens);
        g_ext_tokens = NULL;
    }
    free(g_ext_string);
    g_ext_string = NULL;
    pthread_mutex_unlock(&g_ext_mutex);
    free(tls_scratch);
    tls_scratch = NULL;
}

__attribute__((constructor)) static void shim_ctor(void)
{
    shim_ensure_init();
}

__attribute__((destructor)) static void shim_dtor(void)
{
    /* No glDelete*: EGL has already torn down the context when this runs. */
    shim_fini();
}

/* Exported with the real name so PLT calls are intercepted. Desktop GL,
   gl4es and libOpenGL requests are redirected to Mesa's GLES library: the
   bundled gl4es desktop path drew a black window on nuubOS. This redirect
   is not gated by NUUBOS_SHIM_DESKTOP_GL; that variable only controls the
   identity/version wrappers. */
__attribute__((visibility("default")))
void *dlopen(const char *file, int flags)
{
    shim_ensure_init();
    int desktop_request = 0;
    const char *requested = file;
    if (file) {
        if ((strstr(file, "gl4es") && strstr(file, "libEGL")) ||
            strcmp(file, "libEGL.so.1") == 0 || strcmp(file, "libEGL.so") == 0)
            file = "libEGL.so.1";
        else if ((strstr(file, "gl4es") && strstr(file, "libGL.so")) ||
                 strcmp(file, "libGLESv2.so.2") == 0 ||
                 strcmp(file, "libGL.so.1") == 0 || strcmp(file, "libGL.so") == 0 ||
                 strcmp(file, "libOpenGL.so.0") == 0) {
            desktop_request = strcmp(file, "libGLESv2.so.2") != 0;
            file = "libGLESv2.so.2";
        }
        if (requested != file)
            shim_trace("dlopen: %s -> %s desktop=%d\n", requested, file,
                       desktop_mode() || desktop_request);
        if (strstr(file, "libmonosgen") || strstr(file, "libmono-2.0") ||
            strstr(file, "libcoreclr") || strstr(file, "libhostfxr"))
            g_isolate_llvm = 1;
        if (desktop_request)
            enter_desktop_mode();
        flags = shim_dl_flags(file, flags);
    }
    return ((void *(*)(const char *, int))real_dlopen_fn)(file, flags);
}

/* Interpose dlsym as well as the PLT: SDL and libretro frontends resolve
   most GL functions through eglGetProcAddress/SDL_GL_GetProcAddress or by
   name. Real pointers are cached once in the corresponding slots, and a
   generic gl* lookup goes through shim_lookup_gl() (wrappers first, real
   GLES symbols second). */
__attribute__((visibility("default")))
void *dlsym(void *handle, const char *name)
{
    shim_ensure_init();
    void *(*dsym)(void *, const char *) = real_dlsym_fn;
    if (strcmp(name, "SDL_GL_GetProcAddress") == 0) {
        if (!real_sdl_getproc)
            real_sdl_getproc = dsym(handle, name);
        return (void *)shim_SDL_GL_GetProcAddress;
    }
    if (strcmp(name, "eglGetProcAddress") == 0) {
        if (!real_egl_getproc)
            real_egl_getproc = dsym(handle, name);
        return (void *)shim_eglGetProcAddress;
    }
    if (strcmp(name, "eglSwapInterval") == 0) {
        if (!real_egl_swap_interval)
            real_egl_swap_interval = dsym(handle, name);
        return (void *)shim_egl_swap_interval;
    }
    static const char *egl_names[] = {
        "eglBindAPI", "eglChooseConfig", "eglCreateContext",
        "eglMakeCurrent", "eglDestroyContext"
    };
    void **egl_slots[] = {
        &real_egl_bind_api, &real_egl_choose_config, &real_egl_create_context,
        &real_egl_make_current, &real_egl_destroy_context
    };
    for (size_t i = 0; i < sizeof(egl_names) / sizeof(egl_names[0]); i++) {
        if (strcmp(name, egl_names[i]) == 0) {
            if (!*egl_slots[i])
                *egl_slots[i] = dsym(handle, name);
            void *hook = shim_lookup_egl(name);
            return hook ? hook : dsym(handle, name);
        }
    }
    if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') {
        void *fn = shim_lookup_gl(name);
        if (fn)
            return fn;
    }
    /* RTLD_NEXT is forwarded unchanged; known accepted limitation (2.3). */
    return dsym(handle, name);
}

__attribute__((visibility("default")))
void *shim_SDL_GL_GetProcAddress(const char *proc)
{
    shim_ensure_init();
    if (!proc)
        return NULL;
    void *fn = shim_lookup_gl(proc);
    if (fn)
        return fn;
    if (!real_sdl_getproc)
        real_sdl_getproc = ((void *(*)(void *, const char *))real_dlsym_fn)(
            RTLD_NEXT, "SDL_GL_GetProcAddress");
    if (real_sdl_getproc) {
        fn = ((void *(*)(const char *))real_sdl_getproc)(proc);
        if (fn)
            return fn;
    }
    return ((void *(*)(void *, const char *))real_dlsym_fn)(RTLD_DEFAULT, proc);
}

__attribute__((visibility("default")))
void *shim_eglGetProcAddress(const char *procname)
{
    shim_ensure_init();
    if (!procname)
        return NULL;
    void *fn = shim_lookup_egl(procname);
    if (!fn)
        fn = shim_lookup_gl(procname);
    if (fn)
        return fn;
    if (!real_egl_getproc)
        real_egl_getproc = ((void *(*)(void *, const char *))real_dlsym_fn)(
            RTLD_NEXT, "eglGetProcAddress");
    if (!real_egl_getproc)
        return NULL;
    return ((void *(*)(const char *))real_egl_getproc)(procname);
}

static int ensure_egl_real(const char *name, void **slot)
{
    shim_ensure_init();
    if (*slot)
        return 1;
    *slot = ((void *(*)(void *, const char *))real_dlsym_fn)(RTLD_NEXT, name);
    if (!*slot && real_egl_getproc)
        *slot = ((void *(*)(const char *))real_egl_getproc)(name);
    return *slot != NULL;
}

__attribute__((visibility("default")))
/* Vsync is on by default: uncapped swap is allowed only with
   NUUBOS_ALLOW_UNCAP, since the H700's panel otherwise tears. */
EGLBoolean shim_egl_swap_interval(EGLDisplay dpy, EGLint interval)
{
    shim_ensure_init();
    if (!g_allow_uncap && interval == 0)
        interval = 1;
    if (!ensure_egl_real("eglSwapInterval", &real_egl_swap_interval))
        return EGL_FALSE;
    return ((EGLBoolean (*)(EGLDisplay, EGLint))real_egl_swap_interval)(dpy, interval);
}

__attribute__((visibility("default")))
/* An app asking for the desktop OpenGL API is switched to GLES instead of
   being rejected: Mesa exposes no desktop API in this image. Passive mode
   forwards the request untouched. */
EGLBoolean shim_eglBindAPI(EGLenum api)
{
    shim_ensure_init();
    if (!ensure_egl_real("eglBindAPI", &real_egl_bind_api))
        return EGL_FALSE;
    if (api == EGL_OPENGL_API && g_desktop_policy != 0 && !desktop_mode()) {
        enter_desktop_mode();
        if (desktop_mode())
            return ((EGLBoolean (*)(EGLenum))real_egl_bind_api)(EGL_OPENGL_ES_API);
    }
    return ((EGLBoolean (*)(EGLenum))real_egl_bind_api)(api);
}

__attribute__((visibility("default")))
/* Rewrite EGL_OPENGL_BIT to EGL_OPENGL_ES3_BIT (then ES2 if no config was
   found) in desktop mode. Without this, config selection succeeds but the
   context creation below has nothing renderable to bind. */
EGLBoolean shim_eglChooseConfig(EGLDisplay dpy, const EGLint *attribs,
                                EGLConfig *configs, EGLint config_size, EGLint *num)
{
    shim_ensure_init();
    if (!ensure_egl_real("eglChooseConfig", &real_egl_choose_config))
        return EGL_FALSE;
    if (!desktop_mode() || !attribs)
        return ((EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
                real_egl_choose_config)(dpy, attribs, configs, config_size, num);

    EGLint local[256];
    size_t n = 0;
    while (n + 1 < sizeof(local) / sizeof(local[0]) && attribs[n] != EGL_NONE) {
        local[n] = attribs[n];
        local[n + 1] = attribs[n + 1];
        n += 2;
    }
    local[n] = EGL_NONE;
    int found_renderable = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        if (local[i] == EGL_RENDERABLE_TYPE && (local[i + 1] & EGL_OPENGL_BIT)) {
            local[i + 1] = (local[i + 1] & ~EGL_OPENGL_BIT) | EGL_OPENGL_ES3_BIT;
            found_renderable = 1;
        }
    }
    if (!found_renderable)
        return ((EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
                real_egl_choose_config)(dpy, attribs, configs, config_size, num);
    EGLBoolean ok = ((EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
                     real_egl_choose_config)(dpy, local, configs, config_size, num);
    if (ok && num && *num == 0) {
        for (size_t i = 0; i + 1 < n; i += 2)
            if (local[i] == EGL_RENDERABLE_TYPE)
                local[i + 1] = (local[i + 1] & ~EGL_OPENGL_ES3_BIT) | EGL_OPENGL_ES2_BIT;
        ok = ((EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
              real_egl_choose_config)(dpy, local, configs, config_size, num);
    }
    return ok;
}

static EGLContext ctx_create_real(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
                                  const EGLint *attribs)
{
    shim_ensure_init();
    if (!ensure_egl_real("eglCreateContext", &real_egl_create_context))
        return EGL_NO_CONTEXT;
    return ((EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))
            real_egl_create_context)(dpy, cfg, share, attribs);
}

__attribute__((visibility("default")))
/* Map desktop context attributes (profiles, forward-compatible flag,
   requested 3.2+) onto an ES 3.0 context, then retry as ES 2.0 if the
   driver refuses. On success, allocate/attach the ShimCtx that will hold
   all emulated state for this EGLContext. */
EGLContext shim_eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
                                 const EGLint *attribs)
{
    shim_ensure_init();
    if (!desktop_mode() || !attribs)
        return ctx_create_real(dpy, cfg, share, attribs);

    int major = 3, minor = 0;
    for (size_t i = 0; attribs[i] != EGL_NONE && i + 1 < 256; i += 2) {
        EGLint a = attribs[i];
        if (a == EGL_CONTEXT_MAJOR_VERSION || a == EGL_CONTEXT_CLIENT_VERSION)
            major = 3;
        else if (a == EGL_CONTEXT_OPENGL_PROFILE_MASK ||
                 a == EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE) {
            if (major >= 3 && minor >= 2)
                enter_desktop_mode();
            /* Desktop-only attributes are discarded for ES. */
        }
    }
    if (major > 3 || (major == 3 && minor >= 2))
        enter_desktop_mode();

    EGLint local[260];
    size_t n = 0;
    int have_major = 0;
    for (size_t i = 0; attribs[i] != EGL_NONE && i + 1 < 256; i += 2) {
        EGLint a = attribs[i];
        if (a == EGL_CONTEXT_MAJOR_VERSION || a == EGL_CONTEXT_CLIENT_VERSION) {
            local[n++] = EGL_CONTEXT_MAJOR_VERSION;
            local[n++] = 3;
            have_major = 1;
        } else if (a == EGL_CONTEXT_MINOR_VERSION) {
            local[n++] = EGL_CONTEXT_MINOR_VERSION;
            local[n++] = 0;
        } else if (a == EGL_CONTEXT_OPENGL_PROFILE_MASK ||
                   a == EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE)
            continue;
        else {
            local[n++] = a;
            local[n++] = attribs[i + 1];
        }
    }
    if (!have_major) {
        local[n++] = EGL_CONTEXT_MAJOR_VERSION;
        local[n++] = 3;
        local[n++] = EGL_CONTEXT_MINOR_VERSION;
        local[n++] = 0;
    }
    local[n] = EGL_NONE;
    EGLContext ctx = ctx_create_real(dpy, cfg, share, local);
    if (ctx == EGL_NO_CONTEXT) {
        for (size_t i = 0; i + 1 < n; i += 2)
            if (local[i] == EGL_CONTEXT_MAJOR_VERSION)
                local[i + 1] = 2;
        ctx = ctx_create_real(dpy, cfg, share, local);
    }
    if (ctx != EGL_NO_CONTEXT) {
        pthread_mutex_lock(&g_ctx_mutex);
        ShimCtx *c = g_ctx_registry;
        while (c && c->egl != ctx)
            c = c->next;
        if (!c) {
            c = calloc(1, sizeof(*c));
            if (c) {
                c->egl = ctx;
                c->next = g_ctx_registry;
                g_ctx_registry = c;
                init_ctx_defaults(c);
            }
        }
        pthread_mutex_unlock(&g_ctx_mutex);
    }
    return ctx;
}

__attribute__((visibility("default")))
/* Keep the current EGLContext in TLS so shim_get_ctx() can find the right
   ShimCtx on the first GL call after a switch. */
EGLBoolean shim_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read,
                               EGLContext ctx)
{
    shim_ensure_init();
    if (!ensure_egl_real("eglMakeCurrent", &real_egl_make_current))
        return EGL_FALSE;
    EGLBoolean ok = ((EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))
                     real_egl_make_current)(d, draw, read, ctx);
    if (ok) {
        tls_ctx_handle = ctx;
        tls_ctx_ptr = NULL;
    }
    return ok;
}

__attribute__((visibility("default")))
/* Mark the registry entry destroyed rather than freeing it immediately:
   other threads may still hold a TLS pointer to that ShimCtx until they
   switch context or exit. */
EGLBoolean shim_eglDestroyContext(EGLDisplay d, EGLContext ctx)
{
    shim_ensure_init();
    if (!ensure_egl_real("eglDestroyContext", &real_egl_destroy_context))
        return EGL_FALSE;
    EGLBoolean ok = ((EGLBoolean (*)(EGLDisplay, EGLContext))real_egl_destroy_context)(d, ctx);
    if (ok) {
        pthread_mutex_lock(&g_ctx_mutex);
        for (ShimCtx *c = g_ctx_registry; c; c = c->next)
            if (c->egl == ctx)
                c->destroyed = 1;
        pthread_mutex_unlock(&g_ctx_mutex);
        if (tls_ctx_handle == ctx) {
            tls_ctx_handle = NULL;
            tls_ctx_ptr = NULL;
        }
    }
    return ok;
}

/* PLT-friendly aliases. In passive mode these are pure passthroughs. */
__attribute__((visibility("default"))) void *SDL_GL_GetProcAddress(const char *p)
{ return shim_SDL_GL_GetProcAddress(p); }
__attribute__((visibility("default"))) void *eglGetProcAddress(const char *p)
{ return shim_eglGetProcAddress(p); }
__attribute__((visibility("default"))) EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i)
{ return shim_egl_swap_interval(d, i); }
__attribute__((visibility("default"))) EGLBoolean eglBindAPI(EGLenum a)
{ return shim_eglBindAPI(a); }
__attribute__((visibility("default"))) EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint s, EGLint *n)
{ return shim_eglChooseConfig(d, a, c, s, n); }
__attribute__((visibility("default"))) EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a)
{ return shim_eglCreateContext(d, c, s, a); }
__attribute__((visibility("default"))) EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{ return shim_eglMakeCurrent(d, dr, rd, c); }
__attribute__((visibility("default"))) EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{ return shim_eglDestroyContext(d, c); }

static void *shim_lookup_egl(const char *name)
{
    if (!name)
        return NULL;
    if (!strcmp(name, "eglGetProcAddress"))
        return (void *)shim_eglGetProcAddress;
    if (!strcmp(name, "eglSwapInterval"))
        return (void *)shim_egl_swap_interval;
    if (!strcmp(name, "eglBindAPI"))
        return (void *)shim_eglBindAPI;
    if (!strcmp(name, "eglChooseConfig"))
        return (void *)shim_eglChooseConfig;
    if (!strcmp(name, "eglCreateContext"))
        return (void *)shim_eglCreateContext;
    if (!strcmp(name, "eglMakeCurrent"))
        return (void *)shim_eglMakeCurrent;
    if (!strcmp(name, "eglDestroyContext"))
        return (void *)shim_eglDestroyContext;
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* 3.0.1-3.0.4: lazy GL pointers, context registry, features, errors          */
/* -------------------------------------------------------------------------- */

/* Load libGLESv2 once and fill every real_* pointer from GLFUNCS. Prefer
   RTLD_NOLOAD so an already-loaded Mesa instance is reused; falling back to
   a fresh dlopen keeps early calls working before SDL loads the library. */
static void ensure_gl_ptrs(void)
{
    shim_ensure_init();
    void *h = ((void *(*)(const char *, int))real_dlopen_fn)(
        "libGLESv2.so.2", RTLD_NOLOAD | RTLD_LAZY);
    if (!h)
        h = ((void *(*)(const char *, int))real_dlopen_fn)(
            "libGLESv2.so.2", shim_dl_flags("libGLESv2.so.2", RTLD_LAZY | RTLD_GLOBAL));
    if (!h) {
        shim_trace("ensure_gl_ptrs: libGLESv2.so.2 unavailable\n");
        return;
    }
#define LOAD(ret, name, args) \
    real_##name = (ret (*)args)((void *(*)(void *, const char *))real_dlsym_fn)(h, #name);
    GLFUNCS(LOAD)
#undef LOAD
    if (!real_glGetError || !real_glGetString || !real_glDrawArrays) {
        shim_trace("ensure_gl_ptrs: core symbols missing\n");
    }
}

static void ensure_gl_symbols(void)
{
    pthread_once(&g_gl_once, ensure_gl_ptrs);
}

/* Resolve an extension entry point through the real eglGetProcAddress.
   Used for capability detection and for EXT/OES aliases advertised by the
   shim; never calls the shim's own hook (that would recurse). */
static void *ensure_gl_ext(const char *name)
{
    ensure_gl_symbols();
    if (!name)
        return NULL;
    if (!real_egl_getproc)
        ensure_egl_real("eglGetProcAddress", &real_egl_getproc);
    if (real_egl_getproc)
        return ((void *(*)(const char *))real_egl_getproc)(name);
    return NULL;
}

static int real_ext_has(const char *ext, const char *name)
{
    if (!ext || !name)
        return 0;
    size_t n = strlen(name);
    const char *p = ext;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *end = strchr(p, ' ');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !memcmp(p, name, n))
            return 1;
        if (!end)
            break;
        p = end;
    }
    return 0;
}

/* GL 1.x defaults for a fresh context: identity matrix stacks, white
   current color, unit point size, one white light, standard materials and
   GL_MODULATE texenv. Fixed-function lights are directional and off unless
   the app enables them, as required by the spec. */
static void init_ctx_defaults(ShimCtx *c)
{
    identity(c->mv_stack[0]);
    identity(c->proj_stack[0]);
    for (int u = 0; u < 8; u++)
        identity(c->tex_stack[u][0]);
    c->mv_top = c->proj_top = 0;
    memset(c->tex_top, 0, sizeof(c->tex_top));
    c->client_active_tex = c->active_tex = 0;
    c->matrix_mode = 0x1700; /* GL_MODELVIEW */
    c->current_color[0] = c->current_color[1] = c->current_color[2] = c->current_color[3] = 1.0f;
    c->current_normal[2] = 1.0f;
    for (int u = 0; u < 8; u++) {
        c->current_texcoord[u][0] = c->current_texcoord[u][1] = 0.0f;
        c->current_texcoord[u][2] = 0.0f;
        c->current_texcoord[u][3] = 1.0f;
    }
    c->secondary_color[3] = 1.0f;
    c->point_size = 1.0f;
    c->point_size_set = 0;
    c->tex_matrix_touched = 0;
    c->attrib_top = 0;
    c->alpha_test = 0;
    c->shade_model = GL_SMOOTH;
    c->alpha_func = GL_ALWAYS;
    for (int i = 0; i < 8; i++) {
        c->texenv_mode[i] = GL_MODULATE;
        memset(c->texenv_color[i], 0, sizeof(c->texenv_color[i]));
        c->ff.light[i].ambient[3] = 1.0f;
        c->ff.light[i].position[2] = 1.0f;
        c->ff.light[i].position[3] = 0.0f;
        c->ff.light[i].spotDirection[2] = -1.0f;
        c->ff.light[i].spotCutoff = 180.0f;
        c->ff.light[i].constantAttenuation = 1.0f;
        c->light_spot_cutoff[i] = 180.0f;
    }
    c->ff.light[0].diffuse[0] = c->ff.light[0].diffuse[1] =
        c->ff.light[0].diffuse[2] = c->ff.light[0].diffuse[3] = 1.0f;
    c->ff.light[0].specular[0] = c->ff.light[0].specular[1] =
        c->ff.light[0].specular[2] = c->ff.light[0].specular[3] = 1.0f;
    for (int i = 0; i < 2; i++) {
        ShimFFMaterial *m = i ? &c->ff.back_mat : &c->ff.front_mat;
        m->ambient[0] = m->ambient[1] = m->ambient[2] = 0.2f;
        m->ambient[3] = 1.0f;
        m->diffuse[0] = m->diffuse[1] = m->diffuse[2] = 0.8f;
        m->diffuse[3] = 1.0f;
        m->specular[3] = 1.0f;
        m->emission[3] = 1.0f;
    }
    c->ff.lm_ambient[0] = c->ff.lm_ambient[1] = c->ff.lm_ambient[2] = 0.2f;
    c->ff.lm_ambient[3] = 1.0f;
    c->ff.fog.density = 1.0f;
    c->ff.fog.end = 1.0f;
    c->ff.normal_scale = 1.0f;
}

/* Query the real driver once per context: GLES version, extension list,
   limits and opt-in G.3 prerequisites (compute + TF + >=64 MiB SSBO + at
   least 128 work-group invocations). Feature bits decide which wrappers
   rewrite calls and which capabilities the shim may advertise. */
static void detect_features(ShimCtx *c)
{
    if (c->features.ready || !real_glGetString)
        return;
    const GLubyte *vs = real_glGetString(GL_VERSION);
    if (vs && sscanf((const char *)vs, "OpenGL ES %d.%d",
                     &c->features.es_major, &c->features.es_minor) != 2) {
        c->features.es_major = 3;
        c->features.es_minor = 0;
    }
    /* A software renderer means every texture upload and draw runs on the
       CPU. Never silently degrade: make it explicit in the debug trace. */
    const GLubyte *renderer = real_glGetString(GL_RENDERER);
    c->features.software_renderer = renderer &&
        (strstr((const char *)renderer, "llvmpipe") ||
         strstr((const char *)renderer, "swrast") ||
         strstr((const char *)renderer, "softpipe") ||
         strstr((const char *)renderer, "software") ||
         strstr((const char *)renderer, "Software"));
    if (renderer)
        shim_trace("renderer: %s%s\n", (const char *)renderer,
                   c->features.software_renderer ? " [SOFTWARE]" : "");
    char extbuf[8192];
    extbuf[0] = 0;
    const GLubyte *ext = real_glGetString(GL_EXTENSIONS);
    if (ext && *ext) {
        snprintf(extbuf, sizeof(extbuf), "%s", (const char *)ext);
    } else if (real_glGetStringi && real_glGetIntegerv) {
        GLint count = 0;
        real_glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        size_t at = 0;
        for (GLint i = 0; i < count && i < 256; i++) {
            const GLubyte *e = real_glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (!e)
                continue;
            int n = snprintf(extbuf + at, sizeof(extbuf) - at, "%s%s", at ? " " : "", e);
            if (n <= 0 || (size_t)n >= sizeof(extbuf) - at)
                break;
            at += (size_t)n;
        }
    }
    c->features.has_bgra8888 = real_ext_has(extbuf, "GL_EXT_texture_format_BGRA8888");
    c->features.has_s3tc = real_ext_has(extbuf, "GL_EXT_texture_compression_s3tc") ||
                           real_ext_has(extbuf, "GL_EXT_texture_compression_s3tc_srgb");
    c->features.has_color_buffer_float = real_ext_has(extbuf, "EXT_color_buffer_float");
    c->features.has_border_clamp = real_ext_has(extbuf, "GL_EXT_texture_border_clamp") ||
                                   real_ext_has(extbuf, "GL_OES_texture_border_clamp");
    c->features.has_clip_cull_distance = real_ext_has(extbuf, "GL_EXT_clip_cull_distance");
    c->features.has_compute = c->features.es_major > 3 ||
                              (c->features.es_major == 3 && c->features.es_minor >= 1);
    c->features.has_tf = c->features.es_major >= 3;
    c->features.has_ssbo = c->features.has_compute;
    c->features.max_vertex_attribs = 16;
    c->features.max_ubo_bindings = 24;
    c->features.max_ubo_block_size = 16384;
    GLint inv = 0, ssbo = 0;
    if (real_glGetIntegerv) {
        real_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &c->features.max_vertex_attribs);
        real_glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &c->features.max_ubo_bindings);
        real_glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &c->features.max_ubo_block_size);
        real_glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &inv);
        real_glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &ssbo);
        c->features.max_compute_invocations = inv;
        c->features.ssbo_max_mb = (int)((unsigned)ssbo / (1024u * 1024u));
        c->features.geom_emu = g_geom_requested && c->features.has_compute &&
                               c->features.has_tf && inv >= 128 &&
                               ssbo >= (int)(64u * 1024u * 1024u);
        c->features.tess_emu = g_tess_requested && c->features.geom_emu;
    }
    if (c->features.geom_emu || c->features.tess_emu)
        shim_probe_emu(c);
    else if (ssbo < (int)(64u * 1024u * 1024u) && (g_geom_requested || g_tess_requested))
        shim_trace("Phase 0: max SSBO %d < 64 MiB; emulation disabled\n", ssbo);
    c->features.has_base_vertex = c->features.es_major > 3 ||
        (c->features.es_major == 3 && c->features.es_minor >= 2) ||
        ensure_gl_ext("glDrawElementsBaseVertexEXT") != NULL ||
        ensure_gl_ext("glDrawElementsBaseVertexOES") != NULL;
    c->features.ready = 1;
    shim_trace("features: ES %d.%d s3tc=%d bgra=%d sw=%d clip=%d attrs=%d ubo=%d geom=%d tess=%d\n",
               c->features.es_major, c->features.es_minor, c->features.has_s3tc,
               c->features.has_bgra8888, c->features.software_renderer,
               c->features.has_clip_cull_distance, c->features.max_vertex_attribs,
               c->features.max_ubo_bindings, c->features.geom_emu, c->features.tess_emu);
}

/* Return the ShimCtx for the currently bound EGLContext, creating it on
   first use. Locking is only needed for registry lookup/insertion; after
   that the per-thread pointer makes repeated GL calls lock-free. */
static ShimCtx *shim_get_ctx(void)
{
    if (tls_ctx_ptr)
        return tls_ctx_ptr;
    if (!tls_ctx_handle)
        return NULL;
    ensure_gl_symbols();
    pthread_mutex_lock(&g_ctx_mutex);
    ShimCtx *c = g_ctx_registry;
    while (c && c->egl != tls_ctx_handle)
        c = c->next;
    int created = 0;
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (c) {
            c->egl = tls_ctx_handle;
            c->next = g_ctx_registry;
            g_ctx_registry = c;
            created = 1;
        }
    }
    pthread_mutex_unlock(&g_ctx_mutex);
    if (!c)
        return NULL;
    /* Only the creator initialises: switching contexts must not reset state. */
    if (created)
        init_ctx_defaults(c);
    detect_features(c);
    tls_ctx_ptr = c;
    return c;
}

/* -------------------------------------------------------------------------- */
/* H.4: display-list recording                                                 */
/*                                                                            */
/* GLES has no glNewList/glCallList. Commands are stored as compact records   */
/* (opcode + two ints + floats) and replayed through the normal wrappers so   */
/* replay sees exactly the same state translation as a direct call.           */
/* -------------------------------------------------------------------------- */

static void shim_dl_replay_cmd(ShimCtx *, const ShimDLCmd *);

/* Names are 1-based and the table is grown on demand; glGenLists reserves
   a contiguous range by marking slots generated. create==0 is a pure lookup
   (used by glIsList and replay), create==1 allocates on first use. */
static ShimDL *shim_dl_slot(ShimCtx *c, GLuint name, int create)
{
    if (!c || !name) return NULL;
    size_t idx = (size_t)name - 1;
    if (idx >= c->dl_table_n) {
        if (!create) return NULL;
        size_t nn = idx + 1;
        ShimDL **t = realloc(c->dl_table, nn * sizeof(*t));
        if (!t) return NULL;
        memset(t + c->dl_table_n, 0, (nn - c->dl_table_n) * sizeof(*t));
        c->dl_table = t;
        c->dl_table_n = nn;
    }
    if (!c->dl_table[idx] && create) {
        c->dl_table[idx] = calloc(1, sizeof(ShimDL));
    }
    return c->dl_table[idx];
}

/* Append one command to the active list. Called from the wrappers, never
   from the real driver, so only calls that the shim can replay are stored;
   unsupported calls are silently omitted (documented subset of H.4). */
static void shim_dl_record(ShimCtx *c, ShimDLOp op, int i0, int i1,
                           const float *f, size_t nf)
{
    if (!c || !c->dl_active) return;
    ShimDL *l = c->dl_active;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        ShimDLCmd *p = realloc(l->cmds, nc * sizeof(*p));
        if (!p) return;
        l->cmds = p;
        l->cap = nc;
    }
    ShimDLCmd *cmd = &l->cmds[l->n++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->op = op;
    cmd->i[0] = i0;
    cmd->i[1] = i1;
    if (f && nf) memcpy(cmd->f, f, nf * sizeof(float) < sizeof(cmd->f) ?
                        nf * sizeof(float) : sizeof(cmd->f));
}

/* Extension string is built once, from the REAL driver list plus shim aliases. */
static const char *shim_extra_exts[] = {
    "GL_ARB_framebuffer_object", "GL_EXT_framebuffer_object",
    "GL_EXT_framebuffer_blit", "GL_EXT_framebuffer_multisample",
    "GL_ARB_vertex_array_object", "GL_ARB_texture_non_power_of_two",
    "GL_ARB_texture_float", "GL_ARB_half_float_pixel", "GL_ARB_texture_rg",
    "GL_ARB_draw_buffers", "GL_ARB_instanced_arrays", "GL_ARB_draw_instanced",
    "GL_ARB_map_buffer_range", "GL_ARB_uniform_buffer_object",
    "GL_ARB_sampler_objects", "GL_EXT_texture_compression_s3tc", "GL_EXT_bgra"
};

static int g_ext_ready;

/* Build the string returned by glGetString(GL_EXTENSIONS) in desktop mode:
   the real driver's list plus desktop-only aliases the shim implements.
   Cached, because some engines query it every frame; glGetStringi() uses
   the token array built here to stay consistent. */
static void build_ext_string(void)
{
    if (g_ext_ready)
        return;
    ensure_gl_symbols();
    StrBuf sb = {0};
    const GLubyte *native = real_glGetString ? real_glGetString(GL_EXTENSIONS) : NULL;
    if (native && *native)
        sb_add(&sb, (const char *)native);
    else if (real_glGetStringi && real_glGetIntegerv) {
        GLint n = 0;
        real_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n && i < 256; i++) {
            const GLubyte *e = real_glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (!e)
                continue;
            if (sb.len)
                sb_add(&sb, " ");
            sb_add(&sb, (const char *)e);
        }
    }
    for (size_t i = 0; i < sizeof(shim_extra_exts) / sizeof(shim_extra_exts[0]); i++) {
        if (real_ext_has(sb.data, shim_extra_exts[i]))
            continue;
        if (sb.len)
            sb_add(&sb, " ");
        sb_add(&sb, shim_extra_exts[i]);
    }
    if (g_ff_enabled) {
        ShimCtx *c = shim_get_ctx();
        if (c && c->features.has_clip_cull_distance && !real_ext_has(sb.data, "GL_EXT_clip_cull_distance"))
            sb_add(&sb, " GL_EXT_clip_cull_distance");
    }
    free(g_ext_string);
    g_ext_string = sb.data;
    if (!g_ext_string)
        g_ext_string = strdup("");
    free(g_ext_tokens);
    g_ext_tokens = NULL;
    g_ext_count = 0;
    char *tmp = strdup(g_ext_string);
    if (tmp) {
        for (char *p = strtok(tmp, " "); p; p = strtok(NULL, " "))
            g_ext_count++;
        free(tmp);
        g_ext_tokens = calloc((size_t)g_ext_count, sizeof(char *));
        tmp = strdup(g_ext_string);
        int i = 0;
        for (char *p = strtok(tmp, " "); p && i < g_ext_count; p = strtok(NULL, " "), i++)
            g_ext_tokens[i] = strdup(p);
        free(tmp);
    }
    g_ext_ready = 1;
}

static void ensure_ext_ready(void)
{
    pthread_mutex_lock(&g_ext_mutex);
    if (!g_ext_ready)
        build_ext_string();
    pthread_mutex_unlock(&g_ext_mutex);
}

static void drain_errors(void)
{
    if (!real_glGetError)
        return;
    for (int i = 0; i < 8; i++)
        if (real_glGetError() == 0)
            break;
}

/* Policy 3.0.4: drain the real GL error queue but return the first error
   found. The app's own glGetError() still sees the remaining errors, so
   internal draining never swallows a diagnostic it asked for. */
static GLenum drain_errors_return(void)
{
    if (!real_glGetError)
        return 0;
    GLenum first = real_glGetError();
    /* Keep the first error (that is what glGetError() would have returned to
       the caller) while draining the rest of the queue. */
    for (int i = 0; i < 8 && first != 0; i++)
        if (real_glGetError() == 0) break;
    return first;
}

#define SHIM_EXPORT(ret, name, args) \
    __attribute__((visibility("default"))) ret name args

/* -------------------------------------------------------------------------- */
/* MODULE A: identity, versions and extensions                                */
/* -------------------------------------------------------------------------- */

static void build_version_strings(void)
{
    if (g_ver_ready)
        return;
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    if (!g_ver_forced && c && c->features.ready && c->features.es_major == 3 &&
        c->features.es_minor < 1) {
        g_ver_major = 3;
        g_ver_minor = 0;
    }
    if (g_ver_major >= 3 && g_ver_minor >= 2 && c && !c->features.tess_emu) {
        /* Without tessellation emulation we must not advertise 3.2+ by default. */
        if (!g_ver_forced) {
            g_ver_major = 3;
            g_ver_minor = 1;
        }
    }
    const char *glsl = "1.40";
    if (g_ver_major == 3) {
        if (g_ver_minor == 0) glsl = "1.30";
        else if (g_ver_minor == 1) glsl = "1.40";
        else if (g_ver_minor == 2) glsl = "1.50";
        else glsl = "3.30";
    }
    snprintf(g_glsl_str, sizeof(g_glsl_str), "%s", glsl);

    const char *native = "";
    if (real_glGetString) {
        const GLubyte *s = real_glGetString(GL_VERSION);
        if (s && *s)
            native = (const char *)s;
    }
    const char *mesa = strstr(native, "Mesa ");
    if (!mesa && strncmp(native, "OpenGL ES", 9) != 0)
        mesa = native;
    if (g_ver_forced || strncmp(native, "OpenGL ES", 9) == 0) {
        const char *profile = (g_ver_major > 3 || (g_ver_major == 3 && g_ver_minor >= 2))
                              ? " (Compatibility Profile)" : "";
        snprintf(g_ver_str, sizeof(g_ver_str), "%d.%d%s Mesa %s (nuubOS Panfrost)",
                 g_ver_major, g_ver_minor, profile,
                 mesa ? mesa : (native[0] ? native : "unknown"));
    } else {
        snprintf(g_ver_str, sizeof(g_ver_str), "%s", native);
    }
    g_ver_ready = 1;
}

static const GLubyte *shim_glGetString(GLenum name)
{
    ensure_gl_symbols();
    if (!real_glGetString)
        return NULL;
    if (!desktop_mode())
        return real_glGetString(name);
    switch (name) {
    case GL_VERSION: {
        const GLubyte *native = real_glGetString(GL_VERSION);
        if (!native || strncmp((const char *)native, "OpenGL ES", 9) != 0)
            return native;
        build_version_strings();
        return (const GLubyte *)g_ver_str;
    }
    case GL_SHADING_LANGUAGE_VERSION:
        build_version_strings();
        return (const GLubyte *)g_glsl_str;
    case GL_EXTENSIONS:
        ensure_ext_ready();
        return (const GLubyte *)(g_ext_string ? g_ext_string : "");
    default:
        return real_glGetString(name);
    }
}

static const GLubyte *shim_glGetStringi(GLenum name, GLuint index)
{
    ensure_gl_symbols();
    if (desktop_mode() && name == GL_EXTENSIONS) {
        ensure_ext_ready();
        if (index < (GLuint)g_ext_count && g_ext_tokens)
            return (const GLubyte *)g_ext_tokens[index];
        return NULL;
    }
    if (!real_glGetStringi)
        return NULL;
    return real_glGetStringi(name, index);
}

static void shim_glGetIntegerv(GLenum pname, GLint *data)
{
    ensure_gl_symbols();
    if (!data || !real_glGetIntegerv)
        return;
    if (!desktop_mode()) {
        real_glGetIntegerv(pname, data);
        return;
    }
    switch (pname) {
    case GL_CONTEXT_PROFILE_MASK:
        build_version_strings();
        *data = (g_ver_major > 3 || (g_ver_major == 3 && g_ver_minor >= 2))
                ? GL_CONTEXT_COMPATIBILITY_PROFILE_BIT : 0;
        return;
    case GL_MATRIX_MODE: {
        ShimCtx *c = shim_get_ctx();
        *data = c ? c->matrix_mode : 0x1700;
        return;
    }
    default:
        break;
    }
    real_glGetIntegerv(pname, data);
    switch (pname) {
    case GL_NUM_EXTENSIONS:
        ensure_ext_ready();
        *data = g_ext_count;
        break;
    case GL_MAJOR_VERSION:
        build_version_strings();
        *data = g_ver_major;
        break;
    case GL_MINOR_VERSION:
        build_version_strings();
        *data = g_ver_minor;
        break;
    default:
        break;
    }
}

static void shim_glGetFloatv(GLenum pname, GLfloat *data)
{
    ensure_gl_symbols();
    if (!data)
        return;
    if (!desktop_mode()) {
        if (real_glGetFloatv)
            real_glGetFloatv(pname, data);
        return;
    }
    ShimCtx *c = shim_get_ctx();
    int unit = 0;
    if (c && real_glGetIntegerv)
        real_glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    unit -= (int)GL_TEXTURE0;
    if (unit < 0 || unit >= 8) unit = 0;
    switch (pname) {
    case GL_MODELVIEW_MATRIX:
        if (c) memcpy(data, c->mv_stack[c->mv_top], 16 * sizeof(float));
        return;
    case GL_PROJECTION_MATRIX:
        if (c) memcpy(data, c->proj_stack[c->proj_top], 16 * sizeof(float));
        return;
    case GL_TEXTURE_MATRIX:
        if (c) memcpy(data, c->tex_stack[unit][c->tex_top[unit]], 16 * sizeof(float));
        return;
    case GL_CURRENT_COLOR:
        if (c) memcpy(data, c->current_color, 4 * sizeof(float));
        return;
    case GL_CURRENT_NORMAL:
        if (c) memcpy(data, c->current_normal, 3 * sizeof(float));
        return;
    case GL_CURRENT_TEXTURE_COORDS:
        if (c) memcpy(data, c->current_texcoord[unit], 4 * sizeof(float));
        return;
    case GL_POINT_SIZE:
        if (c) *data = c->point_size;
        return;
    default:
        break;
    }
    if (real_glGetFloatv)
        real_glGetFloatv(pname, data);
}

/* glGetDoublev is absent from GLES. Read floats from the shim/driver and
   widen only the number of components the requested pname actually has;
   writing all 16 would overwrite the caller's buffer for scalar queries. */
SHIM_EXPORT(void, glGetDoublev, (GLenum pname, GLdouble *data))
{
    ensure_gl_symbols();
    if (!data)
        return;
    if (!desktop_mode()) {
        if (real_glGetFloatv) {
            GLfloat tmp[16] = {0};
            real_glGetFloatv(pname, tmp);
            int n = 1;
            if (pname == 0x0BA2 /* GL_VIEWPORT */ || pname == 0x0C22 /* COLOR_CLEAR_VALUE */ ||
                pname == 0x0C10 /* SCISSOR_BOX */) n = 4;
            else if (pname == 0x0B72 /* DEPTH_RANGE */) n = 2;
            for (int i = 0; i < n; i++) data[i] = tmp[i];
        }
        return;
    }
    GLfloat tmp[16] = {0};
    shim_glGetFloatv(pname, tmp);
    int n = 1;
    if (pname == 0x0BA2 || pname == 0x0C22 || pname == 0x0C10) n = 4;
    else if (pname == 0x0B72) n = 2;
    else if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX ||
             pname == GL_TEXTURE_MATRIX || pname == GL_CURRENT_COLOR ||
             pname == GL_CURRENT_TEXTURE_COORDS) n = 4;
    for (int i = 0; i < n; i++) data[i] = tmp[i];
}

SHIM_EXPORT(GLenum, glGetError, (void))
{
    ensure_gl_symbols();
    ShimCtx *c = tls_ctx_ptr;
    if (c && c->pending_error) {
        GLenum e = c->pending_error;
        c->pending_error = 0;
        return e;
    }
    return real_glGetError ? real_glGetError() : (GLenum)0;
}

/* -------------------------------------------------------------------------- */
/* MODULE B: double -> float at entry points                                  */
/*                                                                            */
/* Desktop GL has glClearDepth/glDepthRange with GLdouble parameters and      */
/* glUniform*dv; GLES only has the *f variants. Values are narrowed here and  */
/* again in the uniform wrappers below (TLS scratch converts arrays).         */
/* -------------------------------------------------------------------------- */

SHIM_EXPORT(void, glClearDepth, (GLdouble depth))
{
    ensure_gl_symbols();
    if (!desktop_mode() || !real_glClearDepthf)
        return;
    real_glClearDepthf((GLfloat)depth);
}

SHIM_EXPORT(void, glDepthRange, (GLdouble n, GLdouble f))
{
    ensure_gl_symbols();
    if (!desktop_mode() || !real_glDepthRangef)
        return;
    real_glDepthRangef((GLfloat)n, (GLfloat)f);
}

/* -------------------------------------------------------------------------- */
/* MODULE C: default framebuffer                                              */
/*                                                                            */
/* Desktop GL exposes GL_FRONT/GL_BACK/GL_FRONT_AND_BACK and multiple draw    */
/* buffers on the default framebuffer. GLES only has GL_BACK and FBO 0, so    */
/* the enums are normalised and buffer calls are dropped when no FBO is bound.*/
/* -------------------------------------------------------------------------- */

static int default_fbo_bound(GLenum binding)
{
    GLint fbo = 0;
    if (real_glGetIntegerv)
        real_glGetIntegerv(binding, &fbo);
    return fbo == 0;
}

static GLenum normalize_draw_buffer(GLenum mode)
{
    switch (mode) {
    case GL_FRONT: case GL_FRONT_AND_BACK: case GL_FRONT_LEFT:
    case GL_FRONT_RIGHT: case GL_BACK_LEFT: case GL_BACK_RIGHT:
        return GL_BACK;
    default:
        return mode;
    }
}

SHIM_EXPORT(void, glDrawBuffer, (GLenum mode))
{
    ensure_gl_symbols();
    if (!desktop_mode() || !real_glDrawBuffers)
        return;
    if (default_fbo_bound(GL_DRAW_FRAMEBUFFER_BINDING))
        mode = normalize_draw_buffer(mode);
    real_glDrawBuffers(1, &mode);
}

static void shim_glDrawBuffers(GLsizei n, const GLenum *bufs)
{
    ensure_gl_symbols();
    if (!real_glDrawBuffers)
        return;
    if (!desktop_mode() || !bufs) {
        real_glDrawBuffers(n, bufs);
        return;
    }
    if (n == 1 && default_fbo_bound(GL_DRAW_FRAMEBUFFER_BINDING)) {
        GLenum m = normalize_draw_buffer(bufs[0]);
        real_glDrawBuffers(1, &m);
        return;
    }
    real_glDrawBuffers(n, bufs);
}

SHIM_EXPORT(void, glReadBuffer, (GLenum mode))
{
    ensure_gl_symbols();
    if (!desktop_mode() || !real_glReadBuffer)
        return;
    if (default_fbo_bound(GL_READ_FRAMEBUFFER_BINDING) &&
        (mode == GL_FRONT || mode == GL_FRONT_LEFT || mode == GL_BACK_LEFT))
        mode = GL_BACK;
    real_glReadBuffer(mode);
}

/* -------------------------------------------------------------------------- */
/* MODULE D: glMapBuffer -> glMapBufferRange                                  */
/*                                                                            */
/* glMapBuffer(GL_WRITE_ONLY) is implemented with GL_MAP_WRITE_BIT only.      */
/* Adding GL_MAP_INVALIDATE_BUFFER_BIT would discard the existing contents,   */
/* which desktop GL does not guarantee (ports update vertex data in place).   */
/* -------------------------------------------------------------------------- */

static void *shim_glMapBuffer(GLenum target, GLenum access)
{
    ensure_gl_symbols();
    if (!desktop_mode() || !real_glMapBufferRange || !real_glGetBufferParameteriv)
        return NULL;
    GLint size = 0;
    real_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (size <= 0)
        return NULL;
    GLbitfield flags = 0;
    switch (access) {
    case GL_READ_ONLY:  flags = GL_MAP_READ_BIT; break;
    case GL_WRITE_ONLY: flags = GL_MAP_WRITE_BIT; break; /* no INVALIDATE_BUFFER */
    case GL_READ_WRITE: flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
    default: break;
    }
    return real_glMapBufferRange(target, 0, (GLsizeiptr)size, flags);
}

SHIM_EXPORT(void *, glMapBuffer, (GLenum target, GLenum access))
{ return shim_glMapBuffer(target, access); }
SHIM_EXPORT(void *, glMapBufferARB, (GLenum target, GLenum access))
{ return shim_glMapBuffer(target, access); }
SHIM_EXPORT(GLboolean, glUnmapBufferARB, (GLenum target))
{
    ensure_gl_symbols();
    return real_glUnmapBuffer ? real_glUnmapBuffer(target) : GL_FALSE;
}

/* -------------------------------------------------------------------------- */
/* MODULE E: textures, BGRA swizzle, 1D, readback and CPU DXT                 */
/*                                                                            */
/* GL_TEXTURE_1D becomes a 2D texture of height 1. GL_BGR/GL_BGRA uploads are  */
/* swizzled into RGB/RGBA using UNPACK_* state (PBO included). S3TC is decoded */
/* on the CPU when the driver lacks the extension. glGetTexImage is emulated  */
/* with an FBO readback because GLES has no such entry point.                  */
/* -------------------------------------------------------------------------- */

/* Keep a single storage target in GLES: all 1D state, binds and uploads
   silently use TEXTURE_2D with height 1. */
static GLenum remap_tex_target(GLenum target)
{ return target == GL_TEXTURE_1D ? GL_TEXTURE_2D : target; }

static int ff_has_mipmap_id(GLuint id)
{
    ShimCtx *c = tls_ctx_ptr;
    if (!c) return 0;
    for (int i = 0; i < 64; i++)
        if ((GLuint)c->gen_mipmap_ids[i] == id && id != 0)
            return 1;
    return 0;
}

/* GL_GENERATE_MIPMAP is a desktop-only texture parameter. Track the ids
   that requested it and generate the levels after the next upload instead. */
static void ff_set_mipmap_id(GLuint id, int on)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if ((GLuint)c->gen_mipmap_ids[i] == id) { slot = i; break; }
        if (slot < 0 && c->gen_mipmap_ids[i] == 0) slot = i;
    }
    if (on && id) {
        if (slot < 0) slot = c->gen_mipmap_pos++ & 63;
        c->gen_mipmap_ids[slot] = id;
    } else if (slot >= 0) {
        c->gen_mipmap_ids[slot] = 0;
    }
}

/* GL_CLAMP and GL_CLAMP_TO_BORDER are not universally available in GLES;
   substitute GL_CLAMP_TO_EDGE unless the driver advertises border clamp. */
static GLint clamp_param_value(GLenum pname, GLint v, ShimCtx *c)
{
    if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T ||
         pname == GL_TEXTURE_WRAP_R) && v == GL_CLAMP)
        return GL_CLAMP_TO_EDGE;
    if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T ||
         pname == GL_TEXTURE_WRAP_R) && v == GL_CLAMP_TO_BORDER &&
        (!c || !c->features.has_border_clamp))
        return GL_CLAMP_TO_EDGE;
    return v;
}

static void shim_glBindTexture(GLenum target, GLuint texture)
{
    shim_dl_record(tls_ctx_ptr, DL_BIND_TEXTURE, (int)target, (int)texture, NULL, 0);
    ensure_gl_symbols();
    if (real_glBindTexture)
        real_glBindTexture(remap_tex_target(target), texture);
}

/* Debug-only: record sampler-state changes per texture instead of the first
   N calls, so a scene the user triggers at any time remains visible. */
static void trace_texparam_change(GLint id, GLenum pname, GLint param)
{
    if (!g_debug || id <= 0)
        return;
    static struct {
        GLint id, min, gen, max;
        unsigned seen;
    } states[512];
    static int count, initialised;
    if (!initialised) {
        for (int i = 0; i < 512; i++) {
            states[i].min = states[i].gen = states[i].max = INT_MIN;
        }
        initialised = 1;
    }
    int slot = -1;
    for (int i = 0; i < count; i++)
        if (states[i].id == id) { slot = i; break; }
    if (slot < 0) {
        if (count >= 512) return;
        slot = count++;
        states[slot].id = id;
        states[slot].min = states[slot].gen = states[slot].max = INT_MIN;
        states[slot].seen = 0;
    }
    int index = pname == 0x2801 ? 0 : pname == 0x8191 ? 1 : 2;
    GLint *last = index == 0 ? &states[slot].min :
                  index == 1 ? &states[slot].gen : &states[slot].max;
    if ((states[slot].seen & (1u << index)) && *last == param)
        return;
    states[slot].seen |= 1u << index;
    *last = param;
    shim_trace("texparam: tex=%d pname=0x%x param=0x%x\n", id, pname, param);
}

static void shim_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    target = remap_tex_target(target);
    if (g_debug && (pname == 0x2801 || pname == 0x8191 || pname == 0x813D)) {
        GLint id = -1;
        if (real_glGetIntegerv) real_glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
        trace_texparam_change(id, pname, param);
    }
    if (pname == 0x8191) { /* GL_GENERATE_MIPMAP */
        GLint id = 0;
        if (real_glGetIntegerv) real_glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
        ff_set_mipmap_id((GLuint)id, param != 0);
        return;
    }
    if (pname == GL_TEXTURE_LOD_BIAS || pname == 0x884B || pname == 0x8066)
        return;
    if (real_glTexParameteri)
        real_glTexParameteri(target, pname, clamp_param_value(pname, param, c));
}

static void shim_glGenerateMipmap(GLenum target)
{
    if (g_debug) {
        static int n;
        if (n++ < 200) {
            GLint id = -1;
            if (real_glGetIntegerv) real_glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
            shim_trace("generatemipmap: target=0x%x tex=%d\n", target, id);
        }
    }
    ensure_gl_symbols();
    if (real_glGenerateMipmap) real_glGenerateMipmap(remap_tex_target(target));
}

static void shim_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_debug) {
        static int n;
        if (n++ < 40) shim_trace("viewport: %d %d %dx%d\n", x, y, w, h);
    }
    ensure_gl_symbols();
    if (real_glViewport) real_glViewport(x, y, w, h);
}

static void shim_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_debug) {
        static int n;
        if (n++ < 40) shim_trace("scissor: %d %d %dx%d\n", x, y, w, h);
    }
    ensure_gl_symbols();
    if (real_glScissor) real_glScissor(x, y, w, h);
}

static void shim_glBindFramebuffer(GLenum target, GLuint fb)
{
    if (g_debug) {
        static int n;
        if (n++ < 40) shim_trace("bindfbo: target=0x%x fb=%d\n", target, fb);
    }
    ensure_gl_symbols();
    if (real_glBindFramebuffer) real_glBindFramebuffer(target, fb);
}

static void shim_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    shim_glTexParameteri(target, pname, (GLint)param);
}

static void shim_glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    if (!params) return;
    /* Only GL_TEXTURE_BORDER_COLOR takes four values. Treating every other
       pname as a vector read params[1..3] from a caller buffer that holds a
       single int, and then applied it twice (wrapper + real driver). */
    if (pname == 0x1004) { /* GL_TEXTURE_BORDER_COLOR */
        GLint v[4];
        for (int i = 0; i < 4; i++) v[i] = params[i];
        ensure_gl_symbols();
        if (real_glTexParameteriv)
            real_glTexParameteriv(remap_tex_target(target), pname, v);
        return;
    }
    shim_glTexParameteri(target, pname, params[0]);
}

static void shim_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (!params) return;
    if (pname == 0x1004) { /* GL_TEXTURE_BORDER_COLOR */
        GLfloat v[4];
        for (int i = 0; i < 4; i++) v[i] = params[i];
        ensure_gl_symbols();
        if (real_glTexParameterfv)
            real_glTexParameterfv(remap_tex_target(target), pname, v);
        return;
    }
    shim_glTexParameteri(target, pname, (GLint)params[0]);
}

/* GLES requires a sized internal format for float/half-float textures;
   unsized GL_RGBA/GL_RGB/GL_RG/GL_RED are upgraded here. */
static GLenum sized_float_format(GLenum internal, GLenum format, GLenum type)
{
    if (internal != format) return internal;
    if (type == GL_FLOAT) {
        if (format == GL_RGBA) return GL_RGBA32F;
        if (format == GL_RGB) return GL_RGB32F;
        if (format == GL_RG) return GL_RG32F;
        if (format == GL_RED) return GL_R32F;
        if (format == GL_DEPTH_COMPONENT) return GL_DEPTH_COMPONENT32F;
    } else if (type == GL_HALF_FLOAT) {
        if (format == GL_RGBA) return GL_RGBA16F;
        if (format == GL_RGB) return GL_RGB16F;
        if (format == GL_RG) return GL_RG16F;
        if (format == GL_RED) return GL_R16F;
    }
    return internal;
}

static int is_s3tc(GLenum f)
{
    return (f >= 0x83F0 && f <= 0x83F3) || (f >= 0x8C4C && f <= 0x8C4F);
}

static int is_srgb_s3tc(GLenum f)
{ return f >= 0x8C4C && f <= 0x8C4F; }

/* Swizzle BGRA/BGR into compact RGBA/RGB using the app's unpack state.
   UNPACK_ROW_LENGTH, skips, alignment and a bound PBO are honoured so a
   sub-image upload lands at the same texels as it would in desktop GL. */
static uint8_t *swizzle_upload(GLsizei width, GLsizei height, GLenum src_format,
                               const void *pixels, GLsizei *out_w, GLsizei *out_h)
{
    ensure_gl_symbols();
    if (!pixels || !real_glGetIntegerv) return NULL;
    GLint row = 0, skip_r = 0, skip_p = 0, align = 4, pbo = 0;
    real_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    real_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_r);
    real_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_p);
    real_glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (align < 1) align = 1;
    int bpp = src_format == GL_BGRA ? 4 : 3;
    GLsizei rlen = row > 0 ? row : width;
    size_t stride = ((size_t)rlen * (size_t)bpp + (size_t)align - 1) &
                    ~((size_t)align - 1);
    const uint8_t *base = (const uint8_t *)pixels;
    void *mapped = NULL;
    if (pbo) {
        GLint sz = 0;
        if (!real_glGetBufferParameteriv) return NULL;
        real_glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &sz);
        if ((uintptr_t)pixels >= (uintptr_t)sz) return NULL;
        mapped = real_glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, sz, GL_MAP_READ_BIT);
        if (!mapped) return NULL;
        base = (const uint8_t *)mapped + (uintptr_t)pixels;
    }
    size_t dst_bpp = src_format == GL_BGRA ? 4 : 3;
    size_t need = (size_t)width * (size_t)height * dst_bpp;
    uint8_t *dst = scratch(need);
    if (!dst) {
        if (mapped && real_glUnmapBuffer) real_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        return NULL;
    }
    base += (size_t)skip_r * stride + (size_t)skip_p * (size_t)bpp;
    for (GLsizei y = 0; y < height; y++) {
        const uint8_t *s = base + (size_t)y * stride;
        uint8_t *d = dst + (size_t)y * (size_t)width * dst_bpp;
        for (GLsizei x = 0; x < width; x++, s += bpp, d += dst_bpp) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
            if (dst_bpp == 4) d[3] = s[3];
        }
    }
    if (mapped && real_glUnmapBuffer) real_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    *out_w = width;
    *out_h = height;
    return dst;
}

static void set_unpack_defaults(GLint *row, GLint *sr, GLint *sp, GLint *al, GLint *pbo)
{
    *row = *sr = *sp = *pbo = 0;
    *al = 4;
    if (!real_glGetIntegerv) return;
    real_glGetIntegerv(GL_UNPACK_ROW_LENGTH, row);
    real_glGetIntegerv(GL_UNPACK_SKIP_ROWS, sr);
    real_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, sp);
    real_glGetIntegerv(GL_UNPACK_ALIGNMENT, al);
    real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, pbo);
    if (real_glPixelStorei) {
        real_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        real_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        real_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        real_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (pbo) real_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
}

static void restore_unpack(const GLint *row, const GLint *sr, const GLint *sp,
                           const GLint *al, GLint pbo)
{
    if (!real_glPixelStorei) return;
    real_glPixelStorei(GL_UNPACK_ROW_LENGTH, *row);
    real_glPixelStorei(GL_UNPACK_SKIP_ROWS, *sr);
    real_glPixelStorei(GL_UNPACK_SKIP_PIXELS, *sp);
    real_glPixelStorei(GL_UNPACK_ALIGNMENT, *al);
    if (pbo && real_glBindBuffer) real_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)pbo);
}

/* Main upload path: remap 1D, upgrade unsized float formats, swizzle BGRA,
   then honour the app's unpack state. After a successful upload, generate
   mipmaps for textures that set GL_GENERATE_MIPMAP (see ff_set_mipmap_id). */
static void shim_glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void *pixels)
{
    ensure_gl_symbols();
    if (!real_glTexImage2D) return;
    /* Only needed for the opt-in BGRA extension path and its fallback;
       deferring it keeps the common (RGBA) upload free of context lookup. */
    ShimCtx *c = g_bgra_ext_mode ? shim_get_ctx() : NULL;
    target = remap_tex_target(target);
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV &&
        (format == GL_BGRA || format == GL_RGBA))
        type = GL_UNSIGNED_BYTE;
    if (internalFormat == 0x8040) internalFormat = GL_LUMINANCE;
    else if (internalFormat == 0x8045) internalFormat = GL_LUMINANCE_ALPHA;
    else if (internalFormat == 0x803C) internalFormat = GL_ALPHA;
    internalFormat = (GLint)sized_float_format((GLenum)internalFormat, format, type);
    if (internalFormat == GL_BGRA) internalFormat = GL_RGBA;
    if (internalFormat == GL_BGR) internalFormat = GL_RGB;

    int need_swizzle = (format == GL_BGRA || format == GL_BGR);
    if (c && c->features.has_bgra8888 && g_bgra_ext_mode && format == GL_BGRA &&
        pixels == NULL) need_swizzle = 0;
    if (need_swizzle && pixels && type == GL_UNSIGNED_BYTE) {
        GLsizei w = width, h = height;
        /* swizzle_upload reads the app's GL_UNPACK_* state (including a bound
           PBO), so it must run before those defaults are overwritten. */
        uint8_t *tmp = swizzle_upload(width, height, format, pixels, &w, &h);
        GLenum out_format = format == GL_BGRA ? GL_RGBA : GL_RGB;
        if (!tmp && format == GL_BGRA && !c) c = shim_get_ctx();
        if (!tmp && c && c->features.has_bgra8888 && format == GL_BGRA) {
            /* Conversion failed but the driver accepts BGRA natively: keep
               the caller's bytes and unpack state instead of uploading an
               undefined image or mislabelling BGRA pixels as RGBA. */
            real_glTexImage2D(target, level, internalFormat, width, height, border,
                              format, type, pixels);
        } else {
            GLint row, sr, sp, al, pbo;
            set_unpack_defaults(&row, &sr, &sp, &al, &pbo);
            real_glTexImage2D(target, level, internalFormat, width, height, border,
                              out_format, type, tmp);
            restore_unpack(&row, &sr, &sp, &al, pbo);
        }
    } else {
        if (format == GL_BGRA || format == GL_BGR) {
            if (pixels && type != GL_UNSIGNED_BYTE)
                shim_trace("BGRA with unsupported type (0x%x)\n", type);
            format = format == GL_BGRA ? GL_RGBA : GL_RGB;
        }
        real_glTexImage2D(target, level, internalFormat, width, height, border,
                          format, type, pixels);
    }
    GLint bound_id = -1;
    if (real_glGetIntegerv) real_glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_id);
    if (g_debug) {
        static int n_up, n_big;
        if (level > 0 && n_up++ < 120)
            shim_trace("texupload: tex=%d level=%d %dx%d ifmt=0x%x\n",
                       bound_id, level, width, height, internalFormat);
        if (level == 0 && width > 0 && height > 0 &&
            (size_t)width * (size_t)height >= 65536 && n_big++ < 200)
            shim_trace("texupload: tex=%d level=0 %dx%d fmt=0x%x type=0x%x pixels=%s\n",
                       bound_id, width, height, format, type, pixels ? "yes" : "null");
    }
    /* ff_has_mipmap_id() deliberately rejects id 0, so the old guard with a
       literal 0 could never fire. Check the texture actually bound instead;
       GL_GENERATE_MIPMAP was intercepted above and never reached the driver. */
    if (level == 0 && bound_id > 0 && real_glGenerateMipmap &&
        ff_has_mipmap_id((GLuint)bound_id)) {
        if (g_debug) {
            static int n_mip;
            if (n_mip++ < 60)
                shim_trace("texupload: tex=%d generating deferred mipmaps\n", bound_id);
        }
        real_glGenerateMipmap(target);
    }
}

static void shim_glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                                 GLsizei width, GLsizei height, GLenum format,
                                 GLenum type, const void *pixels)
{
    ensure_gl_symbols();
    if (!real_glTexSubImage2D) return;
    if (g_debug && level == 0 && width > 0 && height > 0) {
        static int n_sub;
        if (n_sub++ < 200) {
            GLint id = -1;
            if (real_glGetIntegerv) real_glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
            shim_trace("texsub: tex=%d at=%d,%d %dx%d fmt=0x%x type=0x%x pixels=%s\n",
                       id, xoff, yoff, width, height, format, type,
                       pixels ? "yes" : "null");
        }
    }
    target = remap_tex_target(target);
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV &&
        (format == GL_BGRA || format == GL_RGBA))
        type = GL_UNSIGNED_BYTE;
    if (format == GL_BGRA || format == GL_BGR) {
        if (pixels && type == GL_UNSIGNED_BYTE) {
            GLsizei w = width, h = height;
            /* See glTexImage2D: read the caller's unpack state before it is
               reset to defaults for the converted upload. */
            uint8_t *tmp = swizzle_upload(width, height, format, pixels, &w, &h);
            ShimCtx *c = !tmp && format == GL_BGRA ? shim_get_ctx() : NULL;
            if (!tmp && c && c->features.has_bgra8888 && format == GL_BGRA) {
                real_glTexSubImage2D(target, level, xoff, yoff, width, height,
                                     format, type, pixels);
            } else {
                GLint row, sr, sp, al, pbo;
                set_unpack_defaults(&row, &sr, &sp, &al, &pbo);
                real_glTexSubImage2D(target, level, xoff, yoff, width, height,
                                     format == GL_BGRA ? GL_RGBA : GL_RGB,
                                     type, tmp);
                restore_unpack(&row, &sr, &sp, &al, pbo);
            }
        } else {
            if (pixels && type != GL_UNSIGNED_BYTE)
                shim_trace("BGRA with unsupported type (0x%x)\n", type);
            format = format == GL_BGRA ? GL_RGBA : GL_RGB;
            real_glTexSubImage2D(target, level, xoff, yoff, width, height,
                                 format, type, pixels);
        }
    } else {
        real_glTexSubImage2D(target, level, xoff, yoff, width, height,
                             format, type, pixels);
    }
}

static void shim_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalFormat,
                                GLsizei width, GLsizei height)
{
    ensure_gl_symbols();
    if (!real_glTexStorage2D) return;
    ShimCtx *c = shim_get_ctx();
    if (is_s3tc(internalFormat) && (!c || !c->features.has_s3tc))
        internalFormat = is_srgb_s3tc(internalFormat) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    if (internalFormat == GL_BGRA) internalFormat = GL_RGBA8;
    target = remap_tex_target(target);
    real_glTexStorage2D(target, levels, internalFormat, width, height);
}

SHIM_EXPORT(void, glTexImage1D, (GLenum target, GLint level, GLint internalFormat,
                                 GLsizei width, GLint border, GLenum format,
                                 GLenum type, const void *pixels))
{
    shim_glTexImage2D(GL_TEXTURE_2D, level, internalFormat, width, 1, border,
                      format, type, pixels);
}

SHIM_EXPORT(void, glTexSubImage1D, (GLenum target, GLint level, GLint xoff,
                                    GLsizei width, GLenum format, GLenum type,
                                    const void *pixels))
{
    shim_glTexSubImage2D(GL_TEXTURE_2D, level, xoff, 0, width, 1, format, type, pixels);
}

/* glGetTexImage: FBO + glReadPixels, with packing respected on the copy.
   Attach the requested level to a temporary FBO, read it into GL_RGBA/ubyte
   and repack into the caller's format/type; other levels/targets fall back
   to a null result, which is the documented subset from the spec. */
SHIM_EXPORT(void, glGetTexImage, (GLenum target, GLint level, GLenum format,
                                  GLenum type, void *pixels))
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    if (!pixels || !c || !real_glGetIntegerv || !real_glFramebufferTexture2D)
        return;
    GLint pbo = 0;
    real_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    if (pbo) return;
    GLint id = 0, w = 0, h = 0;
    real_glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    if (real_glGetTexLevelParameteriv) {
        real_glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
        real_glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
    }
    if (id <= 0 || w <= 0 || h <= 0 || type != GL_UNSIGNED_BYTE) {
        shim_trace("glGetTexImage: unsupported format/level\n");
        return;
    }
    size_t npix = (size_t)w * (size_t)h;
    if (npix > SIZE_MAX / 4) return;
    uint8_t *rgba = scratch(npix * 4);
    if (!rgba) return;
    GLuint fbo = 0;
    if (real_glGenFramebuffers) real_glGenFramebuffers(1, &fbo);
    if (!fbo) return;
    GLint prev_fbo = 0, row, sr, sp, al;
    real_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_fbo);
    real_glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
    real_glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
    real_glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
    real_glGetIntegerv(GL_PACK_ALIGNMENT, &al);
    if (real_glPixelStorei) {
        real_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        real_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        real_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        real_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    if (real_glBindFramebuffer) real_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    real_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, (GLuint)id, level);
    GLenum status = real_glCheckFramebufferStatus
                    ? real_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) : 0;
    if (status == GL_FRAMEBUFFER_COMPLETE && real_glReadPixels) {
        drain_errors();
        real_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        drain_errors();
    } else {
        memset(rgba, 0, npix * 4);
        shim_trace("glGetTexImage: texture is not renderable\n");
    }
    if (real_glBindFramebuffer) real_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_fbo);
    if (fbo && real_glDeleteFramebuffers) real_glDeleteFramebuffers(1, &fbo);
    if (real_glPixelStorei) {
        real_glPixelStorei(GL_PACK_ROW_LENGTH, row);
        real_glPixelStorei(GL_PACK_SKIP_ROWS, sr);
        real_glPixelStorei(GL_PACK_SKIP_PIXELS, sp);
        real_glPixelStorei(GL_PACK_ALIGNMENT, al);
    }
    int bpp = (format == GL_RGBA || format == GL_BGRA) ? 4 :
              (format == GL_RGB || format == GL_BGR) ? 3 : 1;
    size_t rlen = row > 0 ? (size_t)row : (size_t)w;
    if (al < 1) al = 1;
    size_t stride = (rlen * (size_t)bpp + (size_t)al - 1) & ~((size_t)al - 1);
    uint8_t *dst = (uint8_t *)pixels + (size_t)sr * stride + (size_t)sp * (size_t)bpp;
    for (GLsizei y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * (size_t)w * 4;
        uint8_t *d = dst + (size_t)y * stride;
        for (GLsizei x = 0; x < w; x++, s += 4) {
            switch (format) {
            case GL_RGBA: memcpy(d, s, 4); d += 4; break;
            case GL_BGRA: d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d[3]=s[3]; d += 4; break;
            case GL_RGB:  memcpy(d, s, 3); d += 3; break;
            case GL_BGR:  d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d += 3; break;
            case GL_RED:  *d++ = s[0]; break;
            case GL_ALPHA:*d++ = s[3]; break;
            default:      memcpy(d, s, 4); d += 4; break;
            }
        }
    }
}

/* CPU decoding of DXT1/3/5. Used when Mesa does not expose the S3TC
   extension or rejects the compressed format; force_s3tc_enable (set at
   init) normally makes the native path succeed instead. */
static uint8_t *shim_dxt_decode(GLenum ifmt, GLsizei width, GLsizei height,
                                GLsizei imageSize, const void *data)
{
    int dxt1 = ifmt == 0x83F0 || ifmt == 0x83F1 || ifmt == 0x8C4C || ifmt == 0x8C4D;
    int dxt3 = ifmt == 0x83F2 || ifmt == 0x8C4E;
    int dxt5 = ifmt == 0x83F3 || ifmt == 0x8C4F;
    if (!dxt1 && !dxt3 && !dxt5) return NULL;
    /* A NULL data pointer means "allocate storage", not "decode blocks":
       MonoGame uses it for reserved compressed textures. Never dereference
       it in the software decoder; the caller forwards those to the driver. */
    if (!data) return NULL;
    int bb = dxt1 ? 8 : 16;
    size_t bw = ((size_t)width + 3) / 4, bh = ((size_t)height + 3) / 4;
    if ((size_t)imageSize < bw * bh * (size_t)bb) {
        shim_trace("DXT: insufficient imageSize\n");
        return NULL;
    }
    if ((size_t)width > SIZE_MAX / 4 || (size_t)height > SIZE_MAX / ((size_t)width * 4))
        return NULL;
    uint8_t *out = calloc((size_t)width * (size_t)height, 4);
    if (!out) return NULL;
    const uint8_t *blk = data;
    int alpha_dxt1 = (ifmt == 0x83F1 || ifmt == 0x8C4D);
    for (GLsizei by = 0; by < height; by += 4) {
        for (GLsizei bx = 0; bx < width; bx += 4) {
            const uint8_t *b = blk;
            uint16_t c0, c1; uint32_t code;
            memcpy(&c0, b + (dxt1 ? 0 : 8), 2);
            memcpy(&c1, b + (dxt1 ? 2 : 10), 2);
            memcpy(&code, b + (dxt1 ? 4 : 12), 4);
            uint8_t pal[4][4];
            uint8_t r0 = (uint8_t)(((c0 >> 11) & 0x1F) * 255 / 31);
            uint8_t g0 = (uint8_t)(((c0 >> 5) & 0x3F) * 255 / 63);
            uint8_t b0 = (uint8_t)((c0 & 0x1F) * 255 / 31);
            uint8_t r1 = (uint8_t)(((c1 >> 11) & 0x1F) * 255 / 31);
            uint8_t g1 = (uint8_t)(((c1 >> 5) & 0x3F) * 255 / 63);
            uint8_t b1 = (uint8_t)((c1 & 0x1F) * 255 / 31);
            pal[0][0]=r0; pal[0][1]=g0; pal[0][2]=b0; pal[0][3]=255;
            pal[1][0]=r1; pal[1][1]=g1; pal[1][2]=b1; pal[1][3]=255;
            if (dxt3 || dxt5 || c0 > c1) {
                pal[2][0]=(uint8_t)((2*r0+r1)/3); pal[2][1]=(uint8_t)((2*g0+g1)/3);
                pal[2][2]=(uint8_t)((2*b0+b1)/3); pal[2][3]=255;
                pal[3][0]=(uint8_t)((r0+2*r1)/3); pal[3][1]=(uint8_t)((g0+2*g1)/3);
                pal[3][2]=(uint8_t)((b0+2*b1)/3); pal[3][3]=255;
            } else {
                pal[2][0]=(uint8_t)((r0+r1)/2); pal[2][1]=(uint8_t)((g0+g1)/2);
                pal[2][2]=(uint8_t)((b0+b1)/2); pal[2][3]=255;
                pal[3][0]=pal[3][1]=pal[3][2]=0;
                pal[3][3]=alpha_dxt1 ? 0 : 255;
            }
            uint8_t atab[8] = {0};
            uint64_t bits = 0, acode = 0;
            if (dxt3 || dxt5) {
                if (dxt3) {
                    memcpy(&bits, b, 8);
                } else {
                    uint8_t a0 = b[0], a1 = b[1];
                    for (int i = 0; i < 6; i++) acode |= (uint64_t)b[2+i] << (8*i);
                    atab[0]=a0; atab[1]=a1;
                    if (a0 > a1) {
                        for (int j = 1; j <= 6; j++)
                            atab[1+j] = (uint8_t)(((7-j)*(int)a0 + j*(int)a1)/7);
                    } else {
                        for (int j = 1; j <= 4; j++)
                            atab[1+j] = (uint8_t)(((5-j)*(int)a0 + j*(int)a1)/5);
                        atab[6]=0; atab[7]=255;
                    }
                }
            }
            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    GLsizei x = bx + px, y = by + py;
                    if (x >= width || y >= height) continue;
                    int k = py*4 + px;
                    uint8_t *o = out + ((size_t)y * (size_t)width + (size_t)x)*4;
                    int idx = (int)((code >> (2*k)) & 3);
                    memcpy(o, pal[idx], 4);
                    if (dxt3) o[3] = (uint8_t)(((bits >> (k*4)) & 0xF)*17);
                    else if (dxt5) o[3] = atab[(acode >> (k*3)) & 7];
                }
            }
            blk += bb;
        }
    }
    return out;
}

static void dxt_upload(GLenum target, GLint level, GLenum ifmt,
                       GLsizei xoff, GLsizei yoff, GLsizei width, GLsizei height,
                       GLsizei imageSize, const void *data, int sub)
{
    uint8_t *out = shim_dxt_decode(ifmt, width, height, imageSize, data);
    if (!out) return;
    GLenum dst_ifmt = is_srgb_s3tc(ifmt) ? GL_SRGB8_ALPHA8 : GL_RGBA;
    GLint row, sr, sp, al, pbo;
    set_unpack_defaults(&row, &sr, &sp, &al, &pbo);
    if (sub) real_glTexSubImage2D(target, level, xoff, yoff, width, height,
                                  GL_RGBA, GL_UNSIGNED_BYTE, out);
    else real_glTexImage2D(target, level, (GLint)dst_ifmt, width, height, 0,
                           GL_RGBA, GL_UNSIGNED_BYTE, out);
    restore_unpack(&row, &sr, &sp, &al, pbo);
    free(out);
    drain_errors();
}

/* Try the driver's native S3TC upload first, draining its errors so a
   failure can be distinguished from an app-side error; if it is rejected,
   decode the block data on the CPU and upload as RGBA instead. */
static void shim_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt,
                                        GLsizei width, GLsizei height, GLint border,
                                        GLsizei imageSize, const void *data)
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    target = remap_tex_target(target);
    GLint pbo = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    /* NULL data reserves immutable storage; there are no blocks to decode
       and GLES accepts the call directly. Forwarding it avoids dereferencing
       NULL in dxt_upload() when Mesa reports no S3TC support. */
    if (!is_s3tc(ifmt) || pbo || !data || !real_glCompressedTexImage2D) {
        if (real_glCompressedTexImage2D)
            real_glCompressedTexImage2D(target, level, ifmt, width, height,
                                        border, imageSize, data);
        return;
    }
    if (c && c->features.has_s3tc && data) {
        GLenum saved = drain_errors_return();
        real_glCompressedTexImage2D(target, level, ifmt, width, height,
                                    border, imageSize, data);
        GLenum e = drain_errors_return();
        if (e == 0) {
            if (saved && c) c->pending_error = saved;
            return;
        }
        if (e != GL_INVALID_ENUM && e != GL_INVALID_OPERATION) {
            if (c) c->pending_error = e ? e : saved;
            return;
        }
    }
    if (g_debug) {
        static int n_fallback;
        if (n_fallback++ < 20)
            shim_trace("DXT: CPU fallback ifmt=0x%x %dx%d\n", ifmt, width, height);
    }
    dxt_upload(target, level, ifmt, 0, 0, width, height, imageSize, data, 0);
}

static void shim_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoff,
                                            GLint yoff, GLsizei width, GLsizei height,
                                            GLenum format, GLsizei imageSize,
                                            const void *data)
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    target = remap_tex_target(target);
    GLint pbo = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (!is_s3tc(format) || pbo || !data || !c || c->features.has_s3tc) {
        if (real_glCompressedTexSubImage2D)
            real_glCompressedTexSubImage2D(target, level, xoff, yoff, width, height,
                                           format, imageSize, data);
        return;
    }
    if (g_debug) {
        static int n_fallback;
        if (n_fallback++ < 20)
            shim_trace("DXT: CPU subimage fallback ifmt=0x%x %dx%d at %d,%d\n",
                       format, width, height, xoff, yoff);
    }
    dxt_upload(target, level, format, xoff, yoff, width, height, imageSize, data, 1);
}

SHIM_EXPORT(void, glCompressedTexImage2DARB,
            (GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b,
             GLsizei s, const void *d))
{ shim_glCompressedTexImage2D(t, l, f, w, h, b, s, d); }
SHIM_EXPORT(void, glCompressedTexSubImage2DARB,
            (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
             GLenum f, GLsizei s, const void *d))
{ shim_glCompressedTexSubImage2D(t, l, x, y, w, h, f, s, d); }

/* -------------------------------------------------------------------------- */
/* G.2.5: ShimFF UBO synchronisation and program table                        */
/*                                                                            */
/* The global program table records which legacy attributes each translated   */
/* program declares, so draw calls can feed only those attributes. ff_sync()  */
/* folds dirty bits into one UBO upload per frame and rotates the buffer to   */
/* avoid overwriting data a queued frame still reads.                          */
/* -------------------------------------------------------------------------- */

static uint32_t ff_prog_mask(GLuint prog)
{
    if (!__atomic_load_n(&g_ff_shader_count, __ATOMIC_RELAXED) || !prog)
        return 0;
    pthread_mutex_lock(&g_prog_mutex);
    uint32_t mask = 0;
    for (struct ShimProgEntry *e = g_prog_ff; e; e = e->next)
        if (e->prog == prog) { mask = e->mask; break; }
    pthread_mutex_unlock(&g_prog_mutex);
    return mask;
}

static void ff_prog_set(GLuint prog, uint32_t mask)
{
    pthread_mutex_lock(&g_prog_mutex);
    struct ShimProgEntry **p = &g_prog_ff;
    while (*p && (*p)->prog != prog) p = &(*p)->next;
    if (!*p && mask) {
        *p = calloc(1, sizeof(**p));
        if (*p) (*p)->prog = prog;
    }
    if (*p) {
        (*p)->mask = mask;
        if (!mask) {
            struct ShimProgEntry *old = *p;
            *p = old->next;
            free(old);
        }
    }
    pthread_mutex_unlock(&g_prog_mutex);
}

/* Rebuild the derived matrices/products and upload the ShimFF UBO. Called
   lazily at the first draw or program use after a state change, never from
   the setters themselves. */
static void ff_sync(ShimCtx *c)
{
    if (!c || !c->ff_dirty) return;
    ensure_gl_symbols();
    if (!c->ff_ready) {
        if (!real_glGenBuffers || !real_glBindBuffer || !real_glBufferData ||
            !real_glBindBufferBase)
            return;
        c->ff_ubo_binding = c->features.max_ubo_bindings - 1;
        if (c->ff_ubo_binding < 0) c->ff_ubo_binding = 0;
        real_glGenBuffers(4, c->ff_ubo);
        for (int i = 0; i < 4; i++) {
            real_glBindBuffer(GL_UNIFORM_BUFFER, c->ff_ubo[i]);
            real_glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)sizeof(ShimFFBlock),
                              NULL, GL_DYNAMIC_DRAW);
        }
        real_glBindBuffer(GL_UNIFORM_BUFFER, 0);
        c->ff_ready = 1;
        c->ff_dirty = FFD_ALL;
    }
    uint32_t d = c->ff_dirty;
    ShimFFBlock *f = &c->ff;
    memcpy(f->mv, c->mv_stack[c->mv_top], sizeof(f->mv));
    memcpy(f->proj, c->proj_stack[c->proj_top], sizeof(f->proj));
    /* H.2/G.2.4: the texture stack also lives in the UBO. Without this copy
       shim_TextureMatrix stayed zero even while the app animated UVs. */
    for (int u = 0; u < 8; u++)
        memcpy(f->tex_matrix[u], c->tex_stack[u][c->tex_top[u]], 16 * sizeof(float));
    if (d & (FFD_MV | FFD_PROJ))
        mat4_mul(f->mvp, f->proj, f->mv);
    if (__atomic_load_n(&g_ff_need_variants, __ATOMIC_RELAXED)) {
        mat4_inverse(f->mv_inv, f->mv);
        mat4_inverse(f->proj_inv, f->proj);
        mat4_inverse(f->mvp_inv, f->mvp);
        mat4_transpose(f->mv_t, f->mv);
        mat4_transpose(f->proj_t, f->proj);
        mat4_transpose(f->mvp_t, f->mvp);
        mat4_transpose(f->mv_it, f->mv_inv);
        mat4_transpose(f->proj_it, f->proj_inv);
        mat4_transpose(f->mvp_it, f->mvp_inv);
        normal_matrix_from_mv(f->normal_matrix, f->mv);
    }
    if (d & (FFD_LIGHT | FFD_MAT)) {
        for (int i = 0; i < 8; i++) {
            ShimFFLight *l = &f->light[i];
            if (l->spotCutoff >= 179.9f) l->spotCosCutoff = -1.0f;
            else l->spotCosCutoff = cosf(l->spotCutoff * 3.14159265f / 180.0f);
            float len = sqrtf(l->position[0]*l->position[0] +
                              l->position[1]*l->position[1] +
                              l->position[2]*l->position[2]);
            if (len < 1e-6f) len = 1.0f;
            float hx = l->position[0]/len, hy = l->position[1]/len,
                  hz = l->position[2]/len + 1.0f;
            float hl = sqrtf(hx*hx + hy*hy + hz*hz);
            if (hl < 1e-6f) hl = 1.0f;
            l->halfVector[0] = hx/hl; l->halfVector[1] = hy/hl;
            l->halfVector[2] = hz/hl; l->halfVector[3] = 0.0f;
            for (int face = 0; face < 2; face++) {
                ShimFFMaterial *m = face ? &f->back_mat : &f->front_mat;
                ShimFFLightProducts *lp = face ? &f->back_lp[i] : &f->front_lp[i];
                for (int k = 0; k < 3; k++) {
                    lp->ambient[k] = m->ambient[k] * l->ambient[k];
                    lp->diffuse[k] = m->diffuse[k] * l->diffuse[k];
                    lp->specular[k] = m->specular[k] * l->specular[k];
                }
                lp->ambient[3] = lp->diffuse[3] = lp->specular[3] = 1.0f;
            }
        }
        for (int face = 0; face < 2; face++) {
            ShimFFMaterial *m = face ? &f->back_mat : &f->front_mat;
            float *scene = face ? f->back_scene : f->front_scene;
            for (int k = 0; k < 3; k++)
                scene[k] = m->emission[k] + m->ambient[k] * f->lm_ambient[k];
            scene[3] = 1.0f;
        }
        f->fog.scale = (f->fog.end != f->fog.start)
                       ? 1.0f / (f->fog.end - f->fog.start) : 0.0f;
    }
    if (d & FFD_CLIP)
        f->clip_enable_mask = (int32_t)c->clip_enable_mask;
    for (int i = 0; i < 8; i++) {
        memcpy(f->texenv_color[i], c->texenv_color[i], 4 * sizeof(float));
        f->texenv_mode[i][0] = (float)c->texenv_mode[i];
    }
    f->alpha_params[0] = (float)c->alpha_func;
    f->alpha_params[1] = c->alpha_ref;
    f->alpha_params[2] = c->alpha_test ? 1.0f : 0.0f;
    f->point_params[0] = c->point_size;
    f->point_params[1] = (float)c->shade_model;
    f->point_params[2] = c->point_size_set ? 1.0f : 0.0f;
    c->ff_dirty = 0;

    c->ff_ubo_cur = (c->ff_ubo_cur + 1) & 3;
    GLint saved_binding = 0;
    if (real_glGetIntegerv)
        real_glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &saved_binding);
    real_glBindBuffer(GL_UNIFORM_BUFFER, c->ff_ubo[c->ff_ubo_cur]);
    real_glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(*f), f);
    real_glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)saved_binding);
    real_glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)c->ff_ubo_binding,
                          c->ff_ubo[c->ff_ubo_cur]);
}

static const int shim_arr_for_k[13] = {
    SHIM_ARR_VERTEX, SHIM_ARR_COLOR, SHIM_ARR_NORMAL,
    SHIM_ARR_TEX0, SHIM_ARR_TEX0+1, SHIM_ARR_TEX0+2, SHIM_ARR_TEX0+3,
    SHIM_ARR_TEX0+4, SHIM_ARR_TEX0+5, SHIM_ARR_TEX0+6, SHIM_ARR_TEX0+7,
    SHIM_ARR_SECONDARY, SHIM_ARR_FOGCOORD
};
static const char *shim_ff_attr_name(int k)
{
    static const char *names[13] = {
        "shim_Vertex", "shim_Color", "shim_Normal",
        "shim_MultiTexCoord0", "shim_MultiTexCoord1", "shim_MultiTexCoord2",
        "shim_MultiTexCoord3", "shim_MultiTexCoord4", "shim_MultiTexCoord5",
        "shim_MultiTexCoord6", "shim_MultiTexCoord7",
        "shim_SecondaryColor", "shim_FogCoord"
    };
    return names[k];
}

/* Feed the fixed-function attributes declared by a translated user program.
   Legacy client arrays are uploaded into private VBOs and bound to the
   shim_* attribute locations; disabled arrays are replaced by the current
   GL color/normal/texcoord constants via glVertexAttrib4f. */
static void ff_feed_user_program(ShimCtx *c, GLuint prog, GLint first, GLsizei count)
{
    uint32_t mask = ff_prog_mask(prog);
    if (!mask || !real_glGetAttribLocation) return;
    GLint saved_vbo = 0;
    if (real_glGetIntegerv)
        real_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_vbo);
    for (int k = 0; k < 13; k++) {
        if (!(mask & (1u << k))) continue;
        GLint loc = real_glGetAttribLocation(prog, shim_ff_attr_name(k));
        if (loc < 0) continue;
        ShimArray *a = &c->arrays[shim_arr_for_k[k]];
        if (!a->enabled || !a->ptr || a->type == GL_DOUBLE) {
            real_glDisableVertexAttribArray((GLuint)loc);
            const float *v = NULL;
            float tmp[4] = {0,0,0,1};
            switch (k) {
            case 1: v = c->current_color; break;
            case 2: v = c->current_normal; break;
            case 12: tmp[0] = c->fog_coord; v = tmp; break;
            default:
                if (k >= 3 && k <= 10) v = c->current_texcoord[k - 3];
                else { tmp[3] = 1.0f; v = tmp; } /* vertex: undefined value in GL */
                break;
            }
            if (k == 11) { tmp[0]=c->secondary_color[0]; tmp[1]=c->secondary_color[1];
                           tmp[2]=c->secondary_color[2]; tmp[3]=1.0f; v=tmp; }
            if (v && real_glVertexAttrib4f)
                real_glVertexAttrib4f((GLuint)loc, v[0], v[1], v[2], v[3]);
            continue;
        }
        size_t elem = (size_t)a->size * 4;
        GLsizei stride = a->stride ? a->stride : (GLsizei)elem;
        size_t need = (size_t)(first + count) * (size_t)stride;
        GLuint vbo = a->vbo;
        const void *ptr = a->ptr;
        if (!vbo) {
            if (!real_glGenBuffers) continue;
            if (!c->client_vbo[k]) real_glGenBuffers(1, &c->client_vbo[k]);
            if ((size_t)c->client_cap[k] < need) {
                real_glBindBuffer(GL_ARRAY_BUFFER, c->client_vbo[k]);
                real_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)need, NULL, GL_STREAM_DRAW);
                c->client_cap[k] = (int)need;
            }
            real_glBindBuffer(GL_ARRAY_BUFFER, c->client_vbo[k]);
            real_glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)need, a->ptr);
            vbo = c->client_vbo[k];
            ptr = (const void *)0;
        }
        real_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        real_glEnableVertexAttribArray((GLuint)loc);
        int norm = a->normalized;
        if (!norm)
            norm = (k == 1 || k == 2 || k == 11) &&
                   (a->type == GL_UNSIGNED_BYTE || a->type == GL_BYTE ||
                    a->type == GL_SHORT || a->type == GL_UNSIGNED_SHORT ||
                    a->type == GL_INT || a->type == GL_UNSIGNED_INT);
        real_glVertexAttribPointer((GLuint)loc, a->size, a->type, (GLboolean)norm,
                                   stride, ptr);
    }
    if (real_glBindBuffer)
        real_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)saved_vbo);
}

/* -------------------------------------------------------------------------- */
/* MODULE F: legacy rasterisation and quads                                   */
/*                                                                            */
/* GLES 3 has no GL_QUADS/GL_QUAD_STRIP/GL_POLYGON: convert them to triangles  */
/* via a cached index buffer. Legacy state without a direct GLES equivalent   */
/* (glPolygonMode, alpha test, point size) is recorded in ShimCtx and applied  */
/* by the generated shaders instead.                                           */
/* -------------------------------------------------------------------------- */

SHIM_EXPORT(void, glPolygonMode, (GLenum face, GLenum mode))
{ (void)face; (void)mode; /* not available in GLES */ }

SHIM_EXPORT(void, glPointSize, (GLfloat size))
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    c->point_size = size;
    c->point_size_set = 1;
    c->ff_dirty |= FFD_MISC;
}

/* H.2/A14: glAlphaFunc with the 8 GL 1.x comparisons. The state was already
   in ShimCtx but had no setter; the generated fragment shader applies the
   discard by reading shim_AlphaParams from the UBO. GL_ALWAYS disables the
   test so the helper costs nothing in programs that never use alpha test. */
SHIM_EXPORT(void, glAlphaFunc, (GLenum func, GLclampf ref))
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    float v = ref;
    shim_dl_record(c, DL_ALPHA_FUNC, (int)func, 0, &v, 1);
    if (func < GL_NEVER || func > GL_ALWAYS) return;
    c->alpha_func = (int)func;
    c->alpha_ref = ref;
    c->alpha_test = (func != GL_ALWAYS);
    c->ff_dirty |= FFD_MISC;
}

SHIM_EXPORT(void, glShadeModel, (GLenum mode))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || (mode != GL_FLAT && mode != GL_SMOOTH)) return;
    c->shade_model = (int)mode;
    c->ff_dirty |= FFD_MISC;
}

/* Absolute indices (strategy B): the IBO is reuploaded only when the region
   changes. With GL_QUADS/GL_QUAD_STRIP the strip's first vertex is offset
   by 'first'. */
static void ff_quad_indices(ShimCtx *c, GLsizei quads, GLenum mode, GLint first)
{
    GLsizei idx = quads * 6;
    size_t need = (size_t)idx * sizeof(uint32_t);
    if ((size_t)c->quad_ibo_cap < need) {
        if (!c->quad_ibo) real_glGenBuffers(1, &c->quad_ibo);
        real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->quad_ibo);
        real_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)need, NULL, GL_STREAM_DRAW);
        /* Capacity is tracked in bytes, like tmp_ibo_cap; storing the index
           count here made every draw look larger than the buffer and forced a
           fresh GL_STREAM_DRAW allocation on each SpriteBatch submit. */
        c->quad_ibo_cap = (int)need;
        c->quad_abs_first = c->quad_abs_count = -1;
    }
    if (c->quad_abs_first == first && c->quad_abs_count == idx &&
        c->quad_abs_mode == (int)mode)
        return;
    uint32_t *p = (uint32_t *)scratch(need);
    if (!p) return;
    for (GLsizei q = 0; q < quads; q++) {
        uint32_t b = (uint32_t)(first + q * 4);
        if (mode == GL_QUAD_STRIP) {
            p[q*6+0]=b;   p[q*6+1]=b+1; p[q*6+2]=b+2;
            p[q*6+3]=b+1; p[q*6+4]=b+3; p[q*6+5]=b+2;
        } else {
            p[q*6+0]=b;   p[q*6+1]=b+1; p[q*6+2]=b+2;
            p[q*6+3]=b;   p[q*6+4]=b+2; p[q*6+5]=b+3;
        }
    }
    real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->quad_ibo);
    real_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)need, p);
    c->quad_abs_first = first;
    c->quad_abs_count = idx;
    c->quad_abs_mode = (int)mode;
}

/* Draw legacy arrays through a private VAO: bind the shim_* attribute
   locations, feed any missing constants, then issue the real draw. Generic
   attributes set by the app are left untouched. */
static void shim_draw_converted_arrays(ShimCtx *c, GLenum mode, GLint first, GLsizei count)
{
    if (!real_glDrawArrays) return;
    if (c) {
        ff_sync(c);
        /* Only legacy arrays are injected; generic attributes from the app
           (glVertexAttribPointer) must not be overwritten. */
        if (c->cur_prog && c->legacy_va_enabled)
            ff_feed_user_program(c, c->cur_prog, first, count);
    }
    if (mode == GL_POLYGON) {
        real_glDrawArrays(GL_TRIANGLE_FAN, first, count);
        return;
    }
    if (mode != GL_QUADS && mode != GL_QUAD_STRIP) {
        real_glDrawArrays(mode, first, count);
        return;
    }
    GLsizei quads = mode == GL_QUAD_STRIP ? (count >= 2 ? (count - 2) / 2 : 0) : count / 4;
    GLsizei idx = quads * 6;
    if (idx <= 0 || !c) return;
    GLint prev = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev);
    ff_quad_indices(c, quads, mode, first);
    real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->quad_ibo);
    real_glDrawElements(GL_TRIANGLES, idx, GL_UNSIGNED_INT, NULL);
    real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prev);
}

/* Entry point for non-indexed draws: G.3 emulation first, then quad/polygon
   conversion or a direct real_glDrawArrays for native primitives. */
static void shim_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    ensure_gl_symbols();
    if (!real_glDrawArrays) return;
    ShimCtx *c = tls_ctx_ptr;
    if (c && c->emu_cur) {
        shim_emu_draw_arrays(mode, first, count);
        return;
    }
    /* Fast path: three TLS reads, no mutex and no GL calls. */
    if (!c || (!(c->legacy_va_enabled | (int)c->ff_dirty | c->emu_cur) &&
               mode != GL_QUADS && mode != GL_QUAD_STRIP && mode != GL_POLYGON)) {
        real_glDrawArrays(mode, first, count);
        return;
    }
    shim_draw_converted_arrays(c, mode, first, count);
}

static void shim_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    ensure_gl_symbols();
    ShimCtx *c = tls_ctx_ptr;
    if (!c) { if (real_glDrawElements) real_glDrawElements(mode, count, type, indices); return; }
    if (c->emu_cur) {
        shim_emu_draw_elements(mode, count, type, indices);
        return;
    }
    if (!c->legacy_va_enabled && !c->ff_dirty && !c->emu_cur && mode != GL_QUADS &&
        mode != GL_QUAD_STRIP) {
        real_glDrawElements(mode, count, type, indices);
        return;
    }
    ff_sync(c);
    if (c->cur_prog) ff_feed_user_program(c, c->cur_prog, 0, count);
    if (mode == GL_POLYGON) {
        real_glDrawElements(GL_TRIANGLE_FAN, count, type, indices);
        return;
    }
    if (mode != GL_QUADS && mode != GL_QUAD_STRIP) {
        real_glDrawElements(mode, count, type, indices);
        return;
    }
    /* Read indices (client or IBO) and expand 4 -> 6 into tmp_ibo. scratch()
       is one reusable TLS block: asking for the output after the input could
       realloc and leave the input pointer dangling, uploading random indices
       for a batch. Reserve both regions in a single allocation instead. */
    size_t bytes = (size_t)count * (type == GL_UNSIGNED_BYTE ? 1 :
                                    type == GL_UNSIGNED_SHORT ? 2 : 4);
    GLsizei prims = mode == GL_QUADS ? count / 4 : (count >= 2 ? (count - 2) / 2 : 0);
    GLsizei outn = prims * 6;
    if (outn <= 0) return;
    size_t out_bytes = (size_t)outn * 4;
    size_t in_bytes = (size_t)count * 4;
    if (in_bytes > SIZE_MAX - out_bytes || out_bytes + in_bytes > SIZE_MAX - bytes)
        return;
    /* Three regions in one TLS block: triangles, widened input and the raw
       indices. Widening GLES byte/short indices in place would overwrite the
       not-yet-read entries of the source. */
    uint8_t *buf = scratch(out_bytes + in_bytes + bytes);
    if (!buf) return;
    uint32_t *out = (uint32_t *)buf;
    uint32_t *in = (uint32_t *)(buf + out_bytes);
    uint8_t *src = buf + out_bytes + in_bytes;
    GLint ibo = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ibo);
    if (!ibo && indices) memcpy(src, indices, bytes);
    else if (ibo && real_glGetBufferSubData)
        real_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)indices,
                                (GLsizeiptr)bytes, src);
    else if (ibo && real_glMapBufferRange) {
        GLint sz = 0;
        real_glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &sz);
        void *m = real_glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, sz,
                                        GL_MAP_READ_BIT);
        if (!m) return;
        memcpy(src, (const uint8_t *)m + (uintptr_t)indices, bytes);
        real_glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    } else return;
    for (GLsizei i = 0; i < count; i++)
        in[i] = type == GL_UNSIGNED_BYTE ? ((uint8_t *)src)[i] :
                type == GL_UNSIGNED_SHORT ? ((uint16_t *)src)[i] : ((uint32_t *)src)[i];
    for (GLsizei q = 0; q < prims; q++) {
        uint32_t b = q * 4;
        if (mode == GL_QUADS) {
            out[q*6+0]=in[b]; out[q*6+1]=in[b+1]; out[q*6+2]=in[b+2];
            out[q*6+3]=in[b]; out[q*6+4]=in[b+2]; out[q*6+5]=in[b+3];
        } else {
            out[q*6+0]=in[b]; out[q*6+1]=in[b+1]; out[q*6+2]=in[b+2];
            out[q*6+3]=in[b+1]; out[q*6+4]=in[b+3]; out[q*6+5]=in[b+2];
        }
    }
    size_t need = (size_t)outn * 4;
    if ((size_t)c->tmp_ibo_cap < need) {
        if (!c->tmp_ibo) real_glGenBuffers(1, &c->tmp_ibo);
        real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->tmp_ibo);
        real_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)need, NULL, GL_STREAM_DRAW);
        c->tmp_ibo_cap = (int)need;
    }
    real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->tmp_ibo);
    real_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)need, out);
    real_glDrawElements(GL_TRIANGLES, outn, GL_UNSIGNED_INT, NULL);
    real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)ibo);
}

static void shim_glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
                                     GLsizei count, GLenum type, const void *indices)
{
    (void)start; (void)end;
    shim_glDrawElements(mode, count, type, indices);
}

/* ========================================================================== */
/* MODULE G: light rewriting + G.2 fixed-function shaders                     */
/*                                                                            */
/* Translation has two passes: scan the source for built-ins actually used    */
/* (FFUse), then rewrite tokens and prepend only the declarations the shader   */
/* needs. Native GLSL ES sources and simple #version 110 sources are left     */
/* mostly untouched; G.2 shaders receive a ShimFF UBO plus generated helpers. */
/* ========================================================================== */

#define FFV_FRONTCOLOR (1u << 0)
#define FFV_BACKCOLOR  (1u << 1)
#define FFV_FSECCOLOR  (1u << 2)
#define FFV_FOG        (1u << 3)
#define FFV_TEX0       (1u << 4)

/* Result of the usage scan: which legacy attributes/varyings/uniforms the
   app's shader references, and how many lights, texcoord units and clip
   planes must be declared for it. Drives the generated declarations so a
   trivial shader does not pay for the full 3808-byte FF block. */
typedef struct FFUse {
    uint32_t attr_mask, vary_mask, uni_mask;
    unsigned ftransform:1, s1d:1, is1d:1, us1d:1, dbl:1, clip:1;
    unsigned need_variants:1, has_fragcolor:1;
    unsigned fragdata_mask;
    char s1d_names[16][64];
    unsigned char s1d_kind[16];   /* 0 float, 1 int, 2 uint */
    int s1d_count;
    int maxlights, maxtex, maxtexcoord, maxclip, maxvarying;
} FFUse;

typedef struct TokCtx {
    StrBuf *out;
    char rep[2048];
    int emitted;
    size_t skip;
} TokCtx;

typedef void (*TokFn)(const char *tok, size_t len, const char *rest,
                      TokCtx *tc, void *user);

static int shim_ident_start(int c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static int shim_ident_char(int c)
{ return shim_ident_start(c) || (c >= '0' && c <= '9'); }

static const char *shim_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Parse '[ spaces integer spaces ]' and return a pointer just after ']',
   or NULL when there is no integer index. Used for array uniforms such as
   gl_TexCoord[0] and gl_MultiTexCoord[n]. */
static const char *shim_bracket_int(const char *rest, int *value)
{
    const char *p = shim_skip_ws(rest);
    if (*p != '[') return NULL;
    p = shim_skip_ws(p + 1);
    if (*p < '0' || *p > '9') return NULL;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 1024) return NULL;
        p++;
    }
    p = shim_skip_ws(p);
    if (*p != ']') return NULL;
    *value = v;
    return p + 1;
}

static void shim_copyline(const char **pp, StrBuf *out)
{
    const char *p = *pp;
    while (*p && *p != '\n') sb_addn(out, p++, 1);
    if (*p) sb_addn(out, p++, 1);
    *pp = p;
}

/* Tokeniser: skips comments and strings during substitutions, but copies
   them verbatim. Preprocessor directives are copied untouched. */
/* Tokenise GLSL for shim_rewrite_token(): identifiers are passed to fn,
   while comments, strings and preprocessor lines are copied verbatim so
   #version/#extension keep their required position and text is not broken. */
static void shim_lex_process(const char *src, StrBuf *out, TokFn fn, void *user)
{
    const char *p = src;
    StrBuf line = {0};
    while (*p) {
        if ((*p == ' ' || *p == '\t') && (!out || out->len == 0 ||
             out->data[out->len - 1] == '\n')) {
            /* Start of line: a directive? */
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '#') {
                if (out) {
                    sb_addn(out, p, (size_t)(q - p));
                    p = q;
                    shim_copyline(&p, out);
                } else {
                    shim_copyline(&p, &line);
                    line.len = 0;
                }
                continue;
            }
        }
        if (p[0] == '/' && p[1] == '/') {
            if (out) shim_copyline(&p, out);
            else { shim_copyline(&p, &line); line.len = 0; }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            if (out) sb_addn(out, "/*", 2);
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (out) sb_addn(out, p++, 1);
                else p++;
            }
            if (*p) {
                if (out) sb_addn(out, "*/", 2);
                p += 2;
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            if (out) sb_addn(out, &quote, 1);
            while (*p) {
                if (out) sb_addn(out, p, 1);
                if (*p == '\\' && p[1]) {
                    p++;
                    if (out) sb_addn(out, p, 1);
                    p++;
                    continue;
                }
                char ch = *p++;
                if (ch == quote) break;
            }
            continue;
        }
        if (shim_ident_start((unsigned char)*p)) {
            const char *e = p + 1;
            while (shim_ident_char((unsigned char)*e)) e++;
            TokCtx tc = {0};
            tc.out = out;
            if (fn) fn(p, (size_t)(e - p), e, &tc, user);
            if (out) {
                if (tc.emitted) sb_addn(out, tc.rep, strlen(tc.rep));
                else sb_addn(out, p, (size_t)(e - p));
            }
            p = e + tc.skip;
            continue;
        }
        if (out) sb_addn(out, p++, 1);
        else p++;
    }
    free(line.data);
}

static void shim_setrep(TokCtx *tc, const char *s)
{
    snprintf(tc->rep, sizeof(tc->rep), "%s", s);
    tc->emitted = 1;
}

static void shim_setrepf(TokCtx *tc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tc->rep, sizeof(tc->rep), fmt, ap);
    va_end(ap);
    tc->emitted = 1;
}

static int shim_tok_is(const char *tok, size_t len, const char *s)
{ return strlen(s) == len && !memcmp(tok, s, len); }

/* ------------------------------------------------------------------------- */
/* G.3-bis / G.2: usage analysis                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *table[64][2];
    int n;
    uint32_t *attr, *vary, *uni;
    FFUse *u;
    int type;
} ScanCtx;

static void scan_uni(ScanCtx *s, TokCtx *tc)
{
    *s->uni = 1;
    (void)tc;
}

static void scan_ff_token(const char *tok, size_t len, const char *rest,
                          TokCtx *tc, void *user)
{
    ScanCtx *s = user;
    FFUse *u = s->u;
    int idx;
    const char *after;
    if (shim_tok_is(tok, len, "ftransform")) { u->ftransform = 1; *s->uni = 1; *s->attr |= 1u; }
    if (shim_tok_is(tok, len, "gl_Vertex")) *s->attr |= 1u << 0;
    if (shim_tok_is(tok, len, "gl_Color")) {
        if (s->type == GL_FRAGMENT_SHADER) *s->vary |= FFV_FRONTCOLOR;
        else *s->attr |= 1u << 1;
    }
    if (shim_tok_is(tok, len, "gl_Normal")) *s->attr |= 1u << 2;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord0")) *s->attr |= 1u << 3;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord1")) *s->attr |= 1u << 4;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord2")) *s->attr |= 1u << 5;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord3")) *s->attr |= 1u << 6;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord4")) *s->attr |= 1u << 7;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord5")) *s->attr |= 1u << 8;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord6")) *s->attr |= 1u << 9;
    if (shim_tok_is(tok, len, "gl_MultiTexCoord7")) *s->attr |= 1u << 10;
    if (shim_tok_is(tok, len, "gl_SecondaryColor")) {
        if (s->type == GL_FRAGMENT_SHADER) *s->vary |= FFV_FSECCOLOR;
        else *s->attr |= 1u << 11;
    }
    if (shim_tok_is(tok, len, "gl_FogCoord")) *s->attr |= 1u << 12;
    if (shim_tok_is(tok, len, "gl_FrontColor")) *s->vary |= FFV_FRONTCOLOR;
    if (shim_tok_is(tok, len, "gl_BackColor")) *s->vary |= FFV_BACKCOLOR;
    if (shim_tok_is(tok, len, "gl_FrontSecondaryColor")) *s->vary |= FFV_FSECCOLOR;
    if (shim_tok_is(tok, len, "gl_BackSecondaryColor")) *s->vary |= FFV_BACKCOLOR;
    if (shim_tok_is(tok, len, "gl_FogFragCoord")) *s->vary |= FFV_FOG;
    if (shim_tok_is(tok, len, "gl_TexCoord") &&
        (after = shim_bracket_int(rest, &idx)) != NULL && idx >= 0 && idx < 8) {
        *s->vary |= FFV_TEX0 << idx;
        tc->skip = (size_t)(after - rest);
    }
    if (shim_tok_is(tok, len, "gl_FragColor")) u->has_fragcolor = 1;
    if (shim_tok_is(tok, len, "gl_FragData") &&
        (after = shim_bracket_int(rest, &idx)) != NULL && idx >= 0 && idx < 8) {
        u->fragdata_mask |= 1u << idx;
        tc->skip = (size_t)(after - rest);
    }
    if (shim_tok_is(tok, len, "gl_ClipVertex") || shim_tok_is(tok, len, "gl_ClipDistance"))
        u->clip = 1;
    if (shim_tok_is(tok, len, "sampler1D")) {
        u->s1d = 1; *s->uni = 1;
    }
    if (shim_tok_is(tok, len, "isampler1D")) { u->is1d = 1; *s->uni = 1; }
    if (shim_tok_is(tok, len, "usampler1D")) { u->us1d = 1; *s->uni = 1; }
    if (shim_tok_is(tok, len, "double") || shim_tok_is(tok, len, "dvec2") ||
        shim_tok_is(tok, len, "dvec3") || shim_tok_is(tok, len, "dvec4") ||
        shim_tok_is(tok, len, "dmat2") || shim_tok_is(tok, len, "dmat3") ||
        shim_tok_is(tok, len, "dmat4") || shim_tok_is(tok, len, "texture1D") ||
        shim_tok_is(tok, len, "texture1DLod") || shim_tok_is(tok, len, "texture1DProj"))
        u->dbl = 1;
    if (len > 3 && tok[len-2] == 'l' && tok[len-1] == 'f' &&
        ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '.'))
        u->dbl = 1;
    if (shim_tok_is(tok, len, "gl_ModelViewMatrixInverse") ||
        shim_tok_is(tok, len, "gl_ProjectionMatrixInverse") ||
        shim_tok_is(tok, len, "gl_ModelViewProjectionMatrixInverse") ||
        shim_tok_is(tok, len, "gl_ModelViewMatrixTranspose") ||
        shim_tok_is(tok, len, "gl_ProjectionMatrixTranspose") ||
        shim_tok_is(tok, len, "gl_ModelViewProjectionMatrixTranspose") ||
        shim_tok_is(tok, len, "gl_ModelViewMatrixInverseTranspose") ||
        shim_tok_is(tok, len, "gl_ProjectionMatrixInverseTranspose") ||
        shim_tok_is(tok, len, "gl_ModelViewProjectionMatrixInverseTranspose") ||
        shim_tok_is(tok, len, "gl_NormalMatrix"))
        u->need_variants = 1;
    if (shim_tok_is(tok, len, "gl_MaxLights")) { u->maxlights = 1; *s->uni = 1; }
    if (shim_tok_is(tok, len, "gl_MaxTextureUnits")) { u->maxtex = 1; *s->uni = 1; }
    if (shim_tok_is(tok, len, "gl_MaxTextureCoords")) { u->maxtexcoord = 1; *s->uni = 1; }
    if (shim_tok_is(tok, len, "gl_MaxClipPlanes")) { u->maxclip = 1; *s->uni = 1; }
    if (shim_tok_is(tok, len, "gl_MaxVaryingFloats")) { u->maxvarying = 1; *s->uni = 1; }
    for (int i = 0; i < s->n; i++)
        if (shim_tok_is(tok, len, s->table[i][0])) { scan_uni(s, tc); return; }
}

/* G.2.4 built-in names and their equivalents in the ShimFF block. The
   scanner maps the desktop spellings to fields declared by the generated
   UBO; entries missing here are passed through to the driver unchanged. */
static const char *shim_ff_uniforms[][2] = {
    {"gl_ModelViewMatrix", "shim_ModelViewMatrix"},
    {"gl_ProjectionMatrix", "shim_ProjectionMatrix"},
    {"gl_ModelViewProjectionMatrix", "shim_ModelViewProjectionMatrix"},
    {"gl_ModelViewMatrixInverse", "shim_ModelViewMatrixInverse"},
    {"gl_ProjectionMatrixInverse", "shim_ProjectionMatrixInverse"},
    {"gl_ModelViewProjectionMatrixInverse", "shim_ModelViewProjectionMatrixInverse"},
    {"gl_ModelViewMatrixTranspose", "shim_ModelViewMatrixTranspose"},
    {"gl_ProjectionMatrixTranspose", "shim_ProjectionMatrixTranspose"},
    {"gl_ModelViewProjectionMatrixTranspose", "shim_ModelViewProjectionMatrixTranspose"},
    {"gl_ModelViewMatrixInverseTranspose", "shim_ModelViewMatrixInverseTranspose"},
    {"gl_ProjectionMatrixInverseTranspose", "shim_ProjectionMatrixInverseTranspose"},
    {"gl_ModelViewProjectionMatrixInverseTranspose", "shim_ModelViewProjectionMatrixInverseTranspose"},
    {"gl_NormalMatrix", "shim_NormalMatrix"},
    {"gl_NormalScale", "shim_NormalScale"},
    {"gl_TextureMatrix", "shim_TextureMatrix"},
    {"gl_LightSource", "shim_LightSource"},
    {"gl_LightSourceParameters", "shim_LightSourceParameters"},
    {"gl_FrontMaterial", "shim_FrontMaterial"},
    {"gl_BackMaterial", "shim_BackMaterial"},
    {"gl_MaterialParameters", "shim_MaterialParameters"},
    {"gl_LightModel", "shim_LightModel"},
    {"gl_LightModelParameters", "shim_LightModelParameters"},
    {"gl_FrontLightModelProduct", "shim_FrontLightModelProduct"},
    {"gl_BackLightModelProduct", "shim_BackLightModelProduct"},
    {"gl_LightModelProducts", "shim_LightModelProducts"},
    {"gl_FrontLightProduct", "shim_FrontLightProduct"},
    {"gl_BackLightProduct", "shim_BackLightProduct"},
    {"gl_LightProducts", "shim_LightProducts"},
    {"gl_Fog", "shim_Fog"},
    {"gl_FogParameters", "shim_FogParameters"},
    {"gl_ClipPlane", "shim_ClipPlane"},
    {"gl_TextureEnvColor", "shim_TextureEnvColor"}
};

static void scan_ff_usage(const char *body, int type, FFUse *u)
{
    memset(u, 0, sizeof(*u));
    ScanCtx s = {0};
    for (size_t i = 0; i < sizeof(shim_ff_uniforms)/sizeof(shim_ff_uniforms[0]); i++) {
        s.table[s.n][0] = shim_ff_uniforms[i][0];
        s.table[s.n][1] = shim_ff_uniforms[i][1];
        s.n++;
    }
    s.attr = &u->attr_mask;
    s.vary = &u->vary_mask;
    s.uni = &u->uni_mask;
    s.u = u;
    s.type = type;
    shim_lex_process(body, NULL, scan_ff_token, &s);
    for (const char *p = body + 1; *p && !u->dbl; p++)
        if ((*p == 'l' || *p == 'L') && p[1] == 'f' &&
            ((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.'))
            u->dbl = 1;

    /* Collect identifiers declared with (i|u)sampler1D. */
    const char *p = body;
    while ((p = strstr(p, "sampler1D")) != NULL) {
        int kind = 0;
        if (p > body && p[-1] == 'i' && (p - 1 == body || !shim_ident_char((unsigned char)p[-2])))
            kind = 1;
        else if (p > body && p[-1] == 'u' && (p - 1 == body || !shim_ident_char((unsigned char)p[-2])))
            kind = 2;
        const char *r = p + 9;
        const char *id = shim_skip_ws(r);
        if (shim_ident_start((unsigned char)*id) && u->s1d_count < 16) {
            const char *e = id + 1;
            while (shim_ident_char((unsigned char)*e)) e++;
            size_t n = (size_t)(e - id);
            if (n >= sizeof(u->s1d_names[0])) n = sizeof(u->s1d_names[0]) - 1;
            memcpy(u->s1d_names[u->s1d_count], id, n);
            u->s1d_names[u->s1d_count][n] = 0;
            u->s1d_kind[u->s1d_count] = (unsigned char)kind;
            u->s1d_count++;
        }
        p = r;
    }
    if (u->clip)
        u->uni_mask = 1; /* the ShimFF block is mandatory when clipping is used */
}

typedef struct RewriteCtx {
    int ff;                 /* 0 = case 1 (ES 1.00), 1 = case 2 with G.2 */
    FFUse *u;
    int type;
    ShimCtx *c;
    int clip_ext;
} RewriteCtx;

static int s1d_lookup(FFUse *u, const char *name, size_t len)
{
    for (int i = 0; i < u->s1d_count; i++)
        if (strlen(u->s1d_names[i]) == len && !memcmp(u->s1d_names[i], name, len))
            return i;
    return -1;
}

/* Replace one identifier from a desktop shader: built-ins by generated
   declarations/varyings, ftransform by the explicit MVP expression, 1.0lf
   literals by float constants, and double uniforms by their float form. */
static void shim_rewrite_token(const char *tok, size_t len, const char *rest,
                               TokCtx *tc, void *user)
{
    RewriteCtx *rc = user;
    FFUse *u = rc->u;
    int idx;
    const char *after;

    if (!rc->ff)
        return; /* GLSL ES 1.00 (case 1): the original text is already valid */

    /* Module G's lexical-transition workhorses (without G.2). */
    if (shim_tok_is(tok, len, "attribute")) { shim_setrep(tc, "in"); return; }
    if (shim_tok_is(tok, len, "varying")) {
        shim_setrep(tc, rc->type == GL_VERTEX_SHADER ? "out" : "in");
        return;
    }
    if (shim_tok_is(tok, len, "texture2D")) { shim_setrep(tc, "texture"); return; }
    if (shim_tok_is(tok, len, "texture3D") || shim_tok_is(tok, len, "textureCube")) {
        shim_setrep(tc, "texture"); return;
    }
    if (shim_tok_is(tok, len, "texture2DProj")) { shim_setrep(tc, "textureProj"); return; }
    if (shim_tok_is(tok, len, "texture2DLod")) { shim_setrep(tc, "textureLod"); return; }
    if (shim_tok_is(tok, len, "gl_FragColor")) { shim_setrep(tc, "shim_FragColor"); return; }
    if (shim_tok_is(tok, len, "gl_FragData") &&
        (after = shim_bracket_int(rest, &idx)) != NULL && idx >= 0 && idx < 8) {
        shim_setrepf(tc, "shim_FragData_%d", idx);
        tc->skip = (size_t)(after - rest);
        return;
    }

    /* ftransform and G.2 vertex/fragment built-ins. */
    if (shim_tok_is(tok, len, "ftransform")) {
        const char *p = shim_skip_ws(rest);
        if (*p == '(') {
            p = shim_skip_ws(p + 1);
            if (*p == ')') {
                shim_setrep(tc, "(shim_ModelViewProjectionMatrix * shim_Vertex)");
                tc->skip = (size_t)(p + 1 - rest);
                return;
            }
        }
    }
    static const char *attrs[13] = {
        "gl_Vertex", "gl_Color", "gl_Normal",
        "gl_MultiTexCoord0", "gl_MultiTexCoord1", "gl_MultiTexCoord2",
        "gl_MultiTexCoord3", "gl_MultiTexCoord4", "gl_MultiTexCoord5",
        "gl_MultiTexCoord6", "gl_MultiTexCoord7",
        "gl_SecondaryColor", "gl_FogCoord"
    };
    static const char *attr_names[13] = {
        "shim_Vertex", "shim_Color", "shim_Normal",
        "shim_MultiTexCoord0", "shim_MultiTexCoord1", "shim_MultiTexCoord2",
        "shim_MultiTexCoord3", "shim_MultiTexCoord4", "shim_MultiTexCoord5",
        "shim_MultiTexCoord6", "shim_MultiTexCoord7",
        "shim_SecondaryColor", "shim_FogCoord"
    };
    for (int k = 0; k < 13; k++)
        if (shim_tok_is(tok, len, attrs[k])) {
            if (rc->type == GL_VERTEX_SHADER || (k != 1 && k != 11))
                shim_setrep(tc, attr_names[k]);
            else
                shim_setrep(tc, k == 1 ? "shim_FrontColor" : "shim_FrontSecondaryColor");
            return;
        }
    if (rc->type == GL_VERTEX_SHADER) {
        if (shim_tok_is(tok, len, "gl_FrontColor")) { shim_setrep(tc, "shim_FrontColor"); return; }
        if (shim_tok_is(tok, len, "gl_BackColor")) { shim_setrep(tc, "shim_BackColor"); return; }
        if (shim_tok_is(tok, len, "gl_FrontSecondaryColor")) { shim_setrep(tc, "shim_FrontSecondaryColor"); return; }
        if (shim_tok_is(tok, len, "gl_BackSecondaryColor")) { shim_setrep(tc, "shim_BackColor"); return; }
    } else {
        if (shim_tok_is(tok, len, "gl_Color")) { shim_setrep(tc, "shim_FrontColor"); return; }
        if (shim_tok_is(tok, len, "gl_SecondaryColor")) { shim_setrep(tc, "shim_FrontSecondaryColor"); return; }
    }
    if (shim_tok_is(tok, len, "gl_FogFragCoord")) { shim_setrep(tc, "shim_FogFragCoord"); return; }
    if (shim_tok_is(tok, len, "gl_TexCoord") &&
        (after = shim_bracket_int(rest, &idx)) != NULL && idx >= 0 && idx < 8) {
        shim_setrepf(tc, "shim_TexCoord%d", idx);
        tc->skip = (size_t)(after - rest);
        return;
    }

    /* Uniforms from the ShimFF block. */
    for (size_t i = 0; i < sizeof(shim_ff_uniforms)/sizeof(shim_ff_uniforms[0]); i++)
        if (shim_tok_is(tok, len, shim_ff_uniforms[i][0])) {
            shim_setrep(tc, shim_ff_uniforms[i][1]);
            return;
        }
    if (shim_tok_is(tok, len, "gl_MaxLights")) { shim_setrep(tc, "shim_MaxLights"); return; }
    if (shim_tok_is(tok, len, "gl_MaxTextureUnits")) { shim_setrep(tc, "shim_MaxTextureUnits"); return; }
    if (shim_tok_is(tok, len, "gl_MaxTextureCoords")) { shim_setrep(tc, "shim_MaxTextureCoords"); return; }
    if (shim_tok_is(tok, len, "gl_MaxClipPlanes")) { shim_setrep(tc, "shim_MaxClipPlanes"); return; }
    if (shim_tok_is(tok, len, "gl_MaxVaryingFloats")) { shim_setrep(tc, "shim_MaxVaryingFloats"); return; }

    /* sampler1D uniforms and texture() calls using them: rewritten to a 2D
   sampler plus a helper that forces the y coordinate to 0.5. */
    if (shim_tok_is(tok, len, "sampler1D")) { shim_setrep(tc, "sampler2D"); return; }
    if (shim_tok_is(tok, len, "isampler1D")) { shim_setrep(tc, "isampler2D"); return; }
    if (shim_tok_is(tok, len, "usampler1D")) { shim_setrep(tc, "usampler2D"); return; }
    if (shim_tok_is(tok, len, "texture1D")) { shim_setrep(tc, "shim_texture1D"); return; }
    if (shim_tok_is(tok, len, "texture1DLod")) { shim_setrep(tc, "shim_texture1DLod"); return; }
    if (shim_tok_is(tok, len, "texture1DProj")) { shim_setrep(tc, "shim_texture1DProj"); return; }
    if (shim_tok_is(tok, len, "texture") || shim_tok_is(tok, len, "textureLod") ||
        shim_tok_is(tok, len, "textureProj") || shim_tok_is(tok, len, "texelFetch") ||
        shim_tok_is(tok, len, "textureSize")) {
        const char *p = shim_skip_ws(rest);
        if (*p == '(') {
            p = shim_skip_ws(p + 1);
            if (shim_ident_start((unsigned char)*p)) {
                const char *e = p + 1;
                while (shim_ident_char((unsigned char)*e)) e++;
                const char *q = shim_skip_ws(e);
                if (*q == '[') { /* optional index on the uniform */
                    int dummy = 0;
                    const char *z = shim_bracket_int(q, &dummy);
                    if (z) q = shim_skip_ws(z);
                }
                if (*q == ',' && s1d_lookup(u, p, (size_t)(e - p)) >= 0) {
                    const char *rep = shim_tok_is(tok, len, "texture") ? "shim_texture1D" :
                        shim_tok_is(tok, len, "textureLod") ? "shim_texture1DLod" :
                        shim_tok_is(tok, len, "textureProj") ? "shim_texture1DProj" :
                        shim_tok_is(tok, len, "texelFetch") ? "shim_texelFetch1D" :
                        "shim_textureSize1D";
                    shim_setrep(tc, rep);
                }
            }
        }
        return;
    }

    /* double -> float (the GPU has no fp64). */
    if (shim_tok_is(tok, len, "double")) { shim_setrep(tc, "float"); return; }
    if (len == 5 && !memcmp(tok, "dvec", 4) && tok[4] >= '2' && tok[4] <= '4') {
        shim_setrepf(tc, "vec%c", tok[4]); return;
    }
    if (len == 5 && !memcmp(tok, "dmat", 4) && tok[4] >= '2' && tok[4] <= '4') {
        shim_setrepf(tc, "mat%c", tok[4]); return;
    }
    if (len == 7 && !memcmp(tok, "dmat", 4) && tok[4] >= '2' && tok[4] <= '4' &&
        tok[5] == 'x' && tok[6] >= '2' && tok[6] <= '4') {
        shim_setrepf(tc, "mat%cx%c", tok[4], tok[6]);
        return;
    }

    /* gl_ClipDistance / gl_ClipVertex. */
    if (shim_tok_is(tok, len, "gl_ClipDistance") && rc->type == GL_VERTEX_SHADER &&
        (after = shim_bracket_int(rest, &idx)) != NULL && idx >= 0 && idx < 8) {
        const char *p = shim_skip_ws(after);
        if (*p == '=' && p[1] != '=') {
            p = shim_skip_ws(p + 1);
            const char *e = p;
            int depth = 0;
            while (*e) {
                if (*e == '(') depth++;
                else if (*e == ')' && depth > 0) depth--;
                else if (*e == ';' && depth == 0) break;
                e++;
            }
            if (rc->clip_ext)
                shim_setrepf(tc, "gl_ClipDistance[%d] = ((shim_ClipEnableMask >> %d) & 1) != 0 ? (%.*s) : 1.0",
                             idx, idx, (int)(e - p), p);
            else
                shim_setrepf(tc, "shim_ClipDistanceSink[%d] = %.*s", idx, (int)(e - p), p);
            tc->skip = (size_t)(e - rest);
            return;
        }
        shim_setrepf(tc, "shim_ClipDistanceSink[%d]", idx);
        tc->skip = (size_t)(after - rest);
        return;
    }
    if (shim_tok_is(tok, len, "gl_ClipVertex") && rc->type == GL_VERTEX_SHADER) {
        const char *p = shim_skip_ws(rest);
        if (*p == '=' && p[1] != '=') {
            p = shim_skip_ws(p + 1);
            const char *e = p;
            int depth = 0;
            while (*e) {
                if (*e == '(') depth++;
                else if (*e == ')' && depth > 0) depth--;
                else if (*e == ';' && depth == 0) break;
                e++;
            }
            if (rc->clip_ext) {
                StrBuf sb = {0};
                sb_addf(&sb, "{ highp vec4 shim_cv = %.*s;", (int)(e - p), p);
                for (int k = 0; k < 8; k++)
                    sb_addf(&sb, " gl_ClipDistance[%d] = ((shim_ClipEnableMask >> %d) & 1) != 0 ? dot(shim_ClipPlane[%d], shim_cv) : 1.0;", k, k, k);
                sb_add(&sb, " }");
                shim_setrep(tc, sb.data ? sb.data : "");
                free(sb.data);
            } else {
                shim_setrepf(tc, "shim_ClipVertexSink = %.*s", (int)(e - p), p);
            }
            tc->skip = (size_t)(e - rest);
            return;
        }
        shim_setrep(tc, rc->clip_ext ? "gl_Position" : "shim_ClipVertexSink");
        return;
    }
    if (shim_tok_is(tok, len, "gl_ClipDistance")) { shim_setrep(tc, "shim_ClipDistanceSink"); return; }
    if (shim_tok_is(tok, len, "gl_ClipVertex")) { shim_setrep(tc, "shim_ClipVertexSink"); return; }

    /* 1.0lf literals: collected in a character-by-character pass below. */
    (void)user;
}

static void shim_strip_lf(StrBuf *s)
{
    if (!s->data) return;
    size_t w = 0;
    for (size_t r = 0; r < s->len; r++) {
        char ch = s->data[r];
        if ((ch == 'l' || ch == 'L') && s->data[r+1] == 'f' && r > 0 &&
            ((s->data[r-1] >= '0' && s->data[r-1] <= '9') || s->data[r-1] == '.')) {
            r++;
            continue;
        }
        s->data[w++] = ch;
    }
    s->len = w;
    s->data[w] = 0;
}

/* ------------------------------------------------------------------------- */
/* G/G.2: header parsing, declarations and shim_glShaderSource               */
/*                                                                           */
/* The app's source is split into a preprocessor header and a body. The      */
/* header is re-emitted with ES-compatible #version/#extension lines; the    */
/* body is scanned, rewritten and wrapped with the declarations the usage    */
/* scan found necessary.                                                     */
/* ------------------------------------------------------------------------- */

typedef struct SHdr {
    int version;              /* 100, 110, 120, ...; 0 if absent */
    int is_es;                /* '#version NNN es' or 100 */
    int implicit_es;          /* no version but with precision/highp... */
    size_t version_end;       /* offset just after the #version line */
    size_t hdr_end;           /* offset of the first non-preprocessor token */
    int head_lines;           /* header lines, for #line */
    char *head;               /* original directives and precision statements */
} SHdr;

static int shim_line_header(const char *p)
{
    if (*p == '#') return 1;
    if (!strncmp(p, "precision", 9) && (p[9] == ' ' || p[9] == '\t')) return 1;
    return 0;
}

static int parse_header(const char *src, SHdr *h)
{
    memset(h, 0, sizeof(*h));
    const char *p = src;
    /* Locate #version (comments may appear before it). */
    while (*p) {
        const char *q = shim_skip_ws(p);
        if (q[0] == '/' && q[1] == '/') { while (*q && *q != '\n') q++; p = q; continue; }
        if (q[0] == '/' && q[1] == '*') {
            q += 2;
            while (*q && !(q[0] == '*' && q[1] == '/')) q++;
            p = q + (*q ? 2 : 0);
            continue;
        }
        p = q;
        break;
    }
    if (!strncmp(p, "#version", 8)) {
        const char *e = p + 8;
        int v = 0;
        while (*e == ' ' || *e == '\t') e++;
        while (*e >= '0' && *e <= '9') { v = v * 10 + (*e++ - '0'); }
        h->version = v;
        while (*e && *e != '\n') {
            if (*e == 'e' && e[1] == 's') h->is_es = 1;
            e++;
        }
        if (v == 100) h->is_es = 1;
        if (*e) e++;
        h->version_end = (size_t)(e - src);
        p = e;
    }
    /* Header area: directives, precision, comments and blank lines. */
    StrBuf sb = {0};
    while (*p) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        int blank = (*q == '\n' || *q == '\r');
        int comment = (q[0] == '/' && (q[1] == '/' || q[1] == '*'));
        if (!shim_line_header(q) && !blank && !comment)
            break;
        const char *line = p;
        while (*p && *p != '\n') sb_addn(&sb, p++, 1);
        if (*p) sb_addn(&sb, p++, 1);
        if (comment && shim_line_header(line) == 0 && blank == 0 &&
            line[0] == '/' && line[1] == '*') {
            /* Multiline comments may continue: a line was already copied;
               the loop keeps copying while they still look like comments. */
        }
        (void)line;
    }
    h->hdr_end = (size_t)(p - src);
    for (size_t i = 0; i < h->hdr_end; i++)
        if (src[i] == '\n') h->head_lines++;
    if (!h->version) {
        if (strstr(src, "precision ") || strstr(src, "highp ") ||
            strstr(src, "mediump ") || strstr(src, "lowp "))
            h->implicit_es = 1;
    }
    /* App #extension lines become : enable; fp64 ones are emptied. */
    StrBuf fixed = {0};
    const char *l = sb.data;
    while (l && *l) {
        const char *line = l;
        while (*l && *l != '\n') l++;
        size_t n = (size_t)(l - line);
        if (n >= 11 && !strncmp(line, "#extension", 10)) {
            int fp64 = n >= 40 && strstr(line, "GL_ARB_gpu_shader_fp64") != NULL;
            if (!fp64) {
                const char *req = strstr(line, ": require");
                if (req) {
                    sb_addn(&fixed, line, (size_t)(req - line));
                    sb_add(&fixed, ": enable");
                    const char *tail = req + 9;
                    sb_addn(&fixed, tail, (size_t)((line + n) - tail));
                } else sb_addn(&fixed, line, n);
            } else {
                sb_add(&fixed, "\n");
            }
        } else sb_addn(&fixed, line, n);
        if (*l) sb_addn(&fixed, l++, 1);
    }
    free(sb.data);
    h->head = fixed.data ? fixed.data : strdup("");
    return 1;
}

static void append_ffdecl_vertex(StrBuf *d, const FFUse *u, const ShimCtx *c)
{
    static const char *names[13] = {
        "shim_Vertex", "shim_Color", "shim_Normal",
        "shim_MultiTexCoord0", "shim_MultiTexCoord1", "shim_MultiTexCoord2",
        "shim_MultiTexCoord3", "shim_MultiTexCoord4", "shim_MultiTexCoord5",
        "shim_MultiTexCoord6", "shim_MultiTexCoord7",
        "shim_SecondaryColor", "shim_FogCoord"
    };
    int maxa = c ? c->features.max_vertex_attribs : 16;
    if (maxa < 13) maxa = 13;
    for (int k = 0; k < 13; k++) {
        if (!(u->attr_mask & (1u << k))) continue;
        const char *ty = (k == 2) ? "vec3" : (k == 12) ? "float" : "vec4";
        sb_addf(d, "layout(location = %d) in highp %s %s;\n", maxa - 1 - k, ty, names[k]);
    }
    if (u->ftransform && !(u->attr_mask & 1u))
        sb_addf(d, "layout(location = %d) in highp vec4 shim_Vertex;\n", maxa - 1);
    if (u->vary_mask & FFV_FRONTCOLOR) sb_add(d, "out highp vec4 shim_FrontColor;\n");
    if (u->vary_mask & FFV_BACKCOLOR) sb_add(d, "highp vec4 shim_BackColor;\n");
    if (u->vary_mask & FFV_FSECCOLOR) sb_add(d, "out highp vec4 shim_FrontSecondaryColor;\n");
    if (u->vary_mask & FFV_FOG) sb_add(d, "out highp float shim_FogFragCoord;\n");
    for (int k = 0; k < 8; k++)
        if (u->vary_mask & (FFV_TEX0 << k))
            sb_addf(d, "out highp vec4 shim_TexCoord%d;\n", k);
}

static void append_ffdecl_fragment(StrBuf *d, const FFUse *u, const ShimCtx *c)
{
    if (u->vary_mask & FFV_FRONTCOLOR) sb_add(d, "in highp vec4 shim_FrontColor;\n");
    if (u->vary_mask & FFV_FSECCOLOR) sb_add(d, "in highp vec4 shim_FrontSecondaryColor;\n");
    if (u->vary_mask & FFV_FOG) sb_add(d, "in highp float shim_FogFragCoord;\n");
    for (int k = 0; k < 8; k++)
        if (u->vary_mask & (FFV_TEX0 << k))
            sb_addf(d, "in highp vec4 shim_TexCoord%d;\n", k);
    if (u->has_fragcolor) sb_add(d, "out highp vec4 shim_FragColor;\n");
    for (int k = 0; k < 8; k++)
        if (u->fragdata_mask & (1u << k))
            sb_addf(d, "layout(location = %d) out highp vec4 shim_FragData_%d;\n", k, k);
    /* A14/H.2: glAlphaFunc -> discard in the fragment (8 comparisons).
       The helper is always declared and self-disables via the UBO, so a
       glAlphaFunc called after compilation still takes effect. */
    if (c)
        sb_add(d,
            "void shim_alpha_apply(highp float a) {\n"
            " if (shim_AlphaParams.z < 0.5) return;\n"
            " highp float f = shim_AlphaParams.x, r = shim_AlphaParams.y;\n"
            " bool ok = false;\n"
            " if (f < 512.5) ok = false;\n"
            " else if (f < 513.5) ok = a < r;\n"
            " else if (f < 514.5) ok = a == r;\n"
            " else if (f < 515.5) ok = a <= r;\n"
            " else if (f < 516.5) ok = a > r;\n"
            " else if (f < 517.5) ok = a != r;\n"
            " else if (f < 518.5) ok = a >= r;\n"
            " else ok = true;\n"
            " if (!ok) discard;\n"
            "}\n");
}

static void append_ffdecl_ubo(StrBuf *d, const FFUse *u)
{
    if (!u->uni_mask)
        return;
    sb_add(d,
        "struct shim_LightSourceParameters {\n"
        " highp vec4 ambient; highp vec4 diffuse; highp vec4 specular;\n"
        " highp vec4 position; highp vec4 halfVector;\n"
        " highp vec3 spotDirection; highp float spotExponent;\n"
        " highp float spotCutoff; highp float spotCosCutoff;\n"
        " highp float constantAttenuation; highp float linearAttenuation;\n"
        " highp float quadraticAttenuation;\n"
        "};\n"
        "struct shim_MaterialParameters {\n"
        " highp vec4 emission; highp vec4 ambient; highp vec4 diffuse;\n"
        " highp vec4 specular; highp float shininess;\n"
        "};\n"
        "struct shim_LightModelParameters { highp vec4 ambient; };\n"
        "struct shim_LightModelProducts { highp vec4 sceneColor; };\n"
        "struct shim_LightProducts { highp vec4 ambient; highp vec4 diffuse; highp vec4 specular; };\n"
        "struct shim_FogParameters { highp vec4 color; highp float density;\n"
        " highp float start; highp float end; highp float scale; };\n"
        "layout(std140) uniform ShimFF {\n"
        " highp mat4 shim_ModelViewMatrix;\n"
        " highp mat4 shim_ProjectionMatrix;\n"
        " highp mat4 shim_ModelViewProjectionMatrix;\n"
        " highp mat4 shim_ModelViewMatrixInverse;\n"
        " highp mat4 shim_ProjectionMatrixInverse;\n"
        " highp mat4 shim_ModelViewProjectionMatrixInverse;\n"
        " highp mat4 shim_ModelViewMatrixTranspose;\n"
        " highp mat4 shim_ProjectionMatrixTranspose;\n"
        " highp mat4 shim_ModelViewProjectionMatrixTranspose;\n"
        " highp mat4 shim_ModelViewMatrixInverseTranspose;\n"
        " highp mat4 shim_ProjectionMatrixInverseTranspose;\n"
        " highp mat4 shim_ModelViewProjectionMatrixInverseTranspose;\n"
        " highp mat3 shim_NormalMatrix;\n"
        " highp float shim_NormalScale;\n"
        " highp mat4 shim_TextureMatrix[8];\n"
        " shim_LightSourceParameters shim_LightSource[8];\n"
        " shim_MaterialParameters shim_FrontMaterial;\n"
        " shim_MaterialParameters shim_BackMaterial;\n"
        " shim_LightModelParameters shim_LightModel;\n"
        " shim_LightModelProducts shim_FrontLightModelProduct;\n"
        " shim_LightModelProducts shim_BackLightModelProduct;\n"
        " shim_LightProducts shim_FrontLightProduct[8];\n"
        " shim_LightProducts shim_BackLightProduct[8];\n"
        " shim_FogParameters shim_Fog;\n"
        " highp vec4 shim_ClipPlane[8];\n"
        " highp vec4 shim_TextureEnvColor[8];\n"
        " highp int shim_ClipEnableMask;\n"
        " highp vec4 shim_AlphaParams;\n"
        " highp vec4 shim_PointParams;\n"
        " highp vec4 shim_TexEnvMode[8];\n"
        "};\n");
    if (u->maxlights) sb_add(d, "const highp int shim_MaxLights = 8;\n");
    if (u->maxtex) sb_add(d, "const highp int shim_MaxTextureUnits = 8;\n");
    if (u->maxtexcoord) sb_add(d, "const highp int shim_MaxTextureCoords = 8;\n");
    if (u->maxclip) sb_add(d, "const highp int shim_MaxClipPlanes = 8;\n");
    if (u->maxvarying) sb_add(d, "const highp int shim_MaxVaryingFloats = 32;\n");
}

static void append_s1d_helpers(StrBuf *d, const FFUse *u)
{
    int f = u->s1d || u->dbl, i = u->is1d, un = u->us1d;
    for (int k = 0; k < 3; k++) {
        if ((k == 0 && !f) || (k == 1 && !i) || (k == 2 && !un)) continue;
        const char *ty = k == 0 ? "vec4" : k == 1 ? "ivec4" : "uvec4";
        const char *sam = k == 0 ? "sampler2D" : k == 1 ? "isampler2D" : "usampler2D";
        sb_addf(d,
            "%s shim_texture1D(%s s, float x) { return texture(s, vec2(x, 0.5)); }\n"
            "%s shim_texture1D(%s s, float x, float b) { return texture(s, vec2(x, 0.5), b); }\n"
            "%s shim_texture1DLod(%s s, float x, float l) { return textureLod(s, vec2(x, 0.5), l); }\n"
            "%s shim_texture1DProj(%s s, vec2 c) { return texture(s, vec2(c.x / c.y, 0.5)); }\n"
            "%s shim_texture1DProj(%s s, vec4 c) { return texture(s, vec2(c.x / c.w, 0.5)); }\n"
            "%s shim_texelFetch1D(%s s, int x, int l) { return texelFetch(s, ivec2(x, 0), l); }\n"
            "int shim_textureSize1D(%s s, int l) { return textureSize(s, l).x; }\n",
            ty, sam, ty, sam, ty, sam, ty, sam, ty, sam, ty, sam, sam);
    }
}

static void append_ffdecl(StrBuf *d, const FFUse *u, int type,
                          const ShimCtx *c, int clip_ext)
{
    /* The UBO goes first: the alpha-test helper references it. */
    append_ffdecl_ubo(d, u);
    if (type == GL_VERTEX_SHADER) append_ffdecl_vertex(d, u, c);
    else append_ffdecl_fragment(d, u, c);
    if (u->s1d || u->is1d || u->us1d) append_s1d_helpers(d, u);
    if (u->clip && !clip_ext && type == GL_VERTEX_SHADER)
        sb_add(d, "highp float shim_ClipDistanceSink[8];\nhighp vec4 shim_ClipVertexSink;\n");
}

static int needs_310(const char *body)
{
    return strstr(body, "layout(binding") || strstr(body, "layout (binding") ||
           strstr(body, "imageLoad") || strstr(body, "imageStore") ||
           strstr(body, "atomicAdd") || strstr(body, "textureGather") ||
           strstr(body, "shared ") || strstr(body, "memoryBarrier");
}

struct ShimVShader;
static struct ShimVShader *shim_vshader_find(GLuint id);                /* G.3 */
static int shim_emu_virtual_kind(GLuint shader);                        /* G.3 */
static void shim_emu_on_shader_source(GLuint shader, const char *src); /* G.3 */
static void shim_emu_on_shader_type(GLuint shader, GLint type);        /* G.3 */

/* glShaderSource interception: keep the original desktop text for G.3 (its
   translator needs it), scan it for fixed-function usage, and submit either
   a translated ES source or the original one to the real driver. */
static void shim_glShaderSource(GLuint shader, GLsizei count,
                                const GLchar *const *strings, const GLint *length)
{
    ensure_gl_symbols();
    if (!real_glShaderSource || !strings || count <= 0)
        return;
    StrBuf src = {0};
    for (GLsizei i = 0; i < count; i++) {
        if (!strings[i]) continue;
        size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(strings[i]);
        sb_addn(&src, strings[i], n);
    }
    /* G.3.1: mark virtual shaders and capture their text;
       shim_emu_on_shader_type already registered the object. */
    if (!src.data) {
        const char empty[] = "";
        const GLchar *p = empty;
        real_glShaderSource(shader, 1, &p, NULL);
        return;
    }
    GLint type = 0;
    if (real_glGetShaderiv) real_glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    shim_emu_on_shader_type(shader, type);
    int emu_kind = shim_emu_virtual_kind(shader);
    if (emu_kind >= 1 && emu_kind <= 3) {
        /* The G.3 translator needs the original desktop source, not the G.2 one. */
        shim_emu_on_shader_source(shader, src.data);
        free(src.data);
        return;
    }

    ShimCtx *c = shim_get_ctx();
    int active = desktop_mode() && g_ff_enabled && c && c->features.es_major >= 3;
    SHdr h;
    parse_header(src.data, &h);
    const char *body = src.data + h.hdr_end;
    int is_es_native = h.is_es || h.version == 100 || h.implicit_es;
    int ff_active = 0;
    FFUse u;
    memset(&u, 0, sizeof(u));

    if (active && !is_es_native && type != GL_COMPUTE_SHADER) {
        scan_ff_usage(body, type, &u);
        if (c) {
            /* Reconstructed H state needs the UBO even when the app's shader
               mentions no fixed-function built-in at all. */
            u.uni_mask |= 1u;
        }
        if ((u.attr_mask | u.vary_mask | u.uni_mask) || u.ftransform || u.s1d ||
            u.is1d || u.us1d || u.dbl || u.clip || u.has_fragcolor)
            ff_active = 1;
    }
    if (active && !is_es_native && !ff_active && (h.version == 110 || h.version == 0)) {
        /* Case 1: GLSL 1.10 inherits ES 1.00; no token rewriting. */
        StrBuf out = {0};
        sb_add(&out, "#version 100\n");
        sb_add(&out, h.head);
        if (type == GL_FRAGMENT_SHADER)
            sb_add(&out, "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
        else sb_add(&out, "precision highp float;\n");
        sb_add(&out, body);
        real_glShaderSource(shader, 1, (const GLchar *const *)&out.data, NULL);
        shim_emu_on_shader_source(shader, out.data ? out.data : src.data);
        __atomic_add_fetch(&g_ff_shader_count, 1, __ATOMIC_RELAXED);
        free(out.data);
        free(h.head);
        free(src.data);
        return;
    }
    if (active && ff_active) {
        int ver310 = (c->features.es_minor >= 1) && needs_310(body);
        int clip_ext = c->features.has_clip_cull_distance;
        RewriteCtx rc = {0};
        rc.ff = 1;
        rc.u = &u;
        rc.type = type;
        rc.c = c;
        rc.clip_ext = clip_ext;
        StrBuf trans = {0};
        shim_lex_process(body, &trans, shim_rewrite_token, &rc);
        shim_strip_lf(&trans);

        /* Module H over G.2: apply legacy state at the end of main(). */
        if (c && type == GL_FRAGMENT_SHADER) {
            if (u.has_fragcolor)
                shim_inject_before_last_brace(&trans, "shim_alpha_apply(shim_FragColor.a);\n");
            else if (u.fragdata_mask & 1u)
                shim_inject_before_last_brace(&trans, "shim_alpha_apply(shim_FragData_0.a);\n");
        }
        if (c && type == GL_VERTEX_SHADER) {
            if (!strstr(body, "gl_PointSize"))
                shim_inject_before_last_brace(&trans,
                    "if (shim_PointParams.z > 0.5) gl_PointSize = shim_PointParams.x;\n");
            char code[256];
            for (int k = 0; k < 8; k++) {
                if (!(u.vary_mask & (FFV_TEX0 << k))) continue;
                /* gl_TexCoord[n] is initialised BEFORE main() with the texture matrix
                   already applied. Injecting it at the end of main was too
                   late when the shader copied gl_TexCoord into its own
                   varying near the start. */
                if (u.attr_mask & (1u << (3 + k)))
                    snprintf(code, sizeof(code),
                             "shim_TexCoord%d = shim_MultiTexCoord%d;\n"
                             "shim_TexCoord%d = shim_TextureMatrix[%d] * shim_TexCoord%d;\n",
                             k, k, k, k, k);
                else
                    snprintf(code, sizeof(code),
                             "shim_TexCoord%d = shim_TextureMatrix[%d] * shim_TexCoord%d;\n",
                             k, k, k);
                shim_inject_after_main_brace(&trans, code);
            }
        }

        StrBuf decl = {0};
        append_ffdecl(&decl, &u, type, c, clip_ext);

        StrBuf out = {0};
        sb_addf(&out, "#version %d es\n", ver310 ? 310 : 300);
        sb_add(&out, "#extension GL_EXT_shader_implicit_conversions : enable\n");
        if (clip_ext && u.clip && type == GL_VERTEX_SHADER)
            sb_add(&out, "#extension GL_EXT_clip_cull_distance : enable\n");
        sb_add(&out, h.head);
        sb_add(&out, "precision highp float;\nprecision highp int;\n");
        sb_add(&out, decl.data ? decl.data : "");
        if (g_debug) sb_addf(&out, "#line %d\n", h.head_lines + 1);
        sb_add(&out, trans.data ? trans.data : "");

        real_glShaderSource(shader, 1, (const GLchar *const *)&out.data, NULL);
        shim_emu_on_shader_source(shader, out.data ? out.data : src.data);
        __atomic_add_fetch(&g_ff_shader_count, 1, __ATOMIC_RELAXED);
        if (u.clip) c->ff_dirty |= FFD_CLIP;
        if (u.uni_mask || u.ftransform) c->ff_dirty |= FFD_ALL;
        if (u.need_variants)
            __atomic_store_n(&g_ff_need_variants, 1, __ATOMIC_RELAXED);
        shim_trace("G.2: shader %d translated (attrs=0x%x vary=0x%x uni=%d clip=%d dbl=%d)\n",
                   type, u.attr_mask, u.vary_mask, !!u.uni_mask, u.clip, u.dbl);
        free(out.data);
        free(decl.data);
        free(trans.data);
        free(h.head);
        free(src.data);
        return;
    }

    /* Case 2 v2 (including #version 110 without FF built-ins): light rewrite. */
    if (active && !is_es_native && type != GL_COMPUTE_SHADER) {
        RewriteCtx rc = {0};
        rc.ff = 1;
        rc.u = &u;
        rc.type = type;
        rc.c = c;
        StrBuf trans = {0};
        shim_lex_process(body, &trans, shim_rewrite_token, &rc);
        int ver310 = (c->features.es_minor >= 1) && needs_310(body);
        StrBuf out = {0};
        sb_addf(&out, "#version %d es\n", ver310 ? 310 : 300);
        sb_add(&out, "#extension GL_EXT_shader_implicit_conversions : enable\n");
        sb_add(&out, h.head);
        sb_add(&out, "precision highp float;\nprecision highp int;\n");
        if (type == GL_FRAGMENT_SHADER && u.has_fragcolor)
            sb_add(&out, "out highp vec4 shim_FragColor;\n");
        for (int k = 0; k < 8; k++)
            if (u.fragdata_mask & (1u << k))
                sb_addf(&out, "layout(location = %d) out highp vec4 shim_FragData_%d;\n", k, k);
        sb_add(&out, trans.data ? trans.data : "");
        real_glShaderSource(shader, 1, (const GLchar *const *)&out.data, NULL);
        shim_emu_on_shader_source(shader, out.data ? out.data : src.data);
        free(out.data);
        free(trans.data);
    } else {
        real_glShaderSource(shader, count, strings, length);
        shim_emu_on_shader_source(shader, src.data);
    }
    free(h.head);
    free(src.data);
}

/* ========================================================================== */
/* G.2.6/G.2.7: programs, fixed-function state and client arrays              */
/*                                                                            */
/* glLinkProgram binds the generated ShimFF block to a reserved UBO index and  */
/* caches the legacy attribute mask. The enable/state setters below only       */
/* update ShimCtx; ff_sync() uploads the result at the next draw.              */
/* ========================================================================== */

static void shim_emu_on_link_program(GLuint prog);   /* G.3 */
static void shim_emu_on_use_program(GLuint prog);    /* G.3 */
static void shim_emu_on_delete_program(GLuint prog); /* G.3 */

/* Link through the driver, then bind "ShimFF" to the reserved binding point
   and record which shim_* attributes the program declares. G.3 virtual
   programs are built from the same hook before any draw uses them. */
static void shim_glLinkProgram(GLuint prog)
{
    ensure_gl_symbols();
    if (!real_glLinkProgram) return;
    real_glLinkProgram(prog);
    shim_emu_on_link_program(prog);
    if (!__atomic_load_n(&g_ff_shader_count, __ATOMIC_RELAXED))
        return;
    ShimCtx *c = shim_get_ctx();
    GLint ok = 0;
    if (real_glGetProgramiv) real_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { ff_prog_set(prog, 0); return; }
    if (c && real_glGetUniformBlockIndex && real_glUniformBlockBinding) {
        GLuint idx = real_glGetUniformBlockIndex(prog, "ShimFF");
        if (idx != GL_INVALID_INDEX && c->ff_ubo_binding >= 0)
            real_glUniformBlockBinding(prog, idx, (GLuint)c->ff_ubo_binding);
    }
    uint32_t mask = 0;
    if (real_glGetAttribLocation)
        for (int k = 0; k < 13; k++)
            if (real_glGetAttribLocation(prog, shim_ff_attr_name(k)) >= 0)
                mask |= 1u << k;
    ff_prog_set(prog, mask);
    if (c) c->ff_dirty |= FFD_ALL;
}

static void shim_glUseProgram(GLuint prog)
{
    ensure_gl_symbols();
    if (!real_glUseProgram) return;
    real_glUseProgram(prog);
    ShimCtx *c = tls_ctx_ptr;
    if (c) {
        c->cur_prog = prog;
        if (__atomic_load_n(&g_ff_shader_count, __ATOMIC_RELAXED) && !c->ff_ready) {
            c->ff_dirty |= FFD_ALL;
            ff_sync(c);   /* create the UBO and assign its binding point */
        }
        if (c->ff_ready && ff_prog_mask(prog) &&
            real_glGetUniformBlockIndex && real_glUniformBlockBinding) {
            GLuint idx = real_glGetUniformBlockIndex(prog, "ShimFF");
            if (idx != GL_INVALID_INDEX)
                real_glUniformBlockBinding(prog, idx, (GLuint)c->ff_ubo_binding);
        }
    }
    shim_emu_on_use_program(prog);
}

/* Legacy caps (lighting, fog, alpha test, texture 1D/3D, clip planes, ...)
   have no GLES enable bit. Record them in ShimCtx and pretend they are
   enabled; no real glEnable call is issued for them. */
static int shim_is_legacy_cap(GLenum cap)
{
    if (cap == 0x0B50 /* LIGHTING */ || cap == 0x0B57 /* COLOR_MATERIAL */ ||
        cap == 0x0B60 /* FOG */ || cap == 0x0BA1 /* NORMALIZE */ ||
        cap == 0x0BC0 /* ALPHA_TEST */ || cap == 0x0C60 /* TEXTURE_GEN_S */ ||
        cap == 0x0C61 || cap == 0x0C62 || cap == 0x0C63 ||
        cap == GL_TEXTURE_1D || cap == 0x806F /* TEXTURE_3D */)
        return 1;
    if (cap >= 0x4000 && cap <= 0x4007) return 1; /* GL_LIGHT0..7 */
    if (cap >= 0x3000 && cap <= 0x3007) return 1; /* CLIP_PLANE/CLIP_DISTANCE */
    return 0;
}

/* H.3: glActiveTexture also selects the unit whose GL_TEXTURE_2D state
   and matrix stack are modified. The shim did not wrap it, so that state
   always remained on unit 0. */
SHIM_EXPORT(void, glActiveTexture, (GLenum t))
{
    ShimCtx *c = tls_ctx_ptr;
    shim_dl_record(c, DL_ACTIVE_TEXTURE, (int)t, 0, NULL, 0);
    if (c) {
        int u = (int)(t - GL_TEXTURE0);
        if (u >= 0 && u < 8) c->active_tex = u;
    }
    ensure_gl_symbols();
    if (real_glActiveTexture) real_glActiveTexture(t);
}

static void shim_glEnable(GLenum cap)
{
    ShimCtx *c = tls_ctx_ptr;
    shim_dl_record(c, DL_ENABLE, (int)cap, 0, NULL, 0);
    if (cap == GL_TEXTURE_2D) {
        if (c) c->tex2d_enabled[c->active_tex & 7] = 1;
        return; /* 2D is always active in GLES; only record the state */
    }
    if (shim_is_legacy_cap(cap)) {
        if (!c) return;
        if (cap == 0x0B50) c->lighting_enabled = 1;
        else if (cap == 0x0B60) c->fog_enabled = 1;
        else if (cap == 0x0B57) c->color_material = 1;
        else if (cap >= 0x3000 && cap <= 0x3007) {
            c->clip_enable_mask |= 1u << (cap - 0x3000);
            shim_ff_touch(c, FFD_CLIP);
        }
        return;
    }
    ensure_gl_symbols();
    if (real_glEnable) real_glEnable(cap);
}

static void shim_glDisable(GLenum cap)
{
    ShimCtx *c = tls_ctx_ptr;
    shim_dl_record(c, DL_DISABLE, (int)cap, 0, NULL, 0);
    if (cap == GL_TEXTURE_2D) {
        if (c) c->tex2d_enabled[c->active_tex & 7] = 0;
        return;
    }
    if (shim_is_legacy_cap(cap)) {
        if (!c) return;
        if (cap == 0x0B50) c->lighting_enabled = 0;
        else if (cap == 0x0B60) c->fog_enabled = 0;
        else if (cap == 0x0B57) c->color_material = 0;
        else if (cap >= 0x3000 && cap <= 0x3007) {
            c->clip_enable_mask &= ~(1u << (cap - 0x3000));
            shim_ff_touch(c, FFD_CLIP);
        }
        return;
    }
    ensure_gl_symbols();
    if (real_glDisable) real_glDisable(cap);
}

static GLboolean shim_glIsEnabled(GLenum cap)
{
    ShimCtx *c = tls_ctx_ptr;
    if (shim_is_legacy_cap(cap)) {
        if (!c) return GL_FALSE;
        if (cap == 0x0B50) return c->lighting_enabled ? GL_TRUE : GL_FALSE;
        if (cap == 0x0B60) return c->fog_enabled ? GL_TRUE : GL_FALSE;
        if (cap == 0x0B57) return c->color_material ? GL_TRUE : GL_FALSE;
        if (cap >= 0x3000 && cap <= 0x3007)
            return (c->clip_enable_mask & (1u << (cap - 0x3000))) ? GL_TRUE : GL_FALSE;
        return GL_FALSE;
    }
    ensure_gl_symbols();
    return real_glIsEnabled ? real_glIsEnabled(cap) : GL_FALSE;
}

/* ------------------------------------------------------------------------- */
/* Module H (minimal): matrix stacks and current state                        */
/* ------------------------------------------------------------------------- */

#define SHIM_MATRIX_MODELVIEW  0x1700
#define SHIM_MATRIX_PROJECTION 0x1701
#define SHIM_MATRIX_TEXTURE    0x1702

/* Matrix state is one GL stack per mode. FFD_TEX is set from the active
   texture stack, so animating glMatrix(GL_TEXTURE) after switching units
   still reaches the right shim_TextureMatrix[8] slot in the UBO. */
static float *shim_matrix_top(ShimCtx *c)
{
    if (!c) return NULL;
    switch (c->matrix_mode) {
    case SHIM_MATRIX_PROJECTION: return c->proj_stack[c->proj_top];
    case SHIM_MATRIX_TEXTURE:
        return c->tex_stack[c->client_active_tex & 7][c->tex_top[c->client_active_tex & 7]];
    default: return c->mv_stack[c->mv_top];
    }
}

/* G.2.5/H.2: the dirty bit depends on the active stack; FFD_TEX existed
   but was never set, leaving shim_TextureMatrix[8] zero in the UBO. */
static uint32_t shim_matrix_dirty(ShimCtx *c)
{
    if (c->matrix_mode == SHIM_MATRIX_PROJECTION) return FFD_PROJ;
    if (c->matrix_mode == SHIM_MATRIX_TEXTURE) return FFD_TEX;
    return FFD_MV;
}

static void shim_glMatrixMode(GLenum mode)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    shim_dl_record(c, DL_MATRIX_MODE, (int)mode, 0, NULL, 0);
    c->matrix_mode = (int)mode;
    if (mode == SHIM_MATRIX_TEXTURE) c->tex_matrix_touched = 1;
}

static void shim_glLoadIdentity(void)
{
    ShimCtx *c = shim_get_ctx();
    float *m = shim_matrix_top(c);
    if (!m || !c) return;
    shim_dl_record(c, DL_LOAD_IDENTITY, 0, 0, NULL, 0);
    identity(m);
    shim_ff_touch(c, shim_matrix_dirty(c));
}

static void shim_load_matrix(ShimCtx *c, const float *v, int mult)
{
    float *m = shim_matrix_top(c);
    if (!m || !c) return;
    shim_dl_record(c, mult ? DL_MATRIX_MULT : DL_MATRIX_LOAD, 0, 0, v, 16);
    if (mult) {
        float out[16];
        mat4_mul(out, m, v);
        memcpy(m, out, sizeof(out));
    } else memcpy(m, v, 16 * sizeof(float));
    if (c->matrix_mode == SHIM_MATRIX_TEXTURE) c->tex_matrix_touched = 1;
    shim_ff_touch(c, shim_matrix_dirty(c));
}

SHIM_EXPORT(void, glLoadMatrixf, (const GLfloat *m)) { shim_load_matrix(shim_get_ctx(), m, 0); }
SHIM_EXPORT(void, glLoadMatrixd, (const GLdouble *m))
{
    if (!m) return;
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    shim_load_matrix(shim_get_ctx(), f, 0);
}
SHIM_EXPORT(void, glMultMatrixf, (const GLfloat *m)) { shim_load_matrix(shim_get_ctx(), m, 1); }
SHIM_EXPORT(void, glMultMatrixd, (const GLdouble *m))
{
    if (!m) return;
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    shim_load_matrix(shim_get_ctx(), f, 1);
}
/* Fixed-depth stacks: overflow/underflow is a no-op, matching the GL error
   behaviour closely enough without propagating GL_INVALID_OPERATION into
   apps that do not check it. */
SHIM_EXPORT(void, glPushMatrix, (void))
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    shim_dl_record(c, DL_PUSH_MATRIX, 0, 0, NULL, 0);
    if (c->matrix_mode == SHIM_MATRIX_PROJECTION && c->proj_top < 31) {
        memcpy(c->proj_stack[c->proj_top + 1], c->proj_stack[c->proj_top], 16 * sizeof(float));
        c->proj_top++;
    } else if (c->matrix_mode == SHIM_MATRIX_TEXTURE) {
        int u = c->client_active_tex & 7;
        if (c->tex_top[u] < 31) {
            memcpy(c->tex_stack[u][c->tex_top[u] + 1], c->tex_stack[u][c->tex_top[u]], 16 * sizeof(float));
            c->tex_top[u]++;
        }
    } else if (c->mv_top < 31) {
        memcpy(c->mv_stack[c->mv_top + 1], c->mv_stack[c->mv_top], 16 * sizeof(float));
        c->mv_top++;
    }
}
SHIM_EXPORT(void, glPopMatrix, (void))
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    shim_dl_record(c, DL_POP_MATRIX, 0, 0, NULL, 0);
    if (c->matrix_mode == SHIM_MATRIX_PROJECTION && c->proj_top > 0) c->proj_top--;
    else if (c->matrix_mode == SHIM_MATRIX_TEXTURE) {
        int u = c->client_active_tex & 7;
        if (c->tex_top[u] > 0) c->tex_top[u]--;
    } else if (c->mv_top > 0) c->mv_top--;
    shim_ff_touch(c, shim_matrix_dirty(c));
}

/* H.1: attribute stack. Saves the full legacy state read by G.2; masks not
   representable in GLES (hint/line/polygon) are accepted and ignored. Display
   lists are not stacked (H.4, outside this subset). */
SHIM_EXPORT(void, glPushAttrib, (GLbitfield mask))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || c->attrib_top >= 16 || !mask) return;
    ShimAttribState *s = &c->attrib[c->attrib_top++];
    s->matrix_mode = c->matrix_mode;
    s->mv_top = c->mv_top; s->proj_top = c->proj_top;
    s->client_active_tex = c->client_active_tex;
    s->active_tex = c->active_tex;
    s->point_size_set = c->point_size_set;
    for (int i = 0; i < 8; i++) s->tex_top[i] = c->tex_top[i];
    memcpy(s->current_color, c->current_color, sizeof(s->current_color));
    memcpy(s->current_normal, c->current_normal, sizeof(s->current_normal));
    memcpy(s->current_texcoord, c->current_texcoord, sizeof(s->current_texcoord));
    memcpy(s->secondary_color, c->secondary_color, sizeof(s->secondary_color));
    s->fog_coord = c->fog_coord; s->point_size = c->point_size;
    memcpy(s->tex2d_enabled, c->tex2d_enabled, sizeof(s->tex2d_enabled));
    memcpy(s->texenv_mode, c->texenv_mode, sizeof(s->texenv_mode));
    memcpy(s->texenv_color, c->texenv_color, sizeof(s->texenv_color));
    s->alpha_test = c->alpha_test; s->alpha_func = c->alpha_func;
    s->alpha_ref = c->alpha_ref; s->lighting_enabled = c->lighting_enabled;
    s->fog_enabled = c->fog_enabled; s->shade_model = c->shade_model;
    s->color_material = c->color_material;
    s->color_material_face = c->color_material_face;
    s->color_material_mode = c->color_material_mode;
    s->clip_enable_mask = c->clip_enable_mask; s->fog_mode = c->fog_mode;
    for (int i = 0; i < SHIM_ARR_COUNT; i++) s->arrays_enabled[i] = c->arrays[i].enabled;
}

SHIM_EXPORT(void, glPopAttrib, (void))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || c->attrib_top <= 0) return;
    const ShimAttribState *s = &c->attrib[--c->attrib_top];
    c->matrix_mode = s->matrix_mode;
    c->mv_top = s->mv_top; c->proj_top = s->proj_top;
    c->client_active_tex = s->client_active_tex;
    c->active_tex = s->active_tex;
    c->point_size_set = s->point_size_set;
    for (int i = 0; i < 8; i++) c->tex_top[i] = s->tex_top[i];
    memcpy(c->current_color, s->current_color, sizeof(c->current_color));
    memcpy(c->current_normal, s->current_normal, sizeof(c->current_normal));
    memcpy(c->current_texcoord, s->current_texcoord, sizeof(c->current_texcoord));
    memcpy(c->secondary_color, s->secondary_color, sizeof(c->secondary_color));
    c->fog_coord = s->fog_coord; c->point_size = s->point_size;
    memcpy(c->tex2d_enabled, s->tex2d_enabled, sizeof(c->tex2d_enabled));
    memcpy(c->texenv_mode, s->texenv_mode, sizeof(c->texenv_mode));
    memcpy(c->texenv_color, s->texenv_color, sizeof(c->texenv_color));
    c->alpha_test = s->alpha_test; c->alpha_func = s->alpha_func;
    c->alpha_ref = s->alpha_ref; c->lighting_enabled = s->lighting_enabled;
    c->fog_enabled = s->fog_enabled; c->shade_model = s->shade_model;
    c->color_material = s->color_material;
    c->color_material_face = s->color_material_face;
    c->color_material_mode = s->color_material_mode;
    c->clip_enable_mask = s->clip_enable_mask; c->fog_mode = s->fog_mode;
    for (int i = 0; i < SHIM_ARR_COUNT; i++) c->arrays[i].enabled = s->arrays_enabled[i];
    shim_ff_touch(c, FFD_ALL);
}

static void shim_matrix_mul_op(ShimCtx *c, const float m[16])
{
    shim_load_matrix(c, m, 1);
}

SHIM_EXPORT(void, glTranslatef, (GLfloat x, GLfloat y, GLfloat z))
{
    float m[16]; identity(m); m[12]=x; m[13]=y; m[14]=z;
    shim_matrix_mul_op(shim_get_ctx(), m);
}
SHIM_EXPORT(void, glTranslated, (GLdouble x, GLdouble y, GLdouble z))
{ glTranslatef((float)x, (float)y, (float)z); }
SHIM_EXPORT(void, glScalef, (GLfloat x, GLfloat y, GLfloat z))
{
    float m[16] = {0}; m[0]=x; m[5]=y; m[10]=z; m[15]=1;
    shim_matrix_mul_op(shim_get_ctx(), m);
}
SHIM_EXPORT(void, glScaled, (GLdouble x, GLdouble y, GLdouble z))
{ glScalef((float)x, (float)y, (float)z); }
SHIM_EXPORT(void, glRotatef, (GLfloat angle, GLfloat x, GLfloat y, GLfloat z))
{
    float m[16]; identity(m);
    float r = angle * 3.14159265f / 180.0f;
    float s = sinf(r), co = cosf(r), len = sqrtf(x*x + y*y + z*z);
    if (len < 1e-8f) { x=0; y=0; z=1; len=1; }
    x /= len; y /= len; z /= len;
    float ic = 1.0f - co;
    m[0] = x*x*ic + co;   m[1] = y*x*ic + z*s; m[2] = z*x*ic - y*s;
    m[4] = x*y*ic - z*s;  m[5] = y*y*ic + co;  m[6] = z*y*ic + x*s;
    m[8] = x*z*ic + y*s;  m[9] = y*z*ic - x*s; m[10] = z*z*ic + co;
    shim_matrix_mul_op(shim_get_ctx(), m);
}
SHIM_EXPORT(void, glRotated, (GLdouble a, GLdouble x, GLdouble y, GLdouble z))
{ glRotatef((float)a, (float)x, (float)y, (float)z); }
SHIM_EXPORT(void, glOrtho, (GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                            GLdouble n, GLdouble f))
{
    float m[16] = {0};
    float L=(float)l,R=(float)r,B=(float)b,T=(float)t,N=(float)n,F=(float)f;
    m[0] = 2/(R-L); m[5] = 2/(T-B); m[10] = -2/(F-N);
    m[12] = -(R+L)/(R-L); m[13] = -(T+B)/(T-B); m[14] = -(F+N)/(F-N); m[15] = 1;
    shim_matrix_mul_op(shim_get_ctx(), m);
}
SHIM_EXPORT(void, glFrustum, (GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                              GLdouble n, GLdouble f))
{
    float m[16] = {0};
    float L=(float)l,R=(float)r,B=(float)b,T=(float)t,N=(float)n,F=(float)f;
    m[0] = 2*N/(R-L); m[5] = 2*N/(T-B);
    m[8] = (R+L)/(R-L); m[9] = (T+B)/(T-B);
    m[10] = -(F+N)/(F-N); m[11] = -1; m[14] = -2*F*N/(F-N);
    shim_matrix_mul_op(shim_get_ctx(), m);
}

/* ------------------------------------------------------------------------- */
/* G.2.7: colors, normals, texcoords, lights, materials, fog, clip            */
/* ------------------------------------------------------------------------- */

/* Current color is part of the emulated fixed-function state: it feeds the
   current-color fallback in ff_feed_user_program() and is returned by
   glGetFloatv(GL_CURRENT_COLOR). All typed glColor* entry points funnel here. */
static void shim_color4f(float r, float g, float b, float a)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    float v[4] = {r,g,b,a};
    shim_dl_record(c, DL_COLOR4, 0, 0, v, 4);
    c->current_color[0]=r; c->current_color[1]=g; c->current_color[2]=b; c->current_color[3]=a;
    if (c->color_material && c->color_material_mode == 0x1602 /* AMBIENT_AND_DIFFUSE */) {
        ShimFFMaterial *m = (c->color_material_face == GL_BACK) ? &c->ff.back_mat : &c->ff.front_mat;
        for (int i = 0; i < 3; i++) { m->ambient[i]=r*(i==0?1:0); m->diffuse[i]=r; }
        m->ambient[0]=r; m->ambient[1]=g; m->ambient[2]=b;
        m->diffuse[0]=r; m->diffuse[1]=g; m->diffuse[2]=b;
        shim_ff_touch(c, FFD_MAT);
    }
}
SHIM_EXPORT(void, glColor3f, (GLfloat r, GLfloat g, GLfloat b)) { shim_color4f(r,g,b,1); }
SHIM_EXPORT(void, glColor4f, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) { shim_color4f(r,g,b,a); }
SHIM_EXPORT(void, glColor3fv, (const GLfloat *v)) { if (v) shim_color4f(v[0],v[1],v[2],1); }
SHIM_EXPORT(void, glColor4fv, (const GLfloat *v)) { if (v) shim_color4f(v[0],v[1],v[2],v[3]); }
SHIM_EXPORT(void, glColor3ub, (GLubyte r, GLubyte g, GLubyte b))
{ shim_color4f(r/255.0f,g/255.0f,b/255.0f,1); }
SHIM_EXPORT(void, glColor4ub, (GLubyte r, GLubyte g, GLubyte b, GLubyte a))
{ shim_color4f(r/255.0f,g/255.0f,b/255.0f,a/255.0f); }
SHIM_EXPORT(void, glColor3ubv, (const GLubyte *v)) { if (v) glColor3ub(v[0],v[1],v[2]); }
SHIM_EXPORT(void, glColor4ubv, (const GLubyte *v)) { if (v) glColor4ub(v[0],v[1],v[2],v[3]); }
SHIM_EXPORT(void, glColor3d, (GLdouble r, GLdouble g, GLdouble b))
{ shim_color4f((float)r,(float)g,(float)b,1); }
SHIM_EXPORT(void, glColor4d, (GLdouble r, GLdouble g, GLdouble b, GLdouble a))
{ shim_color4f((float)r,(float)g,(float)b,(float)a); }
SHIM_EXPORT(void, glColor3dv, (const GLdouble *v)) { if (v) glColor3d(v[0],v[1],v[2]); }
SHIM_EXPORT(void, glColor4dv, (const GLdouble *v)) { if (v) glColor4d(v[0],v[1],v[2],v[3]); }

SHIM_EXPORT(void, glNormal3f, (GLfloat x, GLfloat y, GLfloat z))
{
    ShimCtx *c=shim_get_ctx();
    if (!c) return;
    float v[3] = {x,y,z};
    shim_dl_record(c, DL_NORMAL3, 0, 0, v, 3);
    c->current_normal[0]=x; c->current_normal[1]=y; c->current_normal[2]=z;
}
SHIM_EXPORT(void, glNormal3fv, (const GLfloat *v)) { if (v) glNormal3f(v[0],v[1],v[2]); }
SHIM_EXPORT(void, glNormal3d, (GLdouble x, GLdouble y, GLdouble z))
{ glNormal3f((float)x,(float)y,(float)z); }
SHIM_EXPORT(void, glNormal3dv, (const GLdouble *v)) { if (v) glNormal3d(v[0],v[1],v[2]); }

static void shim_texcoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    float v[4] = {s,t,r,q};
    shim_dl_record(c, DL_TEXCOORD4, 0, 0, v, 4);
    int u = c->client_active_tex & 7;
    c->current_texcoord[u][0]=s; c->current_texcoord[u][1]=t;
    c->current_texcoord[u][2]=r; c->current_texcoord[u][3]=q;
}
SHIM_EXPORT(void, glTexCoord2f, (GLfloat s, GLfloat t)) { shim_texcoord4f(s,t,0,1); }
SHIM_EXPORT(void, glTexCoord2fv, (const GLfloat *v)) { if (v) shim_texcoord4f(v[0],v[1],0,1); }
SHIM_EXPORT(void, glTexCoord3f, (GLfloat s, GLfloat t, GLfloat r)) { shim_texcoord4f(s,t,r,1); }
SHIM_EXPORT(void, glTexCoord3fv, (const GLfloat *v)) { if (v) shim_texcoord4f(v[0],v[1],v[2],1); }
SHIM_EXPORT(void, glTexCoord4f, (GLfloat s, GLfloat t, GLfloat r, GLfloat q)) { shim_texcoord4f(s,t,r,q); }
SHIM_EXPORT(void, glTexCoord4fv, (const GLfloat *v)) { if (v) shim_texcoord4f(v[0],v[1],v[2],v[3]); }
SHIM_EXPORT(void, glMultiTexCoord2fARB, (GLenum t, GLfloat s, GLfloat tt))
{ ShimCtx *c=shim_get_ctx(); if (!c) return; int old=c->client_active_tex;
  c->client_active_tex=(int)(t-GL_TEXTURE0); shim_texcoord4f(s,tt,0,1); c->client_active_tex=old; }
SHIM_EXPORT(void, glMultiTexCoord2f, (GLenum t, GLfloat s, GLfloat tt))
{ glMultiTexCoord2fARB(t, s, tt); }
SHIM_EXPORT(void, glClientActiveTextureARB, (GLenum t))
{ ShimCtx *c=shim_get_ctx(); if (c) c->client_active_tex=(int)(t-GL_TEXTURE0); }
SHIM_EXPORT(void, glClientActiveTexture, (GLenum t))
{ ShimCtx *c=shim_get_ctx(); if (c) c->client_active_tex=(int)(t-GL_TEXTURE0); }

SHIM_EXPORT(void, glSecondaryColor3f, (GLfloat r, GLfloat g, GLfloat b))
{ ShimCtx *c=shim_get_ctx(); if (c) { c->secondary_color[0]=r;c->secondary_color[1]=g;c->secondary_color[2]=b; } }
SHIM_EXPORT(void, glSecondaryColor3fv, (const GLfloat *v)) { if (v) glSecondaryColor3f(v[0],v[1],v[2]); }
SHIM_EXPORT(void, glSecondaryColor3ub, (GLubyte r, GLubyte g, GLubyte b))
{ glSecondaryColor3f(r/255.0f,g/255.0f,b/255.0f); }
SHIM_EXPORT(void, glFogCoordfEXT, (GLfloat f)) { ShimCtx *c=shim_get_ctx(); if (c) c->fog_coord=f; }
SHIM_EXPORT(void, glFogCoordfvEXT, (const GLfloat *v)) { if (v) glFogCoordfEXT(v[0]); }
SHIM_EXPORT(void, glFogCoordf, (GLfloat f)) { glFogCoordfEXT(f); }
SHIM_EXPORT(void, glFogCoordfv, (const GLfloat *v)) { glFogCoordfvEXT(v); }

static ShimArray *shim_array_for_cap(GLenum cap, ShimCtx *c)
{
    if (!c) return NULL;
    if (cap == GL_VERTEX_ARRAY) return &c->arrays[SHIM_ARR_VERTEX];
    if (cap == GL_NORMAL_ARRAY) return &c->arrays[SHIM_ARR_NORMAL];
    if (cap == GL_COLOR_ARRAY) return &c->arrays[SHIM_ARR_COLOR];
    if (cap == GL_TEXTURE_COORD_ARRAY) return &c->arrays[SHIM_ARR_TEX0 + (c->client_active_tex & 7)];
    if (cap == 0x845E) return &c->arrays[SHIM_ARR_SECONDARY];
    if (cap == 0x8457) return &c->arrays[SHIM_ARR_FOGCOORD];
    return NULL;
}

SHIM_EXPORT(void, glEnableClientState, (GLenum cap))
{
    ShimCtx *c = shim_get_ctx();
    ShimArray *a = shim_array_for_cap(cap, c);
    if (a) { a->enabled = 1; c->legacy_va_enabled = 1; return; }
    shim_glEnable(cap);
}
SHIM_EXPORT(void, glDisableClientState, (GLenum cap))
{
    ShimCtx *c = shim_get_ctx();
    ShimArray *a = shim_array_for_cap(cap, c);
    if (a) {
        a->enabled = 0;
        if (c) {
            c->legacy_va_enabled = 0;
            for (int i = 0; i < SHIM_ARR_COUNT; i++)
                c->legacy_va_enabled |= c->arrays[i].enabled;
        }
        return;
    }
    shim_glDisable(cap);
}
SHIM_EXPORT(GLboolean, glIsEnabledARB, (GLenum cap)) { return shim_glIsEnabled(cap); }

#define SHIM_ARRAY_PTR(name, idx) \
    SHIM_EXPORT(void, name, (GLint size, GLenum type, GLsizei stride, const void *ptr)) \
    { ShimCtx *c=shim_get_ctx(); if (!c) return; \
      ShimArray *a=&c->arrays[idx]; a->size=size; a->type=type; a->stride=stride; \
      a->ptr=ptr; a->vbo=0; a->normalized=0; c->legacy_va_enabled=1; (void)real_glGetIntegerv; }
SHIM_ARRAY_PTR(glVertexPointer, SHIM_ARR_VERTEX)
SHIM_ARRAY_PTR(glNormalPointer, SHIM_ARR_NORMAL)
SHIM_ARRAY_PTR(glColorPointer, SHIM_ARR_COLOR)
SHIM_ARRAY_PTR(glTexCoordPointer, SHIM_ARR_TEX0)
SHIM_ARRAY_PTR(glSecondaryColorPointerEXT, SHIM_ARR_SECONDARY)
SHIM_ARRAY_PTR(glFogCoordPointerEXT, SHIM_ARR_FOGCOORD)
/* Appendix C: unsuffixed aliases required by spec v2.1 (2.6/C7). Some
   ports call the ARB/EXT spelling, others the core spelling; both must land
   on the same wrapper so state is recorded only once. */
SHIM_ARRAY_PTR(glSecondaryColorPointer, SHIM_ARR_SECONDARY)
SHIM_ARRAY_PTR(glFogCoordPointer, SHIM_ARR_FOGCOORD)
#undef SHIM_ARRAY_PTR

/* Active texture for the coord array: tracked in glClientActiveTexture. */
SHIM_EXPORT(void, glTexCoordPointerARB, (GLint size, GLenum type, GLsizei stride, const void *ptr))
{ glTexCoordPointer(size, type, stride, ptr); }

static void shim_light_param(GLenum light, GLenum pname, const float *v, int is_int)
{
    ShimCtx *c = shim_get_ctx();
    if (!c || light < GL_LIGHT0 || light > GL_LIGHT0 + 7) return;
    ShimFFLight *l = &c->ff.light[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: {
        float *dst = pname == GL_AMBIENT ? l->ambient :
                     pname == GL_DIFFUSE ? l->diffuse : l->specular;
        for (int i = 0; i < 3; i++) dst[i] = is_int ? v[i] / 255.0f : v[i];
        dst[3] = is_int ? v[3] / 255.0f : v[3];
        break;
    }
    case GL_POSITION: {
        float eye[4] = {v[0], v[1], v[2], v[3]};
        float out[4];
        const float *m = c->mv_stack[c->mv_top];
        for (int i = 0; i < 4; i++)
            out[i] = m[i]*eye[0] + m[4+i]*eye[1] + m[8+i]*eye[2] + m[12+i]*eye[3];
        memcpy(l->position, out, 4*sizeof(float));
        break;
    }
    case GL_SPOT_DIRECTION: {
        float inv[16]; mat4_inverse(inv, c->mv_stack[c->mv_top]);
        float it[16]; mat4_transpose(it, inv);
        const float *m = it;
        l->spotDirection[0] = m[0]*v[0] + m[4]*v[1] + m[8]*v[2];
        l->spotDirection[1] = m[1]*v[0] + m[5]*v[1] + m[9]*v[2];
        l->spotDirection[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2];
        break;
    }
    case GL_SPOT_EXPONENT: l->spotExponent = v[0]; break;
    case GL_SPOT_CUTOFF: l->spotCutoff = v[0]; c->light_spot_cutoff[light-GL_LIGHT0]=v[0]; break;
    case GL_CONSTANT_ATTENUATION: l->constantAttenuation = v[0]; break;
    case GL_LINEAR_ATTENUATION: l->linearAttenuation = v[0]; break;
    case GL_QUADRATIC_ATTENUATION: l->quadraticAttenuation = v[0]; break;
    default: return;
    }
    shim_ff_touch(c, FFD_LIGHT);
}

SHIM_EXPORT(void, glLightf, (GLenum light, GLenum p, GLfloat v))
{ shim_light_param(light, p, &(float){v}, 0); }
SHIM_EXPORT(void, glLightfv, (GLenum light, GLenum p, const GLfloat *v))
{ if (v) shim_light_param(light, p, v, 0); }
SHIM_EXPORT(void, glLighti, (GLenum light, GLenum p, GLint v))
{ shim_light_param(light, p, &(float){(float)v}, 1); }
SHIM_EXPORT(void, glLightiv, (GLenum light, GLenum p, const GLint *v))
{ if (v) { float f[4]; for (int i=0;i<4;i++) f[i]=(float)v[i]; shim_light_param(light,p,f,1); } }

static void shim_material_param(GLenum face, GLenum pname, const float *v, int is_int)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    ShimFFMaterial *m = face == GL_FRONT ? &c->ff.front_mat :
                        face == GL_BACK ? &c->ff.back_mat : &c->ff.front_mat;
    float *dst = NULL;
    switch (pname) {
    case GL_EMISSION: dst = m->emission; break;
    case GL_AMBIENT: dst = m->ambient; break;
    case GL_DIFFUSE: dst = m->diffuse; break;
    case GL_SPECULAR: dst = m->specular; break;
    case GL_AMBIENT_AND_DIFFUSE:
        for (int i = 0; i < 3; i++) {
            m->ambient[i] = is_int ? v[i]/255.0f : v[i];
            m->diffuse[i] = m->ambient[i];
        }
        m->ambient[3] = m->diffuse[3] = 1.0f;
        shim_ff_touch(c, FFD_MAT);
        return;
    case GL_SHININESS: m->shininess = v[0]; shim_ff_touch(c, FFD_MAT); return;
    default: return;
    }
    for (int i = 0; i < 3; i++) dst[i] = is_int ? v[i]/255.0f : v[i];
    dst[3] = 1.0f;
    if (face == GL_FRONT_AND_BACK) {
        ShimFFMaterial *o = &c->ff.back_mat;
        float *od = pname == GL_EMISSION ? o->emission : pname == GL_AMBIENT ? o->ambient :
                    pname == GL_DIFFUSE ? o->diffuse : o->specular;
        memcpy(od, dst, 4*sizeof(float));
    }
    shim_ff_touch(c, FFD_MAT);
}

SHIM_EXPORT(void, glMaterialf, (GLenum face, GLenum p, GLfloat v))
{ shim_material_param(face,p,&(float){v},0); }
SHIM_EXPORT(void, glMaterialfv, (GLenum face, GLenum p, const GLfloat *v))
{ if (v) shim_material_param(face,p,v,0); }
SHIM_EXPORT(void, glMateriali, (GLenum face, GLenum p, GLint v))
{ shim_material_param(face,p,&(float){(float)v},1); }
SHIM_EXPORT(void, glMaterialiv, (GLenum face, GLenum p, const GLint *v))
{ if (v) { float f[4]; for (int i=0;i<4;i++) f[i]=(float)v[i]; shim_material_param(face,p,f,1); } }

static void shim_lightmodel(const float *v)
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !v) return;
    for (int i = 0; i < 3; i++) c->ff.lm_ambient[i] = v[i];
    c->ff.lm_ambient[3] = 1.0f;
    shim_ff_touch(c, FFD_MAT);
}
SHIM_EXPORT(void, glLightModelf, (GLenum p, const GLfloat *v))
{ if (p == 0x0B53) shim_lightmodel(v); }
SHIM_EXPORT(void, glLightModelfv, (GLenum p, const GLfloat *v))
{ if (p == 0x0B53) shim_lightmodel(v); }
SHIM_EXPORT(void, glLightModeli, (GLenum p, const GLint *v))
{ if (p == 0x0B53 && v) { float f[4]; for (int i=0;i<4;i++) f[i]=(float)v[i]; shim_lightmodel(f); } }
SHIM_EXPORT(void, glLightModeliv, (GLenum p, const GLint *v))
{ glLightModeli(p, v); }

static void shim_fog_param(GLenum p, float v)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    switch (p) {
    case 0x0B62: c->ff.fog.density = v; break;
    case 0x0B63: c->ff.fog.start = v; break;
    case 0x0B64: c->ff.fog.end = v; break;
    case 0x0B65: c->fog_mode = (int)v; break;
    default: return;
    }
    shim_ff_touch(c, FFD_FOG);
}
SHIM_EXPORT(void, glFogf, (GLenum p, GLfloat v)) { shim_fog_param(p, v); }
SHIM_EXPORT(void, glFogi, (GLenum p, GLint v)) { shim_fog_param(p, (float)v); }
SHIM_EXPORT(void, glFogfv, (GLenum p, const GLfloat *v))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !v) return;
    if (p == 0x0B66) {
        memcpy(c->ff.fog.color, v, 4*sizeof(float));
        shim_ff_touch(c, FFD_FOG);
        return;
    }
    shim_fog_param(p, v[0]);
}
SHIM_EXPORT(void, glFogiv, (GLenum p, const GLint *v))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !v) return;
    if (p == 0x0B66) {
        for (int i = 0; i < 4; i++) c->ff.fog.color[i] = v[i] / 255.0f;
        shim_ff_touch(c, FFD_FOG);
        return;
    }
    shim_fog_param(p, (float)v[0]);
}

SHIM_EXPORT(void, glClipPlane, (GLenum plane, const GLdouble *eqn))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !eqn || plane < GL_CLIP_PLANE0 || plane > GL_CLIP_PLANE0 + 7) return;
    float inv[16];
    mat4_inverse(inv, c->mv_stack[c->mv_top]);
    float o[4];
    for (int i = 0; i < 4; i++)
        o[i] = (float)(inv[i]*eqn[0] + inv[4+i]*eqn[1] + inv[8+i]*eqn[2] + inv[12+i]*eqn[3]);
    memcpy(c->ff.clip_plane[plane - GL_CLIP_PLANE0], o, 4*sizeof(float));
    shim_ff_touch(c, FFD_CLIP);
}

SHIM_EXPORT(void, glColorMaterial, (GLenum face, GLenum mode))
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    c->color_material_face = (int)face;
    c->color_material_mode = (int)mode;
}

SHIM_EXPORT(void, glTexEnvf, (GLenum target, GLenum pname, GLfloat param))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || target != GL_TEXTURE_ENV) return;
    shim_dl_record(c, DL_TEXENV_MODE, (int)target, (int)pname, &param, 1);
    int u = c->client_active_tex & 7;
    if (pname == GL_TEXTURE_ENV_MODE) {
        if (param != GL_MODULATE && param != GL_REPLACE && param != GL_DECAL &&
            param != GL_ADD && param != GL_BLEND)
            return;
        c->texenv_mode[u] = (int)param;
        shim_ff_touch(c, FFD_TEXENV);
    }
    /* GL_TEXTURE_ENV_COLOR is delivered via glTexEnvfv/iv (4 components). */
}
SHIM_EXPORT(void, glTexEnvfv, (GLenum target, GLenum pname, const GLfloat *params))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || target != GL_TEXTURE_ENV || !params) return;
    int u = c->client_active_tex & 7;
    if (pname == GL_TEXTURE_ENV_COLOR) {
        shim_dl_record(c, DL_TEXENV_COLOR, (int)target, (int)pname, params, 4);
        memcpy(c->texenv_color[u], params, 4*sizeof(float));
        shim_ff_touch(c, FFD_TEXENV);
    } else if (pname == GL_TEXTURE_ENV_MODE) {
        glTexEnvf(target, pname, params[0]);
    }
}
SHIM_EXPORT(void, glTexEnvi, (GLenum target, GLenum pname, GLint param))
{ glTexEnvf(target, pname, (GLfloat)param); }
SHIM_EXPORT(void, glTexEnviv, (GLenum target, GLenum pname, const GLint *params))
{
    if (!params) return;
    if (pname == GL_TEXTURE_ENV_MODE) { glTexEnvi(target, pname, params[0]); return; }
    float f[4] = {(float)params[0]/255.0f,(float)params[1]/255.0f,
                  (float)params[2]/255.0f,(float)params[3]/255.0f};
    glTexEnvfv(target, pname, f);
}

/* Fixed-function state getters. Return emulated GL 1.x values (matrix
   stacks, current color/normal, texenv, light parameters) from ShimCtx;
   everything else is forwarded to the real driver. */
SHIM_EXPORT(void, glGetLightfv, (GLenum light, GLenum pname, GLfloat *params))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !params || light < GL_LIGHT0 || light > GL_LIGHT0 + 7) return;
    ShimFFLight *l = &c->ff.light[light - GL_LIGHT0];
    const float *v = pname == GL_AMBIENT ? l->ambient : pname == GL_DIFFUSE ? l->diffuse :
                     pname == GL_SPECULAR ? l->specular : pname == GL_POSITION ? l->position :
                     pname == GL_SPOT_DIRECTION ? l->spotDirection : NULL;
    if (v) memcpy(params, v, (pname == GL_SPOT_DIRECTION ? 3 : 4)*sizeof(float));
}
SHIM_EXPORT(void, glGetMaterialfv, (GLenum face, GLenum pname, GLfloat *params))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !params) return;
    ShimFFMaterial *m = face == GL_BACK ? &c->ff.back_mat : &c->ff.front_mat;
    const float *v = pname == GL_EMISSION ? m->emission : pname == GL_AMBIENT ? m->ambient :
                     pname == GL_DIFFUSE ? m->diffuse : pname == GL_SPECULAR ? m->specular : NULL;
    if (v) memcpy(params, v, 4*sizeof(float));
}
SHIM_EXPORT(void, glGetClipPlane, (GLenum plane, GLdouble *eqn))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !eqn || plane < GL_CLIP_PLANE0 || plane > GL_CLIP_PLANE0 + 7) return;
    const float *v = c->ff.clip_plane[plane - GL_CLIP_PLANE0];
    for (int i = 0; i < 4; i++) eqn[i] = v[i];
}

/* ========================================================================== */
/* MODULE B: double -> float uniforms (TLS scratch buffer)                     */
/* ========================================================================== */
/* glUniform*dv has no GLES equivalent. Arrays are converted in a per-thread  */
/* scratch buffer and forwarded as floats; single values use plain casts.     */

/* Uniform wrappers are defined after G.3: virtual programs need a forwarding
   hook before the generic uniform handlers can be compiled. */
void glUniform1f(GLint, GLfloat);
void glUniform1fv(GLint, GLsizei, const GLfloat *);
void glUniform2fv(GLint, GLsizei, const GLfloat *);
void glUniform3fv(GLint, GLsizei, const GLfloat *);
void glUniform4fv(GLint, GLsizei, const GLfloat *);
void glUniformMatrix2fv(GLint, GLsizei, GLboolean, const GLfloat *);
void glUniformMatrix3fv(GLint, GLsizei, GLboolean, const GLfloat *);
void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat *);

SHIM_EXPORT(void, glUniform1d, (GLint loc, GLdouble x)) { glUniform1f(loc,(float)x); }
SHIM_EXPORT(void, glUniform2d, (GLint loc, GLdouble x, GLdouble y))
{ glUniform2fv(loc,1,(const GLfloat[]){x,y}); }
SHIM_EXPORT(void, glUniform3d, (GLint loc, GLdouble x, GLdouble y, GLdouble z))
{ glUniform3fv(loc,1,(const GLfloat[]){x,y,z}); }
SHIM_EXPORT(void, glUniform4d, (GLint loc, GLdouble x, GLdouble y, GLdouble z, GLdouble w))
{ glUniform4fv(loc,1,(const GLfloat[]){x,y,z,w}); }
SHIM_EXPORT(void, glUniform1dv, (GLint loc, GLsizei n, const GLdouble *v))
{
    if (!v || n <= 0) return;
    GLfloat *f = (GLfloat *)scratch((size_t)n*sizeof(GLfloat));
    if (!f) return;
    for (GLsizei i=0;i<n;i++) f[i]=(float)v[i];
    glUniform1fv(loc,n,f);
}
SHIM_EXPORT(void, glUniform2dv, (GLint loc, GLsizei n, const GLdouble *v))
{
    if (!v || n <= 0) return;
    GLfloat *f = (GLfloat *)scratch((size_t)n*2*sizeof(GLfloat));
    if (!f) return;
    for (GLsizei i=0;i<n*2;i++) f[i]=(float)v[i];
    glUniform2fv(loc,n,f);
}
SHIM_EXPORT(void, glUniform3dv, (GLint loc, GLsizei n, const GLdouble *v))
{
    if (!v || n <= 0) return;
    GLfloat *f = (GLfloat *)scratch((size_t)n*3*sizeof(GLfloat));
    if (!f) return;
    for (GLsizei i=0;i<n*3;i++) f[i]=(float)v[i];
    glUniform3fv(loc,n,f);
}
SHIM_EXPORT(void, glUniform4dv, (GLint loc, GLsizei n, const GLdouble *v))
{
    if (!v || n <= 0) return;
    GLfloat *f = (GLfloat *)scratch((size_t)n*4*sizeof(GLfloat));
    if (!f) return;
    for (GLsizei i=0;i<n*4;i++) f[i]=(float)v[i];
    glUniform4fv(loc,n,f);
}
#define SHIM_UNIF_MAT_D(N) \
SHIM_EXPORT(void, glUniformMatrix##N##dv, (GLint loc, GLsizei n, GLboolean tr, const GLdouble *v)) \
{ if (!v||n<=0) return; size_t k=(size_t)n*(N)*(N); GLfloat *f=(GLfloat *)scratch(k*sizeof(GLfloat)); \
  if (!f) return; \
  for (size_t i=0;i<k;i++) f[i]=(float)v[i]; \
  glUniformMatrix##N##fv(loc,n,tr,f); }
SHIM_UNIF_MAT_D(2)
SHIM_UNIF_MAT_D(3)
SHIM_UNIF_MAT_D(4)
#undef SHIM_UNIF_MAT_D

/* ========================================================================== */
/* MODULE H (minimal): immediate mode and internal FF program                  */
/*                                                                            */
/* glBegin/glEnd vertices are buffered in a private VAO and drawn through an   */
/* internal shader that implements alpha test, texenv and point size. The      */
/* internal program does not use the full ShimFF UBO, so its four state        */
/* vectors are uploaded as plain uniforms instead.                              */
/* ========================================================================== */

static const char *shim_ff_internal_vs =
    "#version 300 es\n"
    /* Names must match shim_ff_attr_name(): shim_imm_flush queries
       glGetAttribLocation by name, not by location. */
    "layout(location=0) in highp vec4 shim_Vertex;\n"
    "layout(location=1) in highp vec4 shim_Color;\n"
    "layout(location=2) in highp vec2 shim_MultiTexCoord0;\n"
    "out highp vec4 v_color;\n"
    "out highp vec2 v_uv;\n"
    "uniform highp vec4 u_point;\n"   /* size, shade, set, 0 */
    "layout(std140) uniform ShimFF {\n"
    " highp mat4 shim_ModelViewProjectionMatrix;\n"
    " highp float shim_pad[896];\n"
    "};\n"
    "void main(){ gl_Position = shim_ModelViewProjectionMatrix * shim_Vertex;\n"
    " gl_PointSize = (u_point.z > 0.5) ? u_point.x : 1.0;\n"
    " v_color = shim_Color; v_uv = shim_MultiTexCoord0; }\n";

static const char *shim_ff_internal_fs =
    "#version 300 es\n"
    "precision highp float;\n"
    "in highp vec4 v_color;\n"
    "in highp vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_has_tex;\n"
    "uniform highp vec4 u_alpha;\n"   /* func, ref, enabled, 0 */
    "uniform highp vec4 u_texenv_mode[8];\n"
    "uniform highp vec4 u_texenv_color[8];\n"
    "out highp vec4 frag;\n"
    "bool shim_alpha_ok(highp float a) {\n"
    " if (u_alpha.z < 0.5) return true;\n"
    " highp float f = u_alpha.x, r = u_alpha.y;\n"
    " if (f < 512.5) return false;\n"
    " if (f < 513.5) return a < r;\n"
    " if (f < 514.5) return a == r;\n"
    " if (f < 515.5) return a <= r;\n"
    " if (f < 516.5) return a > r;\n"
    " if (f < 517.5) return a != r;\n"
    " if (f < 518.5) return a >= r;\n"
    " return true;\n"
    "}\n"
    "void main(){ highp vec4 c = v_color;\n"
    " if (u_has_tex == 1) {\n"
    "  highp vec4 t = texture(u_tex, v_uv);\n"
    "  int m = int(u_texenv_mode[0].x);\n"  /* 8448 MODULATE, 7681 REPLACE, 8449 DECAL, 260 ADD, 3042 BLEND */
    "  if (m == 7681) c = t;\n"
    "  else if (m == 8449) c.rgb = mix(c.rgb, t.rgb, t.a);\n"
    "  else if (m == 260) c.rgb = c.rgb + t.rgb;\n"
    "  else if (m == 3042) c.rgb = mix(c.rgb, u_texenv_color[0].rgb, t.a);\n"
    "  else c = c * t;\n"
    " }\n"
    " if (!shim_alpha_ok(c.a)) discard;\n"
    " frag = c; }\n";

/* Lazily compile the internal immediate-mode program. Attribute names must
   match shim_ff_attr_name(): the wrapper looks locations up by name so it
   works with both this program and translated user programs. */
static GLuint shim_ff_internal_program(ShimCtx *c)
{
    if (!c || !real_glCreateShader || !real_glCreateProgram) return 0;
    if (c->imm_prog) return c->imm_prog;
    GLuint vs = real_glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = real_glCreateShader(GL_FRAGMENT_SHADER);
    const GLchar *v = shim_ff_internal_vs, *f = shim_ff_internal_fs;
    real_glShaderSource(vs, 1, &v, NULL);
    real_glShaderSource(fs, 1, &f, NULL);
    real_glCompileShader(vs);
    real_glCompileShader(fs);
    GLuint p = real_glCreateProgram();
    real_glAttachShader(p, vs);
    real_glAttachShader(p, fs);
    real_glLinkProgram(p);
    if (real_glGetUniformBlockIndex && real_glUniformBlockBinding && c->ff_ready) {
        GLuint bi = real_glGetUniformBlockIndex(p, "ShimFF");
        if (bi != GL_INVALID_INDEX)
            real_glUniformBlockBinding(p, bi, (GLuint)c->ff_ubo_binding);
    }
    GLint ok = 0;
    real_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok && g_debug) {
        char log[1024] = {0};
        real_glGetProgramInfoLog(p, sizeof(log)-1, NULL, log);
        shim_trace("internal FF program: %s\n", log);
    }
    c->imm_prog = ok ? p : 0;
    real_glDeleteShader(vs);
    real_glDeleteShader(fs);
    if (!ok) real_glDeleteProgram(p);
    return c->imm_prog;
}

/* Immediate-path H state: the internal shader does not share the full UBO,
   so alpha/point/texenv uniforms are refreshed explicitly at each flush and
   it receives the four vectors it needs as plain uniforms. */
static void shim_ff_internal_uniforms(ShimCtx *c, GLuint prog)
{
    if (!c || !prog || !real_glGetUniformLocation || !real_glUniform4fv) return;
    float v[4];
    GLint l = real_glGetUniformLocation(prog, "u_point");
    if (l >= 0) {
        v[0] = c->point_size; v[1] = (float)c->shade_model;
        v[2] = c->point_size_set ? 1.0f : 0.0f; v[3] = 0.0f;
        real_glUniform4fv(l, 1, v);
    }
    l = real_glGetUniformLocation(prog, "u_alpha");
    if (l >= 0) {
        v[0] = (float)c->alpha_func; v[1] = c->alpha_ref;
        v[2] = c->alpha_test ? 1.0f : 0.0f; v[3] = 0.0f;
        real_glUniform4fv(l, 1, v);
    }
    l = real_glGetUniformLocation(prog, "u_texenv_mode[0]");
    if (l >= 0) real_glUniform4fv(l, 8, &c->ff.texenv_mode[0][0]);
    l = real_glGetUniformLocation(prog, "u_texenv_color[0]");
    if (l >= 0) real_glUniform4fv(l, 8, &c->ff.texenv_color[0][0]);
    if (real_glUniform1i) {
        l = real_glGetUniformLocation(prog, "u_has_tex");
        if (l >= 0) real_glUniform1i(l, c->tex2d_enabled[0] ? 1 : 0);
        l = real_glGetUniformLocation(prog, "u_tex");
        if (l >= 0) real_glUniform1i(l, 0);
    }
}

static int shim_imm_mode;
static GLint shim_imm_count;
static float *shim_imm_verts, *shim_imm_colors, *shim_imm_uvs;
static size_t shim_imm_cap;

static void shim_imm_grow(void)
{
    if (shim_imm_count + 1 < (GLint)shim_imm_cap) return;
    size_t ncap = shim_imm_cap ? shim_imm_cap * 2 : 256;
    shim_imm_verts = realloc(shim_imm_verts, ncap * 4 * sizeof(float));
    shim_imm_colors = realloc(shim_imm_colors, ncap * 4 * sizeof(float));
    shim_imm_uvs = realloc(shim_imm_uvs, ncap * 2 * sizeof(float));
    shim_imm_cap = ncap;
}

SHIM_EXPORT(void, glBegin, (GLenum mode))
{
    shim_dl_record(shim_get_ctx(), DL_BEGIN, (int)mode, 0, NULL, 0);
    shim_imm_mode = (int)mode; shim_imm_count = 0;
}

static void shim_imm_vertex(float x, float y, float z, float w)
{
    ShimCtx *c = shim_get_ctx();
    if (!c) return;
    float v[4] = {x,y,z,w};
    shim_dl_record(c, DL_VERTEX4, 0, 0, v, 4);
    shim_imm_grow();
    float *p = shim_imm_verts + (size_t)shim_imm_count * 4;
    p[0]=x; p[1]=y; p[2]=z; p[3]=w;
    memcpy(shim_imm_colors + (size_t)shim_imm_count*4, c->current_color, 4*sizeof(float));
    int u = c->client_active_tex & 7;
    shim_imm_uvs[shim_imm_count*2] = c->current_texcoord[u][0];
    shim_imm_uvs[shim_imm_count*2+1] = c->current_texcoord[u][1];
    shim_imm_count++;
}

SHIM_EXPORT(void, glVertex2f, (GLfloat x, GLfloat y)) { shim_imm_vertex(x,y,0,1); }
SHIM_EXPORT(void, glVertex3f, (GLfloat x, GLfloat y, GLfloat z)) { shim_imm_vertex(x,y,z,1); }
SHIM_EXPORT(void, glVertex4f, (GLfloat x, GLfloat y, GLfloat z, GLfloat w)) { shim_imm_vertex(x,y,z,w); }
SHIM_EXPORT(void, glVertex2fv, (const GLfloat *v)) { if (v) shim_imm_vertex(v[0],v[1],0,1); }
SHIM_EXPORT(void, glVertex3fv, (const GLfloat *v)) { if (v) shim_imm_vertex(v[0],v[1],v[2],1); }
SHIM_EXPORT(void, glVertex4fv, (const GLfloat *v)) { if (v) shim_imm_vertex(v[0],v[1],v[2],v[3]); }
SHIM_EXPORT(void, glVertex2d, (GLdouble x, GLdouble y)) { shim_imm_vertex((float)x,(float)y,0,1); }
SHIM_EXPORT(void, glVertex3d, (GLdouble x, GLdouble y, GLdouble z)) { shim_imm_vertex((float)x,(float)y,(float)z,1); }
SHIM_EXPORT(void, glVertex2dv, (const GLdouble *v)) { if (v) shim_imm_vertex(v[0],v[1],0,1); }
SHIM_EXPORT(void, glVertex3dv, (const GLdouble *v)) { if (v) shim_imm_vertex(v[0],v[1],v[2],1); }

SHIM_EXPORT(void, glEnd, (void))
{
    shim_dl_record(shim_get_ctx(), DL_END, 0, 0, NULL, 0);
    if (shim_get_ctx() && shim_get_ctx()->dl_active) {
        shim_imm_count = 0;   /* vertices stay in the list, not on the hot path */
        return;
    }
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    if (!c || !real_glDrawArrays) { shim_imm_mode = 0; return; }
    ff_sync(c);
    GLuint prog = c->cur_prog ? c->cur_prog : shim_ff_internal_program(c);
    if (!prog) { shim_imm_mode = 0; return; }
    GLuint vao = 0, vbo[3] = {0,0,0};
    if (real_glGenVertexArrays) real_glGenVertexArrays(1, &vao);
    if (real_glBindVertexArray) real_glBindVertexArray(vao);
    if (real_glGenBuffers) real_glGenBuffers(3, vbo);
    GLint saved_prog = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &saved_prog);
    real_glUseProgram(prog);
    if (!c->cur_prog) shim_ff_internal_uniforms(c, prog);
    size_t nv = (size_t)shim_imm_count;
    GLint loc_pos = real_glGetAttribLocation ?
                    real_glGetAttribLocation(prog, shim_ff_attr_name(0)) : 0;
    GLint loc_col = real_glGetAttribLocation ?
                    real_glGetAttribLocation(prog, shim_ff_attr_name(1)) : 1;
    GLint loc_uv = real_glGetAttribLocation ?
                   real_glGetAttribLocation(prog, shim_ff_attr_name(3)) : 2;
    if (real_glBindBuffer && real_glBufferData) {
        if (loc_pos >= 0) {
            real_glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
            real_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv*4*sizeof(float)), shim_imm_verts, GL_STREAM_DRAW);
            real_glVertexAttribPointer((GLuint)loc_pos, 4, GL_FLOAT, GL_FALSE, 0, NULL);
            real_glEnableVertexAttribArray((GLuint)loc_pos);
        }
        if (loc_col >= 0) {
            real_glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
            real_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv*4*sizeof(float)), shim_imm_colors, GL_STREAM_DRAW);
            real_glVertexAttribPointer((GLuint)loc_col, 4, GL_FLOAT, GL_FALSE, 0, NULL);
            real_glEnableVertexAttribArray((GLuint)loc_col);
        }
        if (loc_uv >= 0) {
            real_glBindBuffer(GL_ARRAY_BUFFER, vbo[2]);
            real_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv*2*sizeof(float)), shim_imm_uvs, GL_STREAM_DRAW);
            real_glVertexAttribPointer((GLuint)loc_uv, 2, GL_FLOAT, GL_FALSE, 0, NULL);
            real_glEnableVertexAttribArray((GLuint)loc_uv);
        }
    }
    if (shim_imm_mode == GL_POLYGON) shim_imm_mode = GL_TRIANGLE_FAN;
    ShimEmuProg *emu = shim_emu_prog_find(c, prog, 0);
    if (emu && emu->link_ok && (emu->gs || emu->tcs || emu->tes)) {
        /* Immediate mode with GS/TES: the TF pass must use the temporary
           vertices, not the app's arrays. */
        shim_emu_draw_arrays((GLenum)shim_imm_mode, 0, shim_imm_count);
        shim_imm_mode = 0;
        return;
    }
    int quad = shim_imm_mode == GL_QUADS || shim_imm_mode == GL_QUAD_STRIP;
    if (!quad)
        real_glDrawArrays((GLenum)shim_imm_mode, 0, shim_imm_count);
    else if (nv >= 4) {
        size_t prims = shim_imm_mode == GL_QUADS ? nv/4 : (nv>=2 ? (nv-2)/2 : 0);
        size_t idxn = prims*6;
        uint32_t *ix = (uint32_t *)scratch(idxn*4);
        if (ix) {
            for (size_t q=0;q<prims;q++) {
                uint32_t b=(uint32_t)(q*4);
                if (shim_imm_mode == GL_QUADS) {
                    ix[q*6]=b; ix[q*6+1]=b+1; ix[q*6+2]=b+2;
                    ix[q*6+3]=b; ix[q*6+4]=b+2; ix[q*6+5]=b+3;
                } else {
                    ix[q*6]=b; ix[q*6+1]=b+1; ix[q*6+2]=b+2;
                    ix[q*6+3]=b+1; ix[q*6+4]=b+3; ix[q*6+5]=b+2;
                }
            }
            GLuint ibo = 0;
            real_glGenBuffers(1,&ibo);
            real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
            real_glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(idxn*4),ix,GL_STREAM_DRAW);
            real_glDrawElements(GL_TRIANGLES,(GLsizei)idxn,GL_UNSIGNED_INT,NULL);
            real_glDeleteBuffers(1,&ibo);
        }
    }
    real_glUseProgram((GLuint)saved_prog);
    if (real_glBindVertexArray) real_glBindVertexArray(0);
    if (real_glDeleteVertexArrays) real_glDeleteVertexArrays(1,&vao);
    real_glDeleteBuffers(3, vbo);
    shim_imm_mode = 0;
    shim_imm_count = 0;
}

/* ========================================================================== */
/* MODULE G.3 (EXPERIMENTAL / opt-in): GS and tessellation via compute          */
/* ========================================================================== */
/* Covers only the closed subset of G.3.0/G.3.8 (documented below):
   GS: points|lines|triangles -> points|line_strip|triangle_strip with
       literal max_vertices, no adjacency/invocations/streams.
   Tess: uniform level (milestone 1), no warps/fractional levels/edge stitching.
   Everything else is traced and its draw is skipped; the rest of the shim
   (A-G.2) is untouched. Enabled only by NUUBOS_SHIM_GEOM_EMU / _TESS_EMU. */

#define SHIM_VLOC_BASE 0x40000000u
#define SHIM_VBLOCK_BASE 0x50000000u

typedef struct ShimVShader {
    struct ShimVShader *next;
    GLuint id;
    int kind;                  /* 1 GS, 2 TCS, 3 TES, 4 compute real */
    char *src;
    GLuint cs;
    int compiled, unsupported;
    char *log;
    int in_prim, out_prim, max_vertices, max_idx, patch_vertices;
    int point_mode, cw, spacing, psize;
    char out[16][64], out_type[16][16], in[16][64], in_type[16][16];
    int nout, nin;
} ShimVShader;

typedef struct ShimShaderSrc {
    struct ShimShaderSrc *next;
    GLuint id;
    char *src;
} ShimShaderSrc;

typedef struct ShimEmuUniform {
    struct ShimEmuUniform *next;
    char name[160];
    int slot;
    GLint l[4];                /* P1, P2 (o P2a), P3, P2b */
} ShimEmuUniform;

static ShimVShader *g_vshaders;
static ShimShaderSrc *g_shader_src;
static int g_emu_count, g_emu_failed;
static int shim_patch_vertices = 3;

static ShimVShader *shim_vshader_find(GLuint id)
{
    for (ShimVShader *v = g_vshaders; v; v = v->next)
        if (v->id == id) return v;
    return NULL;
}

static int shim_emu_virtual_kind(GLuint shader)
{
    ShimVShader *v = shim_vshader_find(shader);
    return v ? v->kind : 0;
}

static const char *shim_shader_src(GLuint id)
{
    for (ShimShaderSrc *s = g_shader_src; s; s = s->next)
        if (s->id == id) return s->src;
    return NULL;
}

static ShimEmuProg *shim_emu_prog_find(ShimCtx *c, GLuint prog, int create)
{
    if (!c) return NULL;
    ShimEmuProg **p = &c->emu_progs;
    while (*p && (*p)->prog != prog) p = &(*p)->next;
    if (!*p && create) {
        *p = calloc(1, sizeof(**p));
        if (*p) (*p)->prog = prog;
    }
    return *p;
}

static void shim_emu_free_prog(ShimEmuProg *ep)
{
    if (!ep) return;
    ShimEmuUniform *u = ep->uniforms;
    while (u) { ShimEmuUniform *n = u->next; free(u); u = n; }
    free(ep->log);
    free(ep);
}

/* -------------------------------------------------------------------------- */
/* Phase 0: trivial compute on the real driver                                */
/*                                                                            */
/* Before translating any geometry shader, prove that the driver can compile, */
/* link and dispatch a compute shader at all. A failure here disables both    */
/* emulation flags; subsequent draws fall back to the A-G.2 shim path.        */
/* -------------------------------------------------------------------------- */

void shim_probe_emu(ShimCtx *c)
{
    if (!c || !real_glCreateShader || !real_glShaderSource || !real_glCompileShader ||
        !real_glCreateProgram || !real_glLinkProgram || !real_glDispatchCompute) {
        if (c) c->features.geom_emu = c->features.tess_emu = 0;
        g_emu_failed = 1;
        shim_trace("Phase 0: missing GLES 3.1 pointers; emulation disabled\n");
        return;
    }
    /* Drain any error left over from EGL config probing before testing the
       probe shader: otherwise an old EGL error could be mistaken for a
       compute failure. Only the first 16 errors are consumed. */
    for (int i = 0; i < 16 && real_glGetError() != GL_NO_ERROR; i++) {}
    static const char *probe =
        "#version 310 es\nlayout(local_size_x = 64) in;\nvoid main(){}\n";
    GLuint s = real_glCreateShader(GL_COMPUTE_SHADER);
    const GLchar *p = probe;
    real_glShaderSource(s, 1, &p, NULL);
    real_glCompileShader(s);
    GLint ok = 0;
    real_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    GLuint pr = ok ? real_glCreateProgram() : 0;
    if (pr) {
        real_glAttachShader(pr, s);
        real_glLinkProgram(pr);
        real_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        if (ok) {
            real_glUseProgram(pr);
            real_glDispatchCompute(1, 1, 1);
            GLenum err = real_glGetError();
            if (err != GL_NO_ERROR) {
                shim_trace("Phase 0: compute dispatch error 0x%x\n", err);
                ok = 0;
            }
        } else {
            char log[256] = {0};
            real_glGetProgramInfoLog(pr, sizeof(log), NULL, log);
            shim_trace("Phase 0: compute link failed: %s\n", log);
        }
        real_glDeleteProgram(pr);
    }
    if (s) real_glDeleteShader(s);
    if (!ok) {
        c->features.geom_emu = c->features.tess_emu = 0;
        g_emu_failed = 1;
    }
    shim_trace("Phase 0: compute %s; ES %d.%d, inv=%d, ssbo=%d MiB, tf=%d, "
               "geom=%d tess=%d\n", ok ? "OK" : "FAIL",
               c->features.es_major, c->features.es_minor,
               c->features.max_compute_invocations,
               c->features.ssbo_max_mb, c->features.has_tf,
               c->features.geom_emu, c->features.tess_emu);
}

static int shim_emu_ok(const ShimCtx *c, int kind)
{
    if (!c || !desktop_mode() || g_emu_failed || g_emu_count >= 4096) return 0;
    return kind == 1 ? c->features.geom_emu :
           kind == 2 ? c->features.tess_emu : 0;
}

/* -------------------------------------------------------------------------- */
/* Virtual shader capture (G.3.1)                                             */
/*                                                                            */
/* GLSL geometry/tessellation shaders do not exist in GLES 310, so the app's   */
/* source is kept verbatim and parsed into a ShimVShader instead of being     */
/* submitted to the driver. glShaderSource/glShaderType record the objects;   */
/* their real shader object stays empty until a G.3 program is built.        */
/* -------------------------------------------------------------------------- */

void shim_emu_on_shader_type(GLuint shader, GLint type)
{
    if (type != GL_GEOMETRY_SHADER && type != GL_TESS_CONTROL_SHADER &&
        type != GL_TESS_EVALUATION_SHADER && type != GL_COMPUTE_SHADER)
        return;
    ShimCtx *c = tls_ctx_ptr;
    if (!c || !desktop_mode()) return;
    int kind = type == GL_GEOMETRY_SHADER ? 1 :
               type == GL_TESS_CONTROL_SHADER ? 2 :
               type == GL_TESS_EVALUATION_SHADER ? 3 : 4;
    if (kind != 4 && !shim_emu_ok(c, kind)) return;
    pthread_mutex_lock(&g_emu_mutex);
    if (!shim_vshader_find(shader)) {
        ShimVShader *v = calloc(1, sizeof(*v));
        if (v) {
            v->id = shader;
            v->kind = kind;
            v->next = g_vshaders;
            g_vshaders = v;
            if (kind != 4) g_emu_count++;
        }
    }
    pthread_mutex_unlock(&g_emu_mutex);
}

void shim_emu_on_shader_source(GLuint shader, const char *src)
{
    ShimCtx *c = tls_ctx_ptr;
    /* Do not rely on g_emu_count: the VS may compile before the GS is created. */
    if (!c || !src || !desktop_mode()) return;
    ShimVShader *v = shim_vshader_find(shader);
    if (v) {
        pthread_mutex_lock(&g_emu_mutex);
        free(v->src);
        v->src = strdup(src);
        v->compiled = 0;
        free(v->log);
        v->log = NULL;
        pthread_mutex_unlock(&g_emu_mutex);
        return;
    }
    pthread_mutex_lock(&g_emu_mutex);
    if (!shim_shader_src(shader)) {
        ShimShaderSrc *s = calloc(1, sizeof(*s));
        if (s) {
            s->id = shader;
            s->src = strdup(src);
            s->next = g_shader_src;
            g_shader_src = s;
        }
    }
    pthread_mutex_unlock(&g_emu_mutex);
}

/* -------------------------------------------------------------------------- */
/* GS -> compute translation (G.3.3)                                          */
/* -------------------------------------------------------------------------- */

static int shim_type_size(const char *t)
{
    if (!t) return 4;
    if (!strncmp(t, "float", 5)) return 1;
    if (!strncmp(t, "vec2", 4)) return 2;
    if (!strncmp(t, "vec3", 4)) return 3;
    if (!strncmp(t, "vec4", 4)) return 4;
    if (!strncmp(t, "int", 3) || !strncmp(t, "uint", 4)) return 1;
    if (!strncmp(t, "ivec", 4) || !strncmp(t, "uvec", 4)) return t[4] - '0';
    if (!strncmp(t, "mat", 3)) return (t[3] - '0') * (t[3] - '0');
    return 4;
}

static const char *shim_c_type(const char *t)
{
    return (t && !strncmp(t, "int", 3)) ? "int" :
           (t && !strncmp(t, "uint", 4)) ? "uint" : "float";
}

struct GsTokCtx {
    ShimVShader *v;
    int stride;
    int in_size[16];
};

static void shim_gs_line(ShimVShader *v, struct GsTokCtx *g,
                         const char *q, StrBuf *body)
{

    /* The generated compute source already carries its own #version and
       #extension lines; copying the app's directives here would either
       duplicate them or place an illegal line before #version. */
    if (!strncmp(q, "#version", 8) || !strncmp(q, "#extension", 10))
        return;
    if (!strncmp(q, "layout", 6)) {
        const char *in = strstr(q, " in;");
        const char *out = strstr(q, " out;");
        if (in) {
            if (strstr(q, "points")) v->in_prim = GL_POINTS;
            else if (strstr(q, "lines")) v->in_prim = GL_LINES;
            else if (strstr(q, "triangles")) v->in_prim = GL_TRIANGLES;
            if (strstr(q, "adjacency") || strstr(q, "invocations"))
                v->unsupported = 1;
        } else if (out) {
            if (strstr(q, "points")) v->out_prim = GL_POINTS;
            else if (strstr(q, "line_strip")) v->out_prim = GL_LINES;
            else if (strstr(q, "triangle_strip")) v->out_prim = GL_TRIANGLES;
            const char *mx = strstr(q, "max_vertices");
            if (mx && (mx = strchr(mx, '='))) v->max_vertices = atoi(mx + 1);
            if (strstr(q, "stream") || (v->max_vertices < 1 || v->max_vertices > 64))
                v->unsupported = 1;
        }
        return;
    }
    if (!strncmp(q, "in ", 3) && strchr(q, '[') && v->nin < 16) {
        char ty[32] = {0}, nm[64] = {0};
        if (sscanf(q, " in %31s %63[^[]", ty, nm) == 2) {
            char *e = nm;
            while (*e && *e != ' ' && *e != ';') e++;
            *e = 0;
            snprintf(v->in[v->nin], sizeof(v->in[0]), "%s", nm);
            snprintf(v->in_type[v->nin], sizeof(v->in_type[0]), "%s", ty);
            g->in_size[v->nin] = shim_type_size(ty);
            v->nin++;
        }
        return;
    }
    if (!strncmp(q, "out ", 4) && v->nout < 16 && !strchr(q, '[') &&
        !strchr(q, '{')) {
        char ty[32] = {0}, nm[64] = {0};
        if (sscanf(q, " out %31s %63[^;]", ty, nm) == 2) {
            char *e = nm;
            while (*e && *e != ' ' && *e != '[') e++;
            *e = 0;
            if (!strcmp(nm, "gl_PointSize")) v->psize = 1;
            else {
                snprintf(v->out_type[v->nout], sizeof(v->out_type[0]), "%s", ty);
                snprintf(v->out[v->nout], sizeof(v->out[0]), "%s", nm);
                v->nout++;
            }
        }
        return;
    }
    /* The rest of the body is kept for the lexical translation. */
    const char *p = q;
    while (*p && *p != '\n') p++;
    sb_addn(body, q, (size_t)(p - q));
    if (*p) sb_addn(body, p, 1);
}

static void shim_gs_token(const char *tok, size_t len, const char *rest,
                          TokCtx *tc, void *user)
{
    struct GsTokCtx *g = user;
    ShimVShader *v = g->v;
    int idx = 0;
    const char *a = NULL;
    if (shim_tok_is(tok, len, "gl_in") &&
        (a = shim_bracket_int(rest, &idx)) != NULL && idx >= 0 && idx < 64) {
        const char *p = shim_skip_ws(a);
        if (*p == '.') {
            p++;
            const char *e = p;
            while (shim_ident_char((unsigned char)*e)) e++;
            size_t fl = (size_t)(e - p);
            if (fl == 11 && !memcmp(p, "gl_Position", 11)) {
                shim_setrepf(tc, "shim_in_pos(%d)", idx);
                tc->skip = (size_t)(e - rest);
            } else if (fl == 11 && !memcmp(p, "gl_PointSize", 11)) {
                shim_setrepf(tc, "shim_in_psize(%d)", idx);
                tc->skip = (size_t)(e - rest);
            } else {
                for (int i = 0; i < v->nin; i++)
                    if (strlen(v->in[i]) == fl && !memcmp(v->in[i], p, fl)) {
                        shim_setrepf(tc, "shim_in_%s(%d)", v->in[i], idx);
                        tc->skip = (size_t)(e - rest);
                        break;
                    }
            }
        } else {
            shim_setrepf(tc, "shim_in_pos(%d)", idx);
            tc->skip = (size_t)(a - rest);
        }
        return;
    }
    /* Replace only the identifier: the original parentheses remain. */
    if (shim_tok_is(tok, len, "EmitVertex")) { shim_setrep(tc, "shim_emit"); return; }
    if (shim_tok_is(tok, len, "EndPrimitive")) { shim_setrep(tc, "shim_endprim"); return; }
    if (shim_tok_is(tok, len, "gl_Position")) { shim_setrep(tc, "shim_Position"); return; }
    if (shim_tok_is(tok, len, "gl_PointSize")) { shim_setrep(tc, "shim_PointSize"); return; }
    if (shim_tok_is(tok, len, "gl_PrimitiveID") || shim_tok_is(tok, len, "gl_PrimitiveIDIn"))
    { shim_setrep(tc, "int(shim_first + gl_GlobalInvocationID.x)"); return; }
    if (shim_tok_is(tok, len, "gl_Layer") || shim_tok_is(tok, len, "gl_ViewportIndex"))
    { shim_setrep(tc, "shim_ignored"); return; }
    if (shim_tok_is(tok, len, "texture") && shim_skip_ws(rest)[0] == '(')
    { shim_setrep(tc, "textureLod"); return; }
    if (shim_tok_is(tok, len, "main") && shim_skip_ws(rest)[0] == '(')
    { shim_setrep(tc, "shim_gs_main"); return; }
}

static int shim_translate_gs(ShimVShader *v, StrBuf *out)
{
    if (!v || v->kind != 1) return 0;
    for (int i = 0; i < v->nin; i++)
        if (v->in_type[i][0] == 'm' || shim_type_size(v->in_type[i]) > 4) {
            shim_trace("G.3: array/matrix varyings unsupported; GS skipped\n");
            return 0;
        }
    StrBuf body = {0};
    struct GsTokCtx gt = {0};
    gt.v = v;
    const char *p = v->src ? v->src : "";
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        /* NUL-terminated copy: strchr/strstr must not read the following
           lines (they previously saw the '{' of "void main(){" and dropped
           declarations such as "out vec2 vuv;"). */
        size_t ln = (size_t)(p - line);
        char lbuf[2048];
        if (ln >= sizeof(lbuf)) ln = sizeof(lbuf) - 1;
        memcpy(lbuf, line, ln);
        lbuf[ln] = 0;
        shim_gs_line(v, &gt, shim_skip_ws(lbuf), &body);
        if (*p) p++;
    }
    if (v->unsupported || !v->max_vertices || v->nin >= 16) {
        shim_trace("G.3: GS with unsupported case; skipped\n");
        free(body.data);
        return 0;
    }
    gt.stride = 4 + (v->psize ? 1 : 0);
    for (int i = 0; i < v->nin; i++) gt.stride += gt.in_size[i];
    if (!v->in_prim) v->in_prim = GL_TRIANGLES;
    if (!v->out_prim) v->out_prim = GL_TRIANGLES;
    int MAXV = v->max_vertices;
    int maxIdx = v->out_prim == GL_POINTS ? MAXV :
                 v->out_prim == GL_LINES ? (MAXV - 1) * 2 :
                 (MAXV > 2 ? (MAXV - 2) * 3 : 3);
    if (maxIdx < 1) maxIdx = 1;
    v->max_idx = maxIdx;
    int ostride = 4 + (v->psize ? 1 : 0);
    for (int i = 0; i < v->nout; i++) ostride += shim_type_size(v->out_type[i]);

    StrBuf trans = {0};
    shim_lex_process(body.data ? body.data : "", &trans, shim_gs_token, &gt);
    sb_add(out, "#version 310 es\nlayout(local_size_x = 64) in;\n"
                "precision highp float;\nprecision highp int;\n"
                "layout(std430, binding = 0) readonly buffer ShimIn { float tf[]; };\n"
                "layout(std430, binding = 1) buffer ShimOutV { float ov[]; };\n"
                "layout(std430, binding = 2) buffer ShimOutI { uint idx[]; };\n"
                "uniform uint shim_nprims; uniform uint shim_first;\n"
                "uniform uint shim_vin;\n"
                "uint shim_first_in; uint shim_n; uint shim_k; int shim_ignored;\n"
                "vec4 shim_Position; float shim_PointSize;\n");
    for (int i = 0; i < v->nout; i++)
        sb_addf(out, "%s %s;\n", v->out_type[i], v->out[i]);
    sb_addf(out, "#define MAXV %d\n#define OSTRIDE %d\n", MAXV, ostride);
    sb_addf(out, "vec4 shim_in_pos(int i){ uint o=(shim_first_in+uint(i)) * %du; "
                 "return vec4(tf[o],tf[o+1u],tf[o+2u],tf[o+3u]); }\n", gt.stride);
    if (v->psize)
        sb_addf(out, "float shim_in_psize(int i){ uint o=(shim_first_in+uint(i)) * %du + 4u; return tf[o]; }\n", gt.stride);
    int off = 4 + (v->psize ? 1 : 0);
    for (int i = 0; i < v->nin; i++) {
        int sz = gt.in_size[i];
        const char *ty = v->in_type[i];
        sb_addf(out, "%s shim_in_%s(int i){ uint o=(shim_first_in+uint(i)) * %du + %du; return %s(",
                ty, v->in[i], gt.stride, off, ty);
        for (int k = 0; k < sz; k++)
            sb_addf(out, "%stf[o%s]", k ? "," : "", k ? (const char *)(k == 1 ? "+1u" :
                         k == 2 ? "+2u" : "+3u") : "");
        sb_add(out, "); }\n");
        off += sz;
    }
    sb_add(out, "void shim_emit(){\n if (shim_n >= uint(MAXV)) return;\n"
                " uint p=gl_GlobalInvocationID.x; uint slot=(p*uint(MAXV)+shim_n)*uint(OSTRIDE);\n"
                " ov[slot]=shim_Position.x; ov[slot+1u]=shim_Position.y;\n"
                " ov[slot+2u]=shim_Position.z; ov[slot+3u]=shim_Position.w;\n");
    int ooff = 4;
    if (v->psize) { sb_add(out, " ov[slot+4u]=shim_PointSize;\n"); ooff = 5; }
    for (int i = 0; i < v->nout; i++) {
        int sz = shim_type_size(v->out_type[i]);
        const char *ty = v->out_type[i];
        sb_addf(out, " { uint o=slot+%du; %s v=%s(%s); ov[o]=%s;\n",
                 ooff, sz == 1 ? shim_c_type(ty) : ty, ty, v->out[i],
                 sz == 1 ? "v" : "v.x");
        if (sz > 1) sb_add(out, " ov[o+1u]=v.y;\n");
        if (sz > 2) sb_add(out, " ov[o+2u]=v.z;\n");
        if (sz > 3) sb_add(out, " ov[o+3u]=v.w;\n");
        sb_add(out, " }\n");
        ooff += sz;
    }
    sb_addf(out, " uint base=p*%du;\n", maxIdx);
    if (v->out_prim == GL_POINTS)
        sb_add(out, " idx[base+shim_n]=p*uint(MAXV)+shim_n;\n");
    else if (v->out_prim == GL_LINES)
        sb_add(out, " if (shim_k>=1u){ idx[base+2u*(shim_k-1u)]=p*uint(MAXV)+shim_n-1u;"
                     " idx[base+2u*(shim_k-1u)+1u]=p*uint(MAXV)+shim_n; }\n");
    else
        sb_add(out, " if (shim_k>=2u){ uint o=base+3u*(shim_k-2u); uint s=p*uint(MAXV)+shim_n;"
                     " if ((shim_k&1u)==0u){ idx[o]=s-2u; idx[o+1u]=s-1u; idx[o+2u]=s; }"
                     " else { idx[o]=s-1u; idx[o+1u]=s-2u; idx[o+2u]=s; } }\n");
    sb_add(out, " shim_n++; shim_k++; }\nvoid shim_endprim(){ shim_k=0u; }\n");
    sb_addf(out, "void shim_fill_indices(){ uint p=gl_GlobalInvocationID.x; uint b=p*%du;"
                 " uint v0=p*uint(MAXV); for(uint i=0u;i<%du;i++) idx[b+i]=v0; }\n",
            maxIdx, maxIdx);
    sb_add(out, trans.data ? trans.data : "");
    sb_addf(out, "void main(){ if (gl_GlobalInvocationID.x>=shim_nprims) return;"
                 " shim_first_in=(shim_first+gl_GlobalInvocationID.x)*shim_vin;"
                 " shim_n=0u; shim_k=0u;"
                 " shim_fill_indices(); shim_gs_main(); }\n");
    free(body.data);
    free(trans.data);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Tessellation: milestone 1, uniform levels                                   */
/*                                                                            */
/* Only a literal gl_TessLevelOuter/Inner assignment is understood: the level */
/* is baked into a uniform used by the compute pass. Fractional levels, warp  */
/* modes and edge stitching are deliberately out of scope (spec G.3.8).      */
/* -------------------------------------------------------------------------- */

static void shim_tess_parse(ShimVShader *v)
{
    const char *p = v->src ? v->src : "";
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        size_t ln = (size_t)(p - line);
        char lbuf[2048];
        if (ln >= sizeof(lbuf)) ln = sizeof(lbuf) - 1;
        memcpy(lbuf, line, ln);
        lbuf[ln] = 0;
        const char *q = shim_skip_ws(lbuf);
        if (!strncmp(q, "layout", 6)) {
            if (v->kind == 2 && strstr(q, "vertices")) {
                const char *e = strstr(q, "vertices");
                e = e ? strchr(e, '=') : NULL;
                if (e) v->patch_vertices = atoi(e + 1);
            }
            if (v->kind == 3) {
                if (strstr(q, "quads")) v->in_prim = 4;
                else if (strstr(q, "isolines")) v->in_prim = 5;
                else v->in_prim = 3;
                if (strstr(q, "fractional_even") || strstr(q, "fractional_odd"))
                    v->unsupported = 1;
                v->point_mode = strstr(q, "point_mode") != NULL;
                v->cw = strstr(q, "cw") && !strstr(q, "ccw");
            }
        }
        if (*p) p++;
    }
    if (v->patch_vertices < 1 || v->patch_vertices > 32) v->unsupported = 1;
}

static int shim_translate_tess(ShimCtx *c, ShimVShader *v, StrBuf *out)
{
    if (!v || v->kind < 2 || v->unsupported) return 0;
    shim_tess_parse(v);
    if (v->unsupported) {
        shim_trace("G.3: tess with fractional/unsupported cases; skipped\n");
        return 0;
    }
    int N = v->kind == 2 ? v->patch_vertices : 64;
    int L = (int)c->features.tess_max;
    if (L < 1) L = 1;
    if (L > 16) L = 16;
    int dom = v->kind == 3 ? v->in_prim : 3;
    int maxv = dom == 4 ? (L + 1) * (L + 1) :
               dom == 5 ? (L + 1) * L : (L + 1) * (L + 2) / 2;
    int maxi = dom == 4 ? L * L * 6 : dom == 5 ? L * L * 2 : L * L * 3;
    if (v->point_mode) maxi = maxv;
    v->max_vertices = maxv;
    v->max_idx = maxi;
    if (v->kind == 2) {
        sb_addf(out, "#version 310 es\nlayout(local_size_x = %d) in;\n"
                     "precision highp float;\n"
                     "layout(std430, binding = 0) readonly buffer ShimIn { float tf[]; };\n"
                     "layout(std430, binding = 1) buffer ShimOutV { float ov[]; };\n"
                     "layout(std430, binding = 3) buffer ShimOutL { float lv[]; };\n"
                     "uniform uint shim_pv;\n"
                     "void main(){ uint patch=gl_WorkGroupID.x; uint i=gl_LocalInvocationID.x;"
                     " if (i>=shim_pv) return; uint o=(patch*shim_pv+i)*4u;"
                     " vec4 p=vec4(tf[o],tf[o+1u],tf[o+2u],tf[o+3u]);"
                     " ov[o]=p.x; ov[o+1u]=p.y; ov[o+2u]=p.z; ov[o+3u]=p.w;"
                     " if (i==0u){ for(int k=0;k<6;k++) lv[patch*6u+uint(k)]=1.0;"
                     " lv[patch*6u]=lv[patch*6u+1u]=lv[patch*6u+2u]=float(%d); } }\n",
                N, L);
    } else {
        sb_addf(out, "#version 310 es\nlayout(local_size_x = 64) in;\n"
                     "precision highp float;\n"
                     "layout(std430, binding = 1) readonly buffer ShimInV { float iv[]; };\n"
                     "layout(std430, binding = 3) readonly buffer ShimInL { float lv[]; };\n"
                     "layout(std430, binding = 1) writeonly buffer ShimOutV { float ov[]; };\n"
                     "layout(std430, binding = 2) writeonly buffer ShimOutI { uint idx[]; };\n"
                     "#define MAXV %d\n#define MAXI %d\n#define L %d\n#define DOM %d\n"
                     "uint lev(uint p, int k){ float v=lv[p*6u+uint(k)];"
                     " int n=int(ceil(v)); if(n<1)n=1; if(n>L)n=L; return uint(n); }\n"
                     "void shim_emit(uint patch,uint v,vec4 p){ uint o=(patch*uint(MAXV)+v)*4u;"
                     " ov[o]=p.x; ov[o+1u]=p.y; ov[o+2u]=p.z; ov[o+3u]=p.w; }\n",
                     maxv, maxi, L, dom);
        sb_add(out, "void main(){ uint patch=gl_WorkGroupID.x;"
                     " uint nu=lev(patch,0), nv=lev(patch,1);"
                     " for(uint t=gl_LocalInvocationID.x;t<uint(MAXV);t+=64u){\n"
                     "  float u=float(t%nu)/float(nu), w=float(t/nu)/float(nv); vec4 p;"
                     "  if (DOM==4) p=vec4(u,w,0.0,1.0);"
                     "  else if (DOM==5) p=vec4(u,float(t%nu)/float(nu),0.0,1.0);"
                     "  else { float v=1.0-u-w; p=vec4(u,w,v,1.0); }"
                     "  shim_emit(patch,t,p); }\n"
                     " for(uint t=gl_LocalInvocationID.x;t<uint(MAXI);t+=64u){\n"
                     "  uint u=t%nu, v=t/nu, o=patch*uint(MAXI)+t;"
                     "  if (DOM==4 || DOM==3) {"
                     "   uint a=patch*uint(MAXV)+v*(nu+1u)+u;"
                     "   uint b=patch*uint(MAXV)+(v+1u)*(nu+1u)+u;"
                     "   if ((u&1u)==0u){ idx[o]=a; idx[o+1u]=a+1u; }"
                     "   else { idx[o]=a; idx[o+1u]=b; } idx[o+2u]=b+1u; }"
                     "  else { idx[o]=patch*uint(MAXV)+v*nu+u;"
                     "         idx[o+1u]=patch*uint(MAXV)+v*nu+u+1u; } } }\n");
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Linking of the four virtual programs (G.3.2)                               */
/*                                                                            */
/* A virtual GS/TCS/TES program becomes: a compute stage for expansion, a VS   */
/* pass that writes gl_Position plus attributes to transform feedback, and a   */
/* final VS/FS pair for rasterisation. All four are linked lazily on the first */
/* glLinkProgram of the containing program.                                    */
/* -------------------------------------------------------------------------- */

static void shim_emu_log(ShimEmuProg *ep, const char *stage, GLuint obj)
{
    if (!real_glGetProgramInfoLog || !ep) return;
    char buf[512] = {0};
    real_glGetProgramInfoLog(obj, sizeof(buf) - 1, NULL, buf);
    if (!*buf) return;
    size_t old = ep->log ? strlen(ep->log) : 0;
    size_t add = strlen(stage) + strlen(buf) + 4;
    char *p = realloc(ep->log, old + add + 1);
    if (!p) return;
    ep->log = p;
    snprintf(ep->log + old, add + 1, "\n%s: %s", stage, buf);
}

static void shim_bind_ff_program(GLuint p, ShimCtx *c)
{
    if (!c || !c->ff_ready || !real_glGetUniformBlockIndex ||
        !real_glUniformBlockBinding || !p) return;
    GLuint bi = real_glGetUniformBlockIndex(p, "ShimFF");
    if (bi != GL_INVALID_INDEX)
        real_glUniformBlockBinding(p, bi, (GLuint)c->ff_ubo_binding);
}

static void shim_vs_tf_names(GLuint vs, char names[16][64], int *pn)
{
    int n = 0;
    if (n < 16) strcpy(names[n++], "gl_Position");
    const char *src = shim_shader_src(vs);
    const char *p = src ? src : "";
    while (*p && n < 16) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        size_t ln = (size_t)(p - line);
        char lbuf[1024];
        if (ln >= sizeof(lbuf)) ln = sizeof(lbuf) - 1;
        memcpy(lbuf, line, ln);
        lbuf[ln] = 0;
        const char *q = shim_skip_ws(lbuf);
        if (!strncmp(q, "out ", 4) && !strchr(q, '[') && !strchr(q, '{')) {
            char ty[32] = {0}, nm[64] = {0};
            if (sscanf(q, " out %31s %63[^;]", ty, nm) == 2) {
                char *e = nm;
                while (*e && *e != ' ' && *e != '[') e++;
                *e = 0;
                if (*nm && strcmp(nm, "gl_Position"))
                    snprintf(names[n++], sizeof(names[0]), "%s", nm);
            }
        }
        if (*p) p++;
    }
    *pn = n;
}

static const char *shim_passthrough_vs(const char *out_src, int *psize)
{
    static char buf[4096];
    size_t w = 0;
    w += (size_t)snprintf(buf + w, sizeof(buf) - w,
                          "#version 300 es\nlayout(location=0) in highp vec4 shim_a_pos;\n");
    *psize = 0;
    char names[16][64];
    const char *p = out_src ? out_src : "";
    while (*p && w < sizeof(buf) - 256 && *psize < 16) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        size_t ln = (size_t)(p - line);
        char lbuf[1024];
        if (ln >= sizeof(lbuf)) ln = sizeof(lbuf) - 1;
        memcpy(lbuf, line, ln);
        lbuf[ln] = 0;
        const char *q = shim_skip_ws(lbuf);
        if (!strncmp(q, "out ", 4) && !strchr(q, '[')) {
            char ty[32] = {0}, nm[64] = {0};
            if (sscanf(q, " out %31s %63[^;]", ty, nm) == 2) {
                char *e = nm;
                while (*e && *e != ' ' && *e != '[') e++;
                *e = 0;
                int flat = ty[0] == 'i' || ty[0] == 'u';
                snprintf(names[*psize], sizeof(names[0]), "%s", nm);
                w += (size_t)snprintf(buf + w, sizeof(buf) - w,
                        "layout(location=%d) in highp %s shim_a_%s; %s out %s %s;\n",
                        *psize + 1, ty, nm, flat ? "flat" : "", ty, nm);
                (*psize)++;
            }
        }
        if (*p) p++;
    }
    w += (size_t)snprintf(buf + w, sizeof(buf) - w,
                          "void main(){ gl_Position=shim_a_pos;\n");
    for (int i = 0; i < *psize && w < sizeof(buf) - 128; i++)
        w += (size_t)snprintf(buf + w, sizeof(buf) - w, " %s=shim_a_%s;\n",
                              names[i], names[i]);
    w += (size_t)snprintf(buf + w, sizeof(buf) - w, " }\n");
    return buf;
}

/* VS used in the TF pass: adds an output varying carrying gl_Position,
   because GLES 3.1 forbids capturing gl_Position directly. */
static const char *shim_tf_vs_source(const char *src)
{
    static char buf[16384];
    if (!src) return NULL;
    /* #version/#extension directives must precede any declaration;
       the varying is inserted after all of them. */
    size_t head = 0;
    const char *p = src;
    while (*p) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '#') break;
        head = (size_t)(e - src + 1);
        p = e + 1;
    }
    const char *last = strrchr(src, '}');
    size_t w = 0;
    if (head >= sizeof(buf) - 256) return NULL;
    memcpy(buf, src, head);
    w = head;
    w += (size_t)snprintf(buf + w, sizeof(buf) - w,
                          "out highp vec4 shim_tf_pos;\n");
    if (last && last > src + head) {
        size_t mid = (size_t)(last - (src + head));
        if (w + mid + 96 >= sizeof(buf)) return NULL;
        memcpy(buf + w, src + head, mid);
        w += mid;
    }
    w += (size_t)snprintf(buf + w, sizeof(buf) - w,
                          "shim_tf_pos=gl_Position;\n}\n");
    return buf;
}

static int shim_emu_build(ShimCtx *c, ShimEmuProg *ep)
{
    if (!ep || !real_glLinkProgram || !real_glCreateProgram ||
        !real_glCreateShader || !real_glShaderSource || !real_glCompileShader)
        return 0;
    ep->link_ok = 0;
    ShimVShader *gs = shim_vshader_find(ep->gs);
    ShimVShader *tcs = shim_vshader_find(ep->tcs);
    ShimVShader *tes = shim_vshader_find(ep->tes);
    if (!gs && !tcs && !tes) return 0;
    if (!ep->vs || !ep->fs) {
        pthread_mutex_lock(&g_prog_mutex);
        for (struct ShimProgAttach *a = g_prog_attach; a; a = a->next)
            if (a->prog == ep->prog) {
                if (!ep->vs && a->vs) { ep->vs = a->vs; ep->vs_attached = 1; }
                if (!ep->fs && a->fs) { ep->fs = a->fs; ep->fs_attached = 1; }
                break;
            }
        pthread_mutex_unlock(&g_prog_mutex);
    }
    free(ep->log);
    ep->log = NULL;
    if (ep->fs && ep->fs_attached && real_glDetachShader)
        real_glDetachShader(ep->prog, ep->fs);
    ep->fs_attached = 0;
    if (ep->vs && !ep->vs_attached && real_glAttachShader)
        real_glAttachShader(ep->prog, ep->vs);
    ep->vs_attached = 1;
    static const char *empty_fs =
        "#version 300 es\nprecision mediump float;void main(){}\n";
    GLuint fs0 = real_glCreateShader(GL_FRAGMENT_SHADER);
    if (fs0) {
        const GLchar *s0 = empty_fs;
        real_glShaderSource(fs0, 1, &s0, NULL);
        real_glCompileShader(fs0);
        real_glAttachShader(ep->prog, fs0);
    }
    GLuint tfvs = 0;
    if (ep->vs) {
        const char *tf_src = shim_tf_vs_source(shim_shader_src(ep->vs));
    if (tf_src && real_glCreateShader && real_glShaderSource &&
            real_glCompileShader && real_glAttachShader) {
            tfvs = real_glCreateShader(GL_VERTEX_SHADER);
            if (tfvs) {
                const GLchar *vsrc = tf_src;
                real_glShaderSource(tfvs, 1, &vsrc, NULL);
                real_glCompileShader(tfvs);
                if (ep->vs_attached && real_glDetachShader)
                    real_glDetachShader(ep->prog, ep->vs);
                real_glAttachShader(ep->prog, tfvs);
                ep->vs_attached = 0;
            }
        }
    }
    if (real_glTransformFeedbackVaryings) {
        char names[16][64];
        int n = 0;
        shim_vs_tf_names(ep->vs, names, &n);
        for (int i = 0; i < n; i++)
            if (!strcmp(names[i], "gl_Position"))
                snprintf(names[i], sizeof(names[0]), "shim_tf_pos");
        if (gs)
            for (int i = 0; i < gs->nin && n < 16; i++)
                snprintf(names[n++], sizeof(names[0]), "%s", gs->in[i]);
        if (n > 0) {
            const GLchar *ptrs[16];
            for (int i = 0; i < n; i++) ptrs[i] = names[i];
            real_glTransformFeedbackVaryings(ep->prog, n, ptrs,
                                              GL_INTERLEAVED_ATTRIBS);
        }
    }
    real_glLinkProgram(ep->prog);
    GLint ok = 0;
    real_glGetProgramiv(ep->prog, GL_LINK_STATUS, &ok);
    if (!ok) shim_emu_log(ep, "P1(TF)", ep->prog);
    if (fs0 && real_glDeleteShader) real_glDeleteShader(fs0);
    if (tfvs && real_glDeleteShader) real_glDeleteShader(tfvs);
    if (!ok) return 0;
    shim_bind_ff_program(ep->prog, c);

    if (gs && gs->compiled && gs->cs) {
        ep->p2 = real_glCreateProgram();
        real_glAttachShader(ep->p2, gs->cs);
        real_glLinkProgram(ep->p2);
        real_glGetProgramiv(ep->p2, GL_LINK_STATUS, &ok);
        if (!ok) shim_emu_log(ep, "P2(GS)", ep->p2);
        shim_bind_ff_program(ep->p2, c);
        ep->out_prim = gs->out_prim;
        ep->max_vertices = gs->max_vertices;
        ep->max_idx_per_prim = gs->max_idx;
        ep->patch_vertices = 1;
        ep->stride = 4 + (gs->psize ? 1 : 0);
        for (int i = 0; i < gs->nin; i++) ep->stride += shim_type_size(gs->in_type[i]);
        ep->nattrs = gs->nout;
        int aoff = 4 + (gs->psize ? 1 : 0);
        for (int i = 0; i < gs->nout; i++) {
            snprintf(ep->attr_name[i], sizeof(ep->attr_name[0]), "%s", gs->out[i]);
            ep->attr_type[i] = gs->out_type[i][0] == 'i' ? GL_INT :
                               gs->out_type[i][0] == 'u' ? GL_UNSIGNED_INT : GL_FLOAT;
            ep->attr_size[i] = shim_type_size(gs->out_type[i]);
            ep->attr_off[i] = aoff;
            aoff += ep->attr_size[i];
        }
        ep->out_stride = aoff;
    } else if (tcs && tes && tcs->compiled && tes->compiled && tcs->cs && tes->cs) {
        ep->p2a = real_glCreateProgram();
        real_glAttachShader(ep->p2a, tcs->cs);
        real_glLinkProgram(ep->p2a);
        real_glGetProgramiv(ep->p2a, GL_LINK_STATUS, &ok);
        if (!ok) shim_emu_log(ep, "P2a(TCS)", ep->p2a);
        ep->p2b = real_glCreateProgram();
        real_glAttachShader(ep->p2b, tes->cs);
        real_glLinkProgram(ep->p2b);
        real_glGetProgramiv(ep->p2b, GL_LINK_STATUS, &ok);
        if (!ok) shim_emu_log(ep, "P2b(TES)", ep->p2b);
        shim_bind_ff_program(ep->p2a, c);
        shim_bind_ff_program(ep->p2b, c);
        ep->patch_vertices = tcs->patch_vertices ? tcs->patch_vertices : shim_patch_vertices;
        ep->out_prim = tes->point_mode ? GL_POINTS :
                       tes->in_prim == 5 ? GL_LINES : GL_TRIANGLES;
        ep->max_vertices = tes->max_vertices;
        ep->max_idx_per_prim = tes->max_idx;
        ep->stride = 4;
        ep->out_stride = 4;
        ep->nattrs = 0;
    } else {
        shim_emu_log(ep, "P2", 0);
        return 0;
    }
    if (!ok) return 0;

    const char *outs = gs ? (gs->src ? gs->src : "") : "";
    int psize = 0;
    const char *pv = shim_passthrough_vs(outs, &psize);
    GLuint vs3 = real_glCreateShader(GL_VERTEX_SHADER);
    if (vs3) {
        const GLchar *v3 = pv;
        real_glShaderSource(vs3, 1, &v3, NULL);
        real_glCompileShader(vs3);
    }
    ep->p3 = real_glCreateProgram();
    if (vs3) real_glAttachShader(ep->p3, vs3);
    if (ep->fs) real_glAttachShader(ep->p3, ep->fs);
    real_glLinkProgram(ep->p3);
    real_glGetProgramiv(ep->p3, GL_LINK_STATUS, &ok);
    if (!ok) shim_emu_log(ep, "P3", ep->p3);
    shim_bind_ff_program(ep->p3, c);
    if (vs3 && real_glDeleteShader) real_glDeleteShader(vs3);
    ep->link_ok = ok ? 1 : 0;
    return ep->link_ok;
}

/* -------------------------------------------------------------------------- */
/* Shader/program/uniform hooks                                               */
/*                                                                            */
/* These hooks bridge the exported entry points to G.3 state. The heavy work  */
/* happens in the virtual-program builder: here we only decide whether the    */
/* currently bound program has an active emulation bundle.                    */
/* -------------------------------------------------------------------------- */

static void shim_emu_on_link_program(GLuint prog)
{ (void)prog; /* The exported glLinkProgram already builds the virtual programs. */ }

/* The exported glDeleteProgram already frees the ShimEmuProg entry, so this
   hook is intentionally a no-op. */
static void shim_emu_on_delete_program(GLuint prog)
{ (void)prog; }

static void shim_emu_on_use_program(GLuint prog)
{
    ShimCtx *c = tls_ctx_ptr;
    ShimEmuProg *ep = c ? shim_emu_prog_find(c, prog, 0) : NULL;
    if (c) c->emu_cur = (ep && ep->link_ok && (ep->gs || ep->tcs || ep->tes)) ? 1 : 0;
}

#define SHIM_EXPORT(ret, name, args) \
    __attribute__((visibility("default"))) ret name args

SHIM_EXPORT(GLuint, glCreateShader, (GLenum type))
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    int kind = type == GL_GEOMETRY_SHADER ? 1 :
               type == GL_TESS_CONTROL_SHADER ? 2 :
               type == GL_TESS_EVALUATION_SHADER ? 3 : 0;
    if (kind && c && shim_emu_ok(c, kind)) {
        /* Virtual ID: an empty real shader, so glIsShader/glDeleteShader
           remain valid in the app. */
        GLuint id = real_glCreateShader ? real_glCreateShader(GL_VERTEX_SHADER) : 0;
        const GLchar *empty = "";
        if (id && real_glShaderSource) real_glShaderSource(id, 1, &empty, NULL);
        pthread_mutex_lock(&g_emu_mutex);
        ShimVShader *v = calloc(1, sizeof(*v));
        if (v) {
            v->id = id;
            v->kind = kind;
            v->next = g_vshaders;
            g_vshaders = v;
            g_emu_count++;
        }
        pthread_mutex_unlock(&g_emu_mutex);
        return id;
    }
    return real_glCreateShader ? real_glCreateShader(type) : 0;
}

SHIM_EXPORT(void, glDeleteShader, (GLuint shader))
{
    ensure_gl_symbols();
    pthread_mutex_lock(&g_emu_mutex);
    ShimVShader **pv = &g_vshaders;
    while (*pv && (*pv)->id != shader) pv = &(*pv)->next;
    if (*pv) {
        ShimVShader *v = *pv;
        *pv = v->next;
        if (v->cs && real_glDeleteShader) real_glDeleteShader(v->cs);
        free(v->src);
        free(v->log);
        free(v);
        g_emu_count--;
    }
    ShimShaderSrc **ps = &g_shader_src;
    while (*ps && (*ps)->id != shader) ps = &(*ps)->next;
    if (*ps) {
        ShimShaderSrc *s = *ps;
        *ps = s->next;
        free(s->src);
        free(s);
    }
    pthread_mutex_unlock(&g_emu_mutex);
    if (real_glDeleteShader) real_glDeleteShader(shader);
}

SHIM_EXPORT(void, glCompileShader, (GLuint shader))
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    ShimVShader *v = shim_vshader_find(shader);
    if (v && v->kind >= 1 && v->kind <= 3) {
        StrBuf cs = {0};
        int ok = v->kind == 1 ? shim_translate_gs(v, &cs) :
                                shim_translate_tess(c, v, &cs);
        free(v->log);
        v->log = NULL;
        if (ok && cs.data && real_glCreateShader) {
            v->cs = real_glCreateShader(GL_COMPUTE_SHADER);
            const GLchar *src = cs.data;
            real_glShaderSource(v->cs, 1, &src, NULL);
            real_glCompileShader(v->cs);
            GLint st = 0;
            real_glGetShaderiv(v->cs, GL_COMPILE_STATUS, &st);
            v->compiled = st ? 1 : 0;
            if (!st && real_glGetShaderInfoLog) {
                char buf[1024] = {0};
                real_glGetShaderInfoLog(v->cs, sizeof(buf) - 1, NULL, buf);
                v->log = strdup(buf);
            }
        } else {
            v->compiled = 0;
            v->log = strdup("NUUBOS_SHIM: case unsupported by G.3 emulation");
        }
        free(cs.data);
        return;
    }
    if (real_glCompileShader) real_glCompileShader(shader);
}

SHIM_EXPORT(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params))
{
    ensure_gl_symbols();
    ShimVShader *v = shim_vshader_find(shader);
    if (v && v->kind >= 1 && v->kind <= 3 && params) {
        if (pname == GL_SHADER_TYPE)
            *params = v->kind == 1 ? GL_GEOMETRY_SHADER :
                      v->kind == 2 ? GL_TESS_CONTROL_SHADER : GL_TESS_EVALUATION_SHADER;
        else if (pname == GL_COMPILE_STATUS) *params = v->compiled ? GL_TRUE : GL_FALSE;
        else if (pname == GL_INFO_LOG_LENGTH) *params = v->log ? (GLint)strlen(v->log) + 1 : 1;
        else *params = 0;
        return;
    }
    if (real_glGetShaderiv) real_glGetShaderiv(shader, pname, params);
}

SHIM_EXPORT(void, glGetShaderInfoLog, (GLuint shader, GLsizei max,
                                       GLsizei *len, GLchar *log))
{
    ensure_gl_symbols();
    ShimVShader *v = shim_vshader_find(shader);
    if (v && log && max > 0) {
        size_t n = v->log ? strlen(v->log) : 0;
        if (n > (size_t)max - 1) n = (size_t)max - 1;
        memcpy(log, v->log ? v->log : "", n);
        log[n] = 0;
        if (len) *len = (GLsizei)n;
        return;
    }
    if (real_glGetShaderInfoLog) real_glGetShaderInfoLog(shader, max, len, log);
}

SHIM_EXPORT(void, glAttachShader, (GLuint prog, GLuint shader))
{
    ensure_gl_symbols();
    if (real_glGetShaderiv && !shim_vshader_find(shader)) {
        GLint ty = 0;
        real_glGetShaderiv(shader, GL_SHADER_TYPE, &ty);
        if (ty == GL_VERTEX_SHADER || ty == GL_FRAGMENT_SHADER) {
            pthread_mutex_lock(&g_prog_mutex);
            struct ShimProgAttach *a = g_prog_attach;
            while (a && a->prog != prog) a = a->next;
            if (!a) {
                a = calloc(1, sizeof(*a));
                if (a) { a->prog = prog; a->next = g_prog_attach; g_prog_attach = a; }
            }
            if (a) {
                if (ty == GL_VERTEX_SHADER) a->vs = shader;
                else a->fs = shader;
            }
            pthread_mutex_unlock(&g_prog_mutex);
        }
    }
    ShimCtx *c = shim_get_ctx();
    ShimVShader *v = shim_vshader_find(shader);
    int emu_enabled = c && !g_emu_failed && desktop_mode() &&
                      (c->features.geom_emu || c->features.tess_emu);
    ShimEmuProg *ep = shim_emu_prog_find(c, prog, emu_enabled);
    if (v && v->kind >= 1 && v->kind <= 3 && ep) {
        if (v->kind == 1) ep->gs = shader;
        else if (v->kind == 2) ep->tcs = shader;
        else ep->tes = shader;
        if (ep->fs && ep->fs_attached && real_glDetachShader) {
            real_glDetachShader(prog, ep->fs);
            ep->fs_attached = 0;
        }
        return;
    }
    if (ep && !v && (ep->gs || ep->tcs || ep->tes)) {
        if (!ep->vs) {
            ep->vs = shader;
            ep->vs_attached = 1;
        } else if (!ep->fs) {
            ep->fs = shader;
            ep->fs_attached = 0;
        }
        return;
    }
    if (real_glAttachShader) real_glAttachShader(prog, shader);
}

SHIM_EXPORT(void, glDetachShader, (GLuint prog, GLuint shader))
{
    ensure_gl_symbols();
    ShimEmuProg *ep = shim_emu_prog_find(shim_get_ctx(), prog, 0);
    if (ep) {
        if (ep->gs == shader) ep->gs = 0;
        if (ep->tcs == shader) ep->tcs = 0;
        if (ep->tes == shader) ep->tes = 0;
        if (ep->fs == shader) ep->fs = 0;
    }
    if (real_glDetachShader) real_glDetachShader(prog, shader);
}

SHIM_EXPORT(void, glLinkProgram, (GLuint prog))
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    ShimEmuProg *ep = shim_emu_prog_find(c, prog, 0);
    if (ep && shim_emu_build(c, ep)) return;
    shim_glLinkProgram(prog);
}

SHIM_EXPORT(void, glDeleteProgram, (GLuint prog))
{
    ensure_gl_symbols();
    ShimCtx *c = tls_ctx_ptr;
    if (c) {
        ShimEmuProg **p = &c->emu_progs;
        while (*p && (*p)->prog != prog) p = &(*p)->next;
        if (*p) {
            ShimEmuProg *old = *p;
            *p = old->next;
            shim_emu_free_prog(old);
        }
        if (c->cur_prog == prog) {
            c->cur_prog = 0;
            c->emu_cur = 0;
        }
    }
    shim_emu_on_delete_program(prog);
    if (real_glDeleteProgram) real_glDeleteProgram(prog);
}

SHIM_EXPORT(void, glGetProgramiv, (GLuint prog, GLenum pname, GLint *params))
{
    ensure_gl_symbols();
    ShimEmuProg *ep = shim_emu_prog_find(shim_get_ctx(), prog, 0);
    if (ep && (ep->gs || ep->tcs || ep->tes) && params) {
        if (pname == GL_LINK_STATUS) { *params = ep->link_ok ? GL_TRUE : GL_FALSE; return; }
        if (pname == GL_INFO_LOG_LENGTH) {
            *params = ep->log ? (GLint)strlen(ep->log) + 1 : 1;
            return;
        }
    }
    if (real_glGetProgramiv) real_glGetProgramiv(prog, pname, params);
}

SHIM_EXPORT(void, glGetProgramInfoLog, (GLuint prog, GLsizei max,
                                        GLsizei *len, GLchar *log))
{
    ensure_gl_symbols();
    ShimEmuProg *ep = shim_emu_prog_find(shim_get_ctx(), prog, 0);
    if (ep && ep->log && log && max > 0) {
        size_t n = strlen(ep->log);
        if (n > (size_t)max - 1) n = (size_t)max - 1;
        memcpy(log, ep->log, n);
        log[n] = 0;
        if (len) *len = (GLsizei)n;
        return;
    }
    if (real_glGetProgramInfoLog) real_glGetProgramInfoLog(prog, max, len, log);
}

static ShimEmuUniform *shim_uni_find(ShimEmuProg *ep, GLint vloc, int create)
{
    if (!ep) return NULL;
    ShimEmuUniform **p = &ep->uniforms;
    while (*p && (GLuint)vloc != SHIM_VLOC_BASE + (GLuint)(*p)->slot) p = &(*p)->next;
    if (!*p && create) {
        *p = calloc(1, sizeof(**p));
        if (*p) for (int i = 0; i < 4; i++) (*p)->l[i] = -1;
    }
    return *p;
}

SHIM_EXPORT(GLint, glGetUniformLocation, (GLuint prog, const GLchar *name))
{
    ensure_gl_symbols();
    ShimCtx *c = shim_get_ctx();
    ShimEmuProg *ep = shim_emu_prog_find(c, prog, 0);
    if (ep && (ep->gs || ep->tcs || ep->tes) && real_glGetUniformLocation && name) {
        ShimEmuUniform *u = NULL;
        for (ShimEmuUniform *it = ep->uniforms; it; it = it->next)
            if (!strcmp(it->name, name)) { u = it; break; }
        if (!u) {
            u = calloc(1, sizeof(*u));
            if (!u) return -1;
            snprintf(u->name, sizeof(u->name), "%s", name);
            for (int i = 0; i < 4; i++) u->l[i] = -1;
            u->l[0] = real_glGetUniformLocation(ep->prog, name);
            GLuint p2 = ep->p2 ? ep->p2 : ep->p2a;
            u->l[1] = p2 ? real_glGetUniformLocation(p2, name) : -1;
            u->l[2] = ep->p3 ? real_glGetUniformLocation(ep->p3, name) : -1;
            u->l[3] = ep->p2b ? real_glGetUniformLocation(ep->p2b, name) : -1;
            if (u->l[0] < 0 && u->l[1] < 0 && u->l[2] < 0 && u->l[3] < 0) {
                free(u);
                return -1;
            }
            u->next = ep->uniforms;
            ep->uniforms = u;
        }
        int slot = 0;
        for (ShimEmuUniform *it = ep->uniforms; it; it = it->next, slot++)
            if (it == u) return (GLint)(SHIM_VLOC_BASE + (GLuint)slot);
    }
    return real_glGetUniformLocation ? real_glGetUniformLocation(prog, name) : -1;
}

SHIM_EXPORT(GLuint, glGetUniformBlockIndex, (GLuint prog, const GLchar *name))
{
    ensure_gl_symbols();
    ShimEmuProg *ep = shim_emu_prog_find(shim_get_ctx(), prog, 0);
    if (ep && (ep->gs || ep->tcs || ep->tes) && real_glGetUniformBlockIndex)
        return real_glGetUniformBlockIndex(ep->prog, name);
    return real_glGetUniformBlockIndex ? real_glGetUniformBlockIndex(prog, name)
                                       : GL_INVALID_INDEX;
}

SHIM_EXPORT(void, glUniformBlockBinding, (GLuint prog, GLuint idx, GLuint binding))
{
    ensure_gl_symbols();
    ShimEmuProg *ep = shim_emu_prog_find(shim_get_ctx(), prog, 0);
    if (ep && (ep->gs || ep->tcs || ep->tes) && real_glUniformBlockBinding) {
        GLuint pp[4] = {ep->prog, ep->p2 ? ep->p2 : ep->p2a, ep->p3, ep->p2b};
        for (int i = 0; i < 4; i++)
            if (pp[i]) real_glUniformBlockBinding(pp[i], idx, binding);
        return;
    }
    if (real_glUniformBlockBinding) real_glUniformBlockBinding(prog, idx, binding);
}

#define SHIM_UNI_PROGS(ep) \
    GLuint pp[4] = {(ep)->prog, (ep)->p2 ? (ep)->p2 : (ep)->p2a, (ep)->p3, (ep)->p2b}

#define SHIM_UNI_1(name, sfx, ctype) \
SHIM_EXPORT(void, name, (GLint loc, ctype x)) \
{ \
    ShimCtx *c = tls_ctx_ptr; \
    ShimEmuProg *ep = c ? shim_emu_prog_find(c, c->cur_prog, 0) : NULL; \
    ShimEmuUniform *u = (ep && loc >= (GLint)SHIM_VLOC_BASE) ? \
                        shim_uni_find(ep, loc, 0) : NULL; \
    if (u) { SHIM_UNI_PROGS(ep); for (int i=0;i<4;i++) \
        if (u->l[i]>=0 && pp[i] && real_glProgramUniform1##sfx) { \
            real_glProgramUniform1##sfx(pp[i], u->l[i], x); return; } } \
    if (real_##name) real_##name(loc, x); \
}

SHIM_UNI_1(glUniform1f, f, GLfloat)
SHIM_UNI_1(glUniform1i, i, GLint)
SHIM_UNI_1(glUniform1ui, ui, GLuint)

#define SHIM_UNI_V(name, n, sfx, ctype) \
SHIM_EXPORT(void, name, (GLint loc, GLsizei cnt, const ctype *v)) \
{ \
    if (!v || cnt <= 0) return; \
    ShimCtx *c = tls_ctx_ptr; \
    ShimEmuProg *ep = c ? shim_emu_prog_find(c, c->cur_prog, 0) : NULL; \
    ShimEmuUniform *u = (ep && loc >= (GLint)SHIM_VLOC_BASE) ? \
                        shim_uni_find(ep, loc, 0) : NULL; \
    if (u) { SHIM_UNI_PROGS(ep); for (int i=0;i<4;i++) \
        if (u->l[i]>=0 && pp[i] && real_glProgramUniform##n##sfx##v) { \
            real_glProgramUniform##n##sfx##v(pp[i], u->l[i], cnt, v); return; } } \
    if (real_##name) real_##name(loc, cnt, v); \
}

SHIM_UNI_V(glUniform1fv, 1, f, GLfloat)
SHIM_UNI_V(glUniform2fv, 2, f, GLfloat)
SHIM_UNI_V(glUniform3fv, 3, f, GLfloat)
SHIM_UNI_V(glUniform4fv, 4, f, GLfloat)

#define SHIM_UNI_M(name, n) \
SHIM_EXPORT(void, name, (GLint loc, GLsizei cnt, GLboolean tr, const GLfloat *v)) \
{ \
    if (!v || cnt <= 0) return; \
    ShimCtx *c = tls_ctx_ptr; \
    ShimEmuProg *ep = c ? shim_emu_prog_find(c, c->cur_prog, 0) : NULL; \
    ShimEmuUniform *u = (ep && loc >= (GLint)SHIM_VLOC_BASE) ? \
                        shim_uni_find(ep, loc, 0) : NULL; \
    if (u) { SHIM_UNI_PROGS(ep); for (int i=0;i<4;i++) \
        if (u->l[i]>=0 && pp[i] && real_glProgramUniformMatrix##n##fv) { \
            real_glProgramUniformMatrix##n##fv(pp[i], u->l[i], cnt, tr, v); return; } } \
    if (real_##name) real_##name(loc, cnt, tr, v); \
}

SHIM_UNI_M(glUniformMatrix2fv, 2)
SHIM_UNI_M(glUniformMatrix3fv, 3)
SHIM_UNI_M(glUniformMatrix4fv, 4)

SHIM_EXPORT(void, glPatchParameteri, (GLenum pname, GLint value))
{
    if (pname != GL_PATCH_VERTICES) return;
    shim_patch_vertices = value < 1 ? 1 : value > 32 ? 32 : value;
}

SHIM_EXPORT(void, glPatchParameterfv, (GLenum pname, const GLfloat *values))
{ (void)pname; (void)values; /* milestone 1: default levels 1.0 */ }

/* -------------------------------------------------------------------------- */
/* Emulated drawing (G.3.5)                                                   */
/*                                                                            */
/* For a program with active emulation, draw calls are split into: (1) a      */
/* compute dispatch that expands vertices into an SSBO, (2) a TF pass that     */
/* feeds attributes to the rasterisation program, and (3) the real draw.      */
/* Unsupported primitives/levels are traced and skipped, never submitted to   */
/* the driver in a half-translated state.                                      */
/* -------------------------------------------------------------------------- */

static GLenum shim_flat_mode(GLenum mode, int *verts, int *extra)
{
    *extra = 0;
    if (mode == GL_POINTS) { *verts = 1; return GL_POINTS; }
    if (mode == GL_LINES) { *verts = 2; return GL_LINES; }
    if (mode == GL_LINE_STRIP) { *verts = 2; *extra = 1; return GL_LINES; }
    if (mode == GL_LINE_LOOP) { *verts = 2; return GL_LINES; }
    if (mode == GL_TRIANGLES) { *verts = 3; return GL_TRIANGLES; }
    if (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) {
        *verts = 3; *extra = 2; return GL_TRIANGLES;
    }
    *verts = 0;
    return GL_NONE;
}

static void shim_emu_buffer(ShimCtx *c, GLuint *buf, unsigned *cap, unsigned need)
{
    if (need <= *cap) return;
    unsigned nc = *cap ? *cap : 4096;
    while (nc < need) nc <<= 1;
    if (!*buf && real_glGenBuffers) real_glGenBuffers(1, buf);
    if (*buf && real_glBindBuffer && real_glBufferData) {
        real_glBindBuffer(GL_SHADER_STORAGE_BUFFER, *buf);
        real_glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)nc, NULL, GL_STREAM_DRAW);
        *cap = nc;
    }
}

static void shim_emu_draw_common(ShimCtx *c, GLenum mode, GLint first, GLsizei count,
                                 GLenum type, const void *indices, int indexed)
{
    ShimEmuProg *ep = shim_emu_prog_find(c, c->cur_prog, 0);
    if (!ep || !ep->link_ok || !real_glBeginTransformFeedback ||
        !real_glDispatchCompute || !real_glDrawElements) return;
    int verts = 0, extra = 0;
    if (mode == GL_LINE_LOOP || mode == GL_QUADS || mode == GL_QUAD_STRIP ||
        mode == GL_POLYGON || (mode == GL_PATCHES && !ep->p2b)) {
        shim_trace("G.3: unsupported topology 0x%x; draw skipped\n", mode);
        return;
    }
    GLenum flat = shim_flat_mode(mode, &verts, &extra);
    int is_tess = mode == GL_PATCHES || ep->p2b != 0;
    if (is_tess) { flat = GL_POINTS; verts = ep->patch_vertices ? ep->patch_vertices : shim_patch_vertices; }
    if (!verts || (flat != GL_POINTS && flat != GL_LINES && flat != GL_TRIANGLES) ||
        count < extra) {
        shim_trace("G.3: unsupported topology (0x%x); draw skipped\n", mode);
        return;
    }
    int np = (count - extra) / verts;
    if (np <= 0) return;
    int stride = is_tess ? 4 : ep->stride;
    unsigned bytes_prim = (unsigned)stride * 4u * (unsigned)(is_tess ? 1 : verts);
    if (!bytes_prim) return;
    unsigned cap = (unsigned)g_geom_cap_mb * 1024u * 1024u;
    if (cap > (64u << 20)) cap = 64u << 20;
    int chunk = (int)(cap / bytes_prim);
    if (chunk < 1) { shim_trace("G.3: a primitive does not fit in SSBO; draw skipped\n"); return; }
    if (chunk > np) chunk = np;
    shim_emu_buffer(c, &c->emu_tf_buf, &c->emu_tf_cap,
                    (unsigned)chunk * bytes_prim);
    shim_emu_buffer(c, &c->emu_out_vbo, &c->emu_out_vbo_cap,
                    (unsigned)chunk * (unsigned)ep->max_vertices *
                    (unsigned)(ep->out_stride ? ep->out_stride : 4) * 4u);
    shim_emu_buffer(c, &c->emu_out_ibo, &c->emu_out_ibo_cap,
                    (unsigned)chunk * (unsigned)ep->max_idx_per_prim * 4u);

    GLint sf = 0, sp = 0, sn = 0, stf = 0, ssbo[4] = {0,0,0,0}, discard = 0;
    GLint svao = 0;
    if (real_glGetIntegerv) {
        real_glGetIntegerv(0x8B8D, &sp);       /* CURRENT_PROGRAM */
        real_glGetIntegerv(0x85B5, &svao);     /* VERTEX_ARRAY_BINDING */
        real_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &sf);
        real_glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, &stf);
        for (int i = 0; i < 4; i++) {
            ssbo[i] = 0;
            if (real_glGetIntegeri_v)
                real_glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,
                                     (GLuint)i, &ssbo[i]);
        }
    }
    if (real_glIsEnabled) discard = real_glIsEnabled(GL_RASTERIZER_DISCARD);
    if (!c->emu_vao && real_glGenVertexArrays) real_glGenVertexArrays(1, &c->emu_vao);

    int out_mode = ep->out_prim == GL_POINTS ? GL_POINTS :
                   ep->out_prim == GL_LINES ? GL_LINES : GL_TRIANGLES;
    size_t tb = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;

    for (int start = 0; start < np; start += chunk) {
        int cur = (np - start < chunk) ? np - start : chunk;
        real_glEnable(GL_RASTERIZER_DISCARD);
        real_glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, c->emu_tf_buf);
        real_glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, c->emu_tf_buf);
        real_glUseProgram(ep->prog);
        real_glBeginTransformFeedback(flat);
        if (indexed) {
            const uint8_t *base = (const uint8_t *)indices +
                                  (size_t)(start * verts) * tb;
            real_glDrawElements(flat, cur * verts, type, base);
        } else {
            real_glDrawArrays(flat, first + start * verts, cur * verts);
        }
        real_glEndTransformFeedback();
        real_glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        real_glDisable(GL_RASTERIZER_DISCARD);
        real_glMemoryBarrier(GL_ALL_BARRIER_BITS);

        GLuint pc = is_tess ? ep->p2a : ep->p2;
        if (pc) {
            real_glUseProgram(pc);
            real_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, c->emu_tf_buf);
            real_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, c->emu_out_vbo);
            real_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, c->emu_out_ibo);
            if (is_tess && real_glProgramUniform1ui)
                real_glProgramUniform1ui(pc, real_glGetUniformLocation(pc, "shim_pv"),
                                         (GLuint)(ep->patch_vertices ? ep->patch_vertices
                                                                     : shim_patch_vertices));
            if (real_glProgramUniform1ui) {
                GLint un = real_glGetUniformLocation(pc, "shim_nprims");
                GLint uf = real_glGetUniformLocation(pc, "shim_first");
                GLint uv = real_glGetUniformLocation(pc, "shim_vin");
                if (un >= 0) real_glProgramUniform1ui(pc, un, (GLuint)cur);
                if (uf >= 0) real_glProgramUniform1ui(pc, uf, (GLuint)start);
                if (uv >= 0) real_glProgramUniform1ui(pc, uv, (GLuint)verts);
            }
            real_glDispatchCompute((GLuint)((cur + 63) / 64), 1, 1);
            if (is_tess && ep->p2b) {
                real_glUseProgram(ep->p2b);
                real_glDispatchCompute((GLuint)((cur + 63) / 64), 1, 1);
            }
            real_glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT |
                                 GL_ELEMENT_ARRAY_BARRIER_BIT);
        }
        if (!is_tess && ep->p3) {
            real_glUseProgram(ep->p3);
            real_glBindVertexArray(c->emu_vao);
            real_glBindBuffer(GL_ARRAY_BUFFER, c->emu_out_vbo);
            int os = ep->out_stride ? ep->out_stride : 4;
            real_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE,
                                       (GLsizei)(os * 4), (const void *)0);
            real_glEnableVertexAttribArray(0);
            for (int i = 0; i < ep->nattrs && i < 15; i++) {
                real_glVertexAttribPointer((GLuint)(i + 1), ep->attr_size[i],
                                           ep->attr_type[i], GL_FALSE,
                                           (GLsizei)(os * 4),
                                           (const void *)(size_t)(ep->attr_off[i] * 4));
                real_glEnableVertexAttribArray((GLuint)(i + 1));
            }
            real_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->emu_out_ibo);
            real_glDrawElements(out_mode, cur * ep->max_idx_per_prim,
                                GL_UNSIGNED_INT, NULL);
        }
    }
    if (real_glBindVertexArray) real_glBindVertexArray((GLuint)svao);
    real_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)sf);
    real_glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)stf);
    for (int i = 0; i < 4; i++)
        real_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)i, (GLuint)ssbo[i]);
    if (discard) real_glEnable(GL_RASTERIZER_DISCARD);
    else real_glDisable(GL_RASTERIZER_DISCARD);
    real_glUseProgram((GLuint)sp);
    (void)sn;
}

void shim_emu_draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    ShimCtx *c = tls_ctx_ptr;
    if (c) shim_emu_draw_common(c, mode, first, count, 0, NULL, 0);
}

void shim_emu_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    ShimCtx *c = tls_ctx_ptr;
    if (c) shim_emu_draw_common(c, mode, 0, count, type, indices, 1);
}

/* -------------------------------------------------------------------------- */
/* Interface-block flattening (G.3.4)                                         */
/*                                                                            */
/* Transform feedback cannot capture a UBO/SSBO block as a whole, so blocks   */
/* referenced by the translated shaders are expanded to individual uniforms   */
/* before linking. This keeps the output layout explicit and portable.        */
/* -------------------------------------------------------------------------- */

StrBuf shim_flatten_interface_blocks(const char *src)
{
    StrBuf out = {0};
    if (!src) return out;
    const char *p = src;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        size_t n = (size_t)(p - line);
        const char *q = shim_skip_ws(line);
        int qual = !strncmp(q, "out ", 4) || !strncmp(q, "in ", 3);
        const char *br = strchr(line, '{');
        if (qual && br) {
            const char *end = strstr(br, "};");
            if (!end) end = strstr(br, "}");
            if (end) {
                char head[128] = {0};
                size_t hn = (size_t)(br - line);
                if (hn >= sizeof(head)) hn = sizeof(head) - 1;
                memcpy(head, line, hn);
                head[hn] = 0;
                char qw[16] = {0}, block[64] = {0};
                if (sscanf(head, " %15s %63s", qw, block) == 2) {
                    const char *m = br + 1;
                    while (m < end) {
                        const char *ml = m;
                        while (m < end && *m != ';') m++;
                        const char *mq = shim_skip_ws(ml);
                        if (mq < end && *mq) {
                            char ty[32] = {0}, nm[64] = {0};
                            if (sscanf(mq, " %31s %63[^;]", ty, nm) == 2) {
                                char *e = nm;
                                while (*e && *e != ' ' && *e != '[') e++;
                                *e = 0;
                                if (*nm) sb_addf(&out, "%s %s %s_%s;\n", qw, ty, block, nm);
                            }
                        }
                        if (m < end) m++;
                    }
                    if (*p) p++;
                    continue;
                }
            }
        }
        sb_addn(&out, line, n);
        if (*p) sb_addn(&out, p++, 1);
    }
    return out;
}

/* -------------------------------------------------------------------------- */
/* Direct exports: LD_PRELOAD must also interpose regular calls              */
/* into libGLESv2, not only those obtained via dlsym().                       */
/*                                                                            */
/* GLFUNCS covers calls reached via dlsym/eglGetProcAddress; these exported    */
/* symbols cover programs linked directly against libGL. Both paths end up in */
/* the same shim_* wrappers, so state is recorded only once.                   */
/* -------------------------------------------------------------------------- */
SHIM_EXPORT(const GLubyte *, glGetString, (GLenum p0)) { return shim_glGetString(p0); }
SHIM_EXPORT(const GLubyte *, glGetStringi, (GLenum p0, GLuint p1)) { return shim_glGetStringi(p0, p1); }
SHIM_EXPORT(void, glGetIntegerv, (GLenum p0, GLint * p1)) { shim_glGetIntegerv(p0, p1); }
SHIM_EXPORT(void, glGetFloatv, (GLenum p0, GLfloat * p1)) { shim_glGetFloatv(p0, p1); }
SHIM_EXPORT(GLboolean, glIsEnabled, (GLenum p0)) { return shim_glIsEnabled(p0); }
SHIM_EXPORT(void, glEnable, (GLenum p0)) { shim_glEnable(p0); }
SHIM_EXPORT(void, glDisable, (GLenum p0)) { shim_glDisable(p0); }
SHIM_EXPORT(void, glDrawBuffers, (GLsizei p0, const GLenum * p1)) { shim_glDrawBuffers(p0, p1); }
SHIM_EXPORT(void, glBindTexture, (GLenum p0, GLuint p1)) { shim_glBindTexture(p0, p1); }
SHIM_EXPORT(void, glTexParameteri, (GLenum p0, GLenum p1, GLint p2)) { shim_glTexParameteri(p0, p1, p2); }
SHIM_EXPORT(void, glTexParameterf, (GLenum p0, GLenum p1, GLfloat p2)) { shim_glTexParameterf(p0, p1, p2); }
SHIM_EXPORT(void, glTexParameteriv, (GLenum p0, GLenum p1, const GLint * p2)) { shim_glTexParameteriv(p0, p1, p2); }
SHIM_EXPORT(void, glTexParameterfv, (GLenum p0, GLenum p1, const GLfloat * p2)) { shim_glTexParameterfv(p0, p1, p2); }
SHIM_EXPORT(void, glTexImage2D, (GLenum p0, GLint p1, GLint p2, GLsizei p3, GLsizei p4, GLint p5, GLenum p6, GLenum p7, const void * p8)) { shim_glTexImage2D(p0, p1, p2, p3, p4, p5, p6, p7, p8); }
SHIM_EXPORT(void, glTexSubImage2D, (GLenum p0, GLint p1, GLint p2, GLint p3, GLsizei p4, GLsizei p5, GLenum p6, GLenum p7, const void * p8)) { shim_glTexSubImage2D(p0, p1, p2, p3, p4, p5, p6, p7, p8); }
SHIM_EXPORT(void, glTexStorage2D, (GLenum p0, GLsizei p1, GLenum p2, GLsizei p3, GLsizei p4)) { shim_glTexStorage2D(p0, p1, p2, p3, p4); }
SHIM_EXPORT(void, glGenerateMipmap, (GLenum p0)) { shim_glGenerateMipmap(p0); }
SHIM_EXPORT(void, glViewport, (GLint p0, GLint p1, GLsizei p2, GLsizei p3)) { shim_glViewport(p0, p1, p2, p3); }
SHIM_EXPORT(void, glScissor, (GLint p0, GLint p1, GLsizei p2, GLsizei p3)) { shim_glScissor(p0, p1, p2, p3); }
SHIM_EXPORT(void, glBindFramebuffer, (GLenum p0, GLuint p1)) { shim_glBindFramebuffer(p0, p1); }
SHIM_EXPORT(void, glCompressedTexImage2D, (GLenum p0, GLint p1, GLenum p2, GLsizei p3, GLsizei p4, GLint p5, GLsizei p6, const void * p7)) { shim_glCompressedTexImage2D(p0, p1, p2, p3, p4, p5, p6, p7); }
SHIM_EXPORT(void, glCompressedTexSubImage2D, (GLenum p0, GLint p1, GLint p2, GLint p3, GLsizei p4, GLsizei p5, GLenum p6, GLsizei p7, const void * p8)) { shim_glCompressedTexSubImage2D(p0, p1, p2, p3, p4, p5, p6, p7, p8); }
SHIM_EXPORT(void, glDrawArrays, (GLenum p0, GLint p1, GLsizei p2)) { shim_glDrawArrays(p0, p1, p2); }
SHIM_EXPORT(void, glDrawElements, (GLenum p0, GLsizei p1, GLenum p2, const void * p3)) { shim_glDrawElements(p0, p1, p2, p3); }
SHIM_EXPORT(void, glDrawRangeElements, (GLenum p0, GLuint p1, GLuint p2, GLsizei p3, GLenum p4, const void * p5)) { shim_glDrawRangeElements(p0, p1, p2, p3, p4, p5); }
SHIM_EXPORT(void, glShaderSource, (GLuint p0, GLsizei p1, const GLchar *const * p2, const GLint * p3)) { shim_glShaderSource(p0, p1, p2, p3); }
SHIM_EXPORT(void, glShaderSourceARB, (GLuint p0, GLsizei p1, const GLchar *const * p2, const GLint * p3)) { shim_glShaderSource(p0, p1, p2, p3); }
SHIM_EXPORT(GLuint, glCreateShaderObjectARB, (GLenum p0)) { return glCreateShader(p0); }
SHIM_EXPORT(void, glCompileShaderARB, (GLuint p0)) { glCompileShader(p0); }
SHIM_EXPORT(void, glAttachShaderARB, (GLuint p0, GLuint p1)) { glAttachShader(p0, p1); }
SHIM_EXPORT(void, glLinkProgramARB, (GLuint p0)) { glLinkProgram(p0); }
SHIM_EXPORT(void, glUseProgram, (GLuint p0)) { shim_glUseProgram(p0); }
SHIM_EXPORT(void, glUseProgramObjectARB, (GLuint p0)) { shim_glUseProgram(p0); }
SHIM_EXPORT(GLint, glGetUniformLocationARB, (GLuint p0, const GLchar * p1)) { return glGetUniformLocation(p0, p1); }
SHIM_EXPORT(void, glMatrixMode, (GLenum p0)) { shim_glMatrixMode(p0); }
SHIM_EXPORT(void, glLoadIdentity, ()) { shim_glLoadIdentity(); }

/* -------------------------------------------------------------------------- */
/* H.4: display-list recording/replay (desktop-only)                           */
/*                                                                            */
/* Replay goes through the shim's own wrappers rather than the real driver:    */
/* this way GL_QUADS conversion, G.2 state updates and immediate-mode buffer   */
/* handling are applied exactly as for a direct call. Nested glCallList uses  */
/* a per-list "executing" flag to break cycles.                                */
/* -------------------------------------------------------------------------- */

static void shim_call_list(ShimCtx *, GLuint);

static void shim_dl_replay_cmd(ShimCtx *c, const ShimDLCmd *cmd)
{
    switch (cmd->op) {
    case DL_BEGIN:        glBegin((GLenum)cmd->i[0]); break;
    case DL_END:          glEnd(); break;
    case DL_VERTEX4:      glVertex4f(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
    case DL_COLOR4:       glColor4f(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
    case DL_NORMAL3:      glNormal3f(cmd->f[0], cmd->f[1], cmd->f[2]); break;
    case DL_TEXCOORD4:    glTexCoord4f(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
    case DL_MATRIX_MODE:  glMatrixMode((GLenum)cmd->i[0]); break;
    case DL_LOAD_IDENTITY: glLoadIdentity(); break;
    case DL_PUSH_MATRIX:  glPushMatrix(); break;
    case DL_POP_MATRIX:   glPopMatrix(); break;
    case DL_MATRIX_LOAD:  glLoadMatrixf(cmd->f); break;
    case DL_MATRIX_MULT:  glMultMatrixf(cmd->f); break;
    case DL_ENABLE:       glEnable((GLenum)cmd->i[0]); break;
    case DL_DISABLE:      glDisable((GLenum)cmd->i[0]); break;
    case DL_BIND_TEXTURE: glBindTexture((GLenum)cmd->i[0], (GLuint)cmd->i[1]); break;
    case DL_ACTIVE_TEXTURE: glActiveTexture((GLenum)cmd->i[0]); break;
    case DL_ALPHA_FUNC:   glAlphaFunc((GLenum)cmd->i[0], cmd->f[0]); break;
    case DL_TEXENV_MODE:  glTexEnvf((GLenum)cmd->i[0], (GLenum)cmd->i[1], cmd->f[0]); break;
    case DL_TEXENV_COLOR: glTexEnvfv((GLenum)cmd->i[0], (GLenum)cmd->i[1], cmd->f); break;
    case DL_CALL_LIST:    shim_call_list(c, (GLuint)cmd->i[0]); break;
    }
}

static void shim_call_list(ShimCtx *c, GLuint name)
{
    if (!c || !name || c->dl_recursion >= 64) return;
    ShimDL *l = shim_dl_slot(c, name, 0);
    if (!l || !l->generated) return;
    c->dl_recursion++;
    for (size_t i = 0; i < l->n; i++)
        shim_dl_replay_cmd(c, &l->cmds[i]);
    c->dl_recursion--;
}

SHIM_EXPORT(GLuint, glGenLists, (GLsizei range))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || range <= 0) return 0;
    GLuint start = 1;
    while (start <= c->dl_table_n) {
        GLuint next = start;
        for (GLsizei k = 0; k < range; k++) {
            size_t idx = (size_t)(start + k) - 1;
            if (idx < c->dl_table_n && c->dl_table[idx]) {
                next = start + k + 1;
                break;
            }
        }
        if (next == start) break;
        start = next;
    }
    for (GLsizei k = 0; k < range; k++) {
        ShimDL *l = shim_dl_slot(c, start + (GLuint)k, 1);
        if (l) l->generated = 1;
    }
    return start;
}

SHIM_EXPORT(void, glDeleteLists, (GLuint list, GLsizei range))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || list == 0 || range <= 0) return;
    for (GLsizei k = 0; k < range; k++) {
        GLuint name = list + (GLuint)k;
        if (!name) continue;
        size_t idx = (size_t)name - 1;
        if (idx >= c->dl_table_n) continue;
        ShimDL *l = c->dl_table[idx];
        if (!l) continue;
        free(l->cmds);
        free(l);
        c->dl_table[idx] = NULL;
    }
}

SHIM_EXPORT(GLboolean, glIsList, (GLuint list))
{
    ShimCtx *c = shim_get_ctx();
    ShimDL *l = c ? shim_dl_slot(c, list, 0) : NULL;
    return (l && l->generated) ? GL_TRUE : GL_FALSE;
}

SHIM_EXPORT(void, glListBase, (GLuint base))
{
    ShimCtx *c = shim_get_ctx();
    if (c) c->list_base = (int)base;
}

SHIM_EXPORT(void, glNewList, (GLuint list, GLenum mode))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !list) return;
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) return;
    ShimDL *l = shim_dl_slot(c, list, 1);
    if (!l) return;
    free(l->cmds);
    l->cmds = NULL;
    l->n = l->cap = 0;
    l->generated = 1;
    c->dl_active = l;
    c->dl_exec = (mode == GL_COMPILE_AND_EXECUTE) ? (int)list : 0;
}

SHIM_EXPORT(void, glEndList, (void))
{
    ShimCtx *c = shim_get_ctx();
    if (!c || !c->dl_active) return;
    c->dl_active = NULL;
    shim_imm_count = 0;
    GLuint exec_name = c->dl_exec ? (GLuint)c->dl_exec : 0;
    c->dl_exec = 0;
    if (exec_name) shim_call_list(c, exec_name);
}

SHIM_EXPORT(void, glCallList, (GLuint list))
{
    ShimCtx *c = shim_get_ctx();
    shim_dl_record(c, DL_CALL_LIST, (int)list, 0, NULL, 0);
    shim_call_list(c, list);
}

SHIM_EXPORT(void, glCallLists, (GLsizei n, GLenum type, const void *lists))
{
    if (!lists || n <= 0) return;
    ShimCtx *c = shim_get_ctx();
    size_t sz = type == GL_BYTE ? 1 : type == GL_UNSIGNED_BYTE ? 1 :
                type == GL_SHORT ? 2 : type == GL_UNSIGNED_SHORT ? 2 :
                type == GL_INT || type == GL_UNSIGNED_INT || type == GL_FLOAT ? 4 : 0;
    if (!sz) return;
    const unsigned char *p = (const unsigned char *)lists;
    for (GLsizei i = 0; i < n; i++, p += sz) {
        GLuint v = sz == 1 ? (type == GL_BYTE ? (GLuint)(int)*(const int8_t *)p : *p) :
                   sz == 2 ? (type == GL_SHORT ? (GLuint)(int)*(const int16_t *)p :
                                                  (GLuint)*(const uint16_t *)p) :
                             ((type == GL_FLOAT) ? (GLuint)*(const float *)p :
                              (type == GL_INT ? (GLuint)*(const int32_t *)p :
                                                *(const uint32_t *)p));
        glCallList((GLuint)c->list_base + v);
    }
}

/* -------------------------------------------------------------------------- */
/* GL/EGL symbol table                                                         */
/*                                                                            */
/* shim_lookup_gl() is the single dispatch point for dynamically resolved GL  */
/* names: wrappers first (so emulation can intercept), then real GLES symbols */
/* from GLFUNCS. Unknown names return NULL and SDL falls back to the driver.   */
/* -------------------------------------------------------------------------- */

static void *shim_lookup_gl(const char *name)
{
    ensure_gl_symbols();
    if (!name) return NULL;
#define WRAP(n, fn) if (!strcmp(name, n)) return (void *)(fn)
#define REAL(n) if (!strcmp(name, #n)) return (void *)real_##n

    WRAP("glGetString", shim_glGetString);
    WRAP("glGetStringi", shim_glGetStringi);
    WRAP("glGetIntegerv", shim_glGetIntegerv);
    WRAP("glGetFloatv", shim_glGetFloatv);
    WRAP("glGetDoublev", glGetDoublev);
    WRAP("glGetError", glGetError);
    WRAP("glIsEnabled", shim_glIsEnabled);
    WRAP("glEnable", shim_glEnable);
    WRAP("glDisable", shim_glDisable);
    WRAP("glActiveTexture", glActiveTexture);
    WRAP("glClearDepth", glClearDepth);
    WRAP("glDepthRange", glDepthRange);
    WRAP("glDrawBuffer", glDrawBuffer);
    WRAP("glReadBuffer", glReadBuffer);
    WRAP("glDrawBuffers", shim_glDrawBuffers);
    WRAP("glMapBuffer", shim_glMapBuffer);
    WRAP("glMapBufferARB", glMapBufferARB);
    WRAP("glUnmapBufferARB", glUnmapBufferARB);
    WRAP("glBindTexture", shim_glBindTexture);
    WRAP("glTexParameteri", shim_glTexParameteri);
    WRAP("glTexParameterf", shim_glTexParameterf);
    WRAP("glTexParameteriv", shim_glTexParameteriv);
    WRAP("glTexParameterfv", shim_glTexParameterfv);
    WRAP("glTexImage2D", shim_glTexImage2D);
    WRAP("glTexSubImage2D", shim_glTexSubImage2D);
    WRAP("glTexStorage2D", shim_glTexStorage2D);
    WRAP("glTexImage1D", glTexImage1D);
    WRAP("glTexSubImage1D", glTexSubImage1D);
    WRAP("glGetTexImage", glGetTexImage);
    WRAP("glCompressedTexImage2D", shim_glCompressedTexImage2D);
    WRAP("glCompressedTexSubImage2D", shim_glCompressedTexSubImage2D);
    WRAP("glCompressedTexImage2DARB", glCompressedTexImage2DARB);
    WRAP("glCompressedTexSubImage2DARB", glCompressedTexSubImage2DARB);
    WRAP("glPolygonMode", glPolygonMode);
    WRAP("glPointSize", glPointSize);
    WRAP("glAlphaFunc", glAlphaFunc); WRAP("glShadeModel", glShadeModel);
    WRAP("glPushAttrib", glPushAttrib); WRAP("glPopAttrib", glPopAttrib);
    WRAP("glDrawArrays", shim_glDrawArrays);
    WRAP("glDrawElements", shim_glDrawElements);
    WRAP("glDrawRangeElements", shim_glDrawRangeElements);
    WRAP("glShaderSource", shim_glShaderSource);
    WRAP("glShaderSourceARB", shim_glShaderSource);
    WRAP("glCreateShader", glCreateShader);
    WRAP("glCreateShaderObjectARB", glCreateShader);
    WRAP("glCompileShader", glCompileShader);
    WRAP("glCompileShaderARB", glCompileShader);
    WRAP("glDeleteShader", glDeleteShader);
    WRAP("glGetShaderiv", glGetShaderiv);
    WRAP("glGetShaderInfoLog", glGetShaderInfoLog);
    WRAP("glAttachShader", glAttachShader);
    WRAP("glAttachShaderARB", glAttachShader);
    WRAP("glDetachShader", glDetachShader);
    WRAP("glLinkProgram", glLinkProgram);
    WRAP("glLinkProgramARB", glLinkProgram);
    WRAP("glDeleteProgram", glDeleteProgram);
    WRAP("glUseProgram", shim_glUseProgram);
    WRAP("glUseProgramObjectARB", shim_glUseProgram);
    WRAP("glGetProgramiv", glGetProgramiv);
    WRAP("glGetProgramInfoLog", glGetProgramInfoLog);
    WRAP("glGetUniformLocation", glGetUniformLocation);
    WRAP("glGetUniformLocationARB", glGetUniformLocation);
    WRAP("glUniformBlockBinding", glUniformBlockBinding);
    WRAP("glGetUniformBlockIndex", glGetUniformBlockIndex);
    WRAP("glPatchParameteri", glPatchParameteri);
    WRAP("glPatchParameterfv", glPatchParameterfv);
    WRAP("glUniform1f", glUniform1f);
    WRAP("glUniform1i", glUniform1i);
    WRAP("glUniform1ui", glUniform1ui);
    WRAP("glUniform1fv", glUniform1fv);
    WRAP("glUniform2fv", glUniform2fv);
    WRAP("glUniform3fv", glUniform3fv);
    WRAP("glUniform4fv", glUniform4fv);
    WRAP("glUniformMatrix2fv", glUniformMatrix2fv);
    WRAP("glUniformMatrix3fv", glUniformMatrix3fv);
    WRAP("glUniformMatrix4fv", glUniformMatrix4fv);
    WRAP("glUniform1d", glUniform1d);
    WRAP("glUniform2d", glUniform2d);
    WRAP("glUniform3d", glUniform3d);
    WRAP("glUniform4d", glUniform4d);
    WRAP("glUniform1dv", glUniform1dv);
    WRAP("glUniform2dv", glUniform2dv);
    WRAP("glUniform3dv", glUniform3dv);
    WRAP("glUniform4dv", glUniform4dv);
    WRAP("glUniformMatrix2dv", glUniformMatrix2dv);
    WRAP("glUniformMatrix3dv", glUniformMatrix3dv);
    WRAP("glUniformMatrix4dv", glUniformMatrix4dv);
    WRAP("glMatrixMode", shim_glMatrixMode);
    WRAP("glLoadIdentity", shim_glLoadIdentity);
    WRAP("glLoadMatrixf", glLoadMatrixf);
    WRAP("glLoadMatrixd", glLoadMatrixd);
    WRAP("glMultMatrixf", glMultMatrixf);
    WRAP("glMultMatrixd", glMultMatrixd);
    WRAP("glPushMatrix", glPushMatrix);
    WRAP("glPopMatrix", glPopMatrix);
    WRAP("glTranslatef", glTranslatef);
    WRAP("glTranslated", glTranslated);
    WRAP("glScalef", glScalef);
    WRAP("glScaled", glScaled);
    WRAP("glRotatef", glRotatef);
    WRAP("glRotated", glRotated);
    WRAP("glOrtho", glOrtho);
    WRAP("glFrustum", glFrustum);
    WRAP("glColor3f", glColor3f); WRAP("glColor4f", glColor4f);
    WRAP("glColor3fv", glColor3fv); WRAP("glColor4fv", glColor4fv);
    WRAP("glColor3ub", glColor3ub); WRAP("glColor4ub", glColor4ub);
    WRAP("glColor3ubv", glColor3ubv); WRAP("glColor4ubv", glColor4ubv);
    WRAP("glColor3d", glColor3d); WRAP("glColor4d", glColor4d);
    WRAP("glColor3dv", glColor3dv); WRAP("glColor4dv", glColor4dv);
    WRAP("glNormal3f", glNormal3f); WRAP("glNormal3fv", glNormal3fv);
    WRAP("glNormal3d", glNormal3d); WRAP("glNormal3dv", glNormal3dv);
    WRAP("glTexCoord2f", glTexCoord2f); WRAP("glTexCoord2fv", glTexCoord2fv);
    WRAP("glTexCoord3f", glTexCoord3f); WRAP("glTexCoord3fv", glTexCoord3fv);
    WRAP("glTexCoord4f", glTexCoord4f); WRAP("glTexCoord4fv", glTexCoord4fv);
    WRAP("glMultiTexCoord2fARB", glMultiTexCoord2fARB);
    WRAP("glMultiTexCoord2f", glMultiTexCoord2f);
    WRAP("glClientActiveTexture", glClientActiveTexture);
    WRAP("glClientActiveTextureARB", glClientActiveTextureARB);
    WRAP("glSecondaryColor3f", glSecondaryColor3f);
    WRAP("glSecondaryColor3fv", glSecondaryColor3fv);
    WRAP("glSecondaryColor3ub", glSecondaryColor3ub);
    WRAP("glFogCoordfEXT", glFogCoordfEXT);
    WRAP("glFogCoordfvEXT", glFogCoordfvEXT);
    WRAP("glFogCoordf", glFogCoordf); WRAP("glFogCoordfv", glFogCoordfv);
    WRAP("glEnableClientState", glEnableClientState);
    WRAP("glDisableClientState", glDisableClientState);
    WRAP("glVertexPointer", glVertexPointer);
    WRAP("glNormalPointer", glNormalPointer);
    WRAP("glColorPointer", glColorPointer);
    WRAP("glTexCoordPointer", glTexCoordPointer);
    WRAP("glTexCoordPointerARB", glTexCoordPointerARB);
    WRAP("glSecondaryColorPointerEXT", glSecondaryColorPointerEXT);
    WRAP("glFogCoordPointerEXT", glFogCoordPointerEXT);
    WRAP("glSecondaryColorPointer", glSecondaryColorPointer);
    WRAP("glFogCoordPointer", glFogCoordPointer);
    WRAP("glLightf", glLightf); WRAP("glLightfv", glLightfv);
    WRAP("glLighti", glLighti); WRAP("glLightiv", glLightiv);
    WRAP("glMaterialf", glMaterialf); WRAP("glMaterialfv", glMaterialfv);
    WRAP("glMateriali", glMateriali); WRAP("glMaterialiv", glMaterialiv);
    WRAP("glLightModelf", glLightModelf); WRAP("glLightModelfv", glLightModelfv);
    WRAP("glLightModeli", glLightModeli); WRAP("glLightModeliv", glLightModeliv);
    WRAP("glFogf", glFogf); WRAP("glFogi", glFogi);
    WRAP("glFogfv", glFogfv); WRAP("glFogiv", glFogiv);
    WRAP("glClipPlane", glClipPlane); WRAP("glColorMaterial", glColorMaterial);
    WRAP("glTexEnvf", glTexEnvf); WRAP("glTexEnvfv", glTexEnvfv);
    WRAP("glTexEnvi", glTexEnvi); WRAP("glTexEnviv", glTexEnviv);
    WRAP("glGetLightfv", glGetLightfv); WRAP("glGetMaterialfv", glGetMaterialfv);
    WRAP("glGetClipPlane", glGetClipPlane);
    WRAP("glBegin", glBegin); WRAP("glEnd", glEnd);
    WRAP("glVertex2f", glVertex2f); WRAP("glVertex3f", glVertex3f);
    WRAP("glVertex4f", glVertex4f); WRAP("glVertex2fv", glVertex2fv);
    WRAP("glVertex3fv", glVertex3fv); WRAP("glVertex4fv", glVertex4fv);
    WRAP("glVertex2d", glVertex2d); WRAP("glVertex3d", glVertex3d);
    WRAP("glVertex2dv", glVertex2dv); WRAP("glVertex3dv", glVertex3dv);
    WRAP("glIsEnabledARB", glIsEnabledARB);
    /* H.4: desktop-only display-list functions. */
    WRAP("glGenLists", glGenLists); WRAP("glDeleteLists", glDeleteLists);
    WRAP("glIsList", glIsList); WRAP("glNewList", glNewList);
    WRAP("glEndList", glEndList); WRAP("glCallList", glCallList);
    WRAP("glCallLists", glCallLists); WRAP("glListBase", glListBase);
    /* Appendix D / spec 969-986: ARB/EXT aliases for advertised extensions. */
    WRAP("glBindFramebufferEXT", real_glBindFramebuffer);
    WRAP("glBindRenderbufferEXT", real_glBindRenderbuffer);
    WRAP("glGenFramebuffersEXT", real_glGenFramebuffers);
    WRAP("glGenRenderbuffersEXT", real_glGenRenderbuffers);
    WRAP("glDeleteFramebuffersEXT", real_glDeleteFramebuffers);
    WRAP("glDeleteRenderbuffersEXT", real_glDeleteRenderbuffers);
    WRAP("glFramebufferTexture2DEXT", real_glFramebufferTexture2D);
    WRAP("glFramebufferRenderbufferEXT", real_glFramebufferRenderbuffer);
    WRAP("glRenderbufferStorageEXT", real_glRenderbufferStorage);
    WRAP("glCheckFramebufferStatusEXT", real_glCheckFramebufferStatus);
    WRAP("glGenerateMipmapEXT", real_glGenerateMipmap);
    WRAP("glIsFramebufferEXT", real_glIsFramebuffer);
    WRAP("glIsRenderbufferEXT", real_glIsRenderbuffer);
    WRAP("glGetFramebufferAttachmentParameterivEXT", real_glGetFramebufferAttachmentParameteriv);
    WRAP("glGetRenderbufferParameterivEXT", real_glGetRenderbufferParameteriv);
    WRAP("glBlitFramebufferEXT", real_glBlitFramebuffer);
    WRAP("glRenderbufferStorageMultisampleEXT", real_glRenderbufferStorageMultisample);
    WRAP("glDrawBuffersARB", real_glDrawBuffers);
    WRAP("glVertexAttribDivisorARB", real_glVertexAttribDivisor);
    WRAP("glDrawArraysInstancedARB", real_glDrawArraysInstanced);
    WRAP("glDrawElementsInstancedARB", real_glDrawElementsInstanced);
    /* Spec 1458-1462: fallback BaseVertex -> EXT -> OES (mismo ABI). */
    WRAP("glDrawElementsBaseVertexEXT", real_glDrawElementsBaseVertex);
    WRAP("glDrawElementsBaseVertexOES", real_glDrawElementsBaseVertex);
#define GLF_LOOKUP(ret, fn, args) REAL(fn);
    GLFUNCS(GLF_LOOKUP)
    /* After GLFUNCS so the traced wrappers win over the real symbol table. */
    WRAP("glGenerateMipmap", shim_glGenerateMipmap);
    WRAP("glGenerateMipmapEXT", shim_glGenerateMipmap);
    WRAP("glViewport", shim_glViewport);
    WRAP("glScissor", shim_glScissor);
    WRAP("glBindFramebuffer", shim_glBindFramebuffer);
#undef GLF_LOOKUP
#undef REAL
#undef WRAP
    return NULL;
}

