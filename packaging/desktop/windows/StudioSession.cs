using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Microsoft.Web.WebView2.WinForms;

namespace KerfKite {
    using Map = Dictionary<string, object>;

    internal sealed class StudioSession {
        internal readonly Uri Url;
        internal readonly WebView2 View;
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 128 * 1024 * 1024 };

        internal static Uri ParseUrl(string address) {
            Uri uri;
            if (!Uri.TryCreate(address, UriKind.Absolute, out uri) || uri.Scheme != "http" ||
                uri.Host != "127.0.0.1" || uri.UserInfo.Length != 0 || uri.PathAndQuery != "/" ||
                uri.Fragment.Length != 0 || uri.Port < 1)
                throw new ArgumentException("KerfKite requires a local workstation address.");
            return uri;
        }

        internal StudioSession(Uri url, WebView2 view) { Url = url; View = view; }

        internal bool AtEditor(string value) {
            Uri uri;
            return Uri.TryCreate(value, UriKind.Absolute, out uri) && uri.Scheme == Url.Scheme &&
                uri.Authority == Url.Authority && uri.UserInfo.Length == 0 && uri.PathAndQuery == "/";
        }

        internal bool IsGuide(string value) {
            Uri uri;
            return Uri.TryCreate(value, UriKind.Absolute, out uri) && uri.Scheme == Url.Scheme &&
                uri.Authority == Url.Authority && uri.UserInfo.Length == 0 &&
                (uri.AbsolutePath == "/help.html" || uri.AbsolutePath == "/materials-guide.html" ||
                 uri.AbsolutePath == "/machining-lessons.html");
        }

        internal async Task<object> Call(string action, object payload = null) {
            var timeout = Stopwatch.StartNew();
            while (true) {
                if (View.CoreWebView2 == null || !AtEditor(View.CoreWebView2.Source))
                    throw new InvalidOperationException("The local editor is unavailable.");
                string script = "(location.origin + '/' === " + json.Serialize(Url.AbsoluteUri) +
                    " && location.pathname === '/' && !location.search && window.KerfKiteDesktop) ? " +
                    "window.KerfKiteDesktop.dispatch(" + json.Serialize(action) + "," + json.Serialize(payload ?? new {}) + ") : null";
                string raw = await View.CoreWebView2.ExecuteScriptAsync(script);
                string body = json.Deserialize<string>(raw);
                if (body == null) throw new InvalidOperationException("Waiting for the Studio editor.");
                Map reply = json.Deserialize<Map>(body);
                if (!Convert.ToBoolean(reply["ok"]))
                    throw new InvalidOperationException(Convert.ToString(reply["error"]));
                if (!reply.ContainsKey("pending")) return reply.ContainsKey("value") ? reply["value"] : null;
                if (timeout.Elapsed.TotalMinutes >= 5) throw new TimeoutException("Studio did not finish the operation.");
                action = "result";
                payload = new { id = Convert.ToInt32(reply["pending"]) };
                await Task.Delay(60);
            }
        }

        internal async Task<Map> State() { return (Map)await Call("state"); }

        internal async Task<Map> Open(string path) {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                if (file.Length > 4000000) throw new InvalidDataException("Studio projects must be 4 MB or smaller.");
                using (var reader = new StreamReader(file, new UTF8Encoding(false, true), true)) {
                    return (Map)await Call("open", new { content = await reader.ReadToEndAsync() });
                }
            }
        }

        internal async Task WriteArtifact(string path, Map artifact, bool project) {
            string content = (string)artifact["content"];
            string folder = Path.GetDirectoryName(Path.GetFullPath(path));
            string temp = Path.Combine(folder, ".kerfkite-" + Guid.NewGuid().ToString("N") + ".tmp");
            try {
                using (var file = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                using (var writer = new StreamWriter(file, new UTF8Encoding(false))) {
                    await writer.WriteAsync(content);
                    await writer.FlushAsync();
                    file.Flush(true);
                }
                if (File.Exists(path)) File.Replace(temp, path, null);
                else File.Move(temp, path);
            } finally {
                if (File.Exists(temp)) File.Delete(temp);
            }
            if (project) await Call("saved", new { content });
        }
    }
}
