using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace KerfKite {
    using Map = Dictionary<string, object>;

    // Build only with --smoke. The runner operates on a fresh simulator and WebView profile.
    internal static class Smoke {
        static void Require(bool pass, string message) { if (!pass) throw new Exception(message); }

        // These helpers address only dialogs owned by this test form.
        [DllImport("user32.dll")] static extern IntPtr GetLastActivePopup(IntPtr owner);
        [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dialog, int item);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);

        static async Task CheckCancelNew(Workbench form) {
            var file = (ToolStripMenuItem)form.MainMenuStrip.Items[0];
            var newProject = (ToolStripMenuItem)file.DropDownItems[0];
            var dialogSeen = new TaskCompletionSource<bool>();
            using (var timer = new Timer { Interval = 80 }) {
                timer.Tick += (s,e) => {
                    var prompt = GetLastActivePopup(form.Handle);
                    // IDYES, IDNO, IDCANCEL uniquely identify the dirty-document decision in this path.
                    if (prompt == form.Handle || GetDlgItem(prompt, 6) == IntPtr.Zero || GetDlgItem(prompt, 7) == IntPtr.Zero ||
                        GetDlgItem(prompt, 2) == IntPtr.Zero) return;
                    timer.Stop();
                    PostMessage(prompt, 0x0111, (IntPtr)2, IntPtr.Zero);
                    dialogSeen.TrySetResult(true);
                };
                timer.Start(); newProject.PerformClick();
                var completed = await Task.WhenAny(dialogSeen.Task, Task.Delay(5000));
                Require(completed == dialogSeen.Task, "Expected a dirty-project confirmation.");
            }
            await Task.Delay(300);
            var state = await form.Session.State();
            Require(Convert.ToBoolean(state["dirty"]) && ((IEnumerable)state["objects"]).Cast<object>().Count() == 1,
                "Cancelling New lost the current drawing.");
        }

        static async Task CheckCadMenus(Workbench form) {
            var workspace = form.MainMenuStrip.Items.OfType<ToolStripMenuItem>().Single(menu => menu.Text == "&Workspace");
            var items = workspace.DropDownItems.OfType<ToolStripMenuItem>().ToArray();
            var command = items.Single(item => item.Text == "CAD &command…");
            Require(command.ShortcutKeys == (Keys.Control | Keys.Shift | Keys.K), "Command shortcut missing.");
            form.Objects.Focus();
            foreach (var item in new[] { command, items.Single(candidate => candidate.Text == "CAD &script…") }) {
                item.PerformClick();
                for (int i = 0; i < 100 && !item.Enabled; i++) await Task.Delay(50);
                string id = item == command ? "cad-command" : "cad-script-source";
                var focused = await form.View.CoreWebView2.ExecuteScriptAsync("document.activeElement.id === '" + id + "'");
                Require(focused == "true", "Native menu failed to focus " + id);
            }
            await form.View.CoreWebView2.ExecuteScriptAsync("document.getElementById('cad-script-dialog').close()");
        }

        [STAThread]
        static int Main(string[] args) {
            if (!Program.ApprovedWindowsClient()) return 1150;
            if (args.Length != 2 || !Directory.Exists(args[1])) return 2;
            string dir = Path.GetFullPath(args[1]), receipt = Path.Combine(dir, "receipt.txt");
            if (File.Exists(receipt)) return 2;
            var output = new List<string>();
            int exit = 1;
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            using (var form = new Workbench(StudioSession.ParseUrl(args[0]), Path.Combine(dir, "isolated-webview"))) {
                form.Shown += async (sender,args2) => {
                    try {
                        for (int i = 0; i < 300 && !form.Ready && form.Failure == null; i++) await Task.Delay(200);
                        Require(form.Ready, form.Failure ?? "Studio failed to become ready.");
                        var session = form.Session;
                        output.Add("WebView2 " + form.View.CoreWebView2.Environment.BrowserVersionString);
                        await session.State();
                        await session.Call("new");
                        using (var click = new Timer { Interval = 100 }) {
                            click.Tick += (s,e) => {
                                var dialog = Application.OpenForms.Cast<Form>().FirstOrDefault(w => w.Text == "Insert rectangle");
                                if (dialog != null) { click.Stop(); ((Button)dialog.AcceptButton).PerformClick(); }
                            };
                            click.Start(); form.Rectangle.PerformClick();
                        }
                        Map state = null;
                        for (int i = 0; i < 30; i++) {
                            await Task.Delay(100);
                            state = await session.State();
                            if (Convert.ToBoolean(state["dirty"]) && form.Objects.Items.Count == 1) break;
                        }
                        Require(form.Objects.Items.Count == 1 && Convert.ToBoolean(state["dirty"]), "Native rectangle insertion failed.");
                        await CheckCancelNew(form);
                        await CheckCadMenus(form);
                        output.Add("Native CAD command and script menus focused the editor from the inspector.");
                        output.Add("Native toolbar and dimensions dialog inserted a rectangle; cancelling New preserved the drawing.");
                        await session.Call("shape", new { kind = "line", x = 5, y = 5, width = 9, height = 2 });
                        await form.RefreshState();
                        form.Objects.SelectedIndex = 0;
                        await Task.Delay(500);
                        Require(Convert.ToInt32((await session.State())["selected"]) == 0, "Native object selection failed.");
                        await session.Call("move", new { x = 18, y = 22 });
                        await session.Call("undo"); await session.Call("redo");
                        await session.Call("select", new { index = 0 });
                        var selection = (Map)(await session.State())["selection"];
                        Require(Math.Abs(Convert.ToDouble(selection["x"]) - 18) < 0.001, "Move/undo/redo did not preserve position.");
                        foreach (string name in new [] { "3D CAD", "Templates", "Help", "CAM", "Design" }) await session.Call("tab", new { name });
                        foreach (string page in new [] { "help.html", "materials-guide.html?tool=flat" }) {
                            var guide = await form.ShowGuide(page);
                            try {
                                bool complete = false;
                                for (int i = 0; i < 70 && !complete; i++) {
                                    await Task.Delay(100);
                                    complete = (await guide.CoreWebView2.ExecuteScriptAsync("document.readyState==='complete' && !!document.querySelector('h1')")) == "true";
                                }
                                Require(complete && session.IsGuide(guide.CoreWebView2.Source), "Native guide failed to render.");
                                Require(session.AtEditor(form.View.CoreWebView2.Source), "Guide replaced the project.");
                            } finally { guide.FindForm().Close(); }
                        }
                        output.Add("Help and materials references opened as native child windows while retaining the editor.");
                        var project = (Map)await session.Call("save");
                        string path = Path.Combine(dir, "test.cncstudio.json");
                        await session.WriteArtifact(path, project, true);
                        Require(!Convert.ToBoolean((await session.State())["dirty"]), "Saved document remains dirty.");
                        await session.WriteArtifact(path, (Map)await session.Call("save"), true);
                        await session.Call("new"); await session.Open(path);
                        Require(((IEnumerable)(await session.State())["objects"]).Cast<object>().Count() == 2, "Reopen lost objects.");
                        await session.Call("preview");
                        var gcode = (Map)await session.Call("export");
                        string gPath = Path.Combine(dir, "test.nc");
                        await session.WriteArtifact(gPath, gcode, false);
                        Require(File.ReadAllText(gPath).Contains("G1"), "No feed move in simulated CNC job.");
                        output.Add("Selected an object, moved, used history, switched tabs, saved, replaced, reopened, previewed and exported G-code.");
                        form.View.CoreWebView2.Navigate("about:blank"); await Task.Delay(250);
                        Require(session.AtEditor(form.View.CoreWebView2.Source), "Left the trusted workstation.");
                        await session.State();
                        output.Add("Foreign top-level navigation was blocked and the open project survived.");
                        output.Add("PASS");
                        exit = 0;
                    } catch (Exception error) { output.Add(error.ToString()); }
                    finally {
                        File.WriteAllLines(receipt, output);
                        form.Dispose();
                        Application.ExitThread();
                    }
                };
                Application.Run(form);
            }
            return exit;
        }
    }
}
