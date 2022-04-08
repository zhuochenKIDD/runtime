// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iterator>
#include <utility>

#include "llvm/ADT/STLExtras.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/PatternMatch.h"
#include "tfrt/basic_kernels/opdefs/basic_kernels.h"
#include "tfrt/gpu/kernels/gpu_ops.h"
#include "tfrt/gpu/passes/passes.h"

namespace tfrt {
namespace gpu {

Value internal::GpuAsyncOpConversionGetStream(Operation *parent) {
  if (auto exec_op = dyn_cast_or_null<conversion::AsyncExecuteOp>(parent))
    return exec_op.getRegion().getArgument(1);
  return Value();
}
Value internal::GpuAsyncOpConversionGetChain(Operation *parent) {
  if (auto exec_op = dyn_cast_or_null<conversion::AsyncExecuteOp>(parent))
    return exec_op.getRegion().back().getTerminator()->getOperand(0);
  return Value();
}
void internal::GpuAsyncOpConversionSetChain(Value chain,
                                            PatternRewriter &rewriter) {
  Operation *terminator = chain.getParentRegion()->back().getTerminator();
  rewriter.updateRootInPlace(
      terminator, [&] { terminator->setOperands(ValueRange(chain)); });
}

namespace {

// Wraps consecutive legal ops within a block into a
// tfrt_gpu_conversion.async.execute op.
struct NestLegalOpsInConversionAsyncExecPattern
    : public OpRewritePattern<FuncOp> {
  NestLegalOpsInConversionAsyncExecPattern(MLIRContext *context,
                                           ConversionTarget &target)
      : OpRewritePattern(context), target(target) {}

 private:
  LogicalResult matchAndRewrite(FuncOp func_op,
                                PatternRewriter &rewriter) const override;
  LogicalResult matchAndRewriteBlock(Block *block,
                                     PatternRewriter &rewriter) const;
  ConversionTarget &target;
};

// Folds a memref.view of !tfrt_gpu.buffer with zero byte_shift.
struct FoldMemrefViewPattern : public OpConversionPattern<memref::ViewOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      memref::ViewOp view_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

// Folds a memref.reinterpret_cast of !tfrt_gpu.buffer with zero static offsets.
struct FoldMemrefReinterpretCastPattern
    : public OpConversionPattern<memref::ReinterpretCastOp> {
  using OpConversionPattern::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      memref::ReinterpretCastOp cast_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

template <typename OpTy>
struct RewriteMemrefAllocPattern : public OpConversionPattern<OpTy> {
  using OpAdaptor = typename OpConversionPattern<OpTy>::OpAdaptor;
  using OpConversionPattern<OpTy>::OpConversionPattern;

 private:
  LogicalResult matchAndRewrite(
      OpTy alloc_op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

struct RewriteMemrefDeallocPattern
    : public OpRewritePattern<memref::DeallocOp> {
  using OpRewritePattern::OpRewritePattern;

 private:
  LogicalResult matchAndRewrite(memref::DeallocOp dealloc_op,
                                PatternRewriter &rewriter) const override;
};

// Dummy pattern to trigger memref to !tfrt_gpu.buffer conversion.
template <typename OpTy>
struct ConvertOpTypesPattern : public OpConversionPattern<OpTy> {
  using OpAdaptor = typename OpConversionPattern<OpTy>::OpAdaptor;
  using OpConversionPattern<OpTy>::OpConversionPattern;
  using OpConversionPattern<OpTy>::typeConverter;

 private:
  LogicalResult matchAndRewrite(
      OpTy op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override;
};

}  // namespace

LogicalResult NestLegalOpsInConversionAsyncExecPattern::matchAndRewrite(
    FuncOp func_op, PatternRewriter &rewriter) const {
  rewriter.startRootUpdate(func_op);
  LogicalResult result = failure();
  func_op.walk([&](Block *block) {
    if (isa<conversion::AsyncExecuteOp>(block->getParentOp()))
      return WalkResult::skip();
    if (succeeded(matchAndRewriteBlock(block, rewriter)))
      result = success();  //
    return WalkResult::advance();
  });
  succeeded(result) ? rewriter.finalizeRootUpdate(func_op)
                    : rewriter.cancelRootUpdate(func_op);
  return result;
}

// Iterate over ops in block, and whenever we transition from a legal to an
// illegal op, wrap preceding legal ops in !tfrt_gpu_conversion.async.execute.
LogicalResult NestLegalOpsInConversionAsyncExecPattern::matchAndRewriteBlock(
    Block *block, PatternRewriter &rewriter) const {
  LogicalResult result = failure();
  Operation *legal_begin = nullptr;
  for (Operation *op : llvm::make_pointer_range(block->getOperations())) {
    if (target.isLegal(op)) {
      if (!legal_begin)  // Start of legal op sequence.
        legal_begin = op;
      continue;
    }
    if (!legal_begin)  // Continue in illegal op sequence.
      continue;

    rewriter.setInsertionPoint(legal_begin);
    auto loc = legal_begin->getLoc();
    auto *body = rewriter.create<conversion::AsyncExecuteOp>(loc).getBody();
    // Move sequence of legal ops into !tfrt_gpu_conversion.async.execute
    // body.
    body->getOperations().splice(body->begin(), op->getBlock()->getOperations(),
                                 legal_begin->getIterator(), op->getIterator());
    legal_begin = nullptr;  // Start of illegal op sequence.
    result = success();
  }
  return result;
}

unsigned GetTypeSizeBytes(const Type &type) {
  if (auto shaped_type = type.dyn_cast<ShapedType>()) {
    return GetTypeSizeBytes(shaped_type.getElementType()) *
           shaped_type.getNumElements();
  }

  if (type.isIntOrFloat()) return (type.getIntOrFloatBitWidth() + 7) / 8;

  if (auto complex_type = type.dyn_cast<mlir::ComplexType>())
    return GetTypeSizeBytes(complex_type.getElementType()) * 2;

  llvm_unreachable("unsupported type");
}

LogicalResult FoldMemrefViewPattern::matchAndRewrite(
    memref::ViewOp view_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (!adaptor.source().getType().isa<BufferType>())
    return rewriter.notifyMatchFailure(view_op, "expected BufferType source");
  if (!adaptor.sizes().empty())
    return rewriter.notifyMatchFailure(view_op, "expected no sizes");

  auto const_offset =
      adaptor.byte_shift().getDefiningOp<arith::ConstantIndexOp>();
  auto dst_size_bytes = GetTypeSizeBytes(view_op.getType());
  auto src_size_bytes = GetTypeSizeBytes(view_op.source().getType());
  if (const_offset && const_offset.value() == 0 &&
      src_size_bytes == dst_size_bytes) {
    rewriter.replaceOp(view_op, {adaptor.source()});
    return success();
  }
  auto loc = view_op->getLoc();
  auto offset = rewriter.create<UnrealizedConversionCastOp>(
      loc, rewriter.getIntegerType(64, false), adaptor.byte_shift());
  auto size = rewriter.create<compiler::ConstantUI64Op>(loc, dst_size_bytes);
  rewriter.replaceOpWithNewOp<MemViewOp>(view_op, adaptor.source(),
                                         offset.getResult(0), size);
  return success();
}

LogicalResult FoldMemrefReinterpretCastPattern::matchAndRewrite(
    memref::ReinterpretCastOp cast_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  if (!adaptor.source().getType().isa<BufferType>())
    return rewriter.notifyMatchFailure(cast_op, "expected BufferType source");
  if (!adaptor.offsets().empty() ||
      llvm::any_of(adaptor.static_offsets(), [](Attribute attribute) {
        return attribute.cast<IntegerAttr>().getInt() != 0;
      }))
    return rewriter.notifyMatchFailure(cast_op, "expected static zero offsets");
  rewriter.replaceOp(cast_op, {adaptor.source()});
  return success();
}

template <typename OpTy>
LogicalResult RewriteMemrefAllocPattern<OpTy>::matchAndRewrite(
    OpTy alloc_op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::gpu::AllocOp>(
      alloc_op, alloc_op.getType(),
      /*asyncDependencies=*/ValueRange(), /*dynamicSizes=*/ValueRange(),
      /*symbolOperands=*/ValueRange());
  return success();
}

LogicalResult RewriteMemrefDeallocPattern::matchAndRewrite(
    memref::DeallocOp dealloc_op, PatternRewriter &rewriter) const {
  rewriter.replaceOpWithNewOp<mlir::gpu::DeallocOp>(
      dealloc_op, /*asyncToken=*/Type(),
      /*asyncDependencies=*/ValueRange(), dealloc_op.memref());
  return success();
}

template <typename OpTy>
LogicalResult ConvertOpTypesPattern<OpTy>::matchAndRewrite(
    OpTy op, OpAdaptor adaptor, ConversionPatternRewriter &rewriter) const {
  SmallVector<Type, 4> result_types;
  if (failed(typeConverter->convertTypes(op->getResultTypes(), result_types)))
    return rewriter.notifyMatchFailure(op, "failed to convert result types");
  rewriter.replaceOpWithNewOp<OpTy>(op, result_types, adaptor.getOperands(),
                                    op->getAttrs());
  return success();
}

void populateGpuAsyncConversionPatterns(RewritePatternSet &patterns,
                                        TypeConverter &converter,
                                        ConversionTarget &target) {
  // Wrap tfrt.call ops with no results to provide chain and stream that may be
  // added to the callee's arguments. tfrt.call with results are not wrapped
  // because tfrt_gpu_conversion.async.execute does not return any results
  // beyond the optional gpu.async.token. This adds a chain and stream argument
  // to all functions containing such tfrt.call. If this turns out to be a
  // problem, we need to analyze the call graph and only wrap calls that execute
  // ops implementing the AsyncOpInterface.
  target.addDynamicallyLegalOp<tfrt::compiler::CallOp>(
      [](Operation *op) { return op->getNumResults() == 0; });

  populateFunctionOpInterfaceTypeConversionPattern<FuncOp>(patterns, converter);
  populateCallOpTypeConversionPattern(patterns, converter);
  populateReturnOpTypeConversionPattern(patterns, converter);
  patterns.add<ConvertOpTypesPattern<compiler::CallOp>,
               ConvertOpTypesPattern<compiler::ReturnOp>>(
      converter, patterns.getContext());

  patterns.add<NestLegalOpsInConversionAsyncExecPattern>(patterns.getContext(),
                                                         target);

  patterns.add<FoldMemrefViewPattern, FoldMemrefReinterpretCastPattern,
               RewriteMemrefAllocPattern<memref::AllocOp>,
               RewriteMemrefAllocPattern<memref::AllocaOp>>(
      converter, patterns.getContext());
  patterns.add<RewriteMemrefDeallocPattern>(patterns.getContext());
}

}  // namespace gpu
}  // namespace tfrt
