package main

import (
	"bytes"
	"html/template"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestNativeServerNeedsTheServerAndTheEditor(t *testing.T) {
	dir := t.TempDir()
	if nativeServer(dir) != "" {
		t.Fatal("an empty package is not a native one")
	}
	server := filepath.Join(dir, executable("kerfkite-studio"))
	if err := os.WriteFile(server, []byte("#!"), 0755); err != nil {
		t.Fatal(err)
	}
	if nativeServer(dir) != "" {
		t.Fatal("the server without the editor's files")
	}
	if err := os.MkdirAll(filepath.Join(dir, "static"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "static", "index.html"), []byte("<!doctype html>"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := nativeServer(dir); got != server {
		t.Fatalf("server %q, want %q", got, server)
	}
	if stepInstalled(dir) {
		t.Fatal("STEP without its add-on")
	}
	if err := os.WriteFile(filepath.Join(dir, executable("kerfkite-step")), []byte("#!"), 0755); err != nil {
		t.Fatal(err)
	}
	if !stepInstalled(dir) {
		t.Fatal("the STEP add-on beside the server")
	}
	// A directory of that name is not the program.
	other := t.TempDir()
	if err := os.MkdirAll(filepath.Join(other, executable("kerfkite-studio")), 0755); err != nil {
		t.Fatal(err)
	}
	if nativeServer(other) != "" {
		t.Fatal("a directory is not the server")
	}
}

func TestNativeArguments(t *testing.T) {
	payload := filepath.Join("pkg", "Resources")
	if got, want := nativeArguments(payload, false), []string{"--port", "18765", "--static", filepath.Join(payload, "static")}; !reflect.DeepEqual(got, want) {
		t.Fatalf("simulation %v, want %v", got, want)
	}
	if got := nativeArguments(payload, true); got[len(got)-1] != "--enable-hardware" {
		t.Fatalf("hardware %v", got)
	}
}

func TestLauncherPageOffersStepOnlyToThePythonService(t *testing.T) {
	render := func(native, step bool) string {
		var out bytes.Buffer
		data := map[string]any{"Phase": "Choose your workshop", "Detail": "", "Active": true, "Choose": true, "Ready": false, "Token": "t", "Studio": studioURL, "Log": "setup.log", "Offers": offers{}, "First": false, "Note": "", "Native": native, "Step": step}
		if err := template.Must(template.New("t").Parse(page)).Execute(&out, data); err != nil {
			t.Fatal(err)
		}
		return out.String()
	}
	if python := render(false, false); !strings.Contains(python, `name="step" value="yes"`) {
		t.Fatal("the Python service installs STEP on request")
	}
	if with := render(true, true); strings.Contains(with, `name="step"`) || !strings.Contains(with, "STEP import and export is installed.") {
		t.Fatal("a native package with the add-on")
	}
	if without := render(true, false); strings.Contains(without, `name="step"`) || !strings.Contains(without, "was not installed") {
		t.Fatal("a native package without the add-on")
	}
}
