// RUN: mlir-rocm-runner %s \
// RUN:   --shared-libs=%rocm_wrapper_library_dir/librocm-runtime-wrappers%shlibext \
// RUN:   --shared-libs=%linalg_test_lib_dir/libmlir_runner_utils%shlibext \
// RUN:   --entry-point-result=void \
// RUN: | FileCheck %s

// CHECK: Hello from 0
// CHECK: Hello from 1
module attributes {gpu.container_module} {
    gpu.module @kernels {
        gpu.func @hello() kernel {
            %0 = "gpu.thread_id"() {dimension="x"} : () -> (index)
            gpu.printf {format = "Hello from %d\n"} %0 : index
            gpu.return
        }
    }

    func @main() {
        %c2 = arith.constant 2 : index
        %c1 = arith.constant 1 : index
        gpu.launch_func @kernels::@hello
            blocks in (%c1, %c1, %c1)
            threads in (%c2, %c1, %c1)        return
    }
}
