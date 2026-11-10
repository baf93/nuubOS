/*
 * nuubOS CRT, written for nuubOS (GPL-3.0-or-later, LICENSING.md).
 *
 * A very light CRT look for systems made for TV screens, on top of the
 * pixel perfect (integer scaled) picture: neighbouring pixels melt into
 * each other horizontally the way a composite/RF signal blurred them, and
 * every source line becomes a beam whose height grows with its brightness
 * (dark gaps between dark lines, bright lines blooming). A TV traced at
 * most ~288 lines a field, so taller sources (a core's 2x internal
 * resolution such as PS1 enhancement, 480i/576i) get one beam per 2 rows
 * (3 above 576) while the colour keeps every row. Below 2x vertical scale
 * of those beam lines there is no room for scanlines and only the
 * horizontal blend is applied. Single pass, two texture fetches.
 *
 * Mali-G31 (one shader core): this pass is most of the GPU time of a 2D
 * game (SNES/PS1 at 720x720: 16 % of the GPU without it, 39 % with it).
 * Everything that is the same for the whole frame is worked out once per
 * vertex, and the colour and beam maths run at mediump (16 bit, twice the
 * rate on Bifrost); texel coordinates stay highp (up to 1024 texels).
 */

#pragma parameter CRT_SCANLINES "Scanline strength" 0.25 0.0 1.0 0.05
#pragma parameter CRT_BLEND "Horizontal blend" 0.30 0.0 0.5 0.05

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

#ifdef GL_ES
precision highp float;
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec2 vTexel;
/* Per frame: 1 / beam rows per line, output rows per beam line, its
 * reciprocal, 1 = whole scale. */
COMPAT_VARYING vec4 vScale;
/* Per frame: beam centre, scanline strength, light gain. */
COMPAT_VARYING vec3 vBeam;

uniform mat4 MVPMatrix;
uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;

#ifdef PARAMETER_UNIFORM
uniform float CRT_SCANLINES;
#else
#define CRT_SCANLINES 0.25
#endif

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vTexel = TexCoord.xy * TextureSize;
    /* Source rows per beam line: 1 up to 288 lines, 2 up to 576, ... (PS1
     * at 2x internal resolution lost its scanlines, user report
     * 2026-10-18). */
    float k = floor((InputSize.y - 1.0) / 288.0) + 1.0;
    float s = OutputSize.y * k / InputSize.y;
    /* A fractional vertical scale (4:3 on a square screen) gives lines 2
     * or 3 rows: sample the pixel's own position. */
    float whole = abs(s - floor(s + 0.5)) < 0.01 ? 1.0 : 0.0;
    /* Light scanlines everywhere (user request 2026-10-11: 0.55 read as
     * dark lines, on N64 and on fractional scales most). 1.99: at exactly
     * 2x (480 source rows on 480 lines, HDMI) the GPU's s could come out a
     * hair below 2.0 and the scanlines vanished. */
    float strength = s >= 1.99 ? CRT_SCANLINES : 0.0;
    vScale = vec4(1.0 / k, s, 1.0 / s, whole);
    /* Beam: centred half an output row above the pixel centre, so at 2x
     * the first row is the beam and the second the gap. Gain back the
     * light the gaps take: at 2x a gap row keeps ~0.5, so mid tones
     * average the original brightness (user report 2026-10-09: the CRT
     * look darkened the picture; was 0.30). */
    vBeam = vec3(whole > 0.5 ? 0.5 - 0.5 / s : 0.5, strength, 1.0 + 0.55 * strength);
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

uniform vec2 TextureSize;
uniform sampler2D Texture;
COMPAT_VARYING vec2 vTexel;
COMPAT_VARYING vec4 vScale;
COMPAT_VARYING vec3 vBeam;

#ifdef PARAMETER_UNIFORM
uniform float CRT_BLEND;
#else
#define CRT_BLEND 0.30
#endif

void main()
{
    vec2 base = floor(vTexel);
    vec2 f = vTexel - base;
    vec2 inv = 1.0 / TextureSize;
    mediump float fl = fract(vTexel.y * vScale.x);

    /* Horizontal melt: towards the neighbour on the near side, only in the
     * outer CRT_BLEND of the pixel, at most half way. */
    mediump float tx = f.x - 0.5;
    float side = tx < 0.0 ? -1.0 : 1.0;
    mediump float melt = 0.5 * smoothstep(0.5 - CRT_BLEND, 0.5, abs(tx));
    mediump vec3 c0 = COMPAT_TEXTURE(Texture, (base + 0.5) * inv).rgb;
    mediump vec3 c1 = COMPAT_TEXTURE(Texture, (base + vec2(side + 0.5, 0.5)) * inv).rgb;
    mediump vec3 col = mix(c0, c1, melt);

    mediump float row = vScale.w > 0.5 ? (floor(fl * vScale.y) + 0.5) * vScale.z : fl;
    mediump float dy = row - vBeam.x;
    mediump float lum = dot(col, vec3(0.299, 0.587, 0.114));
    mediump float width = mix(0.20, 0.34, lum);
    mediump float beam = exp(-(dy * dy) / (2.0 * width * width));
    mediump float scan = 1.0 - vBeam.y * (1.0 - beam);

    col *= scan * vBeam.z;
    FragColor = vec4(min(col, vec3(1.0)), 1.0);
}

#endif
