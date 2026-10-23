/* Render the actual C UI to SVG and drive actual callbacks without hardware. */
#include "inkview.h"
#include "ui_layout.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char svg[131072], plain[32768];
static size_t used = 0, plain_used = 0;
static const ifont *font;
static int color = BLACK, closed = 0;
/* A top system panel shifts scanout and wraps bottom rows to the top. */
static int panel_type = 2;
static void (*keyboard_callback)(char *) = NULL;
static void (*timer_callback)(void) = NULL;
static char *keyboard_buffer;
static int keyboard_size;

static void append(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(svg + used, sizeof svg - used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof svg - used) {
        abort();
    }
    used += (size_t)n;
}

static void escaped(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        switch (s[i]) {
        case '&':
            append("&amp;");
            break;
        case '<':
            append("&lt;");
            break;
        case '>':
            append("&gt;");
            break;
        default:
            append("%c", s[i]);
        }
    }
}

static int screen_dimension(const char *name, int fallback) {
    const char *value = getenv(name);
    return value && atoi(value) > 0 ? atoi(value) : fallback;
}

int ScreenWidth(void) {
    return screen_dimension("EC_UI_WIDTH", UI_CANVAS_WIDTH);
}

int ScreenHeight(void) {
    return screen_dimension("EC_UI_HEIGHT", UI_CANVAS_HEIGHT);
}

ifont *OpenFont(const char *name, int size, int bold) {
    (void)name;
    (void)bold;
    ifont *f = malloc(sizeof *f);
    if (f) {
        f->size = size;
    }
    return f;
}

void CloseFont(ifont *f) {
    free(f);
}

void SetFont(const ifont *f, int value) {
    font = f;
    color = value;
}

void SetCanvas(icanvas *canvas) {
    if (canvas) {
        abort();
    }
}

int GetPanelType(void) {
    return panel_type;
}

void SetPanelType(int type) {
    panel_type = type;
}

static int displayed_y(int y) {
    int offset = panel_type == PANEL_DISABLED ? 0 : ScreenHeight() / 12;
    return (y + offset) % ScreenHeight();
}

void ClearScreen(void) {
    used = plain_used = 0;
    append("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d "
           "%d\"><rect width=\"%d\" height=\"%d\" fill=\"white\"/>",
           ScreenWidth(), ScreenHeight(), ScreenWidth(), ScreenHeight(), ScreenWidth(),
           ScreenHeight());
    plain[0] = 0;
}

char *DrawTextRect(int x, int y, int w, int h, const char *value, int flags) {
    (void)flags;
    int n = snprintf(plain + plain_used, sizeof plain - plain_used, "%s\n", value);
    if (n > 0 && (size_t)n < sizeof plain - plain_used) {
        plain_used += (size_t)n;
    }
    int chars = (int)(w / (font->size * 0.54)), row = 0;
    if (chars < 1) {
        chars = 1;
    }
    const char *p = value;
    while (*p && row * font->size * 5 / 4 + font->size <= h) {
        size_t length = strcspn(p, "\n");
        if (length > (size_t)chars) {
            length = (size_t)chars;
            size_t split = length;
            while (split && p[split] != ' ' && p[split] != '/') {
                --split;
            }
            if (split) {
                length = split;
            }
        }
        append("<text x=\"%d\" y=\"%d\" font-family=\"DejaVu Sans,Arial,sans-serif\" "
               "font-size=\"%d\" fill=\"rgb(%d,%d,%d)\">",
               x, displayed_y(y + font->size + row * font->size * 5 / 4), font->size, (color >> 16) & 255,
               (color >> 8) & 255, color & 255);
        escaped(p, length);
        append("</text>");
        p += length;
        if (*p == '\n' || *p == ' ') {
            ++p;
        }
        ++row;
    }
    return (char *)p;
}

void DrawRect(int x, int y, int w, int h, int value) {
    append("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" "
           "stroke=\"rgb(%d,%d,%d)\"/>",
           x, displayed_y(y), w, h, (value >> 16) & 255, (value >> 8) & 255, value & 255);
}

void FillArea(int x, int y, int w, int h, int value) {
    append("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"rgb(%d,%d,%d)\"/>", x, displayed_y(y), w,
           h, (value >> 16) & 255, (value >> 8) & 255, value & 255);
}

void SetClip(int x, int y, int w, int h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

void FullUpdate(void) {}

void OpenKeyboard(const char *title, char *buffer, int length, int flags,
                  void (*callback)(char *)) {
    (void)title;
    keyboard_buffer = buffer;
    keyboard_size = length;
    keyboard_callback = callback;
    printf("KEYBOARD %s\n", flags & KBD_PASSWORD ? "masked" : "plain");
}

void SetHardTimer(const char *name, void (*callback)(void), int interval) {
    (void)name;
    (void)interval;
    timer_callback = callback;
}

void ClearTimer(void (*callback)(void)) {
    if (timer_callback == callback) {
        timer_callback = NULL;
    }
}

void CloseApp(void) {
    closed = 1;
}

void InkViewMain(iv_handler handler) {
    handler(EVT_INIT, 0, 0);
    if (!closed) {
        handler(EVT_SHOW, 0, 0);
    }
    const char *path = getenv("EC_UI_EVENTS");
    FILE *in = path && strcmp(path, "-") ? fopen(path, "r") : stdin;
    char command[8192];
    while (!closed && in && fgets(command, sizeof command, in)) {
        command[strcspn(command, "\r\n")] = 0;
        int x, y;
        if (!strncmp(command, "press ", 6)) {
            ui_control_id id = ui_control_named(command + 6);
            if (id == UI_CONTROL_NONE) {
                fprintf(stderr, "Unknown UI control: %s\n", command + 6);
                abort();
            }
            ui_rect bounds = ui_control_bounds(id);
            int screen_x = (bounds.x + bounds.width / 2) * ScreenWidth() / UI_CANVAS_WIDTH;
            int screen_y = (bounds.y + bounds.height / 2) * ScreenHeight() / UI_CANVAS_HEIGHT;
            handler(EVT_POINTERUP, screen_x, screen_y);
        } else if (sscanf(command, "tap %d %d", &x, &y) == 2) {
            handler(EVT_POINTERUP, x, y);
        } else if (!strncmp(command, "key ", 4) && keyboard_callback) {
            void (*callback)(char *) = keyboard_callback;
            keyboard_callback = NULL;
            snprintf(keyboard_buffer, (size_t)keyboard_size, "%s", command + 4);
            callback(keyboard_buffer);
        } else if (!strcmp(command, "cancel") && keyboard_callback) {
            void (*callback)(char *) = keyboard_callback;
            keyboard_callback = NULL;
            callback(NULL);
        } else if (!strcmp(command, "network")) {
            setenv("EC_UI_ADDRESS", "", 1);
        } else if (!strncmp(command, "network ", 8)) {
            setenv("EC_UI_ADDRESS", command + 8, 1);
        } else if (!strcmp(command, "hide")) {
            handler(EVT_HIDE, 0, 0);
        } else if (!strcmp(command, "repaint")) {
            SetCanvas(NULL);
            ClearScreen();

            handler(EVT_REPAINT, 0, 0);
        } else if (!strcmp(command, "orientation")) {
            handler(EVT_ORIENTATION, 0, 0);
        } else if (!strcmp(command, "show")) {
            panel_type = 2; /* Firmware restores its panel on foreground entry. */
            handler(EVT_SHOW, 0, 0);
        } else if (!strcmp(command, "background")) {
            handler(EVT_BACKGROUND, 0, 0);
        } else if (!strcmp(command, "tick") && timer_callback) {
            timer_callback();
        } else if (!strncmp(command, "snapshot ", 9)) {
            FILE *out = fopen(command + 9, "w");
            if (!out) {
                abort();
            }
            fprintf(out, "%s</svg>\n", svg);
            fclose(out);
        } else if (!strncmp(command, "text ", 5)) {
            FILE *out = fopen(command + 5, "w");
            if (!out) {
                abort();
            }
            fputs(plain, out);
            fclose(out);
        } else if (!strcmp(command, "quit")) {
            closed = 1;
        }
        puts("UI_EVENT_DONE");
        fflush(stdout);
    }
    if (in && in != stdin) {
        fclose(in);
    }
    handler(EVT_EXIT, 0, 0);
}
