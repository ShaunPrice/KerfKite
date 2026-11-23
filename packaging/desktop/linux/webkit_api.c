/* WebKitGTK found when the window starts. See webkit_api.h. */
#include "webkit_api.h"

#include <dlfcn.h>
#include <stddef.h>
#include <string.h>

static WebKitApi api;
static gboolean ready;

typedef struct {
    size_t offset;
    const char *name;
    gboolean required;
} Symbol;

#define NEED(field, name) {offsetof(WebKitApi, field), name, TRUE}
#define MAY(field, name) {offsetof(WebKitApi, field), name, FALSE}

static const Symbol symbols[] = {
    NEED(web_view_get_type, "webkit_web_view_get_type"),
    NEED(web_view_new, "webkit_web_view_new"),
    NEED(web_view_new_with_related_view, "webkit_web_view_new_with_related_view"),
    NEED(web_view_load_uri, "webkit_web_view_load_uri"),
    NEED(web_view_get_uri, "webkit_web_view_get_uri"),
    NEED(web_view_is_loading, "webkit_web_view_is_loading"),
    NEED(web_view_get_context, "webkit_web_view_get_context"),
    MAY(web_view_evaluate_javascript, "webkit_web_view_evaluate_javascript"),
    MAY(web_view_evaluate_javascript_finish, "webkit_web_view_evaluate_javascript_finish"),
    MAY(web_view_run_javascript, "webkit_web_view_run_javascript"),
    MAY(web_view_run_javascript_finish, "webkit_web_view_run_javascript_finish"),
    MAY(javascript_result_get_js_value, "webkit_javascript_result_get_js_value"),
    MAY(javascript_result_unref, "webkit_javascript_result_unref"),
    NEED(policy_decision_download, "webkit_policy_decision_download"),
    NEED(policy_decision_ignore, "webkit_policy_decision_ignore"),
    NEED(navigation_policy_decision_get_navigation_action, "webkit_navigation_policy_decision_get_navigation_action"),
    NEED(navigation_action_get_request, "webkit_navigation_action_get_request"),
    NEED(uri_request_get_uri, "webkit_uri_request_get_uri"),
    NEED(download_get_web_view, "webkit_download_get_web_view"),
    NEED(download_get_request, "webkit_download_get_request"),
    NEED(download_cancel, "webkit_download_cancel"),
    NEED(download_set_allow_overwrite, "webkit_download_set_allow_overwrite"),
    NEED(download_set_destination, "webkit_download_set_destination"),
    NEED(download_error_quark, "webkit_download_error_quark"),
    NEED(jsc_context_new, "jsc_context_new"),
    NEED(jsc_value_new_from_json, "jsc_value_new_from_json"),
    NEED(jsc_value_to_json, "jsc_value_to_json"),
    NEED(jsc_value_new_string, "jsc_value_new_string"),
    NEED(jsc_value_new_number, "jsc_value_new_number"),
    NEED(jsc_value_new_object, "jsc_value_new_object"),
    NEED(jsc_value_object_set_property, "jsc_value_object_set_property"),
    NEED(jsc_value_object_get_property, "jsc_value_object_get_property"),
    NEED(jsc_value_object_has_property, "jsc_value_object_has_property"),
    NEED(jsc_value_object_get_property_at_index, "jsc_value_object_get_property_at_index"),
    NEED(jsc_value_is_string, "jsc_value_is_string"),
    NEED(jsc_value_is_number, "jsc_value_is_number"),
    NEED(jsc_value_is_boolean, "jsc_value_is_boolean"),
    NEED(jsc_value_is_null, "jsc_value_is_null"),
    NEED(jsc_value_is_undefined, "jsc_value_is_undefined"),
    NEED(jsc_value_is_object, "jsc_value_is_object"),
    NEED(jsc_value_is_array, "jsc_value_is_array"),
    NEED(jsc_value_to_string, "jsc_value_to_string"),
    NEED(jsc_value_to_double, "jsc_value_to_double"),
    NEED(jsc_value_to_boolean, "jsc_value_to_boolean"),
};

const WebKitApi *webkit_api(char **reason) {
    static const char *const libraries[] = {"libwebkit2gtk-4.1.so.0", "libwebkit2gtk-4.0.so.37"};
    if (ready) return &api;
    void *handle = NULL;
    const char *found = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(libraries) && !handle; ++i) {
        handle = dlopen(libraries[i], RTLD_NOW | RTLD_GLOBAL);
        if (handle) found = libraries[i];
    }
    if (!handle) {
        if (reason)
            *reason = g_strdup("WebKitGTK is not installed. Install libwebkit2gtk-4.1-0 (or libwebkit2gtk-4.0-37), "
                               "or choose Open in browser.");
        return NULL;
    }
    WebKitApi loaded = {.library = found};
    for (size_t i = 0; i < G_N_ELEMENTS(symbols); ++i) {
        void *function = dlsym(handle, symbols[i].name);  /* the library's own dependencies are searched too */
        if (!function && symbols[i].required) {
            if (reason) *reason = g_strdup_printf("%s lacks %s. Update WebKitGTK.", found, symbols[i].name);
            dlclose(handle);
            return NULL;
        }
        memcpy((char *)&loaded + symbols[i].offset, &function, sizeof function);
    }
    const gboolean evaluate = loaded.web_view_evaluate_javascript && loaded.web_view_evaluate_javascript_finish;
    const gboolean run = loaded.web_view_run_javascript && loaded.web_view_run_javascript_finish &&
                         loaded.javascript_result_get_js_value && loaded.javascript_result_unref;
    if (!evaluate && !run) {
        if (reason) *reason = g_strdup_printf("%s cannot run the editor's scripts. Update WebKitGTK.", found);
        dlclose(handle);
        return NULL;
    }
    api = loaded;
    ready = TRUE;
    return &api;
}
