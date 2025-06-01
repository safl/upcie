BUILD_DIR ?= builddir

.PHONY: all setup build test install uninstall clean

all: clean setup build install test

setup:
	meson setup $(BUILD_DIR)

build:
	meson compile -C $(BUILD_DIR)

install:
	meson install -C $(BUILD_DIR)

test:
	@cd cijoe && cijoe \
		--config configs/cijoe-config.toml \
		--workflow workflows/guest_test.yaml \
		-l \
		-m

uninstall:
	cd $(BUILD_DIR) && meson --internal uninstall

clean:
	rm -rf $(BUILD_DIR)
