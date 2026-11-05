/* Render the actual C UI to SVG and drive actual callbacks without hardware. */
#include "inkview.h"
#include "ui_layout.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
static char svg[2097152], plain[32768];
static size_t used = 0, plain_used = 0;
static const ifont *font;
static int color = BLACK, closed = 0;
static int displayed_y(int y);
/* A top system panel shifts scanout and wraps bottom rows to the top. */
static int panel_type = 2;
static void (*keyboard_callback)(char *) = NULL;
static void (*timer_callback)(void) = NULL;
static void (*dialog_callback)(int);
static void (*progress_callback)(int);
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

const char *GetDeviceModel(void) {
    const char *model = getenv("EC_UI_MODEL");
    return model ? model : "PB632";
}

const char *GetSoftwareVersion(void) {
    const char *firmware = getenv("EC_UI_FIRMWARE");
    return firmware ? firmware : "U632.6.11.2001";
}

void Dialog(int icon,const char *title,const char *text,const char *yes,const char *no,void (*callback)(int)) {
    (void)icon;
    printf("DIALOG %s: %s [%s/%s]\n",title,text,yes,no?no:"");dialog_callback=callback;
}
void OpenProgressbar(int icon,const char *title,const char *text,int percent,void (*callback)(int)) {
    (void)icon;printf("PROGRESS %s: %s %d\n",title,text,percent);progress_callback=callback;
}
void UpdateProgressbar(const char *text,int percent){printf("PROGRESS %s %d\n",text,percent);}
void CloseProgressbar(void){puts("PROGRESS closed");progress_callback=NULL;}
void Message(int icon, const char *title, const char *message, int timeout) {
    (void)icon; (void)timeout;
    printf("MESSAGE %s: %s\n", title, message);
}

int ScreenHeight(void) {
    return screen_dimension("EC_UI_HEIGHT", UI_CANVAS_HEIGHT);
}

static int theme_integer(const char *key, int fallback) {
    const char *reference = getenv("EC_UI_THEME_REFERENCE");
    if (reference) {
        char path[4096], line[2048];
        snprintf(path, sizeof path, "%s/theme.cfg", reference);
        FILE *in = fopen(path, "r");
        if (in) {
            while (fgets(line, sizeof line, in)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (!strncmp(p, key, strlen(key)) && p[strlen(key)] == '=') {
                    int value = (int)strtol(p + strlen(key) + 1, NULL, 0);
                    fclose(in); return value;
                }
            }
            fclose(in);
        }
    }
    return fallback;
}

static int theme_scale(int value) {
    int side = ScreenWidth() < ScreenHeight() ? ScreenWidth() : ScreenHeight();
    return value * side / 1072;
}

int GetThemeInt(const char *name, int fallback) {
    if (!strcmp(name, "application.caption.height")) fallback = theme_scale(105);
    if (!strcmp(name, "application.caption.button.left.indent.horizontal")) fallback = 0;
    if (!strcmp(name, "application.caption.button.right.indent.horizontal")) fallback = theme_scale(12);
    return theme_integer(name, fallback);
}

int GetCaptionHeight(void) { return GetThemeInt("application.caption.height", 70); }

ibitmap *GetResource(const char *name, const ibitmap *fallback) {
    static const char *names[] = {"home", "refresh", "ci_diagnostic"};
    static ibitmap *resources[3];
    const char *missing = getenv("EC_UI_MISSING_RESOURCE");
    if (missing && !strcmp(missing, name)) return (ibitmap *)fallback;
    for (size_t i = 0; i < 3; ++i) {
        if (strcmp(name, names[i])) continue;
        if (resources[i]) return resources[i];
        const char *reference = getenv("EC_UI_THEME_REFERENCE");
        if (reference) {
            char path[4096]; snprintf(path, sizeof path, "%s/%s.bitmap", reference, name);
            FILE *in = fopen(path, "rb");
            if (!in) return (ibitmap *)fallback;
            if (fseek(in, 0, SEEK_END)) abort();
            long size = ftell(in);
            rewind(in);
            if (size < 8 || size > 1024 * 1024) abort();
            resources[i] = malloc((size_t)size);
            if (!resources[i] || fread(resources[i], 1, (size_t)size, in) != (size_t)size) abort();
            fclose(in);
        } else {
            int side = theme_scale(94);
            size_t size = sizeof(ibitmap) + (size_t)side * (size_t)side;
            resources[i] = malloc(size);
            if (!resources[i]) abort();
            resources[i]->width = resources[i]->height = (unsigned short)side;
            resources[i]->depth = 8;
            resources[i]->scanline = (unsigned short)side;
            memset(resources[i]->data, 255, (size_t)side * (size_t)side);
        }
        return resources[i];
    }
    return (ibitmap *)fallback;
}

void DrawBitmap(int x, int y, const ibitmap *bitmap) {
    const char *name = "unknown";
    const char *names[] = {"home", "refresh", "ci_diagnostic"};
    for (size_t i = 0; i < 3; ++i) if (GetResource(names[i], NULL) == bitmap) name = names[i];
    append("<g data-resource=\"%s\" data-x=\"%d\" data-y=\"%d\" data-width=\"%d\" data-height=\"%d\">",
           name, x, displayed_y(y), bitmap->width, bitmap->height);
    if (bitmap->depth != 8) abort();
    for (unsigned row = 0; row < bitmap->height; ++row) {
        unsigned column = 0;
        while (column < bitmap->width) {
            unsigned start = column;
            unsigned shade = bitmap->data[row * bitmap->scanline + column++];
            while (column < bitmap->width && bitmap->data[row * bitmap->scanline + column] == shade) ++column;
            if (shade != 255)
                append("<rect x=\"%d\" y=\"%d\" width=\"%u\" height=\"1\" fill=\"rgb(%u,%u,%u)\"/>",
                       x + (int)start, displayed_y(y + (int)row), column - start, shade, shade, shade);
        }
    }
    append("</g>");
}

void DimArea(int x, int y, int w, int h, int value) {
    append("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"rgb(%d,%d,%d)\" fill-opacity=\".6\"/>",
           x, displayed_y(y), w, h, (value >> 16) & 255, (value >> 8) & 255, value & 255);
}

void DrawApplicationCaption(const char *caption, const irect *rect) {
    int height = GetCaptionHeight();
    int size = theme_scale(34);
    const char *reference = getenv("EC_UI_THEME_REFERENCE");
    if (reference) {
        char path[4096], line[2048];
        snprintf(path, sizeof path, "%s/theme.cfg", reference);
        FILE *in = fopen(path, "r");
        if (in) {
            while (fgets(line, sizeof line, in))
                if (!strncmp(line, "application.caption.font=", 25)) {
                    char *comma = strchr(line, ',');
                    if (comma) size = atoi(comma + 1);
                }
            fclose(in);
        }
    }
    FillArea(0, 0, ScreenWidth(), height, WHITE);
    FillArea(0, height - 1, ScreenWidth(), 1, BLACK);
    char title[256]; snprintf(title, sizeof title, "%s", caption);
    if (theme_integer("application.caption.text.uppercase", 1))
        for (char *p = title; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    int count = snprintf(plain + plain_used, sizeof plain - plain_used, "%s\n", title);
    if (count > 0 && (size_t)count < sizeof plain - plain_used) plain_used += (size_t)count;
    append("<text data-theme-font=\"application.caption.font\" x=\"%d\" y=\"%d\" "
           "font-family=\"Roboto,Arial,sans-serif\" font-size=\"%d\" font-weight=\"normal\" "
           "text-anchor=\"middle\" dominant-baseline=\"central\" fill=\"black\">",
           rect->x + rect->w / 2, displayed_y(rect->y + rect->h / 2), size);
    escaped(title, strlen(title)); append("</text>");
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
        append("<text x=\"%d\" y=\"%d\" font-family=\"Roboto,Arial,sans-serif\" "
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

void DrawLine(int x1, int y1, int x2, int y2, int value) {
    append("<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"rgb(%d,%d,%d)\"/>",
           x1, displayed_y(y1), x2, displayed_y(y2), (value >> 16) & 255,
           (value >> 8) & 255, value & 255);
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
    /* Firmware may own helper children independently of SSH/FTP. */
    pid_t helper = 0;
    if (getenv("EC_UI_HELPER")) {
        pid_t parent = getpid();
        helper = fork();
        if (!helper) {
#ifdef __linux__
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(1);
#else
            (void)parent;
#endif
            for (;;) pause();
        }
        if (helper < 0) abort();
    }
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
            int screen_x, screen_y;
            if (id == UI_EXIT || id == UI_REFRESH_LIBRARY || id == UI_SAVE_DIAGNOSTICS) {
                int slot = GetResource("home", NULL)->width;
                if (GetResource("refresh", NULL)->width > slot) slot = GetResource("refresh", NULL)->width;
                if (GetResource("ci_diagnostic", NULL)->width > slot) slot = GetResource("ci_diagnostic", NULL)->width;
                bounds = ui_toolbar_bounds(id, ScreenWidth(), GetCaptionHeight(), slot,
                    GetThemeInt("application.caption.button.left.indent.horizontal",0),
                    GetThemeInt("application.caption.button.right.indent.horizontal",0));
                screen_x = bounds.x + bounds.width / 2;
                screen_y = bounds.y + bounds.height / 2;
            } else {
                screen_x = (bounds.x + bounds.width / 2) * ScreenWidth() / UI_CANVAS_WIDTH;
                screen_y = GetCaptionHeight() + (bounds.y + bounds.height / 2 - UI_BODY_TOP) *
                           (ScreenHeight() - GetCaptionHeight()) / (UI_CANVAS_HEIGHT - UI_BODY_TOP);
            }
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
        } else if (!strncmp(command,"dialog ",7) && dialog_callback) {
            void (*callback)(int)=dialog_callback;dialog_callback=NULL;callback(atoi(command+7));
        } else if (!strcmp(command,"cancel-update") && progress_callback) {
            void (*callback)(int)=progress_callback;progress_callback=NULL;callback(0);
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
        } else if (!strcmp(command, "foreground")) {
            panel_type = 2;
            handler(EVT_FOREGROUND, 0, 0);
        } else if (!strcmp(command, "activate")) {
            panel_type = 2;
            handler(EVT_ACTIVATE, 0, 0);
        } else if (!strcmp(command, "focus")) {
            panel_type = 2;
            handler(EVT_FOCUS, 0, 0);
        } else if (!strcmp(command, "background")) {
            handler(EVT_BACKGROUND, 0, 0);
        } else if (!strcmp(command, "tick") && timer_callback) {
            timer_callback();
        } else if (sscanf(command, "wait %d", &x) == 1 && x >= 0 && x <= 5000) {
            struct timespec delay = {x / 1000, (x % 1000) * 1000000};
            nanosleep(&delay, NULL);
            if (timer_callback) {
                timer_callback();
            }
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
        } else if (!strcmp(command, "back")) {
            handler(EVT_KEYPRESS, KEY_BACK, 0);
        }
        puts("UI_EVENT_DONE");
        fflush(stdout);
    }
    if (in && in != stdin) {
        fclose(in);
    }
    handler(EVT_EXIT, 0, 0);
    if (helper) {
        int status;
        if (waitpid(helper, &status, WNOHANG) != 0) {
            fprintf(stderr, "InkView helper was killed or reaped by service cleanup\n");
            exit(1);
        }
        kill(helper, SIGTERM);
        waitpid(helper, NULL, 0);
    }
}
