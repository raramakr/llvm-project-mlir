// RUN: miopen-opt %s | FileCheck %s
// RUN: miopen-opt %s | miopen-opt | FileCheck %s
// Run: miopen-opt -mlir-print-op-generic %s | miopen-opt | FileCheck %s

func @miopen_alloc() {
  // allocation on global.
  %buffer_global = miopen.alloc() : memref<1024xi8>

  // allocation on LDS.
  %buffer_lds = miopen.alloc() : memref<1024xi8, 3>

  // allocation on register (VGPR).
  %buffer_register = miopen.alloc() : memref<1024xi8, 5>

  return
}

// CHECK-LABEL: func @miopen_alloc
//   CHECK: miopen.alloc
//   CHECK-NEXT: miopen.alloc
//   CHECK-NEXT: miopen.alloc

func @miopen_subview(%buffer : memref<1024xi8>) {
  %c0 = arith.constant 0 : index
  %c512 = arith.constant 512 : index

  // 0 offset, same type.
  %view_0 = miopen.subview(%buffer, %c0) : memref<1024xi8> to memref<1024xi8>

  // 0 offset, different type.
  %view_1 = miopen.subview(%buffer, %c0) : memref<1024xi8> to memref<256xf32>

  // 0 offset, different type.
  %view_2 = miopen.subview(%buffer, %c0) : memref<1024xi8> to memref<256xf16>

  // 0 offset, different type, different rank.
  %view_3 = miopen.subview(%buffer, %c0) { dimensions = [ 16, 16 ] } : memref<1024xi8> to memref<16x16xf32>

  // 0 offset, different type, different rank.
  %view_4 = miopen.subview(%buffer, %c0) { dimensions = [ 16, 16 ] } : memref<1024xi8> to memref<16x16xf16>

  // 512 offset, same type.
  %view_5 = miopen.subview(%buffer, %c512) : memref<1024xi8> to memref<512xi8>

  // 512 offset, different type.
  %view_6 = miopen.subview(%buffer, %c512) : memref<1024xi8> to memref<128xf32>

  // 512 offset, different type.
  %view_7 = miopen.subview(%buffer, %c512) : memref<1024xi8> to memref<128xf16>

  // 512 offset, different type, different rank.
  %view_8 = miopen.subview(%buffer, %c512) { dimensions = [ 16, 8 ] } : memref<1024xi8> to memref<16x8xf32>

  // 512 offset, different type, different rank.
  %view_9 = miopen.subview(%buffer, %c512) { dimensions = [ 16, 8 ] } : memref<1024xi8> to memref<16x8xf16>

  return
}

// CHECK-LABEL: func @miopen_subview
//   CHECK: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview
//   CHECK-NEXT: miopen.subview


func @miopen_fill(%buffer_f32 : memref<1024xf32, 5>, %buffer_i32 : memref<2xi32, 5>, %buffer_f16 : memref<1024xf16, 5>) {
  %cst = arith.constant 0.0 : f32
  miopen.fill(%buffer_f32, %cst) : memref<1024xf32, 5>, f32

  %cst_f16 = arith.constant 0.0 : f16
  miopen.fill(%buffer_f16, %cst_f16) : memref<1024xf16, 5>, f16

  %c0 = arith.constant 0 : i32
  miopen.fill(%buffer_i32, %c0) : memref<2xi32, 5>, i32
  return
}

// CHECK-LABEL: func @miopen_fill
//   CHECK: miopen.fill
//   CHECK: miopen.fill
//   CHECK: miopen.fill

func @miopen_workgroup_barrier() {
  miopen.workgroup_barrier
  return
}

// CHECK-LABEL: func @miopen_workgroup_barrier
//   CHECK-NEXT: miopen.workgroup_barrier

func @miopen_lds_barrier() {
  miopen.lds_barrier
  return
}

// CHECK-LABEL: func @miopen_lds_barrier
//   CHECK-NEXT: miopen.lds_barrier

func @miopen_indexing() {
  %0 = miopen.workgroup_id : index
  %1 = miopen.workitem_id : index
  return
}

// CHECK-LABEL: func @miopen_indexing
//   CHECK-NEXT: miopen.workgroup_id
//   CHECK-NEXT: miopen.workitem_id

func @miopen_blockwise_gemm(%A : memref<?x?x?xf32, 3>, %B : memref<?x?x?xf32, 3>, %C : memref<?x?x?xf32, 5>) {
  %c0 = arith.constant 0 : index
  miopen.blockwise_gemm(%A, %B, %C, %c0, %c0) {
    m_per_thread = 64,
    n_per_thread = 64,
    k_per_thread = 16,

    m_level0_cluster = 16,
    n_level0_cluster = 16,
    m_level1_cluster = 16,
    n_level1_cluster = 16,

    matrix_a_source_data_per_read = 4,
    matrix_b_source_data_per_read = 4
  } : memref<?x?x?xf32, 3>, memref<?x?x?xf32, 3>, memref<?x?x?xf32, 5>, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_gemm
//  CHECK: miopen.blockwise_gemm

func @miopen_blockwise_copy(%source : memref<?x?xf32>, %dest : memref<?x?xf32, 3>, %sc0 : index, %sc1 : index, %sc2 : index, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_copy %source[%sc0, %sc1, %sc2] ->  %dest[%dc0, %dc1, %dc2] : memref<?x?xf32>, index, index, index -> memref<?x?xf32, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_copy
//  CHECK: miopen.blockwise_copy %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] -> %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?xf32>, index, index, index -> memref<?x?xf32, 3>, index, index, index

// --------------------------
// blockwise_load tests.

// f32 tests.

func @miopen_blockwise_load_f32(%source : memref<?x?x?xf32>, %sc0 : index, %sc1 : index, %sc2 : index) -> f32  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf32>, index, index, index -> f32
  return %result : f32
}

// CHECK-LABEL: func @miopen_blockwise_load_f32
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf32>, index, index, index -> f32

func @miopen_blockwise_load_2xf32(%source : memref<?x?x?xf32>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<2xf32>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf32>, index, index, index -> vector<2xf32>
  return %result : vector<2xf32>
}

// CHECK-LABEL: func @miopen_blockwise_load_2xf32
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf32>, index, index, index -> vector<2xf32>

func @miopen_blockwise_load_4xf32(%source : memref<?x?x?xf32>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<4xf32>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf32>, index, index, index -> vector<4xf32>
  return %result : vector<4xf32>
}

// CHECK-LABEL: func @miopen_blockwise_load_4xf32
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf32>, index, index, index -> vector<4xf32>

// f16 tests.

func @miopen_blockwise_load_f16(%source : memref<?x?x?xf16>, %sc0 : index, %sc1 : index, %sc2 : index) -> f16  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf16>, index, index, index -> f16
  return %result : f16
}

// CHECK-LABEL: func @miopen_blockwise_load_f16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf16>, index, index, index -> f16

func @miopen_blockwise_load_2xf16(%source : memref<?x?x?xf16>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<2xf16>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf16>, index, index, index -> vector<2xf16>
  return %result : vector<2xf16>
}

// CHECK-LABEL: func @miopen_blockwise_load_2xf16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf16>, index, index, index -> vector<2xf16>

func @miopen_blockwise_load_4xf16(%source : memref<?x?x?xf16>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<4xf16>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf16>, index, index, index -> vector<4xf16>
  return %result : vector<4xf16>
}

// CHECK-LABEL: func @miopen_blockwise_load_4xf16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf16>, index, index, index -> vector<4xf16>

func @miopen_blockwise_load_8xf16(%source : memref<?x?x?xf16>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<8xf16>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xf16>, index, index, index -> vector<8xf16>
  return %result : vector<8xf16>
}

// CHECK-LABEL: func @miopen_blockwise_load_8xf16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xf16>, index, index, index -> vector<8xf16>

// i16 tests.

func @miopen_blockwise_load_i16(%source : memref<?x?x?xi16>, %sc0 : index, %sc1 : index, %sc2 : index) -> i16  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xi16>, index, index, index -> i16
  return %result : i16
}

// CHECK-LABEL: func @miopen_blockwise_load_i16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xi16>, index, index, index -> i16

func @miopen_blockwise_load_2xi16(%source : memref<?x?x?xi16>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<2xi16>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xi16>, index, index, index -> vector<2xi16>
  return %result : vector<2xi16>
}

// CHECK-LABEL: func @miopen_blockwise_load_2xi16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xi16>, index, index, index -> vector<2xi16>

func @miopen_blockwise_load_4xi16(%source : memref<?x?x?xi16>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<4xi16>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xi16>, index, index, index -> vector<4xi16>
  return %result : vector<4xi16>
}

// CHECK-LABEL: func @miopen_blockwise_load_4xi16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xi16>, index, index, index -> vector<4xi16>

func @miopen_blockwise_load_8xi16(%source : memref<?x?x?xi16>, %sc0 : index, %sc1 : index, %sc2 : index) -> vector<8xi16>  {
  %result = miopen.blockwise_load %source[%sc0, %sc1, %sc2] : memref<?x?x?xi16>, index, index, index -> vector<8xi16>
  return %result : vector<8xi16>
}

// CHECK-LABEL: func @miopen_blockwise_load_8xi16
//  CHECK: %{{.*}} = miopen.blockwise_load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<?x?x?xi16>, index, index, index -> vector<8xi16>

// --------------------------
// blockwise_store tests.

// f32 tests.

func @miopen_blockwise_store_f32(%data : f32, %dest : memref<?x?x?xf32, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : f32 -> memref<?x?x?xf32, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_f32
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : f32 -> memref<?x?x?xf32, 3>, index, index, index

func @miopen_blockwise_store_2xf32(%data : vector<2xf32>, %dest : memref<?x?x?xf32, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<2xf32> -> memref<?x?x?xf32, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_2xf32
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<2xf32> -> memref<?x?x?xf32, 3>, index, index, index

func @miopen_blockwise_store_4xf32(%data : vector<4xf32>, %dest : memref<?x?x?xf32, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<4xf32> -> memref<?x?x?xf32, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_4xf32
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<4xf32> -> memref<?x?x?xf32, 3>, index, index, index

// f16 tests.

func @miopen_blockwise_store_f16(%data : f16, %dest : memref<?x?x?xf16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : f16 -> memref<?x?x?xf16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_f16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : f16 -> memref<?x?x?xf16, 3>, index, index, index

func @miopen_blockwise_store_2xf16(%data : vector<2xf16>, %dest : memref<?x?x?xf16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<2xf16> -> memref<?x?x?xf16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_2xf16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<2xf16> -> memref<?x?x?xf16, 3>, index, index, index

func @miopen_blockwise_store_4xf16(%data : vector<4xf16>, %dest : memref<?x?x?xf16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<4xf16> -> memref<?x?x?xf16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_4xf16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<4xf16> -> memref<?x?x?xf16, 3>, index, index, index

func @miopen_blockwise_store_8xf16(%data : vector<8xf16>, %dest : memref<?x?x?xf16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<8xf16> -> memref<?x?x?xf16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_8xf16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<8xf16> -> memref<?x?x?xf16, 3>, index, index, index

// i16 tests.

func @miopen_blockwise_store_i16(%data : i16, %dest : memref<?x?x?xi16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : i16 -> memref<?x?x?xi16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_i16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : i16 -> memref<?x?x?xi16, 3>, index, index, index

func @miopen_blockwise_store_2xi16(%data : vector<2xi16>, %dest : memref<?x?x?xi16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<2xi16> -> memref<?x?x?xi16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_2xi16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<2xi16> -> memref<?x?x?xi16, 3>, index, index, index

func @miopen_blockwise_store_4xi16(%data : vector<4xi16>, %dest : memref<?x?x?xi16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<4xi16> -> memref<?x?x?xi16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_4xi16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<4xi16> -> memref<?x?x?xi16, 3>, index, index, index

func @miopen_blockwise_store_8xi16(%data : vector<8xi16>, %dest : memref<?x?x?xi16, 3>, %dc0 : index, %dc1 : index, %dc2 : index) {
  miopen.blockwise_store %data -> %dest[%dc0, %dc1, %dc2] : vector<8xi16> -> memref<?x?x?xi16, 3>, index, index, index
  return
}

// CHECK-LABEL: func @miopen_blockwise_store_8xi16
//  CHECK: miopen.blockwise_store %{{.*}} ->  %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : vector<8xi16> -> memref<?x?x?xi16, 3>, index, index, index

// --------------------------
// threadwise_copy tests.

#map0 = affine_map<(d0, d1) -> (d0, d1, d0, d1)>
#map1 = affine_map<(d0, d1) -> (d1, d0, d1, d0)>

#map2 = affine_map<(d0, d1) -> (d1, d0 floordiv 9, (d0 mod 9) floordiv 3, (d0 mod 9) mod 3)>
#map3 = affine_map<(d0, d1, d2, d3, d4, d5) -> (d0, d1, d2 * 2 + d3, d4 * 2 + d5)>

func @miopen_threadwise_copy(%source_coord : memref<2xindex, 5>, %dest_coord : memref<2xindex, 5>,
                             %source : memref<?x?xf32, 5>, %dest : memref<?x?xf32, 5>,
                             %source_with_embedded_affine : memref<?x?xf32, #map0, 3>,
                             %dest_with_embedded_affine : memref<?x?xf32, #map1, 3>,
                             %source_with_externally_defined_affine : memref<?x?x?x?xf32>,
                             %dest_with_externally_defined_affine : memref<?x?x?x?xf32>) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %source_coord_y = memref.load %source_coord[%c0] : memref<2xindex, 5>
  %source_coord_x = memref.load %source_coord[%c0] : memref<2xindex, 5>
  %dest_coord_y = memref.load %dest_coord[%c0] : memref<2xindex, 5>
  %dest_coord_x = memref.load %dest_coord[%c0] : memref<2xindex, 5>

  // check source and dest as vanilla memrefs.
  miopen.threadwise_copy
    %source[%source_coord_x, %source_coord_y] ->
    %dest[%dest_coord_x, %dest_coord_y]
    : memref<?x?xf32, 5>, index, index -> memref<?x?xf32, 5>, index, index

  // -----

  // check source with embedded affine maps.
  miopen.threadwise_copy
    %source_with_embedded_affine[%source_coord_x, %source_coord_y] ->
    %dest[%dest_coord_x, %dest_coord_y]
    : memref<?x?xf32, #map0, 3>, index, index -> memref<?x?xf32, 5>, index, index

  // check dest with embedded affine maps.
  miopen.threadwise_copy
    %source[%source_coord_x, %source_coord_y] ->
    %dest_with_embedded_affine[%dest_coord_x, %dest_coord_y]
    : memref<?x?xf32, 5>, index, index -> memref<?x?xf32, #map1, 3>, index, index

  // check source and dest with embedded affine maps.
  miopen.threadwise_copy
    %source_with_embedded_affine[%source_coord_x, %source_coord_y] ->
    %dest_with_embedded_affine[%dest_coord_x, %dest_coord_y]
    : memref<?x?xf32, #map0, 3>, index, index -> memref<?x?xf32, #map1, 3>, index, index

  // -----

  // check source with one externally defined affine map.
  miopen.threadwise_copy
    %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] ->
    %dest[%dest_coord_x, %dest_coord_y]
    {
      coord_transforms = [ { operand = 0, transforms = [#map2] } ]
    } : memref<?x?x?x?xf32>, index, index -> memref<?x?xf32, 5>, index, index

  // check source with multiple externally defined affine maps.
  miopen.threadwise_copy
    %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] ->
    %dest[%dest_coord_x, %dest_coord_y]
    {
      coord_transforms = [ { operand = 0, transforms = [#map2, #map3] } ]
    } : memref<?x?x?x?xf32>, index, index -> memref<?x?xf32, 5>, index, index

  // check destination with one externally defined affine map.
  miopen.threadwise_copy
    %source[%source_coord_x, %source_coord_y] ->
    %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y]
    {
      coord_transforms = [ { operand = 1, transforms = [#map2] } ]
    } : memref<?x?xf32, 5>, index, index -> memref<?x?x?x?xf32>, index, index

  // check destination with multiple externally defined affine map.
  miopen.threadwise_copy
    %source[%source_coord_x, %source_coord_y] ->
    %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y]
    {
      coord_transforms = [ { operand = 1, transforms = [#map2, #map3] } ]
    } : memref<?x?xf32, 5>, index, index -> memref<?x?x?x?xf32>, index, index

  // -----

  // check source and destination with one externally defined affine map.
  miopen.threadwise_copy
    %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] ->
    %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y]
    {
      coord_transforms = [
        { operand = 0, transforms = [#map2] },
        { operand = 1, transforms = [#map2] }
      ]
    } : memref<?x?x?x?xf32>, index, index -> memref<?x?x?x?xf32>, index, index

  // check source and destination with multiple externally defined affine maps.
  miopen.threadwise_copy
    %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] ->
    %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y]
    {
      coord_transforms = [
        { operand = 0, transforms = [#map2, #map3] },
        { operand = 1, transforms = [#map2, #map3] }
      ]
    } : memref<?x?x?x?xf32>, index, index -> memref<?x?x?x?xf32>, index, index

  return
}

// CHECK-LABEL: func @miopen_threadwise_copy
//  CHECK: miopen.threadwise_copy

// --------------------------
// threadwise_load tests.

// CHECK-LABEL: func @miopen_threadwise_load
func @miopen_threadwise_load(%source_coord : memref<2xindex, 5>,
                             %source : memref<?x?xf32>,
                             %source_with_embedded_affine : memref<?x?xf32, #map0>,
                             %source_with_externally_defined_affine : memref<?x?x?x?xf32>) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %source_coord_y = memref.load %source_coord[%c0] : memref<2xindex, 5>
  %source_coord_x = memref.load %source_coord[%c0] : memref<2xindex, 5>

  // check source as vanilla memref, dest as scalar.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}] : memref<?x?xf32>, index, index -> f32
  %v0 = miopen.threadwise_load %source[%source_coord_x, %source_coord_y] : memref<?x?xf32>, index, index -> f32

  // check source as vanilla memref, dest as vector.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}] : memref<?x?xf32>, index, index -> vector<4xf32>
  %v1 = miopen.threadwise_load %source[%source_coord_x, %source_coord_y] : memref<?x?xf32>, index, index -> vector<4xf32>

  // -----

  // check source with embedded affine maps, dest as scalar.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}] : memref<?x?xf32, #map0>, index, index -> f32
  %v2 = miopen.threadwise_load %source_with_embedded_affine[%source_coord_x, %source_coord_y] : memref<?x?xf32, #map0>, index, index -> f32

  // check source with embedded affine maps, dest as vector.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}] : memref<?x?xf32, #map0>, index, index -> vector<4xf32>
  %v3 = miopen.threadwise_load %source_with_embedded_affine[%source_coord_x, %source_coord_y] : memref<?x?xf32, #map0>, index, index -> vector<4xf32>

  // -----

  // check source with one externally defined affine map, dest as scalar.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}]
  %v4 = miopen.threadwise_load %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] { coord_transforms = [ { operand = 0, transforms = [#map2] } ] } : memref<?x?x?x?xf32>, index, index -> f32

  // check source with one externally defined affine map, dest as vector.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}]
  %v5 = miopen.threadwise_load %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] { coord_transforms = [ { operand = 0, transforms = [#map2] } ] } : memref<?x?x?x?xf32>, index, index -> vector<4xf32>

  // check source with multiple externally defined affine maps, dest as scalar.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}]
  %v6 = miopen.threadwise_load %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] { coord_transforms = [ { operand = 0, transforms = [#map2, #map3] } ] } : memref<?x?x?x?xf32>, index, index -> f32

  // check source with multiple externally defined affine maps, dest as vector.
  // CHECK: %{{.*}} = miopen.threadwise_load %{{.*}}[%{{.*}}, %{{.*}}]
  %v7 = miopen.threadwise_load %source_with_externally_defined_affine[%source_coord_x, %source_coord_y] { coord_transforms = [ { operand = 0, transforms = [#map2, #map3] } ] } : memref<?x?x?x?xf32>, index, index -> vector<4xf32>

  return
}

// --------------------------
// threadwise_store tests.

// CHECK-LABEL: func @miopen_threadwise_store
func @miopen_threadwise_store(%data_scalar : f32,
                              %data_vector : vector<4xf32>,
                              %dest_coord : memref<2xindex, 5>,
                              %dest : memref<?x?xf32>,
                              %dest_with_embedded_affine : memref<?x?xf32, #map1>,
                              %dest_with_externally_defined_affine : memref<?x?x?x?xf32>) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %dest_coord_y = memref.load %dest_coord[%c0] : memref<2xindex, 5>
  %dest_coord_x = memref.load %dest_coord[%c0] : memref<2xindex, 5>

  // check dest as vanilla memrefs, data as scalar.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}] : f32 -> memref<?x?xf32>, index, index
  miopen.threadwise_store %data_scalar -> %dest[%dest_coord_x, %dest_coord_y] : f32 -> memref<?x?xf32>, index, index

  // check dest as vanilla memrefs, data as vector.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}] : vector<4xf32> -> memref<?x?xf32>, index, index
  miopen.threadwise_store %data_vector -> %dest[%dest_coord_x, %dest_coord_y] : vector<4xf32> -> memref<?x?xf32>, index, index

  // -----

  // check dest with embedded affine maps, data as scalar.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}] : f32
  miopen.threadwise_store %data_scalar -> %dest_with_embedded_affine[%dest_coord_x, %dest_coord_y] : f32 -> memref<?x?xf32, #map1>, index, index

  // check dest with embedded affine maps, data as vector.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}] : vector<4xf32>
  miopen.threadwise_store %data_vector -> %dest_with_embedded_affine[%dest_coord_x, %dest_coord_y] : vector<4xf32> -> memref<?x?xf32, #map1>, index, index

  // -----

  // check destination with one externally defined affine map, data as scalar.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}]
  miopen.threadwise_store %data_scalar -> %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y] { coord_transforms = [ { operand = 1, transforms = [#map2] } ] } : f32 -> memref<?x?x?x?xf32>, index, index

  // check destination with one externally defined affine map, data as vector.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}]
  miopen.threadwise_store %data_vector -> %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y] { coord_transforms = [ { operand = 1, transforms = [#map2] } ] } : vector<4xf32> -> memref<?x?x?x?xf32>, index, index

  // check destination with multiple externally defined affine map, data as scalar.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}]
  miopen.threadwise_store %data_scalar -> %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y] { coord_transforms = [ { operand = 1, transforms = [#map2, #map3] } ] } : f32 -> memref<?x?x?x?xf32>, index, index

  // check destination with multiple externally defined affine map, data as vector.
  // CHECK: miopen.threadwise_store %{{.*}} -> %{{.*}}[%{{.*}}, %{{.*}}]
  miopen.threadwise_store %data_vector -> %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y] { coord_transforms = [ { operand = 1, transforms = [#map2, #map3] } ] } : vector<4xf32> -> memref<?x?x?x?xf32>, index, index

  return
}

// --------------------------
// threadwise_copy_v2 tests.

#map11 = affine_map<(d0, d1) -> (d1, d0, d1, d0)>

#map12 = affine_map<(d0, d1) -> (d1, d0 floordiv 9, (d0 mod 9) floordiv 3, (d0 mod 9) mod 3)>
#map13 = affine_map<(d0, d1, d2, d3, d4, d5) -> (d0, d1, d2 * 2 + d3, d4 * 2 + d5)>

func @miopen_threadwise_copy_v2( %source_coord : memref<2xindex, 5>, %dest_coord : memref<2xindex, 5>,
                                %source : vector<32xf32>, %dest : memref<?x?xf32>,
                                %dest_with_embedded_affine : memref<?x?xf32, #map11>,
                                %dest_with_externally_defined_affine : memref<?x?x?x?xf32>) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index

  %source_coord_y = memref.load %source_coord[%c0] : memref<2xindex, 5>
  %source_coord_x = memref.load %source_coord[%c1] : memref<2xindex, 5>
  %dest_coord_y = memref.load %dest_coord[%c0] : memref<2xindex, 5>
  %dest_coord_x = memref.load %dest_coord[%c1] : memref<2xindex, 5>

  // check dest as a vanilla memref.
  miopen.threadwise_copy_v2
    %source[%c0] ->
    %dest[%dest_coord_x, %dest_coord_y] { sourceOffset = 0 : index }
    : vector<32xf32>, index -> memref<?x?xf32>, index, index

  // -----

  // check source with one externally defined affine map.
  miopen.threadwise_copy_v2
    %source[%source_coord_x, %source_coord_y] ->
    %dest[%dest_coord_x, %dest_coord_y]
    {
      sourceOffset = 0 : index,
      coord_transforms = [ { operand = 0, transforms = [#map12] } ]
    } : vector<32xf32>, index, index -> memref<?x?xf32>, index, index

  // check source with multiple externally defined affine maps.
  miopen.threadwise_copy_v2
    %source[%source_coord_x, %source_coord_y] ->
    %dest[%dest_coord_x, %dest_coord_y]
    {
      sourceOffset = 0 : index,
      coord_transforms = [ { operand = 0, transforms = [#map12, #map13] } ]
    } : vector<32xf32>, index, index -> memref<?x?xf32>, index, index

  // -----

  // check source and destination with one externally defined affine map.
  miopen.threadwise_copy_v2
    %source[%source_coord_x, %source_coord_y] ->
    %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y]
    {
      sourceOffset = 0 : index,
      coord_transforms = [
        { operand = 0, transforms = [#map12] },
        { operand = 1, transforms = [#map12] }
      ]
    } : vector<32xf32>, index, index -> memref<?x?x?x?xf32>, index, index

  // check source and destination with multiple externally defined affine maps.
  miopen.threadwise_copy_v2
    %source[%source_coord_x, %source_coord_y] ->
    %dest_with_externally_defined_affine[%dest_coord_x, %dest_coord_y]
    {
      sourceOffset = 0 : index,
      coord_transforms = [
        { operand = 0, transforms = [#map12, #map13] },
        { operand = 1, transforms = [#map12, #map13] }
      ]
    } : vector<32xf32>, index, index -> memref<?x?x?x?xf32>, index, index

  return
}

// CHECK-LABEL: func @miopen_threadwise_copy_v2
//  CHECK: miopen.threadwise_copy_v2

func @miopen_threadwise_gemm(%lhs : memref<1x4x8xf32>, %rhs : memref<1x4x8xf32>, %output : memref<1x8x8xf32>) {
  miopen.threadwise_gemm(%lhs, %rhs, %output) : memref<1x4x8xf32>, memref<1x4x8xf32>, memref<1x8x8xf32>
  return
}

// CHECK-LABEL: func @miopen_threadwise_gemm
//  CHECK: miopen.threadwise_gemm

// ----

func @miopen_mfma_v2_f32(%a : f32, %b : f32, %c : vector<32xf32>) -> vector<32xf32> {
  %d = miopen.mfma_v2(%a, %b, %c) { instr = "mfma_f32_32x32x1f32", imm = [1, 0, 0] } : f32, vector<32xf32>
  return %d : vector<32xf32>
}

// CHECK-LABEL: func @miopen_mfma_v2_f32
//   CHECK: miopen.mfma_v2

func @miopen_mfma_v2_f16(%a : vector<4xf16>, %b : vector<4xf16>, %c : vector<32xf32>) -> vector<32xf32> {
  %d = miopen.mfma_v2(%a, %b, %c) { instr = "mfma_f32_32x32x4f16", imm = [1, 0, 0] } : vector<4xf16>, vector<32xf32>
  return %d : vector<32xf32>
}

// CHECK-LABEL: func @miopen_mfma_v2_f16
//   CHECK: miopen.mfma_v2

func @miopen_mfma_v2_bf16(%a : vector<2xi16>, %b : vector<2xi16>, %c : vector<32xf32>) -> vector<32xf32> {
  %d = miopen.mfma_v2(%a, %b, %c) { instr = "mfma_f32_32x32x2bf16", imm = [1, 0, 0] } : vector<2xi16>, vector<32xf32>
  return %d : vector<32xf32>
}

// CHECK-LABEL: func @miopen_mfma_v2_bf16
//   CHECK: miopen.mfma_v2

// ----

func @miopen_xdlops_gemm_v2_one_result(%matrixA : memref<12288xf32, 3>, %matrixB : memref<12288xf32, 3>,
                                       %bufferA : memref<32xf32, 5>, %bufferB : memref<16xf32, 5>) -> vector<32xf32> {
  %c0 = arith.constant 0 : index
  %c0f = arith.constant 0.0 : f32
  %vectorC0 = splat %c0f : vector<32xf32>
  %vectorD0 = miopen.xdlops_gemm_v2(%matrixA, %matrixB, %c0, %c0, %bufferA, %bufferB, %vectorC0) {
    m = 256,
    n = 256,
    k = 16,
    m_per_wave = 128,
    n_per_wave = 64,
    coord_transforms = [{operand = 1 : i32, transforms = [affine_map<(d0) -> (d0 + 8192)>]}, {operand = 0 : i32, transforms = []}]
  } : memref<12288xf32, 3>, memref<12288xf32, 3>, index, index, memref<32xf32, 5>, memref<16xf32, 5>, vector<32xf32> -> vector<32xf32>
  return %vectorD0 : vector<32xf32>
}

// CHECK-LABEL: func @miopen_xdlops_gemm_v2_one_result
//  CHECK: miopen.xdlops_gemm_v2

// ----

func @miopen_xdlops_gemm_v2_two_results(%matrixA : memref<12288xf32, 3>, %matrixB : memref<12288xf32, 3>,
                                        %bufferA : memref<32xf32, 5>, %bufferB: memref<16xf32, 5>) -> (vector<32xf32>, vector<32xf32>) {
  %c0 = arith.constant 0 : index
  %c0f = arith.constant 0.0 : f32
  %vectorC0 = splat %c0f : vector<32xf32>
  %vectorC1 = splat %c0f : vector<32xf32>
  %vectorD0, %vectorD1 = miopen.xdlops_gemm_v2(%matrixA, %matrixB, %c0, %c0, %bufferA, %bufferB, %vectorC0, %vectorC1) {
    m = 256,
    n = 256,
    k = 16,
    m_per_wave = 128,
    n_per_wave = 64,
    coord_transforms = [{operand = 1 : i32, transforms = [affine_map<(d0) -> (d0 + 8192)>]}, {operand = 0 : i32, transforms = []}]
  } : memref<12288xf32, 3>, memref<12288xf32, 3>, index, index, memref<32xf32, 5>, memref<16xf32, 5>, vector<32xf32>, vector<32xf32> -> vector<32xf32>, vector<32xf32>
  return %vectorD0, %vectorD1 : vector<32xf32>, vector<32xf32>
}

// CHECK-LABEL: func @miopen_xdlops_gemm_v2_two_results
//  CHECK: miopen.xdlops_gemm_v2

// ----

func @miopen_blockwise_gemm_v2_one_result(%matrixA : memref<12288xf32, 3>, %matrixB : memref<12288xf32, 3>,
                                          %bufferA : memref<32xf32, 5>, %bufferB : memref<16xf32, 5>) -> vector<32xf32> {
  %c0 = arith.constant 0 : index
  %c0f = arith.constant 0.0 : f32
  %vectorC0 = splat %c0f : vector<32xf32>
  %vectorD0 = miopen.blockwise_gemm_v2(%matrixA, %matrixB, %c0, %c0, %bufferA, %bufferB, %vectorC0) {
    m = 256,
    n = 256,
    k = 16,
    m_per_wave = 128,
    n_per_wave = 64,
    coord_transforms = [{operand = 1 : i32, transforms = [affine_map<(d0) -> (d0 + 8192)>]}, {operand = 0 : i32, transforms = []}]
  } : memref<12288xf32, 3>, memref<12288xf32, 3>, index, index, memref<32xf32, 5>, memref<16xf32, 5>, vector<32xf32> -> vector<32xf32>
  return %vectorD0 : vector<32xf32>
}

// CHECK-LABEL: func @miopen_blockwise_gemm_v2_one_result
//  CHECK: miopen.blockwise_gemm_v2

// ----

func @miopen_blockwise_gemm_v2_two_results(%matrixA : memref<12288xf32, 3>, %matrixB : memref<12288xf32, 3>,
                                           %bufferA : memref<32xf32, 5>, %bufferB : memref<16xf32, 5>) -> (vector<32xf32>, vector<32xf32>) {
  %c0 = arith.constant 0 : index
  %c0f = arith.constant 0.0 : f32
  %vectorC0 = splat %c0f : vector<32xf32>
  %vectorC1 = splat %c0f : vector<32xf32>
  %vectorD0, %vectorD1 = miopen.blockwise_gemm_v2(%matrixA, %matrixB, %c0, %c0, %bufferA, %bufferB, %vectorC0, %vectorC1) {
    m = 256,
    n = 256,
    k = 16,
    m_per_wave = 128,
    n_per_wave = 64,
    coord_transforms = [{operand = 1 : i32, transforms = [affine_map<(d0) -> (d0 + 8192)>]}, {operand = 0 : i32, transforms = []}]
  } : memref<12288xf32, 3>, memref<12288xf32, 3>, index, index, memref<32xf32, 5>, memref<16xf32, 5>, vector<32xf32>, vector<32xf32> -> vector<32xf32>, vector<32xf32>
  return %vectorD0, %vectorD1 : vector<32xf32>, vector<32xf32>
}

// CHECK-LABEL: func @miopen_blockwise_gemm_v2_two_results
//  CHECK: miopen.blockwise_gemm_v2
