# Probing tools: small standalone binaries that report what a runtime or the
# kernel actually does, as opposed to tests, which assert what it should do.
# They are built only where their dependency is present.

incdir = include_directories('../include', '.')

executable(
  'upcie_vfio_delegate_probe',
  files('upcie_vfio_delegate_probe.c'),
  include_directories: incdir,
  install: true,
)

cuda_dep = dependency(
  'cuda', modules: ['cuda'],
  required: false,
)

if cuda_dep.found()
  executable(
    'upcie_dmabuf_probe_cuda',
    files('upcie_dmabuf_probe_cuda.c'),
    include_directories: incdir,
    dependencies: [cuda_dep],
    install: true,
  )
  executable(
    'upcie_vfio_bar_import_probe_cuda',
    files('upcie_vfio_bar_import_probe_cuda.c'),
    include_directories: incdir,
    dependencies: [cuda_dep],
    install: true,
  )

  executable(
    'upcie_vram_ioas_probe_cuda',
    files('upcie_vram_ioas_probe_cuda.c'),
    include_directories: incdir,
    dependencies: [cuda_dep],
    install: true,
  )

  nvcc = find_program('nvcc')

  vfio_share_gpu_kernel_obj = custom_target(
    'vfio_share_gpu_kernel_obj',
    input: files('vfio_share_gpu_kernel.cu'),
    output: 'vfio_share_gpu_kernel.o',
    command: [
      nvcc, '-c', '-arch=' + get_option('cuda-arch'),
      '-I', meson.project_source_root() / 'include',
      '-include', meson.project_build_root() / 'upcie_config.h',
      '@INPUT@', '-o', '@OUTPUT@',
    ],
  )

  executable(
    'upcie_vfio_share_gpu_probe_cuda',
    [files('upcie_vfio_share_gpu_probe_cuda.c'), vfio_share_gpu_kernel_obj],
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
    'upcie_dmabuf_probe_hip',
    files('upcie_dmabuf_probe_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
  executable(
    'upcie_vfio_share_gpu_probe_hip',
    files('upcie_vfio_share_gpu_probe_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
  executable(
    'upcie_vram_ioas_probe_hip',
    files('upcie_vram_ioas_probe_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
  executable(
    'upcie_vfio_bar_import_probe_hip',
    files('upcie_vfio_bar_import_probe_hip.c'),
    include_directories: incdir,
    dependencies: [hip_lib],
    c_args: ['-D__HIP_PLATFORM_AMD__'],
    install: true,
  )
endif
