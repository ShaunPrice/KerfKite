// KerfKite desktop bootstrapper. Build with the Go standard library only.
package main

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha256"
	_ "embed"
	"encoding/json"
	"fmt"
	"html/template"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"sync"
	"time"
)

//go:embed launcher.html
var page string

const address = "127.0.0.1:18764"
const studioURL = "http://127.0.0.1:18765/"

var form = template.Must(template.New("launcher").Parse(page))

type options struct{ hardware, step, desktop, remember bool }

type state struct {
	choices  chan options
	physical bool
	sync.Mutex
	phase, detail string
	active        bool
	desktopOpen   bool
	automatic     bool
}

func (s *state) set(phase, detail string) {
	s.Lock()
	defer s.Unlock()
	s.phase, s.detail = phase, detail
}

func browser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	if err := cmd.Run(); err != nil {
		log.Print(err)
	}
}

// Keep one editable document window per launcher. Closing its window leaves the service running.
func (s *state) openDesktop(payload string, output *os.File) {
	s.Lock()
	defer s.Unlock()
	if s.phase != "Your workshop is ready" || s.desktopOpen {
		return
	}
	name := filepath.Join(payload, "kerfkite-native")
	switch runtime.GOOS {
	case "darwin":
		name = filepath.Join(payload, "..", "MacOS", "KerfKiteNative")
	case "windows":
		name = filepath.Join(payload, "KerfKiteNative.exe")
	}
	cmd := exec.Command(name, studioURL)
	cmd.Stdout, cmd.Stderr = output, output
	err := cmd.Start()
	if err != nil {
		if s.automatic {
			go browser("http://" + address)
		}
		fmt.Fprintln(output, err)
		s.detail = "The desktop window could not start. Choose Open in browser, or reinstall the desktop package."
		return
	}
	s.desktopOpen = true
	go func() {
		err := cmd.Wait()
		s.Lock()
		defer s.Unlock()
		s.desktopOpen = false
		if s.automatic {
			go browser("http://" + address)
		}
		if err != nil {
			fmt.Fprintln(output, err)
			s.detail = "The desktop window closed with an error. Choose Open in browser or check the desktop runtime in the installation guide."
		}
	}()
}

func runStudio(ctx context.Context, s *state, payload, data string, output *os.File, showSetup func()) error {
	// A release gets its own environment. An interrupted setup retries on the next launch.
	wheel, err := filepath.Glob(filepath.Join(payload, "kerfkite_studio-*.whl"))
	if err != nil || len(wheel) != 1 {
		return fmt.Errorf("the application files are incomplete. Reinstall KerfKite")
	}
	hash := sha256.New()
	lock := filepath.Join(payload, "requirements.lock")
	for _, name := range []string{wheel[0], lock, filepath.Join(payload, "step.lock")} {
		f, e := os.Open(name)
		if e != nil {
			return e
		}
		_, e = io.Copy(hash, f)
		f.Close()
		if e != nil {
			return e
		}
	}
	env := filepath.Join(data, fmt.Sprintf("runtime-%s-%x", runtimePlatform, hash.Sum(nil)[:8]))
	python := filepath.Join(env, "bin", "python")
	var uv string
	if runtime.GOOS == "windows" {
		python = filepath.Join(env, "Scripts", "python.exe")
	}
	invoke := func(args ...string) error {
		cmd := exec.CommandContext(ctx, uv, args...)
		configure(cmd)
		cmd.Dir = data
		cmd.Env = append(os.Environ(), "UV_PYTHON_INSTALL_DIR="+filepath.Join(data, "python", runtimePlatform), "UV_CACHE_DIR="+filepath.Join(data, "cache"), "UV_NO_CONFIG=1", "UV_NO_PROGRESS=1", "UV_PYTHON_PREFERENCE=only-managed", "UV_PYTHON_INSTALL_BIN=0")
		cmd.Stdout, cmd.Stderr = output, output
		return cmd.Run()
	}
	marker := filepath.Join(env, ".installed")
	preference := filepath.Join(data, "native-simulation")
	_, prefErr := os.Stat(preference)
	automatic := prefErr == nil

	if _, err = os.Stat(marker); os.IsNotExist(err) {
		automatic = false
		showSetup()
		s.set("Downloading the setup tools", "The first launch needs an internet connection.")
		uv, err = setupTool(ctx, data)
		if err != nil {
			return err
		}
		s.set("Preparing Python", "The first launch downloads a private Python runtime. Keep this window open.")
		if err = invoke("venv", "--allow-existing", "--python", "3.12", env); err != nil {
			return fmt.Errorf("Python setup failed: %w", err)
		}
		s.set("Installing the design tools", "Downloading the geometry and machining libraries. This can take several minutes.")
		if err = invoke("pip", "install", "--python", python, "--require-hashes", "--only-binary", ":all:", "-r", lock); err != nil {
			return fmt.Errorf("library setup failed: %w", err)
		}
		if err = invoke("pip", "install", "--python", python, "--no-deps", wheel[0]); err != nil {
			return fmt.Errorf("Studio setup failed: %w", err)
		}
		if err = os.WriteFile(marker, []byte("ready\n"), 0600); err != nil {
			return err
		}
	} else if err != nil {
		return err
	}
	var choice options
	if automatic {
		choice = options{desktop: true}
	} else {
		s.set("Choose your workshop", "Start in simulation to design, preview and test your project.")
		showSetup()
		select {
		case <-ctx.Done():
			return ctx.Err()
		case choice = <-s.choices:
		}
	}
	s.Lock()
	s.automatic = automatic
	s.Unlock()
	if choice.step {
		stepMarker := filepath.Join(env, ".step-installed")
		if _, e := os.Stat(stepMarker); os.IsNotExist(e) {
			s.set("Adding STEP exchange", "Downloading the optional CAD exchange libraries.")
			uv, err = setupTool(ctx, data)
			if err != nil {
				return err
			}
			if err = invoke("pip", "install", "--python", python, "--require-hashes", "--only-binary", ":all:", "-r", filepath.Join(payload, "step.lock")); err != nil {
				return fmt.Errorf("STEP setup failed. Retry without STEP to open the workshop: %w", err)
			}
			if err = os.WriteFile(stepMarker, []byte("ready\n"), 0600); err != nil {
				return err
			}
		} else if e != nil {
			return e
		}
	}
	s.set("Opening the workshop", "Starting Studio on this computer.")
	args := []string{"-I", "-m", "studio.desktop_server"}
	if choice.hardware {
		args = append(args, "--enable-hardware")
	}
	cmd := exec.CommandContext(ctx, python, args...)
	configure(cmd)
	cmd.Dir = data
	cmd.Stderr = output
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err = cmd.Start(); err != nil {
		return err
	}
	lines := bufio.NewScanner(pipe)
	for lines.Scan() {
		if lines.Text() == "KERFKITE_READY" {
			s.Lock()
			s.physical = choice.hardware
			s.Unlock()
			detail := "Simulation is active. Return to the launcher to reopen the window or quit."
			if choice.hardware {
				detail = "Controller access is enabled. Connect the machine explicitly in Studio."
			}
			if choice.remember && choice.desktop && !choice.hardware {
				if err := os.WriteFile(preference, []byte("simulation\n"), 0600); err != nil {
					fmt.Fprintln(output, err)
				}
			} else if !automatic {
				if err := os.Remove(preference); err != nil && !os.IsNotExist(err) {
					fmt.Fprintln(output, err)
				}
			}
			s.set("Your workshop is ready", detail)
			if choice.desktop {
				s.openDesktop(payload, output)
			}
		} else {
			fmt.Fprintln(output, lines.Text())
		}
	}
	if err = cmd.Wait(); err != nil {
		return fmt.Errorf("Studio could not run. Check whether port 18765 is already in use: %w", err)
	}
	return nil
}

func disconnected() bool {
	resp, err := (&http.Client{Timeout: 2 * time.Second}).Get(studioURL + "api/status")
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	var info struct{ Connected *bool }
	return resp.StatusCode == 200 && json.NewDecoder(resp.Body).Decode(&info) == nil && info.Connected != nil && !*info.Connected
}

func main() {
	if stop, code := windowsStartup(os.Args[1:], runtime.GOOS == "windows", approvedWindowsClient, showWindowsRequirement); stop {
		os.Exit(code)
	}
	ex, err := os.Executable()
	if err != nil {
		return
	}
	payload := filepath.Dir(ex)
	if runtime.GOOS == "darwin" {
		payload = filepath.Join(payload, "..", "Resources")
	}
	home, err := os.UserConfigDir()
	if err != nil {
		return
	}
	data := filepath.Join(home, "KerfKite Studio")
	if runtime.GOOS == "windows" && os.Getenv("LOCALAPPDATA") != "" {
		data = filepath.Join(os.Getenv("LOCALAPPDATA"), "KerfKite Studio", "Data")
	}
	if custom := os.Getenv("KERFKITE_DATA_DIR"); custom != "" && filepath.IsAbs(custom) {
		data = custom
	}
	if err = os.MkdirAll(data, 0700); err != nil {
		return
	}
	ln, err := net.Listen("tcp4", address)
	if err != nil {
		client := http.Client{Timeout: time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
		resp, e := client.Get("http://" + address + "/identity")
		if e == nil {
			defer resp.Body.Close()
			if resp.Header.Get("KerfKite-Launcher") == "1" {
				browser("http://" + address)
				return
			}
		}
		// Report a launcher port conflict in the browser without starting another installer.
		ln, e = net.Listen("tcp4", "127.0.0.1:0")
		if e != nil {
			return
		}
		url := "http://" + ln.Addr().String()
		go browser(url)
		srv := &http.Server{ReadHeaderTimeout: 5 * time.Second, Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "KerfKite cannot start because port 18764 is in use. Close the other application and open KerfKite again.")
		})}
		time.AfterFunc(time.Minute, func() { srv.Close() })
		srv.Serve(ln)
		return
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()
	logfile := filepath.Join(data, "setup.log")
	output, err := os.OpenFile(logfile, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0600)
	if err != nil {
		return
	}
	defer output.Close()
	var secret [24]byte
	if _, err = rand.Read(secret[:]); err != nil {
		return
	}
	token := fmt.Sprintf("%x", secret[:])
	st := &state{choices: make(chan options, 1), phase: "Starting KerfKite"}
	var worker sync.WaitGroup
	start := func() {
		st.Lock()
		if st.active {
			st.Unlock()
			return
		}
		st.active = true
		st.phase = "Starting KerfKite"
		st.Unlock()
		worker.Add(1)
		go func() {
			defer worker.Done()
			var setupOnce sync.Once
			showSetup := func() { setupOnce.Do(func() { go browser("http://" + address) }) }
			err := runStudio(ctx, st, payload, data, output, showSetup)
			if err != nil && ctx.Err() == nil {
				showSetup()
			}
			st.Lock()
			defer st.Unlock()
			st.active = false
			st.physical = false
			if err != nil {
				fmt.Fprintln(output, err)
				st.phase = "Setup needs attention"
				st.detail = err.Error()
			} else {
				st.phase = "Studio has stopped"
				st.detail = "Choose Retry to reopen."
			}
		}()
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/identity", func(w http.ResponseWriter, r *http.Request) { w.Header().Set("KerfKite-Launcher", "1") })
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		if r.Method == "POST" {
			if r.Header.Get("Origin") != "http://"+address || r.FormValue("token") != token {
				http.Error(w, "Reload the launcher.", 403)
				return
			}
			switch r.FormValue("action") {
			case "launch":
				st.Lock()
				if st.phase == "Choose your workshop" {
					st.phase = "Opening the workshop"
					st.choices <- options{hardware: r.FormValue("hardware") == "yes", step: r.FormValue("step") == "yes", desktop: r.FormValue("interface") == "native", remember: r.FormValue("remember") == "yes"}
				}
				st.Unlock()
			case "setup-next":
				err := os.Remove(filepath.Join(data, "native-simulation"))
				if err != nil && !os.IsNotExist(err) {
					http.Error(w, "Could not save the startup preference", 500)
					return
				}
				st.Lock()
				st.detail = "Setup will open the next time you start KerfKite."
				st.Unlock()
			case "desktop":
				st.openDesktop(payload, output)
			case "quit":
				st.Lock()
				if st.desktopOpen {
					st.Unlock()
					http.Error(w, "Close the desktop document window first, then return here to quit KerfKite.", 409)
					return
				}
				physical := st.physical
				st.Unlock()
				if physical && !disconnected() {
					http.Error(w, "Disconnect the controller in Studio before quitting, then return to the launcher.", 409)
					return
				}

				fmt.Fprint(w, "KerfKite is closing. You can close this tab.")
				go cancel()
				return
			case "retry":
				start()
			default:
				http.Error(w, "Unknown action", 400)
				return
			}
			http.Redirect(w, r, "/", 303)
			return
		}
		if r.Method != "GET" {
			http.Error(w, "Unsupported method", 405)
			return
		}
		st.Lock()
		defer st.Unlock()
		form.Execute(w, map[string]any{"Phase": st.phase, "Detail": st.detail, "Active": st.active, "Choose": st.phase == "Choose your workshop", "Ready": st.phase == "Your workshop is ready", "Token": token, "Studio": studioURL, "Log": logfile})
	})
	srv := &http.Server{ReadHeaderTimeout: 5 * time.Second, Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Host != address {
			http.Error(w, "Local access only", 403)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
		r.Body = http.MaxBytesReader(w, r.Body, 2048)
		mux.ServeHTTP(w, r)
	})}
	start()
	go func() {
		<-ctx.Done()
		stop, done := context.WithTimeout(context.Background(), 3*time.Second)
		defer done()
		srv.Shutdown(stop)
	}()
	srv.Serve(ln)
	cancel()
	worker.Wait()
}
