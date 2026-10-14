// S Pen Bridge - Windows receiver.
//
// Receives S Pen samples from the Android app over UDP, injects them as a real
// Windows Ink pen (pressure, tilt, barrel button, eraser) and draws a liquid-glass
// hover lens that is separate from the mouse cursor. One file, no dependencies.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006 // NTDDI_WIN10_RS5: synthetic pointer injection
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

// ----------------------------------------------------------------------------- protocol

static const uint16_t kPort = 47800;
enum : uint8_t { T_DISCOVER = 1, T_OFFER, T_HELLO, T_WELCOME, T_PEN, T_PING, T_PONG, T_KEY, T_BYE };
enum : uint8_t { F_IN_RANGE = 1, F_CONTACT = 2, F_BARREL = 4, F_ERASER = 8 };
static const int kSampleSize = 16;

struct Sample {
    uint8_t flags;
    uint8_t distance;
    float pressure; // 0..1
    float x, y;     // 0..1 inside the phone's active area
    int tiltX, tiltY;
};

#define WM_APP_OVERLAY (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_TRAY (WM_APP + 3)

enum {
    ID_STATUS = 1000, ID_HOVER, ID_LENS, ID_SIZE_S, ID_SIZE_M, ID_SIZE_L, ID_AUTOSTART,
    ID_FIREWALL, ID_EXIT, ID_MONITOR_BASE = 1100
};

// ----------------------------------------------------------------------------- config

struct Config {
    int monitor = -1;        // -1 = primary
    bool hoverInject = false;// false: hover only moves the glass lens, mouse stays put
    bool lens = true;        // live refraction of what's under the lens
    int size = 1;            // 0 small, 1 medium, 2 large
};

static Config g_cfg;
static std::wstring g_iniPath;

static void loadConfig() {
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        std::wstring dir = std::wstring(appdata) + L"\\SpenBridge";
        CoTaskMemFree(appdata);
        CreateDirectoryW(dir.c_str(), nullptr);
        g_iniPath = dir + L"\\spenbridge.ini";
    }
    const wchar_t* f = g_iniPath.c_str();
    g_cfg.monitor = GetPrivateProfileIntW(L"bridge", L"monitor", -1, f);
    g_cfg.hoverInject = GetPrivateProfileIntW(L"bridge", L"hover_inject", 0, f) != 0;
    g_cfg.lens = GetPrivateProfileIntW(L"bridge", L"lens", 1, f) != 0;
    g_cfg.size = std::clamp((int)GetPrivateProfileIntW(L"bridge", L"size", 1, f), 0, 2);
}

static void saveConfig() {
    const wchar_t* f = g_iniPath.c_str();
    auto put = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"bridge", k, std::to_wstring(v).c_str(), f); };
    put(L"monitor", g_cfg.monitor);
    put(L"hover_inject", g_cfg.hoverInject);
    put(L"lens", g_cfg.lens);
    put(L"size", g_cfg.size);
}

// ----------------------------------------------------------------------------- monitors

struct Monitor { RECT rc; bool primary; };
static std::vector<Monitor> g_monitors;
static std::mutex g_monMx;
static RECT g_target = {0, 0, 1920, 1080};

static BOOL CALLBACK enumMon(HMONITOR m, HDC, LPRECT, LPARAM) {
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(m, &mi)) g_monitors.push_back({mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

static void refreshMonitors() {
    std::lock_guard<std::mutex> lk(g_monMx);
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, enumMon, 0);
    std::sort(g_monitors.begin(), g_monitors.end(), [](const Monitor& a, const Monitor& b) {
        return a.rc.left != b.rc.left ? a.rc.left < b.rc.left : a.rc.top < b.rc.top;
    });
    int idx = g_cfg.monitor;
    if (idx < 0 || idx >= (int)g_monitors.size()) {
        idx = 0;
        for (size_t i = 0; i < g_monitors.size(); i++) if (g_monitors[i].primary) idx = (int)i;
    }
    if (!g_monitors.empty()) g_target = g_monitors[idx].rc;
}

static RECT targetRect() {
    std::lock_guard<std::mutex> lk(g_monMx);
    return g_target;
}

// ----------------------------------------------------------------------------- shared state

static HWND g_mainWnd, g_overlayWnd;
static std::atomic<bool> g_quit{false};

struct Hover {
    bool visible = false;
    int x = 0, y = 0;
    bool contact = false, barrel = false, eraser = false;
    float pressure = 0;
};
static std::mutex g_hoverMx;
static Hover g_hover;
static std::atomic<bool> g_overlayPending{false};

static void publishHover(const Hover& h) {
    {
        std::lock_guard<std::mutex> lk(g_hoverMx);
        g_hover = h;
    }
    if (!g_overlayPending.exchange(true)) PostMessageW(g_overlayWnd, WM_APP_OVERLAY, 0, 0);
}

struct ClientInfo {
    bool connected = false;
    sockaddr_in addr{};
    uint32_t session = 0;
    std::wstring name;
    ULONGLONG lastPacket = 0;
    ULONGLONG lastPen = 0;
    uint32_t lastSeq = 0;
    bool seqValid = false;
};
static std::mutex g_clientMx;
static ClientInfo g_client;
static std::atomic<DWORD> g_lastInjectError{0};

// ----------------------------------------------------------------------------- pen injection

static HSYNTHETICPOINTERDEVICE g_pen;
static std::mutex g_penMx;
static bool g_injRange = false, g_injContact = false;
static POINT g_lastPt{};

static void inject(POINTER_FLAGS flags, POINT pt, UINT32 pressure, INT32 tx, INT32 ty, PEN_FLAGS pf) {
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    POINTER_PEN_INFO& p = info.penInfo;
    p.pointerInfo.pointerType = PT_PEN;
    p.pointerInfo.pointerId = 0;
    p.pointerInfo.pointerFlags = flags;
    p.pointerInfo.ptPixelLocation = pt;
    p.penFlags = pf;
    p.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    p.pressure = pressure;
    p.tiltX = tx;
    p.tiltY = ty;
    const BOOL ok = InjectSyntheticPointerInput(g_pen, &info, 1);
    static const bool verbose = GetEnvironmentVariableW(L"SPENBRIDGE_DEBUG", nullptr, 0) > 0;
    if (!ok || verbose) {
        const DWORD err = ok ? 0 : GetLastError();
        if (!ok) g_lastInjectError = err;
        // Failures (or everything, with SPENBRIDGE_DEBUG set) go to %APPDATA%\SpenBridge\log.txt.
        static int logged = 0;
        if ((verbose || logged < 20) && !g_iniPath.empty()) {
            logged++;
            std::wstring path = g_iniPath.substr(0, g_iniPath.find_last_of(L'\\')) + L"\\log.txt";
            if (FILE* f = _wfopen(path.c_str(), L"a")) {
                fprintf(f, "inject %s: flags=0x%X pt=(%ld,%ld) pressure=%u err=%lu\n", ok ? "ok" : "FAILED",
                        (unsigned)flags, pt.x, pt.y, pressure, err);
                fclose(f);
            }
        }
    }
}

// InjectSyntheticPointerInput takes ptPixelLocation relative to the top-left of the
// VIRTUAL screen, not screen coordinates. With a monitor above or left of the primary
// the virtual origin is negative, so skipping this shifts every stroke up/left.
static POINT toInjectSpace(POINT p) {
    p.x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
    p.y -= GetSystemMetrics(SM_YVIRTUALSCREEN);
    return p;
}

// Drives the Windows pen state machine from one full-state sample.
static void applySample(const Sample& s, POINT screenPt) {
    std::lock_guard<std::mutex> lk(g_penMx);
    if (!g_pen) return;
    const bool contact = (s.flags & F_CONTACT) != 0;
    const bool inRange = contact || (s.flags & F_IN_RANGE) != 0;
    const POINT pt = toInjectSpace(screenPt);

    PEN_FLAGS pf = PEN_FLAG_NONE;
    if (s.flags & F_BARREL) pf |= PEN_FLAG_BARREL;
    if (s.flags & F_ERASER) pf |= PEN_FLAG_INVERTED | (contact ? PEN_FLAG_ERASER : 0);
    UINT32 pressure = contact ? (UINT32)std::clamp((int)std::lround(s.pressure * 1024.f), 1, 1024) : 0;
    INT32 tx = std::clamp(s.tiltX, -90, 90), ty = std::clamp(s.tiltY, -90, 90);

    if (contact) {
        if (!g_injRange) {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, pt, 0, tx, ty, pf);
            g_injRange = true;
        }
        if (!g_injContact) {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN, pt, pressure, tx, ty, pf);
            g_injContact = true;
        } else {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE, pt, pressure, tx, ty, pf);
        }
    } else if (inRange) {
        if (g_injContact) {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, pt, 0, tx, ty, pf);
            g_injContact = false;
        }
        if (g_cfg.hoverInject) {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, pt, 0, tx, ty, pf);
            g_injRange = true;
        } else if (g_injRange) {
            inject(POINTER_FLAG_UPDATE, pt, 0, 0, 0, PEN_FLAG_NONE); // leave range: mouse is free again
            g_injRange = false;
        }
    } else {
        if (g_injContact) {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, g_lastPt, 0, 0, 0, PEN_FLAG_NONE);
            g_injContact = false;
        }
        if (g_injRange) {
            inject(POINTER_FLAG_UPDATE, g_lastPt, 0, 0, 0, PEN_FLAG_NONE);
            g_injRange = false;
        }
    }
    if (inRange) g_lastPt = pt;
}

static void releasePen() {
    {
        std::lock_guard<std::mutex> lk(g_penMx);
        if (g_pen) {
            if (g_injContact) inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, g_lastPt, 0, 0, 0, PEN_FLAG_NONE);
            if (g_injRange || g_injContact) inject(POINTER_FLAG_UPDATE, g_lastPt, 0, 0, 0, PEN_FLAG_NONE);
        }
        g_injContact = g_injRange = false;
    }
    publishHover(Hover{});
}

static void sendKeyCombo(uint8_t mods, uint8_t vk) {
    INPUT in[10]{};
    int n = 0;
    const WORD modVk[4] = {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN};
    for (int i = 0; i < 4; i++)
        if (mods & (1 << i)) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = modVk[i]; n++; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    for (int i = 3; i >= 0; i--)
        if (mods & (1 << i)) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = modVk[i]; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    SendInput(n, in, sizeof(INPUT));
}

// ----------------------------------------------------------------------------- network

static SOCKET g_sock = INVALID_SOCKET;
static std::string g_pcName;

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const char* p, int n) {
    int m = MultiByteToWideChar(CP_UTF8, 0, p, n, nullptr, 0);
    std::wstring w(m, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, n, w.data(), m);
    return w;
}

struct Writer {
    char buf[512];
    int n = 0;
    explicit Writer(uint8_t type) { buf[0] = 'S'; buf[1] = 'P'; buf[2] = 1; buf[3] = (char)type; n = 4; }
    template <class T> void put(T v) { memcpy(buf + n, &v, sizeof(T)); n += sizeof(T); }
    void name(const std::string& s) {
        uint8_t len = (uint8_t)std::min<size_t>(s.size(), 64);
        put(len);
        memcpy(buf + n, s.data(), len);
        n += len;
    }
    void sendTo(const sockaddr_in& to) { sendto(g_sock, buf, n, 0, (const sockaddr*)&to, sizeof(to)); }
};

static void monitorSize(uint16_t& w, uint16_t& h) {
    RECT r = targetRect();
    w = (uint16_t)std::clamp((int)(r.right - r.left), 1, 65535);
    h = (uint16_t)std::clamp((int)(r.bottom - r.top), 1, 65535);
}

static void sendWelcome(const sockaddr_in& to, uint32_t session) {
    uint16_t w, h;
    monitorSize(w, h);
    Writer wr(T_WELCOME);
    wr.put(session); wr.put(w); wr.put(h); wr.name(g_pcName);
    wr.sendTo(to);
}

static bool sameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static POINT mapToScreen(float nx, float ny) {
    RECT r = targetRect();
    nx = std::clamp(nx, 0.f, 1.f);
    ny = std::clamp(ny, 0.f, 1.f);
    return POINT{r.left + (LONG)std::lround(nx * (r.right - r.left - 1)),
                 r.top + (LONG)std::lround(ny * (r.bottom - r.top - 1))};
}

static void handlePacket(const char* d, int n, const sockaddr_in& from) {
    if (n < 4 || d[0] != 'S' || d[1] != 'P' || d[2] != 1) return;
    const uint8_t type = (uint8_t)d[3];
    const char* p = d + 4;
    const char* end = d + n;
    const ULONGLONG now = GetTickCount64();

    if (type == T_DISCOVER) {
        uint16_t w, h;
        monitorSize(w, h);
        Writer wr(T_OFFER);
        wr.put(w); wr.put(h); wr.name(g_pcName);
        wr.sendTo(from);
        return;
    }
    if (type == T_HELLO) {
        if (end - p < 5) return;
        uint32_t session;
        memcpy(&session, p, 4);
        uint8_t len = (uint8_t)p[4];
        std::wstring name = widen(p + 5, (int)std::min<ptrdiff_t>(len, end - (p + 5)));
        bool changed;
        {
            std::lock_guard<std::mutex> lk(g_clientMx);
            changed = !g_client.connected || !sameEndpoint(g_client.addr, from) || g_client.session != session;
            g_client.connected = true;
            g_client.addr = from;
            g_client.session = session;
            g_client.name = name;
            g_client.lastPacket = now;
            if (changed) g_client.seqValid = false;
        }
        if (changed) { releasePen(); PostMessageW(g_mainWnd, WM_APP_STATUS, 1, 0); }
        sendWelcome(from, session);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_clientMx);
        if (!g_client.connected || !sameEndpoint(g_client.addr, from)) return;
        g_client.lastPacket = now;
    }

    switch (type) {
    case T_PING: {
        Writer wr(T_PONG);
        int len = std::min<int>((int)(end - p), 64);
        memcpy(wr.buf + 4, p, len);
        wr.n = 4 + len;
        wr.sendTo(from);
        break;
    }
    case T_PEN: {
        if (end - p < 5) return;
        uint32_t seq;
        memcpy(&seq, p, 4);
        int count = (uint8_t)p[4];
        p += 5;
        if (count <= 0 || end - p < count * kSampleSize) return;
        {
            std::lock_guard<std::mutex> lk(g_clientMx);
            if (g_client.seqValid && (int32_t)(seq - g_client.lastSeq) <= 0) return; // duplicate / late
            g_client.lastSeq = seq;
            g_client.seqValid = true;
            g_client.lastPen = now;
        }
        Sample s{};
        POINT pt{};
        for (int i = 0; i < count; i++, p += kSampleSize) {
            uint16_t pr;
            s.flags = (uint8_t)p[0];
            s.distance = (uint8_t)p[1];
            memcpy(&pr, p + 2, 2);
            memcpy(&s.x, p + 4, 4);
            memcpy(&s.y, p + 8, 4);
            s.tiltX = (int8_t)p[12];
            s.tiltY = (int8_t)p[13];
            s.pressure = pr / 65535.f;
            if (!std::isfinite(s.x) || !std::isfinite(s.y)) continue;
            pt = mapToScreen(s.x, s.y);
            applySample(s, pt);
        }
        Hover h;
        h.visible = (s.flags & (F_IN_RANGE | F_CONTACT)) != 0;
        h.x = pt.x; h.y = pt.y;
        h.contact = (s.flags & F_CONTACT) != 0;
        h.barrel = (s.flags & F_BARREL) != 0;
        h.eraser = (s.flags & F_ERASER) != 0;
        h.pressure = s.pressure;
        publishHover(h);
        break;
    }
    case T_KEY:
        if (end - p >= 2) sendKeyCombo((uint8_t)p[0], (uint8_t)p[1]);
        break;
    case T_BYE: {
        {
            std::lock_guard<std::mutex> lk(g_clientMx);
            g_client.connected = false;
        }
        releasePen();
        PostMessageW(g_mainWnd, WM_APP_STATUS, 0, 0);
        break;
    }
    }
}

static void netThread() {
    char buf[2048];
    while (!g_quit) {
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) {
            if (g_quit) break;
            continue;
        }
        handlePacket(buf, n, from);
    }
}

static bool startNetwork() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    BOOL off = FALSE;
    DWORD ret = 0;
    // Stop ICMP "port unreachable" from turning into WSAECONNRESET on recvfrom.
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    int rcv = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof(rcv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_sock, (sockaddr*)&a, sizeof(a)) != 0) return false;
    std::thread(netThread).detach();
    return true;
}

// ----------------------------------------------------------------------------- liquid glass renderer

struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* px = nullptr;
    int w = 0, h = 0;
    void release() {
        if (dc) { SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); }
        dc = nullptr; bmp = nullptr; px = nullptr; w = h = 0;
    }
    bool create(int W, int H) {
        release();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (!bmp) { DeleteDC(dc); dc = nullptr; return false; }
        old = SelectObject(dc, bmp);
        w = W; h = H;
        return true;
    }
};

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct RGBf { float r, g, b; };

// Bilinear sample of the captured background, clamped to the valid (on-screen) rect.
static float sampleCh(const Dib& bg, const RECT& valid, float x, float y, int shift) {
    x = clampf(x, (float)valid.left, valid.right - 1.001f);
    y = clampf(y, (float)valid.top, valid.bottom - 1.001f);
    int x0 = (int)x, y0 = (int)y;
    float fx = x - x0, fy = y - y0;
    int x1 = std::min(x0 + 1, (int)valid.right - 1), y1 = std::min(y0 + 1, (int)valid.bottom - 1);
    auto c = [&](int xx, int yy) { return (float)((bg.px[yy * bg.w + xx] >> shift) & 255); };
    float top = c(x0, y0) + (c(x1, y0) - c(x0, y0)) * fx;
    float bot = c(x0, y1) + (c(x1, y1) - c(x0, y1)) * fx;
    return (top + (bot - top) * fy) / 255.f;
}

struct LensStyle { float r, g, b; };

static void renderLens(Dib& out, const Dib* bg, const RECT& valid, float R, float contactAnim,
                       float pressure, const LensStyle& accent, bool showArc) {
    const float cx = out.w * 0.5f, cy = out.h * 0.5f;
    const float Lx = -0.52f, Ly = -0.85f; // light from the top-left
    const float arcR = R + 3.6f * (R / 20.f) + 1.5f;
    const float arcW = std::max(1.4f, R * 0.075f);
    const float arcEnd = pressure * 6.2831853f;
    const float ringA = 0.30f + 0.65f * contactAnim;

    for (int y = 0; y < out.h; y++) {
        uint32_t* row = out.px + y * out.w;
        for (int x = 0; x < out.w; x++) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            float pr = 0, pg = 0, pb = 0, pa = 0;
            auto over = [&](float r, float g, float b, float a) {
                if (a <= 0) return;
                a = clampf(a, 0.f, 1.f);
                pr = r * a + pr * (1 - a);
                pg = g * a + pg * (1 - a);
                pb = b * a + pb * (1 - a);
                pa = a + pa * (1 - a);
            };

            // 1. soft drop shadow, slightly below the lens
            const float sdy = dy - R * 0.12f;
            const float sd = std::sqrt(dx * dx + sdy * sdy);
            over(0, 0, 0, 0.17f * (1.f - smooth(R * 0.9f, R * 1.36f, sd)));

            // 2. the glass body
            const float cov = clampf(R - d + 0.5f, 0.f, 1.f);
            if (cov > 0) {
                const float rn = d / R;
                const float nx = d > 0.001f ? dx / d : 0.f, ny = d > 0.001f ? dy / d : 0.f;
                float gr, gg, gb, ga;
                if (bg) {
                    // Edge refraction: the rim pulls in content from outside the lens,
                    // the centre stays 1:1 so the pen point is never displaced.
                    const float k = 0.36f * rn * rn * rn * rn;
                    const float bx = cx - 0.5f, by = cy - 0.5f;
                    if (k > 0.003f) { // slight chromatic split at the rim
                        gr = sampleCh(*bg, valid, bx + dx * (1 + k * 1.12f), by + dy * (1 + k * 1.12f), 16);
                        gg = sampleCh(*bg, valid, bx + dx * (1 + k), by + dy * (1 + k), 8);
                        gb = sampleCh(*bg, valid, bx + dx * (1 + k * 0.88f), by + dy * (1 + k * 0.88f), 0);
                    } else {
                        gr = sampleCh(*bg, valid, bx + dx, by + dy, 16);
                        gg = sampleCh(*bg, valid, bx + dx, by + dy, 8);
                        gb = sampleCh(*bg, valid, bx + dx, by + dy, 0);
                    }
                    // faint cool tint + lift
                    gr = gr * 0.93f + 0.07f * 0.86f;
                    gg = gg * 0.93f + 0.07f * 0.94f;
                    gb = gb * 0.93f + 0.07f * 1.00f;
                    ga = 1.f;
                } else {
                    gr = 0.86f; gg = 0.93f; gb = 1.f;
                    ga = 0.07f + 0.12f * rn * rn;
                }
                // Fresnel rim + top-left specular crescent + bottom-right inner bounce
                const float rim = smooth(0.62f, 1.0f, rn);
                const float ndl = nx * Lx + ny * Ly;
                const float band = smooth(0.74f, 0.88f, rn) * (1.f - smooth(0.95f, 1.0f, rn));
                const float spec = std::pow(std::max(0.f, ndl), 2.2f) * band * 0.95f;
                const float bounce = std::pow(std::max(0.f, -ndl), 3.f) * band * 0.45f;
                // soft sheen across the upper half, like light lying on a curved surface
                const float sheen = std::max(0.f, -dy / R) * (1.f - rn * rn) * 0.10f;
                const float lift = clampf(spec + bounce + rim * 0.06f + sheen, 0.f, 1.f);
                gr += (1 - gr) * lift; gg += (1 - gg) * lift; gb += (1 - gb) * lift;
                if (!bg) ga = clampf(ga + lift * 0.9f, 0.f, 1.f);
                // bright glass edge line, then a faint dark hairline just outside it so
                // the lens reads on both white and black canvases
                const float ed = (d - (R - 1.3f)) / 0.75f;
                const float edge = std::exp(-ed * ed) * (0.45f + 0.35f * std::max(0.f, ndl));
                gr += (1 - gr) * edge; gg += (1 - gg) * edge; gb += (1 - gb) * edge;
                const float hd = (d - (R - 0.3f)) / 0.7f;
                const float hair = std::exp(-hd * hd) * 0.22f;
                gr *= 1 - hair; gg *= 1 - hair; gb *= 1 - hair;
                if (!bg) ga = std::max(ga, std::max(hair, edge));
                over(gr, gg, gb, ga * cov);

                // accent ring inside the rim (glows when touching)
                const float rd = std::fabs(d - (R - 2.6f));
                over(accent.r, accent.g, accent.b, clampf(1.0f - rd, 0.f, 1.f) * ringA * 0.85f);
            }

            // 3. pressure arc around the lens while drawing
            if (showArc) {
                const float ad = std::fabs(d - arcR);
                const float aa = clampf(arcW * 0.5f + 0.5f - ad, 0.f, 1.f);
                if (aa > 0) {
                    float ang = std::atan2(dx, -dy);
                    if (ang < 0) ang += 6.2831853f;
                    over(1, 1, 1, aa * 0.16f * contactAnim);
                    if (ang <= arcEnd) over(accent.r, accent.g, accent.b, aa * 0.95f * contactAnim);
                }
            }

            // 4. the exact pen point: white halo + accent core
            over(1, 1, 1, clampf(2.7f - d + 0.5f, 0.f, 1.f) * 0.95f);
            over(accent.r * 0.55f, accent.g * 0.55f, accent.b * 0.55f, clampf(1.5f - d + 0.5f, 0.f, 1.f));

            row[x] = ((uint32_t)(pa * 255.f + 0.5f) << 24) | ((uint32_t)(pr * 255.f + 0.5f) << 16) |
                     ((uint32_t)(pg * 255.f + 0.5f) << 8) | (uint32_t)(pb * 255.f + 0.5f);
        }
    }
}

// ----------------------------------------------------------------------------- overlay window

static Dib g_out, g_bg;
static HDC g_screenDC;
static bool g_overlayShown = false;
static bool g_canExclude = false;
static float g_contactAnim = 0.f;
static Hover g_drawn;
static const UINT_PTR kAnimTimer = 1;

static float lensRadius(int x, int y) {
    static const float dip[3] = {15.f, 21.f, 29.f};
    UINT dpiX = 96, dpiY = 96;
    HMONITOR m = MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST);
    GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    return dip[g_cfg.size] * dpiX / 96.f;
}

static void hideOverlay() {
    if (g_overlayShown) {
        ShowWindow(g_overlayWnd, SW_HIDE);
        KillTimer(g_overlayWnd, kAnimTimer);
        g_overlayShown = false;
    }
}

static void drawOverlay() {
    Hover h;
    {
        std::lock_guard<std::mutex> lk(g_hoverMx);
        h = g_hover;
    }
    if (!h.visible) {
        g_contactAnim = 0;
        hideOverlay();
        return;
    }
    g_drawn = h;
    g_contactAnim += ((h.contact ? 1.f : 0.f) - g_contactAnim) * 0.35f;
    if (std::fabs(g_contactAnim - (h.contact ? 1.f : 0.f)) < 0.01f) g_contactAnim = h.contact ? 1.f : 0.f;

    const float baseR = lensRadius(h.x, h.y);
    const float R = baseR * (1.f - 0.14f * g_contactAnim);
    const int S = 2 * (int)std::ceil(baseR * 1.42f + 8.f);
    if (g_out.w != S && (!g_out.create(S, S) || !g_bg.create(S, S))) return;

    const int x0 = h.x - S / 2, y0 = h.y - S / 2;
    const bool lens = g_cfg.lens && g_canExclude;
    RECT valid{0, 0, S, S};
    if (lens) {
        // Grab what is under the lens. Our own window is excluded from capture
        // (WDA_EXCLUDEFROMCAPTURE), so this is the real desktop content.
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        valid.left = std::max(0, vx - x0);
        valid.top = std::max(0, vy - y0);
        valid.right = std::min(S, vx + vw - x0);
        valid.bottom = std::min(S, vy + vh - y0);
        if (valid.right - valid.left < 2 || valid.bottom - valid.top < 2) valid = RECT{0, 0, S, S};
        BitBlt(g_bg.dc, 0, 0, S, S, g_screenDC, x0, y0, SRCCOPY);
    }

    LensStyle accent{0.62f, 0.86f, 1.0f};                      // hover: ice blue
    if (h.contact) accent = {0.22f, 0.74f, 0.97f};             // drawing: sky
    if (h.barrel) accent = {1.0f, 0.70f, 0.25f};               // side button: amber
    if (h.eraser) accent = {1.0f, 0.38f, 0.47f};               // eraser: rose

    renderLens(g_out, lens ? &g_bg : nullptr, valid, R, g_contactAnim, h.pressure, accent, g_contactAnim > 0.02f);

    POINT dst{x0, y0}, src{0, 0};
    SIZE sz{S, S};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_overlayWnd, g_screenDC, &dst, &sz, g_out.dc, &src, 0, &bf, ULW_ALPHA);
    if (!g_overlayShown) {
        SetWindowPos(g_overlayWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_overlayShown = true;
        // ~60 Hz refresh while visible: keeps the refraction live and finishes animations.
        SetTimer(g_overlayWnd, kAnimTimer, 16, nullptr);
    }
}

static LRESULT CALLBACK overlayProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_APP_OVERLAY:
        g_overlayPending = false;
        drawOverlay();
        return 0;
    case WM_TIMER:
        if (wp == kAnimTimer) drawOverlay();
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ----------------------------------------------------------------------------- tray

static NOTIFYICONDATAW g_nid{};
static UINT g_taskbarCreated;
static HICON g_icon;

static HICON makeIcon(int size) {
    Dib d;
    if (!d.create(size, size)) return LoadIcon(nullptr, IDI_APPLICATION);
    RECT v{0, 0, size, size};
    renderLens(d, nullptr, v, size * 0.40f, 1.f, 0.f, LensStyle{0.22f, 0.74f, 0.97f}, false);
    // Give the icon body a bit more presence than the on-screen lens.
    for (int i = 0; i < size * size; i++) {
        uint32_t c = d.px[i];
        uint32_t a = c >> 24;
        uint32_t na = std::min<uint32_t>(255, a * 2);
        if (a) {
            float s = (float)na / a;
            uint32_t r = std::min<uint32_t>(255, (uint32_t)(((c >> 16) & 255) * s));
            uint32_t g = std::min<uint32_t>(255, (uint32_t)(((c >> 8) & 255) * s));
            uint32_t b = std::min<uint32_t>(255, (uint32_t)((c & 255) * s));
            d.px[i] = (na << 24) | (r << 16) | (g << 8) | b;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, d.bmp};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    d.release();
    return ic ? ic : LoadIcon(nullptr, IDI_APPLICATION);
}

static std::wstring statusText() {
    std::lock_guard<std::mutex> lk(g_clientMx);
    if (!g_client.connected) return L"Waiting for phone on port 47800…";
    wchar_t ip[32];
    InetNtopW(AF_INET, &g_client.addr.sin_addr, ip, 32);
    return L"Connected: " + g_client.name + L" (" + ip + L")";
}

static void updateTip() {
    std::wstring t = L"S Pen Bridge\n" + statusText();
    DWORD err = g_lastInjectError;
    if (err) t += L"\nInject error " + std::to_wstring(err);
    wcsncpy_s(g_nid.szTip, t.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void addTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, L"S Pen Bridge");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    updateTip();
}

static void balloon(const std::wstring& title, const std::wstring& text) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, L"SpenBridge", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static std::wstring exePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + exePath() + L"\"";
        RegSetValueExW(k, L"SpenBridge", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"SpenBridge");
    }
    RegCloseKey(k);
}

static void allowFirewall() {
    std::wstring exe = exePath();
    std::wstring args = L"/c netsh advfirewall firewall delete rule name=\"S Pen Bridge\" >nul & "
                        L"netsh advfirewall firewall add rule name=\"S Pen Bridge\" dir=in action=allow protocol=UDP "
                        L"localport=47800 program=\"" + exe + L"\" profile=private,domain enable=yes";
    ShellExecuteW(nullptr, L"runas", L"cmd.exe", args.c_str(), nullptr, SW_HIDE);
}

static void broadcastWelcome() {
    sockaddr_in to;
    uint32_t session;
    {
        std::lock_guard<std::mutex> lk(g_clientMx);
        if (!g_client.connected) return;
        to = g_client.addr;
        session = g_client.session;
    }
    sendWelcome(to, session);
}

static void showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS, statusText().c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU mon = CreatePopupMenu();
    {
        std::lock_guard<std::mutex> lk(g_monMx);
        for (size_t i = 0; i < g_monitors.size(); i++) {
            const RECT& r = g_monitors[i].rc;
            wchar_t label[96];
            swprintf_s(label, L"Monitor %zu   %ld×%ld%s", i + 1, r.right - r.left, r.bottom - r.top,
                       g_monitors[i].primary ? L"   (primary)" : L"");
            bool sel = EqualRect(&r, &g_target);
            AppendMenuW(mon, MF_STRING | (sel ? MF_CHECKED : 0), ID_MONITOR_BASE + i, label);
        }
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mon, L"Draw on");

    HMENU size = CreatePopupMenu();
    AppendMenuW(size, MF_STRING | (g_cfg.size == 0 ? MF_CHECKED : 0), ID_SIZE_S, L"Small");
    AppendMenuW(size, MF_STRING | (g_cfg.size == 1 ? MF_CHECKED : 0), ID_SIZE_M, L"Medium");
    AppendMenuW(size, MF_STRING | (g_cfg.size == 2 ? MF_CHECKED : 0), ID_SIZE_L, L"Large");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)size, L"Lens size");

    AppendMenuW(menu, MF_STRING | (g_cfg.lens ? MF_CHECKED : 0) | (g_canExclude ? 0 : MF_GRAYED), ID_LENS,
                L"Glass refraction");
    AppendMenuW(menu, MF_STRING | (g_cfg.hoverInject ? MF_CHECKED : 0), ID_HOVER,
                L"Send hover to apps (moves the mouse while hovering)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_FIREWALL, L"Allow through firewall (admin)…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_mainWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void onCommand(int id) {
    if (id >= ID_MONITOR_BASE && id < ID_MONITOR_BASE + 64) {
        g_cfg.monitor = id - ID_MONITOR_BASE;
        refreshMonitors();
        releasePen();
        broadcastWelcome();
    } else switch (id) {
    case ID_HOVER: g_cfg.hoverInject = !g_cfg.hoverInject; releasePen(); break;
    case ID_LENS: g_cfg.lens = !g_cfg.lens; break;
    case ID_SIZE_S: g_cfg.size = 0; break;
    case ID_SIZE_M: g_cfg.size = 1; break;
    case ID_SIZE_L: g_cfg.size = 2; break;
    case ID_AUTOSTART: setAutostart(!autostartEnabled()); break;
    case ID_FIREWALL: allowFirewall(); break;
    case ID_EXIT: DestroyWindow(g_mainWnd); return;
    }
    saveConfig();
}

static LRESULT CALLBACK mainProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == g_taskbarCreated && g_taskbarCreated) {
        addTray();
        return 0;
    }
    switch (m) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            showMenu();
            break;
        }
        return 0;
    case WM_COMMAND:
        onCommand(LOWORD(wp));
        return 0;
    case WM_APP_STATUS:
        updateTip();
        if (wp == 1) balloon(L"Phone connected", statusText());
        return 0;
    case WM_TIMER: {
        // Watchdog: never leave a pen stuck down if packets stop.
        const ULONGLONG now = GetTickCount64();
        bool stalePen = false, lost = false;
        {
            std::lock_guard<std::mutex> lk(g_clientMx);
            if (g_client.connected) {
                stalePen = now - g_client.lastPen > 600;
                if (now - g_client.lastPacket > 4000) { g_client.connected = false; lost = true; }
            }
        }
        bool active;
        {
            std::lock_guard<std::mutex> lk(g_penMx);
            active = g_injRange || g_injContact;
        }
        if ((stalePen || lost) && (active || g_overlayShown)) releasePen();
        if (lost) updateTip();
        return 0;
    }
    case WM_DISPLAYCHANGE:
        refreshMonitors();
        broadcastWelcome();
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ----------------------------------------------------------------------------- snapshot (dev aid)

// SpenBridge.exe --snapshot out.bmp X Y [pressure 0..1]
// Renders the lens over the real screen at X,Y (4x zoom) so its look can be checked
// even though the live lens is excluded from screenshots.
static int snapshotMode(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    const int x = _wtoi(argv[3]), y = _wtoi(argv[4]);
    const float pressure = argc > 5 ? (float)_wtof(argv[5]) : -1.f;
    const bool contact = pressure >= 0;
    const float baseR = lensRadius(x, y);
    const int S = 2 * (int)std::ceil(baseR * 1.42f + 8.f);
    Dib bg, out;
    if (!bg.create(S, S) || !out.create(S, S)) return 1;
    HDC sdc = GetDC(nullptr);
    BitBlt(bg.dc, 0, 0, S, S, sdc, x - S / 2, y - S / 2, SRCCOPY);
    ReleaseDC(nullptr, sdc);
    RECT valid{0, 0, S, S};
    LensStyle accent = contact ? LensStyle{0.22f, 0.74f, 0.97f} : LensStyle{0.62f, 0.86f, 1.0f};
    renderLens(out, &bg, valid, baseR * (contact ? 0.86f : 1.f), contact ? 1.f : 0.f, std::max(0.f, pressure), accent, contact);

    const int Z = 4, W = S * Z;
    std::vector<uint8_t> pix((size_t)W * W * 3);
    for (int yy = 0; yy < W; yy++)
        for (int xx = 0; xx < W; xx++) {
            uint32_t o = out.px[(yy / Z) * S + xx / Z], b = bg.px[(yy / Z) * S + xx / Z];
            float a = (o >> 24) / 255.f;
            uint8_t* p = &pix[((size_t)(W - 1 - yy) * W + xx) * 3]; // bottom-up BMP
            for (int c = 0; c < 3; c++)
                p[c] = (uint8_t)std::clamp((int)(((o >> (c * 8)) & 255) + ((b >> (c * 8)) & 255) * (1 - a)), 0, 255);
        }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = W; ih.biHeight = W; ih.biPlanes = 1; ih.biBitCount = 24;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)pix.size();
    FILE* f = _wfopen(argv[2], L"wb");
    if (!f) return 1;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(pix.data(), 1, pix.size(), f);
    fclose(f);
    return 0;
}

// ----------------------------------------------------------------------------- main

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc >= 2 && wcscmp(argv[1], L"--snapshot") == 0) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            loadConfig();
            return snapshotMode(argc, argv);
        }
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\SpenBridgeSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"S Pen Bridge is already running (see the tray).", L"S Pen Bridge", MB_ICONINFORMATION);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    loadConfig();
    refreshMonitors();

    wchar_t host[256];
    DWORD hn = 256;
    GetComputerNameW(host, &hn);
    g_pcName = utf8(host);

    g_pen = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE);
    if (!g_pen) {
        MessageBoxW(nullptr, L"Could not create the synthetic pen device.\nWindows 10 1809 or newer is required.",
                    L"S Pen Bridge", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.lpfnWndProc = mainProc;
    wc.lpszClassName = L"SpenBridgeMain";
    RegisterClassW(&wc);
    g_mainWnd = CreateWindowExW(0, wc.lpszClassName, L"S Pen Bridge", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    WNDCLASSW oc{};
    oc.hInstance = inst;
    oc.lpfnWndProc = overlayProc;
    oc.lpszClassName = L"SpenBridgeLens";
    RegisterClassW(&oc);
    g_overlayWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   oc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    // Keep the lens out of every screen capture, including our own refraction grab.
    g_canExclude = SetWindowDisplayAffinity(g_overlayWnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    g_screenDC = GetDC(nullptr);

    g_icon = makeIcon(GetSystemMetrics(SM_CXSMICON) * 2);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();

    if (!startNetwork()) {
        MessageBoxW(nullptr, L"Could not open UDP port 47800. Is another copy running?", L"S Pen Bridge", MB_ICONERROR);
        return 1;
    }
    SetTimer(g_mainWnd, 1, 100, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_quit = true;
    releasePen();
    closesocket(g_sock);
    WSACleanup();
    DestroyWindow(g_overlayWnd);
    if (g_pen) DestroySyntheticPointerDevice(g_pen);
    ReleaseDC(nullptr, g_screenDC);
    CloseHandle(single);
    return 0;
}
