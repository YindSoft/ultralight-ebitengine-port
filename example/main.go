// Copyright (c) 2026 Javier Podavini (YindSoft)
// Licensed under the MIT License. See LICENSE file in the project root.

package main

import (
	"fmt"
	"image/color"
	"log"
	"math"
	"os"
	"path/filepath"

	ultralightui "github.com/YindSoft/ultralight-ebitengine-port"
	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/ebitenutil"
	"github.com/hajimehoshi/ebiten/v2/vector"
)

const (
	screenWidth  = 800
	screenHeight = 600
	mainUIWidth  = 600
	sidebarWidth = 200
)

type Game struct {
	mainUI  *ultralightui.UltralightUI
	sidebar *ultralightui.UltralightUI
	counter int
}

func findBaseDir() string {
	// Look for ul_bridge.dll in current dir, then parent (for running from example/).
	if _, err := os.Stat("ul_bridge.dll"); err == nil {
		return ""
	}
	if _, err := os.Stat(filepath.Join("..", "ul_bridge.dll")); err == nil {
		return ".."
	}
	return ""
}

func newGame() (*Game, error) {
	opts := &ultralightui.Options{BaseDir: findBaseDir()}

	mainUI, err := ultralightui.NewFromFile(mainUIWidth, screenHeight, "ui/index.html", opts)
	if err != nil {
		return nil, fmt.Errorf("main UI: %w", err)
	}

	sidebar, err := ultralightui.NewFromFile(sidebarWidth, screenHeight, "ui/sidebar.html", opts)
	if err != nil {
		mainUI.Close()
		return nil, fmt.Errorf("sidebar UI: %w", err)
	}

	g := &Game{mainUI: mainUI, sidebar: sidebar}
	mainUI.OnMessage = g.handleMainMessage
	sidebar.OnMessage = g.handleSidebarMessage

	mainUI.SetBounds(0, 0, mainUIWidth, screenHeight)
	sidebar.SetBounds(mainUIWidth, 0, sidebarWidth, screenHeight)
	mainUI.SetFocus()

	return g, nil
}

func (g *Game) Update() error {
	g.counter++

	if err := g.mainUI.Update(); err != nil {
		return err
	}
	if err := g.sidebar.Update(); err != nil {
		return err
	}

	// Send a counter update every second
	if g.counter%60 == 0 {
		g.mainUI.Eval(fmt.Sprintf("if(typeof updateCounter==='function')updateCounter(%d)", g.counter/60))
	}

	return nil
}

func (g *Game) Draw(screen *ebiten.Image) {
	screen.Fill(color.RGBA{30, 30, 40, 255})

	// Animated shapes behind the HTML (visible through transparent areas)
	t := float64(g.counter) / 60.0
	bx := float32(100 + 80*math.Sin(t*0.5))
	by := float32(200 + 60*math.Cos(t*0.7))
	vector.DrawFilledRect(screen, bx, by, 120, 120, color.RGBA{0, 200, 80, 255}, true)
	vector.DrawFilledRect(screen, bx+140, by+30, 80, 80, color.RGBA{200, 180, 0, 255}, true)

	// Main UI (left, semi-transparent so background shapes show through)
	optsMain := &ebiten.DrawImageOptions{}
	optsMain.ColorScale.Scale(1, 1, 1, 0.5)
	screen.DrawImage(g.mainUI.GetTexture(), optsMain)

	// Sidebar (right)
	optsSidebar := &ebiten.DrawImageOptions{}
	optsSidebar.ColorScale.Scale(1, 1, 1, 0.5)
	optsSidebar.GeoM.Translate(mainUIWidth, 0)
	screen.DrawImage(g.sidebar.GetTexture(), optsSidebar)

	// Animated shape on top of everything
	fx := float32(500 + 50*math.Sin(t*0.8))
	fy := float32(50 + 30*math.Cos(t*0.6))
	vector.DrawFilledRect(screen, fx, fy, 100, 100, color.RGBA{255, 50, 50, 128}, true)

	ebitenutil.DebugPrint(screen, fmt.Sprintf("FPS: %.1f  TPS: %.1f", ebiten.ActualFPS(), ebiten.ActualTPS()))
}

func (g *Game) Layout(_, _ int) (int, int) {
	return screenWidth, screenHeight
}

func (g *Game) handleMainMessage(msg string) {
	log.Printf("[main UI] message: %s", msg)
	switch msg {
	case "greet":
		g.mainUI.Eval("showMessage('Hello from Go!')")
	case "count":
		g.mainUI.Eval(fmt.Sprintf("showMessage('Counter is at %d')", g.counter/60))
	default:
		g.mainUI.Eval(fmt.Sprintf("showMessage('Go received: %s')", msg))
	}
}

func (g *Game) handleSidebarMessage(msg string) {
	log.Printf("[sidebar] message: %s", msg)
	g.sidebar.Send(map[string]string{"echo": msg, "status": "ok"})
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
	defer game.mainUI.Close()
	defer game.sidebar.Close()

	ebiten.SetVsyncEnabled(false)
	ebiten.SetWindowSize(screenWidth, screenHeight)
	ebiten.SetWindowTitle("ultralightui - Ebiten + Ultralight demo")
	ebiten.SetWindowResizingMode(ebiten.WindowResizingModeDisabled)

	if err := ebiten.RunGame(game); err != nil {
		log.Fatalf("run: %v", err)
	}
}
