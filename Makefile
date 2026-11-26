# Builds BatteryHub.exe with MinGW-w64 (MSYS2 MINGW64/UCRT64 shell, or any mingw g++ on PATH).
CXX      ?= g++
WINDRES  ?= windres
VERSION  ?= 0.0.0
CHANNEL  ?= dev
.DEFAULT_GOAL := BatteryHub.exe
LDFLAGS  ?= -municode -mwindows -static -s -Wl,--gc-sections -Lbuild
CXXFLAGS ?= -std=c++17 -Os -Wall -Wno-missing-field-initializers -ffunction-sections -fdata-sections \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -MMD -MP
LIBS     := -ld2d1 -ldwrite -lhid -lsetupapi -lcfgmgr32 -lole32 -loleaut32 -lcomctl32 -lgdi32 -lruntimeobject -lws2_32 -lwinhttp -lbcrypt -lversion -lbluetoothapis -lwinmm -lcomdlg32 -luxtheme -luuid -lshell32 -luser32 -mthreads

SRC := $(wildcard src/*.cpp) $(wildcard src/drivers/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC)) build/app_res.o
DEP := $(OBJ:.o=.d)

# The settings window renders an HTML page through WebView2; the SDK headers are vendored under
# third_party/webview2 (the loader DLL itself is embedded as a resource, see res/app.rc).
# third_party is not in the repository, so a fresh clone has to fetch it once.
WV2_INCLUDE := third_party/webview2/include
WV2_LOADER  := third_party/webview2/x64/WebView2Loader.dll
WV2_CFLAGS  := -I$(WV2_INCLUDE)

$(WV2_INCLUDE)/WebView2.h:
	@echo "The WebView2 SDK is missing. Fetch it once with:"
	@echo "    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/fetch-webview2.ps1"
	@exit 1

BatteryHub.exe: $(OBJ) | build/default-manifest.o
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

# GCC's specs add their own default-manifest.o when the name is found on the link path, which
# collides with the Common Controls v6 manifest in res/app.rc. This stub takes its place so only the
# RC manifest reaches the exe (without it TaskDialogIndirect has no entry point). The linker prints
# ".rsrc merge failure: multiple non-default manifests" while doing this; it is expected and the
# build still succeeds with the RC manifest in the exe.
build/default-manifest.o:
	@mkdir -p build
	@echo "int __batteryhub_manifest_stub;" > build/manifest_stub.c
	$(CXX) -c build/manifest_stub.c -o $@

	@mkdir -p build

# -MMD -MP writes the real header dependencies next to each object, so touching one header rebuilds
# only the sources that include it instead of the whole tree.
build/%.o: src/%.cpp build/version.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WV2_CFLAGS) -municode -c $< -o $@

build/settings_panel.o: $(WV2_INCLUDE)/WebView2.h

-include $(DEP)

# The version only rewrites build/version.h when it actually changes: otherwise every build would
# re-run windres and re-link the whole executable for nothing.
build/.version: FORCE
	@mkdir -p build
	@printf '%s %s\n' "$(VERSION)" "$(CHANNEL)" > build/.version.new
	@if cmp -s build/.version.new build/.version; then rm -f build/.version.new; \
	 else powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-version.ps1 -Version "$(VERSION)" -Channel "$(CHANNEL)" \
	      && mv build/.version.new build/.version; fi

build/version.h: build/.version
	@test -f $@ || powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-version.ps1 -Version "$(VERSION)" -Channel "$(CHANNEL)"
	@touch $@

# Every lang/*.txt is a built-in UI language: joined into one resource, each after an "@file <name>" line.
build/languages.txt: $(wildcard lang/*.txt)
	@mkdir -p build
	@printf '' > $@; for f in $^; do printf '@file %s\n' "$$(basename $$f .txt)" >> $@; cat "$$f" >> $@; printf '\n' >> $@; done

build/app_res.o: res/app.rc res/app.manifest res/app.ico res/app_light.ico res/MaterialIconsOutlined-Regular.otf res/settings.html $(WV2_LOADER) $(WV2_INCLUDE)/WebView2.h build/languages.txt build/version.h
	@mkdir -p build
	$(WINDRES) -i $< -o $@ --include-dir res --include-dir build

clean:
	rm -rf build BatteryHub.exe

test: test-update-core test-update-win test-update-signature
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall tests/device_aliases_test.cpp -o build/device_aliases_test.exe
	./build/device_aliases_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/alert_rules_test.cpp -o build/alert_rules_test.exe
	./build/alert_rules_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/refresh_policy_test.cpp -o build/refresh_policy_test.exe
	./build/refresh_policy_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/history_model_test.cpp -o build/history_model_test.exe
	./build/history_model_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/runtime_estimate_test.cpp -o build/runtime_estimate_test.exe
	./build/runtime_estimate_test.exe
	$(CXX) $(CXXFLAGS) tests/aula_mouse_driver_test.cpp src/drivers/aula_mouse.cpp -o build/aula_mouse_driver_test.exe
	./build/aula_mouse_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/darkrim_driver_test.cpp src/drivers/darkrim.cpp -o build/darkrim_driver_test.exe
	./build/darkrim_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/nuphy_driver_test.cpp src/drivers/nuphy.cpp -o build/nuphy_driver_test.exe
	./build/nuphy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/nuphy_protocol_test.cpp -o build/nuphy_protocol_test.exe
	./build/nuphy_protocol_test.exe
	$(CXX) $(CXXFLAGS) tests/maono_driver_test.cpp src/drivers/maono.cpp -o build/maono_driver_test.exe
	./build/maono_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/astro_driver_test.cpp src/drivers/astro.cpp -o build/astro_driver_test.exe
	./build/astro_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/corsair_bragi_driver_test.cpp src/drivers/corsair.cpp -o build/corsair_bragi_driver_test.exe
	./build/corsair_bragi_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/steelseries_mouse_test.cpp src/drivers/steelseries.cpp -o build/steelseries_mouse_test.exe
	./build/steelseries_mouse_test.exe
	$(CXX) $(CXXFLAGS) tests/pulsar_feinmann_test.cpp src/drivers/pulsar.cpp -o build/pulsar_feinmann_test.exe
	./build/pulsar_feinmann_test.exe
	$(CXX) $(CXXFLAGS) tests/razer_barracuda_chroma_test.cpp src/drivers/razer_headset.cpp -o build/razer_barracuda_chroma_test.exe
	./build/razer_barracuda_chroma_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/rapoo_protocol_test.cpp -o build/rapoo_protocol_test.exe
	./build/rapoo_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/sony_inzone_protocol_test.cpp -o build/sony_inzone_protocol_test.exe
	./build/sony_inzone_protocol_test.exe
	$(CXX) $(CXXFLAGS) tests/rapoo_driver_test.cpp src/drivers/rapoo.cpp -o build/rapoo_driver_test.exe
	./build/rapoo_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/rawm_protocol_test.cpp -o build/rawm_protocol_test.exe
	./build/rawm_protocol_test.exe
	$(CXX) $(CXXFLAGS) tests/rawm_driver_test.cpp src/drivers/rawm.cpp -o build/rawm_driver_test.exe
	./build/rawm_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/ninjutso_driver_test.cpp src/drivers/ninjutso.cpp -o build/ninjutso_driver_test.exe
	./build/ninjutso_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/cherry_driver_test.cpp src/drivers/cherry.cpp -o build/cherry_driver_test.exe
	./build/cherry_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/cherry_xtrfy_driver_test.cpp src/drivers/cherry.cpp -o build/cherry_xtrfy_driver_test.exe
	./build/cherry_xtrfy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/ninjutso_cable_test.cpp src/drivers/ninjutso.cpp -o build/ninjutso_cable_test.exe
	./build/ninjutso_cable_test.exe
	$(CXX) $(CXXFLAGS) tests/machenike_driver_test.cpp src/drivers/machenike.cpp -o build/machenike_driver_test.exe
	./build/machenike_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/niceboy_driver_test.cpp src/drivers/niceboy.cpp -o build/niceboy_driver_test.exe
	./build/niceboy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/redragon_evision_driver_test.cpp src/drivers/redragon_evision.cpp -o build/redragon_evision_driver_test.exe
	./build/redragon_evision_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/alienware_driver_test.cpp src/drivers/alienware.cpp -o build/alienware_driver_test.exe
	./build/alienware_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/audeze_driver_test.cpp src/drivers/audeze.cpp -o build/audeze_driver_test.exe
	./build/audeze_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/mchose_modern_driver_test.cpp src/drivers/mchose_modern.cpp -o build/mchose_modern_driver_test.exe
	./build/mchose_modern_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/mchose_v9pro_driver_test.cpp src/drivers/mchose_v9pro.cpp -o build/mchose_v9pro_driver_test.exe
	./build/mchose_v9pro_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/mchose_driver_test.cpp src/drivers/mchose.cpp -o build/mchose_driver_test.exe
	./build/mchose_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/vgn_driver_test.cpp src/drivers/vgn.cpp -o build/vgn_driver_test.exe
	./build/vgn_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/beiying_driver_test.cpp src/drivers/eweadn.cpp -o build/beiying_driver_test.exe
	./build/beiying_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/vaxee_driver_test.cpp src/drivers/vaxee.cpp -o build/vaxee_driver_test.exe
	./build/vaxee_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/finalmouse_driver_test.cpp src/drivers/finalmouse.cpp -o build/finalmouse_driver_test.exe
	./build/finalmouse_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/keychron_driver_test.cpp src/drivers/keychron.cpp -o build/keychron_driver_test.exe
	./build/keychron_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_haste_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_haste_driver_test.exe
	./build/hyperx_haste_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_dart_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_dart_driver_test.exe
	./build/hyperx_dart_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/jbl_quantum810_driver_test.cpp src/drivers/jbl.cpp -o build/jbl_quantum810_driver_test.exe
	./build/jbl_quantum810_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_cloud_flight_s_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_cloud_flight_s_driver_test.exe
	./build/hyperx_cloud_flight_s_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/aula_sc580_test.cpp src/drivers/aula.cpp -o build/aula_sc580_test.exe
	./build/aula_sc580_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_stinger_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_stinger_driver_test.exe
	./build/hyperx_stinger_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/galaxy_buds_protocol_test.cpp -o build/galaxy_buds_protocol_test.exe
	./build/galaxy_buds_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/sony_headphones_protocol_test.cpp -o build/sony_headphones_protocol_test.exe
	./build/sony_headphones_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/soundcore_protocol_test.cpp -o build/soundcore_protocol_test.exe
	./build/soundcore_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/logitech_g435_0adf_test.cpp -o build/logitech_g435_0adf_test.exe
	./build/logitech_g435_0adf_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Isrc tests/xenta_protocol_test.cpp src/drivers/xenta_protocol.cpp -o build/xenta_protocol_test.exe
	./build/xenta_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/incott_protocol_test.cpp -o build/incott_protocol_test.exe
	./build/incott_protocol_test.exe
	$(CXX) $(CXXFLAGS) -Isrc tests/incott_driver_test.cpp src/drivers/misc_mice.cpp src/drivers/xenta_protocol.cpp -o build/incott_driver_test.exe
	./build/incott_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/labels_test.cpp src/labels.cpp -o build/labels_test.exe
	./build/labels_test.exe
	$(CXX) $(CXXFLAGS) tests/brands_test.cpp src/brands.cpp -o build/brands_test.exe
	./build/brands_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/xbox_bluetooth_identity_test.cpp -o build/xbox_bluetooth_identity_test.exe
	./build/xbox_bluetooth_identity_test.exe
	$(CXX) $(CXXFLAGS) tests/jabra_driver_test.cpp src/drivers/jabra.cpp -o build/jabra_driver_test.exe
	./build/jabra_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/pulsar_driver_test.cpp src/drivers/pulsar.cpp -o build/pulsar_driver_test.exe
	./build/pulsar_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/gamesir_ble_protocol_test.cpp -o build/gamesir_ble_protocol_test.exe
	./build/gamesir_ble_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/fastpair_stream_test.cpp -o build/fastpair_stream_test.exe
	./build/fastpair_stream_test.exe

test-update-core:
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall tests/update_core_test.cpp src/update_core.cpp -o build/update_core_test.exe
	./build/update_core_test.exe

test-update-signature: build/version.h
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall tests/update_signature_test.cpp src/update_signature.cpp -o build/update_signature_test.exe -lbcrypt
	./build/update_signature_test.exe

test-update-win: build/version.h build/app_res.o
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall tests/update_win_test.cpp src/update_core.cpp src/update_signature.cpp src/i18n.cpp build/app_res.o \
	    -o build/update_win_test.exe -lwinhttp -lbcrypt -lcomctl32 -lversion -lole32
	./build/update_win_test.exe

FORCE:

.PHONY: clean test test-update-core test-update-win test-update-signature FORCE
