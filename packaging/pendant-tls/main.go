// kerfkite-pendant-tls: the phone pendant's HTTPS for KerfKite Studio's C++ workstation server.
//
// The pendant listener of kerfkite-studio speaks HTTP on the loopback interface. The server starts this
// program for each pairing; it listens where the phone can reach it, with TLS 1.2 or later, and passes each
// connection on to that listener. Its certificate is made for the pairing and signed by itself (ECDSA P-256,
// valid for 30 days), for the machine's name and addresses. The pairing link carries the SHA-256 fingerprint
// of the certificate, and Pocket accepts that certificate and no other: no certificate authority can vouch
// for a machine reached by a private address. The private key never leaves this process's memory. With
// -cert and -key it serves an operator's certificate instead.
//
// When it is listening it prints "fingerprint HEX" (empty for an operator's certificate) and "port N", one
// line each. It exits when its standard input closes, so it does not outlive the server that started it.
//
// Go standard library only (crypto/tls, crypto/x509, crypto/ecdsa): TLS is not written here.
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
	"errors"
	"flag"
	"fmt"
	"io"
	"math/big"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	handshakeTime  = 10 * time.Second // as the Python listener's socket timeout
	connectionTime = 2 * time.Minute  // one request a connection: a phone's request and its answer
	maxConnections = 16
)

// pairing makes the certificate for one pairing: its DER bytes and private key.
func pairing(hosts []string, days int, now time.Time) (tls.Certificate, []byte, error) {
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
	subject := pkix.Name{CommonName: "KerfKite Studio phone pendant"}
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
	for _, host := range append(append([]string{}, hosts...), "localhost", "127.0.0.1") {
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

func fingerprint(der []byte) string {
	sum := sha256.Sum256(der)
	return hex.EncodeToString(sum[:])
}

// relay passes one phone connection to the pendant listener on the loopback interface.
func relay(phone net.Conn, upstream string) {
	defer phone.Close()
	if conn, ok := phone.(*tls.Conn); ok {
		phone.SetDeadline(time.Now().Add(handshakeTime))
		if conn.Handshake() != nil {
			return // a phone that gives up, or a client without the pinned certificate, is routine
		}
	}
	phone.SetDeadline(time.Now().Add(connectionTime))
	listener, err := net.DialTimeout("tcp", upstream, handshakeTime)
	if err != nil {
		return
	}
	defer listener.Close()
	listener.SetDeadline(time.Now().Add(connectionTime))
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

func serve(listener net.Listener, upstream string) {
	slots := make(chan struct{}, maxConnections)
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
				relay(phone, upstream)
			}()
		default:
			phone.Close()
		}
	}
}

func run(args []string, stdin io.Reader, stdout io.Writer) error {
	flags := flag.NewFlagSet("kerfkite-pendant-tls", flag.ContinueOnError)
	listen := flags.String("listen", "0.0.0.0:8766", "address for the phone: host:port, port 0 for any free port")
	upstream := flags.String("upstream", "", "the pendant listener of kerfkite-studio, 127.0.0.1:port")
	hosts := flags.String("hosts", "", "comma-separated names and addresses the certificate is for")
	days := flags.Int("days", 30, "days the certificate made for this pairing is valid")
	certFile := flags.String("cert", "", "an operator's PEM certificate, in place of one made for the pairing")
	keyFile := flags.String("key", "", "its PEM private key")
	if err := flags.Parse(args); err != nil {
		return err
	}
	host, _, err := net.SplitHostPort(*upstream)
	if err != nil || (host != "127.0.0.1" && host != "::1" && host != "localhost") {
		return errors.New("give -upstream as the loopback address of the pendant listener")
	}
	if (*certFile == "") != (*keyFile == "") {
		return errors.New("give both -cert and -key, or neither")
	}
	if *days < 1 || *days > 397 {
		return errors.New("-days must be from 1 to 397")
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
		serve(listener, *upstream)
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
