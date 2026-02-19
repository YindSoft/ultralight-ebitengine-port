// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

// Example of NewFromFS: loads HTML/CSS/JS from embed.FS (no files on disk).
package main

import (
	"embed"
	"fmt"
	"log"
	"os"
	"path/filepath"

	ultralightui "github.com/YindSoft/ultralight-ebitengine-port"
	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/ebitenutil"
)

//go:embed ui
var uiFiles embed.FS

const (
	screenWidth  = 800
	screenHeight = 600
)

type Game struct {
	ui      *ultralightui.UltralightUI
	counter int
}

func findBaseDir() string {
	if _, err := os.Stat("ul_bridge.dll"); err == nil {
		return ""
	}
	if _, err := os.Stat(filepath.Join("..", "ul_bridge.dll")); err == nil {
		return ".."
	}
	return ""
}

func newGame() (*Game, error) {
	opts := &ultralightui.Options{BaseDir: findBaseDir(), Debug: true}

	ui, err := ultralightui.NewFromFS(screenWidth, screenHeight, "ui/index.html", uiFiles, opts)
	if err != nil {
		return nil, fmt.Errorf("NewFromFS: %w", err)
	}

	g := &Game{ui: ui}
	ui.OnMessage = g.handleMessage
	ui.SetBounds(0, 0, screenWidth, screenHeight)
	ui.SetFocus()

	return g, nil
}

func (g *Game) Update() error {
	g.counter++
	if err := g.ui.Update(); err != nil {
		return err
	}
	if g.counter%60 == 0 {
		g.ui.Eval(fmt.Sprintf("if(typeof updateCounter==='function')updateCounter(%d)", g.counter/60))
	}
	return nil
}

func (g *Game) Draw(screen *ebiten.Image) {
	screen.DrawImage(g.ui.GetTexture(), nil)
	ebitenutil.DebugPrint(screen, fmt.Sprintf("FPS: %.1f  VFS files: %d", ebiten.ActualFPS(), ultralightui.VFSFileCount()))
}

func (g *Game) Layout(_, _ int) (int, int) {
	return screenWidth, screenHeight
}

func (g *Game) handleMessage(msg string) {
	log.Printf("[embed UI] message: %s", msg)
	switch msg {
	case "greet":
		g.ui.Eval("showMessage('Hello from embedded Go!')")
	default:
		g.ui.Eval(fmt.Sprintf("showMessage('Go received: %s')", msg))
	}
}

func main() {
	logFile, err := os.Create("logs.log")
	if err == nil {
		log.SetOutput(logFile)
		defer logFile.Close()
	}
	log.SetFlags(log.Ltime | log.Lmicroseconds)

	game, err := newGame()
	if err != nil {
		log.Fatalf("init: %v", err)
	}
	defer game.ui.Close()

	ebiten.SetVsyncEnabled(false)
	ebiten.SetWindowSize(screenWidth, screenHeight)
	ebiten.SetWindowTitle("ultralightui - embed.FS example")
	ebiten.SetWindowResizingMode(ebiten.WindowResizingModeDisabled)

	if err := ebiten.RunGame(game); err != nil {
		log.Fatalf("run: %v", err)
	}
}
