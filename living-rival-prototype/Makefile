HOSTCC ?= gcc
CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror -Iinclude
BUILD := build
SRCS := $(wildcard src/*.c)
HDRS := $(wildcard include/*.h)
TESTS := $(wildcard test/test_*.c)

ifeq ($(OS),Windows_NT)
EXESUFFIX := .exe
else
EXESUFFIX :=
endif

BINS := $(patsubst test/%.c,$(BUILD)/%$(EXESUFFIX),$(TESTS))

.PHONY: test clean

test: $(BINS)
	@for t in $(BINS); do echo "== $$t"; "$$t" || exit 1; done

$(BUILD)/%$(EXESUFFIX): test/%.c $(SRCS) $(HDRS) | $(BUILD)
	$(HOSTCC) $(CFLAGS) $< $(SRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
