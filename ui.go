// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

package ultralightui

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"unsafe"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/inpututil"
)

// Focus: only the focused UI receives keyboard. Mouse/scroll still require cursor in bounds.
// Clicking inside a UI gives it focus. Call SetFocus() to assign focus without clicking.
var focusedViewID atomic.Int32

// inputFocusViewID tracks which view has a text input element focused in the DOM.
// Used by HasInputFocus() to let the host skip game keybindings while the user types.
var inputFocusViewID atomic.Int32

func init() {
	focusedViewID.Store(-1)
	inputFocusViewID.Store(-1)
}

// GlobalCursorOffsetX/Y are subtracted from cursor coordinates before checking bounds
// and computing local coordinates. Use when the content area doesn't start at (0,0)
// (e.g., a border + topbar offset the content).
var GlobalCursorOffsetX int
var GlobalCursorOffsetY int

// ClearFocus removes keyboard focus from all views. After this call,
// no UI receives key events (keys go back to the game).
func ClearFocus() {
	setFocusedViewID(-1)
}

// HasInputFocus returns true if the currently focused view has a text input
// element (input, textarea, contenteditable) focused in the DOM.
// Use this to skip game keybindings while the user is typing in an HTML view.
func HasInputFocus() bool {
	ifvid := inputFocusViewID.Load()
	return ifvid >= 0 && getFocusedViewID() == ifvid
}

func getFocusedViewID() int32 {
	return focusedViewID.Load()
}

func setFocusedViewID(viewID int32) {
	focusedViewID.Store(viewID)
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
	mouseInside    bool // true if cursor is inside bounds (to detect leave)
	leftDown       bool
	rightDown      bool
	leftOutside    bool // left button was pressed outside bounds (ignore on re-enter)
	rightOutside   bool // right button was pressed outside bounds
	domReady       bool
	frameCount     int
	goHelperInjected bool

	// Reusable buffers to avoid per-frame allocations in forwardKeyboard
	keyBuf     []ebiten.Key
	charBuf    []rune

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
	// Combined create+load in ONE worker roundtrip, no sleeping
	viewID := ulCreateViewWithHTML(int32(width), int32(height), string(html))
	if viewID < 0 {
		return nil, fmt.Errorf("ul_create_view_with_html failed with code %d", viewID)
	}
	registerView()

	ui := &UltralightUI{
		viewID:  viewID,
		texture: ebiten.NewImage(width, height),
		pixels:  make([]byte, width*height*4),
		width:   width,
		height:  height,
	}
	return ui, nil
}

func newUIWithURL(width, height int, url string) (*UltralightUI, error) {
	// Combined create+load in ONE worker roundtrip, no sleeping
	viewID := ulCreateViewWithURL(int32(width), int32(height), url)
	if viewID < 0 {
		return nil, fmt.Errorf("ul_create_view_with_url failed with code %d", viewID)
	}
	registerView()

	ui := &UltralightUI{
		viewID:  viewID,
		texture: ebiten.NewImage(width, height),
		pixels:  make([]byte, width*height*4),
		width:   width,
		height:  height,
	}
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

// MarkDirty is a no-op kept for compatibility. Pixels are automatically copied
// every frame when Ultralight has pending changes.
func (ui *UltralightUI) MarkDirty() {}

// injectGoHelper ensures the window.go namespace is set up. The native __goSend
// function is registered by the C bridge via JavaScriptCore. This ensures
// go.send wraps it with JSON serialization for non-string values.
// It also installs a custom undo/redo system for input/textarea elements,
// triggered from Go via ui.Eval("__ulUndo()") / "__ulRedo()" / "__ulSelectAll()".
func (ui *UltralightUI) injectGoHelper() {
	// Single combined Eval: go.send setup + undo/redo/selectAll
	ui.Eval(`if(typeof window.__goSend==='function'){window.go=window.go||{};if(!window.go.send)window.go.send=function(m){window.__goSend(typeof m==='string'?m:JSON.stringify(m));};}
(function(){
if(window.__ulUndoInit)return;window.__ulUndoInit=1;
var stacks=new WeakMap(),redos=new WeakMap(),skip=0;
function S(e){if(!stacks.has(e))stacks.set(e,[{v:e.value,s:e.selectionStart,e:e.selectionEnd}]);return stacks.get(e)}
function R(e){if(!redos.has(e))redos.set(e,[]);return redos.get(e)}
document.addEventListener('input',function(ev){
if(skip)return;var e=ev.target;if(!e)return;var t=e.tagName;
if(t!=='INPUT'&&t!=='TEXTAREA'&&!e.isContentEditable)return;
if(e.isContentEditable){S(e).push({v:e.innerHTML,s:0,e:0})}
else{S(e).push({v:e.value,s:e.selectionStart,e:e.selectionEnd})}
R(e).length=0;
},true);
window.__ulUndo=function(){
var e=document.activeElement;if(!e)return;
var s=S(e);if(s.length<=1)return;
R(e).push(s.pop());var p=s[s.length-1];
skip=1;
if(e.isContentEditable){e.innerHTML=p.v}
else{e.value=p.v;e.setSelectionRange(p.s,p.e)}
skip=0;
};
window.__ulRedo=function(){
var e=document.activeElement;if(!e)return;
var r=R(e);if(!r.length)return;var p=r.pop();
S(e).push(p);
skip=1;
if(e.isContentEditable){e.innerHTML=p.v}
else{e.value=p.v;e.setSelectionRange(p.s,p.e)}
skip=0;
};
window.__ulSelectAll=function(){
var e=document.activeElement;if(!e)return;
if(e.select){e.select()}
else if(e.isContentEditable){var r=document.createRange();r.selectNodeContents(e);var s=window.getSelection();s.removeAllRanges();s.addRange(r)}
};
})();`)
}

// Tick calls the Ultralight renderer once (Update + RefreshDisplay + Render for all views).
// When using multiple views, call Tick() once per frame BEFORE calling UpdateNoTick() on each view.
// This avoids redundant renderer cycles that happen when each view calls Update().
func Tick() {
	ulTick()
}

// Update should be called every frame from the game's Update. It ticks Ultralight,
// copies pixels to the texture, polls messages, and forwards input.
// Note: each call to Update() triggers a full renderer cycle for ALL views.
// For multiple views, prefer calling Tick() once then UpdateNoTick() on each view.
func (ui *UltralightUI) Update() error {
	if ui.closed {
		return nil
	}
	ulTick()
	return ui.updateInternal()
}

// UpdateNoTick does everything Update() does EXCEPT calling ulTick().
// Use with Tick(): call Tick() once per frame, then UpdateNoTick() on each view.
func (ui *UltralightUI) UpdateNoTick() error {
	if ui.closed {
		return nil
	}
	return ui.updateInternal()
}

// isHidden returns true if the view has zero-size bounds (hidden via SetBounds(0,0,0,0)).
func (ui *UltralightUI) isHidden() bool {
	return ui.BoundsW == 0 && ui.BoundsH == 0 && ui.BoundsX == 0 && ui.BoundsY == 0
}

func (ui *UltralightUI) updateInternal() error {
	ui.frameCount++

	// Poll native messages (JS -> Go via go.send) — always, even if hidden
	for {
		msg, ok := pollMessage(ui.viewID)
		if !ok {
			break
		}
		// Interceptar mensajes de focus de input (no reenviar a OnMessage)
		if ui.handleInputFocusMsg(msg) {
			continue
		}
		if ui.OnMessage != nil {
			ui.OnMessage(msg)
		}
	}

	if !ui.domReady && ui.frameCount > 10 && ui.IsReady() {
		ui.domReady = true
	}

	if ui.domReady && !ui.goHelperInjected {
		ui.injectGoHelper()
		ui.goHelperInjected = true
	}

	// Hidden view: only drain messages, skip input processing and pixel copying
	if ui.isHidden() {
		return nil
	}

	if ui.domReady {
		ui.forwardInput()
	}

	// Copy pixels only if Ultralight has rendered changes (dirty bounds).
	// ul_view_copy_pixels_rgba internally checks if the surface changed;
	// if no changes, returns 0 without copying (very cheap: just reads a rect).
	if ulViewCopyPixelsRGBA(ui.viewID, uintptr(unsafe.Pointer(&ui.pixels[0])), int32(len(ui.pixels))) != 0 {
		ui.texture.WritePixels(ui.pixels)
	}
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
	mx -= GlobalCursorOffsetX
	my -= GlobalCursorOffsetY
	inBounds := ui.inBounds(mx, my)

	if inBounds && inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) {
		setFocusedViewID(ui.viewID)
	}

	if inBounds {
		ui.mouseInside = true
		lx := mx - ui.BoundsX
		ly := my - ui.BoundsY
		if ui.BoundsW <= 0 {
			lx, ly = mx, my
		}

		if lx != ui.mouseX || ly != ui.mouseY {
			// Pass current button state so Ultralight can handle drag-selection in inputs.
			moveBtn := int32(mouseButtonNone)
			if ui.leftDown {
				moveBtn = mouseButtonLeft
			}
			ulViewFireMouse(ui.viewID, mouseEventTypeMoved, int32(lx), int32(ly), moveBtn)
			ui.mouseX = lx
			ui.mouseY = ly
		}

		if ebiten.IsMouseButtonPressed(ebiten.MouseButtonLeft) {
			if !ui.leftDown && !ui.leftOutside {
				ui.leftDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonLeft)
	
			}
		} else {
			if ui.leftDown {
				ui.leftDown = false
				ulViewFireMouse(ui.viewID, mouseEventTypeUp, int32(lx), int32(ly), mouseButtonLeft)
	
			}
			ui.leftOutside = false
		}

		if ebiten.IsMouseButtonPressed(ebiten.MouseButtonRight) {
			if !ui.rightDown && !ui.rightOutside {
				ui.rightDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonRight)
	
			}
		} else {
			if ui.rightDown {
				ui.rightDown = false
				ulViewFireMouse(ui.viewID, mouseEventTypeUp, int32(lx), int32(ly), mouseButtonRight)
	
			}
			ui.rightOutside = false
		}

		_, scrollY := ebiten.Wheel()
		if scrollY != 0 {
			ulViewFireScroll(ui.viewID, scrollEventTypeByPixel, 0, int32(scrollY*100))

		}
	} else {
		// Cursor left bounds: send mouse move outside the view to clear :hover
		if ui.mouseInside {
			ui.mouseInside = false
			ulViewFireMouse(ui.viewID, mouseEventTypeMoved, -1, -1, mouseButtonNone)
			ui.mouseX = -1
			ui.mouseY = -1
		}
		// Cursor outside bounds: if button is pressed outside, mark to ignore on re-enter
		if ebiten.IsMouseButtonPressed(ebiten.MouseButtonLeft) {
			if !ui.leftDown {
				ui.leftOutside = true
			}
		} else {
			ui.leftOutside = false
			ui.leftDown = false
		}
		if ebiten.IsMouseButtonPressed(ebiten.MouseButtonRight) {
			if !ui.rightDown {
				ui.rightOutside = true
			}
		} else {
			ui.rightOutside = false
			ui.rightDown = false
		}
	}

	if getFocusedViewID() == ui.viewID {
		ui.forwardKeyboard()
	}
}

// Key repeat timing in frames (at 60fps: delay ~500ms, interval ~33ms).
const (
	keyRepeatDelay    = 30 // frames before repeat starts
	keyRepeatInterval = 2  // frames between repeats
)

func (ui *UltralightUI) forwardKeyboard() {
	ctrlHeld := ebiten.IsKeyPressed(ebiten.KeyControl) || ebiten.IsKeyPressed(ebiten.KeyMeta)

	// Key down events (RawKeyDown triggers accelerators like Ctrl+C/V/X)
	// Reuse buffer to avoid per-frame allocations
	ui.keyBuf = inpututil.AppendJustPressedKeys(ui.keyBuf[:0])
	for _, key := range ui.keyBuf {
		// Intercept editing shortcuts and handle via JS (Ultralight's native
		// key_identifier support through ulCreateKeyEvent is unreliable).
		if ctrlHeld {
			switch key {
			case ebiten.KeyZ:
				if ebiten.IsKeyPressed(ebiten.KeyShift) {
					ui.Eval("if(window.__ulRedo)__ulRedo()")
				} else {
					ui.Eval("if(window.__ulUndo)__ulUndo()")
				}
				continue
			case ebiten.KeyY:
				ui.Eval("if(window.__ulRedo)__ulRedo()")
				continue
			case ebiten.KeyA:
				ui.Eval("if(window.__ulSelectAll)__ulSelectAll()")
				continue
			}
		}
		vk, mods := keyToVK(key)
		if vk != 0 {
			ulViewFireKey(ui.viewID, keyEventRawKeyDown, vk, mods, vkToChar(vk))
		}
	}
	// Key repeat: re-fire RawKeyDown for held non-character keys (Backspace, Delete, arrows, etc.)
	for _, key := range heldNonCharKeys {
		dur := inpututil.KeyPressDuration(key)
		if dur > keyRepeatDelay && (dur-keyRepeatDelay)%keyRepeatInterval == 0 {
			vk, mods := keyToVK(key)
			if vk != 0 {
				ulViewFireKey(ui.viewID, keyEventRawKeyDown, vk, mods, vkToChar(vk))
			}
		}
	}
	// Character input from OS text input system (handles shift, layout, IME correctly)
	ui.charBuf = ebiten.AppendInputChars(ui.charBuf[:0])
	for _, r := range ui.charBuf {
		if r >= 0x20 { // filter control characters (Ctrl+letter combos)
			ulViewFireKey(ui.viewID, keyEventChar, 0, 0, string(r))
		}
	}
	// Key up events
	ui.keyBuf = inpututil.AppendJustReleasedKeys(ui.keyBuf[:0])
	for _, key := range ui.keyBuf {
		vk, mods := keyToVK(key)
		if vk != 0 {
			ulViewFireKey(ui.viewID, keyEventKeyUp, vk, mods, vkToChar(vk))
		}
	}
}

// vkToChar returns the lowercase character for a virtual key code.
// Ultralight uses the text/unmodified_text fields for matching keyboard shortcuts
// (e.g., Ctrl+Z needs unmodified_text="z" to match the undo command).
func vkToChar(vk int32) string {
	if vk >= 0x41 && vk <= 0x5A { // A-Z → "a"-"z"
		return string(rune('a' + (vk - 0x41)))
	}
	if vk >= 0x30 && vk <= 0x39 { // 0-9
		return string(rune('0' + (vk - 0x30)))
	}
	switch vk {
	case 0x08:
		return "\b" // Backspace
	case 0x09:
		return "\t" // Tab
	case 0x0D:
		return "\r" // Enter
	case 0x20:
		return " " // Space
	}
	return ""
}

// heldNonCharKeys lists keys that need synthetic repeat because the OS text input
// system (AppendInputChars) does not emit characters for them.
var heldNonCharKeys = []ebiten.Key{
	ebiten.KeyBackspace,
	ebiten.KeyDelete,
	ebiten.KeyArrowLeft,
	ebiten.KeyArrowRight,
	ebiten.KeyArrowUp,
	ebiten.KeyArrowDown,
	ebiten.KeyHome,
	ebiten.KeyEnd,
	ebiten.KeyPageUp,
	ebiten.KeyPageDown,
	ebiten.KeyTab,
	ebiten.KeyEnter,
	ebiten.KeyEscape,
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

	// Modifier keys: return 0 so they are NOT sent to Ultralight as standalone
	// events. Modifier state is already encoded in the 'mods' field of each
	// regular key event. Sending them as RawKeyDown/KeyUp creates desync risk
	// (e.g., Ctrl down without KeyUp on focus change → Ultralight thinks Ctrl
	// is still held and fires Ctrl+A when pressing 'A').
	case ebiten.KeyShift, ebiten.KeyShiftLeft, ebiten.KeyShiftRight:
		return 0
	case ebiten.KeyControl, ebiten.KeyControlLeft, ebiten.KeyControlRight:
		return 0
	case ebiten.KeyAlt, ebiten.KeyAltLeft, ebiten.KeyAltRight:
		return 0
	case ebiten.KeyMeta, ebiten.KeyMetaLeft, ebiten.KeyMetaRight:
		return 0

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
	// JSON es sintaxis JS valida: embeber directo sin escapar ni JSON.parse.
	// Evita el loop byte-a-byte de escaping y el doble parsing en JS.
	const prefix = "if(window.go&&typeof window.go.receive==='function')window.go.receive("
	const suffix = ");"
	var sb strings.Builder
	sb.Grow(len(prefix) + len(jsonBytes) + len(suffix))
	sb.WriteString(prefix)
	sb.Write(jsonBytes)
	sb.WriteString(suffix)
	evalJS(ui.viewID, sb.String())
	return nil
}

// IsReady returns true if the view has finished async loading and is usable.
// For synchronously created views this always returns true.
// For async views (NewFromFSAsync), it returns false until priming+loading is done.
func (ui *UltralightUI) IsReady() bool {
	if ui.closed {
		return false
	}
	return ulViewIsReady(ui.viewID) != 0
}

// handleInputFocusMsg intercepts __inputFocus messages sent by common.js
// when a text input gains or loses DOM focus. Returns true if the message
// was consumed (caller should skip OnMessage).
func (ui *UltralightUI) handleInputFocusMsg(msg string) bool {
	if !strings.Contains(msg, "__inputFocus") {
		return false
	}
	var data struct {
		Action  string `json:"action"`
		Focused bool   `json:"focused"`
	}
	if json.Unmarshal([]byte(msg), &data) != nil || data.Action != "__inputFocus" {
		return false
	}
	if data.Focused {
		inputFocusViewID.Store(ui.viewID)
	} else {
		inputFocusViewID.CompareAndSwap(ui.viewID, -1)
	}
	return true
}

// Close releases resources. Call when done (e.g. defer ui.Close()).
// After Close, the UI must not be used.
func (ui *UltralightUI) Close() {
	if ui.closed {
		return
	}
	ui.closed = true
	inputFocusViewID.CompareAndSwap(ui.viewID, -1)
	if getFocusedViewID() == ui.viewID {
		setFocusedViewID(-1)
	}
	ulDestroyView(ui.viewID)
	unregisterView()
}
