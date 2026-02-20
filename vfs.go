// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

package ultralightui

import (
	"fmt"
	"io/fs"
	"path"
	"strings"
	"unsafe"

	"github.com/hajimehoshi/ebiten/v2"
)

// RegisterFile registers a file in Ultralight's VFS.
// filePath is the virtual path (e.g., "ui/style.css"). data is the content.
// Registered files take priority over disk files.
// Must be called BEFORE creating views that reference them.
func RegisterFile(filePath string, data []byte) error {
	if len(data) == 0 {
		return nil
	}
	norm := strings.ReplaceAll(filePath, "\\", "/")
	norm = strings.TrimLeft(norm, "/")
	rc := ulVfsRegister(norm, uintptr(unsafe.Pointer(&data[0])), int64(len(data)))
	if rc != 0 {
		return fmt.Errorf("ul_vfs_register failed for %q: code %d", norm, rc)
	}
	return nil
}

// ClearFiles frees all files registered in the VFS.
func ClearFiles() {
	ulVfsClear()
}

// VFSFileCount returns the number of files registered in the VFS.
func VFSFileCount() int {
	return int(ulVfsCount())
}

// NewFromFS creates a new UI loading all files from the given fs.FS
// into Ultralight's VFS, then loads mainFile as the main page.
//
// mainFile is relative to the FS root (e.g., "ui/index.html").
// All files in the FS are registered so that <link>, <script>, <img>
// can reference them with relative paths.
//
// Example with embed.FS:
//
//	//go:embed ui
//	var uiFiles embed.FS
//	ui, err := ultralightui.NewFromFS(800, 600, "ui/index.html", uiFiles, nil)
func NewFromFS(width, height int, mainFile string, fsys fs.FS, opts *Options) (*UltralightUI, error) {
	baseDir, debug := resolveOpts(opts)
	if err := initBridge(baseDir); err != nil {
		return nil, fmt.Errorf("bridge: %w", err)
	}
	if err := ensureULInit(baseDir, debug); err != nil {
		return nil, err
	}

	// Walk the FS and register each file
	err := fs.WalkDir(fsys, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		data, readErr := fs.ReadFile(fsys, p)
		if readErr != nil {
			return fmt.Errorf("reading %s: %w", p, readErr)
		}
		return RegisterFile(p, data)
	})
	if err != nil {
		return nil, fmt.Errorf("walking FS: %w", err)
	}

	norm := path.Clean(strings.ReplaceAll(mainFile, "\\", "/"))
	norm = strings.TrimLeft(norm, "/")
	url := "file:///" + norm

	// Combined create+load in ONE worker roundtrip, no sleeping
	viewID := ulCreateViewWithURL(int32(width), int32(height), url)
	if viewID < 0 {
		return nil, fmt.Errorf("ul_create_view_with_url failed with code %d", viewID)
	}
	registerView()

	ui := &UltralightUI{
		viewID:         viewID,
		texture:        ebiten.NewImage(width, height),
		pixels:         make([]byte, width*height*4),
		width:          width,
		height:         height,
		dirtyCountdown: 120,
		rawBGRA:        make([]byte, width*height*4),
		asyncWork:      make(chan struct{}, 1),
		asyncDone:      make(chan struct{}, 1),
		asyncStop:      make(chan struct{}),
	}
	go ui.asyncConvertLoop()

	return ui, nil
}

// NewFromFSAsync is like NewFromFS but creates the view asynchronously.
// The view is returned immediately but is not yet ready to use.
// Call IsReady() to check when loading is complete (~5 ticks / ~83ms).
// Update() can be called immediately; it handles the async state gracefully.
// Pixel output will be empty/transparent until the view is ready.
func NewFromFSAsync(width, height int, mainFile string, fsys fs.FS, opts *Options) (*UltralightUI, error) {
	baseDir, debug := resolveOpts(opts)
	if err := initBridge(baseDir); err != nil {
		return nil, fmt.Errorf("bridge: %w", err)
	}
	if err := ensureULInit(baseDir, debug); err != nil {
		return nil, err
	}

	// Walk the FS and register each file
	err := fs.WalkDir(fsys, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		data, readErr := fs.ReadFile(fsys, p)
		if readErr != nil {
			return fmt.Errorf("reading %s: %w", p, readErr)
		}
		return RegisterFile(p, data)
	})
	if err != nil {
		return nil, fmt.Errorf("walking FS: %w", err)
	}

	norm := path.Clean(strings.ReplaceAll(mainFile, "\\", "/"))
	norm = strings.TrimLeft(norm, "/")
	url := "file:///" + norm

	// Crear view asincronica: retorna inmediatamente, carga se procesa en ticks
	viewID := ulCreateViewAsync(int32(width), int32(height), url)
	if viewID < 0 {
		return nil, fmt.Errorf("ul_create_view_async failed with code %d", viewID)
	}
	registerView()

	ui := &UltralightUI{
		viewID:         viewID,
		texture:        ebiten.NewImage(width, height),
		pixels:         make([]byte, width*height*4),
		width:          width,
		height:         height,
		dirtyCountdown: 120,
		rawBGRA:        make([]byte, width*height*4),
		asyncWork:      make(chan struct{}, 1),
		asyncDone:      make(chan struct{}, 1),
		asyncStop:      make(chan struct{}),
	}
	go ui.asyncConvertLoop()

	return ui, nil
}
