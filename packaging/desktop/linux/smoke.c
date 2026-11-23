/* Exercises the real GTK/WebKitGTK workbench against a running loopback Studio simulator.
 *
 * Run under a desktop session or xvfb-run, with the simulator's address as the only argument:
 *   kerfkite-native-smoke http://127.0.0.1:PORT/
 * It answers the window's own dialogs, as a person would, and exits 0 when every step passed. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "workbench.h"

static Workbench *bench;
static GPtrArray *errors; /* what the window reported */

static void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("FAILED: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    if (bench && !bench->destroyed) {
        const char *uri = bench->api->web_view_get_uri(bench->view);
        fprintf(stderr, "  page %s (%s), status: %s\n", uri ? uri : "none", bench->loaded ? "loaded" : "not loaded",
                gtk_label_get_text(GTK_LABEL(bench->status)));
    }
    exit(1);
}

/* Runs the main loop until the condition holds, as the window would between a person's actions. */
#define UNTIL(condition, label)                                                                                   \
    do {                                                                                                         \
        const gint64 start_ = g_get_monotonic_time();                                                            \
        while (!(condition)) {                                                                                   \
            if (g_get_monotonic_time() - start_ > 25 * G_USEC_PER_SEC) fail("Timed out waiting for %s", label);  \
            g_main_context_iteration(NULL, FALSE);                                                               \
            g_usleep(3000);                                                                                      \
        }                                                                                                        \
    } while (0)

#define CHECK(condition, label)                                                                                   \
    do {                                                                                                         \
        if (!(condition)) fail("%s", label);                                                                     \
    } while (0)

static void load_event(WebKitWebView *view, int event, gpointer user) {
    (void)user;
    const char *uri = bench->api->web_view_get_uri(view);
    printf("load event %d: %s\n", event, uri ? uri : "none");
    fflush(stdout);
}

static gboolean load_failed(WebKitWebView *view, int event, const char *uri, GError *error, gpointer user) {
    (void)view;
    (void)user;
    printf("load failed (%d) %s: %s\n", event, uri ? uri : "none", error ? error->message : "");
    fflush(stdout);
    return FALSE;
}

static void recorded(Workbench *workbench, const char *message) {
    (void)workbench;
    g_ptr_array_add(errors, g_strdup(message));
    printf("GTK error: %s\n", message);
    fflush(stdout);
}

/* ---- answering dialogs -------------------------------------------------------------------------------------- */

typedef void (*Configure)(GtkWidget *dialog, gpointer data);

typedef struct {
    GType kind;
    gint response;
    Configure configure;
    gpointer data;
    GtkWidget *dialog;
    gboolean finished;
} Respond;

static gboolean answer(gpointer user) {
    Respond *respond = user;
    gtk_dialog_response(GTK_DIALOG(respond->dialog), respond->response);
    g_object_unref(respond->dialog);
    respond->finished = TRUE;
    return G_SOURCE_REMOVE;
}

static gboolean find_dialog(gpointer user) {
    Respond *respond = user;
    GList *windows = gtk_window_list_toplevels();
    for (GList *at = windows; at; at = at->next) {
        GtkWidget *window = at->data;
        if (G_TYPE_CHECK_INSTANCE_TYPE(window, respond->kind) && gtk_widget_get_visible(window)) {
            if (respond->configure) respond->configure(window, respond->data);
            respond->dialog = g_object_ref(window);
            // GTK's chooser checks file names asynchronously before it accepts the dialog.
            g_timeout_add(350, answer, respond);
            g_list_free(windows);
            return G_SOURCE_REMOVE;
        }
    }
    g_list_free(windows);
    return G_SOURCE_CONTINUE;
}

/* Answers the next visible dialog of a kind; finished becomes TRUE once it has. */
static Respond *respond_to(GType kind, gint response, Configure configure, gpointer data) {
    Respond *respond = g_new0(Respond, 1);
    respond->kind = kind;
    respond->response = response;
    respond->configure = configure;
    respond->data = data;
    g_timeout_add(15, find_dialog, respond);
    return respond;
}

static void save_here(GtkWidget *dialog, gpointer data) {
    char *folder = g_path_get_dirname(data), *name = g_path_get_basename(data);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), name);
    g_free(name);
    g_free(folder);
}

static void open_this(GtkWidget *dialog, gpointer data) { gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), data); }

static Respond *save_to(const char *path) {
    return respond_to(GTK_TYPE_FILE_CHOOSER_DIALOG, GTK_RESPONSE_ACCEPT, save_here, (gpointer)path);
}

/* ---- the page ----------------------------------------------------------------------------------------------- */

static void click_web(const char *selector) {
    JSCValue *text = bench->api->jsc_value_new_string(bench->json, selector);
    char *quoted = bench->api->jsc_value_to_json(text, 0);
    char *script = g_strconcat("document.querySelector(", quoted, ").click()", NULL);
    workbench_evaluate(bench, bench->view, script, NULL, NULL);
    g_free(script);
    g_free(quoted);
    g_object_unref(text);
}

typedef struct {
    gboolean done, value;
} Truth;

static void evaluated(GObject *source, GAsyncResult *result, gpointer user) {
    Truth *truth = user;
    JSCValue *value = workbench_evaluate_finish(bench, (WebKitWebView *)source, result, NULL);
    truth->value = value && bench->api->jsc_value_to_boolean(value);
    if (value) g_object_unref(value);
    truth->done = TRUE;
}

static void check_web(const char *expression, const char *label) {
    Truth truth = {FALSE, FALSE};
    workbench_evaluate(bench, bench->view, expression, evaluated, &truth);
    UNTIL(truth.done, label);
    CHECK(truth.value, label);
}

/* ---- the workbench's state ---------------------------------------------------------------------------------- */

static int objects_shown(void) { return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(bench->store), NULL); }

static gboolean state_has(const char *name) {
    return bench->last && bench->api->jsc_value_object_has_property(bench->last, name);
}

static double state_number(const char *name) { return member_number(bench, bench->last, name, NAN); }

static gboolean state_flag(const char *name) {
    JSCValue *value = bench->last ? member(bench, bench->last, name) : NULL;
    const gboolean flag = truthy(bench, value);
    if (value) g_object_unref(value);
    return flag;
}

static gboolean has_selection(void) { return state_flag("selection"); }

static double selection_number(const char *name) {
    JSCValue *selection = bench->last ? member(bench, bench->last, "selection") : NULL;
    const double number = member_number(bench, selection, name, NAN);
    if (selection) g_object_unref(selection);
    return number;
}

static void select_row(int index) {
    GtkTreeModel *model = GTK_TREE_MODEL(bench->store);
    GtkTreeIter row;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &row); more; more = gtk_tree_model_iter_next(model, &row)) {
        gint shown = -1;
        gtk_tree_model_get(model, &row, 0, &shown, -1);
        if (shown == index) {
            gtk_tree_selection_select_iter(bench->selection, &row);
            return;
        }
    }
    fail("No row for object %d", index);
}

static char *plain(const char *label) {
    GString *out = g_string_new(NULL);
    for (const char *c = label; *c; ++c)
        if (*c != '_') g_string_append_c(out, *c);
    return g_string_free(out, FALSE);
}

/* The menu item of the Workspace menu with this label. */
static GtkWidget *workspace_entry(const char *label) {
    GtkWidget *found = NULL;
    GList *tops = gtk_container_get_children(GTK_CONTAINER(bench->menu_bar));
    for (GList *top = tops; top && !found; top = top->next) {
        char *name = plain(gtk_menu_item_get_label(top->data));
        if (strcmp(name, "Workspace") == 0) {
            GList *entries = gtk_container_get_children(GTK_CONTAINER(gtk_menu_item_get_submenu(top->data)));
            for (GList *entry = entries; entry && !found; entry = entry->next) {
                char *text = plain(gtk_menu_item_get_label(entry->data));
                if (strcmp(text, label) == 0) found = entry->data;
                g_free(text);
            }
            g_list_free(entries);
        }
        g_free(name);
    }
    g_list_free(tops);
    if (!found) fail("No Workspace menu entry %s", label);
    return found;
}

static gboolean help_window_open(void) {
    for (guint i = 0; i < bench->popups->len; ++i) {
        GtkWidget *view = gtk_bin_get_child(GTK_BIN(g_ptr_array_index(bench->popups, i)));
        const char *uri = view ? bench->api->web_view_get_uri((WebKitWebView *)view) : NULL;
        if (uri && g_str_has_suffix(uri, "/help.html")) return TRUE;
    }
    return FALSE;
}

static char *read_text(const char *path) {
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) fail("Could not read %s", path);
    return text;
}

static gboolean is_file(const char *path) { return g_file_test(path, G_FILE_TEST_IS_REGULAR); }

static gboolean nonempty(const char *path) {
    GStatBuf info;
    return g_stat(path, &info) == 0 && info.st_size > 0;
}

/* The JSON text of a project file's objects. */
static char *objects_of(const char *path) {
    char *text = read_text(path);
    JSCValue *project = bench->api->jsc_value_new_from_json(bench->json, text);
    JSCValue *objects = project ? member(bench, project, "objects") : NULL;
    char *json = objects ? bench->api->jsc_value_to_json(objects, 0) : NULL;
    if (!json) fail("%s holds no objects", path);
    if (objects) g_object_unref(objects);
    if (project) g_object_unref(project);
    g_free(text);
    return json;
}

static void destroy_popups(void) {
    while (bench->popups->len) gtk_widget_destroy(g_ptr_array_index(bench->popups, bench->popups->len - 1));
}

static void remove_tree(const char *directory) {
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (!dir) return;
    for (const char *name; (name = g_dir_read_name(dir));) {
        char *path = g_build_filename(directory, name, NULL);
        g_remove(path);
        g_free(path);
    }
    g_dir_close(dir);
    g_rmdir(directory);
}

int main(int argc, char **argv) {
    alarm(180); /* bounds GTK's nested modal loops as well as the waits */
    if (argc != 2) {
        fputs("Usage: kerfkite-native-smoke http://127.0.0.1:PORT/\n", stderr);
        return 2;
    }
    char *url = studio_url(argv[1]);
    if (!url) fail("Not a local Studio URL: %s", argv[1]);
    if (!gtk_init_check(NULL, NULL)) fail("No graphical display");
    char *reason = NULL;
    const WebKitApi *api = webkit_api(&reason);
    if (!api) fail("%s", reason);
    printf("WebKitGTK: %s\n", api->library);
    errors = g_ptr_array_new_with_free_func(g_free);
    bench = workbench_new(api, url);
    bench->report = recorded;
    g_signal_connect(bench->view, "load-changed", G_CALLBACK(load_event), NULL);
    g_signal_connect(bench->view, "load-failed", G_CALLBACK(load_failed), NULL);
    gtk_widget_show_all(GTK_WIDGET(bench->window));

    UNTIL(state_has("documentId"), "the simulator bridge");
    const int initial = objects_shown();
    const char *bad[] = {"https://127.0.0.1:18765/", "http://localhost:18765/", NULL};
    char *help = g_strconcat(url, "help.html", NULL);
    bad[2] = help;
    for (size_t i = 0; i < G_N_ELEMENTS(bad); ++i) {
        char *accepted = studio_url(bad[i]);
        if (accepted) fail("Accepted an invalid Studio URL: %s", bad[i]);
    }
    g_free(help);
    printf("Ready: %s\n", gtk_window_get_title(bench->window));
    fflush(stdout);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(bench->kind), "ellipse");
    gtk_button_clicked(GTK_BUTTON(bench->add_button));
    UNTIL(objects_shown() == initial + 1 && has_selection(), "native shape creation");
    const int ellipse = (int)state_number("selected");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(bench->kind), "rectangle");
    gtk_button_clicked(GTK_BUTTON(bench->add_button));
    UNTIL(objects_shown() == initial + 2, "native rectangle");
    select_row(ellipse);
    UNTIL(state_number("selected") == ellipse, "object selection");
    gtk_spin_button_set_value(bench->move_x, 34);
    gtk_spin_button_set_value(bench->move_y, 54);
    gtk_button_clicked(GTK_BUTTON(bench->move_button));
    UNTIL(selection_number("x") == 34 && selection_number("y") == 54, "native move");
    workbench_command(bench, "undo", NULL, NULL, NULL);
    UNTIL(!has_selection() || selection_number("x") != 34, "undo");
    workbench_command(bench, "redo", NULL, NULL, NULL);
    UNTIL(!bench->busy, "redo");
    puts("Shapes, selection, positioning and history passed");
    fflush(stdout);

    gtk_widget_grab_focus(GTK_WIDGET(bench->move_x));
    gtk_menu_item_activate(GTK_MENU_ITEM(workspace_entry("CAD command\xe2\x80\xa6")));
    UNTIL(!bench->busy, "command menu");
    check_web("document.activeElement.id === 'cad-command'", "keyboard focus in command line");
    gtk_menu_item_activate(GTK_MENU_ITEM(workspace_entry("CAD script\xe2\x80\xa6")));
    UNTIL(!bench->busy, "script menu");
    check_web("document.activeElement.id === 'cad-script-source' && document.getElementById('cad-script-dialog').open",
              "keyboard focus in script editor");
    check_web("(document.getElementById('cad-script-dialog').close(), true)", "close script editor");
    puts("Native CAD command and script menus passed");
    fflush(stdout);

    char *directory = g_dir_make_tmp("kerfkite-window-XXXXXX", NULL);
    if (!directory) fail("No temporary folder");
    char *project = g_build_filename(directory, "sample.cncstudio.json", NULL);
    char *gcode = g_build_filename(directory, "sample.nc", NULL);
    char *embedded = g_build_filename(directory, "portable.cncstudio.json", NULL);
    char *invalid = g_build_filename(directory, "invalid.json", NULL);

    save_to(project);
    workbench_save(bench, FALSE);
    UNTIL(is_file(project) && !bench->busy && !state_flag("dirty"), "save");
    char *original = read_text(project);
    CHECK(strstr(original, "\"pi-cnc-studio/1\"") != NULL, "the saved project's format");
    save_to(embedded);
    click_web("#projectDownload");
    UNTIL(is_file(embedded) && nonempty(embedded), "embedded project download");
    char *downloaded_objects = objects_of(embedded), *saved_objects = objects_of(project);
    CHECK(strcmp(downloaded_objects, saved_objects) == 0, "the portable download holds the saved objects");
    workbench_command(bench, "preview", NULL, NULL, NULL);
    UNTIL(!bench->busy, "asynchronous simulator preview");
    save_to(gcode);
    workbench_export(bench);
    UNTIL(g_file_test(gcode, G_FILE_TEST_EXISTS) && !bench->busy, "native G-code export");
    char *program = read_text(gcode);
    CHECK(strstr(program, "G1") != NULL, "the exported G-code moves");
    workbench_new_project(bench);
    UNTIL(objects_shown() == 0, "new project");
    respond_to(GTK_TYPE_FILE_CHOOSER_DIALOG, GTK_RESPONSE_ACCEPT, open_this, project);
    workbench_open(bench);
    UNTIL(objects_shown() == initial + 2 && !state_flag("dirty"), "open validated project");
    char *again = read_text(project);
    CHECK(strcmp(original, again) == 0, "opening leaves the project file as it was");
    if (!g_file_set_contents(invalid, "{", 1, NULL)) fail("Could not write %s", invalid);
    const double before_document = bench->document;
    respond_to(GTK_TYPE_FILE_CHOOSER_DIALOG, GTK_RESPONSE_ACCEPT, open_this, invalid);
    workbench_open(bench);
    UNTIL(errors->len && !bench->busy, "invalid project error");
    CHECK(errors->len == 1 && bench->document == before_document && bench->path && strcmp(bench->path, project) == 0,
          "an invalid project leaves the open document");
    g_ptr_array_set_size(errors, 0);
    Respond *printed = respond_to(GTK_TYPE_DIALOG, GTK_RESPONSE_CANCEL, NULL, NULL);
    click_web("#printDesign");
    UNTIL(printed->finished, "native print dialog");
    destroy_popups();
    puts("Native dialogs, save, new, open, preview, G-code, portable download and print passed");
    fflush(stdout);

    gtk_button_clicked(GTK_BUTTON(bench->add_button));
    UNTIL(state_flag("dirty"), "dirty state");
    respond_to(GTK_TYPE_MESSAGE_DIALOG, GTK_RESPONSE_CANCEL, NULL, NULL);
    workbench_new_project(bench);
    UNTIL(!bench->busy, "cancel dirty new");
    CHECK(objects_shown() == initial + 3, "cancelling New keeps the document");
    respond_to(GTK_TYPE_MESSAGE_DIALOG, GTK_RESPONSE_CANCEL, NULL, NULL);
    workbench_open(bench);
    UNTIL(!bench->busy, "cancel dirty open");
    CHECK(bench->path && strcmp(bench->path, project) == 0, "cancelling Open keeps the project file");
    Respond *kept = respond_to(GTK_TYPE_MESSAGE_DIALOG, GTK_RESPONSE_CANCEL, NULL, NULL);
    gtk_window_close(bench->window);
    UNTIL(kept->finished && !bench->busy, "cancel dirty close");
    CHECK(!bench->destroyed, "cancelling Close keeps the window");
    JSCValue *tab = payload_new(bench);
    payload_string(bench, tab, "name", "Help");
    workbench_command(bench, "tab", tab, NULL, NULL);
    g_object_unref(tab);
    UNTIL(!bench->busy, "help tab");
    // The real target=_blank help link: the help window never receives desktop commands.
    click_web("a[href=\"./help.html\"]");
    UNTIL(help_window_open(), "native help window");
    char *before = g_strdup(api->web_view_get_uri(bench->view));
    char *foreign = g_strconcat(url, "api/session", NULL);
    api->web_view_load_uri(bench->view, foreign);
    UNTIL(!api->web_view_is_loading(bench->view), "foreign document rejection");
    CHECK(g_strcmp0(api->web_view_get_uri(bench->view), before) == 0, "the workbench stays on the editor");
    CHECK(errors->len == 0, "no errors were reported");
    respond_to(GTK_TYPE_MESSAGE_DIALOG, GTK_RESPONSE_ACCEPT, NULL, NULL);
    gtk_window_close(bench->window);
    UNTIL(bench->destroyed, "dirty close");
    puts("Help window, navigation boundary, unsaved cancel and close passed");
    fflush(stdout);

    remove_tree(directory);
    return 0;
}
