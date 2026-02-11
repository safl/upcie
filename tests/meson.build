test_sources = files(
  'test_vfioctl.c',
  'test_hostmem_shared.c',
  'test_hostmem_heap.c',
  'test_hostmem_dma.c',
  'test_pci_bars.c',
  'test_pci_scan.c',
  'test_hostmem_dmabuf.c',
  'test_hostmem_nvme_readwrite.c',
)

incdir = include_directories('../include')

foreach src : test_sources
  bin_name = fs.stem(src)
  exe = executable(
    bin_name,
    src,
    include_directories: incdir,
    install: true,
  )
endforeach

# Tests depending on linking with CUDA
cutest_sources = files(
  'test_cudamem_heap.c',
  'test_cudamem_dmabuf.c',
  'test_cudamem_nvme_readwrite.c',
)

cuda_dep = dependency(
  'cuda',
  required: false,
)

if cuda_dep.found()
  foreach src : cutest_sources
    bin_name = fs.stem(src)
    exe = executable(
      bin_name,
      src,
      include_directories: incdir,
      dependencies: [cuda_dep],
      link_args: ['-lcuda'],
      install: true,
    )
  endforeach
endif
