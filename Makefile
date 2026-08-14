CC        ?= cc
CXX       ?= c++
CFLAGS    ?= -O2 -Wall -Wextra -std=gnu17
CXXFLAGS  ?= -O2 -Wall -Wextra -std=gnu++17

BUILD_DIR := build
LIB_SRC   := src/libschrodinger.c
LIB_TARGET := $(BUILD_DIR)/libschrodinger.so
DLG_SRC   := src/schrodinger-dialog.cpp
DLG_TARGET := $(BUILD_DIR)/schrodinger-dialog

QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets)
QT_LIBS   := $(shell pkg-config --libs Qt6Widgets)

all: $(LIB_TARGET) $(DLG_TARGET)

$(LIB_TARGET): $(LIB_SRC) | $(BUILD_DIR)
	$(CC) -shared -fPIC $(CFLAGS) -o $@ $< -ldl

$(DLG_TARGET): $(DLG_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -o $@ $< $(QT_LIBS)

$(BUILD_DIR):
	mkdir -p $@

COMPILE_DB := $(BUILD_DIR)/compile_commands.json

# Emit a clean compilation database for clangd/clang-tidy with exactly the
# flags make uses (no compiler-wrapper hardening noise). pkg-config metadata
# is expanded by make before jq embeds it in the JSON command strings.
compile_commands: | $(BUILD_DIR)
	@jq -n \
		--arg dir "$(CURDIR)" \
		--arg cmd1 "$(CC) -fPIC $(CFLAGS) $(LIB_SRC)" \
		--arg file1 "$(LIB_SRC)" \
		--arg cmd2 "$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $(DLG_SRC)" \
		--arg file2 "$(DLG_SRC)" \
		'[{directory: $$dir, command: $$cmd1, file: $$file1}, {directory: $$dir, command: $$cmd2, file: $$file2}]' \
		> $(COMPILE_DB)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean compile_commands
