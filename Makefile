BUILD_DIR ?= builddir
VERSION ?= 0.4.4

# cijoe configuration selecting the guest IOMMU mode:
#   configs/ubuntu-2604-iommu_disabled.toml  IOMMU disabled, uio_pci_generic
#   configs/ubuntu-2604-iommu_enabled.toml   IOMMU enabled, vfio-pci + iommufd
CIJOE_CONFIG ?= configs/ubuntu-2604-iommu_disabled.toml

.PHONY: all config build verify verify-iommu-disabled verify-iommu-enabled provision test guest install uninstall clean docs

all: clean config build install verify

bump:
ifndef NEW_VERSION
	$(error Usage: make bump NEW_VERSION=x.y.z)
endif
	@echo "Bumping version to $(NEW_VERSION)"
	@sed -i "s/^\(VERSION ?= \).*/\1$(NEW_VERSION)/" Makefile
	@sed -i "s/^\([[:space:]]*version: '\)[^']*'/\1$(NEW_VERSION)'/" meson.build
	@python3 -c "import sys, re, pathlib; [p.write_text(re.sub(r'(@version)\s+.*', rf'\1 {sys.argv[1]}', p.read_text()), encoding='utf-8') for p in pathlib.Path('.').rglob('*.h') if '@version' in p.read_text()]" $(NEW_VERSION)
	@echo "Done"

config:
	meson setup $(BUILD_DIR)

config-debug:
	meson setup $(BUILD_DIR) --buildtype=debug

build:
	meson compile -C $(BUILD_DIR)

install:
	meson install -C $(BUILD_DIR)

gen-artifacts:
	meson setup $(BUILD_DIR) || true
	meson dist -C $(BUILD_DIR) --no-tests --formats gztar --allow-dirty
	mkdir -p /tmp/artifacts
	cp $(BUILD_DIR)/meson-dist/upcie-$(VERSION).tar.gz /tmp/artifacts/upcie-src.tar.gz
	ls -l /tmp/artifacts

guest:
	@cd cijoe && cijoe \
		tasks/guest_setup.yaml \
		--config $(CIJOE_CONFIG) \
		--config configs/system_imaging.toml \
		-l \
		-m

provision: gen-artifacts
	@cd cijoe && cijoe \
		tasks/provision.yaml \
		--config $(CIJOE_CONFIG) \
		--config configs/system_imaging.toml \
		-l \
		-m

test:
	@cd cijoe && cijoe \
		tasks/test.yaml \
		--config $(CIJOE_CONFIG) \
		--config configs/system_imaging.toml \
		-l \
		-m

verify:
	$(MAKE) verify-iommu-disabled
	$(MAKE) verify-iommu-enabled

verify-iommu-disabled:
	$(MAKE) provision test CIJOE_CONFIG=configs/ubuntu-2604-iommu_disabled.toml

verify-iommu-enabled:
	$(MAKE) provision test CIJOE_CONFIG=configs/ubuntu-2604-iommu_enabled.toml

uninstall:
	cd $(BUILD_DIR) && meson --internal uninstall

# Build the documentation. Installs the doc tooling (pipx) on first use, then
# builds with warnings-as-errors so missing pages or broken references fail.
docs:
	@command -v upcie-docs-build-html >/dev/null 2>&1 || { \
		echo "uPCIe docs: installing the doc tooling ..."; \
		$(MAKE) -C docs deps; }
	$(MAKE) -C docs html


clean:
	rm -rf $(BUILD_DIR)
