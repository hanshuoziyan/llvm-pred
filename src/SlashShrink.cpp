#include "Resolver.h"
#include "SlashShrink.h"

#include <stdlib.h>
#include <unordered_set>

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Support/CommandLine.h>

#include <ValueProfiling.h>

#include "debug.h"

using namespace std;
using namespace lle;
using namespace llvm;

cl::opt<bool> markd("Mark", cl::desc("Enable Mark some code on IR"));
cl::opt<bool> ExecuteTrap("ExecutePath-Trap", cl::desc("Trap Execute Path to"
         "get know how IR Executed, Implement based on value-profiling"));/*
         may be duplicated with EdgeProfiling's implement effect, need be
         delete*/

StringRef MarkPreserve::MarkNode = "lle.mark";

char SlashShrink::ID = 0;

static RegisterPass<SlashShrink> X("Shrink", "Slash and Shrink Code to make a minicore program");

bool MarkPreserve::enabled()
{
   return ::markd;
}

void MarkPreserve::mark(Instruction* Inst, StringRef origin)
{
   if(!Inst) return;
   if(is_marked(Inst)) return;

   LLVMContext& C = Inst->getContext();
   MDNode* N = MDNode::get(C, MDString::get(C, origin));
   Inst->setMetadata(MarkNode, N);
}

list<Value*> MarkPreserve::mark_all(Value* V, ResolverBase& R, StringRef origin)
{
   list<Value*> empty;
   if(!V) return empty;
   Instruction* I = NULL;
   if(ConstantExpr* CE = dyn_cast<ConstantExpr>(V))
      I = CE->getAsInstruction();
   else I = dyn_cast<Instruction>(V);
   if(!I) return empty;

   mark(I, origin);
   ResolveResult Res = R.resolve(I, [origin](Value* V){
         if(Instruction* Inst = dyn_cast<Instruction>(V))
            mark(Inst, origin);
         });

   return get<1>(Res);
}

bool SlashShrink::runOnFunction(Function &F)
{
   LLVMContext& C = F.getContext();
   int ShrinkLevel = atoi(getenv("SHRINK_LEVEL")?:"1");
   runtime_assert(ShrinkLevel>=0 && ShrinkLevel <=3);
   Constant* Zero = ConstantInt::get(Type::getInt32Ty(C), 0);
   // mask all br inst to keep structure
   for(auto BB = F.begin(), E = F.end(); BB != E; ++BB){
      list<Value*> unsolved, left;
      if(ExecuteTrap){
         Instruction* Prof = ValueProfiler::insertValueTrap(
               Zero, BB->getTerminator());
         MarkPreserve::mark(Prof);
      }

      unsolved = MarkPreserve::mark_all<UseOnlyResolve>(BB->getTerminator(), "terminal");
      for(auto I : unsolved){
         MarkPreserve::mark_all<NoResolve>(I, "terminal");
      }

      for(auto I = BB->begin(), E = BB->end(); I != E; ++I){
         if(StoreInst* SI = dyn_cast<StoreInst>(I)){
            Instruction* LHS = dyn_cast<Instruction>(SI->getOperand(0));
            Instruction* RHS = dyn_cast<Instruction>(SI->getOperand(1));
            if(LHS && RHS && MarkPreserve::is_marked(LHS) && MarkPreserve::is_marked(RHS))
               MarkPreserve::mark(SI, "store");
         }

         if(CallInst* CI = dyn_cast<CallInst>(I)){
            Function* Func = CI->getCalledFunction();
            if(!Func) continue;
            if(Func->empty()) continue; /* a func's body is empty, means it is
                                           not a native function */

            MarkPreserve::mark_all<NoResolve>(CI, "callgraph");
         }
      }
   }

   for(auto BB = F.begin(), E = F.end(); BB !=E; ++BB){
      for(auto I = BB->begin(), E = BB->end(); I != E; ++I){
         if(MarkPreserve::is_marked(I))
            MarkPreserve::mark_all<NoResolve>(I, "closure");
      }
   }

   if(ShrinkLevel == 0) return false;

   if(F.getName() == "main") return false; /* some initial and import code are
      in main function. so we don't shrink it. this is triggy. and the best way
      is to automatic indentify which a function or a part of function is
      important*/

   for(auto BB = F.begin(), E = F.end(); BB != E; ++BB){
      auto I = BB->begin();
      while(I != BB->end()){
         if(!MarkPreserve::is_marked(I)){
            for(uint i=0;i<I->getNumOperands();++i)
               I->setOperand(i, NULL); /* destroy instruction need clean holds
                                          reference */
            (I++)->removeFromParent(); /* use erase from would cause crash let
                                          it freed by Context */
         }else
            ++I;
      }
   }

   return true;
}
