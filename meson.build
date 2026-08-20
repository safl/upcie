project('upcie', 'c',
  version: '0.7.0',
  license: 'BSD-3-Clause',
  meson_version: '>=0.59',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=release',
  ],
)

fs = import('fs')

conf_data = configuration_data()
if get_option('debug-logging').enabled()
  conf_data.set('UPCIE_DEBUG_ENABLED', true)
elif get_option('debug-logging').disabled()
  conf_data.set('UPCIE_DEBUG_ENABLED', false)
else
  conf_data.set('UPCIE_DEBUG_ENABLED', get_option('buildtype') == 'debug')
endif

conf = configure_file(
  configuration : conf_data,
  output : 'upcie_config.h'
)
add_project_arguments(['-include', 'upcie_config.h'], language: 'c')

install_headers(
  [
    'include/upcie/barriers.h',
    'include/upcie/bitfield.h',
    'include/upcie/debug.h',
    'include/upcie/cudamem_config.h',
    'include/upcie/cudamem_dma.h',
    'include/upcie/cudamem_heap.h',
    'include/upcie/cudamem_mapping.h',
    'include/upcie/dmabuf.h',
    'include/upcie/dmamem.h',
    'include/upcie/dmamem_cuda.h',
    'include/upcie/dmamem_dmabuf.h',
    'include/upcie/dmamem_heap.h',
    'include/upcie/dmamem_hip.h',
    'include/upcie/dmamem_hostmem.h',
    'include/upcie/dmamem_memfd.h',
    'include/upcie/dmamem_registry.h',
    'include/upcie/hostmem_config.h',
    'include/upcie/hostmem_dma.h',
    'include/upcie/hostmem.h',
    'include/upcie/hostmem_heap.h',
    'include/upcie/hostmem_hugepage.h',
    'include/upcie/iommufd.h',
    'include/upcie/vfio_cdev.h',
    'include/upcie/mmio.h',
    'include/upcie/hipmem_config.h',
    'include/upcie/hipmem_dma.h',
    'include/upcie/hipmem_heap.h',
    'include/upcie/hipmem_mapping.h',
    'include/upcie/pci.h',
    'include/upcie/upcie.h',
    'include/upcie/upcie_cuda.h',
    'include/upcie/upcie_hip.h',
    'include/upcie/vfioctl.h',
  ],
  subdir: 'upcie'
)

# Separate call: install_headers() flattens its file list into one subdir, so
# headers that must land in upcie/experimental/ cannot ride along with the list
# above. upcie.h includes these, so leaving them out breaks every installed
# consumer.
install_headers(
  [
    'include/upcie/experimental/dmabuf_import.h',
    'include/upcie/experimental/iommu_map_pa.h',
  ],
  subdir: 'upcie/experimental'
)

# Same reason as above: install_headers() flattens, so the nvme/ headers need
# their own call to keep the subdirectory that upcie.h includes them by.
install_headers(
  [
    'include/upcie/nvme/nvme_command.h',
    'include/upcie/nvme/nvme_controller.h',
    'include/upcie/nvme/nvme_controller_dmamem_type1.h',
    'include/upcie/nvme/nvme_controller_dmamem_uio.h',
    'include/upcie/nvme/nvme_controller_dmamem_vfio.h',
    'include/upcie/nvme/nvme_controller_vfio.h',
    'include/upcie/nvme/nvme_controller_vfio_pci.h',
    'include/upcie/nvme/nvme_controller_cuda.h',
    'include/upcie/nvme/nvme_mmio.h',
    'include/upcie/nvme/nvme_qid.h',
    'include/upcie/nvme/nvme_qpair.h',
    'include/upcie/nvme/nvme_qpair_cuda.h',
    'include/upcie/nvme/nvme_request.h',
  ],
  subdir: 'upcie/nvme'
)

# Optional dependency for other Meson projects
upcie_dep = declare_dependency(
  include_directories: include_directories('include')
)

# Add the test programs
subdir('example')
subdir('tests')
subdir('experimental')
subdir('tools')
