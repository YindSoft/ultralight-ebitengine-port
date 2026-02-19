/*
 * ul_bridge.c - C bridge for Ultralight 1.4 (multi-view, keyboard)
 * Copyright (c) 2026 Javier Podavini (YindSoft)
 * Licensed under the MIT License. See LICENSE file in the project root.
 *
 * Architecture: dedicated worker thread for ALL Ultralight API calls.
 * Go (purego) -> ul_bridge exports -> command to worker thread -> Ultralight API.
 *
 * Build (e.g. with w64devkit):
 *   PATH="/c/w64devkit/bin:$PATH" gcc -shared -o ul_bridge.dll bridge/ul_bridge.c -O2 -lkernel32
 */

#include <windows.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* VEH/VCH exception handlers (0x406D1388 = MSVC SetThreadName) */
static LONG CALLBACK msvc_veh_handler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode == 0x406D1388)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK msvc_vch_handler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode == 0x406D1388)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Ultralight type aliases */
typedef void* ULConfig;
typedef void* ULRenderer;
typedef void* ULSession;
typedef void* ULViewConfig;
typedef void* ULView;
typedef void* ULString;
typedef void* ULSurface;
typedef void* ULMouseEvent;
typedef void* ULScrollEvent;
typedef void* ULKeyEvent;
typedef int   ULMessageSource;
typedef int   ULMessageLevel;

/* ULKeyEventType enum (matches CAPI_Defines.h) */
typedef enum {
    kKeyEventType_KeyDown = 0,
    kKeyEventType_KeyUp,
    kKeyEventType_RawKeyDown,
    kKeyEventType_Char,
} ULKeyEventType;

/* Function pointers */
typedef ULString     (*PFN_ulCreateString)(const char*);
typedef void         (*PFN_ulDestroyString)(ULString);
typedef char*        (*PFN_ulStringGetData)(ULString);
typedef size_t       (*PFN_ulStringGetLength)(ULString);
typedef ULConfig     (*PFN_ulCreateConfig)(void);
typedef void         (*PFN_ulDestroyConfig)(ULConfig);
typedef void         (*PFN_ulConfigSetResourcePathPrefix)(ULConfig, ULString);
typedef ULRenderer   (*PFN_ulCreateRenderer)(ULConfig);
typedef void         (*PFN_ulDestroyRenderer)(ULRenderer);
typedef void         (*PFN_ulUpdate)(ULRenderer);
typedef void         (*PFN_ulRefreshDisplay)(ULRenderer, unsigned int);
typedef void         (*PFN_ulRender)(ULRenderer);
typedef ULViewConfig (*PFN_ulCreateViewConfig)(void);
typedef void         (*PFN_ulDestroyViewConfig)(ULViewConfig);
typedef void         (*PFN_ulVCSetIsAccelerated)(ULViewConfig, bool);
typedef void         (*PFN_ulVCSetIsTransparent)(ULViewConfig, bool);
typedef void         (*PFN_ulVCSetInitialDeviceScale)(ULViewConfig, double);
typedef ULView       (*PFN_ulCreateView)(ULRenderer, unsigned int, unsigned int, ULViewConfig, ULSession);
typedef void         (*PFN_ulDestroyView)(ULView);
typedef void         (*PFN_ulViewLoadHTML)(ULView, ULString);
typedef void         (*PFN_ulViewLoadURL)(ULView, ULString);
typedef ULSurface    (*PFN_ulViewGetSurface)(ULView);
typedef void         (*PFN_ulViewFocus)(ULView);
typedef ULString     (*PFN_ulViewEvaluateScript)(ULView, ULString, ULString*);
typedef void (*ULConsoleCallback)(void*, ULView, ULMessageSource, ULMessageLevel, ULString, unsigned int, unsigned int, ULString);
typedef void (*PFN_ulViewSetConsoleCallback)(ULView, ULConsoleCallback, void*);
typedef ULMouseEvent  (*PFN_ulCreateMouseEvent)(int, int, int, int);
typedef void          (*PFN_ulDestroyMouseEvent)(ULMouseEvent);
typedef void          (*PFN_ulViewFireMouseEvent)(ULView, ULMouseEvent);
typedef ULScrollEvent (*PFN_ulCreateScrollEvent)(int, int, int);
typedef void          (*PFN_ulDestroyScrollEvent)(ULScrollEvent);
typedef void          (*PFN_ulViewFireScrollEvent)(ULView, ULScrollEvent);
typedef void          (*PFN_ulViewFireKeyEvent)(ULView, ULKeyEvent);
typedef ULKeyEvent    (*PFN_ulCreateKeyEvent)(ULKeyEventType, unsigned int, int, int, ULString, ULString, bool, bool, bool);
typedef void          (*PFN_ulDestroyKeyEvent)(ULKeyEvent);
typedef void*         (*PFN_ulSurfaceLockPixels)(ULSurface);
typedef void          (*PFN_ulSurfaceUnlockPixels)(ULSurface);
typedef unsigned int  (*PFN_ulSurfaceGetWidth)(ULSurface);
typedef unsigned int  (*PFN_ulSurfaceGetHeight)(ULSurface);
typedef unsigned int  (*PFN_ulSurfaceGetRowBytes)(ULSurface);
typedef void          (*PFN_ulSurfaceClearDirtyBounds)(ULSurface);
typedef const char*   (*PFN_ulVersionString)(void);
typedef void (*PFN_ulEnablePlatformFontLoader)(void);
typedef void (*PFN_ulEnablePlatformFileSystem)(ULString);
typedef void (*PFN_ulEnableDefaultLogger)(ULString);

/* Ultralight JS context access */
typedef void* JSContextRef;
typedef void* JSObjectRef;
typedef void* JSValueRef;
typedef void* JSStringRef;
typedef JSContextRef (*PFN_ulViewLockJSContext)(ULView);
typedef void         (*PFN_ulViewUnlockJSContext)(ULView);

/* JavaScriptCore C API (exported from WebCore.dll) */
typedef JSObjectRef  (*PFN_JSContextGetGlobalObject)(JSContextRef);
typedef JSStringRef  (*PFN_JSStringCreateWithUTF8CString)(const char*);
typedef void         (*PFN_JSStringRelease)(JSStringRef);
typedef size_t       (*PFN_JSStringGetMaximumUTF8CStringSize)(JSStringRef);
typedef size_t       (*PFN_JSStringGetUTF8CString)(JSStringRef, char*, size_t);
typedef JSValueRef (*JSObjectCallAsFunctionCallback)(JSContextRef ctx, JSObjectRef function,
    JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);
typedef JSObjectRef  (*PFN_JSObjectMakeFunctionWithCallback)(JSContextRef, JSStringRef, JSObjectCallAsFunctionCallback);
typedef void         (*PFN_JSObjectSetProperty)(JSContextRef, JSObjectRef, JSStringRef, JSValueRef, unsigned int, JSValueRef*);
typedef bool         (*PFN_JSValueIsString)(JSContextRef, JSValueRef);
typedef JSStringRef  (*PFN_JSValueToStringCopy)(JSContextRef, JSValueRef, JSValueRef*);

static PFN_ulCreateString              pfn_CreateString;
static PFN_ulDestroyString             pfn_DestroyString;
static PFN_ulStringGetData             pfn_StringGetData;
static PFN_ulStringGetLength           pfn_StringGetLength;
static PFN_ulCreateConfig              pfn_CreateConfig;
static PFN_ulDestroyConfig             pfn_DestroyConfig;
static PFN_ulConfigSetResourcePathPrefix pfn_ConfigSetResourcePathPrefix;
static PFN_ulCreateRenderer            pfn_CreateRenderer;
static PFN_ulDestroyRenderer          pfn_DestroyRenderer;
static PFN_ulUpdate                    pfn_Update;
static PFN_ulRefreshDisplay            pfn_RefreshDisplay;
static PFN_ulRender                    pfn_Render;
static PFN_ulCreateViewConfig          pfn_CreateViewConfig;
static PFN_ulDestroyViewConfig         pfn_DestroyViewConfig;
static PFN_ulVCSetIsAccelerated        pfn_VCSetIsAccelerated;
static PFN_ulVCSetIsTransparent        pfn_VCSetIsTransparent;
static PFN_ulVCSetInitialDeviceScale   pfn_VCSetInitialDeviceScale;
static PFN_ulCreateView                pfn_CreateView;
static PFN_ulDestroyView               pfn_DestroyView;
static PFN_ulViewLoadHTML              pfn_ViewLoadHTML;
static PFN_ulViewLoadURL               pfn_ViewLoadURL;
static PFN_ulViewGetSurface            pfn_ViewGetSurface;
static PFN_ulViewFocus                 pfn_ViewFocus;
static PFN_ulViewEvaluateScript        pfn_ViewEvaluateScript;
static PFN_ulViewSetConsoleCallback    pfn_ViewSetConsoleCallback;
static PFN_ulViewFireMouseEvent        pfn_ViewFireMouseEvent;
static PFN_ulViewFireScrollEvent       pfn_ViewFireScrollEvent;
static PFN_ulViewFireKeyEvent          pfn_ViewFireKeyEvent;
static PFN_ulCreateKeyEvent            pfn_CreateKeyEvent;
static PFN_ulDestroyKeyEvent           pfn_DestroyKeyEvent;
static PFN_ulCreateMouseEvent          pfn_CreateMouseEvent;
static PFN_ulDestroyMouseEvent         pfn_DestroyMouseEvent;
static PFN_ulCreateScrollEvent         pfn_CreateScrollEvent;
static PFN_ulDestroyScrollEvent        pfn_DestroyScrollEvent;
static PFN_ulSurfaceLockPixels         pfn_SurfaceLockPixels;
static PFN_ulSurfaceUnlockPixels       pfn_SurfaceUnlockPixels;
static PFN_ulSurfaceGetWidth           pfn_SurfaceGetWidth;
static PFN_ulSurfaceGetHeight          pfn_SurfaceGetHeight;
static PFN_ulSurfaceGetRowBytes        pfn_SurfaceGetRowBytes;
static PFN_ulSurfaceClearDirtyBounds   pfn_SurfaceClearDirtyBounds;
static PFN_ulVersionString             pfn_VersionString;
static PFN_ulEnablePlatformFontLoader  pfn_EnablePlatformFontLoader;
static PFN_ulEnablePlatformFileSystem  pfn_EnablePlatformFileSystem;
static PFN_ulEnableDefaultLogger       pfn_EnableDefaultLogger;
static PFN_ulViewLockJSContext         pfn_ViewLockJSContext;
static PFN_ulViewUnlockJSContext       pfn_ViewUnlockJSContext;
static PFN_JSContextGetGlobalObject            pfn_JSContextGetGlobalObject;
static PFN_JSStringCreateWithUTF8CString       pfn_JSStringCreateWithUTF8CString;
static PFN_JSStringRelease                     pfn_JSStringRelease;
static PFN_JSStringGetMaximumUTF8CStringSize   pfn_JSStringGetMaximumUTF8CStringSize;
static PFN_JSStringGetUTF8CString              pfn_JSStringGetUTF8CString;
static PFN_JSObjectMakeFunctionWithCallback    pfn_JSObjectMakeFunctionWithCallback;
static PFN_JSObjectSetProperty                 pfn_JSObjectSetProperty;
static PFN_JSValueIsString                     pfn_JSValueIsString;
static PFN_JSValueToStringCopy                 pfn_JSValueToStringCopy;

#define MAX_VIEWS 16
#define CONSOLE_MSG_MAX    64
#define CONSOLE_MSG_BUFLEN 2048
#define MSG_QUEUE_MAX      64
#define MSG_QUEUE_BUFLEN   2048
#define MOUSE_QUEUE_MAX    64
#define SCROLL_QUEUE_MAX   16
#define KEY_QUEUE_MAX      32
#define KEY_TEXT_LEN       32
#define JS_QUEUE_MAX       32
#define JS_QUEUE_BUFLEN    1024

typedef struct { int type, x, y, button; } MouseQueueEntry;
typedef struct { int type, dx, dy; } ScrollQueueEntry;
typedef struct { int type; int vk; unsigned int mods; char text[KEY_TEXT_LEN]; } KeyQueueEntry;

typedef struct {
    ULView    view;
    ULSurface surface;
    int       width;
    int       height;
    bool      used;
    char      console_msgs[CONSOLE_MSG_MAX][CONSOLE_MSG_BUFLEN];
    int       console_lens[CONSOLE_MSG_MAX];
    int       console_head;
    int       console_tail;
    int       console_count;
    MouseQueueEntry mouse_queue[MOUSE_QUEUE_MAX];
    int       mouse_count;
    ScrollQueueEntry scroll_queue[SCROLL_QUEUE_MAX];
    int       scroll_count;
    KeyQueueEntry key_queue[KEY_QUEUE_MAX];
    int       key_count;
    char      js_queue[JS_QUEUE_MAX][JS_QUEUE_BUFLEN];
    int       js_count;
    /* Native message queue (JS -> Go via __goSend, not console) */
    char      msg_queue[MSG_QUEUE_MAX][MSG_QUEUE_BUFLEN];
    int       msg_lens[MSG_QUEUE_MAX];
    int       msg_head;
    int       msg_tail;
    int       msg_count;
} ViewSlot;

static ULRenderer g_renderer = NULL;
static ViewSlot   g_views[MAX_VIEWS];
static int        g_view_count = 0;

static FILE* g_log = NULL;
static void blog(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

enum CmdType {
    CMD_NONE = 0,
    CMD_INIT,
    CMD_CREATE_VIEW,
    CMD_DESTROY_VIEW,
    CMD_LOAD_HTML,
    CMD_LOAD_URL,
    CMD_TICK,
    CMD_QUIT
};

static HANDLE g_worker_thread = NULL;
static HANDLE g_cmd_event = NULL;
static HANDLE g_done_event = NULL;
static volatile enum CmdType g_cmd_type = CMD_NONE;
static const char* g_cmd_str_arg = NULL;
static volatile int g_cmd_int1 = 0;  /* view_id or width */
static volatile int g_cmd_int2 = 0;  /* height */
static volatile int g_cmd_result = 0;
static char g_init_base_dir[MAX_PATH];
static volatile int g_debug = 0;

static void console_message_cb(void* user_data, ULView caller, ULMessageSource source, ULMessageLevel level,
                                ULString message, unsigned int line, unsigned int col, ULString source_id) {
    int vid = (int)(intptr_t)user_data;
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used || !pfn_StringGetData || !pfn_StringGetLength) return;
    ViewSlot* v = &g_views[vid];
    char* data = pfn_StringGetData(message);
    size_t len = pfn_StringGetLength(message);
    if (!data || len == 0) return;
    if (v->console_count >= CONSOLE_MSG_MAX) {
        v->console_tail = (v->console_tail + 1) % CONSOLE_MSG_MAX;
        v->console_count--;
    }
    size_t copy_len = len < (CONSOLE_MSG_BUFLEN - 1) ? len : (CONSOLE_MSG_BUFLEN - 1);
    memcpy(v->console_msgs[v->console_head], data, copy_len);
    v->console_msgs[v->console_head][copy_len] = '\0';
    v->console_lens[v->console_head] = (int)copy_len;
    v->console_head = (v->console_head + 1) % CONSOLE_MSG_MAX;
    v->console_count++;
}

#define RESOLVE(mod, var, name) *(FARPROC*)&(var) = GetProcAddress(mod, name); if (!(var)) { blog("FAIL: %s", name); return -100; }

static int resolve_functions(void) {
    HMODULE ul = GetModuleHandleA("Ultralight.dll");
    HMODULE ac = GetModuleHandleA("AppCore.dll");
    if (!ul) return -50;
    if (!ac) return -51;
    RESOLVE(ul, pfn_CreateString, "ulCreateString");
    RESOLVE(ul, pfn_DestroyString, "ulDestroyString");
    RESOLVE(ul, pfn_StringGetData, "ulStringGetData");
    RESOLVE(ul, pfn_StringGetLength, "ulStringGetLength");
    RESOLVE(ul, pfn_CreateConfig, "ulCreateConfig");
    RESOLVE(ul, pfn_DestroyConfig, "ulDestroyConfig");
    RESOLVE(ul, pfn_ConfigSetResourcePathPrefix, "ulConfigSetResourcePathPrefix");
    RESOLVE(ul, pfn_CreateRenderer, "ulCreateRenderer");
    RESOLVE(ul, pfn_DestroyRenderer, "ulDestroyRenderer");
    RESOLVE(ul, pfn_Update, "ulUpdate");
    RESOLVE(ul, pfn_RefreshDisplay, "ulRefreshDisplay");
    RESOLVE(ul, pfn_Render, "ulRender");
    RESOLVE(ul, pfn_CreateViewConfig, "ulCreateViewConfig");
    RESOLVE(ul, pfn_DestroyViewConfig, "ulDestroyViewConfig");
    RESOLVE(ul, pfn_VCSetIsAccelerated, "ulViewConfigSetIsAccelerated");
    RESOLVE(ul, pfn_VCSetIsTransparent, "ulViewConfigSetIsTransparent");
    RESOLVE(ul, pfn_VCSetInitialDeviceScale, "ulViewConfigSetInitialDeviceScale");
    RESOLVE(ul, pfn_CreateView, "ulCreateView");
    RESOLVE(ul, pfn_DestroyView, "ulDestroyView");
    RESOLVE(ul, pfn_ViewLoadHTML, "ulViewLoadHTML");
    RESOLVE(ul, pfn_ViewLoadURL, "ulViewLoadURL");
    RESOLVE(ul, pfn_ViewGetSurface, "ulViewGetSurface");
    RESOLVE(ul, pfn_ViewFocus, "ulViewFocus");
    RESOLVE(ul, pfn_ViewEvaluateScript, "ulViewEvaluateScript");
    RESOLVE(ul, pfn_ViewSetConsoleCallback, "ulViewSetAddConsoleMessageCallback");
    RESOLVE(ul, pfn_ViewFireMouseEvent, "ulViewFireMouseEvent");
    RESOLVE(ul, pfn_ViewFireScrollEvent, "ulViewFireScrollEvent");
    RESOLVE(ul, pfn_ViewFireKeyEvent, "ulViewFireKeyEvent");
    RESOLVE(ul, pfn_CreateKeyEvent, "ulCreateKeyEvent");
    RESOLVE(ul, pfn_DestroyKeyEvent, "ulDestroyKeyEvent");
    RESOLVE(ul, pfn_CreateMouseEvent, "ulCreateMouseEvent");
    RESOLVE(ul, pfn_DestroyMouseEvent, "ulDestroyMouseEvent");
    RESOLVE(ul, pfn_CreateScrollEvent, "ulCreateScrollEvent");
    RESOLVE(ul, pfn_DestroyScrollEvent, "ulDestroyScrollEvent");
    RESOLVE(ul, pfn_SurfaceLockPixels, "ulSurfaceLockPixels");
    RESOLVE(ul, pfn_SurfaceUnlockPixels, "ulSurfaceUnlockPixels");
    RESOLVE(ul, pfn_SurfaceGetWidth, "ulSurfaceGetWidth");
    RESOLVE(ul, pfn_SurfaceGetHeight, "ulSurfaceGetHeight");
    RESOLVE(ul, pfn_SurfaceGetRowBytes, "ulSurfaceGetRowBytes");
    RESOLVE(ul, pfn_SurfaceClearDirtyBounds, "ulSurfaceClearDirtyBounds");
    RESOLVE(ul, pfn_VersionString, "ulVersionString");
    RESOLVE(ac, pfn_EnablePlatformFontLoader, "ulEnablePlatformFontLoader");
    RESOLVE(ac, pfn_EnablePlatformFileSystem, "ulEnablePlatformFileSystem");
    RESOLVE(ac, pfn_EnableDefaultLogger, "ulEnableDefaultLogger");
    /* JS context (Ultralight.dll) */
    RESOLVE(ul, pfn_ViewLockJSContext, "ulViewLockJSContext");
    RESOLVE(ul, pfn_ViewUnlockJSContext, "ulViewUnlockJSContext");
    /* JavaScriptCore (WebCore.dll) */
    HMODULE wc = GetModuleHandleA("WebCore.dll");
    if (!wc) { blog("FAIL: WebCore.dll handle"); return -52; }
    RESOLVE(wc, pfn_JSContextGetGlobalObject, "JSContextGetGlobalObject");
    RESOLVE(wc, pfn_JSStringCreateWithUTF8CString, "JSStringCreateWithUTF8CString");
    RESOLVE(wc, pfn_JSStringRelease, "JSStringRelease");
    RESOLVE(wc, pfn_JSStringGetMaximumUTF8CStringSize, "JSStringGetMaximumUTF8CStringSize");
    RESOLVE(wc, pfn_JSStringGetUTF8CString, "JSStringGetUTF8CString");
    RESOLVE(wc, pfn_JSObjectMakeFunctionWithCallback, "JSObjectMakeFunctionWithCallback");
    RESOLVE(wc, pfn_JSObjectSetProperty, "JSObjectSetProperty");
    RESOLVE(wc, pfn_JSValueIsString, "JSValueIsString");
    RESOLVE(wc, pfn_JSValueToStringCopy, "JSValueToStringCopy");
    return 0;
}
#undef RESOLVE

/* JSC native callback: called when JS invokes window.__goSend(msg).
 * user_data encodes the view ID. Runs on the worker thread. */
static JSValueRef jsc_goSend_callback(JSContextRef ctx, JSObjectRef function,
    JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception) {
    /* Find view ID from the function's private data - we store vid in a static per-view */
    /* Since JSC doesn't give us nice user_data, we search which view owns this context */
    int vid = -1;
    for (int i = 0; i < MAX_VIEWS; i++) {
        if (g_views[i].used && g_views[i].view) {
            JSContextRef vc = pfn_ViewLockJSContext(g_views[i].view);
            pfn_ViewUnlockJSContext(g_views[i].view);
            if (vc == ctx) { vid = i; break; }
        }
    }
    if (vid < 0) { blog("jsc_goSend_callback: no matching view for ctx"); return NULL; }
    if (argumentCount < 1) { blog("jsc_goSend_callback: no args"); return NULL; }

    /* Convert first argument to UTF-8 string */
    JSStringRef jsStr = pfn_JSValueToStringCopy(ctx, arguments[0], NULL);
    if (!jsStr) { blog("jsc_goSend_callback: JSValueToStringCopy failed"); return NULL; }

    size_t maxLen = pfn_JSStringGetMaximumUTF8CStringSize(jsStr);
    ViewSlot* v = &g_views[vid];

    if (v->msg_count < MSG_QUEUE_MAX && maxLen < MSG_QUEUE_BUFLEN) {
        size_t written = pfn_JSStringGetUTF8CString(jsStr, v->msg_queue[v->msg_head], MSG_QUEUE_BUFLEN);
        if (written > 0) written--; /* JSStringGetUTF8CString includes null terminator in count */
        v->msg_lens[v->msg_head] = (int)written;
        v->msg_head = (v->msg_head + 1) % MSG_QUEUE_MAX;
        v->msg_count++;
        blog("jsc_goSend_callback: vid=%d msg='%s' len=%d", vid, v->msg_queue[(v->msg_head - 1 + MSG_QUEUE_MAX) % MSG_QUEUE_MAX], (int)written);
    } else {
        blog("jsc_goSend_callback: queue full or msg too large (count=%d maxLen=%zu)", v->msg_count, maxLen);
    }
    pfn_JSStringRelease(jsStr);
    return NULL;
}

/* Register window.__goSend and window.go.send as native JSC functions.
 * Must be called on the worker thread after the view's JS context is ready. */
static void setup_js_bindings(int vid) {
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used) return;
    ViewSlot* v = &g_views[vid];

    JSContextRef ctx = pfn_ViewLockJSContext(v->view);
    if (!ctx) return;

    JSObjectRef global = pfn_JSContextGetGlobalObject(ctx);

    /* Register window.__goSend */
    JSStringRef fnName = pfn_JSStringCreateWithUTF8CString("__goSend");
    JSObjectRef fnObj = pfn_JSObjectMakeFunctionWithCallback(ctx, fnName, jsc_goSend_callback);
    pfn_JSObjectSetProperty(ctx, global, fnName, fnObj, 0, NULL);
    pfn_JSStringRelease(fnName);

    pfn_ViewUnlockJSContext(v->view);

    /* Set up window.go namespace (preserve existing props like user-defined go.receive) */
    ULString goNs = pfn_CreateString("window.go=window.go||{};window.go.send=window.__goSend;");
    pfn_ViewEvaluateScript(v->view, goNs, NULL);
    pfn_DestroyString(goNs);
    blog("setup_js_bindings: vid=%d done", vid);
}

static int worker_do_init(void) {
    if (g_debug) {
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\ultralight.log", g_init_base_dir);
        ULString lp = pfn_CreateString(path);
        pfn_EnableDefaultLogger(lp);
        pfn_DestroyString(lp);
    }
    pfn_EnablePlatformFontLoader();
    ULString bd = pfn_CreateString(g_init_base_dir);
    pfn_EnablePlatformFileSystem(bd);
    pfn_DestroyString(bd);
    ULConfig config = pfn_CreateConfig();
    ULString rp = pfn_CreateString("/");
    pfn_ConfigSetResourcePathPrefix(config, rp);
    pfn_DestroyString(rp);
    g_renderer = pfn_CreateRenderer(config);
    pfn_DestroyConfig(config);
    if (!g_renderer) { blog("worker_do_init: renderer NULL"); return -10; }
    memset(g_views, 0, sizeof(g_views));
    g_view_count = 0;
    blog("worker_do_init: OK");
    return 0;
}

static int worker_do_create_view(int width, int height) {
    int vid;
    for (vid = 0; vid < MAX_VIEWS; vid++)
        if (!g_views[vid].used) break;
    if (vid >= MAX_VIEWS) { blog("worker_do_create_view: no slot"); return -1; }
    ViewSlot* v = &g_views[vid];
    ULViewConfig vc = pfn_CreateViewConfig();
    pfn_VCSetIsAccelerated(vc, false);
    pfn_VCSetIsTransparent(vc, true);
    pfn_VCSetInitialDeviceScale(vc, 1.0);
    v->view = pfn_CreateView(g_renderer, (unsigned int)width, (unsigned int)height, vc, NULL);
    pfn_DestroyViewConfig(vc);
    if (!v->view) { blog("worker_do_create_view: view NULL"); return -11; }
    v->surface = pfn_ViewGetSurface(v->view);
    v->width = width;
    v->height = height;
    v->used = true;
    v->console_head = v->console_tail = v->console_count = 0;
    v->msg_head = v->msg_tail = v->msg_count = 0;
    v->mouse_count = v->scroll_count = v->key_count = v->js_count = 0;
    pfn_ViewSetConsoleCallback(v->view, console_message_cb, (void*)(intptr_t)vid);
    pfn_ViewFocus(v->view);
    g_view_count++;
    for (int i = 0; i < 8; i++) {
        pfn_Update(g_renderer);
        Sleep(10);
    }
    pfn_Render(g_renderer);
    setup_js_bindings(vid);
    blog("worker_do_create_view: vid=%d", vid);
    return vid;
}

static void worker_do_destroy_view(int vid) {
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used) return;
    ViewSlot* v = &g_views[vid];
    if (v->view) { pfn_DestroyView(v->view); v->view = NULL; }
    v->surface = NULL;
    v->used = false;
    g_view_count--;
}

static void worker_do_load(int vid, const char* str, bool is_url) {
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used) return;
    ULView view = g_views[vid].view;
    ULString s = pfn_CreateString(str);
    if (is_url) pfn_ViewLoadURL(view, s);
    else        pfn_ViewLoadHTML(view, s);
    pfn_DestroyString(s);
    for (int i = 0; i < 20; i++) {
        pfn_Update(g_renderer);
        Sleep(10);
    }
    pfn_Render(g_renderer);
    /* Re-register JSC bindings (page load resets JS context) */
    setup_js_bindings(vid);
}

static void worker_do_tick(void) {
    for (int vid = 0; vid < MAX_VIEWS; vid++) {
        ViewSlot* v = &g_views[vid];
        if (!v->used || !v->view) continue;
        for (int i = 0; i < v->mouse_count; i++) {
            MouseQueueEntry* e = &v->mouse_queue[i];
            int x = e->x, y = e->y;
            if (x < 0) x = 0; if (y < 0) y = 0;
            if (v->width > 0 && x >= v->width) x = v->width - 1;
            if (v->height > 0 && y >= v->height) y = v->height - 1;
            ULMouseEvent evt = pfn_CreateMouseEvent(e->type, x, y, e->button);
            pfn_ViewFireMouseEvent(v->view, evt);
            pfn_DestroyMouseEvent(evt);
        }
        v->mouse_count = 0;
        for (int i = 0; i < v->scroll_count; i++) {
            ScrollQueueEntry* e = &v->scroll_queue[i];
            ULScrollEvent evt = pfn_CreateScrollEvent(e->type, e->dx, e->dy);
            pfn_ViewFireScrollEvent(v->view, evt);
            pfn_DestroyScrollEvent(evt);
        }
        v->scroll_count = 0;
        for (int i = 0; i < v->key_count; i++) {
            KeyQueueEntry* e = &v->key_queue[i];
            /* Map our type (0=RawKeyDown,1=KeyDown,2=KeyUp,3=Char) to SDK enum (0=KeyDown,1=KeyUp,2=RawKeyDown,3=Char) */
            unsigned int ul_type = (unsigned int)(e->type == 0 ? 2 : e->type == 1 ? 0 : e->type == 2 ? 1 : 3);
            ULString s_text = pfn_CreateString(e->text[0] ? e->text : "");
            ULString s_umod = pfn_CreateString(e->text[0] ? e->text : "");
            ULKeyEvent evt = pfn_CreateKeyEvent((ULKeyEventType)ul_type, e->mods, e->vk, e->vk, s_text, s_umod, false, false, false);
            pfn_ViewFireKeyEvent(v->view, evt);
            pfn_DestroyKeyEvent(evt);
            pfn_DestroyString(s_text);
            pfn_DestroyString(s_umod);
        }
        v->key_count = 0;
        for (int i = 0; i < v->js_count; i++) {
            ULString s = pfn_CreateString(v->js_queue[i]);
            pfn_ViewEvaluateScript(v->view, s, NULL);
            pfn_DestroyString(s);
        }
        v->js_count = 0;
    }
    pfn_Update(g_renderer);
    pfn_RefreshDisplay(g_renderer, 0);
    pfn_Render(g_renderer);
}

static void send_cmd(enum CmdType cmd, const char* str_arg, int i1, int i2) {
    g_cmd_str_arg = str_arg;
    g_cmd_int1 = i1;
    g_cmd_int2 = i2;
    g_cmd_type = cmd;
    SetEvent(g_cmd_event);
    WaitForSingleObject(g_done_event, INFINITE);
}

static DWORD WINAPI worker_thread_proc(LPVOID param) {
    blog("worker: started");
    while (1) {
        WaitForSingleObject(g_cmd_event, INFINITE);
        enum CmdType cmd = g_cmd_type;
        g_cmd_type = CMD_NONE;
        switch (cmd) {
        case CMD_INIT:
            g_cmd_result = worker_do_init();
            break;
        case CMD_CREATE_VIEW:
            g_cmd_result = worker_do_create_view(g_cmd_int1, g_cmd_int2);
            break;
        case CMD_DESTROY_VIEW:
            worker_do_destroy_view(g_cmd_int1);
            break;
        case CMD_LOAD_HTML:
            worker_do_load(g_cmd_int1, g_cmd_str_arg, false);
            break;
        case CMD_LOAD_URL:
            worker_do_load(g_cmd_int1, g_cmd_str_arg, true);
            break;
        case CMD_TICK:
            worker_do_tick();
            break;
        case CMD_QUIT:
            for (int i = 0; i < MAX_VIEWS; i++)
                worker_do_destroy_view(i);
            if (g_renderer) { pfn_DestroyRenderer(g_renderer); g_renderer = NULL; }
            SetEvent(g_done_event);
            return 0;
        default:
            break;
        }
        SetEvent(g_done_event);
    }
    return 0;
}

/* Exports for Go */
__declspec(dllexport) int ul_init(const char* base_dir, int debug) {
    g_debug = debug;
    if (g_debug) {
        char logname[MAX_PATH];
        snprintf(logname, MAX_PATH, "%s\\bridge.log", base_dir);
        g_log = fopen(logname, "w");
    }
    blog("ul_init: base_dir='%s' debug=%d", base_dir, debug);
    AddVectoredExceptionHandler(1, msvc_veh_handler);
    AddVectoredContinueHandler(1, msvc_vch_handler);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\UltralightCore.dll", base_dir);
    if (!LoadLibraryA(path)) { blog("FAIL: UltralightCore"); return -1; }
    snprintf(path, MAX_PATH, "%s\\WebCore.dll", base_dir);
    if (!LoadLibraryA(path)) { blog("FAIL: WebCore"); return -2; }
    snprintf(path, MAX_PATH, "%s\\Ultralight.dll", base_dir);
    if (!LoadLibraryA(path)) { blog("FAIL: Ultralight"); return -3; }
    snprintf(path, MAX_PATH, "%s\\AppCore.dll", base_dir);
    if (!LoadLibraryA(path)) { blog("FAIL: AppCore"); return -4; }
    int rc = resolve_functions();
    if (rc != 0) { blog("FAIL: resolve rc=%d", rc); return rc; }
    blog("ul_init: Ultralight %s", pfn_VersionString());
    strncpy(g_init_base_dir, base_dir, MAX_PATH - 1);
    g_init_base_dir[MAX_PATH - 1] = '\0';
    g_cmd_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_worker_thread = CreateThread(NULL, 0, worker_thread_proc, NULL, 0, NULL);
    if (!g_worker_thread) { blog("FAIL: CreateThread"); return -20; }
    send_cmd(CMD_INIT, NULL, 0, 0);
    if (g_cmd_result != 0) { blog("FAIL: worker init rc=%d", g_cmd_result); return g_cmd_result; }
    blog("ul_init: OK");
    return 0;
}

__declspec(dllexport) int ul_create_view(int width, int height) {
    if (!g_worker_thread) return -1;
    send_cmd(CMD_CREATE_VIEW, NULL, width, height);
    return g_cmd_result;
}

__declspec(dllexport) void ul_destroy_view(int view_id) {
    if (!g_worker_thread || view_id < 0 || view_id >= MAX_VIEWS) return;
    send_cmd(CMD_DESTROY_VIEW, NULL, view_id, 0);
}

__declspec(dllexport) void ul_view_load_html(int view_id, const char* html) {
    if (!html || !g_worker_thread || view_id < 0 || view_id >= MAX_VIEWS) return;
    send_cmd(CMD_LOAD_HTML, html, view_id, 0);
}

__declspec(dllexport) void ul_view_load_url(int view_id, const char* url) {
    if (!url || !g_worker_thread || view_id < 0 || view_id >= MAX_VIEWS) return;
    send_cmd(CMD_LOAD_URL, url, view_id, 0);
}

__declspec(dllexport) void ul_tick(void) {
    if (!g_worker_thread) return;
    send_cmd(CMD_TICK, NULL, 0, 0);
}

__declspec(dllexport) void* ul_view_get_pixels(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !g_views[view_id].surface) return NULL;
    return pfn_SurfaceLockPixels(g_views[view_id].surface);
}

__declspec(dllexport) void ul_view_unlock_pixels(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    if (pfn_SurfaceUnlockPixels) pfn_SurfaceUnlockPixels(g_views[view_id].surface);
    if (pfn_SurfaceClearDirtyBounds) pfn_SurfaceClearDirtyBounds(g_views[view_id].surface);
}

__declspec(dllexport) unsigned int ul_view_get_width(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return 0;
    return (unsigned int)g_views[view_id].width;
}

__declspec(dllexport) unsigned int ul_view_get_height(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return 0;
    return (unsigned int)g_views[view_id].height;
}

__declspec(dllexport) unsigned int ul_view_get_row_bytes(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !g_views[view_id].surface) return 0;
    return pfn_SurfaceGetRowBytes(g_views[view_id].surface);
}

__declspec(dllexport) void ul_view_fire_mouse(int view_id, int type, int x, int y, int button) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    ViewSlot* v = &g_views[view_id];
    if (v->mouse_count >= MOUSE_QUEUE_MAX) return;
    MouseQueueEntry* e = &v->mouse_queue[v->mouse_count++];
    e->type = type; e->x = x; e->y = y; e->button = button;
}

__declspec(dllexport) void ul_view_fire_scroll(int view_id, int type, int dx, int dy) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    ViewSlot* v = &g_views[view_id];
    if (v->scroll_count >= SCROLL_QUEUE_MAX) return;
    ScrollQueueEntry* e = &v->scroll_queue[v->scroll_count++];
    e->type = type; e->dx = dx; e->dy = dy;
}

__declspec(dllexport) void ul_view_fire_key(int view_id, int type, int vk, unsigned int mods, const char* text) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    ViewSlot* v = &g_views[view_id];
    if (v->key_count >= KEY_QUEUE_MAX) return;
    KeyQueueEntry* e = &v->key_queue[v->key_count++];
    e->type = type; e->vk = vk; e->mods = mods;
    if (text) {
        size_t n = strlen(text);
        if (n >= KEY_TEXT_LEN) n = KEY_TEXT_LEN - 1;
        memcpy(e->text, text, n + 1);
    } else
        e->text[0] = '\0';
}

__declspec(dllexport) void ul_view_eval_js(int view_id, const char* js) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !js) return;
    ViewSlot* v = &g_views[view_id];
    if (v->js_count >= JS_QUEUE_MAX) return;
    size_t len = strlen(js);
    if (len >= JS_QUEUE_BUFLEN) return;
    memcpy(v->js_queue[v->js_count], js, len + 1);
    v->js_count++;
}

__declspec(dllexport) int ul_view_get_message(int view_id, char* buf, int buf_size) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !buf || buf_size <= 0) return 0;
    ViewSlot* v = &g_views[view_id];
    if (v->msg_count <= 0) return 0;
    int len = v->msg_lens[v->msg_tail];
    int cl = len < (buf_size - 1) ? len : (buf_size - 1);
    memcpy(buf, v->msg_queue[v->msg_tail], cl);
    buf[cl] = '\0';
    v->msg_tail = (v->msg_tail + 1) % MSG_QUEUE_MAX;
    v->msg_count--;
    return cl;
}

__declspec(dllexport) int ul_view_get_console_message(int view_id, char* buf, int buf_size) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !buf || buf_size <= 0) return 0;
    ViewSlot* v = &g_views[view_id];
    if (v->console_count <= 0) return 0;
    int len = v->console_lens[v->console_tail];
    int cl = len < (buf_size - 1) ? len : (buf_size - 1);
    memcpy(buf, v->console_msgs[v->console_tail], cl);
    buf[cl] = '\0';
    v->console_tail = (v->console_tail + 1) % CONSOLE_MSG_MAX;
    v->console_count--;
    return cl;
}

__declspec(dllexport) void ul_destroy(void) {
    if (g_worker_thread) {
        send_cmd(CMD_QUIT, NULL, 0, 0);
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
        g_worker_thread = NULL;
    }
    if (g_cmd_event) { CloseHandle(g_cmd_event); g_cmd_event = NULL; }
    if (g_done_event) { CloseHandle(g_done_event); g_done_event = NULL; }
    if (g_log) { fclose(g_log); g_log = NULL; }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
