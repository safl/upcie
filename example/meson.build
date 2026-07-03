example_sources = files(
  'upcie_nvme_driver.c',
  'dmamem_memfd_smoketest.c',
  'dmamem_nvme_identify_smoketest.c',
  'dmamem_vfio_bar_smoketest.c',
  'dmamem_nvme_vram_smoketest.c',
)

incdir = include_directories('../include', '.')

foreach src : example_sources
  bin_name = fs.stem(src)
  exe = executable(
    bin_name,
    src,
    include_directories: incdir,
    install: true,
  )
endforeach

# HIP-linked examples: build only when the amdhip64 library and hip header
# are available. Mirrors the tests/meson.build pattern.
hip_example_sources = files(
  'dmamem_hip_bar_import_smoketest.c',
)

cc = meson.get_compiler('c')
hip_lib = cc.find_library('amdhip64', required: false)
hip_hdr = cc.has_header('hip/hip_runtime_api.h', args: '-D__HIP_PLATFORM_AMD__')

if hip_lib.found() and hip_hdr
  foreach src : hip_example_sources
    bin_name = fs.stem(src)
    executable(
      bin_name,
      src,
      include_directories: incdir,
      dependencies: [hip_lib],
      c_args: ['-D__HIP_PLATFORM_AMD__'],
      install: true,
    )
  endforeach
endif

# libdrm_amdgpu examples: build only when libdrm-dev is available.
drm_amdgpu_example_sources = files(
  'dmamem_libdrm_bar_import_smoketest.c',
)

drm_amdgpu_lib = cc.find_library('drm_amdgpu', required: false)
drm_amdgpu_hdr = cc.has_header('libdrm/amdgpu.h')

if drm_amdgpu_lib.found() and drm_amdgpu_hdr
  foreach src : drm_amdgpu_example_sources
    bin_name = fs.stem(src)
    executable(
      bin_name,
      src,
      include_directories: incdir,
      dependencies: [drm_amdgpu_lib],
      install: true,
    )
  endforeach
endif

# HIP + libdrm_amdgpu combined examples
hip_drm_example_sources = files(
  'dmamem_hip_bar_via_libdrm_smoketest.c',
)

if hip_lib.found() and hip_hdr and drm_amdgpu_lib.found() and drm_amdgpu_hdr
  foreach src : hip_drm_example_sources
    bin_name = fs.stem(src)
    executable(
      bin_name,
      src,
      include_directories: incdir,
      dependencies: [hip_lib, drm_amdgpu_lib],
      c_args: ['-D__HIP_PLATFORM_AMD__'],
      install: true,
    )
  endforeach
endif
