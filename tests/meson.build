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

# Tests depending on linking with CUDA and compiled CUDA kernels
cuda_kernel_test_sources = files(
  'test_cuda_nvme_readwrite.c',
)

cuda_dep = dependency(
  'cuda', modules: ['cuda', 'cudart'],
  required: false,
)

if cuda_dep.found()
  nvcc = find_program('nvcc')

  foreach src : cutest_sources
    bin_name = fs.stem(src)
    exe = executable(
      bin_name,
      src,
      include_directories: incdir,
      dependencies: [cuda_dep],
      install: true,
    )
  endforeach

  nvme_cuda_kernels_obj = custom_target(
    'nvme_cuda_kernels_obj',
    input: files('nvme_cuda_kernels.cu'),
    depend_files: files('../include/upcie/nvme/nvme_qpair_cuda.h',
                        '../include/upcie/nvme/nvme_command.h'),
    output: 'nvme_cuda_kernels.o',
    command: [
      nvcc, '-c', '-arch=sm_75',
      '-I', meson.project_source_root() / 'include',
      '-include', meson.project_build_root() / 'upcie_config.h',
      '@INPUT@', '-o', '@OUTPUT@',
    ],
  )

  foreach src : cuda_kernel_test_sources
    bin_name = fs.stem(src)
    executable(
      bin_name,
      [src, nvme_cuda_kernels_obj],
      include_directories: incdir,
      dependencies: [cuda_dep],
      install: true,
    )
  endforeach
endif
