package main

import "testing"

func TestWindowsClientEligibility(t *testing.T) {
	for _, sample := range []struct {
		name                string
		major, minor, build uint32
		product             byte
		process, host       uint16
		caption             string
		fallback, supported bool
	}{
		{"Windows 10 initial", 10, 0, 10240, 1, 0, 0x8664, "", false, true},
		{"Windows 10 22H2", 10, 0, 19045, 1, 0, 0x8664, "", false, true},
		{"Windows 11 26H2", 10, 0, 26300, 1, 0, 0x8664, "", false, true},
		{"client prerelease", 10, 0, 27999, 1, 0, 0x8664, " Microsoft Windows 11 专业版 ", true, true},
		{"insider Windows 10", 10, 0, 18995, 1, 0, 0x8664, "Windows 10 Pro Insider Preview", true, true},
		{"Server shares 24H2 build", 10, 0, 26100, 3, 0, 0x8664, "Windows 11", false, false},
		{"domain controller", 10, 0, 17763, 2, 0, 0x8664, "", false, false},
		{"x64 emulated on ARM", 10, 0, 26100, 1, 0x8664, 0xaa64, "", false, false},
		{"x86 emulated on x64", 10, 0, 19045, 1, 0x014c, 0x8664, "", false, false},
		{"native x86", 10, 0, 19045, 1, 0, 0x014c, "", false, false},
		{"pre-10", 6, 3, 9600, 1, 0, 0x8664, "Windows 11", false, false},
		{"pre-release pre-10", 10, 0, 9841, 1, 0, 0x8664, "Windows 10", false, false},
		{"future NT", 11, 0, 32000, 1, 0, 0x8664, "Windows 11", false, false},
		{"unconfirmed version", 10, 0, 31001, 1, 0, 0x8664, "", true, false},
		{"future product", 10, 0, 31001, 1, 0, 0x8664, "Microsoft Windows 12 Pro", true, false},
		{"lookalike caption", 10, 0, 31001, 1, 0, 0x8664, "Microsoft Windows 110", true, false},
		{"misleading caption", 10, 0, 31001, 1, 0, 0x8664, "Windows Server with Windows 11 compatibility", true, false},
	} {
		t.Run(sample.name, func(t *testing.T) {
			queried := false
			got := supportedWindowsClient(sample.major, sample.minor, sample.build, sample.product, sample.process, sample.host,
				func() string { queried = true; return sample.caption })
			if got != sample.supported || queried != sample.fallback {
				t.Fatalf("accepted=%v, used caption=%v; want %v, %v", got, queried, sample.supported, sample.fallback)
			}
		})
	}
}

func TestWindowsStartup(t *testing.T) {
	for _, sample := range []struct {
		name                         string
		args                         []string
		windows, valid, stop, dialog bool
		code                         int
	}{
		{"eligible query", []string{"--check-windows"}, true, true, true, false, 0},
		{"rejected query", []string{"--check-windows"}, true, false, true, false, 1150},
		{"eligible launch", nil, true, true, false, false, 0},
		{"rejected launch", nil, true, false, true, true, 1150},
		{"extra flags cannot bypass", []string{"--force", "--check-windows"}, true, false, true, true, 1150},
		{"other desktop", nil, false, false, false, false, 0},
		{"non-Windows query", []string{"--check-windows"}, false, false, true, false, 1150},
	} {
		t.Run(sample.name, func(t *testing.T) {
			probed, prompted := false, false
			stop, code := windowsStartup(sample.args, sample.windows, func() bool { probed = true; return sample.valid }, func() { prompted = true })
			if stop != sample.stop || code != sample.code || prompted != sample.dialog || probed != sample.windows {
				t.Fatalf("stop=%v, code=%d, dialog=%v, probe=%v", stop, code, prompted, probed)
			}
		})
	}
}
