package ultralightui

import (
	"testing"
)

func TestParseMessage_Empty(t *testing.T) {
	v, err := ParseMessage("")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if v != nil {
		t.Fatalf("expected nil, got %v", v)
	}
}

func TestParseMessage_Whitespace(t *testing.T) {
	v, err := ParseMessage("   ")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if v != nil {
		t.Fatalf("expected nil, got %v", v)
	}
}

func TestParseMessage_PlainString(t *testing.T) {
	v, err := ParseMessage("hello world")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	s, ok := v.(string)
	if !ok {
		t.Fatalf("expected string, got %T", v)
	}
	if s != "hello world" {
		t.Fatalf("expected %q, got %q", "hello world", s)
	}
}

func TestParseMessage_JSONObject(t *testing.T) {
	v, err := ParseMessage(`{"action":"click","id":42}`)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	m, ok := v.(map[string]interface{})
	if !ok {
		t.Fatalf("expected map, got %T", v)
	}
	if m["action"] != "click" {
		t.Fatalf("expected action=click, got %v", m["action"])
	}
	if m["id"] != float64(42) {
		t.Fatalf("expected id=42, got %v", m["id"])
	}
}

func TestParseMessage_JSONArray(t *testing.T) {
	v, err := ParseMessage(`[1,2,3]`)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	arr, ok := v.([]interface{})
	if !ok {
		t.Fatalf("expected slice, got %T", v)
	}
	if len(arr) != 3 {
		t.Fatalf("expected 3 elements, got %d", len(arr))
	}
}

func TestParseMessage_InvalidJSON(t *testing.T) {
	v, err := ParseMessage("{not json}")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	s, ok := v.(string)
	if !ok {
		t.Fatalf("expected fallback to string, got %T", v)
	}
	if s != "{not json}" {
		t.Fatalf("expected raw string, got %q", s)
	}
}

func TestParseMessage_JSONNumber(t *testing.T) {
	v, err := ParseMessage("42")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	n, ok := v.(float64)
	if !ok {
		t.Fatalf("expected float64, got %T", v)
	}
	if n != 42 {
		t.Fatalf("expected 42, got %v", n)
	}
}

func TestParseMessage_JSONBool(t *testing.T) {
	v, err := ParseMessage("true")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	b, ok := v.(bool)
	if !ok {
		t.Fatalf("expected bool, got %T", v)
	}
	if !b {
		t.Fatal("expected true")
	}
}

func TestVkToChar_Letters(t *testing.T) {
	tests := []struct {
		vk   int32
		want string
	}{
		{0x41, "a"}, // A
		{0x5A, "z"}, // Z
		{0x4D, "m"}, // M
	}
	for _, tt := range tests {
		got := vkToChar(tt.vk)
		if got != tt.want {
			t.Errorf("vkToChar(0x%X) = %q, want %q", tt.vk, got, tt.want)
		}
	}
}

func TestVkToChar_Digits(t *testing.T) {
	for i := int32(0); i <= 9; i++ {
		vk := int32(0x30) + i
		want := string(rune('0' + i))
		got := vkToChar(vk)
		if got != want {
			t.Errorf("vkToChar(0x%X) = %q, want %q", vk, got, want)
		}
	}
}

func TestVkToChar_SpecialKeys(t *testing.T) {
	tests := []struct {
		vk   int32
		want string
	}{
		{0x08, "\b"},
		{0x09, "\t"},
		{0x0D, "\r"},
		{0x20, " "},
		{0x70, ""},  // F1 - no char
	}
	for _, tt := range tests {
		got := vkToChar(tt.vk)
		if got != tt.want {
			t.Errorf("vkToChar(0x%X) = %q, want %q", tt.vk, got, tt.want)
		}
	}
}

func TestInBounds(t *testing.T) {
	ui := &UltralightUI{BoundsX: 100, BoundsY: 50, BoundsW: 200, BoundsH: 150}
	tests := []struct {
		mx, my int
		want   bool
	}{
		{150, 100, true},   // inside
		{100, 50, true},    // top-left corner
		{299, 199, true},   // bottom-right edge
		{300, 200, false},  // just outside
		{50, 100, false},   // left of bounds
		{150, 250, false},  // below bounds
	}
	for _, tt := range tests {
		got := ui.inBounds(tt.mx, tt.my)
		if got != tt.want {
			t.Errorf("inBounds(%d,%d) = %v, want %v (bounds=%d,%d,%d,%d)",
				tt.mx, tt.my, got, tt.want, ui.BoundsX, ui.BoundsY, ui.BoundsW, ui.BoundsH)
		}
	}
}

func TestInBounds_ZeroBounds(t *testing.T) {
	ui := &UltralightUI{BoundsX: 0, BoundsY: 0, BoundsW: 0, BoundsH: 0}
	// Zero bounds means "accept all input" (no restriction)
	if !ui.inBounds(500, 500) {
		t.Error("zero bounds should accept any coordinate")
	}
}

func TestResolveOpts_Nil(t *testing.T) {
	baseDir, debug := resolveOpts(nil)
	if debug {
		t.Error("nil opts should have debug=false")
	}
	if baseDir == "" {
		t.Error("nil opts should resolve to a non-empty base directory")
	}
}

func TestResolveOpts_Custom(t *testing.T) {
	opts := &Options{BaseDir: "/custom/path", Debug: true}
	baseDir, debug := resolveOpts(opts)
	if baseDir != "/custom/path" {
		t.Errorf("expected /custom/path, got %s", baseDir)
	}
	if !debug {
		t.Error("expected debug=true")
	}
}

func TestErrClosed(t *testing.T) {
	if ErrClosed == nil {
		t.Fatal("ErrClosed should not be nil")
	}
	if ErrClosed.Error() != "ultralightui: UI is closed" {
		t.Errorf("unexpected error message: %s", ErrClosed.Error())
	}
}
