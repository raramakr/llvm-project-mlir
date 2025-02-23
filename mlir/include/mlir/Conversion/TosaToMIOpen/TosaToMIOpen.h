//===-- TosaToMIOpen.h - TOSA optimization pass declarations ----------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the passes for the TOSA MIOpen Dialect in MLIR.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_CONVERSION_TOSATOMIOPEN_TOSATOMIOPEN_H
#define MLIR_CONVERSION_TOSATOMIOPEN_TOSATOMIOPEN_H

#include "mlir/Pass/Pass.h"

namespace mlir {
namespace tosa {

/// Create a pass to convert Tosa conv2d operations to MIOpen operations.
std::unique_ptr<Pass> createTosaToMIOpenPass();

/// Populates passes to convert from TOSA to MIOpen on buffers. At the end of
/// the pass, the function will only contain MIOpen ops or standard ops if the
/// pipeline succeeds.
void addTosaToMIOpenPasses(OpPassManager &pm);

/// Populates conversion passes from TOSA dialect to MIOpen dialect.
void populateTosaToMIOpenConversionPatterns(MLIRContext *context,
                                            OwningRewritePatternList *patterns);

} // namespace tosa
} // namespace mlir

#endif // MLIR_CONVERSION_TOSATOMIOPEN_TOSATOMIOPEN_H
