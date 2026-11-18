package main

import (
	"context"
	"os/exec"
	"path/filepath"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

// RTL_OSVERSIONINFOEXW fits OSVERSIONINFOEXW; the last byte is reserved.
type rtlVersionInfo struct {
	size, major, minor, build, platform       uint32
	servicePack                               [128]uint16
	servicePackMajor, servicePackMinor, suite uint16
	productType, reserved                     byte
}

func approvedWindowsClient() bool {
	if runtime.GOARCH != "amd64" {
		return false
	}
	var version rtlVersionInfo
	version.size = uint32(unsafe.Sizeof(version))
	rtlGetVersion := syscall.NewLazyDLL("ntdll.dll").NewProc("RtlGetVersion")
	if rtlGetVersion.Find() != nil {
		return false
	}
	status, _, _ := rtlGetVersion.Call(uintptr(unsafe.Pointer(&version)))
	if status != 0 || version.platform != 2 { // VER_PLATFORM_WIN32_NT
		return false
	}
	kernel := syscall.NewLazyDLL("kernel32.dll")
	var processMachine, nativeMachine uint16
	probe := kernel.NewProc("IsWow64Process2")
	if probe.Find() == nil {
		ok, _, _ := probe.Call(^uintptr(0), uintptr(unsafe.Pointer(&processMachine)), uintptr(unsafe.Pointer(&nativeMachine)))
		if ok == 0 {
			return false
		}
	} else {
		// Windows 10 before 1709 predates IsWow64Process2 and x64-on-ARM emulation.
		// Never use GetNativeSystemInfo's compatibility view on newer releases.
		legacy := kernel.NewProc("GetNativeSystemInfo")
		if version.major != 10 || version.minor != 0 || version.build >= 16299 || legacy.Find() != nil {
			return false
		}
		// SYSTEM_INFO starts with wProcessorArchitecture. Use pointer-aligned storage.
		var info [8]uintptr
		legacy.Call(uintptr(unsafe.Pointer(&info[0])))
		if uint16(info[0]) != 9 { // PROCESSOR_ARCHITECTURE_AMD64
			return false
		}
		nativeMachine = 0x8664
	}
	return supportedWindowsClient(version.major, version.minor, version.build, version.productType, processMachine, nativeMachine, windowsCaption)
}

// Only unfamiliar NT 10.0 client builds need WMI. Use the system's PowerShell and
// the read-only local WMI provider; never trust registry display names or PATH.
func windowsCaption() string {
	var directory [32768]uint16
	system := syscall.NewLazyDLL("kernel32.dll").NewProc("GetSystemDirectoryW")
	if system.Find() != nil {
		return ""
	}
	length, _, _ := system.Call(uintptr(unsafe.Pointer(&directory[0])), uintptr(len(directory)))
	if length == 0 || length >= uintptr(len(directory)) {
		return ""
	}
	base := syscall.UTF16ToString(directory[:length])
	shell := filepath.Join(base, "WindowsPowerShell", "v1.0", "powershell.exe")
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	const wmi = `$ErrorActionPreference='Stop'; [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $query=[System.Management.ManagementObjectSearcher]::new('SELECT Caption FROM Win32_OperatingSystem'); foreach ($os in $query.Get()) { [Console]::WriteLine($os['Caption']) }`
	cmd := exec.CommandContext(ctx, shell, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", wmi)
	configure(cmd)
	cmd.Dir = base
	output, err := cmd.Output()
	if err != nil {
		return ""
	}
	return string(output)
}

func showWindowsRequirement() {
	show := syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW")
	if show.Find() == nil {
		message, _ := syscall.UTF16PtrFromString(windowsRequirement)
		title, _ := syscall.UTF16PtrFromString("KerfKite Studio")
		show.Call(0, uintptr(unsafe.Pointer(message)), uintptr(unsafe.Pointer(title)), 0x10)
	}
}
