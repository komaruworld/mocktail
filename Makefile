.DEFAULT_GOAL := all
.PHONY: all build release debug test apk install register-url-handler release-runtime portable portable-test standalone appimage flatpak run-flatpak run run-smoke run-unlimited run-game run-input run-resize run-network auto-run run-gles run-angle update-roblox update-auto update-auto-launch payload-status payload-rollback support-bundle clean submodules help

BUILD_DIR  := build
BINARY     := $(BUILD_DIR)/mocktail
JOBS       := $(shell nproc)
BUILD_TYPE ?= Release
LIBC ?= auto
MODE ?= standalone
CMAKE_TOOLCHAIN_FILE ?=
CMAKE_SYSROOT ?=
RELEASE_BUILD_DIR ?=
PORTABLE_MODE ?= standalone
PORTABLE_CANONICAL_MODE := $(if $(filter dynamic minimal,$(PORTABLE_MODE)),thin,$(if $(filter full static,$(PORTABLE_MODE)),standalone,$(PORTABLE_MODE)))
PORTABLE_DIR ?= dist/mocktail-linux-x86_64-glibc-$(PORTABLE_CANONICAL_MODE)
APPIMAGE ?= dist/Mocktail-x86_64.AppImage
APPIMAGE_FORMAT ?= classic
ANYLINUX_PACKAGER ?= quick-sharun
ANYLINUX_APPIMAGETOOL ?= appimagetool
PREFIX ?= /usr
FLATPAK_BUILD_DIR ?= build-flatpak
FLATPAK_MANIFEST ?= packaging/flatpak/space.bigrat.mocktail.json
FLATPAK_JOBS ?= 4

define build_native_runtime
	@git submodule update --init --recursive
	@cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(1)" \
		-DBUILD_TESTING="$(2)" \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		$(if $(strip $(CMAKE_TOOLCHAIN_FILE)),-DCMAKE_TOOLCHAIN_FILE="$(CMAKE_TOOLCHAIN_FILE)") \
		$(if $(strip $(CMAKE_SYSROOT)),-DCMAKE_SYSROOT="$(CMAKE_SYSROOT)")
	@cmake --build "$(BUILD_DIR)" -j"$(JOBS)"
endef

all: build ## Dynamic Release build

build: ## Build the dynamic Release runtime in build/
	$(call build_native_runtime,$(BUILD_TYPE),OFF)

release: ## Build AppImage (LIBC=auto|glibc|musl MODE=standalone|thin)
	@MOCKTAIL_BUILD_JOBS="$(JOBS)" \
		MOCKTAIL_BUILD_TYPE="$(BUILD_TYPE)" \
		MOCKTAIL_RELEASE_BUILD_DIR="$(RELEASE_BUILD_DIR)" \
		CMAKE_TOOLCHAIN_FILE="$(CMAKE_TOOLCHAIN_FILE)" \
		CMAKE_SYSROOT="$(CMAKE_SYSROOT)" \
		./scripts/build_release.sh --libc "$(LIBC)" --mode "$(MODE)"

debug: ## Debug build
	$(call build_native_runtime,Debug,OFF)

install: ## Install an already-built dynamic runtime (use: sudo make install)
	@cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)"

register-url-handler: build ## Select this build for Roblox website links
	@./scripts/register_url_handler.sh --set-default \
		--executable "$(abspath $(BINARY))" \
		--desktop-file "$(abspath packaging/space.bigrat.mocktail.desktop)"

test: ## Build and run all unit tests
	$(call build_native_runtime,Debug,ON)
	@ctest --test-dir "$(BUILD_DIR)" --output-on-failure -j"$(JOBS)"

apk: ## Extract libroblox.so and build (usage: make apk APK=/path/to/roblox.apk)
ifndef APK
	$(error APK variable not set. Usage: make apk APK=/path/to/roblox-x86_64.apk)
endif
	@./scripts/build.sh --apk "$(APK)" --build-type $(BUILD_TYPE) --jobs $(JOBS)

release-runtime: build

portable: appimage ## Build the portable AppImage

portable-test: release-runtime ## Verify portable packaging and relocation
	@./tests/portable_packaging_test.sh

standalone: appimage ## Build the standalone x86-64 AppImage

appimage: release-runtime ## Build AppImage (APPIMAGE_FORMAT=classic|anylinux)
	@MOCKTAIL_APPIMAGE_FORMAT="$(APPIMAGE_FORMAT)" \
		MOCKTAIL_ANYLINUX_PACKAGER="$(ANYLINUX_PACKAGER)" \
		MOCKTAIL_ANYLINUX_APPIMAGETOOL="$(ANYLINUX_APPIMAGETOOL)" \
		./scripts/package_portable.sh --build-dir "$(BUILD_DIR)" \
		--libc glibc --output "$(PORTABLE_DIR)" --mode "$(PORTABLE_MODE)" \
		--appimage "$(APPIMAGE)"

flatpak: ## Build and install the local x86-64 Flatpak
	@./scripts/build_flatpak.sh \
		--build-dir "$(FLATPAK_BUILD_DIR)" \
		--manifest "$(FLATPAK_MANIFEST)" \
		--jobs "$(FLATPAK_JOBS)"

run-flatpak: ## Run the installed Flatpak
	@flatpak run space.bigrat.mocktail

run: export MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS = 0
run: build ## Run the native binary interactively until the window is closed
	@$(BINARY)

run-smoke: export MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS = 3000
run-smoke: release-runtime ## Run the short Vulkan lifecycle readiness gate
	@./scripts/real_bringup_smoke.sh C

run-unlimited: export MOCKTAIL_FRAME_RATE_LIMIT = unlimited
run-unlimited: export MOCKTAIL_VSYNC = off
run-unlimited: export MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS = 5000
run-unlimited: release-runtime ## Run 240-FPS/unthrottled Vulkan diagnostic mode
	@./scripts/real_bringup_smoke.sh C

run-game: release-runtime ## Run local UGCGame/Vulkan gate (not an authenticated join)
	@./scripts/real_bringup_smoke.sh GAME

run-input: release-runtime ## Run local GAME gate and route SDL mouse/touch through native input
	@./scripts/real_bringup_smoke.sh INPUT

run-resize: release-runtime ## Run real SDL compositor resize and typed JNI surface-rebind gate
	@./scripts/real_bringup_smoke.sh RESIZE

run-network: export MOCKTAIL_PLACE_ID = $(PLACE_ID)
run-network: release-runtime ## Run authenticated public-place gate (usage: make run-network PLACE_ID=...)
ifndef PLACE_ID
	$(error PLACE_ID variable not set. Usage: make run-network PLACE_ID=positive-place-id)
endif
	@./scripts/real_bringup_smoke.sh NETWORK

auto-run: ## Build/run loop with crash summaries
	@./scripts/auto_runtime_loop.sh

run-gles: build ## Run strict system EGL/OpenGL ES 3 without Vulkan fallback
	@$(BINARY) --graphics opengl

run-angle: build ## Run with ANGLE/Vulkan compatibility backend
	@$(BINARY) --graphics angle-vulkan

update-roblox: ## Validate and import the current x86_64 Roblox bundle cached by Sober
	@./scripts/update_roblox_payload.sh

update-auto: build ## Download latest Roblox, derive its ABI, run two canaries, then activate
	@./$(BUILD_DIR)/mocktail_updater update

update-auto-launch: build ## Update Roblox and launch the verified current payload
	@./$(BUILD_DIR)/mocktail_updater update
	@./$(BINARY)

payload-status: build ## Show current and previous-good immutable payloads
	@./$(BUILD_DIR)/mocktail_updater status

payload-rollback: build ## Atomically restore the previous-good payload
	@./$(BUILD_DIR)/mocktail_updater rollback

support-bundle: ## Create a private sanitized diagnostics archive
	@./scripts/collect_support_bundle.sh --context manual --reason manual-request

submodules: ## Initialise git submodules (libjnivm, mcpelauncher-linker)
	@./scripts/add_submodules.sh

clean: ## Remove the build directory
	rm -rf $(BUILD_DIR)
	@echo "Build directory removed."

help: ## Show available targets
	@grep -E '^[a-zA-Z_-]+:.*##' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*## "}; {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'
