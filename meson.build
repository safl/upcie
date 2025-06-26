project('upcie', 'c',
  version: '0.1.0',
  license: 'BSD-3-Clause'
)

fs = import('fs')

install_headers(
  [
    'include/upcie/bitfield.h',
    'include/upcie/hostmem_dma.h',
    'include/upcie/hostmem.h',
    'include/upcie/hostmem_heap.h',
    'include/upcie/hostmem_hugepage.h',
    'include/upcie/mmio.h',
    'include/upcie/nvme/nvme_command.h',
    'include/upcie/nvme/nvme_controller.h',
    'include/upcie/nvme/nvme_mmio.h',
    'include/upcie/nvme/nvme_qid.h',
    'include/upcie/nvme/nvme_qpair.h',
    'include/upcie/nvme/nvme_request.h',
    'include/upcie/pci.h',
    'include/upcie/upcie.h',
    'include/upcie/vfioctl.h',
  ],
  subdir: 'upcie'
)

# Optional dependency for other Meson projects
upcie_dep = declare_dependency(
  include_directories: include_directories('include')
)

# Add the test programs
subdir('scripts')
subdir('example')
subdir('tests')
