// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

package ultralightui

import (
	"fmt"
	"runtime"
	"sync"
	"unsafe"

	"github.com/ebitengine/purego"
)

func init() {
	// Ultralight requires all API calls to be made from the same OS thread.
	runtime.LockOSThread()
}

// Mouse event types (ULMouseEventType / ULMouseButton)
const (
	mouseEventTypeMoved = 0
	mouseEventTypeDown  = 1
	mouseEventTypeUp    = 2

	mouseButtonNone  = 0
	mouseButtonLeft  = 1
	mouseButtonRight = 3
)

const scrollEventTypeByPixel = 0

// Key event types for Ultralight
const (
	keyEventRawKeyDown = 0
	keyEventKeyDown    = 1
	keyEventKeyUp      = 2
	keyEventChar       = 3
)

// Key modifier bits
const (
	keyModAlt   = 1
	keyModCtrl  = 2
	keyModMeta  = 4
	keyModShift = 8
)

var (
	ulInit                  func(baseDir string, debug int32) int32
	ulCreateView            func(width, height int32) int32
	ulDestroyView           func(viewID int32)
	ulViewLoadHTML          func(viewID int32, html string)
	ulViewLoadURL           func(viewID int32, url string)
	ulTick                  func()
	ulViewGetPixels         func(viewID int32) uintptr
	ulViewUnlockPixels      func(viewID int32)
	ulViewGetWidth          func(viewID int32) uint32
	ulViewGetHeight         func(viewID int32) uint32
	ulViewGetRowBytes       func(viewID int32) uint32
	ulViewFireMouse         func(viewID int32, eventType, x, y, button int32)
	ulViewFireScroll        func(viewID int32, eventType, dx, dy int32)
	ulViewFireKey           func(viewID int32, keyType int32, vk int32, mods uint32, text string)
	ulViewEvalJS            func(viewID int32, js string)
	ulViewGetMessage        func(viewID int32, buf uintptr, bufSize int32) int32
	ulViewGetConsoleMessage func(viewID int32, buf uintptr, bufSize int32) int32
	ulDestroy               func()
	ulVfsRegister           func(path string, data uintptr, size int64) int32
	ulVfsClear              func()
	ulVfsCount              func() int32
)

var (
	bridgeOnce  sync.Once
	initErr     error
	ulInitOnce  sync.Once
	ulInitErr   error
	viewCount   int32
	viewCountMu sync.Mutex
)

func initBridge(baseDir string) error {
	bridgeOnce.Do(func() {
		initErr = doInitBridge(baseDir)
	})
	return initErr
}

// ensureULInit calls ul_init(baseDir, debug) once. Must be called after initBridge.
func ensureULInit(baseDir string, debug bool) error {
	ulInitOnce.Do(func() {
		d := int32(0)
		if debug {
			d = 1
		}
		if rc := ulInit(baseDir, d); rc != 0 {
			ulInitErr = fmt.Errorf("ul_init failed with code %d", rc)
		}
	})
	return ulInitErr
}

func registerView() {
	viewCountMu.Lock()
	viewCount++
	viewCountMu.Unlock()
}

func unregisterView() {
	viewCountMu.Lock()
	viewCount--
	if viewCount <= 0 {
		viewCount = 0
		ulDestroy()
	}
	viewCountMu.Unlock()
}

// resolveAllSymbols registers all exported symbols from the bridge using
// getSymbolAddr (defined in bridge_windows.go or bridge_unix.go).
func resolveAllSymbols(handle uintptr) error {
	for _, reg := range []struct {
		fptr interface{}
		name string
	}{
		{&ulInit, "ul_init"},
		{&ulCreateView, "ul_create_view"},
		{&ulDestroyView, "ul_destroy_view"},
		{&ulViewLoadHTML, "ul_view_load_html"},
		{&ulViewLoadURL, "ul_view_load_url"},
		{&ulTick, "ul_tick"},
		{&ulViewGetPixels, "ul_view_get_pixels"},
		{&ulViewUnlockPixels, "ul_view_unlock_pixels"},
		{&ulViewGetWidth, "ul_view_get_width"},
		{&ulViewGetHeight, "ul_view_get_height"},
		{&ulViewGetRowBytes, "ul_view_get_row_bytes"},
		{&ulViewFireMouse, "ul_view_fire_mouse"},
		{&ulViewFireScroll, "ul_view_fire_scroll"},
		{&ulViewFireKey, "ul_view_fire_key"},
		{&ulViewEvalJS, "ul_view_eval_js"},
		{&ulViewGetMessage, "ul_view_get_message"},
		{&ulViewGetConsoleMessage, "ul_view_get_console_message"},
		{&ulDestroy, "ul_destroy"},
		{&ulVfsRegister, "ul_vfs_register"},
		{&ulVfsClear, "ul_vfs_clear"},
		{&ulVfsCount, "ul_vfs_count"},
	} {
		sym, err := getSymbolAddr(handle, reg.name)
		if err != nil {
			return fmt.Errorf("%s: %w", reg.name, err)
		}
		purego.RegisterFunc(reg.fptr, sym)
	}
	return nil
}

func evalJS(viewID int32, js string) {
	ulViewEvalJS(viewID, js)
}

func pollMessage(viewID int32) (string, bool) {
	var buf [2048]byte
	n := ulViewGetMessage(viewID, uintptr(unsafe.Pointer(&buf[0])), 2048)
	if n <= 0 {
		return "", false
	}
	return string(buf[:n]), true
}

func pollConsoleMessage(viewID int32) (string, bool) {
	var buf [2048]byte
	n := ulViewGetConsoleMessage(viewID, uintptr(unsafe.Pointer(&buf[0])), 2048)
	if n <= 0 {
		return "", false
	}
	return string(buf[:n]), true
}
