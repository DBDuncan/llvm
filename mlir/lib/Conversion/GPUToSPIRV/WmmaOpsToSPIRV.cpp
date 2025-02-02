//===------ WmmaOpsToSPIRV.cpp - WMMA LD/ST/Compute to SPIRV lowering -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions of patterns to lower GPU Subgroup MMA ops to
// SPIRV Cooperative Matrix ops.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/GPUToSPIRV/GPUToSPIRV.h"
#include "mlir/Conversion/GPUToSPIRV/GPUToSPIRVPass.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVAttributes.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVEnums.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVOps.h"
#include "mlir/Dialect/SPIRV/IR/TargetAndABI.h"
#include "mlir/Dialect/SPIRV/Transforms/SPIRVConversion.h"
#include "mlir/IR/TypeUtilities.h"

namespace mlir::nv {
namespace {

/// Creates a SPIR-V op to replace the given GPU subgroup mma elementwise op
/// when the elementwise op directly supports with cooperative matrix type.
/// Returns false if cannot.
///
/// See SPV_NV_cooperative_matrix for supported elementwise ops.
static bool createElementwiseOp(ConversionPatternRewriter &builder,
                                gpu::SubgroupMmaElementwiseOp op,
                                spirv::CooperativeMatrixNVType coopType,
                                ValueRange operands) {
  switch (op.getOpType()) {
  case gpu::MMAElementwiseOp::ADDF:
    builder.replaceOpWithNewOp<spirv::FAddOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::ADDI:
    builder.replaceOpWithNewOp<spirv::IAddOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::SUBF:
    builder.replaceOpWithNewOp<spirv::FSubOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::SUBI:
    builder.replaceOpWithNewOp<spirv::ISubOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::DIVF:
    builder.replaceOpWithNewOp<spirv::FDivOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::DIVS:
    builder.replaceOpWithNewOp<spirv::SDivOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::DIVU:
    builder.replaceOpWithNewOp<spirv::UDivOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::NEGATEF:
    builder.replaceOpWithNewOp<spirv::FNegateOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::NEGATES:
    builder.replaceOpWithNewOp<spirv::SNegateOp>(op, coopType, operands);
    return true;
  case gpu::MMAElementwiseOp::EXTF:
    builder.replaceOpWithNewOp<spirv::FConvertOp>(op, coopType, operands);
    return true;
  default:
    break;
  }
  return false;
}

/// Converts the GPU MMA loadOp to NVCooperativeMatrixLoad op in the SPIRV
/// dialect.
struct WmmaLoadOpToSPIRVLowering final
    : OpConversionPattern<gpu::SubgroupMmaLoadMatrixOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaLoadMatrixOp subgroupMmaLoadMatrixOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = subgroupMmaLoadMatrixOp->getLoc();
    gpu::MMAMatrixType retType =
        cast<gpu::MMAMatrixType>(subgroupMmaLoadMatrixOp.getRes().getType());
    auto memrefType =
        cast<MemRefType>(subgroupMmaLoadMatrixOp.getSrcMemref().getType());
    Value bufferPtr = spirv::getElementPtr(
        *getTypeConverter<const SPIRVTypeConverter>(), memrefType,
        adaptor.getSrcMemref(), adaptor.getIndices(), loc, rewriter);
    auto coopType = convertMMAToSPIRVCoopMatrixNVType(retType);
    int64_t stride = subgroupMmaLoadMatrixOp.getLeadDimension().getSExtValue();
    auto i32Type = rewriter.getI32Type();
    auto strideValue = rewriter.create<spirv::ConstantOp>(
        loc, i32Type, IntegerAttr::get(i32Type, stride));
    bool isColMajor = static_cast<bool>(subgroupMmaLoadMatrixOp.getTranspose());
    auto columnMajor = rewriter.create<spirv::ConstantOp>(
        loc, rewriter.getI1Type(), rewriter.getBoolAttr(isColMajor));
    rewriter.replaceOpWithNewOp<spirv::NVCooperativeMatrixLoadOp>(
        subgroupMmaLoadMatrixOp, coopType, bufferPtr, strideValue, columnMajor,
        spirv::MemoryAccessAttr());
    return success();
  }
};

/// Converts the GPU MMA StoreOp to NVCooperativeMatrixStore op in the SPIRV
/// dialect.
struct WmmaStoreOpToSPIRVLowering final
    : OpConversionPattern<gpu::SubgroupMmaStoreMatrixOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaStoreMatrixOp subgroupMmaStoreMatrixOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = subgroupMmaStoreMatrixOp->getLoc();
    auto memrefType =
        cast<MemRefType>(subgroupMmaStoreMatrixOp.getDstMemref().getType());
    Value bufferPtr = spirv::getElementPtr(
        *getTypeConverter<const SPIRVTypeConverter>(), memrefType,
        adaptor.getDstMemref(), adaptor.getIndices(), loc, rewriter);
    int64_t stride = subgroupMmaStoreMatrixOp.getLeadDimension().getSExtValue();
    auto i32Type = rewriter.getI32Type();
    auto strideValue = rewriter.create<spirv::ConstantOp>(
        loc, i32Type, IntegerAttr::get(i32Type, stride));
    bool useColMajor =
        static_cast<bool>(subgroupMmaStoreMatrixOp.getTranspose());
    auto columnMajor = rewriter.create<spirv::ConstantOp>(
        loc, rewriter.getI1Type(), rewriter.getBoolAttr(useColMajor));
    rewriter.replaceOpWithNewOp<spirv::NVCooperativeMatrixStoreOp>(
        subgroupMmaStoreMatrixOp, bufferPtr, adaptor.getSrc(), strideValue,
        columnMajor, spirv::MemoryAccessAttr());
    return success();
  }
};

/// Converts GPU MMA Compute to
/// NVCooperativeMatrixMulAdd op in the SPIRV dialect.
struct WmmaMmaOpToSPIRVLowering final
    : OpConversionPattern<gpu::SubgroupMmaComputeOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaComputeOp subgroupMmaComputeOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<spirv::NVCooperativeMatrixMulAddOp>(
        subgroupMmaComputeOp, adaptor.getOpC().getType(), adaptor.getOpA(),
        adaptor.getOpB(), adaptor.getOpC());
    return success();
  }
};

/// Converts GPU MMA ConstantMatrixOp to constant SPIR-V NV cooperative matrix
/// ops.
struct WmmaConstantOpToSPIRVLowering final
    : OpConversionPattern<gpu::SubgroupMmaConstantMatrixOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaConstantMatrixOp subgroupMmaConstantMatrixOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value cst = adaptor.getOperands()[0];
    auto coopType = convertMMAToSPIRVCoopMatrixNVType(
        cast<gpu::MMAMatrixType>(subgroupMmaConstantMatrixOp.getType()));
    rewriter.replaceOpWithNewOp<spirv::CompositeConstructOp>(
        subgroupMmaConstantMatrixOp, coopType, cst);
    return success();
  }
};

/// Converts elementwise ops to SPIR-V cooperative matrix elementwise ops for
/// the default case.
struct WmmaElementwiseOpToSPIRVDefaultLowering final
    : OpConversionPattern<gpu::SubgroupMmaElementwiseOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaElementwiseOp elementwiseOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // All operands should be of cooperative matrix types.
    for (Value operand : adaptor.getOperands()) {
      if (!isa<spirv::CooperativeMatrixNVType>(operand.getType()))
        return failure();
    }
    auto coopType = convertMMAToSPIRVCoopMatrixNVType(
        cast<gpu::MMAMatrixType>(elementwiseOp.getType()));
    return success(createElementwiseOp(rewriter, elementwiseOp, coopType,
                                       adaptor.getOperands()));
  }
};

/// Converts elementwise ops to SPIR-V cooperative matrix elementwise ops for
/// matrix times scalar case.
struct WmmaElementwiseOpToSPIRVScalarMulLowering final
    : OpConversionPattern<gpu::SubgroupMmaElementwiseOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(gpu::SubgroupMmaElementwiseOp elementwiseOp,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (adaptor.getOperands().size() != 2)
      return failure();
    // All operands should be of cooperative matrix types.
    for (Value operand : adaptor.getOperands()) {
      if (!isa<spirv::CooperativeMatrixNVType>(operand.getType()))
        return failure();
    }

    if (elementwiseOp.getOpType() != gpu::MMAElementwiseOp::MULF)
      return failure();

    // Use the original operands to check whether one of the operands is a splat
    // scalar value.
    Value lhs = elementwiseOp.getOperands().front();
    Value rhs = elementwiseOp.getOperands().back();
    Value splat = nullptr;
    Value matrix = nullptr;
    if (lhs.getDefiningOp<gpu::SubgroupMmaConstantMatrixOp>()) {
      splat = adaptor.getOperands().front();
      matrix = adaptor.getOperands().back();
    } else if (rhs.getDefiningOp<gpu::SubgroupMmaConstantMatrixOp>()) {
      matrix = adaptor.getOperands().front();
      splat = adaptor.getOperands().back();
    }
    if (!splat || !matrix)
      return failure();

    // Constant MMA matrix ops are converted to spirv.CompositeConstruct ops.
    Value scalar = nullptr;
    auto cc = splat.getDefiningOp<spirv::CompositeConstructOp>();
    if (!cc)
      return failure();
    assert(cc.getConstituents().size() == 1);
    scalar = cc.getConstituents().front();

    auto coopType = convertMMAToSPIRVCoopMatrixNVType(
        cast<gpu::MMAMatrixType>(elementwiseOp.getType()));
    rewriter.replaceOpWithNewOp<spirv::MatrixTimesScalarOp>(
        elementwiseOp, coopType, ValueRange{matrix, scalar});
    return success();
  }
};

} // namespace
} // namespace mlir::nv

mlir::spirv::CooperativeMatrixNVType
mlir::convertMMAToSPIRVCoopMatrixNVType(gpu::MMAMatrixType type) {
  ArrayRef<int64_t> retTypeShape = type.getShape();
  Type elementType = type.getElementType();
  return spirv::CooperativeMatrixNVType::get(
      elementType, spirv::Scope::Subgroup, retTypeShape[0], retTypeShape[1]);
}

void mlir::populateGpuWMMAToSPIRVCoopMatrixNVConversionPatterns(
    SPIRVTypeConverter &converter, RewritePatternSet &patterns) {
  using namespace mlir;
  MLIRContext *context = patterns.getContext();
  patterns
      .add<nv::WmmaLoadOpToSPIRVLowering, nv::WmmaMmaOpToSPIRVLowering,
           nv::WmmaStoreOpToSPIRVLowering, nv::WmmaConstantOpToSPIRVLowering,
           nv::WmmaElementwiseOpToSPIRVDefaultLowering>(converter, context);
  // Give the following patterns higher benefit to prevail over the default one.
  patterns.add<nv::WmmaElementwiseOpToSPIRVScalarMulLowering>(converter,
                                                              context,
                                                              /*benefit=*/2);
}
