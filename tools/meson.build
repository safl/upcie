# Probing tools: small standalone binaries that report what a runtime or the
# kernel actually does, as opposed to tests, which assert what it should do.
# They are built only where their dependency is present.

incdir = include_directories('../include', '.')

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
endif
