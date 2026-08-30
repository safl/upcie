# Probing tools: small standalone binaries that report what a runtime or the
# kernel actually does, as opposed to tests, which assert what it should do.
# They are built only where their dependency is present.

incdir = include_directories('../include', '.')

executable(
  'upcie_probe_vfio_delegate',
  files('upcie_probe_vfio_delegate.c'),
  include_directories: incdir,
  install: true,
)

executable(
  'upcie_probe_vfio_cdev',
  files('upcie_probe_vfio_cdev.c'),
  include_directories: incdir,
  install: true,
)

executable(
  'upcie_probe_vfio_share',
  files('upcie_probe_vfio_share.c'),
  include_directories: incdir,
  install: true,
)

cuda_dep = dependency(
  'cuda', modules: ['cuda'],
  required: false,
)

if cuda_dep.found()
  executable(
    'upcie_probe_dmabuf_cuda',
    files('upcie_probe_dmabuf_cuda.c'),
    include_directories: incdir,
    dependencies: [cuda_dep],
    install: true,
  )
  executable(
    'upcie_probe_vfio_bar_import_cuda',
    files('upcie_probe_vfio_bar_import_cuda.c'),
    include_directories: incdir,
    dependencies: [cuda_dep],
    install: true,
  )

  executable(
    'upcie_probe_vram_ioas_cuda',
    files('upcie_probe_vram_ioas_cuda.c'),
    include_directories: incdir,
    dependencies: [cuda_dep],
    install: true,
  )

  nvcc = find_program('nvcc')

  probe_vfio_share_gpu_kernel_obj = custom_target(
    'probe_vfio_share_gpu_kernel_obj',
    input: files('probe_vfio_share_gpu_kernel.cu'),
    output: 'probe_vfio_share_gpu_kernel.o',
    command: [
      nvcc, '-c', '-arch=' + get_option('cuda-arch'),
      '-I', meson.project_source_root() / 'include',
      '-include', meson.project_build_root() / 'upcie_config.h',
      '@INPUT@', '-o', '@OUTPUT@',
    ],
  )

  executable(
    'upcie_probe_vfio_share_gpu_cuda',
    [files('upcie_probe_vfio_share_gpu_cuda.c'), probe_vfio_share_gpu_kernel_obj],
    include_directories: incdir,
    dependencies: [cuda_dep],
    # nvcc emits a C++ static-init guard in the kernel object (CUDA 13), so
    # link libstdc++ for __cxa_guard_acquire/release in this C target.
    link_args: ['-lstdc++'],
    install: true,
  )
endif

cc = meson.get_compiler('c')
hip_lib = cc.find_library('amdhip64', required: false)
hip_hdr = cc.has_header('hip/hip_runtime_api.h', args: '-D__HIP_PLATFORM_AMD__')

if hip_lib.found() and hip_hdr
  executable(
    'upcie_probe_dmabuf_hip',
    files('upcie_probe_dmabuf_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
  executable(
    'upcie_probe_vfio_bar_import_hip',
    files('upcie_probe_vfio_bar_import_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
  executable(
    'upcie_probe_vram_ioas_hip',
    files('upcie_probe_vram_ioas_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
  executable(
    'upcie_probe_vfio_share_gpu_hip',
    files('upcie_probe_vfio_share_gpu_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
endif
