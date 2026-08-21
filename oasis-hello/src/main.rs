//! oasis-hello — a tiny, deliberately unfancy "hello world" for the Kindle
//! Oasis. Renders a hand-rolled 5x7 bitmap font into a full-panel PNG and
//! (by default) hands it to the device's own `eips` utility for display.
//!
//! Fun-Friday scope: no font library, no TTF, no framebuffer ioctl juggling.
//! Just prove pixels-in-a-PNG can look clean on this screen.

use image::{GrayImage, Luma};
use std::process::Command;

// Kindle Oasis 1st-gen panel, per the jailbroken-device wiki page.
const PANEL_W: u32 = 1072;
const PANEL_H: u32 = 1448;

const BLACK: u8 = 0;
const WHITE: u8 = 255;

/// 7 rows of 5 columns each, '#' = ink, ' ' = blank. Hand-drawn, blocky on
/// purpose — this is a vibe-coded demo, not a type foundry.
type Glyph = [&'static str; 7];

fn glyph(c: char) -> Glyph {
    match c {
        'A' => [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
        'B' => ["#### ", "#   #", "#### ", "#   #", "#   #", "#   #", "#### "],
        'C' => [" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "],
        'D' => ["#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "],
        'E' => ["#####", "#    ", "#### ", "#    ", "#    ", "#    ", "#####"],
        'F' => ["#####", "#    ", "#### ", "#    ", "#    ", "#    ", "#    "],
        'G' => [" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ### "],
        'H' => ["#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
        'I' => ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"],
        'J' => ["  ###", "   # ", "   # ", "   # ", "#  # ", "#  # ", " ##  "],
        'K' => ["#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"],
        'L' => ["#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"],
        'M' => ["#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"],
        'N' => ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"],
        'O' => [" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
        'P' => ["#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "],
        'Q' => [" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"],
        'R' => ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
        'S' => [" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "],
        'T' => ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
        'U' => ["#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
        'V' => ["#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
        'W' => ["#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"],
        'X' => ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"],
        'Y' => ["#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "],
        'Z' => ["#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"],
        '0' => [" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "],
        '1' => ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"],
        '2' => [" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"],
        '3' => ["#####", "    #", "   # ", "  ## ", "    #", "#   #", " ### "],
        '4' => ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "],
        '5' => ["#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "],
        '6' => [" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "],
        '7' => ["#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "],
        '8' => [" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "],
        '9' => [" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "],
        '-' => ["     ", "     ", "     ", "#####", "     ", "     ", "     "],
        ':' => ["     ", "  #  ", "  #  ", "     ", "  #  ", "  #  ", "     "],
        '.' => ["     ", "     ", "     ", "     ", "     ", "  ## ", "  ## "],
        _ => ["     ", "     ", "     ", "     ", "     ", "     ", "     "], // space & unknowns
    }
}

fn draw_char(img: &mut GrayImage, x0: i64, y0: i64, scale: i64, c: char) {
    for (row, line) in glyph(c).iter().enumerate() {
        for (col, px) in line.chars().enumerate() {
            if px != '#' {
                continue;
            }
            for dy in 0..scale {
                for dx in 0..scale {
                    let x = x0 + col as i64 * scale + dx;
                    let y = y0 + row as i64 * scale + dy;
                    if x >= 0 && y >= 0 && (x as u32) < img.width() && (y as u32) < img.height() {
                        img.put_pixel(x as u32, y as u32, Luma([BLACK]));
                    }
                }
            }
        }
    }
}

/// Draws left-aligned text at (x0, y0) and returns the pixel width consumed.
fn draw_text(img: &mut GrayImage, x0: i64, y0: i64, scale: i64, text: &str) -> i64 {
    let advance = 6 * scale; // 5 glyph columns + 1 column of spacing
    for (i, c) in text.chars().enumerate() {
        draw_char(img, x0 + i as i64 * advance, y0, scale, c);
    }
    text.chars().count() as i64 * advance
}

fn text_width(text: &str, scale: i64) -> i64 {
    text.chars().count() as i64 * 6 * scale
}

fn draw_centered(img: &mut GrayImage, y0: i64, scale: i64, text: &str) {
    let w = text_width(text, scale);
    let x0 = (PANEL_W as i64 - w) / 2;
    draw_text(img, x0, y0, scale, text);
}

fn draw_rect_border(img: &mut GrayImage, margin: u32, thickness: u32) {
    let (w, h) = (img.width(), img.height());
    for t in 0..thickness {
        for x in margin..(w - margin) {
            img.put_pixel(x, margin + t, Luma([BLACK]));
            img.put_pixel(x, h - margin - 1 - t, Luma([BLACK]));
        }
        for y in margin..(h - margin) {
            img.put_pixel(margin + t, y, Luma([BLACK]));
            img.put_pixel(w - margin - 1 - t, y, Luma([BLACK]));
        }
    }
}

fn draw_hline(img: &mut GrayImage, y: u32, x0: u32, x1: u32, thickness: u32) {
    for t in 0..thickness {
        for x in x0..x1 {
            if y + t < img.height() {
                img.put_pixel(x, y + t, Luma([BLACK]));
            }
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let out_path = args
        .next()
        .unwrap_or_else(|| "/mnt/us/oasis-hello.png".to_string());
    let skip_eips = std::env::args().any(|a| a == "--no-eips");

    let mut img = GrayImage::from_pixel(PANEL_W, PANEL_H, Luma([WHITE]));

    draw_rect_border(&mut img, 24, 4);
    draw_centered(&mut img, 260, 12, "KINDLE-COBALT");
    draw_hline(&mut img, 420, 120, (PANEL_W - 120) as u32, 3);
    draw_centered(&mut img, 480, 6, "HARDFLOAT MUSL: CONFIRMED");
    draw_centered(&mut img, 560, 6, "RUNNING NATIVE ON KINDLEOS");
    draw_hline(&mut img, 1280, 120, (PANEL_W - 120) as u32, 3);
    draw_centered(&mut img, 1340, 4, "FRIDAY PROJECT - GENERATED ON DEVICE");

    img.save(&out_path).expect("failed to write PNG");
    println!("wrote {out_path} ({PANEL_W}x{PANEL_H})");

    if skip_eips {
        return;
    }

    let clear = Command::new("eips").arg("-c").status();
    println!("eips -c: {clear:?}");
    let show = Command::new("eips")
        .args(["-g", &out_path, "-f"])
        .status();
    println!("eips -g {out_path} -f: {show:?}");
}
