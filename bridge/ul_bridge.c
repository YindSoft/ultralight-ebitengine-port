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

/* ── Platform abstractions ─────────────────────────────────────── */
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

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/* ── VEH/VCH exception handlers (Windows only, 0x406D1388 = MSVC SetThreadName) ── */
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
typedef void         (*PFN_ulConfigSetCachePath)(ULConfig, ULString);
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
typedef struct { int left, top, right, bottom; } ULIntRect;
typedef ULIntRect     (*PFN_ulSurfaceGetDirtyBounds)(ULSurface);
typedef const char*   (*PFN_ulVersionString)(void);
typedef void (*PFN_ulEnablePlatformFontLoader)(void);
typedef void (*PFN_ulEnablePlatformFileSystem)(ULString);
typedef void (*PFN_ulEnableDefaultLogger)(ULString);

/* ULBuffer (Ultralight 1.4 opaque buffer) */
typedef void* ULBuffer;
typedef void (*ulDestroyBufferCallback)(void* user_data, void* data);
typedef ULBuffer (*PFN_ulCreateBuffer)(void* data, size_t size, void* user_data, ulDestroyBufferCallback cb);
typedef ULBuffer (*PFN_ulCreateBufferFromCopy)(const void* data, size_t size);
typedef void     (*PFN_ulDestroyBuffer)(ULBuffer);

/* ULFileSystem: custom file system callbacks (Ultralight 1.4 API) */
typedef bool      (*PFN_FS_FileExists)(ULString);
typedef ULString  (*PFN_FS_GetFileMimeType)(ULString);
typedef ULString  (*PFN_FS_GetFileCharset)(ULString);
typedef ULBuffer  (*PFN_FS_OpenFile)(ULString);  /* returns ULBuffer with data, NULL on failure */
typedef struct {
    PFN_FS_FileExists       file_exists;
    PFN_FS_GetFileMimeType  get_file_mime_type;
    PFN_FS_GetFileCharset   get_file_charset;
    PFN_FS_OpenFile         open_file;
} ULFileSystem;
typedef void (*PFN_ulPlatformSetFileSystem)(ULFileSystem);

/* ULClipboard: clipboard callbacks (Ultralight 1.4 API) */
typedef void (*PFN_CB_Clear)(void);
typedef void (*PFN_CB_ReadPlainText)(ULString result);
typedef void (*PFN_CB_WritePlainText)(ULString text);
typedef struct {
    PFN_CB_Clear          clear;
    PFN_CB_ReadPlainText  read_plain_text;
    PFN_CB_WritePlainText write_plain_text;
} ULClipboard;
typedef void (*PFN_ulPlatformSetClipboard)(ULClipboard);
typedef void (*PFN_ulStringAssignCString)(ULString, const char*);

/* Ultralight JS context access */
typedef void* JSContextRef;
typedef void* JSObjectRef;
typedef void* JSValueRef;
typedef void* JSStringRef;
typedef JSContextRef (*PFN_ulViewLockJSContext)(ULView);
typedef void         (*PFN_ulViewUnlockJSContext)(ULView);

/* JavaScriptCore C API (exported from WebCore) */
typedef JSContextRef (*PFN_JSContextGetGlobalContext)(JSContextRef);
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
static PFN_ulConfigSetCachePath          pfn_ConfigSetCachePath;
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
static PFN_ulSurfaceGetDirtyBounds    pfn_SurfaceGetDirtyBounds;
static PFN_ulVersionString             pfn_VersionString;
static PFN_ulEnablePlatformFontLoader  pfn_EnablePlatformFontLoader;
static PFN_ulEnablePlatformFileSystem  pfn_EnablePlatformFileSystem;
static PFN_ulEnableDefaultLogger       pfn_EnableDefaultLogger;
static PFN_ulPlatformSetFileSystem     pfn_PlatformSetFileSystem;
static PFN_ulCreateBuffer              pfn_CreateBuffer;
static PFN_ulCreateBufferFromCopy      pfn_CreateBufferFromCopy;
static PFN_ulPlatformSetClipboard      pfn_PlatformSetClipboard;
static PFN_ulStringAssignCString       pfn_StringAssignCString;
static PFN_ulViewLockJSContext         pfn_ViewLockJSContext;
static PFN_ulViewUnlockJSContext       pfn_ViewUnlockJSContext;
static PFN_JSContextGetGlobalContext            pfn_JSContextGetGlobalContext;
static PFN_JSContextGetGlobalObject            pfn_JSContextGetGlobalObject;
static PFN_JSStringCreateWithUTF8CString       pfn_JSStringCreateWithUTF8CString;
static PFN_JSStringRelease                     pfn_JSStringRelease;
static PFN_JSStringGetMaximumUTF8CStringSize   pfn_JSStringGetMaximumUTF8CStringSize;
static PFN_JSStringGetUTF8CString              pfn_JSStringGetUTF8CString;
static PFN_JSObjectMakeFunctionWithCallback    pfn_JSObjectMakeFunctionWithCallback;
static PFN_JSObjectSetProperty                 pfn_JSObjectSetProperty;
static PFN_JSValueIsString                     pfn_JSValueIsString;
static PFN_JSValueToStringCopy                 pfn_JSValueToStringCopy;

/* ── Queue and view constants ────────────────────────────────────── */
#define MAX_VIEWS 16
#define CONSOLE_MSG_MAX    64
#define CONSOLE_MSG_BUFLEN 2048
#define MSG_QUEUE_MAX      64
#define MSG_QUEUE_BUFLEN   8192
#define MOUSE_QUEUE_MAX    64
#define SCROLL_QUEUE_MAX   16
#define KEY_QUEUE_MAX      32
#define KEY_TEXT_LEN       32
#define JS_QUEUE_MAX       32
#define JS_QUEUE_BUFLEN    8192

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
    /* Async loading state: 0=ready, 1=priming, 2=post_load */
    int       load_phase;
    int       phase_counter;
    char*     pending_load_str;   /* URL or HTML to load after priming (strdup'd) */
    bool      pending_is_url;
    bool      js_bound;           /* true if setup_js_bindings completed successfully */
    JSContextRef cached_ctx;    /* cached JSC context for callback matching */
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

/* ── Library handles (for resolve_functions) ────────────────────── */
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

/* ── Worker thread commands ──────────────────────────────────────── */
enum CmdType {
    CMD_NONE = 0,
    CMD_INIT,
    CMD_CREATE_VIEW,
    CMD_DESTROY_VIEW,
    CMD_LOAD_HTML,
    CMD_LOAD_URL,
    CMD_TICK,
    CMD_QUIT,
    CMD_CREATE_AND_LOAD,  /* Async: crea view + inicia carga diferida */
    CMD_CREATE_WITH_HTML, /* Sync: create + load HTML in one shot, no sleeping */
    CMD_CREATE_WITH_URL   /* Sync: create + load URL in one shot, no sleeping */
};

/* ── Worker thread synchronization ────────────────────────────────── */
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

/* ── VFS (Virtual File System) ────────────────────────────────────────── */
#define VFS_MAX_FILES 256
#define VFS_PATH_MAX  512

typedef struct {
    char    path[VFS_PATH_MAX];   /* normalized key (no leading /) */
    char*   data;                 /* malloc'd copy */
    size_t  size;
} VfsEntry;

static VfsEntry      g_vfs_files[VFS_MAX_FILES];
static int           g_vfs_count = 0;

/* Normalize path: replace \ with /, strip leading / */
static void vfs_normalize_path(const char* src, char* dst, size_t dst_size) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    size_t j = 0;
    size_t start = 0;
    /* Strip file:/// prefix */
    if (len > 8 && strncmp(src, "file:///", 8) == 0) start = 8;
    for (size_t i = start; i < len && j < dst_size - 1; i++) {
        char c = src[i];
        if (c == '\\') c = '/';
        dst[j++] = c;
    }
    dst[j] = '\0';
    /* Strip leading / */
    char* p = dst;
    while (*p == '/') p++;
    if (p != dst) memmove(dst, p, strlen(p) + 1);
}

/* Find VFS entry by normalized path */
static int vfs_find(const char* normalized) {
    for (int i = 0; i < g_vfs_count; i++) {
        if (strcmp(g_vfs_files[i].path, normalized) == 0) return i;
    }
    return -1;
}

/* Extract path from ULString to normalized C buffer */
static void vfs_extract_path(ULString s, char* out, size_t out_size) {
    if (!pfn_StringGetData || !pfn_StringGetLength) { out[0] = '\0'; return; }
    char* data = pfn_StringGetData(s);
    size_t len = pfn_StringGetLength(s);
    if (!data || len == 0) { out[0] = '\0'; return; }
    char raw[VFS_PATH_MAX];
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(raw, data, len);
    raw[len] = '\0';
    vfs_normalize_path(raw, out, out_size);
}

/* MIME type by file extension */
static const char* vfs_mime_for_ext(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".woff") == 0) return "font/woff";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcmp(dot, ".ttf") == 0) return "font/ttf";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".xml") == 0) return "text/xml";
    if (strcmp(dot, ".txt") == 0) return "text/plain";
    return "application/octet-stream";
}

/* Build full disk fallback path */
static void vfs_disk_path(const char* normalized, char* out, size_t out_size) {
    snprintf(out, out_size, "%s%c%s", g_init_base_dir, PATH_SEP, normalized);
    /* Convert / to native PATH_SEP */
#ifdef _WIN32
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
#endif
}

/* Callback to free disk data when Ultralight destroys the buffer */
static void vfs_free_disk_data(void* user_data, void* data) {
    (void)user_data;
    free(data);
}

/* ── Clipboard callbacks for ulPlatformSetClipboard ───────────────── */
#ifdef _WIN32

static void clipboard_cb_clear(void) {
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

static void clipboard_cb_read(ULString result) {
    if (!OpenClipboard(NULL)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t* wtext = (wchar_t*)GlobalLock(h);
        if (wtext) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
            if (len > 0) {
                char* utf8 = (char*)malloc(len);
                WideCharToMultiByte(CP_UTF8, 0, wtext, -1, utf8, len, NULL, NULL);
                pfn_StringAssignCString(result, utf8);
                free(utf8);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

static void clipboard_cb_write(ULString text) {
    char* data = pfn_StringGetData(text);
    size_t len = pfn_StringGetLength(text);
    if (!data || len == 0) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data, (int)len, NULL, 0);
    if (wlen <= 0) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
    if (!hMem) return;
    wchar_t* dest = (wchar_t*)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, data, (int)len, dest, wlen);
    dest[wlen] = L'\0';
    GlobalUnlock(hMem);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
    } else {
        GlobalFree(hMem);
    }
}

#else /* POSIX clipboard stubs (no X11/Wayland integration yet) */

static char g_posix_clipboard[4096] = {0};

static void clipboard_cb_clear(void) {
    g_posix_clipboard[0] = '\0';
}

static void clipboard_cb_read(ULString result) {
    pfn_StringAssignCString(result, g_posix_clipboard);
}

static void clipboard_cb_write(ULString text) {
    char* data = pfn_StringGetData(text);
    size_t len = pfn_StringGetLength(text);
    if (!data || len == 0) { g_posix_clipboard[0] = '\0'; return; }
    if (len >= sizeof(g_posix_clipboard)) len = sizeof(g_posix_clipboard) - 1;
    memcpy(g_posix_clipboard, data, len);
    g_posix_clipboard[len] = '\0';
}

#endif /* _WIN32 / POSIX clipboard */

/* ── VFS callbacks for ulPlatformSetFileSystem (Ultralight 1.4 API) ── */
static bool vfs_cb_file_exists(ULString path_str) {
    char norm[VFS_PATH_MAX];
    vfs_extract_path(path_str, norm, VFS_PATH_MAX);
    if (norm[0] == '\0') return false;
    /* Check VFS first */
    if (vfs_find(norm) >= 0) { blog("vfs_exists: VFS hit '%s'", norm); return true; }
    /* Fallback to disk */
    char disk[PATHBUF_SIZE];
    vfs_disk_path(norm, disk, PATHBUF_SIZE);
    FILE* f = fopen(disk, "rb");
    if (f) { fclose(f); blog("vfs_exists: disk hit '%s'", disk); return true; }
    blog("vfs_exists: miss '%s'", norm);
    return false;
}

static ULString vfs_cb_get_file_mime_type(ULString path_str) {
    char norm[VFS_PATH_MAX];
    vfs_extract_path(path_str, norm, VFS_PATH_MAX);
    const char* mime = vfs_mime_for_ext(norm);
    blog("vfs_mime: '%s' -> '%s'", norm, mime);
    return pfn_CreateString(mime);
}

static ULString vfs_cb_get_file_charset(ULString path_str) {
    return pfn_CreateString("utf-8");
}

/* open_file: returns ULBuffer with full file data (Ultralight 1.4) */
static ULBuffer vfs_cb_open_file(ULString path_str) {
    char norm[VFS_PATH_MAX];
    vfs_extract_path(path_str, norm, VFS_PATH_MAX);
    if (norm[0] == '\0') return NULL;
    /* Check VFS first: zero-copy wrap (VFS owns data) */
    int idx = vfs_find(norm);
    if (idx >= 0) {
        VfsEntry* e = &g_vfs_files[idx];
        blog("vfs_open: VFS '%s' size=%zu", norm, e->size);
        return pfn_CreateBuffer(e->data, e->size, NULL, NULL);
    }
    /* Fallback to disk: read full file, Ultralight frees via callback */
    char disk[PATHBUF_SIZE];
    vfs_disk_path(norm, disk, PATHBUF_SIZE);
    FILE* f = fopen(disk, "rb");
    if (!f) { blog("vfs_open: NOT FOUND '%s'", norm); return NULL; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { fclose(f); blog("vfs_open: empty '%s'", disk); return NULL; }
    char* buf = (char*)malloc((size_t)file_size);
    if (!buf) { fclose(f); blog("vfs_open: OOM '%s' size=%ld", disk, file_size); return NULL; }
    size_t read = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    blog("vfs_open: disk '%s' size=%zu", disk, read);
    return pfn_CreateBuffer(buf, read, NULL, vfs_free_disk_data);
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

/* ── Symbol resolution ──────────────────────────────────────────── */
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
    RESOLVE(g_hUltralight, pfn_ConfigSetCachePath, "ulConfigSetCachePath");
    RESOLVE(g_hUltralight, pfn_CreateRenderer, "ulCreateRenderer");
    RESOLVE(g_hUltralight, pfn_DestroyRenderer, "ulDestroyRenderer");
    RESOLVE(g_hUltralight, pfn_Update, "ulUpdate");
    /* Opcional: ulRefreshDisplay no existe en versiones públicas del SDK */
    *(void**)&pfn_RefreshDisplay = GETSYM(g_hUltralight, "ulRefreshDisplay");
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
    RESOLVE(g_hUltralight, pfn_SurfaceGetDirtyBounds, "ulSurfaceGetDirtyBounds");
    RESOLVE(g_hUltralight, pfn_VersionString, "ulVersionString");
    RESOLVE(g_hAppCore, pfn_EnablePlatformFontLoader, "ulEnablePlatformFontLoader");
    RESOLVE(g_hAppCore, pfn_EnablePlatformFileSystem, "ulEnablePlatformFileSystem");
    RESOLVE(g_hAppCore, pfn_EnableDefaultLogger, "ulEnableDefaultLogger");
    RESOLVE(g_hUltralight, pfn_PlatformSetFileSystem, "ulPlatformSetFileSystem");
    RESOLVE(g_hUltralight, pfn_CreateBuffer, "ulCreateBuffer");
    RESOLVE(g_hUltralight, pfn_CreateBufferFromCopy, "ulCreateBufferFromCopy");
    RESOLVE(g_hUltralight, pfn_PlatformSetClipboard, "ulPlatformSetClipboard");
    RESOLVE(g_hUltralight, pfn_StringAssignCString, "ulStringAssignCString");
    /* JS context (Ultralight) */
    RESOLVE(g_hUltralight, pfn_ViewLockJSContext, "ulViewLockJSContext");
    RESOLVE(g_hUltralight, pfn_ViewUnlockJSContext, "ulViewUnlockJSContext");
    /* JavaScriptCore (WebCore) */
    if (!g_hWebCore) { blog("FAIL: WebCore handle"); return -52; }
    /* Opcional: JSContextGetGlobalContext normaliza execution ctx -> global ctx (necesario en algunas versiones del SDK) */
    *(void**)&pfn_JSContextGetGlobalContext = GETSYM(g_hWebCore, "JSContextGetGlobalContext");
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

/* JSC native callback: invoked when JS calls window.__goSend(msg).
 * Runs on the worker thread. */
static JSValueRef jsc_goSend_callback(JSContextRef ctx, JSObjectRef function,
    JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception) {
    /* Normalizar execution context -> global context para matching */
    JSContextRef globalCtx = pfn_JSContextGetGlobalContext ? pfn_JSContextGetGlobalContext(ctx) : ctx;
    int vid = -1;
    for (int i = 0; i < MAX_VIEWS; i++) {
        if (g_views[i].used && g_views[i].view && g_views[i].cached_ctx == globalCtx) {
            vid = i; break;
        }
    }
    if (vid < 0) { blog("jsc_goSend_callback: no matching view for ctx=%p global=%p", (void*)ctx, (void*)globalCtx); return NULL; }
    if (argumentCount < 1) { blog("jsc_goSend_callback: no args"); return NULL; }

    /* Convert first argument to UTF-8 */
    JSStringRef jsStr = pfn_JSValueToStringCopy(ctx, arguments[0], NULL);
    if (!jsStr) { blog("jsc_goSend_callback: JSValueToStringCopy failed"); return NULL; }

    size_t maxLen = pfn_JSStringGetMaximumUTF8CStringSize(jsStr);
    ViewSlot* v = &g_views[vid];

    if (v->msg_count < MSG_QUEUE_MAX && maxLen < MSG_QUEUE_BUFLEN) {
        size_t written = pfn_JSStringGetUTF8CString(jsStr, v->msg_queue[v->msg_head], MSG_QUEUE_BUFLEN);
        if (written > 0) written--; /* JSStringGetUTF8CString includes null in the count */
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
 * Must be called on the worker thread when the JS context is ready.
 * Returns true if bindings were set up, false if context not ready. */
static bool setup_js_bindings(int vid) {
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used) return false;
    ViewSlot* v = &g_views[vid];

    JSContextRef ctx = pfn_ViewLockJSContext(v->view);
    if (!ctx) return false;

    /* Cachear el contexto para matching en el callback */
    v->cached_ctx = ctx;

    JSObjectRef global = pfn_JSContextGetGlobalObject(ctx);

    /* Register window.__goSend */
    JSStringRef fnName = pfn_JSStringCreateWithUTF8CString("__goSend");
    JSObjectRef fnObj = pfn_JSObjectMakeFunctionWithCallback(ctx, fnName, jsc_goSend_callback);
    pfn_JSObjectSetProperty(ctx, global, fnName, fnObj, 0, NULL);
    pfn_JSStringRelease(fnName);

    pfn_ViewUnlockJSContext(v->view);

    /* Set up window.go namespace (preserve existing props) */
    ULString goNs = pfn_CreateString("window.go=window.go||{};window.go.send=window.__goSend;");
    pfn_ViewEvaluateScript(v->view, goNs, NULL);
    pfn_DestroyString(goNs);
    v->js_bound = true;
    blog("setup_js_bindings: vid=%d done", vid);
    return true;
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
    /* Custom VFS: Check VFS first, fall back to disk (g_init_base_dir) */
    ULFileSystem fs;
    fs.file_exists       = vfs_cb_file_exists;
    fs.get_file_mime_type = vfs_cb_get_file_mime_type;
    fs.get_file_charset  = vfs_cb_get_file_charset;
    fs.open_file         = vfs_cb_open_file;
    pfn_PlatformSetFileSystem(fs);
    /* Clipboard support (Ctrl+C/V/X) */
    ULClipboard cb;
    cb.clear           = clipboard_cb_clear;
    cb.read_plain_text = clipboard_cb_read;
    cb.write_plain_text = clipboard_cb_write;
    pfn_PlatformSetClipboard(cb);
    ULConfig config = pfn_CreateConfig();
    ULString rp = pfn_CreateString("/");
    pfn_ConfigSetResourcePathPrefix(config, rp);
    pfn_DestroyString(rp);
    /* Cache path: use system TEMP to avoid creating folders next to the exe */
    {
        char tmp[PATHBUF_SIZE];
#ifdef _WIN32
        DWORD len = GetTempPathA(PATHBUF_SIZE, tmp);
        if (len > 0 && len < PATHBUF_SIZE) {
            strncat(tmp, "ultralight_cache", PATHBUF_SIZE - len - 1);
        }
#else
        snprintf(tmp, PATHBUF_SIZE, "/tmp/ultralight_cache");
#endif
        ULString cp = pfn_CreateString(tmp);
        pfn_ConfigSetCachePath(config, cp);
        pfn_DestroyString(cp);
    }
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
    v->js_bound = false;
    v->console_head = v->console_tail = v->console_count = 0;
    v->msg_head = v->msg_tail = v->msg_count = 0;
    v->mouse_count = v->scroll_count = v->key_count = v->js_count = 0;
    pfn_ViewSetConsoleCallback(v->view, console_message_cb, (void*)(intptr_t)vid);
    pfn_ViewFocus(v->view);
    g_view_count++;
    /* Single update cycle, no sleeping — pfn_Update processes synchronously */
    pfn_Update(g_renderer);
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
    v->js_bound = false;
    v->load_phase = 0;
    if (v->pending_load_str) { free(v->pending_load_str); v->pending_load_str = NULL; }
    g_view_count--;
}

static void worker_do_load(int vid, const char* str, bool is_url) {
    if (vid < 0 || vid >= MAX_VIEWS || !g_views[vid].used) return;
    ULView view = g_views[vid].view;
    ULString s = pfn_CreateString(str);
    if (is_url) pfn_ViewLoadURL(view, s);
    else        pfn_ViewLoadHTML(view, s);
    pfn_DestroyString(s);
    /* A few updates to process the load, no sleeping */
    for (int i = 0; i < 3; i++)
        pfn_Update(g_renderer);
    if (pfn_RefreshDisplay) pfn_RefreshDisplay(g_renderer, 0);
    pfn_Render(g_renderer);
    /* Re-register JSC bindings (page load resets the JS context) */
    setup_js_bindings(vid);
}

/* Async create: crea la view sin loops de priming, guarda URL/HTML para carga diferida.
 * La carga real ocurre progresivamente en worker_do_tick. Retorna view_id inmediatamente. */
static int worker_do_create_and_load(int width, int height, const char* str, bool is_url) {
    int vid;
    for (vid = 0; vid < MAX_VIEWS; vid++)
        if (!g_views[vid].used) break;
    if (vid >= MAX_VIEWS) { blog("worker_do_create_and_load: no slot"); return -1; }
    ViewSlot* v = &g_views[vid];
    ULViewConfig vc = pfn_CreateViewConfig();
    pfn_VCSetIsAccelerated(vc, false);
    pfn_VCSetIsTransparent(vc, true);
    pfn_VCSetInitialDeviceScale(vc, 1.0);
    v->view = pfn_CreateView(g_renderer, (unsigned int)width, (unsigned int)height, vc, NULL);
    pfn_DestroyViewConfig(vc);
    if (!v->view) { blog("worker_do_create_and_load: view NULL"); return -11; }
    v->surface = pfn_ViewGetSurface(v->view);
    v->width = width;
    v->height = height;
    v->used = true;
    v->js_bound = false;
    v->console_head = v->console_tail = v->console_count = 0;
    v->msg_head = v->msg_tail = v->msg_count = 0;
    v->mouse_count = v->scroll_count = v->key_count = v->js_count = 0;
    pfn_ViewSetConsoleCallback(v->view, console_message_cb, (void*)(intptr_t)vid);
    pfn_ViewFocus(v->view);
    g_view_count++;
    /* Guardar contenido para carga diferida */
    v->pending_load_str = strdup(str);
    v->pending_is_url = is_url;
    v->load_phase = 1; /* priming */
    v->phase_counter = 0;
    blog("worker_do_create_and_load: vid=%d (async)", vid);
    return vid;
}

/* Fast sync create + load: single worker roundtrip, no sleeping.
 * Combines view creation and content loading into one operation. */
static int worker_do_create_with_content(int width, int height, const char* content, bool is_url) {
    int vid;
    for (vid = 0; vid < MAX_VIEWS; vid++)
        if (!g_views[vid].used) break;
    if (vid >= MAX_VIEWS) { blog("worker_do_create_with_content: no slot"); return -1; }
    ViewSlot* v = &g_views[vid];
    ULViewConfig vc = pfn_CreateViewConfig();
    pfn_VCSetIsAccelerated(vc, false);
    pfn_VCSetIsTransparent(vc, true);
    pfn_VCSetInitialDeviceScale(vc, 1.0);
    v->view = pfn_CreateView(g_renderer, (unsigned int)width, (unsigned int)height, vc, NULL);
    pfn_DestroyViewConfig(vc);
    if (!v->view) { blog("worker_do_create_with_content: view NULL"); return -11; }
    v->surface = pfn_ViewGetSurface(v->view);
    v->width = width;
    v->height = height;
    v->used = true;
    v->js_bound = false;
    v->console_head = v->console_tail = v->console_count = 0;
    v->msg_head = v->msg_tail = v->msg_count = 0;
    v->mouse_count = v->scroll_count = v->key_count = v->js_count = 0;
    v->load_phase = 0;
    v->phase_counter = 0;
    v->pending_load_str = NULL;
    pfn_ViewSetConsoleCallback(v->view, console_message_cb, (void*)(intptr_t)vid);
    pfn_ViewFocus(v->view);
    g_view_count++;
    /* Load content immediately */
    if (content && content[0]) {
        ULString s = pfn_CreateString(content);
        if (is_url) pfn_ViewLoadURL(v->view, s);
        else        pfn_ViewLoadHTML(v->view, s);
        pfn_DestroyString(s);
    }
    /* Minimal processing: one update to kick off parsing.
     * Render is deferred to the next ul_tick() call for maximum speed.
     * JS bindings are set up after the update so the context is ready. */
    pfn_Update(g_renderer);
    setup_js_bindings(vid);
    blog("worker_do_create_with_content: vid=%d", vid);
    return vid;
}

static void worker_do_tick(void) {
    /* Procesar vistas en carga asincronica */
    for (int vid = 0; vid < MAX_VIEWS; vid++) {
        ViewSlot* v = &g_views[vid];
        if (!v->used || v->load_phase == 0) continue;
        v->phase_counter++;
        if (v->load_phase == 1 && v->phase_counter >= 2) {
            /* Priming completado: cargar contenido */
            pfn_Render(g_renderer);
            if (v->pending_load_str) {
                ULString s = pfn_CreateString(v->pending_load_str);
                if (v->pending_is_url) pfn_ViewLoadURL(v->view, s);
                else                   pfn_ViewLoadHTML(v->view, s);
                pfn_DestroyString(s);
                free(v->pending_load_str);
                v->pending_load_str = NULL;
            }
            v->load_phase = 2; /* post-load */
            v->phase_counter = 0;
            blog("async view %d: priming done, loading content", vid);
        } else if (v->load_phase == 2 && v->phase_counter >= 3) {
            /* Post-load completado: setup JS bindings */
            pfn_Render(g_renderer);
            setup_js_bindings(vid);
            v->load_phase = 0; /* ready */
            blog("async view %d: ready", vid);
        }
    }

    for (int vid = 0; vid < MAX_VIEWS; vid++) {
        ViewSlot* v = &g_views[vid];
        if (!v->used || !v->view) continue;
        /* Retry JS bindings if they weren't set up during fast creation */
        if (!v->js_bound && v->load_phase == 0)
            setup_js_bindings(vid);
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
            /* Map type (0=RawKeyDown,1=KeyDown,2=KeyUp,3=Char) to SDK enum (0=KeyDown,1=KeyUp,2=RawKeyDown,3=Char) */
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
    if (pfn_RefreshDisplay) pfn_RefreshDisplay(g_renderer, 0);
    pfn_Render(g_renderer);
}

/* ── send_cmd / worker_thread_proc (platform-specific) ───────────── */
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
        case CMD_CREATE_AND_LOAD:
            g_cmd_result = worker_do_create_and_load(g_cmd_int1, g_cmd_int2, g_cmd_str_arg, true);
            break;
        case CMD_CREATE_WITH_HTML:
            g_cmd_result = worker_do_create_with_content(g_cmd_int1, g_cmd_int2, g_cmd_str_arg, false);
            break;
        case CMD_CREATE_WITH_URL:
            g_cmd_result = worker_do_create_with_content(g_cmd_int1, g_cmd_int2, g_cmd_str_arg, true);
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
        case CMD_CREATE_AND_LOAD:
            g_cmd_result = worker_do_create_and_load(g_cmd_int1, g_cmd_int2, g_cmd_str_arg, true);
            break;
        case CMD_CREATE_WITH_HTML:
            g_cmd_result = worker_do_create_with_content(g_cmd_int1, g_cmd_int2, g_cmd_str_arg, false);
            break;
        case CMD_CREATE_WITH_URL:
            g_cmd_result = worker_do_create_with_content(g_cmd_int1, g_cmd_int2, g_cmd_str_arg, true);
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

/* ── SDK library loading ──────────────────────────────────────────── */
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

/* ── Exported functions for Go ────────────────────────────────────── */
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

/* Async create + load URL: crea la view y programa la carga sin bloquear.
 * La carga real se procesa progresivamente en ul_tick.
 * Retorna view_id (>= 0) inmediatamente, o negativo si error.
 * Usar ul_view_is_ready para saber cuando esta lista. */
EXPORT int ul_create_view_async(int width, int height, const char* url) {
#ifdef _WIN32
    if (!url || !g_worker_thread) return -1;
#else
    if (!url || !g_worker_started) return -1;
#endif
    send_cmd(CMD_CREATE_AND_LOAD, url, width, height);
    return g_cmd_result;
}

/* Fast sync create + load HTML: one worker roundtrip, no sleeping.
 * Retorna view_id (>= 0) o negativo si error. */
EXPORT int ul_create_view_with_html(int width, int height, const char* html) {
#ifdef _WIN32
    if (!g_worker_thread) return -1;
#else
    if (!g_worker_started) return -1;
#endif
    send_cmd(CMD_CREATE_WITH_HTML, html ? html : "", width, height);
    return g_cmd_result;
}

/* Fast sync create + load URL: one worker roundtrip, no sleeping.
 * Retorna view_id (>= 0) o negativo si error. */
EXPORT int ul_create_view_with_url(int width, int height, const char* url) {
#ifdef _WIN32
    if (!url || !g_worker_thread) return -1;
#else
    if (!url || !g_worker_started) return -1;
#endif
    send_cmd(CMD_CREATE_WITH_URL, url, width, height);
    return g_cmd_result;
}

/* Devuelve 1 si la view esta lista (carga async completada), 0 si no. */
EXPORT int ul_view_is_ready(int view_id) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used) return 0;
    return g_views[view_id].load_phase == 0 ? 1 : 0;
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

/* Copia pixels BGRA->RGBA al buffer destino solo si la superficie cambio.
 * Retorna 1 si se copiaron pixels (dirty), 0 si no hubo cambios. */
EXPORT int ul_view_copy_pixels_rgba(int view_id, unsigned char* dest, int dest_size) {
    if (view_id < 0 || view_id >= MAX_VIEWS || !g_views[view_id].used || !g_views[view_id].surface) return 0;
    ViewSlot* v = &g_views[view_id];
    /* Verificar si la superficie tiene cambios */
    ULIntRect dirty = pfn_SurfaceGetDirtyBounds(v->surface);
    if (dirty.left >= dirty.right || dirty.top >= dirty.bottom) return 0;
    unsigned char* src = (unsigned char*)pfn_SurfaceLockPixels(v->surface);
    if (!src) return 0;
    int w = v->width;
    int h = v->height;
    unsigned int rowBytes = pfn_SurfaceGetRowBytes(v->surface);
    int needed = w * h * 4;
    if (dest_size < needed) {
        pfn_SurfaceUnlockPixels(v->surface);
        return 0;
    }
    /* Conversion BGRA -> RGBA en C (mucho mas rapido que Go) */
    int dstIdx = 0;
    for (int y = 0; y < h; y++) {
        unsigned char* row = src + y * rowBytes;
        for (int x = 0; x < w; x++) {
            int off = x * 4;
            dest[dstIdx+0] = row[off+2];
            dest[dstIdx+1] = row[off+1];
            dest[dstIdx+2] = row[off+0];
            dest[dstIdx+3] = row[off+3];
            dstIdx += 4;
        }
    }
    pfn_SurfaceUnlockPixels(v->surface);
    pfn_SurfaceClearDirtyBounds(v->surface);
    return 1;
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

/* ── VFS exports for Go ──────────────────────────────────────────────── */
EXPORT int ul_vfs_register(const char* path, const void* data, long long size) {
    if (!path || !data || size < 0) return -1;
    char norm[VFS_PATH_MAX];
    vfs_normalize_path(path, norm, VFS_PATH_MAX);
    /* Overwrite if already exists */
    int idx = vfs_find(norm);
    if (idx >= 0) {
        free(g_vfs_files[idx].data);
        g_vfs_files[idx].data = (char*)malloc((size_t)size);
        if (!g_vfs_files[idx].data) return -2;
        memcpy(g_vfs_files[idx].data, data, (size_t)size);
        g_vfs_files[idx].size = (size_t)size;
        blog("vfs_register: overwrite '%s' size=%lld", norm, size);
        return 0;
    }
    if (g_vfs_count >= VFS_MAX_FILES) { blog("vfs_register: FULL"); return -3; }
    VfsEntry* e = &g_vfs_files[g_vfs_count];
    strncpy(e->path, norm, VFS_PATH_MAX - 1);
    e->path[VFS_PATH_MAX - 1] = '\0';
    e->data = (char*)malloc((size_t)size);
    if (!e->data) return -2;
    memcpy(e->data, data, (size_t)size);
    e->size = (size_t)size;
    g_vfs_count++;
    blog("vfs_register: '%s' size=%lld count=%d", norm, size, g_vfs_count);
    return 0;
}

EXPORT void ul_vfs_clear(void) {
    for (int i = 0; i < g_vfs_count; i++) {
        free(g_vfs_files[i].data);
        g_vfs_files[i].data = NULL;
        g_vfs_files[i].size = 0;
        g_vfs_files[i].path[0] = '\0';
    }
    g_vfs_count = 0;
    blog("vfs_clear: done");
}

EXPORT int ul_vfs_count(void) {
    return g_vfs_count;
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
    ul_vfs_clear();
    if (g_log) { fclose(g_log); g_log = NULL; }
}

/* ── DllMain (Windows only) ──────────────────────────────────────────── */
#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
#endif
