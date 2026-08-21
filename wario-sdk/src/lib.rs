//! wario-sdk — the smallest possible toolkit for drawing something on a
//! jailbroken Kindle Oasis and getting it on screen.
//!
//! No component model, no virtual DOM, no app trait, no lifecycle. Cobalt's
//! own `kobo-ui` (the thing this project was originally inspired by) is the
//! same shape: a hand-rolled retained tree + rasterizer with exactly one
//! external dependency, because nothing general-purpose (Dioxus/Yew/Leptos/
//! egui/iced/slint — checked) targets e-ink refresh semantics. This is
//! smaller still: just draw into a `GrayImage` with the functions below,
//! then call [`display`].

use image::{GrayImage, Luma};
use std::io;
use std::process::Command;

/// Kindle Oasis 1st-gen panel dimensions.
pub const PANEL_W: u32 = 1072;
pub const PANEL_H: u32 = 1448;

pub const BLACK: u8 = 0;
pub const WHITE: u8 = 255;

/// A fresh white canvas sized for the Oasis panel.
pub fn new_canvas() -> GrayImage {
    GrayImage::from_pixel(PANEL_W, PANEL_H, Luma([WHITE]))
}

/// 7 rows of 5 columns each, '#' = ink, ' ' = blank. Hand-drawn, blocky on
/// purpose. Covers A-Z, 0-9, space, '-', ':', '.'.
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

/// Draws one character at (x0, y0) scaled up by `scale` pixels per glyph dot.
pub fn draw_char(img: &mut GrayImage, x0: i64, y0: i64, scale: i64, c: char) {
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
pub fn draw_text(img: &mut GrayImage, x0: i64, y0: i64, scale: i64, text: &str) -> i64 {
    let advance = 6 * scale; // 5 glyph columns + 1 column of spacing
    for (i, c) in text.chars().enumerate() {
        draw_char(img, x0 + i as i64 * advance, y0, scale, c);
    }
    text.chars().count() as i64 * advance
}

/// Pixel width `text` would occupy at `scale`, without drawing it.
pub fn text_width(text: &str, scale: i64) -> i64 {
    text.chars().count() as i64 * 6 * scale
}

/// Draws `text` horizontally centered on the panel at row `y0`.
pub fn draw_centered(img: &mut GrayImage, y0: i64, scale: i64, text: &str) {
    let w = text_width(text, scale);
    let x0 = (PANEL_W as i64 - w) / 2;
    draw_text(img, x0, y0, scale, text);
}

/// Draws a rectangular border inset `margin` px from the panel edge.
pub fn draw_rect_border(img: &mut GrayImage, margin: u32, thickness: u32) {
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

/// Draws a horizontal rule from `x0` to `x1` at row `y`.
pub fn draw_hline(img: &mut GrayImage, y: u32, x0: u32, x1: u32, thickness: u32) {
    for t in 0..thickness {
        for x in x0..x1 {
            if y + t < img.height() {
                img.put_pixel(x, y + t, Luma([BLACK]));
            }
        }
    }
}

/// Saves `img` as a PNG at `path` and shows it via the device's own `eips`
/// utility (clear, then a full-waveform refresh). Meant to be run on-device;
/// `eips` isn't present anywhere else.
pub fn display(img: &GrayImage, path: &str) -> io::Result<()> {
    img.save(path).map_err(|e| io::Error::other(e.to_string()))?;
    Command::new("eips").arg("-c").status()?;
    Command::new("eips").args(["-g", path, "-f"]).status()?;
    Ok(())
}
