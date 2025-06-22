BUILD_DIR ?= builddir

.PHONY: all setup build test install uninstall clean docs

all: clean setup build install test

setup:
	meson setup $(BUILD_DIR)

build:
	meson compile -C $(BUILD_DIR)

install:
	meson install -C $(BUILD_DIR)

guest:
	@cd cijoe && cijoe \
		--config configs/cijoe-config.toml \
		--workflow workflows/guest_setup.yaml \
		-l \
		-m

test:
	@cd cijoe && cijoe \
		--config configs/cijoe-config.toml \
		--workflow workflows/guest_test.yaml \
		-l \
		-m

uninstall:
	cd $(BUILD_DIR) && meson --internal uninstall

docs:
	doxygen docs/Doxyfile
	

clean:
	rm -rf $(BUILD_DIR)
