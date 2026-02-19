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
	BaseDir string // Directory containing the bridge shared library and Ultralight SDK libraries. Defaults to working directory.
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
		if _, err := os.Stat(filepath.Join(baseDir, bridgeLibName())); err != nil {
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
	// Key down events (RawKeyDown triggers accelerators like Ctrl+C/V/X/A)
	for _, key := range inpututil.AppendJustPressedKeys(nil) {
		vk, mods := keyToVK(key)
		if vk != 0 {
			ulViewFireKey(ui.viewID, keyEventRawKeyDown, vk, mods, "")
		}
	}
	// Character input from OS text input system (handles shift, layout, IME correctly)
	for _, r := range ebiten.AppendInputChars(nil) {
		ulViewFireKey(ui.viewID, keyEventChar, 0, 0, string(r))
	}
	// Key up events
	for _, key := range inpututil.AppendJustReleasedKeys(nil) {
		vk, mods := keyToVK(key)
		if vk != 0 {
			ulViewFireKey(ui.viewID, keyEventKeyUp, vk, mods, "")
		}
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
	if ebiten.IsKeyPressed(ebiten.KeyMeta) {
		mods |= keyModMeta
	}
	vk := ebitenKeyToVK(key)
	return vk, mods
}

func ebitenKeyToVK(key ebiten.Key) int32 {
	switch key {
	// Editing keys
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
	case ebiten.KeyDelete:
		return 0x2E
	case ebiten.KeyInsert:
		return 0x2D

	// Navigation
	case ebiten.KeyHome:
		return 0x24
	case ebiten.KeyEnd:
		return 0x23
	case ebiten.KeyPageUp:
		return 0x21
	case ebiten.KeyPageDown:
		return 0x22
	case ebiten.KeyArrowLeft:
		return 0x25
	case ebiten.KeyArrowUp:
		return 0x26
	case ebiten.KeyArrowRight:
		return 0x27
	case ebiten.KeyArrowDown:
		return 0x28

	// Modifier keys
	case ebiten.KeyShift, ebiten.KeyShiftLeft, ebiten.KeyShiftRight:
		return 0x10
	case ebiten.KeyControl, ebiten.KeyControlLeft, ebiten.KeyControlRight:
		return 0x11
	case ebiten.KeyAlt, ebiten.KeyAltLeft, ebiten.KeyAltRight:
		return 0x12
	case ebiten.KeyMeta, ebiten.KeyMetaLeft, ebiten.KeyMetaRight:
		return 0x5B

	// Lock keys
	case ebiten.KeyCapsLock:
		return 0x14
	case ebiten.KeyNumLock:
		return 0x90
	case ebiten.KeyScrollLock:
		return 0x91

	// System keys
	case ebiten.KeyPause:
		return 0x13
	case ebiten.KeyPrintScreen:
		return 0x2C
	case ebiten.KeyContextMenu:
		return 0x5D

	// Function keys
	case ebiten.KeyF1:
		return 0x70
	case ebiten.KeyF2:
		return 0x71
	case ebiten.KeyF3:
		return 0x72
	case ebiten.KeyF4:
		return 0x73
	case ebiten.KeyF5:
		return 0x74
	case ebiten.KeyF6:
		return 0x75
	case ebiten.KeyF7:
		return 0x76
	case ebiten.KeyF8:
		return 0x77
	case ebiten.KeyF9:
		return 0x78
	case ebiten.KeyF10:
		return 0x79
	case ebiten.KeyF11:
		return 0x7A
	case ebiten.KeyF12:
		return 0x7B

	// Numpad operators
	case ebiten.KeyNumpadMultiply:
		return 0x6A
	case ebiten.KeyNumpadAdd:
		return 0x6B
	case ebiten.KeyNumpadSubtract:
		return 0x6D
	case ebiten.KeyNumpadDecimal:
		return 0x6E
	case ebiten.KeyNumpadDivide:
		return 0x6F
	case ebiten.KeyNumpadEqual:
		return 0xBB

	// Punctuation / symbols (Windows VK_OEM codes)
	case ebiten.KeySemicolon:
		return 0xBA
	case ebiten.KeyEqual:
		return 0xBB
	case ebiten.KeyComma:
		return 0xBC
	case ebiten.KeyMinus:
		return 0xBD
	case ebiten.KeyPeriod:
		return 0xBE
	case ebiten.KeySlash:
		return 0xBF
	case ebiten.KeyBackquote:
		return 0xC0
	case ebiten.KeyBracketLeft:
		return 0xDB
	case ebiten.KeyBackslash, ebiten.KeyIntlBackslash:
		return 0xDC
	case ebiten.KeyBracketRight:
		return 0xDD
	case ebiten.KeyQuote:
		return 0xDE

	default:
		if key >= ebiten.KeyDigit0 && key <= ebiten.KeyDigit9 {
			return 0x30 + int32(key-ebiten.KeyDigit0)
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
