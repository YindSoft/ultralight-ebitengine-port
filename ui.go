// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

package ultralightui

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"unsafe"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/inpututil"
)

// Focus: only the focused UI receives keyboard. Mouse/scroll still require cursor in bounds.
// Clicking inside a UI gives it focus. Call SetFocus() to assign focus without clicking.
var (
	focusedViewID   int32 = -1
	focusedViewIDMu sync.Mutex
)

func getFocusedViewID() int32 {
	focusedViewIDMu.Lock()
	defer focusedViewIDMu.Unlock()
	return focusedViewID
}

func setFocusedViewID(viewID int32) {
	focusedViewIDMu.Lock()
	defer focusedViewIDMu.Unlock()
	focusedViewID = viewID
}

// Options for creating the UI. All fields are optional.
type Options struct {
	BaseDir string // Directory containing ul_bridge.dll and Ultralight SDK DLLs. Defaults to working directory.
	Debug   bool   // Enable debug logging (creates bridge.log and ultralight.log). Default false.
}

// UltralightUI represents an HTML view rendered as an Ebiten texture.
// Multiple instances can exist; each has its own view in the Ultralight bridge.
type UltralightUI struct {
	viewID  int32
	texture *ebiten.Image
	pixels  []byte

	width  int
	height int

	// Bounds in screen coordinates for input routing. Set via SetBounds so that
	// only the view under the cursor receives mouse/scroll input.
	BoundsX, BoundsY, BoundsW, BoundsH int

	mouseX, mouseY int
	leftDown       bool
	rightDown      bool
	domReady       bool
	frameCount     int
	goHelperInjected bool

	// OnMessage is called when the page sends a message via go.send(msg).
	// msg is a string or JSON string. Use ParseMessage to get structured data.
	OnMessage func(msg string)

	closed bool
}

// NewFromFile creates a new UI loading HTML from a local file.
func NewFromFile(width, height int, filePath string, opts *Options) (*UltralightUI, error) {
	baseDir, debug := resolveOpts(opts)
	if err := initBridge(baseDir); err != nil {
		return nil, fmt.Errorf("bridge: %w", err)
	}
	if err := ensureULInit(baseDir, debug); err != nil {
		return nil, err
	}
	htmlBytes, err := os.ReadFile(filePath)
	if err != nil {
		return nil, fmt.Errorf("reading HTML file %s: %w", filePath, err)
	}
	return newUI(width, height, htmlBytes)
}

// NewFromURL creates a new UI loading content from a URL.
func NewFromURL(width, height int, url string, opts *Options) (*UltralightUI, error) {
	baseDir, debug := resolveOpts(opts)
	if err := initBridge(baseDir); err != nil {
		return nil, fmt.Errorf("bridge: %w", err)
	}
	if err := ensureULInit(baseDir, debug); err != nil {
		return nil, err
	}
	return newUIWithURL(width, height, url)
}

// NewFromHTML creates a new UI with the given HTML bytes (no file or URL).
func NewFromHTML(width, height int, html []byte, opts *Options) (*UltralightUI, error) {
	baseDir, debug := resolveOpts(opts)
	if err := initBridge(baseDir); err != nil {
		return nil, fmt.Errorf("bridge: %w", err)
	}
	if err := ensureULInit(baseDir, debug); err != nil {
		return nil, err
	}
	return newUI(width, height, html)
}

// New is a convenience alias for NewFromFile.
func New(width, height int, htmlPath string, opts *Options) (*UltralightUI, error) {
	return NewFromFile(width, height, htmlPath, opts)
}

func newUI(width, height int, html []byte) (*UltralightUI, error) {
	viewID := ulCreateView(int32(width), int32(height))
	if viewID < 0 {
		return nil, fmt.Errorf("ul_create_view failed with code %d", viewID)
	}
	registerView()

	ui := &UltralightUI{
		viewID:  viewID,
		texture: ebiten.NewImage(width, height),
		pixels:  make([]byte, width*height*4),
		width:   width,
		height:  height,
	}
	if len(html) > 0 {
		ulViewLoadHTML(viewID, string(html))
	}
	return ui, nil
}

func newUIWithURL(width, height int, url string) (*UltralightUI, error) {
	viewID := ulCreateView(int32(width), int32(height))
	if viewID < 0 {
		return nil, fmt.Errorf("ul_create_view failed with code %d", viewID)
	}
	registerView()

	ui := &UltralightUI{
		viewID:  viewID,
		texture: ebiten.NewImage(width, height),
		pixels:  make([]byte, width*height*4),
		width:   width,
		height:  height,
	}
	ulViewLoadURL(viewID, url)
	return ui, nil
}

func resolveOpts(opts *Options) (string, bool) {
	debug := false
	baseDir := ""
	if opts != nil {
		baseDir = opts.BaseDir
		debug = opts.Debug
	}
	if baseDir == "" {
		baseDir, _ = os.Getwd()
		if _, err := os.Stat(filepath.Join(baseDir, "ul_bridge.dll")); err != nil {
			if exe, _ := os.Executable(); exe != "" {
				baseDir = filepath.Dir(exe)
			}
		}
	}
	return baseDir, debug
}

// SetFocus gives this UI keyboard focus. Only the focused UI receives key events,
// regardless of cursor position. Mouse and scroll still require the cursor inside bounds.
// Clicking inside a UI also gives it focus.
func (ui *UltralightUI) SetFocus() {
	setFocusedViewID(ui.viewID)
}

// SetBounds sets the screen rectangle for this UI. Mouse and scroll are only
// forwarded when the cursor is inside these bounds. Keyboard goes to the focused UI.
// Use (0,0,0,0) to disable input.
func (ui *UltralightUI) SetBounds(x, y, w, h int) {
	ui.BoundsX, ui.BoundsY, ui.BoundsW, ui.BoundsH = x, y, w, h
}

// injectGoHelper ensures the window.go namespace is set up. The native __goSend
// function is registered by the C bridge via JavaScriptCore. This ensures
// go.send wraps it with JSON serialization for non-string values.
func (ui *UltralightUI) injectGoHelper() {
	ui.Eval("if(typeof window.__goSend==='function'){window.go=window.go||{};if(!window.go.send)window.go.send=function(m){window.__goSend(typeof m==='string'?m:JSON.stringify(m));};}")
}

// Update should be called every frame from the game's Update. It ticks Ultralight,
// copies pixels to the texture, polls messages, and forwards input.
func (ui *UltralightUI) Update() error {
	if ui.closed {
		return nil
	}
	ui.frameCount++

	// Poll native messages (JS -> Go via go.send)
	for {
		msg, ok := pollMessage(ui.viewID)
		if !ok {
			break
		}
		if ui.OnMessage != nil {
			ui.OnMessage(msg)
		}
	}

	ulTick()

	if !ui.domReady && ui.frameCount > 30 {
		ui.domReady = true
	}

	if ui.domReady && !ui.goHelperInjected {
		ui.injectGoHelper()
		ui.goHelperInjected = true
	}

	if ui.domReady {
		ui.forwardInput()
	}

	ui.copyPixels()
	return nil
}

func (ui *UltralightUI) inBounds(mx, my int) bool {
	if ui.BoundsW <= 0 || ui.BoundsH <= 0 {
		return true
	}
	return mx >= ui.BoundsX && mx < ui.BoundsX+ui.BoundsW &&
		my >= ui.BoundsY && my < ui.BoundsY+ui.BoundsH
}

func (ui *UltralightUI) forwardInput() {
	mx, my := ebiten.CursorPosition()
	inBounds := ui.inBounds(mx, my)

	if inBounds && inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) {
		setFocusedViewID(ui.viewID)
	}

	if inBounds {
		lx := mx - ui.BoundsX
		ly := my - ui.BoundsY
		if ui.BoundsW <= 0 {
			lx, ly = mx, my
		}

		if lx != ui.mouseX || ly != ui.mouseY {
			ulViewFireMouse(ui.viewID, mouseEventTypeMoved, int32(lx), int32(ly), mouseButtonNone)
			ui.mouseX = lx
			ui.mouseY = ly
		}

		if ebiten.IsMouseButtonPressed(ebiten.MouseButtonLeft) {
			if !ui.leftDown {
				ui.leftDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonLeft)
			}
		} else if ui.leftDown {
			ui.leftDown = false
			ulViewFireMouse(ui.viewID, mouseEventTypeUp, int32(lx), int32(ly), mouseButtonLeft)
		}

		if ebiten.IsMouseButtonPressed(ebiten.MouseButtonRight) {
			if !ui.rightDown {
				ui.rightDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonRight)
			}
		} else if ui.rightDown {
			ui.rightDown = false
			ulViewFireMouse(ui.viewID, mouseEventTypeUp, int32(lx), int32(ly), mouseButtonRight)
		}

		_, scrollY := ebiten.Wheel()
		if scrollY != 0 {
			ulViewFireScroll(ui.viewID, scrollEventTypeByPixel, 0, int32(scrollY*100))
		}
	}

	if getFocusedViewID() == ui.viewID {
		ui.forwardKeyboard()
	}
}

func (ui *UltralightUI) forwardKeyboard() {
	for _, key := range inpututil.AppendJustPressedKeys(nil) {
		vk, mods := keyToVK(key)
		ulViewFireKey(ui.viewID, keyEventKeyDown, vk, mods, "")
		if r := keyToRune(key); r != 0 {
			ulViewFireKey(ui.viewID, keyEventChar, 0, mods, string(r))
		}
	}
	for _, key := range inpututil.AppendJustReleasedKeys(nil) {
		vk, mods := keyToVK(key)
		ulViewFireKey(ui.viewID, keyEventKeyUp, vk, mods, "")
	}
}

func keyToVK(key ebiten.Key) (int32, uint32) {
	mods := uint32(0)
	if ebiten.IsKeyPressed(ebiten.KeyShift) {
		mods |= keyModShift
	}
	if ebiten.IsKeyPressed(ebiten.KeyControl) {
		mods |= keyModCtrl
	}
	if ebiten.IsKeyPressed(ebiten.KeyAlt) {
		mods |= keyModAlt
	}
	vk := ebitenKeyToVK(key)
	return vk, mods
}

func keyToRune(key ebiten.Key) rune {
	switch {
	case key >= ebiten.KeyA && key <= ebiten.KeyZ:
		return rune('a' + (key - ebiten.KeyA))
	case key >= ebiten.Key0 && key <= ebiten.Key9:
		return rune('0' + (key - ebiten.Key0))
	case key == ebiten.KeySpace:
		return ' '
	case key == ebiten.KeyEnter || key == ebiten.KeyNumpadEnter:
		return '\n'
	case key == ebiten.KeyTab:
		return '\t'
	default:
		return 0
	}
}

func ebitenKeyToVK(key ebiten.Key) int32 {
	switch key {
	case ebiten.KeyBackspace:
		return 0x08
	case ebiten.KeyTab:
		return 0x09
	case ebiten.KeyEnter, ebiten.KeyNumpadEnter:
		return 0x0D
	case ebiten.KeyEscape:
		return 0x1B
	case ebiten.KeySpace:
		return 0x20
	case ebiten.KeyArrowLeft:
		return 0x25
	case ebiten.KeyArrowUp:
		return 0x26
	case ebiten.KeyArrowRight:
		return 0x27
	case ebiten.KeyArrowDown:
		return 0x28
	case ebiten.KeyShift:
		return 0x10
	case ebiten.KeyControl:
		return 0x11
	case ebiten.KeyAlt:
		return 0x12
	default:
		if key >= ebiten.Key0 && key <= ebiten.Key9 {
			return 0x30 + int32(key-ebiten.Key0)
		}
		if key >= ebiten.KeyA && key <= ebiten.KeyZ {
			return 0x41 + int32(key-ebiten.KeyA)
		}
		if key >= ebiten.KeyNumpad0 && key <= ebiten.KeyNumpad9 {
			return 0x60 + int32(key-ebiten.KeyNumpad0)
		}
		return 0
	}
}

func (ui *UltralightUI) copyPixels() {
	ptr := ulViewGetPixels(ui.viewID)
	if ptr == 0 {
		return
	}

	w := ulViewGetWidth(ui.viewID)
	h := ulViewGetHeight(ui.viewID)
	rowBytes := ulViewGetRowBytes(ui.viewID)

	if w == 0 || h == 0 {
		ulViewUnlockPixels(ui.viewID)
		return
	}

	totalBytes := uintptr(rowBytes) * uintptr(h)
	src := unsafe.Slice((*byte)(unsafe.Pointer(ptr)), totalBytes)

	dstIdx := 0
	for y := 0; y < int(h); y++ {
		srcRowStart := y * int(rowBytes)
		for x := 0; x < int(w); x++ {
			srcOff := srcRowStart + x*4
			ui.pixels[dstIdx+0] = src[srcOff+2] // BGRA -> RGBA
			ui.pixels[dstIdx+1] = src[srcOff+1]
			ui.pixels[dstIdx+2] = src[srcOff+0]
			ui.pixels[dstIdx+3] = src[srcOff+3]
			dstIdx += 4
		}
	}

	ulViewUnlockPixels(ui.viewID)
	ui.texture.WritePixels(ui.pixels)
}

// GetTexture returns the Ebiten image with the current HTML content rendered.
func (ui *UltralightUI) GetTexture() *ebiten.Image {
	return ui.texture
}

// Eval runs JavaScript in the page. Fire-and-forget (no return value).
func (ui *UltralightUI) Eval(script string) {
	if ui.closed {
		return
	}
	evalJS(ui.viewID, script)
}

// ParseMessage parses msg as JSON if it looks like JSON (starts with '{' or '[').
// Returns the parsed value, or the raw string if it's not JSON.
func ParseMessage(msg string) (interface{}, error) {
	msg = strings.TrimSpace(msg)
	if msg == "" {
		return nil, nil
	}
	if (strings.HasPrefix(msg, "{") && strings.HasSuffix(msg, "}")) ||
		(strings.HasPrefix(msg, "[") && strings.HasSuffix(msg, "]")) {
		var v interface{}
		if err := json.Unmarshal([]byte(msg), &v); err != nil {
			return nil, err
		}
		return v, nil
	}
	return msg, nil
}

// Send sends structured data to the page. It serializes to JSON and invokes
// window.go.receive(data). Define go.receive in your HTML to handle it.
func (ui *UltralightUI) Send(data interface{}) error {
	if ui.closed {
		return nil
	}
	jsonBytes, err := json.Marshal(data)
	if err != nil {
		return fmt.Errorf("Send: %w", err)
	}
	var sb strings.Builder
	sb.WriteString("if(window.go&&typeof window.go.receive==='function')window.go.receive(JSON.parse(\"")
	for _, c := range string(jsonBytes) {
		switch c {
		case '\\':
			sb.WriteString(`\\`)
		case '"':
			sb.WriteString(`\"`)
		case '\n':
			sb.WriteString(`\n`)
		case '\r':
			sb.WriteString(`\r`)
		default:
			sb.WriteRune(c)
		}
	}
	sb.WriteString("\"));")
	evalJS(ui.viewID, sb.String())
	return nil
}

// Close releases resources. Call when done (e.g. defer ui.Close()).
// After Close, the UI must not be used.
func (ui *UltralightUI) Close() {
	if ui.closed {
		return
	}
	ui.closed = true
	if getFocusedViewID() == ui.viewID {
		setFocusedViewID(-1)
	}
	ulDestroyView(ui.viewID)
	unregisterView()
}
