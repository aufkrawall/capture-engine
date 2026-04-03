// OpenGL Test App for Capture + FPS Limiter Testing
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#include <GL/gl.h>
#include <GL/glu.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "testapp_common.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "glu32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static int g_WindowWidth = 800;
static int g_WindowHeight = 600;
static int g_GpuLoadPasses = 10;
static int g_VSync = 0;
static int g_Fullscreen = 1;
bool g_Running = true;
float g_BarPosition = 0.0f;
auto g_StartTime = std::chrono::high_resolution_clock::now();

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";

    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

void SetupPixelFormat(HDC hDC) {
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),                               // size
        1,                                                           // version
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,  // flags
        PFD_TYPE_RGBA,                                               // pixel type
        32,                                                          // color bits
        0,
        0,
        0,
        0,
        0,
        0,  // color bits ignored
        0,  // alpha buffer
        0,  // shift bit ignored
        0,  // no accumulation buffer
        0,
        0,
        0,
        0,               // accum bits ignored
        24,              // z-buffer
        8,               // stencil buffer
        0,               // no auxiliary buffer
        PFD_MAIN_PLANE,  // main layer
        0,               // reserved
        0,
        0,
        0  // layer masks ignored
    };
    int pixelFormat = ChoosePixelFormat(hDC, &pfd);
    SetPixelFormat(hDC, pixelFormat, &pfd);
}
void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = (float)std::fmod((double)(elapsed * 0.5f), 1.0);

    // Clear background with a color that shifts over time
    float r = 0.1f + 0.1f * std::sin(elapsed);
    float g = 0.1f + 0.1f * std::cos(elapsed);
    glClearColor(r, g, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_WindowWidth, 0, g_WindowHeight, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Draw Bar
    float barX = g_BarPosition * (g_WindowWidth - 100);
    float barY = g_WindowHeight / 2.0f;

    // Draw sliding bar
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(barX, barY - 50);
    glVertex2f(barX + 100, barY - 50);
    glVertex2f(barX + 100, barY + 50);
    glVertex2f(barX, barY + 50);
    glEnd();

    // Draw a spinning triangle in the center
    glPushMatrix();
    glTranslatef(g_WindowWidth / 2.0f, g_WindowHeight / 2.0f, 0);
    glRotatef(elapsed * 100.0f, 0, 0, 1);
    glColor3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex2f(-50, -50);
    glVertex2f(50, -50);
    glVertex2f(0, 50);
    glEnd();
    glPopMatrix();

    // GPU Load Simulation (Redraws)
    for (int i = 0; i < g_GpuLoadPasses; i++) {
        glColor3f(0.1f + (i % 2) * 0.01f, 0.1f, 0.1f);
        glBegin(GL_TRIANGLES);
        glVertex2f(0, 0);
        glVertex2f(10, 0);
        glVertex2f(0, 10);
        glEnd();
    }

    // Console FPS logging (every 2 seconds)
    static auto lastLog = now;
    static int frames = 0;
    frames++;
    if (std::chrono::duration<float>(now - lastLog).count() >= 2.0f) {
        float fps = frames / 2.0f;
        printf("FPS: %.2f\n", fps);
        frames = 0;
        lastLog = now;
    }
}

int main(int argc, char* argv[]) {
    testapp::EnableGameDpiAwareness();
    LoadConfig();

    testapp::ApplyGameScheduling();

    bool forceLegacy = false;
    if (argc >= 3) {
        g_WindowWidth = atoi(argv[1]);
        g_WindowHeight = atoi(argv[2]);
    }
    if (argc >= 4) {
        g_GpuLoadPasses = atoi(argv[3]);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--legacy") == 0)
            forceLegacy = true;
    }

    WNDCLASS wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "OpenGLTestApp";
    RegisterClass(&wc);

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
        RECT wr = testapp::AdjustWindowRectForClientSize(WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, g_WindowWidth,
                                                         g_WindowHeight);
        winW = wr.right - wr.left;
        winH = wr.bottom - wr.top;
    }

    HWND hWnd = CreateWindow("OpenGLTestApp", "OpenGL Test", winStyle, posX, posY, winW, winH, nullptr, nullptr,
                             wc.hInstance, nullptr);

    // Ensure window is actually shown and not just a title bar
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    HDC hDC = GetDC(hWnd);
    SetupPixelFormat(hDC);
    HGLRC hRC = wglCreateContext(hDC);
    wglMakeCurrent(hDC, hRC);

    // Upgrade to Modern Context (GL 3.3+) if possible
    typedef HGLRC(WINAPI * PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC hDC, HGLRC hShareContext, const int* attribList);
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB && !forceLegacy) {
        int attribs[] = {0x2091 /*WGL_CONTEXT_MAJOR_VERSION_ARB*/,
                         3,
                         0x2092 /*WGL_CONTEXT_MINOR_VERSION_ARB*/,
                         3,
                         0x2094 /*WGL_CONTEXT_PROFILE_MASK_ARB*/,
                         0x00000002 /*WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB*/,
                         0};

        HGLRC hRC3 = wglCreateContextAttribsARB(hDC, 0, attribs);
        if (hRC3) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(hRC);
            hRC = hRC3;
            wglMakeCurrent(hDC, hRC);
            printf("Created OpenGL 3.3 Compatibility Context\n");
        } else {
            printf("Failed to create GL 3.3 context, falling back to Legacy\n");
        }
    } else {
        printf(forceLegacy ? "Legacy Context Forced\n"
                           : "wglCreateContextAttribsARB not found, using Legacy Context\n");
    }

    typedef BOOL(WINAPI * PFNWGLSWAPINTERVALEXTPROC)(int interval);
    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(g_VSync);
    }

    MSG msg = {0};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Render();
        SwapBuffers(hDC);
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hRC);
    ReleaseDC(hWnd, hDC);
    DestroyWindow(hWnd);

    return 0;
}
