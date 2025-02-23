// RUN: miopen-opt %s | FileCheck %s
// RUN: miopen-opt %s | miopen-opt | FileCheck %s
// Run: miopen-opt -mlir-print-op-generic %s | miopen-opt | FileCheck %s

func @miopen_conv2d(%filter : memref<?x?x?x?x?xf32>, %input : memref<?x?x?x?x?xf32>, %output : memref<?x?x?x?x?xf32>) {
  miopen.conv2d(%filter, %input, %output) {
    filter_layout = ["g", "k", "c", "y", "x"],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    output_layout = ["n", "go", "k", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?x?x?xf32>, memref<?x?x?x?x?xf32>, memref<?x?x?x?x?xf32>
  return
}
// CHECK-LABEL: func @miopen_conv2d
// CHECK-NEXT: miopen.conv2d

func @miopen_conv2d_f16(%filter : memref<?x?x?x?x?xf16>, %input : memref<?x?x?x?x?xf16>, %output : memref<?x?x?x?x?xf16>) {
  miopen.conv2d(%filter, %input, %output) {
    filter_layout = ["g" ,"k", "c", "y", "x"],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    output_layout = ["n", "go", "k", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?x?x?xf16>, memref<?x?x?x?x?xf16>, memref<?x?x?x?x?xf16>
  return
}
// CHECK-LABEL: func @miopen_conv2d_f16
// CHECK-NEXT: miopen.conv2d

func @miopen_conv2d_bwd_data(%filter : memref<?x?x?x?x?xf32>, %input : memref<?x?x?x?x?xf32>, %output : memref<?x?x?x?x?xf32>) {
  miopen.conv2d_bwd_data(%filter, %input, %output) {
    filter_layout = ["g", "k", "c", "y", "x"],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    output_layout = ["n", "go", "k", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?x?x?xf32>, memref<?x?x?x?x?xf32>, memref<?x?x?x?x?xf32>
  return
}
// CHECK-LABEL: func @miopen_conv2d_bwd_data
// CHECK-NEXT: miopen.conv2d_bwd_data

func @miopen_conv2d_bwd_data_f16(%filter : memref<?x?x?x?x?xf16>, %input : memref<?x?x?x?x?xf16>, %output : memref<?x?x?x?x?xf16>) {
  miopen.conv2d_bwd_data(%filter, %input, %output) {
    filter_layout = ["g", "k", "c", "y", "x"],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    output_layout = ["n", "go", "k", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?x?x?xf16>, memref<?x?x?x?x?xf16>, memref<?x?x?x?x?xf16>
  return
}
// CHECK-LABEL: func @miopen_conv2d_bwd_data_f16
// CHECK-NEXT: miopen.conv2d_bwd_data

func @miopen_conv2d_bwd_weight(%filter : memref<?x?x?x?x?xf32>, %input : memref<?x?x?x?x?xf32>, %output : memref<?x?x?x?x?xf32>) {
  miopen.conv2d_bwd_weight(%filter, %input, %output) {
    filter_layout = ["g", "k", "c", "y", "x"],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    output_layout = ["n", "go", "k", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?x?x?xf32>, memref<?x?x?x?x?xf32>, memref<?x?x?x?x?xf32>
  return
}
// CHECK-LABEL: func @miopen_conv2d_bwd_weight
// CHECK-NEXT: miopen.conv2d_bwd_weight

func @miopen_conv2d_bwd_weight_f16(%filter : memref<?x?x?x?x?xf16>, %input : memref<?x?x?x?x?xf16>, %output : memref<?x?x?x?x?xf16>) {
  miopen.conv2d_bwd_weight(%filter, %input, %output) {
    filter_layout = ["g", "k", "c", "y", "x"],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    output_layout = ["n", "go", "k", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?x?x?xf16>, memref<?x?x?x?x?xf16>, memref<?x?x?x?x?xf16>
  return
}

// CHECK-LABEL: func @miopen_conv2d_bwd_weight_f16
// CHECK-NEXT: miopen.conv2d_bwd_weight

// test 1-1 dimension mappings.
func @miopen_transform_1_to_1(%memref: memref<?x?x?x?x?xf32>) {
  %transformed_memref = miopen.transform(%memref) {
    layout = [
      {
        upper_layer_dimensions = [0],
        upper_layer_names = ["g"],
        transformation = "PassThrough",
        lower_layer_dimensions = [1],
        lower_layer_names = ["g"]
      },
      {
        upper_layer_dimensions = [1],
        upper_layer_names = ["n"],
        transformation = "PassThrough",
        lower_layer_dimensions = [0],
        lower_layer_names = ["g"]
      },
      {
        upper_layer_dimensions = [2],
        upper_layer_names = ["c"],
        transformation = "PassThrough",
        lower_layer_dimensions = [2],
        lower_layer_names = ["c"]
      },
      {
        upper_layer_dimensions = [3],
        upper_layer_names = ["hipad"],
        transformation = "Pad",
        parameters = [1, 1],
        lower_layer_dimensions = [3],
        lower_layer_names = ["hi"]
      },
      {
        upper_layer_dimensions = [4],
        upper_layer_names = ["wipad"],
        transformation = "Pad",
        parameters = [2, 2],
        lower_layer_dimensions = [4],
        lower_layer_names = ["wi"]
      }
    ],
    lower_layer_layout = ["gi", "n", "c", "hi", "wi"],
    upper_layer_layout = ["n", "gi", "c", "hipad", "wipad"]
  } : memref<?x?x?x?x?xf32> to memref<?x?x?x?x?xf32>
  return
}
// CHECK-LABEL: func @miopen_transform_1_to_1
//  CHECK-NEXT: miopen.transform

// test multiple source dimensions map to 1 target dimension.
func @miopen_transform_n_to_1(%memref : memref<1x128x64x32x16xf32>) {
  %transformed_memref = miopen.transform(%memref) {
    layout = [
      {
        upper_layer_dimensions = [0],
        upper_layer_names = ["gemmG"],
        transformation = "PassThrough",
        lower_layer_dimensions = [0],
        lower_layer_names = ["g"]
      },
      {
        upper_layer_dimensions = [1],
        upper_layer_names = ["gemmK"],
        transformation = "Merge",
        lower_layer_dimensions = [2, 3, 4],
        lower_layer_names = ["c", "y", "x"]
      },
      {
        upper_layer_dimensions = [2],
        upper_layer_names = ["gemmM"],
        transformation = "PassThrough",
        lower_layer_dimensions = [1],
        lower_layer_names = ["k"]
      }
    ],
    lower_layer_layout = ["g", "k", "c", "y", "x"],
    upper_layer_layout = ["gemmG", "gemmK", "gemmM"],
    gridwise_gemm_argument_pos = 0
  } : memref<1x128x64x32x16xf32> to memref<?x?x?xf32>
  return
}
// CHECK-LABEL: func @miopen_transform_n_to_1
//  CHECK-NEXT: miopen.transform

// test 1 source dimension map to multiple target dimensions.
func @miopen_transform_1_to_n(%memref : memref<?x?x?x?x?xf32>) {
  %transformed_memref = miopen.transform(%memref) {
    layout = [
      {
        upper_layer_dimensions = [0],
        upper_layer_names = ["g"],
        transformation = "PassThrough",
        lower_layer_dimensions = [1],
        lower_layer_names = ["g"]
      },
      {
        upper_layer_dimensions = [1],
        upper_layer_names = ["n"],
        transformation = "PassThrough",
        lower_layer_dimensions = [0],
        lower_layer_names = ["g"]
      },
      {
        upper_layer_dimensions = [2],
        upper_layer_names = ["c"],
        transformation = "PassThrough",
        lower_layer_dimensions = [2],
        lower_layer_names = ["c"]
      },
      {
        upper_layer_dimensions = [3, 4],
        upper_layer_names = ["y", "ho"],
        transformation = "Embed",
        parameters = [1, 1, 0],
        lower_layer_dimensions = [3],
        lower_layer_names = ["hipad"]
      },
      {
        upper_layer_dimensions = [5, 6],
        upper_layer_names = ["x", "wo"],
        transformation = "Embed",
        parameters = [1, 1, 0],
        lower_layer_dimensions = [4],
        lower_layer_names = ["wipad"]
      }
    ],
    lower_layer_layout = ["n", "gi", "c", "hipad", "wipad"],
    upper_layer_layout = ["n", "go", "c", "y", "ho", "x", "wo"]
  } : memref<?x?x?x?x?xf32> to memref<?x?x?x?x?x?x?xf32>
  return
}

// CHECK-LABEL: func @miopen_transform_1_to_n
//  CHECK-NEXT: miopen.transform

func @miopen_gridwise_gemm(%A : memref<?x?x?xf32>, %B : memref<?x?x?xf32>, %C : memref<?x?x?xf32>) {
  miopen.gridwise_gemm(%A, %B, %C) {
    filter_layout = ["g", "k", "c", "y", "x"],
    filter_dimension = [0, 1, 2, 3, 4],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    input_dimension = [5, 6, 7, 8, 9],
    output_layout = ["n", "go", "k", "ho", "wo"],
    output_dimension = [10, 11, 12, 13, 14],
    strides = [1, 1],
    dilations = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?xf32>, memref<?x?x?xf32>, memref<?x?x?xf32>
  return
}

// CHECK-LABEL: func @miopen_gridwise_gemm
//  CHECK-NEXT: miopen.gridwise_gemm

func @miopen_gridwise_gemm_v2(%A : memref<?x?x?xf32>, %B : memref<?x?x?xf32>, %C : memref<?x?x?xf32>) {
  miopen.gridwise_gemm_v2(%A, %B, %C) {
    filter_layout = ["g", "k", "c", "y", "x"],
    filter_dimension = [0, 1, 2, 3, 4],
    input_layout = ["n", "gi", "c", "hi", "wi"],
    input_dimension = [5, 6, 7, 8, 9],
    output_layout = ["n", "go", "k", "ho", "wo"],
    output_dimension = [10, 11, 12, 13, 14],
    strides = [1, 1],
    dilations = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<?x?x?xf32>, memref<?x?x?xf32>, memref<?x?x?xf32>
  return
}

// CHECK-LABEL: func @miopen_gridwise_gemm_v2
// CHECK-NEXT: miopen.gridwise_gemm_v2

func @miopen_data_convert() {
    %0 = arith.constant 3.2 : f32
    %1 = miopen.data_convert %0  : f32 to i16
    return
}
// CHECK-LABEL: func @miopen_data_convert
// CHECK: miopen.data_convert

func @miopen_in_warp_transpose(%v : vector<8xf32>) -> vector<8xf32> {
  %cst4 = arith.constant 4 : index
  %l = miopen.workitem_id : index
  %l2 = arith.remui %l, %cst4 : index
  %0 = miopen.in_warp_transpose { size = 4 : i32,
    inGroupPerm = [0 : i32, 1 : i32, 2 : i32, 3 : i32]
  } %v, %l2 : vector<8xf32>, index
  return %0 : vector<8xf32>
}
// CHECK-LABEL: func @miopen_in_warp_transpose
// CHECK: miopen.in_warp_transpose
