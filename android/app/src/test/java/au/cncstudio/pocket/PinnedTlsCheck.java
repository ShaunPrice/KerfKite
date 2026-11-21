package au.cncstudio.pocket;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/**
 * Dependency-free check of the pairing-link parsing and fingerprint comparison in {@link PinnedTls}.
 * Run through validation/pendant_tls_pin_check.py, or from the android directory:
 *
 *   javac -d build/pin-check app/src/main/java/au/cncstudio/pocket/PinnedTls.java app/src/test/java/au/cncstudio/pocket/PinnedTlsCheck.java
 *   java -cp build/pin-check au.cncstudio.pocket.PinnedTlsCheck
 */
public final class PinnedTlsCheck {
    private static int checks;

    private static void check(boolean condition, String what) {
        checks++;
        if (!condition) throw new AssertionError(what);
    }

    public static void main(String[] args) throws Exception {
        String token = "abcdefghijklmnopqrstuvwxyzABCDEF_-0123456789";
        String fp = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

        String[] plain = PinnedTls.parse("token=" + token);
        check(plain != null && plain[0].equals(token) && plain[1] == null, "a token-only link has no fingerprint");
        String[] pinned = PinnedTls.parse("token=" + token + "&fp=" + fp);
        check(pinned != null && pinned[0].equals(token) && fp.equals(pinned[1]), "a pinned link yields the token and fingerprint");
        check(PinnedTls.parse(null) == null, "a missing fragment is rejected");
        check(PinnedTls.parse("token=short") == null, "a short token is rejected");
        check(PinnedTls.parse("fp=" + fp + "&token=" + token) == null, "the fingerprint must follow the token");
        check(PinnedTls.parse("token=" + token + "&fp=" + fp.toUpperCase()) == null, "an upper-case fingerprint is rejected");
        check(PinnedTls.parse("token=" + token + "&fp=" + fp.substring(2)) == null, "a truncated fingerprint is rejected");
        check(PinnedTls.parse("token=" + token + "&fp=" + fp + "&x=1") == null, "trailing fields are rejected");
        check(PinnedTls.parse("token=" + token + "&fp=") == null, "an empty fingerprint is rejected");

        byte[] certificate = "any encoded certificate".getBytes(StandardCharsets.UTF_8);
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(certificate)) hex.append(String.format("%02x", b));
        check(PinnedTls.matches(certificate, hex.toString()), "the SHA-256 of the certificate matches its fingerprint");
        check(!PinnedTls.matches("another certificate".getBytes(StandardCharsets.UTF_8), hex.toString()), "another certificate does not match");
        check(!PinnedTls.matches(certificate, fp), "another fingerprint does not match");
        check(!PinnedTls.matches(certificate, hex.substring(2)), "a short fingerprint does not match");
        check(!PinnedTls.matches(certificate, hex.substring(0, 62) + "zz"), "a non-hex fingerprint does not match");
        check(!PinnedTls.matches(null, hex.toString()) && !PinnedTls.matches(certificate, null), "missing values do not match");

        System.out.println("PASS " + checks + " pairing-link and fingerprint checks");
    }
}
