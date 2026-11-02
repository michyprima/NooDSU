/*
    Copyright 2019-2026 Hydr8gon
    Copyright 2026 michyprima

    This file is part of NooDS U.

    NooDS U is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS U is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS U. If not, see <https://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cmath>

#include "screen_frame.h"

namespace ScreenFrame {
    const char *const colorNames[COLOR_COUNT] = {
        "Black", "White", "Blue", "Metallic Blue", "Pink", "Lime Green", "Red", "Orange"
    };

    struct Color {
        float r, g, b;
    };

    struct Palette {
        uint32_t color, frame, ink;
        float grain;
    };

    // Every DSi is one colour inside and out, with a darker frame around the screens and light or dark markings
    static const Palette palettes[COLOR_COUNT] = {
        { 0x2B2C2F, 0x3B3C40, 0xC2C4C8, 2.0f },
        { 0xE9EAEA, 0xC4C5C6, 0x74767A, 1.5f },
        { 0x27AED6, 0x1F93B8, 0xF2FAFD, 2.0f },
        { 0x23509F, 0x1B3F80, 0xE4EBF7, 11.0f },
        { 0xE0587F, 0xC04467, 0xFFF2F5, 2.0f },
        { 0xA5CE3C, 0x8BB02C, 0x34480C, 2.0f },
        { 0xC11F2E, 0x9E1623, 0xFDECED, 2.0f },
        { 0xEE7A18, 0xCC640C, 0xFFF4E8, 2.0f }
    };

    static const Color BLACK = { 6, 6, 7 };

    static Color rgb(uint32_t hex) {
        return { float(hex >> 16), float((hex >> 8) & 0xFF), float(hex & 0xFF) };
    }

    static Color shade(Color color, float amount) {
        // Mix a color toward white for positive amounts and black for negative ones
        float target = (amount < 0) ? 0 : 255, t = std::abs(amount);
        return { color.r + (target - color.r) * t, color.g + (target - color.g) * t, color.b + (target - color.b) * t };
    }

    static Color lerp(Color a, Color b, float t) {
        return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
    }

    static float clamp01(float value) {
        return std::min(1.0f, std::max(0.0f, value));
    }

    class Canvas {
    public:
        uint32_t *pixels;
        int width, height;
        const uint8_t *fontBmp, *charWidths;
        int hidden[2][4]; // The parts of the display that screens will cover, which can be left out, from left to right
        int hiddenCount;

        // Call a function with the parts of a row that won't be covered by a screen
        template <typename Function>
        void visibleSpans(int y, int x0, int x1, Function function) {
            for (int i = 0; i < hiddenCount; i++) {
                if (y < hidden[i][1] || y >= hidden[i][3]) continue;
                if (x0 < std::min(x1, hidden[i][0])) function(x0, std::min(x1, hidden[i][0]));
                x0 = std::max(x0, hidden[i][2]);
            }
            if (x0 < x1) function(x0, x1);
        }

        void span(int y, int x0, int x1, Color color, float alpha) {
            // Fill part of a row with one color using whole numbers, since converting floats for each pixel is slow
            int r = color.r + 0.5f, g = color.g + 0.5f, b = color.b + 0.5f, a = alpha * 256 + 0.5f;
            uint32_t solid = 0xFF000000 | (b << 16) | (g << 8) | r;
            x0 = std::max(x0, 0);
            x1 = std::min(x1, width);
            visibleSpans(y, x0, x1, [=, this](int start, int end) {
                uint32_t *row = &pixels[y * width];
                if (a >= 256) {
                    std::fill(&row[start], &row[end], solid);
                    return;
                }
                for (int x = start; x < end; x++) {
                    int pr = row[x] & 0xFF, pg = (row[x] >> 8) & 0xFF, pb = (row[x] >> 16) & 0xFF;
                    pr += ((r - pr) * a + 128) >> 8;
                    pg += ((g - pg) * a + 128) >> 8;
                    pb += ((b - pb) * a + 128) >> 8;
                    row[x] = 0xFF000000 | (pb << 16) | (pg << 8) | pr;
                }
            });
        }

        void blend(int x, int y, Color color, float alpha) {
            // Blend a color over a pixel, which holds alpha, blue, green, and red from the top bits down
            uint32_t pixel = pixels[y * width + x];
            int r = (pixel & 0xFF) + (color.r - (pixel & 0xFF)) * alpha + 0.5f;
            int g = ((pixel >> 8) & 0xFF) + (color.g - ((pixel >> 8) & 0xFF)) * alpha + 0.5f;
            int b = ((pixel >> 16) & 0xFF) + (color.b - ((pixel >> 16) & 0xFF)) * alpha + 0.5f;
            pixels[y * width + x] = 0xFF000000 | (std::min(b, 255) << 16) | (std::min(g, 255) << 8) | std::min(r, 255);
        }

        // Fill the pixels in a box using functions that give the coverage and color at each pixel's center
        template <typename Coverage, typename Shader>
        void fill(float x0, float y0, float x1, float y1, Coverage coverage, Shader shader) {
            int left = std::max(0, int(std::floor(x0))), right = std::min(width, int(std::ceil(x1)));
            int top = std::max(0, int(std::floor(y0))), bottom = std::min(height, int(std::ceil(y1)));
            for (int y = top; y < bottom; y++) {
                for (int x = left; x < right; x++) {
                    float alpha = coverage(x + 0.5f, y + 0.5f);
                    if (alpha > 0) blend(x, y, shader(x + 0.5f, y + 0.5f), alpha);
                }
            }
        }

        void rect(float x, float y, float w, float h, Color color, float alpha = 1) {
            // Fill a rectangle, with partly covered pixels at its edges
            float right = x + w, bottom = y + h;
            int first = std::ceil(x), last = std::floor(right);
            int top = std::max(0, int(std::floor(y))), end = std::min(height, int(std::ceil(bottom)));
            for (int row = top; row < end; row++) {
                float cover = (std::min(row + 1.0f, bottom) - std::max(float(row), y)) * alpha;
                if (first <= last) span(row, first, last, color, cover);
                int left = first - 1;
                if (x < first && left >= 0 && left < width)
                    blend(left, row, color, (std::min(first + 0.0f, right) - x) * cover);
                if (right > last && last >= first && last >= 0 && last < width)
                    blend(last, row, color, (right - last) * cover);
            }
        }

        static float roundedDistance(float px, float py, float x, float y, float w, float h, float radius) {
            // Get the distance from a rounded rectangle's edge, negative inside
            float dx = std::abs(px - (x + w / 2)) - (w / 2 - radius);
            float dy = std::abs(py - (y + h / 2)) - (h / 2 - radius);
            float mx = std::max(dx, 0.0f), my = std::max(dy, 0.0f);
            return std::sqrt(mx * mx + my * my) + std::min(std::max(dx, dy), 0.0f) - radius;
        }

        template <typename Shader>
        void rounded(float x, float y, float w, float h, float radius, Shader shader) {
            fill(x - 1, y - 1, x + w + 1, y + h + 1, [=](float px, float py) {
                return clamp01(0.5f - roundedDistance(px, py, x, y, w, h, radius));
            }, shader);
        }

        void rounded(float x, float y, float w, float h, float radius, Color color) {
            rounded(x, y, w, h, radius, [=](float, float) { return color; });
        }

        void roundedOutline(float x, float y, float w, float h, float radius, float line, Color color) {
            fill(x - line - 1, y - line - 1, x + w + line + 1, y + h + line + 1, [=](float px, float py) {
                return clamp01(line / 2 + 0.5f - std::abs(roundedDistance(px, py, x, y, w, h, radius)));
            }, [=](float, float) { return color; });
        }

        template <typename Shader>
        void circle(float x, float y, float radius, Shader shader) {
            fill(x - radius - 1, y - radius - 1, x + radius + 1, y + radius + 1, [=](float px, float py) {
                return clamp01(0.5f + radius - std::hypot(px - x, py - y));
            }, shader);
        }

        void circle(float x, float y, float radius, Color color, float alpha = 1) {
            fill(x - radius - 1, y - radius - 1, x + radius + 1, y + radius + 1, [=](float px, float py) {
                return clamp01(0.5f + radius - std::hypot(px - x, py - y)) * alpha;
            }, [=](float, float) { return color; });
        }

        void line(float x0, float y0, float x1, float y1, float thickness, Color color) {
            // Draw a line with round ends
            float pad = thickness / 2 + 1, dx = x1 - x0, dy = y1 - y0, length = dx * dx + dy * dy;
            fill(std::min(x0, x1) - pad, std::min(y0, y1) - pad, std::max(x0, x1) + pad, std::max(y0, y1) + pad,
                [=](float px, float py) {
                    float t = length ? clamp01(((px - x0) * dx + (py - y0) * dy) / length) : 0;
                    return clamp01(thickness / 2 + 0.5f - std::hypot(px - x0 - dx * t, py - y0 - dy * t));
                }, [=](float, float) { return color; });
        }

        void arc(float x, float y, float radius, float start, float span, float thickness, Color color) {
            // Draw an arc with round ends, going from an angle in the direction angles grow on screen
            const float TAU = 6.2831853f;
            float pad = radius + thickness / 2 + 1;
            float x0 = x + std::cos(start) * radius, y0 = y + std::sin(start) * radius;
            float x1 = x + std::cos(start + span) * radius, y1 = y + std::sin(start + span) * radius;
            fill(x - pad, y - pad, x + pad, y + pad, [=](float px, float py) {
                float angle = std::atan2(py - y, px - x) - start;
                angle -= TAU * std::floor(angle / TAU);
                float distance = (angle <= span) ? std::abs(std::hypot(px - x, py - y) - radius) :
                    std::min(std::hypot(px - x0, py - y0), std::hypot(px - x1, py - y1));
                return clamp01(thickness / 2 + 0.5f - distance);
            }, [=](float, float) { return color; });
        }

        float glyphAlpha(int c, float u, float v, float footprint) {
            // Average the font sheet's alpha over a pixel's footprint in a character's 48-pixel cell
            int samples = std::max(1, int(std::ceil(footprint))), total = 0;
            for (int j = 0; j < samples; j++) {
                for (int i = 0; i < samples; i++) {
                    int x = u + (i + 0.5f) * footprint / samples, y = v + (j + 0.5f) * footprint / samples;
                    if (x < 0 || y < 0 || x >= 47 || y >= 47) continue;
                    x += 48 * (c % 10);
                    y += 48 * (c / 10);
                    total += fontBmp[0x46 + (((511 - y) * 512 + x) << 2) + 3];
                }
            }
            return total / (255.0f * samples * samples);
        }

        void text(const char *string, float x, float y, float fontSize, Color color, int align = 0) {
            // Draw text with the app's font, matching capital letters to the given font size's and centering them on y
            // Capitals in the font sheet are 25 pixels tall, with their middle 23.5 pixels down each cell
            float cell = fontSize * 1.34f, scale = cell / 48, total = 0;
            for (const char *c = string; *c; c++) total += charWidths[*c - 32] * scale;
            float pen = x - ((align < 0) ? 0 : (align > 0) ? total : total / 2), top = y - 23.5f * scale;
            for (const char *c = string; *c; c++) {
                int index = *c - 32;
                float left = pen, footprint = 1 / scale;
                fill(left, top, left + cell, top + cell, [=, this](float px, float py) {
                    return glyphAlpha(index, (px - 0.5f - left) / scale, (py - 0.5f - top) / scale, footprint);
                }, [=](float, float) { return color; });
                pen += charWidths[index] * scale;
            }
        }
    };

    static void drawShell(Canvas &canvas, const Palette &palette, int seed) {
        // Fill the display with the shell's color, a little lighter at the top and with a fine grain over it, using fixed
        // point since it's the most pixels this draws
        Color top = shade(rgb(palette.color), 0.05f), bottom = shade(rgb(palette.color), -0.06f);
        int grain = palette.grain * 256;
        uint32_t state = seed * 7 + 11;
        for (int y = 0; y < canvas.height; y++) {
            Color row = lerp(top, bottom, float(y) / canvas.height);
            int r = row.r * 256, g = row.g * 256, b = row.b * 256;
            uint32_t *pixel = &canvas.pixels[y * canvas.width];
            canvas.visibleSpans(y, 0, canvas.width, [&](int start, int end) {
                for (int x = start; x < end; x++) {
                    state = state * 1664525 + 1013904223;
                    int noise = int(((state >> 16) * grain) >> 16) - grain / 2 + 128;
                    int nr = (r + noise) >> 8, ng = (g + noise) >> 8, nb = (b + noise) >> 8;
                    pixel[x] = 0xFF000000 | (std::min(255, std::max(0, nb)) << 16) |
                        (std::min(255, std::max(0, ng)) << 8) | std::min(255, std::max(0, nr));
                }
            });
        }
    }

    static void drawHingeBar(Canvas &canvas, Color base, float x, float w, float y, float t, float lift) {
        // Shade a piece of the hinge like a cylinder
        Color edgeTop = shade(base, -0.28f + lift), light = shade(base, 0.08f + lift);
        Color dark = shade(base, -0.04f + lift), edgeBottom = shade(base, -0.34f + lift);
        for (int row = std::floor(y - t); row < std::ceil(y + t); row++) {
            float at = clamp01((row + 0.5f - (y - t)) / (t * 2));
            Color color = (at < 0.3f) ? lerp(edgeTop, light, at / 0.3f) :
                (at < 0.72f) ? lerp(light, dark, (at - 0.3f) / 0.42f) : lerp(dark, edgeBottom, (at - 0.72f) / 0.28f);
            float top = std::max(float(row), y - t), bottom = std::min(row + 1.0f, y + t);
            canvas.rect(x, top, w, bottom - top, color);
        }
    }

    static void drawHinge(Canvas &canvas, const Palette &palette, float y, float t, float s,
        float leftRoom, float rightRoom, bool lights) {
        // Draw the hinge across the display, with a soft shadow around it
        Color base = rgb(palette.color), ink = rgb(palette.ink);
        float w = canvas.width;
        canvas.rect(0, y - t - 2 * s, w, t * 2 + 5 * s, { 0, 0, 0 }, 0.3f);
        drawHingeBar(canvas, base, 0, w, y, t, 0);

        // The two ends are separate pieces, and the left one carries the lights
        float seam = std::max(1.0f, 1.5f * s);
        if (leftRoom >= 60 * s) {
            float end = leftRoom * 0.58f;
            drawHingeBar(canvas, base, 0, end, y, t, 0.04f);
            canvas.rect(end, y - t, seam, t * 2, { 0, 0, 0 }, 0.45f);
            if (lights) {
                float u = std::min(leftRoom / 160, 1.3f), x = leftRoom * 0.28f;
                for (int i = -1; i <= 1; i++) {
                    Color light = (i == 1) ? Color{ 0x5F, 0xB8, 0xFF } : shade(base, -0.4f);
                    canvas.rounded(x + i * 16 * u - 2 * u, y - 7 * u, 4 * u, 14 * u, 2 * u, light);
                }
            }
        }
        if (rightRoom >= 60 * s) {
            float end = rightRoom * 0.58f;
            drawHingeBar(canvas, base, w - end, end, y, t, 0.04f);
            canvas.rect(w - end, y - t, seam, t * 2, { 0, 0, 0 }, 0.45f);
        }

        // Draw the camera and mic in the middle
        if (lights) {
            canvas.circle(w / 2, y, 11 * s, BLACK);
            canvas.circle(w / 2, y, 7 * s, Color{ 0x1C, 0x22, 0x30 });
            canvas.circle(w / 2 - 2 * s, y - 2 * s, 1.8f * s, Color{ 255, 255, 255 }, 0.55f);
            canvas.circle(w / 2 + 34 * s, y, 2 * s, BLACK);
            canvas.text("MIC.", w / 2 + 41 * s, y, 8.5f * s, ink, -1);
        }
    }

    static void drawSpeaker(Canvas &canvas, float x, float y, float u) {
        // Draw one small slot, with a faint highlight on its lower edge
        canvas.fill(x - 16 * u, y - 4 * u, x + 16 * u, y + 7 * u, [=](float px, float py) {
            return clamp01(0.5f - Canvas::roundedDistance(px, py, x - 15 * u, y - 3 * u, 30 * u, 9 * u, 4.5f * u)) * 0.14f;
        }, [](float, float) { return Color{ 255, 255, 255 }; });
        canvas.rounded(x - 15 * u, y - 4.5f * u, 30 * u, 9 * u, 4.5f * u, BLACK);
    }

    static void drawPad(Canvas &canvas, Color base, float x, float y, float u) {
        // Draw one of the small rubber pads the lid closes on
        canvas.rounded(x - 9 * u, y - 9 * u, 18 * u, 18 * u, 3 * u, shade(base, -0.07f));
        canvas.roundedOutline(x - 9 * u, y - 9 * u, 18 * u, 18 * u, 3 * u, std::max(1.0f, u), shade(base, -0.16f));
    }

    static void drawPlus(Canvas &canvas, float x, float y, float reach, float arm, float radius, Color top, Color bottom) {
        auto shader = [=](float, float py) { return lerp(top, bottom, clamp01((py - (y - reach)) / (reach * 2))); };
        canvas.fill(x - reach - 1, y - reach - 1, x + reach + 1, y + reach + 1, [=](float px, float py) {
            float a = Canvas::roundedDistance(px, py, x - arm, y - reach, arm * 2, reach * 2, radius);
            float b = Canvas::roundedDistance(px, py, x - reach, y - arm, reach * 2, arm * 2, radius);
            return clamp01(0.5f - std::min(a, b));
        }, shader);
    }

    static void drawDpad(Canvas &canvas, const Palette &palette, float x, float y, float u) {
        // Draw a raised cross in the shell's color, with a thin seam around it and a mark on each arm
        Color base = rgb(palette.color), ink = rgb(palette.ink), seam = shade(base, -0.42f);
        float reach = 40 * u, arm = 13.5f * u;
        drawPlus(canvas, x, y + 2.4f * u, reach + 1.6f * u, arm + 1.6f * u, 4 * u, seam, seam);
        drawPlus(canvas, x, y, reach, arm, 3.5f * u, shade(base, 0.09f), shade(base, -0.06f));
        static const int dirs[][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
        for (auto &d : dirs)
            canvas.line(x + d[0] * 21 * u, y + d[1] * 21 * u, x + d[0] * 32 * u, y + d[1] * 32 * u, 1.8f * u, ink);
    }

    static void drawRoundButton(Canvas &canvas, Color base, float x, float y, float radius, float u) {
        canvas.circle(x, y + 2.2f * u, radius + 0.8f * u, shade(base, -0.42f));
        Color top = shade(base, 0.1f), bottom = shade(base, -0.07f);
        canvas.circle(x, y, radius, [=](float, float py) {
            return lerp(top, bottom, clamp01((py - (y - radius)) / (radius * 2)));
        });
    }

    static void drawFaceButtons(Canvas &canvas, const Palette &palette, float x, float y, float u) {
        static const struct { const char *name; int x, y; } buttons[] =
            { { "X", 0, -1 }, { "Y", -1, 0 }, { "A", 1, 0 }, { "B", 0, 1 } };
        for (auto &b : buttons) {
            drawRoundButton(canvas, rgb(palette.color), x + b.x * 38 * u, y + b.y * 38 * u, 18.5f * u, u);
            canvas.text(b.name, x + b.x * 38 * u, y + b.y * 38 * u + 0.5f * u, 17 * u, rgb(palette.ink));
        }
    }

    static void drawPowerIcon(Canvas &canvas, float x, float y, float radius, float thickness, Color color) {
        const float PI = 3.1415927f;
        canvas.arc(x, y, radius, -PI * 0.28f, PI * 1.56f, thickness, color);
        canvas.line(x, y - radius * 1.25f, x, y - radius * 0.2f, thickness, color);
    }

    static void drawPowerButton(Canvas &canvas, const Palette &palette, float x, float y, float u) {
        canvas.circle(x, y + 1.6f * u, 10.5f * u, shade(rgb(palette.color), -0.42f));
        Color top = { 0xD9, 0xDA, 0xDC }, bottom = { 0xA4, 0xA6, 0xA9 };
        canvas.circle(x, y, 10 * u, [=](float, float py) {
            return lerp(top, bottom, clamp01((py - (y - 10 * u)) / (20 * u)));
        });
        drawPowerIcon(canvas, x, y + 0.5f * u, 4.2f * u, 1.3f * u, Color{ 0x7C, 0x7E, 0x82 });
        canvas.text("POWER", x - 16 * u, y, 10 * u, rgb(palette.ink), 1);
    }

    static void drawStatusIcons(Canvas &canvas, Color ink, float x, float y, float u) {
        // Draw the wireless, charging, and power icons above the hinge's lights
        const float PI = 3.1415927f;
        float wireless = x - 16 * u, thick = 1.4f * u;
        for (int radius = 3; radius <= 9; radius += 3)
            canvas.arc(wireless, y + 5 * u, radius * u, -PI * 0.75f, PI * 0.5f, thick, ink);
        canvas.circle(wireless, y + 5 * u, 1.2f * u, ink);
        canvas.rounded(x - 3 * u, y - 4 * u, 6 * u, 6 * u, 1.5f * u, ink);
        canvas.rect(x - 0.8f * u, y + 2 * u, 1.6f * u, 4 * u, ink);
        canvas.rect(x - 2 * u, y - 7 * u, 1.2f * u, 3 * u, ink);
        canvas.rect(x + 0.8f * u, y - 7 * u, 1.2f * u, 3 * u, ink);
        drawPowerIcon(canvas, x + 16 * u, y, 4.6f * u, thick, ink);
    }

    void draw(uint32_t *pixels, int width, int height, int color, const Screen *screens, int count, bool lights,
        const uint8_t *fontBmp, const uint8_t *charWidths) {
        Canvas canvas = { pixels, width, height, fontBmp, charWidths };
        const Palette &palette = palettes[color];

        // Most of a display is usually covered by screens, so leave those parts black to save time
        std::fill(pixels, pixels + width * height, 0xFF000000);
        canvas.hiddenCount = count;
        for (int i = 0; i < count; i++) {
            canvas.hidden[i][0] = std::ceil(screens[i].x);
            canvas.hidden[i][1] = std::ceil(screens[i].y);
            canvas.hidden[i][2] = std::floor(screens[i].x + screens[i].width);
            canvas.hidden[i][3] = std::floor(screens[i].y + screens[i].height);
        }
        if (count == 2 && canvas.hidden[0][0] > canvas.hidden[1][0])
            std::swap(canvas.hidden[0], canvas.hidden[1]);
        Color base = rgb(palette.color), ink = rgb(palette.ink);
        float s = height / 720.0f, t = 26 * s;
        drawShell(canvas, palette, color);

        // Place the hinge at the edge of a display with one screen, or between stacked screens
        // Screens side by side are too far apart for one, since a real system doesn't open that way
        bool hasHinge = true;
        float hinge = 0;
        if (count == 1) {
            hinge = screens[0].bottom ? t : height - t;
        }
        else {
            const Screen &a = screens[0], &b = screens[1];
            hasHinge = (a.y + a.height <= b.y + 1 || b.y + b.height <= a.y + 1);
            const Screen &upper = (a.y < b.y) ? a : b, &lower = (a.y < b.y) ? b : a;
            hinge = (upper.y + upper.height + lower.y) / 2;
        }

        // Find the room beside each screen, up to the display's edge or the other screen
        float leftRooms[2], rightRooms[2], hingeLeft = width, hingeRight = width;
        for (int i = 0; i < count; i++) {
            float start = 0, end = width;
            if (count == 2) {
                const Screen &other = screens[!i];
                if (other.y < screens[i].y + screens[i].height - 1 && screens[i].y < other.y + other.height - 1) {
                    if (other.x + other.width <= screens[i].x + 1) start = other.x + other.width;
                    if (screens[i].x + screens[i].width <= other.x + 1) end = other.x;
                }
            }
            leftRooms[i] = screens[i].x - start;
            rightRooms[i] = end - (screens[i].x + screens[i].width);
            hingeLeft = std::min(hingeLeft, screens[i].x);
            hingeRight = std::min(hingeRight, width - (screens[i].x + screens[i].width));
        }
        if (hasHinge) drawHinge(canvas, palette, hinge, t, s, hingeLeft, hingeRight, lights);

        // Draw what's beside each screen, leaving out any side without room for it
        for (int i = 0; i < count; i++) {
            const Screen &screen = screens[i];
            float y0 = 0, y1 = height;
            if (hasHinge) {
                if (screen.y + screen.height / 2 < hinge) y1 = hinge - t;
                else y0 = hinge + t;
            }
            float span = y1 - y0, leftRoom = leftRooms[i], rightRoom = rightRooms[i];
            float leftStart = screen.x - leftRoom, rightStart = screen.x + screen.width;
            float leftU = std::min(leftRoom / 160, span / 288), rightU = std::min(rightRoom / 160, span / 288);
            bool left = (leftRoom >= 60 * s), right = (rightRoom >= 60 * s);
            if (!screen.bottom) {
                float slot = screen.y + screen.height * 0.78f;
                if (left) {
                    drawSpeaker(canvas, leftStart + leftRoom / 2, slot, leftU);
                    drawPad(canvas, base, leftStart + leftRoom / 2, y0 + 30 * leftU, leftU);
                    drawPad(canvas, base, leftStart + leftRoom / 2, y1 - 22 * leftU, leftU);
                    if (hasHinge) drawStatusIcons(canvas, ink, hingeLeft * 0.28f, y1 - 13 * leftU, leftU);
                }
                if (right) {
                    drawSpeaker(canvas, rightStart + rightRoom / 2, slot, rightU);
                    drawPad(canvas, base, rightStart + rightRoom / 2, y0 + 30 * rightU, rightU);
                    drawPad(canvas, base, rightStart + rightRoom / 2, y1 - 22 * rightU, rightU);
                }
            }
            else {
                // Keep the small buttons and their labels readable on small displays, as far as they fit beside the screen
                auto small = [](float u, float room) { return std::max(u, std::min(1.15f, room / 93)); };
                if (left) {
                    drawDpad(canvas, palette, leftStart + leftRoom / 2, y0 + span * 0.38f, leftU);
                    drawPowerButton(canvas, palette, leftStart + leftRoom * 0.74f, y0 + span * 0.86f, small(leftU, leftRoom));
                }
                if (right) {
                    drawFaceButtons(canvas, palette, rightStart + rightRoom / 2, y0 + span * 0.3f, rightU);
                    // Line up the small buttons' left edges with the Y button's, and the lower one's bottom with the power button's
                    static const struct { const char *name; float up; } buttons[] = { { "START", 0.1f }, { "SELECT", 0 } };
                    float u = small(rightU, rightRoom);
                    float x = rightStart + rightRoom / 2 - (38 + 18.5f) * rightU + 8 * u;
                    float lowest = y0 + span * 0.86f + 10 * small(leftU, leftRoom) - 8 * u;
                    for (auto &b : buttons) {
                        float y = lowest - span * b.up;
                        drawRoundButton(canvas, base, x, y, 8 * u, u);
                        canvas.text(b.name, x + 13 * u, y, 10 * u, ink, -1);
                    }
                }
            }
        }

        // Draw the frame around the screens last, so the art never covers any of a screen, and the dark edges after all
        // of the frames, so that where the screens meet, one's frame doesn't break the other's edge
        // They're on whole pixels, since partly covered edges would show darker where they overlap
        for (int pass = 0; pass < 2; pass++) {
            float out = (pass ? 2.5f : 10) * s;
            Color band = pass ? Color{ 8, 8, 10 } : rgb(palette.frame);
            for (int i = 0; i < count; i++) {
                const Screen &screen = screens[i];
                float left = std::floor(screen.x - out), top = std::floor(screen.y - out);
                float right = std::ceil(screen.x + screen.width + out), bottom = std::ceil(screen.y + screen.height + out);
                canvas.rect(left, top, right - left, bottom - top, band);
            }
        }
    }
}
