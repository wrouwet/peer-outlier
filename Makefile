# Convenience wrapper. The library itself builds with plain CMake (see
# README.md) or as a Zephyr module; this Makefile only runs the host tests
# and builds the Zephyr sample.
#
#   make test                                 host tests (gcc, ASan/UBSan)
#   make zephyr BOARD=<board> [ZEPHYR_BASE=]  Zephyr sample for any board

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

HOST_CC     ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion \
               -Wdouble-promotion -fsanitize=address,undefined
SRCS := $(wildcard $(REPO)/src/*.c) $(REPO)/adapters/cmis/src/pod_cmis.c $(REPO)/sim/fleet_sim.c \
        $(REPO)/tests/host/test_core.c $(REPO)/tests/host/test_cmis.c
HDRS := $(wildcard $(REPO)/include/pod/*.h $(REPO)/adapters/cmis/include/pod_cmis/*.h $(REPO)/sim/*.h)
INCS := -I$(REPO)/include -I$(REPO)/adapters/cmis/include -I$(REPO)/sim
TEST := $(REPO)/build/host/test_pod

.DEFAULT_GOAL := help
.PHONY: help test zephyr clean

help:
	@echo "make test                                  Host unit tests and simulated scenarios"
	@echo "make zephyr BOARD=<board> [ZEPHYR_BASE=]   Build samples/fleet_sim for a Zephyr board"
	@echo "make clean"

$(TEST): $(SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(INCS) $(SRCS) -lm -o $@

test: $(TEST)
	$(TEST)

zephyr:
	@test -n "$(BOARD)" || { echo "set BOARD=<zephyr board>"; exit 1; }
	@test -n "$(ZEPHYR_BASE)" || { echo "set ZEPHYR_BASE (or source zephyr-env.sh)"; exit 1; }
	west build -b $(BOARD) -d $(REPO)/build/$(subst /,_,$(BOARD)) $(REPO)/samples/fleet_sim \
		-- -DZEPHYR_EXTRA_MODULES=$(REPO)

clean:
	rm -rf $(REPO)/build
