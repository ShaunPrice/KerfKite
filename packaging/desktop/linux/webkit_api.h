/* The WebKitGTK and JavaScriptCore functions the Linux window uses.
 *
 * They are found when the window starts, in WebKitGTK 4.1 or else 4.0 (the same interface, over libsoup 3 or 2),
 * so one build runs on distributions that carry either, and building needs only the GTK 3 headers. */
#ifndef KERFKITE_WEBKIT_API_H
#define KERFKITE_WEBKIT_API_H

#include <gtk/gtk.h>

typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitPolicyDecision WebKitPolicyDecision;
typedef struct _WebKitNavigationAction WebKitNavigationAction;
typedef struct _WebKitURIRequest WebKitURIRequest;
typedef struct _WebKitDownload WebKitDownload;
typedef struct _WebKitJavascriptResult WebKitJavascriptResult;
typedef struct _JSCContext JSCContext;
typedef struct _JSCValue JSCValue;

/* WebKitPolicyDecisionType, WebKitLoadEvent and WebKitDownloadError values. */
enum { KK_POLICY_NAVIGATION_ACTION = 0, KK_POLICY_NEW_WINDOW_ACTION = 1 };
enum { KK_LOAD_STARTED = 0, KK_LOAD_FINISHED = 3 };
enum { KK_DOWNLOAD_ERROR_CANCELLED_BY_USER = 400 };

typedef struct {
    const char *library; /* the WebKitGTK library found */
    GType (*web_view_get_type)(void);
    GtkWidget *(*web_view_new)(void);
    GtkWidget *(*web_view_new_with_related_view)(WebKitWebView *view);
    void (*web_view_load_uri)(WebKitWebView *view, const char *uri);
    const char *(*web_view_get_uri)(WebKitWebView *view);
    gboolean (*web_view_is_loading)(WebKitWebView *view);
    WebKitWebContext *(*web_view_get_context)(WebKitWebView *view);
    /* WebKitGTK 2.40 and later; run_javascript before. */
    void (*web_view_evaluate_javascript)(WebKitWebView *view, const char *script, gssize length, const char *world,
                                         const char *source_uri, GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer data);
    JSCValue *(*web_view_evaluate_javascript_finish)(WebKitWebView *view, GAsyncResult *result, GError **error);
    void (*web_view_run_javascript)(WebKitWebView *view, const char *script, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer data);
    WebKitJavascriptResult *(*web_view_run_javascript_finish)(WebKitWebView *view, GAsyncResult *result,
                                                              GError **error);
    JSCValue *(*javascript_result_get_js_value)(WebKitJavascriptResult *result);
    void (*javascript_result_unref)(WebKitJavascriptResult *result);
    void (*policy_decision_download)(WebKitPolicyDecision *decision);
    void (*policy_decision_ignore)(WebKitPolicyDecision *decision);
    WebKitNavigationAction *(*navigation_policy_decision_get_navigation_action)(WebKitPolicyDecision *decision);
    WebKitURIRequest *(*navigation_action_get_request)(WebKitNavigationAction *action);
    const char *(*uri_request_get_uri)(WebKitURIRequest *request);
    WebKitWebView *(*download_get_web_view)(WebKitDownload *download);
    WebKitURIRequest *(*download_get_request)(WebKitDownload *download);
    void (*download_cancel)(WebKitDownload *download);
    void (*download_set_allow_overwrite)(WebKitDownload *download, gboolean allowed);
    void (*download_set_destination)(WebKitDownload *download, const char *uri);
    GQuark (*download_error_quark)(void);
    JSCContext *(*jsc_context_new)(void);
    JSCValue *(*jsc_value_new_from_json)(JSCContext *context, const char *json);
    char *(*jsc_value_to_json)(JSCValue *value, guint indent);
    JSCValue *(*jsc_value_new_string)(JSCContext *context, const char *text);
    JSCValue *(*jsc_value_new_number)(JSCContext *context, double number);
    JSCValue *(*jsc_value_new_object)(JSCContext *context, gpointer instance, gpointer jsc_class);
    void (*jsc_value_object_set_property)(JSCValue *value, const char *name, JSCValue *property);
    JSCValue *(*jsc_value_object_get_property)(JSCValue *value, const char *name);
    gboolean (*jsc_value_object_has_property)(JSCValue *value, const char *name);
    JSCValue *(*jsc_value_object_get_property_at_index)(JSCValue *value, guint index);
    gboolean (*jsc_value_is_string)(JSCValue *value);
    gboolean (*jsc_value_is_number)(JSCValue *value);
    gboolean (*jsc_value_is_boolean)(JSCValue *value);
    gboolean (*jsc_value_is_null)(JSCValue *value);
    gboolean (*jsc_value_is_undefined)(JSCValue *value);
    gboolean (*jsc_value_is_object)(JSCValue *value);
    gboolean (*jsc_value_is_array)(JSCValue *value);
    char *(*jsc_value_to_string)(JSCValue *value);
    double (*jsc_value_to_double)(JSCValue *value);
    gboolean (*jsc_value_to_boolean)(JSCValue *value);
} WebKitApi;

/* The functions, loaded on the first call. NULL when neither WebKitGTK 4.1 nor 4.0 is installed or one lacks a
 * function; *reason (when given) then says which, for g_free. */
const WebKitApi *webkit_api(char **reason);

#endif
