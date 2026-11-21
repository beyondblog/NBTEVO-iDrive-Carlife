# Invoke from the repository root with SUPRA_SRC and QNX_TARGET set.
# 'objects' and 'link' can run on separate hosts using the same OUT contents.
.DEFAULT_GOAL := all
CLANGXX ?= clang++
QNX_LD ?= ld.lld
OUT ?= build/qnx-armv7le

QNX_LIB = $(QNX_TARGET)/armle-v7/lib
QNX_INCLUDE = $(QNX_TARGET)/usr/include
GXX_INCLUDE = $(QNX_INCLUDE)/c++/4.4.2
SUPRA_AUDIO = asound_device audio_route browser_audio config drift mixer playout stream_rate resampler pcm_ring speech_out speech_focus a2i_stream mic_uplink
SUPRA_MODULES = usb_direct video_manager mmr_launch touch_probe ccs_input volume_input client dsi_client sdr_builder gcf_client $(SUPRA_AUDIO)
GUARDED = video_manager touch_probe
OBJECTS = $(addprefix $(OUT)/,$(addsuffix .o,carlife_session wifi_transport nbt_main discovery display_commands firewall wifi_credentials wifi_key qnx_arm_rt $(SUPRA_MODULES))) $(OUT)/speex.o

vpath %.cpp nbt $(SUPRA_SRC)/src $(SUPRA_SRC)/src/car $(SUPRA_SRC)/src/most $(SUPRA_SRC)/src/audio

.PHONY: all objects link plugin
all: objects
	$(MAKE) -f nbt/clang-qnx.mk link
	$(MAKE) -f nbt/clang-qnx.mk plugin

objects: $(OBJECTS)

COMPILE = $(CLANGXX) --target=armv7-unknown-linux-gnueabi -U__linux__ -U__linux -Ulinux \
    -D__QNX__ -D__QNXNTO__ -D__ARM__ -D__LITTLEENDIAN__ -D_QNX_SOURCE \
    -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp -mno-unaligned-access -fno-pie \
    -Wno-c++11-narrowing -U__GXX_EXPERIMENTAL_CXX0X__ -nostdinc++ \
    -isystem "$(GXX_INCLUDE)" -isystem "$(GXX_INCLUDE)/arm-unknown-nto-qnx6.5.0eabi" \
    -isystem "$(QNX_INCLUDE)" -I"$(SUPRA_SRC)/include" -I"$(SUPRA_SRC)/src" -Inbt \
    -std=c++11 -O2 -MMD -MP -c "$<" -o "$@"

$(OUT)/%.o: %.cpp
	@mkdir -p "$(OUT)"
	$(COMPILE)

$(OUT)/speex.o: $(SUPRA_SRC)/src/audio/speex.c
	@mkdir -p "$(OUT)"
	$(CLANGXX) --target=armv7-unknown-linux-gnueabi -U__linux__ -U__linux -Ulinux \
	  -D__QNX__ -D__QNXNTO__ -D__ARM__ -D__LITTLEENDIAN__ -D_QNX_SOURCE -march=armv7-a \
	  -mfpu=vfpv3 -mfloat-abi=softfp -isystem "$(QNX_INCLUDE)" -I"$(SUPRA_SRC)/src" \
	  -x c -O2 -MMD -MP -c "$<" -o "$@"

$(addprefix $(OUT)/,$(addsuffix .o,$(GUARDED))): $(OUT)/%.o: $(OUT)/guarded_%.cpp
	$(COMPILE)

$(addprefix $(OUT)/guarded_,$(addsuffix .cpp,$(GUARDED))): $(OUT)/guarded_%.cpp: $(SUPRA_SRC)/src/%.cpp nbt/guard-supra.awk
	@mkdir -p "$(OUT)"
	awk -v module="$*" -f nbt/guard-supra.awk "$<" > "$@.tmp"
	mv "$@.tmp" "$@"

-include $(OBJECTS:.o=.d)

link:
	$(QNX_LD) -m armelf -dynamic-linker /usr/lib/ldqnx.so.2 --hash-style=sysv \
	  --no-undefined --no-allow-shlib-undefined -z max-page-size=4096 -o "$(OUT)/nbt-carlife" \
	  "$(QNX_LIB)/crt1.o" "$(QNX_LIB)/crti.o" "$(QNX_LIB)/crtbegin.o" \
	  $(OBJECTS) -L"$(SUPRA_SRC)/lib" -L"$(QNX_LIB)" -L"$(QNX_LIB)/gcc/4.4.2" \
	  -rpath-link "$(QNX_LIB)" -rpath /opt/mm/lib:/usr/lib:/lib \
	  -lscreen -lusbdi -lsocket -la2i_client -l:libstdc++.a -lm -lc -l:libc.a \
	  "$(QNX_LIB)/crtend.o" "$(QNX_LIB)/crtn.o"

$(OUT)/carlife_app.o: nbt/rhmi/carlife_app.cpp nbt/lifecycle.hpp nbt/firewall.hpp nbt/wifi_credentials.hpp
	@mkdir -p "$(OUT)"
	$(COMPILE) -fPIC -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables

plugin: $(OUT)/carlife_app.o
	$(QNX_LD) -m armelf -shared --no-undefined --hash-style=sysv -soname libNbtCarLife.so \
	  -z max-page-size=4096 -o "$(OUT)/libNbtCarLife.so" "$<" \
	  -L"$(SUPRA_SRC)/rhmiapp/lib" -L"$(QNX_LIB)" \
	  -l:libAppFramework.A.so -l:libCoreFramework.A.so -l:libstdc++.so.6 -lc
