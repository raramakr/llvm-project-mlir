// This tests checks the following aspects of lowering component:
// * Input has three transformations in total
// * Input has correct output_layout across transformations

// RUN: miopen-opt -miopen-affix-params -miopen-lowering %s | FileCheck %s

func @miopen_conv2d_cyxk_cnhw_knhw(%filter : memref<1x8x3x3x128xf32>, %input : memref<1x8x128x32x32xf32>, %output : memref<1x128x128x30x30xf32>) {
  miopen.conv2d(%filter, %input, %output) {
    arch = "gfx906",
    num_cu = 64,
    filter_layout = ["g", "c", "y", "x", "k"],
    input_layout = ["gi", "ci", "ni", "hi", "wi"],
    output_layout = ["go", "ko", "no", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<1x8x3x3x128xf32>, memref<1x8x128x32x32xf32>, memref<1x128x128x30x30xf32>
  return
}
// CHECK-LABEL: func @miopen_conv2d
// CHECK-NEXT:  miopen.transform(%arg0)
// CHECK-NEXT:  miopen.transform(%arg1)
// CHECK:       upper_layer_layout = ["gi", "ci", "ni", "hipad", "wipad"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gi", "ci", "ni", "y", "ho", "x", "wo"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gemmG", "gemmK", "gemmN"]
// CHECK-NEXT:  miopen.transform(%arg2)

func @miopen_conv2d_bwd_data_gcyxk_gcnhw_gknhw(%filter: memref<1x1024x1x1x1024xf32>, %input: memref<1x1024x128x14x14xf32>, %output: memref<1x1024x128x14x14xf32>) attributes {kernel = 0 : i32} {
  miopen.conv2d_bwd_data(%filter, %input, %output) {
    arch = "gfx908",
    dilations = [1 : i32, 1 : i32],
    filter_layout = ["g", "c", "y", "x", "k"],
    gemm_id = 0 : i32,
    input_layout = ["gi", "ci", "ni", "hi", "wi"],
    num_cu = 120 : i32,
    output_layout = ["go", "ko", "no", "ho", "wo"],
    padding = [0 : i32, 0 : i32, 0 : i32, 0 : i32],
    strides = [1 : i32, 1 : i32],
    xdlopsV2 = true
  } : memref<1x1024x1x1x1024xf32>, memref<1x1024x128x14x14xf32>, memref<1x1024x128x14x14xf32>
  return
}

// CHECK-LABEL: func @miopen_conv2d_bwd_data
// CHECK-NEXT:  miopen.transform(%arg0)
// CHECK:       miopen.transform(%arg1)
// CHECK:       upper_layer_layout = ["gi", "ni", "ci", "hipad", "wipad"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gi", "ni", "ci", "ytilda", "htilda", "xtilda", "wtilda"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gemmG", "gemmM", "gemmN"]
// CHECK-NEXT:  miopen.transform(%arg2)

func @miopen_conv2d_bwd_weight_cyxk_cnhw_knhw(%filter : memref<1x8x3x3x128xf32>, %input : memref<1x8x128x32x32xf32>, %output : memref<1x128x128x30x30xf32>) {
  miopen.conv2d_bwd_weight(%filter, %input, %output) {
    arch = "gfx906",
    num_cu = 64,
    filter_layout = ["g", "c", "y", "x", "k"],
    input_layout = ["gi", "ci", "ni", "hi", "wi"],
    output_layout = ["go", "ko", "no", "ho", "wo"],
    dilations = [1, 1],
    strides = [1, 1],
    padding = [0, 0, 0, 0]
  } : memref<1x8x3x3x128xf32>, memref<1x8x128x32x32xf32>, memref<1x128x128x30x30xf32>
  return
}
// CHECK-LABEL: func @miopen_conv2d_bwd_weight
// CHECK-NEXT:  miopen.transform(%arg0)
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gemmG", "gemmM", "gemmNPad"]
// CHECK-NEXT:  miopen.transform(%arg1)
// CHECK:       upper_layer_layout = ["gi", "ci", "ni", "hipad", "wipad"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gi", "ci", "ni", "y", "ho", "x", "wo"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gemmG", "gemmK", "gemmN"]
// CHECK-NEXT:  miopen.transform
// CHECK:       upper_layer_layout = ["gemmG", "gemmK", "gemmNPad"]
// CHECK-NEXT:  miopen.transform(%arg2)
