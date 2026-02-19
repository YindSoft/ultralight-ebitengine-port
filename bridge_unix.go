// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

//go:build linux || darwin

package ultralightui

import (
	"fmt"
	"path/filepath"
	"runtime"

	"github.com/ebitengine/purego"
)

func doInitBridge(baseDir string) error {
	libPath := filepath.Join(baseDir, bridgeLibName())
	absPath, err := filepath.Abs(libPath)
	if err != nil {
		absPath = libPath
	}
	handle, err := purego.Dlopen(absPath, purego.RTLD_NOW|purego.RTLD_GLOBAL)
	if err != nil {
		return fmt.Errorf("failed to load %s from %s: %w", bridgeLibName(), absPath, err)
	}
	return resolveAllSymbols(handle)
}

func getSymbolAddr(handle uintptr, name string) (uintptr, error) {
	sym, err := purego.Dlsym(handle, name)
	if err != nil {
		return 0, err
	}
	return sym, nil
}

func bridgeLibName() string {
	if runtime.GOOS == "darwin" {
		return "libul_bridge.dylib"
	}
	return "libul_bridge.so"
}
