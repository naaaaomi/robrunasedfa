/* menu_webview.cpp - WebView2 host for the embedded HTML settings menu.
 *
 * A dedicated window thread hosts the page compiled into app.rc. BR_Config
 * provides settings shared with the overlay. Messages are strings:
 *   HTML -> host: set:<key>:<int>, col:<index>:r,g,b, act:save|load,
 *                 profile_save:<name>, profile_load:<name>, drag:<x>,<y>,<dx>,<dy>, close:, hello:
 *   host -> HTML: state:<json>, telem:<json>, toast:<text>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <climits>
#include <string.h>
#include <wchar.h>

#include "menu_bridge.h"
#include "app_resources.h"
#include "wv2_cb.h"
#include "rr_game.h"
#include "rr_mem.h"
#include "rr_unity.h"

#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <combaseapi.h>
#include "WebView2.h"
#include "wv2_cb_impl.h"
#pragma comment(lib, "WebView2Loader.dll.lib")

static HWND g_hwnd = NULL;
static ICoreWebView2Environment* g_env = nullptr;
static ICoreWebView2* g_wv = nullptr;
static ICoreWebView2Controller* g_ctl = nullptr;
static HANDLE g_thread = NULL;
static DWORD g_thread_id = 0;
static volatile LONG g_quit = 0;
static volatile LONG g_visible = 1;
static bool g_dragging = false;
static bool g_page_ready = false;
static HRESULT g_start_error = S_OK;
static HWND g_prev_fg = NULL;
static POINT g_drag_origin = { 0, 0 };
static const int WIN_W = 976;
static const int WIN_H = 640;

static void menu_log(const char* stage, HRESULT hr) {
    if (FILE* f = fopen("wv2_error.log", "a")) {
        fprintf(f, "%lu Port menu: %s (0x%08lX)\n", GetTickCount(), stage,
                static_cast<unsigned long>(hr));
        fclose(f);
    }
}

static void menu_failed(const char* stage, HRESULT hr) {
    g_start_error = FAILED(hr) ? hr : E_FAIL;
    menu_log(stage, g_start_error);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static HRESULT navigate_menu(ICoreWebView2* webview) {
    HMODULE module = GetModuleHandleW(NULL);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_MENU_HTML), MAKEINTRESOURCEW(10));
    if (!resource) return E_FAIL;
    HGLOBAL data = LoadResource(module, resource);
    const char* bytes = static_cast<const char*>(LockResource(data));
    int size = (int)SizeofResource(module, resource);
    if (!bytes || size <= 0) return E_FAIL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, NULL, 0);
    if (!count) return E_FAIL;
    std::wstring html((size_t)count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, &html[0], count))
        return E_FAIL;
    return webview->NavigateToString(html.c_str());
}

static void jcat_int(std::string& s, const char* k, int v, bool first = false) {
    char b[96];
    snprintf(b, sizeof(b), "%s\"%s\":%d", first ? "" : ",", k, v);
    s += b;
}

static std::string build_state_json(BR_Config* b) {
    std::string s("{");
#define BR_JSON(field, initial, minimum, maximum) jcat_int(s, #field, b->field, s.size() == 1);
    BR_SETTINGS(BR_JSON)
#undef BR_JSON
    s += ",\"colors\":{";
    for (int i = 0; i < BR_COLOR_COUNT; ++i) {
        char rgb[40];
        snprintf(rgb, sizeof(rgb), "%s\"%d\":[%u,%u,%u]", i ? "," : "", i,
                 (unsigned)b->color[i].rgb[0], (unsigned)b->color[i].rgb[1],
                 (unsigned)b->color[i].rgb[2]);
        s += rgb;
    }
    s += "}}";
    return s;
}

static std::string build_telem_json(BR_Config* b) {
    std::string s("{");
    jcat_int(s, "pid", b->telem_game_pid, true);
    jcat_int(s, "targets", b->telem_targets);
    jcat_int(s, "los", b->telem_los_state);
    s += "}";
    return s;
}

static void wv_send(const std::string& msg) {
    if (!g_wv) return;
    std::wstring w(msg.begin(), msg.end());
    g_wv->PostWebMessageAsString(w.c_str());
}

/* Return only fixed profile paths; no message-supplied path reaches the filesystem. */
static int profile_path(const char* profile, wchar_t* path, size_t capacity) {
    const wchar_t* filename = NULL;
    wchar_t exe[MAX_PATH];
    wchar_t folder[MAX_PATH];
    DWORD length;

    if (!profile || !path || capacity == 0) return 0;
    if (_stricmp(profile, "rage") == 0) filename = L"Rage.cfg";
    else if (_stricmp(profile, "legit") == 0) filename = L"Legit.cfg";
    else if (_stricmp(profile, "closet") == 0) filename = L"Closet.cfg";
    else return 0;

    length = GetModuleFileNameW(NULL, exe, _countof(exe));
    if (!length || length >= _countof(exe)) return 0;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return 0;
    *slash = L'\0';

    if (swprintf_s(folder, _countof(folder), L"%ls\\Profiles", exe) < 0) return 0;
    if (!CreateDirectoryW(folder, NULL)) {
        DWORD error = GetLastError();
        DWORD attrs = GetFileAttributesW(folder);
        if (error != ERROR_ALREADY_EXISTS || attrs == INVALID_FILE_ATTRIBUTES ||
            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    }
    return swprintf_s(path, capacity, L"%ls\\%ls", folder, filename) >= 0;
}

static int profile_save(const char* profile, const BR_Config* config) {
    wchar_t path[MAX_PATH];
    wchar_t temp[MAX_PATH + 32];
    FILE* file;
    int ok = 1;

    if (!config || !profile_path(profile, path, _countof(path))) return 0;
    if (swprintf_s(temp, _countof(temp), L"%ls.%lu.tmp", path,
                   (unsigned long)GetCurrentProcessId()) < 0) return 0;
    file = _wfopen(temp, L"w");
    if (!file) return 0;

    fputs("# ROB RUN settings profile\n", file);
#define BR_PROFILE_WRITE(field, initial, minimum, maximum) \
    fprintf(file, #field "=%d\n", config->field);
    BR_SETTINGS(BR_PROFILE_WRITE)
#undef BR_PROFILE_WRITE

    for (int i = 0; i < BR_COLOR_COUNT; ++i) {
        fprintf(file, "color%d=%u,%u,%u\n", i,
                (unsigned)config->color[i].rgb[0],
                (unsigned)config->color[i].rgb[1],
                (unsigned)config->color[i].rgb[2]);
    }

    if (ferror(file)) ok = 0;
    if (fclose(file) != 0) ok = 0;
    if (ok && MoveFileExW(temp, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(temp);
    return 0;
}

static int profile_load(const char* profile, BR_Config* config) {
    wchar_t path[MAX_PATH];
    FILE* file;
    BR_Config loaded;
    char line[256];
    int ok;

    if (!config || !profile_path(profile, path, _countof(path))) return 0;
    file = _wfopen(path, L"r");
    if (!file) return 0;
    loaded = *config;

    while (fgets(line, sizeof(line), file)) {
        char* newline = strpbrk(line, "\r\n");
        char* value;
        if (newline) *newline = '\0';
        if (!line[0] || line[0] == '#') continue;
        value = strchr(line, '=');
        if (!value) continue;
        *value++ = '\0';

        if (strncmp(line, "color", 5) == 0) {
            char* end = NULL;
            long index = strtol(line + 5, &end, 10);
            int red, green, blue;
            if (end != line + 5 && *end == '\0' && index >= 0 &&
                index < BR_COLOR_COUNT &&
                sscanf(value, "%d,%d,%d", &red, &green, &blue) == 3 &&
                (unsigned)red <= 255 && (unsigned)green <= 255 &&
                (unsigned)blue <= 255) {
                loaded.color[index].rgb[0] = (unsigned char)red;
                loaded.color[index].rgb[1] = (unsigned char)green;
                loaded.color[index].rgb[2] = (unsigned char)blue;
            }
            continue;
        }

        char* end = NULL;
        long number = strtol(value, &end, 10);
        if (end != value && *end == '\0' && number >= INT_MIN && number <= INT_MAX)
            (void)br_config_set(&loaded, line, (int)number);
    }

    ok = !ferror(file);
    fclose(file);
    if (!ok) return 0;

#define BR_PROFILE_APPLY(field, initial, minimum, maximum) config->field = loaded.field;
    BR_SETTINGS(BR_PROFILE_APPLY)
#undef BR_PROFILE_APPLY
    memcpy(config->color, loaded.color, sizeof(config->color));
    return 1;
}

static const char* profile_label(const std::string& profile) {
    if (_stricmp(profile.c_str(), "rage") == 0) return "Rage";
    if (_stricmp(profile.c_str(), "legit") == 0) return "Legit";
    if (_stricmp(profile.c_str(), "closet") == 0) return "Closet";
    return NULL;
}

static HRESULT STDMETHODCALLTYPE msg_cb(ICoreWebView2* sender,
    ICoreWebView2WebMessageReceivedEventArgs* args, void* ctx) {
    (void)sender;
    (void)ctx;
    if (!args) return S_OK;
    LPWSTR w = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&w)) || !w) return S_OK;
    std::string msg;
    for (const wchar_t* p = w; *p; ++p) if (*p < 128) msg += (char)*p;
    CoTaskMemFree(w);

    BR_Config* b = br_get();
    size_t c1 = msg.find(':');
    if (c1 == std::string::npos) return S_OK;
    std::string cmd = msg.substr(0, c1);
    std::string body = msg.substr(c1 + 1);

    if (cmd == "set") {
        size_t c2 = body.find(':');
        if (c2 != std::string::npos)
            br_config_set(b, body.substr(0, c2).c_str(), atoi(body.substr(c2 + 1).c_str()));
    }
    else if (cmd == "col") {
        int index, red, green, blue;
        if (sscanf(body.c_str(), "%d:%d,%d,%d", &index, &red, &green, &blue) == 4 &&
            index >= 0 && index < BR_COLOR_COUNT &&
            (unsigned)red <= 255 && (unsigned)green <= 255 && (unsigned)blue <= 255) {
            b->color[index].rgb[0] = (unsigned char)red;
            b->color[index].rgb[1] = (unsigned char)green;
            b->color[index].rgb[2] = (unsigned char)blue;
        }
    }
    else if (cmd == "act") {
        if (body == "save") {
            wv_send(br_config_save() ? "toast:Settings saved" : "toast:Could not save CoreRR.cfg");
        } else if (body == "load") {
            if (br_config_load()) {
                wv_send("state:" + build_state_json(b));
                wv_send("toast:Settings loaded");
            } else {
                wv_send("toast:Could not load CoreRR.cfg");
            }
        }
    }
    else if (cmd == "profile_save") {
        const char* label = profile_label(body);
        if (!label) {
            wv_send("toast:Unknown profile");
        } else if (profile_save(body.c_str(), b)) {
            wv_send(std::string("toast:") + label + " profile saved");
        } else {
            wv_send(std::string("toast:Could not save ") + label + " profile");
        }
    }
    else if (cmd == "profile_load") {
        const char* label = profile_label(body);
        if (!label) {
            wv_send("toast:Unknown profile");
        } else if (profile_load(body.c_str(), b)) {
            wv_send("state:" + build_state_json(b));
            wv_send(std::string("toast:") + label + " profile loaded");
        } else {
            wv_send(std::string("toast:") + label + " profile not found or could not be loaded");
        }
    }
    else if (cmd == "drag") {
        int sx = 0, sy = 0, dx = 0, dy = 0;
        if (sscanf(body.c_str(), "%d,%d,%d,%d", &sx, &sy, &dx, &dy) == 4) {
            if (!g_dragging) {
                g_dragging = true;
                RECT wr;
                GetWindowRect(g_hwnd, &wr);
                g_drag_origin.x = wr.left;
                g_drag_origin.y = wr.top;
            }
            SetWindowPos(g_hwnd, NULL, g_drag_origin.x + dx, g_drag_origin.y + dy,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            (void)sx;
            (void)sy;
        }
    }
    else if (cmd == "dragend") {
        g_dragging = false;
        g_drag_origin.x = g_drag_origin.y = 0;
    }
    else if (cmd == "close") {
        br_set_menu_visible(0);
    }
    else if (cmd == "hello") {
        if (!g_page_ready) menu_log("embedded menu ready", S_OK);
        g_page_ready = true;
        g_start_error = S_OK;
        wv_send("state:" + build_state_json(b));
        wv_send("telem:" + build_telem_json(b));
    }
    return S_OK;
}

static LRESULT WINAPI wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DESTROY: PostQuitMessage(0); break;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(h, &paint);
        if (!g_page_ready) {
            RECT rect;
            GetClientRect(h, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(241, 235, 251));
            SetBkMode(dc, TRANSPARENT);
            const wchar_t* text = FAILED(g_start_error)
                ? L"Felonious Funnery could not open WebView2. See wv2_error.log. Press F1 to close."
                : L"Loading Felonious Funnery...";
            DrawTextW(dc, text, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        EndPaint(h, &paint);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static HRESULT controller_ready(HRESULT hr, ICoreWebView2Controller* controller) {
    if (FAILED(hr) || !controller) {
        menu_failed("create browser controller", hr);
        return S_OK;
    }
    g_ctl = controller;
    g_ctl->AddRef();
    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    g_ctl->put_Bounds(bounds);
    g_ctl->put_IsVisible(IsWindowVisible(g_hwnd));

    hr = g_ctl->get_CoreWebView2(&g_wv);
    if (SUCCEEDED(hr) && g_wv) {
        auto handler = MakeMsgCb([](ICoreWebView2* sender,
            ICoreWebView2WebMessageReceivedEventArgs* args) {
                return msg_cb(sender, args, nullptr);
            });
        EventRegistrationToken token{};
        hr = g_wv->add_WebMessageReceived(handler, &token);
        handler->Release();
        if (SUCCEEDED(hr)) hr = navigate_menu(g_wv);
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        g_ctl->put_IsVisible(FALSE);
        menu_failed("open embedded menu", hr);
    }
    return S_OK;
}

static void start_webview(const wchar_t* data_dir, bool allow_retry);

static void browser_start_failed(HRESULT hr, bool allow_retry) {
    wchar_t fallback[MAX_PATH];
    DWORD length = GetTempPathW(_countof(fallback), fallback);
    if (allow_retry && length && length < _countof(fallback) &&
        swprintf_s(fallback + length, _countof(fallback) - length,
                   L"RRToolPortWebView2_%lu", GetCurrentProcessId()) >= 0) {
        menu_log("retry browser with a separate profile", hr);
        start_webview(fallback, false);
        return;
    }
    menu_failed("create browser environment", hr);
}

static void start_webview(const wchar_t* data_dir, bool allow_retry) {
    auto handler = MakeEnvCb([allow_retry](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(hr) || !env) {
            browser_start_failed(FAILED(hr) ? hr : E_FAIL, allow_retry);
            return S_OK;
        }
        g_env = env;
        g_env->AddRef();
        auto controller = MakeCtlCb(controller_ready);
        hr = env->CreateCoreWebView2Controller(g_hwnd, controller);
        controller->Release();
        if (FAILED(hr)) menu_failed("request browser controller", hr);
        return S_OK;
    });
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data_dir, nullptr, handler);
    handler->Release();
    if (FAILED(hr)) browser_start_failed(hr, allow_retry);
}

static DWORD WINAPI menu_thread(LPVOID) {
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { menu_log("initialize COM", com); return 1; }
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"FeloniousFunneryMenu";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
        L"Felonious Funnery", WS_POPUP, sw / 2 - WIN_W / 2, 80, WIN_W, WIN_H,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        menu_log("create menu window", HRESULT_FROM_WIN32(GetLastError()));
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    wchar_t data_dir[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", data_dir, _countof(data_dir));
    if (!length || length >= _countof(data_dir))
        length = GetTempPathW(_countof(data_dir), data_dir);
    if (!length || length >= _countof(data_dir) ||
        wcscat_s(data_dir, L"\\RRToolPortWebView2") != 0) {
        menu_failed("resolve browser profile directory", E_FAIL);
    } else {
        start_webview(data_dir, true);
    }

    BR_Config* b = br_get();
    DWORD last_telem = 0;
    DWORD shown_at = 0;
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT && !g_quit) {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (!InterlockedCompareExchange(&g_visible, 0, 0)) {
            if (IsWindowVisible(g_hwnd)) {
                if (g_ctl) g_ctl->put_IsVisible(FALSE);
                ShowWindow(g_hwnd, SW_HIDE);
                if (g_prev_fg && IsWindow(g_prev_fg)) SetForegroundWindow(g_prev_fg);
                g_prev_fg = NULL;
            }
            MsgWaitForMultipleObjectsEx(0, NULL, 80, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        if (!IsWindowVisible(g_hwnd)) {
            shown_at = GetTickCount();
            g_prev_fg = GetForegroundWindow();
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
            if (g_ctl && SUCCEEDED(g_start_error)) g_ctl->put_IsVisible(TRUE);
        }

        DWORD now = GetTickCount();
        if (!g_page_ready && SUCCEEDED(g_start_error) && now - shown_at > 15000) {
            if (g_ctl) g_ctl->put_IsVisible(FALSE);
            menu_failed("wait for embedded menu", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        if (g_wv && now - last_telem > 500) {
            last_telem = now;
            wv_send("telem:" + build_telem_json(b));
        }
        MsgWaitForMultipleObjectsEx(0, NULL, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (g_ctl) { g_ctl->Close(); g_ctl->Release(); g_ctl = nullptr; }
    if (g_wv) { g_wv->Release(); g_wv = nullptr; }
    if (g_env) { g_env->Release(); g_env = nullptr; }
    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    UnregisterClassW(L"FeloniousFunneryMenu", GetModuleHandleW(NULL));
    CoUninitialize();
    return 0;
}

/* Public menu bridge. */
static BR_Config g_br_inst;
static volatile LONG g_br_inited = 0;

BR_Config* br_get(void) {
    if (InterlockedCompareExchange(&g_br_inited, 1, 0) == 0) {
        ZeroMemory(&g_br_inst, sizeof(g_br_inst));
        g_br_inst.magic = BR_MAGIC;
        g_br_inst.version = BR_VERSION;
#define BR_DEFAULT(field, initial, minimum, maximum) g_br_inst.field = initial;
        BR_SETTINGS(BR_DEFAULT)
#undef BR_DEFAULT
        g_br_inst.menu_open = 1;
        static const unsigned char pal[BR_COLOR_COUNT][3] = {
            { 86,168,255 },{255,86,86},{120,255,140},{86,255,246},
            {158,158,158},{255,255,255},{255,255,255},{255,255,255},
            {255,255,255},{255,255,255},{255,255,255},{120,255,140},
            {255,176,66},{139,92,246},{220,220,220},
        };
        for (int i = 0; i < BR_COLOR_COUNT; ++i) {
            g_br_inst.color[i].rgb[0] = pal[i][0];
            g_br_inst.color[i].rgb[1] = pal[i][1];
            g_br_inst.color[i].rgb[2] = pal[i][2];
        }
    }
    return &g_br_inst;
}

int br_start_menu(void) {
    if (g_thread) return 0;
    g_quit = 0;
    g_thread = CreateThread(NULL, 0, menu_thread, NULL, 0, &g_thread_id);
    return g_thread ? 0 : 1;
}

void br_stop_menu(void) {
    if (!g_thread) return;
    g_quit = 1;
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

void br_request_toggle(void) {
    br_set_menu_visible(!InterlockedCompareExchange(&g_visible, 0, 0));
}

void br_set_menu_visible(int visible) {
    br_get()->menu_open = visible != 0;
    InterlockedExchange(&g_visible, visible != 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
}
