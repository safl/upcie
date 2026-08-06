experimental_cuda_dep = dependency(
  'cuda', modules: ['cuda', 'cudart'],
  required: false,
)

if experimental_cuda_dep.found()
  executable(
    'test_cudamem_iommu_map_nvme_readwrite',
    'tests/test_cudamem_iommu_map_nvme_readwrite.c',
    include_directories: [
      include_directories('../../include'),
      include_directories('include'),
    ],
    dependencies: [experimental_cuda_dep],
    build_by_default: false,
    install: false,
  )
endif
