/*
 * OHOS Scrcpy 客户端 - 渲染实现
 * 使用 SDL2 + OpenGL 渲染 YUV 视频帧
 */

#include "renderer.h"
#include "toolbar.h"
#include "../../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <SDL2/SDL_opengl.h>

#define RENDER_TAG "Renderer"

/* OpenGL YUV→RGB 转换着色器 */
static const char *VERTEX_SHADER =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    TexCoord = aTexCoord;\n"
    "}\n";

static const char *FRAGMENT_SHADER =
    "#version 330 core\n"
    "in vec2 TexCoord;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D texY;\n"
    "uniform sampler2D texU;\n"
    "uniform sampler2D texV;\n"
    "void main() {\n"
    "    float y = texture(texY, TexCoord).r;\n"
    "    float u = texture(texU, TexCoord).r - 0.5;\n"
    "    float v = texture(texV, TexCoord).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344136 * u - 0.714136 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

/* OpenGL RGBA 渲染着色器 */
static const char *RGBA_FRAGMENT_SHADER =
    "#version 330 core\n"
    "in vec2 TexCoord;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D texRGBA;\n"
    "void main() {\n"
    "    FragColor = texture(texRGBA, TexCoord);\n"
    "}\n";

typedef struct {
    SDL_Window   *window;
    SDL_GLContext  glContext;
    GLuint        program;
    GLuint        rgbaProgram;
    GLuint        vao, vbo;
    GLuint        texY, texU, texV;
    GLuint        texRGBA;
    int           videoWidth;
    int           videoHeight;
    int           windowWidth;
    int           windowHeight;
    int           toolbarWidth;
    bool          fullscreen;
    bool          shouldClose;
    bool          rgbaMode;
    RenderStats   stats;
    uint32_t      lastFpsTime;
    uint32_t      fpsFrameCount;
} RendererContext;

static RendererContext g_renderCtx = {0};

/* 编译着色器 */
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
        LOG_TAG_E(RENDER_TAG, "Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* 创建着色器程序 */
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
        LOG_TAG_E(RENDER_TAG, "Program link failed: %s", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

/* 创建 YUV 纹理 */
static void create_yuv_textures(void)
{
    glGenTextures(1, &g_renderCtx.texY);
    glGenTextures(1, &g_renderCtx.texU);
    glGenTextures(1, &g_renderCtx.texV);

    GLuint textures[] = { g_renderCtx.texY, g_renderCtx.texU, g_renderCtx.texV };
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

/* 更新视口保持宽高比，留出右侧工具栏空间 */
static void update_viewport(void)
{
    int winW = g_renderCtx.windowWidth - g_renderCtx.toolbarWidth;
    int winH = g_renderCtx.windowHeight;
    int vidW = g_renderCtx.videoWidth;
    int vidH = g_renderCtx.videoHeight;

    if (winW <= 0) winW = g_renderCtx.windowWidth;
    if (vidW <= 0 || vidH <= 0 || winW <= 0 || winH <= 0) return;

    float winAspect = (float)winW / winH;
    float vidAspect = (float)vidW / vidH;

    int renderW, renderH, offsetX, offsetY;
    if (winAspect > vidAspect) {
        renderH = winH;
        renderW = (int)(winH * vidAspect);
        offsetX = (winW - renderW) / 2;
        offsetY = 0;
    } else {
        renderW = winW;
        renderH = (int)(winW / vidAspect);
        offsetX = 0;
        offsetY = (winH - renderH) / 2;
    }

    glViewport(offsetX, offsetY, renderW, renderH);
}

int renderer_create(const RendererConfig *config)
{
    memset(&g_renderCtx, 0, sizeof(g_renderCtx));

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_TAG_E(RENDER_TAG, "SDL init failed: %s", SDL_GetError());
        return -1;
    }

    /* 设置 OpenGL 属性 */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* 计算窗口尺寸 */
    int winW = config->windowWidth > 0 ? config->windowWidth : 800;
    int winH = config->windowHeight > 0 ? config->windowHeight : 600;

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                   SDL_WINDOW_ALLOW_HIGHDPI;
    if (config->alwaysOnTop) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    if (config->borderless) flags |= SDL_WINDOW_BORDERLESS;

    g_renderCtx.window = SDL_CreateWindow(
        "OHOS Scrcpy",
        config->windowX >= 0 ? config->windowX : SDL_WINDOWPOS_CENTERED,
        config->windowY >= 0 ? config->windowY : SDL_WINDOWPOS_CENTERED,
        winW, winH, flags);

    if (!g_renderCtx.window) {
        LOG_TAG_E(RENDER_TAG, "Window create failed: %s", SDL_GetError());
        return -1;
    }

    g_renderCtx.glContext = SDL_GL_CreateContext(g_renderCtx.window);
    if (!g_renderCtx.glContext) {
        LOG_TAG_E(RENDER_TAG, "GL context create failed: %s", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(g_renderCtx.window, g_renderCtx.glContext);
    SDL_GL_SetSwapInterval(0); /* 禁用 VSync 以降低延迟 */

    /* 初始化 GLEW */
    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        LOG_TAG_E(RENDER_TAG, "GLEW init failed: %s", glewGetErrorString(glewErr));
        return -1;
    }

    /* 创建着色器程序 */
    g_renderCtx.program = create_program(VERTEX_SHADER, FRAGMENT_SHADER);
    if (!g_renderCtx.program) return -1;

    /* 创建 VAO/VBO */
    float vertices[] = {
        /* pos       texcoord */
        -1.0f,  1.0f,  0.0f, 0.0f,  /* 左上 */
        -1.0f, -1.0f,  0.0f, 1.0f,  /* 左下 */
         1.0f, -1.0f,  1.0f, 1.0f,  /* 右下 */
         1.0f,  1.0f,  1.0f, 0.0f,  /* 右上 */
    };

    glGenVertexArrays(1, &g_renderCtx.vao);
    glGenBuffers(1, &g_renderCtx.vbo);

    glBindVertexArray(g_renderCtx.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_renderCtx.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glBindVertexArray(0);

    /* 创建 YUV 纹理 */
    create_yuv_textures();

    /* 创建 RGBA 着色器程序 */
    g_renderCtx.rgbaProgram = create_program(VERTEX_SHADER, RGBA_FRAGMENT_SHADER);
    if (!g_renderCtx.rgbaProgram) {
        LOG_TAG_E(RENDER_TAG, "RGBA shader program create failed");
        return -1;
    }

    /* 创建 RGBA 纹理 */
    glGenTextures(1, &g_renderCtx.texRGBA);
    glBindTexture(GL_TEXTURE_2D, g_renderCtx.texRGBA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g_renderCtx.rgbaMode = false;

    /* 获取窗口尺寸 */
    SDL_GetWindowSize(g_renderCtx.window,
                      &g_renderCtx.windowWidth, &g_renderCtx.windowHeight);

    LOG_TAG_I(RENDER_TAG, "Renderer created (%dx%d)", winW, winH);
    return 0;
}

void renderer_set_toolbar_width(int width)
{
    g_renderCtx.toolbarWidth = width;
    SDL_GL_MakeCurrent(g_renderCtx.window, g_renderCtx.glContext);
    update_viewport();
}

int renderer_update_video_size(int width, int height)
{
    g_renderCtx.videoWidth = width;
    g_renderCtx.videoHeight = height;

    /* 根据视频宽高比调整窗口大小，额外加上工具栏宽度 */
    if (width > 0 && height > 0) {
        float aspect = (float)width / height;
        int winH = g_renderCtx.windowHeight;
        int winW = (int)(winH * aspect) + g_renderCtx.toolbarWidth;

        SDL_SetWindowSize(g_renderCtx.window, winW, winH);
        g_renderCtx.windowWidth = winW;
    }

    update_viewport();
    LOG_TAG_I(RENDER_TAG, "Video size: %dx%d", width, height);
    return 0;
}

int renderer_render_frame(const uint8_t *yData, int yStride,
                          const uint8_t *uData, int uStride,
                          const uint8_t *vData, int vStride,
                          int width, int height)
{
    SDL_GL_MakeCurrent(g_renderCtx.window, g_renderCtx.glContext);
    update_viewport();

    /* 更新 Y 纹理 */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_renderCtx.texY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, yData);

    /* 更新 U 纹理 */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_renderCtx.texU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0,
                 GL_RED, GL_UNSIGNED_BYTE, uData);

    /* 更新 V 纹理 */
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_renderCtx.texV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0,
                 GL_RED, GL_UNSIGNED_BYTE, vData);

    /* 渲染 */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(g_renderCtx.program);
    glUniform1i(glGetUniformLocation(g_renderCtx.program, "texY"), 0);
    glUniform1i(glGetUniformLocation(g_renderCtx.program, "texU"), 1);
    glUniform1i(glGetUniformLocation(g_renderCtx.program, "texV"), 2);

    glBindVertexArray(g_renderCtx.vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    toolbar_render();
    SDL_GL_SwapWindow(g_renderCtx.window);

    /* 更新 FPS 统计 */
    g_renderCtx.stats.frameCount++;
    g_renderCtx.fpsFrameCount++;
    uint32_t now = SDL_GetTicks();
    if (now - g_renderCtx.lastFpsTime >= 1000) {
        g_renderCtx.stats.fps = g_renderCtx.fpsFrameCount;
        g_renderCtx.fpsFrameCount = 0;
        g_renderCtx.lastFpsTime = now;
    }

    return 0;
}

int renderer_render_rgba_frame(const uint8_t *rgbaData, int width, int height, uint32_t stride)
{
    (void)stride;
    SDL_GL_MakeCurrent(g_renderCtx.window, g_renderCtx.glContext);
    update_viewport();

    /* 首次收到 RGBA 帧时切换到 RGBA 模式 */
    if (!g_renderCtx.rgbaMode) {
        g_renderCtx.rgbaMode = true;
        renderer_update_video_size(width, height);
        LOG_TAG_I(RENDER_TAG, "Switched to RGBA mode (%dx%d)", width, height);
    }

    /* 更新 RGBA 纹理 */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_renderCtx.texRGBA);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);

    /* 渲染 */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(g_renderCtx.rgbaProgram);
    glUniform1i(glGetUniformLocation(g_renderCtx.rgbaProgram, "texRGBA"), 0);

    glBindVertexArray(g_renderCtx.vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    toolbar_render();
    SDL_GL_SwapWindow(g_renderCtx.window);

    /* 更新 FPS 统计 */
    g_renderCtx.stats.frameCount++;
    g_renderCtx.fpsFrameCount++;
    uint32_t now = SDL_GetTicks();
    if (now - g_renderCtx.lastFpsTime >= 1000) {
        g_renderCtx.stats.fps = g_renderCtx.fpsFrameCount;
        g_renderCtx.fpsFrameCount = 0;
        g_renderCtx.lastFpsTime = now;
    }

    return 0;
}

bool renderer_poll_events(void)
{
    return !g_renderCtx.shouldClose;
}

void renderer_handle_event(const SDL_Event *event)
{
    if (!event) return;

    switch (event->type) {
        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_renderCtx.windowWidth = event->window.data1;
                g_renderCtx.windowHeight = event->window.data2;
                SDL_GL_MakeCurrent(g_renderCtx.window, g_renderCtx.glContext);
                update_viewport();
            }
            break;
        case SDL_KEYDOWN:
            if (event->key.keysym.sym == SDLK_F11) {
                renderer_toggle_fullscreen();
            } else if (event->key.keysym.sym == SDLK_ESCAPE &&
                       g_renderCtx.fullscreen) {
                renderer_toggle_fullscreen();
            }
            break;
        case SDL_QUIT:
            g_renderCtx.shouldClose = true;
            break;
    }
}

void renderer_toggle_fullscreen(void)
{
    g_renderCtx.fullscreen = !g_renderCtx.fullscreen;
    SDL_SetWindowFullscreen(g_renderCtx.window,
                            g_renderCtx.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

bool renderer_should_close(void)
{
    return g_renderCtx.shouldClose;
}

void renderer_get_window_size(int *width, int *height)
{
    *width = g_renderCtx.windowWidth;
    *height = g_renderCtx.windowHeight;
}

SDL_Window *renderer_get_window(void)
{
    return g_renderCtx.window;
}

void renderer_get_stats(RenderStats *stats)
{
    if (stats) *stats = g_renderCtx.stats;
}

static int write_bmp(const char *path, int width, int height, const uint8_t *bgra)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LOG_TAG_E(RENDER_TAG, "Failed to open screenshot file: %s", path);
        return -1;
    }

    int rowSize = width * 4;
    int paddedRowSize = (rowSize + 3) & ~3;
    int imageSize = paddedRowSize * height;
    int fileSize = 14 + 40 + imageSize;

    /* BMP 文件头 */
    uint8_t fileHeader[14] = {
        'B', 'M',
        (uint8_t)(fileSize & 0xFF), (uint8_t)((fileSize >> 8) & 0xFF),
        (uint8_t)((fileSize >> 16) & 0xFF), (uint8_t)((fileSize >> 24) & 0xFF),
        0, 0, 0, 0,
        54, 0, 0, 0
    };

    /* DIB 头 BITMAPINFOHEADER */
    uint8_t dibHeader[40] = {
        40, 0, 0, 0,
        (uint8_t)(width & 0xFF), (uint8_t)((width >> 8) & 0xFF),
        (uint8_t)((width >> 16) & 0xFF), (uint8_t)((width >> 24) & 0xFF),
        (uint8_t)(height & 0xFF), (uint8_t)((height >> 8) & 0xFF),
        (uint8_t)((height >> 16) & 0xFF), (uint8_t)((height >> 24) & 0xFF),
        1, 0,
        32, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    fwrite(fileHeader, 1, sizeof(fileHeader), fp);
    fwrite(dibHeader, 1, sizeof(dibHeader), fp);

    /* BMP 行自下而上，且需要 4 字节对齐 */
    uint8_t padding[4] = {0};
    for (int y = height - 1; y >= 0; y--) {
        const uint8_t *row = bgra + y * width * 4;
        fwrite(row, 1, rowSize, fp);
        if (paddedRowSize > rowSize) {
            fwrite(padding, 1, paddedRowSize - rowSize, fp);
        }
    }

    fclose(fp);
    return 0;
}

int renderer_take_screenshot(const char *path)
{
    if (!path || !g_renderCtx.window) return -1;

    SDL_GL_MakeCurrent(g_renderCtx.window, g_renderCtx.glContext);

    int width = g_renderCtx.windowWidth;
    int height = g_renderCtx.windowHeight;
    if (width <= 0 || height <= 0) return -1;

    uint8_t *pixels = (uint8_t *)malloc(width * height * 4);
    if (!pixels) return -1;

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    /* BMP 使用 BGRA 顺序，交换 R 与 B */
    for (int i = 0; i < width * height; i++) {
        uint8_t tmp = pixels[i * 4 + 0];
        pixels[i * 4 + 0] = pixels[i * 4 + 2];
        pixels[i * 4 + 2] = tmp;
    }

    int ret = write_bmp(path, width, height, pixels);
    free(pixels);

    if (ret == 0) {
        LOG_TAG_I(RENDER_TAG, "Screenshot saved: %s", path);
    }
    return ret;
}

void renderer_destroy(void)
{
    if (g_renderCtx.vao) glDeleteVertexArrays(1, &g_renderCtx.vao);
    if (g_renderCtx.vbo) glDeleteBuffers(1, &g_renderCtx.vbo);
    if (g_renderCtx.texY) glDeleteTextures(1, &g_renderCtx.texY);
    if (g_renderCtx.texU) glDeleteTextures(1, &g_renderCtx.texU);
    if (g_renderCtx.texV) glDeleteTextures(1, &g_renderCtx.texV);
    if (g_renderCtx.texRGBA) glDeleteTextures(1, &g_renderCtx.texRGBA);
    if (g_renderCtx.program) glDeleteProgram(g_renderCtx.program);
    if (g_renderCtx.rgbaProgram) glDeleteProgram(g_renderCtx.rgbaProgram);

    if (g_renderCtx.glContext) {
        SDL_GL_DeleteContext(g_renderCtx.glContext);
    }
    if (g_renderCtx.window) {
        SDL_DestroyWindow(g_renderCtx.window);
    }
    SDL_Quit();

    memset(&g_renderCtx, 0, sizeof(g_renderCtx));
}
