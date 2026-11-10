/*
 * nuubOS sharp bilinear (GPL-3.0-or-later, LICENSING.md).
 *
 * Scales by the largest integer factor that fits with nearest-neighbour
 * sharpness and blends only across the pixel edges left by the remaining
 * non-integer factor, so low resolution content fills 640x480, 720x720 or
 * an HDMI output without uneven pixels or a blurry image. Single pass,
 * one texture fetch: negligible on Mali-G31.
 */

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
COMPAT_VARYING vec2 vTexel;
/* Per frame: integer scale, its hard-edged half width. */
COMPAT_VARYING vec4 vScale;

uniform mat4 MVPMatrix;
uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vTexel = TexCoord.xy * TextureSize;
    /* Integer part of the scale: those pixels stay hard edged. */
    vec2 scale = max(floor(OutputSize / InputSize), vec2(1.0));
    vScale = vec4(scale, 0.5 - 0.5 / scale);
}

#elif defined(FRAGMENT)

#ifdef GL_ES
/* Texel coordinates of a 1024 px texture need more than mediump. */
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

void main()
{
    vec2 offset = fract(vTexel) - 0.5;
    /* Inside a pixel sample its centre; near its border interpolate
     * towards the neighbour over one output pixel. */
    vec2 blend = (offset - clamp(offset, -vScale.zw, vScale.zw)) * vScale.xy + 0.5;

    FragColor = vec4(COMPAT_TEXTURE(Texture,
                     (floor(vTexel) + blend) * (1.0 / TextureSize)).rgb, 1.0);
}

#endif
