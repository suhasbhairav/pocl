// LLVM function pass that adds implicit barriers to branches where it sees
// beneficial (and legal).
// 
// Copyright (c) 2013 Pekka Jääskeläinen / TUT
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "config.h"
#include "ImplicitConditionalBarriers.h"
#include "Barrier.h"
#include "BarrierBlock.h"
#include "Workgroup.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#if (defined LLVM_3_1 or defined LLVM_3_2)
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#else
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#endif

#include <iostream>

//#define DEBUG_COND_BARRIERS

using namespace llvm;
using namespace pocl;

namespace {
  static
  RegisterPass<ImplicitConditionalBarriers> X("implicit-cond-barriers",
                                              "Adds implicit barriers to branches.");
}

char ImplicitConditionalBarriers::ID = 0;

void
ImplicitConditionalBarriers::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<PostDominatorTree>();
  AU.addPreserved<PostDominatorTree>();
  AU.addRequired<DominatorTree>();
  AU.addPreserved<DominatorTree>();

}

/**
 * Finds a predecessor that does not come from a back edge.
 *
 * This is used to include loops in the conditional parallel region.
 */
BasicBlock*
ImplicitConditionalBarriers::firstNonBackedgePredecessor(
    llvm::BasicBlock *bb) {

    DominatorTree *DT = &getAnalysis<DominatorTree>();

    pred_iterator I = pred_begin(bb), E = pred_end(bb);
    if (I == E) return NULL;
    while (DT->dominates(bb, *I) && I != E) ++I;
    if (I == E) return NULL;
    else return *I;
}

bool
ImplicitConditionalBarriers::runOnFunction(Function &F) {
{
  if (!Workgroup::isKernelToProcess(F))
    return false;
  
  PDT = &getAnalysis<PostDominatorTree>();

  typedef std::vector<BasicBlock*> BarrierBlockIndex;
  BarrierBlockIndex conditionalBarriers;
  for (Function::iterator i = F.begin(), e = F.end(); i != e; ++i) {
    BasicBlock *b = i;
    if (!Barrier::hasBarrier(b)) continue;

    // Unconditional barrier postdominates the entry node.
    if (PDT->dominates(b, &F.getEntryBlock())) continue;

#ifdef DEBUG_COND_BARRIERS
    std::cerr << "### found a conditional barrier";
    b->dump();
#endif
    conditionalBarriers.push_back(b);
  }

  if (conditionalBarriers.size() == 0) return false;

  bool changed = false;

  for (BarrierBlockIndex::const_iterator i = conditionalBarriers.begin();
       i != conditionalBarriers.end(); ++i) {
    BasicBlock *b = *i;
    // Trace upwards from the barrier until one encounters another
    // barrier or the split point that makes the barrier conditional. 
    // In case of the latter, add a new barrier to both branches of the split point. 

    // The split BB where to inject the barriers. The barriers will be
    // injected to the beginning of each of the blocks it branch to to
    // minimize the peeling effect.
    BasicBlock *pos = firstNonBackedgePredecessor(b);

    // If our BB post dominates the given block, we know it is not the
    // branching block that makes the barrier conditional.
    while (!isa<BarrierBlock>(pos) && PDT->dominates(b, pos)) {

#ifdef DEBUG_COND_BARRIERS
      std::cerr << "### looking at BB " << pos->getName().str() << std::endl;
#endif
      // Find the first edge that is not a loop edge so we include loops
      // inside the new parallel region.
      pos = firstNonBackedgePredecessor(pos);

      if (pos == b) break; // Traced across a loop edge, skip this case.
    }

    if (isa<BarrierBlock>(pos) || pos == b) continue;

    // Inject a barrier at the destinations of the branch BB and let 
    // the CanonicalizeBarrier to clean it up (split to separate BBs).

    // mri-q of parboil breaks in case injected at the beginning
    // TODO: investigate. It might related to the alloca-converted
    // PHIs. It has a loop that is autoconverted to a b-loop and the
    // conditional barrier is inserted after the loop short cut check.
    llvm::TerminatorInst *term = pos->getTerminator();
    for (unsigned i = 0; i < term->getNumSuccessors(); ++i) 
    {
        BasicBlock *succ = term->getSuccessor(i);    
        Instruction *where = succ->getFirstNonPHI();
        if (isa<Barrier>(where)) continue;
        Barrier::Create(where);
#ifdef DEBUG_COND_BARRIERS
        std::cerr << "### added an implicit barrier to the BB" << std::endl;
        succ->dump();
#endif
    }
    changed = true;
  }

  //F.dump();
  //F.viewCFG();

  return changed;
}

}


