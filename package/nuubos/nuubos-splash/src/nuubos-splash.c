#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BG_COLOR      0x00131514u
#define FG_COLOR      0x00E7E2D8u
#define SUB_COLOR     0x009EA69Fu
#define ACCENT_COLOR  0x005B86D6u

struct buffer {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t size;
    uint8_t *map;
};

static const char *glyph_rows(char c)
{
    switch (c) {
    case 'A': return " ### " "#   #" "#   #" "#####" "#   #" "#   #" "#   #";
    case 'B': return "#### " "#   #" "#   #" "#### " "#   #" "#   #" "#### ";
    case 'O': return " ### " "#   #" "#   #" "#   #" "#   #" "#   #" " ### ";
    case 'P': return "#### " "#   #" "#   #" "#### " "#    " "#    " "#    ";
    case 'R': return "#### " "#   #" "#   #" "#### " "#  # " "#   #" "#   #";
    case 'S': return " ####" "#    " "#    " " ### " "    #" "    #" "#### ";
    case 'a': return "     " "     " " ### " "    #" " ####" "#   #" " ####";
    case 'b': return "#    " "#    " "#### " "#   #" "#   #" "#   #" "#### ";
    case 'd': return "    #" "    #" " ####" "#   #" "#   #" "#   #" " ####";
    case 'e': return "     " "     " " ### " "#   #" "#####" "#    " " ####";
    case 'f': return "  ## " " #   " " ### " " #   " " #   " " #   " " #   ";
    case 'g': return "     " " ####" "#   #" "#   #" " ####" "    #" " ### ";
    case 'i': return "  #  " "     " " ##  " "  #  " "  #  " "  #  " " ### ";
    case 'l': return " ##  " "  #  " "  #  " "  #  " "  #  " "  #  " " ### ";
    case 'm': return "     " "     " "## ##" "# # #" "# # #" "#   #" "#   #";
    case 'n': return "     " "     " "#### " "#   #" "#   #" "#   #" "#   #";
    case 'o': return "     " "     " " ### " "#   #" "#   #" "#   #" " ### ";
    case 'p': return "     " "     " "#### " "#   #" "#   #" "#### " "#    ";
    case 'r': return "     " "     " "# ## " "##  #" "#    " "#    " "#    ";
    case 's': return "     " "     " " ####" "#    " " ### " "    #" "#### ";
    case 't': return " #   " " #   " "###  " " #   " " #   " " #  #" "  ## ";
    case 'u': return "     " "     " "#   #" "#   #" "#   #" "#   #" " ####";
    case 'v': return "     " "     " "#   #" "#   #" "#   #" " # # " "  #  ";
    case 'w': return "     " "     " "#   #" "#   #" "# # #" "# # #" " # # ";
    case 'y': return "     " "     " "#   #" "#   #" " ####" "    #" " ### ";
    case '.': return "     " "     " "     " "     " "     " " ##  " " ##  ";
    case ' ': return "     " "     " "     " "     " "     " "     " "     ";
    default: return "     " "     " "     " "     " "     " "     " "     ";
    }
}

static void put_pixel(struct buffer *buf, int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= buf->width || (uint32_t)y >= buf->height)
        return;

    ((uint32_t *)(buf->map + y * buf->stride))[x] = color;
}

static void fill_rect(struct buffer *buf, int x, int y, int w, int h, uint32_t color)
{
    int xx, yy;
    for (yy = 0; yy < h; yy++)
        for (xx = 0; xx < w; xx++)
            put_pixel(buf, x + xx, y + yy, color);
}

static void draw_glyph(struct buffer *buf, int x, int y, char c, int scale, uint32_t color)
{
    const char *rows = glyph_rows(c);
    int row, col, xx, yy;

    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            if (rows[row * 5 + col] == ' ')
                continue;

            for (yy = 0; yy < scale; yy++)
                for (xx = 0; xx < scale; xx++)
                    put_pixel(buf, x + col * scale + xx, y + row * scale + yy, color);
        }
    }
}

static int text_width(const char *text, int scale, int tracking)
{
    int n = (int)strlen(text);

    if (n <= 0)
        return 0;

    return n * 6 * scale - scale + (n - 1) * tracking;
}

static void draw_text(
    struct buffer *buf,
    int x,
    int y,
    const char *text,
    int scale,
    int tracking,
    uint32_t color)
{
    int cursor = x;

    while (*text) {
        draw_glyph(buf, cursor, y, *text, scale, color);
        cursor += 6 * scale + tracking;
        text++;
    }
}

static const char *subtitle_for_mode(const char *mode)
{
    if (mode && (!strcmp(mode, "reboot") || !strcmp(mode, "restart")))
        return "Reboot";

    if (mode && (!strcmp(mode, "poweroff") || !strcmp(mode, "shutdown")))
        return "Poweroff";

    return "Small System. Big Adventures.";
}

static void render_splash(struct buffer *buf, const char *mode)
{
    const char *brand_left = "nuub";
    const char *brand_right = "OS";
    const char *subtitle = subtitle_for_mode(mode);
    int title_scale = (int)buf->height / 68;
    int subtitle_scale;
    int brand_tracking;
    int left_w, right_w, title_w;
    int cx, cy, brand_y;
    int line_w, line_h, line_y, sub_y;
    int x;

    if (title_scale < 7)
        title_scale = 7;
    if (title_scale > 14)
        title_scale = 14;

    subtitle_scale = title_scale / 2;
    if (subtitle_scale < 3)
        subtitle_scale = 3;
    if (subtitle_scale > 6)
        subtitle_scale = 6;

    brand_tracking = title_scale / 2;
    if (brand_tracking < 2)
        brand_tracking = 2;

    fill_rect(buf, 0, 0, (int)buf->width, (int)buf->height, BG_COLOR);

    left_w = text_width(brand_left, title_scale, brand_tracking);
    right_w = text_width(brand_right, title_scale, brand_tracking);
    title_w = left_w + brand_tracking + right_w;

    cx = (int)buf->width / 2;
    cy = (int)buf->height / 2;
    brand_y = cy - title_scale * 12;

    x = cx - title_w / 2;
    draw_text(buf, x, brand_y, brand_left, title_scale, brand_tracking, FG_COLOR);
    x += left_w + brand_tracking;
    draw_text(buf, x, brand_y, brand_right, title_scale, brand_tracking, ACCENT_COLOR);

    line_w = title_w + title_scale * 8;
    if (line_w > (int)buf->width - title_scale * 10)
        line_w = (int)buf->width - title_scale * 10;

    line_h = title_scale / 2;
    if (line_h < 3)
        line_h = 3;

    line_y = brand_y + title_scale * 11;
    fill_rect(buf, cx - line_w / 2, line_y, line_w, line_h, ACCENT_COLOR);

    sub_y = line_y + title_scale * 5;
    draw_text(
        buf,
        cx - text_width(subtitle, subtitle_scale, 0) / 2,
        sub_y,
        subtitle,
        subtitle_scale,
        0,
        SUB_COLOR
    );
}

int main(int argc, char **argv)
{
    const char *mode = "boot";
    int prepare_only = 0;
    int fd;
    int i;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    struct buffer buf;
    size_t map_size;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare")) {
            prepare_only = 1;
            continue;
        }
        mode = argv[i];
    }

    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    memset(&buf, 0, sizeof(buf));

    fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("FBIOGET_FSCREENINFO");
        close(fd);
        return 1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("FBIOGET_VSCREENINFO");
        close(fd);
        return 1;
    }

    if (var.bits_per_pixel != 32) {
        fprintf(stderr, "unsupported fb0 bpp: %u\n", var.bits_per_pixel);
        close(fd);
        return 1;
    }

    buf.width = var.xres;
    buf.height = var.yres;
    buf.stride = fix.line_length;
    map_size = (size_t)buf.stride * (size_t)var.yres_virtual;
    buf.size = map_size;

    buf.map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf.map == MAP_FAILED) {
        perror("mmap fb0");
        close(fd);
        return 1;
    }

    if (!prepare_only)
        ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);

    render_splash(&buf, mode);

    if (msync(buf.map, map_size, MS_SYNC) < 0)
        perror("msync fb0");

    if (!prepare_only)
        ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);

    munmap(buf.map, map_size);
    close(fd);
    return 0;
}
