package au.cncstudio.pocket;

import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLPeerUnverifiedException;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * Certificate pinning for a Studio receiver reached by private address. The pairing link carries the SHA-256
 * fingerprint of the certificate Studio created for that pairing; only that certificate is accepted. No
 * certificate authority, host name or system trust store is consulted. Plain Java so it can be checked off-device.
 */
final class PinnedTls {
    private static final Pattern FRAGMENT = Pattern.compile("token=([A-Za-z0-9_-]{32,256})(?:&fp=([0-9a-f]{64}))?");

    private PinnedTls() {}

    /** {token, fingerprint or null} from a pairing link's fragment, or null when the fragment is not a pairing. */
    static String[] parse(String fragment) {
        if (fragment == null) return null;
        Matcher m = FRAGMENT.matcher(fragment);
        return m.matches() ? new String[]{m.group(1), m.group(2)} : null;
    }

    /** Whether the SHA-256 of an encoded certificate equals the lowercase hex fingerprint. Constant-time. */
    static boolean matches(byte[] encoded, String fingerprint) {
        if (encoded == null || fingerprint == null || fingerprint.length() != 64) return false;
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(encoded);
            byte[] expected = new byte[32];
            for (int i = 0; i < 32; i++) {
                int high = Character.digit(fingerprint.charAt(2 * i), 16), low = Character.digit(fingerprint.charAt(2 * i + 1), 16);
                if (high < 0 || low < 0) return false;
                expected[i] = (byte) (high << 4 | low);
            }
            return MessageDigest.isEqual(digest, expected);
        } catch (GeneralSecurityException e) {
            return false;
        }
    }

    /** A socket factory whose handshake succeeds only when the receiver presents the pinned certificate. */
    static SSLSocketFactory factory(final String fingerprint) throws GeneralSecurityException {
        TrustManager pinned = new X509TrustManager() {
            @Override public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
                throw new CertificateException("Client certificates are not used.");
            }
            @Override public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
                if (chain == null || chain.length == 0 || !matches(chain[0].getEncoded(), fingerprint))
                    throw new CertificateException("The receiver’s certificate does not match the pairing link. Copy a fresh link from Studio.");
                chain[0].checkValidity();
            }
            @Override public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
        };
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(null, new TrustManager[]{pinned}, null);
        return context.getSocketFactory();
    }

    /** The receiver is identified by its certificate, not its address: re-check the pin on the negotiated session. */
    static HostnameVerifier verifier(final String fingerprint) {
        return (hostname, session) -> {
            try {
                Certificate[] peer = session.getPeerCertificates();
                return peer.length > 0 && matches(peer[0].getEncoded(), fingerprint);
            } catch (SSLPeerUnverifiedException | CertificateException e) {
                return false;
            }
        };
    }
}
