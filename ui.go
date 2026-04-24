// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

package ultralightui

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
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

// DebugInput enables coordinate logging in forwardInput(). When true, every
// mouse click logs the full coordinate chain (cursor, offset, bounds, local)
// to stdout via log.Printf. Useful for diagnosing input issues on specific platforms.
var DebugInput bool

// MouseCoordScale overrides the auto-detected mouse coordinate scale factor.
// 0 (default) = auto-detect from surface dimensions.
// >0 = manual scale applied to local coordinates before sending to Ultralight.
// On macOS Retina, Ultralight may expect coordinates at 2x even when the
// view's device scale is set to 1.0. Set to 2.0 to fix click misalignment.
var MouseCoordScale float64

// ClearFocus removes keyboard focus from all views. After this call,
// no UI receives key events (keys go back to the game).
func ClearFocus() {
	setFocusedViewID(-1)
}

// HasInputFocus returns true if the currently focused view has a text input
// element (input, textarea, contenteditable) focused in the DOM.
// Use this to skip game keybindings while the user is typing in an HTML view.
//
// Note: the two atomic loads are not jointly atomic, but this is benign because
// both values are only modified from Ebiten's single-threaded Update loop.
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

	// mouseScale is the ratio of actual surface size to requested size.
	// Used to scale mouse coordinates for HiDPI (e.g., macOS Retina where
	// the surface may be 2x despite deviceScale=1.0). Auto-detected.
	mouseScale float64

	// OnMessage is called when the page sends a message via go.send(msg).
	// msg is a string or JSON string. Use ParseMessage to get structured data.
	OnMessage func(msg string)

	// BlockInput, cuando es true, hace que forwardInput trate el cursor como si
	// estuviese fuera de los bounds. Sirve para evitar que una vista oculta por
	// otra encima reciba clicks o movimiento. No afecta el teclado si la vista
	// no tiene foco.
	BlockInput bool

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
	if width <= 0 || height <= 0 {
		return nil, fmt.Errorf("invalid dimensions: %dx%d", width, height)
	}
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
	ui.detectMouseScale()
	return ui, nil
}

func newUIWithURL(width, height int, url string) (*UltralightUI, error) {
	if width <= 0 || height <= 0 {
		return nil, fmt.Errorf("invalid dimensions: %dx%d", width, height)
	}
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
	ui.detectMouseScale()
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

// injectGoHelper installs a custom undo/redo system for input/textarea elements,
// triggered from Go via ui.Eval("__ulUndo()") / "__ulRedo()" / "__ulSelectAll()".
// JS→Go messaging uses the native __goSend JSC callback registered by the C bridge
// in setup_js_bindings(). common.js wraps it as window.go.send().
func (ui *UltralightUI) injectGoHelper() {
	ui.Eval(`(function(){
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

	// Re-check closed: an OnMessage callback above may have called Close().
	if ui.closed {
		return nil
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
	if len(ui.pixels) > 0 && ui.texture != nil {
		if ulViewCopyPixelsRGBA(ui.viewID, uintptr(unsafe.Pointer(&ui.pixels[0])), int32(len(ui.pixels))) != 0 {
			ui.texture.WritePixels(ui.pixels)
		}
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
	rawMx, rawMy := mx, my // guardamos para debug
	mx -= GlobalCursorOffsetX
	my -= GlobalCursorOffsetY
	inBounds := ui.inBounds(mx, my)
	// Si la vista esta ocluida por otra encima, se comporta como si el cursor
	// estuviera fuera de sus bounds: no recibe clicks, move ni scroll nuevos.
	// Los press iniciados previamente dentro (leftDown/rightDown) mantienen la
	// captura hasta que se suelten, preservando el comportamiento de drag.
	if ui.BlockInput {
		inBounds = false
	}

	if inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) {
		if inBounds {
			setFocusedViewID(ui.viewID)
		} else if getFocusedViewID() == ui.viewID {
			// Click fuera de esta vista que tenia foco: liberar foco para que
			// las teclas (flechas, etc.) no sigan llegando al HTML.
			setFocusedViewID(-1)
		}
	}

	// "Mouse capture": si el press inicio dentro de esta vista, seguimos
	// reenviando eventos aunque el cursor salga de los bounds, hasta que
	// se suelte el boton (igual que el comportamiento nativo de un browser).
	captured := ui.leftDown || ui.rightDown

	if inBounds || captured {
		if inBounds {
			ui.mouseInside = true
		}
		lx := mx - ui.BoundsX
		ly := my - ui.BoundsY
		if ui.BoundsW <= 0 {
			lx, ly = mx, my
		}

		// Escalar coordenadas locales para HiDPI (macOS Retina u otros)
		scale := ui.getMouseScale()
		if scale > 1.0 {
			lx = int(float64(lx) * scale)
			ly = int(float64(ly) * scale)
		}

		// Debug logging: solo en clicks para no spamear
		if DebugInput && inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) {
			log.Printf("[ultralightui] click viewID=%d cursor=(%d,%d) offset=(%d,%d) adjusted=(%d,%d) bounds=(%d,%d,%d,%d) local=(%d,%d) scale=%.1f",
				ui.viewID, rawMx, rawMy, GlobalCursorOffsetX, GlobalCursorOffsetY,
				mx, my, ui.BoundsX, ui.BoundsY, ui.BoundsW, ui.BoundsH, lx, ly, scale)
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

		// Left button — use JustPressed to catch sub-frame clicks (macOS trackpad)
		justPressedLeft := inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft)
		pressedLeft := ebiten.IsMouseButtonPressed(ebiten.MouseButtonLeft)

		// Solo iniciar press nuevo si estamos dentro de bounds (no si solo captured)
		if inBounds {
			if justPressedLeft && !ui.leftDown && !ui.leftOutside {
				ui.leftDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonLeft)
			} else if pressedLeft && !ui.leftDown && !ui.leftOutside {
				// Button held from previous frame without JustPressed (edge case)
				ui.leftDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonLeft)
			}
		}

		if !pressedLeft {
			if ui.leftDown {
				ui.leftDown = false
				ulViewFireMouse(ui.viewID, mouseEventTypeUp, int32(lx), int32(ly), mouseButtonLeft)
			}
			ui.leftOutside = false
		}

		// Right button — same pattern
		justPressedRight := inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonRight)
		pressedRight := ebiten.IsMouseButtonPressed(ebiten.MouseButtonRight)

		if inBounds {
			if justPressedRight && !ui.rightDown && !ui.rightOutside {
				ui.rightDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonRight)
			} else if pressedRight && !ui.rightDown && !ui.rightOutside {
				ui.rightDown = true
				ulViewFireMouse(ui.viewID, mouseEventTypeDown, int32(lx), int32(ly), mouseButtonRight)
			}
		}

		if !pressedRight {
			if ui.rightDown {
				ui.rightDown = false
				ulViewFireMouse(ui.viewID, mouseEventTypeUp, int32(lx), int32(ly), mouseButtonRight)
			}
			ui.rightOutside = false
		}

		// Scroll solo dentro de bounds
		if inBounds {
			_, scrollY := ebiten.Wheel()
			if scrollY != 0 {
				ulViewFireScroll(ui.viewID, scrollEventTypeByPixel, 0, int32(scrollY*100))
			}
		}

		// Si termino la captura y estamos fuera de bounds, enviar leave
		if !inBounds && !ui.leftDown && !ui.rightDown {
			if ui.mouseInside {
				ui.mouseInside = false
				ulViewFireMouse(ui.viewID, mouseEventTypeMoved, -1, -1, mouseButtonNone)
				ui.mouseX = -1
				ui.mouseY = -1
			}
		}
	} else {
		// Cursor fuera de bounds y sin captura
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
// Returns nil if the UI has been closed.
func (ui *UltralightUI) GetTexture() *ebiten.Image {
	if ui.closed {
		return nil
	}
	return ui.texture
}

// Eval runs JavaScript in the page. Fire-and-forget (no return value).
func (ui *UltralightUI) Eval(script string) {
	if ui.closed {
		return
	}
	evalJS(ui.viewID, script)
}

// ParseMessage attempts to parse msg as JSON. If parsing succeeds, the parsed
// value is returned (map, slice, float64, bool, or nil). If parsing fails,
// the raw string is returned as-is with no error.
func ParseMessage(msg string) (interface{}, error) {
	msg = strings.TrimSpace(msg)
	if msg == "" {
		return nil, nil
	}
	var v interface{}
	if err := json.Unmarshal([]byte(msg), &v); err == nil {
		return v, nil
	}
	return msg, nil
}

// Send sends structured data to the page. It serializes to JSON and invokes
// window.go.receive(data). Define go.receive in your HTML to handle it.
func (ui *UltralightUI) Send(data interface{}) error {
	if ui.closed {
		return ErrClosed
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

// SupportsBinarySend retorna true si el bridge nativo tiene los simbolos JSC
// necesarios para SendBinary. Si false, el caller debe usar Send con base64.
func SupportsBinarySend() bool {
	return ulSupportsBinarySend != nil && ulSupportsBinarySend() != 0
}

// SendBinary envia un mensaje a window.go.receive con un objeto JS construido
// zero-copy: las props no-binarias vienen del map serializado a JSON; binData
// se monta como Uint8Array bajo la propiedad binKey. Bypassa el path de
// ulViewEvaluateScript (que tendria que recibir base64 + JSON.parse).
//
// El objeto recibido en JS tiene la forma {...props, [binKey]: Uint8Array}.
// Ideal para pasar PNG/JPEG/raw pixels sin overhead de base64.
//
// Si el bridge no soporta el path binario (SupportsBinarySend() == false),
// retorna error sin enviar nada — el caller deberia hacer fallback a Send.
func (ui *UltralightUI) SendBinary(props map[string]interface{}, binKey string, binData []byte) error {
	if ui.closed {
		return ErrClosed
	}
	if !SupportsBinarySend() {
		return errors.New("ultralightui: bridge does not support binary send (JSC API missing)")
	}
	if binKey == "" {
		return errors.New("ultralightui: binKey is empty")
	}
	if len(binData) == 0 {
		return errors.New("ultralightui: binData is empty")
	}
	jsonBytes, err := json.Marshal(props)
	if err != nil {
		return fmt.Errorf("SendBinary: marshal props: %w", err)
	}
	// El C-side hace strdup + memcpy de los buffers, asi que nuestros []byte
	// pueden liberarse al retornar.
	dataPtr := uintptr(unsafe.Pointer(&binData[0]))
	ulViewSendBinary(ui.viewID, string(jsonBytes), binKey, dataPtr, int32(len(binData)))
	// Mantener vivo binData hasta despues de la llamada (el GC podria moverlo
	// entre la conversion a uintptr y el procesamiento C).
	runtime.KeepAlive(binData)
	return nil
}

// SurfaceSize returns the actual surface dimensions as reported by Ultralight.
// On standard displays this matches (width, height). On HiDPI displays the
// surface may be larger (e.g., 2x on macOS Retina).
func (ui *UltralightUI) SurfaceSize() (int, int) {
	if ui.closed {
		return ui.width, ui.height
	}
	sw := int(ulViewGetSurfaceWidth(ui.viewID))
	sh := int(ulViewGetSurfaceHeight(ui.viewID))
	if sw <= 0 {
		sw = ui.width
	}
	if sh <= 0 {
		sh = ui.height
	}
	return sw, sh
}

// detectMouseScale auto-detects the DPI scale factor by comparing the actual
// surface size with the requested size. If they differ, the ratio is stored
// as mouseScale so coordinates can be scaled in forwardInput().
func (ui *UltralightUI) detectMouseScale() {
	sw, _ := ui.SurfaceSize()
	if sw > 0 && ui.width > 0 && sw != ui.width {
		ui.mouseScale = float64(sw) / float64(ui.width)
		log.Printf("ultralightui: DPI scale auto-detected: %.1fx (surface=%d, requested=%d)", ui.mouseScale, sw, ui.width)
	} else {
		ui.mouseScale = 1.0
	}
}

// getMouseScale returns the effective mouse coordinate scale factor.
// Uses MouseCoordScale (manual override) if set, otherwise the auto-detected value.
func (ui *UltralightUI) getMouseScale() float64 {
	if MouseCoordScale > 0 {
		return MouseCoordScale
	}
	if ui.mouseScale > 0 {
		return ui.mouseScale
	}
	return 1.0
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
	if !strings.HasPrefix(msg, "{\"action\":\"__inputFocus\"") {
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
	if ui.texture != nil {
		ui.texture.Deallocate()
		ui.texture = nil
	}
	ui.pixels = nil
}
