package main

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
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

// startRelay runs the relay in front of a stand-in listener that answers "ok"; it returns the address, the
// announced fingerprint and the function that ends the relay.
func startRelay(t *testing.T, extra ...string) (string, string, func()) {
	t.Helper()
	upstream, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		for {
			conn, err := upstream.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				bufio.NewReader(conn).ReadString('\n')
				io.WriteString(conn, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
			}()
		}
	}()
	stdin, closeStdin := io.Pipe()
	output, stdout := io.Pipe()
	finished := make(chan error, 1)
	args := append([]string{"-listen", "127.0.0.1:0", "-upstream", upstream.Addr().String()}, extra...)
	go func() { finished <- run(args, stdin, stdout) }()
	lines := bufio.NewReader(output)
	first, _ := lines.ReadString('\n')
	second, _ := lines.ReadString('\n')
	go io.Copy(io.Discard, lines)
	stop := func() {
		closeStdin.Close()
		<-finished
		upstream.Close()
	}
	return "127.0.0.1:" + strings.TrimSpace(strings.TrimPrefix(second, "port ")), strings.TrimSpace(strings.TrimPrefix(first, "fingerprint ")), stop
}

func get(t *testing.T, conn *tls.Conn) string {
	t.Helper()
	io.WriteString(conn, "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n")
	reply, _ := io.ReadAll(conn)
	conn.Close()
	return string(reply)
}

func TestStalledConnectionDoesNotBlockOthers(t *testing.T) {
	address, pin, stop := startRelay(t, "-hosts", "kerfkite-pi")
	defer stop()
	stalled, err := net.Dial("tcp", address) // connects and never starts a handshake
	if err != nil {
		t.Fatal(err)
	}
	defer stalled.Close()
	conn, err := pinned(address, pin)
	if err != nil {
		t.Fatalf("a request behind a stalled connection failed: %v", err)
	}
	if reply := get(t, conn); !strings.HasSuffix(reply, "ok") {
		t.Fatalf("reply %q", reply)
	}
}

func TestOperatorCertificate(t *testing.T) {
	// An operator's certificate and key, as PEM files: served as they are, with no fingerprint announced.
	directory := t.TempDir()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(7),
		Subject:      pkix.Name{CommonName: "workshop.example"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		DNSNames:     []string{"localhost"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	encoded, _ := x509.MarshalPKCS8PrivateKey(key)
	certFile, keyFile := filepath.Join(directory, "server.pem"), filepath.Join(directory, "server-key.pem")
	os.WriteFile(certFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600)
	os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: encoded}), 0o600)
	address, pin, stop := startRelay(t, "-cert", certFile, "-key", keyFile)
	defer stop()
	if pin != "" {
		t.Fatalf("an operator's certificate is not pinned, got %q", pin)
	}
	conn, err := pinned(address, fingerprint(der))
	if err != nil {
		t.Fatalf("the operator's certificate was not served: %v", err)
	}
	if reply := get(t, conn); !strings.HasSuffix(reply, "ok") {
		t.Fatalf("reply %q", reply)
	}
}

func TestProxyProtocolLine(t *testing.T) {
	// The hosted editor learns each client's address from the line the relay sends first.
	upstream, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer upstream.Close()
	lines := make(chan string, 1)
	go func() {
		conn, err := upstream.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		reader := bufio.NewReader(conn)
		first, _ := reader.ReadString('\n')
		reader.ReadString('\n')
		lines <- first
		io.WriteString(conn, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
	}()
	stdin, closeStdin := io.Pipe()
	output, stdout := io.Pipe()
	finished := make(chan error, 1)
	go func() {
		finished <- run([]string{"-listen", "127.0.0.1:0", "-upstream", upstream.Addr().String(), "-hosts", "kerfkite-pi", "-proxy-protocol",
			"-max-connections", "4", "-connection-time", "30s"}, stdin, stdout)
	}()
	reader := bufio.NewReader(output)
	first, _ := reader.ReadString('\n')
	second, _ := reader.ReadString('\n')
	go io.Copy(io.Discard, reader)
	conn, err := pinned("127.0.0.1:"+strings.TrimSpace(strings.TrimPrefix(second, "port ")), strings.TrimSpace(strings.TrimPrefix(first, "fingerprint ")))
	if err != nil {
		t.Fatal(err)
	}
	local := conn.LocalAddr().(*net.TCPAddr)
	remote := conn.RemoteAddr().(*net.TCPAddr)
	if reply := get(t, conn); !strings.HasSuffix(reply, "ok") {
		t.Fatalf("reply %q", reply)
	}
	want := fmt.Sprintf("PROXY TCP4 127.0.0.1 127.0.0.1 %d %d\r\n", local.Port, remote.Port)
	if line := <-lines; line != want {
		t.Fatalf("PROXY line %q, want %q", line, want)
	}
	closeStdin.Close()
	if err := <-finished; err != nil {
		t.Fatal(err)
	}
	six := proxyLine(&net.TCPAddr{IP: net.ParseIP("2001:db8::7"), Port: 5000}, &net.TCPAddr{IP: net.ParseIP("192.0.2.1"), Port: 443})
	if six != "PROXY TCP6 2001:db8::7 ::ffff:192.0.2.1 5000 443\r\n" {
		t.Fatalf("mixed families %q", six)
	}
	for _, args := range [][]string{
		{"-upstream", "127.0.0.1:80", "-max-connections", "0"},
		{"-upstream", "127.0.0.1:80", "-connection-time", "2h"},
	} {
		if err := run(args, strings.NewReader(""), io.Discard); err == nil {
			t.Fatalf("%v must be refused", args)
		}
	}
}

func TestMakeCertificate(t *testing.T) {
	directory := t.TempDir()
	certFile, keyFile := filepath.Join(directory, "web.pem"), filepath.Join(directory, "web-key.pem")
	var out strings.Builder
	if err := run([]string{"-make-cert", "-cert", certFile, "-key", keyFile, "-hosts", "studio.example,192.0.2.9", "-days", "90"}, strings.NewReader(""), &out); err != nil {
		t.Fatal(err)
	}
	if out.String() != "certificate made\n" {
		t.Fatalf("printed %q", out.String())
	}
	pair, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		t.Fatal(err)
	}
	leaf, err := x509.ParseCertificate(pair.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	if leaf.Subject.CommonName != "studio.example" || strings.Join(leaf.DNSNames, ",") != "studio.example" || len(leaf.IPAddresses) != 1 ||
		leaf.IPAddresses[0].String() != "192.0.2.9" {
		t.Fatalf("certificate for %v %v %v", leaf.Subject, leaf.DNSNames, leaf.IPAddresses)
	}
	if days := leaf.NotAfter.Sub(leaf.NotBefore).Hours() / 24; days < 90 || days > 90.01 {
		t.Fatalf("valid for %v days", days)
	}
	if info, err := os.Stat(keyFile); err != nil || (os.PathSeparator == '/' && info.Mode().Perm() != 0o600) {
		t.Fatalf("the key must be private: %v %v", info.Mode(), err)
	}
	before, _ := os.ReadFile(certFile)
	if err := run([]string{"-make-cert", "-cert", certFile, "-key", filepath.Join(directory, "other.pem"), "-hosts", "x"}, strings.NewReader(""), io.Discard); err == nil {
		t.Fatal("an existing certificate must not be replaced")
	}
	after, _ := os.ReadFile(certFile)
	if string(before) != string(after) {
		t.Fatal("the certificate changed")
	}
	if _, err := os.Stat(filepath.Join(directory, "other.pem")); err == nil {
		t.Fatal("no key may be written beside a certificate that was there")
	}
	// A certificate that cannot be written leaves no private key behind.
	missing := filepath.Join(directory, "no-such-folder", "web.pem")
	if err := makeCertificate(missing, filepath.Join(directory, "lone-key.pem"), []string{"x"}, 1, time.Now()); err == nil {
		t.Fatal("a certificate in a missing folder must fail")
	}
	if _, err := os.Stat(filepath.Join(directory, "lone-key.pem")); err == nil {
		t.Fatal("the key was left without its certificate")
	}
}

func TestFetch(t *testing.T) {
	// A receiver with a certificate of its own authority, as kerfkite-web destination --ca-file trusts.
	directory := t.TempDir()
	certFile, keyFile := filepath.Join(directory, "receiver.pem"), filepath.Join(directory, "receiver-key.pem")
	if err := makeCertificate(certFile, keyFile, []string{"127.0.0.1", "keys.example"}, 1, time.Now()); err != nil {
		t.Fatal(err)
	}
	pair, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		t.Fatal(err)
	}
	listener, err := tls.Listen("tcp", "127.0.0.1:0", &tls.Config{Certificates: []tls.Certificate{pair}})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	seen := make(chan string, 8)
	go http.Serve(listener, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		seen <- r.Method + " " + r.URL.Path + " " + r.Header.Get("Authorization") + " " + string(body)
		switch r.URL.Path {
		case "/host":
			w.Write([]byte(r.Host))
		case "/moved":
			http.Redirect(w, r, "/api/receive", http.StatusFound)
		case "/big":
			w.Write([]byte(strings.Repeat("x", 70000)))
		case "/short", "/short-chunked":
			// The answer stops before its end: 5 of 10 bytes, or within a chunk.
			connection, buffered, _ := w.(http.Hijacker).Hijack()
			if r.URL.Path == "/short" {
				buffered.WriteString("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello")
			} else {
				buffered.WriteString("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\na\r\nhello")
			}
			buffered.Flush()
			connection.Close()
		default:
			w.Write([]byte(`{"status":"stored_for_review"}`))
		}
	}))
	base := "https://" + listener.Addr().String()
	// A proxy in the environment is not used: one that is not there would fail every request.
	t.Setenv("HTTPS_PROXY", "http://127.0.0.1:1")
	t.Setenv("HTTP_PROXY", "http://127.0.0.1:1")
	ask := func(path, ca string) answer {
		text, _ := json.Marshal(request{Method: "POST", URL: base + path, Headers: map[string]string{"Authorization": "Bearer t"}, Body: "{}", CAFile: ca, Timeout: 5, Limit: 65537})
		return fetch(strings.NewReader(string(text)))
	}
	if a := ask("/api/receive", certFile); a.Error != "" || a.Status != 200 || string(a.Body) != `{"status":"stored_for_review"}` {
		t.Fatalf("answer %+v", a)
	}
	if got := <-seen; got != "POST /api/receive Bearer t {}" {
		t.Fatalf("received %q", got)
	}
	if a := ask("/moved", certFile); a.Status != http.StatusFound || len(a.Headers["Location"]) != 1 || a.Headers["Location"][0] != "/api/receive" {
		t.Fatalf("a redirect must be returned with its headers, not followed: %+v", a)
	}
	<-seen
	if a := ask("/big", certFile); a.Status != 200 || len(a.Body) != 65537 {
		t.Fatalf("the answer must be cut at the limit: %d bytes", len(a.Body))
	}
	<-seen
	if a := ask("/short", certFile); a.Error != "" || a.Status != 200 || string(a.Body) != "hello" || !a.Short || a.HeadLines != 1 || a.LongestLine != 20 {
		t.Fatalf("an answer cut short of its length is what arrived, with its head's one line: %+v", a)
	}
	<-seen
	if a := ask("/short-chunked", certFile); a.Error == "" {
		t.Fatalf("an answer cut short in a chunk did not arrive: %+v", a)
	}
	<-seen
	if a := ask("/api/receive", ""); a.Error == "" {
		t.Fatal("a certificate the system does not trust must be refused")
	}
	// An address resolved beforehand: the certificate is checked for the name, which is also the Host header.
	named := func(name string) answer {
		text, _ := json.Marshal(request{Method: "GET", URL: base + "/host", CAFile: certFile, Timeout: 5, ServerName: name, Host: name})
		return fetch(strings.NewReader(string(text)))
	}
	if a := named("keys.example"); a.Error != "" || a.Status != 200 || string(a.Body) != "keys.example" || a.Short {
		t.Fatalf("a request to an address for a name: %+v", a)
	}
	<-seen
	if a := named("other.example"); a.Error == "" {
		t.Fatalf("a certificate that is not for the name must be refused: %+v", a)
	}
	if a := ask("/api/receive", filepath.Join(directory, "missing.pem")); a.Error != "ca_file" {
		t.Fatalf("a missing CA file: %+v", a)
	}
	// check_only reads the CA file and makes no request.
	check := func(ca string) answer {
		text, _ := json.Marshal(request{CAFile: ca, CheckOnly: true})
		return fetch(strings.NewReader(string(text)))
	}
	if a := check(certFile); a.Error != "" || a.Status != 0 {
		t.Fatalf("a CA file with a certificate: %+v", a)
	}
	if err := os.WriteFile(filepath.Join(directory, "empty.pem"), []byte("no certificate here\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if a := check(filepath.Join(directory, "empty.pem")); a.Error != "ca_file" {
		t.Fatalf("a CA file without a certificate: %+v", a)
	}
	if a := fetch(strings.NewReader("not json")); a.Error == "" {
		t.Fatal("a request that is not JSON must be refused")
	}
	var out strings.Builder
	text, _ := json.Marshal(request{Method: "GET", URL: base + "/x", CAFile: certFile, Timeout: 5})
	if err := run([]string{"-fetch"}, strings.NewReader(string(text)), &out); err != nil {
		t.Fatal(err)
	}
	var printed answer
	if err := json.Unmarshal([]byte(out.String()), &printed); err != nil || printed.Status != 200 || !strings.HasSuffix(out.String(), "\n") {
		t.Fatalf("printed %q", out.String())
	}
}

func TestHeadLines(t *testing.T) {
	if _, _, head := headLines([]byte("HTTP/1.1 100 Continue\r\nX: 1\r\n\r\nHTTP/1.1 302 Found\r\nLocation: / \r\nSet-Cookie: a=\xe9\r\n\r\nbody")); head != "Location: / \r\nSet-Cookie: a=\u00e9\r\n\r\n" {
		t.Errorf("the last head's lines: %q", head)
	}
	for _, c := range []struct {
		head          string
		most, longest int
	}{
		{"HTTP/1.1 200 OK\r\nA: 1\r\nB: 22\r\n\r\nbody\r\n\r\n", 2, 17},
		{"HTTP/1.1 200 OK\nA: 1\n\nbody", 1, 16},
		{"HTTP/1.1 100 Continue\r\nX: 1\r\nY: 2\r\nZ: 3\r\n\r\nHTTP/1.1 200 OK\r\nA: 1\r\n\r\n", 3, 23},
		{"HTTP/1.1 204 No Content\r\n\r\n", 0, 25},
		{"HTTP/1.1 200 OK\r\nA: " + strings.Repeat("v", 70000), 1, 70003},
		{"", 0, 0},
	} {
		if most, longest, _ := headLines([]byte(c.head)); most != c.most || longest != c.longest {
			t.Errorf("%.40q: %d lines, longest %d; want %d and %d", c.head, most, longest, c.most, c.longest)
		}
	}
}
