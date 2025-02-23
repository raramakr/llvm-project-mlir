//===- LowerMIOpenOps.h - MLIR to C++ for MIOpen conversion ---------------===//
//
// Part of the MLIR Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the lowering pass for the MLIR to MIOpen C++ conversion.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_DIALECT_MIOPEN_LOWERMIOPENOPS_H
#define MLIR_DIALECT_MIOPEN_LOWERMIOPENOPS_H

#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MIOpen/AffineMapHelper.h"
#include "mlir/Dialect/MIOpen/MIOpenOps.h"
#include "mlir/Dialect/MIOpen/Passes.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Vector/VectorOps.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"

#include "XdlopsCodeSelection.h"
#include "mlir/Dialect/MIOpen/Tuning/GridwiseGemmParams.h"
#include "mlir/Dialect/MIOpen/utility/BackwardWeightV4R4Helper.h"
#include "mlir/Dialect/MIOpen/utility/common.h"
#include "mlir/Dialect/MIOpen/utility/math.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/IndexedMap.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <iterator>

using namespace mlir;
using namespace mlir::miopen;
using namespace mlir::arith;

// 2G ,INT MAX Value = 2147483647, use 2147483648 as offset and buffer
// store do nothing
static constexpr int kTwoGB = 2147483647;

//===----------------------------------------------------------------------===//
// FIXME. XXX.
// Workaround to obtain gemmKExtra / gemmMExtra / gemmNExtra attribute.
// And use it to override legacy load/store debug switch.
//===----------------------------------------------------------------------===//
inline bool overrideLoadStoreHack(const DictionaryAttr &transformSpec,
                                  bool original) {
  if (transformSpec) {
    Attribute metadataAttr = transformSpec.get("metadata");
    if (metadataAttr) {
      ArrayAttr layeredTransformMetadata =
          metadataAttr.template cast<ArrayAttr>();
      for (unsigned iter = 0; iter < layeredTransformMetadata.size(); ++iter) {
        DictionaryAttr dictAttr =
            layeredTransformMetadata[iter].template cast<DictionaryAttr>();
        // enable workaround when padding kernel,
        // if gemmKExtra || gemmMExtra || gemmNExtraAttr
        // use workaround to skip index map errors
        auto gemmKExtraAttr = dictAttr.get("gemmKExtra");
        auto gemmMExtraAttr = dictAttr.get("gemmMExtra");
        auto gemmNExtraAttr = dictAttr.get("gemmNExtra");
        if (gemmKExtraAttr) {
          auto gemmKExtra =
              gemmKExtraAttr.template cast<IntegerAttr>().getInt();
          if (gemmKExtra > 0) {
            return true;
          }
        }

        if (gemmMExtraAttr) {
          auto gemmMExtra =
              gemmMExtraAttr.template cast<IntegerAttr>().getInt();
          if (gemmMExtra > 0) {
            return true;
          }
        }

        if (gemmNExtraAttr) {
          auto gemmNExtra =
              gemmNExtraAttr.template cast<IntegerAttr>().getInt();
          if (gemmNExtra > 0) {
            return true;
          }
        }
      }
    }
  }
  return original;
}

//===----------------------------------------------------------------------===//
// Utility function to determine the type to be loaded
//===----------------------------------------------------------------------===//
template <typename T>
inline std::tuple<Type, int, int, int>
computeLoadStoreTypeInfo(OpBuilder &b, T &gop, Type elementType,
                         SmallVectorImpl<Type> &variadicTypes,
                         const SmallVector<uint64_t, 3> &dims, bool isMatrixA) {
  uint64_t loadLength = 1;
  uint64_t storeLength = 1;
  int vectorDim = dims.size() - 1;
  if (isMatrixA) {
    loadLength = gop->getAttr("matrix_a_source_data_per_read")
                     .template cast<IntegerAttr>()
                     .getInt();
    storeLength = gop->getAttr("matrix_a_dest_data_per_write_dim_m")
                      .template cast<IntegerAttr>()
                      .getInt();
    vectorDim = gop->getAttr("matrix_a_source_vector_read_dim")
                    .template cast<IntegerAttr>()
                    .getInt();
  } else {
    loadLength = gop->getAttr("matrix_b_source_data_per_read")
                     .template cast<IntegerAttr>()
                     .getInt();
    storeLength = gop->getAttr("matrix_b_dest_data_per_write_dim_n")
                      .template cast<IntegerAttr>()
                      .getInt();
    vectorDim = gop->getAttr("matrix_b_source_vector_read_dim")
                    .template cast<IntegerAttr>()
                    .getInt();
  }

  int64_t itemsToCopy = 1;
  for (auto l : dims)
    itemsToCopy *= l;

  for (unsigned iter = 0; iter < itemsToCopy; ++iter)
    variadicTypes.push_back(elementType);

  return std::make_tuple(elementType, vectorDim, loadLength, storeLength);
}

//===----------------------------------------------------------------------===//
// Utility function to compute sliceLengths for threadwise_copy and
// threadwise_copy_v2 to determine the bounds of load/store loops.
//===----------------------------------------------------------------------===//
inline void
computeSliceLengths(SmallVector<uint64_t, 2> &sliceLengths,
                    const Optional<AffineMap> &composedSourceTransform,
                    const Optional<AffineMap> &composedDestTransform,
                    const ArrayAttr &coordTransformsAttr,
                    const Optional<ArrayAttr> &boundAttr, Type sourceType,
                    Type destType) {
  auto populateSliceLengthsWithTypeShape =
      [](SmallVector<uint64_t, 2> &sliceLengths, Type type) {
        assert(type.isa<MemRefType>() || type.isa<VectorType>());
        if (type.isa<MemRefType>()) {
          // Use the shape of memref as initial slice lengths.
          for (auto dim : type.template cast<MemRefType>().getShape())
            sliceLengths.push_back(dim);
        } else if (type.isa<VectorType>()) {
          // Use the shape of vector as initial slice lengths.
          for (auto dim : type.template cast<VectorType>().getShape())
            sliceLengths.push_back(dim);
        }
      };

  // Order to decide the slice lengths:
  // - bound attribute.
  // - domain attribute from the source in case both source and dest has affine
  //   transformations.
  // - shape of the dest in case only the source has affine transformations.
  // - shape of the source in case the source has no affine transfromations.
  if (boundAttr) {
    for (unsigned i = 0; i < boundAttr->size(); ++i)
      sliceLengths.push_back(
          (*boundAttr)[i].template cast<IntegerAttr>().getInt());
  } else {
    if (composedSourceTransform) {
      if (composedDestTransform) {
        // Use domain attribute from source.
        for (auto attr : coordTransformsAttr) {
          auto dictAttr = attr.template cast<DictionaryAttr>();
          auto operandIndex =
              dictAttr.get("operand").template cast<IntegerAttr>().getInt();
          if (operandIndex == 0) {
            auto domainAttr = dictAttr.get("domain").template cast<ArrayAttr>();
            for (unsigned i = 0; i < domainAttr.size(); ++i)
              sliceLengths.push_back(
                  domainAttr[i].template cast<IntegerAttr>().getInt());
          }
        }
      } else {
        populateSliceLengthsWithTypeShape(sliceLengths, destType);
      }
    } else {
      populateSliceLengthsWithTypeShape(sliceLengths, sourceType);
    }
  }
}

//===----------------------------------------------------------------------===//
// Utility function to emit constant float op. Returns a scalar.
//===----------------------------------------------------------------------===//
inline Value createConstantFloatOp(OpBuilder &b, Location loc, Type elementType,
                                   float value) {
  Value ret;
  if (elementType == b.getF32Type()) {
    ret = b.create<ConstantFloatOp>(loc, APFloat(value), b.getF32Type());
  } else if (elementType == b.getF16Type()) {
    bool lossy = false;
    APFloat constant(value);
    constant.convert(APFloat::IEEEhalf(), llvm::RoundingMode::TowardZero,
                     &lossy);
    ret = b.create<ConstantFloatOp>(loc, constant, b.getF16Type());
  } else if (elementType == b.getIntegerType(16)) {
    ret = b.create<ConstantIntOp>(loc, static_cast<int>(value),
                                  b.getIntegerType(16));
  }
  return ret;
}

//===----------------------------------------------------------------------===//
// Utility function to emit constant zero op. Can return scalars or vectors.
//===----------------------------------------------------------------------===//
inline Value createZeroConstantFloatOp(OpBuilder &b, Location loc, Type type) {
  Type elementType = type;
  if (type.isa<VectorType>())
    elementType = type.template cast<VectorType>().getElementType();
  auto semantics = static_cast<APFloat::Semantics>(-1);
  if (elementType == b.getF32Type()) {
    semantics = APFloat::S_IEEEsingle;
  } else if (elementType == b.getF16Type()) {
    semantics = APFloat::S_IEEEhalf;
  } else if (elementType == b.getIntegerType(16)) {
    semantics = APFloat::S_BFloat;
  } else {
    llvm_unreachable("Unexpected float semantics");
  }

  auto zero = APFloat::getZero(APFloat::EnumToSemantics(semantics));
  Value retValue;

  if (auto vecType = type.dyn_cast<VectorType>()) {
    Attribute constValue;
    if (auto intType = elementType.dyn_cast<IntegerType>()) {
      auto intZero = zero.bitcastToAPInt();
      assert(intType.getIntOrFloatBitWidth() == intZero.getBitWidth());
      constValue = b.getIntegerAttr(elementType, intZero);
    } else {
      constValue = b.getFloatAttr(elementType, zero);
    }
    llvm::SmallVector<Attribute> constValues;
    std::fill_n(std::back_inserter(constValues), vecType.getNumElements(),
                constValue);
    retValue = b.create<mlir::ConstantOp>(
        loc, DenseElementsAttr::get(vecType, constValues), type);
  } else {
    if (auto intType = elementType.dyn_cast<IntegerType>()) {
      auto intZero = zero.bitcastToAPInt();
      assert(intType.getIntOrFloatBitWidth() == intZero.getBitWidth());
      retValue =
          b.create<mlir::ConstantOp>(loc, b.getIntegerAttr(intType, intZero), type);
    } else {
      retValue =
          b.create<mlir::ConstantOp>(loc, b.getFloatAttr(elementType, zero), type);
    }
  }

  return retValue;
}

//===----------------------------------------------------------------------===//
// Utility function to emit load instructions with potentially OOB checks.
//===----------------------------------------------------------------------===//
inline Value emitLoadLogic(OpBuilder &b, Location loc, MemRefType sourceType,
                           Type loadedType, bool toEmitOOBLoadCheckLogic,
                           const SmallVector<unsigned, 8> &oobLoadCheckDims,
                           const Value &source,
                           const SmallVector<Value, 8> &srcLowerIndices) {
  auto emitLoadInstruction =
      [&b, &loc](const SmallVector<Value, 8> &srcLowerIndices,
                 MemRefType sourceType, Type loadedType,
                 const Value &source) -> Value {
    Value loadedValue;
    if (loadedType.isa<VectorType>()) {
      // Issue vector load.
      if (sourceType.getMemorySpaceAsInt() == 0) {
        // Option 1: buffer load.
        // use buffer load if the source memref is on address space 0
        SmallVector<Value, 4> srcLowerIndicesI32;
        for (auto v : srcLowerIndices)
          srcLowerIndicesI32.push_back(
              b.create<IndexCastOp>(loc, v, b.getIntegerType(32)));
        loadedValue = b.create<gpu::MubufLoadOp>(loc, loadedType, source,
                                                 srcLowerIndicesI32);
      } else {
        // Option 2: scalar load + vector.insertelement
        VectorType loadedVectorType = loadedType.template cast<VectorType>();
        Type elementType = loadedVectorType.getElementType();
        int64_t vectorLength = loadedVectorType.getShape()[0];

        Value loadedVector =
            createZeroConstantFloatOp(b, loc, loadedVectorType);

        SmallVector<Value, 8> srcLowerIndicesUpdated = srcLowerIndices;
        int64_t dim = sourceType.getRank() - 1;
        for (int64_t iter = 0; iter < vectorLength; ++iter) {
          auto iterIndex = b.create<ConstantIndexOp>(loc, iter);
          srcLowerIndicesUpdated[dim] =
              b.create<AddIOp>(loc, srcLowerIndices[dim], iterIndex);
          auto loadedElement = b.create<memref::LoadOp>(loc, elementType, source,
                                                srcLowerIndicesUpdated);

          loadedVector = b.create<vector::InsertElementOp>(
              loc, loadedVectorType, loadedElement, loadedVector, iterIndex);
        }
        loadedValue = loadedVector;
      }
    } else {
      // Issue scalar load.
      loadedValue = b.create<memref::LoadOp>(loc, loadedType, source, srcLowerIndices);
    }
    return loadedValue;
  };

  Value loadedValue;
  auto zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);

  if (toEmitOOBLoadCheckLogic) {
    // Pre-populate srcLowerLoadOOBIndices. It will be modified inside
    // toEmitOOBCheckLogic basic block.
    SmallVector<Value, 8> srcLowerLoadOOBIndices = srcLowerIndices;

    // Emit a useful constant 0f for later use.
    Value zeroOp = createZeroConstantFloatOp(b, loc, loadedType);

    // Walkthrough all lower level indices where the dimension has
    // padding, check if the result lies within boundaries.

    // Logic in C++:
    // bool withinBounds = true;
    // for (auto dim : oobLoadCheckDims) {
    //   withBounds &=
    //     (srcLowerIndices[dim] >= 0 &&
    //      srcLowerIndices[dim] < sourceType.getShape()[dim]) {
    // }
    Value withinBoundsOp = b.create<ConstantIntOp>(loc, 1, b.getIntegerType(1));
    for (auto dim : oobLoadCheckDims) {
      Value coord = srcLowerIndices[dim];
      Value lowerBoundCheckOp =
          b.create<CmpIOp>(loc, CmpIPredicate::sge, coord, zeroConstantOp);
      Value upperBoundOp =
          b.create<ConstantIndexOp>(loc, sourceType.getShape()[dim]);
      Value upperBoundCheckOp =
          b.create<CmpIOp>(loc, CmpIPredicate::slt, coord, upperBoundOp);
      Value withinBoundInOneDimOp =
          b.create<AndIOp>(loc, lowerBoundCheckOp, upperBoundCheckOp);

      withinBoundsOp =
          b.create<AndIOp>(loc, withinBoundsOp, withinBoundInOneDimOp);

      // Prepare srcLowerLoadOOBIndices.
      srcLowerLoadOOBIndices[dim] = zeroConstantOp;
    }

    // Logic:
    // if (withinBounds) {
    //   // load address = lower indices from affine transform.
    // } else {
    //   // OOB. Prepare an address known NOT OOB.
    //   // For example, in NGCHW case:
    //   // load address = {N, G, C, 0, 0}
    //   // In NGHWC case:
    //   // load address = {N, G, 0, 0, C}
    // }
    // V = load(load address)
    // if (withinBounds) {
    //   return V
    // } else {
    //   return 0
    // }

    // Emit the first IfOp.
    auto firstIfWithinBoundsOp = b.create<scf::IfOp>(
        loc,
        TypeRange{b.getIndexType(), b.getIndexType(), b.getIndexType(),
                  b.getIndexType(), b.getIndexType()},
        withinBoundsOp, /*withElseRegion=*/true);

    // Then part.
    auto firstIfWithinBoundsThenBuilder =
        firstIfWithinBoundsOp.getThenBodyBuilder();
    firstIfWithinBoundsThenBuilder.create<scf::YieldOp>(
        loc,
        ValueRange{srcLowerIndices[0], srcLowerIndices[1], srcLowerIndices[2],
                   srcLowerIndices[3], srcLowerIndices[4]});

    // Else part.
    auto firstIfWithinBoundsElseBuilder =
        firstIfWithinBoundsOp.getElseBodyBuilder();
    firstIfWithinBoundsElseBuilder.create<scf::YieldOp>(
        loc, ValueRange{srcLowerLoadOOBIndices[0], srcLowerLoadOOBIndices[1],
                        srcLowerLoadOOBIndices[2], srcLowerLoadOOBIndices[3],
                        srcLowerLoadOOBIndices[4]});

    // Issue scalar load.
    SmallVector<Value, 8> srcLowerIndicesUpdated;
    for (unsigned iter = 0; iter < 5; ++iter)
      srcLowerIndicesUpdated.push_back(firstIfWithinBoundsOp.results()[iter]);
    loadedValue = emitLoadInstruction(srcLowerIndicesUpdated, sourceType,
                                      loadedType, source);

    // Emit the second IfOp.
    auto secondIfWithinBoundsOp =
        b.create<scf::IfOp>(loc, loadedType, withinBoundsOp, true);
    auto secondIfWithinBoundsThenBuilder =
        secondIfWithinBoundsOp.getThenBodyBuilder();
    secondIfWithinBoundsThenBuilder.create<scf::YieldOp>(loc, loadedValue);
    auto secondIfWithinBoundsElseBuilder =
        secondIfWithinBoundsOp.getElseBodyBuilder();
    secondIfWithinBoundsElseBuilder.create<scf::YieldOp>(loc, zeroOp);

    loadedValue = secondIfWithinBoundsOp.results()[0];

  } else {
    loadedValue =
        emitLoadInstruction(srcLowerIndices, sourceType, loadedType, source);
  }
  return loadedValue;
}

//===----------------------------------------------------------------------===//
// Utility function to emit store instructions with potentially OOB checks.
//===----------------------------------------------------------------------===//
enum InMemoryDataOperation { Set, AtomicAdd };
// we set attr at affixThreadwiseCopyV2Attributes or
// affixThreadwiseCopyAttributes first we use
// NotBwdPaddingOrStrideOne,StrideTwoXdlopsNHWC,StrideTwoXdlopsNCHW,StrideTwoNonXdlopsNCHW,
// StrideTwoNonXdlopsNHWC then in ThreadwiseCopyV2RewritePattern, due to xdlops
// have lots of different workaround we split
// NotBwdPaddingOrStrideOne,StrideTwoXdlopsNHWC,StrideTwoXdlopsNCHW, into
// GemmMPadStrideTwoXdlopsNCHW,
// GemmNPadStrideTwoXdlopsNCHW,GemmMNPadStrideTwoXdlopsNCHW,GemmNPadStrideTwoXdlopsNHWC
// if we don't need workaround , use NotBwdPaddingOrStrideOne
// non xdlops should split also ,but not implement yet
enum BwdPaddingKernelStatus {
  NotBwdPaddingOrStrideOne,
  StrideTwoNonXdlopsNCHW,
  StrideTwoNonXdlopsNHWC,
  StrideTwoXdlopsNHWC,
  StrideTwoXdlopsNCHW,
  GemmMPadStrideTwoXdlopsNCHW,
  GemmNPadStrideTwoXdlopsNCHW,
  GemmMNPadStrideTwoXdlopsNCHW,
  GemmNPadStrideTwoXdlopsNHWC,
  GemmMNPadStrideTwoXdlopsNHWC,
  GemmMPadStrideTwoXdlopsNHWC
};

inline void emitStoreLogic(
    BwdPaddingKernelStatus bwdPaddingStatus, OpBuilder &b, Location loc,
    MemRefType destType, Type typeToStore, bool toEmitOOBStoreCheckLogic,
    const SmallVector<unsigned, 8> &oobStoreCheckDims, const Value &dest,
    const SmallVector<Value, 8> &destLowerIndices, const Value &value,
    InMemoryDataOperation memoryOp = InMemoryDataOperation::Set) {
  auto emitStoreInstruction = [&b, &loc](
                                  const Value &value, MemRefType destType,
                                  Type typeToStore, const Value &dest,
                                  const SmallVector<Value, 8> &destLowerIndices,
                                  const Value &oob) {
    if (typeToStore.isa<VectorType>()) {
      // Issue vector store.
      if (destType.getMemorySpaceAsInt() == 0) {
        // use raw buffer store if the dest memref is on address space 0
        Value oobI32 = b.create<IndexCastOp>(loc, oob, b.getIntegerType(32));
        SmallVector<Value, 4> destLowerIndicesI32;
        for (auto v : destLowerIndices)
          destLowerIndicesI32.push_back(
              b.create<IndexCastOp>(loc, v, b.getIntegerType(32)));
        b.create<gpu::RawbufStoreOp>(loc, value, dest, oobI32,
                                     destLowerIndicesI32);
      } else {
        // Option 2: vector.extractelement + scalar store.
        assert(destType.getRank() == 1);
        assert(destLowerIndices.size() == 1);
        VectorType valueVectorType = typeToStore.template cast<VectorType>();
        Type elementType = destType.getElementType();
        int64_t vectorLength = valueVectorType.getShape()[0];
        SmallVector<Value, 8> destLowerIndicesUpdated = destLowerIndices;
        for (int64_t iter = 0; iter < vectorLength; ++iter) {
          Value iterOp = b.create<ConstantIndexOp>(loc, iter);
          destLowerIndicesUpdated[0] =
              b.create<AddIOp>(loc, destLowerIndices[0], iterOp);
          auto element = b.create<vector::ExtractElementOp>(loc, elementType,
                                                            value, iterOp);
          b.create<memref::StoreOp>(loc, element, dest, destLowerIndicesUpdated);
        }
      }
    } else {
      // Issue scalar store.
      if (destType.getMemorySpaceAsInt() == 0) {
        // use raw buffer store if the dest memref is on address space 0
        SmallVector<Value, 4> destLowerIndicesI32;
        Value oobI32 = b.create<IndexCastOp>(loc, oob, b.getIntegerType(32));
        for (auto v : destLowerIndices)
          destLowerIndicesI32.push_back(
              b.create<IndexCastOp>(loc, v, b.getIntegerType(32)));
        b.create<gpu::RawbufStoreOp>(loc, value, dest, oobI32,
                                     destLowerIndicesI32);
      } else {
        b.create<memref::StoreOp>(loc, value, dest, destLowerIndices);
      }
    }
  };

  auto zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);
  auto oobAddrOp = b.create<ConstantIndexOp>(loc, kTwoGB);

  if (toEmitOOBStoreCheckLogic) {
    SmallVector<Value, 8> destLowerStoreOOBIndices = destLowerIndices;

    // Logic in C++:
    // bool withinBounds = true;
    // for (auto dim : oobStoreCheckDims) {
    //   withBounds &=
    //     (destLowerIndices[dim] >= 0 &&
    //      destLowerIndices[dim] < destType.getShape()[dim]) {
    // }

    Value withinStoreBoundsOp =
        b.create<ConstantIntOp>(loc, 1, b.getIntegerType(1));
    for (auto dim : oobStoreCheckDims) {
      Value coordStore = destLowerIndices[dim];
      Value lowerBoundCheckOp =
          b.create<CmpIOp>(loc, CmpIPredicate::sge, coordStore, zeroConstantOp);
      Value upperBoundOp =
          b.create<ConstantIndexOp>(loc, destType.getShape()[dim]);
      Value upperBoundCheckOp =
          b.create<CmpIOp>(loc, CmpIPredicate::slt, coordStore, upperBoundOp);
      Value withinBoundInOneDimOp =
          b.create<AndIOp>(loc, lowerBoundCheckOp, upperBoundCheckOp);

      withinStoreBoundsOp =
          b.create<AndIOp>(loc, withinStoreBoundsOp, withinBoundInOneDimOp);

      destLowerStoreOOBIndices[dim] = zeroConstantOp;
    }

    auto ifWithinBoundsOp = b.create<scf::IfOp>(
        loc,
        TypeRange{b.getIndexType(), b.getIndexType(), b.getIndexType(),
                  b.getIndexType(), b.getIndexType(), b.getIndexType()},
        withinStoreBoundsOp, true);

    auto thenBuilder = ifWithinBoundsOp.getThenBodyBuilder();
    thenBuilder.create<scf::YieldOp>(
        loc, ValueRange{zeroConstantOp, destLowerIndices[0],
                        destLowerIndices[1], destLowerIndices[2],
                        destLowerIndices[3], destLowerIndices[4]});

    auto elseBuilder = ifWithinBoundsOp.getElseBodyBuilder();
    // here is workaround of backward data padding kernel to avoid compiler
    // issues
    if (bwdPaddingStatus == BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne) {
      elseBuilder.create<scf::YieldOp>(
          loc,
          ValueRange{oobAddrOp, destLowerStoreOOBIndices[0],
                     destLowerStoreOOBIndices[1], destLowerStoreOOBIndices[2],
                     destLowerStoreOOBIndices[3], destLowerStoreOOBIndices[4]});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::StrideTwoNonXdlopsNHWC) {
      elseBuilder.create<scf::YieldOp>(
          loc,
          ValueRange{oobAddrOp, destLowerIndices[0], zeroConstantOp,
                     zeroConstantOp, destLowerIndices[3], destLowerIndices[4]});

    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::StrideTwoNonXdlopsNCHW) {
      elseBuilder.create<scf::YieldOp>(
          loc, ValueRange{oobAddrOp, destLowerIndices[0], destLowerIndices[1],
                          destLowerIndices[2], zeroConstantOp, zeroConstantOp});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::GemmMPadStrideTwoXdlopsNCHW) {
      // c!=64 pad=0 nchw
      elseBuilder.create<scf::YieldOp>(
          loc,
          ValueRange{oobAddrOp, destLowerIndices[0], destLowerIndices[1],
                     zeroConstantOp, destLowerIndices[3], destLowerIndices[4]});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::GemmNPadStrideTwoXdlopsNCHW) {
      // n!=64 pad=0 nchw
      elseBuilder.create<scf::YieldOp>(
          loc, ValueRange{oobAddrOp, zeroConstantOp, destLowerIndices[1],
                          destLowerIndices[2], destLowerIndices[3],
                          destLowerIndices[4]});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::GemmMNPadStrideTwoXdlopsNCHW) {
      // n!=64 c!=64 pad=0 nchw
      elseBuilder.create<scf::YieldOp>(
          loc,
          ValueRange{oobAddrOp, zeroConstantOp, destLowerIndices[1],
                     zeroConstantOp, destLowerIndices[3], destLowerIndices[4]});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::GemmNPadStrideTwoXdlopsNHWC) {
      // gemmn%64!=0 padh=padw=0 nhwc
      elseBuilder.create<scf::YieldOp>(
          loc, ValueRange{oobAddrOp, zeroConstantOp, destLowerIndices[1],
                          destLowerIndices[2], destLowerIndices[3],
                          destLowerIndices[4]});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::GemmMPadStrideTwoXdlopsNHWC) {
      // gemmm%64!=0 padh=padw=0 nhwc
      elseBuilder.create<scf::YieldOp>(
          loc,
          ValueRange{oobAddrOp, destLowerIndices[0], destLowerIndices[1],
                     destLowerIndices[2], destLowerIndices[3], zeroConstantOp});
    } else if (bwdPaddingStatus ==
               BwdPaddingKernelStatus::GemmMNPadStrideTwoXdlopsNHWC) {
      // gemmm%64!=0 gemmN%64!=0 padh=padw=0 nhwc
      elseBuilder.create<scf::YieldOp>(
          loc,
          ValueRange{oobAddrOp, zeroConstantOp, destLowerIndices[1],
                     destLowerIndices[2], destLowerIndices[3], zeroConstantOp});
    }

    // ifWithinBoundsOp results:
    // - 0 : oob address, 0 if inbound, 2GB if oob.
    // - 1~5 : 5D naive tensor address.
    SmallVector<Value, 8> destLowerIndicesUpdated;
    for (unsigned iter = 1; iter <= 5; ++iter)
      destLowerIndicesUpdated.push_back(ifWithinBoundsOp.getResults()[iter]);

    emitStoreInstruction(value, destType, typeToStore, dest,
                         destLowerIndicesUpdated,
                         /*oob=*/ifWithinBoundsOp.getResults()[0]);

  } else {
    if (memoryOp == InMemoryDataOperation::AtomicAdd) {
      SmallVector<Value, 8> destLowerStoreIndices;
      for (unsigned i = 0; i < destLowerIndices.size(); ++i) {
        auto dstIndex = b.create<IndexCastOp>(loc, destLowerIndices[i],
                                              b.getIntegerType(32));
        destLowerStoreIndices.push_back(dstIndex);
      }
      b.create<gpu::AtomicFAddOp>(loc, value, dest, destLowerStoreIndices);
    } else {
      emitStoreInstruction(value, destType, typeToStore, dest, destLowerIndices,
                           /*oob=*/zeroConstantOp);
    }
  }
}
//===----------------------------------------------------------------------===//
// Utility function to determine if we need to emit codes for OOB checks.
//===----------------------------------------------------------------------===//
inline bool obtainOOBCheckInfo(const Optional<AffineMap> &composedTransform,
                               const ArrayAttr &boundCheckAttr,
                               SmallVector<unsigned, 8> &oobCheckDims) {
  // Determine if we need to emit codes for out-of-bound check.
  bool ret = false;
  if (composedTransform && boundCheckAttr) {
    if (boundCheckAttr.size() == composedTransform->getNumResults()) {
      for (unsigned iter = 0; iter < boundCheckAttr.size(); ++iter) {
        if (boundCheckAttr[iter].template cast<IntegerAttr>().getInt()) {
          ret = true;
          oobCheckDims.push_back(iter);
        }
      }
    }
  }
  return ret;
}

//===----------------------------------------------------------------------===//
// Utility function to compute various information wrt threadwise_copy.
// - coordinate length : return value.
// - composed affine transform.
// - layered affine transform.
// - specification of transformation.
// - bound check attribute.
//===----------------------------------------------------------------------===//
inline unsigned obtainGenericTensorTransformationInfo(
    int64_t operandIndex, const Type type, const ArrayAttr &coordTransformsAttr,
    Optional<AffineMap> &composedTransform,
    SmallVector<AffineMap> &layeredTransform, DictionaryAttr &transformSpec,
    ArrayAttr &boundCheckAttr) {
  // Get source and dest coordinates.
  //
  // 1. For memrefs with no externally defined affine maps in
  // coord_transforms
  //    attribute, or embedded affine maps. Use its rank.
  // 2. For memrefs with externally defined maps, use its input rank.
  // 3. For memrefs with embedded maps, use its input rank.
  assert(type.isa<MemRefType>() || type.isa<VectorType>());
  unsigned coordLength = 0;
  SmallVector<AffineMap> typeAffineMaps;
  if (type.isa<MemRefType>()) {
    MemRefType memrefType = type.cast<MemRefType>();
    coordLength = memrefType.getRank();
    if (!memrefType.getLayout().isIdentity())
      typeAffineMaps.push_back(memrefType.getLayout().getAffineMap());
  } else if (type.isa<VectorType>()) {
    VectorType vectorType = type.cast<VectorType>();
    coordLength = vectorType.getShape().size();
    // Vector types doesn't have type-associated affine maps.
    // Keep typeAffineMaps uninitialized.
  }

  if (typeAffineMaps.size()) {
    coordLength = typeAffineMaps[0].getNumInputs();
    // Compose affine maps.
    composedTransform = composeTransforms(typeAffineMaps);

    // Populate affine maps for each layer.
    layeredTransform.assign(typeAffineMaps.begin(), typeAffineMaps.end());
  }
  // Obtain metadata of coordinate transformations.
  if (coordTransformsAttr) {
    for (auto attr : coordTransformsAttr) {
      auto dictAttr = attr.template cast<DictionaryAttr>();
      auto index =
          dictAttr.get("operand").template cast<IntegerAttr>().getInt();
      auto transforms = dictAttr.get("transforms").template cast<ArrayAttr>();
      if (index == operandIndex) {
        coordLength = transforms[0]
                          .template cast<AffineMapAttr>()
                          .getValue()
                          .getNumInputs();
        transformSpec = dictAttr;
        // Compose affine maps.
        composedTransform = composeTransforms(transforms);

        // Populate affine maps for each layer.
        for (auto &am : transforms)
          layeredTransform.push_back(
              am.template cast<AffineMapAttr>().getValue());

        auto bcAttr = dictAttr.get("bound_check");
        if (bcAttr)
          boundCheckAttr = bcAttr.template cast<ArrayAttr>();
      }
    }
  }

  // Return computed coordinate length.
  return coordLength;
}

//===----------------------------------------------------------------------===//
// Utility function to emit type conversion ops.
//===----------------------------------------------------------------------===//
inline Value createTypeConversionOp(OpBuilder &b, Location loc, Value source,
                                    Type sourceType, Type destType) {
  // Convert from sourceType to destType if necessary.
  Value result = source;
  Type sourceElemType = sourceType;
  Type destElemType = destType;
  if (auto sourceVec = sourceType.dyn_cast<VectorType>()) {
    if (auto destVec = destType.dyn_cast<VectorType>()) {
      assert(sourceVec.getNumElements() == destVec.getNumElements() &&
             "source and destinatioon have same length");
      sourceElemType = sourceVec.getElementType();
      destElemType = destVec.getElementType();
    } else {
      llvm_unreachable("Can't store vector sources to scalar destinations in "
                       "output writeback");
    }
  }
  if (sourceElemType != destElemType) {
    // Possible cases:
    // - fp16 -> fp32 : use fpext.
    // - fp32 -> fp16 : use fptrunc.
    // - fp16/fp32 -> bf16(i16) : use miopen.data_convert.
    // All these ops act elementwise on vectors
    // except the BFloat conversion
    if (sourceElemType == b.getF16Type() && destElemType == b.getF32Type()) {
      result = b.create<arith::ExtFOp>(loc, source, destType);
    } else if (sourceElemType == b.getF32Type() &&
               destElemType == b.getF16Type()) {
      result = b.create<arith::TruncFOp>(loc, source, destType);
    } else if (destElemType == b.getIntegerType(16)) {
      if (sourceElemType == sourceType) {
        result = b.create<miopen::DataConvertOp>(loc, destType, source);
      } else {
        result = createZeroConstantFloatOp(b, loc, destType);
        int64_t numElements = destType.cast<VectorType>().getNumElements();
        for (int64_t i = 0; i < numElements; ++i) {
          Value extracted = b.create<vector::ExtractElementOp>(
              loc, source, b.create<ConstantIndexOp>(loc, i));
          Value converted =
              b.create<miopen::DataConvertOp>(loc, destElemType, extracted);
          result = b.create<vector::InsertElementOp>(
              loc, converted, result, b.create<ConstantIndexOp>(loc, i));
        }
      }
    }
  }
  return result;
}

//===----------------------------------------------------------------------===//
// Utility function to emit the logic to copy between naive tensors.
// This function is used within the lowering logic of threadwise_copy.
//===----------------------------------------------------------------------===//
inline void emitNaiveTensorCopyLogic(
    OpBuilder &b, Location loc, int64_t NSliceRow, int64_t NSliceCol,
    int64_t DataPerAccess, const SmallVector<Value, 8> &sourceCoord,
    const SmallVector<Value, 8> &destCoord,
    const Optional<AffineMap> &composedSourceTransform,
    const Optional<AffineMap> &composedDestTransform, Type sourceElementType,
    Type destElementType, Value source, Value dest) {
  auto oneConstantOp = b.create<ConstantIndexOp>(loc, 1);

  SmallVector<Value, 8> srcUpperIndices;
  srcUpperIndices.append(sourceCoord);
  SmallVector<Value, 8> destUpperIndices;
  destUpperIndices.append(destCoord);

  // Emit fully-unrolled loops.
  for (unsigned ivo = 0; ivo < NSliceRow; ++ivo) {
    for (unsigned ivi = 0; ivi < NSliceCol; ivi += DataPerAccess) {
      // src_index = (0, ivo_i32, ivi_i32) + sourceCoord
      // Apply affine transformations to compute the low-level coordinate.
      SmallVector<Value, 8> srcLowerIndices;
      if (composedSourceTransform)
        srcLowerIndices =
            expandAffineMap(b, loc, *composedSourceTransform, srcUpperIndices)
                .getValue();
      else
        srcLowerIndices = srcUpperIndices;

      // Load from source.
      // Issue scalar load.
      Value scalarValue =
          b.create<memref::LoadOp>(loc, sourceElementType, source, srcLowerIndices);
      srcUpperIndices[2] =
          b.create<AddIOp>(loc, srcUpperIndices[2], oneConstantOp);
      // Convert from sourceElementType to destElementType if necessary.
      Value convertedScalarValue = createTypeConversionOp(
          b, loc, scalarValue, sourceElementType, destElementType);

      // dst_index = (0, ivo_i32, ivi_i32) + destCoord
      // Apply affine transformations to compute the low-level coordinate.
      SmallVector<Value, 8> destLowerIndices;
      if (composedDestTransform)
        destLowerIndices =
            expandAffineMap(b, loc, *composedDestTransform, destUpperIndices)
                .getValue();
      else
        destLowerIndices = destUpperIndices;

      // Store to dest.
      // Issue scalar store.
      b.create<memref::StoreOp>(loc, convertedScalarValue, dest, destLowerIndices);
      destUpperIndices[2] =
          b.create<AddIOp>(loc, destUpperIndices[2], oneConstantOp);

    } // ivi
    srcUpperIndices[1] =
        b.create<AddIOp>(loc, srcUpperIndices[1], oneConstantOp);
    destUpperIndices[1] =
        b.create<AddIOp>(loc, destUpperIndices[1], oneConstantOp);
  }   // ivo
}

//===----------------------------------------------------------------------===//
// Utility function to populate the transform metadata in cases there is none.
// Just populate "lower_layer_bounds" attribute from the lower level shape.
//===----------------------------------------------------------------------===//
inline void
populateTransformMetadataFromLowerType(OpBuilder &b, ShapedType lowerType,
                                       ArrayAttr &transformMetadata) {
  SmallVector<Attribute, 4> lowerShapeAttr;
  for (auto &v : lowerType.getShape())
    lowerShapeAttr.push_back(b.getI32IntegerAttr(v));
  transformMetadata = b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr(
      "lower_layer_bounds", b.getArrayAttr({lowerShapeAttr}))})});
}

//===---------------------------------------------------------
// Determine if the operation provided is a constant, and return its value if it
// is
//===---------------------------------------------------------
inline Optional<int64_t> isConstantValue(Value v) {
  auto *op = v.getDefiningOp();
  while (auto cast = dyn_cast<IndexCastOp>(op)) {
    op = cast.in().getDefiningOp();
  }
  if (auto intOp = dyn_cast<ConstantIntOp>(op)) {
    return intOp.value();
  }
  if (auto indexOp = dyn_cast<ConstantIndexOp>(op)) {
    return indexOp.value();
  }
  return llvm::None;
}

//===----------------------------------------------------------------------===//
// Utility function to compute index diff map.
//===----------------------------------------------------------------------===//
inline void
computeIndexDiffMap(OpBuilder &b, Location loc,
                    const SmallVector<Value, 8> &upperIndicesDiff,
                    const DictionaryAttr &transformMetadata,
                    const SmallVector<Value, 8> &lowerIndicesOriginal,
                    SmallVector<Value, 8> &lowerIndicesDiff,
                    SmallVector<Value, 8> &lowerIndicesUpdated) {
  Value zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);
  // Obtain the shape of lower level memref.
  ArrayAttr lowerLayerShape =
      transformMetadata.get("lower_layer_bounds").template cast<ArrayAttr>();

  // Input:
  // - upper_diff
  // - upper_indices_original
  // - upper_layer_bounds
  // - lower_indices_original
  // - lower_layer_bounds
  // - F : a vector of functions mapping upper level dimensions to lower level
  // dimensions.
  // - G : metadata of F
  //
  // Output:
  // - lower_diff : the computed diffs on the lower layer. such information
  //                would be passed to the next layer below as upper diff.
  // - lower_indices_updated : the updated lower layer indices. clients will
  //                           use the values to issue loads / stores.
  //
  // For each transform g specified in G:
  //   Let P be the upper dimensions used by g.
  //   Let Q be the lower dimensions used by g.
  //   Let T be upper_layer_bounds.
  //
  //   Switch g:
  //     Case Pad :
  //       |P| = |Q|
  //       For each i in P, and its counterpart j in Q
  //         lower_diff[j] = upper_diff[i]
  //         lower_indices_updated[j] = lower_indices_origina[j] + lower_diff[j]
  //
  //     Case PassThrough :
  //       |P| = |Q|
  //       For each i in P, and its counterpart j in Q
  //         lower_diff[j] = upper_diff[i]
  //         lower_indices_updated[j] = lower_indices_origina[j] + lower_diff[j]
  //
  //     Case Slice :
  //       |P| = |Q|
  //       For each i in P, and its counterpart j in Q
  //         lower_diff[j] = upper_diff[i]
  //         lower_indices_updated[j] = lower_indices_origina[j] + lower_diff[j]
  //
  //     Case Embed:
  //       |P| = k, currently k will be >= 2.
  //       |Q| shall be 1
  //       Let (p_{0}, ... , p_{k-1}) be elements in P, |P| = k
  //       Let (e_{0}, ... , e_{k-1}) be parameters of P
  //       Let j be the counterpart in q
  //       lower_diff[j] = sum_over_P(e_{i} * upper_diff[p_{i}])
  //       lower_indices_updated[j] = lower_indices_origina[j] + lower_diff[j]
  //
  //     Case UnMerge:
  //       |Q| shall be 1
  //       Let (p_{0}, ... , p_{k-1}) be elements in P, |P| = k
  //       Let (e_{0}, ... , e_{k-1}) be parameters of P
  //       Let (f_{0}, ... , f_{k-1})
  //         The value of f_{i} is defined as:
  //           f_{k-1} = 1
  //           f_{i} = mul_over_{domain: e_[i+1 .. k-1], iterator=l}(T_{l})
  //       Let j be the counterpart in q
  //         lower_diff[j] = sum_over_P(f_{i} * upper_diff[p_{i}])
  //         lower_indices_updated[j] = lower_indices_origina[j] + lower_diff[j]
  //
  //     Case Unfold:
  //       This transformation is currently only used on filter, when c/y/x
  //       dimensions are together.
  //       |P| shall be 1
  //       Let (q_{0}, ... , q_{k-1}) be elements in Q, |Q| = k
  //       Let (f_{0}, ... , f_{k-1}) be elements in F to compute from P to Q
  //       For each i in Q,
  //         lower_diff_tilda[i] = f_{i}(upper_diff)
  //       For each i in Q,
  //         lower_indices_modified[i] = lower_indices_original[i] +
  //           lower_diff_tilda[i]
  //       lower_diff = lower_diff_tilda
  //       lower_indices_updated = lower_indices_modified
  //
  //     Case Merge:
  //       |P| shall be 1
  //       Let (q_{0}, ... , q_{k-1}) be elements in Q, |Q| = k
  //       Let (f_{0}, ... , f_{k-1}) be elements in F to compute from P to Q
  //       For each i in Q,
  //         lower_diff_tilda[i] = f_{i}(upper_diff)
  //       For each i in Q,
  //         lower_indices_modified[i] = lower_indices_original[i] +
  //           lower_diff_tilda[i]
  //       For each i in Q, starting from i-1 down to 0 in descending order
  //         lower_indices_carrychecked[i] = carry/overflow check for
  //           lower_indices_modified[i]
  //       lower_diff = lower_indices_carrychecked - lower_indices_original
  //       lower_indices_updated = lower_indices_carrychecked
  //

  // llvm::errs() << "Transform metadata:\n";
  // llvm::errs() << transformMetadata << "\n";
  // llvm::errs() << "Upper indices diff size: "
  //              << upperIndicesDiff.size() << "\n";
  // llvm::errs() << "Lower indices original size: "
  //              << lowerIndicesOriginal.size() << "\n\n";

  // Look into layout attribute inside transform metadata.
  auto layoutAttr = transformMetadata.get("layout");
  assert(layoutAttr);
  // layoutArrayAttr is G in the pseudo code above.
  ArrayAttr layoutArrayAttr = layoutAttr.template cast<ArrayAttr>();

  // lower level diff map
  // key : lower level dimension value.
  // value : lower level diff on that dimension.
  DenseMap<int64_t, Value> lowerIndicesDiffMap;

  // lower level updated coordinate map
  // key : lower level dimension value.
  // value : lower level updated coordinate on that dimension.
  DenseMap<int64_t, Value> lowerIndicesUpdatedMap;

  auto addToOriginal = [&b, loc](Value original, Value diff) -> Value {
    auto mbDiffConst = isConstantValue(diff);
    if (mbDiffConst.hasValue()) {
      int64_t diff = mbDiffConst.getValue();
      if (diff == 0) {
        return original;
      }
      auto mbOriginalConst = isConstantValue(original);
      if (mbOriginalConst.hasValue()) {
        return b.create<ConstantIndexOp>(loc,
                                         diff + mbOriginalConst.getValue());
      }
    }
    return b.create<AddIOp>(loc, original, diff);
  };

  // Iterate through all transformations specified in g.
  for (auto &mapping : layoutArrayAttr) {
    DictionaryAttr g = mapping.template cast<DictionaryAttr>();
    // llvm::errs() << "g: " << g << "\n";

    // Obtain transformation information from g.
    StringAttr transformation =
        g.get("transformation").template cast<StringAttr>();
    auto p = g.get("upper_layer_dimensions").template cast<ArrayAttr>();
    auto q = g.get("lower_layer_dimensions").template cast<ArrayAttr>();

    if (transformation.getValue() == "Embed") {
      auto e = g.get("parameters").template cast<ArrayAttr>();
      assert(e.size() == p.size());
      assert(q.size() == 1);
      Value lowerDiff = zeroConstantOp;
      for (unsigned iter = 0; iter < e.size(); ++iter) {
        int64_t coefficient = e[iter].template cast<IntegerAttr>().getInt();
        int64_t upperDim = p[iter].template cast<IntegerAttr>().getInt();
        auto mbUpperDiff = isConstantValue(upperIndicesDiff[upperDim]);
        auto mbLowerDiff = isConstantValue(lowerDiff);
        if (mbUpperDiff.hasValue() && mbLowerDiff.hasValue()) {
          lowerDiff = b.create<ConstantIndexOp>(
              loc,
              mbLowerDiff.getValue() + coefficient * mbUpperDiff.getValue());
        } else {
          lowerDiff = b.create<AddIOp>(
              loc, lowerDiff,
              b.create<MulIOp>(loc, b.create<ConstantIndexOp>(loc, coefficient),
                               upperIndicesDiff[upperDim]));
        }
      }

      int64_t lowerDim = q[0].template cast<IntegerAttr>().getInt();
      lowerIndicesDiffMap[lowerDim] = lowerDiff;
      lowerIndicesUpdatedMap[lowerDim] =
          addToOriginal(lowerIndicesOriginal[lowerDim], lowerDiff);
    } else if (transformation.getValue() == "UnMerge") {
      auto e = g.get("parameters").template cast<ArrayAttr>();
      assert(e.size() == p.size());
      assert(q.size() == 1);
      int64_t upperDim = p[0].template cast<IntegerAttr>().getInt();
      Value lowerDiff = upperIndicesDiff[upperDim];
      for (unsigned iter = 1; iter < e.size(); ++iter) {
        int64_t coefficient = e[iter].template cast<IntegerAttr>().getInt();
        int64_t upperDim = p[iter].template cast<IntegerAttr>().getInt();
        auto mbUpperDiff = isConstantValue(upperIndicesDiff[upperDim]);
        auto mbLowerDiff = isConstantValue(lowerDiff);
        if (mbUpperDiff.hasValue() && mbLowerDiff.hasValue()) {
          lowerDiff = b.create<ConstantIndexOp>(
              loc,
              mbUpperDiff.getValue() + coefficient * mbLowerDiff.getValue());
        } else {
          lowerDiff = b.create<AddIOp>(
              loc, upperIndicesDiff[upperDim],
              b.create<MulIOp>(loc, b.create<ConstantIndexOp>(loc, coefficient),
                               lowerDiff));
        }
      }
      int64_t lowerDim = q[0].template cast<IntegerAttr>().getInt();
      lowerIndicesDiffMap[lowerDim] = lowerDiff;
      lowerIndicesUpdatedMap[lowerDim] =
          addToOriginal(lowerIndicesOriginal[lowerDim], lowerDiff);
    } else if ((transformation.getValue() == "PassThrough") ||
               (transformation.getValue() == "Pad") ||
               (transformation.getValue() == "Slice")) {
      assert(p.size() == q.size());
      for (unsigned iter = 0; iter < q.size(); ++iter) {
        int64_t upperDim = p[iter].template cast<IntegerAttr>().getInt();
        int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
        Value upperDiff = upperIndicesDiff[upperDim];
        Value lowerDiff = upperDiff;
        lowerIndicesDiffMap[lowerDim] = lowerDiff;
        lowerIndicesUpdatedMap[lowerDim] =
            addToOriginal(lowerIndicesOriginal[lowerDim], lowerDiff);
      }
    } else if ((transformation.getValue() == "Merge") ||
               (transformation.getValue() == "Unfold")) {
      assert(p.size() == 1);
      int64_t upperDim = p[0].template cast<IntegerAttr>().getInt();

      // Obtain the transformation.
      AffineMap transform = transformMetadata.get("map")
                                .template cast<ArrayAttr>()[0]
                                .template cast<AffineMapAttr>()
                                .getValue();

      SmallVector<Value, 8> lowerDiffModified;
      auto mbUpperDiffVal = isConstantValue(upperIndicesDiff[upperDim]);
      if (mbUpperDiffVal.hasValue()) {
        // In case upper level diff is a constant, use constantFold.
        int64_t upperDiff = mbUpperDiffVal.getValue();

        // Populate an upper diff vector with all indices 0, other than
        // upperDim dimension set as upperDiff.
        SmallVector<Attribute, 8> upperDiffModified;
        for (unsigned iter = 0; iter < upperIndicesDiff.size(); ++iter) {
          int64_t v = (iter == upperDim) ? upperDiff : 0;
          upperDiffModified.push_back(b.getI32IntegerAttr(v));
        }
        assert(upperDiffModified.size() == upperIndicesDiff.size());

        // Apply map to compute index lower diff, from index upper diff using
        // constantFold.
        SmallVector<Attribute, 8> lowerDiffModifiedAttr;
        (void)transform.constantFold(upperDiffModified, lowerDiffModifiedAttr);
        assert(lowerDiffModifiedAttr.size() == lowerIndicesOriginal.size());

        for (unsigned iter = 0; iter < lowerDiffModifiedAttr.size(); ++iter) {
          lowerDiffModified.push_back(
              b.create<ConstantIndexOp>(loc, lowerDiffModifiedAttr[iter]
                                                 .template cast<IntegerAttr>()
                                                 .getInt()));
        }
        assert(lowerDiffModified.size() == lowerIndicesOriginal.size());
      } else {
        // In case upper level diff is not constant, use expandAffineMap.

        Value upperDiff = upperIndicesDiff[upperDim];

        // Populate an upper diff vector with all indices 0, other than
        // upperDim dimension set as upperDiff.
        SmallVector<Value, 8> upperDiffModified;
        for (unsigned iter = 0; iter < upperIndicesDiff.size(); ++iter) {
          Value v = (iter == upperDim) ? upperDiff : zeroConstantOp;
          upperDiffModified.push_back(v);
        }
        assert(upperDiffModified.size() == upperIndicesDiff.size());

        // Apply map to compute index lower diff, from index upper diff using
        // expandAffineMap.
        lowerDiffModified =
            expandAffineMap(b, loc, transform, upperDiffModified).getValue();
        ;
        assert(lowerDiffModified.size() == lowerIndicesOriginal.size());
      }

      // Obtain lower diffs prior to carry check.
      SmallVector<Value, 8> lowerDiffs;
      for (unsigned iter = 0; iter < q.size(); ++iter) {
        int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
        Value lowerDiff = lowerDiffModified[lowerDim];
        lowerDiffs.push_back(lowerDiff);
      }
      assert(lowerDiffs.size() == q.size());

      // Compute updated lower indices by adding original lower indices with
      // lower diffs.
      SmallVector<Value, 8> lowerIndicesModified;
      for (unsigned iter = 0; iter < q.size(); ++iter) {
        int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
        lowerIndicesModified.push_back(
            addToOriginal(lowerIndicesOriginal[lowerDim], lowerDiffs[iter]));
      }
      assert(lowerIndicesModified.size() == q.size());

      // Add carry check for Merge.
      // For Unfold it's not needed.
      if (transformation.getValue() == "Merge") {
        // Get lower layer bounds.
        SmallVector<int64_t, 8> lowerLayerBounds;
        for (unsigned iter = 0; iter < q.size(); ++iter) {
          int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
          int64_t v =
              lowerLayerShape[lowerDim].template cast<IntegerAttr>().getInt();
          lowerLayerBounds.push_back(v);
        }
        assert(lowerLayerBounds.size() == lowerIndicesModified.size());

        // Carry checked lower indices.
        // FIXME: study how to properly lowerDiffsCarryChecked.
        DenseMap<int64_t, Value> lowerDiffsCarryChecked;
        DenseMap<int64_t, Value> lowerIndicesCarryChecked;
        for (unsigned iter = 0; iter < q.size(); ++iter) {
          int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
          lowerDiffsCarryChecked[lowerDim] = lowerDiffs[iter];
          lowerIndicesCarryChecked[lowerDim] = lowerIndicesModified[iter];
        }
        assert(lowerDiffsCarryChecked.size() == lowerIndicesModified.size());
        assert(lowerIndicesCarryChecked.size() == lowerIndicesModified.size());

        // We only implement carry logic. Borrow logic would never happen as
        // upper index diffs would always be positive in the current algorithm.
        Value overflowOp = zeroConstantOp;
        for (ssize_t iter = q.size() - 1; iter >= 0; --iter) {
          int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
          int64_t upperBound = lowerLayerBounds[iter];
          // If the overflow is statically 0, nothing gets added
          Value diff =
              addToOriginal(lowerDiffsCarryChecked[lowerDim], overflowOp);
          Value index =
              addToOriginal(lowerIndicesCarryChecked[lowerDim], overflowOp);

          // Don't generate overflow for the uppermost dimension,
          // as this can lead to oob loads
          if (iter == 0) {
            lowerDiffsCarryChecked[lowerDim] = diff;
            lowerIndicesCarryChecked[lowerDim] = index;
            continue;
          }
          auto mbConstantDiff = isConstantValue(diff);
          auto mbConstantIndex = isConstantValue(index);

          // If we get lucky, everything is constant and so we have a constant
          // result
          if (mbConstantIndex.hasValue() && mbConstantDiff.hasValue()) {
            int64_t index = mbConstantIndex.getValue();
            int64_t diff = mbConstantDiff.getValue();
            if (index < upperBound) {
              overflowOp = zeroConstantOp;
              lowerIndicesCarryChecked[lowerDim] =
                  b.create<ConstantIndexOp>(loc, index);
              lowerDiffsCarryChecked[lowerDim] =
                  b.create<ConstantIndexOp>(loc, diff);
            } else {
              int64_t carry = index / upperBound;
              int64_t newIndex = index % upperBound;
              int64_t newDiff = diff - (carry * upperBound);
              overflowOp = b.create<ConstantIndexOp>(loc, carry);
              lowerIndicesCarryChecked[lowerDim] =
                  b.create<ConstantIndexOp>(loc, newIndex);
              lowerDiffsCarryChecked[lowerDim] =
                  b.create<ConstantIndexOp>(loc, newDiff);
            }
            continue;
          }
          // No change -> no carry-out
          if (mbConstantDiff.getValueOr(-1L) == 0) {
            overflowOp = zeroConstantOp;
            lowerDiffsCarryChecked[lowerDim] = diff;
            lowerIndicesCarryChecked[lowerDim] = index;
            continue;
          }

          Value upperBoundOp = b.create<ConstantIndexOp>(loc, upperBound);
          Value carry = b.create<DivUIOp>(loc, index, upperBoundOp);
          Value newIndex = b.create<RemUIOp>(loc, index, upperBoundOp);
          // If the merge is, as is typical, near the end of the transformations
          // this computation should get hit by the dead code eleminator
          Value newDiff = b.create<SubIOp>(
              loc, diff, b.create<MulIOp>(loc, carry, upperBoundOp));

          overflowOp = carry;
          lowerDiffsCarryChecked[lowerDim] = newDiff;
          lowerIndicesCarryChecked[lowerDim] = newIndex;
        }

        assert(lowerDiffsCarryChecked.size() == lowerIndicesModified.size());
        assert(lowerIndicesCarryChecked.size() == lowerIndicesModified.size());
        lowerDiffs.clear();
        lowerIndicesModified.clear();
        for (unsigned iter = 0; iter < q.size(); ++iter) {
          int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
          lowerDiffs.push_back(lowerDiffsCarryChecked[lowerDim]);
          lowerIndicesModified.push_back(lowerIndicesCarryChecked[lowerDim]);
        }
        assert(lowerDiffs.size() == q.size());
        assert(lowerIndicesModified.size() == q.size());
      }

      // Set lowerIndicesDiffMap and lowerIndicesUpdatedMap.
      for (unsigned iter = 0; iter < q.size(); ++iter) {
        int64_t lowerDim = q[iter].template cast<IntegerAttr>().getInt();
        lowerIndicesDiffMap[lowerDim] = lowerDiffs[iter];
        lowerIndicesUpdatedMap[lowerDim] = lowerIndicesModified[iter];
      }
    }
  } // for (auto &mapping : layoutAttr)

  // Convert lowerIndicesDiffMap to lowerIndicesDiff.
  assert(lowerIndicesDiffMap.size() == lowerLayerShape.size());
  for (unsigned iter = 0; iter < lowerLayerShape.size(); ++iter)
    lowerIndicesDiff.push_back(lowerIndicesDiffMap[iter]);

  // Convert lowerIndicesUpdatedMap to lowerIndicesUpdated.
  assert(lowerIndicesUpdatedMap.size() == lowerLayerShape.size());
  for (unsigned iter = 0; iter < lowerLayerShape.size(); ++iter)
    lowerIndicesUpdated.push_back(lowerIndicesUpdatedMap[iter]);
}

//===----------------------------------------------------------------------===//
// Utility function to progresseively apply index diff maps to compute the
// coordinate for the next layer.
//===----------------------------------------------------------------------===//
inline void populateLayeredIndicesWithIndexDiffMap(
    OpBuilder &b, Location loc, const ArrayAttr &layeredTransformMetadata,
    const SmallVector<AffineMap> &layeredTransform,
    const SmallVector<SmallVector<Value, 8>, 2> &layeredIndices,
    const SmallVector<Value, 8> &topDiff,
    SmallVector<SmallVector<Value, 8>, 2> &layeredDiffs,
    SmallVector<SmallVector<Value, 8>, 2> &layeredIndicesUpdated) {
  SmallVector<Value, 8> upperDiff = topDiff;
  // llvm::errs() << "\npopulateLayeredIndicesWithIndexDiffMap\n";
  // llvm::errs() << "layeredTransformMetadata: " << layeredTransformMetadata
  //              << "\n";
  // llvm::errs() << "layeredTransformMetadata.size():
  //              << layeredTransformMetadata.size() << "\n";
  // llvm::errs() << "layeredTransform.size(): " << layeredTransform.size()
  //              << "\n";
  // for (unsigned layer = 0; layer < layeredTransform.size(); ++layer) {
  //   llvm::errs() << "layeredTransform: " << layeredTransform[layer] << "\n";
  // }
  // llvm::errs() << "layeredIndices.size(): " << layeredIndices.size() << "\n";
  // llvm::errs() << "topDiff.size(): " << topDiff.size() << "\n";

  if (layeredTransform.size() == 0) {
    // In case there is no transform:
    // - lower level diff =  upper level diff
    // - lower level indices updated = lower level indices original + lower
    // level diff
    SmallVector<Value, 8> lowerDiff = upperDiff;
    SmallVector<Value, 8> lowerIndicesUpdated;
    assert(layeredIndices.size() == 1);
    SmallVector<Value, 8> lowerIndicesOriginal = layeredIndices[0];
    for (unsigned iter = 0; iter < lowerDiff.size(); ++iter)
      lowerIndicesUpdated.push_back(
          b.create<AddIOp>(loc, lowerIndicesOriginal[iter], lowerDiff[iter]));
    layeredDiffs.push_back(upperDiff);
    layeredIndicesUpdated.push_back(lowerIndicesUpdated);
  } else {
    // Use layeredTransformMetadata to count the layer.
    //
    // Why layeredTransform is not used here is because in some layers
    // identity map may be used and that would result in MLIR optimizing away
    // the map. layeredTransformMetadata has the most authentic number of
    // layers.
    //
    // For example, in Slice transformation where "begins" parameters are 0,
    // an identity map will be built.
    for (unsigned layer = 0; layer < layeredTransformMetadata.size(); ++layer) {
      SmallVector<Value, 8> lowerDiff;
      SmallVector<Value, 8> lowerIndicesUpdated;
      DictionaryAttr transformMetadata =
          layeredTransformMetadata[layer].template cast<DictionaryAttr>();
      SmallVector<Value, 8> lowerIndicesOriginal = layeredIndices[layer + 1];
      computeIndexDiffMap(b, loc, upperDiff, transformMetadata,
                          lowerIndicesOriginal, lowerDiff, lowerIndicesUpdated);
      layeredDiffs.push_back(lowerDiff);
      layeredIndicesUpdated.push_back(lowerIndicesUpdated);
      upperDiff.clear();
      upperDiff.append(lowerDiff);
    }
  }
}

//===----------------------------------------------------------------------===//
// Utility function to progressively use index diff map to compute the
// coordinate at the bottom most layer.
//===----------------------------------------------------------------------===//
inline void computeBottomIndicesWithIndexDiffMap(
    OpBuilder &b, Location loc,
    const SmallVector<int64_t, 8> &loopIVsPerAccessOrder,
    const ArrayAttr &layeredTransformMetadata,
    const SmallVector<AffineMap> &layeredTransform,
    const SmallVector<SmallVector<Value, 8>, 2> &layeredIndices,
    SmallVector<Value, 8> &bottomIndices) {
  // Coordinates across the layers of transformations.
  // If the vector is of size n, 0 is the top layer, and
  // n-1 is the bottom layer.
  SmallVector<SmallVector<Value, 8>, 2> layeredDiffs;
  SmallVector<SmallVector<Value, 8>, 2> layeredIndicesUpdated;

  SmallVector<Value, 8> topDiff;
  for (unsigned iter = 0; iter < loopIVsPerAccessOrder.size(); ++iter)
    topDiff.push_back(
        b.create<ConstantIndexOp>(loc, loopIVsPerAccessOrder[iter]));
  layeredDiffs.push_back(topDiff);
  // Progressively apply index diff maps across all coordinate
  // transformation layers.
  populateLayeredIndicesWithIndexDiffMap(
      b, loc, layeredTransformMetadata, layeredTransform, layeredIndices,
      topDiff, layeredDiffs, layeredIndicesUpdated);

  // Fetch bottom most layer coordinate.
  SmallVector<Value, 8> bottomIndicesUpdated =
      layeredIndicesUpdated[layeredIndicesUpdated.size() - 1];
  bottomIndices = bottomIndicesUpdated;
}

//===----------------------------------------------------------------------===//
// Utility function to repeatedly apply affine transformation to compute the
// coordinate for the next layer.
//===----------------------------------------------------------------------===//
inline void populateLayeredIndicesWithTransformMetadata(
    OpBuilder &b, Location loc,
    SmallVector<SmallVector<Value, 8>, 2> &layeredIndices,
    const SmallVector<Value, 8> &topIndices,
    const ArrayAttr &layeredTransformMetadata) {
  SmallVector<Value, 8> currentIndices = topIndices;
  layeredIndices.push_back(currentIndices);

  if (!layeredTransformMetadata) {
    // In case there is no metadata, simply return. The top layer indices have
    // recorded earlier.
    return;
  }
  // Go through each layer of transform metadata, fetch the map attribute
  // and apply it to obtain the indices for the next layer.
  for (unsigned layer = 0; layer < layeredTransformMetadata.size(); ++layer) {
    DictionaryAttr transformMetadata =
        layeredTransformMetadata[layer].template cast<DictionaryAttr>();
    AffineMap am = transformMetadata.get("map")
                       .template cast<ArrayAttr>()[0]
                       .template cast<AffineMapAttr>()
                       .getValue();
    SmallVector<Value, 8> nextLayerIndices =
        expandAffineMap(b, loc, am, currentIndices).getValue();

    layeredIndices.push_back(nextLayerIndices);

    currentIndices.clear();
    currentIndices = nextLayerIndices;
  }
}

//===----------------------------------------------------------------------===//
// Utility function to repeatedly apply affine transformation to compute the
// coordinate for the next layer.
//===----------------------------------------------------------------------===//
inline void populateLayeredIndicesWithAffineMap(
    OpBuilder &b, Location loc,
    SmallVector<SmallVector<Value, 8>, 2> &layeredIndices,
    const SmallVector<Value, 8> &topIndices,
    const SmallVector<AffineMap> &layeredTransform) {
  SmallVector<Value, 8> currentIndices = topIndices;
  layeredIndices.push_back(currentIndices);
  for (auto am : layeredTransform) {
    SmallVector<Value, 8> nextLayerIndices =
        expandAffineMap(b, loc, am, currentIndices).getValue();

    layeredIndices.push_back(nextLayerIndices);

    currentIndices.clear();
    currentIndices = nextLayerIndices;
  }
}

//===----------------------------------------------------------------------===//
// Utility function to compute the uppermost layer and bottommost layer
// coorindates using affine map.
//===----------------------------------------------------------------------===//
inline void computeTopAndBottomIndicesWithAffineMap(
    OpBuilder &b, Location &loc, SmallVector<Value, 8> &topIndices,
    SmallVector<Value, 8> &bottomIndices,
    const SmallVector<Value, 8> &originalCoords,
    const SmallVector<int64_t, 8> &loopIVsPerAccessOrder,
    const ArrayAttr &dimAccessOrder,
    const SmallVector<AffineMap> &layeredTransforms) {
  // Compute high-level coordinate.
  // index = (iv_0, iv_1, ...) + originalCoords
  topIndices.clear();
  topIndices.append(originalCoords);

  for (unsigned iter = 0; iter < loopIVsPerAccessOrder.size(); ++iter) {
    auto dim = dimAccessOrder[iter].template cast<IntegerAttr>().getInt();
    auto loopIV = b.create<ConstantIndexOp>(loc, loopIVsPerAccessOrder[dim]);
    topIndices[iter] = b.create<AddIOp>(loc, loopIV, topIndices[iter]);
  }

  // Populate coorindates across the layers of transformations.
  SmallVector<SmallVector<Value, 8>, 2> layeredIndices;
  populateLayeredIndicesWithAffineMap(b, loc, layeredIndices, topIndices,
                                      layeredTransforms);

  // Fetch low-level coordinate.
  bottomIndices = layeredIndices[layeredIndices.size() - 1];
}

//===----------------------------------------------------------------------===//
// Conv2D (forward, backward) lowering.
//===----------------------------------------------------------------------===//

template <typename T>
struct Conv2DRewritePattern : public OpRewritePattern<T> {
  const static ArgumentFields fields;
  const static miopen::ConvOpType convOpType;
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T op, PatternRewriter &b) const override {
    bool isXdlops = false;
    auto xdlopsV2Attr = op->template getAttrOfType<BoolAttr>("xdlopsV2");
    if (xdlopsV2Attr && xdlopsV2Attr.getValue() == true)
      isXdlops = true;
    auto dataType =
        op.input().getType().template cast<MemRefType>().getElementType();
    if (miopen::ConvOpType::Conv2DBwdDataOpType == convOpType) {
      return backwardData(op, b);
    }
    auto loc = op.getLoc();

    auto archAttr = op->template getAttrOfType<StringAttr>("arch");
    auto numCuAttr = op->template getAttrOfType<IntegerAttr>("num_cu");

    auto filterLayoutAttr =
        op->template getAttrOfType<ArrayAttr>("filter_layout");
    auto inputLayoutAttr =
        op->template getAttrOfType<ArrayAttr>("input_layout");
    auto outputLayoutAttr =
        op->template getAttrOfType<ArrayAttr>("output_layout");

    auto dilationsAttr = op->template getAttrOfType<ArrayAttr>("dilations");
    auto stridesAttr = op->template getAttrOfType<ArrayAttr>("strides");
    auto paddingAttr = op->template getAttrOfType<ArrayAttr>("padding");

    // Get shape of filter tensor.
    auto filterType = op.filter().getType().template cast<MemRefType>();
    auto filterShape = filterType.getShape();
    auto filterElementType = filterType.getElementType();

    // Get shape of input tensor.
    auto inputType = op.input().getType().template cast<MemRefType>();
    auto inputShape = inputType.getShape();
    auto inputElementType = inputType.getElementType();

    // Get shape of output tensor.
    auto outputType = op.output().getType().template cast<MemRefType>();
    auto outputShape = outputType.getShape();
    auto outputElementType = outputType.getElementType();

    // HO/WO dimension for output tensor.
    int64_t outputHDim = 0, outputWDim = 0;

    // Find Ho/Wo dimension for output tensor. They will be used in
    // transforming input tensor.
    for (unsigned i = 0; i < outputLayoutAttr.size(); ++i) {
      if (auto strAttr =
              outputLayoutAttr.getValue()[i].template cast<StringAttr>()) {
        if (strAttr.getValue() == "ho") {
          outputHDim = i;
        } else if (strAttr.getValue() == "wo") {
          outputWDim = i;
        }
      }
    }

    // Obtain convolution parameters: padding / dialtion / stride.
    auto leftPadH =
        paddingAttr.getValue()[0].template cast<IntegerAttr>().getInt();
    auto leftPadW =
        paddingAttr.getValue()[2].template cast<IntegerAttr>().getInt();
    auto rightPadH =
        paddingAttr.getValue()[1].template cast<IntegerAttr>().getInt();
    auto rightPadW =
        paddingAttr.getValue()[3].template cast<IntegerAttr>().getInt();

    auto dilationH =
        dilationsAttr.getValue()[0].template cast<IntegerAttr>().getInt();
    auto dilationW =
        dilationsAttr.getValue()[1].template cast<IntegerAttr>().getInt();
    auto strideH =
        stridesAttr.getValue()[0].template cast<IntegerAttr>().getInt();
    auto strideW =
        stridesAttr.getValue()[1].template cast<IntegerAttr>().getInt();

    // get y, x, ho, wo, hi, wi, k, c, n
    int64_t y, x, ho, wo, hi, wi, k, c, n;
    y = x = ho = wo = hi = wi = k = c = n = 0;
    llvm::DenseMap<StringRef, int> nameToDims;
    for (unsigned i = 0; i < filterLayoutAttr.size(); ++i) {
      auto filterAttr =
          filterLayoutAttr.getValue()[i].template cast<StringAttr>();
      auto inputAttr =
          inputLayoutAttr.getValue()[i].template cast<StringAttr>();
      auto outputAttr =
          outputLayoutAttr.getValue()[i].template cast<StringAttr>();

      nameToDims[filterAttr.getValue()] = i;
      nameToDims[inputAttr.getValue()] = i;
      nameToDims[outputAttr.getValue()] = i;

      if (filterAttr.getValue() == "y") {
        y = filterShape[i];
      } else if (filterAttr.getValue() == "x") {
        x = filterShape[i];
      } else if (filterAttr.getValue() == "k") {
        k = filterShape[i];
      } else if (filterAttr.getValue() == "c") {
        c = filterShape[i];
      }

      if (inputAttr.getValue() == "hi") {
        hi = inputShape[i];
      } else if (inputAttr.getValue() == "wi") {
        wi = inputShape[i];
      } else if (inputAttr.getValue() == "ni") {
        n = inputShape[i];
      }

      if (outputAttr.getValue() == "ho") {
        ho = outputShape[i];
      } else if (outputAttr.getValue() == "wo") {
        wo = outputShape[i];
      }
    }

    int64_t gemmM_size, gemmN_size, gemmK_size;
    int64_t gemmMExtra, gemmNExtra, gemmKExtra;
    gemmM_size = gemmN_size = gemmK_size = 0;
    gemmMExtra = gemmNExtra = gemmKExtra = 0;
    // compute we should use extra padding kernel or not
    // c,k already / g ,so we can skip / g here
    switch (convOpType) {
    case miopen::ConvOpType::Conv2DOpType:
      gemmM_size = k;
      gemmK_size = c * y * x;
      gemmN_size = n * ho * wo;
      break;
    case miopen::ConvOpType::Conv2DBwdDataOpType:
      gemmM_size = c;
      gemmK_size = k * y * x;
      gemmN_size = n * ho * wo;
      break;
    case miopen::ConvOpType::Conv2DBwdWeightOpType:
      gemmM_size = k;
      gemmK_size = n * ho * wo;
      gemmN_size = c * y * x;
      break;
    }

    bool needExtraPad = false;

    auto calculatePaddingKernelSize = [&needExtraPad, gemmM_size, gemmN_size,
                                       gemmK_size, &gemmMExtra, &gemmNExtra,
                                       &gemmKExtra](auto populateParams) {
      auto config_params = populateParams.getTuningParameters();
      unsigned numOfFailedConfigs = 0;
      for (auto &params : config_params) {
        if (gemmM_size % params.gemmMPerBlock == 0 &&
            gemmK_size % params.gemmKPerBlock == 0 &&
            gemmN_size % params.gemmNPerBlock == 0) {
          break;
        } else {
          numOfFailedConfigs++;
        }
      }

      auto extraParams = populateParams.getUniversalParameters();
      if (numOfFailedConfigs == config_params.size()) {
        needExtraPad = true;
        int gemmM_remain, gemmK_remain, gemmN_remain;

        gemmM_remain = gemmM_size % extraParams.gemmMPerBlock;
        if (gemmM_remain != 0)
          gemmMExtra = extraParams.gemmMPerBlock - gemmM_remain;

        gemmN_remain = gemmN_size % extraParams.gemmNPerBlock;
        if (gemmN_remain != 0)
          gemmNExtra = extraParams.gemmNPerBlock - gemmN_remain;

        gemmK_remain = gemmK_size % extraParams.gemmKPerBlock;
        if (gemmK_remain != 0)
          gemmKExtra = extraParams.gemmKPerBlock - gemmK_remain;

        // llvm::errs() << "gemmMExtra: " << gemmMExtra << "gemmNExtra: " <<
        // gemmNExtra << "gemmKExtra: " << gemmKExtra << "\n";
      }
    };

    if (!isXdlops) {
      PopulateParams populateParams;
      calculatePaddingKernelSize(populateParams);
    } else { // xdlops
      PopulateParamsXDL populateParamsXDL;
      calculatePaddingKernelSize(populateParamsXDL);
    }

    if (miopen::ConvOpType::Conv2DBwdWeightOpType == convOpType && isXdlops &&
        dataType == b.getF32Type() && needExtraPad == false) {
      // current backward weight with atomic_add can only run under xdlops +
      // fp32
      return backwardWeightAtomicAdd(op, b, fields);
    }
    // compute padding hi/wi.
    auto hiPadded = hi + leftPadH + rightPadH;
    auto wiPadded = wi + leftPadW + rightPadW;
    // Transform filter tensor.

    // Y/X dimension for filter tensor.
    int64_t filterYDim = 0, filterXDim = 0;

    llvm::SmallVector<int64_t, 2> transformedFilterShape;

    llvm::SmallVector<NamedAttribute, 3> transformedFilterAttrs;

    SmallString<5> arg0TargetLayoutName0("gemmG");
    SmallString<4> arg0TargetLayoutName1("gemm");
    arg0TargetLayoutName1.append(fields.gemmTargetCharName[0].substr(0, 1));
    SmallString<4> arg0TargetLayoutName2("gemm");
    arg0TargetLayoutName2.append(fields.gemmTargetCharName[0].substr(1, 1));

    // filter dims need oob check
    llvm::DenseSet<int> filterOobCheckDims;
    // set layout attribute.
    // Weight tensor transformation for Conv2DOp
    // - Part 1: Merge non-K dimensions to dimension 0, name it as gemmK.
    //           Optimization: If non-K dimensions are consequetive, apply
    //           unfold.
    // - Part 2: PassThrough K dimension to dimension 1, name it as gemmM.
    //
    // Weight tensor transformation for Conv2DBwdDataOp
    // - Part 1: Merge K dimensions to dimension 0, name it as gemmK.
    // - Part 2: PassThrough non-K dimension to dimension 1, name it as gemmM.
    //           Optimization: If non-K dimensions are consequetive, apply
    //           unfold.
    //
    // Weight tensor transformation for Conv2DBwdWeightOp
    // - Part 1: Merge non-K dimensions to dimension 1, name it as gemmN.
    // - Part 2: PassThrough K dimension to dimension 0, name it as gemmM.
    {
      llvm::SmallVector<IntegerAttr, 3> nonKDims;
      IntegerAttr kDim;
      IntegerAttr gDim;
      llvm::SmallVector<StringAttr, 3> nonKDimNames;
      StringAttr kDimName;
      StringAttr gDimName;

      for (unsigned i = 0; i < filterLayoutAttr.size(); ++i) {
        if (auto strAttr =
                filterLayoutAttr.getValue()[i].template cast<StringAttr>()) {
          if (strAttr.getValue() == "k") {
            kDim = b.getI32IntegerAttr(i);
            kDimName = strAttr;
          } else if (strAttr.getValue() == "g") {
            gDim = b.getI32IntegerAttr(i);
            gDimName = strAttr;
          } else {
            // Register filter Y/X dimension to be used later when transforming
            // input tensor.
            if (strAttr.getValue() == "y") {
              filterYDim = i;
            } else if (strAttr.getValue() == "x") {
              filterXDim = i;
            }
            nonKDims.push_back(b.getI32IntegerAttr(i));
            nonKDimNames.push_back(strAttr);
          }
        }
      }

      // Compute transformed filter shape dimension.
      int64_t nonKDimSize = 1;
      for (unsigned i = 0; i < filterShape.size(); ++i) {
        if (i != kDim.getInt() && i != gDim.getInt()) {
          nonKDimSize *= filterShape[i];
        }
      }
      transformedFilterShape.push_back(filterShape[gDim.getInt()]);
      transformedFilterShape.push_back(nonKDimSize);
      transformedFilterShape.push_back(filterShape[kDim.getInt()]);

      llvm::SmallVector<NamedAttribute, 2> sourceProbCYXDimAttr{
          b.getNamedAttr("lower_layer_dimensions",
                         b.getArrayAttr(ArrayRef<Attribute>(nonKDims.begin(),
                                                            nonKDims.end()))),
          b.getNamedAttr("lower_layer_names",
                         b.getArrayAttr(ArrayRef<Attribute>(
                             nonKDimNames.begin(), nonKDimNames.end())))};
      if (kDim.getInt() != 1 && kDim.getInt() != 4) {
        sourceProbCYXDimAttr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("Merge")));
      } else {
        sourceProbCYXDimAttr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("Unfold")));
      }

      llvm::SmallVector<NamedAttribute, 3> sourceProbGDimAttr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({gDim})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gDimName}))};

      llvm::SmallVector<NamedAttribute, 3> sourceProbKDimAttr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({kDim})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({kDimName}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemm0DimAttr{
          b.getNamedAttr("upper_layer_dimensions",
                         b.getArrayAttr({b.getI32IntegerAttr(0)})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({b.getStringAttr(
                                                  arg0TargetLayoutName0)}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemm1DimAttr{
          b.getNamedAttr("upper_layer_dimensions",
                         b.getArrayAttr({b.getI32IntegerAttr(1)})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({b.getStringAttr(
                                                  arg0TargetLayoutName1)}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemm2DimAttr{
          b.getNamedAttr("upper_layer_dimensions",
                         b.getArrayAttr({b.getI32IntegerAttr(2)})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({b.getStringAttr(
                                                  arg0TargetLayoutName2)}))};

      llvm::SmallVector<NamedAttribute, 0> layoutAttr0;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr1;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr2;

      if (convOpType == miopen::ConvOpType::Conv2DOpType) {
        layoutAttr0.append(targetGemm0DimAttr.begin(),
                           targetGemm0DimAttr.end());
        layoutAttr0.append(sourceProbGDimAttr.begin(),
                           sourceProbGDimAttr.end());
        layoutAttr1.append(targetGemm1DimAttr.begin(),
                           targetGemm1DimAttr.end());
        layoutAttr1.append(sourceProbCYXDimAttr.begin(),
                           sourceProbCYXDimAttr.end());
        layoutAttr2.append(targetGemm2DimAttr.begin(),
                           targetGemm2DimAttr.end());
        layoutAttr2.append(sourceProbKDimAttr.begin(),
                           sourceProbKDimAttr.end());
      } else {
        layoutAttr0.append(targetGemm0DimAttr.begin(),
                           targetGemm0DimAttr.end());
        layoutAttr0.append(sourceProbGDimAttr.begin(),
                           sourceProbGDimAttr.end());
        layoutAttr1.append(targetGemm1DimAttr.begin(),
                           targetGemm1DimAttr.end());
        layoutAttr1.append(sourceProbKDimAttr.begin(),
                           sourceProbKDimAttr.end());
        layoutAttr2.append(targetGemm2DimAttr.begin(),
                           targetGemm2DimAttr.end());
        layoutAttr2.append(sourceProbCYXDimAttr.begin(),
                           sourceProbCYXDimAttr.end());
      }

      transformedFilterAttrs.push_back(b.getNamedAttr(
          "layout", b.getArrayAttr({
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr0.begin(), layoutAttr0.end())}),

                        // Part 2: Passthrough part.
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr1.begin(), layoutAttr1.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr2.begin(), layoutAttr2.end())}),
                    })));
    }

    // set upper_layer_layout attribute.
    transformedFilterAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout",
        b.getArrayAttr({b.getStringAttr(arg0TargetLayoutName0),
                        b.getStringAttr(arg0TargetLayoutName1),
                        b.getStringAttr(arg0TargetLayoutName2)})));
    // set gridwise_gemm_argument_pos attribute.
    transformedFilterAttrs.push_back(b.getNamedAttr(
        "gridwise_gemm_argument_position",
        b.getI32IntegerAttr(fields.gridwiseGemmArgumentPosition[0])));

    // set gemmMExtra & gemmKExtra & gemmNExtra
    transformedFilterAttrs.push_back(
        b.getNamedAttr("gemmMExtra", b.getI32IntegerAttr(gemmMExtra)));
    transformedFilterAttrs.push_back(
        b.getNamedAttr("gemmKExtra", b.getI32IntegerAttr(gemmKExtra)));
    transformedFilterAttrs.push_back(
        b.getNamedAttr("gemmNExtra", b.getI32IntegerAttr(gemmNExtra)));
    // set needExtraPad
    transformedFilterAttrs.push_back(
        b.getNamedAttr("extraPad", b.getBoolAttr(needExtraPad)));

    // set lower_layer_layout attribute.
    transformedFilterAttrs.push_back(
        b.getNamedAttr("lower_layer_layout", filterLayoutAttr));

    auto transformedFilterMemRefType =
        MemRefType::get(transformedFilterShape, filterElementType);
    // set lowest_layer attribute.
    transformedFilterAttrs.push_back(
        b.getNamedAttr("lowest_layer", b.getBoolAttr(true)));
    auto gemmA = b.create<miopen::TransformOp>(
        loc, transformedFilterMemRefType, op.filter(), transformedFilterAttrs,
        /*populateBounds=*/true);

    auto gemmAPad = gemmA;
    bool isFilterPad = false;
    SmallString<8> gemmKPad_name("gemmKPad");
    SmallString<8> gemmMPad_name("gemmMPad");
    SmallString<8> gemmNPad_name("gemmNPad");

    int64_t nonGemmMSize = transformedFilterShape[1];
    int64_t gemmMSize = transformedFilterShape[2];
    // filter pad start
    // K:output channel, C:input channel,Y:filter height,X:filter width
    // filter dim : K & merge(C,Y,X) , if C*Y*X is under 64 or 32
    // we pad CYX to 32 or 64, then mlir can do gemm
    // we add more one transform to do pad
    bool filterCheckPadGemmM = false;
    bool filterCheckPadGemmK = false;
    bool filterCheckPadGemmN = false;
    filterCheckPadGemmM =
        (convOpType == miopen::ConvOpType::Conv2DOpType && gemmMExtra > 0) ||
        (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType &&
         gemmMExtra > 0);
    filterCheckPadGemmK =
        (convOpType == miopen::ConvOpType::Conv2DOpType && gemmKExtra > 0);
    filterCheckPadGemmN =
        (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType &&
         gemmNExtra > 0);
    if (filterCheckPadGemmM || filterCheckPadGemmK || filterCheckPadGemmN) {
      StringAttr gemmDim0TargetName = b.getStringAttr(arg0TargetLayoutName0);
      StringAttr gemmDim1TargetName;
      StringAttr gemmDim2TargetName;

      bool isGemmDim1Pad = false;
      bool isGemmDim2Pad = false;

      llvm::SmallVector<NamedAttribute, 3> paddingFilterAttrs;
      llvm::SmallVector<int64_t, 2> paddingFilterShape;

      llvm::SmallVector<NamedAttribute, 0> layoutAttr0;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr1;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr2;

      StringAttr gemmDim0Name = b.getStringAttr(arg0TargetLayoutName0);
      IntegerAttr GemmDim0 = b.getI32IntegerAttr(0);
      StringAttr gemmDim1Name = b.getStringAttr(arg0TargetLayoutName1);
      IntegerAttr GemmDim1 = b.getI32IntegerAttr(1);
      StringAttr gemmDim2Name = b.getStringAttr(arg0TargetLayoutName2);
      IntegerAttr GemmDim2 = b.getI32IntegerAttr(2);

      paddingFilterShape.push_back(transformedFilterShape[0]);
      paddingFilterShape.push_back(transformedFilterShape[1]);
      paddingFilterShape.push_back(transformedFilterShape[2]);

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim0Attr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim0})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim0Name}))};

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim1Attr{
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim1})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim1Name}))};

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim2Attr{
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim2})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim2Name}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim0Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim0})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({GemmDim0}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim1Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim1}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim2Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim2}))};

      // gemmdim0 is G, only pad gemmdim1 and gemmdim2
      if (filterCheckPadGemmK) {
        if (arg0TargetLayoutName1 == "gemmK") {
          isFilterPad = true;
          isGemmDim1Pad = true;
          gemmDim1TargetName = b.getStringAttr(gemmKPad_name);

          // forward
          paddingFilterShape[1] = nonGemmMSize + gemmKExtra;
          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("parameters", b.getArrayAttr({
                                               b.getI32IntegerAttr(0),
                                               b.getI32IntegerAttr(gemmKExtra),
                                           })));

          targetGemmDim1Attr.push_back(
              b.getNamedAttr("upper_layer_names",
                             b.getArrayAttr({b.getStringAttr(gemmKPad_name)})));
        }
        // filter of forward, gemmK=c*y*x
        if (filterYDim == 2) {
          // kyxc
          filterOobCheckDims.insert(nameToDims["y"]);
        } else {
          // kcyx
          filterOobCheckDims.insert(nameToDims["c"]);
        }
      }

      if (filterCheckPadGemmM) {
        if (arg0TargetLayoutName1 == "gemmM") {
          // backward weights
          isFilterPad = true;
          isGemmDim1Pad = true;
          gemmDim1TargetName = b.getStringAttr(gemmMPad_name);
          // even dim1 name is gemmM ,the size of dim 2 is gemmM
          paddingFilterShape[2] = gemmMSize + gemmMExtra;

          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("parameters", b.getArrayAttr({
                                               b.getI32IntegerAttr(0),
                                               b.getI32IntegerAttr(gemmMExtra),
                                           })));

          targetGemmDim1Attr.push_back(b.getNamedAttr(
              "names", b.getArrayAttr({b.getStringAttr(gemmMPad_name)})));
        } else if (arg0TargetLayoutName2 == "gemmM") {
          // forward
          isFilterPad = true;
          isGemmDim2Pad = true;
          gemmDim2TargetName = b.getStringAttr(gemmMPad_name);
          // gemmM = k when forward, pad gemmMExtra
          paddingFilterShape[2] = gemmMSize + gemmMExtra;
          // gemmM = k when forward
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("parameters", b.getArrayAttr({
                                               b.getI32IntegerAttr(0),
                                               b.getI32IntegerAttr(gemmMExtra),
                                           })));

          targetGemmDim2Attr.push_back(b.getNamedAttr(
              "names", b.getArrayAttr({b.getStringAttr(gemmMPad_name)})));
        }
        // filter of forward, gemmM=k
        filterOobCheckDims.insert(nameToDims["k"]);
      }

      if (filterCheckPadGemmN) {
        if (arg0TargetLayoutName2 == "gemmN") {
          // backward weights
          isFilterPad = true;
          isGemmDim2Pad = true;
          gemmDim2TargetName = b.getStringAttr(gemmNPad_name);
          // backward weights input: gemmK, gemmN
          // so padd gemmNExtra

          paddingFilterShape[1] = nonGemmMSize + gemmNExtra;
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("parameters", b.getArrayAttr({
                                               b.getI32IntegerAttr(0),
                                               b.getI32IntegerAttr(gemmNExtra),
                                           })));

          targetGemmDim2Attr.push_back(b.getNamedAttr(
              "names", b.getArrayAttr({b.getStringAttr(gemmNPad_name)})));
        }
        // FIXME: if we set every dim in merge transformation to store oob,
        // can't  pass verification, but only set top dim , it's ok
        if (filterYDim == 2) {
          // kyxc
          filterOobCheckDims.insert(nameToDims["y"]);
        } else {
          // kcyx
          filterOobCheckDims.insert(nameToDims["c"]);
        }
      }

      if (!isGemmDim1Pad) {
        gemmDim1TargetName = gemmDim1Name;
        sourceGemmDim1Attr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")));
        targetGemmDim1Attr.push_back(b.getNamedAttr(
            "upper_layer_names", b.getArrayAttr({gemmDim1Name})));
      } else if (!isGemmDim2Pad) {
        gemmDim2TargetName = gemmDim2Name;
        sourceGemmDim2Attr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")));
        targetGemmDim2Attr.push_back(b.getNamedAttr(
            "upper_layer_names", b.getArrayAttr({gemmDim2Name})));
      }

      layoutAttr0.append(targetGemmDim0Attr.begin(), targetGemmDim0Attr.end());
      layoutAttr0.append(sourceGemmDim0Attr.begin(), sourceGemmDim0Attr.end());
      layoutAttr1.append(targetGemmDim1Attr.begin(), targetGemmDim1Attr.end());
      layoutAttr1.append(sourceGemmDim1Attr.begin(), sourceGemmDim1Attr.end());
      layoutAttr2.append(targetGemmDim2Attr.begin(), targetGemmDim2Attr.end());
      layoutAttr2.append(sourceGemmDim2Attr.begin(), sourceGemmDim2Attr.end());

      paddingFilterAttrs.push_back(b.getNamedAttr(
          "layout", b.getArrayAttr({
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr0.begin(), layoutAttr0.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr1.begin(), layoutAttr1.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr2.begin(), layoutAttr2.end())}),
                    })));

      paddingFilterAttrs.push_back(
          b.getNamedAttr("upper_layer_layout",
                         b.getArrayAttr({gemmDim0TargetName, gemmDim1TargetName,
                                         gemmDim2TargetName})));

      paddingFilterAttrs.push_back(b.getNamedAttr(
          "lower_layer_layout",
          b.getArrayAttr({gemmDim0Name, gemmDim1Name, gemmDim2Name})));

      if (filterOobCheckDims.size()) {
        llvm::SmallVector<IntegerAttr, 5> boundDims;
        for (size_t i = 0; i < filterShape.size(); i++) {
          if (filterOobCheckDims.find(i) != filterOobCheckDims.end())
            boundDims.push_back(b.getI32IntegerAttr(1));
          else
            boundDims.push_back(b.getI32IntegerAttr(0));
        }
        paddingFilterAttrs.push_back(b.getNamedAttr(
            "bound_check",
            b.getArrayAttr({boundDims.begin(), boundDims.end()})));
      }
      auto paddingFilterMemRefType =
          MemRefType::get(paddingFilterShape, filterElementType);
      gemmAPad = b.create<miopen::TransformOp>(loc, paddingFilterMemRefType,
                                               gemmA, paddingFilterAttrs,
                                               /*populateBounds=*/true);
      // filter pad end
    }

    // Transform input tensor.
    // Input tensor step 1: padded input.
    llvm::SmallVector<int64_t, 5> paddedInputShape;

    llvm::SmallVector<NamedAttribute, 4> paddedInputAttrs;

    // reorderedPaddedInputDimNames would be used by the next stage.
    llvm::SmallVector<StringAttr, 5> reorderedPaddedInputDimNames;

    // input dims need oob check
    llvm::DenseSet<int> inputOobCheckDims;
    // set layout attribute.
    // Padded input tensor transformation:
    // - Part 1: PassThrough ni dimension to its original dimension, name it as
    // ni.
    // - Part 2: PassThrough ci dimension to its original dimension, name it as
    // ci.
    // - Part 3: Pad hi/wi dimensions to their original dimensions, name it as
    // hipad/wipad.
    {
      IntegerAttr nDim, cDim, gDim;
      StringAttr nDimName, cDimName, gDimName;
      llvm::SmallVector<IntegerAttr, 2> hwDims;
      llvm::SmallVector<StringAttr, 2> hwDimNames;
      for (unsigned i = 0; i < inputLayoutAttr.size(); ++i) {
        if (auto strAttr =
                inputLayoutAttr.getValue()[i].template cast<StringAttr>()) {
          if (strAttr.getValue() == "ni") {
            nDim = b.getI32IntegerAttr(i);
            nDimName = strAttr;
          } else if (strAttr.getValue() == "gi") {
            gDim = b.getI32IntegerAttr(i);
            gDimName = strAttr;
          } else if (strAttr.getValue() == "ci") {
            cDim = b.getI32IntegerAttr(i);
            cDimName = strAttr;
          } else {
            hwDims.push_back(b.getI32IntegerAttr(i));
            hwDimNames.push_back(strAttr);
          }
        }
      }

      llvm::SmallVector<StringAttr, 2> hwPaddedDimNames;
      for (auto strAttr : hwDimNames) {
        hwPaddedDimNames.push_back(
            b.getStringAttr((strAttr.getValue() + "pad").str()));
      }

      for (unsigned i = 0, j = 0; i < inputLayoutAttr.size(); ++i) {
        if (APInt(32, i) == nDim.getValue()) {
          reorderedPaddedInputDimNames.push_back(nDimName);
          paddedInputShape.push_back(inputShape[nDim.getInt()]);
        } else if (APInt(32, i) == gDim.getValue()) {
          reorderedPaddedInputDimNames.push_back(gDimName);
          paddedInputShape.push_back(inputShape[gDim.getInt()]);
        } else if (APInt(32, i) == cDim.getValue()) {
          reorderedPaddedInputDimNames.push_back(cDimName);
          paddedInputShape.push_back(inputShape[cDim.getInt()]);
        } else {
          // Set padded dimension.
          auto strAttr =
              inputLayoutAttr.getValue()[i].template cast<StringAttr>();
          if (strAttr.getValue() == "hi") {
            paddedInputShape.push_back(hiPadded);
          } else if (strAttr.getValue() == "wi") {
            paddedInputShape.push_back(wiPadded);
          }

          reorderedPaddedInputDimNames.push_back(hwPaddedDimNames[j++]);
        }
      }

      paddedInputAttrs.push_back(b.getNamedAttr(
          "layout",
          b.getArrayAttr({
              // Part 0: Passthrough for gi dimension.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({gDim})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({gDimName})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({gDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({gDimName})),
              }),
              // Part 1: Passthrough for ni dimension.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({nDim})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({nDimName})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({nDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({nDimName})),
              }),

              // Part 2: Passthrough for ci dimension.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({cDim})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({cDimName})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({cDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({cDimName})),
              }),

              // Part 3: Pad for h/w dimensions.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr(ArrayRef<Attribute>(
                                     hwDims.begin(), hwDims.end()))),
                  b.getNamedAttr(
                      "upper_layer_names",
                      b.getArrayAttr(ArrayRef<Attribute>(
                          hwPaddedDimNames.begin(), hwPaddedDimNames.end()))),
                  b.getNamedAttr("transformation", b.getStringAttr("Pad")),
                  b.getNamedAttr("parameters",
                                 b.getArrayAttr({
                                     b.getI32IntegerAttr(leftPadH),
                                     b.getI32IntegerAttr(rightPadH),
                                     b.getI32IntegerAttr(leftPadW),
                                     b.getI32IntegerAttr(rightPadW),
                                 })),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr(ArrayRef<Attribute>(
                                     hwDims.begin(), hwDims.end()))),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr(ArrayRef<Attribute>(
                                     hwDimNames.begin(), hwDimNames.end()))),
              }),
          })));
      if (leftPadH || rightPadH) {
        inputOobCheckDims.insert(nameToDims["hi"]);
      }
      if (leftPadW || rightPadW) {
        inputOobCheckDims.insert(nameToDims["wi"]);
      }
    }
    // set lower_layer_layout attribute.
    paddedInputAttrs.push_back(
        b.getNamedAttr("lower_layer_layout", inputLayoutAttr));
    // set upper_layer_layout attribute.
    paddedInputAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout", b.getArrayAttr(ArrayRef<Attribute>(
                                  reorderedPaddedInputDimNames.begin(),
                                  reorderedPaddedInputDimNames.end()))));

    // set gemmKExtra & gemmNExtra & gemmNExtra
    paddedInputAttrs.push_back(
        b.getNamedAttr("gemmKExtra", b.getI32IntegerAttr(gemmKExtra)));
    paddedInputAttrs.push_back(
        b.getNamedAttr("gemmNExtra", b.getI32IntegerAttr(gemmNExtra)));
    paddedInputAttrs.push_back(
        b.getNamedAttr("gemmMExtra", b.getI32IntegerAttr(gemmMExtra)));
    // set needExtraPad
    paddedInputAttrs.push_back(
        b.getNamedAttr("extraPad", b.getBoolAttr(needExtraPad)));

    auto paddedInputMemRefType =
        MemRefType::get(paddedInputShape, inputElementType);
    // set lowest_layer attribute.
    paddedInputAttrs.push_back(
        b.getNamedAttr("lowest_layer", b.getBoolAttr(true)));
    Value paddedInput = b.create<miopen::TransformOp>(
        loc, paddedInputMemRefType, op.input(), paddedInputAttrs,
        /*populateBounds=*/true);

    // Input tensor step 2 : embedded input.
    llvm::SmallVector<int64_t, 7> embeddedInputShape;

    llvm::SmallVector<NamedAttribute, 4> embeddedInputAttrs;

    // reorderedEmbeddedInputDimNames would be used by the next stage.
    llvm::SmallVector<StringAttr, 7> reorderedEmbeddedInputDimNames;

    // Embedded input tensor transformation:
    // - Part 1: PassThrough ni dimension to its original dimension, name it as
    // ni.
    // - Part 2: PassThrough ci dimension to its original dimension, name it as
    // ci.
    // - Part 3: Embed hipad dimension to 2 dimensions, name them as: y, ho.
    // - Part 4: Embed wipad dimension to 2 dimensions, name them as: x, wo.
    {
      IntegerAttr nDim, cDim, gDim;
      StringAttr nDimName, cDimName, gDimName;
      IntegerAttr hDim, wDim;
      StringAttr hDimName, wDimName;
      // reorder dimensions from 4 to 6.
      // ex: (ni, ci, hipad, wipad) -> (ni, ci, y, ho, x, wo).
      IntegerAttr reorderedNDim, reorderedCDim, reorderedGDim;
      llvm::SmallVector<IntegerAttr, 2> reorderedYHoDim;
      llvm::SmallVector<IntegerAttr, 2> reorderedXWoDim;
      unsigned dimCtr = 0;
      for (unsigned i = 0; i < reorderedPaddedInputDimNames.size(); ++i) {
        auto strAttr = reorderedPaddedInputDimNames[i];
        if (strAttr.getValue() == "gi") {
          gDim = b.getI32IntegerAttr(i);
          gDimName = strAttr;

          reorderedGDim = b.getI32IntegerAttr(dimCtr++);

          reorderedEmbeddedInputDimNames.push_back(strAttr);

          embeddedInputShape.push_back(inputShape[gDim.getInt()]);
        } else if (strAttr.getValue() == "ni") {
          nDim = b.getI32IntegerAttr(i);
          nDimName = strAttr;

          reorderedNDim = b.getI32IntegerAttr(dimCtr++);

          reorderedEmbeddedInputDimNames.push_back(strAttr);

          embeddedInputShape.push_back(inputShape[nDim.getInt()]);
        } else if (strAttr.getValue() == "ci") {
          cDim = b.getI32IntegerAttr(i);
          cDimName = strAttr;

          reorderedCDim = b.getI32IntegerAttr(dimCtr++);

          reorderedEmbeddedInputDimNames.push_back(strAttr);

          embeddedInputShape.push_back(inputShape[cDim.getInt()]);
        } else if (strAttr.getValue() == "hipad") {
          hDim = b.getI32IntegerAttr(i);
          hDimName = strAttr;

          reorderedYHoDim.push_back(b.getI32IntegerAttr(dimCtr++));
          reorderedYHoDim.push_back(b.getI32IntegerAttr(dimCtr++));

          reorderedEmbeddedInputDimNames.push_back(b.getStringAttr("y"));
          reorderedEmbeddedInputDimNames.push_back(b.getStringAttr("ho"));

          embeddedInputShape.push_back(filterShape[filterYDim]);
          embeddedInputShape.push_back(outputShape[outputHDim]);
        } else if (strAttr.getValue() == "wipad") {
          wDim = b.getI32IntegerAttr(i);
          wDimName = strAttr;

          reorderedXWoDim.push_back(b.getI32IntegerAttr(dimCtr++));
          reorderedXWoDim.push_back(b.getI32IntegerAttr(dimCtr++));

          reorderedEmbeddedInputDimNames.push_back(b.getStringAttr("x"));
          reorderedEmbeddedInputDimNames.push_back(b.getStringAttr("wo"));

          embeddedInputShape.push_back(filterShape[filterXDim]);
          embeddedInputShape.push_back(outputShape[outputWDim]);
        }
      }

      embeddedInputAttrs.push_back(b.getNamedAttr(
          "layout",
          b.getArrayAttr({
              // Part 0: Passthrough for gi dimension.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({reorderedGDim})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({gDimName})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({gDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({gDimName})),
              }),

              // Part 1: Passthrough for ni dimension.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({reorderedNDim})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({nDimName})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({nDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({nDimName})),
              }),

              // Part 2: Passthrough for ci dimension.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({reorderedCDim})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({cDimName})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({cDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({cDimName})),
              }),

              // Part 3: Embed for y, ho dimensions.
              b.getDictionaryAttr({
                  b.getNamedAttr(
                      "upper_layer_dimensions",
                      b.getArrayAttr(ArrayRef<Attribute>(
                          reorderedYHoDim.begin(), reorderedYHoDim.end()))),
                  b.getNamedAttr("upper_layer_names", b.getArrayAttr({
                                                          b.getStringAttr("y"),
                                                          b.getStringAttr("ho"),
                                                      })),
                  b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                  // Embed parmeters.
                  // 0: dilationH
                  // 1: strideH
                  b.getNamedAttr("parameters",
                                 b.getArrayAttr({
                                     b.getI32IntegerAttr(dilationH),
                                     b.getI32IntegerAttr(strideH),
                                 })),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({hDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({hDimName})),
              }),

              // Part 4: Embed for x, wo dimensions.
              b.getDictionaryAttr({
                  b.getNamedAttr(
                      "upper_layer_dimensions",
                      b.getArrayAttr(ArrayRef<Attribute>(
                          reorderedXWoDim.begin(), reorderedXWoDim.end()))),
                  b.getNamedAttr("upper_layer_names", b.getArrayAttr({
                                                          b.getStringAttr("x"),
                                                          b.getStringAttr("wo"),
                                                      })),
                  b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                  // Embed parmeters.
                  // 0: dilationW
                  // 1: strideW
                  b.getNamedAttr("parameters",
                                 b.getArrayAttr({
                                     b.getI32IntegerAttr(dilationW),
                                     b.getI32IntegerAttr(strideW),
                                 })),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({wDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({wDimName})),
              }),
          })));
    }
    // set lower_layer_layout attribute.
    embeddedInputAttrs.push_back(b.getNamedAttr(
        "lower_layer_layout", b.getArrayAttr(ArrayRef<Attribute>(
                                  reorderedPaddedInputDimNames.begin(),
                                  reorderedPaddedInputDimNames.end()))));
    // set upper_layer_layout attribute.
    embeddedInputAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout", b.getArrayAttr(ArrayRef<Attribute>(
                                  reorderedEmbeddedInputDimNames.begin(),
                                  reorderedEmbeddedInputDimNames.end()))));
    auto embeddedInputMemRefType =
        MemRefType::get(embeddedInputShape, inputElementType);
    auto embeddedInput = b.create<miopen::TransformOp>(
        loc, embeddedInputMemRefType, paddedInput, embeddedInputAttrs,
        /*populateBounds=*/true);

    // Input tensor step 3: transformed input.
    llvm::SmallVector<int64_t, 3> transformedInputShape;

    llvm::SmallVector<NamedAttribute, 3> transformedInputAttrs;

    SmallString<5> arg1TargetLayoutName0("gemmG");
    SmallString<4> arg1TargetLayoutName1("gemm");
    arg1TargetLayoutName1.append(fields.gemmTargetCharName[1].substr(0, 1));
    SmallString<4> arg1TargetLayoutName2("gemm");
    arg1TargetLayoutName2.append(fields.gemmTargetCharName[1].substr(1, 1));

    // set layout attribute.
    // Transformed input tensor transformation:
    // - Part 1: Merge ci, y, x dimensions to dimension 0, name it as gemmK.
    // - Part 2: Merge ni, ho, wo dimensions to dimension 1, name it as gemmN.
    //
    // input tensor transformation for Conv2DBwdWeightOp
    // - Part 1: Merge ni, ho, wo dimensions to dimension 0, name it as gemmK.
    // - Part 2: Merge ci, y, x dimensions to dimension 1, name it as gemmN.
    {
      IntegerAttr nDim, cDim, gDim;
      StringAttr nDimName, cDimName, gDimName;
      IntegerAttr hDim, wDim;
      StringAttr hDimName, wDimName;
      IntegerAttr yDim, xDim;
      StringAttr yDimName, xDimName;
      // reorder dimensions from 6 to 2.
      // ex: (ni, ci, y, ho, x, wo) -> ((ci, y, x), (ni, ho, wo)).
      for (unsigned i = 0; i < reorderedEmbeddedInputDimNames.size(); ++i) {
        auto strAttr = reorderedEmbeddedInputDimNames[i];
        if (strAttr.getValue() == "gi") {
          gDim = b.getI32IntegerAttr(i);
          gDimName = strAttr;
        } else if (strAttr.getValue() == "ni") {
          nDim = b.getI32IntegerAttr(i);
          nDimName = strAttr;
        } else if (strAttr.getValue() == "ci") {
          cDim = b.getI32IntegerAttr(i);
          cDimName = strAttr;
        } else if (strAttr.getValue() == "ho") {
          hDim = b.getI32IntegerAttr(i);
          hDimName = strAttr;
        } else if (strAttr.getValue() == "wo") {
          wDim = b.getI32IntegerAttr(i);
          wDimName = strAttr;
        } else if (strAttr.getValue() == "y") {
          yDim = b.getI32IntegerAttr(i);
          yDimName = strAttr;
        } else if (strAttr.getValue() == "x") {
          xDim = b.getI32IntegerAttr(i);
          xDimName = strAttr;
        }
      }

      llvm::SmallVector<StringAttr, 3> mergedPart1DimNames;
      llvm::SmallVector<IntegerAttr, 3> mergedPart1Dims;

      if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
	      // Assume hDim is always less than wDim.
	      if (nDim.getInt() < hDim.getInt()) {
		      mergedPart1DimNames.push_back(nDimName);
		      mergedPart1DimNames.push_back(hDimName);
		      mergedPart1DimNames.push_back(wDimName);
		      mergedPart1Dims.push_back(nDim);
		      mergedPart1Dims.push_back(hDim);
		      mergedPart1Dims.push_back(wDim);
	      } else {
		      mergedPart1DimNames.push_back(hDimName);
		      mergedPart1DimNames.push_back(wDimName);
		      mergedPart1DimNames.push_back(nDimName);
		      mergedPart1Dims.push_back(hDim);
		      mergedPart1Dims.push_back(wDim);
		      mergedPart1Dims.push_back(nDim);
	      }
      } else {
	      // Assume yDim is always less than xDim.
	      if (cDim.getInt() < yDim.getInt()) {
		      mergedPart1DimNames.push_back(cDimName);
		      mergedPart1DimNames.push_back(yDimName);
		      mergedPart1DimNames.push_back(xDimName);
		      mergedPart1Dims.push_back(cDim);
		      mergedPart1Dims.push_back(yDim);
		      mergedPart1Dims.push_back(xDim);
	      } else {
		      mergedPart1DimNames.push_back(yDimName);
		      mergedPart1DimNames.push_back(xDimName);
		      mergedPart1DimNames.push_back(cDimName);
		      mergedPart1Dims.push_back(yDim);
		      mergedPart1Dims.push_back(xDim);
		      mergedPart1Dims.push_back(cDim);
	      }
      }

      llvm::SmallVector<StringAttr, 3> mergedPart2DimNames;
      llvm::SmallVector<IntegerAttr, 3> mergedPart2Dims;

      if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
	      // Assume yDim is always less than xDim.
	      if (cDim.getInt() < yDim.getInt()) {
		      mergedPart2DimNames.push_back(cDimName);
		      mergedPart2DimNames.push_back(yDimName);
		      mergedPart2DimNames.push_back(xDimName);
		      mergedPart2Dims.push_back(cDim);
		      mergedPart2Dims.push_back(yDim);
		      mergedPart2Dims.push_back(xDim);
	      } else {
		      mergedPart2DimNames.push_back(yDimName);
		      mergedPart2DimNames.push_back(xDimName);
		      mergedPart2DimNames.push_back(cDimName);
		      mergedPart2Dims.push_back(yDim);
		      mergedPart2Dims.push_back(xDim);
		      mergedPart2Dims.push_back(cDim);
	      }
      } else {
	      // Assume hDim is always less than wDim.
	      if (nDim.getInt() < hDim.getInt()) {
		      mergedPart2DimNames.push_back(nDimName);
		      mergedPart2DimNames.push_back(hDimName);
		      mergedPart2DimNames.push_back(wDimName);
		      mergedPart2Dims.push_back(nDim);
		      mergedPart2Dims.push_back(hDim);
		      mergedPart2Dims.push_back(wDim);
	      } else {
		      mergedPart2DimNames.push_back(hDimName);
		      mergedPart2DimNames.push_back(wDimName);
		      mergedPart2DimNames.push_back(nDimName);
		      mergedPart2Dims.push_back(hDim);
		      mergedPart2Dims.push_back(wDim);
		      mergedPart2Dims.push_back(nDim);
	      }
      }

      if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
        transformedInputShape.push_back(embeddedInputShape[gDim.getInt()]);
        transformedInputShape.push_back(embeddedInputShape[hDim.getInt()] *
                                        embeddedInputShape[wDim.getInt()] *
                                        embeddedInputShape[nDim.getInt()]);
        transformedInputShape.push_back(embeddedInputShape[cDim.getInt()] *
                                        embeddedInputShape[yDim.getInt()] *
                                        embeddedInputShape[xDim.getInt()]);
      } else {
        transformedInputShape.push_back(embeddedInputShape[gDim.getInt()]);
        transformedInputShape.push_back(embeddedInputShape[cDim.getInt()] *
                                        embeddedInputShape[yDim.getInt()] *
                                        embeddedInputShape[xDim.getInt()]);
        transformedInputShape.push_back(embeddedInputShape[hDim.getInt()] *
                                        embeddedInputShape[wDim.getInt()] *
                                        embeddedInputShape[nDim.getInt()]);
      }

      transformedInputAttrs.push_back(b.getNamedAttr(
          "layout",
          b.getArrayAttr({
              // Part 0: g dimensions.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(0)})),
                  b.getNamedAttr(
                      "upper_layer_names",
                      b.getArrayAttr({b.getStringAttr(arg1TargetLayoutName0)})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough")),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({gDim})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({gDimName})),
              }),
              // Part 1: Merge ci, y, x dimensions.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(1)})),
                  b.getNamedAttr(
                      "upper_layer_names",
                      b.getArrayAttr({b.getStringAttr(arg1TargetLayoutName1)})),
                  b.getNamedAttr("transformation", b.getStringAttr("Merge")),
                  b.getNamedAttr(
                      "lower_layer_dimensions",
                      b.getArrayAttr(ArrayRef<Attribute>(
                          mergedPart1Dims.begin(), mergedPart1Dims.end()))),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr(ArrayRef<Attribute>(
                                     mergedPart1DimNames.begin(),
                                     mergedPart1DimNames.end()))),
              }),

              // Part 2: Merge ni, ho, wo dimensions.
              b.getDictionaryAttr({
                  b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(2)})),
                  b.getNamedAttr(
                      "upper_layer_names",
                      b.getArrayAttr({b.getStringAttr(arg1TargetLayoutName2)})),
                  b.getNamedAttr("transformation", b.getStringAttr("Merge")),
                  b.getNamedAttr(
                      "lower_layer_dimensions",
                      b.getArrayAttr(ArrayRef<Attribute>(
                          mergedPart2Dims.begin(), mergedPart2Dims.end()))),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr(ArrayRef<Attribute>(
                                     mergedPart2DimNames.begin(),
                                     mergedPart2DimNames.end()))),
              }),
          })));
    }
    // set lower_layer_layout attribute.
    transformedInputAttrs.push_back(b.getNamedAttr(
        "lower_layer_layout", b.getArrayAttr(ArrayRef<Attribute>(
                                  reorderedEmbeddedInputDimNames.begin(),
                                  reorderedEmbeddedInputDimNames.end()))));
    // set upper_layer_layout attribute.
    transformedInputAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout",
        b.getArrayAttr({b.getStringAttr(arg1TargetLayoutName0),
                        b.getStringAttr(arg1TargetLayoutName1),
                        b.getStringAttr(arg1TargetLayoutName2)})));

    if (inputOobCheckDims.size()) {
      llvm::SmallVector<IntegerAttr, 5> boundDims;
      for (size_t i = 0; i < inputShape.size(); i++) {
        if (inputOobCheckDims.find(i) != inputOobCheckDims.end())
          boundDims.push_back(b.getI32IntegerAttr(1));
        else
          boundDims.push_back(b.getI32IntegerAttr(0));
      }
      transformedInputAttrs.push_back(b.getNamedAttr(
          "bound_check", b.getArrayAttr({boundDims.begin(), boundDims.end()})));
    }
    // set gridwise_gemm_argument_pos attribute.
    transformedInputAttrs.push_back(b.getNamedAttr(
        "gridwise_gemm_argument_position",
        b.getI32IntegerAttr(fields.gridwiseGemmArgumentPosition[1])));
    auto transformedInputMemRefType =
        MemRefType::get(transformedInputShape, inputElementType);
    auto gemmB = b.create<miopen::TransformOp>(
        loc, transformedInputMemRefType, embeddedInput, transformedInputAttrs,
        /*populateBounds=*/true);

    auto gemmBPad = gemmB;
    bool isInputPad = false;
    // input padding start
    // input : NHW & CRS , if CRS is under 64 or 32
    // we pad CRS to 32 or 64, then mlir can do gemm
    // we add more one transform to do pad

    // input forward : gemmK,gemmN
    // backward weights: gemmK,gemmN
    // so we don't need to pad gemmK
    bool inputCheckPadGemmK = false;
    bool inputCheckPadGemmN = false;
    inputCheckPadGemmK =
        (convOpType == miopen::ConvOpType::Conv2DOpType && gemmKExtra > 0) ||
        (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType &&
         gemmKExtra > 0);
    inputCheckPadGemmN =
        (convOpType == miopen::ConvOpType::Conv2DOpType && gemmNExtra > 0) ||
        (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType &&
         gemmNExtra > 0);
    if (inputCheckPadGemmK || inputCheckPadGemmN) {
      llvm::SmallVector<int64_t, 3> paddingInputShape;
      llvm::SmallVector<NamedAttribute, 3> paddingInputAttrs;

      llvm::SmallVector<NamedAttribute, 0> layoutAttr0;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr1;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr2;

      StringAttr gemmDim0TargetName = b.getStringAttr(arg1TargetLayoutName0);
      StringAttr gemmDim1TargetName;
      StringAttr gemmDim2TargetName;

      bool isGemmDim1Pad = false;
      bool isGemmDim2Pad = false;

      StringAttr gemmDim0Name = b.getStringAttr(arg1TargetLayoutName0);
      IntegerAttr GemmDim0 = b.getI32IntegerAttr(0);
      StringAttr gemmDim1Name = b.getStringAttr(arg1TargetLayoutName1);
      IntegerAttr GemmDim1 = b.getI32IntegerAttr(1);
      StringAttr gemmDim2Name = b.getStringAttr(arg1TargetLayoutName2);
      IntegerAttr GemmDim2 = b.getI32IntegerAttr(2);

      paddingInputShape.push_back(transformedInputShape[0]);
      paddingInputShape.push_back(transformedInputShape[1]);
      paddingInputShape.push_back(transformedInputShape[2]);

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim0Attr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim0})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim0Name}))};

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim1Attr{
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim1})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim1Name}))};

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim2Attr{
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim2})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim2Name}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim0Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim0})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({gemmDim0Name}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim1Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim1}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim2Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim2}))};

      if (inputCheckPadGemmK) {
        if (arg1TargetLayoutName1 == "gemmK") {
          isInputPad = true;
          isGemmDim1Pad = true;
          // both forward and backward weights dim1 of input matrix
          // are gemmK ,but forward gemmK is combining  c,y,x
          // backward weights gemmK is combining  n,h,w
          gemmDim1TargetName = b.getStringAttr(gemmKPad_name);
          paddingInputShape[1] = paddingInputShape[1] + gemmKExtra;

          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim1Attr.push_back(b.getNamedAttr(
              "parameters", b.getArrayAttr({b.getI32IntegerAttr(0),
                                            b.getI32IntegerAttr(gemmKExtra)})));
          targetGemmDim1Attr.push_back(
              b.getNamedAttr("upper_layer_names",
                             b.getArrayAttr({b.getStringAttr(gemmKPad_name)})));

          // input gemmK fwd: CYX   backward weights:NHW
          // due to it's load , we can use whole dim in gemmK
          // if it's store , use top one
          if (convOpType == miopen::ConvOpType::Conv2DOpType) {
            inputOobCheckDims.insert(nameToDims["ci"]);
          } else if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
            inputOobCheckDims.insert(nameToDims["ni"]);
          }

          inputOobCheckDims.insert(nameToDims["hi"]);
          inputOobCheckDims.insert(nameToDims["wi"]);
        }
      }

      if (inputCheckPadGemmN) {
        if (arg1TargetLayoutName2 == "gemmN") {
          isInputPad = true;
          isGemmDim2Pad = true;
          gemmDim2TargetName = b.getStringAttr(gemmNPad_name);
          // both forward and backward weights have the same dim2 gemmN
          // so padding gemmNExtra

          paddingInputShape[2] = paddingInputShape[2] + gemmNExtra;
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim2Attr.push_back(b.getNamedAttr(
              "parameters", b.getArrayAttr({b.getI32IntegerAttr(0),
                                            b.getI32IntegerAttr(gemmNExtra)})));
          targetGemmDim2Attr.push_back(
              b.getNamedAttr("upper_layer_names",
                             b.getArrayAttr({b.getStringAttr(gemmNPad_name)})));

          // input forward  gemmN: n,h,w
          // backward weights gemmN :C,Y,X
          if (convOpType == miopen::ConvOpType::Conv2DOpType) {
            inputOobCheckDims.insert(nameToDims["ni"]);
            inputOobCheckDims.insert(nameToDims["hi"]);
            inputOobCheckDims.insert(nameToDims["wi"]);
          } else if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
            inputOobCheckDims.insert(nameToDims["ci"]);
            inputOobCheckDims.insert(nameToDims["hi"]);
            inputOobCheckDims.insert(nameToDims["wi"]);
          }
        }
      }

      // gemmdim0 is G, only pad gemmdim1 and gemmdim2
      if (!isGemmDim1Pad) {
        gemmDim1TargetName = gemmDim1Name;
        sourceGemmDim1Attr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")));
        targetGemmDim1Attr.push_back(b.getNamedAttr(
            "upper_layer_names", b.getArrayAttr({gemmDim1Name})));
      } else if (!isGemmDim2Pad) {
        gemmDim2TargetName = gemmDim2Name;
        sourceGemmDim2Attr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")));
        targetGemmDim2Attr.push_back(b.getNamedAttr(
            "upper_layer_names", b.getArrayAttr({gemmDim2Name})));
      }

      layoutAttr0.append(targetGemmDim0Attr.begin(), targetGemmDim0Attr.end());
      layoutAttr0.append(sourceGemmDim0Attr.begin(), sourceGemmDim0Attr.end());
      layoutAttr1.append(targetGemmDim1Attr.begin(), targetGemmDim1Attr.end());
      layoutAttr1.append(sourceGemmDim1Attr.begin(), sourceGemmDim1Attr.end());
      layoutAttr2.append(targetGemmDim2Attr.begin(), targetGemmDim2Attr.end());
      layoutAttr2.append(sourceGemmDim2Attr.begin(), sourceGemmDim2Attr.end());

      paddingInputAttrs.push_back(b.getNamedAttr(
          "layout",
          b.getArrayAttr({b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                              layoutAttr0.begin(), layoutAttr0.end())}),
                          b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                              layoutAttr1.begin(), layoutAttr1.end())}),
                          b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                              layoutAttr2.begin(), layoutAttr2.end())})})));

      paddingInputAttrs.push_back(
          b.getNamedAttr("upper_layer_layout",
                         b.getArrayAttr({gemmDim0TargetName, gemmDim1TargetName,
                                         gemmDim2TargetName})));

      paddingInputAttrs.push_back(b.getNamedAttr(
          "lower_layer_layout",
          b.getArrayAttr({gemmDim0Name, gemmDim1Name, gemmDim2Name})));

      if (inputOobCheckDims.size()) {
        llvm::SmallVector<IntegerAttr, 5> boundDims;
        for (size_t i = 0; i < inputShape.size(); i++) {
          if (inputOobCheckDims.find(i) != inputOobCheckDims.end())
            boundDims.push_back(b.getI32IntegerAttr(1));
          else
            boundDims.push_back(b.getI32IntegerAttr(0));
        }
        paddingInputAttrs.push_back(b.getNamedAttr(
            "bound_check",
            b.getArrayAttr({boundDims.begin(), boundDims.end()})));
      }

      auto paddingInputMemRefType =
          MemRefType::get(paddingInputShape, inputElementType);

      gemmBPad = b.create<miopen::TransformOp>(loc, paddingInputMemRefType,
                                               gemmB, paddingInputAttrs,
                                               /*populateBounds=*/true);

      // input padding end
    }

    // Transform output tensor.
    llvm::SmallVector<int64_t, 3> transformedOutputShape;

    llvm::SmallVector<NamedAttribute, 4> transformedOutputAttrs;

    SmallString<5> arg2TargetLayoutName0("gemmG");
    SmallString<5> arg2TargetLayoutName1("gemm");
    arg2TargetLayoutName1.append(fields.gemmTargetCharName[2].substr(0, 1));
    SmallString<5> arg2TargetLayoutName2("gemm");
    arg2TargetLayoutName2.append(fields.gemmTargetCharName[2].substr(1, 1));

    // output dims need oob ckeck
    llvm::DenseSet<int> outputOobCheckDims;
    // set layout attribute.
    // Output tensor transformation:
    // - Part 1: PassThrough K dimension to dimension 0, name it as gemmM.
    // - Part 2: Merge non-K dimensions to dimension 1, name it as gemmN.
    //
    // Output tensor transformation for backward weight:
    // - Part 1: Merge non-K dimensions to dimension 0, name it as gemmK.
    // - Part 2: PassThrough K dimension to dimension 1, name it as gemmM.
    {
      llvm::SmallVector<IntegerAttr, 3> nonKDims;
      IntegerAttr kDim, gDim;
      llvm::SmallVector<StringAttr, 3> nonKDimNames;
      StringAttr kDimName, gDimName;
      for (unsigned i = 0; i < outputLayoutAttr.size(); ++i) {
        if (auto strAttr =
                outputLayoutAttr.getValue()[i].template cast<StringAttr>()) {
          if (strAttr.getValue() == "ko") {
            kDim = b.getI32IntegerAttr(i);
            kDimName = strAttr;
          } else if (strAttr.getValue() == "go") {
            gDim = b.getI32IntegerAttr(i);
            gDimName = strAttr;
          } else {
            nonKDims.push_back(b.getI32IntegerAttr(i));
            nonKDimNames.push_back(strAttr);
          }
        }
      }

      // Compute transformed filter shape dimension.
      int64_t nonKDimSize = 1;
      for (unsigned i = 0; i < outputShape.size(); ++i) {
        if (i != kDim.getInt() && i != gDim.getInt()) {
          nonKDimSize *= outputShape[i];
        }
      }
      if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
        transformedOutputShape.push_back(outputShape[gDim.getInt()]);
        transformedOutputShape.push_back(nonKDimSize);
        transformedOutputShape.push_back(outputShape[kDim.getInt()]);
      } else {
        transformedOutputShape.push_back(outputShape[gDim.getInt()]);
        transformedOutputShape.push_back(outputShape[kDim.getInt()]);
        transformedOutputShape.push_back(nonKDimSize);
      }

      llvm::SmallVector<NamedAttribute, 3> sourceProbGDimAttr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({gDim})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gDimName}))};

      llvm::SmallVector<NamedAttribute, 3> sourceProbNHoWoDimAttr{
          b.getNamedAttr("lower_layer_dimensions",
                         b.getArrayAttr(ArrayRef<Attribute>(nonKDims.begin(),
                                                            nonKDims.end()))),
          b.getNamedAttr("lower_layer_names",
                         b.getArrayAttr(ArrayRef<Attribute>(
                             nonKDimNames.begin(), nonKDimNames.end()))),
          b.getNamedAttr("transformation", b.getStringAttr("Merge"))};

      llvm::SmallVector<NamedAttribute, 3> sourceProbKDimAttr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({kDim})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({kDimName}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemm0DimAttr{
          b.getNamedAttr("upper_layer_dimensions",
                         b.getArrayAttr({b.getI32IntegerAttr(0)})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({b.getStringAttr(
                                                  arg2TargetLayoutName0)}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemm1DimAttr{
          b.getNamedAttr("upper_layer_dimensions",
                         b.getArrayAttr({b.getI32IntegerAttr(1)})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({b.getStringAttr(
                                                  arg2TargetLayoutName1)}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemm2DimAttr{
          b.getNamedAttr("upper_layer_dimensions",
                         b.getArrayAttr({b.getI32IntegerAttr(2)})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({b.getStringAttr(
                                                  arg2TargetLayoutName2)}))};

      llvm::SmallVector<NamedAttribute, 0> layoutAttr0;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr1;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr2;

      if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
        layoutAttr0.append(targetGemm0DimAttr.begin(),
                           targetGemm0DimAttr.end());
        layoutAttr0.append(sourceProbGDimAttr.begin(),
                           sourceProbGDimAttr.end());
        layoutAttr1.append(targetGemm1DimAttr.begin(),
                           targetGemm1DimAttr.end());
        layoutAttr1.append(sourceProbNHoWoDimAttr.begin(),
                           sourceProbNHoWoDimAttr.end());
        layoutAttr2.append(targetGemm2DimAttr.begin(),
                           targetGemm2DimAttr.end());
        layoutAttr2.append(sourceProbKDimAttr.begin(),
                           sourceProbKDimAttr.end());
      } else {
        layoutAttr0.append(targetGemm0DimAttr.begin(),
                           targetGemm0DimAttr.end());
        layoutAttr0.append(sourceProbGDimAttr.begin(),
                           sourceProbGDimAttr.end());
        layoutAttr1.append(targetGemm1DimAttr.begin(),
                           targetGemm1DimAttr.end());
        layoutAttr1.append(sourceProbKDimAttr.begin(),
                           sourceProbKDimAttr.end());
        layoutAttr2.append(targetGemm2DimAttr.begin(),
                           targetGemm2DimAttr.end());
        layoutAttr2.append(sourceProbNHoWoDimAttr.begin(),
                           sourceProbNHoWoDimAttr.end());
      }

      transformedOutputAttrs.push_back(b.getNamedAttr(
          "layout", b.getArrayAttr({
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr0.begin(), layoutAttr0.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr1.begin(), layoutAttr1.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr2.begin(), layoutAttr2.end())}),
                    })));
    }

    // set lower_layer_layout attribute.
    transformedOutputAttrs.push_back(
        b.getNamedAttr("lower_layer_layout", outputLayoutAttr));
    // set upper_layer_layout attribute.
    transformedOutputAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout", b.getArrayAttr({
                                  b.getStringAttr(arg2TargetLayoutName0),
                                  b.getStringAttr(arg2TargetLayoutName1),
                                  b.getStringAttr(arg2TargetLayoutName2),
                              })));
    // set gridwise_gemm_argument_pos attribute.
    transformedOutputAttrs.push_back(b.getNamedAttr(
        "gridwise_gemm_argument_position",
        b.getI32IntegerAttr(fields.gridwiseGemmArgumentPosition[2])));
    // set gemmM & gemmN
    transformedOutputAttrs.push_back(
        b.getNamedAttr("gemmMExtra", b.getI32IntegerAttr(gemmMExtra)));
    transformedOutputAttrs.push_back(
        b.getNamedAttr("gemmNExtra", b.getI32IntegerAttr(gemmNExtra)));
    // set needExtraPad
    transformedOutputAttrs.push_back(
        b.getNamedAttr("extraPad", b.getBoolAttr(needExtraPad)));

    auto transformedOutputMemRefType =
        MemRefType::get(transformedOutputShape, outputElementType);
    // set lowest_layer attribute.
    transformedOutputAttrs.push_back(
        b.getNamedAttr("lowest_layer", b.getBoolAttr(true)));
    auto gemmC = b.create<miopen::TransformOp>(
        loc, transformedOutputMemRefType, op.output(), transformedOutputAttrs,
        /*populateBounds=*/true);

    auto gemmCPad = gemmC;
    bool isOutputPad = false;
    // output padding start
    // output matrix dim: K & NHW
    // when backward weight , GEMMK = NHW
    // N:batch size, H:output height ,W:output width
    // If size of N*h*w is under 32 or 64 ,we pad it to 32 or 64
    // then mlir can do gemm
    // we just add more one transform to do it

    bool outputCheckPadGemmK = false;
    bool outputCheckPadGemmM = false;
    bool outputCheckPadGemmN = false;
    outputCheckPadGemmK =
        (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType &&
         gemmKExtra > 0);
    outputCheckPadGemmM =
        (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType &&
         gemmMExtra > 0) ||
        (convOpType == miopen::ConvOpType::Conv2DOpType && gemmMExtra > 0);
    outputCheckPadGemmN =
        (convOpType == miopen::ConvOpType::Conv2DOpType && gemmNExtra > 0);
    if (outputCheckPadGemmK || outputCheckPadGemmM || outputCheckPadGemmN) {
      StringAttr gemmDim0TargetName = b.getStringAttr(arg2TargetLayoutName0);
      StringAttr gemmDim1TargetName;
      StringAttr gemmDim2TargetName;

      bool isGemmDim1Pad = false;
      bool isGemmDim2Pad = false;

      llvm::SmallVector<NamedAttribute, 3> paddingOutputAttrs;
      llvm::SmallVector<int64_t, 2> paddingOutputShape;

      llvm::SmallVector<NamedAttribute, 0> layoutAttr0;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr1;
      llvm::SmallVector<NamedAttribute, 0> layoutAttr2;

      StringAttr gemmDim0Name = b.getStringAttr(arg2TargetLayoutName0);
      IntegerAttr GemmDim0 = b.getI32IntegerAttr(0);
      StringAttr gemmDim1Name = b.getStringAttr(arg2TargetLayoutName1);
      IntegerAttr GemmDim1 = b.getI32IntegerAttr(1);
      StringAttr gemmDim2Name = b.getStringAttr(arg2TargetLayoutName2);
      IntegerAttr GemmDim2 = b.getI32IntegerAttr(2);

      paddingOutputShape.push_back(transformedOutputShape[0]);
      paddingOutputShape.push_back(transformedOutputShape[1]);
      paddingOutputShape.push_back(transformedOutputShape[2]);

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim0Attr{
          b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim0})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim0Name}))};

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim1Attr{
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim1})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim1Name}))};

      llvm::SmallVector<NamedAttribute, 3> sourceGemmDim2Attr{
          b.getNamedAttr("lower_layer_dimensions", b.getArrayAttr({GemmDim2})),
          b.getNamedAttr("lower_layer_names", b.getArrayAttr({gemmDim2Name}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim0Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim0})),
          b.getNamedAttr("upper_layer_names", b.getArrayAttr({GemmDim0}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim1Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim1}))};

      llvm::SmallVector<NamedAttribute, 3> targetGemmDim2Attr{
          b.getNamedAttr("upper_layer_dimensions", b.getArrayAttr({GemmDim2}))};

      if (outputCheckPadGemmK) {
        if (arg2TargetLayoutName1 == "gemmK") {
          isOutputPad = true;
          isGemmDim1Pad = true;
          gemmDim1TargetName = b.getStringAttr(gemmKPad_name);
          // backward weights  dim 1 is composing of (N,H,W)
          // N:batch size, H: output height ,W:output width
          paddingOutputShape[1] = paddingOutputShape[1] + gemmKExtra;
          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim1Attr.push_back(b.getNamedAttr(
              "parameters", b.getArrayAttr({b.getI32IntegerAttr(0),
                                            b.getI32IntegerAttr(gemmKExtra)})));

          targetGemmDim1Attr.push_back(
              b.getNamedAttr("upper_layer_names",
                             b.getArrayAttr({b.getStringAttr(gemmKPad_name)})));
          // output backward weights gemmK is composed of  n,h,w, check all dims
          // due to it's load , not store ,if it's store ,check only no dim
          outputOobCheckDims.insert(nameToDims["no"]);
          outputOobCheckDims.insert(nameToDims["ho"]);
          outputOobCheckDims.insert(nameToDims["wo"]);
        }
      }

      if (outputCheckPadGemmM) {
        if (arg2TargetLayoutName1 == "gemmM") {
          isOutputPad = true;
          isGemmDim1Pad = true;
          gemmDim1TargetName = b.getStringAttr(gemmMPad_name);
          // output forward gemmM is k
          paddingOutputShape[1] = paddingOutputShape[1] + gemmMExtra;
          // output forward gemmM is k
          // so padding gemmMExtra
          sourceGemmDim1Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim1Attr.push_back(b.getNamedAttr(
              "parameters", b.getArrayAttr({b.getI32IntegerAttr(0),
                                            b.getI32IntegerAttr(gemmMExtra)})));
          // output forward gemmM is k, check the ko dim
          targetGemmDim1Attr.push_back(b.getNamedAttr(
              "names", b.getArrayAttr({b.getStringAttr(gemmMPad_name)})));
          outputOobCheckDims.insert(nameToDims["ko"]);
        } else if (arg2TargetLayoutName2 == "gemmM") {
          isOutputPad = true;
          isGemmDim2Pad = true;
          gemmDim2TargetName = b.getStringAttr(gemmMPad_name);
          // output backward weights gemmM is k
          // so padding gemmMExtra
          paddingOutputShape[2] = paddingOutputShape[2] + gemmMExtra;
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim2Attr.push_back(b.getNamedAttr(
              "parameters", b.getArrayAttr({b.getI32IntegerAttr(0),
                                            b.getI32IntegerAttr(gemmMExtra)})));
          // output backward weights gemmM is k,
          // so padding gemmMExtra
          targetGemmDim2Attr.push_back(b.getNamedAttr(
              "names", b.getArrayAttr({b.getStringAttr(gemmMPad_name)})));
          outputOobCheckDims.insert(nameToDims["ko"]);
        }
      }

      if (outputCheckPadGemmN) {
        if (arg2TargetLayoutName2 == "gemmN") {
          // forward output gemmN is nhw
          isOutputPad = true;
          isGemmDim2Pad = true;
          gemmDim2TargetName = b.getStringAttr(gemmNPad_name);
          // forward output gemmN is combining(N,H,W)
          // so padding gemmNExtra

          paddingOutputShape[2] = paddingOutputShape[2] + gemmNExtra;
          sourceGemmDim2Attr.push_back(
              b.getNamedAttr("transformation", b.getStringAttr("Pad")));
          sourceGemmDim2Attr.push_back(b.getNamedAttr(
              "parameters", b.getArrayAttr({b.getI32IntegerAttr(0),
                                            b.getI32IntegerAttr(gemmNExtra)})));

          targetGemmDim2Attr.push_back(b.getNamedAttr(
              "names", b.getArrayAttr({b.getStringAttr(gemmNPad_name)})));
          // FIXME: to set dim in merge transormation to oob store,
          // set only top dim or you will get zero values
          // output forward gemmM is composed of n , h ,w, check the top dim :no
          outputOobCheckDims.insert(nameToDims["no"]);
        }
      }

      if (!isGemmDim1Pad) {
        gemmDim1TargetName = gemmDim1Name;
        sourceGemmDim1Attr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")));
        targetGemmDim1Attr.push_back(b.getNamedAttr(
            "upper_layer_names", b.getArrayAttr({gemmDim1Name})));
      } else if (!isGemmDim2Pad) {
        gemmDim2TargetName = gemmDim2Name;
        sourceGemmDim2Attr.push_back(
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")));
        targetGemmDim2Attr.push_back(b.getNamedAttr(
            "upper_layer_names", b.getArrayAttr({gemmDim2Name})));
      }

      layoutAttr0.append(targetGemmDim0Attr.begin(), targetGemmDim0Attr.end());
      layoutAttr0.append(sourceGemmDim0Attr.begin(), sourceGemmDim0Attr.end());
      layoutAttr1.append(targetGemmDim1Attr.begin(), targetGemmDim1Attr.end());
      layoutAttr1.append(sourceGemmDim1Attr.begin(), sourceGemmDim1Attr.end());
      layoutAttr2.append(targetGemmDim2Attr.begin(), targetGemmDim2Attr.end());
      layoutAttr2.append(sourceGemmDim2Attr.begin(), sourceGemmDim2Attr.end());

      // set gemmKExtra & gemmNExtra & gemmNExtra
      paddingOutputAttrs.push_back(
          b.getNamedAttr("gemmKExtra", b.getI32IntegerAttr(gemmKExtra)));
      paddingOutputAttrs.push_back(
          b.getNamedAttr("gemmNExtra", b.getI32IntegerAttr(gemmNExtra)));
      paddingOutputAttrs.push_back(
          b.getNamedAttr("gemmMExtra", b.getI32IntegerAttr(gemmMExtra)));

      paddingOutputAttrs.push_back(b.getNamedAttr(
          "layout", b.getArrayAttr({
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr0.begin(), layoutAttr0.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr1.begin(), layoutAttr1.end())}),
                        b.getDictionaryAttr({ArrayRef<NamedAttribute>(
                            layoutAttr2.begin(), layoutAttr2.end())}),
                    })));

      paddingOutputAttrs.push_back(
          b.getNamedAttr("upper_layer_layout",
                         b.getArrayAttr({gemmDim0TargetName, gemmDim1TargetName,
                                         gemmDim2TargetName})));

      paddingOutputAttrs.push_back(b.getNamedAttr(
          "lower_layer_layout",
          b.getArrayAttr({gemmDim0Name, gemmDim1Name, gemmDim2Name})));

      if (outputOobCheckDims.size()) {
        llvm::SmallVector<IntegerAttr, 5> boundDims;
        for (size_t i = 0; i < outputShape.size(); i++) {
          if (outputOobCheckDims.find(i) != outputOobCheckDims.end())
            boundDims.push_back(b.getI32IntegerAttr(1));
          else
            boundDims.push_back(b.getI32IntegerAttr(0));
        }
        paddingOutputAttrs.push_back(b.getNamedAttr(
            "bound_check",
            b.getArrayAttr({boundDims.begin(), boundDims.end()})));
      }
      auto paddingOutputMemRefType =
          MemRefType::get(paddingOutputShape, outputElementType);

      gemmCPad = b.create<miopen::TransformOp>(loc, paddingOutputMemRefType,
                                               gemmC, paddingOutputAttrs,
                                               /*populateBounds=*/true);
      // output padding end
    }

    // Set attributes for gridwise_gemm op.
    llvm::SmallVector<NamedAttribute, 8> gridwiseGemmAttrs{
        b.getNamedAttr("arch", archAttr),
        b.getNamedAttr("num_cu", numCuAttr),
        b.getNamedAttr("filter_layout", filterLayoutAttr),
        b.getNamedAttr("filter_dimension", b.getI64ArrayAttr(filterShape)),
        b.getNamedAttr("input_layout", inputLayoutAttr),
        b.getNamedAttr("input_dimension", b.getI64ArrayAttr(inputShape)),
        b.getNamedAttr("output_layout", outputLayoutAttr),
        b.getNamedAttr("output_dimension", b.getI64ArrayAttr(outputShape)),
        b.getNamedAttr("dilations", dilationsAttr),
        b.getNamedAttr("strides", stridesAttr),
        b.getNamedAttr("padding", paddingAttr),
    };

    // xdlopsV2.
    if (isXdlops)
      gridwiseGemmAttrs.push_back(
          b.getNamedAttr("xdlopsV2", b.getBoolAttr(true)));

    if (convOpType == miopen::ConvOpType::Conv2DBwdDataOpType) {
      gridwiseGemmAttrs.push_back(b.getNamedAttr(
          "kernel_algorithm", b.getStringAttr("backward_data_v1r1")));
    } else if (convOpType == miopen::ConvOpType::Conv2DOpType) {
      gridwiseGemmAttrs.push_back(
          b.getNamedAttr("kernel_algorithm", b.getStringAttr("v4r4")));
    } else if (convOpType == miopen::ConvOpType::Conv2DBwdWeightOpType) {
      gridwiseGemmAttrs.push_back(b.getNamedAttr(
          "kernel_algorithm", b.getStringAttr("backward_weight_v4r4")));
    }

    // Emit miopen.gridwise_gemm op.
    // Emit miopen.gridwise_gemm_v2 if xdlopsV2 attribute is true.
    if (isFilterPad)
      gemmA = gemmAPad;
    if (isInputPad)
      gemmB = gemmBPad;
    if (isOutputPad)
      gemmC = gemmCPad;

    auto arguments = SmallVector<Value, 3>{gemmA, gemmB, gemmC};

    if (xdlopsV2Attr && xdlopsV2Attr.getValue() == true) {
      auto gop = b.create<miopen::GridwiseGemmV2Op>(
          loc, ArrayRef<Type>{},
          ValueRange{arguments[fields.gridwiseGemmArgumentPosition[0]],
                     arguments[fields.gridwiseGemmArgumentPosition[1]],
                     arguments[fields.gridwiseGemmArgumentPosition[2]]},
          gridwiseGemmAttrs);
      affixGridwiseGemmAttributes(op, gop, b);
    } else {
      auto gop = b.create<miopen::GridwiseGemmOp>(
          loc, ArrayRef<Type>{},
          ValueRange{arguments[fields.gridwiseGemmArgumentPosition[0]],
                     arguments[fields.gridwiseGemmArgumentPosition[1]],
                     arguments[fields.gridwiseGemmArgumentPosition[2]]},
          gridwiseGemmAttrs);
      affixGridwiseGemmAttributes(op, gop, b);
    }

    // Finally, erase the original Conv2D op.
    op.erase();

    return success();
  }

  LogicalResult backwardData(T op, PatternRewriter &b) const {
    auto loc = op.getLoc();
    auto gemmIdAttr = op->template getAttrOfType<IntegerAttr>("gemm_id");
    auto archAttr = op->template getAttrOfType<StringAttr>("arch");
    auto numCuAttr = op->template getAttrOfType<IntegerAttr>("num_cu");

    auto filterLayoutAttr =
        op->template getAttrOfType<ArrayAttr>("filter_layout");
    auto inputLayoutAttr =
        op->template getAttrOfType<ArrayAttr>("input_layout");
    auto outputLayoutAttr =
        op->template getAttrOfType<ArrayAttr>("output_layout");

    auto dilationsAttr = op->template getAttrOfType<ArrayAttr>("dilations");
    auto stridesAttr = op->template getAttrOfType<ArrayAttr>("strides");
    auto paddingAttr = op->template getAttrOfType<ArrayAttr>("padding");

    // Get shape of filter tensor.
    auto filterType = op.filter().getType().template cast<MemRefType>();
    auto filterShape = filterType.getShape();
    auto filterElementType = filterType.getElementType();

    // Get shape of input tensor.
    auto inputType = op.input().getType().template cast<MemRefType>();
    auto inputShape = inputType.getShape();
    auto inputElementType = inputType.getElementType();

    // Get shape of output tensor.
    auto outputType = op.output().getType().template cast<MemRefType>();
    auto outputShape = outputType.getShape();
    auto outputElementType = outputType.getElementType();

    // Obtain convolution parameters: padding / dialtion / stride.
    auto leftPadH =
        paddingAttr.getValue()[0].template cast<IntegerAttr>().getInt();
    auto leftPadW =
        paddingAttr.getValue()[2].template cast<IntegerAttr>().getInt();
    auto rightPadH =
        paddingAttr.getValue()[1].template cast<IntegerAttr>().getInt();
    auto rightPadW =
        paddingAttr.getValue()[3].template cast<IntegerAttr>().getInt();

    auto dilationH =
        dilationsAttr.getValue()[0].template cast<IntegerAttr>().getInt();
    auto dilationW =
        dilationsAttr.getValue()[1].template cast<IntegerAttr>().getInt();
    auto strideH =
        stridesAttr.getValue()[0].template cast<IntegerAttr>().getInt();
    auto strideW =
        stridesAttr.getValue()[1].template cast<IntegerAttr>().getInt();
    // get y, x, ho, wo, hi, wi
    int64_t g, n, k, c, y, x, ho, wo, hi, wi;
    g = n = k = c = y = x = ho = wo = hi = wi = 0;

    // nameToDims can store <dim name ,dim index> ,we can use it to do oob check
    llvm::DenseSet<int> filterOobCheckDims;
    llvm::DenseMap<StringRef, int> nameToDims;

    for (unsigned i = 0; i < filterLayoutAttr.size(); ++i) {
      auto filterAttr =
          filterLayoutAttr.getValue()[i].template cast<StringAttr>();
      auto inputAttr =
          inputLayoutAttr.getValue()[i].template cast<StringAttr>();
      auto outputAttr =
          outputLayoutAttr.getValue()[i].template cast<StringAttr>();

      nameToDims[filterAttr.getValue()] = i;
      nameToDims[inputAttr.getValue()] = i;
      nameToDims[outputAttr.getValue()] = i;

      if (filterAttr.getValue() == "g") {
        g = filterShape[i];
      } else if (filterAttr.getValue() == "k") {
        k = filterShape[i];
      } else if (filterAttr.getValue() == "c") {
        c = filterShape[i];
      } else if (filterAttr.getValue() == "y") {
        y = filterShape[i];
      } else if (filterAttr.getValue() == "x") {
        x = filterShape[i];
      }

      if (inputAttr.getValue() == "ni") {
        n = inputShape[i];
      } else if (inputAttr.getValue() == "hi") {
        hi = inputShape[i];
      } else if (inputAttr.getValue() == "wi") {
        wi = inputShape[i];
      }

      if (outputAttr.getValue() == "ho") {
        ho = outputShape[i];
      } else if (outputAttr.getValue() == "wo") {
        wo = outputShape[i];
      }
    }

    // compute padding hi/wi.
    auto hiPadded = hi + leftPadH + rightPadH;
    auto wiPadded = wi + leftPadW + rightPadW;

    auto gcdStrideDilationH = math_util::gcd(strideH, dilationH);
    auto gcdStrideDilationW = math_util::gcd(strideW, dilationW);

    auto yTilda = strideH / gcdStrideDilationH;
    auto xTilda = strideW / gcdStrideDilationW;

    auto yDot = math_util::integer_divide_ceil(y, yTilda);
    auto xDot = math_util::integer_divide_ceil(x, xTilda);

    auto hTilda = ho + math_util::integer_divide_ceil(dilationH * (y - 1), strideH);
    auto wTilda = wo + math_util::integer_divide_ceil(dilationW * (x - 1), strideW);

    auto iHTildaLeft = math_util::integer_divide_floor(
        std::max(0l, leftPadH - dilationH * (yTilda - 1)), strideH);
    auto iWTildaLeft = math_util::integer_divide_floor(
        std::max(0l, leftPadW - dilationW * (xTilda - 1)), strideW);

    auto iHTildaRight = std::min(
        hTilda, math_util::integer_divide_ceil(leftPadH + hi - 1, strideH) + 1);
    auto iWTildaRight = std::min(
        wTilda, math_util::integer_divide_ceil(leftPadW + wi - 1, strideW) + 1);

    auto hTildaSlice = iHTildaRight - iHTildaLeft;
    auto wTildaSlice = iWTildaRight - iWTildaLeft;

    auto getGemmId = [&](int kernelId) {
      // kernelId 0 must be gemmId 0
      if (kernelId <= 0)
        return 0;

      llvm::SmallVector<int> gemmIds;
      for (int gemmId = 0; gemmId < yTilda * xTilda; gemmId++) {
        // gemm_k size is different for each GEMM
        const auto iYTilda = gemmId / xTilda;
        const auto iXTilda = gemmId % xTilda;

        auto yDotSlice = math_util::integer_divide_ceil(y - iYTilda, yTilda);
        auto xDotSlice = math_util::integer_divide_ceil(x - iXTilda, xTilda);
        // gemmK must > 0, otherwise not need to run
        if (yDotSlice * xDotSlice > 0) {
          gemmIds.push_back(gemmId);
        }
      }
      assert(gemmIds.size() > static_cast<size_t>(kernelId));
      return gemmIds[kernelId];
    };
    auto gemmId = getGemmId(gemmIdAttr.getInt());
    auto iYTilda = gemmId / xTilda;
    auto iXTilda = gemmId % xTilda;
    auto yDotSlice = math_util::integer_divide_ceil(y - iYTilda, yTilda);
    auto xDotSlice = math_util::integer_divide_ceil(x - iXTilda, xTilda);

    bool needExtraPad = false;
    bool isOriginalKernelSupport = true;
    int64_t gemmM_size, gemmN_size, gemmK_size;
    int64_t gemmMExtra, gemmNExtra, gemmKExtra;
    // backward data only, it's igemm v4r1 algo
    // c is input chaneels , k is output channels
    // n is batch , yDotSlice,xDotSlice computed in above
    gemmMExtra = gemmNExtra = gemmKExtra = 0;
    gemmM_size = c;
    gemmK_size = k * yDotSlice * xDotSlice;
    gemmN_size = n * hTildaSlice * wTildaSlice;

    bool isXdlops = false;
    auto xdlopsV2Attr = op->template getAttrOfType<BoolAttr>("xdlopsV2");
    if (xdlopsV2Attr && xdlopsV2Attr.getValue() == true)
      isXdlops = true;

    bool isSupportPaddingKernel = true;
    bool isStride2Pad1 = false;
    if (strideH > 1 || strideW > 1) {
      if (leftPadH > 0 || leftPadW > 0 || rightPadH > 0 || rightPadW > 0) {
        isStride2Pad1 = true;
      }
    }
    // we use this lamda to compute extra padding size
    // for example, if gemmM size is 3 and gemmMPerBlock is 64
    // we gemmMExtra is 64 so (gemmM + gemmMExtra )%gemmMPerBlock =0
    auto calculatePaddingKernelSize = [&isOriginalKernelSupport, &needExtraPad,
                                       gemmM_size, gemmN_size, gemmK_size,
                                       &gemmMExtra, &gemmNExtra,
                                       &gemmKExtra](auto &populateParams) {
      auto config_params = populateParams.getTuningParameters();
      unsigned numOfFailedConfigs = 0;
      for (auto &params : config_params) {
        if (gemmM_size % params.gemmMPerBlock == 0 &&
            gemmK_size % params.gemmKPerBlock == 0 &&
            gemmN_size % params.gemmNPerBlock == 0) {
          isOriginalKernelSupport = true;
          break;
        } else {
          isOriginalKernelSupport = false;
          numOfFailedConfigs++;
        }
      }

      auto extraParams = populateParams.getUniversalParameters();
      if (numOfFailedConfigs == config_params.size()) {

        needExtraPad = true;
        int gemmM_remain, gemmK_remain, gemmN_remain;

        gemmM_remain = gemmM_size % extraParams.gemmMPerBlock;
        if (gemmM_remain != 0)
          gemmMExtra = extraParams.gemmMPerBlock - gemmM_remain;

        gemmN_remain = gemmN_size % extraParams.gemmNPerBlock;
        if (gemmN_remain != 0)
          gemmNExtra = extraParams.gemmNPerBlock - gemmN_remain;

        gemmK_remain = gemmK_size % extraParams.gemmKPerBlock;
        if (gemmK_remain != 0)
          gemmKExtra = extraParams.gemmKPerBlock - gemmK_remain;

        // llvm::errs() << "gemmMExtra: " << gemmMExtra << "gemmNExtra: " <<
        // gemmNExtra << "gemmKExtra: " << gemmKExtra << "\n";
      }
    }; // calculatePaddingKernelSize end

    if (!isXdlops) {
      PopulateParams populateParams;
      calculatePaddingKernelSize(populateParams);
    } else { // xdlops
      PopulateParamsXDL populateParamsXDL;
      calculatePaddingKernelSize(populateParamsXDL);
    }

    isSupportPaddingKernel = isSupportBackwardDataPaddingKernel(
        isXdlops, isStride2Pad1, gemmMExtra, gemmKExtra, gemmNExtra);
    // don't do backward data padding kernel if isSupportPaddingKernel=false
    if (!isSupportPaddingKernel && !isOriginalKernelSupport)
      return failure();
    // Transform filter tensor.

    // set layout attribute.
    // Weight tensor transformation for Conv2DOp
    auto getGemmA = [&]() -> Value {
      // key to dim
      std::map<StringRef, int> filterKeyToDim;
      for (unsigned i = 0; i < filterLayoutAttr.size(); ++i) {
        if (auto strAttr =
                filterLayoutAttr.getValue()[i].template cast<StringAttr>()) {
          filterKeyToDim[strAttr.getValue()] = i;
        }
      }

      // wei_g_k_c_ydot_ytilda_xdot_xtilda
      llvm::SmallVector<StringAttr, 7> firtFilterDimName;
      auto getWeiGKCYDotYTildaXDotXTilda = [&]() {
        decltype(firtFilterDimName) &curOutputDimName = firtFilterDimName;
        llvm::SmallVector<int64_t, 6> transformedFilterShape;
        llvm::SmallVector<NamedAttribute, 3> transformedFilterAttrs;
        // g
        curOutputDimName.push_back(b.getStringAttr("g"));
        transformedFilterShape.push_back(g);
        llvm::SmallVector<NamedAttribute, 5> gDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[0]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(filterKeyToDim["g"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("g")}))};

        // k
        curOutputDimName.push_back(b.getStringAttr("k"));
        transformedFilterShape.push_back(k);
        llvm::SmallVector<NamedAttribute, 5> kDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(1)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[1]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(filterKeyToDim["k"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("k")}))};

        // c
        curOutputDimName.push_back(b.getStringAttr("c"));
        transformedFilterShape.push_back(c);
        llvm::SmallVector<NamedAttribute, 5> cDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(2)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[2]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(filterKeyToDim["c"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("c")}))};

        // y
        curOutputDimName.push_back(b.getStringAttr("ydot"));
        curOutputDimName.push_back(b.getStringAttr("ytilda"));
        transformedFilterShape.push_back(yDot);
        transformedFilterShape.push_back(yTilda);
        llvm::SmallVector<NamedAttribute, 6> yDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(3),
                                           b.getI32IntegerAttr(4)})),
            b.getNamedAttr(
                "upper_layer_names",
                b.getArrayAttr({curOutputDimName[3], curOutputDimName[4]})),
            b.getNamedAttr("transformation", b.getStringAttr("Embed")),
            b.getNamedAttr("parameters", b.getArrayAttr({
                                             b.getI32IntegerAttr(
                                                 strideH / gcdStrideDilationH),
                                             b.getI32IntegerAttr(1),
                                         })),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(filterKeyToDim["y"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("y")}))};

        // x
        curOutputDimName.push_back(b.getStringAttr("xdot"));
        curOutputDimName.push_back(b.getStringAttr("xtilda"));
        transformedFilterShape.push_back(xDot);
        transformedFilterShape.push_back(xTilda);
        llvm::SmallVector<NamedAttribute, 6> xDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(5),
                                           b.getI32IntegerAttr(6)})),
            b.getNamedAttr(
                "upper_layer_names",
                b.getArrayAttr({curOutputDimName[5], curOutputDimName[6]})),
            b.getNamedAttr("transformation", b.getStringAttr("Embed")),
            b.getNamedAttr("parameters", b.getArrayAttr({
                                             b.getI32IntegerAttr(
                                                 strideW / gcdStrideDilationW),
                                             b.getI32IntegerAttr(1),
                                         })),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(filterKeyToDim["x"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("x")}))};

        transformedFilterAttrs.push_back(b.getNamedAttr(
            "layout",
            b.getArrayAttr(
                {b.getDictionaryAttr(gDimAttr), b.getDictionaryAttr(kDimAttr),
                 b.getDictionaryAttr(cDimAttr), b.getDictionaryAttr(yDimAttr),
                 b.getDictionaryAttr(xDimAttr)})));
        transformedFilterAttrs.push_back(b.getNamedAttr(
            "upper_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                               curOutputDimName.end()))));

        transformedFilterAttrs.push_back(
            b.getNamedAttr("lower_layer_layout", filterLayoutAttr));

        auto transformedFilterMemRefType =
            MemRefType::get(transformedFilterShape, filterElementType);
        // set lowest_layer attribute.
        transformedFilterAttrs.push_back(
            b.getNamedAttr("lowest_layer", b.getBoolAttr(true)));
        auto gemm = b.create<miopen::TransformOp>(
            loc, transformedFilterMemRefType, op.filter(),
            transformedFilterAttrs, /*populateBounds=*/true);
        return gemm;
      };

      auto weiGKCYDotYTildaXDotXTilda = getWeiGKCYDotYTildaXDotXTilda();
      // from wei_g_k_c_ydot_ytilda_xdot_xtilda to
      // wei_g_k_c_ydotslice_ytidaslice_xdotslice_xtildaslice
      llvm::SmallVector<StringAttr, 7> secondFilterDimName;
      auto getWeiGKCYDotSliceYTidaSliceXDotSliceXTildaSlice =
          [&](decltype(firtFilterDimName) &preOutputDimName,
              llvm::SmallVector<StringAttr, 7> &curOutputDimName) {
            llvm::SmallVector<int64_t, 6> transformedFilterShape;
            // g
            curOutputDimName.push_back(b.getStringAttr("g"));
            transformedFilterShape.push_back(g);
            llvm::SmallVector<NamedAttribute, 5> gDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[0]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[0]}))};

            // k
            curOutputDimName.push_back(b.getStringAttr("k"));
            transformedFilterShape.push_back(k);
            llvm::SmallVector<NamedAttribute, 5> kDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[1]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[1]}))};

            // c
            curOutputDimName.push_back(b.getStringAttr("c"));
            transformedFilterShape.push_back(c);
            llvm::SmallVector<NamedAttribute, 5> cDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[2]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[2]}))};

            // slice ydot xdot
            curOutputDimName.push_back(b.getStringAttr("ydotslice"));
            curOutputDimName.push_back(b.getStringAttr("ytildaslice"));
            curOutputDimName.push_back(b.getStringAttr("xdotslice"));
            curOutputDimName.push_back(b.getStringAttr("xtildaslice"));

            transformedFilterShape.push_back(yDotSlice);
            transformedFilterShape.push_back(1);
            transformedFilterShape.push_back(xDotSlice);
            transformedFilterShape.push_back(1);

            llvm::SmallVector<NamedAttribute, 6> yxDotSliceDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(5)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[3], curOutputDimName[5]})),
                b.getNamedAttr("transformation", b.getStringAttr("Slice")),
                b.getNamedAttr("begins", b.getArrayAttr({
                                             b.getI32IntegerAttr(0),
                                             b.getI32IntegerAttr(0),
                                         })),
                b.getNamedAttr("ends", b.getArrayAttr({
                                           b.getI32IntegerAttr(yDotSlice),
                                           b.getI32IntegerAttr(xDotSlice),
                                       })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(5)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[3],
                                               preOutputDimName[5]}))};

            // xy tilda slice
            llvm::SmallVector<NamedAttribute, 6> yxTildaSliceDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[4], curOutputDimName[6]})),
                b.getNamedAttr("transformation", b.getStringAttr("Slice")),
                b.getNamedAttr("begins", b.getArrayAttr({
                                             b.getI32IntegerAttr(iYTilda),
                                             b.getI32IntegerAttr(iXTilda),
                                         })),
                b.getNamedAttr("ends", b.getArrayAttr({
                                           b.getI32IntegerAttr(iYTilda + 1),
                                           b.getI32IntegerAttr(iXTilda + 1),
                                       })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[4],
                                               preOutputDimName[6]}))};

            llvm::SmallVector<NamedAttribute, 3> transformedFilterAttrs;
            transformedFilterAttrs.push_back(b.getNamedAttr(
                "layout",
                b.getArrayAttr({b.getDictionaryAttr(gDimAttr),
                                b.getDictionaryAttr(kDimAttr),
                                b.getDictionaryAttr(cDimAttr),
                                b.getDictionaryAttr(yxDotSliceDimAttr),
                                b.getDictionaryAttr(yxTildaSliceDimAttr)})));
            transformedFilterAttrs.push_back(b.getNamedAttr(
                "upper_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                                   curOutputDimName.end()))));

            transformedFilterAttrs.push_back(b.getNamedAttr(
                "lower_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                                   preOutputDimName.end()))));

            auto transformedFilterMemRefType =
                MemRefType::get(transformedFilterShape, filterElementType);
            auto gemm = b.create<miopen::TransformOp>(
                loc, transformedFilterMemRefType, weiGKCYDotYTildaXDotXTilda,
                transformedFilterAttrs, /*populateBounds=*/true);
            return gemm;
          };
      auto weiGKCYDotSliceYTidaSliceXDotSliceXTildaSlice =
          getWeiGKCYDotSliceYTidaSliceXDotSliceXTildaSlice(firtFilterDimName,
                                                           secondFilterDimName);

      auto getWeiGemmGGemmKGemmM =
          [&](decltype(secondFilterDimName) &preOutputDimName) -> Value {
        llvm::SmallVector<StringAttr, 7> curOutputDimName;
        llvm::SmallVector<int64_t, 7> transformedFilterShape;
        // gemmG
        curOutputDimName.push_back(b.getStringAttr("gemmG"));
        transformedFilterShape.push_back(g);
        llvm::SmallVector<NamedAttribute, 5> gemmGDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[0]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr("lower_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({preOutputDimName[0]}))};

        // gemmK
        curOutputDimName.push_back(b.getStringAttr("gemmK"));
        transformedFilterShape.push_back(k * yDotSlice * xDotSlice);
        llvm::SmallVector<NamedAttribute, 5> gemmKDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(1)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[1]})),
            b.getNamedAttr("transformation", b.getStringAttr("Merge")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(1), b.getI32IntegerAttr(3),
                                b.getI32IntegerAttr(5)})),
            b.getNamedAttr(
                "lower_layer_names",
                b.getArrayAttr({preOutputDimName[1], preOutputDimName[3],
                                preOutputDimName[5]}))};

        // gemmM
        curOutputDimName.push_back(b.getStringAttr("gemmM"));
        transformedFilterShape.push_back(c);
        llvm::SmallVector<NamedAttribute, 5> gemmMDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(2)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[2]})),
            b.getNamedAttr("transformation", b.getStringAttr("Merge")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(2), b.getI32IntegerAttr(4),
                                b.getI32IntegerAttr(6)})),
            b.getNamedAttr(
                "lower_layer_names",
                b.getArrayAttr({preOutputDimName[2], preOutputDimName[4],
                                preOutputDimName[6]}))};

        llvm::SmallVector<NamedAttribute, 3> transformedFilterAttrs;
        transformedFilterAttrs.push_back(b.getNamedAttr(
            "layout", b.getArrayAttr({b.getDictionaryAttr(gemmGDimAttr),
                                      b.getDictionaryAttr(gemmKDimAttr),
                                      b.getDictionaryAttr(gemmMDimAttr)})));
        transformedFilterAttrs.push_back(b.getNamedAttr(
            "upper_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                               curOutputDimName.end()))));

        transformedFilterAttrs.push_back(b.getNamedAttr(
            "lower_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                               preOutputDimName.end()))));

        transformedFilterAttrs.push_back(b.getNamedAttr(
            "gridwise_gemm_argument_position", b.getI32IntegerAttr(0)));

        auto transformedFilterMemRefType =
            MemRefType::get(transformedFilterShape, filterElementType);
        auto gemm = b.create<miopen::TransformOp>(
            loc, transformedFilterMemRefType,
            weiGKCYDotSliceYTidaSliceXDotSliceXTildaSlice,
            transformedFilterAttrs, /*populateBounds=*/true);
        return gemm;
      };
      auto weiGemmGGemmKGemmM = getWeiGemmGGemmKGemmM(secondFilterDimName);

      // if the config do not do padding kernel,
      // gemmAPad = weiGemmGGemmKGemmM
      Value gemmAPad = padFilter(isXdlops, gemmMExtra, gemmNExtra, gemmKExtra,
                                 weiGemmGGemmKGemmM, b, loc, filterOobCheckDims,
                                 nameToDims, filterShape, filterElementType, g,
                                 k, c, yDotSlice, xDotSlice);
      return gemmAPad;
    };

    auto getGemmB = [&]() -> Value {
      // dim of oob check
      llvm::DenseSet<int> inputOobCheckDims;
      // key to dim
      std::map<StringRef, int> currentKeyToDim;
      for (unsigned i = 0; i < inputLayoutAttr.size(); ++i) {
        if (auto strAttr =
                inputLayoutAttr.getValue()[i].template cast<StringAttr>()) {
          currentKeyToDim[strAttr.getValue()] = i;
        }
      }

      llvm::SmallVector<StringAttr, 5> firstOutputDimName;
      auto getInGNCHipWip = [&]() {
        decltype(firstOutputDimName) &curOutputDimName = firstOutputDimName;
        llvm::SmallVector<int64_t, 7> transformedShape;
        llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
        // gi
        curOutputDimName.push_back(b.getStringAttr("gi"));
        transformedShape.push_back(g);
        llvm::SmallVector<NamedAttribute, 5> gDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[0]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["gi"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("gi")}))};
        // ni
        curOutputDimName.push_back(b.getStringAttr("ni"));
        transformedShape.push_back(n);
        llvm::SmallVector<NamedAttribute, 5> nDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(1)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[1]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["ni"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("ni")}))};
        // ci
        curOutputDimName.push_back(b.getStringAttr("ci"));
        transformedShape.push_back(c);
        llvm::SmallVector<NamedAttribute, 5> cDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(2)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[2]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["ci"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("ci")}))};

        // hip wip
        curOutputDimName.push_back(b.getStringAttr("hipad"));
        curOutputDimName.push_back(b.getStringAttr("wipad"));
        transformedShape.push_back(hiPadded);
        transformedShape.push_back(wiPadded);
        llvm::SmallVector<NamedAttribute, 6> hwpadDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(3),
                                           b.getI32IntegerAttr(4)})),
            b.getNamedAttr(
                "upper_layer_names",
                b.getArrayAttr({curOutputDimName[3], curOutputDimName[4]})),
            b.getNamedAttr("transformation", b.getStringAttr("Pad")),
            b.getNamedAttr("parameters", b.getArrayAttr({
                                             b.getI32IntegerAttr(leftPadH),
                                             b.getI32IntegerAttr(rightPadH),
                                             b.getI32IntegerAttr(leftPadW),
                                             b.getI32IntegerAttr(rightPadW),
                                         })),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["hi"]),
                                b.getI32IntegerAttr(currentKeyToDim["wi"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("hi"),
                                           b.getStringAttr("wi")}))};
        auto isInputHipBoundCheck = [&]() {
          // FIXME:  wTildaSlice > wo will let stride2 backwaed data kernel
          // fail, so when (wTildaSlice > wo),  h and w dim check is must but if
          // stride =1 and wTildaSlice > wo , don't do additional check or the
          // padding kernel will fail due compiler issue
          if ((wTildaSlice > wo && strideW > 1) ||
              (hTildaSlice > ho && strideH > 1))
            return true;
          // if pad = 0 , not need oob check
          if (leftPadH == 0 && rightPadH == 0 && leftPadW == 0 &&
              rightPadW == 0)
            return false;
          // if stride = 1, slice will make it not out range
          if (strideH == 1 && strideW == 1) {
            return false;
          }
          return true;
        };
        if (isInputHipBoundCheck()) {
          llvm::SmallVector<IntegerAttr, 2> padDim;
          if (leftPadH || rightPadH || (hTildaSlice > ho && strideH > 1)) {
            inputOobCheckDims.insert(currentKeyToDim["hi"]);
          }
          if (leftPadW || rightPadW || (wTildaSlice > wo && strideW > 1)) {
            inputOobCheckDims.insert(currentKeyToDim["wi"]);
          }
        }

        transformedAttrs.push_back(b.getNamedAttr(
            "layout", b.getArrayAttr({b.getDictionaryAttr(gDimAttr),
                                      b.getDictionaryAttr(nDimAttr),
                                      b.getDictionaryAttr(cDimAttr),
                                      b.getDictionaryAttr(hwpadDimAttr)})));
        transformedAttrs.push_back(b.getNamedAttr(
            "upper_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                               curOutputDimName.end()))));

        transformedAttrs.push_back(
            b.getNamedAttr("lower_layer_layout", inputLayoutAttr));

        auto transformedMemRefType =
            MemRefType::get(transformedShape, inputElementType);
        // set lowest_layer attribute.
        transformedAttrs.push_back(
            b.getNamedAttr("lowest_layer", b.getBoolAttr(true)));
        auto gemm = b.create<miopen::TransformOp>(loc, transformedMemRefType,
                                                  op.input(), transformedAttrs,
                                                  /*populateBounds=*/true);
        return gemm;
      };
      auto inGNCHipWip = getInGNCHipWip();

      llvm::SmallVector<StringAttr, 7> secondOutputDimName;
      auto getInGNCYTildaHTildaXTildaWTilda =
          [&](decltype(firstOutputDimName) &preOutputDimName,
              decltype(secondOutputDimName) &curOutputDimName) {
            llvm::SmallVector<int64_t, 7> transformedShape;
            llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
            // g
            curOutputDimName.push_back(b.getStringAttr("gi"));
            transformedShape.push_back(g);
            llvm::SmallVector<NamedAttribute, 5> gDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[0]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[0]}))};
            // n
            curOutputDimName.push_back(b.getStringAttr("ni"));
            transformedShape.push_back(n);
            llvm::SmallVector<NamedAttribute, 5> nDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[1]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[1]}))};
            // c
            curOutputDimName.push_back(b.getStringAttr("ci"));
            transformedShape.push_back(c);
            llvm::SmallVector<NamedAttribute, 5> cDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[2]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[2]}))};

            // hi
            curOutputDimName.push_back(b.getStringAttr("ytilda"));
            curOutputDimName.push_back(b.getStringAttr("htilda"));
            transformedShape.push_back(yTilda);
            transformedShape.push_back(hTilda);
            llvm::SmallVector<NamedAttribute, 6> hiDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(4)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[3], curOutputDimName[4]})),
                b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                b.getNamedAttr("parameters", b.getArrayAttr({
                                                 b.getI32IntegerAttr(dilationH),
                                                 b.getI32IntegerAttr(strideH),
                                             })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[3]}))};

            // wi
            curOutputDimName.push_back(b.getStringAttr("xtilda"));
            curOutputDimName.push_back(b.getStringAttr("wtilda"));
            transformedShape.push_back(xTilda);
            transformedShape.push_back(wTilda);
            llvm::SmallVector<NamedAttribute, 6> wiDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(5),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[5], curOutputDimName[6]})),
                b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                b.getNamedAttr("parameters", b.getArrayAttr({
                                                 b.getI32IntegerAttr(dilationW),
                                                 b.getI32IntegerAttr(strideW),
                                             })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[4]}))};

            transformedAttrs.push_back(b.getNamedAttr(
                "layout", b.getArrayAttr({b.getDictionaryAttr(gDimAttr),
                                          b.getDictionaryAttr(nDimAttr),
                                          b.getDictionaryAttr(cDimAttr),
                                          b.getDictionaryAttr(hiDimAttr),
                                          b.getDictionaryAttr(wiDimAttr)})));
            transformedAttrs.push_back(b.getNamedAttr(
                "upper_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                                   curOutputDimName.end()))));

            transformedAttrs.push_back(b.getNamedAttr(
                "lower_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                                   preOutputDimName.end()))));

            auto transformedFilterMemRefType =
                MemRefType::get(transformedShape, inputElementType);
            auto gemm = b.create<miopen::TransformOp>(
                loc, transformedFilterMemRefType, inGNCHipWip, transformedAttrs,
                /*populateBounds=*/true);
            return gemm;
          };

      auto inGNCYTildaHTildaXTildaWTilda = getInGNCYTildaHTildaXTildaWTilda(
          firstOutputDimName, secondOutputDimName);

      llvm::SmallVector<StringAttr, 7> thirdOutputDimName;
      auto getInGNCYTildaSliceHTidaSliceXTildaSliceWTildaSlice =
          [&](decltype(secondOutputDimName) &preOutputDimName,
              llvm::SmallVector<StringAttr, 7> &curOutputDimName) {
            llvm::SmallVector<int64_t, 6> transformedShape;
            // g
            curOutputDimName.push_back(b.getStringAttr("gi"));
            transformedShape.push_back(g);
            llvm::SmallVector<NamedAttribute, 5> gDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[0]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[0]}))};

            // n
            curOutputDimName.push_back(b.getStringAttr("ni"));
            transformedShape.push_back(n);
            llvm::SmallVector<NamedAttribute, 5> nDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[1]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[1]}))};

            // c
            curOutputDimName.push_back(b.getStringAttr("ci"));
            transformedShape.push_back(c);
            llvm::SmallVector<NamedAttribute, 5> cDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[2]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[2]}))};

            // slice ytilda xtilda
            curOutputDimName.push_back(b.getStringAttr("ytildaslice"));
            curOutputDimName.push_back(b.getStringAttr("htildaslice"));
            curOutputDimName.push_back(b.getStringAttr("xtildaslice"));
            curOutputDimName.push_back(b.getStringAttr("wtildaslice"));

            transformedShape.push_back(1);
            transformedShape.push_back(hTildaSlice);
            transformedShape.push_back(1);
            transformedShape.push_back(wTildaSlice);

            llvm::SmallVector<NamedAttribute, 6> yxTildaSliceDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(5)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[3], curOutputDimName[5]})),
                b.getNamedAttr("transformation", b.getStringAttr("Slice")),
                b.getNamedAttr("begins", b.getArrayAttr({
                                             b.getI32IntegerAttr(iYTilda),
                                             b.getI32IntegerAttr(iXTilda),
                                         })),
                b.getNamedAttr("ends", b.getArrayAttr({
                                           b.getI32IntegerAttr(iYTilda + 1),
                                           b.getI32IntegerAttr(iXTilda + 1),
                                       })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(5)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[3],
                                               preOutputDimName[5]}))};

            // hw tilda slice
            llvm::SmallVector<NamedAttribute, 6> hwTildaSliceDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[4], curOutputDimName[6]})),
                b.getNamedAttr("transformation", b.getStringAttr("Slice")),
                b.getNamedAttr("begins", b.getArrayAttr({
                                             b.getI32IntegerAttr(iHTildaLeft),
                                             b.getI32IntegerAttr(iWTildaLeft),
                                         })),
                b.getNamedAttr("ends", b.getArrayAttr({
                                           b.getI32IntegerAttr(iHTildaRight),
                                           b.getI32IntegerAttr(iWTildaRight),
                                       })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[4],
                                               preOutputDimName[6]}))};

            llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
            transformedAttrs.push_back(b.getNamedAttr(
                "layout",
                b.getArrayAttr({b.getDictionaryAttr(gDimAttr),
                                b.getDictionaryAttr(nDimAttr),
                                b.getDictionaryAttr(cDimAttr),
                                b.getDictionaryAttr(yxTildaSliceDimAttr),
                                b.getDictionaryAttr(hwTildaSliceDimAttr)})));
            transformedAttrs.push_back(b.getNamedAttr(
                "upper_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                                   curOutputDimName.end()))));

            transformedAttrs.push_back(b.getNamedAttr(
                "lower_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                                   preOutputDimName.end()))));

            auto transformedMemRefType =
                MemRefType::get(transformedShape, inputElementType);
            auto gemm = b.create<miopen::TransformOp>(
                loc, transformedMemRefType, inGNCYTildaHTildaXTildaWTilda,
                transformedAttrs, /*populateBounds=*/true);
            return gemm;
          };
      auto inGNCYTildaSliceHTidaSliceXTildaSliceWTildaSlice =
          getInGNCYTildaSliceHTidaSliceXTildaSliceWTildaSlice(
              secondOutputDimName, thirdOutputDimName);

      auto getInGemmGGemmMGemmN = [&](decltype(
                                      thirdOutputDimName) &preOutputDimName) {
        llvm::SmallVector<StringAttr, 7> curOutputDimName;
        llvm::SmallVector<int64_t, 7> transformedShape;
        // gemmG
        curOutputDimName.push_back(b.getStringAttr("gemmG"));
        transformedShape.push_back(g);
        llvm::SmallVector<NamedAttribute, 5> gemmGDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[0]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr("lower_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({preOutputDimName[0]}))};

        // gemmM
        curOutputDimName.push_back(b.getStringAttr("gemmM"));
        transformedShape.push_back(c * 1 * 1);
        llvm::SmallVector<NamedAttribute, 5> gemmMDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(1)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[1]})),
            b.getNamedAttr("transformation", b.getStringAttr("Merge")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(2), b.getI32IntegerAttr(3),
                                b.getI32IntegerAttr(5)})),
            b.getNamedAttr(
                "lower_layer_names",
                b.getArrayAttr({preOutputDimName[2], preOutputDimName[3],
                                preOutputDimName[5]}))};

        // gemmN
        curOutputDimName.push_back(b.getStringAttr("gemmN"));
        transformedShape.push_back(n * hTildaSlice * wTildaSlice);
        llvm::SmallVector<NamedAttribute, 5> gemmNDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(2)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[2]})),
            b.getNamedAttr("transformation", b.getStringAttr("Merge")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(1), b.getI32IntegerAttr(4),
                                b.getI32IntegerAttr(6)})),
            b.getNamedAttr(
                "lower_layer_names",
                b.getArrayAttr({preOutputDimName[1], preOutputDimName[4],
                                preOutputDimName[6]}))};

        llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
        transformedAttrs.push_back(b.getNamedAttr(
            "layout", b.getArrayAttr({b.getDictionaryAttr(gemmGDimAttr),
                                      b.getDictionaryAttr(gemmMDimAttr),
                                      b.getDictionaryAttr(gemmNDimAttr)})));
        transformedAttrs.push_back(b.getNamedAttr(
            "upper_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                               curOutputDimName.end()))));

        transformedAttrs.push_back(b.getNamedAttr(
            "lower_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                               preOutputDimName.end()))));

        transformedAttrs.push_back(b.getNamedAttr(
            "gridwise_gemm_argument_position", b.getI32IntegerAttr(2)));

        if (inputOobCheckDims.size()) {
          llvm::SmallVector<IntegerAttr, 5> boundDims;
          for (size_t i = 0; i < inputShape.size(); i++) {
            if (inputOobCheckDims.find(i) != inputOobCheckDims.end())
              boundDims.push_back(b.getI32IntegerAttr(1));
            else
              boundDims.push_back(b.getI32IntegerAttr(0));
          }
          transformedAttrs.push_back(b.getNamedAttr(
              "bound_check",
              b.getArrayAttr({boundDims.begin(), boundDims.end()})));
        }

        auto transformedMemRefType =
            MemRefType::get(transformedShape, inputElementType);
        Value gemm = b.create<miopen::TransformOp>(
            loc, transformedMemRefType,
            inGNCYTildaSliceHTidaSliceXTildaSliceWTildaSlice, transformedAttrs,
            /*populateBounds=*/true);

        // if the config do not do padding kernel,
        // gemmBPad = gemm

        Value gemmBPad =
            padInput(isXdlops, gemmMExtra, gemmNExtra, gemmKExtra, gemm, b, loc,
                     inputOobCheckDims, nameToDims, transformedShape,
                     inputShape, inputElementType);

        return gemmBPad;
      };
      auto inGemmGGemmMGemmN = getInGemmGGemmMGemmN(thirdOutputDimName);

      return inGemmGGemmMGemmN;
    };

    auto getGemmC = [&]() -> Value {
      // dim of oob check
      llvm::DenseSet<int> outputOobCheckDims;
      // key to dim
      std::map<StringRef, int> currentKeyToDim;
      for (unsigned i = 0; i < outputLayoutAttr.size(); ++i) {
        if (auto strAttr =
                outputLayoutAttr.getValue()[i].template cast<StringAttr>()) {
          currentKeyToDim[strAttr.getValue()] = i;
        }
      }

      // wei_g_k_c_ydot_ytilda_xdot_xtilda
      llvm::SmallVector<StringAttr, 7> firstOutputDimName;
      auto getOutGNKYDotHTildaXDotWHilda = [&]() {
        decltype(firstOutputDimName) &curOutputDimName = firstOutputDimName;
        llvm::SmallVector<int64_t, 7> transformedShape;
        llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
        // go
        curOutputDimName.push_back(b.getStringAttr("go"));
        transformedShape.push_back(g);
        llvm::SmallVector<NamedAttribute, 5> gDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[0]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["go"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("go")}))};
        // no
        curOutputDimName.push_back(b.getStringAttr("no"));
        transformedShape.push_back(n);
        llvm::SmallVector<NamedAttribute, 5> nDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(1)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[1]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["no"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("no")}))};
        // ko
        curOutputDimName.push_back(b.getStringAttr("ko"));
        transformedShape.push_back(k);
        llvm::SmallVector<NamedAttribute, 5> kDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(2)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[2]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["ko"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("ko")}))};

        // ho
        curOutputDimName.push_back(b.getStringAttr("ydot"));
        curOutputDimName.push_back(b.getStringAttr("htilda"));
        transformedShape.push_back(yDot);
        transformedShape.push_back(hTilda);
        llvm::SmallVector<NamedAttribute, 6> hoDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(3),
                                           b.getI32IntegerAttr(4)})),
            b.getNamedAttr(
                "upper_layer_names",
                b.getArrayAttr({curOutputDimName[3], curOutputDimName[4]})),
            b.getNamedAttr("transformation", b.getStringAttr("Embed")),
            b.getNamedAttr(
                "parameters",
                b.getArrayAttr({
                    b.getI32IntegerAttr((-dilationH) / gcdStrideDilationH),
                    b.getI32IntegerAttr(1),
                })),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["ho"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("ho")}))};

        if (y > 1) {
          if (!((leftPadH == rightPadH) && (y - leftPadH == 1))) {
            outputOobCheckDims.insert(currentKeyToDim["ho"]);
          }
        }
        // wo
        curOutputDimName.push_back(b.getStringAttr("xdot"));
        curOutputDimName.push_back(b.getStringAttr("wtilda"));
        transformedShape.push_back(xDot);
        transformedShape.push_back(wTilda);
        llvm::SmallVector<NamedAttribute, 6> woDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(5),
                                           b.getI32IntegerAttr(6)})),
            b.getNamedAttr(
                "upper_layer_names",
                b.getArrayAttr({curOutputDimName[5], curOutputDimName[6]})),
            b.getNamedAttr("transformation", b.getStringAttr("Embed")),
            b.getNamedAttr(
                "parameters",
                b.getArrayAttr({
                    b.getI32IntegerAttr((-dilationW) / gcdStrideDilationW),
                    b.getI32IntegerAttr(1),
                })),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(currentKeyToDim["wo"])})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({b.getStringAttr("wo")}))};

        if (x > 1) {
          if (!((leftPadW == rightPadW) && (x - leftPadW == 1))) {
            outputOobCheckDims.insert(currentKeyToDim["wo"]);
          }
        }
        transformedAttrs.push_back(b.getNamedAttr(
            "layout",
            b.getArrayAttr(
                {b.getDictionaryAttr(gDimAttr), b.getDictionaryAttr(nDimAttr),
                 b.getDictionaryAttr(kDimAttr), b.getDictionaryAttr(hoDimAttr),
                 b.getDictionaryAttr(woDimAttr)})));
        transformedAttrs.push_back(b.getNamedAttr(
            "upper_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                               curOutputDimName.end()))));

        transformedAttrs.push_back(
            b.getNamedAttr("lower_layer_layout", outputLayoutAttr));

        auto transformedFilterMemRefType =
            MemRefType::get(transformedShape, outputElementType);
        // set lowest_layer attribute.
        transformedAttrs.push_back(
            b.getNamedAttr("lowest_layer", b.getBoolAttr(true)));
        auto gemm = b.create<miopen::TransformOp>(
            loc, transformedFilterMemRefType, op.output(), transformedAttrs,
            /*populateBounds=*/true);
        return gemm;
      };

      auto outGNKYDotHTildaXDotWHilda = getOutGNKYDotHTildaXDotWHilda();

      llvm::SmallVector<StringAttr, 7> secondOutputDimName;
      auto getOutGNKYDotSliceHTidaSliceXDotSliceWTildaSlice =
          [&](decltype(firstOutputDimName) &preOutputDimName,
              llvm::SmallVector<StringAttr, 7> &curOutputDimName) {
            llvm::SmallVector<int64_t, 6> transformedShape;
            // go
            curOutputDimName.push_back(b.getStringAttr("go"));
            transformedShape.push_back(g);
            llvm::SmallVector<NamedAttribute, 5> gDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[0]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(0)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[0]}))};

            // no
            curOutputDimName.push_back(b.getStringAttr("no"));
            transformedShape.push_back(n);
            llvm::SmallVector<NamedAttribute, 5> nDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[1]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(1)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[1]}))};

            // ko
            curOutputDimName.push_back(b.getStringAttr("ko"));
            transformedShape.push_back(k);
            llvm::SmallVector<NamedAttribute, 5> kDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("upper_layer_names",
                               b.getArrayAttr({curOutputDimName[2]})),
                b.getNamedAttr("transformation",
                               b.getStringAttr("PassThrough")),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(2)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[2]}))};

            // slice ydot xdot
            curOutputDimName.push_back(b.getStringAttr("ydotslice"));
            curOutputDimName.push_back(b.getStringAttr("htildaslice"));
            curOutputDimName.push_back(b.getStringAttr("xdotslice"));
            curOutputDimName.push_back(b.getStringAttr("wtildaslice"));

            transformedShape.push_back(yDotSlice);
            transformedShape.push_back(hTildaSlice);
            transformedShape.push_back(xDotSlice);
            transformedShape.push_back(wTildaSlice);

            llvm::SmallVector<NamedAttribute, 6> yxDotSliceDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(5)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[3], curOutputDimName[5]})),
                b.getNamedAttr("transformation", b.getStringAttr("Slice")),
                b.getNamedAttr("begins", b.getArrayAttr({
                                             b.getI32IntegerAttr(0),
                                             b.getI32IntegerAttr(0),
                                         })),
                b.getNamedAttr("ends", b.getArrayAttr({
                                           b.getI32IntegerAttr(yDotSlice),
                                           b.getI32IntegerAttr(xDotSlice),
                                       })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(3),
                                               b.getI32IntegerAttr(5)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[3],
                                               preOutputDimName[5]}))};

            // hw tilda slice
            llvm::SmallVector<NamedAttribute, 6> hwTildaSliceDimAttr{
                b.getNamedAttr("upper_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr(
                    "upper_layer_names",
                    b.getArrayAttr({curOutputDimName[4], curOutputDimName[6]})),
                b.getNamedAttr("transformation", b.getStringAttr("Slice")),
                b.getNamedAttr("begins", b.getArrayAttr({
                                             b.getI32IntegerAttr(iHTildaLeft),
                                             b.getI32IntegerAttr(iWTildaLeft),
                                         })),
                b.getNamedAttr("ends", b.getArrayAttr({
                                           b.getI32IntegerAttr(iHTildaRight),
                                           b.getI32IntegerAttr(iWTildaRight),
                                       })),
                b.getNamedAttr("lower_layer_dimensions",
                               b.getArrayAttr({b.getI32IntegerAttr(4),
                                               b.getI32IntegerAttr(6)})),
                b.getNamedAttr("lower_layer_names",
                               b.getArrayAttr({preOutputDimName[4],
                                               preOutputDimName[6]}))};

            llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
            transformedAttrs.push_back(b.getNamedAttr(
                "layout",
                b.getArrayAttr({b.getDictionaryAttr(gDimAttr),
                                b.getDictionaryAttr(nDimAttr),
                                b.getDictionaryAttr(kDimAttr),
                                b.getDictionaryAttr(yxDotSliceDimAttr),
                                b.getDictionaryAttr(hwTildaSliceDimAttr)})));
            transformedAttrs.push_back(b.getNamedAttr(
                "upper_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                                   curOutputDimName.end()))));

            transformedAttrs.push_back(b.getNamedAttr(
                "lower_layer_layout",
                b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                                   preOutputDimName.end()))));

            auto transformedMemRefType =
                MemRefType::get(transformedShape, outputElementType);
            auto gemm = b.create<miopen::TransformOp>(
                loc, transformedMemRefType, outGNKYDotHTildaXDotWHilda,
                transformedAttrs, /*populateBounds=*/true);
            return gemm;
          };
      auto outGNKYDotSliceHTidaSliceXDotSliceWTildaSlice =
          getOutGNKYDotSliceHTidaSliceXDotSliceWTildaSlice(firstOutputDimName,
                                                           secondOutputDimName);

      auto getOutGemmGGemmKGemmN = [&](decltype(
                                       secondOutputDimName) &preOutputDimName) {
        llvm::SmallVector<StringAttr, 7> curOutputDimName;
        llvm::SmallVector<int64_t, 7> transformedShape;
        // gemmG
        curOutputDimName.push_back(b.getStringAttr("gemmG"));
        transformedShape.push_back(g);
        llvm::SmallVector<NamedAttribute, 5> gemmGDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[0]})),
            b.getNamedAttr("transformation", b.getStringAttr("PassThrough")),
            b.getNamedAttr("lower_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(0)})),
            b.getNamedAttr("lower_layer_names",
                           b.getArrayAttr({preOutputDimName[0]}))};

        // gemmK
        curOutputDimName.push_back(b.getStringAttr("gemmK"));
        transformedShape.push_back(k * yDotSlice * xDotSlice);
        llvm::SmallVector<NamedAttribute, 5> gemmKDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(1)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[1]})),
            b.getNamedAttr("transformation", b.getStringAttr("Merge")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(2), b.getI32IntegerAttr(3),
                                b.getI32IntegerAttr(5)})),
            b.getNamedAttr(
                "lower_layer_names",
                b.getArrayAttr({preOutputDimName[2], preOutputDimName[3],
                                preOutputDimName[5]}))};

        // gemmN
        curOutputDimName.push_back(b.getStringAttr("gemmN"));
        transformedShape.push_back(n * hTildaSlice * wTildaSlice);
        llvm::SmallVector<NamedAttribute, 5> gemmNDimAttr{
            b.getNamedAttr("upper_layer_dimensions",
                           b.getArrayAttr({b.getI32IntegerAttr(2)})),
            b.getNamedAttr("upper_layer_names",
                           b.getArrayAttr({curOutputDimName[2]})),
            b.getNamedAttr("transformation", b.getStringAttr("Merge")),
            b.getNamedAttr(
                "lower_layer_dimensions",
                b.getArrayAttr({b.getI32IntegerAttr(1), b.getI32IntegerAttr(4),
                                b.getI32IntegerAttr(6)})),
            b.getNamedAttr(
                "lower_layer_names",
                b.getArrayAttr({preOutputDimName[1], preOutputDimName[4],
                                preOutputDimName[6]}))};

        llvm::SmallVector<NamedAttribute, 3> transformedAttrs;
        transformedAttrs.push_back(b.getNamedAttr(
            "layout", b.getArrayAttr({b.getDictionaryAttr(gemmGDimAttr),
                                      b.getDictionaryAttr(gemmKDimAttr),
                                      b.getDictionaryAttr(gemmNDimAttr)})));
        transformedAttrs.push_back(b.getNamedAttr(
            "upper_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(curOutputDimName.begin(),
                                               curOutputDimName.end()))));

        transformedAttrs.push_back(b.getNamedAttr(
            "lower_layer_layout",
            b.getArrayAttr(ArrayRef<Attribute>(preOutputDimName.begin(),
                                               preOutputDimName.end()))));

        transformedAttrs.push_back(b.getNamedAttr(
            "gridwise_gemm_argument_position", b.getI32IntegerAttr(1)));

        if (outputOobCheckDims.size()) {
          llvm::SmallVector<IntegerAttr, 5> boundDims;
          for (size_t i = 0; i < outputShape.size(); i++) {
            if (outputOobCheckDims.find(i) != outputOobCheckDims.end())
              boundDims.push_back(b.getI32IntegerAttr(1));
            else
              boundDims.push_back(b.getI32IntegerAttr(0));
          }
          transformedAttrs.push_back(b.getNamedAttr(
              "bound_check",
              b.getArrayAttr({boundDims.begin(), boundDims.end()})));
        }
        auto transformedMemRefType =
            MemRefType::get(transformedShape, outputElementType);
        Value gemm = b.create<miopen::TransformOp>(
            loc, transformedMemRefType,
            outGNKYDotSliceHTidaSliceXDotSliceWTildaSlice, transformedAttrs,
            /*populateBounds=*/true);

        // if the config do not do padding kernel,
        // gemmCPad = gemm
        Value gemmCPad =
            padOutput(isXdlops, gemmMExtra, gemmNExtra, gemmKExtra, gemm, b,
                      loc, outputOobCheckDims, nameToDims, transformedShape,
                      outputShape, outputElementType);
        return gemmCPad;
      };
      auto outGemmGGemmKGemmN = getOutGemmGGemmKGemmN(secondOutputDimName);

      return outGemmGGemmKGemmN;
    };
    Value gemmA = getGemmA();

    Value gemmB = getGemmB();

    Value gemmC = getGemmC();

    // Set attributes for gridwise_gemm op.
    llvm::SmallVector<NamedAttribute, 8> gridwiseGemmAttrs{
        b.getNamedAttr("gemm_id", gemmIdAttr),
        b.getNamedAttr("arch", archAttr),
        b.getNamedAttr("num_cu", numCuAttr),
        b.getNamedAttr("filter_layout", filterLayoutAttr),
        b.getNamedAttr("filter_dimension", b.getI64ArrayAttr(filterShape)),
        b.getNamedAttr("input_layout", inputLayoutAttr),
        b.getNamedAttr("input_dimension", b.getI64ArrayAttr(inputShape)),
        b.getNamedAttr("output_layout", outputLayoutAttr),
        b.getNamedAttr("output_dimension", b.getI64ArrayAttr(outputShape)),
        b.getNamedAttr("dilations", dilationsAttr),
        b.getNamedAttr("strides", stridesAttr),
        b.getNamedAttr("padding", paddingAttr),
    };

    // xdlopsV2.
    if (isXdlops)
      gridwiseGemmAttrs.push_back(
          b.getNamedAttr("xdlopsV2", b.getBoolAttr(true)));

    gridwiseGemmAttrs.push_back(b.getNamedAttr(
        "kernel_algorithm", b.getStringAttr("backward_data_v4r1")));

    // Emit miopen.gridwise_gemm op.
    // Emit miopen.gridwise_gemm_v2 if xdlopsV2 attribute is true.
    auto arguments = SmallVector<Value, 3>{gemmA, gemmB, gemmC};

    if (xdlopsV2Attr && xdlopsV2Attr.getValue() == true) {
      auto gop = b.create<miopen::GridwiseGemmV2Op>(
          loc, ArrayRef<Type>{},
          ValueRange{arguments[fields.gridwiseGemmArgumentPosition[0]],
                     arguments[fields.gridwiseGemmArgumentPosition[1]],
                     arguments[fields.gridwiseGemmArgumentPosition[2]]},
          gridwiseGemmAttrs);
      affixGridwiseGemmAttributes(op, gop, b);
    } else {
      auto gop = b.create<miopen::GridwiseGemmOp>(
          loc, ArrayRef<Type>{},
          ValueRange{arguments[fields.gridwiseGemmArgumentPosition[0]],
                     arguments[fields.gridwiseGemmArgumentPosition[1]],
                     arguments[fields.gridwiseGemmArgumentPosition[2]]},
          gridwiseGemmAttrs);
      affixGridwiseGemmAttributes(op, gop, b);
    }
    // Finally, erase the original Conv2D op.
    op.erase();

    return success();
  }
};

// Forward-declare field values to suppress warnings
template <> const ArgumentFields Conv2DRewritePattern<miopen::Conv2DOp>::fields;
template <>
const miopen::ConvOpType Conv2DRewritePattern<miopen::Conv2DOp>::convOpType;
template <>
const ArgumentFields Conv2DRewritePattern<miopen::Conv2DBwdDataOp>::fields;
template <>
const miopen::ConvOpType
    Conv2DRewritePattern<miopen::Conv2DBwdDataOp>::convOpType;
template <>
const ArgumentFields Conv2DRewritePattern<miopen::Conv2DBwdWeightOp>::fields;
template <>
const miopen::ConvOpType
    Conv2DRewritePattern<miopen::Conv2DBwdWeightOp>::convOpType;
//===----------------------------------------------------------------------===//
// Assigning attributes.
//===----------------------------------------------------------------------===//

static void affixThreadwiseCopyAttributes(miopen::ThreadwiseCopyOp top,
                                          miopen::GridwiseGemmOp gop,
                                          OpBuilder &b) {
  //  gop.getNamedAttr("padding", paddingAttr)
  auto stridesAttr = gop->template getAttrOfType<ArrayAttr>("strides");
  auto strideH =
      stridesAttr.getValue()[0].template cast<IntegerAttr>().getInt();
  auto strideW =
      stridesAttr.getValue()[1].template cast<IntegerAttr>().getInt();

  auto algorithmAttr =
      gop->template getAttrOfType<StringAttr>("kernel_algorithm");
  auto algorithm_name = algorithmAttr.getValue();

  BwdPaddingKernelStatus status =
      BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
  if (strideH > 1 && strideW > 1 && algorithm_name == "backward_data_v4r1") {
    status = BwdPaddingKernelStatus::StrideTwoNonXdlopsNHWC;
    auto inputLayoutAttr =
        gop->template getAttrOfType<ArrayAttr>("input_layout");
    for (unsigned i = 0; i < inputLayoutAttr.size(); ++i) {
      auto inputAttr =
          inputLayoutAttr.getValue()[i].template cast<StringAttr>();
      if (inputAttr.getValue() == "ci") {
        if (i == 2) // gnchw
          status = BwdPaddingKernelStatus::StrideTwoNonXdlopsNCHW;
      }
    }
  }

  top->setAttr("dim_access_order", b.getI32ArrayAttr({0, 1, 2, 3, 4}));
  // NOTE: these are currently ignored by ThreadwiseCopy
  top->setAttr("vector_read_write_dim",
               gop->getAttr("matrix_c_dest_vector_write_dim"));
  top->setAttr("source_data_per_read", gop->getAttr("matrix_c_data_per_copy"));
  top->setAttr("bwd_padding_kernel_status", b.getI32IntegerAttr(status));
  top->setAttr("dest_data_per_write", gop->getAttr("matrix_c_data_per_copy"));
}

static void affixThreadwiseCopyV2Attributes(miopen::ThreadwiseCopyV2Op top,
                                            miopen::GridwiseGemmV2Op gop,
                                            OpBuilder &b, bool isSwizzled) {
  auto stridesAttr = gop->template getAttrOfType<ArrayAttr>("strides");
  auto strideH =
      stridesAttr.getValue()[0].template cast<IntegerAttr>().getInt();
  auto strideW =
      stridesAttr.getValue()[1].template cast<IntegerAttr>().getInt();

  auto algorithmAttr =
      gop->template getAttrOfType<StringAttr>("kernel_algorithm");
  auto algorithm_name = algorithmAttr.getValue();

  BwdPaddingKernelStatus status =
          BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
  if ((strideH > 1 && strideW > 1) && algorithm_name == "backward_data_v4r1") {

    status = BwdPaddingKernelStatus::StrideTwoXdlopsNHWC;
    auto inputLayoutAttr =
        gop->template getAttrOfType<ArrayAttr>("input_layout");
    for (unsigned i = 0; i < inputLayoutAttr.size(); ++i) {
      auto inputAttr =
          inputLayoutAttr.getValue()[i].template cast<StringAttr>();
      if (inputAttr.getValue() == "ci") {
        if (i == 2) // gnchw
          status = BwdPaddingKernelStatus::StrideTwoXdlopsNCHW;
      }
    }
  }

  top->setAttr("bwd_padding_kernel_status", b.getI32IntegerAttr(status));
  if (isSwizzled) {
    top->setAttr("dim_access_order", b.getI32ArrayAttr({0, 1, 2, 3, 4, 5}));
  } else {
    top->setAttr("dim_access_order", b.getI32ArrayAttr({0, 1, 2, 3, 4}));
  }

  // Account for split m/n dimension
  bool vectorStoreOverride = false;
  int64_t vectorGemmDim =
      gop->getAttrOfType<IntegerAttr>("matrix_c_source_vector_read_dim")
          .getInt();
  // Remap vectorized gemm dimensions to account for
  if (vectorGemmDim == gemmCDimM) {
    vectorGemmDim = gemmCSplitDimM2;
  } else if (vectorGemmDim == gemmCDimN) {
    if (isSwizzled) {
      vectorGemmDim = gemmCSplitDimN2;
    } else {
      vectorGemmDim = gemmCSplitDimN;
      // Need swizzles for this to be vector motion but swizzles are off
      vectorStoreOverride = true;
    }
  }
  Attribute dataPerCopy = gop->getAttr("matrix_c_data_per_copy");
  if (vectorStoreOverride) {
    dataPerCopy = b.getI32IntegerAttr(1);
  }
  top->setAttr("upper_vector_read_dim", b.getI32IntegerAttr(vectorGemmDim));
  top->setAttr("vector_read_write_dim",
               gop->getAttr("matrix_c_dest_vector_write_dim"));
  top->setAttr("source_data_per_read", dataPerCopy);
  top->setAttr("dest_data_per_write", dataPerCopy);
}

// XXX: Figure out a way to do away with isThreadwiseLoad parameter.
template <typename T, typename U>
static void affixThreadwiseCopyAttributes(T &top, U &bop, OpBuilder &b,
                                          bool isThreadwiseLoad) {
  if (isThreadwiseLoad) {
    top->setAttr("dim_access_order", bop->getAttr("source_dim_access_order"));
    top->setAttr("vector_read_write_dim",
                 bop->getAttr("source_vector_read_dim"));
    top->setAttr("source_data_per_read", bop->getAttr("source_data_per_read"));
    top->setAttr("dest_data_per_write", bop->getAttr("dest_data_per_write"));
  } else {
    top->setAttr("dim_access_order", bop->getAttr("dest_dim_access_order"));
    top->setAttr("vector_read_write_dim",
                 bop->getAttr("dest_vector_write_dim"));
    top->setAttr("source_data_per_read", bop->getAttr("source_data_per_read"));
    top->setAttr("dest_data_per_write", bop->getAttr("dest_data_per_write"));
  }
  // set bound attribute.
  top->setAttr("bound", bop->getAttr("bound"));
}

// XXX: figure out a better way to get rid of isMatrixA parameter.
static void affixThreadwiseCopyAttributes(miopen::ThreadwiseCopyOp top,
                                          miopen::BlockwiseGemmOp bop,
                                          OpBuilder &b, bool isMatrixA) {
  if (isMatrixA) {
    top->setAttr("n_slice_row", bop->getAttr("k_per_thread"));
    top->setAttr("n_slice_col", bop->getAttr("m_per_thread"));
    // XXX: TBD review how vector load/store attributes are passed down.
    // top->setAttr("data_per_access", bop->getAttr("m_per_thread"));
    top->setAttr("data_per_access", b.getI32IntegerAttr(1));
  } else {
    top->setAttr("n_slice_row", bop->getAttr("k_per_thread"));
    top->setAttr("n_slice_col", bop->getAttr("n_per_thread"));
    // XXX: TBD review how vector load/store attributes are passed down.
    // top->setAttr("data_per_access", bop->getAttr("n_per_thread"));
    top->setAttr("data_per_access", b.getI32IntegerAttr(1));
  }
}

template <typename T, typename U>
void affixBlockwiseCopyAttributes(
    T &bop, U &gop, OpBuilder &b,
    const SmallVector<uint64_t, 3> &blockwiseCopyBounds, int vectorDim,
    int blockwiseLoadLength, int blockwiseStoreLength) {
  bop->setAttr("block_size", gop->getAttr("block_size"));

  bop->setAttr("source_dim_access_order", b.getI32ArrayAttr({0, 1, 2}));
  bop->setAttr("dest_dim_access_order", b.getI32ArrayAttr({0, 1, 2}));
  bop->setAttr("source_vector_read_dim", b.getI32IntegerAttr(vectorDim));
  bop->setAttr("dest_vector_write_dim", b.getI32IntegerAttr(vectorDim));
  bop->setAttr("source_data_per_read",
               b.getI32IntegerAttr(blockwiseLoadLength));
  bop->setAttr("dest_data_per_write",
               b.getI32IntegerAttr(blockwiseStoreLength));

  // set bound attribute.
  SmallVector<Attribute, 2> blockwiseCopyBoundsAttr;
  for (auto v : blockwiseCopyBounds) {
    blockwiseCopyBoundsAttr.push_back(b.getI32IntegerAttr(v));
  }
  bop->setAttr("bound", b.getArrayAttr(blockwiseCopyBoundsAttr));
}

template <typename T, typename U>
void affixGridwiseGemmAttributes(T &convOp, U &gop, OpBuilder &b) {
  gop->setAttr("block_size", convOp->getAttr("block_size"));
  gop->setAttr("m_per_block", convOp->getAttr("m_per_block"));
  gop->setAttr("n_per_block", convOp->getAttr("n_per_block"));
  gop->setAttr("k_per_block", convOp->getAttr("k_per_block"));
  gop->setAttr("matrix_a_dest_data_per_write_dim_m",
               convOp->getAttr("matrix_a_dest_data_per_write_dim_m"));
  gop->setAttr("matrix_a_source_data_per_read",
               convOp->getAttr("matrix_a_source_data_per_read"));
  gop->setAttr("matrix_a_source_vector_read_dim",
               convOp->getAttr("matrix_a_source_vector_read_dim"));
  gop->setAttr("matrix_b_dest_data_per_write_dim_n",
               convOp->getAttr("matrix_b_dest_data_per_write_dim_n"));
  gop->setAttr("matrix_b_source_data_per_read",
               convOp->getAttr("matrix_b_source_data_per_read"));
  gop->setAttr("matrix_b_source_vector_read_dim",
               convOp->getAttr("matrix_b_source_vector_read_dim"));
  gop->setAttr("matrix_c_data_per_copy",
               convOp->getAttr("matrix_c_data_per_copy"));
  gop->setAttr("matrix_c_dest_vector_write_dim",
               convOp->getAttr("matrix_c_dest_vector_write_dim"));
  gop->setAttr("matrix_c_source_vector_read_dim",
               convOp->getAttr("matrix_c_source_vector_read_dim"));

  auto xdlopsV2Attr = convOp->template getAttrOfType<BoolAttr>("xdlopsV2");
  if (xdlopsV2Attr && xdlopsV2Attr.getValue() == true) {
    gop->setAttr("m_per_wave", convOp->getAttr("m_per_wave"));
    gop->setAttr("n_per_wave", convOp->getAttr("n_per_wave"));
  } else {
    gop->setAttr("m_per_thread", convOp->getAttr("m_per_thread"));
    gop->setAttr("n_per_thread", convOp->getAttr("n_per_thread"));
    gop->setAttr("k_per_thread", convOp->getAttr("k_per_thread"));
    gop->setAttr("m_level0_cluster", convOp->getAttr("m_level0_cluster"));
    gop->setAttr("m_level1_cluster", convOp->getAttr("m_level1_cluster"));
    gop->setAttr("n_level0_cluster", convOp->getAttr("n_level0_cluster"));
    gop->setAttr("n_level1_cluster", convOp->getAttr("n_level1_cluster"));
  }
}

//===----------------------------------------------------------------------===//
// GridwiseGemm lowering.
//===----------------------------------------------------------------------===//

struct GridwiseGemmRewritePattern : public OpRewritePattern<miopen::GridwiseGemmOp> {
  using OpRewritePattern<miopen::GridwiseGemmOp>::OpRewritePattern;

  void computeLDSBlockSizes(miopen::GridwiseGemmOp op, int64_t &a_block_space,
                            int64_t &b_block_space,
                            int64_t &block_space) const {
    int64_t ABlockCopyDstDataPerWrite_M =
        op->getAttr("matrix_a_dest_data_per_write_dim_m")
            .template cast<IntegerAttr>()
            .getInt();
    int64_t BBlockCopyDstDataPerWrite_N =
        op->getAttr("matrix_b_dest_data_per_write_dim_n")
            .template cast<IntegerAttr>()
            .getInt();
    int64_t ThreadGemmAThreadCopySrcDataPerRead_M =
        op->getAttr("m_per_thread").template cast<IntegerAttr>().getInt();
    int64_t ThreadGemmBThreadCopySrcDataPerRead_N =
        op->getAttr("n_per_thread").template cast<IntegerAttr>().getInt();

    int64_t max_lds_align =
        math_util::lcm(ABlockCopyDstDataPerWrite_M, BBlockCopyDstDataPerWrite_N,
                  ThreadGemmAThreadCopySrcDataPerRead_M,
                  ThreadGemmBThreadCopySrcDataPerRead_N);

    int64_t KPerBlock =
        op->getAttr("k_per_block").template cast<IntegerAttr>().getInt();
    int64_t MPerBlock =
        op->getAttr("m_per_block").template cast<IntegerAttr>().getInt();
    int64_t NPerBlock =
        op->getAttr("n_per_block").template cast<IntegerAttr>().getInt();

    int64_t AlignedNPerBlock =
        max_lds_align *
        math_util::integer_divide_ceil<int64_t>(NPerBlock, max_lds_align);

    // A matrix in LDS memory, dst of blockwise copy
    //   be careful of LDS alignment
    // Original C++ logic:
    // constexpr auto a_k_m_block_desc = make_native_tensor_descriptor_aligned(
    //    Sequence<KPerBlock, MPerBlock>{}, Number<max_lds_align>{});
    // constexpr index_t a_block_space =
    //    math_util::integer_least_multiple(a_k_m_block_desc.GetElementSpace(),
    //    max_lds_align);
    int64_t AlignedMPerBlock =
        max_lds_align *
        math_util::integer_divide_ceil<int64_t>(MPerBlock, max_lds_align);
    a_block_space = math_util::integer_least_multiple(KPerBlock * AlignedMPerBlock,
                                                 max_lds_align);

    // B matrix in LDS memory, dst of blockwise copy
    //   be careful of LDS alignment
    // Original C++ logic:
    // constexpr auto b_k_n_block_desc = make_native_tensor_descriptor_aligned(
    //    Sequence<KPerBlock, NPerBlock>{}, Number<max_lds_align>{});
    // constexpr index_t b_block_space =
    //    math_util::integer_least_multiple(b_k_n_block_desc.GetElementSpace(),
    //    max_lds_align);
    b_block_space = math_util::integer_least_multiple(KPerBlock * AlignedNPerBlock,
                                                 max_lds_align);

    block_space = a_block_space + b_block_space;

    // llvm::errs() << "a_block_space: " << a_block_space << "\n";
    // llvm::errs() << "b_block_space: " << b_block_space << "\n";
    // llvm::errs() << "double_block_space: " << double_block_space << "\n\n";
  }

  void affixBlockwiseGemmAttributes(miopen::BlockwiseGemmOp bop,
                                    miopen::GridwiseGemmOp gop,
                                    OpBuilder &b) const {
    bop->setAttr("block_size", gop->getAttr("block_size"));
    // Attributes used in non-xdlops lowering path.
    bop->setAttr("m_per_thread", gop->getAttr("m_per_thread"));
    bop->setAttr("n_per_thread", gop->getAttr("n_per_thread"));
    bop->setAttr("k_per_thread", gop->getAttr("k_per_thread"));
    bop->setAttr("m_level0_cluster", gop->getAttr("m_level0_cluster"));
    bop->setAttr("m_level1_cluster", gop->getAttr("m_level1_cluster"));
    bop->setAttr("n_level0_cluster", gop->getAttr("n_level0_cluster"));
    bop->setAttr("n_level1_cluster", gop->getAttr("n_level1_cluster"));
  }

  template <typename T>
  MemRefType computeSubviewResultType(T &op, MemRefType inputType,
                                      unsigned offset,
                                      ArrayRef<int64_t> outputShape,
                                      Type outputElementType) const {
    auto outputRank = outputShape.size();

    auto expr = getAffineConstantExpr(offset, op.getContext());
    unsigned stride = 1;
    for (int i = outputRank - 1; i >= 0; --i) {
      expr = expr + getAffineDimExpr(i, op.getContext()) *
                        getAffineConstantExpr(stride, op.getContext());
      stride *= outputShape[i];
    }

    AffineMap transformAffineMap = AffineMap::get(
        outputRank, 0, ArrayRef<AffineExpr>{expr}, op.getContext());
    AffineMap inputAffineMap = inputType.getLayout().getAffineMap();
    AffineMap outputAffineMap = inputAffineMap.compose(transformAffineMap);
    auto transformedOutputType =
        MemRefType::get(outputShape, outputElementType, {outputAffineMap},
                        inputType.getMemorySpaceAsInt());
    return transformedOutputType;
  }

  LogicalResult matchAndRewrite(miopen::GridwiseGemmOp op, PatternRewriter &b) const override {
    auto loc = op.getLoc();

    // Determine the type used in the filter/input/output tensors.
    auto elementType = op.output()
                           .getType()
                           .cast<MemRefType>()
                           .getElementType()
                           .template cast<Type>();

    // Determine the type used on VGPR to act as accumulator.
    // f32: f32.
    // f16: f32 to prevent overflow from happening.
    // i16(bf16) : i16.
    Type accumulatorType = elementType;
    if (elementType == b.getF16Type()) {
      accumulatorType = b.getF32Type();
    }

    // Prepare some useful constants.
    Value zeroConstantFloatOp =
        createZeroConstantFloatOp(b, loc, accumulatorType);
    auto zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);

    // Obtain critical matrix dimensions.
    int64_t G = op.filter().getType().template cast<MemRefType>().getShape()[0];
    int64_t K = op.filter().getType().template cast<MemRefType>().getShape()[1];
    int64_t M = op.filter().getType().template cast<MemRefType>().getShape()[2];
    int64_t N = op.input().getType().template cast<MemRefType>().getShape()[2];

    // Obtain critical tuning parameters.
    int64_t BlockSize =
        op->getAttr("block_size").template cast<IntegerAttr>().getInt();
    int64_t KPerBlock =
        op->getAttr("k_per_block").template cast<IntegerAttr>().getInt();
    int64_t MPerBlock =
        op->getAttr("m_per_block").template cast<IntegerAttr>().getInt();
    int64_t NPerBlock =
        op->getAttr("n_per_block").template cast<IntegerAttr>().getInt();
    int64_t MPerThread =
        op->getAttr("m_per_thread").template cast<IntegerAttr>().getInt();
    int64_t NPerThread =
        op->getAttr("n_per_thread").template cast<IntegerAttr>().getInt();
    auto MPerThreadConstantOp = b.create<ConstantIndexOp>(loc, MPerThread);
    auto NPerThreadConstantOp = b.create<ConstantIndexOp>(loc, NPerThread);

    int64_t MLevel0Cluster =
        op->getAttr("m_level0_cluster").template cast<IntegerAttr>().getInt();
    int64_t MLevel1Cluster =
        op->getAttr("m_level1_cluster").template cast<IntegerAttr>().getInt();
    int64_t NLevel0Cluster =
        op->getAttr("n_level0_cluster").template cast<IntegerAttr>().getInt();
    int64_t NLevel1Cluster =
        op->getAttr("n_level1_cluster").template cast<IntegerAttr>().getInt();
    auto NLevel0ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, NLevel0Cluster);
    auto NLevel1ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, NLevel1Cluster);

    int64_t matrix_a_source_data_per_read =
        op->getAttr("matrix_a_source_data_per_read")
            .template cast<IntegerAttr>()
            .getInt();
    int64_t matrix_b_source_data_per_read =
        op->getAttr("matrix_b_source_data_per_read")
            .template cast<IntegerAttr>()
            .getInt();
    auto matrix_a_source_vector_read_dim = static_cast<GemmDimensions>(
        op->getAttr("matrix_a_source_vector_read_dim")
            .template cast<IntegerAttr>()
            .getInt());
    auto matrix_b_source_vector_read_dim = static_cast<GemmDimensions>(
        op->getAttr("matrix_b_source_vector_read_dim")
            .template cast<IntegerAttr>()
            .getInt());

    // Get current workgroup ID.
    auto bid = b.create<miopen::WorkgroupIdOp>(loc, b.getIndexType());

    int64_t MBlockWork = M / MPerBlock;
    int64_t NBlockWork = N / NPerBlock;
    int64_t GStride = MBlockWork * NBlockWork;

    // llvm::errs() << "\ngridwise_gemm op:\n";
    // op.dump();
    // llvm::errs() << "\n";

    // llvm::errs() << "M: " << M << "\n";
    // llvm::errs() << "N: "  << N << "\n";
    // llvm::errs() << "K: "  << K << "\n";
    // llvm::errs() << "BlockSize: " << BlockSize << "\n";
    // llvm::errs() << "MPerBlock: " << MPerBlock << "\n";
    // llvm::errs() << "NPerBlock: " << NPerBlock << "\n";
    // llvm::errs() << "KPerBlock: " << KPerBlock << "\n";
    // llvm::errs() << "MPerThread: " << MPerThread << "\n";
    // llvm::errs() << "NPerThread: " << NPerThread << "\n";
    // llvm::errs() << "MBlockWork = M / MPerBlock: " << MBlockWork << "\n";
    // llvm::errs() << "NBlockWork = N / NPerBlock: " << NBlockWork << "\n";

    auto NBlockWorkConstantOp = b.create<ConstantIndexOp>(loc, NBlockWork);
    auto GStridOp = b.create<ConstantIndexOp>(loc, GStride);
    auto block_work_id_g =
        b.create<DivUIOp>(loc, bid, GStridOp); // id_g of coordinate
    auto block_work_rem = b.create<RemUIOp>(loc, bid, GStridOp);
    auto block_work_id_m =
        b.create<DivUIOp>(loc, block_work_rem, NBlockWorkConstantOp);
    auto block_work_id_n =
        b.create<RemUIOp>(loc, block_work_rem, NBlockWorkConstantOp);
    auto MPerBlockConstantOp = b.create<ConstantIndexOp>(loc, MPerBlock);
    auto NPerBlockConstantOp = b.create<ConstantIndexOp>(loc, NPerBlock);
    auto m_block_data_on_global =
        b.create<MulIOp>(loc, block_work_id_m, MPerBlockConstantOp);
    auto n_block_data_on_global =
        b.create<MulIOp>(loc, block_work_id_n, NPerBlockConstantOp);

    // llvm::errs() << "KPerBlock: " << KPerBlock << "\n";
    // llvm::errs() << "MPerBlock: " << MPerBlock << "\n";
    // llvm::errs() << "NPerBlock: " << NPerBlock << "\n";
    // llvm::errs() << "matrix_a_source_data_per_read: " << matrix_a_source_data_per_read << "\n";
    // llvm::errs() << "matrix_b_source_data_per_read: " << matrix_b_source_data_per_read << "\n";

    // Compute ThreadSliceLengths for Matrix A.
    uint64_t GemmABlockCopyNumberDataPerThread =
        MPerBlock * KPerBlock / BlockSize;

    uint64_t GemmABlockCopyThreadSliceLengths_GemmK;
    uint64_t GemmABlockCopyThreadSliceLengths_GemmM;
    switch (matrix_a_source_vector_read_dim) {
    case GemmK:
      GemmABlockCopyThreadSliceLengths_GemmK = matrix_a_source_data_per_read;
      GemmABlockCopyThreadSliceLengths_GemmM =
          GemmABlockCopyNumberDataPerThread /
          GemmABlockCopyThreadSliceLengths_GemmK;
      break;
    case GemmMorN:
      GemmABlockCopyThreadSliceLengths_GemmM = matrix_a_source_data_per_read;
      GemmABlockCopyThreadSliceLengths_GemmK =
          GemmABlockCopyNumberDataPerThread /
          GemmABlockCopyThreadSliceLengths_GemmM;
      break;
    case GemmG:
      llvm_unreachable("Vector loads/stores aren't possible in the G dimension "
                       "and should not haven been attempted");
    }

    // llvm::errs() << "thread slice lengths for Matrix A\n";
    // llvm::errs() << GemmABlockCopyThreadSliceLengths_GemmK << " ";
    // llvm::errs() << GemmABlockCopyThreadSliceLengths_GemmM << "\n";

    // Compute ThreadClusterLengths for Matrix A.
    int64_t GemmABlockCopyClusterLengths_GemmK =
        KPerBlock / GemmABlockCopyThreadSliceLengths_GemmK;
    // int64_t GemmABlockCopyClusterLengths_GemmM =
    //    MPerBlock / GemmABlockCopyThreadSliceLengths_GemmM;

    // llvm::errs() << "thread cluster lengths for Matrix A\n";
    // llvm::errs() << GemmABlockCopyClusterLengths_GemmK << " ";
    // llvm::errs() << GemmABlockCopyClusterLengths_GemmM << "\n";

    // Compute ThreadSliceLengths for Matrix B.
    uint64_t GemmBBlockCopyNumberDataPerThread =
        NPerBlock * KPerBlock / BlockSize;

    uint64_t GemmBBlockCopyThreadSliceLengths_GemmK;
    uint64_t GemmBBlockCopyThreadSliceLengths_GemmN;
    assert(matrix_b_source_vector_read_dim != GemmG);
    switch (matrix_b_source_vector_read_dim) {
    case GemmK:
      GemmBBlockCopyThreadSliceLengths_GemmK = matrix_b_source_data_per_read;
      GemmBBlockCopyThreadSliceLengths_GemmN =
          GemmBBlockCopyNumberDataPerThread /
          GemmBBlockCopyThreadSliceLengths_GemmK;
      break;
    case GemmMorN:
      GemmBBlockCopyThreadSliceLengths_GemmN = matrix_b_source_data_per_read;
      GemmBBlockCopyThreadSliceLengths_GemmK =
          GemmBBlockCopyNumberDataPerThread /
          GemmBBlockCopyThreadSliceLengths_GemmN;
      break;
    case GemmG:
      llvm_unreachable("Vector loads/stores aren't possible in the G dimension "
                       "and should not haven been attempted");
    }

    // llvm::errs() << "thread slice lengths for Matrix B\n";
    // llvm::errs() << GemmBBlockCopyThreadSliceLengths_GemmK << " ";
    // llvm::errs() << GemmBBlockCopyThreadSliceLengths_GemmN << "\n";

    // Compute ThreadClusterLengths for Matrix B.
    // int64_t GemmBBlockCopyClusterLengths_GemmK =
    //    KPerBlock / GemmBBlockCopyThreadSliceLengths_GemmK;
    uint64_t GemmBBlockCopyClusterLengths_GemmN =
        NPerBlock / GemmBBlockCopyThreadSliceLengths_GemmN;

    // llvm::errs() << "thread cluster lengths for Matrix B\n";
    // llvm::errs() << GemmBBlockCopyClusterLengths_GemmK << " ";
    // llvm::errs() << GemmBBlockCopyClusterLengths_GemmN << "\n";

    // Get current workitem ID.

    auto tid = b.create<miopen::WorkitemIdOp>(loc, b.getIndexType());

    // Compute thread_data_id_begin for Matrix A.
    // ClusterArrangeOrder for Matrix A is <1, 0>.
    // So divide by GemmABlockCopyClusterLengths_GemmK.
    auto GemmABlockCopyClusterLengths_GemmKConstantOp =
        b.create<ConstantIndexOp>(loc, GemmABlockCopyClusterLengths_GemmK);
    auto GemmABlockCopyThreadSliceLengths_GemmKConstantOp =
        b.create<ConstantIndexOp>(loc, GemmABlockCopyThreadSliceLengths_GemmK);
    auto GemmABlockCopyThreadSliceLengths_GemmMConstantOp =
        b.create<ConstantIndexOp>(loc, GemmABlockCopyThreadSliceLengths_GemmM);

    auto GemmABlockCopyThreadClusterId_Y = b.create<RemUIOp>(
        loc, tid, GemmABlockCopyClusterLengths_GemmKConstantOp);
    auto GemmABlockCopyThreadClusterId_X = b.create<DivUIOp>(
        loc, tid, GemmABlockCopyClusterLengths_GemmKConstantOp);
    auto GemmAThreadDataIdBegin_Y =
        b.create<MulIOp>(loc, GemmABlockCopyThreadClusterId_Y,
                         GemmABlockCopyThreadSliceLengths_GemmKConstantOp);
    auto GemmAThreadDataIdBegin_X =
        b.create<MulIOp>(loc, GemmABlockCopyThreadClusterId_X,
                         GemmABlockCopyThreadSliceLengths_GemmMConstantOp);

    auto GemmABlockCopySourceCoord_Y = GemmAThreadDataIdBegin_Y;
    auto GemmABlockCopySourceCoord_X =
        b.create<AddIOp>(loc, m_block_data_on_global, GemmAThreadDataIdBegin_X);
    auto GemmABlockCopyDestCoord_Y = GemmAThreadDataIdBegin_Y;
    auto GemmABlockCopyDestCoord_X = GemmAThreadDataIdBegin_X;

    // Compute thread_data_id_begin for Matrix B.
    // ClusterArrangeOrder for Matrix B is <0, 1>
    // So divide by GemmBBlockCopyClusterLengths_GemmN.
    auto GemmBBlockCopyClusterLengths_GemmNConstantOp =
        b.create<ConstantIndexOp>(loc, GemmBBlockCopyClusterLengths_GemmN);
    auto GemmBBlockCopyThreadSliceLengths_GemmKConstantOp =
        b.create<ConstantIndexOp>(loc, GemmBBlockCopyThreadSliceLengths_GemmK);
    auto GemmBBlockCopyThreadSliceLengths_GemmNConstantOp =
        b.create<ConstantIndexOp>(loc, GemmBBlockCopyThreadSliceLengths_GemmN);

    auto GemmBBlockCopyThreadClusterId_Y = b.create<DivUIOp>(
        loc, tid, GemmBBlockCopyClusterLengths_GemmNConstantOp);
    auto GemmBBlockCopyThreadClusterId_X = b.create<RemUIOp>(
        loc, tid, GemmBBlockCopyClusterLengths_GemmNConstantOp);
    auto GemmBThreadDataIdBegin_Y =
        b.create<MulIOp>(loc, GemmBBlockCopyThreadClusterId_Y,
                         GemmBBlockCopyThreadSliceLengths_GemmKConstantOp);
    auto GemmBThreadDataIdBegin_X =
        b.create<MulIOp>(loc, GemmBBlockCopyThreadClusterId_X,
                         GemmBBlockCopyThreadSliceLengths_GemmNConstantOp);

    auto GemmBBlockCopySourceCoord_Y = GemmBThreadDataIdBegin_Y;
    auto GemmBBlockCopySourceCoord_X =
        b.create<AddIOp>(loc, n_block_data_on_global, GemmBThreadDataIdBegin_X);
    auto GemmBBlockCopyDestCoord_Y = GemmBThreadDataIdBegin_Y;
    auto GemmBBlockCopyDestCoord_X = GemmBThreadDataIdBegin_X;

    auto GemmDataIdBegin_G = block_work_id_g;
    auto GemmBlockCoord_G = GemmDataIdBegin_G;
    // Compute required LDS sizes.
    int64_t ldsBlockASize, ldsBlockBSize, ldsBlockSize;
    computeLDSBlockSizes(op, ldsBlockASize, ldsBlockBSize, ldsBlockSize);

    // Allocate LDS.
    auto ldsMemRefType =
        MemRefType::get({ldsBlockSize}, elementType, {},
                        gpu::GPUDialect::getWorkgroupAddressSpace());
    auto ldsGpuAllocOp = b.create<miopen::GpuAllocOp>(loc, ldsMemRefType);

    // Subviews for Matrix A.
    auto ldsBlockAOffset = 0;
    auto ldsBlockAOffsetConstantOp =
        b.create<ConstantIndexOp>(loc, ldsBlockAOffset);
    auto ldsBlockAMemRefType = computeSubviewResultType(
        op, ldsMemRefType, ldsBlockAOffset, {ldsBlockASize}, elementType);
    auto ldsBlockASubviewOp = b.create<miopen::SubviewOp>(
        loc, ldsBlockAMemRefType, ldsGpuAllocOp, ldsBlockAOffsetConstantOp);

    // Get 2D subviews.
    // Compute matrix A dimension from attributes.
    auto lds2DMatrixAMemRefType = computeSubviewResultType(
        op, ldsBlockAMemRefType, 0, {1, KPerBlock, MPerBlock}, elementType);

    auto lds2DMatrixASubviewOp = b.create<miopen::SubviewOp>(
        loc, lds2DMatrixAMemRefType, ldsBlockASubviewOp, zeroConstantOp);

    // Subviews for Matrix B.
    auto ldsBlockBOffset = ldsBlockASize;

    auto ldsBlockBOffsetConstantOp =
        b.create<ConstantIndexOp>(loc, ldsBlockBOffset);
    auto ldsBlockBMemRefType = computeSubviewResultType(
        op, ldsMemRefType, ldsBlockBOffset, {ldsBlockBSize}, elementType);
    auto ldsBlockBSubviewOp = b.create<miopen::SubviewOp>(
        loc, ldsBlockBMemRefType, ldsGpuAllocOp, ldsBlockBOffsetConstantOp);

    // Get 2D subviews.
    // Compute matrix B dimension from attributes.
    auto lds2DMatrixBMemRefType = computeSubviewResultType(
        op, ldsBlockBMemRefType, 0, {1, KPerBlock, NPerBlock}, elementType);

    auto lds2DMatrixBSubviewOp = b.create<miopen::SubviewOp>(
        loc, lds2DMatrixBMemRefType, ldsBlockBSubviewOp, zeroConstantOp);

    // Alloc for Matrix C on registers.
    // Compute register size from attributes.
    int64_t GemmMRepeat = 0, GemmNRepeat = 0;

    // llvm::errs() << "MPerThread: " << MPerThread << "\n";
    // llvm::errs() << "NPerThread: " << NPerThread << "\n";

    GemmMRepeat = MPerBlock / (MPerThread * MLevel0Cluster * MLevel1Cluster);
    GemmNRepeat = NPerBlock / (NPerThread * NLevel0Cluster * NLevel1Cluster);

    // llvm::errs() << "GemmMRepeat: " << GemmMRepeat << "\n";
    // llvm::errs() << "GemmNRepeat: " << GemmNRepeat << "\n";

    auto threadCRegisterMemRefType = MemRefType::get(
        {1, GemmMRepeat * MPerThread, GemmNRepeat * NPerThread},
        accumulatorType, {}, gpu::GPUDialect::getPrivateAddressSpace());
    Value registerMatrixCAllocOp =
        b.create<miopen::GpuAllocOp>(loc, threadCRegisterMemRefType);

    // Determine vector / scalar load type for Matrix A / B.
    SmallVector<uint64_t, 3> blockwiseCopyABounds = {
        1, GemmABlockCopyThreadSliceLengths_GemmK,
        GemmABlockCopyThreadSliceLengths_GemmM};
    Type blockwiseLoadAType;
    SmallVector<Type, 8> blockwiseLoadATypes;
    int blockwiseAVectorDim;
    int blockwiseLoadAVectorLength;
    int blockwiseStoreAVectorLength;

    // llvm::errs() << "GemmABlockCopyThreadSliceLengths_GemmK: "
    //              << GemmABlockCopyThreadSliceLengths_GemmK << "\n";
    // llvm::errs() << "GemmABlockCopyThreadSliceLengths_GemmM: "
    //              << GemmABlockCopyThreadSliceLengths_GemmM << "\n";
    // llvm::errs() << "blockwise copy A bounds: ";
    // for (auto v : blockwiseCopyABounds)
    //   llvm::errs() << v << " ";
    // llvm::errs() << "\n";

    std::tie(blockwiseLoadAType, blockwiseAVectorDim,
             blockwiseLoadAVectorLength, blockwiseStoreAVectorLength) =
        computeLoadStoreTypeInfo(b, op, elementType, blockwiseLoadATypes,
                                 blockwiseCopyABounds, true);

    // llvm::errs() << "vector load dim: " << blockwiseAVectorDim << "\n";
    // llvm::errs() << "element type: " << blockwiseLoadAType << "\n";
    // llvm::errs() << "load size: " << blockwiseLoadAVectorLength << "\n";
    // llvm::errs() << "store size: " << blockwiseStoreAVectorLength << "\n";

    SmallVector<uint64_t, 3> blockwiseCopyBBounds = {
        1, GemmBBlockCopyThreadSliceLengths_GemmK,
        GemmBBlockCopyThreadSliceLengths_GemmN};
    Type blockwiseLoadBType;
    SmallVector<Type, 8> blockwiseLoadBTypes;
    int blockwiseBVectorDim;
    int blockwiseLoadBVectorLength;
    int blockwiseStoreBVectorLength;

    // llvm::errs() << "GemmBBlockCopyThreadSliceLengths_GemmK: "
    //              << GemmBBlockCopyThreadSliceLengths_GemmK << "\n";
    // llvm::errs() << "GemmBBlockCopyThreadSliceLengths_GemmN: "
    //              << GemmBBlockCopyThreadSliceLengths_GemmN << "\n";
    // llvm::errs() << "blockwise copy B bounds: ";
    // for (auto v : blockwiseCopyBBounds)
    //   llvm::errs() << v << " ";
    // llvm::errs() << "\n";

    std::tie(blockwiseLoadBType, blockwiseBVectorDim,
             blockwiseLoadBVectorLength, blockwiseStoreBVectorLength) =
        computeLoadStoreTypeInfo(b, op, elementType, blockwiseLoadBTypes,
                                 blockwiseCopyBBounds, false);

    // llvm::errs() << "vector load dim: " << blockwiseBVectorDim << "\n";
    // llvm::errs() << "element type: " << blockwiseLoadBType << "\n";
    // llvm::errs() << "load size: " << blockwiseLoadBVectorLength << "\n";
    // llvm::errs() << "store size: " << blockwiseStoreBVectorLength << "\n";

    // Zero init Matrix C on registers.
    b.create<miopen::FillOp>(loc, registerMatrixCAllocOp, zeroConstantFloatOp);

    // Blockwise copies before the loop.
    // Blockwise copy from global (generic tensor) to LDS (naive tensor).

    // Compute source and destination coordinates for BlockwiseCopy ops.
    // Matrix A: {0, 0, m_block_data_on_global}, {0, 0, 0}
    // Matrix B: {0, 0, n_block_data_on_global}, {0, 0, 0}

    Value mMyThreadOffsetA, mMyThreadOffsetB;
    Value c_thread_mtx_index_row, c_thread_mtx_index_col;
    Value m_thread_data_on_global, n_thread_data_on_global;

    // Compute c_thread_mtx_index for Matrix C.
    int64_t ThreadPerLevel0Cluster = MLevel0Cluster * NLevel0Cluster;
    auto ThreadPerLevel0ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, ThreadPerLevel0Cluster);
    auto level1_id =
        b.create<DivUIOp>(loc, tid, ThreadPerLevel0ClusterConstantOp);
    auto level1_m_id =
        b.create<DivUIOp>(loc, level1_id, NLevel1ClusterConstantOp);
    auto level1_n_id =
        b.create<RemUIOp>(loc, level1_id, NLevel1ClusterConstantOp);

    auto level0_id =
        b.create<RemUIOp>(loc, tid, ThreadPerLevel0ClusterConstantOp);
    auto level0_m_id =
        b.create<DivUIOp>(loc, level0_id, NLevel0ClusterConstantOp);
    auto level0_n_id =
        b.create<RemUIOp>(loc, level0_id, NLevel0ClusterConstantOp);

    int64_t MPerLevel0Cluster = MPerThread * MLevel0Cluster;
    int64_t NPerLevel0Cluster = NPerThread * NLevel0Cluster;
    auto MPerLevel0ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, MPerLevel0Cluster);
    auto NPerLevel0ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, NPerLevel0Cluster);

    // mMyThreadOffsetA = BlockMatrixA::GetOffsetFromMultiIndex{0,
    // c_thread_mtx_index.row} = c_thread_mtx_index_row
    c_thread_mtx_index_row = b.create<AddIOp>(
        loc, b.create<MulIOp>(loc, level1_m_id, MPerLevel0ClusterConstantOp),
        b.create<MulIOp>(loc, level0_m_id, MPerThreadConstantOp));
    mMyThreadOffsetA = c_thread_mtx_index_row;

    // mMyThreadOffsetB = BlockMatrixB::GetOffsetFromMultiIndex{0,
    // c_thread_mtx_index.col} = c_thread_mtx_index_col
    c_thread_mtx_index_col = b.create<AddIOp>(
        loc, b.create<MulIOp>(loc, level1_n_id, NPerLevel0ClusterConstantOp),
        b.create<MulIOp>(loc, level0_n_id, NPerThreadConstantOp));
    mMyThreadOffsetB = c_thread_mtx_index_col;

    m_thread_data_on_global =
        b.create<AddIOp>(loc, m_block_data_on_global, c_thread_mtx_index_row);
    n_thread_data_on_global =
        b.create<AddIOp>(loc, n_block_data_on_global, c_thread_mtx_index_col);

    SmallVector<Value, 4> blockwiseLoadACoords =
        ValueRange{GemmBlockCoord_G, GemmABlockCopySourceCoord_Y,
                   GemmABlockCopySourceCoord_X};
    // Emit blockwise_load for matrix A.
    auto blockwiseLoadA = b.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadATypes, op.filter(), blockwiseLoadACoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadA, op, b, /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);
    // Emit blockwise_store for matrix A.
    auto blockwiseStoreA = b.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadA.getResults(), lds2DMatrixASubviewOp,
        ValueRange{zeroConstantOp, GemmABlockCopyDestCoord_Y,
                   GemmABlockCopyDestCoord_X});
    affixBlockwiseCopyAttributes(
        blockwiseStoreA, op, b, /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);

    // Emit blockwise_load for matrix B.
    SmallVector<Value, 4> blockwiseLoadBCoords = {GemmBlockCoord_G,
                                                  GemmBBlockCopySourceCoord_Y,
                                                  GemmBBlockCopySourceCoord_X};
    auto blockwiseLoadB = b.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadBTypes, op.input(), blockwiseLoadBCoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadB, op, b,
        /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);
    // Emit blockwise_store for matrix B.
    auto blockwiseStoreB = b.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadB.getResults(), lds2DMatrixBSubviewOp,
        ValueRange{zeroConstantOp, GemmBBlockCopyDestCoord_Y,
                   GemmBBlockCopyDestCoord_X});
    affixBlockwiseCopyAttributes(
        blockwiseStoreB, op, b,
        /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);

    // Emit loop.
    // Compute loop iterations from attributes.

    auto KPerBlockConstantOp = b.create<ConstantIndexOp>(loc, KPerBlock);

    int64_t loopIteration = (K - KPerBlock) / KPerBlock;

    // Assign iter args.
    // 0: blockwise copy A src y coordinate.
    // 1: blockwise copy B src y coordinate.
    SmallVector<Value, 2> iterArgs = {blockwiseLoadA.sourceCoord()[1],
                                      blockwiseLoadB.sourceCoord()[1]};

    auto loopOp = b.create<AffineForOp>(loc, 0, loopIteration, 1, iterArgs);

    // inside the loop.
    auto lb = OpBuilder::atBlockBegin(loopOp.getBody());

    // LDS barrier.
    lb.create<miopen::LDSBarrierOp>(loc);

    // Emit blockwise GEMM.
    auto blockwiseGemmOp = lb.create<miopen::BlockwiseGemmOp>(
        loc, lds2DMatrixASubviewOp, lds2DMatrixBSubviewOp,
        registerMatrixCAllocOp, mMyThreadOffsetA, mMyThreadOffsetB);
    affixBlockwiseGemmAttributes(blockwiseGemmOp, op, b);

    // LDS barrier.
    // This barrier prevents halo part of outputs having weird values.
    lb.create<miopen::LDSBarrierOp>(loc);

    const auto &args = loopOp.getRegionIterArgs();
    Value blockwiseCopyASrcUpdated =
        lb.create<AddIOp>(loc, args[0], KPerBlockConstantOp);
    // Emit blockwise_load for matrix A.
    blockwiseLoadACoords[1] = blockwiseCopyASrcUpdated;
    auto blockwiseLoadATop = lb.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadATypes, op.filter(), blockwiseLoadACoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadATop, op, b, /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);

    Value blockwiseCopyBSrcUpdated =
        lb.create<AddIOp>(loc, args[1], KPerBlockConstantOp);
    blockwiseLoadBCoords[1] = blockwiseCopyBSrcUpdated;
    // Emit blockwise_load for matrix B.
    auto blockwiseLoadBTop = lb.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadBTypes, op.input(), blockwiseLoadBCoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadBTop, op, b, /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);

    // Blockwise copy from register (naive tensor) to LDS (naive tensor).
    // Emit blockwise_store for matrix A.
    auto blockwiseStoreABottom = lb.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadATop.getResults(), lds2DMatrixASubviewOp,
        blockwiseStoreA.destCoord());
    affixBlockwiseCopyAttributes(
        blockwiseStoreABottom, op, b,
        /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);
    // Emit blockwise_store for matrix B.
    auto blockwiseStoreBBottom = lb.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadBTop.getResults(), lds2DMatrixBSubviewOp,
        blockwiseStoreB.destCoord());
    affixBlockwiseCopyAttributes(
        blockwiseStoreBBottom, op, b,
        /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);

    // update iter args.
    // blockwiseCopyASrcVector and blockwiseCopyBSrcVector are updated.
    iterArgs[0] = blockwiseCopyASrcUpdated;
    iterArgs[1] = blockwiseCopyBSrcUpdated;
    // emit loop yield so iter args can be passed to the next iteration.
    lb.create<AffineYieldOp>(loc, iterArgs);

    // outside the loop.

    // LDS barrier.
    b.create<miopen::LDSBarrierOp>(loc);

    // Emit blockwise GEMM for the loop tail.
    auto blockwiseGemmTailOp = b.create<miopen::BlockwiseGemmOp>(
        loc, lds2DMatrixASubviewOp, lds2DMatrixBSubviewOp,
        registerMatrixCAllocOp, mMyThreadOffsetA, mMyThreadOffsetB);
    affixBlockwiseGemmAttributes(blockwiseGemmTailOp, op, b);

    // Threadwise copy from register (naive tensor) to global (generic tensor).
    int64_t M1 = MPerThread * MLevel0Cluster * MLevel1Cluster;
    int64_t M0 = M / M1;
    int64_t N1 = NPerThread * NLevel0Cluster * NLevel1Cluster;
    int64_t N0 = N / N1;

    auto M1ConstantOp = b.create<ConstantIndexOp>(loc, M1);
    auto N1ConstantOp = b.create<ConstantIndexOp>(loc, N1);

    // build affine expression: d0 = g
    // (d0, d1, d2, d3, d4) -> (d0, d1 * M1 + d2, d3 * N1 + d4)
    auto affineMap5to3 =
        AffineMap::get(5, 0,
                       {getAffineDimExpr(0, op.getContext()),
                        getAffineDimExpr(2, op.getContext()) +
                            getAffineDimExpr(1, op.getContext()) *
                                getAffineConstantExpr(M1, op.getContext()),
                        getAffineDimExpr(4, op.getContext()) +
                            getAffineDimExpr(3, op.getContext()) *
                                getAffineConstantExpr(N1, op.getContext())},
                       op.getContext());

    // compose with output tensor affine map.
    auto outputType = op.output().getType().template cast<MemRefType>();
    SmallVector<AffineMap> newOutputAffineMaps({outputType.getLayout().getAffineMap()});
    newOutputAffineMaps.insert(newOutputAffineMaps.begin(), affineMap5to3);

    // emit TransformOp for output tensor.
    llvm::SmallVector<NamedAttribute, 3> transformedNewOutputAttrs;
    // set map attribute.
    transformedNewOutputAttrs.push_back(
        b.getNamedAttr("map", b.getAffineMapArrayAttr({affineMap5to3})));
    // set layout attribute.
    transformedNewOutputAttrs.push_back(b.getNamedAttr(
        "layout",
        b.getArrayAttr(
            {b.getDictionaryAttr(
                 {b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(0)})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({b.getStringAttr("g")})),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(0)})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({b.getStringAttr("gemmG")})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough"))}),
             b.getDictionaryAttr({
                 b.getNamedAttr("upper_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(1),
                                                b.getI32IntegerAttr(2)})),
                 b.getNamedAttr("upper_layer_names",
                                b.getArrayAttr({b.getStringAttr("m0"),
                                                b.getStringAttr("m1")})),
                 b.getNamedAttr("lower_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(1)})),
                 b.getNamedAttr("lower_layer_names",
                                b.getArrayAttr({b.getStringAttr("gemmM")})),
                 b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                 b.getNamedAttr("parameters",
                                b.getArrayAttr({b.getI32IntegerAttr(M1),
                                                b.getI32IntegerAttr(1)})),
             }),
             b.getDictionaryAttr({
                 b.getNamedAttr("upper_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(3),
                                                b.getI32IntegerAttr(4)})),
                 b.getNamedAttr("upper_layer_names",
                                b.getArrayAttr({b.getStringAttr("n0"),
                                                b.getStringAttr("n1")})),
                 b.getNamedAttr("lower_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(2)})),
                 b.getNamedAttr("lower_layer_names",
                                b.getArrayAttr({b.getStringAttr("gemmN")})),
                 b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                 b.getNamedAttr("parameters",
                                b.getArrayAttr({b.getI32IntegerAttr(N1),
                                                b.getI32IntegerAttr(1)})),
             })})));
    // set lower_layer_layout attribute.
    transformedNewOutputAttrs.push_back(b.getNamedAttr(
        "lower_layer_layout",
        b.getArrayAttr({b.getStringAttr("gemmG"), b.getStringAttr("gemmM"),
                        b.getStringAttr("gemmN")})));
    // set upper_layer_layout attribute.
    transformedNewOutputAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout",
        b.getArrayAttr({b.getStringAttr("g"), b.getStringAttr("m0"),
                        b.getStringAttr("m1"), b.getStringAttr("n0"),
                        b.getStringAttr("n1")})));
    auto newOutputType = MemRefType::get(
        {G, M0, M1, N0, N1}, outputType.getElementType(), composeTransforms(newOutputAffineMaps));
    auto newOutputTransformOp = b.create<miopen::TransformOp>(
        loc, newOutputType, op.output(), transformedNewOutputAttrs,
        /*populateBounds=*/true);

    // build affine expression: d0 = g
    // (d0, d1, d2, d3, d4) -> (d0, d1 * MPerThread + d2, d3 * NPerThread + d4)
    auto matrixCAffineMap5to3 = AffineMap::get(
        5, 0,
        {getAffineDimExpr(0, op.getContext()),
         getAffineDimExpr(2, op.getContext()) +
             getAffineDimExpr(1, op.getContext()) *
                 getAffineConstantExpr(MPerThread, op.getContext()),
         getAffineDimExpr(4, op.getContext()) +
             getAffineDimExpr(3, op.getContext()) *
                 getAffineConstantExpr(NPerThread, op.getContext())},
        op.getContext());

    // emit TransformOp for Matrix C on VGPR.
    llvm::SmallVector<NamedAttribute, 3> transformedMatrixCAttrs;
    // set map attribute.
    transformedMatrixCAttrs.push_back(
        b.getNamedAttr("map", b.getAffineMapArrayAttr({matrixCAffineMap5to3})));
    // set layout attribute.
    transformedMatrixCAttrs.push_back(b.getNamedAttr(
        "layout",
        b.getArrayAttr(
            {b.getDictionaryAttr(
                 {b.getNamedAttr("upper_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(0)})),
                  b.getNamedAttr("upper_layer_names",
                                 b.getArrayAttr({b.getStringAttr("g")})),
                  b.getNamedAttr("lower_layer_dimensions",
                                 b.getArrayAttr({b.getI32IntegerAttr(0)})),
                  b.getNamedAttr("lower_layer_names",
                                 b.getArrayAttr({b.getStringAttr("gemmG")})),
                  b.getNamedAttr("transformation",
                                 b.getStringAttr("PassThrough"))}),
             b.getDictionaryAttr({
                 b.getNamedAttr("upper_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(1),
                                                b.getI32IntegerAttr(2)})),
                 b.getNamedAttr(
                     "upper_layer_names",
                     b.getArrayAttr({b.getStringAttr("gemmMRepeat"),
                                     b.getStringAttr("mPerThread")})),
                 b.getNamedAttr("lower_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(1)})),
                 b.getNamedAttr("lower_layer_names",
                                b.getArrayAttr({b.getStringAttr("gemmM")})),
                 b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                 b.getNamedAttr("parameters",
                                b.getArrayAttr({b.getI32IntegerAttr(MPerThread),
                                                b.getI32IntegerAttr(1)})),
             }),
             b.getDictionaryAttr({
                 b.getNamedAttr("upper_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(3),
                                                b.getI32IntegerAttr(4)})),
                 b.getNamedAttr(
                     "upper_layer_names",
                     b.getArrayAttr({b.getStringAttr("gemmNRepeat"),
                                     b.getStringAttr("nPerThread")})),
                 b.getNamedAttr("lower_layer_dimensions",
                                b.getArrayAttr({b.getI32IntegerAttr(2)})),
                 b.getNamedAttr("lower_layer_names",
                                b.getArrayAttr({b.getStringAttr("gemmN")})),
                 b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                 b.getNamedAttr("parameters",
                                b.getArrayAttr({b.getI32IntegerAttr(NPerThread),
                                                b.getI32IntegerAttr(1)})),
             })})));
    // set lower_layer_layout attribute.
    transformedMatrixCAttrs.push_back(b.getNamedAttr(
        "lower_layer_layout",
        b.getArrayAttr({b.getStringAttr("gemmG"), b.getStringAttr("gemmM"),
                        b.getStringAttr("gemmN")})));
    // set upper_layer_layout attribute.
    transformedMatrixCAttrs.push_back(b.getNamedAttr(
        "upper_layer_layout",
        b.getArrayAttr({b.getStringAttr("g"), b.getStringAttr("gemmMRepeat"),
                        b.getStringAttr("mPerThread"),
                        b.getStringAttr("gemmNRepeat"),
                        b.getStringAttr("nPerThread")})));
    auto register5DMatrixCType = MemRefType::get(
        {1, GemmMRepeat, MPerThread, GemmNRepeat, NPerThread}, elementType,
        {matrixCAffineMap5to3}, gpu::GPUDialect::getPrivateAddressSpace());
    auto matrixCTransformOp = b.create<miopen::TransformOp>(
        loc, register5DMatrixCType, registerMatrixCAllocOp,
        transformedMatrixCAttrs, /*populateBounds=*/true);

    SmallVector<Value, 5> matrixCThreadwiseCopySourceCoords;
    std::fill_n(std::back_inserter(matrixCThreadwiseCopySourceCoords), 5,
                zeroConstantOp.getResult());

    SmallVector<Value, 5> matrixCThreadwiseCopyDestCoords = {
        GemmDataIdBegin_G,
        b.create<DivUIOp>(loc, m_thread_data_on_global, M1ConstantOp),
        b.create<RemUIOp>(loc, m_thread_data_on_global, M1ConstantOp),
        b.create<DivUIOp>(loc, n_thread_data_on_global, N1ConstantOp),
        b.create<RemUIOp>(loc, n_thread_data_on_global, N1ConstantOp)};
    // g index

    auto threadwiseCopyCMatrixOp = b.create<miopen::ThreadwiseCopyOp>(
        loc, matrixCTransformOp, newOutputTransformOp,
        matrixCThreadwiseCopySourceCoords, matrixCThreadwiseCopyDestCoords);
    affixThreadwiseCopyAttributes(threadwiseCopyCMatrixOp, op, b);

    op.erase();

    return success();
  }
};

//===----------------------------------------------------------------------===//
// GridwiseGemmV2 lowering.
//===----------------------------------------------------------------------===//

struct GridwiseGemmV2RewritePattern
    : public OpRewritePattern<miopen::GridwiseGemmV2Op> {
  using OpRewritePattern<miopen::GridwiseGemmV2Op>::OpRewritePattern;

  void computeLDSBlockSizes(miopen::GridwiseGemmV2Op op, int64_t &a_block_space,
                            int64_t &b_block_space,
                            int64_t &total_block_space) const {
    int64_t ABlockCopyDstDataPerWrite_M =
        op->getAttr("matrix_a_dest_data_per_write_dim_m")
            .template cast<IntegerAttr>()
            .getInt();
    int64_t BBlockCopyDstDataPerWrite_N =
        op->getAttr("matrix_b_dest_data_per_write_dim_n")
            .template cast<IntegerAttr>()
            .getInt();

    int64_t max_lds_align =
        math_util::lcm(ABlockCopyDstDataPerWrite_M, BBlockCopyDstDataPerWrite_N);

    int64_t KPerBlock =
        op->getAttr("k_per_block").template cast<IntegerAttr>().getInt();
    int64_t MPerBlock =
        op->getAttr("m_per_block").template cast<IntegerAttr>().getInt();
    int64_t NPerBlock =
        op->getAttr("n_per_block").template cast<IntegerAttr>().getInt();

    int64_t AlignedNPerBlock =
        max_lds_align *
        math_util::integer_divide_ceil<int64_t>(NPerBlock, max_lds_align);

    // A matrix in LDS memory, dst of blockwise copy
    int64_t AlignedMPerBlock =
        max_lds_align *
        math_util::integer_divide_ceil<int64_t>(MPerBlock, max_lds_align);

    // llvm::errs() << "MPerBlock : " << MPerBlock << "\n";
    // llvm::errs() << "NPerBlock : " << NPerBlock << "\n";
    // llvm::errs() << "max_lds_align : " << max_lds_align << "\n";
    // llvm::errs() << "AlignedMPerBlock : " << AlignedMPerBlock << "\n";
    // llvm::errs() << "AlignedNPerBlock : " << AlignedNPerBlock << "\n";

    a_block_space = math_util::integer_least_multiple(KPerBlock * AlignedMPerBlock,
                                                 max_lds_align);

    // B matrix in LDS memory, dst of blockwise copy
    b_block_space = math_util::integer_least_multiple(KPerBlock * AlignedNPerBlock,
                                                 max_lds_align);

    total_block_space = a_block_space + b_block_space;

    // llvm::errs() << "a_block_space: " << a_block_space << "\n";
    // llvm::errs() << "b_block_space: " << b_block_space << "\n";
    // llvm::errs() << "total_block_space: " << total_block_space << "\n\n";
  }

  void affixXdlopsGemmV2Attributes(miopen::XdlopsGemmV2Op xop,
                                   miopen::GridwiseGemmV2Op gop,
                                   OpBuilder &b) const {
    xop->setAttr("block_size", gop->getAttr("block_size"));
    // xdlopsV2.
    auto xdlopsV2Attr = gop->template getAttrOfType<BoolAttr>("xdlopsV2");
    if (xdlopsV2Attr && xdlopsV2Attr.getValue() == true) {
      int64_t MPerBlock =
          gop->getAttr("m_per_block").template cast<IntegerAttr>().getInt();
      int64_t NPerBlock =
          gop->getAttr("n_per_block").template cast<IntegerAttr>().getInt();
      int64_t MPerWave =
          gop->getAttr("m_per_wave").template cast<IntegerAttr>().getInt();
      int64_t NPerWave =
          gop->getAttr("n_per_wave").template cast<IntegerAttr>().getInt();
      int64_t MWaves = MPerBlock / MPerWave;
      int64_t NWaves = NPerBlock / NPerWave;

      xop->setAttr("m_per_wave", gop->getAttr("m_per_wave"));
      xop->setAttr("n_per_wave", gop->getAttr("n_per_wave"));
      xop->setAttr("m_waves", b.getI32IntegerAttr(MWaves));
      xop->setAttr("n_waves", b.getI32IntegerAttr(NWaves));

      xop->setAttr("xdlopsV2", b.getBoolAttr(true));
    }
  }

  void affixBlockwiseGemmV2Attributes(miopen::BlockwiseGemmV2Op bop,
                                      miopen::GridwiseGemmV2Op gop,
                                      OpBuilder &b) const {
    bop->setAttr("block_size", gop->getAttr("block_size"));

    int64_t MPerBlock =
        gop->getAttr("m_per_block").template cast<IntegerAttr>().getInt();
    int64_t NPerBlock =
        gop->getAttr("n_per_block").template cast<IntegerAttr>().getInt();
    int64_t MPerWave =
        gop->getAttr("m_per_wave").template cast<IntegerAttr>().getInt();
    int64_t NPerWave =
        gop->getAttr("n_per_wave").template cast<IntegerAttr>().getInt();
    int64_t MWaves = MPerBlock / MPerWave;
    int64_t NWaves = NPerBlock / NPerWave;

    bop->setAttr("m_per_wave", gop->getAttr("m_per_wave"));
    bop->setAttr("n_per_wave", gop->getAttr("n_per_wave"));
    bop->setAttr("m_waves", b.getI32IntegerAttr(MWaves));
    bop->setAttr("n_waves", b.getI32IntegerAttr(NWaves));

    int64_t M =
        bop.matrixA().getType().template cast<MemRefType>().getShape()[2];
    int64_t N =
        bop.matrixB().getType().template cast<MemRefType>().getShape()[2];
    int64_t K =
        bop.matrixA().getType().template cast<MemRefType>().getShape()[1];

    bop->setAttr("m", b.getI32IntegerAttr(M));
    bop->setAttr("n", b.getI32IntegerAttr(N));
    bop->setAttr("k", b.getI32IntegerAttr(K));

    bop->setAttr("coord_transforms", b.getArrayAttr({}));
  }

  template <typename T>
  MemRefType computeSubviewResultType(T &op, MemRefType inputType,
                                      unsigned offset,
                                      ArrayRef<int64_t> outputShape,
                                      Type outputElementType) const {
    auto outputRank = outputShape.size();

    auto expr = getAffineConstantExpr(offset, op.getContext());
    unsigned stride = 1;
    for (int i = outputRank - 1; i >= 0; --i) {
      expr = expr + getAffineDimExpr(i, op.getContext()) *
                        getAffineConstantExpr(stride, op.getContext());
      stride *= outputShape[i];
    }

    AffineMap transformAffineMap = AffineMap::get(
        outputRank, 0, ArrayRef<AffineExpr>{expr}, op.getContext());
    AffineMap outputAffineMap =
      inputType.getLayout().getAffineMap().compose(transformAffineMap);
    auto transformedOutputType =
        MemRefType::get(outputShape, outputElementType, {outputAffineMap},
                        inputType.getMemorySpaceAsInt());
    return transformedOutputType;
  }

  LogicalResult matchAndRewrite(miopen::GridwiseGemmV2Op op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    // Obtain data type.
    auto elementType =
        op.output().getType().cast<MemRefType>().getElementType();

    // Prepare some useful constants.
    auto zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);

    // Obtain critical matrix dimensions.
    int64_t G = op.filter().getType().template cast<MemRefType>().getShape()[0];
    int64_t K = op.filter().getType().template cast<MemRefType>().getShape()[1];
    int64_t M = op.filter().getType().template cast<MemRefType>().getShape()[2];
    int64_t N = op.input().getType().template cast<MemRefType>().getShape()[2];

    // Obtain critical tuning parameters.
    int64_t BlockSize =
        op->getAttr("block_size").template cast<IntegerAttr>().getInt();
    int64_t KPerBlock =
        op->getAttr("k_per_block").template cast<IntegerAttr>().getInt();
    int64_t MPerBlock =
        op->getAttr("m_per_block").template cast<IntegerAttr>().getInt();
    int64_t NPerBlock =
        op->getAttr("n_per_block").template cast<IntegerAttr>().getInt();

    int64_t matrix_a_source_data_per_read =
        op->getAttr("matrix_a_source_data_per_read")
            .template cast<IntegerAttr>()
            .getInt();
    int64_t matrix_b_source_data_per_read =
        op->getAttr("matrix_b_source_data_per_read")
            .template cast<IntegerAttr>()
            .getInt();
    auto matrix_a_source_vector_read_dim = static_cast<GemmDimensions>(
        op->getAttr("matrix_a_source_vector_read_dim")
            .template cast<IntegerAttr>()
            .getInt());
    auto matrix_b_source_vector_read_dim = static_cast<GemmDimensions>(
        op->getAttr("matrix_b_source_vector_read_dim")
            .template cast<IntegerAttr>()
            .getInt());

    // Obtain XDLOPS-related attributes.
    int64_t MPerWave =
        op->getAttr("m_per_wave").template cast<IntegerAttr>().getInt();
    int64_t NPerWave =
        op->getAttr("n_per_wave").template cast<IntegerAttr>().getInt();
    // int64_t MWaves = MPerBlock / MPerWave;
    int64_t NWaves = NPerBlock / NPerWave;
    auto dataType =
        op.input().getType().template cast<MemRefType>().getElementType();

    auto MPerWaveConstantOp = b.create<ConstantIndexOp>(loc, MPerWave);
    auto NPerWaveConstantOp = b.create<ConstantIndexOp>(loc, NPerWave);
    auto NWavesConstantOp = b.create<ConstantIndexOp>(loc, NWaves);

    int64_t WaveSize = 64;
    auto waveSizeConstantOp = b.create<ConstantIndexOp>(loc, WaveSize);

    // Get current workgroup ID.
    auto bid = b.create<miopen::WorkgroupIdOp>(loc, b.getIndexType());

    // Get current workitem ID.
    auto tid = b.create<miopen::WorkitemIdOp>(loc, b.getIndexType());

    int64_t MBlockWork = M / MPerBlock;
    int64_t NBlockWork = N / NPerBlock;
    int64_t GStride = MBlockWork * NBlockWork;

    // int64_t MWavePerBlock = MPerBlock / MPerWave;
    // int64_t NWavePerBlock = NPerBlock / NPerWave;

    // llvm::errs() << "M: " << M << "\n";
    // llvm::errs() << "N: " << N << "\n";
    // llvm::errs() << "K: " << K << "\n";
    // llvm::errs() << "MPerBlock: " << MPerBlock << "\n";
    // llvm::errs() << "NPerBlock: " << NPerBlock << "\n";
    // llvm::errs() << "KPerBlock: " << KPerBlock << "\n";
    // llvm::errs() << "MBlockWork = M / MPerBlock: " << MBlockWork << "\n";
    // llvm::errs() << "NBlockWork = N / NPerBlock: " << NBlockWork << "\n";
    // llvm::errs() << "MPerWave: " << MPerWave << "\n";
    // llvm::errs() << "NPerWave: " << NPerWave << "\n";
    // llvm::errs() << "MWaves = MPerBlock / MPerWave: " << MWaves << "\n";
    // llvm::errs() << "NWaves = NPerBlock / NPerWave: " << NWaves << "\n";
    // llvm::errs() << "MWavesPerBlock = MPerBlock / MPerWave: " <<
    // MWavePerBlock
    //              << "\n";
    // llvm::errs() << "NWavesPerBlock = NPerBlock / NPerWave: " <<
    // NWavePerBlock
    //              << "\n";

    // llvm::errs() << "matrix_a_source_data_per_read: "
    //              << matrix_a_source_data_per_read << "\n";
    // llvm::errs() << "matrix_b_source_data_per_read: "
    //              << matrix_b_source_data_per_read << "\n";
    // llvm::errs() << "matrix_a_source_vector_read_dim: "
    //              << matrix_a_source_vector_read_dim << "\n";
    // llvm::errs() << "matrix_b_source_vector_read_dim: "
    //              << matrix_b_source_vector_read_dim << "\n";

    auto MPerBlockConstantOp = b.create<ConstantIndexOp>(loc, MPerBlock);
    auto NPerBlockConstantOp = b.create<ConstantIndexOp>(loc, NPerBlock);
    auto KPerBlockConstantOp = b.create<ConstantIndexOp>(loc, KPerBlock);
    auto MBlockWorkConstantOp = b.create<ConstantIndexOp>(loc, MBlockWork);
    auto GStridOp = b.create<ConstantIndexOp>(loc, GStride);
    // -----

    // Compute the coordinate for the current workgroup on global memory.

    // Original C++ logic:
    // constexpr auto wkgrp_schd_order = NBlock1MBlock0;
    // constexpr auto block_work_sequence =
    //     make_batch_block_work_sequence<G, MBlockWork, NBlockWork,
    //     WorkgroupSchdOrder>{}.get();
    // constexpr auto block_work_desc =
    // make_cluster_descriptor(block_work_sequence); const auto block_work_id =
    // block_work_desc.CalculateClusterIndex(get_block_1d_id());

    // Result block_work_desc is <NBlockWorkd, MBlockWork>

    auto block_work_id_g = b.create<DivUIOp>(loc, bid, GStridOp);
    auto block_work_rem = b.create<RemUIOp>(loc, bid, GStridOp);
    auto block_work_id_m =
        b.create<RemUIOp>(loc, block_work_rem, MBlockWorkConstantOp);
    auto block_work_id_n =
        b.create<DivUIOp>(loc, block_work_rem, MBlockWorkConstantOp);

    auto m_block_data_on_global =
        b.create<MulIOp>(loc, block_work_id_m, MPerBlockConstantOp);
    auto n_block_data_on_global =
        b.create<MulIOp>(loc, block_work_id_n, NPerBlockConstantOp);

    // -----

    // Logic to prepare parameters for blockwise_copy.

    // Compute ThreadSliceLengths for Matrix A.
    uint64_t GemmABlockCopyNumberDataPerThread =
        MPerBlock * KPerBlock / BlockSize;

    uint64_t GemmABlockCopyThreadSliceLengths_GemmK;
    uint64_t GemmABlockCopyThreadSliceLengths_GemmM;
    switch (matrix_a_source_vector_read_dim) {
    case GemmK:
      GemmABlockCopyThreadSliceLengths_GemmK = matrix_a_source_data_per_read;
      GemmABlockCopyThreadSliceLengths_GemmM =
          GemmABlockCopyNumberDataPerThread /
          GemmABlockCopyThreadSliceLengths_GemmK;
      break;
    case GemmMorN:
      GemmABlockCopyThreadSliceLengths_GemmM = matrix_a_source_data_per_read;
      GemmABlockCopyThreadSliceLengths_GemmK =
          GemmABlockCopyNumberDataPerThread /
          GemmABlockCopyThreadSliceLengths_GemmM;
      break;
    case GemmG:
      llvm_unreachable("Vector loads/stores aren't possible in the G dimension "
                       "and should not haven been attempted");
    }

    // llvm::errs() << "thread slice lengths for Matrix A\n";
    // llvm::errs() << GemmABlockCopyThreadSliceLengths_GemmK << " ";
    // llvm::errs() << GemmABlockCopyThreadSliceLengths_GemmM << "\n";

    // Compute ThreadClusterLengths for Matrix A.
    uint64_t GemmABlockCopyClusterLengths_GemmK =
        KPerBlock / GemmABlockCopyThreadSliceLengths_GemmK;
    // int64_t GemmABlockCopyClusterLengths_GemmM =
    //    MPerBlock / GemmABlockCopyThreadSliceLengths_GemmM;

    // llvm::errs() << "thread cluster lengths for Matrix A\n";
    // llvm::errs() << GemmABlockCopyClusterLengths_GemmK << " ";
    // llvm::errs() << GemmABlockCopyClusterLengths_GemmM << "\n";

    // Compute ThreadSliceLengths for Matrix B.
    uint64_t GemmBBlockCopyNumberDataPerThread =
        NPerBlock * KPerBlock / BlockSize;

    uint64_t GemmBBlockCopyThreadSliceLengths_GemmK;
    uint64_t GemmBBlockCopyThreadSliceLengths_GemmN;
    switch (matrix_b_source_vector_read_dim) {
    case GemmK:
      GemmBBlockCopyThreadSliceLengths_GemmK = matrix_b_source_data_per_read;
      GemmBBlockCopyThreadSliceLengths_GemmN =
          GemmBBlockCopyNumberDataPerThread /
          GemmBBlockCopyThreadSliceLengths_GemmK;
      break;
    case GemmMorN:
      GemmBBlockCopyThreadSliceLengths_GemmN = matrix_b_source_data_per_read;
      GemmBBlockCopyThreadSliceLengths_GemmK =
          GemmBBlockCopyNumberDataPerThread /
          GemmBBlockCopyThreadSliceLengths_GemmN;
      break;
    case GemmG:
      llvm_unreachable("Vector loads/stores aren't possible in the G dimension "
                       "and should not haven been attempted");
    }

    // llvm::errs() << "thread slice lengths for Matrix B\n";
    // llvm::errs() << GemmBBlockCopyThreadSliceLengths_GemmK << " ";
    // llvm::errs() << GemmBBlockCopyThreadSliceLengths_GemmN << "\n";

    // Compute ThreadClusterLengths for Matrix B.
    // int64_t GemmBBlockCopyClusterLengths_GemmK =
    //    KPerBlock / GemmBBlockCopyThreadSliceLengths_GemmK;
    uint64_t GemmBBlockCopyClusterLengths_GemmN =
        NPerBlock / GemmBBlockCopyThreadSliceLengths_GemmN;

    // llvm::errs() << "thread cluster lengths for Matrix B\n";
    // llvm::errs() << GemmBBlockCopyClusterLengths_GemmK << " ";
    // llvm::errs() << GemmBBlockCopyClusterLengths_GemmN << "\n";

    // Compute thread_data_id_begin for Matrix A.
    // ClusterArrangeOrder for Matrix A is <1, 0>.
    // So divide by GemmABlockCopyClusterLengths_GemmK.
    auto GemmABlockCopyClusterLengths_GemmKConstantOp =
        b.create<ConstantIndexOp>(loc, GemmABlockCopyClusterLengths_GemmK);
    auto GemmABlockCopyThreadSliceLengths_GemmKConstantOp =
        b.create<ConstantIndexOp>(loc, GemmABlockCopyThreadSliceLengths_GemmK);
    auto GemmABlockCopyThreadSliceLengths_GemmMConstantOp =
        b.create<ConstantIndexOp>(loc, GemmABlockCopyThreadSliceLengths_GemmM);

    auto GemmABlockCopyThreadClusterId_Y = b.create<RemUIOp>(
        loc, tid, GemmABlockCopyClusterLengths_GemmKConstantOp);
    auto GemmABlockCopyThreadClusterId_X = b.create<DivUIOp>(
        loc, tid, GemmABlockCopyClusterLengths_GemmKConstantOp);
    auto GemmAThreadDataIdBegin_Y =
        b.create<MulIOp>(loc, GemmABlockCopyThreadClusterId_Y,
                         GemmABlockCopyThreadSliceLengths_GemmKConstantOp);
    auto GemmAThreadDataIdBegin_X =
        b.create<MulIOp>(loc, GemmABlockCopyThreadClusterId_X,
                         GemmABlockCopyThreadSliceLengths_GemmMConstantOp);

    auto GemmABlockCopySourceCoord_Y = GemmAThreadDataIdBegin_Y;
    auto GemmABlockCopySourceCoord_X =
        b.create<AddIOp>(loc, m_block_data_on_global, GemmAThreadDataIdBegin_X);
    auto GemmABlockCopyDestCoord_Y = GemmAThreadDataIdBegin_Y;
    auto GemmABlockCopyDestCoord_X = GemmAThreadDataIdBegin_X;

    // Compute thread_data_id_begin for Matrix B.
    // ClusterArrangeOrder for Matrix B is <0, 1>
    // So divide by GemmBBlockCopyClusterLengths_GemmN.
    auto GemmBBlockCopyClusterLengths_GemmNConstantOp =
        b.create<ConstantIndexOp>(loc, GemmBBlockCopyClusterLengths_GemmN);
    auto GemmBBlockCopyThreadSliceLengths_GemmKConstantOp =
        b.create<ConstantIndexOp>(loc, GemmBBlockCopyThreadSliceLengths_GemmK);
    auto GemmBBlockCopyThreadSliceLengths_GemmNConstantOp =
        b.create<ConstantIndexOp>(loc, GemmBBlockCopyThreadSliceLengths_GemmN);

    auto GemmBBlockCopyThreadClusterId_Y = b.create<DivUIOp>(
        loc, tid, GemmBBlockCopyClusterLengths_GemmNConstantOp);
    auto GemmBBlockCopyThreadClusterId_X = b.create<RemUIOp>(
        loc, tid, GemmBBlockCopyClusterLengths_GemmNConstantOp);
    auto GemmBThreadDataIdBegin_Y =
        b.create<MulIOp>(loc, GemmBBlockCopyThreadClusterId_Y,
                         GemmBBlockCopyThreadSliceLengths_GemmKConstantOp);
    auto GemmBThreadDataIdBegin_X =
        b.create<MulIOp>(loc, GemmBBlockCopyThreadClusterId_X,
                         GemmBBlockCopyThreadSliceLengths_GemmNConstantOp);

    auto GemmBBlockCopySourceCoord_Y = GemmBThreadDataIdBegin_Y;
    auto GemmBBlockCopySourceCoord_X =
        b.create<AddIOp>(loc, n_block_data_on_global, GemmBThreadDataIdBegin_X);
    auto GemmBBlockCopyDestCoord_Y = GemmBThreadDataIdBegin_Y;
    auto GemmBBlockCopyDestCoord_X = GemmBThreadDataIdBegin_X;

    auto GemmBlockCoord_G = block_work_id_g;
    // -----

    // Alocate LDS and create subviews.

    // Compute required LDS sizes.
    int64_t ldsBlockASize, ldsBlockBSize, ldsBlockSize;
    computeLDSBlockSizes(op, ldsBlockASize, ldsBlockBSize, ldsBlockSize);

    // Allocate LDS.
    auto ldsMemRefType =
        MemRefType::get({ldsBlockSize}, elementType, {},
                        gpu::GPUDialect::getWorkgroupAddressSpace());
    auto ldsGpuAllocOp = b.create<miopen::GpuAllocOp>(loc, ldsMemRefType);

    // Subviews for Matrix A.
    auto ldsBlockAOffset = 0;

    auto ldsBlockAOffsetConstantOp =
        b.create<ConstantIndexOp>(loc, ldsBlockAOffset);
    auto ldsBlockAMemRefType = computeSubviewResultType(
        op, ldsMemRefType, ldsBlockAOffset, {ldsBlockASize}, elementType);
    auto ldsBlockASubviewOp = b.create<miopen::SubviewOp>(
        loc, ldsBlockAMemRefType, ldsGpuAllocOp, ldsBlockAOffsetConstantOp);

    // Get 2D subviews.
    // Compute matrix A dimension from attributes.
    auto lds2DMatrixAMemRefType = computeSubviewResultType(
        op, ldsBlockAMemRefType, 0, {1, KPerBlock, MPerBlock}, elementType);
    auto lds2DMatrixASubviewOp = b.create<miopen::SubviewOp>(
        loc, lds2DMatrixAMemRefType, ldsBlockASubviewOp, zeroConstantOp);

    // Subviews for Matrix B.
    auto ldsBlockBOffset = ldsBlockASize;

    auto ldsBlockBOffsetConstantOp =
        b.create<ConstantIndexOp>(loc, ldsBlockBOffset);
    auto ldsBlockBMemRefType = computeSubviewResultType(
        op, ldsMemRefType, ldsBlockBOffset, {ldsBlockBSize}, elementType);
    auto ldsBlockBSubviewOp = b.create<miopen::SubviewOp>(
        loc, ldsBlockBMemRefType, ldsGpuAllocOp, ldsBlockBOffsetConstantOp);

    // Get 2D subviews.
    // Compute matrix B dimension from attributes.
    auto lds2DMatrixBMemRefType = computeSubviewResultType(
        op, ldsBlockBMemRefType, 0, {1, KPerBlock, NPerBlock}, elementType);
    auto lds2DMatrixBSubviewOp = b.create<miopen::SubviewOp>(
        loc, lds2DMatrixBMemRefType, ldsBlockBSubviewOp, zeroConstantOp);

    // -----

    // Determine vector / scalar load type for Matrix A / B.
    SmallVector<uint64_t, 3> blockwiseCopyABounds = {
        1, GemmABlockCopyThreadSliceLengths_GemmK,
        GemmABlockCopyThreadSliceLengths_GemmM};
    Type blockwiseLoadAType;
    SmallVector<Type, 8> blockwiseLoadATypes;
    int blockwiseAVectorDim;
    int blockwiseLoadAVectorLength;
    int blockwiseStoreAVectorLength;
    std::tie(blockwiseLoadAType, blockwiseAVectorDim,
             blockwiseLoadAVectorLength, blockwiseStoreAVectorLength) =
        computeLoadStoreTypeInfo(b, op, elementType, blockwiseLoadATypes,
                                 blockwiseCopyABounds, true);

    SmallVector<uint64_t, 3> blockwiseCopyBBounds = {
        1, GemmBBlockCopyThreadSliceLengths_GemmK,
        GemmBBlockCopyThreadSliceLengths_GemmN};
    Type blockwiseLoadBType;
    SmallVector<Type, 8> blockwiseLoadBTypes;
    int blockwiseBVectorDim;
    int blockwiseLoadBVectorLength;
    int blockwiseStoreBVectorLength;
    std::tie(blockwiseLoadBType, blockwiseBVectorDim,
             blockwiseLoadBVectorLength, blockwiseStoreBVectorLength) =
        computeLoadStoreTypeInfo(b, op, elementType, blockwiseLoadBTypes,
                                 blockwiseCopyBBounds, false);

    // -----

    // Compute source and destination coordinates for BlockwiseCopy ops.
    // Matrix A: {0, 0, m_block_data_on_global}, {0, 0, 0}
    // Matrix B: {0, 0, n_block_data_on_global}, {0, 0, 0}

    // -----

    // Blockwise copies before the loop.
    // Blockwise copy from global (generic tensor) to LDS (naive tensor).

    // Emit blockwise_load for matrix A.
    SmallVector<Value, 4> blockwiseLoadACoords = {GemmBlockCoord_G,
                                                  GemmABlockCopySourceCoord_Y,
                                                  GemmABlockCopySourceCoord_X};
    auto blockwiseLoadA = b.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadATypes, op.filter(), blockwiseLoadACoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadA, op, b, /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);
    // Emit blockwise_store for matrix A.
    auto blockwiseStoreA = b.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadA.getResults(), lds2DMatrixASubviewOp,
        ValueRange{zeroConstantOp, GemmABlockCopyDestCoord_Y,
                   GemmABlockCopyDestCoord_X});
    affixBlockwiseCopyAttributes(
        blockwiseStoreA, op, b, /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);

    // Emit blockwise_load for matrix B.
    SmallVector<Value, 4> blockwiseLoadBCoords = {GemmBlockCoord_G,
                                                  GemmBBlockCopySourceCoord_Y,
                                                  GemmBBlockCopySourceCoord_X};
    auto blockwiseLoadB = b.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadBTypes, op.input(), blockwiseLoadBCoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadB, op, b, /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);
    // Emit blockwise_store for matrix B.
    auto blockwiseStoreB = b.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadB.getResults(), lds2DMatrixBSubviewOp,
        ValueRange{zeroConstantOp, GemmBBlockCopyDestCoord_Y,
                   GemmBBlockCopyDestCoord_X});
    affixBlockwiseCopyAttributes(
        blockwiseStoreB, op, b, /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);

    // -----

    // Logic to do XDLOPS code selection.
    // llvm::errs() << "Invoke XDLOPS code selection logic:\n";
    // llvm::errs() << "dataType: "; dataType.dump(); llvm::errs() << "\n";
    // llvm::errs() << "MPerWave: " << MPerWave << "\n";
    // llvm::errs() << "NPerWave: " << NPerWave << "\n";

    XdlopsCodeSelection xcs =
        XdlopsCodeSelection::get(dataType, MPerWave, NPerWave, b);

    // Extract values from XdlopsCodeSelection.
    int64_t MPerXdlops = xcs.MPerXdlops;
    int64_t NPerXdlops = xcs.NPerXdlops;
    int64_t MRepeats = xcs.MRepeats;
    int64_t NRepeats = xcs.NRepeats;
    VectorType vectorType = xcs.vectorType;
    int64_t vectorNumber = xcs.vectorNumber;
    SmallVector<SmallVector<unsigned, 3>, 2> imms = xcs.imms;

    int64_t group_size = xcs.group_size;
    int64_t num_groups_blk = xcs.num_groups_blk;
    int64_t num_threads_blk = xcs.num_threads_blk;
    int64_t wave_size = xcs.wave_size;
    int64_t num_input_blks = xcs.num_input_blks;
    int64_t num_output_blks = xcs.num_output_blks;
    int64_t m = xcs.m;
    int64_t n = xcs.n;

    // -----

    // Logic to setup blockwise_gemm_v2 parameters.
    //
    // Original C++ logic:
    // index_t mMyWaveOffsetA;
    // index_t mMyWaveOffsetB;
    // const index_t waveId   = get_thread_local_1d_id() / WaveSize;
    // const index_t waveId_m = waveId / GemmNWaves;
    // const index_t waveId_n = waveId % GemmNWaves;
    // mMyWaveOffsetA = waveId_m * GemmMPerWave;
    // mMyWaveOffsetB = waveId_n * GemmNPerWave;
    auto waveId = b.create<DivUIOp>(loc, tid, waveSizeConstantOp);
    auto waveId_m = b.create<DivUIOp>(loc, waveId, NWavesConstantOp);
    auto waveId_n = b.create<RemUIOp>(loc, waveId, NWavesConstantOp);

    Value mMyWaveOffsetA, mMyWaveOffsetB;
    mMyWaveOffsetA = b.create<MulIOp>(loc, waveId_m, MPerWaveConstantOp);
    mMyWaveOffsetB = b.create<MulIOp>(loc, waveId_n, NPerWaveConstantOp);

    // Logic to setup buffers for blockwise_gemm_v2.

    bool IsKReduction = (num_output_blks == 1) && (num_input_blks > 1);
    int64_t arrayASize = (!IsKReduction)
                             ? (KPerBlock * MRepeats)
                             : (KPerBlock / num_input_blks * MRepeats);
    int64_t arrayBSize = (!IsKReduction)
                             ? (KPerBlock * NRepeats)
                             : (KPerBlock / num_input_blks * NRepeats);
    // TBD. Determine registerFloatABType.
    // Refer to commit 7bc1fcd1f8fd9ba39b12f8ec9deec3c0e3ed085b .
    auto arrayAType = MemRefType::get(
        {arrayASize}, dataType, {}, gpu::GPUDialect::getPrivateAddressSpace());
    auto arrayA = b.create<miopen::GpuAllocOp>(loc, arrayAType);
    auto arrayBType = MemRefType::get(
        {arrayBSize}, dataType, {}, gpu::GPUDialect::getPrivateAddressSpace());
    auto arrayB = b.create<miopen::GpuAllocOp>(loc, arrayBType);

    // -----

    // Logic to allocate 0-initialized vectors for C.
    SmallVector<Value, 4> vectorCs;
    SmallVector<Type, 4> vectorCTypes;
    auto vectorZeroConst = createZeroConstantFloatOp(b, loc, vectorType);
    std::fill_n(std::back_inserter(vectorCs), vectorNumber, vectorZeroConst);
    std::fill_n(std::back_inserter(vectorCTypes), vectorNumber, vectorType);

    // -----

    // Emit loop.

    int64_t loopIteration = (K - KPerBlock) / KPerBlock;

    // Assign iter args.
    // 0: blockwise copy A src y coordinate.
    // 1: blockwise copy B src y coordinate.
    // 2-x : vectorCs.
    SmallVector<Value, 6> iterArgs = {blockwiseLoadA.sourceCoord()[1],
                                      blockwiseLoadB.sourceCoord()[1]};
    iterArgs.append(vectorCs);

    auto mfmaLoopOp = b.create<AffineForOp>(loc, 0, loopIteration, 1, iterArgs);

    // inside the loop.
    auto mfmalb = OpBuilder::atBlockBegin(mfmaLoopOp.getBody());

    const auto &mfmalArgs = mfmaLoopOp.getRegionIterArgs();
    // get vectorCs for this iteration.
    std::copy(mfmalArgs.begin() + 2, mfmalArgs.end(), vectorCs.begin());

    // Blockwise copy from global (generic tensor) to register (naive tensor).
    Value blockwiseCopyASrcUpdated =
        mfmalb.create<AddIOp>(loc, mfmalArgs[0], KPerBlockConstantOp);
    blockwiseLoadACoords[1] = blockwiseCopyASrcUpdated;
    // Emit blockwise_load for matrix A.
    auto blockwiseLoadATop = mfmalb.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadATypes, op.filter(), blockwiseLoadACoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadATop, op, b, /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);

    Value blockwiseCopyBSrcUpdated =
        mfmalb.create<AddIOp>(loc, mfmalArgs[1], KPerBlockConstantOp);
    blockwiseLoadBCoords[1] = blockwiseCopyBSrcUpdated;
    // Emit blockwise_load for matrix B.
    auto blockwiseLoadBTop = mfmalb.create<miopen::BlockwiseLoadOp>(
        loc, blockwiseLoadBTypes, op.input(), blockwiseLoadBCoords);
    affixBlockwiseCopyAttributes(
        blockwiseLoadBTop, op, b, /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);

    // LDS barrier : guarantees LDS update completion before reading out to register.
    // requires LDS fence + barrier.
    mfmalb.create<miopen::LDSBarrierOp>(loc);

    // Emit blockwise V2 GEMM.
    auto blockwiseGemmV2Op = mfmalb.create<miopen::BlockwiseGemmV2Op>(
        loc, vectorCTypes, lds2DMatrixASubviewOp, lds2DMatrixBSubviewOp,
        mMyWaveOffsetA, mMyWaveOffsetB, arrayA, arrayB, vectorCs);
    affixBlockwiseGemmV2Attributes(blockwiseGemmV2Op, op, b);

    // LDS barrier : defer the next LDS update until this round's GEMM calculation is done.
    // requires barrier only.
    mfmalb.create<miopen::LDSBarrierOp>(loc);

    // Blockwise copy from register (naive tensor) to LDS (naive tensor).
    // Emit blockwise_store for matrix A.
    auto blockwiseStoreABottom = mfmalb.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadATop.getResults(), lds2DMatrixASubviewOp,
        blockwiseStoreA.destCoord());
    affixBlockwiseCopyAttributes(
        blockwiseStoreABottom, op, b,
        /*blockwiseCopyBounds=*/blockwiseCopyABounds,
        /*vectorDim=*/blockwiseAVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadAVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreAVectorLength);
    // Emit blockwise_store for matrix B.
    auto blockwiseStoreBBottom = mfmalb.create<miopen::BlockwiseStoreOp>(
        loc, blockwiseLoadBTop.getResults(), lds2DMatrixBSubviewOp,
        blockwiseStoreB.destCoord());
    affixBlockwiseCopyAttributes(
        blockwiseStoreBBottom, op, b,
        /*blockwiseCopyBounds=*/blockwiseCopyBBounds,
        /*vectorDim=*/blockwiseBVectorDim,
        /*blockwiseLoadLength=*/blockwiseLoadBVectorLength,
        /*blockwiseStoreLength=*/blockwiseStoreBVectorLength);

    // Update iter args.
    // blockwiseCopyASrcVector and blockwiseCopyBSrcVector are updated.
    iterArgs[0] = blockwiseCopyASrcUpdated;
    iterArgs[1] = blockwiseCopyBSrcUpdated;
    // blockwise_gemm_v2 updates iter args[4-].
    std::copy(blockwiseGemmV2Op.getResults().begin(),
              blockwiseGemmV2Op.getResults().end(), iterArgs.begin() + 2);

    // emit loop yield so iter args can be passed to the next iteration.
    mfmalb.create<AffineYieldOp>(loc, iterArgs);
    // outside the loop.

    // Emit loop tail.

    // LDS barrier.
    b.create<miopen::LDSBarrierOp>(loc);

    // get vectorCs for loop tail.
    std::copy(mfmaLoopOp.getResults().begin() + 2,
              mfmaLoopOp.getResults().end(), vectorCs.begin());

    // Emit blockwise GEMM for the loop tail.
    auto blockwiseGemmV2TailOp = b.create<miopen::BlockwiseGemmV2Op>(
        loc, vectorCTypes, lds2DMatrixASubviewOp, lds2DMatrixBSubviewOp,
        mMyWaveOffsetA, mMyWaveOffsetB, arrayA, arrayB, vectorCs);
    affixBlockwiseGemmV2Attributes(blockwiseGemmV2TailOp, op, b);

    // -----

    // Matrix C write out logic.

    // Original C++ logic.
    // __device__ static constexpr index_t GetNumBlksPerXdlops() {
    //     return (MPerXdlops * NPerXdlops) / (mfma_type.m * mfma_type.n);
    // }
    //
    // struct OutputLayout {
    //     __device__ static constexpr index_t GetBlkSize() { return
    //     mfma_type.num_regs_blk; }
    //     __device__ static constexpr index_t GetNumBlks() {
    //         return GetNumBlksPerXdlops() * MRepeats * NRepeats;
    //     }
    // };
    // using CThreadCopySliceLengths = Sequence<M0, 1, M2, 1>;
    // constexpr index_t BlkSize = blockwise_gemm.GetBlkSize();
    // constexpr index_t NumBlks = blockwise_gemm.GetNumBlks();

    // int64_t BlkSize = xcs.num_regs_blk;
    int64_t NumBlksPerXdlops = (MPerXdlops * NPerXdlops) / (m * n);
    int64_t NumBlks = NumBlksPerXdlops * MRepeats * NRepeats;

    int64_t iterationsPerVectorC = NumBlks / vectorNumber;
    int64_t vectorCoffset = vectorType.getShape()[0] / iterationsPerVectorC;

    // llvm::errs() << "MPerXlops: " << MPerXdlops << "\n";
    // llvm::errs() << "NPerXlops: " << NPerXdlops << "\n";
    // llvm::errs() << "m: " << m << "\n";
    // llvm::errs() << "n: " << n << "\n";
    // llvm::errs() << "MRepeat: " << MRepeats << "\n";
    // llvm::errs() << "NRepeat: " << NRepeats << "\n\n";

    // llvm::errs() << "BlkSize: " << BlkSize << "\n";
    // llvm::errs() << "NumBlksPerXdlops: " << NumBlksPerXdlops << "\n";
    // llvm::errs() << "NumBlks: " << NumBlks << "\n\n";

    // llvm::errs() << "iterationsPerVectorC: " << iterationsPerVectorC << "\n";
    // llvm::errs() << "vectorCoffset: " << vectorCoffset << "\n";

    auto group_size_ConstantOp = b.create<ConstantIndexOp>(loc, group_size);
    auto wave_size_ConstantOp = b.create<ConstantIndexOp>(loc, wave_size);
    auto num_threads_blk_ConstantOp =
        b.create<ConstantIndexOp>(loc, num_threads_blk);

    // Threadwise copy from register (naive tensor) to global (generic tensor).

    int64_t M3 = num_groups_blk;
    int64_t M1 = num_input_blks;
    int64_t M2 = group_size;
    int64_t M0 = M / (M1 * M2);
    int64_t N1 = group_size;
    int64_t N0 = N / N1;
    // llvm::errs() << "M0: " << M0 << "\n";
    // llvm::errs() << "M1: num_input_blks: " << M1 << "\n";
    // llvm::errs() << "M2: group_size: " << M2 << "\n";
    // llvm::errs() << "M3: num_groups_blk: " << M3 << "\n\n";

    auto M2ConstantOp = b.create<ConstantIndexOp>(loc, M2);
    auto M2TimesM1Op = b.create<ConstantIndexOp>(loc, M2 * M1);
    auto N1ConstantOp = M2ConstantOp;

    auto laneId_xdlops_gemm = b.create<RemUIOp>(loc, tid, wave_size_ConstantOp);
    auto blk_id_xdlops_gemm =
        b.create<DivUIOp>(loc, laneId_xdlops_gemm, num_threads_blk_ConstantOp);
    auto blk_td_xdlops_gemm =
        b.create<RemUIOp>(loc, laneId_xdlops_gemm, num_threads_blk_ConstantOp);

    auto gemmCVectorizedMatrixDim =
        op->getAttrOfType<IntegerAttr>("matrix_c_source_vector_read_dim");
    int64_t matrixCDataPerCopy =
        op->getAttrOfType<IntegerAttr>("matrix_c_data_per_copy").getInt();

    constexpr int64_t swizzleGroup = 4;
    // Ensure that the prerequisites are met
    // - The N dimension of the output will be stored vectorized
    // - The lowest level of splitting in registers is equal to swizzleGroup
    //    so transpose is well defined
    // - None of the larger dimensions of interest have overhangs that lead to
    //    incomplete transposes
    // - The writes will vectorize: if we're not getting vectorization
    //    due to HW % swizzleGroup != 0, then there's no point
    bool enableOutSwizzles =
        gemmCVectorizedMatrixDim.getInt() == gemmCDimN &&
        (matrixCDataPerCopy >= swizzleGroup) &&
        (M2 == swizzleGroup && (m % swizzleGroup == 0) &&
         (n % swizzleGroup == 0) && (MPerWave % swizzleGroup == 0) &&
         (NPerWave % swizzleGroup == 0));
    const auto &tailResults = blockwiseGemmV2TailOp->getResults();
    auto *ctx = op.getContext();

    Value newOutputTransformOp;
    SmallVector<Value, 4> vectors;
    vectors.reserve(tailResults.size());
    AffineMap tensorToXdlopsVectorMap;
    if (enableOutSwizzles) {
      // The swizzle operation doesn't fundamentally affect the mapping
      // of "expanded GEMM" (G x M0 X M1 X M2 X N) to GEMM (G X M X N)
      // space, just how we walk across it and where each thread starts.

      // However, because of the 4x4 transpose we'll be imposing
      // instead of holding N constant and walking up the M2 dimension,
      // we'll need to take 4 steps in the N dimension but hold the
      // divisible-by-4 part of the N coordinate constant. Therefore, we need to
      // break the N dimension into N0 and N1 The affine map remains otherwise
      // unchanged and becomes
      //  (d0, d1, d2, d3, d4, d5) ->
      //  (d0, d1 * M1 * M2 + d2 * M2 + d3, d4 * M1 + d5)
      auto affineMap6to3 = AffineMap::get(
          6, 0,
          {getAffineDimExpr(0, ctx),
           getAffineDimExpr(1, ctx) * getAffineConstantExpr(M1, ctx) *
                   getAffineConstantExpr(M2, ctx) +
               getAffineDimExpr(2, ctx) * getAffineConstantExpr(M2, ctx) +
               getAffineDimExpr(3, ctx),
           getAffineDimExpr(4, ctx) * getAffineConstantExpr(N1, ctx) +
               getAffineDimExpr(5, ctx)},
          ctx);

      // compose with output tensor affine map.
      auto outputType = op.output().getType().template cast<MemRefType>();
      SmallVector<AffineMap> newOutputAffineMaps(
          {outputType.getLayout().getAffineMap()});
      newOutputAffineMaps.insert(newOutputAffineMaps.begin(), affineMap6to3);

      // emit TransformOp for output tensor.
      llvm::SmallVector<NamedAttribute, 3> transformedNewOutputAttrs;
      // set map attribute.
      transformedNewOutputAttrs.push_back(
          b.getNamedAttr("map", b.getAffineMapArrayAttr({affineMap6to3})));
      // set layout attribute.
      transformedNewOutputAttrs.push_back(b.getNamedAttr(
          "layout",
          b.getArrayAttr(
              {b.getDictionaryAttr(
                   {b.getNamedAttr("upper_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(0)})),
                    b.getNamedAttr("upper_layer_names",
                                   b.getArrayAttr({b.getStringAttr("g")})),
                    b.getNamedAttr("lower_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(0)})),
                    b.getNamedAttr("lower_layer_names",
                                   b.getArrayAttr({b.getStringAttr("gemmG")})),
                    b.getNamedAttr("transformation",
                                   b.getStringAttr("PassThrough"))}),
               b.getDictionaryAttr({
                   b.getNamedAttr("upper_layer_dimensions",
                                  b.getArrayAttr({b.getI32IntegerAttr(1),
                                                  b.getI32IntegerAttr(2),
                                                  b.getI32IntegerAttr(3)})),
                   b.getNamedAttr("upper_layer_names",
                                  b.getArrayAttr({b.getStringAttr("m0"),
                                                  b.getStringAttr("m1"),
                                                  b.getStringAttr("m2")})),
                   b.getNamedAttr("lower_layer_dimensions",
                                  b.getArrayAttr({b.getI32IntegerAttr(1)})),
                   b.getNamedAttr("lower_layer_names",
                                  b.getArrayAttr({b.getStringAttr("gemmM")})),
                   b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                   b.getNamedAttr("parameters",
                                  b.getArrayAttr({b.getI32IntegerAttr(M1 * M2),
                                                  b.getI32IntegerAttr(M2),
                                                  b.getI32IntegerAttr(1)})),
               }),
               b.getDictionaryAttr(
                   {b.getNamedAttr("upper_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(4),
                                                   b.getI32IntegerAttr(5)})),
                    b.getNamedAttr("upper_layer_names",
                                   b.getArrayAttr({b.getStringAttr("n0"),
                                                   b.getStringAttr("n1")})),
                    b.getNamedAttr("lower_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(2)})),
                    b.getNamedAttr("lower_layer_names",
                                   b.getArrayAttr({b.getStringAttr("gemmN")})),
                    b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                    b.getNamedAttr(
                        "parameters",
                        b.getArrayAttr({b.getI32IntegerAttr(N1),
                                        b.getI32IntegerAttr(1)}))})})));
      // set lower_layer_layout attribute.
      transformedNewOutputAttrs.push_back(b.getNamedAttr(
          "lower_layer_layout",
          b.getArrayAttr({b.getStringAttr("gemmG"), b.getStringAttr("gemmM"),
                          b.getStringAttr("gemmN")})));
      // set upper_layer_layout attribute.
      transformedNewOutputAttrs.push_back(b.getNamedAttr(
          "upper_layer_layout",
          b.getArrayAttr({b.getStringAttr("g"), b.getStringAttr("m0"),
                          b.getStringAttr("m1"), b.getStringAttr("m2"),
                          b.getStringAttr("n0"), b.getStringAttr("n1")})));
      auto newOutputType =
          MemRefType::get({G, M0, M1, M2, N0, N1}, outputType.getElementType(),
                          composeTransforms(newOutputAffineMaps));
      newOutputTransformOp = b.create<miopen::TransformOp>(
          loc, newOutputType, op.output(), transformedNewOutputAttrs,
          /*populateBounds=*/true);

      // Here is the first main effect of the swizzling transformation
      // Instead of having the fastest coordinate be the M2 dimension
      // it's now the N1 dimension, since each group of 4 values in a vector
      // corresponds to 4 successive N values after the transpose, as opposed
      // to 4 successive M values.

      // Therefore, this map is (d0, d1, d2, d3, d4, d5) -> (d1 * M3 + d5)
      tensorToXdlopsVectorMap = AffineMap::get(
          6, 0,
          {getAffineDimExpr(1, ctx) * getAffineConstantExpr(M2, ctx) +
           getAffineDimExpr(5, ctx)},
          ctx);
      // Actually perform the swizzles
      for (const Value &result : tailResults) {
        auto swizzle = b.create<miopen::InWarpTransposeOp>(
            loc, result.getType(), result, laneId_xdlops_gemm,
            b.getI32IntegerAttr(group_size), b.getI32ArrayAttr({0, 1, 2, 3}));
        vectors.push_back(swizzle);
      }
    } else {
      // build affine expression: d0 = g
      // (d0, d1, d2, d3, d4) -> (d0, d1 * M1 * M2 + d2 * M2 + d3, d4)
      auto affineMap5to3 = AffineMap::get(
          5, 0,
          {getAffineDimExpr(0, ctx),
           getAffineDimExpr(1, ctx) * getAffineConstantExpr(M1, ctx) *
                   getAffineConstantExpr(M2, ctx) +
               getAffineDimExpr(2, ctx) * getAffineConstantExpr(M2, ctx) +
               getAffineDimExpr(3, ctx),
           getAffineDimExpr(4, ctx)},
          ctx);

      // compose with output tensor affine map.
      auto outputType = op.output().getType().template cast<MemRefType>();
      SmallVector<AffineMap> newOutputAffineMaps(
          {outputType.getLayout().getAffineMap()});
      newOutputAffineMaps.insert(newOutputAffineMaps.begin(), affineMap5to3);

      // emit TransformOp for output tensor.
      llvm::SmallVector<NamedAttribute, 3> transformedNewOutputAttrs;
      // set map attribute.
      transformedNewOutputAttrs.push_back(
          b.getNamedAttr("map", b.getAffineMapArrayAttr({affineMap5to3})));
      // set layout attribute.
      transformedNewOutputAttrs.push_back(b.getNamedAttr(
          "layout",
          b.getArrayAttr(
              {b.getDictionaryAttr(
                   {b.getNamedAttr("upper_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(0)})),
                    b.getNamedAttr("upper_layer_names",
                                   b.getArrayAttr({b.getStringAttr("g")})),
                    b.getNamedAttr("lower_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(0)})),
                    b.getNamedAttr("lower_layer_names",
                                   b.getArrayAttr({b.getStringAttr("gemmG")})),
                    b.getNamedAttr("transformation",
                                   b.getStringAttr("PassThrough"))}),
               b.getDictionaryAttr({
                   b.getNamedAttr("upper_layer_dimensions",
                                  b.getArrayAttr({b.getI32IntegerAttr(1),
                                                  b.getI32IntegerAttr(2),
                                                  b.getI32IntegerAttr(3)})),
                   b.getNamedAttr("upper_layer_names",
                                  b.getArrayAttr({b.getStringAttr("m0"),
                                                  b.getStringAttr("m1"),
                                                  b.getStringAttr("m2")})),
                   b.getNamedAttr("lower_layer_dimensions",
                                  b.getArrayAttr({b.getI32IntegerAttr(1)})),
                   b.getNamedAttr("lower_layer_names",
                                  b.getArrayAttr({b.getStringAttr("gemmM")})),
                   b.getNamedAttr("transformation", b.getStringAttr("Embed")),
                   b.getNamedAttr("parameters",
                                  b.getArrayAttr({b.getI32IntegerAttr(M1 * M2),
                                                  b.getI32IntegerAttr(M2),
                                                  b.getI32IntegerAttr(1)})),
               }),
               b.getDictionaryAttr(
                   {b.getNamedAttr("upper_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(4)})),
                    b.getNamedAttr("upper_layer_names",
                                   b.getArrayAttr({b.getStringAttr("n")})),
                    b.getNamedAttr("lower_layer_dimensions",
                                   b.getArrayAttr({b.getI32IntegerAttr(2)})),
                    b.getNamedAttr("lower_layer_names",
                                   b.getArrayAttr({b.getStringAttr("gemmN")})),
                    b.getNamedAttr("transformation",
                                   b.getStringAttr("PassThrough"))})})));
      // set lower_layer_layout attribute.
      transformedNewOutputAttrs.push_back(b.getNamedAttr(
          "lower_layer_layout",
          b.getArrayAttr({b.getStringAttr("gemmG"), b.getStringAttr("gemmM"),
                          b.getStringAttr("gemmN")})));
      // set upper_layer_layout attribute.
      transformedNewOutputAttrs.push_back(b.getNamedAttr(
          "upper_layer_layout",
          b.getArrayAttr({b.getStringAttr("g"), b.getStringAttr("m0"),
                          b.getStringAttr("m1"), b.getStringAttr("m2"),
                          b.getStringAttr("n")})));
      auto newOutputType =
          MemRefType::get({G, M0, M1, M2, N}, outputType.getElementType(),
                          composeTransforms(newOutputAffineMaps));
      newOutputTransformOp = b.create<miopen::TransformOp>(
          loc, newOutputType, op.output(), transformedNewOutputAttrs,
          /*populateBounds=*/true);

      // Build affine expression for Sequence<1, M0, 1, M2, 1>
      // (d0, d1, d2, d3, d4) -> (d1 * M2 + d3)
      tensorToXdlopsVectorMap = AffineMap::get(
          5, 0,
          {getAffineDimExpr(1, ctx) * getAffineConstantExpr(M2, ctx) +
           getAffineDimExpr(3, ctx)},
          op.getContext());
      std::copy(tailResults.begin(), tailResults.end(),
                std::back_inserter(vectors));
    }
    Value c_thread_mtx_index_row, c_thread_mtx_index_col;
    Value m_thread_data_on_global, n_thread_data_on_global;

    // emit unrolled loop.
    for (int64_t iter = 0; iter < NumBlks; ++iter) {
      // In gridwise_gemm_xdlops.hpp:
      //
      // Original C++ logic:
      // const auto c_thread_mtx_on_block =
      // blockwise_gemm.GetBeginOfThreadMatrixC(i); const index_t
      // m_thread_data_on_global =
      //     m_block_data_on_global + c_thread_mtx_on_block.row;
      // const index_t n_thread_data_on_global =
      //     n_block_data_on_global + c_thread_mtx_on_block.col;

      // compute thread_mtx_on_blk_row and thread_mtx_on_blk_col.

      // Original C++ logic.
      //
      // In xdlops_gemm.hpp:
      //
      // static constexpr bool IsABroadcast() { return NPerXdlops >= MPerXdlops;
      // }
      // __device__ static MatrixIndex GetBeginOfThreadBlk(index_t i) {
      //     const index_t xdlops_i = i / GetNumBlksPerXdlops();
      //     const index_t j        = i % GetNumBlksPerXdlops();
      //     const index_t m_i = xdlops_i / NRepeats;
      //     const index_t n_i = xdlops_i % NRepeats;
      //     const index_t laneId = get_thread_local_1d_id() %
      //     mfma_type.wave_size; const index_t blk_id = laneId /
      //     mfma_type.num_threads_blk; const index_t blk_td = laneId %
      //     mfma_type.num_threads_blk; index_t col_blk = j %
      //     mfma_type.num_output_blks; index_t row_blk = j /
      //     mfma_type.num_output_blks; static_if<!IsABroadcast>{}([&](auto) {
      //         col_blk = j / mfma_type.num_output_blks;
      //         row_blk = j % mfma_type.num_output_blks;
      //     });
      //     index_t col = col_blk * mfma_type.n + blk_td + n_i * NPerXdlops;
      //     index_t row = row_blk * mfma_type.m + blk_id * mfma_type.group_size
      //     + m_i * MPerXdlops; return MatrixIndex{row, col};
      // }
      //
      int64_t xdlops_i_xdlops_gemm = iter / NumBlksPerXdlops;
      int64_t j_xdlops_gemm = iter % NumBlksPerXdlops;
      int64_t m_i_xdlops_gemm = xdlops_i_xdlops_gemm / NRepeats;
      int64_t n_i_xdlops_gemm = xdlops_i_xdlops_gemm % NRepeats;

      int64_t col_blk_xdlops_gemm, row_blk_xdlops_gemm;
      bool IsABroadcast = (NPerXdlops >= MPerXdlops);
      if (IsABroadcast) {
        col_blk_xdlops_gemm = j_xdlops_gemm % num_output_blks;
        row_blk_xdlops_gemm = j_xdlops_gemm / num_output_blks;
      } else {
        col_blk_xdlops_gemm = j_xdlops_gemm / num_output_blks;
        row_blk_xdlops_gemm = j_xdlops_gemm % num_output_blks;
      }

      // Within a group of elements, a non-swizzled loop will output
      // to (ignoring OOB) [(i, j), (i + 1, j), (i + 2, j), (i + 3, j)]
      // for some starting position (i, j) that's a function of coordinates
      // that very slower.

      // The swizzles mean that each thread instead outputs to
      //  [(i, j), (i, j+1), (i, j+2), (i, j+3)]
      // Therefore, in order to ensure that values remain output to the correct
      // place we must map the starting coordinates through
      //  (i, j) -> (i / 4 * 4 + j % 4, j / 4 + 4 + i % 4)
      Value threadMtxColInBlock;
      if (enableOutSwizzles) {
        // The starting coordinate remap means that we must start
        // at (blk_td / 4) * 4, since blk_td % 4 is moved to the
        // row coordinate by the transpose and nothing replaces it
        // (the unswizzled row coordinate is always a multiple of 4
        // in cases where swizzles are enabled)
        threadMtxColInBlock =
            b.create<MulIOp>(loc,
                             b.create<arith::DivUIOp>(loc, blk_td_xdlops_gemm,
                                                      group_size_ConstantOp),
                             group_size_ConstantOp);
      } else {
        // Original C++ logic.
        //     index_t col = col_blk * mfma_type.n + blk_td + n_i * NPerXdlops;
        threadMtxColInBlock = blk_td_xdlops_gemm;
      }
      int64_t thread_mtx_on_blk_col_const =
          col_blk_xdlops_gemm * n + n_i_xdlops_gemm * NPerXdlops;
      Value thread_mtx_on_blk_col = b.create<AddIOp>(
          loc, threadMtxColInBlock,
          b.create<ConstantIndexOp>(loc, thread_mtx_on_blk_col_const));

      // Original C++ logic.
      //     index_t row = row_blk * mfma_type.m + blk_id * mfma_type.group_size
      //     + m_i * MPerXdlops;
      Value threadMtxRowInBlock =
          b.create<MulIOp>(loc, blk_id_xdlops_gemm, group_size_ConstantOp);
      if (enableOutSwizzles) {
        // Here, we must incorporate the mod-4 parts of blk_td
        // since while, without swizzles, these four values
        // were stored on successive threads, now they're stored
        // in four consecutive vector entries on the same thread
        threadMtxRowInBlock =
            b.create<AddIOp>(loc, threadMtxRowInBlock,
                             b.create<arith::RemUIOp>(loc, blk_td_xdlops_gemm,
                                                      group_size_ConstantOp));
      }
      int64_t thread_mtx_on_blk_row_const =
          row_blk_xdlops_gemm * m + m_i_xdlops_gemm * MPerXdlops;
      auto thread_mtx_on_blk_row = b.create<AddIOp>(
          loc, threadMtxRowInBlock,
          b.create<ConstantIndexOp>(loc, thread_mtx_on_blk_row_const));

      // compute c_thread_mtx_index_row, c_thread_mtx_index_col.
      // compute c_thread_mtx_index_row_i32, c_thread_mtx_index_col_i32.

      // In blockwise_gemm_xdlops.hpp:
      //
      // Original C++ logic:
      //  __device__ static constexpr index_t GetNumBlks()
      //      return GetNumBlksPerXdlops() * MRepeats * NRepeats;
      //
      // __device__ static MatrixIndex GetBeginOfThreadMatrixC(index_t i) {
      //     const index_t waveId = get_thread_local_1d_id() / WaveSize;
      //     const index_t xdlops_i = i /
      //     XdlopsGemm.GetOutputLayout().GetNumBlks(); const index_t j        =
      //     i % XdlopsGemm.GetOutputLayout().GetNumBlks(); const index_t m =
      //     xdlops_i / NRepeats; const index_t n = xdlops_i % NRepeats; const
      //     auto thread_mtx_on_blk = XdlopsGemm.GetBeginOfThreadBlk(j); const
      //     index_t col =
      //         (waveId % GemmNWaves) * GemmNPerWave + n * NPerXdlops +
      //         thread_mtx_on_blk.col;
      //     const index_t row =
      //         (waveId / GemmNWaves) * GemmMPerWave + m * MPerXdlops +
      //         thread_mtx_on_blk.row;
      //     return MatrixIndex{row, col};
      // }

      int64_t xdlops_i_blockwise_gemm = iter / NumBlks;
      int64_t m_blockwise_gemm = xdlops_i_blockwise_gemm / NRepeats;
      int64_t n_blockwise_gemm = xdlops_i_blockwise_gemm % NRepeats;

      // Original C++ logic.
      // const index_t col = (waveId % GemmNWaves) * GemmNPerWave + n *
      // NPerXdlops + thread_mtx_on_blk.col;
      c_thread_mtx_index_col = b.create<AddIOp>(
          loc,
          b.create<AddIOp>(
              loc,
              b.create<MulIOp>(loc,
                               b.create<RemUIOp>(loc, waveId, NWavesConstantOp),
                               NPerWaveConstantOp),
              b.create<ConstantIndexOp>(loc, n_blockwise_gemm * NPerXdlops)),
          thread_mtx_on_blk_col);

      // Original C++ logic.
      // const index_t row = (waveId / GemmNWaves) * GemmMPerWave + m *
      // MPerXdlops + thread_mtx_on_blk.row;
      c_thread_mtx_index_row = b.create<AddIOp>(
          loc,
          b.create<AddIOp>(
              loc,
              b.create<MulIOp>(loc,
                               b.create<DivUIOp>(loc, waveId, NWavesConstantOp),
                               MPerWaveConstantOp),
              b.create<ConstantIndexOp>(loc, m_blockwise_gemm * MPerXdlops)),
          thread_mtx_on_blk_row);

      // In gridwise_gemm_xdlops.hpp:
      //
      // const auto c_thread_mtx_on_block =
      // blockwise_gemm.GetBeginOfThreadMatrixC(i); const index_t
      // m_thread_data_on_global =
      //     m_block_data_on_global + c_thread_mtx_on_block.row;
      // const index_t n_thread_data_on_global =
      //     n_block_data_on_global + c_thread_mtx_on_block.col;

      m_thread_data_on_global =
          b.create<AddIOp>(loc, m_block_data_on_global, c_thread_mtx_index_row);
      n_thread_data_on_global =
          b.create<AddIOp>(loc, n_block_data_on_global, c_thread_mtx_index_col);

      SmallVector<Value, 6> matrixCThreadwiseCopySourceCoords;
      SmallVector<Value, 6> matrixCThreadwiseCopyDestCoords;
      if (enableOutSwizzles) {
        std::fill_n(std::back_inserter(matrixCThreadwiseCopySourceCoords), 6,
                    zeroConstantOp.getResult());
        matrixCThreadwiseCopyDestCoords.append(
            {// g
             GemmBlockCoord_G,
             // m_thread_data_on_global / (M2 * M1)
             b.create<DivUIOp>(loc, m_thread_data_on_global, M2TimesM1Op),
             // m_thread_data_on_global % (M2 * M1) / M2
             b.create<DivUIOp>(
                 loc,
                 b.create<RemUIOp>(loc, m_thread_data_on_global, M2TimesM1Op),
                 M2ConstantOp),
             // m_thread_data_on_global % M2
             b.create<RemUIOp>(loc, m_thread_data_on_global, M2ConstantOp),
             // n_thread_data_on_global / N1
             b.create<DivUIOp>(loc, n_thread_data_on_global, N1ConstantOp),
             // n_thread-data_on_global % N1
             b.create<RemUIOp>(loc, n_thread_data_on_global, N1ConstantOp)});
      } else {
        std::fill_n(std::back_inserter(matrixCThreadwiseCopySourceCoords), 5,
                    zeroConstantOp.getResult());
        matrixCThreadwiseCopyDestCoords.append(
            {// g
             GemmBlockCoord_G,
             // m_thread_data_on_global / (M2 * M1)
             b.create<DivUIOp>(loc, m_thread_data_on_global, M2TimesM1Op),
             // m_thread_data_on_global % (M2 * M1) / M2
             b.create<DivUIOp>(
                 loc,
                 b.create<RemUIOp>(loc, m_thread_data_on_global, M2TimesM1Op),
                 M2ConstantOp),
             // m_thread_data_on_global % M2
             b.create<RemUIOp>(loc, m_thread_data_on_global, M2ConstantOp),
             // n_thread_data_on_global
             n_thread_data_on_global});
      }
      // Select which vector C to use, and offset.
      int64_t vectorCIndex = iter / iterationsPerVectorC;
      int64_t vectorCOffset = vectorCoffset * (iter % iterationsPerVectorC);
      auto vectorCOffsetConstantAttr = b.getIndexAttr(vectorCOffset);

      // Emit threadwise_copy_v2.
      auto threadwiseCopyV2CMatrixOp = b.create<miopen::ThreadwiseCopyV2Op>(
          loc, vectors[vectorCIndex], newOutputTransformOp,
          vectorCOffsetConstantAttr, matrixCThreadwiseCopySourceCoords,
          matrixCThreadwiseCopyDestCoords);
      affixThreadwiseCopyV2Attributes(threadwiseCopyV2CMatrixOp, op, b,
                                      enableOutSwizzles);

      if (enableOutSwizzles) {
        // affix coord_transforms attributes.
        threadwiseCopyV2CMatrixOp->setAttr(
            "coord_transforms",
            b.getArrayAttr({b.getDictionaryAttr({
                b.getNamedAttr("operand", b.getI32IntegerAttr(0)),
                b.getNamedAttr("transforms", b.getAffineMapArrayAttr(
                                                 tensorToXdlopsVectorMap)),
                b.getNamedAttr(
                    "metadata",
                    b.getArrayAttr({
                        b.getDictionaryAttr(
                            {b.getNamedAttr("map",
                                            b.getAffineMapArrayAttr(
                                                {tensorToXdlopsVectorMap})),
                             b.getNamedAttr(
                                 "layout",
                                 b.getArrayAttr({
                                     b.getDictionaryAttr(
                                         {b.getNamedAttr(
                                              "lower_layer_dimensions",
                                              b.getArrayAttr(
                                                  {b.getI32IntegerAttr(0)})),
                                          b.getNamedAttr(
                                              "lower_layer_names",
                                              b.getArrayAttr(
                                                  {b.getStringAttr("raw")})),
                                          b.getNamedAttr(
                                              "transformation",
                                              b.getStringAttr("Embed")),
                                          b.getNamedAttr(
                                              "upper_layer_dimensions",
                                              b.getArrayAttr(
                                                  {b.getI32IntegerAttr(0),
                                                   b.getI32IntegerAttr(1),
                                                   b.getI32IntegerAttr(2),
                                                   b.getI32IntegerAttr(3),
                                                   b.getI32IntegerAttr(4),
                                                   b.getI32IntegerAttr(5)})),
                                          b.getNamedAttr(
                                              "parameters",
                                              b.getArrayAttr(
                                                  {b.getI32IntegerAttr(M3 * N1),
                                                   b.getI32IntegerAttr(N1),
                                                   b.getI32IntegerAttr(N1),
                                                   b.getI32IntegerAttr(N1),
                                                   b.getI32IntegerAttr(N1),
                                                   b.getI32IntegerAttr(1)})),
                                          b.getNamedAttr(
                                              "upper_layer_names",
                                              b.getArrayAttr(
                                                  {b.getStringAttr("dim0"),
                                                   b.getStringAttr("m3"),
                                                   b.getStringAttr("dim2"),
                                                   b.getStringAttr("dim3"),
                                                   b.getStringAttr("dim4"),
                                                   b.getStringAttr(
                                                       "n1")}))}) // dicitionary
                                                                  // attr inside
                                                                  // layout
                                 })),                             // layout
                             b.getNamedAttr("lower_layer_bounds",
                                            b.getArrayAttr({b.getI32IntegerAttr(
                                                1 * M3 * 1 * 1 * 1 * N1)})),
                             b.getNamedAttr(
                                 "lower_layer_layout",
                                 b.getArrayAttr({b.getStringAttr("raw")})),
                             b.getNamedAttr(
                                 "upper_layer_bounds",
                                 b.getArrayAttr({b.getI32IntegerAttr(1),
                                                 b.getI32IntegerAttr(M3),
                                                 b.getI32IntegerAttr(1),
                                                 b.getI32IntegerAttr(1),
                                                 b.getI32IntegerAttr(1),
                                                 b.getI32IntegerAttr(N1)})),
                             b.getNamedAttr(
                                 "upper_layer_layout",
                                 b.getArrayAttr({b.getStringAttr("dim0"),
                                                 b.getStringAttr("m3"),
                                                 b.getStringAttr("dim2"),
                                                 b.getStringAttr("dim3"),
                                                 b.getStringAttr("dim4"),
                                                 b.getStringAttr(
                                                     "n1")}))}) // metadata dict
                    }))                                         // metadata
            })}));

        // affix bound attributes.
        threadwiseCopyV2CMatrixOp->setAttr("bound", b.getArrayAttr({
                                                        b.getI32IntegerAttr(1),
                                                        b.getI32IntegerAttr(M3),
                                                        b.getI32IntegerAttr(1),
                                                        b.getI32IntegerAttr(1),
                                                        b.getI32IntegerAttr(1),
                                                        b.getI32IntegerAttr(N1),
                                                    }));
      } else {
        // affix coord_transforms attributes.
        threadwiseCopyV2CMatrixOp->setAttr(
            "coord_transforms",
            b.getArrayAttr({b.getDictionaryAttr({
                b.getNamedAttr("operand", b.getI32IntegerAttr(0)),
                b.getNamedAttr("transforms", b.getAffineMapArrayAttr(
                                                 tensorToXdlopsVectorMap)),
                b.getNamedAttr(
                    "metadata",
                    b.getArrayAttr({
                        b.getDictionaryAttr(
                            {b.getNamedAttr("map",
                                            b.getAffineMapArrayAttr(
                                                {tensorToXdlopsVectorMap})),
                             b.getNamedAttr(
                                 "layout",
                                 b.getArrayAttr({
                                     b.getDictionaryAttr(
                                         {b.getNamedAttr(
                                              "lower_layer_dimensions",
                                              b.getArrayAttr(
                                                  {b.getI32IntegerAttr(0)})),
                                          b.getNamedAttr(
                                              "lower_layer_names",
                                              b.getArrayAttr(
                                                  {b.getStringAttr("raw")})),
                                          b.getNamedAttr(
                                              "transformation",
                                              b.getStringAttr("Embed")),
                                          b.getNamedAttr(
                                              "upper_layer_dimensions",
                                              b.getArrayAttr(
                                                  {b.getI32IntegerAttr(0),
                                                   b.getI32IntegerAttr(1),
                                                   b.getI32IntegerAttr(2),
                                                   b.getI32IntegerAttr(3),
                                                   b.getI32IntegerAttr(4)})),
                                          b.getNamedAttr(
                                              "parameters",
                                              b.getArrayAttr(
                                                  {b.getI32IntegerAttr(M3 * M2),
                                                   b.getI32IntegerAttr(M2),
                                                   b.getI32IntegerAttr(M2),
                                                   b.getI32IntegerAttr(1),
                                                   b.getI32IntegerAttr(1)})),
                                          b.getNamedAttr(
                                              "upper_layer_names",
                                              b.getArrayAttr(
                                                  {b.getStringAttr("dim0"),
                                                   b.getStringAttr("m3"),
                                                   b.getStringAttr("dim2"),
                                                   b.getStringAttr("m2"),
                                                   b.getStringAttr(
                                                       "dim4")}))}) // dicitionary
                                                                    // attr
                                                                    // inside
                                                                    // layout
                                 })), // layout
                             b.getNamedAttr("lower_layer_bounds",
                                            b.getArrayAttr({b.getI32IntegerAttr(
                                                1 * M3 * 1 * M2 * 1)})),
                             b.getNamedAttr(
                                 "lower_layer_layout",
                                 b.getArrayAttr({b.getStringAttr("raw")})),
                             b.getNamedAttr(
                                 "upper_layer_bounds",
                                 b.getArrayAttr({b.getI32IntegerAttr(1),
                                                 b.getI32IntegerAttr(M3),
                                                 b.getI32IntegerAttr(1),
                                                 b.getI32IntegerAttr(M2),
                                                 b.getI32IntegerAttr(1)})),
                             b.getNamedAttr(
                                 "upper_layer_layout",
                                 b.getArrayAttr(
                                     {b.getStringAttr("dim0"),
                                      b.getStringAttr("m3"),
                                      b.getStringAttr("dim2"),
                                      b.getStringAttr("m2"),
                                      b.getStringAttr("dim4")}))}) // metadata
                                                                   // dict
                    }))                                            // metadata
            })}));

        // affix bound attributes.
        threadwiseCopyV2CMatrixOp->setAttr("bound", b.getArrayAttr({
                                                        b.getI32IntegerAttr(1),
                                                        b.getI32IntegerAttr(M3),
                                                        b.getI32IntegerAttr(1),
                                                        b.getI32IntegerAttr(M2),
                                                        b.getI32IntegerAttr(1),
                                                    }));
      }
      auto dataOperation = op->getAttr("data_operation");
      if (dataOperation) {
        threadwiseCopyV2CMatrixOp->setAttr("data_operation", dataOperation);
      }
    }

    op.erase();

    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseGemm lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseGemmRewritePattern
    : public OpRewritePattern<miopen::BlockwiseGemmOp> {
  using OpRewritePattern<miopen::BlockwiseGemmOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::BlockwiseGemmOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    // Prepare some useful constants.
    auto zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);

    auto blockAType = op.matrixA().getType().cast<MemRefType>();

    auto elementType =
        op.matrixC().getType().cast<MemRefType>().getElementType();

    // Obtain critical matrix dimensions.
    int64_t K = blockAType.getShape()[1];

    // Non-xdlops path.

    // Obtain critical attributes.
    int64_t KPerThread =
        op->getAttr("k_per_thread").template cast<IntegerAttr>().getInt();
    int64_t MPerThread =
        op.matrixC().getType().template cast<MemRefType>().getShape()[1];
    int64_t NPerThread =
        op.matrixC().getType().template cast<MemRefType>().getShape()[2];
    int64_t MPerThreadSubC =
        op->getAttr("m_per_thread").template cast<IntegerAttr>().getInt();
    int64_t NPerThreadSubC =
        op->getAttr("n_per_thread").template cast<IntegerAttr>().getInt();

    // llvm::errs() << "MPerThread: " << MPerThread << "\n";
    // llvm::errs() << "MPerThreadSubC: " << MPerThreadSubC << "\n";
    // llvm::errs() << "NPerThread: " << NPerThread << "\n";
    // llvm::errs() << "NPerThreadSubC: " << NPerThreadSubC << "\n";

    auto MPerThreadSubCConstantOp =
        b.create<ConstantIndexOp>(loc, MPerThreadSubC);
    auto NPerThreadSubCConstantOp =
        b.create<ConstantIndexOp>(loc, NPerThreadSubC);

    int64_t MLevel0Cluster =
        op->getAttr("m_level0_cluster").template cast<IntegerAttr>().getInt();
    int64_t MLevel1Cluster =
        op->getAttr("m_level1_cluster").template cast<IntegerAttr>().getInt();
    int64_t NLevel0Cluster =
        op->getAttr("n_level0_cluster").template cast<IntegerAttr>().getInt();
    int64_t NLevel1Cluster =
        op->getAttr("n_level1_cluster").template cast<IntegerAttr>().getInt();

    int64_t MPerLevel1Cluster =
        MPerThreadSubC * MLevel0Cluster * MLevel1Cluster;
    int64_t NPerLevel1Cluster =
        NPerThreadSubC * NLevel0Cluster * NLevel1Cluster;
    auto MPerLevel1ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, MPerLevel1Cluster);
    auto NPerLevel1ClusterConstantOp =
        b.create<ConstantIndexOp>(loc, NPerLevel1Cluster);

    int64_t MRepeat = MPerThread / MPerThreadSubC;
    int64_t NRepeat = NPerThread / NPerThreadSubC;

    // Alloc register for thread_a and thread_b.
    auto threadARegisterMemRefType =
        MemRefType::get({1, KPerThread, MPerThread}, elementType, {},
                        gpu::GPUDialect::getPrivateAddressSpace());
    auto threadAAllocOp =
        b.create<miopen::GpuAllocOp>(loc, threadARegisterMemRefType);

    auto threadBRegisterMemRefType =
        MemRefType::get({1, KPerThread, NPerThread}, elementType, {},
                        gpu::GPUDialect::getPrivateAddressSpace());
    auto threadBAllocOp =
        b.create<miopen::GpuAllocOp>(loc, threadBRegisterMemRefType);

    // Main loop.
    auto loopIteration = K / KPerThread;
    auto loopOp = b.create<AffineForOp>(loc, 0, loopIteration);

    // inside the main loop.
    auto lb = OpBuilder::atBlockTerminator(loopOp.getBody());

    auto iv = loopOp.getInductionVar();

    // read matrix A loop.
    auto loopReadMatrixAIteration = MRepeat;
    auto loopReadMatrixAOp =
        lb.create<AffineForOp>(loc, 0, loopReadMatrixAIteration);

    // inside read matrix A loop.
    auto lab = OpBuilder::atBlockTerminator(loopReadMatrixAOp.getBody());

    auto iva = loopReadMatrixAOp.getInductionVar();

    // Threadwise copy from LDS (naive tensor) to register (generic tensor).

    // Set copy sorce and dest coordinate acoording to original C++ logic:
    SmallVector<Value, 3> matrixAThreadwiseCopySourceCoords = {
        zeroConstantOp, iv,
        lab.create<AddIOp>(
            loc, lab.create<MulIOp>(loc, iva, MPerLevel1ClusterConstantOp),
            op.threadOffsetA())};
    SmallVector<Value, 3> matrixAThreadwiseCopyDestCoords = {
        zeroConstantOp, zeroConstantOp,
        lab.create<MulIOp>(loc, iva, MPerThreadSubCConstantOp)};

    // Emit threadwise_copy.
    auto threadwiseCopyAMatrixOp = lab.create<miopen::ThreadwiseCopyOp>(
        loc, op.matrixA(), threadAAllocOp, matrixAThreadwiseCopySourceCoords,
        matrixAThreadwiseCopyDestCoords);
    affixThreadwiseCopyAttributes(threadwiseCopyAMatrixOp, op, b,
                                  /*isMatrixA=*/true);

    // read matrix B loop.
    auto loopReadMatrixBIteration = NRepeat;
    auto loopReadMatrixBOp =
        lb.create<AffineForOp>(loc, 0, loopReadMatrixBIteration);

    // inside read matrix B loop.
    auto lbb = OpBuilder::atBlockTerminator(loopReadMatrixBOp.getBody());

    auto ivb = loopReadMatrixBOp.getInductionVar();

    // Threadwise copy from LDS (naive tensor) to register (generic tensor).

    // Set copy sorce and dest coordinate acoording to original C++ logic:
    SmallVector<Value, 3> matrixBThreadwiseCopySourceCoords{
        zeroConstantOp, iv,
        lbb.create<AddIOp>(
            loc, lbb.create<MulIOp>(loc, ivb, NPerLevel1ClusterConstantOp),
            op.threadOffsetB())};
    SmallVector<Value, 3> matrixBThreadwiseCopyDestCoords = {
        zeroConstantOp, zeroConstantOp,
        lbb.create<MulIOp>(loc, ivb, NPerThreadSubCConstantOp)};

    // Emit threadwise_copy.
    auto threadwiseCopyBMatrixOp = lbb.create<miopen::ThreadwiseCopyOp>(
        loc, op.matrixB(), threadBAllocOp, matrixBThreadwiseCopySourceCoords,
        matrixBThreadwiseCopyDestCoords);
    affixThreadwiseCopyAttributes(threadwiseCopyBMatrixOp, op, b,
                                  /*isMatrixA=*/false);

    lb.create<miopen::ThreadwiseGemmOp>(loc, threadAAllocOp, threadBAllocOp,
                                        op.matrixC());

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseCopy lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseCopyRewritePattern
    : public OpRewritePattern<miopen::BlockwiseCopyOp> {
  using OpRewritePattern<miopen::BlockwiseCopyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::BlockwiseCopyOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    MemRefType sourceType = op.source().getType().cast<MemRefType>();
    MemRefType destType = op.dest().getType().cast<MemRefType>();
    MemRefType bufferType;
    if (op.buffer())
      bufferType = op.buffer().getType().cast<MemRefType>();

    // Prepare some useful constants.
    auto zeroConstantOp = b.create<ConstantIndexOp>(loc, 0);

    // Check the address spaces of source and destination values and determine
    // lowering logic.
    // - 0 (global) -> 3 (LDS) : load + store
    // - 0 (global) -> 5 (register) : load
    // - 5 (register) -> 3 (LDS) : store
    if (sourceType.getMemorySpaceAsInt() == 0 && destType.getMemorySpaceAsInt() == 3) {
      // Threadwise copy from global (generic tensor) to register (naive
      // tensor).
      SmallVector<Value, 5> threadwiseCopyBufferCoords;
      std::fill_n(std::back_inserter(threadwiseCopyBufferCoords),
                  bufferType.getRank(), zeroConstantOp.getResult());

      auto threadwiseCopyLoadOp = b.create<miopen::ThreadwiseCopyOp>(
          loc, op.source(), op.buffer(), op.sourceCoord(),
          threadwiseCopyBufferCoords);
      affixThreadwiseCopyAttributes(threadwiseCopyLoadOp, op, b,
                                    /*isThreadwiseLoad=*/true);

      // Threadwise copy from register (naive tensor) to LDS (naive tensor).
      auto threadwiseCopyStoreOp = b.create<miopen::ThreadwiseCopyOp>(
          loc, op.buffer(), op.dest(), threadwiseCopyBufferCoords,
          op.destCoord());
      affixThreadwiseCopyAttributes(threadwiseCopyStoreOp, op, b,
                                    /*isThreadwiseLoad=*/false);
    } else if (sourceType.getMemorySpaceAsInt() == 0 &&
               destType.getMemorySpaceAsInt() == 5) {
      // Threadwise copy from global (generic tensor) to register (naive
      // tensor).
      SmallVector<Value, 5> threadwiseCopyDestCoords;
      std::fill_n(std::back_inserter(threadwiseCopyDestCoords),
                  destType.getRank(), zeroConstantOp.getResult());

      auto threadwiseCopyLoadOp = b.create<miopen::ThreadwiseCopyOp>(
          loc, op.source(), op.dest(), op.sourceCoord(),
          threadwiseCopyDestCoords);
      affixThreadwiseCopyAttributes(threadwiseCopyLoadOp, op, b,
                                    /*isThreadwiseLoad=*/true);
    } else if (sourceType.getMemorySpaceAsInt() == 5 &&
               destType.getMemorySpaceAsInt() == 3) {
      // Threadwise copy from register (naive tensor) to LDS (naive tensor).
      SmallVector<Value, 5> threadwiseCopySourceCoords;
      std::fill_n(std::back_inserter(threadwiseCopySourceCoords),
                  sourceType.getRank(), zeroConstantOp.getResult());

      auto threadwiseCopyStoreOp = b.create<miopen::ThreadwiseCopyOp>(
          loc, op.source(), op.dest(), threadwiseCopySourceCoords,
          op.destCoord());
      affixThreadwiseCopyAttributes(threadwiseCopyStoreOp, op, b,
                                    /*isThreadwiseLoad=*/false);
    } else {
      return op.emitOpError("UNSUPPORTED ThreadwiseCopyOp\n");
    }

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseLoad lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseLoadRewritePattern
    : public OpRewritePattern<miopen::BlockwiseLoadOp> {
  using OpRewritePattern<miopen::BlockwiseLoadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::BlockwiseLoadOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();
    TypeRange resultTypes = op.result().getTypes();

    // BlockwiseLoad only accepts the following data movement:
    // - 0 (global) -> 5 (register) : load

    // Threadwise copy from global (generic tensor) to register (naive
    // tensor).

    auto threadwiseLoadOp = b.create<miopen::ThreadwiseLoadOp>(
        loc, resultTypes, op.source(), op.sourceCoord());
    affixThreadwiseCopyAttributes(threadwiseLoadOp, op, b,
                                  /*isThreadwiseLoad=*/true);

    op.replaceAllUsesWith(threadwiseLoadOp.getResults());
    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseStore lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseStoreRewritePattern
    : public OpRewritePattern<miopen::BlockwiseStoreOp> {
  using OpRewritePattern<miopen::BlockwiseStoreOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::BlockwiseStoreOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();
    // BlockwiseLoad only accepts the following data movement:
    // - 5 (register) -> 3 (LDS) : store

    // Threadwise copy from register (naive tensor) to LDS (naive tensor).
    auto threadwiseStoreOp = b.create<miopen::ThreadwiseStoreOp>(
        loc, op.data(), op.dest(), op.destCoord());
    affixThreadwiseCopyAttributes(threadwiseStoreOp, op, b,
                                  /*isThreadwiseLoad=*/false);

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Fill lowering.
//===----------------------------------------------------------------------===//

struct FillRewritePattern : public OpRewritePattern<miopen::FillOp> {
  using OpRewritePattern<miopen::FillOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::FillOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();
    auto inputType = op.input().getType().cast<MemRefType>();
    auto inputShape = inputType.getShape();

    AffineForOp currentLoop;
    OpBuilder currentScope = b;
    std::vector<mlir::Value> range;

    for (unsigned i = 0; i < inputShape.size(); ++i) {
      // Rank 1 loop.
      currentLoop = currentScope.create<AffineForOp>(loc, 0, inputShape[i]);

      // collect current loop induction var for store indexes
      range.push_back(currentLoop.getInductionVar());

      // inside loop.
      currentScope = OpBuilder::atBlockTerminator(currentLoop.getBody());
    }

    // Store fill value
    currentScope.create<memref::StoreOp>(loc, op.value(), op.input(), range);

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// InWarpTranspose lowering.
//===----------------------------------------------------------------------===//
static constexpr size_t swizzleGroupSize = InWarpTransposeOp::swizzleGroupSize;
struct InWarpTransposeRewritePattern
    : public OpRewritePattern<miopen::InWarpTransposeOp> {
  using OpRewritePattern<miopen::InWarpTransposeOp>::OpRewritePattern;

  enum RotationDirection {
    // left rotation by 1 is a b c d -> b c d a
    // right rotation by 1 is a b c d -> d a b c
    Left,
    Right
  };

  // Emit the in-regester rotations needed for an in-register transpose
  //
  // This will rotate the values each line holds by `laneId % groupSize`
  // The emitted code uses a barrel rotator to enable performing these
  // `groupSize` different rotations in O(log(groupSize)) operations Arguments:
  // - `vector`: The vector of values to be rotated
  // - `laneId`: The current lane ID (thread ID % warpSize)
  // - `rotationDir`: whether to rotate left or right
  // - `groupSize` and `totalSize`: the size of the transpose
  // and the total vector length, respectively

  // - `lanePerm` : A mapping of physical lanes to logical lanes in each grou
  // That is, lanePerm[i] tells you where the value in lane i would "normally"
  // be, with all indices modulo the swizzle group size. If empty, the identity
  // map is used. For example, with lanePerm = [0, 2, 1, 3], lanes 1 and 3 will
  // rotate their values by 2 places, as opposed to lanes  2 and 3 Returns: The
  // vector of rotated values
  Value emitRotations(Location loc, PatternRewriter &b, Value vector,
                      Value laneId, RotationDirection dir, uint32_t groupSize,
                      uint32_t totalSize,
                      Optional<ArrayRef<uint32_t>> lanePerm) const {
    assert(totalSize % groupSize == 0 &&
           "block size is divisible by group size");

    uint32_t logGroupSize = llvm::Log2_32_Ceil(groupSize);

    int32_t base = 0, offset = 0, target = 0;
    switch (dir) {
    case Left:
      base = 0;
      offset = 1;
      target = logGroupSize;
      break;
    case Right:
      base = logGroupSize - 1;
      offset = -1;
      target = -1;
      break;
    }

    Value zeroConst = b.create<ConstantIndexOp>(loc, 0);

    llvm::SmallVector<Value, swizzleGroupSize> indexConsts;
    for (uint32_t i = 0; i < totalSize; ++i) {
      indexConsts.push_back(b.create<ConstantIndexOp>(loc, i));
    }

    Value laneInSwizzleGroup;
    if (lanePerm.hasValue()) {
      Value groupSizeConst = b.create<ConstantIndexOp>(loc, swizzleGroupSize);
      laneInSwizzleGroup = b.create<RemUIOp>(loc, laneId, groupSizeConst);
    }

    Value result = vector;

    for (int32_t logRotation = base; logRotation != target;
         logRotation += offset) {
      uint32_t rotation = 1 << logRotation;
      Value shouldParticipate;
      if (lanePerm.hasValue()) {
        // Non-standard arrangement of rows -> lanes, use longer test
        ArrayRef<uint32_t> theLanePerm = lanePerm.getValue();
        llvm::SmallVector<Value, swizzleGroupSize> comparisons;
        for (uint32_t i = 0; i < theLanePerm.size(); ++i) {
          if ((theLanePerm[i] & rotation) != 0) {
            Value toTest = b.create<ConstantIndexOp>(loc, i);
            comparisons.push_back(b.create<CmpIOp>(loc, CmpIPredicate::eq,
                                                   laneInSwizzleGroup, toTest));
          }
        }
        if (comparisons.empty()) {
          llvm_unreachable("Permutation on [0, 2^k) didn't have any entries "
                           "with some bit set");
        }
        shouldParticipate =
            std::accumulate(comparisons.begin() + 1, comparisons.end(),
                            comparisons[0], [&b, &loc](Value v1, Value v2) {
                              return b.create<OrIOp>(loc, v1, v2);
                            });
      } else { // The usual case
        Value maskConst = b.create<ConstantIndexOp>(loc, rotation);
        Value shouldParticipateVal = b.create<AndIOp>(loc, laneId, maskConst);
        shouldParticipate = b.create<CmpIOp>(loc, CmpIPredicate::ne,
                                             shouldParticipateVal, zeroConst);
      }

// TODO(kdrewnia): xplicitly emit selects until SWDEV-302607 and SWDEV-302609
// are fixed
#if 0
      scf::IfOp ifb = b.create<scf::IfOp>(
          loc, vector.getType(), shouldParticipate, /*withElseRegion=*/true);
      OpBuilder thenb = ifb.getThenBodyBuilder(b.getListener());

      Value thenResult = result;
      SmallVector<Value> extracted;
      for (uint32_t i = 0; i < totalSize; ++i) {
        extracted.push_back(thenb.create<vector::ExtractElementOp>(
            loc, thenResult, indexConsts[i]));
      }
      for (uint32_t group = 0; group < totalSize; group += groupSize) {
        for (uint32_t i = 0; i < groupSize; ++i) {
          uint32_t dest = 0xdeadbeef;
          switch (dir) {
          case Left:
            // We use groupSize - rotation to prevent underflow
            dest = (i + (groupSize - rotation)) % groupSize;
            break;
          case Right:
            dest = (i + rotation) % groupSize;
            break;
          }
          Value toInsert = extracted[group + i];
          thenResult = thenb.create<vector::InsertElementOp>(
              loc, toInsert, thenResult, indexConsts[group + dest]);
        }
      }
      thenb.create<scf::YieldOp>(loc, thenResult);

      OpBuilder elseb = ifb.getElseBodyBuilder(b.getListener());
      elseb.create<scf::YieldOp>(loc, result);

      result = ifb.getResult(0);
#endif

      SmallVector<Value> extracted;
      for (uint32_t i = 0; i < totalSize; ++i) {
        extracted.push_back(
            b.create<vector::ExtractElementOp>(loc, result, indexConsts[i]));
      }
      for (uint32_t group = 0; group < totalSize; group += groupSize) {
        for (uint32_t i = 0; i < groupSize; ++i) {
          uint32_t dest = 0xdeadbeef;
          switch (dir) {
          case Left:
            // We use groupSize - rotation to prevent underflow
            dest = (i + (groupSize - rotation)) % groupSize;
            break;
          case Right:
            dest = (i + rotation) % groupSize;
            break;
          }
          Value whenRotating = extracted[group + i];
          Value stable = extracted[group + dest];
          Value toInsert =
              b.create<SelectOp>(loc, shouldParticipate, whenRotating, stable);
          result = b.create<vector::InsertElementOp>(loc, toInsert, result,
                                                     indexConsts[group + dest]);
        }
      }
    }

    return result;
  }

  // Before calling this function, we will have emitted rotations so that the
  // group
  //  r[]: 0   1   2   3
  //  t0: 0,0 1,0 2,0 3,0
  //  t1: 0,1 1,1 2,1 3,1
  //  t2: 0,2 1,2 2,2 3,2
  //  t3: 0,3 1,3 2,3 3,3
  // will have become
  //  0,0 1,0 2,0 3,0
  //  3,1 0,1 1,1 2,1
  //  2,2 3,2 0,2 1,2
  //  1,3 2,3 3,3 0,3

  // (plus-minus size changes for other operations).
  // These rotations are the first step in the in-register transpose algorithm
  // as they allow the inter-lane shuffles to be permutation.

  // The goal of this function is to emit code that will lead to the result
  // state
  //  0,0 0,1 0,2 0,3
  //  1,3 1,0 1,1 1,2
  //  2,2 2,3 2,0 2,1
  //  3,1 3,2 3,3 3,0

  Value emitSwizzles(Location loc, PatternRewriter &b, Value vector,
                     uint32_t groupSize, uint32_t totalSize,
                     ArrayRef<uint32_t> inGroupPerm) const {

    llvm::SmallVector<ArrayAttr, swizzleGroupSize> swizzlePerms;

    llvm::SmallVector<int32_t, swizzleGroupSize> perm;
    llvm::SmallVector<uint32_t, swizzleGroupSize> have;
    llvm::SmallVector<uint32_t, swizzleGroupSize> want;
    for (uint32_t r = 0; r < groupSize; ++r) {
      perm.clear();
      have.clear();
      want.clear();

      for (uint32_t t = 0; t < swizzleGroupSize; ++t) {
        // Must correct for, say, 2x2 transpose being a 4 thread x 2 register
        // swizzle
        uint32_t smallGroupDup = groupSize * (t / groupSize);
        uint32_t preSwizzleI =
            (r + (groupSize - t)) % groupSize + smallGroupDup;
        uint32_t preSwizzleJ = t;

        uint32_t expectedThread = inGroupPerm[t];
        uint32_t postSwizzleI = expectedThread;
        uint32_t postSwizzleJ = (r + (groupSize - expectedThread)) % groupSize +
                                groupSize * (expectedThread / groupSize);
        uint32_t preSwizzleElem = preSwizzleJ + swizzleGroupSize * preSwizzleI;
        uint32_t postSwizzleElem =
            postSwizzleJ + swizzleGroupSize * postSwizzleI;
        /*         llvm::dbgs() << "//r = " << r << " t = " << t << ": " <<
           "have ("
                  << preSwizzleI << ", " << preSwizzleJ << ") = " <<
           preSwizzleElem
                  << " want (" << postSwizzleI << ", " << postSwizzleJ << ") = "
                  << postSwizzleElem << "\n"; */
        have.push_back(preSwizzleElem);
        want.push_back(postSwizzleElem);
      }

      for (uint32_t t = 0; t < swizzleGroupSize; ++t) {
        auto *srcElemIter = std::find(have.begin(), have.end(), want[t]);
        assert(srcElemIter != have.end() && "swizzle is not a permutation");
        auto readIdx = srcElemIter - have.begin();
        perm.push_back(readIdx);
      }

      if (perm[0] == 0 && perm[1] == 1 && perm[2] == 2 && perm[3] == 3) {
        swizzlePerms.push_back(b.getI32ArrayAttr({}));
      } else {
        swizzlePerms.push_back(b.getI32ArrayAttr(perm));
      }
    }

    Value result = b.create<vector::BitCastOp>(
        loc,
        VectorType::get(vector.getType().cast<VectorType>().getShape(),
                        b.getI32Type()),
        vector);
    // TODO(kdrewnia): Make this operation variadic and not just vector-valued
    SmallVector<Value> accessConsts;
    SmallVector<Value> initialRegisters;
    for (uint32_t i = 0; i < totalSize; ++i) {
      Value accessConst = b.create<ConstantIndexOp>(loc, i);
      initialRegisters.push_back(
          b.create<vector::ExtractElementOp>(loc, result, accessConst));
      accessConsts.push_back(accessConst);
    }

    SmallVector<Value> swizzledRegisters;
    for (uint32_t i = 0; i < totalSize; ++i) {
      ArrayAttr swizzleSelector = swizzlePerms[i % groupSize];
      if (0 == swizzleSelector.size()) {
        swizzledRegisters.push_back(initialRegisters[i]);
        continue;
      }
      Value swizzled = b.create<gpu::WarpSwizzleOp>(
          loc, b.getI32Type(), initialRegisters[i], swizzleSelector);
      swizzledRegisters.push_back(swizzled);
    }

    for (uint32_t i = 0; i < totalSize; ++i) {
      if (swizzledRegisters[i] != initialRegisters[i]) {
        result = b.create<vector::InsertElementOp>(loc, swizzledRegisters[i],
                                                   result, accessConsts[i]);
      }
    }

    result = b.create<vector::BitCastOp>(loc, vector.getType(), result);
    return result;
  }

  LogicalResult matchAndRewrite(miopen::InWarpTransposeOp op,
                                PatternRewriter &b) const override {
    Location loc = op.getLoc();

    Value vector = op.vector();
    uint32_t totalSize = vector.getType().cast<VectorType>().getNumElements();

    Value laneId = op.laneId();
    uint32_t groupSize = op.size();

    ArrayAttr inGroupPermAttr = op.inGroupPerm();
    llvm::SmallVector<uint32_t, swizzleGroupSize> inGroupPerm;
    auto inGroupPermArr = inGroupPermAttr.getValue();
    // ::verify() ensures this is a permutation
    for (uint32_t i = 0; i < swizzleGroupSize; ++i) {
      inGroupPerm.push_back(inGroupPermArr[i]
                                .cast<mlir::IntegerAttr>()
                                .getValue()
                                .getZExtValue());
    }

    Optional<ArrayRef<uint32_t>> maybeInGroupPerm = llvm::None;
    if (inGroupPermAttr != b.getI32ArrayAttr({0, 1, 2, 3})) {
      maybeInGroupPerm = inGroupPerm;
    }

    Value rotatedRight = emitRotations(loc, b, vector, laneId, Right, groupSize,
                                       totalSize, llvm::None);
    Value swizzled =
        emitSwizzles(loc, b, rotatedRight, groupSize, totalSize, inGroupPerm);
    Value rotatedLeft = emitRotations(loc, b, swizzled, laneId, Left, groupSize,
                                      totalSize, maybeInGroupPerm);

    op.replaceAllUsesWith(rotatedLeft);
    op.erase();

    return success();
  }
};

//===----------------------------------------------------------------------===//
// ThreadwiseGemm lowering.
//===----------------------------------------------------------------------===//

struct ThreadwiseGemmRewritePattern
    : public OpRewritePattern<miopen::ThreadwiseGemmOp> {
  using OpRewritePattern<miopen::ThreadwiseGemmOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::ThreadwiseGemmOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    auto gemmA = op.matrixA();
    auto gemmB = op.matrixB();
    auto gemmC = op.matrixC();
    auto dataType =
        gemmA.getType().template cast<MemRefType>().getElementType();

    ArrayRef<int64_t> gemmAShape =
        gemmA.getType().cast<MemRefType>().getShape();
    ArrayRef<int64_t> gemmBShape =
        gemmB.getType().cast<MemRefType>().getShape();

    auto loopG = b.create<AffineForOp>(loc, 0, gemmAShape[0]);
    auto lbG = loopG.getBody();
    b.setInsertionPointToStart(lbG);

    auto loopK = b.create<AffineForOp>(loc, 0, gemmAShape[1]);
    auto lbK = loopK.getBody();
    b.setInsertionPointToStart(lbK);

    auto loopM = b.create<AffineForOp>(loopK.getLoc(), 0, gemmAShape[2]);
    auto lbM = loopM.getBody();
    b.setInsertionPointToStart(lbM);

    auto loopN = b.create<AffineForOp>(loc, 0, gemmBShape[2]);
    auto lbN = loopN.getBody();
    b.setInsertionPointToStart(lbN);

    SmallVector<Value, 3> memIndicesKM;
    extractForInductionVars({loopG, loopK, loopM}, &memIndicesKM);
    auto gemmAKM = b.create<AffineLoadOp>(loc, gemmA, memIndicesKM);

    SmallVector<Value, 3> memIndicesKN;
    extractForInductionVars({loopG, loopK, loopN}, &memIndicesKN);
    auto gemmBKN = b.create<AffineLoadOp>(loc, gemmB, memIndicesKN);

    Value mul;
    if (dataType.isa<IntegerType>())
      mul = b.create<MulIOp>(loc, dataType, gemmAKM, gemmBKN);
    else
      mul = b.create<MulFOp>(loc, dataType, gemmAKM, gemmBKN);
    SmallVector<Value, 3> memIndicesMN;
    extractForInductionVars({loopG, loopM, loopN}, &memIndicesMN);
    auto gemmCMN = b.create<AffineLoadOp>(loc, gemmC, memIndicesMN);

    Value add;
    if (dataType.isa<IntegerType>())
      add = b.create<AddIOp>(loc, dataType, mul, gemmCMN);
    else
      add = b.create<AddFOp>(loc, dataType, mul, gemmCMN);
    b.create<AffineStoreOp>(loc, add, gemmC, memIndicesMN);

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// ThreadwiseCopy lowering.
//===----------------------------------------------------------------------===//

struct ThreadwiseCopyRewritePattern
    : public OpRewritePattern<miopen::ThreadwiseCopyOp> {
  using OpRewritePattern<miopen::ThreadwiseCopyOp>::OpRewritePattern;

  // NOTE: when extending this logic to support vectors
  // ensure the results of the non-xdlops gemm are stored in a vectorizable
  // layout. This'll likely require something analogous to the in_warp_transpose
  // call in the xdlops case
  LogicalResult matchAndRewrite(miopen::ThreadwiseCopyOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    auto sourceElementType =
        op.source().getType().cast<MemRefType>().getElementType().cast<Type>();
    auto destElementType =
        op.dest().getType().cast<MemRefType>().getElementType().cast<Type>();

    auto sourceType = op.source().getType().cast<MemRefType>();
    auto destType = op.dest().getType().cast<MemRefType>();

    // Debug switches.
    // true : use the slow but proven affine map.
    // false : use the faster index diff map.
    auto legacyLoadAttr = op->getAttr("legacy_load");
    auto legacyStoreAttr = op->getAttr("legacy_store");
    bool legacyLoad =
        (legacyLoadAttr &&
         legacyLoadAttr.template cast<BoolAttr>().getValue() == true);
    bool legacyStore =
        (legacyStoreAttr &&
         legacyStoreAttr.template cast<BoolAttr>().getValue() == true);

    Optional<AffineMap> composedSourceTransform;
    Optional<AffineMap> composedDestTransform;
    SmallVector<AffineMap> layeredSourceTransform;
    SmallVector<AffineMap> layeredDestTransform;
    DictionaryAttr srcTransformSpec;
    DictionaryAttr destTransformSpec;
    ArrayAttr boundCheckSourceAttr;
    ArrayAttr boundCheckDestAttr;

    auto coordTransformsAttr =
        op->getAttr("coord_transforms").template cast<ArrayAttr>();

    // Obtain coordinate lengths, as well as information of affine
    // transformations.
    unsigned sourceCoordLength = obtainGenericTensorTransformationInfo(
        /*operandIndex=*/0, sourceType, coordTransformsAttr,
        composedSourceTransform, layeredSourceTransform, srcTransformSpec,
        boundCheckSourceAttr);
    auto sourceCoord = op.sourceCoord();
    if (sourceCoordLength != sourceCoord.size()) {
      return op.emitOpError("Got " + Twine(sourceCoord.size()) +
                            " source coordinates but expected " +
                            Twine(sourceCoordLength));
    }

    unsigned destCoordLength = obtainGenericTensorTransformationInfo(
        /*operandIndex=*/1, destType, coordTransformsAttr,
        composedDestTransform, layeredDestTransform, destTransformSpec,
        boundCheckDestAttr);
    auto destCoord = op.destCoord();
    if (destCoordLength != destCoord.size()) {
      return op.emitOpError("Got " + Twine(destCoord.size()) +
                            " dest coordinates but expected " +
                            Twine(destCoordLength));
    }

    // FIXME. XXX.
    legacyLoad = overrideLoadStoreHack(srcTransformSpec, legacyLoad);
    legacyStore = overrideLoadStoreHack(destTransformSpec, legacyStore);

    // Determine if we need to emit codes for out-of-bound check, and which
    // dimensions need to dconduct such check.
    SmallVector<unsigned, 8> oobLoadCheckDims;
    bool toEmitOOBLoadCheckLogic = obtainOOBCheckInfo(
        composedSourceTransform, boundCheckSourceAttr, oobLoadCheckDims);
    SmallVector<unsigned, 8> oobStoreCheckDims;
    bool toEmitOOBStoreCheckLogic = obtainOOBCheckInfo(
        composedDestTransform, boundCheckDestAttr, oobStoreCheckDims);

    // Distinguish between generic <-> naive v naive <-> naive tensors.
    auto NSliceRowAttr = op->getAttr("n_slice_row");
    auto NSliceColAttr = op->getAttr("n_slice_col");
    auto DataPerAccessAttr = op->getAttr("data_per_access");
    if (NSliceRowAttr && NSliceColAttr && DataPerAccessAttr) {
      // In cases where attributes n_slice_row/n_slice_col/data_per_access are
      // specified, source and dest memrefs are all on LDS or VGPR, use the
      // simpler algorithm because they are all naive tensors.
      int64_t NSliceRow = NSliceRowAttr.template cast<IntegerAttr>().getInt();
      int64_t NSliceCol = NSliceColAttr.template cast<IntegerAttr>().getInt();
      int64_t DataPerAccess =
          DataPerAccessAttr.template cast<IntegerAttr>().getInt();

      emitNaiveTensorCopyLogic(b, loc, NSliceRow, NSliceCol, DataPerAccess,
                               sourceCoord, destCoord, composedSourceTransform,
                               composedDestTransform, sourceElementType,
                               destElementType, op.source(), op.dest());
    } else {
      // Otherwise, employ the more elaborated algorithm.

      // llvm::errs() << "\nthreadwise_copy op:\n";
      // op.dump();
      // llvm::errs() << "\n";

      auto dimAccessOrder =
          op->getAttr("dim_access_order").template cast<ArrayAttr>();

      Optional<ArrayAttr> boundAttr;
      if (op->getAttr("bound"))
        boundAttr = op->getAttr("bound").template cast<ArrayAttr>();

      // Figure out the bounds of load/store loops.
      SmallVector<uint64_t, 2> sliceLengths;

      computeSliceLengths(sliceLengths, composedSourceTransform,
                          composedDestTransform, coordTransformsAttr, boundAttr,
                          sourceType, destType);

      // llvm::errs() << "slice lengths: ";
      // for (unsigned i = 0; i < sliceLengths.size(); ++i)
      //   llvm::errs() << sliceLengths[i] << " ";
      // llvm::errs() << "\n";

      SmallVector<Value, 8> srcUpperIndices;
      SmallVector<Value, 8> srcLowerIndices;
      SmallVector<Value, 8> destUpperIndices;
      SmallVector<Value, 8> destLowerIndices;
      // Coordinates across the layers of transformations.
      // If the vector is of size n, 0 is the top layer, and
      // n-1 is the bottom layer.
      SmallVector<SmallVector<Value, 8>, 2> layeredSourceIndices;
      SmallVector<SmallVector<Value, 8>, 2> layeredDestIndices;

      ArrayAttr layeredSourceTransformMetadata;
      ArrayAttr layeredDestTransformMetadata;

      // Obtain transform metadata and populate coordinates for all layers
      // wthe the metadata.
      // Only do such computation in the new approach where index diff maps
      // would be used.
      if (legacyLoad == false) {
        // Populate coorindates across the layers of transformations.
        if (srcTransformSpec) {
          Attribute metadataAttr = srcTransformSpec.get("metadata");
          if (metadataAttr)
            layeredSourceTransformMetadata =
                metadataAttr.template cast<ArrayAttr>();
          else
            populateTransformMetadataFromLowerType(
                b, sourceType, layeredSourceTransformMetadata);
        }

        // Compute high-level coordinate for source memref.
        srcUpperIndices.append(sourceCoord.begin(), sourceCoord.end());

        // Populate coorindates across the layers of transformations.
        populateLayeredIndicesWithTransformMetadata(
            b, loc, layeredSourceIndices, srcUpperIndices,
            layeredSourceTransformMetadata);

        // Fetch low-level coordinate.
        srcLowerIndices = layeredSourceIndices[layeredSourceIndices.size() - 1];
      }

      // Obtain transform metadata and populate coordinates for all layers
      // wthe the metadata.
      // Only do such computation in the new approach where index diff maps
      // would be used.
      if (legacyStore == false) {
        // Populate coorindates across the layers of transformations.
        if (destTransformSpec) {
          Attribute metadataAttr = destTransformSpec.get("metadata");
          if (metadataAttr)
            layeredDestTransformMetadata =
                metadataAttr.template cast<ArrayAttr>();
          else
            populateTransformMetadataFromLowerType(
                b, destType, layeredDestTransformMetadata);
        }

        // Compute high-level coordinate for dest memref.
        destUpperIndices.append(destCoord.begin(), destCoord.end());

        // Populate coorindates across the layers of transformations.
        populateLayeredIndicesWithTransformMetadata(
            b, loc, layeredDestIndices, destUpperIndices,
            layeredDestTransformMetadata);

        // Fetch low-level coordinate.
        destLowerIndices = layeredDestIndices[layeredDestIndices.size() - 1];
      }

      // Emit fully unrolled loops for vector loads / stores.
      SmallVector<int64_t, 8> loopIVsPerAccessOrder;
      SmallVector<int64_t, 8> loopBoundsPerAccessOrder;
      for (unsigned iter = 0; iter < dimAccessOrder.size(); ++iter) {
        auto dim = dimAccessOrder[iter].template cast<IntegerAttr>().getInt();
        loopIVsPerAccessOrder.push_back(0);
        loopBoundsPerAccessOrder.push_back(sliceLengths[dim]);
      }

      int gemmNExtra = 0;
      int gemmMExtra = 0;
      int gemmKExtra = 0;
      // judge extra pad
      if (destTransformSpec) {
        Attribute metadataAttr = destTransformSpec.get("metadata");
        if (metadataAttr) {
          ArrayAttr layeredTransformMetadata =
              metadataAttr.template cast<ArrayAttr>();
          for (unsigned iter = 0; iter < layeredTransformMetadata.size();
               ++iter) {
            DictionaryAttr dictAttr =
                layeredTransformMetadata[iter].template cast<DictionaryAttr>();
            auto gemmKExtraAttr = dictAttr.get("gemmKExtra");
            auto gemmMExtraAttr = dictAttr.get("gemmMExtra");
            auto gemmNExtraAttr = dictAttr.get("gemmNExtra");
            if (gemmNExtraAttr) {
              gemmNExtra = gemmNExtraAttr.template cast<IntegerAttr>().getInt();
            }

            if (gemmMExtraAttr) {
              gemmMExtra = gemmMExtraAttr.template cast<IntegerAttr>().getInt();
            }
            if (gemmKExtraAttr) {
              gemmKExtra = gemmKExtraAttr.template cast<IntegerAttr>().getInt();
            }
          }
        }
      }
      BwdPaddingKernelStatus bwdPaddingStatus =
          BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;

      if ((gemmMExtra || gemmKExtra || gemmNExtra)) {

        auto PaddingKernelStatusAttr = op->getAttr("bwd_padding_kernel_status");
        if (PaddingKernelStatusAttr) {
          int64_t PaddingKernelStatus =
              PaddingKernelStatusAttr.template cast<IntegerAttr>().getInt();
          if (PaddingKernelStatus ==
              BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne)
            bwdPaddingStatus = BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
          else if (PaddingKernelStatus ==
                   BwdPaddingKernelStatus::StrideTwoNonXdlopsNHWC)
            bwdPaddingStatus = BwdPaddingKernelStatus::StrideTwoNonXdlopsNHWC;
          else if (PaddingKernelStatus ==
                   BwdPaddingKernelStatus::StrideTwoNonXdlopsNCHW)
            bwdPaddingStatus = BwdPaddingKernelStatus::StrideTwoNonXdlopsNCHW;
          else
            bwdPaddingStatus = BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
        }
      } else {
        bwdPaddingStatus = BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
      }

      // Main code emission loop.
      bool toExit = false;
      do {
        // Use the old logic in case "legacy_load" attribute is specified.
        if (legacyLoad == true) {
          computeTopAndBottomIndicesWithAffineMap(
              b, loc, srcUpperIndices, srcLowerIndices, sourceCoord,
              loopIVsPerAccessOrder, dimAccessOrder, layeredSourceTransform);
        } else {
          // New approach. Use index diff map.
          // Progressively use index diff map to compute the coordinate at the
          // bottom most layer.
          computeBottomIndicesWithIndexDiffMap(
              b, loc, loopIVsPerAccessOrder, layeredSourceTransformMetadata,
              layeredSourceTransform, layeredSourceIndices, srcLowerIndices);
        }

        // Load from source.
        Value scalarValue = emitLoadLogic(
            b, loc, sourceType, sourceElementType, toEmitOOBLoadCheckLogic,
            oobLoadCheckDims, op.source(), srcLowerIndices);

        // Convert from sourceElementType to destElementType if necessary.
        Value convertedScalarValue = createTypeConversionOp(
            b, loc, scalarValue, sourceElementType, destElementType);

        // Use the old logic in case "legacy_store" attribute is specified.
        if (legacyStore == true) {
          computeTopAndBottomIndicesWithAffineMap(
              b, loc, destUpperIndices, destLowerIndices, destCoord,
              loopIVsPerAccessOrder, dimAccessOrder, layeredDestTransform);
        } else {
          // New approach. Use index diff map.
          // Progressively use index diff map to compute the coordinate at the
          // bottom most layer.
          computeBottomIndicesWithIndexDiffMap(
              b, loc, loopIVsPerAccessOrder, layeredDestTransformMetadata,
              layeredDestTransform, layeredDestIndices, destLowerIndices);
        }

        // Store to dest.
        emitStoreLogic(bwdPaddingStatus, b, loc, destType, destElementType,
                       toEmitOOBStoreCheckLogic, oobStoreCheckDims, op.dest(),
                       destLowerIndices, convertedScalarValue);

        // increase IVs
        bool toIncreaseNextDigit = true;
        int iter = loopIVsPerAccessOrder.size() - 1;
        for (; toIncreaseNextDigit && iter >= 0; --iter) {
          if (++loopIVsPerAccessOrder[iter] == loopBoundsPerAccessOrder[iter]) {
            loopIVsPerAccessOrder[iter] = 0;
            toIncreaseNextDigit = true;
          } else {
            toIncreaseNextDigit = false;
          }
        }

        // check if need to exit
        if (iter < 0 && toIncreaseNextDigit == true) {
          toExit = true;
        }
      } while (!toExit);
    }

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// ThreadwiseLoad lowering.
//===----------------------------------------------------------------------===//

struct ThreadwiseLoadRewritePattern
    : public OpRewritePattern<miopen::ThreadwiseLoadOp> {
  using OpRewritePattern<miopen::ThreadwiseLoadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::ThreadwiseLoadOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    // The types in elements of the input are all the same.
    auto destElementType = op.getResult(0).getType().cast<Type>();

    auto sourceType = op.source().getType().cast<MemRefType>();
    auto destTypes = op.getResultTypes();

    // Debug switches.
    // true : use the slow but proven affine map.
    // false : use the faster index diff map.
    auto legacyLoadAttr = op->getAttr("legacy_load");
    bool legacyLoad =
        (legacyLoadAttr &&
         legacyLoadAttr.template cast<BoolAttr>().getValue() == true);

    Optional<AffineMap> composedSourceTransform;
    SmallVector<AffineMap> layeredSourceTransform;
    DictionaryAttr srcTransformSpec;
    ArrayAttr boundCheckSourceAttr;

    auto coordTransformsAttr =
        op->getAttr("coord_transforms").template cast<ArrayAttr>();

    // Obtain coordinate lengths, as well as information of affine
    // transformations.
    unsigned sourceCoordLength = obtainGenericTensorTransformationInfo(
        /*operandIndex=*/0, sourceType, coordTransformsAttr,
        composedSourceTransform, layeredSourceTransform, srcTransformSpec,
        boundCheckSourceAttr);

    auto sourceCoord = op.sourceCoord();
    if (sourceCoordLength != sourceCoord.size()) {
      llvm::errs() << "INCORRECT source coordinates assigned!";
      return failure();
    }

    // FIXME. XXX.
    // Workaround to obtain gemmKExtra attribute.
    // And use it to override legacy load/store debug switch.
    legacyLoad = overrideLoadStoreHack(srcTransformSpec, legacyLoad);

    // Determine if we need to emit codes for out-of-bound check, and which
    // dimensions need to dconduct such check.
    SmallVector<unsigned, 8> oobLoadCheckDims;
    bool toEmitOOBLoadCheckLogic = obtainOOBCheckInfo(
        composedSourceTransform, boundCheckSourceAttr, oobLoadCheckDims);

    // llvm::errs() << "\nthreadwise_load op:\n";
    // op.dump();
    // llvm::errs() << "\n";

    // --------------------------------

    auto dimAccessOrder =
        op->getAttr("dim_access_order").template cast<ArrayAttr>();

    auto srcDataPerRead = op->getAttr("source_data_per_read")
                              .template cast<IntegerAttr>()
                              .getInt();

    auto vectorReadWriteDim = op->getAttr("vector_read_write_dim")
                                  .template cast<IntegerAttr>()
                                  .getInt();

    Optional<ArrayAttr> boundAttr;
    if (op->getAttr("bound"))
      boundAttr = op->getAttr("bound").template cast<ArrayAttr>();

    // Figure out the bounds of load/store loops.
    SmallVector<uint64_t, 2> sliceLengths;

    computeSliceLengths(sliceLengths, composedSourceTransform,
                        /*composedDestTransform=*/Optional<AffineMap>{},
                        coordTransformsAttr, boundAttr, sourceType, nullptr);

    // llvm::errs() << "slice lengths: ";
    // for (unsigned i = 0; i < sliceLengths.size(); ++i)
    //   llvm::errs() << sliceLengths[i] << " ";
    // llvm::errs() << "\n";

    // llvm::errs() << "vector dim: " << vectorReadWriteDim << "\n";
    // llvm::errs() << "source data per read: " << srcDataPerRead << "\n";

    sliceLengths[vectorReadWriteDim] /= srcDataPerRead;
    assert(sliceLengths[vectorReadWriteDim] != 0);

    // llvm::errs() << "modified lengths: ";
    // for (unsigned i = 0; i < sliceLengths.size(); ++i)
    //   llvm::errs() << sliceLengths[i] << " ";
    // llvm::errs() << "\n";

    // --------------------------------

    SmallVector<Value, 8> srcUpperIndices;
    SmallVector<Value, 8> srcLowerIndices;
    // Coordinates across the layers of transformations.
    // If the vector is of size n, 0 is the top layer, and
    // n-1 is the bottom layer.
    SmallVector<SmallVector<Value, 8>, 2> layeredSourceIndices;

    ArrayAttr layeredSourceTransformMetadata;

    // Obtain transform metadata and populate coordinates for all layers
    // wthe the metadata.
    // Only do such computation in the new approach where index diff maps
    // would be used.
    if (legacyLoad == false) {
      // Populate coorindates across the layers of transformations.
      if (srcTransformSpec) {
        Attribute metadataAttr = srcTransformSpec.get("metadata");
        if (metadataAttr)
          layeredSourceTransformMetadata =
              metadataAttr.template cast<ArrayAttr>();
        else
          populateTransformMetadataFromLowerType(
              b, sourceType, layeredSourceTransformMetadata);
      }

      // Compute high-level coordinate for source memref.
      srcUpperIndices.append(sourceCoord.begin(), sourceCoord.end());

      // Populate coorindates across the layers of transformations.
      populateLayeredIndicesWithTransformMetadata(
          b, loc, layeredSourceIndices, srcUpperIndices,
          layeredSourceTransformMetadata);

      // Fetch low-level coordinate.
      srcLowerIndices = layeredSourceIndices[layeredSourceIndices.size() - 1];
    }

    // --------------------------------

    // Emit fully unrolled loops for vector loads / stores.
    SmallVector<int64_t, 8> loopIVsPerAccessOrder;
    SmallVector<int64_t, 8> loopBoundsPerAccessOrder;
    for (unsigned iter = 0; iter < dimAccessOrder.size(); ++iter) {
      auto dim = dimAccessOrder[iter].template cast<IntegerAttr>().getInt();
      loopIVsPerAccessOrder.push_back(0);
      loopBoundsPerAccessOrder.push_back(sliceLengths[dim]);
    }

    // --------------------------------

    // Main code emission loop.
    DenseMap<int64_t, Value> loadedValues;
    bool toExit = false;
    do {
      // llvm::errs() << "IVs: ";
      // for (auto v : loopIVsPerAccessOrder)
      //   llvm::errs() << v << " ";
      // llvm::errs() << "\n";

      // Use the old logic in case "legacy_load" attribute is specified.
      if (legacyLoad == true) {
        computeTopAndBottomIndicesWithAffineMap(
            b, loc, srcUpperIndices, srcLowerIndices, sourceCoord,
            loopIVsPerAccessOrder, dimAccessOrder, layeredSourceTransform);
      } else {
        // New approach. Use index diff map.
        // Progressively use index diff map to compute the coordinate at the
        // bottom most layer.
        computeBottomIndicesWithIndexDiffMap(
            b, loc, loopIVsPerAccessOrder, layeredSourceTransformMetadata,
            layeredSourceTransform, layeredSourceIndices, srcLowerIndices);
      }

      // Determine the type to be loaded.
      // Construct a vector in case we can do vector load.
      Type loadedType = destElementType;
      if (srcDataPerRead > 1)
        loadedType = VectorType::get({srcDataPerRead}, destElementType);

      // Load from source.
      Value loadedValue =
          emitLoadLogic(b, loc, sourceType, loadedType, toEmitOOBLoadCheckLogic,
                        oobLoadCheckDims, op.source(), srcLowerIndices);

      // Compute the final index on the loadedValues, following IVs.
      int64_t inputsIndex = 0;
      int64_t stride = 1;
      int64_t vectorDimStride = 0;
      for (int64_t iter = loopIVsPerAccessOrder.size() - 1; iter >= 0; --iter) {
        inputsIndex += loopIVsPerAccessOrder[iter] * stride;
        if (iter == vectorReadWriteDim) {
          vectorDimStride = stride;
          stride *= (loopBoundsPerAccessOrder[iter] * srcDataPerRead);
        } else {
          stride *= loopBoundsPerAccessOrder[iter];
        }
      }
      // llvm::errs() << "inputsIndex: " << inputsIndex << "\n";

      // In case we do vector load, decompose the elements as the
      // results of threadwise_load only hold scalars.
      if (srcDataPerRead > 1) {
        assert(loadedValue.getType().isa<VectorType>());

        for (int64_t iter = 0; iter < srcDataPerRead; ++iter) {
          auto loadedElement = b.create<vector::ExtractElementOp>(
              loc, destElementType, loadedValue,
              b.create<ConstantIndexOp>(loc, iter));
          int64_t decomposedInputsIndex = inputsIndex + iter * vectorDimStride;
          // llvm::errs() << "decomposedInputsIndex: " << decomposedInputsIndex
          //              << "\n";

          loadedValues[decomposedInputsIndex] = loadedElement;
        }
      } else {
        loadedValues[inputsIndex] = loadedValue;
      }

      // increase IVs
      bool toIncreaseNextDigit = true;
      int iter = loopIVsPerAccessOrder.size() - 1;
      for (; toIncreaseNextDigit && iter >= 0; --iter) {
        loopIVsPerAccessOrder[iter] += 1;
        if (loopIVsPerAccessOrder[iter] >= loopBoundsPerAccessOrder[iter]) {
          loopIVsPerAccessOrder[iter] = 0;
          toIncreaseNextDigit = true;
        } else {
          toIncreaseNextDigit = false;
        }
      }

      // check if need to exit
      if (iter < 0 && toIncreaseNextDigit == true) {
        toExit = true;
      }
    } while (!toExit);

    // --------------------------------

    // Extract the loaded values into a variadic results.
    assert(loadedValues.size() == destTypes.size());
    SmallVector<Value, 8> outputValues;
    for (int64_t iter = 0; iter < loadedValues.size(); ++iter) {
      assert(loadedValues.count(iter) == 1);
      outputValues.push_back(loadedValues[iter]);
    }
    op.replaceAllUsesWith(outputValues);
    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// ThreadwiseStore lowering.
//===----------------------------------------------------------------------===//

struct ThreadwiseStoreRewritePattern
    : public OpRewritePattern<miopen::ThreadwiseStoreOp> {
  using OpRewritePattern<miopen::ThreadwiseStoreOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::ThreadwiseStoreOp op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    auto sourceTypes = op.data().getTypes();
    // The types in the data arguments are all the same.
    auto sourceElementType = sourceTypes[0];

    auto destType = op.dest().getType().cast<MemRefType>();

    // Debug switches.
    // true : use the slow but proven affine map.
    // false : use the faster index diff map.
    auto legacyStoreAttr = op->getAttr("legacy_store");
    bool legacyStore =
        (legacyStoreAttr &&
         legacyStoreAttr.template cast<BoolAttr>().getValue() == true);

    Optional<AffineMap> composedDestTransform;
    SmallVector<AffineMap> layeredDestTransform;
    DictionaryAttr destTransformSpec;
    ArrayAttr boundCheckDestAttr;

    auto coordTransformsAttr =
        op->getAttr("coord_transforms").template cast<ArrayAttr>();

    // Obtain coordinate lengths, as well as information of affine
    // transformations.
    unsigned destCoordLength = obtainGenericTensorTransformationInfo(
        /*operandIndex=*/sourceTypes.size(), destType, coordTransformsAttr,
        composedDestTransform, layeredDestTransform, destTransformSpec,
        boundCheckDestAttr);

    auto destCoord = op.destCoord();
    if (destCoordLength != destCoord.size()) {
      llvm::errs() << "INCORRECT dest coordinates assigned!\n";
      return failure();
    }

    // FIXME. XXX.
    // Workaround to obtain gemmKExtra attribute.
    // And use it to override legacy load/store debug switch.
    legacyStore = overrideLoadStoreHack(destTransformSpec, legacyStore);

    // Determine if we need to emit codes for out-of-bound check, and which
    // dimensions need to dconduct such check.
    SmallVector<unsigned, 8> oobStoreCheckDims;
    bool toEmitOOBStoreCheckLogic = obtainOOBCheckInfo(
        composedDestTransform, boundCheckDestAttr, oobStoreCheckDims);

    // llvm::errs() << "\nthreadwise_store op:\n";
    // op.dump();
    // llvm::errs() << "\n";

    // --------------------------------

    auto dimAccessOrder =
        op->getAttr("dim_access_order").template cast<ArrayAttr>();

    auto dstDataPerWrite = op->getAttr("dest_data_per_write")
                               .template cast<IntegerAttr>()
                               .getInt();

    auto vectorReadWriteDim = op->getAttr("vector_read_write_dim")
                                  .template cast<IntegerAttr>()
                                  .getInt();

    Optional<ArrayAttr> boundAttr;
    if (op->getAttr("bound"))
      boundAttr = op->getAttr("bound").template cast<ArrayAttr>();

    // Figure out the bounds of load/store loops.
    SmallVector<uint64_t, 2> sliceLengths;

    computeSliceLengths(sliceLengths,
                        /*composedSourceTransform=*/Optional<AffineMap>{},
                        composedDestTransform, coordTransformsAttr, boundAttr,
                        nullptr, destType);

    // llvm::errs() << "slice lengths: ";
    // for (unsigned i = 0; i < sliceLengths.size(); ++i)
    //   llvm::errs() << sliceLengths[i] << " ";
    // llvm::errs() << "\n";

    // llvm::errs() << "vector dim: " << vectorReadWriteDim << "\n";
    // llvm::errs() << "dest data per write: " << dstDataPerWrite << "\n";

    sliceLengths[vectorReadWriteDim] /= dstDataPerWrite;
    assert(sliceLengths[vectorReadWriteDim] != 0);

    // llvm::errs() << "modified lengths: ";
    // for (unsigned i = 0; i < sliceLengths.size(); ++i)
    //   llvm::errs() << sliceLengths[i] << " ";
    // llvm::errs() << "\n";

    // --------------------------------

    SmallVector<Value, 8> destUpperIndices;
    SmallVector<Value, 8> destLowerIndices;
    // Coordinates across the layers of transformations.
    // If the vector is of size n, 0 is the top layer, and
    // n-1 is the bottom layer.
    SmallVector<SmallVector<Value, 8>, 2> layeredDestIndices;

    ArrayAttr layeredDestTransformMetadata;

    // Obtain transform metadata and populate coordinates for all layers
    // wthe the metadata.
    // Only do such computation in the new approach where index diff maps
    // would be used.
    if (legacyStore == false) {
      // Populate coorindates across the layers of transformations.
      if (destTransformSpec) {
        Attribute metadataAttr = destTransformSpec.get("metadata");
        if (metadataAttr)
          layeredDestTransformMetadata =
              metadataAttr.template cast<ArrayAttr>();
        else
          populateTransformMetadataFromLowerType(b, destType,
                                                 layeredDestTransformMetadata);
      }

      // Compute high-level coordinate for dest memref.
      destUpperIndices.append(destCoord.begin(), destCoord.end());

      // Populate coorindates across the layers of transformations.
      populateLayeredIndicesWithTransformMetadata(b, loc, layeredDestIndices,
                                                  destUpperIndices,
                                                  layeredDestTransformMetadata);

      // Fetch low-level coordinate.
      destLowerIndices = layeredDestIndices[layeredDestIndices.size() - 1];
    }

    // --------------------------------

    // Emit fully unrolled loops for vector loads / stores.
    SmallVector<int64_t, 8> loopIVsPerAccessOrder;
    SmallVector<int64_t, 8> loopBoundsPerAccessOrder;
    for (unsigned iter = 0; iter < dimAccessOrder.size(); ++iter) {
      auto dim = dimAccessOrder[iter].template cast<IntegerAttr>().getInt();
      loopIVsPerAccessOrder.push_back(0);
      loopBoundsPerAccessOrder.push_back(sliceLengths[dim]);
    }

    // --------------------------------

    // Main code emission loop.
    bool toExit = false;
    do {
      // llvm::errs() << "IVs: ";
      // for (auto v : loopIVsPerAccessOrder)
      //   llvm::errs() << v << " ";
      // llvm::errs() << "\n";

      // Use the old logic in case "legacy_store" attribute is specified.
      if (legacyStore == true) {
        computeTopAndBottomIndicesWithAffineMap(
            b, loc, destUpperIndices, destLowerIndices, destCoord,
            loopIVsPerAccessOrder, dimAccessOrder, layeredDestTransform);
      } else {
        // New approach. Use index diff map.
        // Progressively use index diff map to compute the coordinate at the
        // bottom most layer.
        computeBottomIndicesWithIndexDiffMap(
            b, loc, loopIVsPerAccessOrder, layeredDestTransformMetadata,
            layeredDestTransform, layeredDestIndices, destLowerIndices);
      }

      // Determine the type to be stored.
      // Construct a vector in case we can do vector store.
      Type typeToStore = sourceElementType;
      if (dstDataPerWrite > 1)
        typeToStore = VectorType::get({dstDataPerWrite}, sourceElementType);

      // Compute the starting index inside the inputs, following IVs.
      int64_t inputsIndex = 0;
      int64_t stride = 1;
      int64_t vectorDimStride = 0;
      for (int iter = loopIVsPerAccessOrder.size() - 1; iter >= 0; --iter) {
        inputsIndex += loopIVsPerAccessOrder[iter] * stride;
        if (iter == vectorReadWriteDim) {
          vectorDimStride = stride;
          stride *= (loopBoundsPerAccessOrder[iter] * dstDataPerWrite);
        } else {
          stride *= loopBoundsPerAccessOrder[iter];
        }
      }
      // llvm::errs() << "inputsIndex: " << inputsIndex << "\n";

      // In case we do vector store, decompose the elements as the arguments
      // only hold scalars.
      Value valueToStore;
      if (dstDataPerWrite > 1) {
        assert(typeToStore.isa<VectorType>());

        valueToStore = createZeroConstantFloatOp(b, loc, typeToStore);
        for (int64_t iter = 0; iter < dstDataPerWrite; ++iter) {
          int64_t decomposedInputsIndex = inputsIndex + iter * vectorDimStride;
          // llvm::errs() << "decomposedInputsIndex: " << decomposedInputsIndex
          // << "\n";
          Value element = op.data()[decomposedInputsIndex];
          valueToStore = b.create<vector::InsertElementOp>(
              loc, typeToStore, element, valueToStore,
              b.create<ConstantIndexOp>(loc, iter));
        }
      } else {
        valueToStore = op.data()[inputsIndex];
      }

      // Store to dest. this part do not need backward data padding kernel
      // workaround just use NotBwdPaddingOrStrideOne
      BwdPaddingKernelStatus bwdPaddingStatus =
          BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
      emitStoreLogic(bwdPaddingStatus, b, loc, destType, typeToStore,
                     toEmitOOBStoreCheckLogic, oobStoreCheckDims, op.dest(),
                     destLowerIndices, valueToStore);

      // increase IVs
      bool toIncreaseNextDigit = true;
      int iter = loopIVsPerAccessOrder.size() - 1;
      for (; toIncreaseNextDigit && iter >= 0; --iter) {
        loopIVsPerAccessOrder[iter] += 1;
        if (loopIVsPerAccessOrder[iter] >= loopBoundsPerAccessOrder[iter]) {
          loopIVsPerAccessOrder[iter] %= loopBoundsPerAccessOrder[iter];
          toIncreaseNextDigit = true;
        } else {
          toIncreaseNextDigit = false;
        }
      }

      // check if need to exit
      if (iter < 0 && toIncreaseNextDigit == true) {
        toExit = true;
      }
    } while (!toExit);

    // --------------------------------

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// ThreadwiseCopyV2 lowering.
//===----------------------------------------------------------------------===//

struct ThreadwiseCopyV2RewritePattern
    : public OpRewritePattern<miopen::ThreadwiseCopyV2Op> {
  using OpRewritePattern<miopen::ThreadwiseCopyV2Op>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::ThreadwiseCopyV2Op op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    auto sourceElementType =
        op.source().getType().cast<VectorType>().getElementType().cast<Type>();
    auto destElementType =
        op.dest().getType().cast<MemRefType>().getElementType().cast<Type>();

    auto sourceType = op.source().getType().cast<VectorType>();
    auto destType = op.dest().getType().cast<MemRefType>();
    InMemoryDataOperation dataOpration = InMemoryDataOperation::Set;
    auto dataOpAttr = op->getAttr("data_operation");
    if (dataOpAttr) {
      int64_t dataOp = dataOpAttr.template cast<IntegerAttr>().getInt();
      if (dataOp == 1)
        dataOpration = InMemoryDataOperation::AtomicAdd;
    }

    Value sourceOffsetOp =
        b.create<ConstantIndexOp>(loc, op.sourceOffset().getZExtValue());
    // Get source offset, and dest coordinates.
    //
    // 1. For memrefs with no externally defined affine maps in coord_transforms
    //    attribute, or embedded affine maps. Use its rank.
    // 2. For memrefs with externally defined maps, use its input rank.
    // 3. For memrefs with embedded maps, use its input rank.
    Optional<AffineMap> composedSourceTransform;
    Optional<AffineMap> composedDestTransform;
    SmallVector<AffineMap> layeredSourceTransform;
    SmallVector<AffineMap> layeredDestTransform;
    DictionaryAttr srcTransformSpec;
    DictionaryAttr destTransformSpec;
    ArrayAttr boundCheckSourceAttr;
    ArrayAttr boundCheckDestAttr;

    int64_t dataPerCopy =
        op->getAttrOfType<IntegerAttr>("dest_data_per_write").getInt();
    int64_t sourceDataPerRead =
        op->getAttrOfType<IntegerAttr>("source_data_per_read").getInt();
    assert(dataPerCopy == sourceDataPerRead &&
           "source and dest vector copy lengths are equal");

    int64_t upperVectorDim =
        op->getAttrOfType<IntegerAttr>("upper_vector_read_dim").getInt();
    int64_t lowerVectorDim =
        op->getAttrOfType<IntegerAttr>("vector_read_write_dim").getInt();
    assert((dataPerCopy == 1 || lowerVectorDim == 4) &&
           "Was asked to vectorize non-final dimension");

    auto coordTransformsAttr =
        op->getAttr("coord_transforms").template cast<ArrayAttr>();

    // Obtain coordinate lengths, as well as information of affine
    // transformations.
    unsigned sourceCoordLength = obtainGenericTensorTransformationInfo(
        /*operandIndex=*/0, sourceType, coordTransformsAttr,
        composedSourceTransform, layeredSourceTransform, srcTransformSpec,
        boundCheckSourceAttr);

    auto sourceCoord = op.sourceCoord();
    if (sourceCoordLength != sourceCoord.size()) {
      return op.emitOpError("Got " + Twine(sourceCoord.size()) +
                            " source coordinates but expected " +
                            Twine(sourceCoordLength));
    }

    unsigned destCoordLength = obtainGenericTensorTransformationInfo(
        /*operandIndex=*/1, destType, coordTransformsAttr,
        composedDestTransform, layeredDestTransform, destTransformSpec,
        boundCheckDestAttr);
    auto destCoord = op.destCoord();
    if (destCoordLength != destCoord.size()) {
      return op.emitOpError("Got " + Twine(destCoord.size()) +
                            " dest coordinates but expected " +
                            Twine(destCoordLength));
    }

    // Determine if we need to emit codes for out-of-bound check, and which
    // dimensions need to dconduct such check.
    SmallVector<unsigned, 8> oobStoreCheckDims;
    bool toEmitOOBStoreCheckLogic = obtainOOBCheckInfo(
        composedDestTransform, boundCheckDestAttr, oobStoreCheckDims);
    if (toEmitOOBStoreCheckLogic) {
      // TODO(kdrewnia) Work out if we can have stores that statically
      // won't OOB still be vectorized
      dataPerCopy = 1;
    }

    // llvm::errs() << "\nthreadwise_copy_v2 op:\n";
    // op.dump();
    // llvm::errs() << "\n";

    auto dimAccessOrder =
        op->getAttr("dim_access_order").template cast<ArrayAttr>();

    Optional<ArrayAttr> boundAttr;
    if (op->getAttr("bound"))
      boundAttr = op->getAttr("bound").template cast<ArrayAttr>();

    // Figure out the bounds of load/store loops.
    SmallVector<uint64_t, 2> sliceLengths;

    computeSliceLengths(sliceLengths, composedSourceTransform,
                        composedDestTransform, coordTransformsAttr, boundAttr,
                        sourceType, destType);
    if (upperVectorDim >= 0) {
      sliceLengths[upperVectorDim] /= dataPerCopy;
      assert(sliceLengths[upperVectorDim] != 0);
    }

    // llvm::errs() << "slice lengths: ";
    // for (unsigned i = 0; i < sliceLengths.size(); ++i)
    //   llvm::errs() << sliceLengths[i] << " ";
    // llvm::errs() << "\n";
    // Compute high-level coordinate for source memref.
    SmallVector<Value, 8> srcUpperIndices;
    srcUpperIndices.append(sourceCoord.begin(), sourceCoord.end());

    // Coordinates across the layers of transformations.
    // If the vector is of size n, 0 is the top layer, and
    // n-1 is the bottom layer.
    SmallVector<SmallVector<Value, 8>, 2> layeredSourceIndices;

    // Populate coorindates across the layers of transformations.
    ArrayAttr layeredSourceTransformMetadata =
        srcTransformSpec.get("metadata").template cast<ArrayAttr>();
    // Populate coorindates across the layers of transformations.
    populateLayeredIndicesWithTransformMetadata(b, loc, layeredSourceIndices,
                                                srcUpperIndices,
                                                layeredSourceTransformMetadata);

    // Fetch low-level coordinate.
    SmallVector<Value, 8> srcLowerIndices =
        layeredSourceIndices[layeredSourceIndices.size() - 1];

    // Compute high-level coordinate for dest memref.
    SmallVector<Value, 8> destUpperIndices;
    destUpperIndices.append(destCoord.begin(), destCoord.end());

    // Coordinates across the layers of transformations.
    // If the vector is of size n, 0 is the top layer, and
    // n-1 is the bottom layer.
    SmallVector<SmallVector<Value, 8>, 2> layeredDestIndices;

    // Populate coorindates across the layers of transformations.
    ArrayAttr layeredDestTransformMetadata =
        destTransformSpec.get("metadata").template cast<ArrayAttr>();
    // Populate coorindates across the layers of transformations.
    populateLayeredIndicesWithTransformMetadata(b, loc, layeredDestIndices,
                                                destUpperIndices,
                                                layeredDestTransformMetadata);

    // Fetch low-level coordinate.
    SmallVector<Value, 8> destLowerIndices =
        layeredDestIndices[layeredDestIndices.size() - 1];

    // Emit fully unrolled loops for vector loads / stores.
    SmallVector<int64_t, 8> loopIVsPerAccessOrder;
    SmallVector<int64_t, 8> loopBoundsPerAccessOrder;
    for (unsigned iter = 0; iter < dimAccessOrder.size(); ++iter) {
      auto dim = dimAccessOrder[iter].template cast<IntegerAttr>().getInt();
      loopIVsPerAccessOrder.push_back(0);
      loopBoundsPerAccessOrder.push_back(sliceLengths[dim]);
    }

    int gemmNExtra = 0;
    int gemmMExtra = 0;
    int gemmKExtra = 0;
    // judge extra pad
    if (destTransformSpec) {
      Attribute metadataAttr = destTransformSpec.get("metadata");
      if (metadataAttr) {
        ArrayAttr layeredTransformMetadata =
            metadataAttr.template cast<ArrayAttr>();
        for (unsigned iter = 0; iter < layeredTransformMetadata.size();
             ++iter) {
          DictionaryAttr dictAttr =
              layeredTransformMetadata[iter].template cast<DictionaryAttr>();
          auto gemmKExtraAttr = dictAttr.get("gemmKExtra");
          auto gemmMExtraAttr = dictAttr.get("gemmMExtra");
          auto gemmNExtraAttr = dictAttr.get("gemmNExtra");
          if (gemmNExtraAttr) {
            gemmNExtra = gemmNExtraAttr.template cast<IntegerAttr>().getInt();
          }

          if (gemmMExtraAttr) {
            gemmMExtra = gemmMExtraAttr.template cast<IntegerAttr>().getInt();
          }
          if (gemmKExtraAttr) {
            gemmKExtra = gemmKExtraAttr.template cast<IntegerAttr>().getInt();
          }
        }
      }
    }

    BwdPaddingKernelStatus bwdPaddingStatus =
        BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
    if ((gemmMExtra || gemmKExtra || gemmNExtra)) {
      auto PaddingKernelStatusAttr = op->getAttr("bwd_padding_kernel_status");
      if (PaddingKernelStatusAttr) {
        int64_t PaddingKernelStatus =
            PaddingKernelStatusAttr.template cast<IntegerAttr>().getInt();
        if (PaddingKernelStatus ==
            BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne)
          bwdPaddingStatus = BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
        else if (PaddingKernelStatus ==
                 BwdPaddingKernelStatus::StrideTwoXdlopsNHWC) {
          // gemmMExtra>0 can't get correct results now
          if (gemmMExtra == 0 && gemmNExtra > 0)
            bwdPaddingStatus =
                BwdPaddingKernelStatus::GemmNPadStrideTwoXdlopsNHWC;
          else if (gemmMExtra > 0 && gemmNExtra > 0)
            bwdPaddingStatus =
                BwdPaddingKernelStatus::GemmMNPadStrideTwoXdlopsNHWC;
          else if (gemmMExtra > 0 && gemmNExtra == 0)
            bwdPaddingStatus =
                BwdPaddingKernelStatus::GemmMPadStrideTwoXdlopsNHWC;
        } else if (PaddingKernelStatus ==
                   BwdPaddingKernelStatus::StrideTwoXdlopsNCHW) {
          if (gemmMExtra > 0 && gemmNExtra > 0)
            bwdPaddingStatus =
                BwdPaddingKernelStatus::GemmMNPadStrideTwoXdlopsNCHW;
          if (gemmMExtra > 0)
            bwdPaddingStatus =
                BwdPaddingKernelStatus::GemmMPadStrideTwoXdlopsNCHW;
          else if (gemmNExtra > 0)
            bwdPaddingStatus =
                BwdPaddingKernelStatus::GemmNPadStrideTwoXdlopsNCHW;
        } else
          bwdPaddingStatus = BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
      }
    } else {
      bwdPaddingStatus = BwdPaddingKernelStatus::NotBwdPaddingOrStrideOne;
    }

    Type typeToLoad = sourceElementType;
    if (dataPerCopy > 1) {
      typeToLoad = VectorType::get({dataPerCopy}, typeToLoad);
    }
    Type typeToStore = destElementType;
    if (dataPerCopy > 1) {
      typeToStore = VectorType::get({dataPerCopy}, typeToStore);
    }

    bool toExit = false;
    do {
      // Load from source vector.

      // Progressively use index diff map to compute the coordinate at the
      // bottom most layer.
      computeBottomIndicesWithIndexDiffMap(
          b, loc, loopIVsPerAccessOrder, layeredSourceTransformMetadata,
          layeredSourceTransform, layeredSourceIndices, srcLowerIndices);

      // Add sourceOffset to derive the position in the vector.
      auto srcPosition =
          b.create<AddIOp>(loc, srcLowerIndices[0], sourceOffsetOp);

      // Load from source.
      Value loadedValue;
      if (dataPerCopy > 1) {
        loadedValue = createZeroConstantFloatOp(b, loc, typeToLoad);
        for (int64_t i = 0; i < dataPerCopy; ++i) {
          Value index = b.create<ConstantIndexOp>(loc, i);
          Value extracted = b.create<vector::ExtractElementOp>(
              loc, sourceElementType, op.source(),
              b.create<AddIOp>(loc, srcPosition, index));
          loadedValue = b.create<vector::InsertElementOp>(loc, extracted,
                                                          loadedValue, index);
        }
      } else {
        loadedValue = b.create<vector::ExtractElementOp>(
            loc, sourceElementType, op.source(), srcPosition);
      }

      // Convert from sourceElementType to destElementType if necessary.
      Value convertedValue =
          createTypeConversionOp(b, loc, loadedValue, typeToLoad, typeToStore);

      // Store to dest memref.

      // Progressively use index diff map to compute the coordinate at the
      // bottom most layer.
      computeBottomIndicesWithIndexDiffMap(
          b, loc, loopIVsPerAccessOrder, layeredDestTransformMetadata,
          layeredDestTransform, layeredDestIndices, destLowerIndices);

      // Store to dest.
      emitStoreLogic(bwdPaddingStatus, b, loc, destType, typeToStore,
                     toEmitOOBStoreCheckLogic, oobStoreCheckDims, op.dest(),
                     destLowerIndices, convertedValue, dataOpration);
      // increase IVs
      bool toIncreaseNextDigit = true;
      int iter = loopIVsPerAccessOrder.size() - 1;
      for (; toIncreaseNextDigit && iter >= 0; --iter) {
        if (++loopIVsPerAccessOrder[iter] == loopBoundsPerAccessOrder[iter]) {
          loopIVsPerAccessOrder[iter] = 0;
          toIncreaseNextDigit = true;
        } else {
          toIncreaseNextDigit = false;
        }
      }

      // check if need to exit
      if (iter < 0 && toIncreaseNextDigit == true) {
        toExit = true;
      }
    } while (!toExit);

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Subview lowering.
//===----------------------------------------------------------------------===//

struct SubviewRewritePattern : public OpRewritePattern<miopen::SubviewOp> {
  using OpRewritePattern<miopen::SubviewOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::SubviewOp op,
                                PatternRewriter &b) const override {
    auto inputType = op.input().getType().cast<MemRefType>();
    auto outputType = op.output().getType().cast<MemRefType>();

    // Pass the output affine map to users of this op.
    for (auto user : op.output().getUsers()) {
      unsigned userOperandIndex = 0;
      for (userOperandIndex = 0; userOperandIndex < user->getNumOperands(); ++userOperandIndex)
        if (user->getOperand(userOperandIndex) == op.output())
          break;

      auto coordTransformAttrs = user->getAttr("coord_transforms");
      if (!coordTransformAttrs) {
        SmallVector<Attribute, 4> upperLayerShape;
        SmallVector<Attribute, 4> upperLayerDims;
        SmallVector<Attribute, 4> upperLayerStrides;
        SmallVector<Attribute, 4> lowerLayerShape;
        SmallVector<Attribute, 4> lowerLayerDims;

        // Compute upper layer dimensions and bounds.
        for (unsigned iter = 0; iter < outputType.getShape().size(); ++iter) {
          upperLayerShape.push_back(
              b.getI32IntegerAttr(outputType.getShape()[iter]));
          upperLayerDims.push_back(b.getI32IntegerAttr(iter));
        }
        // Compute upper layer strides.
        int64_t stride = 1;
        upperLayerStrides.push_back(b.getI32IntegerAttr(stride));
        for (int64_t iter = outputType.getShape().size() - 1; iter > 0;
             --iter) {
          stride *= outputType.getShape()[iter];
          upperLayerStrides.insert(upperLayerStrides.begin(),
                                   b.getI32IntegerAttr(stride));
        }

        // Compute lower layer dimensions and bounds.
        for (unsigned iter = 0; iter < inputType.getShape().size(); ++iter) {
          lowerLayerShape.push_back(
              b.getI32IntegerAttr(inputType.getShape()[iter]));
          lowerLayerDims.push_back(b.getI32IntegerAttr(iter));
        }

        // Populate metadata attribute.
        DictionaryAttr metadata = b.getDictionaryAttr(
            {b.getNamedAttr(
                 "map", b.getAffineMapArrayAttr({outputType.getLayout().getAffineMap()})),
             b.getNamedAttr(
                 "layout",
                 b.getArrayAttr({b.getDictionaryAttr(
                     {b.getNamedAttr("lower_layer_dimensions",
                                     b.getArrayAttr(lowerLayerDims)),
                      b.getNamedAttr("transformation",
                                     b.getStringAttr("Embed")),
                      b.getNamedAttr("parameters",
                                     b.getArrayAttr(upperLayerStrides)),
                      b.getNamedAttr("upper_layer_dimensions",
                                     b.getArrayAttr(upperLayerDims))})})),
             b.getNamedAttr("upper_layer_bounds",
                            b.getArrayAttr(upperLayerShape)),
             b.getNamedAttr("lower_layer_bounds",
                            b.getArrayAttr(lowerLayerShape))});

        user->setAttr(
            "coord_transforms",
            b.getArrayAttr({b.getDictionaryAttr(
                {b.getNamedAttr("operand",
                                b.getI32IntegerAttr(userOperandIndex)),
                 b.getNamedAttr("transforms", b.getAffineMapArrayAttr(
                                                {outputType.getLayout().getAffineMap()})),
                 b.getNamedAttr("metadata", b.getArrayAttr({metadata}))})}));
      } else {
        // Only do this for miopen.xdlops_gemm_v2 operation.
        // Do not alter attributes if the user is miopen.threadwise_copy.
        if ((user->getName().getStringRef() ==
             miopen::XdlopsGemmV2Op::getOperationName())) {

          // create a deep-copy of existing attributes, and amend the new one.
          // need to figure out if there's a better way than this.
          auto arrayAttr = coordTransformAttrs.cast<ArrayAttr>();
          llvm::SmallVector<Attribute, 2> augmentedArrayAttr;

          //llvm::errs() << "\nexisting transforms:\n";
          //coordTransformAttrs.dump();
          //llvm::errs() << "\ntransform to be added:\n";
          //llvm::errs() << "operand: " << userOperandIndex << "\n";
          //if (!outputType.getLayout().isIdentity()) {
          //  llvm::errs() << "transforms: " << outputType.getAffineMap() << "\n";
          //}

          bool augmented = false;
          for (unsigned idx = 0; idx < arrayAttr.size(); ++idx) {
            auto dictAttr = arrayAttr.getValue()[idx].cast<DictionaryAttr>();
            auto operandIndex =
                dictAttr.get("operand").cast<IntegerAttr>().getInt();

            if (operandIndex != userOperandIndex) {
              augmentedArrayAttr.push_back(dictAttr);
            } else {
              //auto existingTransforms =
              //    dictAttr.get("transforms").cast<ArrayAttr>();
              llvm::SmallVector<Attribute, 4> augmentedTransforms;
              //augmentedTransforms.append(existingTransforms.begin(),
              //                           existingTransforms.end());
              augmentedTransforms.push_back(
                 AffineMapAttr::get(outputType.getLayout().getAffineMap()));

              augmentedArrayAttr.push_back(b.getDictionaryAttr(
                  {b.getNamedAttr("operand",
                                  b.getI32IntegerAttr(userOperandIndex)),
                   b.getNamedAttr("transforms",
                                  b.getArrayAttr(augmentedTransforms))}));
              augmented = true;
            }
          }
          if (!augmented)
            augmentedArrayAttr.push_back(b.getDictionaryAttr(
                {b.getNamedAttr("operand",
                                b.getI32IntegerAttr(userOperandIndex)),
                 b.getNamedAttr("transforms", b.getAffineMapArrayAttr(
                                              {outputType.getLayout().getAffineMap()}))}));

          //llvm::errs() << "\naugmented transforms:\n";
          //b.getArrayAttr(augmentedArrayAttr).dump();
          user->setAttr("coord_transforms", b.getArrayAttr(augmentedArrayAttr));
        }
      }
    }

    // Pass the input to uses of this op.
    op.replaceAllUsesWith(op.input());

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Transform lowering.
//===----------------------------------------------------------------------===//

struct TransformRewritePattern : public OpRewritePattern<miopen::TransformOp> {
  using OpRewritePattern<miopen::TransformOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::TransformOp op,
                                PatternRewriter &b) const override {
    auto outputType = op.output().getType().cast<MemRefType>();
    auto outputShape = outputType.getShape();
    auto boundCheckAttr = op->template getAttrOfType<ArrayAttr>("bound_check");

    // determine output shape and track it in an attribute.
    llvm::SmallVector<Attribute, 4> shapeAttrVec;
    for (unsigned i = 0; i < outputShape.size(); ++i) {
      shapeAttrVec.push_back(b.getI32IntegerAttr(outputShape[i]));
    }

    // auto attr = b.getNamedAttr("domain",
    //               b.getArrayAttr(shapeAttrVec));
    // llvm::errs() << "\n\ndomain attr:\n";
    // attr.second.dump();
    // llvm::errs() << "\n";
    // llvm::errs() << "\n========\nTransformOp:\n";
    // op.dump();

    // Pass the output affine map to users of this op.
    if (!outputType.getLayout().isIdentity())
      for (auto user : op.output().getUsers()) {
        // llvm::errs() << "\n========\nTransformOp user:\n";
        // user->dump();

        // determine user domain

        unsigned userOperandIndex = 0;
        for (userOperandIndex = 0; userOperandIndex < user->getNumOperands();
             ++userOperandIndex)
          if (user->getOperand(userOperandIndex) == op.output())
            break;

        auto coordTransformAttrs = user->getAttr("coord_transforms");
        if (!coordTransformAttrs) {
          llvm::SmallVector<NamedAttribute, 5> arrayAttr{
              b.getNamedAttr("operand", b.getI32IntegerAttr(userOperandIndex)),
              b.getNamedAttr("transforms", b.getAffineMapArrayAttr(
                                           {outputType.getLayout().getAffineMap()})),
              b.getNamedAttr("domain", b.getArrayAttr(shapeAttrVec)),
              b.getNamedAttr("metadata", b.getArrayAttr({b.getDictionaryAttr(
                                             op->getAttrs())}))};

          if (boundCheckAttr)
            arrayAttr.push_back(b.getNamedAttr("bound_check", boundCheckAttr));

          user->setAttr("coord_transforms",
                        b.getArrayAttr({b.getDictionaryAttr(
                            {arrayAttr.begin(), arrayAttr.end()})}));
        } else {
          // create a deep-copy of existing attributes, and amend the new one.
          // need to figure out if there's a better way than this.
          auto arrayAttr = coordTransformAttrs.cast<ArrayAttr>();
          llvm::SmallVector<Attribute, 2> augmentedArrayAttr;

          bool augmented = false;
          for (unsigned idx = 0; idx < arrayAttr.size(); ++idx) {
            auto dictAttr = arrayAttr.getValue()[idx].cast<DictionaryAttr>();
            auto operandIndex =
                dictAttr.get("operand").cast<IntegerAttr>().getInt();

            if (operandIndex != userOperandIndex) {
              augmentedArrayAttr.push_back(dictAttr);
            } else {
              auto existingTransforms =
                  dictAttr.get("transforms").cast<ArrayAttr>();

              auto existingDomain = dictAttr.get("domain").cast<ArrayAttr>();
              auto existingMetadata =
                  dictAttr.get("metadata").cast<ArrayAttr>();
              llvm::SmallVector<Attribute, 5> augmentedMetadata;
              augmentedMetadata.append(existingMetadata.begin(),
                                       existingMetadata.end());
              augmentedMetadata.push_back(b.getDictionaryAttr(op->getAttrs()));

              llvm::SmallVector<NamedAttribute, 4> arrayAttr{
                  b.getNamedAttr("operand",
                                 b.getI32IntegerAttr(userOperandIndex)),
                  b.getNamedAttr("transforms", existingTransforms),
                  b.getNamedAttr("domain", existingDomain),
                  b.getNamedAttr("metadata",
                                 b.getArrayAttr(augmentedMetadata))};
              auto existingBoundCheck = dictAttr.get("bound_check");
              if (boundCheckAttr && !existingBoundCheck)
                arrayAttr.push_back(
                    b.getNamedAttr("bound_check", boundCheckAttr));
              else if (!boundCheckAttr && existingBoundCheck)
                arrayAttr.push_back(
                    b.getNamedAttr("bound_check", existingBoundCheck));
              else if (boundCheckAttr && existingBoundCheck) {
                llvm::SmallVector<Attribute, 5> boundVector;
                for (size_t j = 0; j < boundCheckAttr.size(); j++) {
                  auto value = boundCheckAttr[j].cast<IntegerAttr>().getInt();
                  if (value)
                    boundVector.push_back(boundCheckAttr[j]);
                  else
                    boundVector.push_back(
                        existingBoundCheck.cast<ArrayAttr>()[j]);
                }
                arrayAttr.push_back(b.getNamedAttr(
                    "bound_check",
                    b.getArrayAttr({boundVector.begin(), boundVector.end()})));
              }
              augmentedArrayAttr.push_back(
                  b.getDictionaryAttr({arrayAttr.begin(), arrayAttr.end()}));

              augmented = true;
            }
          }
          if (!augmented) {
            llvm::SmallVector<NamedAttribute, 4> arrayAttr{
                b.getNamedAttr("operand",
                               b.getI32IntegerAttr(userOperandIndex)),
                b.getNamedAttr("transforms", b.getAffineMapArrayAttr(
                                                 {outputType.getLayout().getAffineMap()})),
                b.getNamedAttr("domain", b.getArrayAttr(shapeAttrVec)),
                b.getNamedAttr("metadata", b.getArrayAttr({b.getDictionaryAttr(
                                               op->getAttrs())}))};

            if (boundCheckAttr)
              arrayAttr.push_back(
                  b.getNamedAttr("bound_check", boundCheckAttr));
            augmentedArrayAttr.push_back(
                b.getDictionaryAttr({arrayAttr.begin(), arrayAttr.end()}));
          }
          user->setAttr("coord_transforms", b.getArrayAttr(augmentedArrayAttr));
        }

        // llvm::errs() << "\n========\nTransformOp updated user:\n";
        // user->dump();
        // llvm::errs() << "\n";
      }

    // Pass the input to uses of this op.
    op.replaceAllUsesWith(op.input());

    op.erase();
    return success();
  }
};

//===----------------------------------------------------------------------===//
// XdlopsGemmV2 lowering.
//===----------------------------------------------------------------------===//

struct XdlopsGemmV2RewritePattern
    : public OpRewritePattern<miopen::XdlopsGemmV2Op> {
  using OpRewritePattern<miopen::XdlopsGemmV2Op>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::XdlopsGemmV2Op op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    // Obtain critical information.
    int64_t M = op->getAttr("m").template cast<IntegerAttr>().getInt();
    int64_t N = op->getAttr("n").template cast<IntegerAttr>().getInt();
    int64_t K = op->getAttr("k").template cast<IntegerAttr>().getInt();
    int64_t MPerWave =
        op->getAttr("m_per_wave").template cast<IntegerAttr>().getInt();
    int64_t NPerWave =
        op->getAttr("n_per_wave").template cast<IntegerAttr>().getInt();

    // Obtain coordinate transforms for Matrix A and B.
    auto coordTransformsAttr =
        op->getAttr("coord_transforms").template cast<ArrayAttr>();
    AffineMap transformMatrixA, transformMatrixB;
    for (auto transformAttr : coordTransformsAttr) {
      auto dictAttr = transformAttr.template cast<DictionaryAttr>();
      auto operandIndex =
          dictAttr.get("operand").template cast<IntegerAttr>().getInt();
      auto transforms = dictAttr.get("transforms").template cast<ArrayAttr>();
      if (transforms.size() > 0) {
        // Use the first affine map in the transforms array.
        auto affineMap = transforms[0].template cast<AffineMapAttr>();
        if (operandIndex == 0) {
          transformMatrixA = affineMap.getValue();
        } else if (operandIndex == 1) {
          transformMatrixB = affineMap.getValue();
        }
      }
    }

    auto dataType =
        op.matrixA().getType().template cast<MemRefType>().getElementType();

    auto MConstantOp = b.create<ConstantIndexOp>(loc, M);
    auto NConstantOp = b.create<ConstantIndexOp>(loc, N);
    auto KConstantOp = b.create<ConstantIndexOp>(loc, K);

    // Logic to do XDLOPS code selection.
    // llvm::errs() << "Invoke XDLOPS code selection logic:\n";
    // llvm::errs() << "dataType: "; dataType.dump(); llvm::errs() << "\n";
    // llvm::errs() << "MPerWave: " << MPerWave << "\n";
    // llvm::errs() << "NPerWave: " << NPerWave << "\n";

    XdlopsCodeSelection xcs = XdlopsCodeSelection::get(dataType, MPerWave, NPerWave, b);

    // Extract values from XdlopsCodeSelection.
    StringRef mfmaInstr = xcs.mfmaInstr;
    int64_t MPerXdlops = xcs.MPerXdlops;
    int64_t NPerXdlops = xcs.NPerXdlops;
    int64_t MRepeats = xcs.MRepeats;
    int64_t NRepeats = xcs.NRepeats;
    VectorType vectorType = xcs.vectorType;
    int64_t vectorNumber = xcs.vectorNumber;
    SmallVector<SmallVector<unsigned, 3>, 2> imms = xcs.imms;
    Type argType = xcs.argType;

    int64_t num_threads_blk = xcs.num_threads_blk;
    int64_t wave_size = xcs.wave_size;
    int64_t num_input_blks = xcs.num_input_blks;
    int64_t num_output_blks = xcs.num_output_blks;
    int64_t k_base = xcs.k_base;

    bool IsKReduction = (num_output_blks == 1) && (num_input_blks > 1);

    // Original C++ logic.
    // const index_t laneId = get_thread_local_1d_id() % mfma_type.wave_size;
    // FloatA a[K * MRepeats];
    // FloatB b[K * NRepeats];
    // constexpr index_t KRepeats = sizeof(FloatA) / (sizeof(data_type) *
    // mfma_type.k_base); auto pa = reinterpret_cast<const data_type*>(&a); auto
    // pb = reinterpret_cast<const data_type*>(&b); constexpr index_t AStride =
    // K * KRepeats; constexpr index_t BStride = K * KRepeats;

    auto tid = b.create<miopen::WorkitemIdOp>(loc, b.getIndexType());
    auto laneId =
        b.create<RemUIOp>(loc, tid, b.create<ConstantIndexOp>(loc, wave_size));

    // TBD. FloatA / FloatB could be vectorized via KPack tuning parameter.
    // Ignore this for now. use arrayA as pa for now. use arrayB as pb for now.

    // TBD. FloatA / FloatB could be vectorized via KPack tuning parameter.
    // Ignore this for now. This must be fixed when we test fp16 / bf16 data
    // types.

    // TBD. Existing logic for fp16/bf16 may still NOT be 100% correct.
    int64_t KRepeats = 0;
    if (dataType == b.getF32Type()) {
      KRepeats = 1 / k_base;
    } else if (dataType == b.getF16Type() || dataType == b.getIntegerType(16)) {
      VectorType argVectorType = argType.template cast<VectorType>();
      KRepeats = argVectorType.getShape()[0] / k_base;
    }

    auto MPerXdlopsConstantOp = b.create<ConstantIndexOp>(loc, MPerXdlops);
    auto NPerXdlopsConstantOp = b.create<ConstantIndexOp>(loc, NPerXdlops);
    auto kBaseKRepeatsConstantOp =
        b.create<ConstantIndexOp>(loc, KRepeats * k_base);

    if (!IsKReduction) {
      // store bufferA logic.

      // Original C++ logic.
      // static_if<!IsKReduction>{}([&](auto) {
      //     for(index_t m_i = 0; m_i < MRepeats; ++m_i)
      //         for(index_t k_i      = 0; k_i < K; ++k_i)
      //             a[k_i + m_i * K] = p_a_wave[k_i * M + laneId + MPerXdlops *
      //             m_i];
      // p_a_wave need to be offseted by waveOffsetA.

      auto outerLoopM = b.create<AffineForOp>(loc, 0, MRepeats);
      auto olmb = OpBuilder::atBlockTerminator(outerLoopM.getBody());
      auto olmiv = outerLoopM.getInductionVar();
      auto mOffset = olmb.create<MulIOp>(loc, MPerXdlopsConstantOp, olmiv);
      auto kOffsetA = olmb.create<MulIOp>(loc, olmiv, KConstantOp);

      auto innerLoopMK = olmb.create<AffineForOp>(loc, 0, K);
      auto ilmkb = OpBuilder::atBlockTerminator(innerLoopMK.getBody());
      auto ilmkiv = innerLoopMK.getInductionVar();

      //             a[k_i + m_i * K] = p_a_wave[k_i * M + laneId + MPerXdlops *
      //             m_i];
      // p_a_wave need to be offseted by waveOffsetA.
      Value sourceOffsetBeforeTransformA = ilmkb.create<AddIOp>(
          loc, op.waveOffsetA(),
          ilmkb.create<AddIOp>(
              loc,
              ilmkb.create<AddIOp>(
                  loc, ilmkb.create<MulIOp>(loc, ilmkiv, MConstantOp), laneId),
              mOffset));

      // Apply coord_transform for matrix A if necessarily.
      SmallVector<Value, 8> sourceOffsetA;
      if (transformMatrixA)
        sourceOffsetA =
            expandAffineMap(ilmkb, loc, transformMatrixA,
                            ValueRange{sourceOffsetBeforeTransformA})
                .getValue();
      else
        sourceOffsetA.push_back(sourceOffsetBeforeTransformA);

      auto destOffsetA = ilmkb.create<AddIOp>(loc, ilmkiv, kOffsetA);

      auto valueA =
          ilmkb.create<memref::LoadOp>(loc, dataType, op.matrixA(), sourceOffsetA);
      ilmkb.create<memref::StoreOp>(loc, valueA, op.bufferA(), ValueRange{destOffsetA});

      // store bufferB logic.

      // Original C++ logic.
      //     for(index_t n_i = 0; n_i < NRepeats; ++n_i)
      //         for(index_t k_i      = 0; k_i < K; ++k_i)
      //             b[k_i + n_i * K] = p_b_wave[k_i * N + laneId + NPerXdlops *
      //             n_i];
      // p_b_wave need to be offseted by waveOffsetB.

      auto outerLoopN = b.create<AffineForOp>(loc, 0, NRepeats);
      auto olnb = OpBuilder::atBlockTerminator(outerLoopN.getBody());
      auto olniv = outerLoopN.getInductionVar();
      auto nOffset = olnb.create<MulIOp>(loc, NPerXdlopsConstantOp, olniv);
      auto kOffsetB = olnb.create<MulIOp>(loc, olniv, KConstantOp);

      auto innerLoopNK = olnb.create<AffineForOp>(loc, 0, K);
      auto ilnkb = OpBuilder::atBlockTerminator(innerLoopNK.getBody());
      auto ilnkiv = innerLoopNK.getInductionVar();

      //             b[k_i + n_i * K] = p_b_wave[k_i * N + laneId + NPerXdlops *
      //             n_i];
      // p_b_wave need to be offseted by waveOffsetB.
      Value sourceOffsetBeforeTransformB = ilnkb.create<AddIOp>(
          loc, op.waveOffsetB(),
          ilnkb.create<AddIOp>(
              loc,
              ilnkb.create<AddIOp>(
                  loc, ilnkb.create<MulIOp>(loc, ilnkiv, NConstantOp), laneId),
              nOffset));

      // Apply coord_transform for matrix B if necessarily.
      SmallVector<Value, 8> sourceOffsetB;
      if (transformMatrixB)
        sourceOffsetB =
            expandAffineMap(ilnkb, loc, transformMatrixB,
                            ValueRange{sourceOffsetBeforeTransformB})
                .getValue();
      else
        sourceOffsetB.push_back(sourceOffsetBeforeTransformB);

      auto destOffsetB = ilnkb.create<AddIOp>(loc, ilnkiv, kOffsetB);

      auto valueB =
          ilnkb.create<memref::LoadOp>(loc, dataType, op.matrixB(), sourceOffsetB);
      ilnkb.create<memref::StoreOp>(loc, valueB, op.bufferB(), ValueRange{destOffsetB});

      // Original C++ logic.
      // for(index_t k_i = 0; k_i < K * KRepeats; ++k_i)
      // {
      //     p_c_thread = mfma_type.template run<MPerXdlops * MRepeats,
      //                                         NPerXdlops * NRepeats,
      //                                         AStride,
      //                                         BStride>(
      //         &pa[k_i * mfma_type.k_base], &pb[k_i * mfma_type.k_base],
      //         p_c_thread);
      // }

      // Instead of following C++ logic where the induction variable is
      // increased by one, increase by k_base. Mathmetically they are
      // equivalent.
      auto loopK =
          b.create<AffineForOp>(loc, 0, K * KRepeats, k_base, op.vectorCs());
      auto loopKb = OpBuilder::atBlockBegin(loopK.getBody());
      auto loopKiv = loopK.getInductionVar();

      Value argA, argB;
      if (dataType == b.getF32Type()) {
        argA = loopKb.create<memref::LoadOp>(loc, dataType, op.bufferA(),
                                     ValueRange{loopKiv});
        argB = loopKb.create<memref::LoadOp>(loc, dataType, op.bufferB(),
                                     ValueRange{loopKiv});
      } else if (dataType == b.getF16Type() ||
                 dataType == b.getIntegerType(16)) {
        argA = loopKb.create<vector::TransferReadOp>(
            loc, argType.template cast<VectorType>(), op.bufferA(),
            ValueRange{loopKiv});
        argB = loopKb.create<vector::TransferReadOp>(
            loc, argType.template cast<VectorType>(), op.bufferB(),
            ValueRange{loopKiv});
      }

      SmallVector<Value, 4> mfmas;
      for (int64_t i = 0; i < vectorNumber; ++i) {
        auto vectorC = loopK.getRegionIterArgs()[i];

        // issue MFMA logic.
        // TBD: need to consider the case to use argA[AStride] and argB[BStride]
        auto mfma = loopKb.create<miopen::MFMAV2Op>(loc, vectorType, argA, argB,
                                                    vectorC);

        mfma->setAttr("instr", loopKb.getStringAttr(mfmaInstr));
        mfma->setAttr(
            "imm", loopKb.getArrayAttr({loopKb.getI32IntegerAttr(imms[i][0]),
                                        loopKb.getI32IntegerAttr(imms[i][1]),
                                        loopKb.getI32IntegerAttr(imms[i][2])}));
        mfmas.push_back(mfma);
      }
      loopKb.create<AffineYieldOp>(loc, mfmas);
      op.replaceAllUsesWith(loopK.results());
      op.erase();
    } else {
      // Original C++ logic.
      //     const index_t blk_id = laneId / mfma_type.num_threads_blk;
      //     const index_t blk_td = laneId % mfma_type.num_threads_blk;

      auto NumThreadsBlkConstantOp = b.create<ConstantIndexOp>(loc, num_threads_blk);
      auto blk_id = b.create<DivUIOp>(loc, laneId, NumThreadsBlkConstantOp);
      auto blk_td = b.create<RemUIOp>(loc, laneId, NumThreadsBlkConstantOp);

      // Original C++ logic.
      //     // load into registers
      //     for(index_t k_i = 0; k_i < K; k_i += mfma_type.num_input_blks) {
      //         a[k_i] = p_a_wave[(k_i + blk_id) * M + blk_td];
      //         b[k_i] = p_b_wave[(k_i + blk_id) * N + blk_td];
      //     }
      // p_a_wave need to be offseted by waveOffsetA.
      // p_b_wave need to be offseted by waveOffsetB.

      // Instead loop to K, change loop bound to K / num_input_blks.
      auto loopKLoadIteration = K / num_input_blks;
      auto loopKLoad = b.create<AffineForOp>(loc, 0, loopKLoadIteration);

      auto NumInputBlksConstantOp = b.create<ConstantIndexOp>(loc, num_input_blks);

      auto lklb = OpBuilder::atBlockTerminator(loopKLoad.getBody());
      auto lkliv = loopKLoad.getInductionVar();

      //         a[k_i] = p_a_wave[(k_i + blk_id) * M + blk_td];
      //         b[k_i] = p_b_wave[(k_i + blk_id) * N + blk_td];
      // p_a_wave need to be offseted by waveOffsetA.
      // p_b_wave need to be offseted by waveOffsetB.

      // NOTICE: We times k_i by num_input_blks in MLIR path.
      Value sourceOffsetBeforeTransformA = lklb.create<AddIOp>(
          loc, op.waveOffsetA(),
          lklb.create<AddIOp>(
              loc,
              lklb.create<MulIOp>(
                  loc,
                  lklb.create<AddIOp>(
                      loc,
                      lklb.create<MulIOp>(loc, lkliv, NumInputBlksConstantOp),
                      blk_id),
                  MConstantOp),
              blk_td));

      // Apply coord_transform for matrix A if necessarily.
      SmallVector<Value, 8> sourceOffsetA;
      if (transformMatrixA)
        sourceOffsetA =
            expandAffineMap(lklb, loc, transformMatrixA,
                            ValueRange{sourceOffsetBeforeTransformA})
                .getValue();
      else
        sourceOffsetA.push_back(sourceOffsetBeforeTransformA);

      auto valueA = lklb.create<memref::LoadOp>(loc, dataType, op.matrixA(),
                                        ValueRange{sourceOffsetA});
      lklb.create<memref::StoreOp>(loc, valueA, op.bufferA(), ValueRange{lkliv});

      // NOTICE: We times k_i by num_input_blks in MLIR path.
      Value sourceOffsetBeforeTransformB = lklb.create<AddIOp>(
          loc, op.waveOffsetB(),
          lklb.create<AddIOp>(
              loc,
              lklb.create<MulIOp>(
                  loc,
                  lklb.create<AddIOp>(
                      loc,
                      lklb.create<MulIOp>(loc, lkliv, NumInputBlksConstantOp),
                      blk_id),
                  NConstantOp),
              blk_td));

      // Apply coord_transform for matrix B if necessarily.
      SmallVector<Value, 8> sourceOffsetB;
      if (transformMatrixB)
        sourceOffsetB =
            expandAffineMap(lklb, loc, transformMatrixB,
                            ValueRange{sourceOffsetBeforeTransformB})
                .getValue();
      else
        sourceOffsetB.push_back(sourceOffsetBeforeTransformB);

      auto valueB = lklb.create<memref::LoadOp>(loc, dataType, op.matrixB(),
                                        ValueRange{sourceOffsetB});
      lklb.create<memref::StoreOp>(loc, valueB, op.bufferB(), ValueRange{lkliv});

      // Original C++ logic.
      // for(index_t k_i = 0; k_i < K; k_i += mfma_type.num_input_blks)
      // {
      //     for(index_t i = 0; i < KRepeats; ++i)
      //         p_c_thread = mfma_type.template run<MPerXdlops, NPerXdlops,
      //         AStride, BStride>(
      //             &pa[(k_i * KRepeats + i) * mfma_type.k_base],
      //             &pb[(k_i * KRepeats + i) * mfma_type.k_base],
      //             p_c_thread);
      // }

      // Change loop bound to the same as loopKLoadIteration.
      // Instead of increasing num_input_blks, increase k_base.
      auto outerLoop = b.create<AffineForOp>(loc, 0, loopKLoadIteration, k_base,
                                             op.vectorCs());
      auto outerLoopb = OpBuilder::atBlockBegin(outerLoop.getBody());
      auto outerLoopiv = outerLoop.getInductionVar();
      auto outerOffset =
          outerLoopb.create<MulIOp>(loc, outerLoopiv, kBaseKRepeatsConstantOp);

      auto innerLoop = outerLoopb.create<AffineForOp>(
          loc, 0, KRepeats * k_base, k_base, outerLoop.getRegionIterArgs());
      auto innerLoopb = OpBuilder::atBlockBegin(innerLoop.getBody());
      auto innerLoopiv = innerLoop.getInductionVar();

      auto offset = innerLoopb.create<AddIOp>(loc, outerOffset, innerLoopiv);

      Value argA, argB;
      if (dataType == b.getF32Type()) {
        argA = innerLoopb.create<memref::LoadOp>(loc, dataType, op.bufferA(),
                                         ValueRange{offset});
        argB = innerLoopb.create<memref::LoadOp>(loc, dataType, op.bufferB(),
                                         ValueRange{offset});
      } else if (dataType == b.getF16Type() ||
                 dataType == b.getIntegerType(16)) {
        argA = innerLoopb.create<vector::TransferReadOp>(
            loc, argType.template cast<VectorType>(), op.bufferA(),
            ValueRange{offset});
        argB = innerLoopb.create<vector::TransferReadOp>(
            loc, argType.template cast<VectorType>(), op.bufferB(),
            ValueRange{offset});
      }

      SmallVector<Value, 4> mfmas;
      for (int64_t i = 0; i < vectorNumber; ++i) {
        auto vectorC = innerLoop.getRegionIterArgs()[i];
        auto mfma = innerLoopb.create<miopen::MFMAV2Op>(loc, vectorType, argA,
                                                        argB, vectorC);

        mfma->setAttr("instr", innerLoopb.getStringAttr(mfmaInstr));
        mfma->setAttr("imm", innerLoopb.getArrayAttr(
                                 {innerLoopb.getI32IntegerAttr(imms[i][0]),
                                  innerLoopb.getI32IntegerAttr(imms[i][1]),
                                  innerLoopb.getI32IntegerAttr(imms[i][2])}));
        mfmas.push_back(mfma);
      }
      innerLoopb.create<AffineYieldOp>(loc, mfmas);

      outerLoopb.create<AffineYieldOp>(loc, innerLoop.results());

      op.replaceAllUsesWith(outerLoop.results());
      op.erase();
    }

    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseGemmV2 lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseGemmV2RewritePattern
    : public OpRewritePattern<miopen::BlockwiseGemmV2Op> {
  using OpRewritePattern<miopen::BlockwiseGemmV2Op>::OpRewritePattern;

  LogicalResult matchAndRewrite(miopen::BlockwiseGemmV2Op op,
                                PatternRewriter &b) const override {
    auto loc = op.getLoc();

    int64_t MPerWave =
        op->getAttr("m_per_wave").template cast<IntegerAttr>().getInt();
    int64_t NPerWave =
        op->getAttr("n_per_wave").template cast<IntegerAttr>().getInt();

    // Original C++ logic.
    // static constexpr index_t MRepeats = (GemmMPerWave > 64) ? (GemmMPerWave /
    // 64) : 1; static constexpr index_t NRepeats = (GemmNPerWave > 64) ?
    // (GemmNPerWave / 64) : 1; static constexpr index_t MPerXdlops =
    // (GemmMPerWave > 64) ? 64 : GemmMPerWave; static constexpr index_t
    // NPerXdlops = (GemmNPerWave > 64) ? 64 : GemmNPerWave;

    int64_t MRepeats = (MPerWave > 64) ? (MPerWave / 64) : 1;
    int64_t NRepeats = (NPerWave > 64) ? (NPerWave / 64) : 1;
    int64_t MPerXdlops = (MPerWave > 64) ? 64 : MPerWave;
    int64_t NPerXdlops = (NPerWave > 64) ? 64 : NPerWave;

    if (MRepeats == 1 && NRepeats == 1) {
      SmallVector<Type, 2> resultTypes;
      for (auto result : op.vectorDs()) {
        resultTypes.push_back(result.getType());
      }

      auto xdlopsGemmV2Op = b.create<miopen::XdlopsGemmV2Op>(
          loc, resultTypes, op.matrixA(), op.matrixB(), op.waveOffsetA(),
          op.waveOffsetB(), op.bufferA(), op.bufferB(), op.vectorCs());

      xdlopsGemmV2Op->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op->setAttr("k", op->getAttr("k"));
      xdlopsGemmV2Op->setAttr("m_per_wave", op->getAttr("m_per_wave"));
      xdlopsGemmV2Op->setAttr("n_per_wave", op->getAttr("n_per_wave"));
      xdlopsGemmV2Op->setAttr("coord_transforms",
                              op->getAttr("coord_transforms"));

      op.replaceAllUsesWith(xdlopsGemmV2Op.vectorDs());
      op.erase();
    } else if (MRepeats == 2 && NRepeats == 1) {
      // Original C++ logic.
      // p_c_thread.s.x.l = XdlopsGemm.template Run<M, N, K>(p_a_block,
      // p_b_block, p_c_thread.s.x.l); p_c_thread.s.y.l = XdlopsGemm.template
      // Run<M, N, K>(p_a_block + MPerXdlops, p_b_block, p_c_thread.s.y.l);

      SmallVector<Type, 2> resultTypes0;
      resultTypes0.push_back(op.vectorDs()[0].getType());
      resultTypes0.push_back(op.vectorDs()[1].getType());

      auto xdlopsGemmV2Op0 = b.create<miopen::XdlopsGemmV2Op>(
          loc, resultTypes0, op.matrixA(), op.matrixB(), op.waveOffsetA(),
          op.waveOffsetB(), op.bufferA(), op.bufferB(),
          ValueRange{op.vectorCs()[0], op.vectorCs()[1]});

      xdlopsGemmV2Op0->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op0->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op0->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op0->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op0->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op0->setAttr("coord_transforms",
                               op->getAttr("coord_transforms"));

      SmallVector<Type, 2> resultTypes1;
      resultTypes1.push_back(op.vectorDs()[2].getType());
      resultTypes1.push_back(op.vectorDs()[3].getType());

      auto MPerXdlopsConstantOp = b.create<ConstantIndexOp>(loc, MPerXdlops);
      auto xdlopsGemmV2Op1 = b.create<miopen::XdlopsGemmV2Op>(
          loc, resultTypes1, op.matrixA(), op.matrixB(),
          b.create<AddIOp>(loc, op.waveOffsetA(), MPerXdlopsConstantOp),
          op.waveOffsetB(), op.bufferA(), op.bufferB(),
          ValueRange{op.vectorCs()[2], op.vectorCs()[3]});

      xdlopsGemmV2Op1->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op1->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op1->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op1->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op1->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op1->setAttr("coord_transforms",
                               op->getAttr("coord_transforms"));

      op.replaceAllUsesWith(ValueRange{
          xdlopsGemmV2Op0.vectorDs()[0], xdlopsGemmV2Op0.vectorDs()[1],
          xdlopsGemmV2Op1.vectorDs()[0], xdlopsGemmV2Op1.vectorDs()[1]});
      op.erase();
    } else if (MRepeats == 1 && NRepeats == 2) {
      // Original C++ logic.
      // p_c_thread.s.x.l = XdlopsGemm.template Run<M, N, K>(p_a_block,
      // p_b_block, p_c_thread.s.x.l); p_c_thread.s.y.l = XdlopsGemm.template
      // Run<M, N, K>(p_a_block, p_b_block + NPerXdlops, p_c_thread.s.y.l);

      SmallVector<Type, 2> resultTypes0;
      resultTypes0.push_back(op.vectorDs()[0].getType());
      resultTypes0.push_back(op.vectorDs()[1].getType());

      auto xdlopsGemmV2Op0 = b.create<miopen::XdlopsGemmV2Op>(
          loc, resultTypes0, op.matrixA(), op.matrixB(), op.waveOffsetA(),
          op.waveOffsetB(), op.bufferA(), op.bufferB(),
          ValueRange{op.vectorCs()[0], op.vectorCs()[1]});

      xdlopsGemmV2Op0->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op0->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op0->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op0->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op0->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op0->setAttr("coord_transforms",
                               op->getAttr("coord_transforms"));

      SmallVector<Type, 2> resultTypes1;
      resultTypes1.push_back(op.vectorDs()[2].getType());
      resultTypes1.push_back(op.vectorDs()[3].getType());

      auto NPerXdlopsConstantOp = b.create<ConstantIndexOp>(loc, NPerXdlops);
      auto xdlopsGemmV2Op1 = b.create<miopen::XdlopsGemmV2Op>(
          loc, resultTypes1, op.matrixA(), op.matrixB(), op.waveOffsetA(),
          b.create<AddIOp>(loc, op.waveOffsetB(), NPerXdlopsConstantOp),
          op.bufferA(), op.bufferB(),
          ValueRange{op.vectorCs()[2], op.vectorCs()[3]});

      xdlopsGemmV2Op1->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op1->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op1->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op1->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op1->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op1->setAttr("coord_transforms",
                               op->getAttr("coord_transforms"));

      op.replaceAllUsesWith(ValueRange{
          xdlopsGemmV2Op0.vectorDs()[0], xdlopsGemmV2Op0.vectorDs()[1],
          xdlopsGemmV2Op1.vectorDs()[0], xdlopsGemmV2Op1.vectorDs()[1]});
      op.erase();
    }

    return success();
  }
};

#endif // MLIR_DIALECT_MIOPEN_LOWERMIOPENOPS_H
