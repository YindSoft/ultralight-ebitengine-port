# Makefile for ultralight-ebitengine-port
# Compiles the C bridge shared library for the current platform.

BRIDGE_SRC = bridge/ul_bridge.c
CFLAGS     = -O2

# Auto-detect platform
ifeq ($(OS),Windows_NT)
  BRIDGE_OUT = ul_bridge.dll
  BRIDGE_CMD = gcc -shared -o $(BRIDGE_OUT) $(BRIDGE_SRC) $(CFLAGS) -lkernel32
else
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Darwin)
    BRIDGE_OUT = libul_bridge.dylib
    BRIDGE_CMD = gcc -shared -fPIC -o $(BRIDGE_OUT) $(BRIDGE_SRC) $(CFLAGS) -lpthread -ldl
  else
    BRIDGE_OUT = libul_bridge.so
    BRIDGE_CMD = gcc -shared -fPIC -o $(BRIDGE_OUT) $(BRIDGE_SRC) $(CFLAGS) -lpthread -ldl
  endif
endif

.PHONY: bridge bridge-windows bridge-linux bridge-macos clean test vet

# Default: build for current platform
bridge:
	$(BRIDGE_CMD)

bridge-windows:
	gcc -shared -o ul_bridge.dll $(BRIDGE_SRC) $(CFLAGS) -lkernel32

bridge-linux:
	gcc -shared -fPIC -o libul_bridge.so $(BRIDGE_SRC) $(CFLAGS) -lpthread -ldl

bridge-macos:
	gcc -shared -fPIC -o libul_bridge.dylib $(BRIDGE_SRC) $(CFLAGS) -lpthread -ldl

test:
	go test -v ./...

vet:
	go vet ./...

clean:
	rm -f ul_bridge.dll libul_bridge.so libul_bridge.dylib
