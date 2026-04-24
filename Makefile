LIB         = libc-utils.a
CC          = gcc
AR          = ar
BUILD_DIR   = build
TEST_DIR    = build-test
STACK_LIMIT = 65536

# Warning flags (shared across all variants)
WARN_FLAGS  = -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
              -Wunused-variable -Wunused-parameter -Wunused-result \
              -Wdouble-promotion -Wformat=2 -Wformat-truncation \
              -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
              -Wcast-align -Wcast-qual -Wnull-dereference \
              -Wconversion -Wsign-conversion

# Hardening flags
HARDEN_FLAGS = -fstack-protector-strong -fstack-clash-protection

# Common flags (all variants)
COMMON_CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L \
                $(WARN_FLAGS) $(HARDEN_FLAGS) \
                -Iinclude -Ilib/cJSON

# Build variants
RELEASE_CFLAGS = $(COMMON_CFLAGS) -O2
DEBUG_CFLAGS   = $(COMMON_CFLAGS) -Og -g
ASAN_CFLAGS    = $(COMMON_CFLAGS) -O1 -g -fsanitize=address -fno-omit-frame-pointer
COV_CFLAGS     = $(COMMON_CFLAGS) -Og -g --coverage

# Default to release
CFLAGS = $(RELEASE_CFLAGS)

# Vendor-relaxed flags (vendored code we don't control)
VENDOR_RELAX = -Wno-conversion -Wno-sign-conversion -Wno-cast-qual \
               -Wno-double-promotion -Wno-unused-parameter

# Source files
SRCS     = src/error.c \
           src/db.c \
           src/migrations.c \
           src/config.c \
           src/config_yaml.c \
           src/log.c \
           src/push.c \
           src/error_loop.c \
           src/appguard.c
VENDOR   = lib/cJSON/cJSON.c

# Object files routed to build/
SRC_OBJS    = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
VENDOR_OBJS = $(patsubst lib/cJSON/%.c,$(BUILD_DIR)/vendor/%.o,$(VENDOR))
ALL_OBJS    = $(SRC_OBJS) $(VENDOR_OBJS)

# Build static library (release)
$(BUILD_DIR)/$(LIB): $(ALL_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

# Build variants
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean-lib $(BUILD_DIR)/$(LIB)

asan: CFLAGS = $(ASAN_CFLAGS)
asan: clean-lib $(BUILD_DIR)/$(LIB)

# Directories
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)/vendor $(BUILD_DIR)/analyze

$(TEST_DIR):
	@mkdir -p $(TEST_DIR)/obj $(TEST_DIR)/vendor $(TEST_DIR)/bin

# Compile source files (library build)
$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vendor/%.o: lib/cJSON/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(VENDOR_RELAX) -c $< -o $@

# Syntax check
check:
	@$(CC) $(CFLAGS) -fsyntax-only $(SRCS)

# --- Tests ---
# Test objects and binaries go to TEST_DIR, keeping build/ clean

TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(patsubst tests/%.c,$(TEST_DIR)/bin/%,$(TEST_SRCS))

# Test library (separate from release library, may have different flags)
TEST_SRC_OBJS    = $(patsubst src/%.c,$(TEST_DIR)/obj/%.o,$(SRCS))
TEST_VENDOR_OBJS = $(patsubst lib/cJSON/%.c,$(TEST_DIR)/vendor/%.o,$(VENDOR))
TEST_ALL_OBJS    = $(TEST_SRC_OBJS) $(TEST_VENDOR_OBJS)
TEST_LIB         = $(TEST_DIR)/$(LIB)
TEST_LIBS        = -L$(TEST_DIR) -lc-utils -lsqlite3 -lcurl -lcrypto -lcmocka -lpthread

$(TEST_DIR)/obj/%.o: src/%.c | $(TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_DIR)/vendor/%.o: lib/cJSON/%.c | $(TEST_DIR)
	$(CC) $(CFLAGS) $(VENDOR_RELAX) -c $< -o $@

$(TEST_LIB): $(TEST_ALL_OBJS) | $(TEST_DIR)
	$(AR) rcs $@ $^

$(TEST_DIR)/bin/%: tests/%.c $(TEST_LIB) | $(TEST_DIR)
	$(CC) $(CFLAGS) -Isrc $< -o $@ $(TEST_LIBS)

test: CFLAGS = $(DEBUG_CFLAGS)
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t ==="; ./$$t || exit 1; done

test-asan: CFLAGS = $(ASAN_CFLAGS)
test-asan: TEST_LIBS += -fsanitize=address
test-asan: clean-test $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t (asan) ==="; ./$$t || exit 1; done

# Coverage
coverage: CFLAGS = $(COV_CFLAGS)
coverage: TEST_LIBS += --coverage
coverage: clean-test $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t (cov) ==="; ./$$t || exit 1; done
	@gcovr --root . --object-directory $(TEST_DIR) --filter 'src/' --print-summary
	@gcovr --root . --object-directory $(TEST_DIR) --filter 'src/' --branches --print-summary 2>/dev/null || true

# Static analysis
ANALYZE_CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L $(WARN_FLAGS) -O2 -Iinclude -Ilib/cJSON

analyze: check
	@echo "=== stack-usage ==="
	@mkdir -p $(BUILD_DIR)/analyze
	@for f in $(SRCS); do \
	    base=$$(basename $$f .c); \
	    $(CC) $(ANALYZE_CFLAGS) -fstack-usage -c $$f \
	        -o $(BUILD_DIR)/analyze/$$base.o 2>/dev/null; \
	done
	@fail=0; for su in $(BUILD_DIR)/analyze/*.su; do \
	    [ -f "$$su" ] || continue; \
	    awk -v limit=$(STACK_LIMIT) -v file="$$su" \
	        '$$2+0 > limit { printf "STACK OVERFLOW RISK: %s %s (%s bytes, limit %d)\n", file, $$1, $$2, limit; found=1 } \
	         END { exit (found ? 1 : 0) }' "$$su" || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "stack-usage: FAIL"; exit 1; fi
	@echo "stack-usage: OK"
	@echo "=== gcc-fanalyzer ==="
	@for f in $(SRCS); do \
	    $(CC) $(ANALYZE_CFLAGS) -fanalyzer -fsyntax-only $$f 2>&1; \
	done
	@echo "gcc-fanalyzer: OK"
	@echo "=== cppcheck ==="
	@cppcheck --enable=warning,performance,portability --error-exitcode=1 \
	    --suppress=missingIncludeSystem \
	    --suppress=normalCheckLevelMaxBranches \
	    --suppress=toomanyconfigs \
	    --inline-suppr --quiet \
	    -Iinclude -Ilib/cJSON $(SRCS)
	@echo "cppcheck: OK"

# Linting
lint:
	@echo "=== clang-tidy ==="
	@clang-tidy $(SRCS) -- -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude -Ilib/cJSON 2>&1 | \
	    grep -E "warning:|error:" || echo "clang-tidy: OK"

# Clean
clean-lib:
	rm -rf $(BUILD_DIR)

clean-test:
	rm -rf $(TEST_DIR)

clean: clean-lib clean-test

.PHONY: check test test-asan coverage debug asan analyze lint clean clean-lib clean-test
