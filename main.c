#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "raylib.h"
#include <ghostty/vt.h>

// Embed the font file directly into the binary at compile time using C23
// #embed so we don't need to locate it at runtime.
static const unsigned char font_jetbrains_mono[] = {
    #embed "fonts/JetBrainsMono-Regular.ttf"
};

// ---------------------------------------------------------------------------
// PTY helpers
// ---------------------------------------------------------------------------

// Spawn /bin/sh in a new pseudo-terminal.
//
// Creates a pty pair via forkpty(), sets the initial window size, execs the
// shell in the child, and puts the master fd into non-blocking mode so we
// can poll it each frame without stalling the render loop.
//
// Returns the master fd on success (>= 0) and stores the child pid in
// *child_out.  Returns -1 on failure.
static int pty_spawn(pid_t *child_out, uint16_t cols, uint16_t rows)
{
    int pty_fd;
    struct winsize ws = { .ws_row = rows, .ws_col = cols };

    // forkpty() combines openpty + fork + login_tty into one call.
    // In the child it sets up the slave side as stdin/stdout/stderr.
    pid_t child = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        // Child process — replace ourselves with the shell.
        // TERM tells programs what escape sequences we understand.
        setenv("TERM", "xterm-256color", 1);
        execl("/bin/sh", "sh", NULL);
        _exit(127); // execl only returns on error
    }

    // Parent — make the master fd non-blocking so read() returns EAGAIN
    // instead of blocking when there's no data, letting us poll each frame.
    int flags = fcntl(pty_fd, F_GETFL);
    fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK);

    *child_out = child;
    return pty_fd;
}

// Drain all available output from the pty master and feed it into the
// ghostty terminal.  The terminal's VT parser will process any escape
// sequences and update its internal screen/cursor/style state.
//
// Because the fd is non-blocking, read() returns -1 with EAGAIN once
// the kernel buffer is empty, at which point we stop.
static void pty_read(int pty_fd, GhosttyTerminal terminal)
{
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(pty_fd, buf, sizeof(buf));
        if (n > 0)
            ghostty_terminal_vt_write(terminal, buf, (size_t)n);
        else
            break;
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

// Encode a single Unicode codepoint into UTF-8 and write it to the pty.
// This is used for printable characters that raylib delivers as codepoints
// via GetCharPressed().
static void pty_write_codepoint(int pty_fd, int cp)
{
    char utf8[4];
    int len;

    // Standard UTF-8 encoding: 1–4 bytes depending on codepoint range.
    if (cp < 0x80) {
        utf8[0] = (char)cp;
        len = 1;
    } else if (cp < 0x800) {
        utf8[0] = (char)(0xC0 | (cp >> 6));
        utf8[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        utf8[0] = (char)(0xE0 | (cp >> 12));
        utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        utf8[0] = (char)(0xF0 | (cp >> 18));
        utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    write(pty_fd, utf8, len);
}

// Poll raylib for keyboard events and write the corresponding byte
// sequences to the pty.
//
// Three categories of input:
//   1. Printable characters — delivered by GetCharPressed() as Unicode
//      codepoints; we UTF-8 encode them and send them straight through.
//   2. Special / function keys — mapped to the VT escape sequences that
//      programs on the other side of the pty expect (e.g. arrow keys
//      send ESC [ A/B/C/D).
//   3. Ctrl+letter combos — produce the traditional control characters
//      (ctrl+a = 0x01, ctrl+c = 0x03, ctrl+d = 0x04, etc.).
static void handle_input(int pty_fd)
{
    // --- 1. Printable characters ---
    int ch;
    while ((ch = GetCharPressed()) != 0)
        pty_write_codepoint(pty_fd, ch);

    // --- 2. Special keys → VT escape sequences ---
    // Each entry maps a raylib key constant to the byte sequence a
    // traditional terminal would emit for that key.
    static const struct { int rl_key; const char *seq; } key_map[] = {
        { KEY_ENTER,     "\r" },       // carriage return
        { KEY_BACKSPACE, "\177" },     // DEL (ASCII 127)
        { KEY_TAB,       "\t" },       // horizontal tab
        { KEY_ESCAPE,    "\033" },     // ESC
        { KEY_UP,        "\033[A" },   // cursor up
        { KEY_DOWN,      "\033[B" },   // cursor down
        { KEY_RIGHT,     "\033[C" },   // cursor right
        { KEY_LEFT,      "\033[D" },   // cursor left
        { KEY_HOME,      "\033[H" },   // home
        { KEY_END,       "\033[F" },   // end
        { KEY_DELETE,    "\033[3~" },  // delete forward
        { KEY_PAGE_UP,   "\033[5~" },  // page up
        { KEY_PAGE_DOWN, "\033[6~" },  // page down
    };
    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        if (IsKeyPressed(key_map[i].rl_key) || IsKeyPressedRepeat(key_map[i].rl_key))
            write(pty_fd, key_map[i].seq, strlen(key_map[i].seq));
    }

    // --- 3. Ctrl+letter combos ---
    // Control characters are 1–26 (ctrl+a through ctrl+z).  Raylib's
    // KEY_A..KEY_Z constants are contiguous, so we can loop over them.
    for (int k = KEY_A; k <= KEY_Z; k++) {
        if ((IsKeyPressed(k) || IsKeyPressedRepeat(k)) && IsKeyDown(KEY_LEFT_CONTROL)) {
            char ctrl = (char)(k - KEY_A + 1);
            write(pty_fd, &ctrl, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Resolve a style color to an RGB value using the render-state palette.
// Falls back to the given default if the color is unset.
static GhosttyColorRgb resolve_color(GhosttyStyleColor color,
                                     const GhosttyRenderStateColors *colors,
                                     GhosttyColorRgb fallback)
{
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_RGB:     return color.value.rgb;
    case GHOSTTY_STYLE_COLOR_PALETTE: return colors->palette[color.value.palette];
    default:                          return fallback;
    }
}

// Encode a single Unicode codepoint into a UTF-8 byte buffer.
// Returns the number of bytes written (1–4).
static int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

// Render the current terminal screen using the RenderState API.
//
// For each row/cell we read the grapheme codepoints and the cell's style,
// resolve foreground/background colors via the palette, and draw each
// character individually with DrawTextEx.  This supports per-cell colors
// from SGR sequences (bold, 256-color, 24-bit RGB, etc.).
//
// cell_width and cell_height are the measured dimensions of a single
// monospace glyph at the current font size, in screen (logical) pixels.
// font_size is the logical font size (before DPI scaling).
static void render_terminal(GhosttyRenderState render_state,
                            GhosttyRenderStateRowIterator row_iter,
                            GhosttyRenderStateRowCells cells,
                            Font font,
                            int cell_width, int cell_height,
                            int font_size)
{
    // Grab colors (palette, default fg/bg) from the render state so we
    // can resolve palette-indexed cell colors.
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_colors_get(render_state, &colors) != GHOSTTY_SUCCESS)
        return;

    // The bare terminal has no config, so the default fg/bg may both be
    // (0,0,0).  Fall back to standard terminal defaults (white on black)
    // so text is actually visible.
    // https://github.com/ghostty-org/ghostty/issues/11704
    if (colors.foreground.r == 0 && colors.foreground.g == 0 && colors.foreground.b == 0 &&
        colors.background.r == 0 && colors.background.g == 0 && colors.background.b == 0) {
        colors.foreground = (GhosttyColorRgb){ 255, 255, 255 };
    }

    // Populate the row iterator from the current render state snapshot.
    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter) != GHOSTTY_SUCCESS)
        return;

    // Small padding from the window edges.
    const int pad = 4;
    int y = pad;

    while (ghostty_render_state_row_iterator_next(row_iter)) {
        // Get the cells for this row (reuses the same cells handle).
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells) != GHOSTTY_SUCCESS)
            continue;

        int x = pad;

        while (ghostty_render_state_row_cells_next(cells)) {
            // How many codepoints make up the grapheme? 0 = empty cell.
            uint32_t grapheme_len = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &grapheme_len);

            if (grapheme_len == 0) {
                // The cell has no text, but it might be a bg-color-only cell.
                // Check the content tag on the raw cell to find out.
                GhosttyCell raw_cell = 0;
                ghostty_render_state_row_cells_get(cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw_cell);

                GhosttyCellContentTag content_tag = 0;
                ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_CONTENT_TAG, &content_tag);

                if (content_tag == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
                    // Palette index is stored in bits [2:9] of the packed cell.
                    uint8_t palette_idx = (uint8_t)((raw_cell >> 2) & 0xFF);
                    GhosttyColorRgb bg = colors.palette[palette_idx];
                    DrawRectangle(x, y, cell_width, cell_height,
                                  (Color){ bg.r, bg.g, bg.b, 255 });
                } else if (content_tag == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
                    // RGB is stored in bits [2:25] of the packed cell (r, g, b
                    // each 8 bits, little-endian packed struct order).
                    uint8_t r = (uint8_t)((raw_cell >> 2) & 0xFF);
                    uint8_t g = (uint8_t)((raw_cell >> 10) & 0xFF);
                    uint8_t b = (uint8_t)((raw_cell >> 18) & 0xFF);
                    DrawRectangle(x, y, cell_width, cell_height,
                                  (Color){ r, g, b, 255 });
                }

                x += cell_width;
                continue;
            }

            // Read the grapheme codepoints.
            uint32_t codepoints[16];
            uint32_t len = grapheme_len < 16 ? grapheme_len : 16;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, codepoints);

            // Build a UTF-8 string from the grapheme codepoints.
            char text[64];
            int pos = 0;
            for (uint32_t i = 0; i < len && pos < 60; i++) {
                char u8[4];
                int n = utf8_encode(codepoints[i], u8);
                memcpy(&text[pos], u8, n);
                pos += n;
            }
            text[pos] = '\0';

            // Read the style and resolve the foreground color.
            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);

            GhosttyColorRgb fg = resolve_color(style.fg_color, &colors, colors.foreground);
            Color ray_fg = { fg.r, fg.g, fg.b, 255 };

            // Draw a background rectangle if the cell has a non-default bg.
            if (style.bg_color.tag != GHOSTTY_STYLE_COLOR_NONE) {
                GhosttyColorRgb bg = resolve_color(style.bg_color, &colors, colors.background);
                DrawRectangle(x, y, cell_width, cell_height, (Color){ bg.r, bg.g, bg.b, 255 });
            }

            DrawTextEx(font, text, (Vector2){x, y}, font_size, 0, ray_fg);
            x += cell_width;
        }

        // Clear per-row dirty flag after rendering it.
        bool clean = false;
        ghostty_render_state_row_set(row_iter,
            GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);

        y += cell_height;
    }

    // Draw the cursor.
    bool cursor_visible = false;
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursor_visible);
    bool cursor_in_viewport = false;
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursor_in_viewport);

    if (cursor_visible && cursor_in_viewport) {
        uint16_t cx = 0, cy = 0;
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

        // Draw the cursor using the foreground color (or explicit cursor
        // color if the terminal set one).
        GhosttyColorRgb cur_rgb = colors.foreground;
        if (colors.cursor_has_value)
            cur_rgb = colors.cursor;
        int cur_x = pad + cx * cell_width;
        int cur_y = pad + cy * cell_height;
        DrawRectangle(cur_x, cur_y, cell_width, cell_height, (Color){ cur_rgb.r, cur_rgb.g, cur_rgb.b, 128 });
    }

    // Reset global dirty state so the next update reports changes accurately.
    GhosttyRenderStateDirty clean_state = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(render_state,
        GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean_state);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void)
{
    // Desired font size in logical (screen) points — the actual texture
    // will be rasterized at font_size * dpi_scale so glyphs stay crisp on
    // HiDPI / Retina displays.
    const int font_size = 16;

    // Enable HiDPI *before* creating the window so raylib can set up the
    // framebuffer at the native display resolution.
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);

    // Initialize window
    InitWindow(800, 600, "ghostling");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(60);

    // Query the DPI scale so we can rasterize the font at the true pixel
    // size.  On a 2× Retina display this returns {2.0, 2.0}.
    Vector2 dpi_scale = GetWindowScaleDPI();

    // Load the embedded monospace font at the native pixel size so every
    // glyph maps 1:1 to screen pixels — no texture scaling, no blur.
    int font_size_px = (int)(font_size * dpi_scale.y);
    Font mono_font = LoadFontFromMemory(".ttf", font_jetbrains_mono,
                         (int)sizeof(font_jetbrains_mono), font_size_px, NULL, 0);

    // Use bilinear filtering; the texture is already at native resolution
    // so there's no magnification blur, and this avoids jagged edges when
    // fractional positioning occurs.
    SetTextureFilter(mono_font.texture, TEXTURE_FILTER_BILINEAR);

    // Measure a representative glyph to derive the monospace cell size.
    // MeasureTextEx returns logical-pixel dimensions (already accounts for
    // the font's internal scaling), so divide by the DPI scale to get the
    // screen-space cell size we use for layout.
    Vector2 glyph_size = MeasureTextEx(mono_font, "M", font_size_px, 0);
    int cell_width  = (int)(glyph_size.x / dpi_scale.x);
    int cell_height = (int)(glyph_size.y / dpi_scale.y);

    // Small padding from window edges — must match the constant in
    // render_terminal().
    const int pad = 4;

    // Compute the initial grid from the window size and measured cell
    // metrics.
    int scr_w = GetScreenWidth();
    int scr_h = GetScreenHeight();
    uint16_t term_cols = (uint16_t)((scr_w - 2 * pad) / cell_width);
    uint16_t term_rows = (uint16_t)((scr_h - 2 * pad) / cell_height);
    if (term_cols < 1) term_cols = 1;
    if (term_rows < 1) term_rows = 1;

    // Create a ghostty virtual terminal with the computed grid and 1000
    // lines of scrollback.  This holds all the parsed screen state (cells,
    // cursor, styles, modes) but knows nothing about the pty or the window.
    GhosttyTerminal terminal;
    GhosttyTerminalOptions opts = { .cols = term_cols, .rows = term_rows, .max_scrollback = 1000 };
    GhosttyResult err = ghostty_terminal_new(NULL, &terminal, opts);
    assert(err == GHOSTTY_SUCCESS);

    // Spawn a child shell connected to a pseudo-terminal.  The master fd
    // is what we read/write; the child's stdin/stdout/stderr are wired to
    // the slave side.
    pid_t child;
    int pty_fd = pty_spawn(&child, term_cols, term_rows);
    if (pty_fd < 0)
        return 1;

    // Create the render state and its reusable iterator/cells handles once
    // up front.  These are updated each frame rather than recreated.
    GhosttyRenderState render_state = NULL;
    err = ghostty_render_state_new(NULL, &render_state);
    assert(err == GHOSTTY_SUCCESS);

    GhosttyRenderStateRowIterator row_iter = NULL;
    err = ghostty_render_state_row_iterator_new(NULL, &row_iter);
    assert(err == GHOSTTY_SUCCESS);

    GhosttyRenderStateRowCells row_cells = NULL;
    err = ghostty_render_state_row_cells_new(NULL, &row_cells);
    assert(err == GHOSTTY_SUCCESS);

    // Track window size so we only recalculate the grid on actual changes.
    int prev_width = scr_w;
    int prev_height = scr_h;

    // Each frame: handle resize → read pty → process input → render.
    while (!WindowShouldClose()) {
        // Recalculate grid dimensions when the window is resized.
        // We update both the ghostty terminal (so it reflows text) and the
        // pty's winsize (so the child shell knows about the new size and
        // can send SIGWINCH to its foreground process group).
        if (IsWindowResized()) {
            int w = GetScreenWidth();
            int h = GetScreenHeight();
            if (w != prev_width || h != prev_height) {
                int cols = (w - 2 * pad) / cell_width;
                int rows = (h - 2 * pad) / cell_height;
                if (cols < 1) cols = 1;
                if (rows < 1) rows = 1;
                term_cols = (uint16_t)cols;
                term_rows = (uint16_t)rows;
                ghostty_terminal_resize(terminal, term_cols, term_rows);
                struct winsize new_ws = { .ws_row = term_rows, .ws_col = term_cols };
                ioctl(pty_fd, TIOCSWINSZ, &new_ws);
                prev_width = w;
                prev_height = h;
            }
        }

        // Drain any pending output from the shell and update terminal state.
        pty_read(pty_fd, terminal);

        // Forward keyboard input to the shell.
        handle_input(pty_fd);

        // Snapshot the terminal state into our render state.  This is the
        // only point where we need access to the terminal; after this the
        // render state owns everything we need to draw the frame.
        ghostty_render_state_update(render_state, terminal);

        // Get the terminal's background color from the render state.
        GhosttyRenderStateColors bg_colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
        ghostty_render_state_colors_get(render_state, &bg_colors);
        Color win_bg = { bg_colors.background.r, bg_colors.background.g, bg_colors.background.b, 255 };

        // Draw the current terminal screen.
        BeginDrawing();
        ClearBackground(win_bg);
        render_terminal(render_state, row_iter, row_cells, mono_font,
                        cell_width, cell_height, font_size);
        EndDrawing();
    }

    // Cleanup
    UnloadFont(mono_font);
    CloseWindow();
    close(pty_fd);
    kill(child, SIGHUP);    // signal the child shell to exit
    waitpid(child, NULL, 0); // reap the child to avoid a zombie
    ghostty_render_state_row_cells_free(row_cells);
    ghostty_render_state_row_iterator_free(row_iter);
    ghostty_render_state_free(render_state);
    ghostty_terminal_free(terminal);
    return 0;
}
