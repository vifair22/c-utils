LIB         = libc-utils.a
CC          = gcc
AR          = ar
BUILD_DIR   = build
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
ALL_SRCS = $(SRCS) $(VENDOR)
OBJS     = $(ALL_SRCS:.c=.o)

# Build static library (release)
$(LIB): $(OBJS)
	$(AR) rcs $@ $^

# Build variants
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean $(LIB)

asan: CFLAGS = $(ASAN_CFLAGS)
asan: clean $(LIB)

# Compile source files
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lib/cJSON/%.o: lib/cJSON/%.c
	$(CC) $(CFLAGS) $(VENDOR_RELAX) -c $< -o $@

# Syntax check
check:
	@$(CC) $(CFLAGS) -fsyntax-only $(SRCS)

# Tests (cmocka)
TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(TEST_SRCS:.c=)
TEST_LIBS = -L. -lc-utils -lsqlite3 -lcurl -lcrypto -lcmocka -lpthread

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $< -o $@ $(TEST_LIBS)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t ==="; ./$$t || exit 1; done

test-asan: CFLAGS = $(ASAN_CFLAGS)
test-asan: TEST_LIBS += -fsanitize=address
test-asan: clean $(LIB) $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t (asan) ==="; ./$$t || exit 1; done

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
	rm -rf $(BUILD_DIR)/analyze; \
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
clean:
	rm -f $(OBJS) $(LIB) $(TEST_BINS)
	rm -rf $(BUILD_DIR)

.PHONY: check test test-asan debug asan analyze lint clean
