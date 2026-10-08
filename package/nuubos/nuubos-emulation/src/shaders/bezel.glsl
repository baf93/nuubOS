/*
 * nuubOS bezel (MIT), written for nuubOS.
 *
 * Draws the game pixel perfect (largest integer scale, square pixels,
 * centred horizontally, at the top of the screen) inside a procedural
 * bezel that fills the rest of the screen: no artwork, so any resolution
 * and any core geometry work. RetroArch's viewport is the whole screen
 * (aspect "full", no integer scaling) and this pass does the placement.
 *
 * BEZEL_R/G/B is the system's colour (systems.conf), BEZEL_STYLE 0 a TV
 * console (dark bezel around the picture tube) and 1 a handheld (the
 * console's plastic body around its screen); CRT 1 adds the nuubOS CRT
 * look (crt.glsl) to the picture. Single pass, two texture fetches.
 */

#pragma parameter BEZEL_R "Bezel red" 0.40 0.0 1.0 0.01
#pragma parameter BEZEL_G "Bezel green" 0.50 0.0 1.0 0.01
#pragma parameter BEZEL_B "Bezel blue" 0.90 0.0 1.0 0.01
#pragma parameter BEZEL_STYLE "Bezel style (0 TV, 1 handheld)" 0.0 0.0 1.0 1.0
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
COMPAT_VARYING vec2 vTexCoord;

uniform mat4 MVPMatrix;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vTexCoord = TexCoord.xy;
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
uniform sampler2D Texture;
COMPAT_VARYING vec2 vTexCoord;

#ifdef PARAMETER_UNIFORM
uniform float BEZEL_R;
uniform float BEZEL_G;
uniform float BEZEL_B;
uniform float BEZEL_STYLE;
uniform float CRT;
#else
#define BEZEL_R 0.40
#define BEZEL_G 0.50
#define BEZEL_B 0.90
#define BEZEL_STYLE 0.0
#define CRT 0.0
#endif

/* Signed distance to a rounded box of half size b. */
float round_box(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

/* The picture at source position src (texels from its top-left), with the
 * CRT look when enabled; s = integer scale. Texture rows count from the
 * bottom of the picture in this pass. */
vec3 picture(vec2 src, float s)
{
    vec2 base = floor(src);
    vec2 f = src - base;
    vec2 tex = vec2(base.x, InputSize.y - 1.0 - base.y);
    vec3 c0 = COMPAT_TEXTURE(Texture, (tex + 0.5) / TextureSize).rgb;
    if (CRT < 0.5)
        return c0;

    float tx = f.x - 0.5;
    float side = tx < 0.0 ? -1.0 : 1.0;
    float melt = 0.5 * smoothstep(0.2, 0.5, abs(tx));
    vec3 c1 = COMPAT_TEXTURE(Texture, (tex + vec2(side, 0.0) + 0.5) / TextureSize).rgb;
    vec3 col = mix(c0, c1, melt);
    float centre = 0.5 - 0.5 / s;
    float row = (floor(f.y * s) + 0.5) / s;
    float dy = row - centre;
    float width = mix(0.20, 0.34, dot(col, vec3(0.299, 0.587, 0.114)));
    float strength = s >= 2.0 ? 0.55 : 0.0;
    float scan = 1.0 - strength * (1.0 - exp(-(dy * dy) / (2.0 * width * width)));
    return min(col * scan * (1.0 + 0.30 * strength), vec3(1.0));
}

void main()
{
    /* Output pixel, origin top-left (the final pass has y up). */
    vec2 p = vTexCoord * TextureSize / InputSize * OutputSize;
    p.y = OutputSize.y - p.y;
    vec2 fit = floor(OutputSize / InputSize);
    float s = max(min(fit.x, fit.y), 1.0);
    /* Content that would stay at 1x with a large margin (PSP 480x272 or NDS
     * on 640x480) is scaled to fit inside the frame instead (same rule as
     * RetroArch patch 0003): unscaled it is just too small. */
    float fill = max(InputSize.x * s / OutputSize.x, InputSize.y * s / OutputSize.y);
    if (s < 2.0 && fill < 0.88) {
        float k = OutputSize.y / 480.0;
        vec2 room = vec2(OutputSize.x - 44.0 * k, OutputSize.y - 22.0 * k) / InputSize;
        s = max(min(room.x, room.y), 1.0);
    }
    vec2 size = floor(InputSize * s);
    vec2 origin = vec2(floor((OutputSize.x - size.x) * 0.5), 0.0);
    vec2 local = p - origin;

    if (local.x >= 0.0 && local.y >= 0.0 && local.x < size.x && local.y < size.y) {
        FragColor = vec4(picture(local / s, s), 1.0);
        return;
    }

    /* Bezel. u = one 480p pixel, so every screen gets the same look. */
    float u = OutputSize.y / 480.0;
    vec3 accent = vec3(BEZEL_R, BEZEL_G, BEZEL_B);
    bool handheld = BEZEL_STYLE > 0.5;
    vec2 half_size = size * 0.5;
    vec2 centre = origin + half_size;

    /* Background: matte, tinted by the system colour, soft vignette. */
    vec2 uv = p / OutputSize - 0.5;
    vec3 bg = mix(vec3(0.045, 0.050, 0.062), accent, 0.10);
    bg *= 1.0 - 0.55 * dot(uv, uv);

    /* Frame around the picture. */
    float margin = (handheld ? 14.0 : 8.0) * u;
    float radius = (handheld ? 16.0 : 9.0) * u;
    float d = round_box(p - centre, half_size + margin, radius);
    vec3 body = handheld ? mix(vec3(0.20, 0.21, 0.23), accent, 0.38)
                         : mix(vec3(0.085, 0.090, 0.105), accent, 0.12);
    /* Light from above: the upper part of the frame is a little brighter. */
    body *= 1.0 + 0.18 * (0.5 - (p.y - origin.y) / (size.y + margin));
    float shadow = 1.0 - 0.6 * exp(-max(d, 0.0) / (10.0 * u));
    vec3 col = mix(body, bg * shadow, smoothstep(-0.75, 0.75, d));

    /* Thin accent line where the picture meets the frame. */
    float edge = round_box(p - centre, half_size, 1.0 * u);
    float line = 1.0 - smoothstep(0.6 * u, 1.8 * u, edge);
    col = mix(col, accent * (handheld ? 0.55 : 0.75), line * 0.85);

    FragColor = vec4(col, 1.0);
}

#endif
