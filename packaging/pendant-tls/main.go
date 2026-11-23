// kerfkite-pendant-tls: KerfKite's TLS relay, the HTTPS of its C++ servers: the phone pendant of the
// workstation server (kerfkite-studio) and the hosted editor (kerfkite-web).
//
// Both servers speak HTTP on the loopback interface. The server starts this program; it listens where clients
// reach it, with TLS 1.2 or later, and passes each connection on to the server. For the pendant its certificate
// is made for each pairing and signed by itself (ECDSA P-256, valid for 30 days), for the machine's name and
// addresses. The pairing link carries the SHA-256 fingerprint of the certificate, and Pocket accepts that
// certificate and no other: no certificate authority can vouch for a machine reached by a private address. The
// private key never leaves this process's memory. With -cert and -key it serves an operator's certificate
// instead; with -proxy-protocol each connection starts with a PROXY protocol (version 1) line naming the client,
// whose address the hosted editor's login limits count.
//
// When it is listening it prints "fingerprint HEX" (empty for an operator's certificate) and "port N", one
// line each. It exits when its standard input closes, so it does not outlive the server that started it.
//
// Two other modes run once and exit: -make-cert writes a self-signed certificate and key for -hosts to -cert
// and -key, never over files that are there; -fetch reads one HTTP request as JSON from standard input (a
// receiver's upload token travels there, never on the command line) and prints its answer as JSON.
//
// Go standard library only (crypto/tls, crypto/x509, crypto/ecdsa, net/http): TLS is not written here.
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
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	handshakeTime = 10 * time.Second // as the Python listener's socket timeout
)

// selfSigned makes a certificate signed by its own key (ECDSA P-256) for the hosts, valid from five minutes
// before now for the days: its DER bytes and private key.
func selfSigned(name string, hosts []string, days int, now time.Time) (tls.Certificate, []byte, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, nil, err
	}
	// A positive serial of up to 159 random bits, as RFC 5280 allows and cryptography's random_serial_number gives.
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 159))
	if err != nil {
		return tls.Certificate{}, nil, err
	}
	serial.Add(serial, big.NewInt(1))
	subject := pkix.Name{CommonName: name}
	template := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               subject,
		Issuer:                subject,
		NotBefore:             now.Add(-5 * time.Minute),
		NotAfter:              now.Add(time.Duration(days) * 24 * time.Hour),
		BasicConstraintsValid: true,
		IsCA:                  false,
	}
	seen := map[string]bool{}
	for _, host := range hosts {
		host = strings.TrimSpace(host)
		if host == "" || seen[host] {
			continue
		}
		seen[host] = true
		if ip := net.ParseIP(host); ip != nil {
			template.IPAddresses = append(template.IPAddresses, ip)
		} else {
			template.DNSNames = append(template.DNSNames, host)
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, nil, err
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		return tls.Certificate{}, nil, err
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key, Leaf: leaf}, der, nil
}

// pairing makes the certificate for one pairing: its DER bytes and private key.
func pairing(hosts []string, days int, now time.Time) (tls.Certificate, []byte, error) {
	return selfSigned("KerfKite Studio phone pendant", append(append([]string{}, hosts...), "localhost", "127.0.0.1"), days, now)
}

// makeCertificate writes a certificate for the hosts (the first is its common name) and its PKCS #8 key as PEM
// files, as the Python hosted editor's certificate command did, refusing to replace a file that is there.
func makeCertificate(certFile, keyFile string, hosts []string, days int, now time.Time) error {
	if len(hosts) == 0 || strings.TrimSpace(hosts[0]) == "" {
		return errors.New("give -hosts for the certificate")
	}
	certificate, der, err := selfSigned(strings.TrimSpace(hosts[0]), hosts, days, now)
	if err != nil {
		return err
	}
	encoded, err := x509.MarshalPKCS8PrivateKey(certificate.PrivateKey)
	if err != nil {
		return err
	}
	for _, file := range []string{certFile, keyFile} {
		if _, err := os.Lstat(file); err == nil {
			return errors.New("Certificate/key already exists; refusing to overwrite")
		}
	}
	write := func(file string, mode os.FileMode, block *pem.Block) error {
		out, err := os.OpenFile(file, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode)
		if err != nil {
			return err
		}
		if err := pem.Encode(out, block); err != nil {
			out.Close()
			return err
		}
		return out.Close()
	}
	if err := write(keyFile, 0o600, &pem.Block{Type: "PRIVATE KEY", Bytes: encoded}); err != nil {
		return err
	}
	return write(certFile, 0o644, &pem.Block{Type: "CERTIFICATE", Bytes: der})
}

// proxyLine is the PROXY protocol's version 1 line for a connection (haproxy.org's proxy-protocol.txt, 2.1).
func proxyLine(client, local net.Addr) string {
	from, ok := client.(*net.TCPAddr)
	to, ok2 := local.(*net.TCPAddr)
	if !ok || !ok2 {
		return "PROXY UNKNOWN\r\n"
	}
	if from.IP.To4() != nil && to.IP.To4() != nil {
		return fmt.Sprintf("PROXY TCP4 %s %s %d %d\r\n", from.IP.To4(), to.IP.To4(), from.Port, to.Port)
	}
	six := func(ip net.IP) string {
		if v4 := ip.To4(); v4 != nil {
			return "::ffff:" + v4.String() // TCP6 names both ends in IPv6's notation
		}
		return ip.String()
	}
	return fmt.Sprintf("PROXY TCP6 %s %s %d %d\r\n", six(from.IP), six(to.IP), from.Port, to.Port)
}

func fingerprint(der []byte) string {
	sum := sha256.Sum256(der)
	return hex.EncodeToString(sum[:])
}

// limits are how long a connection may last, how many there may be at once, and whether each is announced
// with a PROXY protocol line.
type limits struct {
	connectionTime time.Duration
	maxConnections int
	proxy          bool
}

// relay passes one client's connection to the server's listener on the loopback interface.
func relay(phone net.Conn, upstream string, limit limits) {
	defer phone.Close()
	if conn, ok := phone.(*tls.Conn); ok {
		phone.SetDeadline(time.Now().Add(handshakeTime))
		if conn.Handshake() != nil {
			return // a phone that gives up, or a client without the pinned certificate, is routine
		}
	}
	phone.SetDeadline(time.Now().Add(limit.connectionTime))
	listener, err := net.DialTimeout("tcp", upstream, handshakeTime)
	if err != nil {
		return
	}
	defer listener.Close()
	listener.SetDeadline(time.Now().Add(limit.connectionTime))
	if limit.proxy {
		if _, err := io.WriteString(listener, proxyLine(phone.RemoteAddr(), phone.LocalAddr())); err != nil {
			return
		}
	}
	done := make(chan struct{}, 2)
	go func() {
		io.Copy(listener, phone)
		if tcp, ok := listener.(*net.TCPConn); ok {
			tcp.CloseWrite() // the request is complete: the listener answers and closes
		}
		done <- struct{}{}
	}()
	go func() {
		io.Copy(phone, listener)
		if conn, ok := phone.(*tls.Conn); ok {
			conn.CloseWrite()
		}
		done <- struct{}{}
	}()
	<-done
	<-done
}

func serve(listener net.Listener, upstream string, limit limits) {
	slots := make(chan struct{}, limit.maxConnections)
	for {
		phone, err := listener.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		select {
		case slots <- struct{}{}:
			go func() {
				defer func() { <-slots }()
				relay(phone, upstream, limit)
			}()
		default:
			phone.Close()
		}
	}
}

// request is what -fetch reads: one HTTP request to make.
type request struct {
	Method  string            `json:"method"`
	URL     string            `json:"url"`
	Headers map[string]string `json:"headers"`
	Body    string            `json:"body"`
	CAFile  string            `json:"ca_file"` // trust only the certificates in this PEM file; the system's when empty
	Timeout float64           `json:"timeout_seconds"`
	Limit   int64             `json:"limit"` // the most of the answer's body to read
}

// answer is what -fetch prints: the status and body (base64 in JSON), or what went wrong.
type answer struct {
	Status int    `json:"status,omitempty"`
	Body   []byte `json:"body,omitempty"`
	Error  string `json:"error,omitempty"`
}

// fetch makes the request as urllib did for the hosted editor's receivers: no proxy, no redirects followed, TLS
// 1.2 or later with the receiver's own certificate authority if it has one, and a bounded answer.
func fetch(in io.Reader) answer {
	var r request
	if err := json.NewDecoder(io.LimitReader(in, 64<<20)).Decode(&r); err != nil {
		return answer{Error: "the request is not JSON"}
	}
	config := &tls.Config{MinVersion: tls.VersionTLS12}
	if r.CAFile != "" {
		text, err := os.ReadFile(r.CAFile)
		pool := x509.NewCertPool()
		if err != nil || !pool.AppendCertsFromPEM(text) {
			return answer{Error: "ca_file"}
		}
		config.RootCAs = pool
	}
	timeout := time.Duration(r.Timeout * float64(time.Second))
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	client := &http.Client{
		Transport: &http.Transport{Proxy: nil, TLSClientConfig: config, DisableCompression: true, DisableKeepAlives: true},
		Timeout:   timeout,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse // a redirect is an answer that cannot be verified, never followed
		},
	}
	outgoing, err := http.NewRequest(r.Method, r.URL, strings.NewReader(r.Body))
	if err != nil {
		return answer{Error: "the URL cannot be used"}
	}
	for name, value := range r.Headers {
		outgoing.Header.Set(name, value)
	}
	outgoing.Header.Set("User-Agent", "KerfKite-Studio")
	response, err := client.Do(outgoing)
	if err != nil {
		return answer{Error: "the receiver did not answer"}
	}
	defer response.Body.Close()
	limit := r.Limit
	if limit <= 0 {
		limit = 65537
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, limit))
	if err != nil {
		return answer{Error: "the answer did not arrive whole"}
	}
	return answer{Status: response.StatusCode, Body: body}
}

func run(args []string, stdin io.Reader, stdout io.Writer) error {
	flags := flag.NewFlagSet("kerfkite-pendant-tls", flag.ContinueOnError)
	listen := flags.String("listen", "0.0.0.0:8766", "address for the clients: host:port, port 0 for any free port")
	upstream := flags.String("upstream", "", "the server's listener on the loopback interface, 127.0.0.1:port")
	hosts := flags.String("hosts", "", "comma-separated names and addresses the certificate is for")
	days := flags.Int("days", 30, "days the certificate made for this pairing (or by -make-cert) is valid")
	certFile := flags.String("cert", "", "an operator's PEM certificate, in place of one made for the pairing")
	keyFile := flags.String("key", "", "its PEM private key")
	proxy := flags.Bool("proxy-protocol", false, "start each connection to the server with a PROXY protocol line naming the client")
	connections := flags.Int("max-connections", 16, "connections relayed at once; more are closed")
	connectionTime := flags.Duration("connection-time", 2*time.Minute, "how long one connection may last: a request and its answer")
	makeCert := flags.Bool("make-cert", false, "write a self-signed certificate for -hosts to -cert and -key, then exit")
	fetchOnce := flags.Bool("fetch", false, "make the HTTP request read as JSON from standard input, print its answer, then exit")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *days < 1 || *days > 397 {
		return errors.New("-days must be from 1 to 397")
	}
	if *fetchOnce {
		text, _ := json.Marshal(fetch(stdin))
		fmt.Fprintf(stdout, "%s\n", text)
		return nil
	}
	if *makeCert {
		if *certFile == "" || *keyFile == "" {
			return errors.New("give -cert and -key for the certificate to make")
		}
		if err := makeCertificate(*certFile, *keyFile, strings.Split(*hosts, ","), *days, time.Now()); err != nil {
			return err
		}
		fmt.Fprintln(stdout, "certificate made")
		return nil
	}
	if *connections < 1 || *connections > 1024 {
		return errors.New("-max-connections must be from 1 to 1024")
	}
	if *connectionTime < time.Second || *connectionTime > time.Hour {
		return errors.New("-connection-time must be from 1s to 1h")
	}
	host, _, err := net.SplitHostPort(*upstream)
	if err != nil || (host != "127.0.0.1" && host != "::1" && host != "localhost") {
		return errors.New("give -upstream as the loopback address of the pendant listener")
	}
	if (*certFile == "") != (*keyFile == "") {
		return errors.New("give both -cert and -key, or neither")
	}
	var certificate tls.Certificate
	pin := ""
	if *certFile != "" {
		certificate, err = tls.LoadX509KeyPair(*certFile, *keyFile)
		if err != nil {
			return fmt.Errorf("the monitor certificate could not be read: %w", err)
		}
	} else {
		var der []byte
		certificate, der, err = pairing(strings.Split(*hosts, ","), *days, time.Now())
		if err != nil {
			return fmt.Errorf("the pairing certificate could not be made: %w", err)
		}
		pin = fingerprint(der)
	}
	config := &tls.Config{Certificates: []tls.Certificate{certificate}, MinVersion: tls.VersionTLS12}
	listener, err := tls.Listen("tcp", *listen, config)
	if err != nil {
		return fmt.Errorf("the phone listener could not start: %w", err)
	}
	defer listener.Close()
	fmt.Fprintf(stdout, "fingerprint %s\nport %d\n", pin, listener.Addr().(*net.TCPAddr).Port)
	if f, ok := stdout.(interface{ Sync() error }); ok {
		f.Sync()
	}
	var wait sync.WaitGroup
	wait.Add(1)
	go func() {
		defer wait.Done()
		serve(listener, *upstream, limits{connectionTime: *connectionTime, maxConnections: *connections, proxy: *proxy})
	}()
	// The server keeps this program's standard input open while the pairing lasts.
	io.Copy(io.Discard, bufio.NewReader(stdin))
	listener.Close()
	wait.Wait()
	return nil
}

func main() {
	if err := run(os.Args[1:], os.Stdin, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "kerfkite-pendant-tls:", err)
		os.Exit(2)
	}
}
