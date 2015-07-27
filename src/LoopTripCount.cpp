#define DEBUG_TYPE "loop-cycle"
#include "preheader.h"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <map>
#include <vector>
#include <algorithm>
#include <llvm/IR/Type.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Analysis/ScalarEvolutionExpander.h>

#include "util.h"
#include "config.h"
#include "LoopTripCount.h"
#include "Resolver.h"
#include "ddg.h"
#include "debug.h"

using namespace std;
using namespace lle;
using namespace llvm;

char LoopTripCount::ID = 0;

static RegisterPass<LoopTripCount> X("Loop-Trip-Count","Generate and insert loop trip count pass", false, false);
static unsigned LoopCount;
static unsigned UnfoundCount;

//find start value fron induction variable
static Value* tryFindStart(PHINode* IND,Loop* L,BasicBlock*& StartBB)
{
	if(L->getLoopPredecessor()){ 
		StartBB = L->getLoopPredecessor();
		return IND->getIncomingValueForBlock(StartBB);
	} else {
		Value* start = NULL;
		for(int I = 0,E = IND->getNumIncomingValues();I!=E;++I){
			if(L->contains(IND->getIncomingBlock(I))) continue;
			//if there are many entries, assume they are all equal
			//****??? should do castoff ???******
			if(start && start != IND->getIncomingValue(I)) return NULL;
			start = IND->getIncomingValue(I);
			StartBB = IND->getIncomingBlock(I);
		}
		return start;
	}
}


void LoopTripCount::getAnalysisUsage(llvm::AnalysisUsage & AU) const
{
	AU.addRequired<LoopInfo>();
   AU.addRequired<ScalarEvolution>();
}

LoopTripCount::AnalysisedLoop LoopTripCount::analysis(Loop* L)
{
#ifdef TC_USE_SCEV
   auto& SE = getAnalysis<ScalarEvolution>();
   const SCEV* LoopInfo = SE.getBackedgeTakenCount(L);
   Value* TC = NULL;
   /*BasicBlock* Preheader = L->getLoopPreheader();
   if(Preheader){
      string HName = (L->getHeader()->getName()+".tc").str();
      auto Found = find_if(Preheader->begin(),Preheader->end(), [HName](Instruction& I){
            return I.getName()==HName;
            });
      if(Found != Preheader->end()) TC = &*Found;
   }*/
   if(!isa<SCEVCouldNotCompute>(LoopInfo)){
      AnalysisedLoop ret = {0};
      ret.userdata = const_cast<void*>((const void*)LoopInfo);
      ret.TripCount = TC;
      return ret;
   }
#endif
	Value* start = NULL;
	Value* ind = NULL;
	Value* end = NULL;
	ConstantInt* step = NULL,*PrevStep = NULL;/*only used if next is phi node*/
   ResolveEngine RE;
   RE.addRule(RE.base_rule);
   RE.addRule(RE.useonly_rule);

	// inspired from Loop::getCanonicalInductionVariable
	BasicBlock *H = L->getHeader();
	BasicBlock* LoopPred = L->getLoopPredecessor();
	BasicBlock* startBB = NULL;//which basicblock stores start value
	int OneStep = 0;// the extra add or plus step for calc

   AssertThrow(LoopPred, not_found("Require Loop has a Pred"));
	/** whats difference on use of predecessor and preheader??*/
	AssertThrow(L->getLoopLatch(), not_found("need loop simplify form"));

	BasicBlock* TE = NULL;//True Exit
	SmallVector<BasicBlock*,4> Exits;
	L->getExitingBlocks(Exits);

   if(Exits.size()==1) 
      TE = Exits.front();
	else{
		if(std::find(Exits.begin(),Exits.end(),L->getLoopLatch())!=Exits.end()) TE = L->getLoopLatch();
		else{
			SmallVector<llvm::Loop::Edge,4> ExitEdges;
			L->getExitEdges(ExitEdges);
			//stl 用法,先把所有满足条件的元素(出口的结束符是不可到达)移动到数组的末尾,再统一删除
			ExitEdges.erase(std::remove_if(ExitEdges.begin(), ExitEdges.end(), 
						[](llvm::Loop::Edge& I){
						return isa<UnreachableInst>(I.second->getTerminator());
						}), ExitEdges.end());
			if(ExitEdges.size()==1) TE = const_cast<BasicBlock*>(ExitEdges.front().first);
		}
	}
	//process true exit
	AssertThrow(TE, not_found("need have a true exit"));

	Instruction* IndOrNext = NULL;
   //终止块的终止指令：分情况讨论branchinst,switchinst;
   //跳转指令br bool a1,a2;condition<-->bool
	if(isa<BranchInst>(TE->getTerminator())){
		const BranchInst* EBR = cast<BranchInst>(TE->getTerminator());
		AssertThrow(EBR->isConditional(), not_found("end branch is not conditional"));
		ICmpInst* EC = dyn_cast<ICmpInst>(EBR->getCondition());
      AssertThrow(EC, not_found("end condition is not icmp"));
      
      int flag = 1;
      if(L->isLoopInvariant(EC->getOperand(1))){
         end = EC->getOperand(1);
         flag = 1;
      }else if(auto LI_ptr = dyn_cast<LoadInst>(EC->getOperand(1))){
         if(L->isLoopInvariant(LI_ptr->getOperand(0))){
            end = LI_ptr->getOperand(0);
            flag = 1;
         }
      }else if(L->isLoopInvariant(EC->getOperand(0))){
         end = EC->getOperand(0);
         flag = 0;
      }else if(auto LI_ptr0 = dyn_cast<LoadInst>(EC->getOperand(0))){
         if(L->isLoopInvariant(LI_ptr0->getOperand(0))){
            end = LI_ptr0->getOperand(0);
            flag = 0;
         }
      }else{
		    end = EC->getOperand(1);
          flag = 1;
      }
      IndOrNext = dyn_cast<Instruction>(castoff(EC->getOperand(1-flag)));

      int a = -1;
      if(!L->contains(EBR->getSuccessor(0)))
            a = 0;
      else if(!L->contains(EBR->getSuccessor(1)))
            a = 1;
      AssertThrow((a != -1), not_found(dbg()<<"abnormal exit"<<*EBR));

	/*	if(EC->getPredicate() == EC->ICMP_SGT){
         //终止块的终止指令---->跳出执行循环外的指令
         OneStep += 1;
      } else if(EC->getPredicate() == EC->ICMP_EQ) {
         a = 0;
         AssertThrow(!L->contains(EBR->getSuccessor(0)), not_found(dbg()<<"abnormal exit with great than:"<<*EBR));
      } else if(EC->getPredicate() == EC->ICMP_SLT) {
         a = 1;
         AssertThrow(!L->contains(EBR->getSuccessor(1)), not_found(dbg()<<"abnormal exit with less than:"<<*EBR));
      } else {
         AssertThrow(0, not_found(dbg()<<"unknow combination of end condition:"<<*EC));
      }*/
      //patch______________________________
		//end = EC->getOperand(1-a);//去掉类型转化
    	}else if(isa<SwitchInst>(TE->getTerminator())){
		SwitchInst* ESW = const_cast<SwitchInst*>(cast<SwitchInst>(TE->getTerminator()));
		IndOrNext = dyn_cast<Instruction>(castoff(ESW->getCondition()));
		for(auto I = ESW->case_begin(),E = ESW->case_end();I!=E;++I){
			if(!L->contains(I.getCaseSuccessor())){
				AssertThrow(!end, not_found("shouldn't have two ends"));
				end = I.getCaseValue();
			}
		}
	}else{
		AssertThrow(0 ,not_found("unknow terminator type"));
	}
   string s;
   lle::coutValue(end, s);

   AssertThrow(IndOrNext, not_found(s));
	AssertThrow(L->isLoopInvariant(end), not_found(/*"end value should be loop invariant"*/s));//至此得END值

	Instruction* next = NULL;
	bool addfirst = false;//add before icmp ed

	DISABLE(errs()<<*IndOrNext<<"\n");
	if(isa<LoadInst>(IndOrNext)){
		//memory depend analysis
		Value* PSi = IndOrNext->getOperand(0);//point type Step.i
    //  if(!dyn_cast<Argument>(PSi)){
		int SICount[2] = {0};//store in predecessor count,store in loop body count

      Value* Store;
      RE.resolve(&IndOrNext->getOperandUse(0), RE.findStore(Store));
      if(Store && isa<StoreInst>(Store)){
         StoreInst* SI = cast<StoreInst>(Store);
         if(L->isLoopInvariant(SI->getValueOperand())){
            start = SI->getValueOperand();
            startBB = SI->getParent();
            // we always found the nearest storeinst
            SICount[0] = 1;
         }
      }

		for(auto I = PSi->user_begin(),E = PSi->user_end();I!=E;++I){
			StoreInst* SI = dyn_cast<StoreInst>(*I);
			if(SI==NULL || SI->getOperand(1) != PSi) continue;
         if(L->contains(SI)){
            Instruction* SI0 = dyn_cast<Instruction>(SI->getValueOperand());
            if(SI0 && SI0->getOpcode() == Instruction::Add){
               next = SI0;
               ++SICount[1];
            }
         }
		}

      AssertThrow(SICount[0]==1 && SICount[1]==1, 
            not_found(dbg() <<"should only have 1 store in/before loop:"
               <<SICount[1] <<"," <<SICount[0]<<*PSi));
     // }
     // else{
       //  start = PSi;
     // }
		ind = IndOrNext;
	}else{
		if(isa<PHINode>(IndOrNext)){
			PHINode* PHI = cast<PHINode>(IndOrNext);
			ind = IndOrNext;
			if(castoff(PHI->getIncomingValue(0)) == castoff(PHI->getIncomingValue(1)) && PHI->getParent() != H)
				ind = castoff(PHI->getIncomingValue(0));
			addfirst = false;
		}else if(IndOrNext->getOpcode() == Instruction::Add){
			next = IndOrNext;
			addfirst = true;
		}else{
			AssertThrow(0 , not_found("unknow how to analysis"));
		}

		for(auto I = H->begin();isa<PHINode>(I);++I){
			PHINode* P = cast<PHINode>(I);
			if(ind && P == ind){
				start = tryFindStart(P, L, startBB);
				next = dyn_cast<Instruction>(P->getIncomingValueForBlock(L->getLoopLatch()));
			}else if(next && P->getIncomingValueForBlock(L->getLoopLatch()) == next){
				start = tryFindStart(P, L, startBB);
				ind = P;
			}
		}
	}
   
   string s_ind;
   coutValue(ind, s_ind);
  // AssertThrow(ind, not_found("no ind"));
	AssertThrow(start , not_found(/*"couldn't find a start value"*/s_ind));

	//process non add later
	unsigned next_phi_idx = 0;
   AssertThrow(next, not_found("Next not found"));
	PHINode* next_phi = dyn_cast<PHINode>(next);
	do{
		if(next_phi) {
			next = dyn_cast<Instruction>(next_phi->getIncomingValue(next_phi_idx));
			AssertThrow(next, not_found("Next not found"));
			if(step&&PrevStep){
				Assert(step->getSExtValue() == PrevStep->getSExtValue(),"");
			}
			PrevStep = step;
		}
		//Assert(next->getOpcode() == Instruction::Add , "why induction increment is not Add");
      AssertThrow(next->getOpcode() == Instruction::Add, not_found("why induction increment is not add"));
		//Assert(next->getOperand(0) == ind ,"why induction increment is not add it self");
      AssertThrow(next->getOperand(0) == ind ,not_found("why induction increment is not add it self"));

		step = dyn_cast<ConstantInt>(castoff(next->getOperand(1)));
      AssertThrow(step, not_found(dbg() << "step is not a constant: " << *next->getOperand(1)))
	}while(next_phi && ++next_phi_idx<next_phi->getNumIncomingValues());

	if(addfirst) OneStep -= 1;
	if(step->isMinusOne()) OneStep*=-1;
	assert(OneStep<=1 && OneStep>=-1);
   return AnalysisedLoop{OneStep, start,step,end,ind};
}
Value* ScevToInst(const SCEV *scev_expr,llvm::Instruction *InsertPos){
   if(auto constant_scev = dyn_cast<SCEVConstant>(scev_expr)){
      return constant_scev->getValue();
   }
   if(auto value_scev = dyn_cast<SCEVUnknown>(scev_expr)){
      return value_scev->getValue();
   }
   IRBuilder<> Builder(InsertPos);
   Value* inst = NULL;
   if(auto mul_expr = dyn_cast<SCEVMulExpr>(scev_expr)){
      auto first_op = mul_expr->op_begin();
      Value* LHS = ScevToInst(*first_op,InsertPos);
      if(LHS == NULL)
         return NULL;
      for(auto O = mul_expr->op_begin()+1, E = mul_expr->op_end(); O != E; ++O){
         Value* RHS = ScevToInst(*O,InsertPos);
         if(RHS == NULL)
            return NULL;
         inst = Builder.CreateMul(LHS,RHS);
      }
   }else if(auto add_expr = dyn_cast<SCEVAddExpr>(scev_expr)){
      auto first_op = add_expr->op_begin();
      Value* LHS_add = ScevToInst(*first_op,InsertPos);
      if(LHS_add == NULL)
         return NULL;
      for(auto O_add = add_expr->op_begin()+1, E_add = add_expr->op_end(); O_add != E_add; ++O_add){
         Value* RHS_add = ScevToInst(*O_add,InsertPos);
         if(RHS_add == NULL)
            return NULL;
         inst = Builder.CreateAdd(LHS_add,RHS_add);
      }
   }else if(auto cast_expr = dyn_cast<SCEVCastExpr>(scev_expr)){
      return ScevToInst(cast_expr->getOperand(), InsertPos);
   }
  return inst;
}

Value* LoopTripCount::insertTripCount(AnalysisedLoop AL, StringRef HeaderName, Instruction* InsertPos)
{
#ifdef TC_USE_SCEV
   SCEV* scev_expr = (SCEV*)AL.userdata;
   if(scev_expr && !isa<SCEVCouldNotCompute>(scev_expr)){
      ScalarEvolution& SE = getAnalysis<ScalarEvolution>();
      SCEVExpander Expander(SE,"loop-trip-count");
      Value *inst = Expander.expandCodeFor(scev_expr,scev_expr->getType(),InsertPos);
      IRBuilder<> B(InsertPos);
      Type* I32Ty = B.getInt32Ty();
      if(inst->getType() != I32Ty){
         inst = B.CreateCast(CastInst::getCastOpcode(inst, false, I32Ty, false), inst, I32Ty);
      }
      if(inst != NULL){
         inst->setName(HeaderName+".tc");
      }
      return inst;
   }
#endif
	Value* RES = NULL;
   Value* start = AL.Start, *END = AL.End;
   if(!start || !END || !InsertPos || !AL.Step) return NULL;
   ConstantInt* Step = dyn_cast<ConstantInt>(AL.Step);
   int OneStep = AL.AdjustStep;
	//if there are no predecessor, we can insert code into start value basicblock
	IRBuilder<> Builder(InsertPos);
   Type* I32Ty = Builder.getInt32Ty();
	Assert(start->getType()->isIntegerTy() && END->getType()->isIntegerTy() , " why increment is not integer type");

#define AdjustType(v) ((v->getType() != I32Ty)?\
         Builder.CreateCast(CastInst::getCastOpcode(v, false, I32Ty, false), v, I32Ty):\
         v)
   // adjust type to int 32
   start = AdjustType(start);
   END = AdjustType(END);
   Step = dyn_cast<ConstantInt>(AdjustType(Step));
   AssertRuntime(Step, "");
#undef AdjustType

	if(Step->isMinusOne())
		RES = Builder.CreateSub(start,END);
	else//Step Couldn't be zero
		RES = Builder.CreateSub(END, start);
	RES = (OneStep==1)?Builder.CreateAdd(RES,Step):(OneStep==-1)?Builder.CreateSub(RES, Step):RES;
	if(!Step->isMinusOne()&&!Step->isOne())
		RES = Builder.CreateSDiv(RES, Step);
	RES->setName(HeaderName+".tc");

	return RES;
}

bool LoopTripCount::runOnFunction(Function &F)
{
   LI = &getAnalysis<LoopInfo>();
   LoopMap.clear();
   CycleMap.clear();
   unfound_str = "";
   LoopCount = UnfoundCount = 0;

   for(Loop* TopL : *LI){
      for(auto LIte = df_begin(TopL), E = df_end(TopL); LIte!=E; ++LIte){
         Loop* L = *LIte;
         Value* TC = NULL;
         AnalysisedLoop AL = {0};
         ++LoopCount;
         try{
            AL = analysis(L);
            /**trying to find inserted loop trip count in preheader */
            BasicBlock* Preheader = L->getLoopPreheader();
            if(Preheader){
               string HName = (L->getHeader()->getName()+".tc").str();
               auto Found = find_if(Preheader->begin(),Preheader->end(), [HName](Instruction& I){
                     return I.getName()==HName;
                     });
               if(Found != Preheader->end()) TC = &*Found;
            }
            AL.TripCount = TC;
         }catch(NotFound& E){
            ++UnfoundCount;
            unfound<<"  "<<E.get_line()<<":  "<<E.what()<<"\n";
            unfound<<"\t"<<*L<<"\n";
         }
         LoopMap[L] = CycleMap.size(); // write to cache
         CycleMap.push_back(AL);
         AssertRuntime(LoopMap[L] < CycleMap.size() ," should insert indeed");
      }
   }
   unfound.str();

	return true;
}

void LoopTripCount::print(llvm::raw_ostream& OS,const llvm::Module*) const
{
   for(Loop* TopL : *LI){
      for(auto LIte = df_begin(TopL), E = df_end(TopL); LIte != E; ++LIte){
         Loop* L = *LIte;
         const AnalysisedLoop* AL = get(L);
         if(AL == NULL) continue;
         OS<<"In Loop:"<<*L<<"\n";
         OS<<"Start:"<<*AL->Start<<"\n";
         OS<<"Step:"<<*AL->Step<<"\n";
         OS<<"End:"<<*AL->End<<"\n";
      }
   }
   OS << "there are " << LoopCount << " loops " << UnfoundCount
          << " unfound\n";
   OS<<unfound_str;
}

Loop* LoopTripCount::getLoopFor(BasicBlock *BB) const
{
   return LI->getLoopFor(BB);
}

Value* LoopTripCount::getOrInsertTripCount(Loop *L)
{
   if(L->getLoopPreheader()==NULL){
      InsertPreheaderForLoop(L, this);
   }
   Instruction* InsertPos = L->getLoopPredecessor()->getTerminator();
   Value* V = getTripCount(L);
   if(V==NULL){
      auto ite = LoopMap.find(L);
      if(ite == LoopMap.end()) return NULL;
      AnalysisedLoop& AL = CycleMap[ite->second];
      AL.TripCount = V = insertTripCount(AL, L->getHeader()->getName(), InsertPos);
   }
   return V;
}

void LoopTripCount::updateCache(LoopInfo& LI)
{
   size_t Idx = 0;
   for(Loop* TopL : LI){
      for(auto LIte = df_begin(TopL), E = df_end(TopL); LIte != E; ++LIte){
         LoopMap[*LIte] = Idx++;
      }
   }
}
