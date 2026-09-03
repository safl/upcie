cpu:
	gcc -o dmabuf_import_cpu dmabuf_import_cpu.c

gpu:
	gcc -o dmabuf_import_gpu dmabuf_import_gpu.c -lcuda -I/usr/local/cuda/include

probe-cuda:
	gcc -o dmabuf_import_probe_cuda dmabuf_import_probe_cuda.c \
		-lcuda -I/usr/local/cuda/include

probe-hip:
	gcc -o dmabuf_import_probe_hip dmabuf_import_probe_hip.c \
		-I/opt/rocm/include -L/opt/rocm/lib -lamdhip64

gpu-hip:
	gcc -o dmabuf_import_gpu_hip dmabuf_import_gpu_hip.c -I/opt/rocm/include \
		-L/opt/rocm/lib -lamdhip64

# Target-side validation programs. They compile standalone against the system
# headers, but running them needs the stock /dev/udmabuf (for UDMABUF_CREATE)
# and the loaded module at /dev/dmabuf_import.
tests: bounds placement

bounds:
	gcc -o test/dmabuf_import_bounds test/dmabuf_import_bounds.c

placement:
	gcc -o test/dmabuf_import_placement test/dmabuf_import_placement.c
