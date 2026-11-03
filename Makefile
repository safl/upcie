BUILD_DIR ?= builddir
VERSION ?= 0.10.4

# cijoe configuration selecting the guest IOMMU mode:
#   configs/iommu_disabled.toml  IOMMU disabled, uio_pci_generic
#   configs/iommu_enabled.toml   IOMMU enabled, vfio-pci + iommufd
CIJOE_CONFIG ?= configs/iommu_disabled.toml
# The Ubuntu release the QEMU guest runs: ubuntu-2604 or ubuntu-2610.
# Passed first, so a CIJOE_CONFIG for a real machine overrides it.
RELEASE ?= ubuntu-2604
CIJOE_CONFIGS = --config configs/$(RELEASE).toml --config $(CIJOE_CONFIG)
# What deploy makes first: the source archive it copies to the target. Emptied
# where the archive is made elsewhere, as devenv-verify does
DEPLOY_ARTIFACTS ?= gen-artifacts
# The rig the QEMU guest boots with, from cijoe/rigs/, its PCIe topology and devices
RIG ?= upcie-nvme

# The guest is booted by xnvme-devenv's rig scripts, which its host container
# carries along with QEMU and cijoe; the devenv-* targets run there
DEVENV_IMAGE ?= ghcr.io/xnvme/xnvme-devenv/host:v0.15.0
DEVENV_ENGINE ?= docker
DEVENV_NAME ?= upcie-devenv-$(notdir $(CURDIR))
DEVENV_HOME ?= $(CURDIR)/.devenv
DEVENV_RUN = $(DEVENV_ENGINE) exec $(DEVENV_NAME) bash -euo pipefail -c '$(1)'

.PHONY: all config build verify verify-iommu-disabled verify-iommu-enabled deploy test guest devenv-host devenv-run devenv-verify devenv-down install uninstall clean docs dkms-deb dkms-version-check

all: clean config build install verify

bump:
ifndef NEW_VERSION
	$(error Usage: make bump NEW_VERSION=x.y.z)
endif
	@echo "Bumping version to $(NEW_VERSION)"
	@sed -i "s/^\(VERSION ?= \).*/\1$(NEW_VERSION)/" Makefile
	@sed -i "s/^\([[:space:]]*version: '\)[^']*'/\1$(NEW_VERSION)'/" meson.build
	@python3 -c "import sys, re, pathlib; [p.write_text(re.sub(r'(@version)\s+.*', rf'\1 {sys.argv[1]}', p.read_text()), encoding='utf-8') for p in pathlib.Path('.').rglob('*.h') if '@version' in p.read_text()]" $(NEW_VERSION)
	@sed -i "s/MODULE_VERSION(\"[^\"]*\")/MODULE_VERSION(\"$(NEW_VERSION)\")/" experimental/*/module/*.c
	@if ! head -1 experimental/debian/changelog | grep -q "^upcie-experimental ($(NEW_VERSION))"; then \
		{ \
			echo "upcie-experimental ($(NEW_VERSION)) unstable; urgency=medium"; \
			echo ""; \
			echo "  * Track upcie $(NEW_VERSION)."; \
			echo ""; \
			echo " -- Simon A. F. Lund <os@safl.dk>  $$(date -R)"; \
			echo ""; \
			cat experimental/debian/changelog; \
		} > experimental/debian/changelog.bump && \
		mv experimental/debian/changelog.bump experimental/debian/changelog; \
	fi
	@echo "Done"

# The DKMS package is versioned as upcie, so the changelog it takes its version
# from must not drift from VERSION above. CI runs this.
dkms-version-check:
	@ver=$$(sed -n '1s/^upcie-experimental (\(.*\)).*/\1/p' experimental/debian/changelog); \
	if [ "$$ver" != "$(VERSION)" ]; then \
		echo "experimental/debian/changelog is at $$ver, VERSION is $(VERSION); run 'make bump NEW_VERSION=$(VERSION)'"; \
		exit 1; \
	fi; \
	echo "DKMS package version $$ver matches upcie $(VERSION)"

dkms-deb: dkms-version-check
	cd experimental && dpkg-buildpackage -us -uc -b

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

# Bring up the QEMU guest with its rig. QEMU-specific, so it takes the
# system_imaging config and the rig; the retargetable deploy/test below do not.
# Runs where xnvme-devenv's rig scripts are: in its host container, see devenv-*.
guest:
	@cd cijoe && cijoe \
		tasks/guest_setup.yaml \
		$(CIJOE_CONFIGS) \
		--config configs/system_imaging.toml \
		--config rigs/$(RIG).toml \
		-l \
		-m

# Deploy uPCIe to the transport target: build, install, drivers, hugepages. The
# same task retargets to a real machine by selecting a CIJOE_CONFIG whose first
# transport points at it (see configs/hw-example.toml).
deploy: $(DEPLOY_ARTIFACTS)
	@cd cijoe && cijoe \
		tasks/deploy.yaml \
		$(CIJOE_CONFIGS) \
		-l \
		-m

test:
	@cd cijoe && cijoe \
		tasks/test.yaml \
		$(CIJOE_CONFIGS) \
		-l \
		-m

verify:
	$(MAKE) verify-iommu-disabled
	$(MAKE) verify-iommu-enabled

verify-iommu-disabled:
	$(MAKE) guest deploy test CIJOE_CONFIG=configs/iommu_disabled.toml

verify-iommu-enabled:
	$(MAKE) guest deploy test CIJOE_CONFIG=configs/iommu_enabled.toml

# Start the xnvme-devenv host container, unless running, with this tree and
# /tmp/artifacts in it; DEVENV_HOME keeps its guests and images between runs
devenv-host:
	mkdir -p $(DEVENV_HOME) /tmp/artifacts
	$(DEVENV_ENGINE) inspect $(DEVENV_NAME) >/dev/null 2>&1 || \
		$(DEVENV_ENGINE) run -d --name $(DEVENV_NAME) --privileged --ulimit memlock=-1 \
			-v $(DEVENV_HOME):/root -v $(CURDIR):/root/git/upcie -v /tmp/artifacts:/tmp/artifacts \
			-w /root/git/upcie $(DEVENV_IMAGE) sleep infinity

# Run CMD in the host container, e.g. make devenv-run CMD="make guest RIG=upcie-nvme"
devenv-run: devenv-host
	$(if $(strip $(CMD)),,$(error no command; give CMD=<command>))
	$(call DEVENV_RUN,$(CMD))

# verify, in the host container. The source archive is made here first, as
# meson dist needs the git repository, which a worktree does not carry inside
devenv-verify: gen-artifacts devenv-host
	$(call DEVENV_RUN,make verify DEPLOY_ARTIFACTS= RELEASE=$(RELEASE) RIG=$(RIG))

# Stop and remove the host container and the guest in it; DEVENV_HOME stays
devenv-down:
	$(DEVENV_ENGINE) rm -f $(DEVENV_NAME)

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
