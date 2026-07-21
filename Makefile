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

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
TEST_DIR   = test
TEST_BIN   = $(BUILD_DIR)/test_swm

# Sources recompiled with -DTESTING so main() is excluded and config
# parser internals are exposed.
TEST_OBJS  = $(BUILD_DIR)/test_main.o \
             $(BUILD_DIR)/test_config.o \
             $(BUILD_DIR)/test_tests.o

# Only xkbcommon and libm are needed at link time: the config parser
# calls xkb_keysym_from_name, and the test uses fabsf.  The wlroots
# library is not required because the test objects don't call any
# wlr_* functions (the TESTING build excludes all such call sites).
TEST_LIBS  = -lxkbcommon -lm

$(BUILD_DIR)/test_main.o: src/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DTESTING -c $< -o $@

$(BUILD_DIR)/test_config.o: src/config.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DTESTING -c $< -o $@

$(BUILD_DIR)/test_tests.o: $(TEST_DIR)/tests.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DTESTING -I$(SRC_DIR) -c $< -o $@

$(TEST_BIN): $(TEST_OBJS)
	$(CC) $(TEST_OBJS) -o $@ $(TEST_LIBS)

test-unit: $(TEST_BIN)
	./$(TEST_BIN)

# Smoke test: launch swm nested inside the running Wayland session,
# connect with wayland-info, and shut down cleanly.
test-smoke: $(TARGET)
	$(TEST_DIR)/smoke.sh $(TARGET)

test: test-unit test-smoke

.PHONY: all clean run install uninstall test test-unit test-smoke
