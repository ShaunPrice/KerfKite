"""Exercise the real GTK/WebKit workbench against a running loopback Studio simulator.

Run under a desktop session or xvfb-run. Pass the simulator URL as the sole argument. KERFKITE_NATIVE_HOST may point to an installed executable.
"""

from importlib.machinery import SourceFileLoader
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from types import ModuleType

loader = SourceFileLoader("kerfkite_gtk", os.environ.get("KERFKITE_NATIVE_HOST", str(Path(__file__).with_name("kerfkite-native"))))
ui = ModuleType(loader.name)
loader.exec_module(ui)
Gtk, GLib = ui.Gtk, ui.GLib


def until(test, label, seconds=25):
    start = time.monotonic()
    while not test():
        if time.monotonic() - start > seconds:
            raise AssertionError("Timed out waiting for " + label)
        GLib.MainContext.default().iteration(False)
        time.sleep(0.003)


def respond(kind, response, configure=None):
    # GTK's chooser validates filenames asynchronously before accepting the dialog.
    finished = []
    def find():
        for dialog in Gtk.Window.list_toplevels():
            if isinstance(dialog, kind) and dialog.get_visible():
                if configure:
                    configure(dialog)
                def answer():
                    dialog.response(response)
                    finished.append(True)
                    return False
                GLib.timeout_add(350, answer)
                return False
        return True
    GLib.timeout_add(15, find)
    return lambda: bool(finished)


def run(url):
    Gtk.init([])
    win = ui.Workbench(url)
    win.show_all()
    def failed(message):
        errors.append(str(message))
        print("GTK error:", message, flush=True)
    errors = []
    def click_web(button):
        script = "document.querySelector(" + json.dumps(button) + ").click()"
        if hasattr(win.view, "evaluate_javascript"):
            win.view.evaluate_javascript(script, -1, None, None, None, None, None)
        else:
            win.view.run_javascript(script, None, None, None)
    def check_web(expression, label):
        result = []
        def done(view, value, *_):
            if hasattr(view, "evaluate_javascript_finish"):
                result.append(view.evaluate_javascript_finish(value).to_boolean())
            else:
                result.append(view.run_javascript_finish(value).get_js_value().to_boolean())
        if hasattr(win.view, "evaluate_javascript"):
            win.view.evaluate_javascript(expression, -1, None, None, None, done, None)
        else:
            win.view.run_javascript(expression, None, done, None)
        until(lambda: bool(result), label)
        assert result[0], label
    win.error = failed
    until(lambda: "documentId" in win.last, "the simulator bridge")
    initial = len(win.store)
    for bad in ("https://127.0.0.1:18765/", "http://localhost:18765/", url + "help.html"):
        try:
            ui.studio_url(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("Accepted an invalid Studio URL")
    print("Ready:", win.get_title(), flush=True)

    win.kind.set_active_id("ellipse")
    win.add_button.clicked()
    until(lambda: len(win.store) == initial + 1 and win.last.get("selection"), "native shape creation")
    ellipse = win.last["selected"]
    win.kind.set_active_id("rectangle")
    win.add_button.clicked()
    until(lambda: len(win.store) == initial + 2, "native rectangle")
    for row in win.store:
        if row[0] == ellipse:
            win.selection.select_iter(row.iter)
            break
    until(lambda: win.last["selected"] == ellipse, "object selection")
    win.move_x.set_value(34)
    win.move_y.set_value(54)
    win.move_button.clicked()
    until(lambda: win.last["selection"]["x"] == 34 and win.last["selection"]["y"] == 54, "native move")
    win.command("undo")
    until(lambda: win.last["selection"] is None or win.last["selection"]["x"] != 34, "undo")
    win.command("redo")
    until(lambda: not win.busy, "redo")
    print("Shapes, selection, positioning and history passed", flush=True)
    bar = next(widget for widget in win.get_child().get_children() if isinstance(widget, Gtk.MenuBar))
    workspace = next(item for item in bar.get_children() if item.get_label().replace("_", "") == "Workspace")
    entries = {item.get_label().replace("_", ""): item for item in workspace.get_submenu().get_children()}
    win.move_x.grab_focus()
    entries["CAD command…"].activate()
    until(lambda: not win.busy, "command menu")
    check_web("document.activeElement.id === 'cad-command'", "keyboard focus in command line")
    entries["CAD script…"].activate()
    until(lambda: not win.busy, "script menu")
    check_web("document.activeElement.id === 'cad-script-source' && document.getElementById('cad-script-dialog').open", "keyboard focus in script editor")
    check_web("(document.getElementById('cad-script-dialog').close(), true)", "close script editor")
    print("Native CAD command and script menus passed", flush=True)

    with tempfile.TemporaryDirectory() as directory:
        project = Path(directory, "sample.cncstudio.json")
        gcode = Path(directory, "sample.nc")
        def save_to(path):
            def configure(dialog):
                dialog.set_current_folder(str(path.parent))
                dialog.set_current_name(path.name)
            respond(Gtk.FileChooserDialog, Gtk.ResponseType.ACCEPT, configure)
        save_to(project)
        win.save()
        until(lambda: project.is_file() and not win.busy and not win.last["dirty"], "save")
        assert '"pi-cnc-studio/1"' in project.read_text()
        original = project.read_bytes()
        embedded = Path(directory, "portable.cncstudio.json")
        save_to(embedded)
        click_web("#projectDownload")
        until(lambda: embedded.is_file() and embedded.stat().st_size > 0, "embedded project download")
        assert json.loads(embedded.read_bytes())["objects"] == json.loads(original)["objects"]
        win.command("preview")
        until(lambda: not win.busy, "asynchronous simulator preview")
        save_to(gcode)
        win.export()
        until(lambda: gcode.exists() and not win.busy, "native G-code export")
        assert "G1" in gcode.read_text()
        win.new()
        until(lambda: len(win.store) == 0, "new project")
        respond(Gtk.FileChooserDialog, Gtk.ResponseType.ACCEPT, lambda dialog: dialog.set_filename(str(project)))
        win.open()
        until(lambda: len(win.store) == initial + 2 and not win.last["dirty"], "open validated project")
        assert original == project.read_bytes()
        invalid = Path(directory, "invalid.json")
        invalid.write_text("{")
        before_id = win.document
        respond(Gtk.FileChooserDialog, Gtk.ResponseType.ACCEPT, lambda dialog: dialog.set_filename(str(invalid)))
        win.open()
        until(lambda: errors and not win.busy, "invalid project error")
        assert len(errors) == 1 and win.document == before_id and win.path == project
        errors.clear()
        answered = respond(Gtk.Dialog, Gtk.ResponseType.CANCEL)
        click_web("#printDesign")
        until(answered, "native print dialog")
        for popup in tuple(win.active_popups):
            popup.destroy()
        print("Native dialogs, save, new, open, preview, G-code, portable download and print passed", flush=True)

        win.add_button.clicked()
        until(lambda: win.last["dirty"], "dirty state")
        respond(Gtk.MessageDialog, Gtk.ResponseType.CANCEL)
        win.new()
        until(lambda: not win.busy, "cancel dirty new")
        assert len(win.store) == initial + 3
        respond(Gtk.MessageDialog, Gtk.ResponseType.CANCEL)
        win.open()
        until(lambda: not win.busy, "cancel dirty open")
        assert win.path == project
        answered = respond(Gtk.MessageDialog, Gtk.ResponseType.CANCEL)
        win.close()
        until(lambda: answered() and not win.busy, "cancel dirty close")
        assert not win.destroyed
        win.command("tab", {"name": "Help"})
        until(lambda: not win.busy, "help tab")
        # Drive the actual target=_blank help link. The auxiliary window never receives desktop commands.
        click_web('a[href="./help.html"]')
        until(lambda: any(w.get_child().get_uri() and w.get_child().get_uri().endswith("/help.html")
                          for w in win.active_popups), "native help window")
        before = win.view.get_uri()
        win.view.load_uri(url + "api/session")
        until(lambda: not win.view.is_loading(), "foreign document rejection")
        assert win.view.get_uri() == before
        assert not errors, errors
        respond(Gtk.MessageDialog, Gtk.ResponseType.ACCEPT)
        win.close()
        until(lambda: win.destroyed, "dirty close")
    print("Help window, navigation boundary, unsaved cancel and close passed", flush=True)


if __name__ == "__main__":
    # Bound nested GTK modal loops as well as the asynchronous operations checked by until().
    import signal
    signal.alarm(180)
    run(sys.argv[1])
