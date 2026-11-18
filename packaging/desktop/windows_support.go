package main

import (
	"regexp"
	"strings"
)

var windowsClientCaption = regexp.MustCompile(`(?i)^(?:Microsoft\s+)?Windows\s+(?:10|11)(?:\s|$)`)

// ERROR_OLD_WIN_VERSION. Setup and both Windows entry points return this on rejection.
const unsupportedWindows = 1150

const windowsRequirement = "KerfKite Studio requires a native x64 client edition of Windows 10 or Windows 11. This computer does not meet that requirement, or Windows could not confirm it."

func supportedWindowsClient(major, minor, build uint32, productType byte, processMachine, nativeMachine uint16, getCaption func() string) bool {
	// VER_NT_WORKSTATION, IMAGE_FILE_MACHINE_UNKNOWN (native process), IMAGE_FILE_MACHINE_AMD64.
	if major != 10 || minor != 0 || build < 10240 || productType != 1 || processMachine != 0 || nativeMachine != 0x8664 {
		return false
	}
	// Windows 10 and Windows 11 both report NT 10.0 and share build numbers with Server.
	// Compare the release build, independent of the cumulative-update revision.
	// https://learn.microsoft.com/windows/release-health/release-information
	// https://learn.microsoft.com/windows/release-health/windows11-release-information
	switch build {
	case 10240, 10586, 14393, 15063, 16299, 17134, 17763, 18362, 18363, 19041, 19042, 19043, 19044, 19045,
		22000, 22621, 22631, 26100, 26200, 26300, 28000:
		return true
	}
	// Insider and new release builds qualify only when WMI identifies the product.
	return windowsClientCaption.MatchString(strings.TrimSpace(getCaption()))
}

// A query never starts setup. A normal Windows launch cannot proceed without a positive probe.
func windowsStartup(args []string, windows bool, check func() bool, explain func()) (stop bool, code int) {
	query := len(args) == 1 && args[0] == "--check-windows"
	if !windows {
		if query {
			return true, unsupportedWindows
		}
		return false, 0
	}
	if !check() {
		if !query {
			explain()
		}
		return true, unsupportedWindows
	}
	return query, 0
}
