/* The Linux Studio workbench: the browser editor in a WebKitGTK view, with native menus, a toolbar, an object
 * inspector and file dialogs. It sends the editor commands through window.KerfKiteDesktop.dispatch
 * (studio/static/desktop-workspace.js), as the Windows and macOS windows do. */
#ifndef KERFKITE_WORKBENCH_H
#define KERFKITE_WORKBENCH_H

#include "webkit_api.h"

#define PROJECT_LIMIT 4000000 /* bytes of a project file the window opens */

typedef struct Workbench Workbench;

/* The editor's answer to a command: its value (borrowed for the call), or why it failed. */
typedef void (*Answer)(Workbench *bench, JSCValue *value, const char *error, gpointer data);

struct Workbench {
    const WebKitApi *api;
    JSCContext *json; /* reads the editor's answers and writes the commands it is sent */
    GtkWindow *window;
    WebKitWebView *view;
    GtkWidget *menu_bar, *inspector, *properties, *status, *move_button, *add_button;
    GtkListStore *store; /* object index, name */
    GtkTreeSelection *selection;
    GtkComboBoxText *kind;
    GtkSpinButton *move_x, *move_y, *shape_x, *shape_y, *shape_w, *shape_h;
    char *url;    /* http://127.0.0.1:PORT/ */
    char *netloc; /* 127.0.0.1:PORT */
    gboolean busy, polling, loaded, destroyed, syncing;
    guint revision, generation, timer;
    JSCValue *last; /* the editor's last state; NULL before the first */
    char *path;     /* the file of the open project; NULL when none */
    gboolean has_document;
    double document;  /* the editor's number for the document that file holds */
    GPtrArray *popups; /* the help and print windows */
    /* Shows a failure: a dialog, unless the smoke test records it instead. */
    void (*report)(Workbench *bench, const char *message);
};

/* http://127.0.0.1:PORT/ for a local Studio address, else NULL. For g_free. */
char *studio_url(const char *url);

/* Whether a help or print window may show uri: a page of this Studio, or a blank one. */
gboolean auxiliary_uri(const Workbench *bench, const char *uri);

/* The workbench for a Studio address studio_url accepted. It lives until the process ends. */
Workbench *workbench_new(const WebKitApi *api, const char *url);

/* Sends the editor a command; success (which may be NULL) receives its value. A failure is reported. */
void workbench_command(Workbench *bench, const char *action, JSCValue *payload, Answer success, gpointer data);
void workbench_new_project(Workbench *bench);
void workbench_open(Workbench *bench);
void workbench_save(Workbench *bench, gboolean save_as);
void workbench_export(Workbench *bench);

/* A command's argument object, and its members. */
JSCValue *payload_new(Workbench *bench);
void payload_number(Workbench *bench, JSCValue *payload, const char *name, double value);
void payload_string(Workbench *bench, JSCValue *payload, const char *name, const char *text);

/* Members of the editor's answers: NULL, or a reference for g_object_unref. */
JSCValue *member(Workbench *bench, JSCValue *object, const char *name);
gboolean truthy(Workbench *bench, JSCValue *value);
/* A member's number (fallback when it is not a number) and text (for g_free; NULL when it is missing). */
double member_number(Workbench *bench, JSCValue *object, const char *name, double fallback);
char *member_text(Workbench *bench, JSCValue *object, const char *name);

/* Runs a script in the editor's page with WebKitGTK's own interface for its release. */
void workbench_evaluate(Workbench *bench, WebKitWebView *view, const char *script, GAsyncReadyCallback callback,
                        gpointer data);
JSCValue *workbench_evaluate_finish(Workbench *bench, WebKitWebView *view, GAsyncResult *result, GError **error);

/* Writes content to path through a file beside it, so a failed write leaves the old file. */
gboolean atomic_write(const char *path, const char *content, gsize length, GError **error);

#endif
