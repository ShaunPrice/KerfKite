package main

import (
	"bytes"
	"html/template"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func environment(values map[string]string) func(string) string {
	return func(key string) string { return values[key] }
}

func TestExecValueRoundTrips(t *testing.T) {
	for _, path := range []string{
		"/opt/kerfkite/KerfKite",
		"/home/sam/My Apps/kerfkite/KerfKite",
		`/home/sam/odd "quoted" $HOME ` + "`tick`" + ` back\slash/KerfKite`,
		"/home/sam/Téléchargements/kerfkite/KerfKite",
	} {
		text, err := desktopEntry(path, "kerfkite-studio")
		if err != nil {
			t.Fatal(err)
		}
		program, ok := execProgram(text)
		if !ok || program != path {
			t.Fatalf("%q came back as %q (%v) from\n%s", path, program, ok, text)
		}
	}
	if program, ok := execProgram("[Desktop Entry]\nExec=/opt/kerfkite/KerfKite %U\n"); !ok || program != "/opt/kerfkite/KerfKite" {
		t.Fatalf("unquoted Exec read as %q", program)
	}
	if program, ok := execProgram("[Desktop Action Other]\nExec=/bin/false\n"); ok {
		t.Fatalf("Exec outside the main group was read: %q", program)
	}
	for _, path := range []string{"/home/sam/bad\nname/KerfKite", "/home/sam/100%/KerfKite"} {
		if _, err := desktopEntry(path, "icon"); err == nil {
			t.Fatalf("%q was accepted", path)
		}
	}
}

func TestLinuxDesktopFolder(t *testing.T) {
	home := t.TempDir()
	config := filepath.Join(home, ".config")
	os.MkdirAll(config, 0o755)
	if got := linuxDesktop(home, environment(nil)); got != filepath.Join(home, "Desktop") {
		t.Fatalf("default desktop %q", got)
	}
	os.WriteFile(filepath.Join(config, "user-dirs.dirs"), []byte("# comment\nXDG_DESKTOP_DIR=\"$HOME/Bureau\"\n"), 0o644)
	if got := linuxDesktop(home, environment(nil)); got != filepath.Join(home, "Bureau") {
		t.Fatalf("localised desktop %q", got)
	}
	os.WriteFile(filepath.Join(config, "user-dirs.dirs"), []byte("XDG_DESKTOP_DIR=\"$HOME/\"\n"), 0o644)
	if got := linuxDesktop(home, environment(nil)); got != "" {
		t.Fatalf("a desktop set to home should mean none, got %q", got)
	}
	if got := linuxDesktop(home, environment(map[string]string{"XDG_DESKTOP_DIR": "/srv/desk"})); got != "/srv/desk" {
		t.Fatalf("override ignored: %q", got)
	}
}

func TestPortableLinuxShortcuts(t *testing.T) {
	trusted := []string{}
	defer func(previous func(string)) { trustLauncher = previous }(trustLauncher)
	trustLauncher = func(path string) { trusted = append(trusted, path) }
	home := t.TempDir()
	folder := filepath.Join(home, "Apps", "kerf kite")
	os.MkdirAll(folder, 0o755)
	os.MkdirAll(filepath.Join(home, "Desktop"), 0o755)
	exe := filepath.Join(folder, "KerfKite")
	os.WriteFile(exe, nil, 0o755)
	os.WriteFile(filepath.Join(folder, "kerfkite-studio.png"), nil, 0o644)
	env := environment(map[string]string{"XDG_DATA_DIRS": filepath.Join(home, "system")})
	p := findPlaces("linux", exe, home, env)
	if o := p.offers(); !o.Menu || !o.Icon || o.DiskImage {
		t.Fatalf("offers %+v", o)
	}
	st := &state{}
	st.addShortcuts(p, true, true, io.Discard)
	if !strings.Contains(st.note, "applications menu") || !strings.Contains(st.note, "desktop") {
		t.Fatalf("note %q", st.note)
	}
	menu := filepath.Join(home, ".local", "share", "applications", portableEntry)
	icon := filepath.Join(home, "Desktop", desktopEntryName)
	for path, mode := range map[string]os.FileMode{menu: 0o644, icon: 0o755} {
		info, err := os.Stat(path)
		if err != nil || info.Mode().Perm() != mode || !launches(path, exe) || !owned(path) {
			t.Fatalf("%s: %v %v", path, info, err)
		}
		text, _ := os.ReadFile(path)
		if !strings.Contains(string(text), "Icon="+filepath.Join(folder, "kerfkite-studio.png")+"\n") {
			t.Fatalf("icon missing from\n%s", text)
		}
	}
	if len(trusted) != 1 || trusted[0] != icon {
		t.Fatalf("trusted %v", trusted)
	}
	if o := p.offers(); o.Menu || o.Icon {
		t.Fatalf("offers after adding %+v", o)
	}

	// The folder moves: the launcher repoints its own entries and nothing else.
	moved := filepath.Join(home, "Elsewhere", "KerfKite")
	os.MkdirAll(filepath.Dir(moved), 0o755)
	os.WriteFile(moved, nil, 0o755)
	foreign := filepath.Join(home, "Desktop", "other.desktop")
	os.WriteFile(foreign, []byte("[Desktop Entry]\nExec=/somewhere/else\n"), 0o755)
	q := findPlaces("linux", moved, home, env)
	q.refresh(io.Discard)
	if !launches(menu, moved) || !launches(icon, moved) || !launches(foreign, "/somewhere/else") {
		t.Fatal("refresh did not repoint the launcher's entries")
	}
}

func TestInstalledLinuxPackage(t *testing.T) {
	home := t.TempDir()
	system := filepath.Join(home, "usr", "share")
	exe := filepath.Join(home, "opt", "kerfkite", "KerfKite")
	os.MkdirAll(filepath.Dir(exe), 0o755)
	os.WriteFile(exe, nil, 0o755)
	os.MkdirAll(filepath.Join(system, "applications"), 0o755)
	os.WriteFile(filepath.Join(system, "applications", "org.kerfkite.Studio.desktop"),
		[]byte("[Desktop Entry]\nType=Application\nName=KerfKite Studio\nExec="+exe+"\nIcon=kerfkite-studio\n"), 0o644)
	p := findPlaces("linux", exe, home, environment(map[string]string{"XDG_DATA_DIRS": "relative/ignored:" + system}))
	if p.systemMenu == "" {
		t.Fatal("the package's menu entry was not found")
	}
	if o := p.offers(); o.Menu || o.Icon {
		t.Fatalf("offers without a desktop folder %+v", o)
	}
	os.MkdirAll(filepath.Join(home, "Desktop"), 0o755)
	p = findPlaces("linux", exe, home, environment(map[string]string{"XDG_DATA_DIRS": system}))
	if o := p.offers(); o.Menu || !o.Icon {
		t.Fatalf("offers %+v", o)
	}
	st := &state{}
	st.addShortcuts(p, true, true, io.Discard)
	if exists(p.menuPath()) {
		t.Fatal("a second menu entry was written beside the package's")
	}
	if !launches(p.iconPath(), exe) {
		t.Fatal("desktop shortcut missing")
	}
}

func TestMacShortcuts(t *testing.T) {
	if p := findPlaces("darwin", "/Volumes/KerfKite Studio/KerfKite Studio.app/Contents/MacOS/KerfKite", "/Users/sam", environment(nil)); !p.offers().DiskImage || p.offers().Icon {
		t.Fatalf("disk image not recognised: %+v", p.offers())
	}
	translocated := "/private/var/folders/xy/T/AppTranslocation/0A1B/d/KerfKite Studio.app/Contents/MacOS/KerfKite"
	if p := findPlaces("darwin", translocated, "/Users/sam", environment(nil)); !p.offers().DiskImage {
		t.Fatal("translocated copy not recognised")
	}
	home := t.TempDir()
	os.MkdirAll(filepath.Join(home, "Desktop"), 0o755)
	app := filepath.Join(home, "Applications", appName+".app")
	exe := filepath.Join(app, "Contents", "MacOS", "KerfKite")
	os.MkdirAll(filepath.Dir(exe), 0o755)
	p := findPlaces("darwin", exe, home, environment(nil))
	if o := p.offers(); o.DiskImage || !o.Icon || o.Menu {
		t.Fatalf("offers %+v", o)
	}
	if err := p.addMenu(); err == nil {
		t.Fatal("the Mac has no launcher-made menu entry")
	}
	if err := p.addIcon(); err != nil {
		t.Fatal(err)
	}
	if target, err := os.Readlink(filepath.Join(home, "Desktop", appName)); err != nil || target != app {
		t.Fatalf("link %q %v", target, err)
	}
	// The app moves to /Applications: the dangling link follows it.
	moved := filepath.Join(home, "System Applications", appName+".app")
	os.MkdirAll(filepath.Join(moved, "Contents", "MacOS"), 0o755)
	os.RemoveAll(app)
	findPlaces("darwin", filepath.Join(moved, "Contents", "MacOS", "KerfKite"), home, environment(nil)).refresh(io.Discard)
	if target, _ := os.Readlink(filepath.Join(home, "Desktop", appName)); target != moved {
		t.Fatalf("link not repointed: %q", target)
	}
}

func TestWindowsLeavesShortcutsToTheInstaller(t *testing.T) {
	p := findPlaces("windows", `C:\Users\sam\AppData\Local\Programs\KerfKite Studio\KerfKite.exe`, `C:\Users\sam`, environment(nil))
	if o := p.offers(); o != (offers{}) {
		t.Fatalf("offers %+v", o)
	}
}

func TestLauncherPageShowsShortcutChoices(t *testing.T) {
	render := func(data map[string]any) string {
		var out bytes.Buffer
		if err := template.Must(template.New("t").Parse(page)).Execute(&out, data); err != nil {
			t.Fatal(err)
		}
		return out.String()
	}
	base := func(extra map[string]any) map[string]any {
		data := map[string]any{"Phase": "Choose your workshop", "Detail": "", "Active": true, "Choose": true, "Ready": false, "Token": "t", "Studio": studioURL, "Log": "setup.log", "Offers": offers{Menu: true, Icon: true}, "First": true, "Note": ""}
		for k, v := range extra {
			data[k] = v
		}
		return data
	}
	first := render(base(nil))
	for _, want := range []string{`name="shortcuts" value="offered"`, `name="menu" value="yes" checked`, `name="icon" value="yes" checked`} {
		if !strings.Contains(first, want) {
			t.Fatalf("first launch page lacks %s", want)
		}
	}
	later := render(base(map[string]any{"First": false}))
	if strings.Contains(later, `name="shortcuts"`) {
		t.Fatal("the choice was offered again")
	}
	ready := render(base(map[string]any{"Phase": "Your workshop is ready", "Choose": false, "Ready": true, "First": false, "Note": "A shortcut <is> on the desktop."}))
	for _, want := range []string{`value="add-menu"`, `value="add-icon"`, "A shortcut &lt;is&gt; on the desktop."} {
		if !strings.Contains(ready, want) {
			t.Fatalf("ready page lacks %s", want)
		}
	}
	mac := render(base(map[string]any{"Offers": offers{DiskImage: true, Volume: "/Volumes/KerfKite Studio"}}))
	if !strings.Contains(mac, "Move KerfKite Studio to Applications") || !strings.Contains(mac, `value="show-volume"`) || strings.Contains(mac, `name="shortcuts"`) {
		t.Fatal("disk image guidance missing")
	}
}
