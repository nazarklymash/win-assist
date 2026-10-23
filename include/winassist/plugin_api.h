/*
 * win-assist native plugin ABI (C, stable across compilers).
 *
 * A native plugin is a DLL placed in plugins/<id>/ next to a manifest.json
 * with "type": "native" and "entry": "<file>.dll". All strings are UTF-8.
 *
 * Required exports:
 *   const wa_plugin_info* wa_plugin_get_info(void);
 *   char* wa_plugin_invoke(const char* request_json);
 *   void  wa_plugin_free(char* response_json);
 * Optional exports:
 *   int  wa_plugin_init(const wa_host_api* host);  // non-zero = failure
 *   void wa_plugin_shutdown(void);
 *
 * wa_plugin_init and wa_plugin_shutdown run on the daemon's UI thread, so a
 * plugin may create windows there (the daemon pumps messages). wa_plugin_invoke
 * runs on a background thread, one call at a time.
 *
 * Request JSON:
 *   {"intent":"...","parameters":{...},"utterance":"...",
 *    "active_window":{"title":"...","process":"...","hwnd":123}}
 * Response JSON:
 *   {"status":"ok"|"error","display":"...","speech":"...","inject_text":"..."}
 */
#ifndef WINASSIST_PLUGIN_API_H
#define WINASSIST_PLUGIN_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_PLUGIN_API_VERSION 1u

#if defined(_WIN32)
#define WA_PLUGIN_EXPORT __declspec(dllexport)
#else
#define WA_PLUGIN_EXPORT
#endif

enum wa_log_level { WA_LOG_INFO = 0, WA_LOG_WARN = 1, WA_LOG_ERROR = 2 };

typedef struct wa_host_api {
    uint32_t api_version;
    void* host;
    void (*log)(void* host, int level, const char* message);
    /* POSTs `body_json` to the configured Ollama server at `path` (e.g.
     * "/api/embed"). Returns the response body or NULL; release it with
     * free_string. Safe to call from wa_plugin_invoke. */
    char* (*ollama_post)(void* host, const char* path, const char* body_json);
    void (*free_string)(void* host, char* text);
    /* Current daemon configuration as JSON; release with free_string. */
    char* (*config_json)(void* host);
    /* Schedule a local reminder. delay_ms in [1000, 86400000]. Returns 0 on
     * success. May be NULL on older hosts — check before calling. */
    int (*schedule_reminder)(void* host, int delay_ms, const char* message_utf8);
} wa_host_api;

typedef struct wa_plugin_info {
    uint32_t api_version;
    const char* id;
    const char* version;
} wa_plugin_info;

typedef const wa_plugin_info* (*wa_plugin_get_info_fn)(void);
typedef int (*wa_plugin_init_fn)(const wa_host_api* host);
typedef char* (*wa_plugin_invoke_fn)(const char* request_json);
typedef void (*wa_plugin_free_fn)(char* response_json);
typedef void (*wa_plugin_shutdown_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* WINASSIST_PLUGIN_API_H */
