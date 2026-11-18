using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("KerfKiteNativeSmoke")]

namespace KerfKite {
    using Map = Dictionary<string, object>;

    internal static class Program {
        // Use the exact same native eligibility probe as Setup and the launcher, including when
        // the user opens this executable directly. The probe performs no application setup.
        internal static bool ApprovedWindowsClient() {
            string launcher = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "KerfKite.exe");
            try {
                var command = new System.Diagnostics.ProcessStartInfo(launcher, "--check-windows") {
                    UseShellExecute = false, CreateNoWindow = true
                };
                using (var probe = System.Diagnostics.Process.Start(command)) {
                    return probe != null && probe.WaitForExit(10000) && probe.ExitCode == 0;
                }
            } catch (System.ComponentModel.Win32Exception) { return false; }
              catch (SystemException) { return false; }
        }

        [STAThread]
        static void Main(string[] args) {
            bool query = args.Length == 1 && args[0] == "--check-windows";
            if (!ApprovedWindowsClient()) {
                if (!query) MessageBox.Show("KerfKite Studio requires a native x64 client edition of Windows 10 or Windows 11. Reinstall Studio if this computer meets that requirement.",
                    "KerfKite Studio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Environment.ExitCode = 1150;
                return;
            }
            if (!query) OpenWorkbench(args);
        }

        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        static void OpenWorkbench(string[] args) {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            try {
                if (args.Length > 1) throw new ArgumentException("Expected the local workstation address.");
                var address = StudioSession.ParseUrl(args.Length == 1 ? args[0] : "http://127.0.0.1:18765/");
                Application.Run(new Workbench(address));
            } catch (Exception error) {
                MessageBox.Show(error.Message, "KerfKite Studio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Environment.ExitCode = 1;
            }
        }
    }

    internal sealed class Workbench : Form {
        internal readonly WebView2 View = new WebView2 { Dock = DockStyle.Fill, AccessibleName = "Studio workspace" };
        internal readonly StudioSession Session;
        readonly StatusStrip footer = new StatusStrip();
        readonly ToolStripStatusLabel status = new ToolStripStatusLabel("Opening local workstation…") { Spring = true, TextAlign = ContentAlignment.MiddleLeft };
        readonly ToolStripStatusLabel mode = new ToolStripStatusLabel();
        readonly Panel inspector = new Panel { Dock = DockStyle.Right, Width = 205, Padding = new Padding(8) };
        internal readonly ListBox Objects = new ListBox { Dock = DockStyle.Fill, IntegralHeight = false, AccessibleName = "Design objects" };
        readonly NumericUpDown posX = Distance("Position X", 0, -100000);
        readonly NumericUpDown posY = Distance("Position Y", 0, -100000);
        readonly Label dimensions = new Label { AutoSize = true };
        readonly Button move = new Button { Text = "Apply position", AutoSize = true, Dock = DockStyle.Top };
        readonly MenuStrip menu = new MenuStrip();
        readonly ToolStrip toolbar = new ToolStrip { GripStyle = ToolStripGripStyle.Hidden };
        readonly Timer timer = new Timer { Interval = 600 };
        readonly List<int> indices = new List<int>();
        CoreWebView2Environment environment;
        readonly List<ToolStripItem> commands = new List<ToolStripItem>();
        readonly Dictionary<Keys, ToolStripMenuItem> accelerators = new Dictionary<Keys, ToolStripMenuItem>();
        bool refreshing, busy, allowClose, editingPosition, navigationStarted, loaded;
        int selected = -1, pathDocument = -1, inspectorDocument = -1;
        decimal lastX, lastY;
        string documentPath;
        internal bool Ready { get; private set; }
        internal string Failure { get; private set; }
        internal ToolStripButton Rectangle { get; private set; }

        internal Workbench(Uri address, string profile = null) {
            Session = new StudioSession(address, View);
            Text = "KerfKite Studio";
            Font = SystemFonts.MessageBoxFont;
            AutoScaleMode = AutoScaleMode.Dpi;
            MinimumSize = new Size(740, 500);
            Size = new Size(1300, 840);
            WindowState = FormWindowState.Maximized;
            StartPosition = FormStartPosition.CenterScreen;
            var appIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            if (appIcon != null) Icon = appIcon;
            MainMenuStrip = menu;
            BuildMenus();
            BuildInspector();
            View.KeyDown += (s,e) => {
                ToolStripMenuItem command;
                if (!accelerators.TryGetValue(e.KeyData, out command)) return;
                e.Handled = e.SuppressKeyPress = true;
                // WebView2 blocks its browser thread during accelerator events; run menu work afterwards.
                if (command.Enabled) BeginInvoke(new Action(command.PerformClick));
            };
            footer.Items.AddRange(new ToolStripItem[] { status, mode });
            Controls.Add(View);
            Controls.Add(inspector);
            Controls.Add(toolbar);
            Controls.Add(menu);
            Controls.Add(footer);
            SetEnabled(false);
            FormClosing += ClosingWindow;
            FormClosed += (s,e) => timer.Dispose();
            Shown += async (s,e) => await Start(profile);
            timer.Tick += async (s,e) => {
                if (refreshing || busy) return;
                refreshing = true;
                try { await RefreshState(); }
                catch (Exception error) { if (loaded) status.Text = error.Message; }
                finally { refreshing = false; }
            };
        }

        static NumericUpDown Distance(string label, decimal value, decimal minimum) {
            return new NumericUpDown { AccessibleName = label, DecimalPlaces = 3, Increment = 1,
                Minimum = minimum, Maximum = 100000, Value = value, Dock = DockStyle.Fill };
        }

        ToolStripMenuItem Group(string name) {
            var item = new ToolStripMenuItem(name);
            menu.Items.Add(item);
            return item;
        }

        void Item(ToolStripMenuItem group, string title, Keys shortcut, Func<Task> function) {
            var item = new ToolStripMenuItem(title) { ShortcutKeys = shortcut };
            item.Click += async (s,e) => await Run(function);
            group.DropDownItems.Add(item);
            commands.Add(item);
            if (shortcut != Keys.None) accelerators.Add(shortcut, item);
        }

        ToolStripButton Tool(string name, Func<Task> function) {
            var button = new ToolStripButton(name);
            button.Click += async (s,e) => await Run(function);
            toolbar.Items.Add(button);
            commands.Add(button);
            return button;
        }

        ToolStripButton ShapeTool(string name) {
            var button = Tool(name, () => InsertDialog(name));
            var icon = new Bitmap(24, 24);
            using (var graphics = Graphics.FromImage(icon))
            using (var pen = new Pen(SystemColors.ControlText, 2)) {
                graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                if (name == "Ellipse") graphics.DrawEllipse(pen, 3, 5, 18, 14);
                else if (name == "Line") graphics.DrawLine(pen, 3, 19, 21, 5);
                else graphics.DrawRectangle(pen, 3, 5, 18, 14);
            }
            button.Image = icon;
            button.DisplayStyle = ToolStripItemDisplayStyle.ImageAndText;
            button.Disposed += (s,e) => icon.Dispose();
            return button;
        }

        void BuildMenus() {
            var file = Group("&File");
            Item(file, "&New project", Keys.Control | Keys.N, NewProject);
            Item(file, "&Open project…", Keys.Control | Keys.O, OpenDialog);
            Item(file, "&Save project", Keys.Control | Keys.S, () => Save(false, false));
            Item(file, "Save project &as…", Keys.Control | Keys.Shift | Keys.S, () => Save(true, false));
            Item(file, "&Export G-code…", Keys.Control | Keys.Shift | Keys.E, () => Save(true, true));
            file.DropDownItems.Add(new ToolStripSeparator());
            file.DropDownItems.Add("E&xit", null, (s,e) => Close());
            var edit = Group("&Edit");
            Item(edit, "&Undo", Keys.Control | Keys.Z, () => Session.Call("undo"));
            Item(edit, "&Redo", Keys.Control | Keys.Y, () => Session.Call("redo"));
            var design = Group("&Design");
            foreach (string shape in new [] { "Rectangle", "Ellipse", "Line" })
                Item(design, "Insert " + shape.ToLowerInvariant() + "…", Keys.None, () => InsertDialog(shape));
            var workspace = Group("&Workspace");
            Item(workspace, "CAD &command…", Keys.Control | Keys.Shift | Keys.K, () => OpenAuthoring("command-line"));
            Item(workspace, "CAD &script…", Keys.None, () => OpenAuthoring("script-editor"));
            workspace.DropDownItems.Add(new ToolStripSeparator());
            foreach (string name in new [] { "Design", "3D CAD", "CAM", "Templates", "Layers", "Control", "Projects", "Text", "Images", "Properties", "Help" })
                Item(workspace, name, Keys.None, () => Session.Call("tab", new { name }));
            workspace.DropDownItems.Add(new ToolStripSeparator());
            Item(workspace, "User guide…", Keys.F1, () => ShowGuide("help.html"));
            Item(workspace, "Materials guide…", Keys.None, () => ShowGuide("materials-guide.html"));
            Item(workspace, "Machining lessons…", Keys.None, () => ShowGuide("machining-lessons.html"));
            var toggle = new ToolStripMenuItem("Object &inspector") { Checked = true, CheckOnClick = true };
            toggle.CheckedChanged += (s,e) => inspector.Visible = toggle.Checked;
            workspace.DropDownItems.Add(new ToolStripSeparator());
            workspace.DropDownItems.Add(toggle);
            Tool("Open…", OpenDialog);
            Tool("Save", () => Save(false, false));
            toolbar.Items.Add(new ToolStripSeparator());
            Rectangle = ShapeTool("Rectangle");
            ShapeTool("Ellipse");
            ShapeTool("Line");
            toolbar.Items.Add(new ToolStripSeparator());
            Tool("Preview", () => Session.Call("preview"));
            Tool("Export G-code…", () => Save(true, true));
        }

        async Task OpenAuthoring(string action) {
            View.Focus();
            await Session.Call(action);
        }

        static void Heading(TableLayoutPanel grid, string name) {
            var heading = new Label { Text = name, AutoSize = true, Font = new Font(SystemFonts.MessageBoxFont, FontStyle.Bold),
                Margin = new Padding(3, 9, 3, 7) };
            grid.Controls.Add(heading);
            grid.SetColumnSpan(heading, 2);
        }

        void BuildInspector() {
            var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 8 };
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            for (int row = 0; row < grid.RowCount; row++)
                grid.RowStyles.Add(new RowStyle(row == 1 ? SizeType.Percent : SizeType.AutoSize, row == 1 ? 100 : 0));
            Heading(grid, "Design objects");
            grid.Controls.Add(Objects, 0, 1); grid.SetColumnSpan(Objects, 2);
            Heading(grid, "Object position");
            grid.Controls.Add(new Label { Text = "X (mm)", AutoSize = true }, 0, 3); grid.Controls.Add(posX, 1, 3);
            grid.Controls.Add(new Label { Text = "Y (mm)", AutoSize = true }, 0, 4); grid.Controls.Add(posY, 1, 4);
            grid.Controls.Add(dimensions, 0, 5); grid.SetColumnSpan(dimensions, 2);
            grid.Controls.Add(move, 0, 6); grid.SetColumnSpan(move, 2);
            inspector.Controls.Add(grid);
            Objects.SelectedIndexChanged += async (s,e) => {
                var row = Objects.SelectedIndex;
                if (!syncing && row >= 0 && row < indices.Count)
                    await Run(() => Session.Call("select", new { index = indices[row] }));
            };
            posX.ValueChanged += (s,e) => { if (!syncing) editingPosition = true; };
            posY.ValueChanged += (s,e) => { if (!syncing) editingPosition = true; };
            move.Click += async (s,e) => await Run(async () => {
                await Session.Call("move", new { x = posX.Value, y = posY.Value });
                editingPosition = false;
            });
        }

        async Task Start(string profile) {
            try {
                profile = profile ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KerfKite", "DesktopWebView");
                CoreWebView2Environment.SetLoaderDllFolderPath(AppDomain.CurrentDomain.BaseDirectory);
                environment = await CoreWebView2Environment.CreateAsync(null, profile);
                await View.EnsureCoreWebView2Async(environment);
                var core = View.CoreWebView2;
                core.Settings.AreHostObjectsAllowed = false;
                core.Settings.IsWebMessageEnabled = false;
                core.Settings.AreBrowserAcceleratorKeysEnabled = false;
                core.Settings.AreDefaultContextMenusEnabled = false;
                core.Settings.AreDevToolsEnabled = false;
                View.AllowExternalDrop = false;
                core.NavigationStarting += (s,e) => {
                    // Only the first navigation may replace the document. A reload can discard unsaved edits.
                    if (navigationStarted || !Session.AtEditor(e.Uri)) e.Cancel = true;
                    else navigationStarted = true;
                };
                core.FrameNavigationStarting += (s,e) => { e.Cancel = true; };
                core.NewWindowRequested += async (s,e) => {
                    e.Handled = true;
                    bool print = string.IsNullOrEmpty(e.Uri) || e.Uri == "about:blank";
                    if (!e.IsUserInitiated || !Session.AtEditor(core.Source) || (!print && !Session.IsGuide(e.Uri))) return;
                    using (var deferred = e.GetDeferral()) {
                        try {
                            var child = await DocumentWindow(print ? "Print document" : "Studio guide", print);
                            e.NewWindow = child.CoreWebView2;
                        } catch (Exception error) { status.Text = error.Message; }
                    }
                };
                core.LaunchingExternalUriScheme += (s,e) => e.Cancel = true;
                core.NavigationCompleted += (s,e) => {
                    if (e.IsSuccess) { loaded = true; timer.Start(); }
                    else if (!loaded) Fail("Studio could not load the local workstation (" + e.WebErrorStatus + ").");
                };
                core.Navigate(Session.Url.AbsoluteUri);
            } catch (WebView2RuntimeNotFoundException) {
                Fail("Install the Microsoft Edge WebView2 Evergreen x64 runtime, then reopen KerfKite Studio.", true);
            } catch (Exception error) { Fail("Unable to open the local workstation. " + error.Message); }
        }

        // Auxiliary documents never receive native project commands. The originating editor stays open.
        async Task<WebView2> DocumentWindow(string title, bool print) {
            var window = new Form { Text = title + " — KerfKite Studio", StartPosition = FormStartPosition.CenterParent,
                Size = new Size(1000, 760), Icon = Icon };
            var document = new WebView2 { Dock = DockStyle.Fill, AccessibleName = title };
            window.Controls.Add(document); window.Show(this);
            try {
                await document.EnsureCoreWebView2Async(environment);
                var core = document.CoreWebView2;
                core.Settings.AreHostObjectsAllowed = false;
                core.Settings.IsWebMessageEnabled = false;
                core.Settings.AreDevToolsEnabled = false;
                core.Settings.AreDefaultContextMenusEnabled = false;
                document.AllowExternalDrop = false;
                core.NavigationStarting += (s,e) => e.Cancel = print ? e.Uri != "about:blank" : !Session.IsGuide(e.Uri);
                core.FrameNavigationStarting += (s,e) => e.Cancel = true;
                core.LaunchingExternalUriScheme += (s,e) => e.Cancel = true;
                core.NewWindowRequested += (s,e) => {
                    e.Handled = true;
                    if (!print && e.IsUserInitiated && Session.IsGuide(core.Source)) {
                        Uri source;
                        if (Uri.TryCreate(e.Uri, UriKind.Absolute, out source) && source.Scheme == "https" && source.UserInfo.Length == 0 &&
                            MessageBox.Show(window, "Open this reference in your browser?\n\n" + source.AbsoluteUri, title,
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes) {
                            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(source.AbsoluteUri) { UseShellExecute = true }); }
                            catch (Exception error) { status.Text = error.Message; }
                        }
                    }
                };
                core.WindowCloseRequested += (s,e) => window.Close();
                core.DocumentTitleChanged += (s,e) => window.Text = core.DocumentTitle + " — KerfKite Studio";
                return document;
            } catch { window.Dispose(); throw; }
        }

        internal async Task<WebView2> ShowGuide(string relativePath) {
            var address = new Uri(Session.Url, relativePath).AbsoluteUri;
            if (!Session.IsGuide(address)) throw new ArgumentException("Unknown local guide.");
            var document = await DocumentWindow("Studio guide", false);
            document.CoreWebView2.Navigate(address);
            return document;
        }

        void Fail(string message, bool missingRuntime = false) {
            Failure = status.Text = message;
            Controls.Remove(View);
            var panel = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24), FlowDirection = FlowDirection.TopDown };
            panel.Controls.Add(new Label { Text = message, AutoSize = true, MaximumSize = new Size(600, 0) });
            if (missingRuntime) {
                var install = new LinkLabel { Text = "Get WebView2 from Microsoft", AutoSize = true, Margin = new Padding(3, 16, 3, 3) };
                install.LinkClicked += (s,e) => {
                    try {
                        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(
                            "https://developer.microsoft.com/en-us/microsoft-edge/webview2/") { UseShellExecute = true });
                    } catch (Exception error) { status.Text = error.Message; }
                };
                panel.Controls.Add(install);
            }
            Controls.Add(panel); panel.BringToFront();
        }

        void SetEnabled(bool value) {
            foreach (var command in commands) command.Enabled = value;
            inspector.Enabled = value;
        }

        internal async Task Run(Func<Task> function) {
            if (busy || !Ready) return;
            busy = true; SetEnabled(false);
            try {
                await function();
                await RefreshState();
            } catch (Exception error) {
                status.Text = error.Message;
                MessageBox.Show(this, error.Message, "KerfKite Studio", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            } finally {
                busy = false;
                if (!IsDisposed) SetEnabled(Ready);
            }
        }

        bool syncing;
        internal async Task RefreshState() {
            var state = await Session.State();
            if (IsDisposed) return;
            bool dirty = Convert.ToBoolean(state["dirty"]);
            string title = Convert.ToString(state["title"]);
            Text = (dirty ? "* " : "") + title + " — KerfKite Studio";
            int doc = Convert.ToInt32(state["documentId"]);
            if (doc != pathDocument) documentPath = null;
            int selection = state["selected"] == null ? -1 : Convert.ToInt32(state["selected"]);
            var rows = ((IEnumerable)state["objects"]).Cast<Map>().ToList();
            var newIndices = rows.Select(row => Convert.ToInt32(row["index"])).ToList();
            var names = rows.Select(row => Convert.ToString(row["name"])).ToArray();
            syncing = true;
            try {
                if (!indices.SequenceEqual(newIndices) || !Objects.Items.Cast<string>().SequenceEqual(names)) {
                    Objects.BeginUpdate();
                    try {
                        Objects.Items.Clear(); Objects.Items.AddRange(names);
                        indices.Clear(); indices.AddRange(newIndices);
                    } finally { Objects.EndUpdate(); }
                }
                Objects.SelectedIndex = indices.IndexOf(selection);
                var obj = state["selection"] as Map;
                move.Enabled = posX.Enabled = posY.Enabled = obj != null;
                dimensions.Text = obj == null ? "Select an object to position it." :
                    string.Format(CultureInfo.CurrentCulture, "{0:0.###} × {1:0.###} mm", obj["width"], obj["height"]);
                if (obj != null) {
                    decimal x = Convert.ToDecimal(obj["x"]), y = Convert.ToDecimal(obj["y"]);
                    if (selection != selected || doc != inspectorDocument || x != lastX || y != lastY || !editingPosition) {
                        posX.Value = Math.Min(posX.Maximum, Math.Max(posX.Minimum, x));
                        posY.Value = Math.Min(posY.Maximum, Math.Max(posY.Minimum, y));
                        editingPosition = false;
                    }
                    lastX = x; lastY = y;
                }
                selected = selection; inspectorDocument = doc;
                mode.Text = Convert.ToString(state["mode"]);
                if (!Ready) {
                    Ready = true;
                    status.Text = "Ready";
                    if (!busy) SetEnabled(true);
                }
            } finally { syncing = false; }
        }

        async Task<bool> CanReplace() {
            var state = await Session.State();
            if (!Convert.ToBoolean(state["dirty"])) return true;
            var answer = MessageBox.Show(this, "Save changes to “" + state["title"] + "”?", "KerfKite Studio",
                MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question, MessageBoxDefaultButton.Button1);
            if (answer == DialogResult.Yes) return await Save(false, false) && !Convert.ToBoolean((await Session.State())["dirty"]);
            return answer == DialogResult.No;
        }

        async void ClosingWindow(object sender, FormClosingEventArgs e) {
            if (allowClose || !Ready) return;
            e.Cancel = true;
            if (busy) return;
            busy = true; SetEnabled(false);
            try {
                if (await CanReplace()) { allowClose = true; BeginInvoke(new Action(Close)); }
            } catch (Exception error) {
                var answer = MessageBox.Show(this, error.Message + "\n\nClose the workstation anyway?", "KerfKite Studio",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
                if (answer == DialogResult.Yes) { allowClose = true; BeginInvoke(new Action(Close)); }
            } finally { busy = false; if (!IsDisposed) SetEnabled(Ready); }
        }

        async Task NewProject() {
            if (!await CanReplace()) return;
            await Session.Call("new");
            documentPath = null;
        }

        async Task OpenDialog() {
            using (var dialog = new OpenFileDialog { Filter = "Studio projects (*.cncstudio.json;*.json)|*.cncstudio.json;*.json|All files (*.*)|*.*",
                    Title = "Open KerfKite project", Multiselect = false, RestoreDirectory = true }) {
                if (dialog.ShowDialog(this) != DialogResult.OK || !await CanReplace()) return;
                var opened = await Session.Open(dialog.FileName);
                pathDocument = Convert.ToInt32(opened["documentId"]);
                documentPath = dialog.FileName;
                status.Text = "Opened " + Path.GetFileName(documentPath);
            }
        }

        async Task<bool> Save(bool choose, bool export) {
            var artifact = (Map)await Session.Call(export ? "export" : "save");
            int doc = export ? -1 : Convert.ToInt32(artifact["documentId"]);
            string path = export || choose || pathDocument != doc ? null : documentPath;
            if (path == null) {
                using (var dialog = new SaveFileDialog {
                    Title = export ? "Export G-code" : "Save KerfKite project",
                    Filter = export ? "G-code (*.nc;*.gcode)|*.nc;*.gcode|All files (*.*)|*.*" : "Studio project (*.cncstudio.json)|*.cncstudio.json",
                    FileName = Path.GetFileName(Convert.ToString(artifact["name"])), AddExtension = true,
                    DefaultExt = export ? "nc" : "cncstudio.json", OverwritePrompt = true, RestoreDirectory = true }) {
                    if (dialog.ShowDialog(this) != DialogResult.OK) return false;
                    path = dialog.FileName;
                }
            }
            await Session.WriteArtifact(path, artifact, !export);
            if (!export) { documentPath = path; pathDocument = doc; }
            status.Text = (export ? "Exported " : "Saved ") + Path.GetFileName(path);
            return true;
        }

        internal async Task InsertDialog(string kind) {
            using (var dialog = new Form { Text = "Insert " + kind.ToLowerInvariant(), FormBorderStyle = FormBorderStyle.FixedDialog,
                    StartPosition = FormStartPosition.CenterParent, MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false,
                    AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Font = Font }) {
                var fields = new TableLayoutPanel { AutoSize = true, Padding = new Padding(12), ColumnCount = 2, RowCount = 5 };
                var x = Distance("X", 10, -100000); var y = Distance("Y", 10, -100000);
                var width = Distance("Width", 30, 0); var height = Distance("Height", 20, 0);
                string[] labels = { "X (mm)", "Y (mm)", "Width (mm)", "Height (mm)" };
                var values = new [] { x, y, width, height };
                for (int i = 0; i < values.Length; i++) {
                    fields.Controls.Add(new Label { Text = labels[i], AutoSize = true, Anchor = AnchorStyles.Left }, 0, i);
                    fields.Controls.Add(values[i], 1, i);
                }
                var ok = new Button { Text = "Insert", AutoSize = true, DialogResult = DialogResult.OK };
                var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
                fields.Controls.Add(ok, 0, 4); fields.Controls.Add(cancel, 1, 4);
                dialog.Controls.Add(fields); dialog.AcceptButton = ok; dialog.CancelButton = cancel;
                if (dialog.ShowDialog(this) == DialogResult.OK)
                    await Session.Call("shape", new { kind = kind.ToLowerInvariant(), x = x.Value, y = y.Value, width = width.Value, height = height.Value });
            }
        }
    }
}
