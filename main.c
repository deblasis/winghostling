#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "raylib.h"
#include <ghostty/vt.h>

int main(void)
{
    // Create an 80x24 terminal
    GhosttyTerminal terminal;
    GhosttyTerminalOptions opts = { .cols = 80, .rows = 24, .max_scrollback = 1000 };
    GhosttyResult err = ghostty_terminal_new(NULL, &terminal, opts);
    assert(err == GHOSTTY_SUCCESS);

    // Feed some sample data
    const char *cmds[] = {
        "Hello from \033[1mghostling\033[0m!\r\n",
        "Powered by \033[32mlibghostty-vt\033[0m + \033[34mraylib\033[0m\r\n",
        "\033[31mRed\033[0m \033[33mYellow\033[0m \033[36mCyan\033[0m\r\n",
    };
    for (size_t i = 0; i < 3; i++)
        ghostty_terminal_vt_write(terminal, (const uint8_t *)cmds[i], strlen(cmds[i]));

    InitWindow(800, 600, "ghostling");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(BLACK);

        // Render terminal contents as plain text for now
        GhosttyFormatterTerminalOptions fmt = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
        fmt.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
        fmt.trim = true;

        GhosttyFormatter formatter;
        if (ghostty_formatter_terminal_new(NULL, &formatter, terminal, fmt) == GHOSTTY_SUCCESS) {
            uint8_t *buf = NULL;
            size_t len = 0;
            if (ghostty_formatter_format_alloc(formatter, NULL, &buf, &len) == GHOSTTY_SUCCESS) {
                // Draw each line
                int y = 10;
                const char *line_start = (const char *)buf;
                for (size_t i = 0; i <= len; i++) {
                    if (i == len || buf[i] == '\n') {
                        int line_len = (int)((const char *)&buf[i] - line_start);
                        char line[256] = {0};
                        if (line_len > 0 && line_len < 255) {
                            memcpy(line, line_start, line_len);
                            line[line_len] = '\0';
                            DrawText(line, 10, y, 16, GREEN);
                        }
                        y += 18;
                        line_start = (const char *)&buf[i + 1];
                    }
                }
                free(buf);
            }
            ghostty_formatter_free(formatter);
        }

        EndDrawing();
    }

    CloseWindow();
    ghostty_terminal_free(terminal);
    return 0;
}
