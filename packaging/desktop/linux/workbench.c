/* The Linux Studio workbench. See workbench.h. */
#include "workbench.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const TABS[] = {"Design", "Objects", "Properties", "Layers",  "Text",
                                   "Images", "Templates", "3D CAD",   "CAM",     "3D relief",
                                   "Rotary", "Control",  "Projects", "Quote",   "Help"};

/* ---- addresses ---------------------------------------------------------------------------------------------- */

char *studio_url(const char *url) {
    static const char host[] = "127.0.0.1:";
    if (!url || g_ascii_strncasecmp(url, "http://", 7) != 0 || strncmp(url + 7, host, sizeof host - 1) != 0)
        return NULL;
    const char *digits = url + 7 + sizeof host - 1, *end = digits;
    long port = 0;
    while (*end >= '0' && *end <= '9' && end - digits < 6) port = port * 10 + (*end++ - '0');
    // One way to write the port: no leading zero, nothing after it but the root path.
    if (end == digits || digits[0] == '0' || port > 65535) return NULL;
    if (!(end[0] == '\0' || (end[0] == '/' && end[1] == '\0'))) return NULL;
    return g_strdup_printf("http://127.0.0.1:%ld/", port);
}

gboolean auxiliary_uri(const Workbench *bench, const char *uri) {
    if (!uri || !*uri || strcmp(uri, "about:blank") == 0) return TRUE;
    const char *colon = strchr(uri, ':');
    if (!colon || !g_ascii_isalpha(uri[0])) return FALSE;
    for (const char *c = uri; c < colon; ++c)
        if (!(g_ascii_isalnum(*c) || *c == '+' || *c == '-' || *c == '.')) return FALSE;
    if (colon - uri != 4 || g_ascii_strncasecmp(uri, "http", 4) != 0 || strncmp(colon + 1, "//", 2) != 0) return FALSE;
    const char *netloc = colon + 3;
    const size_t length = strcspn(netloc, "/?#");
    return length == strlen(bench->netloc) && strncmp(netloc, bench->netloc, length) == 0;
}

static gboolean trusted(Workbench *bench) {
    if (!bench->loaded || bench->destroyed) return FALSE;
    const char *uri = bench->api->web_view_get_uri(bench->view);
    return uri && strcmp(uri, bench->url) == 0;
}

/* ---- the editor's values ------------------------------------------------------------------------------------ */

JSCValue *payload_new(Workbench *bench) { return bench->api->jsc_value_new_object(bench->json, NULL, NULL); }

void payload_number(Workbench *bench, JSCValue *payload, const char *name, double value) {
    JSCValue *number = bench->api->jsc_value_new_number(bench->json, value);
    bench->api->jsc_value_object_set_property(payload, name, number);
    g_object_unref(number);
}

void payload_string(Workbench *bench, JSCValue *payload, const char *name, const char *text) {
    JSCValue *string = bench->api->jsc_value_new_string(bench->json, text);
    bench->api->jsc_value_object_set_property(payload, name, string);
    g_object_unref(string);
}

JSCValue *member(Workbench *bench, JSCValue *object, const char *name) {
    const WebKitApi *api = bench->api;
    if (!object || !api->jsc_value_is_object(object) || !api->jsc_value_object_has_property(object, name))
        return NULL;
    return api->jsc_value_object_get_property(object, name);
}

gboolean truthy(Workbench *bench, JSCValue *value) {
    const WebKitApi *api = bench->api;
    if (!value || api->jsc_value_is_null(value) || api->jsc_value_is_undefined(value)) return FALSE;
    return api->jsc_value_is_object(value) || api->jsc_value_to_boolean(value);
}

double member_number(Workbench *bench, JSCValue *object, const char *name, double fallback) {
    JSCValue *value = member(bench, object, name);
    double number = fallback;
    if (value && bench->api->jsc_value_is_number(value)) number = bench->api->jsc_value_to_double(value);
    if (value) g_object_unref(value);
    return number;
}

char *member_text(Workbench *bench, JSCValue *object, const char *name) {
    JSCValue *value = member(bench, object, name);
    char *text = NULL;
    if (value && !bench->api->jsc_value_is_null(value) && !bench->api->jsc_value_is_undefined(value))
        text = bench->api->jsc_value_to_string(value);
    if (value) g_object_unref(value);
    return text;
}

/* A member as JSON text, "null" when it is missing: how two states are compared. */
static char *member_json(Workbench *bench, JSCValue *object, const char *name) {
    JSCValue *value = member(bench, object, name);
    char *text = value ? bench->api->jsc_value_to_json(value, 0) : NULL;
    if (value) g_object_unref(value);
    return text ? text : g_strdup("null");
}

static gboolean whole_number(Workbench *bench, JSCValue *value, double *out) {
    if (!value || !bench->api->jsc_value_is_number(value)) return FALSE;
    const double number = bench->api->jsc_value_to_double(value);
    if (!isfinite(number) || floor(number) != number) return FALSE;
    *out = number;
    return TRUE;
}

/* ---- commands to the editor --------------------------------------------------------------------------------- */

void workbench_evaluate(Workbench *bench, WebKitWebView *view, const char *script, GAsyncReadyCallback callback,
                        gpointer data) {
    if (bench->api->web_view_evaluate_javascript)
        bench->api->web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL, callback, data);
    else
        bench->api->web_view_run_javascript(view, script, NULL, callback, data);
}

JSCValue *workbench_evaluate_finish(Workbench *bench, WebKitWebView *view, GAsyncResult *result, GError **error) {
    const WebKitApi *api = bench->api;
    if (api->web_view_evaluate_javascript) return api->web_view_evaluate_javascript_finish(view, result, error);
    WebKitJavascriptResult *answer = api->web_view_run_javascript_finish(view, result, error);
    if (!answer) return NULL;
    JSCValue *value = g_object_ref(api->javascript_result_get_js_value(answer));
    api->javascript_result_unref(answer);
    return value;
}

typedef struct {
    Workbench *bench;
    char *action;
    Answer done;
    gpointer data;
    guint generation;
    gboolean pending; /* asking for an operation's result: keep asking while it runs */
} Call;

typedef struct {
    Workbench *bench;
    double ticket;
    Answer done;
    gpointer data;
    guint generation;
} Poll;

static void ask(Workbench *bench, const char *action, JSCValue *payload, Answer done, gpointer data, gboolean pending);

static gboolean poll_result(gpointer user) {
    Poll *poll = user;
    Workbench *bench = poll->bench;
    if (poll->generation == bench->generation && !bench->destroyed) {
        JSCValue *payload = payload_new(bench);
        payload_number(bench, payload, "id", poll->ticket);
        ask(bench, "result", payload, poll->done, poll->data, TRUE);
        g_object_unref(payload);
    } else if (!bench->destroyed) {
        poll->done(bench, NULL, "The workspace changed.", poll->data);
    }
    g_free(poll);
    return G_SOURCE_REMOVE;
}

static void answered(GObject *source, GAsyncResult *result, gpointer user) {
    Call *call = user;
    Workbench *bench = call->bench;
    const WebKitApi *api = bench->api;
    GError *error = NULL;
    JSCValue *value = workbench_evaluate_finish(bench, (WebKitWebView *)source, result, &error);
    JSCValue *data = NULL, *reply = NULL;
    char *problem = NULL;
    gboolean waiting = FALSE;
    if (bench->destroyed) goto finished;
    if (call->generation != bench->generation || !trusted(bench)) {
        problem = g_strdup("The workspace changed.");
    } else if (!value) {
        problem = g_strdup(error && error->message ? error->message : "Studio could not complete the operation.");
    } else if (!api->jsc_value_is_string(value)) {
        problem = g_strdup("Waiting for the Studio editor.");
    } else {
        char *text = api->jsc_value_to_string(value);
        data = text ? api->jsc_value_new_from_json(bench->json, text) : NULL;
        g_free(text);
        JSCValue *ok = data && api->jsc_value_is_object(data) && !api->jsc_value_is_array(data)
                           ? member(bench, data, "ok")
                           : NULL;
        if (!data || !api->jsc_value_is_object(data) || api->jsc_value_is_array(data)) {
            problem = g_strdup("Unexpected editor response.");
        } else if (!truthy(bench, ok)) {
            char *message = member_text(bench, data, "error");
            if (message && *message) problem = message;
            else {
                g_free(message);
                problem = g_strdup("Studio could not complete the operation.");
            }
        } else {
            JSCValue *pending = member(bench, data, "pending");
            double ticket = 0;
            waiting = whole_number(bench, pending, &ticket) &&
                      (call->pending || strcmp(call->action, "open") == 0 || strcmp(call->action, "preview") == 0);
            if (pending) g_object_unref(pending);
            if (waiting) {
                Poll *poll = g_new0(Poll, 1);
                poll->bench = bench;
                poll->ticket = ticket;
                poll->done = call->done;
                poll->data = call->data;
                poll->generation = call->generation;
                g_timeout_add(60, poll_result, poll);
            } else {
                reply = member(bench, data, "value");
                if (!reply) problem = g_strdup("Unexpected editor response.");
            }
        }
        if (ok) g_object_unref(ok);
    }
    if (!waiting) call->done(bench, problem ? NULL : reply, problem, call->data);
finished:
    g_free(problem);
    if (reply) g_object_unref(reply);
    if (data) g_object_unref(data);
    if (value) g_object_unref(value);
    g_clear_error(&error);
    g_free(call->action);
    g_free(call);
}

/* Sends window.KerfKiteDesktop.dispatch(action, payload) to the editor of this Studio, and nothing to any other
 * page: done receives the value of its answer. */
static void ask(Workbench *bench, const char *action, JSCValue *payload, Answer done, gpointer data, gboolean pending) {
    const WebKitApi *api = bench->api;
    if (!trusted(bench)) {
        done(bench, NULL, "Studio is unavailable.", data);
        return;
    }
    JSCValue *empty = payload ? NULL : payload_new(bench);
    JSCValue *name = api->jsc_value_new_string(bench->json, action);
    JSCValue *address = api->jsc_value_new_string(bench->json, bench->url);
    char *action_json = api->jsc_value_to_json(name, 0);
    char *payload_json = api->jsc_value_to_json(payload ? payload : empty, 0);
    char *url_json = api->jsc_value_to_json(address, 0);
    char *script = g_strconcat("(()=>{if(window!==window.top || location.href!==", url_json,
                               " || !window.KerfKiteDesktop)return null;return window.KerfKiteDesktop.dispatch(...[",
                               action_json, ",", payload_json, "])})()", NULL);
    Call *call = g_new0(Call, 1);
    call->bench = bench;
    call->action = g_strdup(action);
    call->done = done;
    call->data = data;
    call->generation = bench->generation;
    call->pending = pending;
    workbench_evaluate(bench, bench->view, script, answered, call);
    g_free(script);
    g_free(url_json);
    g_free(payload_json);
    g_free(action_json);
    g_object_unref(address);
    g_object_unref(name);
    if (empty) g_object_unref(empty);
}

/* ---- the state shown beside the editor ---------------------------------------------------------------------- */

static void show_state(Workbench *bench, JSCValue *state) {
    const WebKitApi *api = bench->api;
    JSCValue *objects = member(bench, state, "objects");
    if (!objects || !api->jsc_value_is_array(objects)) {
        if (objects) g_object_unref(objects);
        return;
    }
    const guint count = (guint)member_number(bench, objects, "length", 0);
    const double selected = member_number(bench, state, "selected", -1);
    char *objects_json = api->jsc_value_to_json(objects, 0);
    char *before = bench->last ? member_json(bench, bench->last, "objects") : NULL;
    const gboolean changed = !before || g_strcmp0(objects_json, before) != 0;
    GtkTreeModel *model = GTK_TREE_MODEL(bench->store);
    bench->syncing = TRUE;
    if (changed) {
        gtk_list_store_clear(bench->store);
        for (guint i = 0; i < count; ++i) {
            JSCValue *item = api->jsc_value_object_get_property_at_index(objects, i);
            char *name = member_text(bench, item, "name");
            GtkTreeIter row;
            gtk_list_store_append(bench->store, &row);
            gtk_list_store_set(bench->store, &row, 0, (gint)member_number(bench, item, "index", -1), 1, name ? name : "",
                               -1);
            g_free(name);
            g_object_unref(item);
        }
    }
    GtkTreeIter row;
    GtkTreeModel *shown = NULL;
    gint displayed = -1;
    if (gtk_tree_selection_get_selected(bench->selection, &shown, &row)) gtk_tree_model_get(shown, &row, 0, &displayed, -1);
    if ((double)displayed != selected) {
        gtk_tree_selection_unselect_all(bench->selection);
        for (gboolean more = gtk_tree_model_get_iter_first(model, &row); more; more = gtk_tree_model_iter_next(model, &row)) {
            gint index = -1;
            gtk_tree_model_get(model, &row, 0, &index, -1);
            if ((double)index == selected) {
                gtk_tree_selection_select_iter(bench->selection, &row);
                break;
            }
        }
    }
    bench->syncing = FALSE;

    JSCValue *selection = member(bench, state, "selection");
    char *selection_json = selection ? api->jsc_value_to_json(selection, 0) : NULL;
    if (!selection_json) selection_json = g_strdup("null");
    gboolean renew = TRUE;
    if (bench->last && api->jsc_value_object_has_property(bench->last, "selection")) {
        char *previous = member_json(bench, bench->last, "selection");
        renew = strcmp(previous, selection_json) != 0 || member_number(bench, bench->last, "selected", NAN) != selected;
        g_free(previous);
    }
    if (renew) {
        const gboolean any = truthy(bench, selection);
        gtk_widget_set_sensitive(GTK_WIDGET(bench->move_x), any);
        gtk_widget_set_sensitive(GTK_WIDGET(bench->move_y), any);
        gtk_widget_set_sensitive(bench->move_button, any);
        if (any) {
            gtk_spin_button_set_value(bench->move_x, member_number(bench, selection, "x", 0));
            gtk_spin_button_set_value(bench->move_y, member_number(bench, selection, "y", 0));
            char *name = member_text(bench, selection, "name");
            char *text = g_strdup_printf("%s\n%.2f \xc3\x97 %.2f mm", name ? name : "",
                                         member_number(bench, selection, "width", 0),
                                         member_number(bench, selection, "height", 0));
            gtk_label_set_text(GTK_LABEL(bench->properties), text);
            g_free(text);
            g_free(name);
        } else {
            gtk_label_set_text(GTK_LABEL(bench->properties), "Select an object");
        }
    }
    if (bench->last) g_object_unref(bench->last);
    bench->last = g_object_ref(state);

    JSCValue *dirty = member(bench, state, "dirty");
    char *title = member_text(bench, state, "title");
    char *window_title =
        g_strconcat(truthy(bench, dirty) ? "\xe2\x80\xa2 " : "", title ? title : "", " \xe2\x80\x94 KerfKite Studio", NULL);
    gtk_window_set_title(bench->window, window_title);
    char *mode = member_text(bench, state, "mode");
    char *status = g_strdup_printf("%s   \xc2\xb7   %u objects", mode ? mode : "", count);
    gtk_label_set_text(GTK_LABEL(bench->status), status);
    g_free(status);
    g_free(mode);
    g_free(window_title);
    g_free(title);
    if (dirty) g_object_unref(dirty);
    g_free(selection_json);
    if (selection) g_object_unref(selection);
    g_free(before);
    g_free(objects_json);
    g_object_unref(objects);
}

static void updated(Workbench *bench, JSCValue *state, const char *error, gpointer data) {
    bench->polling = FALSE;
    if (bench->busy || GPOINTER_TO_UINT(data) != bench->revision) return;
    if (error) gtk_label_set_text(GTK_LABEL(bench->status), error);
    else show_state(bench, state);
}

static gboolean refresh(gpointer user) {
    Workbench *bench = user;
    if (bench->destroyed) return G_SOURCE_REMOVE;
    if (trusted(bench) && !bench->busy && !bench->polling) {
        bench->polling = TRUE;
        ask(bench, "state", NULL, updated, GUINT_TO_POINTER(bench->revision), FALSE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- commands, with their dialogs --------------------------------------------------------------------------- */

static void show_error(Workbench *bench, const char *message) {
    GtkWidget *dialog = gtk_message_dialog_new(bench->window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                               "%s", "Studio could not complete the operation");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

typedef struct {
    Answer success;
    gpointer data;
    GDestroyNotify free_data;
} Completion;

static void completed(Workbench *bench, JSCValue *value, const char *error, gpointer user) {
    Completion *completion = user;
    bench->busy = FALSE;
    if (error) bench->report(bench, error);
    else if (completion->success) completion->success(bench, value, NULL, completion->data);
    if (completion->free_data) completion->free_data(completion->data);
    g_free(completion);
    refresh(bench);
}

static void command_with(Workbench *bench, const char *action, JSCValue *payload, Answer success, gpointer data,
                         GDestroyNotify free_data) {
    if (bench->busy) {
        if (free_data) free_data(data);
        return;
    }
    bench->busy = TRUE;
    bench->revision += 1;
    Completion *completion = g_new0(Completion, 1);
    completion->success = success;
    completion->data = data;
    completion->free_data = free_data;
    ask(bench, action, payload, completed, completion, FALSE);
}

void workbench_command(Workbench *bench, const char *action, JSCValue *payload, Answer success, gpointer data) {
    command_with(bench, action, payload, success, data, NULL);
}

static gboolean confirm_discard(Workbench *bench, const char *verb) {
    GtkWidget *dialog = gtk_message_dialog_new(bench->window, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                               "%s", "Discard unsaved project changes?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", "Save the project first to keep your edits.");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), verb, GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_ACCEPT;
}

typedef void (*Proceed)(Workbench *bench);

typedef struct {
    const char *verb;
    Proceed proceed;
    gboolean closing;
} Guard;

static void checked(Workbench *bench, JSCValue *state, const char *error, gpointer user) {
    Guard *guard = user;
    bench->busy = FALSE;
    if (error && !guard->closing) {
        bench->report(bench, error);
    } else {
        gboolean dirty;
        if (error) {
            dirty = bench->last != NULL;  // the editor no longer answers: unsaved unless it never had a state
        } else {
            JSCValue *value = member(bench, state, "dirty");
            dirty = truthy(bench, value);
            if (value) g_object_unref(value);
        }
        if (!dirty || confirm_discard(bench, guard->verb)) guard->proceed(bench);
    }
    g_free(guard);
}

/* Runs proceed once the person has agreed to discard unsaved changes, when there are any. */
static void guarded(Workbench *bench, const char *verb, Proceed proceed, gboolean closing) {
    if (bench->busy) return;
    if (closing && !trusted(bench)) {
        JSCValue *value = bench->last ? member(bench, bench->last, "dirty") : NULL;
        const gboolean dirty = truthy(bench, value);
        if (value) g_object_unref(value);
        if (!dirty || confirm_discard(bench, verb)) proceed(bench);
        return;
    }
    bench->busy = TRUE;
    Guard *guard = g_new0(Guard, 1);
    guard->verb = verb;
    guard->proceed = proceed;
    guard->closing = closing;
    ask(bench, "state", NULL, checked, guard, FALSE);
}

static char *choose(Workbench *bench, const char *title, gboolean saving, const char *name, const char *pattern,
                    const char *folder) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(title, bench->window,
                                                    saving ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "Cancel", GTK_RESPONSE_CANCEL, saving ? "Save" : "Open",
                                                    GTK_RESPONSE_ACCEPT, NULL);
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_file_chooser_set_local_only(chooser, TRUE);
    if (folder && g_file_test(folder, G_FILE_TEST_IS_DIR)) gtk_file_chooser_set_current_folder(chooser, folder);
    if (saving) {
        gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
        char *base = g_path_get_basename(name && *name ? name : "design");
        gtk_file_chooser_set_current_name(chooser, strcmp(base, "/") == 0 || strcmp(base, ".") == 0 ? "design" : base);
        g_free(base);
    }
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, strcmp(pattern, "*.json") == 0 ? "Project files (*.json)" : pattern);
    gtk_file_filter_add_pattern(filter, pattern);
    gtk_file_chooser_add_filter(chooser, filter);
    if (strcmp(pattern, "*") != 0) {
        GtkFileFilter *other = gtk_file_filter_new();
        gtk_file_filter_set_name(other, "All files");
        gtk_file_filter_add_pattern(other, "*");
        gtk_file_chooser_add_filter(chooser, other);
    }
    char *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) path = gtk_file_chooser_get_filename(chooser);
    gtk_widget_destroy(dialog);
    return path;
}

gboolean atomic_write(const char *path, const char *content, gsize length, GError **error) {
    char *directory = g_path_get_dirname(path);
    // Beside the file, on its file system, so the old file stays whole until the new one replaces it.
    char *temporary = g_build_filename(directory, ".kerfkite-XXXXXX", NULL);
    g_free(directory);
    struct stat old;
    const gboolean existed = stat(path, &old) == 0;
    mode_t mode;
    if (existed) {
        mode = old.st_mode & 07777;
    } else {
        const mode_t mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }
    int failure = 0;
    const int fd = g_mkstemp_full(temporary, O_WRONLY, 0600);
    if (fd < 0) {
        failure = errno;
    } else {
        for (const char *at = content; length > 0 && !failure;) {
            const ssize_t written = write(fd, at, length);
            if (written < 0 && errno != EINTR) failure = errno;
            if (written > 0) {
                at += written;
                length -= (gsize)written;
            }
        }
        if (!failure && fsync(fd) != 0) failure = errno;
        if (!failure && fchmod(fd, mode) != 0) failure = errno;
        if (close(fd) != 0 && !failure) failure = errno;
        if (!failure && rename(temporary, path) != 0) failure = errno;
        if (failure) unlink(temporary);
    }
    if (failure)
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(failure), "Could not save %s: %s", path,
                    g_strerror(failure));
    g_free(temporary);
    return !failure;
}

static void run_new(Workbench *bench) { workbench_command(bench, "new", NULL, NULL, NULL); }

void workbench_new_project(Workbench *bench) { guarded(bench, "Discard and create", run_new, FALSE); }

/* The text of a project file, or why it cannot be opened (for g_free). */
static char *read_project(const char *path, char **content) {
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) return g_strdup("Choose a regular project file.");
    FILE *file = fopen(path, "rb");
    if (!file) return g_strdup_printf("Could not open %s: %s", path, g_strerror(errno));
    char *raw = g_malloc(PROJECT_LIMIT + 2);
    const size_t size = fread(raw, 1, PROJECT_LIMIT + 1, file);
    const gboolean failed = ferror(file) != 0;
    fclose(file);
    char *problem = NULL;
    const char *start = raw;
    size_t length = size;
    if (length >= 3 && memcmp(raw, "\xef\xbb\xbf", 3) == 0) {
        start += 3;
        length -= 3;
    }
    if (failed) problem = g_strdup_printf("Could not read %s", path);
    else if (size > PROJECT_LIMIT) problem = g_strdup("Projects must be 4 MB or smaller.");
    else if (!g_utf8_validate(start, (gssize)length, NULL)) problem = g_strdup("The project file is not UTF-8 text.");
    else *content = g_strndup(start, length);
    g_free(raw);
    return problem;
}

static void opened(Workbench *bench, JSCValue *state, const char *error, gpointer data) {
    (void)error;
    g_free(bench->path);
    bench->path = g_strdup(data);
    bench->document = member_number(bench, state, "documentId", NAN);
    bench->has_document = TRUE;
}

static void browse(Workbench *bench) {
    char *folder = bench->path ? g_path_get_dirname(bench->path) : NULL;
    char *path = choose(bench, "Open project", FALSE, "", "*.json", folder);
    g_free(folder);
    if (!path) return;
    char *content = NULL;
    char *problem = read_project(path, &content);
    if (problem) {
        bench->report(bench, problem);
        g_free(problem);
        g_free(path);
        return;
    }
    JSCValue *payload = payload_new(bench);
    payload_string(bench, payload, "content", content);
    command_with(bench, "open", payload, opened, path, g_free);
    g_object_unref(payload);
    g_free(content);
}

void workbench_open(Workbench *bench) { guarded(bench, "Discard and open", browse, FALSE); }

static void write_project(Workbench *bench, JSCValue *project, const char *error, gpointer data) {
    (void)error;
    const gboolean save_as = GPOINTER_TO_INT(data);
    const double document = member_number(bench, project, "documentId", NAN);
    char *path = !save_as && bench->path && bench->has_document && bench->document == document ? g_strdup(bench->path)
                                                                                             : NULL;
    if (!path) {
        char *name = member_text(bench, project, "name");
        char *folder = bench->path ? g_path_get_dirname(bench->path) : NULL;
        path = choose(bench, "Save project", TRUE, name ? name : "", "*.json", folder);
        g_free(folder);
        g_free(name);
    }
    if (!path) return;
    char *content = member_text(bench, project, "content");
    GError *failure = NULL;
    if (!content) {
        bench->report(bench, "The editor gave no project to save.");
    } else if (!atomic_write(path, content, strlen(content), &failure)) {
        bench->report(bench, failure->message);
        g_error_free(failure);
    } else {
        g_free(bench->path);
        bench->path = g_strdup(path);
        bench->document = document;
        bench->has_document = TRUE;
        JSCValue *payload = payload_new(bench);
        payload_string(bench, payload, "content", content);
        workbench_command(bench, "saved", payload, NULL, NULL);
        g_object_unref(payload);
    }
    g_free(content);
    g_free(path);
}

void workbench_save(Workbench *bench, gboolean save_as) {
    workbench_command(bench, "save", NULL, write_project, GINT_TO_POINTER(save_as));
}

static void write_program(Workbench *bench, JSCValue *program, const char *error, gpointer data) {
    (void)error;
    (void)data;
    char *name = member_text(bench, program, "name");
    char *path = choose(bench, "Export G-code", TRUE, name ? name : "", "*", NULL);
    g_free(name);
    if (!path) return;
    char *content = member_text(bench, program, "content");
    GError *failure = NULL;
    if (!content) {
        bench->report(bench, "The editor gave no program to export.");
    } else if (!atomic_write(path, content, strlen(content), &failure)) {
        bench->report(bench, failure->message);
        g_error_free(failure);
    }
    g_free(content);
    g_free(path);
}

void workbench_export(Workbench *bench) { workbench_command(bench, "export", NULL, write_program, NULL); }

/* ---- the web views ------------------------------------------------------------------------------------------ */

static const char *uri_of_action(Workbench *bench, WebKitNavigationAction *action) {
    const char *uri = bench->api->uri_request_get_uri(bench->api->navigation_action_get_request(action));
    return uri ? uri : "";
}

static gboolean decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision, int type, gpointer user) {
    Workbench *bench = user;
    const WebKitApi *api = bench->api;
    if (type != KK_POLICY_NAVIGATION_ACTION && type != KK_POLICY_NEW_WINDOW_ACTION) return FALSE;
    const char *uri = uri_of_action(bench, api->navigation_policy_decision_get_navigation_action(decision));
    // The editor exports vectors, meshes and reports through Blob downloads: take them as downloads, so these
    // documents never replace the workbench.
    char *blob = g_strconcat("blob:", bench->url, NULL);
    const gboolean document = g_str_has_prefix(uri, blob) || g_str_has_prefix(uri, "data:");
    g_free(blob);
    if (type == KK_POLICY_NAVIGATION_ACTION && document) {
        api->policy_decision_download(decision);
        return TRUE;
    }
    // New views never receive native commands: they may show only Studio's own pages and its print window.
    const gboolean allowed = type == KK_POLICY_NEW_WINDOW_ACTION || view != bench->view ? auxiliary_uri(bench, uri)
                                                                                       : strcmp(uri, bench->url) == 0;
    if (!allowed) {
        api->policy_decision_ignore(decision);
        return TRUE;
    }
    return FALSE;
}

static void popup_gone(GtkWidget *window, gpointer user) {
    Workbench *bench = user;
    g_ptr_array_remove(bench->popups, window);
}

static GtkWidget *create_view(WebKitWebView *parent, WebKitNavigationAction *action, gpointer user) {
    Workbench *bench = user;
    if (!auxiliary_uri(bench, uri_of_action(bench, action))) return NULL;
    GtkWidget *child = bench->api->web_view_new_with_related_view(parent);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "KerfKite Studio");
    gtk_window_set_transient_for(GTK_WINDOW(window), bench->window);
    gtk_window_set_default_size(GTK_WINDOW(window), 950, 700);
    gtk_container_add(GTK_CONTAINER(window), child);
    g_signal_connect(child, "decide-policy", G_CALLBACK(decide_policy), bench);
    g_signal_connect_swapped(child, "ready-to-show", G_CALLBACK(gtk_widget_show_all), window);
    g_signal_connect_swapped(child, "close", G_CALLBACK(gtk_widget_destroy), window);
    g_ptr_array_add(bench->popups, window);
    g_signal_connect(window, "destroy", G_CALLBACK(popup_gone), bench);
    return child;
}

static void load_changed(WebKitWebView *view, int event, gpointer user) {
    Workbench *bench = user;
    if (event == KK_LOAD_STARTED) {
        bench->generation += 1;
        bench->loaded = FALSE;
        g_clear_pointer(&bench->path, g_free);
        bench->has_document = FALSE;
    } else if (event == KK_LOAD_FINISHED) {
        const char *uri = bench->api->web_view_get_uri(view);
        bench->loaded = uri && strcmp(uri, bench->url) == 0;
        refresh(bench);
    }
}

static void terminated(WebKitWebView *view, int reason, gpointer user) {
    (void)view;
    (void)reason;
    Workbench *bench = user;
    bench->loaded = FALSE;
    bench->generation += 1;
    gtk_label_set_text(GTK_LABEL(bench->status), "The workspace stopped responding. Reopen Studio to recover.");
}

static gboolean decide_destination(WebKitDownload *download, const char *suggested, gpointer user) {
    Workbench *bench = user;
    char *path = choose(bench, "Save download", TRUE, suggested, "*", NULL);
    char *uri = path ? g_filename_to_uri(path, NULL, NULL) : NULL;
    if (uri) {
        bench->api->download_set_allow_overwrite(download, TRUE);
        bench->api->download_set_destination(download, uri);
    } else {
        bench->api->download_cancel(download);
    }
    g_free(uri);
    g_free(path);
    return TRUE;
}

static void download_failed(WebKitDownload *download, GError *error, gpointer user) {
    (void)download;
    Workbench *bench = user;
    if (!bench->destroyed &&
        !g_error_matches(error, bench->api->download_error_quark(), KK_DOWNLOAD_ERROR_CANCELLED_BY_USER))
        bench->report(bench, error->message);
}

static void download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user) {
    (void)context;
    Workbench *bench = user;
    const WebKitApi *api = bench->api;
    GtkWidget *owner = (GtkWidget *)api->download_get_web_view(download);
    gboolean ours = owner && owner == (GtkWidget *)bench->view;
    for (guint i = 0; i < bench->popups->len && !ours; ++i)
        ours = owner && gtk_bin_get_child(GTK_BIN(g_ptr_array_index(bench->popups, i))) == owner;
    const char *uri = api->uri_request_get_uri(api->download_get_request(download));
    char *blob = g_strconcat("blob:", bench->url, NULL);
    const gboolean wanted =
        uri && (auxiliary_uri(bench, uri) || g_str_has_prefix(uri, blob) || g_str_has_prefix(uri, "data:"));
    g_free(blob);
    if (!ours || !wanted) {
        api->download_cancel(download);
        return;
    }
    g_signal_connect(download, "decide-destination", G_CALLBACK(decide_destination), bench);
    g_signal_connect(download, "failed", G_CALLBACK(download_failed), bench);
}

/* ---- the window --------------------------------------------------------------------------------------------- */

typedef void (*Action)(Workbench *bench, const char *argument);

typedef struct {
    Workbench *bench;
    Action run;
    const char *argument;
} Item;

static void activated(GtkWidget *widget, gpointer user) {
    (void)widget;
    Item *item = user;
    item->run(item->bench, item->argument);
}

static Item *item_for(Workbench *bench, Action run, const char *argument) {
    Item *item = g_new0(Item, 1);
    item->bench = bench;
    item->run = run;
    item->argument = argument;
    return item;
}

static void act_new(Workbench *bench, const char *argument) {
    (void)argument;
    workbench_new_project(bench);
}

static void act_open(Workbench *bench, const char *argument) {
    (void)argument;
    workbench_open(bench);
}

static void act_save(Workbench *bench, const char *argument) { workbench_save(bench, argument != NULL); }

static void act_export(Workbench *bench, const char *argument) {
    (void)argument;
    workbench_export(bench);
}

static void act_close(Workbench *bench, const char *argument) {
    (void)argument;
    gtk_window_close(bench->window);
}

static void act_command(Workbench *bench, const char *action) { workbench_command(bench, action, NULL, NULL, NULL); }

static void act_authoring(Workbench *bench, const char *action) {
    if (bench->busy) return;
    gtk_widget_grab_focus(GTK_WIDGET(bench->view));
    workbench_command(bench, action, NULL, NULL, NULL);
}

static void act_tab(Workbench *bench, const char *name) {
    JSCValue *payload = payload_new(bench);
    payload_string(bench, payload, "name", name);
    workbench_command(bench, "tab", payload, NULL, NULL);
    g_object_unref(payload);
}

static void act_inspector(Workbench *bench, const char *argument) {
    (void)argument;
    gtk_widget_set_visible(bench->inspector, !gtk_widget_get_visible(bench->inspector));
}

static void add_item(Workbench *bench, GtkWidget *menu, GtkAccelGroup *accels, const char *label, const char *key,
                     Action run, const char *argument) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect_data(item, "activate", G_CALLBACK(activated), item_for(bench, run, argument),
                          (GClosureNotify)(void (*)(void))g_free, 0);
    if (key) {
        guint keyval = 0;
        GdkModifierType modifiers = 0;
        gtk_accelerator_parse(key, &keyval, &modifiers);
        gtk_widget_add_accelerator(item, "activate", accels, keyval, modifiers, GTK_ACCEL_VISIBLE);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static GtkWidget *add_menu(GtkWidget *bar, const char *name) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(name);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static GtkWidget *menu_bar(Workbench *bench, GtkAccelGroup *accels) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = add_menu(bar, "_File");
    add_item(bench, file, accels, "_New project", "<Ctrl>n", act_new, NULL);
    add_item(bench, file, accels, "_Open project\xe2\x80\xa6", "<Ctrl>o", act_open, NULL);
    add_item(bench, file, accels, "_Save project", "<Ctrl>s", act_save, NULL);
    add_item(bench, file, accels, "Save _as\xe2\x80\xa6", "<Ctrl><Shift>s", act_save, "as");
    add_item(bench, file, accels, "_Export G-code\xe2\x80\xa6", NULL, act_export, NULL);
    add_item(bench, file, accels, "_Close", "<Ctrl>w", act_close, NULL);
    GtkWidget *edit = add_menu(bar, "_Edit");
    add_item(bench, edit, accels, "_Undo", "<Ctrl>z", act_command, "undo");
    add_item(bench, edit, accels, "_Redo", "<Ctrl><Shift>z", act_command, "redo");
    GtkWidget *workspace = add_menu(bar, "_Workspace");
    add_item(bench, workspace, accels, "CAD _command\xe2\x80\xa6", "<Ctrl><Shift>k", act_authoring, "command-line");
    add_item(bench, workspace, accels, "CAD _script\xe2\x80\xa6", NULL, act_authoring, "script-editor");
    for (size_t i = 0; i < G_N_ELEMENTS(TABS); ++i) add_item(bench, workspace, accels, TABS[i], NULL, act_tab, TABS[i]);
    return bar;
}

static GtkWidget *toolbar(Workbench *bench) {
    static const struct {
        const char *icon, *label;
        Action run;
        const char *argument;
    } buttons[] = {
        {"document-open-symbolic", "Open", act_open, NULL},
        {"document-save-symbolic", "Save", act_save, NULL},
        {"edit-undo-symbolic", "Undo", act_command, "undo"},
        {"edit-redo-symbolic", "Redo", act_command, "redo"},
        {"media-playback-start-symbolic", "Preview", act_command, "preview"},
        {"view-dual-symbolic", "Inspector", act_inspector, NULL},
    };
    GtkWidget *bar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_BOTH_HORIZ);
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); ++i) {
        GtkToolItem *button = gtk_tool_button_new(NULL, buttons[i].label);
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(button), buttons[i].icon);
        gtk_tool_item_set_tooltip_text(button, buttons[i].label);
        gtk_tool_item_set_is_important(button, TRUE);
        g_signal_connect_data(button, "clicked", G_CALLBACK(activated),
                              item_for(bench, buttons[i].run, buttons[i].argument),
                              (GClosureNotify)(void (*)(void))g_free, 0);
        gtk_toolbar_insert(GTK_TOOLBAR(bar), button, -1);
    }
    return bar;
}

static void heading(GtkWidget *panel, const char *title) {
    GtkWidget *label = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(panel), label, FALSE, FALSE, 0);
}

static void fields(GtkWidget *panel, const char *const *labels, const double *values, GtkSpinButton **spins, size_t count) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_box_pack_start(GTK_BOX(panel), grid, FALSE, FALSE, 0);
    for (size_t row = 0; row < count; ++row) {
        GtkWidget *spin = gtk_spin_button_new_with_range(0, 100000, 0.1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 3);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), values[row]);
        gtk_widget_set_hexpand(spin, TRUE);
        char *text = g_strdup_printf("%s (mm)", labels[row]);
        GtkWidget *caption = gtk_label_new(text);
        g_free(text);
        gtk_label_set_xalign(GTK_LABEL(caption), 0);
        gtk_grid_attach(GTK_GRID(grid), caption, 0, (gint)row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), spin, 1, (gint)row, 1, 1);
        spins[row] = GTK_SPIN_BUTTON(spin);
    }
}

static void add_shape(GtkButton *button, gpointer user) {
    (void)button;
    Workbench *bench = user;
    JSCValue *payload = payload_new(bench);
    const char *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(bench->kind));
    payload_string(bench, payload, "kind", kind ? kind : "rectangle");
    payload_number(bench, payload, "x", gtk_spin_button_get_value(bench->shape_x));
    payload_number(bench, payload, "y", gtk_spin_button_get_value(bench->shape_y));
    payload_number(bench, payload, "width", gtk_spin_button_get_value(bench->shape_w));
    payload_number(bench, payload, "height", gtk_spin_button_get_value(bench->shape_h));
    workbench_command(bench, "shape", payload, NULL, NULL);
    g_object_unref(payload);
}

static void move_selected(GtkButton *button, gpointer user) {
    (void)button;
    Workbench *bench = user;
    JSCValue *payload = payload_new(bench);
    payload_number(bench, payload, "x", gtk_spin_button_get_value(bench->move_x));
    payload_number(bench, payload, "y", gtk_spin_button_get_value(bench->move_y));
    workbench_command(bench, "move", payload, NULL, NULL);
    g_object_unref(payload);
}

static void select_object(GtkTreeSelection *selection, gpointer user) {
    Workbench *bench = user;
    GtkTreeModel *model = NULL;
    GtkTreeIter row;
    if (bench->syncing || !gtk_tree_selection_get_selected(selection, &model, &row)) return;
    gint index = -1;
    gtk_tree_model_get(model, &row, 0, &index, -1);
    JSCValue *payload = payload_new(bench);
    payload_number(bench, payload, "index", index);
    workbench_command(bench, "select", payload, NULL, NULL);
    g_object_unref(payload);
}

static GtkWidget *button_in(GtkWidget *panel, const char *label, GCallback clicked, Workbench *bench) {
    GtkWidget *button = gtk_button_new_with_label(label);
    g_signal_connect(button, "clicked", clicked, bench);
    gtk_box_pack_start(GTK_BOX(panel), button, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *build_inspector(Workbench *bench) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    g_object_set(panel, "margin", 10, NULL);
    heading(panel, "Objects");
    bench->store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(bench->store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree), 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), gtk_tree_view_column_new_with_attributes(
                                                         "Object", gtk_cell_renderer_text_new(), "text", 1, NULL));
    bench->selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    g_signal_connect(bench->selection, "changed", G_CALLBACK(select_object), bench);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroller), GTK_SHADOW_IN);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 80);
    gtk_container_add(GTK_CONTAINER(scroller), tree);
    gtk_box_pack_start(GTK_BOX(panel), scroller, TRUE, TRUE, 0);

    heading(panel, "Selection");
    bench->properties = gtk_label_new("Select an object");
    gtk_label_set_line_wrap(GTK_LABEL(bench->properties), TRUE);
    gtk_label_set_xalign(GTK_LABEL(bench->properties), 0);
    gtk_box_pack_start(GTK_BOX(panel), bench->properties, FALSE, FALSE, 0);
    static const char *const position[] = {"X", "Y"};
    static const double origin[] = {0, 0};
    GtkSpinButton *moves[2];
    fields(panel, position, origin, moves, 2);
    bench->move_x = moves[0];
    bench->move_y = moves[1];
    bench->move_button = button_in(panel, "Move selection", G_CALLBACK(move_selected), bench);

    heading(panel, "Add a shape");
    bench->kind = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append(bench->kind, "rectangle", "Rectangle");
    gtk_combo_box_text_append(bench->kind, "ellipse", "Ellipse");
    gtk_combo_box_text_append(bench->kind, "line", "Line");
    gtk_combo_box_set_active(GTK_COMBO_BOX(bench->kind), 0);
    gtk_box_pack_start(GTK_BOX(panel), GTK_WIDGET(bench->kind), FALSE, FALSE, 0);
    static const char *const shape[] = {"X", "Y", "Width", "Height"};
    static const double initial[] = {10, 10, 40, 20};
    GtkSpinButton *sizes[4];
    fields(panel, shape, initial, sizes, 4);
    bench->shape_x = sizes[0];
    bench->shape_y = sizes[1];
    bench->shape_w = sizes[2];
    bench->shape_h = sizes[3];
    bench->add_button = button_in(panel, "Add shape", G_CALLBACK(add_shape), bench);
    return panel;
}

static void destroy_window(Workbench *bench) { gtk_widget_destroy(GTK_WIDGET(bench->window)); }

static gboolean close_requested(GtkWidget *widget, GdkEvent *event, gpointer user) {
    (void)widget;
    (void)event;
    guarded(user, "Discard and close", destroy_window, TRUE);
    return TRUE;
}

static void on_destroy(GtkWidget *widget, gpointer user) {
    (void)widget;
    Workbench *bench = user;
    bench->destroyed = TRUE;
    if (bench->timer) g_source_remove(bench->timer);
    bench->timer = 0;
    // Each window leaves the list as it goes: destroy them from a copy.
    GPtrArray *windows = g_ptr_array_sized_new(bench->popups->len);
    for (guint i = 0; i < bench->popups->len; ++i) g_ptr_array_add(windows, g_ptr_array_index(bench->popups, i));
    for (guint i = 0; i < windows->len; ++i) gtk_widget_destroy(g_ptr_array_index(windows, i));
    g_ptr_array_free(windows, TRUE);
    if (gtk_main_level() > 0) gtk_main_quit();
}

Workbench *workbench_new(const WebKitApi *api, const char *url) {
    Workbench *bench = g_new0(Workbench, 1);
    bench->api = api;
    bench->url = g_strdup(url);
    bench->netloc = g_strndup(url + 7, strlen(url) - 8); /* between http:// and the final / */
    bench->popups = g_ptr_array_new();
    bench->report = show_error;
    bench->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(bench->window, "KerfKite Studio");
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = display ? gdk_display_get_primary_monitor(display) : NULL;
    if (!monitor && display) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle area = {0, 0, 1360, 850};
    if (monitor) gdk_monitor_get_workarea(monitor, &area);
    gtk_window_set_default_size(bench->window, MIN(1360, area.width), MIN(850, area.height));
    gtk_window_set_position(bench->window, GTK_WIN_POS_CENTER);
    GtkAccelGroup *accels = gtk_accel_group_new();
    gtk_window_add_accel_group(bench->window, accels);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(bench->window), outer);
    bench->menu_bar = menu_bar(bench, accels);
    gtk_box_pack_start(GTK_BOX(outer), bench->menu_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), toolbar(bench), FALSE, FALSE, 0);
    GtkWidget *split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outer), split, TRUE, TRUE, 0);
    bench->inspector = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bench->inspector), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(bench->inspector), 218);
    GtkWidget *panel = build_inspector(bench);
    gtk_container_add(GTK_CONTAINER(bench->inspector), panel);
    if (area.width < 1024) {
        // On a small screen the inspector starts hidden; the toolbar shows it.
        gtk_widget_set_no_show_all(bench->inspector, TRUE);
        gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(bench->inspector)));
    }
    gtk_paned_pack1(GTK_PANED(split), bench->inspector, FALSE, FALSE);
    bench->view = (WebKitWebView *)api->web_view_new();
    // Only once WebKit has started: a JavaScriptCore context made first sets up the main loop WebKit then waits
    // on in vain, and no page ever loads.
    bench->json = api->jsc_context_new();
    g_signal_connect(bench->view, "decide-policy", G_CALLBACK(decide_policy), bench);
    g_signal_connect(bench->view, "create", G_CALLBACK(create_view), bench);
    g_signal_connect(bench->view, "load-changed", G_CALLBACK(load_changed), bench);
    g_signal_connect(bench->view, "web-process-terminated", G_CALLBACK(terminated), bench);
    g_signal_connect(api->web_view_get_context(bench->view), "download-started", G_CALLBACK(download_started), bench);
    gtk_paned_pack2(GTK_PANED(split), GTK_WIDGET(bench->view), TRUE, TRUE);
    bench->status = gtk_label_new("Opening Studio\xe2\x80\xa6");
    gtk_label_set_xalign(GTK_LABEL(bench->status), 0);
    gtk_widget_set_margin_start(bench->status, 8);
    gtk_widget_set_margin_end(bench->status, 8);
    gtk_box_pack_end(GTK_BOX(outer), bench->status, FALSE, FALSE, 3);
    g_signal_connect(bench->window, "delete-event", G_CALLBACK(close_requested), bench);
    g_signal_connect(bench->window, "destroy", G_CALLBACK(on_destroy), bench);
    api->web_view_load_uri(bench->view, bench->url);
    bench->timer = g_timeout_add(800, refresh, bench);
    return bench;
}
