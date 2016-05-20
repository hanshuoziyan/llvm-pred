#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Support/Format.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/DenseMap.h>
#include <ProfileInfo.h>
#include <ProfileInfoLoader.h>




namespace lle {
   class BranchProbabilityPosterior;
};

class lle::BranchProbabilityPosterior:public llvm::ModulePass
{
   public:
      static char ID;
      llvm::ProfileInfoLoader &PIL;
      llvm::ProfileInfo* PI;
      explicit BranchProbabilityPosterior(llvm::ProfileInfoLoader& _PIL):llvm::ModulePass(ID),PIL(_PIL){}
      bool runOnModule(llvm::Module& M);
      llvm::BranchProbability getEdgeProbability(const llvm::BasicBlock *Src,const llvm::BasicBlock *Dst) const;
      llvm::BlockFrequency getBbCount(const llvm::BasicBlock *Src) const;
      void getAnalysisUsage(llvm::AnalysisUsage& AU) const override;
};
