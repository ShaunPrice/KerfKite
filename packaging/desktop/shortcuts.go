package main

// The installers add KerfKite Studio to Start and to the Linux applications
// menu. The launcher covers what an installer cannot: a menu entry for the
// portable Linux archive, a desktop shortcut when the person asks for one, and
// a reminder to move the Mac app off its disk image.

import (
	"bufio"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const appName = "KerfKite Studio"
const portableEntry = "org.kerfkite.Studio.Portable.desktop"
const desktopEntryName = "kerfkite-studio.desktop"

// ownedKey marks the Linux entries this launcher wrote, so it only ever
// rewrites its own files.
const ownedKey = "X-KerfKite-Launcher=shortcut"

// trustLauncher lets GNOME open a desktop entry without asking first. Other
// desktops only need the executable bit, and a failure here is harmless.
var trustLauncher = func(path string) {
	if gio, err := exec.LookPath("gio"); err == nil {
		exec.Command(gio, "set", path, "metadata::trusted", "true").Run()
	}
}

type places struct {
	goos       string
	exe        string // the running launcher, symbolic links resolved
	data       string // XDG data home on Linux
	desktop    string // the desktop folder, empty when there is none
	systemMenu string // an installed menu entry that starts this launcher
}

// What the launcher page offers. DiskImage means a Mac app still running from
// its disk image; Volume is that image, when it can be shown in Finder.
type offers struct {
	Menu, Icon, DiskImage bool
	Volume                string
}

func findPlaces(goos, exe, home string, getenv func(string) string) places {
	p := places{goos: goos, exe: exe}
	if !filepath.IsAbs(home) {
		p.goos = "unknown"
	}
	switch p.goos {
	case "linux":
		p.data = getenv("XDG_DATA_HOME")
		if !filepath.IsAbs(p.data) {
			p.data = filepath.Join(home, ".local", "share")
		}
		p.desktop = linuxDesktop(home, getenv)
		dirs := getenv("XDG_DATA_DIRS")
		if dirs == "" {
			dirs = "/usr/local/share:/usr/share"
		}
		for _, dir := range filepath.SplitList(dirs) {
			entry := filepath.Join(dir, "applications", "org.kerfkite.Studio.desktop")
			if filepath.IsAbs(dir) && launches(entry, exe) {
				p.systemMenu = entry
				break
			}
		}
	case "darwin":
		p.desktop = filepath.Join(home, "Desktop")
	}
	if p.desktop != "" {
		if info, err := os.Stat(p.desktop); err != nil || !info.IsDir() {
			p.desktop = ""
		}
	}
	return p
}

// linuxDesktop follows the XDG user directories: an environment override,
// then user-dirs.dirs, then ~/Desktop. A desktop set to the home folder means
// the session has no desktop.
func linuxDesktop(home string, getenv func(string) string) string {
	if dir := getenv("XDG_DESKTOP_DIR"); filepath.IsAbs(dir) {
		return dir
	}
	config := getenv("XDG_CONFIG_HOME")
	if !filepath.IsAbs(config) {
		config = filepath.Join(home, ".config")
	}
	if f, err := os.Open(filepath.Join(config, "user-dirs.dirs")); err == nil {
		defer f.Close()
		lines := bufio.NewScanner(f)
		for lines.Scan() {
			value, found := strings.CutPrefix(strings.TrimSpace(lines.Text()), "XDG_DESKTOP_DIR=")
			if !found {
				continue
			}
			value = strings.Trim(value, `"`)
			if rest, ok := strings.CutPrefix(value, "$HOME"); ok {
				if strings.Trim(rest, "/") == "" {
					return ""
				}
				return filepath.Join(home, rest)
			}
			if filepath.IsAbs(value) {
				return value
			}
		}
	}
	return filepath.Join(home, "Desktop")
}

func (p places) menuPath() string { return filepath.Join(p.data, "applications", portableEntry) }

func (p places) iconPath() string {
	if p.goos == "darwin" {
		return filepath.Join(p.desktop, appName)
	}
	return filepath.Join(p.desktop, desktopEntryName)
}

// bundle is the Mac app that holds this launcher, or "" outside one.
func (p places) bundle() string {
	app := filepath.Dir(filepath.Dir(filepath.Dir(p.exe)))
	if p.goos != "darwin" || filepath.Ext(app) != ".app" {
		return ""
	}
	return app
}

func (p places) offers() offers {
	var o offers
	switch p.goos {
	case "linux":
		o.Menu = p.systemMenu == "" && !exists(p.menuPath())
		o.Icon = p.desktop != "" && !exists(p.iconPath())
	case "darwin":
		app := p.bundle()
		// A quarantined app opened from the disk image runs from a randomised
		// read-only copy; either way it disappears when the image is ejected.
		o.DiskImage = strings.HasPrefix(app, "/Volumes/") || strings.Contains(app, "/AppTranslocation/")
		if o.DiskImage {
			volume := filepath.Join("/Volumes", appName)
			if rest, ok := strings.CutPrefix(app, "/Volumes/"); ok {
				volume = filepath.Join("/Volumes", strings.Split(rest, "/")[0])
			}
			if info, err := os.Stat(volume); err == nil && info.IsDir() {
				o.Volume = volume
			}
		}
		o.Icon = app != "" && !o.DiskImage && p.desktop != "" && !exists(p.iconPath())
	}
	return o
}

func (p places) addMenu() error {
	if p.goos != "linux" {
		return errors.New("the installer adds KerfKite Studio to this system's applications list")
	}
	if p.systemMenu != "" {
		return nil
	}
	return p.writeEntry(p.menuPath(), 0o644)
}

func (p places) addIcon() error {
	if p.desktop == "" {
		return errors.New("this session has no desktop folder")
	}
	switch p.goos {
	case "linux":
		if err := p.writeEntry(p.iconPath(), 0o755); err != nil {
			return err
		}
		trustLauncher(p.iconPath())
		return nil
	case "darwin":
		app := p.bundle()
		if app == "" || p.offers().DiskImage {
			return errors.New("move KerfKite Studio to Applications first")
		}
		return os.Symlink(app, p.iconPath())
	}
	return errors.New("the installer adds the desktop shortcut on this system")
}

// refresh repoints entries this launcher wrote when the portable folder or the
// Mac app has moved since. Files it did not write are left alone.
func (p places) refresh(output io.Writer) {
	switch p.goos {
	case "linux":
		for path, mode := range map[string]os.FileMode{p.menuPath(): 0o644, p.iconPath(): 0o755} {
			if p.desktop == "" && path == p.iconPath() {
				continue
			}
			if owned(path) && !launches(path, p.exe) {
				if err := p.writeEntry(path, mode); err != nil {
					io.WriteString(output, "Shortcut update failed: "+err.Error()+"\n")
				}
			}
		}
	case "darwin":
		app, link := p.bundle(), p.iconPath()
		if app == "" || p.desktop == "" || p.offers().DiskImage {
			return
		}
		info, err := os.Lstat(link)
		if err != nil || info.Mode()&os.ModeSymlink == 0 {
			return
		}
		target, err := os.Readlink(link)
		if err != nil || target == app || filepath.Base(target) != filepath.Base(app) || exists(target) {
			return
		}
		if os.Remove(link) == nil {
			if err = os.Symlink(app, link); err != nil {
				io.WriteString(output, "Shortcut update failed: "+err.Error()+"\n")
			}
		}
	}
}

func (p places) writeEntry(path string, mode os.FileMode) error {
	text, err := desktopEntry(p.exe, p.icon())
	if err != nil {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	partial := filepath.Join(filepath.Dir(path), "."+filepath.Base(path)+".new")
	if err = os.WriteFile(partial, []byte(text), mode); err != nil {
		return err
	}
	if err = os.Chmod(partial, mode); err == nil {
		err = os.Rename(partial, path)
	}
	if err != nil {
		os.Remove(partial)
	}
	return err
}

func (p places) icon() string {
	picture := filepath.Join(filepath.Dir(p.exe), "kerfkite-studio.png")
	if exists(picture) {
		return picture
	}
	return "kerfkite-studio"
}

func desktopEntry(exe, icon string) (string, error) {
	if strings.ContainsAny(exe+icon, "\x00\n\r") {
		return "", errors.New("the KerfKite folder name contains a line break")
	}
	// GLib checks the program before expanding %% and drops a lone %, so no
	// spelling of a percent sign starts the program.
	if strings.Contains(exe, "%") {
		return "", errors.New("desktop menus cannot start a program from a folder whose name contains %. Rename the folder and open KerfKite again")
	}
	return "[Desktop Entry]\nType=Application\nName=" + appName + "\nComment=Design and simulate CNC jobs\n" +
		"Exec=" + execValue(exe) + "\nIcon=" + escapeValue(icon) + "\nTerminal=false\n" +
		"Categories=Graphics;Engineering;\n" + ownedKey + "\n", nil
}

// execValue quotes a program path for a desktop entry's Exec key: the quoting
// rules first, then the string escape.
func execValue(path string) string {
	var quoted strings.Builder
	quoted.WriteByte('"')
	for _, r := range path {
		if strings.ContainsRune("\"`$\\", r) {
			quoted.WriteByte('\\')
		}
		quoted.WriteRune(r)
	}
	quoted.WriteByte('"')
	return escapeValue(quoted.String())
}

func escapeValue(s string) string { return strings.ReplaceAll(s, `\`, `\\`) }

// launches reports whether a desktop entry starts exe.
func launches(entry, exe string) bool {
	text, err := os.ReadFile(entry)
	if err != nil {
		return false
	}
	program, ok := execProgram(string(text))
	if !ok {
		return false
	}
	if resolved, err := filepath.EvalSymlinks(program); err == nil {
		program = resolved
	}
	return program == exe
}

func execProgram(text string) (string, bool) {
	group := ""
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "[") {
			group = line
			continue
		}
		value, found := strings.CutPrefix(line, "Exec=")
		if group != "[Desktop Entry]" || !found {
			continue
		}
		var unescaped strings.Builder
		for i := 0; i < len(value); i++ {
			if value[i] == '\\' && i+1 < len(value) {
				i++
				switch value[i] {
				case 's':
					unescaped.WriteByte(' ')
				case 'n', 't', 'r':
					return "", false
				default:
					unescaped.WriteByte(value[i])
				}
				continue
			}
			unescaped.WriteByte(value[i])
		}
		command := strings.ReplaceAll(unescaped.String(), "%%", "%")
		if !strings.HasPrefix(command, `"`) {
			program, _, _ := strings.Cut(command, " ")
			return program, program != ""
		}
		var program strings.Builder
		for i := 1; i < len(command); i++ {
			switch command[i] {
			case '\\':
				if i+1 < len(command) {
					i++
					program.WriteByte(command[i])
				}
			case '"':
				return program.String(), true
			default:
				program.WriteByte(command[i])
			}
		}
		return "", false
	}
	return "", false
}

func owned(path string) bool {
	text, err := os.ReadFile(path)
	return err == nil && strings.Contains(string(text), "\n"+ownedKey+"\n")
}

func exists(path string) bool {
	_, err := os.Lstat(path)
	return err == nil
}

// addShortcuts adds what the person asked for and leaves a note on the page.
// It runs outside the state lock: macOS may ask first before a desktop write.
func (s *state) addShortcuts(p places, menu, icon bool, output io.Writer) {
	available := p.offers()
	var notes []string
	if menu && available.Menu {
		if err := p.addMenu(); err != nil {
			io.WriteString(output, "Menu entry failed: "+err.Error()+"\n")
			notes = append(notes, "KerfKite Studio could not be added to the applications menu: "+err.Error()+".")
		} else {
			notes = append(notes, "KerfKite Studio is now in the applications menu. Open it from there next time.")
		}
	}
	if icon && available.Icon {
		if err := p.addIcon(); err != nil {
			io.WriteString(output, "Desktop shortcut failed: "+err.Error()+"\n")
			notes = append(notes, "The desktop shortcut could not be added: "+err.Error()+".")
		} else {
			notes = append(notes, "A KerfKite Studio shortcut is now on the desktop. Double-click it to start KerfKite next time.")
		}
	}
	if len(notes) > 0 {
		s.Lock()
		s.note = strings.Join(notes, " ")
		s.Unlock()
	}
}
