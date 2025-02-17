//===- InlineElementals.cpp - Inline chained hlfir.elemental ops ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Chained elemental operations like a + b + c can inline the first elemental
// at the hlfir.apply in the body of the second one (as described in
// docs/HighLevelFIR.md). This has to be done in a pass rather than in lowering
// so that it happens after the HLFIR intrinsic simplification pass.
//===----------------------------------------------------------------------===//

#include "flang/Optimizer/Builder/FIRBuilder.h"
#include "flang/Optimizer/Builder/HLFIRTools.h"
#include "flang/Optimizer/Dialect/Support/FIRContext.h"
#include "flang/Optimizer/Dialect/Support/KindMapping.h"
#include "flang/Optimizer/HLFIR/HLFIROps.h"
#include "flang/Optimizer/HLFIR/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/TypeSwitch.h"
#include <iterator>

namespace hlfir {
#define GEN_PASS_DEF_INLINEELEMENTALS
#include "flang/Optimizer/HLFIR/Passes.h.inc"
} // namespace hlfir

/// If the elemental has only two uses and those two are an apply operation and
/// a destory operation, return those two, otherwise return {}
static std::optional<std::pair<hlfir::ApplyOp, hlfir::DestroyOp>>
getTwoUses(hlfir::ElementalOp elemental) {
  mlir::Operation::user_range users = elemental->getUsers();
  // don't inline anything with more than one use (plus hfir.destroy)
  if (std::distance(users.begin(), users.end()) != 2) {
    return std::nullopt;
  }

  hlfir::ApplyOp apply;
  hlfir::DestroyOp destroy;
  for (mlir::Operation *user : users)
    mlir::TypeSwitch<mlir::Operation *, void>(user)
        .Case([&](hlfir::ApplyOp op) { apply = op; })
        .Case([&](hlfir::DestroyOp op) { destroy = op; });

  if (!apply || !destroy)
    return std::nullopt;
  return std::pair{apply, destroy};
}

namespace {
class InlineElementalConversion
    : public mlir::OpRewritePattern<hlfir::ElementalOp> {
public:
  using mlir::OpRewritePattern<hlfir::ElementalOp>::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(hlfir::ElementalOp elemental,
                  mlir::PatternRewriter &rewriter) const override {
    std::optional<std::pair<hlfir::ApplyOp, hlfir::DestroyOp>> maybeTuple =
        getTwoUses(elemental);
    if (!maybeTuple) {
      return rewriter.notifyMatchFailure(elemental.getLoc(),
                                         [](mlir::Diagnostic &) {});
    }
    auto [apply, destroy] = *maybeTuple;

    assert(elemental.getRegion().hasOneBlock() &&
           "expect elemental region to have one block");

    fir::FirOpBuilder builder{rewriter,
                              fir::KindMapping{rewriter.getContext()}};
    builder.setInsertionPointAfter(apply);
    hlfir::YieldElementOp yield = hlfir::inlineElementalOp(
        elemental.getLoc(), builder, elemental, apply.getIndices());

    // remove the old elemental and all of the bookkeeping
    rewriter.replaceAllUsesWith(apply.getResult(), yield.getElementValue());
    rewriter.eraseOp(yield);
    rewriter.eraseOp(apply);
    rewriter.eraseOp(destroy);
    rewriter.eraseOp(elemental);

    return mlir::success();
  }
};

class InlineElementalsPass
    : public hlfir::impl::InlineElementalsBase<InlineElementalsPass> {
public:
  void runOnOperation() override {
    mlir::func::FuncOp func = getOperation();
    mlir::MLIRContext *context = &getContext();

    mlir::GreedyRewriteConfig config;
    // Prevent the pattern driver from merging blocks.
    config.enableRegionSimplification = false;

    mlir::RewritePatternSet patterns(context);
    patterns.insert<InlineElementalConversion>(context);

    if (mlir::failed(mlir::applyPatternsAndFoldGreedily(
            func, std::move(patterns), config))) {
      mlir::emitError(func->getLoc(), "failure in HLFIR elemental inlining");
      signalPassFailure();
    }
  }
};
} // namespace

std::unique_ptr<mlir::Pass> hlfir::createInlineElementalsPass() {
  return std::make_unique<InlineElementalsPass>();
}
