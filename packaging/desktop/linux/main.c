/* kerfkite-native: the Linux desktop window of KerfKite Studio. The launcher starts it with the address of the
 * Studio it runs: kerfkite-native http://127.0.0.1:PORT/
 *
 * kerfkite-native --check reports whether WebKitGTK can be loaded, without opening a window. */
#include <stdio.h>
#include <string.h>

#include "workbench.h"

int main(int argc, char **argv) {
    char *reason = NULL;
    if (argc == 2 && strcmp(argv[1], "--check") == 0) {
        const WebKitApi *api = webkit_api(&reason);
        if (!api) {
            fprintf(stderr, "kerfkite-native: %s\n", reason);
            return 1;
        }
        printf("kerfkite-native: %s\n", api->library);
        return 0;
    }
    if (argc != 2) {
        fputs("Usage: kerfkite-native http://127.0.0.1:PORT/\n", stderr);
        return 1;
    }
    char *url = studio_url(argv[1]);
    if (!url) {
        fputs("kerfkite-native: The desktop workspace requires a local Studio URL.\n", stderr);
        return 1;
    }
    if (!gtk_init_check(NULL, NULL)) {
        fputs("kerfkite-native: no graphical display is available.\n", stderr);
        return 1;
    }
    const WebKitApi *api = webkit_api(&reason);
    if (!api) {
        fprintf(stderr, "kerfkite-native: %s\n", reason);
        return 1;
    }
    Workbench *bench = workbench_new(api, url);
    gtk_widget_show_all(GTK_WIDGET(bench->window));
    gtk_main();
    return 0;
}
