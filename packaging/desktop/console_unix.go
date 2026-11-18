//go:build !windows

package main

import "os/exec"

func configure(cmd *exec.Cmd) {}

func approvedWindowsClient() bool { return false }
func showWindowsRequirement()     {}
