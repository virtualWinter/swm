# Minimal C workspace for a wlroots-based compositor.
# Uses make + gcc, plain pkg-config for dependencies.

CC        ?= gcc
CFLAGS    ?= -std=gnu99 -Wall -Wextra -O2 -g -DWLR_USE_UNSTABLE -MMD -MP
PKG_CONFIG ?= pkg-config

# wlroots (installed as wlroots-0.20) plus the wayland server lib it wraps.
PKGS       = wlroots-0.20 wayland-server xkbcommon
LDLIBS    += $(shell $(PKG_CONFIG) --libs $(PKGS))
CFLAGS    += $(shell $(PKG_CONFIG) --cflags $(PKGS))

SRC_DIR    = src
BUILD_DIR  = build

PREFIX    ?= $(HOME)/.local
DESTDIR   ?=
BINDIR     = $(DESTDIR)$(PREFIX)/bin

SRCS       = $(wildcard $(SRC_DIR)/*.c)
OBJS       = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TARGET     = $(BUILD_DIR)/swm

all: $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

run: all
	$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(BINDIR)/swm

uninstall:
	rm -f $(BINDIR)/swm

.PHONY: all clean run install uninstall
