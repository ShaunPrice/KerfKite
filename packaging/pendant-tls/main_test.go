package main

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/asn1"
	"encoding/hex"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"
)

func TestPairingCertificate(t *testing.T) {
	now := time.Date(2026, 11, 23, 12, 0, 0, 0, time.UTC)
	certificate, der, err := pairing([]string{"kerfkite-pi", "192.0.2.20", "kerfkite-pi", ""}, 30, now)
	if err != nil {
		t.Fatal(err)
	}
	leaf := certificate.Leaf
	if leaf.Subject.CommonName != "KerfKite Studio phone pendant" || leaf.Issuer.CommonName != leaf.Subject.CommonName {
		t.Fatalf("subject %v issuer %v", leaf.Subject, leaf.Issuer)
	}
	if strings.Join(leaf.DNSNames, ",") != "kerfkite-pi,localhost" {
		t.Fatalf("names %v", leaf.DNSNames)
	}
	if len(leaf.IPAddresses) != 2 || leaf.IPAddresses[0].String() != "192.0.2.20" || leaf.IPAddresses[1].String() != "127.0.0.1" {
		t.Fatalf("addresses %v", leaf.IPAddresses)
	}
	if !leaf.BasicConstraintsValid || leaf.IsCA {
		t.Fatal("the certificate must say it is not a certificate authority")
	}
	critical := false
	for _, extension := range leaf.Extensions {
		if extension.Id.Equal(asn1.ObjectIdentifier{2, 5, 29, 19}) {
			critical = extension.Critical
		}
	}
	if !critical {
		t.Fatal("basic constraints must be critical")
	}
	if !leaf.NotBefore.Equal(now.Add(-5*time.Minute)) || !leaf.NotAfter.Equal(now.Add(30*24*time.Hour)) {
		t.Fatalf("valid from %v to %v", leaf.NotBefore, leaf.NotAfter)
	}
	key, ok := leaf.PublicKey.(*ecdsa.PublicKey)
	if !ok || key.Curve != elliptic.P256() {
		t.Fatal("the key must be ECDSA P-256")
	}
	if leaf.SerialNumber.Sign() <= 0 || leaf.SerialNumber.BitLen() > 160 {
		t.Fatalf("serial %v", leaf.SerialNumber)
	}
	if err := leaf.CheckSignature(leaf.SignatureAlgorithm, leaf.RawTBSCertificate, leaf.Signature); err != nil { // signed by its own key
		t.Fatal(err)
	}
	sum := sha256.Sum256(der)
	if fingerprint(der) != hex.EncodeToString(sum[:]) || len(fingerprint(der)) != 64 {
		t.Fatal("fingerprint")
	}
	_, other, _ := pairing(nil, 30, now)
	if fingerprint(other) == fingerprint(der) {
		t.Fatal("each pairing must have its own certificate")
	}
}

// pinned dials as Pocket does: no certificate authority, only the certificate with the fingerprint.
func pinned(address, pin string) (*tls.Conn, error) {
	config := &tls.Config{
		InsecureSkipVerify: true, // replaced by the pin check below, as in Pocket's PinnedTls
		MinVersion:         tls.VersionTLS12,
		VerifyPeerCertificate: func(raw [][]byte, _ [][]*x509.Certificate) error {
			if len(raw) == 0 || fingerprint(raw[0]) != pin {
				return errors.New("not the paired certificate")
			}
			return nil
		},
	}
	return tls.DialWithDialer(&net.Dialer{Timeout: 5 * time.Second}, "tcp", address, config)
}

func TestRelayToTheLoopbackListener(t *testing.T) {
	// A stand-in for the pendant listener: one request a connection, answered and closed.
	upstream, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer upstream.Close()
	go func() {
		for {
			conn, err := upstream.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				line, _ := bufio.NewReader(conn).ReadString('\n')
				io.WriteString(conn, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n"+strings.Fields(line + " x x")[1][:6])
			}()
		}
	}()
	stdin, closeStdin := io.Pipe()
	output, stdout := io.Pipe()
	finished := make(chan error, 1)
	go func() {
		finished <- run([]string{"-listen", "127.0.0.1:0", "-upstream", upstream.Addr().String(), "-hosts", "kerfkite-pi"}, stdin, stdout)
	}()
	lines := bufio.NewReader(output)
	first, _ := lines.ReadString('\n')
	second, _ := lines.ReadString('\n')
	go io.Copy(io.Discard, lines)
	pin := strings.TrimSpace(strings.TrimPrefix(first, "fingerprint "))
	port := strings.TrimSpace(strings.TrimPrefix(second, "port "))
	if len(pin) != 64 || port == "" || port == "0" {
		t.Fatalf("announced %q %q", first, second)
	}
	address := "127.0.0.1:" + port

	conn, err := pinned(address, pin)
	if err != nil {
		t.Fatalf("the pinned certificate is refused: %v", err)
	}
	if state := conn.ConnectionState(); state.Version < tls.VersionTLS12 {
		t.Fatalf("TLS version %x", state.Version)
	}
	io.WriteString(conn, "GET /status HTTP/1.1\r\nHost: x\r\n\r\n")
	reply, _ := io.ReadAll(conn)
	conn.Close()
	if !strings.HasPrefix(string(reply), "HTTP/1.1 200 OK") || !strings.HasSuffix(string(reply), "/statu") {
		t.Fatalf("reply %q", reply)
	}
	if _, err := pinned(address, strings.Repeat("0", 64)); err == nil {
		t.Fatal("a different pin must be refused")
	}
	old := &tls.Config{InsecureSkipVerify: true, MaxVersion: tls.VersionTLS11} // TLS 1.0 and 1.1 are refused
	if c, err := tls.Dial("tcp", address, old); err == nil {
		c.Close()
		t.Fatal("TLS 1.1 must be refused")
	}
	plain, err := net.Dial("tcp", address)
	if err == nil {
		io.WriteString(plain, "GET / HTTP/1.1\r\n\r\n")
		plain.SetDeadline(time.Now().Add(15 * time.Second))
		answer, _ := io.ReadAll(plain)
		plain.Close()
		if strings.Contains(string(answer), "200 OK") {
			t.Fatal("plain HTTP must not reach the listener")
		}
	}
	closeStdin.Close() // the server ends the pairing
	select {
	case err := <-finished:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the relay did not stop when its standard input closed")
	}
}

func TestRefusals(t *testing.T) {
	for _, args := range [][]string{
		{"-upstream", "192.0.2.5:80"},
		{"-upstream", "127.0.0.1:80", "-cert", "only.pem"},
		{"-upstream", "127.0.0.1:80", "-days", "0"},
		{"-upstream", "127.0.0.1:80", "-cert", "missing.pem", "-key", "missing.key"},
	} {
		if err := run(args, strings.NewReader(""), io.Discard); err == nil {
			t.Fatalf("%v must be refused", args)
		}
	}
}
