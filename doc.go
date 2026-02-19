// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

// Package ultralightui renders HTML views as Ebiten textures using Ultralight 1.4.
//
// It provides a simple API for embedding HTML/CSS/JS interfaces in Ebitengine games
// and applications, with bidirectional Go <-> JavaScript communication.
//
// Basic usage:
//
//	import "github.com/YindSoft/ultralight-ebitengine-port"
//
//	// Create a UI from a local HTML file, a URL, or raw HTML bytes:
//	ui, err := ultralightui.NewFromFile(800, 600, "ui/index.html", nil)
//	// or: ultralightui.NewFromURL(800, 600, "https://example.com", nil)
//	// or: ultralightui.NewFromHTML(800, 600, htmlBytes, nil)
//	if err != nil { ... }
//	defer ui.Close()
//
//	// Receive messages from the page (JS calls go.send("action") or go.send({key: "val"}))
//	ui.OnMessage = func(msg string) {
//	    data, _ := ultralightui.ParseMessage(msg) // parses JSON if applicable
//	    ...
//	}
//
//	// In Ebiten Update():
//	ui.Update()
//
//	// In Ebiten Draw():
//	screen.DrawImage(ui.GetTexture(), opts)
//
//	// Optional: restrict input to a screen region and manage keyboard focus
//	ui.SetBounds(0, 0, 800, 600)
//	ui.SetFocus()
//
//	// Send structured data to the page (serialized as JSON):
//	ui.Send(map[string]any{"hp": 80, "maxHp": 100})
//	// HTML receives it via: go.receive = function(data) { ... }
//
//	// Run arbitrary JavaScript:
//	ui.Eval("updateScore(100)")
//
// Embedded assets (VFS):
//
// Use [NewFromFS] to load HTML/CSS/JS/images from an [embed.FS] or any [fs.FS].
// Files are registered in a virtual file system so Ultralight serves them from memory,
// without exposing assets on disk:
//
//	//go:embed ui
//	var uiFiles embed.FS
//	ui, err := ultralightui.NewFromFS(800, 600, "ui/index.html", uiFiles, nil)
//
// JS -> Go communication uses native JavaScriptCore bindings (no console.log hacks).
// Go -> JS communication calls window.go.receive(data) with parsed JSON.
//
// Multiple UltralightUI instances are supported. Each has its own Ultralight view.
// Mouse and scroll input are forwarded when the cursor is inside the view's bounds.
// Keyboard input goes to whichever view has focus (via SetFocus or clicking).
//
// Requirements: the bridge shared library (ul_bridge.dll on Windows,
// libul_bridge.so on Linux, libul_bridge.dylib on macOS) and the Ultralight 1.4
// SDK libraries must be present next to the executable or in the directory
// specified by [Options.BaseDir].
package ultralightui
