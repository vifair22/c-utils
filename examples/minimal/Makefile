# Minimal c-utils consumer example.
#
# Assumes c-utils lives two directories up:
#   ~/git/home/c-utils/         (this is the library)
#     examples/
#       minimal/                (this is the example)
#
# Build c-utils first, then run `make` here:
#
#   $ make -C ../../        # build libc-utils.a in ../../build/
#   $ make                   # build ./minimal
#   $ ./minimal              # generates config.yaml template on first run, exits
#   $ ./minimal              # parses config.yaml, runs end-to-end
#
# For a consuming app that lives elsewhere on the filesystem, set
# CUTILS_DIR explicitly.

CUTILS_DIR ?= ../..
BUILD_DIR   = $(CUTILS_DIR)/build
LIB         = $(BUILD_DIR)/libc-utils.a

INCLUDES = -I$(CUTILS_DIR)/include -I$(CUTILS_DIR)/lib/cJSON
LIBS     = $(LIB) -lsqlite3 -lcurl -lcrypto -lpthread

CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2

minimal: main.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) main.c -o $@ $(LIBS)

$(LIB):
	$(MAKE) -C $(CUTILS_DIR)

clean:
	rm -f minimal config.yaml app.db app.db-wal app.db-shm

.PHONY: clean
