package au.cncstudio.pocket;

import java.net.URI;
import javax.net.ssl.HttpsURLConnection;

/**
 * Makes one pinned request the way PendantClient does and prints the HTTP status, or REJECTED when the
 * handshake fails. Used by validation/pendant_tls_pin_check.py against a real Studio phone listener.
 * Arguments: origin (https://host:port), fingerprint, bearer token.
 */
public final class PinnedTlsProbe {
    public static void main(String[] args) throws Exception {
        HttpsURLConnection http = (HttpsURLConnection) URI.create(args[0] + "/api/status").toURL().openConnection();
        http.setSSLSocketFactory(PinnedTls.factory(args[1]));
        http.setHostnameVerifier(PinnedTls.verifier(args[1]));
        http.setConnectTimeout(3000);
        http.setReadTimeout(3000);
        http.setRequestProperty("Authorization", "Bearer " + args[2]);
        http.setRequestProperty("Connection", "close");
        try {
            System.out.println("STATUS " + http.getResponseCode());
        } catch (javax.net.ssl.SSLException e) {
            System.out.println("REJECTED " + e.getClass().getSimpleName());
        } finally {
            http.disconnect();
        }
    }
}
