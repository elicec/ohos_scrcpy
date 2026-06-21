/*
 * OHOS Scrcpy 客户端 - 右侧快捷工具栏实现
 * 使用 OpenGL 3.3 Core 渲染纯色按钮与图标
 */

#include "toolbar.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <SDL2/SDL_opengl.h>

#define TOOLBAR_TAG "Toolbar"

static const char *SOLID_VERTEX_SHADER =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *SOLID_FRAGMENT_SHADER =
    "#version 330 core\n"
    "out vec4 FragColor;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "    FragColor = uColor;\n"
    "}\n";

static const float COLOR_BG[4]       = {0.18f, 0.18f, 0.18f, 1.0f};
static const float COLOR_BG_HOVER[4] = {0.30f, 0.30f, 0.30f, 1.0f};
static const float COLOR_BG_ACTIVE[4]= {0.15f, 0.50f, 0.85f, 1.0f};
static const float COLOR_ICON[4]     = {0.95f, 0.95f, 0.95f, 1.0f};

#define BUTTON_MARGIN 6
#define BUTTON_HEIGHT 54
#define ICON_MARGIN 10

typedef struct {
    int x, y;
    int width, height;
} ToolbarButton;

typedef struct {
    int windowWidth;
    int windowHeight;
    int toolbarX;
    GLuint program;
    GLuint vao, vbo;
    ToolbarButton buttons[TOOLBAR_ACTION_COUNT];
    int hoverIndex;
    int activeIndex;
} ToolbarContext;

static ToolbarContext g_toolbarCtx = {0};

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOG_TAG_E(TOOLBAR_TAG, "Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(const char *vertSrc, const char *fragSrc)
{
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vertSrc);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vert || !frag) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        LOG_TAG_E(TOOLBAR_TAG, "Program link failed: %s", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

static void ndc_from_window(float x, float y, float *nx, float *ny)
{
    float w = (float)g_toolbarCtx.windowWidth;
    float h = (float)g_toolbarCtx.windowHeight;
    *nx = (x / w) * 2.0f - 1.0f;
    *ny = 1.0f - (y / h) * 2.0f;
}

static void set_color(const float color[4])
{
    GLint loc = glGetUniformLocation(g_toolbarCtx.program, "uColor");
    glUniform4f(loc, color[0], color[1], color[2], color[3]);
}

static void draw_rect(float x, float y, float w, float h)
{
    float x1, y1, x2, y2;
    ndc_from_window(x, y, &x1, &y1);
    ndc_from_window(x + w, y + h, &x2, &y2);

    float vertices[] = {
        x1, y1,
        x1, y2,
        x2, y2,
        x2, y1,
    };

    glBindBuffer(GL_ARRAY_BUFFER, g_toolbarCtx.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(g_toolbarCtx.vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

static void draw_rect_lines(float x, float y, float w, float h, float thickness)
{
    /* 使用 4 个填充矩形模拟带厚度的边框 */
    draw_rect(x, y, w, thickness);             /* 上 */
    draw_rect(x, y + h - thickness, w, thickness); /* 下 */
    draw_rect(x, y, thickness, h);             /* 左 */
    draw_rect(x + w - thickness, y, thickness, h); /* 右 */
}

static void draw_triangle(float x1, float y1, float x2, float y2, float x3, float y3)
{
    float nx1, ny1, nx2, ny2, nx3, ny3;
    ndc_from_window(x1, y1, &nx1, &ny1);
    ndc_from_window(x2, y2, &nx2, &ny2);
    ndc_from_window(x3, y3, &nx3, &ny3);

    float vertices[] = {
        nx1, ny1,
        nx2, ny2,
        nx3, ny3,
    };

    glBindBuffer(GL_ARRAY_BUFFER, g_toolbarCtx.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(g_toolbarCtx.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void draw_circle(float cx, float cy, float radius, int segments)
{
    float vertices[(32 + 2) * 2];
    int count = segments > 32 ? 32 : segments;
    if (count < 3) count = 3;

    float nx, ny;
    ndc_from_window(cx, cy, &nx, &ny);
    vertices[0] = nx;
    vertices[1] = ny;

    float nx1, ny1;
    ndc_from_window(cx + radius, cy, &nx1, &ny1);
    float scaleX = nx1 - nx;
    float scaleY = ny1 - ny;  /* 负值，因为 NDC y 方向与窗口 y 相反 */

    for (int i = 0; i <= count; i++) {
        float theta = (float)i / (float)count * 2.0f * (float)M_PI;
        vertices[(i + 1) * 2 + 0] = nx + cosf(theta) * scaleX;
        vertices[(i + 1) * 2 + 1] = ny + sinf(theta) * scaleY;
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_toolbarCtx.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * (count + 2) * 2, vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(g_toolbarCtx.vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, count + 2);
}

static void draw_circle_outline(float cx, float cy, float radius, float thickness, int segments)
{
    /* 用多个小矩形模拟圆环 */
    int count = segments > 32 ? 32 : segments;
    if (count < 3) count = 3;

    for (int i = 0; i < count; i++) {
        float theta1 = (float)i / (float)count * 2.0f * (float)M_PI;
        float theta2 = (float)(i + 1) / (float)count * 2.0f * (float)M_PI;

        float wx1 = cx + cosf(theta1) * radius;
        float wy1 = cy + sinf(theta1) * radius;
        float wx2 = cx + cosf(theta2) * radius;
        float wy2 = cy + sinf(theta2) * radius;

        /* 线段中点 */
        float mx = (wx1 + wx2) * 0.5f;
        float my = (wy1 + wy2) * 0.5f;

        /* 线段长度 */
        float len = sqrtf((wx2 - wx1) * (wx2 - wx1) + (wy2 - wy1) * (wy2 - wy1));
        if (len < 0.001f) continue;

        /* 垂直方向 */
        float dx = (wx2 - wx1) / len;
        float dy = (wy2 - wy1) / len;

        /* 厚度在垂直方向的分量（近似） */
        float tx = -dy * thickness;
        float ty = dx * thickness;

        draw_triangle(
            mx - dx * len * 0.5f - tx, my - dy * len * 0.5f - ty,
            mx + dx * len * 0.5f - tx, my + dy * len * 0.5f - ty,
            mx + dx * len * 0.5f + tx, my + dy * len * 0.5f + ty
        );
        draw_triangle(
            mx - dx * len * 0.5f - tx, my - dy * len * 0.5f - ty,
            mx + dx * len * 0.5f + tx, my + dy * len * 0.5f + ty,
            mx - dx * len * 0.5f + tx, my - dy * len * 0.5f + ty
        );
    }
}

static void layout_buttons(void)
{
    int startY = BUTTON_MARGIN;
    int x = g_toolbarCtx.toolbarX + BUTTON_MARGIN;
    int w = TOOLBAR_WIDTH - BUTTON_MARGIN * 2;

    for (int i = 0; i < TOOLBAR_ACTION_COUNT; i++) {
        g_toolbarCtx.buttons[i].x = x;
        g_toolbarCtx.buttons[i].y = startY + i * (BUTTON_HEIGHT + BUTTON_MARGIN);
        g_toolbarCtx.buttons[i].width = w;
        g_toolbarCtx.buttons[i].height = BUTTON_HEIGHT;
    }
}

static void draw_back_icon(const ToolbarButton *btn)
{
    int cx = btn->x + btn->width / 2;
    int cy = btn->y + btn->height / 2;
    int size = btn->width / 2 - ICON_MARGIN;

    /* 左指向三角形 */
    draw_triangle(
        cx - size / 2, cy,
        cx + size / 2, cy - size / 2,
        cx + size / 2, cy + size / 2
    );
}

static void draw_home_icon(const ToolbarButton *btn)
{
    int cx = btn->x + btn->width / 2;
    int cy = btn->y + btn->height / 2;
    int size = btn->width - ICON_MARGIN * 2;

    int x = cx - size / 2;
    int y = cy - size / 2 + size / 6;
    int h = size - size / 6;
    int roofH = size / 2;

    /* 房体 */
    draw_rect(x, y + roofH / 2, size, h - roofH / 2);
    /* 屋顶三角形 */
    draw_triangle(x - size / 10, y + roofH / 2,
                  cx, y - roofH / 2,
                  x + size + size / 10, y + roofH / 2);
}

static void draw_power_icon(const ToolbarButton *btn)
{
    int cx = btn->x + btn->width / 2;
    int cy = btn->y + btn->height / 2;
    int size = btn->width - ICON_MARGIN * 2;
    float radius = size / 2.5f;
    float thickness = 3.0f;

    draw_circle_outline(cx, cy, radius, thickness, 24);
    /* 顶部竖线 */
    draw_rect(cx - thickness / 2, cy - radius - thickness, thickness, radius + thickness / 2);
}

static void draw_volume_up_icon(const ToolbarButton *btn)
{
    int cx = btn->x + btn->width / 2;
    int cy = btn->y + btn->height / 2;
    int size = btn->width - ICON_MARGIN * 2;
    int barW = size / 4;
    int barH = size;

    draw_rect(cx - barW / 2, cy - barH / 2, barW, barH);
    draw_rect(cx - barH / 2, cy - barW / 2, barH, barW);
}

static void draw_volume_down_icon(const ToolbarButton *btn)
{
    int cx = btn->x + btn->width / 2;
    int cy = btn->y + btn->height / 2;
    int size = btn->width - ICON_MARGIN * 2;
    int barW = size / 4;
    int barH = size;

    draw_rect(cx - barH / 2, cy - barW / 2, barH, barW);
}

static void draw_screenshot_icon(const ToolbarButton *btn)
{
    int cx = btn->x + btn->width / 2;
    int cy = btn->y + btn->height / 2;
    int size = btn->width - ICON_MARGIN * 2;
    int x = cx - size / 2;
    int y = cy - size / 2;
    int thickness = 3;

    /* 外框 */
    draw_rect_lines(x, y, size, size, thickness);
    /* 内部小圆点 */
    draw_circle(cx + size / 5, cy + size / 5, size / 8, 12);
}

static void draw_button_icon(ToolbarAction action, const ToolbarButton *btn)
{
    switch (action) {
        case TOOLBAR_ACTION_BACK:         draw_back_icon(btn); break;
        case TOOLBAR_ACTION_HOME:         draw_home_icon(btn); break;
        case TOOLBAR_ACTION_POWER:        draw_power_icon(btn); break;
        case TOOLBAR_ACTION_VOLUME_UP:    draw_volume_up_icon(btn); break;
        case TOOLBAR_ACTION_VOLUME_DOWN:  draw_volume_down_icon(btn); break;
        case TOOLBAR_ACTION_SCREENSHOT:   draw_screenshot_icon(btn); break;
        default: break;
    }
}

int toolbar_init(int window_width, int window_height)
{
    memset(&g_toolbarCtx, 0, sizeof(g_toolbarCtx));
    g_toolbarCtx.windowWidth = window_width;
    g_toolbarCtx.windowHeight = window_height;
    g_toolbarCtx.toolbarX = window_width - TOOLBAR_WIDTH;
    g_toolbarCtx.hoverIndex = -1;
    g_toolbarCtx.activeIndex = -1;

    g_toolbarCtx.program = create_program(SOLID_VERTEX_SHADER, SOLID_FRAGMENT_SHADER);
    if (!g_toolbarCtx.program) {
        LOG_TAG_E(TOOLBAR_TAG, "Failed to create toolbar shader");
        return -1;
    }

    glGenVertexArrays(1, &g_toolbarCtx.vao);
    glGenBuffers(1, &g_toolbarCtx.vbo);

    glBindVertexArray(g_toolbarCtx.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_toolbarCtx.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);

    layout_buttons();

    LOG_TAG_I(TOOLBAR_TAG, "Toolbar initialized");
    return 0;
}

void toolbar_resize(int window_width, int window_height)
{
    g_toolbarCtx.windowWidth = window_width;
    g_toolbarCtx.windowHeight = window_height;
    g_toolbarCtx.toolbarX = window_width - TOOLBAR_WIDTH;
    layout_buttons();
}

void toolbar_update_mouse(int x, int y)
{
    g_toolbarCtx.hoverIndex = -1;
    if (x < g_toolbarCtx.toolbarX) return;

    for (int i = 0; i < TOOLBAR_ACTION_COUNT; i++) {
        const ToolbarButton *b = &g_toolbarCtx.buttons[i];
        if (x >= b->x && x < b->x + b->width &&
            y >= b->y && y < b->y + b->height) {
            g_toolbarCtx.hoverIndex = i;
            break;
        }
    }
}

ToolbarAction toolbar_handle_mouse_click(int x, int y, int button, int action)
{
    (void)button;

    if (action == 0) { /* mouse down */
        g_toolbarCtx.activeIndex = -1;
        if (x < g_toolbarCtx.toolbarX) return TOOLBAR_ACTION_NONE;

        for (int i = 0; i < TOOLBAR_ACTION_COUNT; i++) {
            const ToolbarButton *b = &g_toolbarCtx.buttons[i];
            if (x >= b->x && x < b->x + b->width &&
                y >= b->y && y < b->y + b->height) {
                g_toolbarCtx.activeIndex = i;
                return (ToolbarAction)i;
            }
        }
    } else if (action == 1) { /* mouse up */
        g_toolbarCtx.activeIndex = -1;
    }

    return TOOLBAR_ACTION_NONE;
}

void toolbar_render(void)
{
    if (!g_toolbarCtx.program) return;

    /* 保存当前 OpenGL 状态 */
    GLint lastProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    GLint lastVao;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVao);

    /* 设置全窗口视口 */
    glViewport(0, 0, g_toolbarCtx.windowWidth, g_toolbarCtx.windowHeight);

    glUseProgram(g_toolbarCtx.program);
    glBindVertexArray(g_toolbarCtx.vao);

    for (int i = 0; i < TOOLBAR_ACTION_COUNT; i++) {
        const ToolbarButton *b = &g_toolbarCtx.buttons[i];

        /* 背景 */
        const float *bg;
        if (g_toolbarCtx.activeIndex == i) {
            bg = COLOR_BG_ACTIVE;
        } else if (g_toolbarCtx.hoverIndex == i) {
            bg = COLOR_BG_HOVER;
        } else {
            bg = COLOR_BG;
        }
        set_color(bg);
        draw_rect((float)b->x, (float)b->y, (float)b->width, (float)b->height);

        /* 圆角效果通过稍小的内矩形实现（简单近似） */
        set_color(COLOR_ICON);
        draw_button_icon((ToolbarAction)i, b);
    }

    /* 恢复状态 */
    glUseProgram((GLuint)lastProgram);
    glBindVertexArray((GLuint)lastVao);
}

int toolbar_get_width(void)
{
    return TOOLBAR_WIDTH;
}

void toolbar_destroy(void)
{
    if (g_toolbarCtx.vao) glDeleteVertexArrays(1, &g_toolbarCtx.vao);
    if (g_toolbarCtx.vbo) glDeleteBuffers(1, &g_toolbarCtx.vbo);
    if (g_toolbarCtx.program) glDeleteProgram(g_toolbarCtx.program);
    memset(&g_toolbarCtx, 0, sizeof(g_toolbarCtx));
}
