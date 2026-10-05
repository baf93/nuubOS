//! nuubOS lifecycle splash, rendered procedurally at the exact physical
//! output size.
//!
//! This MUST stay pixel-identical to the two other splash renderers so the
//! kernel → compositor → UI → shutdown handoff never jumps:
//!   - package/nuubos/nuubos-splash/src/nuubos-splash.c (fbdev)
//!   - package/nuubos/labwc/0001-nuubos-root-atomic-first-frame.patch
//! The same file is duplicated in nuubos-quick-menu/src/src/splash.rs.

use slint::{Image, Rgba8Pixel, SharedPixelBuffer};

const BG: [u8; 3] = [0x13, 0x15, 0x14];
const FG: [u8; 3] = [0xE7, 0xE2, 0xD8];
const SUB: [u8; 3] = [0x9E, 0xA6, 0x9F];
const ACCENT: [u8; 3] = [0x5B, 0x86, 0xD6];

fn glyph_rows(c: char) -> &'static str {
    match c {
        'A' => " ### #   ##   #######   ##   ##   #",
        'B' => "#### #   ##   ##### #   ##   ##### ",
        'O' => " ### #   ##   ##   ##   ##   # ### ",
        'P' => "#### #   ##   ##### #    #    #    ",
        'R' => "#### #   ##   ##### #  # #   ##   #",
        'S' => " #####    #     ###     #    ##### ",
        'a' => "           ###     # #####   # ####",
        'b' => "#    #    #### #   ##   ##   ##### ",
        'd' => "    #    # #####   ##   ##   # ####",
        'e' => "           ### #   #######     ####",
        'f' => "  ##  #    ###  #    #    #    #   ",
        'g' => "      #####   ##   # ####    # ### ",
        'i' => "  #        ##    #    #    #   ### ",
        'l' => " ##    #    #    #    #    #   ### ",
        'm' => "          ## ### # ## # ##   ##   #",
        'n' => "          #### #   ##   ##   ##   #",
        'o' => "           ### #   ##   ##   # ### ",
        'p' => "          #### #   ##   ##### #    ",
        'r' => "          # ## ##  ##    #    #    ",
        's' => "           #####     ###     ##### ",
        't' => " #    #   ###   #    #    #  #  ## ",
        'u' => "          #   ##   ##   ##   # ####",
        'v' => "          #   ##   ##   # # #   #  ",
        'w' => "          #   ##   ## # ## # # # # ",
        'y' => "          #   ##   # ####    # ### ",
        '.' => "                          ##   ##  ",
        ' ' => "                                   ",
        _ => "                                   ",
    }
}

struct Canvas {
    buffer: SharedPixelBuffer<Rgba8Pixel>,
    width: i32,
    height: i32,
}

impl Canvas {
    fn fill_rect(&mut self, x: i32, y: i32, w: i32, h: i32, c: [u8; 3]) {
        let x0 = x.max(0);
        let y0 = y.max(0);
        let x1 = (x + w).min(self.width);
        let y1 = (y + h).min(self.height);
        if x0 >= x1 || y0 >= y1 {
            return;
        }
        let stride = self.width as usize;
        let pixels = self.buffer.make_mut_slice();
        let px = Rgba8Pixel { r: c[0], g: c[1], b: c[2], a: 255 };
        for yy in y0..y1 {
            let row = yy as usize * stride;
            pixels[row + x0 as usize..row + x1 as usize].fill(px);
        }
    }

    fn glyph(&mut self, x: i32, y: i32, ch: char, scale: i32, c: [u8; 3]) {
        let rows = glyph_rows(ch).as_bytes();
        for row in 0..7 {
            for col in 0..5 {
                if rows[(row * 5 + col) as usize] != b' ' {
                    self.fill_rect(x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
    }

    fn text(&mut self, x: i32, y: i32, text: &str, scale: i32, tracking: i32, c: [u8; 3]) {
        let mut cursor = x;
        for ch in text.chars() {
            self.glyph(cursor, y, ch, scale, c);
            cursor += 6 * scale + tracking;
        }
    }
}

fn text_width(text: &str, scale: i32, tracking: i32) -> i32 {
    let n = text.chars().count() as i32;
    if n <= 0 {
        return 0;
    }
    n * 6 * scale - scale + (n - 1) * tracking
}

fn subtitle_for_mode(mode: &str) -> &'static str {
    match mode {
        "reboot" | "restart" => "Reboot",
        "poweroff" | "shutdown" => "Poweroff",
        _ => "Small System. Big Adventures.",
    }
}

/// Render the splash for `mode` ("boot", "reboot", "poweroff") at
/// `width` x `height` physical pixels.
pub fn render(mode: &str, width: u32, height: u32) -> Image {
    let width = width.max(1);
    let height = height.max(1);
    let mut canvas = Canvas {
        buffer: SharedPixelBuffer::new(width, height),
        width: width as i32,
        height: height as i32,
    };

    let title_scale = (canvas.height / 68).clamp(7, 14);
    let subtitle_scale = (title_scale / 2).clamp(3, 6);
    let tracking = (title_scale / 2).max(2);

    canvas.fill_rect(0, 0, canvas.width, canvas.height, BG);

    let left_w = text_width("nuub", title_scale, tracking);
    let right_w = text_width("OS", title_scale, tracking);
    let title_w = left_w + tracking + right_w;
    let cx = canvas.width / 2;
    let cy = canvas.height / 2;
    let brand_y = cy - title_scale * 12;

    let mut x = cx - title_w / 2;
    canvas.text(x, brand_y, "nuub", title_scale, tracking, FG);
    x += left_w + tracking;
    canvas.text(x, brand_y, "OS", title_scale, tracking, ACCENT);

    let line_w = (title_w + title_scale * 8).min(canvas.width - title_scale * 10);
    let line_h = (title_scale / 2).max(3);
    let line_y = brand_y + title_scale * 11;
    canvas.fill_rect(cx - line_w / 2, line_y, line_w, line_h, ACCENT);

    let subtitle = subtitle_for_mode(mode);
    let sub_y = line_y + title_scale * 5;
    canvas.text(
        cx - text_width(subtitle, subtitle_scale, 0) / 2,
        sub_y,
        subtitle,
        subtitle_scale,
        0,
        SUB,
    );

    Image::from_rgba8(canvas.buffer)
}

/// Curtain geometry matching the splash accent line: (split fraction of
/// the height, line width fraction of the width, line height in physical px).
pub fn geometry(width: u32, height: u32) -> (f32, f32, u32) {
    let w = width.max(1) as i32;
    let h = height.max(1) as i32;
    let title_scale = (h / 68).clamp(7, 14);
    let tracking = (title_scale / 2).max(2);
    let title_w = text_width("nuub", title_scale, tracking) + tracking
        + text_width("OS", title_scale, tracking);
    let line_w = (title_w + title_scale * 8).min(w - title_scale * 10);
    let line_h = (title_scale / 2).max(3);
    let line_y = h / 2 - title_scale * 12 + title_scale * 11;
    (line_y as f32 / h as f32, line_w as f32 / w as f32, line_h as u32)
}
