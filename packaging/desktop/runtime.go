package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"time"
)

const uvVersion = "0.13.1"

type asset struct{ name, digest string }

var binaries = map[string]asset{
	"darwin/arm64":  {"uv-aarch64-apple-darwin.tar.gz", "cfe454573412ff7380d15fc7dae5997ab53ffce72fce0a78e91ddbc84001acd2"},
	"darwin/amd64":  {"uv-x86_64-apple-darwin.tar.gz", "bb7b805bdcf3d0bc8c74ceb94569e6b3e8aa35bd492acdbb9d46bf46d9d4ff6b"},
	"linux/arm64":   {"uv-aarch64-unknown-linux-gnu.tar.gz", "9041c73c99fdc5892cf128140beb354eca9231d502068cc36e6cb53a8e59ad14"},
	"linux/amd64":   {"uv-x86_64-unknown-linux-gnu.tar.gz", "eadd9d27c134ab5ac1ffd4473b97a741407f3ea238288f03121fc4deddf90220"},
	"windows/amd64": {"uv-x86_64-pc-windows-msvc.zip", "3b2661a463a5baea7f5076ea2b25ab71d77f36a6c0d809e5350651792f80a672"},
}

func unpack(f *os.File, size int64, name string, out io.Writer) error {
	if name == "uv.exe" {
		z, e := zip.NewReader(f, size)
		if e != nil {
			return e
		}
		for _, entry := range z.File {
			if path.Base(entry.Name) == name {
				r, e := entry.Open()
				if e != nil {
					return e
				}
				defer r.Close()
				_, e = io.Copy(out, r)
				return e
			}
		}
	} else {
		if _, e := f.Seek(0, 0); e != nil {
			return e
		}
		g, e := gzip.NewReader(f)
		if e != nil {
			return e
		}
		defer g.Close()
		t := tar.NewReader(g)
		for {
			h, e := t.Next()
			if e == io.EOF {
				break
			}
			if e != nil {
				return e
			}
			if h.Typeflag == tar.TypeReg && path.Base(h.Name) == name {
				_, e = io.Copy(out, t)
				return e
			}
		}
	}
	return fmt.Errorf("missing %s in the installer download", name)
}
func setupTool(ctx context.Context, dir string) (string, error) {
	a := binaries[runtime.GOOS+"/"+runtime.GOARCH]
	if a.name == "" {
		return "", fmt.Errorf("unsupported platform")
	}
	name := "uv"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	dir = filepath.Join(dir, "tools", uvVersion)
	dest := filepath.Join(dir, name)
	if _, e := os.Stat(dest); e == nil {
		return dest, nil
	}
	if e := os.MkdirAll(dir, 0700); e != nil {
		return "", e
	}
	f, e := os.CreateTemp(dir, "download-*")
	if e != nil {
		return "", e
	}
	defer func() { f.Close(); os.Remove(f.Name()) }()
	req, e := http.NewRequestWithContext(ctx, "GET", "https://github.com/astral-sh/uv/releases/download/"+uvVersion+"/"+a.name, nil)
	if e != nil {
		return "", e
	}
	resp, e := (&http.Client{Timeout: 10 * time.Minute}).Do(req)
	if e != nil {
		return "", e
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("runtime download returned HTTP %d", resp.StatusCode)
	}
	hash := sha256.New()
	size, e := io.Copy(io.MultiWriter(f, hash), io.LimitReader(resp.Body, 100<<20))
	if e != nil {
		return "", e
	}
	if fmt.Sprintf("%x", hash.Sum(nil)) != a.digest {
		return "", fmt.Errorf("runtime download failed its SHA-256 check")
	}
	bin, e := os.CreateTemp(dir, "install-*")
	if e != nil {
		return "", e
	}
	defer func() { bin.Close(); os.Remove(bin.Name()) }()
	if e = unpack(f, size, name, bin); e != nil {
		return "", e
	}
	if e = bin.Chmod(0700); e != nil {
		return "", e
	}
	if e = bin.Close(); e != nil {
		return "", e
	}
	return dest, os.Rename(bin.Name(), dest)
}
