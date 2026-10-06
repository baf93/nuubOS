/*
 * nuubOS sharp bilinear (MIT).
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
COMPAT_VARYING vec2 vTexCoord;

uniform mat4 MVPMatrix;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vTexCoord = TexCoord.xy;
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

uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec2 vTexCoord;

void main()
{
    vec2 texel = vTexCoord * TextureSize;
    /* Integer part of the scale: those pixels stay hard edged. */
    vec2 scale = max(floor(OutputSize / InputSize), vec2(1.0));
    vec2 edge = 0.5 - 0.5 / scale;
    vec2 offset = fract(texel) - 0.5;
    /* Inside a pixel sample its centre; near its border interpolate
     * towards the neighbour over one output pixel. */
    vec2 blend = (offset - clamp(offset, -edge, edge)) * scale + 0.5;

    FragColor = vec4(COMPAT_TEXTURE(Texture,
                     (floor(texel) + blend) / TextureSize).rgb, 1.0);
}

#endif
