# PC viewer for the OLED frames streamed by ../../lomenett_bridge.h (needs SDL2).
#
#   make run                      view /dev/ttyACM0
#   make run PORT=/dev/ttyACM1    another port
#   make run ZOOM=6               bigger pixels

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/bridge_viewer
PORT      ?= /dev/ttyACM0
ZOOM      ?= 4

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra $(shell pkg-config --cflags sdl2)
LDFLAGS  := $(shell pkg-config --libs sdl2)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): bridge_viewer.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET) $(PORT) $(ZOOM)

clean:
	rm -rf $(BUILD_DIR)
