// Legacy OpenGL 2.1 Test App for fixed-function overlay validation
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#include <GL/gl.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testapp_common.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif

#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif

typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC hdc, HGLRC shareContext, const int* attribList);
typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);

static int g_WindowWidth = 1024;
static int g_WindowHeight = 768;
static int g_WorkloadPasses = 10;
static int g_VSync = 0;
static int g_Fullscreen = 1;
static bool g_Running = true;
static float g_BarPosition = 0.0f;
static auto g_StartTime = std::chrono::high_resolution_clock::now();

static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WorkloadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_WorkloadPasses, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        g_Running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        g_Running = false;
        DestroyWindow(hWnd);
        return 0;
    }
    if (msg == WM_SIZE) {
        g_WindowWidth = LOWORD(lParam);
        g_WindowHeight = HIWORD(lParam);
        glViewport(0, 0, g_WindowWidth, g_WindowHeight);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool SetupPixelFormat(HDC hDC) {
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),
        1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,
        32,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        24,
        8,
        0,
        PFD_MAIN_PLANE,
        0,
        0,
        0,
        0,
    };

    int pixelFormat = ChoosePixelFormat(hDC, &pfd);
    if (pixelFormat == 0)
        return false;
    return SetPixelFormat(hDC, pixelFormat, &pfd) == TRUE;
}

static HGLRC CreateLegacyContext(HDC hDC) {
    HGLRC bootstrapContext = wglCreateContext(hDC);
    if (!bootstrapContext)
        return nullptr;

    if (!wglMakeCurrent(hDC, bootstrapContext)) {
        wglDeleteContext(bootstrapContext);
        return nullptr;
    }

    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!wglCreateContextAttribsARB) {
        printf("wglCreateContextAttribsARB unavailable, using bootstrap context\n");
        return bootstrapContext;
    }

    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 2, WGL_CONTEXT_MINOR_VERSION_ARB, 1, 0,
    };

    HGLRC legacyContext = wglCreateContextAttribsARB(hDC, nullptr, attribs);
    if (!legacyContext) {
        printf("Failed to request OpenGL 2.1 context, using bootstrap context\n");
        return bootstrapContext;
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(bootstrapContext);
    if (!wglMakeCurrent(hDC, legacyContext)) {
        wglDeleteContext(legacyContext);
        return nullptr;
    }

    printf("Created explicit OpenGL 2.1 context\n");
    return legacyContext;
}

static void RenderFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = static_cast<float>(std::fmod(elapsed * 0.35f, 1.0f));

    float red = 0.08f + 0.05f * std::sin(elapsed * 0.75f);
    float green = 0.08f + 0.05f * std::cos(elapsed * 0.50f);
    glClearColor(red, green, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glViewport(0, 0, g_WindowWidth, g_WindowHeight);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(g_WindowWidth), 0.0, static_cast<double>(g_WindowHeight), -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);

    const float barWidth = 180.0f;
    const float barX = g_BarPosition * static_cast<float>(g_WindowWidth - static_cast<int>(barWidth));
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const float barY = g_WindowHeight * 0.55f;

    glBegin(GL_QUADS);
    glColor3f(0.14f, 0.14f, 0.22f);
    glVertex2f(0.0f, 0.0f);
    glVertex2f(static_cast<float>(g_WindowWidth), 0.0f);
    glVertex2f(static_cast<float>(g_WindowWidth), static_cast<float>(g_WindowHeight));
    glVertex2f(0.0f, static_cast<float>(g_WindowHeight));

    glColor3f(0.95f, 0.95f, 0.95f);
    glVertex2f(barX, barY - 48.0f);
    glVertex2f(barX + barWidth, barY - 48.0f);
    glVertex2f(barX + barWidth, barY + 48.0f);
    glVertex2f(barX, barY + 48.0f);
    glEnd();

    glPushMatrix();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    glTranslatef(g_WindowWidth * 0.5f, g_WindowHeight * 0.35f, 0.0f);
    glRotatef(elapsed * 90.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_TRIANGLES);
    glColor3f(0.95f, 0.20f, 0.20f);
    glVertex2f(0.0f, 90.0f);
    glColor3f(0.20f, 0.95f, 0.20f);
    glVertex2f(-78.0f, -58.0f);
    glColor3f(0.20f, 0.45f, 0.95f);
    glVertex2f(78.0f, -58.0f);
    glEnd();
    glPopMatrix();

    glBegin(GL_LINES);
    for (int pass = 0; pass < g_WorkloadPasses; ++pass) {
        float x = static_cast<float>((pass * 73) % (g_WindowWidth + 80));
        float y = static_cast<float>((pass * 41 + static_cast<int>(elapsed * 85.0f)) % (g_WindowHeight + 40));
        float t = static_cast<float>(pass + 1) / static_cast<float>(g_WorkloadPasses + 1);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        glColor3f(0.25f + 0.45f * t, 0.35f, 0.55f + 0.25f * std::sin(elapsed + pass));
        glVertex2f(x - 30.0f, y - 18.0f);
        glVertex2f(x + 30.0f, y + 18.0f);
    }
    glEnd();

    glBegin(GL_LINE_LOOP);
    glColor3f(0.95f, 0.80f, 0.25f);
    glVertex2f(24.0f, 24.0f);
    glVertex2f(static_cast<float>(g_WindowWidth - 24), 24.0f);
    glVertex2f(static_cast<float>(g_WindowWidth - 24), static_cast<float>(g_WindowHeight - 24));
    glVertex2f(24.0f, static_cast<float>(g_WindowHeight - 24));
    glEnd();

    static auto lastLog = now;
    static int frames = 0;
    ++frames;
    if (std::chrono::duration<float>(now - lastLog).count() >= 2.0f) {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        printf("Legacy OpenGL FPS: %.2f\n", frames / 2.0f);
        frames = 0;
        lastLog = now;
    }
}

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    testapp::EnableGameDpiAwareness();
    LoadConfig();

    testapp::ApplyGameScheduling();

    if (argc >= 3) {
        g_WindowWidth = testapp::ParseIntOrZero(argv[1]);
        g_WindowHeight = testapp::ParseIntOrZero(argv[2]);
    }
    if (argc >= 4) {
        g_WorkloadPasses = testapp::ParseIntOrZero(argv[3]);
    }

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "OpenGLLegacyTestApp";
    RegisterClassA(&wc);

    RECT monitorRect = testapp::GetPrimaryMonitorRect();

    if (g_Fullscreen) {
        g_WindowWidth = monitorRect.right - monitorRect.left;
        g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }

    DWORD winStyle = g_Fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    int posX = g_Fullscreen ? monitorRect.left : CW_USEDEFAULT;
    int posY = g_Fullscreen ? monitorRect.top : CW_USEDEFAULT;
    int winW = g_WindowWidth;
    int winH = g_WindowHeight;
    if (!g_Fullscreen) {
        RECT wr =
            testapp::AdjustWindowRectForClientSize(WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, g_WindowWidth, g_WindowHeight);
        winW = wr.right - wr.left;
        winH = wr.bottom - wr.top;
    }

    HWND hWnd = CreateWindowA("OpenGLLegacyTestApp", "Legacy OpenGL 2.1 Test", winStyle, posX, posY, winW, winH,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hWnd)
        return 1;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    HDC hDC = GetDC(hWnd);
    if (!hDC || !SetupPixelFormat(hDC))
        return 1;

    HGLRC hRC = CreateLegacyContext(hDC);
    if (!hRC)
        return 1;

    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    printf("Legacy OpenGL context: %s\n", version ? reinterpret_cast<const char*>(version) : "unknown");
    printf("Renderer: %s\n", renderer ? reinterpret_cast<const char*>(renderer) : "unknown");

    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT =
        reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(wglGetProcAddress("wglSwapIntervalEXT"));
    if (wglSwapIntervalEXT)
        wglSwapIntervalEXT(g_VSync);

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_Running)
            break;
        RenderFrame();
        SwapBuffers(hDC);
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hRC);
    ReleaseDC(hWnd, hDC);
    DestroyWindow(hWnd);
    return 0;
}
