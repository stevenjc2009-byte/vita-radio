# Vita Radio - plain make build (no cmake).
#   make                 -> build/VitaRadio.vpk
#   make BUILD=build-x   -> separate object/output dir
#   make TLS=openssl     -> force the SDK's curl + OpenSSL instead of vendored curl+mbedTLS
#   make clean           -> removes $(BUILD) only

VITASDK ?= $(HOME)/vitasdk
PREFIX  := $(VITASDK)/bin/arm-vita-eabi
CC      := $(PREFIX)-gcc
BUILD   ?= build

TITLE     := Vita Radio
TITLE_ID  := VRAD00001
TARGET    := vita_radio
VPK_NAME  := VitaRadio.vpk

TLS_DIR := third_party/tls-mbedtls
ifeq ($(wildcard $(TLS_DIR)/lib/libcurl.a),)
TLS ?= openssl
else
TLS ?= mbedtls
endif
$(info TLS backend: $(TLS))

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

CFLAGS := -std=gnu11 -Wall -Wextra -O2 -Isrc -DCURL_STATICLIB -MMD -MP

ifeq ($(TLS),mbedtls)
CFLAGS   += -I$(TLS_DIR)/include
TLS_LIBS := -L$(TLS_DIR)/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
else ifeq ($(TLS),openssl)
TLS_LIBS := -lcurl -lssl -lcrypto
else
$(error TLS must be mbedtls or openssl, got '$(TLS)')
endif
CFLAGS += -I$(VITASDK)/arm-vita-eabi/include

# Static link order: each library before the libraries it depends on.
LIBS := $(TLS_LIBS) -lz -lzstd \
	-lavcodec -lswresample -lavutil -lmp3lame -lmpg123 \
	-lvita2d -lfreetype -lpng -lbz2 -lz \
	-lSceDisplay_stub -lSceGxm_stub -lSceSysmodule_stub -lSceCtrl_stub \
	-lScePgf_stub -lScePvf_stub -lSceCommonDialog_stub -lSceAppUtil_stub \
	-lSceAudio_stub -lSceNet_stub -lSceNetCtl_stub -lScePower_stub \
	-lSceAppMgr_stub -lSceRtc_stub -lSceIofilemgr_stub -lSceLibKernel_stub \
	-lSceKernelThreadMgr_stub -lSceProcessmgr_stub -lSceSysmem_stub \
	-lSceKernelModulemgr_stub -lScePromoterUtil_stub \
	-lpthread -lm -lc -lgcc

# Updater helper title: installs a downloaded release over Vita Radio.
UPD_TITLE    := Vita Radio Updater
UPD_TITLE_ID := VRADUPDTR
UPD_SRCS     := updater/main.c src/promote.c src/pkg_meta.c src/sha1.c
UPD_OBJS     := $(patsubst %.c,$(BUILD)/upd/%.o,$(UPD_SRCS))
UPD_LIBS     := -lSceAppMgr_stub -lScePromoterUtil_stub -lSceSysmodule_stub \
	-lSceIofilemgr_stub -lSceLibKernel_stub -lSceKernelThreadMgr_stub \
	-lSceProcessmgr_stub -lSceSysmem_stub -lSceRtc_stub -lSceKernelModulemgr_stub \
	-lSceNet_stub -lSceNetCtl_stub -lpthread -lc -lgcc
DEPS += $(UPD_OBJS:.o=.d)

VPK_FILES := \
	sce_sys/icon0.png \
	sce_sys/livearea/contents/bg.png \
	sce_sys/livearea/contents/startup.png \
	sce_sys/livearea/contents/template.xml \
	assets/cacert.pem \
	assets/head.bin

.PHONY: all clean
all: $(BUILD)/$(VPK_NAME)

$(BUILD)/$(VPK_NAME): $(BUILD)/eboot.bin $(BUILD)/param.sfo $(VPK_FILES) \
		$(BUILD)/upd/eboot.bin $(BUILD)/upd/param.sfo
	$(VITASDK)/bin/vita-pack-vpk -s $(BUILD)/param.sfo -b $(BUILD)/eboot.bin \
		$(foreach f,$(VPK_FILES),-a $(f)=$(f)) \
		-a $(BUILD)/upd/eboot.bin=updater/eboot.bin \
		-a $(BUILD)/upd/param.sfo=updater/param.sfo $@

$(BUILD)/upd/eboot.bin: $(BUILD)/upd/updater.velf
	$(VITASDK)/bin/vita-make-fself -s $< $@

$(BUILD)/upd/param.sfo: | $(BUILD)
	mkdir -p $(BUILD)/upd
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(UPD_TITLE_ID) -d ATTRIBUTE2=12 "$(UPD_TITLE)" $@

$(BUILD)/upd/updater.velf: $(BUILD)/upd/updater.elf
	$(VITASDK)/bin/vita-elf-create $< $@

$(BUILD)/upd/updater.elf: $(UPD_OBJS)
	$(CC) -Wl,-q -o $@ $(UPD_OBJS) $(UPD_LIBS)

$(BUILD)/upd/%.o: %.c Makefile
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/eboot.bin: $(BUILD)/$(TARGET).velf
	$(VITASDK)/bin/vita-make-fself -s $< $@

$(BUILD)/param.sfo: | $(BUILD)
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(TITLE_ID) -d ATTRIBUTE2=12 "$(TITLE)" $@

$(BUILD)/$(TARGET).velf: $(BUILD)/$(TARGET).elf
	$(VITASDK)/bin/vita-elf-create $< $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) -Wl,-q -o $@ $(OBJS) $(LIBS)

$(BUILD)/%.o: src/%.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
