package main

import (
	"os"
	"path/filepath"
	"runtime"
)

// A package of the workstation server without Python (native/server) carries kerfkite-studio beside the
// launcher, the editor's files in static/ and, when STEP exchange was installed with it, the Open CASCADE
// add-on kerfkite-step. Such a package needs no Python runtime and nothing downloaded on its first start.

func executable(name string) string {
	if runtime.GOOS == "windows" {
		return name + ".exe"
	}
	return name
}

func regularFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func directory(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

// nativeServer is the package's C++ workstation server, or "" for a package that installs the Python service.
func nativeServer(payload string) string {
	server := filepath.Join(payload, executable("kerfkite-studio"))
	if !regularFile(server) || !regularFile(filepath.Join(payload, "static", "index.html")) {
		return ""
	}
	return server
}

// stepInstalled says whether the package carries the STEP add-on, which the server finds beside itself.
func stepInstalled(payload string) bool {
	return regularFile(filepath.Join(payload, executable("kerfkite-step")))
}

// nativeArguments starts the server as studio.desktop_server starts the Python service: the launcher's
// port, the simulator unless the operator enabled the controller ports for this session.
func nativeArguments(payload string, hardware bool) []string {
	args := []string{"--port", "18765", "--static", filepath.Join(payload, "static")}
	if hardware {
		args = append(args, "--enable-hardware")
	}
	return args
}
