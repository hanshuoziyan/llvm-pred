#include "BranchProbabilityPosterior.h"

using namespace llvm;
using namespace lle;

static double ignoreMissing(double w)
{
   if (w == ProfileInfo::MissingValue) return 0;
   return w;
}

char BranchProbabilityPosterior::ID = 0;
void BranchProbabilityPosterior::getAnalysisUsage(AnalysisUsage &AU) const
{
   //AU.addRequired<lle::BranchProbabilityPosterior>();
   AU.addRequired<ProfileInfo>();
}
bool BranchProbabilityPosterior::runOnModule(llvm::Module& M)
{
   PI = &getAnalysis<ProfileInfo>();
  // errs()<<"Hello world!"<<"\n";
  // std::vector<std::pair<llvm::Function*, double> >FunctionCounts;
  // std::vector<std::pair<llvm::BasicBlock*, double> >BasicBlockCounts;
  // for (Module::iterator FI = M.begin(),FE = M.end();FI != FE; ++FI)
  // {
  //    if (FI->isDeclaration()) continue;
  //    //errs()<<PI->getExecutionCount(&(FI->getEntryBlock()))<<"<<<<<<<<<<<<<<<<<<<<<<<<\n";
  //    //this->getEdgeProbability(&(FI->getEntryBlock()), &(FI->getEntryBlock()));
  //    double w = ignoreMissing(PI->getExecutionCount(FI));
  //    FunctionCounts.push_back(std::make_pair(FI,w));
  //    Function::iterator BTmp = FI->begin(); 
  //    for (Function::iterator BB = FI->begin(), BBE = FI->end(); BB != BBE; ++BB)
  //    {
  //       //errs()<<"==========================";
  //       double w = ignoreMissing(PI->getExecutionCount(BB));
  //       BasicBlockCounts.push_back(std::make_pair(BB, w));
  //       //errs()<<w<<"\n";
  //       errs()<<PI->getEdgeWeight(PI->getEdge(BB,BTmp))<<"\n";
  //       BTmp = BB;
  //    }
  // }
  // errs()<<BasicBlockCounts.size()<<"\n";
  // errs()<<FunctionCounts.size()<<"\n";
   return false;
}
BranchProbability BranchProbabilityPosterior::getEdgeProbability(const BasicBlock* Src, const BasicBlock* Dst) const
{
   double srcCount = PI->getExecutionCount(Src);
   double edgeCount = PI->getEdgeWeight(PI->getEdge(Src, Dst));
   //errs()<<"================================="<<srcCount<<"\n";
   //errs()<<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"<<edgeCount<<"\n";
   //if(srcCount <= dstCount)
   //   return BranchProbability(1,1);
   return BranchProbability(edgeCount,srcCount);

}
