LIB      = libc-utils.a
CC       = gcc
AR       = ar
CFLAGS   = -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
           -Wunused-variable -Wunused-parameter -Wunused-result \
           -Wdouble-promotion -Wformat=2 -Wformat-truncation \
           -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
           -Wcast-align -Wcast-qual -Wnull-dereference \
           -Wconversion -Wsign-conversion \
           -fstack-protector-strong -fstack-clash-protection \
           -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
           -Iinclude -Ilib/cJSON

# Source files
SRCS     = src/error.c \
           src/db.c \
           src/migrations.c \
           lib/cJSON/cJSON.c
OBJS     = $(SRCS:.c=.o)

# Build static library
$(LIB): $(OBJS)
	$(AR) rcs $@ $^

# Compile source files
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lib/cJSON/%.o: lib/cJSON/%.c
	$(CC) $(CFLAGS) -Wno-conversion -Wno-sign-conversion -Wno-cast-qual \
	      -Wno-double-promotion -Wno-unused-parameter -c $< -o $@

# Syntax check
check:
	$(CC) $(CFLAGS) -fsyntax-only $(filter src/%.c,$(SRCS))

# Tests (cmocka)
TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(TEST_SRCS:.c=)
TEST_CFLAGS = $(CFLAGS)
TEST_LIBS = -L. -lc-utils -lsqlite3 -lcurl -lcrypto -lcmocka -lpthread

tests/%: tests/%.c $(LIB)
	$(CC) $(TEST_CFLAGS) $< -o $@ $(TEST_LIBS)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "=== $$t ==="; ./$$t || exit 1; done

# Static analysis
analyze: check
	$(CC) $(CFLAGS) -fanalyzer $(filter src/%.c,$(SRCS)) -fsyntax-only
	cppcheck --enable=warning,performance,portability \
	         --suppress=missingIncludeSystem \
	         -Iinclude -Ilib/cJSON src/

# Linting
lint:
	clang-tidy $(filter src/%.c,$(SRCS)) -- $(CFLAGS)

# Clean
clean:
	rm -f $(OBJS) $(LIB) $(TEST_BINS)

.PHONY: check test analyze lint clean
