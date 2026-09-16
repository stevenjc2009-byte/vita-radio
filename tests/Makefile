# Host unit tests for the pure-C modules (run inside WSL):
#   make -C tests          build + run every test_*.c
#   make -C tests BUILD=build-host-x   separate object dir for parallel runs
CC     ?= gcc
BUILD  ?= build-host
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I../src -fsanitize=address,undefined
LDLIBS := -lpthread

# module under test for each test binary
SRC_test_ringbuf := ../src/ringbuf.c
SRC_test_icy     := ../src/icy.c
SRC_test_sniff   := ../src/sniff.c
# playlist.c is pure C, so player.c links the real one (as it does ringbuf and
# sniff); hls.c needs libcurl, so test_player.c stubs it (as it does http_stream).
SRC_test_player  := ../src/player.c ../src/ringbuf.c ../src/sniff.c ../src/playlist.c ../src/url_util.c
SRC_test_updater_logic := ../src/version.c ../src/release_json.c
SRC_test_pkg_meta := ../src/pkg_meta.c ../src/sha1.c
SRC_test_zip_extract := ../src/zip_extract.c ../src/fs_util.c

# v2.0.0 modules. Every one of these is pure C with no libcurl and no Vita
# headers, which is why each is unit-testable on the host at all.
SRC_test_url_util     := ../src/url_util.c
SRC_test_m3u8         := ../src/m3u8.c ../src/url_util.c
SRC_test_ts_demux     := ../src/ts_demux.c
SRC_test_adts_scan    := ../src/adts_scan.c
SRC_test_aes128       := ../src/aes128.c
SRC_test_hls_crypt    := ../src/hls_crypt.c ../src/aes128.c
SRC_test_json         := ../src/json.c
SRC_test_playlist     := ../src/playlist.c ../src/url_util.c
SRC_test_station_list := ../src/station_list.c ../src/stations.c
SRC_test_favourites   := ../src/favourites.c ../src/station_list.c ../src/stations.c

# The host has zlib's runtime but no dev headers: borrow the (portable)
# zlib.h/zconf.h from the VitaSDK, read-only, and link libz.so.1 by name.
VITASDK ?= $(HOME)/vitasdk
ZINC    := $(BUILD)/zinc
INC_test_zip_extract := -I$(ZINC)
LIB_test_zip_extract := -l:libz.so.1
DEP_test_zip_extract := $(ZINC)/zlib.h

TESTS := $(patsubst %.c,%,$(wildcard test_*.c))
BINS  := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all run clean
all: run

.SECONDEXPANSION:

$(BUILD)/%: %.c $$(SRC_$$*) $$(DEP_$$*) ../src/*.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC_$*) -o $@ $< $(SRC_$*) $(LDLIBS) $(LIB_$*)

$(ZINC)/zlib.h: | $(BUILD)
	mkdir -p $(ZINC)
	cp $(VITASDK)/arm-vita-eabi/include/zlib.h $(VITASDK)/arm-vita-eabi/include/zconf.h $(ZINC)/

$(BUILD):
	mkdir -p $@

run: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t"; ./$$t || { echo "FAILED: $$t (exit $$?)"; fail=1; }; done; \
	echo "== tests run: $(words $(BINS))"; exit $$fail

clean:
	rm -rf $(BUILD)
