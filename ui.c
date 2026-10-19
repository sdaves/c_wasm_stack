#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include "db.h"
#include "font8x8.h"

#define USERNAME_CAP 32
#define BALANCE_CAP 12
#define STATUS_CAP 192
#define MAX_POST_BODY 256

typedef struct {
    float r, g, b, a;
} Color;

typedef struct {
    float x, y, w, h;
} Rect;

typedef struct {
    float x, y;
    float r, g, b, a;
} Vertex;

static const Color COLOR_TEXT       = { 0.91f, 0.96f, 0.97f, 1.00f };
static const Color COLOR_MUTED      = { 0.54f, 0.67f, 0.71f, 1.00f };
static const Color COLOR_ACCENT     = { 0.26f, 0.88f, 0.69f, 1.00f };
static const Color COLOR_ACCENT_DIM = { 0.10f, 0.47f, 0.37f, 1.00f };
static const Color COLOR_DANGER     = { 1.00f, 0.42f, 0.42f, 1.00f };
static const Color COLOR_PANEL      = { 0.035f, 0.065f, 0.090f, 0.94f };
static const Color COLOR_PANEL_EDGE = { 0.14f, 0.31f, 0.38f, 0.60f };
static const Color COLOR_FIELD      = { 0.015f, 0.040f, 0.055f, 1.00f };
static const Color COLOR_ROW        = { 0.055f, 0.090f, 0.115f, 0.80f };
static const Color COLOR_ROW_ALT    = { 0.030f, 0.055f, 0.075f, 0.80f };
static const Color COLOR_BUTTON_INK = { 0.010f, 0.060f, 0.040f, 1.00f };

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_context;
static GLuint g_program;
static GLuint g_vbo;
static GLuint g_vao;
static GLint g_resolution_location;

static Vertex *g_vertices;
static size_t g_vertex_count;
static size_t g_vertex_capacity;

static int g_width = 1280;
static int g_height = 800;
static int g_framebuffer_width = 1280;
static int g_framebuffer_height = 800;
static float g_scale = 1.0f;

static float g_content_x = 24.0f;
static float g_content_w = 960.0f;
static Rect g_form_panel;
static Rect g_list_panel;
static Rect g_user_field;
static Rect g_balance_field;
static Rect g_save_button;
static float g_row_y;
static float g_row_height = 30.0f;
static size_t g_visible_rows;
static size_t g_max_scroll;

static char g_username[USERNAME_CAP];
static size_t g_username_length;
static char g_balance[BALANCE_CAP];
static size_t g_balance_length;
static int g_focus = 0;

static UserRecord *g_users;
static size_t g_user_count;
static size_t g_scroll;

static char g_status[STATUS_CAP] = "Loading records...";
static int g_status_kind = 0;
static int g_saving = 0;
static char g_pending_name[USERNAME_CAP];
static char g_post_body[MAX_POST_BODY];

static double g_last_frame_time;
static double g_fps_accumulator;
static int g_fps_frames;
static double g_fps = 60.0;

/* ------------------------------------------------------------------ */
/* Geometry                                                           */
/* ------------------------------------------------------------------ */

static int vertex_reserve(size_t extra) {
    if (g_vertex_count + extra <= g_vertex_capacity) return 0;
    size_t capacity = g_vertex_capacity ? g_vertex_capacity * 2 : 4096;
    while (capacity < g_vertex_count + extra) capacity *= 2;
    Vertex *vertices = realloc(g_vertices, capacity * sizeof(Vertex));
    if (!vertices) return -1;
    g_vertices = vertices;
    g_vertex_capacity = capacity;
    return 0;
}

static void add_vertex(float x, float y, Color color) {
    Vertex *vertex = &g_vertices[g_vertex_count++];
    vertex->x = x * g_scale;
    vertex->y = y * g_scale;
    vertex->r = color.r;
    vertex->g = color.g;
    vertex->b = color.b;
    vertex->a = color.a;
}

static void add_quad(float x, float y, float w, float h, Color color) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (vertex_reserve(6) != 0) return;

    float x2 = x + w;
    float y2 = y + h;
    add_vertex(x,  y,  color);
    add_vertex(x2, y,  color);
    add_vertex(x2, y2, color);
    add_vertex(x,  y,  color);
    add_vertex(x2, y2, color);
    add_vertex(x,  y2, color);
}

static void add_border(Rect rect, float thickness, Color color) {
    add_quad(rect.x, rect.y, rect.w, thickness, color);
    add_quad(rect.x, rect.y + rect.h - thickness, rect.w, thickness, color);
    add_quad(rect.x, rect.y, thickness, rect.h, color);
    add_quad(rect.x + rect.w - thickness, rect.y, thickness, rect.h, color);
}

static int point_in_rect(Rect rect, float x, float y) {
    return x >= rect.x && x <= rect.x + rect.w &&
           y >= rect.y && y <= rect.y + rect.h;
}

static float text_width(const char *text, float scale) {
    return (float)strlen(text) * 8.0f * scale;
}

static void draw_glyph(float x, float y, unsigned char character, float scale,
                       Color color) {
    if (character < 32 || character > 126) character = '?';
    const unsigned char *glyph = font8x8_basic[character];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (int column = 0; column < 8; column++) {
            if (bits & (1u << column)) {
                add_quad(x + (float)column * scale,
                         y + (float)row * scale,
                         scale, scale, color);
            }
        }
    }
}

static float draw_text(float x, float y, const char *text, float scale,
                       Color color, float max_x) {
    float cursor = x;
    for (const char *p = text; *p; p++) {
        if (max_x > 0.0f && cursor + 8.0f * scale > max_x) break;
        draw_glyph(cursor, y, (unsigned char)*p, scale, color);
        cursor += 8.0f * scale;
    }
    return cursor;
}

static void draw_text_right(float right, float y, const char *text,
                            float scale, Color color) {
    draw_text(right - text_width(text, scale), y, text, scale, color, 0.0f);
}

static void draw_wrapped_text(float x, float y, float max_width, float scale,
                              float line_height, const char *text, Color color,
                              int max_lines) {
    size_t characters_per_line = (size_t)(max_width / (8.0f * scale));
    if (characters_per_line == 0) return;

    size_t length = strlen(text);
    for (int line = 0; line < max_lines; line++) {
        size_t offset = (size_t)line * characters_per_line;
        if (offset >= length) break;
        size_t count = length - offset;
        if (count > characters_per_line) count = characters_per_line;

        char chunk[STATUS_CAP];
        if (count >= sizeof(chunk)) count = sizeof(chunk) - 1;
        memcpy(chunk, text + offset, count);
        chunk[count] = '\0';
        draw_text(x, y + (float)line * line_height, chunk, scale, color, 0.0f);
    }
}

static void format_number(uint32_t value, char *out, size_t capacity) {
    char digits[16];
    int length = snprintf(digits, sizeof(digits), "%u", value);
    if (length < 0) {
        snprintf(out, capacity, "%u", value);
        return;
    }

    int commas = (length - 1) / 3;
    int total = length + commas;
    if ((size_t)total + 1 > capacity) {
        snprintf(out, capacity, "%u", value);
        return;
    }

    int position = total - 1;
    int group = 0;
    out[total] = '\0';
    for (int i = length - 1; i >= 0; i--) {
        out[position--] = digits[i];
        group++;
        if (group % 3 == 0 && i > 0) out[position--] = ',';
    }
}

/* ------------------------------------------------------------------ */
/* Application state                                                  */
/* ------------------------------------------------------------------ */

static void set_status(const char *text, int kind) {
    snprintf(g_status, sizeof(g_status), "%s", text);
    g_status_kind = kind;
}

static void clamp_scroll(void) {
    if (g_user_count <= g_visible_rows) {
        g_scroll = 0;
        g_max_scroll = 0;
    } else {
        g_max_scroll = g_user_count - g_visible_rows;
        if (g_scroll > g_max_scroll) g_scroll = g_max_scroll;
    }
}

static void append_username(char character) {
    if (g_username_length + 1 >= sizeof(g_username)) return;
    g_username[g_username_length++] = character;
    g_username[g_username_length] = '\0';
}

static void append_balance(char character) {
    if (g_balance_length + 1 >= sizeof(g_balance)) return;
    g_balance[g_balance_length++] = character;
    g_balance[g_balance_length] = '\0';
}

static void backspace_username(void) {
    if (g_username_length == 0) return;
    g_username[--g_username_length] = '\0';
}

static void backspace_balance(void) {
    if (g_balance_length == 0) return;
    g_balance[--g_balance_length] = '\0';
}

static int parse_balance_text(uint32_t *value_out) {
    if (g_balance_length == 0) return -1;
    uint64_t value = 0;
    for (size_t i = 0; i < g_balance_length; i++) {
        char character = g_balance[i];
        if (!isdigit((unsigned char)character)) return -1;
        unsigned digit = (unsigned)(character - '0');
        if (value > (UINT32_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    *value_out = (uint32_t)value;
    return 0;
}

static void url_encode(const char *source, char *destination, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    for (const unsigned char *p = (const unsigned char *)source;
         *p && out + 4 < capacity; p++) {
        unsigned char character = *p;
        if (isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            destination[out++] = (char)character;
        } else {
            destination[out++] = '%';
            destination[out++] = hex[character >> 4];
            destination[out++] = hex[character & 0x0f];
        }
    }
    destination[out] = '\0';
}

/* ------------------------------------------------------------------ */
/* Fetch API                                                          */
/* ------------------------------------------------------------------ */

static void on_users_loaded(emscripten_fetch_t *fetch) {
    if (fetch->status != 200) {
        char message[64];
        snprintf(message, sizeof(message), "Could not load records (HTTP %u).",
                 (unsigned)fetch->status);
        set_status(message, 2);
        emscripten_fetch_close(fetch);
        return;
    }

    size_t count = (size_t)(fetch->numBytes / sizeof(UserRecord));
    UserRecord *users = NULL;
    if (count > 0) {
        users = malloc(count * sizeof(UserRecord));
        if (!users) {
            set_status("Out of memory while loading records.", 2);
            emscripten_fetch_close(fetch);
            return;
        }
        memcpy(users, fetch->data, count * sizeof(UserRecord));
    }

    free(g_users);
    g_users = users;
    g_user_count = count;
    g_scroll = 0;
    clamp_scroll();

    if (g_pending_name[0]) {
        char message[STATUS_CAP];
        snprintf(message, sizeof(message), "Saved %s. %zu records loaded.",
                 g_pending_name, g_user_count);
        set_status(message, 1);
        g_pending_name[0] = '\0';
    } else {
        char message[STATUS_CAP];
        snprintf(message, sizeof(message), "Loaded %zu records.", g_user_count);
        set_status(message, 1);
    }

    emscripten_fetch_close(fetch);
}

static void on_users_error(emscripten_fetch_t *fetch) {
    set_status("Cannot load records. Is the server running?", 2);
    emscripten_fetch_close(fetch);
}

static void fetch_users(void) {
    emscripten_fetch_attr_t attributes;
    emscripten_fetch_attr_init(&attributes);
    strcpy(attributes.requestMethod, "GET");
    attributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attributes.onsuccess = on_users_loaded;
    attributes.onerror = on_users_error;
    attributes.timeoutMSecs = 5000;
    emscripten_fetch(&attributes, "/api/users.bin");
}

static void on_save_loaded(emscripten_fetch_t *fetch) {
    g_saving = 0;

    if (fetch->status != 201) {
        char message[64];
        snprintf(message, sizeof(message), "Save failed (HTTP %u).",
                 (unsigned)fetch->status);
        set_status(message, 2);
        g_pending_name[0] = '\0';
        emscripten_fetch_close(fetch);
        return;
    }

    g_username[0] = '\0';
    g_username_length = 0;
    g_balance[0] = '\0';
    g_balance_length = 0;
    fetch_users();
    emscripten_fetch_close(fetch);
}

static void on_save_error(emscripten_fetch_t *fetch) {
    g_saving = 0;
    g_pending_name[0] = '\0';
    set_status("Save failed. Is the server running?", 2);
    emscripten_fetch_close(fetch);
}

static void save_record(void) {
    if (g_saving) return;

    if (g_username_length == 0) {
        set_status("Username is required.", 2);
        g_focus = 0;
        return;
    }

    uint32_t balance;
    if (parse_balance_text(&balance) != 0) {
        set_status("Balance must be an unsigned 32-bit number.", 2);
        g_focus = 1;
        return;
    }
    (void)balance;

    char encoded_name[128];
    url_encode(g_username, encoded_name, sizeof(encoded_name));
    snprintf(g_post_body, sizeof(g_post_body), "username=%s&balance=%s",
             encoded_name, g_balance);

    snprintf(g_pending_name, sizeof(g_pending_name), "%s", g_username);
    g_saving = 1;
    set_status("Saving record...", 0);

    static const char *const headers[] = {
        "Content-Type", "application/x-www-form-urlencoded", NULL
    };

    emscripten_fetch_attr_t attributes;
    emscripten_fetch_attr_init(&attributes);
    strcpy(attributes.requestMethod, "POST");
    attributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attributes.requestHeaders = headers;
    attributes.requestData = g_post_body;
    attributes.requestDataSize = strlen(g_post_body);
    attributes.onsuccess = on_save_loaded;
    attributes.onerror = on_save_error;
    attributes.timeoutMSecs = 5000;
    emscripten_fetch(&attributes, "/api/users");
}

/* ------------------------------------------------------------------ */
/* Event handlers                                                     */
/* ------------------------------------------------------------------ */

static EM_BOOL on_resize(int event_type, const EmscriptenUiEvent *event,
                         void *user_data) {
    (void)event_type;
    (void)user_data;

    g_width = event->windowInnerWidth > 0 ? event->windowInnerWidth : g_width;
    g_height = event->windowInnerHeight > 0 ? event->windowInnerHeight : g_height;
    g_scale = (float)emscripten_get_device_pixel_ratio();
    if (g_scale < 1.0f) g_scale = 1.0f;

    g_framebuffer_width = (int)((float)g_width * g_scale);
    g_framebuffer_height = (int)((float)g_height * g_scale);
    emscripten_set_canvas_element_size("#canvas", g_framebuffer_width,
                                       g_framebuffer_height);
    glViewport(0, 0, g_framebuffer_width, g_framebuffer_height);
    return EM_TRUE;
}

static EM_BOOL on_click(int event_type, const EmscriptenMouseEvent *event,
                        void *user_data) {
    (void)event_type;
    (void)user_data;

    float x = (float)event->targetX;
    float y = (float)event->targetY;

    if (point_in_rect(g_user_field, x, y)) {
        g_focus = 0;
    } else if (point_in_rect(g_balance_field, x, y)) {
        g_focus = 1;
    } else if (point_in_rect(g_save_button, x, y)) {
        save_record();
    } else {
        g_focus = -1;
    }
    return EM_TRUE;
}

static EM_BOOL on_wheel(int event_type, const EmscriptenWheelEvent *event,
                        void *user_data) {
    (void)event_type;
    (void)user_data;

    if (g_user_count <= g_visible_rows) return EM_FALSE;

    int direction = event->deltaY > 0.0 ? 1 : -1;
    if (direction > 0 && g_scroll < g_max_scroll) {
        g_scroll++;
    } else if (direction < 0 && g_scroll > 0) {
        g_scroll--;
    }
    return EM_TRUE;
}

static EM_BOOL on_keydown(int event_type, const EmscriptenKeyboardEvent *event,
                          void *user_data) {
    (void)event_type;
    (void)user_data;

    if (event->ctrlKey || event->metaKey || event->altKey) return EM_FALSE;
    if (g_saving) return EM_TRUE;

    if (event->keyCode == 9) { /* Tab */
        if (g_focus == 0) g_focus = 1;
        else g_focus = 0;
        return EM_TRUE;
    }
    if (event->keyCode == 13) { /* Enter */
        save_record();
        return EM_TRUE;
    }
    if (event->keyCode == 27) { /* Escape */
        g_focus = -1;
        return EM_TRUE;
    }
    if (event->keyCode == 8) { /* Backspace */
        if (g_focus == 0) backspace_username();
        else if (g_focus == 1) backspace_balance();
        return EM_TRUE;
    }

    if (g_focus < 0) return EM_FALSE;

    size_t length = strlen(event->key);
    if (length == 1) {
        unsigned char character = (unsigned char)event->key[0];
        if (character >= 32 && character < 127) {
            if (g_focus == 0) {
                append_username((char)character);
            } else if (isdigit(character)) {
                append_balance((char)character);
            }
            return EM_TRUE;
        }
    }

    return EM_FALSE;
}

/* ------------------------------------------------------------------ */
/* Layout and rendering                                               */
/* ------------------------------------------------------------------ */

static void compute_layout(void) {
    float margin = 24.0f;
    float content_width = (float)g_width - margin * 2.0f;
    if (content_width > 1000.0f) content_width = 1000.0f;
    if (content_width < 260.0f) content_width = (float)g_width - 16.0f;

    g_content_w = content_width;
    g_content_x = ((float)g_width - content_width) * 0.5f;

    float title_scale = g_width < 640 ? 3.0f : 4.0f;
    float top = 52.0f + 8.0f * title_scale + 12.0f + 8.0f * 1.4f + 26.0f;

    if (content_width >= 820.0f) {
        float form_width = 360.0f;
        float gap = 18.0f;
        float list_width = content_width - form_width - gap;
        float list_height = (float)g_height - top - 24.0f;
        if (list_height > 480.0f) list_height = 480.0f;
        if (list_height < 260.0f) list_height = 260.0f;

        g_form_panel = (Rect){ g_content_x, top, form_width, 370.0f };
        g_list_panel = (Rect){ g_content_x + form_width + gap, top,
                               list_width, list_height };
    } else {
        float list_y = top + 370.0f + 18.0f;
        float list_height = (float)g_height - list_y - 24.0f;
        if (list_height > 420.0f) list_height = 420.0f;
        if (list_height < 240.0f) list_height = 240.0f;

        g_form_panel = (Rect){ g_content_x, top, content_width, 370.0f };
        g_list_panel = (Rect){ g_content_x, list_y, content_width, list_height };
    }

    g_user_field = (Rect){ g_form_panel.x + 20.0f, g_form_panel.y + 76.0f,
                           g_form_panel.w - 40.0f, 42.0f };
    g_balance_field = (Rect){ g_form_panel.x + 20.0f, g_form_panel.y + 154.0f,
                              g_form_panel.w - 40.0f, 42.0f };
    g_save_button = (Rect){ g_form_panel.x + 20.0f, g_form_panel.y + 220.0f,
                            g_form_panel.w - 40.0f, 44.0f };

    g_row_y = g_list_panel.y + 84.0f;
    float available = g_list_panel.h - 110.0f;
    g_visible_rows = available > 0.0f ? (size_t)(available / g_row_height) : 0;
    clamp_scroll();
}

static void draw_panel(Rect rect) {
    add_quad(rect.x, rect.y, rect.w, rect.h, COLOR_PANEL);
    add_border(rect, 1.0f, COLOR_PANEL_EDGE);
}

static void draw_field(Rect rect, int focused, const char *text,
                       const char *placeholder) {
    add_quad(rect.x, rect.y, rect.w, rect.h, COLOR_FIELD);
    add_border(rect, focused ? 1.5f : 1.0f,
               focused ? COLOR_ACCENT : COLOR_PANEL_EDGE);

    float scale = 2.0f;
    float padding = 12.0f;
    float text_y = rect.y + (rect.h - 8.0f * scale) * 0.5f;
    float max_x = rect.x + rect.w - padding;
    size_t max_characters = (size_t)((rect.w - padding * 2.0f) /
                                     (8.0f * scale));

    const char *visible = text;
    size_t length = strlen(text);
    if (length > max_characters && max_characters > 0) {
        visible = text + (length - max_characters);
    }

    float end_x;
    if (length == 0) {
        if (focused) {
            end_x = rect.x + padding;
        } else {
            end_x = draw_text(rect.x + padding, text_y, placeholder, scale,
                              COLOR_MUTED, max_x);
        }
    } else {
        end_x = draw_text(rect.x + padding, text_y, visible, scale,
                          COLOR_TEXT, max_x);
    }

    if (focused && ((long)(emscripten_get_now() / 500.0) % 2 == 0)) {
        add_quad(end_x + 1.0f, rect.y + 10.0f, 2.0f, rect.h - 20.0f,
                 COLOR_ACCENT);
    }
}

static void draw_button(Rect rect, const char *label, int disabled) {
    Color fill = disabled ? COLOR_ACCENT_DIM : COLOR_ACCENT;
    add_quad(rect.x, rect.y, rect.w, rect.h, fill);
    add_border(rect, 1.0f, COLOR_ACCENT);

    float scale = 2.0f;
    float x = rect.x + (rect.w - text_width(label, scale)) * 0.5f;
    float y = rect.y + (rect.h - 8.0f * scale) * 0.5f;
    draw_text(x, y, label, scale, COLOR_BUTTON_INK, 0.0f);
}

static void draw_header(void) {
    float title_scale = g_width < 640 ? 3.0f : 4.0f;
    float max_x = g_content_x + g_content_w;

    draw_text(g_content_x, 52.0f, "C/WASM RECORD STORE", title_scale,
              COLOR_TEXT, max_x);
    draw_text(g_content_x, 52.0f + 8.0f * title_scale + 12.0f,
              "ADD A RECORD - SAVED TO data.bin - RELOAD SHOWS IT",
              1.4f, COLOR_MUTED, max_x);
}

static void draw_form(void) {
    Rect panel = g_form_panel;
    draw_panel(panel);

    draw_text(panel.x + 20.0f, panel.y + 18.0f, "ADD A RECORD", 2.0f,
              COLOR_TEXT, panel.x + panel.w - 20.0f);

    draw_text(panel.x + 20.0f, panel.y + 54.0f, "USERNAME", 1.5f,
              COLOR_MUTED, 0.0f);
    draw_field(g_user_field, g_focus == 0, g_username, "Carol");

    draw_text(panel.x + 20.0f, panel.y + 132.0f, "BALANCE", 1.5f,
              COLOR_MUTED, 0.0f);
    draw_field(g_balance_field, g_focus == 1, g_balance, "1000");

    draw_button(g_save_button, g_saving ? "SAVING..." : "SAVE RECORD", g_saving);

    Color status_color = COLOR_MUTED;
    if (g_status_kind == 1) status_color = COLOR_ACCENT;
    else if (g_status_kind == 2) status_color = COLOR_DANGER;
    draw_wrapped_text(panel.x + 20.0f, panel.y + 284.0f,
                      panel.w - 40.0f, 1.4f, 18.0f,
                      g_status, status_color, 3);

    draw_text(panel.x + 20.0f, panel.y + panel.h - 22.0f,
              "CLICK OR TAB - ENTER SAVES", 1.1f,
              COLOR_MUTED, panel.x + panel.w - 20.0f);
}

static void draw_records(void) {
    Rect panel = g_list_panel;
    draw_panel(panel);

    draw_text(panel.x + 20.0f, panel.y + 18.0f, "STORED RECORDS", 2.0f,
              COLOR_TEXT, panel.x + panel.w - 20.0f);

    char count_text[32];
    snprintf(count_text, sizeof(count_text), "%zu record%s", g_user_count,
             g_user_count == 1 ? "" : "s");
    draw_text_right(panel.x + panel.w - 20.0f, panel.y + 20.0f,
                    count_text, 1.4f, COLOR_MUTED);

    draw_text(panel.x + 20.0f, panel.y + 56.0f, "ID", 1.4f, COLOR_MUTED, 0.0f);
    draw_text(panel.x + 84.0f, panel.y + 56.0f, "USERNAME", 1.4f,
              COLOR_MUTED, 0.0f);
    draw_text_right(panel.x + panel.w - 20.0f, panel.y + 56.0f, "BALANCE",
                    1.4f, COLOR_MUTED);
    add_quad(panel.x + 12.0f, panel.y + 78.0f, panel.w - 24.0f, 1.0f,
             COLOR_PANEL_EDGE);

    if (g_user_count == 0) {
        const char *empty = "NO RECORDS YET";
        float scale = 1.6f;
        float x = panel.x + (panel.w - text_width(empty, scale)) * 0.5f;
        draw_text(x, panel.y + 130.0f, empty, scale, COLOR_MUTED, 0.0f);
        return;
    }

    size_t end = g_scroll + g_visible_rows;
    if (end > g_user_count) end = g_user_count;

    for (size_t i = g_scroll; i < end; i++) {
        const UserRecord *record = &g_users[i];
        float y = g_row_y + (float)(i - g_scroll) * g_row_height;

        add_quad(panel.x + 12.0f, y - 5.0f, panel.w - 24.0f, 28.0f,
                 (i & 1) ? COLOR_ROW_ALT : COLOR_ROW);

        char id_text[32];
        snprintf(id_text, sizeof(id_text), "%llu",
                 (unsigned long long)record->id);
        draw_text(panel.x + 20.0f, y, id_text, 1.4f, COLOR_MUTED, 0.0f);

        draw_text(panel.x + 84.0f, y, record->username, 1.4f, COLOR_TEXT,
                  panel.x + panel.w - 170.0f);

        char balance_text[32];
        format_number(record->balance, balance_text, sizeof(balance_text));
        draw_text_right(panel.x + panel.w - 20.0f, y, balance_text, 1.4f,
                        COLOR_ACCENT);
    }

    if (g_user_count > g_visible_rows) {
        float track_x = panel.x + panel.w - 8.0f;
        float track_y = g_row_y;
        float track_height = panel.h - 110.0f;
        add_quad(track_x, track_y, 3.0f, track_height, COLOR_PANEL_EDGE);

        float thumb_height = track_height *
                             (float)g_visible_rows / (float)g_user_count;
        float travel = track_height - thumb_height;
        float offset = g_max_scroll > 0
            ? travel * (float)g_scroll / (float)g_max_scroll
            : 0.0f;
        add_quad(track_x, track_y + offset, 3.0f, thumb_height,
                 COLOR_ACCENT_DIM);
    }
}

static void draw_hud(void) {
    char hud[64];
    snprintf(hud, sizeof(hud), "WEBGL 2.0 | %.0f FPS", g_fps);
    float scale = 1.3f;
    draw_text((float)g_width - text_width(hud, scale) - 16.0f, 12.0f,
              hud, scale, COLOR_MUTED, 0.0f);
}

static void update_fps(void) {
    double now = emscripten_get_now();
    double delta = now - g_last_frame_time;
    g_last_frame_time = now;

    if (delta > 0.0) {
        g_fps_accumulator += delta;
        g_fps_frames++;
        if (g_fps_frames >= 60) {
            double average = g_fps_accumulator / 60.0;
            g_fps = average > 0.0 ? 1000.0 / average : 0.0;
            printf("[UI BENCHMARK] Performance: %.2f FPS | Latency: %.3f ms\n",
                   g_fps, average);
            g_fps_accumulator = 0.0;
            g_fps_frames = 0;
        }
    }
}

static void render_frame(void) {
    update_fps();
    compute_layout();

    g_vertex_count = 0;
    draw_header();
    draw_form();
    draw_records();
    draw_hud();

    glClearColor(0.02f, 0.04f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(g_program);
    glUniform2f(g_resolution_location,
                (float)g_framebuffer_width, (float)g_framebuffer_height);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(g_vertex_count * sizeof(Vertex)),
                 g_vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_vertex_count);
}

/* ------------------------------------------------------------------ */
/* WebGL setup                                                        */
/* ------------------------------------------------------------------ */

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("Shader compile error: %s\n", log);
        return 0;
    }
    return shader;
}

static int init_gl(void) {
    static const char *vertex_source =
        "#version 300 es\n"
        "layout(location=0) in vec2 a_pos;\n"
        "layout(location=1) in vec4 a_color;\n"
        "uniform vec2 u_resolution;\n"
        "out vec4 v_color;\n"
        "void main() {\n"
        "  vec2 p = a_pos / u_resolution;\n"
        "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
        "  v_color = a_color;\n"
        "}\n";

    static const char *fragment_source =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec4 v_color;\n"
        "out vec4 out_color;\n"
        "void main() { out_color = v_color; }\n";

    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex_shader || !fragment_shader) return -1;

    g_program = glCreateProgram();
    glAttachShader(g_program, vertex_shader);
    glAttachShader(g_program, fragment_shader);
    glLinkProgram(g_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(g_program, sizeof(log), NULL, log);
        printf("Program link error: %s\n", log);
        return -1;
    }

    g_resolution_location = glGetUniformLocation(g_program, "u_resolution");

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)(sizeof(float) * 2));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    return 0;
}

int main(void) {
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.antialias = EM_TRUE;

    g_context = emscripten_webgl_create_context("#canvas", &attributes);
    if (!g_context) {
        printf("WebGL 2.0 context creation failed.\n");
        return 1;
    }
    emscripten_webgl_make_context_current(g_context);

    double css_width = 1280.0;
    double css_height = 800.0;
    if (emscripten_get_element_css_size("#canvas", &css_width, &css_height) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        css_width < 1.0 || css_height < 1.0) {
        css_width = 1280.0;
        css_height = 800.0;
    }
    g_width = (int)css_width;
    g_height = (int)css_height;
    g_scale = (float)emscripten_get_device_pixel_ratio();
    if (g_scale < 1.0f) g_scale = 1.0f;
    g_framebuffer_width = (int)((float)g_width * g_scale);
    g_framebuffer_height = (int)((float)g_height * g_scale);
    emscripten_set_canvas_element_size("#canvas", g_framebuffer_width,
                                       g_framebuffer_height);
    glViewport(0, 0, g_framebuffer_width, g_framebuffer_height);

    if (init_gl() != 0) {
        printf("WebGL initialization failed.\n");
        return 1;
    }

    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                   EM_FALSE, on_resize);
    emscripten_set_click_callback("#canvas", NULL, EM_FALSE, on_click);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                    EM_FALSE, on_keydown);
    emscripten_set_wheel_callback("#canvas", NULL, EM_FALSE, on_wheel);

    g_last_frame_time = emscripten_get_now();
    set_status("Loading records...", 0);
    fetch_users();

    printf("WebGL 2.0 record store active.\n");
    emscripten_set_main_loop(render_frame, 0, 1);
    return 0;
}
