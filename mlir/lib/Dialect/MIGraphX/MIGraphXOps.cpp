//===- MIGraphXOps.cpp - MIGraphX MLIR Operations -----------------------------===//
//
// Part of the MLIR Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

//#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/MathExtras.h"
#include "mlir/Dialect/MIGraphX/MIGraphXOps.h"

#include "mlir/Dialect/MIGraphX/MIGraphXOpsDialect.cpp.inc"
#include "mlir/Dialect/MIGraphX/MIGraphXTypes.cpp.inc"

using namespace mlir;
//using namespace mlir::migraphx;
using namespace migraphx;

//===----------------------------------------------------------------------===//
// MIGraphXDialect
//===----------------------------------------------------------------------===//

void MIGraphXDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "mlir/Dialect/MIGraphX/MIGraphXOps.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// ConvolutionOp
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// BatchNormOp
//===----------------------------------------------------------------------===//
static ParseResult parseBatchNormOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<Type, 2> types;
  return failure(
      parser.parseOperandList(ops, OpAsmParser::Delimiter::Paren) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types) ||
      parser.resolveOperands(ops, types, parser.getNameLoc(), result.operands));
}

static void print(OpAsmPrinter &p, BatchNormOp op) {
  p << op.getOperationName() << "(" << op.getOperands() << ")";
  p.printOptionalAttrDict(op->getAttrs());
  p << " : " << op.getOperandTypes();
}

static LogicalResult verify(BatchNormOp op) {
  return success();
}

//===----------------------------------------------------------------------===//
// ReluOp
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// PoolingOp
//===----------------------------------------------------------------------===//
static ParseResult parsePoolingOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<Type, 2> types;
  return failure(
      parser.parseOperandList(ops, OpAsmParser::Delimiter::Paren) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types) ||
      parser.resolveOperands(ops, types, parser.getNameLoc(), result.operands));
}

static void print(OpAsmPrinter &p, PoolingOp op) {
  p << op.getOperationName() << "(" << op.getOperand() << ")";
  p.printOptionalAttrDict(op->getAttrs());
}

static LogicalResult verify(PoolingOp op) {
  return success();
}

//===----------------------------------------------------------------------===//
// FlattenOp
//===----------------------------------------------------------------------===//
static ParseResult parseFlattenOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<Type, 2> types;
  return failure(
      parser.parseOperandList(ops, OpAsmParser::Delimiter::Paren) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types) ||
      parser.resolveOperands(ops, types, parser.getNameLoc(), result.operands));
}

static void print(OpAsmPrinter &p, FlattenOp op) {
  p << op.getOperationName() << "(" << op.getOperand() << ")";
  p.printOptionalAttrDict(op->getAttrs());
}

static LogicalResult verify(FlattenOp op) {
  return success();
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// MultiBroadcastOp
//===----------------------------------------------------------------------===//
static ParseResult parseMultiBroadcastOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<Type, 2> types;
  return failure(
      parser.parseOperandList(ops, OpAsmParser::Delimiter::Paren) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types) ||
      parser.resolveOperands(ops, types, parser.getNameLoc(), result.operands));
}

static void print(OpAsmPrinter &p, MultiBroadcastOp op) {
  p << op.getOperationName() << "(" << op.getOperand() << ")";
  p.printOptionalAttrDict(op->getAttrs());
}

static LogicalResult verify(MultiBroadcastOp op) {
  return success();
}

//===----------------------------------------------------------------------===//
// DotOp
//===----------------------------------------------------------------------===//
static ParseResult parseDotOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<Type, 2> types;
  return failure(
      parser.parseOperandList(ops, OpAsmParser::Delimiter::Paren) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types) ||
      parser.resolveOperands(ops, types, parser.getNameLoc(), result.operands));
}

static void print(OpAsmPrinter &p, DotOp op) {
  p << op.getOperationName() << "(" << op.getOperands() << ")";
  p.printOptionalAttrDict(op->getAttrs());
  p << " : " << op.getOperandTypes();
}

static LogicalResult verify(DotOp op) {
  return success();
}

#define GET_OP_CLASSES
#include "mlir/Dialect/MIGraphX/MIGraphXOps.cpp.inc"
