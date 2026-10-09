/*
 * nuubOS bezel (MIT), written for nuubOS.
 *
 * The game keeps exactly the size and place it has without bezels (the
 * same rule as RetroArch's smart integer scaling, axis Y, with the core's
 * aspect: integer vertical scale, fit to the screen when that would leave
 * more than 12 % unused, centred horizontally, at the top); the bezel only
 * fills the black around it. RetroArch's viewport is the whole screen
 * (aspect "full", no integer scaling) and this pass does the placement.
 *
 * Look: the nuubOS matte surface (splash colours) tinted with the system
 * colour, a thin lip around the picture and the nuubOS blue accent line
 * under it. Side panels wide enough show the platform's silhouette
 * (SYSICON, the system icon of the library, CC BY 4.0) on the left and
 * the nuubOS face-button dots and wordmark (BRAND) on the right; a bottom
 * strip shows both small at its ends. BEZEL_STYLE 1 (handhelds) turns the
 * surface into the console's plastic body.
 *
 * BEZEL_R/G/B is the system colour (systems.conf), HAS_ICON 0 when the
 * system has no icon; CRT 1 adds the nuubOS CRT look (crt.glsl) to the
 * picture. Single pass; the picture costs one texture fetch (up to four
 * where sharp bilinear blends a non-integer scale, two with CRT) and a
 * bezel pixel at most one: cheap at any output size, HDMI included.
 */

#pragma parameter BEZEL_R "Bezel red" 0.40 0.0 1.0 0.01
#pragma parameter BEZEL_G "Bezel green" 0.50 0.0 1.0 0.01
#pragma parameter BEZEL_B "Bezel blue" 0.90 0.0 1.0 0.01
#pragma parameter BEZEL_STYLE "Bezel style (0 TV, 1 handheld)" 0.0 0.0 1.0 1.0
#pragma parameter HAS_ICON "System icon available" 0.0 0.0 1.0 1.0
#pragma parameter CRT "CRT look" 0.0 0.0 1.0 1.0

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec2 vScreen;
COMPAT_VARYING float vRowsDown;

uniform mat4 MVPMatrix;
uniform vec2 TextureSize;
uniform vec2 InputSize;

/* RetroArch hands a hardware rendered core's frame (PSP, N64) bottom row
 * first and a software one (GBA) top row first, flipping the texture
 * coordinates to match: placement uses the screen position (clip space,
 * the same for every core) and the corners tell which way rows run. */
void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vScreen = vec2(gl_Position.x + 1.0, 1.0 - gl_Position.y) * 0.5;
    bool tex_low = TexCoord.y * TextureSize.y < 0.5 * InputSize.y;
    vRowsDown = (tex_low == (vScreen.y < 0.5)) ? 1.0 : 0.0;
}

#elif defined(FRAGMENT)

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#endif

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform float OriginalAspect;
uniform sampler2D Texture;
uniform sampler2D SYSICON;
uniform sampler2D BRAND;
COMPAT_VARYING vec2 vScreen;
COMPAT_VARYING float vRowsDown;

#ifdef PARAMETER_UNIFORM
uniform float BEZEL_R;
uniform float BEZEL_G;
uniform float BEZEL_B;
uniform float BEZEL_STYLE;
uniform float HAS_ICON;
uniform float CRT;
#else
#define BEZEL_R 0.40
#define BEZEL_G 0.50
#define BEZEL_B 0.90
#define BEZEL_STYLE 0.0
#define HAS_ICON 0.0
#define CRT 0.0
#endif

/* nuubOS identity (splash.rs): matte background and the blue accent. */
const vec3 NUUB_BG = vec3(0.075, 0.082, 0.078);
const vec3 NUUB_ACCENT = vec3(0.357, 0.525, 0.839);
const vec2 BRAND_SIZE = vec2(73.0, 14.0);

/* Signed distance to a rounded box of half size b. */
float round_box(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

/* Texel t of the picture (origin top-left, whichever way rows run). */
vec3 texel(vec2 t)
{
    t = clamp(t, vec2(0.0), InputSize - 1.0);
    if (vRowsDown < 0.5)
        t.y = InputSize.y - 1.0 - t.y;
    return COMPAT_TEXTURE(Texture, (t + 0.5) / TextureSize).rgb;
}

/* The picture at source position src (texels from its top-left), scaled
 * by sc per axis: sharp bilinear (hard pixels, a one output pixel blend
 * only where a non-integer scale would make them uneven), or the CRT look
 * when enabled. */
vec3 picture(vec2 src, vec2 sc)
{
    vec2 base = floor(src);
    vec2 f = src - base;
    if (CRT < 0.5) {
        vec2 k = max(floor(sc + 0.01), vec2(1.0));
        if (abs(k.x - sc.x) < 0.01 && abs(k.y - sc.y) < 0.01)
            return texel(base);
        vec2 off = f - 0.5;
        vec2 edge = 0.5 - 0.5 / k;
        vec2 b = (off - clamp(off, -edge, edge)) * k;
        vec2 dir = sign(b);
        vec2 w = abs(b);
        vec3 top = mix(texel(base), texel(base + vec2(dir.x, 0.0)), w.x);
        vec3 bottom = mix(texel(base + vec2(0.0, dir.y)), texel(base + dir), w.x);
        return mix(top, bottom, w.y);
    }

    float s = sc.y;
    float tx = f.x - 0.5;
    float side = tx < 0.0 ? -1.0 : 1.0;
    float melt = 0.5 * smoothstep(0.2, 0.5, abs(tx));
    vec3 col = mix(texel(base), texel(base + vec2(side, 0.0)), melt);
    float centre = 0.5 - 0.5 / s;
    float row = (floor(f.y * s) + 0.5) / s;
    float dy = row - centre;
    float width = mix(0.20, 0.34, dot(col, vec3(0.299, 0.587, 0.114)));
    float strength = s >= 2.0 ? 0.55 : 0.0;
    float scan = 1.0 - strength * (1.0 - exp(-(dy * dy) / (2.0 * width * width)));
    return min(col * scan * (1.0 + 0.30 * strength), vec3(1.0));
}

/* Picture rectangle (xy origin, zw size) exactly as RetroArch places it
 * without bezels: video_viewport_get_scaled_integer, smart mode, axis Y,
 * plus nuubOS patch 0003 (overscan cropped evenly) and bias x 0.5, y 0. */
vec4 placement(vec2 screen)
{
    float aspect = OriginalAspect > 0.0 ? OriginalAspect : InputSize.x / InputSize.y;
    vec2 one = vec2(floor(InputSize.y * aspect + 0.5), InputSize.y);
    vec2 size;
    if (one.x > screen.x || one.y > screen.y) {
        size = vec2(0.0);
    } else {
        float under = floor(screen.y / one.y);
        float min_h = one.y - 6.0;
        if (one.y >= 192.0 && one.y <= 240.0)
            min_h = 192.0;
        else if (one.y >= 384.0 && one.y <= 480.0)
            min_h = 384.0;
        float sh = floor(screen.y / (under + 1.0)) >= min_h ? under + 1.0 : under;
        float s = min(floor(screen.x / one.x), sh);
        size = one * s;
        vec2 use = size / screen;
        if (max(use.x, use.y) < 0.88)
            size = vec2(0.0);
    }
    if (size.x == 0.0) {
        /* Fit with the aspect (video_viewport_get_scaled_aspect). */
        size = screen.x / screen.y > aspect
            ? vec2(floor(screen.y * aspect + 0.5), screen.y)
            : vec2(screen.x, floor(screen.x / aspect + 0.5));
    }
    vec2 origin = vec2(floor((screen.x - size.x) * 0.5),
                       size.y > screen.y ? floor((screen.y - size.y) * 0.5) : 0.0);
    return vec4(origin, size);
}

/* Coverage (0..1) of an image of size box drawn at origin o, by its alpha. */
float stamp(sampler2D tex, vec2 p, vec2 o, vec2 box)
{
    vec2 uv = (p - o) / box;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
        return 0.0;
    return COMPAT_TEXTURE(tex, uv).a;
}

/* Wordmark at integer scale k (pixel font: crisp), colours of its own. */
vec4 wordmark(vec2 p, vec2 o, float k)
{
    vec2 box = BRAND_SIZE * k;
    vec2 uv = (p - o) / box;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0)
        return vec4(0.0);
    return COMPAT_TEXTURE(BRAND, (floor(uv * BRAND_SIZE) + 0.5) / BRAND_SIZE);
}

/* The four nuubOS face-button dots (the hint glyph), south one filled. */
float dots(vec2 p, vec2 c, float r, float aa, out float filled)
{
    float g = r * 2.6;
    vec2 q = p - c;
    float best = 1e4;
    float south = length(q - vec2(0.0, g));
    best = min(best, abs(length(q - vec2(0.0, -g)) - r * 0.78));
    best = min(best, abs(length(q - vec2(-g, 0.0)) - r * 0.78));
    best = min(best, abs(length(q - vec2(g, 0.0)) - r * 0.78));
    filled = 1.0 - smoothstep(r - aa, r + aa, south);
    return 1.0 - smoothstep(r * 0.22 - aa, r * 0.22 + aa, best);
}

void main()
{
    /* Output pixel, origin top-left. */
    vec2 p = vScreen * OutputSize;
    vec4 pic = placement(OutputSize);
    vec2 origin = pic.xy;
    vec2 size = pic.zw;
    vec2 local = p - origin;

    if (local.x >= 0.0 && local.y >= 0.0 && local.x < size.x && local.y < size.y) {
        FragColor = vec4(picture(local / size * InputSize, size / InputSize), 1.0);
        return;
    }

    /* Bezel. u = one 480p pixel, so every screen gets the same look. */
    float u = OutputSize.y / 480.0;
    float aa = 0.75;
    vec3 sys = vec3(BEZEL_R, BEZEL_G, BEZEL_B);
    bool handheld = BEZEL_STYLE > 0.5;
    vec2 centre = origin + size * 0.5;
    float left = max(origin.x, 0.0);
    float right = max(OutputSize.x - origin.x - size.x, 0.0);
    float bottom = max(OutputSize.y - origin.y - size.y, 0.0);

    /* Surface: nuubOS matte tinted by the system (TV), or the console's
     * plastic body (handheld); light from above, soft vignette. */
    vec2 uv = p / OutputSize - 0.5;
    vec3 col = handheld ? mix(vec3(0.17, 0.18, 0.19), sys, 0.34)
                        : mix(NUUB_BG, sys, 0.14);
    col *= 1.06 - 0.16 * (p.y / OutputSize.y) - 0.35 * dot(uv, uv);

    /* Lip around the picture: a darker recess, softly shadowed. */
    float d = round_box(p - centre, size * 0.5, 2.0 * u);
    float lip = (handheld ? 5.0 : 3.0) * u;
    col = mix(col * 0.45, col, smoothstep(lip, lip + 6.0 * u, d));
    col = mix(col, vec3(0.02), 1.0 - smoothstep(0.0, lip, d));

    /* nuubOS accent line under the picture (the splash line). */
    float under = p.y - (origin.y + size.y);
    if (bottom >= 6.0 * u && under >= 0.0) {
        float line = 1.0 - smoothstep(1.5 * u - aa, 1.5 * u + aa, abs(under - 3.0 * u));
        float span = 1.0 - smoothstep(size.x * 0.5 - 1.0, size.x * 0.5 + 1.0, abs(p.x - centre.x));
        col = mix(col, NUUB_ACCENT, line * span);
    }

    vec3 ink = mix(sys, vec3(1.0), 0.30);
    float panel = min(left, right);
    bool sides = panel >= 56.0 * u;
    bool strip = bottom >= 26.0 * u;
    float mid = origin.y + size.y * 0.5;
    if (sides && p.y < origin.y + size.y) {
        /* Side panels: platform silhouette left, nuubOS dots right (and
         * the wordmark when there is no bottom strip and it fits). */
        if (p.x < left) {
            float box = min(panel * 0.86, size.y * 0.62);
            if (HAS_ICON > 0.5) {
                vec2 o = vec2((left - box) * 0.5, mid - box * 0.5);
                col = mix(col, ink, 0.92 * stamp(SYSICON, p, o, vec2(box)));
            }
        } else {
            float x0 = OutputSize.x - right;
            float filled;
            float r = min(right * 0.085, 10.0 * u);
            float ring = dots(p, vec2(x0 + right * 0.5, mid), r, aa, filled);
            col = mix(col, ink * 0.85, ring);
            col = mix(col, NUUB_ACCENT, filled);
            float k = floor((right - 24.0 * u) / BRAND_SIZE.x);
            if (!strip && k >= 1.0) {
                vec2 wsize = BRAND_SIZE * k;
                vec4 w = wordmark(p, vec2(x0 + floor((right - wsize.x) * 0.5),
                                          origin.y + size.y - wsize.y - 12.0 * u), k);
                col = mix(col, w.rgb, w.a);
            }
        }
    } else if (strip && p.y > origin.y + size.y) {
        /* Bottom strip: silhouette at the left end (unless the side panel
         * already shows it), wordmark at the right end. */
        float y0 = origin.y + size.y + 6.0 * u;
        float h = bottom - 12.0 * u;
        if (HAS_ICON > 0.5 && !sides) {
            float box = min(h, 64.0 * u);
            vec2 o = vec2(left + 10.0 * u, y0 + (h - box) * 0.5);
            col = mix(col, ink, 0.92 * stamp(SYSICON, p, o, vec2(box)));
        }
        float k = max(floor(min(h * 0.42, 28.0 * u) / BRAND_SIZE.y), 1.0);
        vec2 wsize = BRAND_SIZE * k;
        vec4 w = wordmark(p, vec2(OutputSize.x - right - 10.0 * u - wsize.x,
                                  y0 + floor((h - wsize.y) * 0.5)), k);
        col = mix(col, w.rgb, w.a);
    }

    FragColor = vec4(col, 1.0);
}

#endif
