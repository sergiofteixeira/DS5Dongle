# Local build helper mirroring .github/workflows/build.yml

PICO_SDK_REF ?= 2.2.0
TINYUSB_REF  ?= 0.20.0

# Default to a repo-local SDK checkout (same as CI).
PICO_SDK_PATH ?= $(CURDIR)/pico-sdk

CMAKE ?= cmake

.PHONY: help submodules sdk tinyusb \
        configure-standard build-standard standard \
        configure-debug build-debug debug \
        configure-dse build-dse dse \
        all artifacts clean

help:
	@printf "%s\n" \
	  "Targets:" \
	  "  submodules   - init/update git submodules" \
	  "  sdk          - clone Pico SDK into ./pico-sdk" \
	  "  tinyusb      - checkout TinyUSB tag in the Pico SDK" \
	  "  standard     - build standard UF2 (artifacts/ds5-bridge.uf2)" \
	  "  debug        - build debug UF2 (artifacts/ds5-bridge-debug.uf2)" \
	  "  dse          - build DSE UF2 (artifacts/ds5-bridge-dse.uf2)" \
	  "  all          - build standard + debug" \
	  "  clean        - remove build/ and artifacts/" \
	  "" \
	  "Variables (override like: make standard PICO_SDK_PATH=...):" \
	  "  PICO_SDK_PATH=$(PICO_SDK_PATH)" \
	  "  PICO_SDK_REF=$(PICO_SDK_REF)" \
	  "  TINYUSB_REF=$(TINYUSB_REF)"

submodules:
	git submodule update --init --recursive

sdk:
	@[ -d "$(PICO_SDK_PATH)/.git" ] || git clone --depth 1 --branch "$(PICO_SDK_REF)" https://github.com/raspberrypi/pico-sdk.git "$(PICO_SDK_PATH)"
	git -C "$(PICO_SDK_PATH)" submodule update --init --recursive

tinyusb: sdk
	@if ! git -C "$(PICO_SDK_PATH)/lib/tinyusb" rev-parse -q --verify "refs/tags/$(TINYUSB_REF)" >/dev/null; then \
	  git -C "$(PICO_SDK_PATH)/lib/tinyusb" fetch --depth 1 origin "refs/tags/$(TINYUSB_REF):refs/tags/$(TINYUSB_REF)"; \
	fi
	git -C "$(PICO_SDK_PATH)/lib/tinyusb" checkout --detach "$(TINYUSB_REF)"

configure-standard: submodules tinyusb
	$(CMAKE) -S . -B build/standard -G Ninja \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DPICO_SDK_PATH="$(PICO_SDK_PATH)"

build-standard:
	$(CMAKE) --build build/standard --target ds5-bridge

standard: configure-standard build-standard artifacts
	cp build/standard/ds5-bridge.uf2 artifacts/ds5-bridge.uf2

configure-debug: submodules tinyusb
	$(CMAKE) -S . -B build/debug -G Ninja \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
	  -DENABLE_SERIAL=ON \
	  -DENABLE_VERBOSE=ON

build-debug:
	$(CMAKE) --build build/debug --target ds5-bridge

debug: configure-debug build-debug artifacts
	cp build/debug/ds5-bridge.uf2 artifacts/ds5-bridge-debug.uf2

configure-dse: submodules tinyusb
	$(CMAKE) -S . -B build/dse -G Ninja \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DPICO_SDK_PATH="$(PICO_SDK_PATH)" \
	  -DENABLE_DSE=ON

build-dse:
	$(CMAKE) --build build/dse --target ds5-bridge

dse: configure-dse build-dse artifacts
	cp build/dse/ds5-bridge.uf2 artifacts/ds5-bridge-dse.uf2

all: standard debug

artifacts:
	mkdir -p artifacts

clean:
	rm -rf build artifacts
