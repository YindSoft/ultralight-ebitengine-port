/*
 * ul_bridge.c - C bridge for Ultralight 1.4 (multi-view, keyboard)
 * Copyright (c) 2026 Javier Podavini (YindSoft)
 * Licensed under the MIT License. See LICENSE file in the project root.
 *
 * Architecture: dedicated worker thread for ALL Ultralight API calls.
 * Go (purego) -> ul_bridge exports -> command to worker thread -> Ultralight API.
 *
 * Build:
 *   Windows: gcc -shared -o ul_bridge.dll bridge/ul_bridge.c -O2 -lkernel32
 *   Linux:   gcc -shared -fPIC -o libul_bridge.so bridge/ul_bridge.c -O2 -lpthread -ldl
 *   macOS:   gcc -shared -fPIC -o libul_bridge.dylib bridge/ul_bridge.c -O2 -lpthread -ldl
 */

/* ── Abstracciones de plataforma ─────────────────────────────────────── */
#ifdef _WIN32
  #include <windows.h>
  #define EXPORT __declspec(dllexport)
  #define PATH_SEP '\\'
  #define PATHBUF_SIZE MAX_PATH
#else
  #include <pthread.h>
  #include <dlfcn.h>
  #include <unistd.h>
  #include <limits.h>
  #define EXPORT __attribute__((visibility("default")))
  #define PATH_SEP '/'
  #define PATHBUF_SIZE PATH_MAX
#endif

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/* ── VEH/VCH exception handlers (solo Windows, 0x406D1388 = MSVC SetThreadName) ── */
#ifdef _WIN32
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
#endif

/* ── Ultralight type aliases ─────────────────────────────────────────── */
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

/* ── Function pointers ───────────────────────────────────────────────── */
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

/* JavaScriptCore C API (exported from WebCore) */
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

/* ── Constantes de colas y vistas ────────────────────────────────────── */
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
    /* Cola de mensajes nativos (JS -> Go via __goSend, no console) */
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

/* ── Handles de libreria (para resolve_functions) ────────────────────── */
#ifdef _WIN32
  static HMODULE g_hUltralight = NULL;
  static HMODULE g_hAppCore = NULL;
  static HMODULE g_hWebCore = NULL;
  static HMODULE g_hUltralightCore = NULL;
#else
  static void* g_hUltralight = NULL;
  static void* g_hAppCore = NULL;
  static void* g_hWebCore = NULL;
  static void* g_hUltralightCore = NULL;
#endif

/* ── Comandos del worker thread ──────────────────────────────────────── */
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

/* ── Sincronizacion del worker thread ────────────────────────────────── */
#ifdef _WIN32
  static HANDLE g_worker_thread = NULL;
  static HANDLE g_cmd_event = NULL;
  static HANDLE g_done_event = NULL;
#else
  static pthread_t g_worker_thread;
  static int g_worker_started = 0;
  static pthread_mutex_t g_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;
  static pthread_cond_t g_cmd_cond = PTHREAD_COND_INITIALIZER;
  static pthread_cond_t g_done_cond = PTHREAD_COND_INITIALIZER;
  static int g_cmd_ready = 0;
  static int g_cmd_done = 0;
#endif

static volatile enum CmdType g_cmd_type = CMD_NONE;
static const char* g_cmd_str_arg = NULL;
static volatile int g_cmd_int1 = 0;  /* view_id or width */
static volatile int g_cmd_int2 = 0;  /* height */
static volatile int g_cmd_result = 0;
static char g_init_base_dir[PATHBUF_SIZE];
static volatile int g_debug = 0;

/* ── Sleep multiplataforma ───────────────────────────────────────────── */
static void msleep(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* ── Console message callback ────────────────────────────────────────── */
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

/* ── Resolucion de simbolos ──────────────────────────────────────────── */
#ifdef _WIN32
  #define GETSYM(handle, name) (void*)GetProcAddress((HMODULE)(handle), (name))
#else
  #define GETSYM(handle, name) dlsym((handle), (name))
#endif

#define RESOLVE(handle, var, name) *(void**)&(var) = GETSYM(handle, name); if (!(var)) { blog("FAIL: %s", name); return -100; }

static int resolve_functions(void) {
    if (!g_hUltralight) return -50;
    if (!g_hAppCore) return -51;
    RESOLVE(g_hUltralight, pfn_CreateString, "ulCreateString");
    RESOLVE(g_hUltralight, pfn_DestroyString, "ulDestroyString");
    RESOLVE(g_hUltralight, pfn_StringGetData, "ulStringGetData");
    RESOLVE(g_hUltralight, pfn_StringGetLength, "ulStringGetLength");
    RESOLVE(g_hUltralight, pfn_CreateConfig, "ulCreateConfig");
    RESOLVE(g_hUltralight, pfn_DestroyConfig, "ulDestroyConfig");
    RESOLVE(g_hUltralight, pfn_ConfigSetResourcePathPrefix, "ulConfigSetResourcePathPrefix");
    RESOLVE(g_hUltralight, pfn_CreateRenderer, "ulCreateRenderer");
    RESOLVE(g_hUltralight, pfn_DestroyRenderer, "ulDestroyRenderer");
    RESOLVE(g_hUltralight, pfn_Update, "ulUpdate");
    RESOLVE(g_hUltralight, pfn_RefreshDisplay, "ulRefreshDisplay");
    RESOLVE(g_hUltralight, pfn_Render, "ulRender");
    RESOLVE(g_hUltralight, pfn_CreateViewConfig, "ulCreateViewConfig");
    RESOLVE(g_hUltralight, pfn_DestroyViewConfig, "ulDestroyViewConfig");
    RESOLVE(g_hUltralight, pfn_VCSetIsAccelerated, "ulViewConfigSetIsAccelerated");
    RESOLVE(g_hUltralight, pfn_VCSetIsTransparent, "ulViewConfigSetIsTransparent");
    RESOLVE(g_hUltralight, pfn_VCSetInitialDeviceScale, "ulViewConfigSetInitialDeviceScale");
    RESOLVE(g_hUltralight, pfn_CreateView, "ulCreateView");
    RESOLVE(g_hUltralight, pfn_DestroyView, "ulDestroyView");
    RESOLVE(g_hUltralight, pfn_ViewLoadHTML, "ulViewLoadHTML");
    RESOLVE(g_hUltralight, pfn_ViewLoadURL, "ulViewLoadURL");
    RESOLVE(g_hUltralight, pfn_ViewGetSurface, "ulViewGetSurface");
    RESOLVE(g_hUltralight, pfn_ViewFocus, "ulViewFocus");
    RESOLVE(g_hUltralight, pfn_ViewEvaluateScript, "ulViewEvaluateScript");
    RESOLVE(g_hUltralight, pfn_ViewSetConsoleCallback, "ulViewSetAddConsoleMessageCallback");
    RESOLVE(g_hUltralight, pfn_ViewFireMouseEvent, "ulViewFireMouseEvent");
    RESOLVE(g_hUltralight, pfn_ViewFireScrollEvent, "ulViewFireScrollEvent");
    RESOLVE(g_hUltralight, pfn_ViewFireKeyEvent, "ulViewFireKeyEvent");
    RESOLVE(g_hUltralight, pfn_CreateKeyEvent, "ulCreateKeyEvent");
    RESOLVE(g_hUltralight, pfn_DestroyKeyEvent, "ulDestroyKeyEvent");
    RESOLVE(g_hUltralight, pfn_CreateMouseEvent, "ulCreateMouseEvent");
    RESOLVE(g_hUltralight, pfn_DestroyMouseEvent, "ulDestroyMouseEvent");
    RESOLVE(g_hUltralight, pfn_CreateScrollEvent, "ulCreateScrollEvent");
    RESOLVE(g_hUltralight, pfn_DestroyScrollEvent, "ulDestroyScrollEvent");
    RESOLVE(g_hUltralight, pfn_SurfaceLockPixels, "ulSurfaceLockPixels");
    RESOLVE(g_hUltralight, pfn_SurfaceUnlockPixels, "ulSurfaceUnlockPixels");
    RESOLVE(g_hUltralight, pfn_SurfaceGetWidth, "ulSurfaceGetWidth");
    RESOLVE(g_hUltralight, pfn_SurfaceGetHeight, "ulSurfaceGetHeight");
    RESOLVE(g_hUltralight, pfn_SurfaceGetRowBytes, "ulSurfaceGetRowBytes");
    RESOLVE(g_hUltralight, pfn_SurfaceClearDirtyBounds, "ulSurfaceClearDirtyBounds");
    RESOLVE(g_hUltralight, pfn_VersionString, "ulVersionString");
    RESOLVE(g_hAppCore, pfn_EnablePlatformFontLoader, "ulEnablePlatformFontLoader");
    RESOLVE(g_hAppCore, pfn_EnablePlatformFileSystem, "ulEnablePlatformFileSystem");
    RESOLVE(g_hAppCore, pfn_EnableDefaultLogger, "ulEnableDefaultLogger");
    /* JS context (Ultralight) */
    RESOLVE(g_hUltralight, pfn_ViewLockJSContext, "ulViewLockJSContext");
    RESOLVE(g_hUltralight, pfn_ViewUnlockJSContext, "ulViewUnlockJSContext");
    /* JavaScriptCore (WebCore) */
    if (!g_hWebCore) { blog("FAIL: WebCore handle"); return -52; }
    RESOLVE(g_hWebCore, pfn_JSContextGetGlobalObject, "JSContextGetGlobalObject");
    RESOLVE(g_hWebCore, pfn_JSStringCreateWithUTF8CString, "JSStringCreateWithUTF8CString");
    RESOLVE(g_hWebCore, pfn_JSStringRelease, "JSStringRelease");
    RESOLVE(g_hWebCore, pfn_JSStringGetMaximumUTF8CStringSize, "JSStringGetMaximumUTF8CStringSize");
    RESOLVE(g_hWebCore, pfn_JSStringGetUTF8CString, "JSStringGetUTF8CString");
    RESOLVE(g_hWebCore, pfn_JSObjectMakeFunctionWithCallback, "JSObjectMakeFunctionWithCallback");
    RESOLVE(g_hWebCore, pfn_JSObjectSetProperty, "JSObjectSetProperty");
    RESOLVE(g_hWebCore, pfn_JSValueIsString, "JSValueIsString");
    RESOLVE(g_hWebCore, pfn_JSValueToStringCopy, "JSValueToStringCopy");
    return 0;
}
#undef RESOLVE
#undef GETSYM

/* JSC native callback: se invoca cuando JS llama window.__goSend(msg).
 * user_data codifica el view ID. Corre en el worker thread. */
static JSValueRef jsc_goSend_callback(JSContextRef ctx, JSObjectRef function,
    JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception) {
    /* Buscar view ID por contexto JSC */
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

    /* Convertir primer argumento a UTF-8 */
    JSStringRef jsStr = pfn_JSValueToStringCopy(ctx, arguments[0], NULL);
    if (!jsStr) { blog("jsc_goSend_callback: JSValueToStringCopy failed"); return NULL; }

    size_t maxLen = pfn_JSStringGetMaximumUTF8CStringSize(jsStr);
    ViewSlot* v = &g_views[vid];

    if (v->msg_count < MSG_QUEUE_MAX && maxLen < MSG_QUEUE_BUFLEN) {
        size_t written = pfn_JSStringGetUTF8CString(jsStr, v->msg_queue[v->msg_head], MSG_QUEUE_BUFLEN);
        if (written > 0) written--; /* JSStringGetUTF8CString incluye null en el conteo */
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

/* Registrar window.__goSend y window.go.send como funciones nativas JSC.
 * Debe llamarse en el worker thread cuando el contexto JS esta listo. */
static void setup_js_bindings(int vid) {
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used) return;
    ViewSlot* v = &g_views[vid];

    JSContextRef ctx = pfn_ViewLockJSContext(v->view);
    if (!ctx) return;

    JSObjectRef global = pfn_JSContextGetGlobalObject(ctx);

    /* Registrar window.__goSend */
    JSStringRef fnName = pfn_JSStringCreateWithUTF8CString("__goSend");
    JSObjectRef fnObj = pfn_JSObjectMakeFunctionWithCallback(ctx, fnName, jsc_goSend_callback);
    pfn_JSObjectSetProperty(ctx, global, fnName, fnObj, 0, NULL);
    pfn_JSStringRelease(fnName);

    pfn_ViewUnlockJSContext(v->view);

    /* Configurar namespace window.go (preservar props existentes) */
    ULString goNs = pfn_CreateString("window.go=window.go||{};window.go.send=window.__goSend;");
    pfn_ViewEvaluateScript(v->view, goNs, NULL);
    pfn_DestroyString(goNs);
    blog("setup_js_bindings: vid=%d done", vid);
}

static int worker_do_init(void) {
    if (g_debug) {
        char path[PATHBUF_SIZE];
        snprintf(path, PATHBUF_SIZE, "%s%cultralight.log", g_init_base_dir, PATH_SEP);
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
        msleep(10);
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
        msleep(10);
    }
    pfn_Render(g_renderer);
    /* Re-registrar bindings JSC (la carga de pagina resetea el contexto JS) */
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
            /* Mapear tipo (0=RawKeyDown,1=KeyDown,2=KeyUp,3=Char) a enum SDK (0=KeyDown,1=KeyUp,2=RawKeyDown,3=Char) */
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

/* ── send_cmd / worker_thread_proc (plataforma-especifico) ───────────── */
#ifdef _WIN32

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

#else /* POSIX (Linux, macOS) */

static void send_cmd(enum CmdType cmd, const char* str_arg, int i1, int i2) {
    pthread_mutex_lock(&g_cmd_mutex);
    g_cmd_str_arg = str_arg;
    g_cmd_int1 = i1;
    g_cmd_int2 = i2;
    g_cmd_type = cmd;
    g_cmd_ready = 1;
    pthread_cond_signal(&g_cmd_cond);
    while (!g_cmd_done)
        pthread_cond_wait(&g_done_cond, &g_cmd_mutex);
    g_cmd_done = 0;
    pthread_mutex_unlock(&g_cmd_mutex);
}

static void* worker_thread_proc(void* param) {
    blog("worker: started");
    while (1) {
        pthread_mutex_lock(&g_cmd_mutex);
        while (!g_cmd_ready)
            pthread_cond_wait(&g_cmd_cond, &g_cmd_mutex);
        g_cmd_ready = 0;
        enum CmdType cmd = g_cmd_type;
        g_cmd_type = CMD_NONE;
        pthread_mutex_unlock(&g_cmd_mutex);

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
            pthread_mutex_lock(&g_cmd_mutex);
            g_cmd_done = 1;
            pthread_cond_signal(&g_done_cond);
            pthread_mutex_unlock(&g_cmd_mutex);
            return NULL;
        default:
            break;
        }

        pthread_mutex_lock(&g_cmd_mutex);
        g_cmd_done = 1;
        pthread_cond_signal(&g_done_cond);
        pthread_mutex_unlock(&g_cmd_mutex);
    }
    return NULL;
}

#endif /* _WIN32 / POSIX */

/* ── Carga de librerias SDK ──────────────────────────────────────────── */
#ifdef _WIN32

static int load_sdk_libs(const char* base_dir) {
    char path[PATHBUF_SIZE];
    snprintf(path, PATHBUF_SIZE, "%s\\UltralightCore.dll", base_dir);
    g_hUltralightCore = LoadLibraryA(path);
    if (!g_hUltralightCore) { blog("FAIL: UltralightCore"); return -1; }
    snprintf(path, PATHBUF_SIZE, "%s\\WebCore.dll", base_dir);
    g_hWebCore = LoadLibraryA(path);
    if (!g_hWebCore) { blog("FAIL: WebCore"); return -2; }
    snprintf(path, PATHBUF_SIZE, "%s\\Ultralight.dll", base_dir);
    g_hUltralight = LoadLibraryA(path);
    if (!g_hUltralight) { blog("FAIL: Ultralight"); return -3; }
    snprintf(path, PATHBUF_SIZE, "%s\\AppCore.dll", base_dir);
    g_hAppCore = LoadLibraryA(path);
    if (!g_hAppCore) { blog("FAIL: AppCore"); return -4; }
    return 0;
}

#else /* POSIX */

#ifdef __APPLE__
  #define LIB_PREFIX "lib"
  #define LIB_EXT ".dylib"
#else
  #define LIB_PREFIX "lib"
  #define LIB_EXT ".so"
#endif

static int load_sdk_libs(const char* base_dir) {
    char path[PATHBUF_SIZE];
    snprintf(path, PATHBUF_SIZE, "%s/%sUltralightCore%s", base_dir, LIB_PREFIX, LIB_EXT);
    g_hUltralightCore = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_hUltralightCore) { blog("FAIL: UltralightCore: %s", dlerror()); return -1; }
    snprintf(path, PATHBUF_SIZE, "%s/%sWebCore%s", base_dir, LIB_PREFIX, LIB_EXT);
    g_hWebCore = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_hWebCore) { blog("FAIL: WebCore: %s", dlerror()); return -2; }
    snprintf(path, PATHBUF_SIZE, "%s/%sUltralight%s", base_dir, LIB_PREFIX, LIB_EXT);
    g_hUltralight = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_hUltralight) { blog("FAIL: Ultralight: %s", dlerror()); return -3; }
    snprintf(path, PATHBUF_SIZE, "%s/%sAppCore%s", base_dir, LIB_PREFIX, LIB_EXT);
    g_hAppCore = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_hAppCore) { blog("FAIL: AppCore: %s", dlerror()); return -4; }
    return 0;
}

#endif /* _WIN32 / POSIX */

/* ── Funciones exportadas para Go ────────────────────────────────────── */
EXPORT int ul_init(const char* base_dir, int debug) {
    g_debug = debug;
    if (g_debug) {
        char logname[PATHBUF_SIZE];
        snprintf(logname, PATHBUF_SIZE, "%s%cbridge.log", base_dir, PATH_SEP);
        g_log = fopen(logname, "w");
    }
    blog("ul_init: base_dir='%s' debug=%d", base_dir, debug);

#ifdef _WIN32
    AddVectoredExceptionHandler(1, msvc_veh_handler);
    AddVectoredContinueHandler(1, msvc_vch_handler);
#endif

    int rc = load_sdk_libs(base_dir);
    if (rc != 0) return rc;
    rc = resolve_functions();
    if (rc != 0) { blog("FAIL: resolve rc=%d", rc); return rc; }
    blog("ul_init: Ultralight %s", pfn_VersionString());
    strncpy(g_init_base_dir, base_dir, PATHBUF_SIZE - 1);
    g_init_base_dir[PATHBUF_SIZE - 1] = '\0';

#ifdef _WIN32
    g_cmd_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_worker_thread = CreateThread(NULL, 0, worker_thread_proc, NULL, 0, NULL);
    if (!g_worker_thread) { blog("FAIL: CreateThread"); return -20; }
#else
    int pt_rc = pthread_create(&g_worker_thread, NULL, worker_thread_proc, NULL);
    if (pt_rc != 0) { blog("FAIL: pthread_create rc=%d", pt_rc); return -20; }
    g_worker_started = 1;
#endif

    send_cmd(CMD_INIT, NULL, 0, 0);
    if (g_cmd_result != 0) { blog("FAIL: worker init rc=%d", g_cmd_result); return g_cmd_result; }
    blog("ul_init: OK");
    return 0;
}

EXPORT int ul_create_view(int width, int height) {
#ifdef _WIN32
    if (!g_worker_thread) return -1;
#else
    if (!g_worker_started) return -1;
#endif
    send_cmd(CMD_CREATE_VIEW, NULL, width, height);
    return g_cmd_result;
}

EXPORT void ul_destroy_view(int view_id) {
#ifdef _WIN32
    if (!g_worker_thread || view_id < 0 || view_id >= MAX_VIEWS) return;
#else
    if (!g_worker_started || view_id < 0 || view_id >= MAX_VIEWS) return;
#endif
    send_cmd(CMD_DESTROY_VIEW, NULL, view_id, 0);
}

EXPORT void ul_view_load_html(int view_id, const char* html) {
#ifdef _WIN32
    if (!html || !g_worker_thread || view_id < 0 || view_id >= MAX_VIEWS) return;
#else
    if (!html || !g_worker_started || view_id < 0 || view_id >= MAX_VIEWS) return;
#endif
    send_cmd(CMD_LOAD_HTML, html, view_id, 0);
}

EXPORT void ul_view_load_url(int view_id, const char* url) {
#ifdef _WIN32
    if (!url || !g_worker_thread || view_id < 0 || view_id >= MAX_VIEWS) return;
#else
    if (!url || !g_worker_started || view_id < 0 || view_id >= MAX_VIEWS) return;
#endif
    send_cmd(CMD_LOAD_URL, url, view_id, 0);
}

EXPORT void ul_tick(void) {
#ifdef _WIN32
    if (!g_worker_thread) return;
#else
    if (!g_worker_started) return;
#endif
    send_cmd(CMD_TICK, NULL, 0, 0);
}

EXPORT void* ul_view_get_pixels(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !g_views[view_id].surface) return NULL;
    return pfn_SurfaceLockPixels(g_views[view_id].surface);
}

EXPORT void ul_view_unlock_pixels(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    if (pfn_SurfaceUnlockPixels) pfn_SurfaceUnlockPixels(g_views[view_id].surface);
    if (pfn_SurfaceClearDirtyBounds) pfn_SurfaceClearDirtyBounds(g_views[view_id].surface);
}

EXPORT unsigned int ul_view_get_width(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return 0;
    return (unsigned int)g_views[view_id].width;
}

EXPORT unsigned int ul_view_get_height(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return 0;
    return (unsigned int)g_views[view_id].height;
}

EXPORT unsigned int ul_view_get_row_bytes(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !g_views[view_id].surface) return 0;
    return pfn_SurfaceGetRowBytes(g_views[view_id].surface);
}

EXPORT void ul_view_fire_mouse(int view_id, int type, int x, int y, int button) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    ViewSlot* v = &g_views[view_id];
    if (v->mouse_count >= MOUSE_QUEUE_MAX) return;
    MouseQueueEntry* e = &v->mouse_queue[v->mouse_count++];
    e->type = type; e->x = x; e->y = y; e->button = button;
}

EXPORT void ul_view_fire_scroll(int view_id, int type, int dx, int dy) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return;
    ViewSlot* v = &g_views[view_id];
    if (v->scroll_count >= SCROLL_QUEUE_MAX) return;
    ScrollQueueEntry* e = &v->scroll_queue[v->scroll_count++];
    e->type = type; e->dx = dx; e->dy = dy;
}

EXPORT void ul_view_fire_key(int view_id, int type, int vk, unsigned int mods, const char* text) {
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

EXPORT void ul_view_eval_js(int view_id, const char* js) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !js) return;
    ViewSlot* v = &g_views[view_id];
    if (v->js_count >= JS_QUEUE_MAX) return;
    size_t len = strlen(js);
    if (len >= JS_QUEUE_BUFLEN) return;
    memcpy(v->js_queue[v->js_count], js, len + 1);
    v->js_count++;
}

EXPORT int ul_view_get_message(int view_id, char* buf, int buf_size) {
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

EXPORT int ul_view_get_console_message(int view_id, char* buf, int buf_size) {
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

EXPORT void ul_destroy(void) {
#ifdef _WIN32
    if (g_worker_thread) {
        send_cmd(CMD_QUIT, NULL, 0, 0);
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
        g_worker_thread = NULL;
    }
    if (g_cmd_event) { CloseHandle(g_cmd_event); g_cmd_event = NULL; }
    if (g_done_event) { CloseHandle(g_done_event); g_done_event = NULL; }
#else
    if (g_worker_started) {
        send_cmd(CMD_QUIT, NULL, 0, 0);
        pthread_join(g_worker_thread, NULL);
        g_worker_started = 0;
    }
    pthread_mutex_destroy(&g_cmd_mutex);
    pthread_cond_destroy(&g_cmd_cond);
    pthread_cond_destroy(&g_done_cond);
#endif
    if (g_log) { fclose(g_log); g_log = NULL; }
}

/* ── DllMain (solo Windows) ──────────────────────────────────────────── */
#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
#endif
