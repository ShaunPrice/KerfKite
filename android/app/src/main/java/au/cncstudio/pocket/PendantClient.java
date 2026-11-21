package au.cncstudio.pocket;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URISyntaxException;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.UUID;
import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLSocketFactory;
import org.json.JSONException;
import org.json.JSONObject;

/** An in-memory bearer session with a single Pi monitor origin. */
final class PendantClient {
    static final class PairingExpired extends IOException {
        PairingExpired() { super("Pairing expired. Copy a fresh link from the receiver."); }
    }

    final String origin;
    final boolean cleartext;
    /** True when the link carried a certificate fingerprint and only that certificate is accepted. */
    final boolean pinned;
    private final String token;
    private final SSLSocketFactory pinnedSockets;
    private final HostnameVerifier pinnedVerifier;

    PendantClient(String link) {
        URI uri;
        try { uri = new URI(link.trim()); }
        catch (URISyntaxException e) { throw new IllegalArgumentException("Paste the complete monitor pairing link."); }
        String host = uri.getHost();
        String fragment = uri.getRawFragment();
        boolean https = "https".equalsIgnoreCase(uri.getScheme());
        cleartext = "http".equalsIgnoreCase(uri.getScheme());
        if ((!https && !cleartext) || host == null || uri.getRawUserInfo() != null || uri.getRawQuery() != null
                || (!uri.getRawPath().isEmpty() && !"/".equals(uri.getRawPath())) || uri.getPort() == 0 || uri.getPort() > 65535
                || PinnedTls.parse(fragment) == null) {
            throw new IllegalArgumentException("Use the monitor’s full pairing link, including #token= and the pairing token.");
        }
        String[] pairing = PinnedTls.parse(fragment);
        pinned = pairing[1] != null;
        if (pinned && !https)
            throw new IllegalArgumentException("A pairing link with a certificate fingerprint must use HTTPS. Copy a fresh link from Studio.");
        if (cleartext && !privateHost(host))
            throw new IllegalArgumentException("For HTTP use the Pi’s private IPv4 address. Use HTTPS for hostnames.");
        origin = (https ? "https://" : "http://") + uri.getRawAuthority();
        token = pairing[0];
        try {
            pinnedSockets = pinned ? PinnedTls.factory(pairing[1]) : null;
            pinnedVerifier = pinned ? PinnedTls.verifier(pairing[1]) : null;
        } catch (GeneralSecurityException e) {
            throw new IllegalArgumentException("This phone cannot open an encrypted connection to the receiver.");
        }
    }

    private static boolean privateHost(String host) {
        if ("localhost".equalsIgnoreCase(host) || "[::1]".equals(host)) return true;
        String[] parts = host.split("\\.", -1);
        if (parts.length != 4) return false;
        int[] bytes = new int[4];
        for (int i=0;i<4;i++) {
            if (!parts[i].matches("0|[1-9][0-9]{0,2}")) return false;
            bytes[i] = Integer.parseInt(parts[i]);
            if (bytes[i] > 255) return false;
        }
        return bytes[0]==127 || bytes[0]==10 || (bytes[0]==192 && bytes[1]==168)
                || (bytes[0]==172 && bytes[1]>=16 && bytes[1]<=31);
    }

    JSONObject status() throws IOException, JSONException { return request("/api/status",null); }

    void command(String action, String axis, double distance, int feed, String controlEpoch) throws IOException, JSONException {
        JSONObject data=new JSONObject().put("action",action).put("request_id",UUID.randomUUID().toString());
        if (!"stop".equals(action)) {
            if (controlEpoch.isEmpty()) throw new IllegalArgumentException("Update the Studio receiver to use this control.");
            data.put("control_epoch",controlEpoch);
        }
        String path;
        switch (action) {
            case "jog":
                if (!("X".equals(axis)||"Y".equals(axis)||"Z".equals(axis))) throw new IllegalArgumentException("Unknown jog axis.");
                data.put("axis",axis).put("distance",distance).put("feed",feed);
                path="/api/pendant";break;
            case "home": case "zero": path="/api/pendant";break;
            case "start": case "stop": case "pause": case "resume": path="/api/action";break;
            default: throw new IllegalArgumentException("Unknown pendant action.");
        }
        if (!request(path,data).optBoolean("ok")) throw new IOException("The receiver did not acknowledge the action.");
    }

    private JSONObject request(String path, JSONObject body) throws IOException, JSONException {
        HttpURLConnection http=(HttpURLConnection)URI.create(origin+path).toURL().openConnection();
        try {
            if (pinned) {
                // The receiver's self-signed certificate is trusted by fingerprint alone.
                ((HttpsURLConnection) http).setSSLSocketFactory(pinnedSockets);
                ((HttpsURLConnection) http).setHostnameVerifier(pinnedVerifier);
            }
            http.setInstanceFollowRedirects(false);
            http.setConnectTimeout(3000);
            http.setReadTimeout(3000);
            http.setUseCaches(false);
            http.setRequestProperty("Authorization","Bearer "+token);
            http.setRequestProperty("Accept","application/json");
            http.setRequestProperty("Connection","close");
            if (body!=null) {
                byte[] bytes=body.toString().getBytes(StandardCharsets.UTF_8);
                http.setRequestMethod("POST");http.setDoOutput(true);
                http.setFixedLengthStreamingMode(bytes.length);
                http.setRequestProperty("Content-Type","application/json");
                try(OutputStream out=http.getOutputStream()) { out.write(bytes); }
            }
            int code=http.getResponseCode();
            if (code==401) throw new PairingExpired();
            if (code>=300 && code<400) throw new IOException("The receiver redirected the request. Use its direct monitor address.");
            boolean success=code>=200 && code<300;
            InputStream stream=success?http.getInputStream():http.getErrorStream();
            if (stream==null) throw new IOException("Receiver returned HTTP "+code+".");
            JSONObject result;
            try(InputStream in=stream) {
                byte[] buf=new byte[4096];
                ByteArrayOutputStream out=new ByteArrayOutputStream();
                int count;
                while ((count=in.read(buf))!=-1) {
                    if (out.size()+count>128*1024) throw new IOException("Receiver response exceeded the status limit.");
                    out.write(buf,0,count);
                }
                result=new JSONObject(new String(out.toByteArray(),StandardCharsets.UTF_8));
            }
            if (!success) throw new IOException(result.optString("error","Receiver returned HTTP "+code+"."));
            return result;
        } finally { http.disconnect(); }
    }
}
