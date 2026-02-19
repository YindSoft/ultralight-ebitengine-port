// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

//go:build windows

package ultralightui

import (
	"fmt"
	"path/filepath"
	"syscall"
)

func doInitBridge(baseDir string) error {
	dllPath := filepath.Join(baseDir, "ul_bridge.dll")
	absPath, err := filepath.Abs(dllPath)
	if err != nil {
		absPath = dllPath
	}
	lib, err := syscall.LoadLibrary(absPath)
	if err != nil {
		return fmt.Errorf("failed to load ul_bridge.dll from %s: %w", absPath, err)
	}
	return resolveAllSymbols(uintptr(lib))
}

func getSymbolAddr(handle uintptr, name string) (uintptr, error) {
	sym, err := syscall.GetProcAddress(syscall.Handle(handle), name)
	if err != nil {
		return 0, err
	}
	if sym == 0 {
		return 0, fmt.Errorf("symbol %q not found in DLL", name)
	}
	return sym, nil
}

func bridgeLibName() string {
	return "ul_bridge.dll"
}
