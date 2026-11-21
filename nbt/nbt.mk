# Load after the pinned milestone-7 Makefile and run from that directory.
# CARLIFE_NBT is the absolute path to this directory.
CARLIFE_OBJECTS = src/usb_direct.o src/mmr_launch.o \
                  src/car/ccs_input.o src/car/volume_input.o src/most/client.o src/dsi_client.o src/sdr_builder.o src/gcf_client.o \
                  $(addprefix src/audio/,$(addsuffix .o,asound_device audio_route browser_audio config drift mixer playout stream_rate resampler pcm_ring speech_out speech_focus a2i_stream mic_uplink speex))

CARLIFE_SOURCES = $(addprefix $(CARLIFE_NBT)/,carlife_session.cpp wifi_transport.cpp nbt_main.cpp discovery.cpp display_commands.cpp firewall.cpp wifi_credentials.cpp wifi_key.cpp)
CARLIFE_HEADERS = $(wildcard $(CARLIFE_NBT)/*.hpp)
CARLIFE_GUARDED = .carlife-video_manager.cpp .carlife-touch_probe.cpp

.carlife-%.cpp: src/%.cpp $(CARLIFE_NBT)/guard-supra.awk
	awk -v module="$*" -f "$(CARLIFE_NBT)/guard-supra.awk" "$<" > "$@.tmp"
	mv "$@.tmp" "$@"

nbt-carlife: $(CARLIFE_OBJECTS) $(CARLIFE_SOURCES) $(CARLIFE_HEADERS) $(CARLIFE_GUARDED)
	$(CXX) $(CXXFLAGS) -I$(CARLIFE_NBT) -o $@ $(CARLIFE_OBJECTS) $(CARLIFE_SOURCES) $(CARLIFE_GUARDED) -L./lib -lsocket -lscreen -lusbdi -la2i_client -lang-c++ -lm -Wl,-rpath,/opt/mm/lib:/lib:/usr/lib

libNbtCarLife.so: $(CARLIFE_NBT)/rhmi/carlife_app.cpp $(CARLIFE_HEADERS)
	$(PLUGIN_CXX) $(PLUGIN_CXXFLAGS) -shared -Wl,-soname,libNbtCarLife.so -o $@ $< $(RHMI_LIBS) -Wl,--allow-shlib-undefined -lang-c++
