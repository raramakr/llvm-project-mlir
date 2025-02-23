// RUN: mlir-miopen-driver -ph -p | FileCheck %s

// CHECK:  [[MIN:%.*]] = arith.constant 1 : i16
// CHECK-NEXT:   [[MAX:%.*]] = arith.constant 1 : i16
// CHECK-NEXT:   [[SEED:%.*]] = arith.constant 1 : i32
// CHECK-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// CHECK-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1| FileCheck %s --check-prefix=RAND1

// RAND1:  [[MIN:%.*]] = arith.constant -5 : i16
// RAND1-NEXT:   [[MAX:%.*]] = arith.constant 5 : i16
// RAND1-NEXT:   [[SEED:%.*]] = arith.constant 1 : i32
// RAND1-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// RAND1-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 2| FileCheck %s --check-prefix=RAND2

// RAND2:  [[MIN:%.*]] = arith.constant -5 : i16
// RAND2-NEXT:   [[MAX:%.*]] = arith.constant 5 : i16
// RAND2-NEXT:   [[SEED:%.*]] = arith.constant 2 : i32
// RAND2-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// RAND2-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_side filter | FileCheck %s --check-prefix=FWD_FILTER

// FWD_FILTER:    [[ONE:%.*]] = arith.constant 1 : i16
// FWD_FILTER-NEXT:  [[ZERO:%.*]] = arith.constant 0 : i16
// FWD_FILTER-NEXT:  [[MIN:%.*]] = arith.constant -5 : i16
// FWD_FILTER-NEXT:   [[MAX:%.*]] = arith.constant 5 : i16
// FWD_FILTER-NEXT:   [[SEED:%.*]] = arith.constant 1 : i32
// FWD_FILTER-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// FWD_FILTER-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[ONE]], [[ONE]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_side input | FileCheck %s --check-prefix=FWD_INPUT

// FWD_INPUT:    [[ONE:%.*]] = arith.constant 1 : i16
// FWD_INPUT-NEXT:  [[ZERO:%.*]] = arith.constant 0 : i16
// FWD_INPUT-NEXT:  [[MIN:%.*]] = arith.constant -5 : i16
// FWD_INPUT-NEXT:   [[MAX:%.*]] = arith.constant 5 : i16
// FWD_INPUT-NEXT:   [[SEED:%.*]] = arith.constant 1 : i32
// FWD_INPUT-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[ONE]], [[ONE]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// FWD_INPUT-NEXT:    call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_side filter -operation conv2d_bwd_data | FileCheck %s --check-prefix=BWD_DATA_FILTER

// BWD_DATA_FILTER:  [[ONE:%.*]] = arith.constant 1 : i16
// BWD_DATA_FILTER-NEXT:  [[ZERO:%.*]] = arith.constant 0 : i16
// BWD_DATA_FILTER-NEXT:  [[MIN:%.*]] = arith.constant -5 : i16
// BWD_DATA_FILTER-NEXT:  [[MAX:%.*]] = arith.constant 5 : i16
// BWD_DATA_FILTER-NEXT:  [[SEED:%.*]] = arith.constant 1 : i32
// BWD_DATA_FILTER-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_DATA_FILTER-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ZERO]], [[ZERO]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_DATA_FILTER-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ONE]], [[ONE]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_side output  -operation conv2d_bwd_data | FileCheck %s --check-prefix=BWD_DATA_OUTPUT

// BWD_DATA_OUTPUT:  [[ONE:%.*]] = arith.constant 1 : i16
// BWD_DATA_OUTPUT-NEXT:  [[ZERO:%.*]] = arith.constant 0 : i16
// BWD_DATA_OUTPUT-NEXT:  [[MIN:%.*]] = arith.constant -5 : i16
// BWD_DATA_OUTPUT-NEXT:  [[MAX:%.*]] = arith.constant 5 : i16
// BWD_DATA_OUTPUT-NEXT:  [[SEED:%.*]] = arith.constant 1 : i32
// BWD_DATA_OUTPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ONE]], [[ONE]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_DATA_OUTPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ZERO]], [[ZERO]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_DATA_OUTPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_side input -operation conv2d_bwd_weight | FileCheck %s --check-prefix=BWD_WEIGHT_INPUT

// BWD_WEIGHT_INPUT:  [[ONE:%.*]] = arith.constant 1 : i16
// BWD_WEIGHT_INPUT-NEXT:  [[ZERO:%.*]] = arith.constant 0 : i16
// BWD_WEIGHT_INPUT-NEXT:  [[MIN:%.*]] = arith.constant -5 : i16
// BWD_WEIGHT_INPUT-NEXT:  [[MAX:%.*]] = arith.constant 5 : i16
// BWD_WEIGHT_INPUT-NEXT:  [[SEED:%.*]] = arith.constant 1 : i32
// BWD_WEIGHT_INPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ZERO]], [[ZERO]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_WEIGHT_INPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_WEIGHT_INPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ONE]], [[ONE]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_side output  -operation conv2d_bwd_weight | FileCheck %s --check-prefix=BWD_WEIGHT_OUTPUT

// BWD_WEIGHT_OUTPUT:  [[ONE:%.*]] = arith.constant 1 : i16
// BWD_WEIGHT_OUTPUT-NEXT:  [[ZERO:%.*]] = arith.constant 0 : i16
// BWD_WEIGHT_OUTPUT-NEXT:  [[MIN:%.*]] = arith.constant -5 : i16
// BWD_WEIGHT_OUTPUT-NEXT:  [[MAX:%.*]] = arith.constant 5 : i16
// BWD_WEIGHT_OUTPUT-NEXT:  [[SEED:%.*]] = arith.constant 1 : i32
// BWD_WEIGHT_OUTPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ZERO]], [[ZERO]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_WEIGHT_OUTPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[ONE]], [[ONE]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// BWD_WEIGHT_OUTPUT-NEXT:   call @mcpuMemset5DFloatRandInt({{.*}}, [[MIN]], [[MAX]], [[SEED]]) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()

// RUN: mlir-miopen-driver -ph -p -rand 1 -rand_type float | FileCheck %s --check-prefix=RAND_FLOAT

// RAND_FLOAT:  [[MIN:%.*]] = arith.constant -1 : i16
// RAND_FLOAT-NEXT:   [[MAX:%.*]] = arith.constant 1 : i16
// RAND_FLOAT-NEXT:   [[SEED:%.*]] = arith.constant 1 : i32
// RAND_FLOAT-NEXT:   call @mcpuMemset5DFloatRandFloat(%3, %c-1_i16, %c1_i16, %c1_i32) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
// RAND_FLOAT-NEXT:   call @mcpuMemset5DFloatRandFloat(%4, %c-1_i16, %c1_i16, %c1_i32) : (memref<?x?x?x?x?xf32>, i16, i16, i32) -> ()
