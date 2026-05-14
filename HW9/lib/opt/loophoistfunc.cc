#define DEBUG
#undef DEBUG

#include <algorithm>
#include <string>
#include <stack>
#include <set>
#include <variant>
#include <vector>
#include <map>
#include "quad.hh"
#include "flowinfo.hh"
#include "loopopt.hh"

using namespace std;
using namespace quad;

// Main entry point for loop optimization
// Complete the function!!

namespace {

int labelNum(QuadBlock *block) {
    return (block != nullptr && block->entry_label != nullptr) ? block->entry_label->num : -1;
}

int tempNum(QuadTemp *temp) {
    if (temp == nullptr || temp->temp == nullptr) return -1;
    return temp->temp->num;
}

int tempNum(QuadTerm *term) {
    if (term == nullptr || term->kind != QuadTermKind::TEMP) return -1;
    return tempNum(term->get_temp());
}

int defTemp(QuadStm *stm) {
    if (stm == nullptr) return -1;
    switch (stm->kind) {
        case QuadKind::MOVE:
            return tempNum(dynamic_cast<QuadMove*>(stm)->dst);
        case QuadKind::MOVE_BINOP:
            return tempNum(dynamic_cast<QuadMoveBinop*>(stm)->dst);
        case QuadKind::PTR_CALC:
            return tempNum(dynamic_cast<QuadPtrCalc*>(stm)->dst);
        default:
            return -1;
    }
}

void collectDefInFunction(QuadFuncDecl *func, map<int, int> &def_block) {
    if (func == nullptr || func->quadblocklist == nullptr) return;
    for (QuadBlock *block : *func->quadblocklist) {
        if (block == nullptr || block->quadlist == nullptr) continue;
        int block_label = labelNum(block);
        for (QuadStm *stm : *block->quadlist) {
            int def = -1;
            switch (stm->kind) {
                case QuadKind::MOVE:
                    def = tempNum(dynamic_cast<QuadMove*>(stm)->dst);
                    break;
                case QuadKind::LOAD:
                    def = tempNum(dynamic_cast<QuadLoad*>(stm)->dst);
                    break;
                case QuadKind::MOVE_BINOP:
                    def = tempNum(dynamic_cast<QuadMoveBinop*>(stm)->dst);
                    break;
                case QuadKind::MOVE_CALL:
                    def = tempNum(dynamic_cast<QuadMoveCall*>(stm)->dst);
                    break;
                case QuadKind::MOVE_EXTCALL:
                    def = tempNum(dynamic_cast<QuadMoveExtCall*>(stm)->dst);
                    break;
                case QuadKind::PHI:
                    def = tempNum(dynamic_cast<QuadPhi*>(stm)->temp_exp);
                    break;
                case QuadKind::PTR_CALC:
                    def = tempNum(dynamic_cast<QuadPtrCalc*>(stm)->dst);
                    break;
                default:
                    break;
            }
            if (def >= 0) def_block[def] = block_label;
        }
    }
}

bool termInvariant(QuadTerm *term, const set<int> &body, const map<int, int> &def_block,
                   const set<int> &invariant_defs) {
    if (term == nullptr) return true;
    if (term->kind != QuadTermKind::TEMP) return true;

    int temp = tempNum(term);
    if (temp < 0) return true;
    if (invariant_defs.count(temp) > 0) return true;

    auto it = def_block.find(temp);
    if (it == def_block.end()) return true;
    return body.count(it->second) == 0;
}

bool hoistable(QuadStm *stm, const set<int> &body, const map<int, int> &def_block,
               const set<int> &invariant_defs) {
    if (stm == nullptr) return false;
    switch (stm->kind) {
        case QuadKind::MOVE: {
            auto *move = dynamic_cast<QuadMove*>(stm);
            return move != nullptr && termInvariant(move->src, body, def_block, invariant_defs);
        }
        case QuadKind::MOVE_BINOP: {
            auto *binop = dynamic_cast<QuadMoveBinop*>(stm);
            return binop != nullptr &&
                   termInvariant(binop->left, body, def_block, invariant_defs) &&
                   termInvariant(binop->right, body, def_block, invariant_defs);
        }
        case QuadKind::PTR_CALC: {
            auto *ptr = dynamic_cast<QuadPtrCalc*>(stm);
            return ptr != nullptr &&
                   termInvariant(ptr->ptr, body, def_block, invariant_defs) &&
                   termInvariant(ptr->offset, body, def_block, invariant_defs);
        }
        default:
            return false;
    }
}

bool isTerminator(QuadStm *stm) {
    if (stm == nullptr) return false;
    if (stm->kind == QuadKind::JUMP || stm->kind == QuadKind::CJUMP || stm->kind == QuadKind::RETURN)
        return true;
    if (stm->kind == QuadKind::EXTCALL) {
        auto *call = dynamic_cast<QuadExtCall*>(stm);
        return call != nullptr && call->extfun == "exit";
    }
    if (stm->kind == QuadKind::MOVE_EXTCALL) {
        auto *call = dynamic_cast<QuadMoveExtCall*>(stm);
        return call != nullptr && call->extcall != nullptr && call->extcall->extfun == "exit";
    }
    return false;
}

QuadBlock* findBlock(QuadFuncDecl *func, int label) {
    if (func == nullptr || func->quadblocklist == nullptr) return nullptr;
    for (QuadBlock *block : *func->quadblocklist) {
        if (labelNum(block) == label) return block;
    }
    return nullptr;
}

int findPreheader(QuadFuncDecl *func, LoopHeader *loop) {
    if (func == nullptr || func->quadblocklist == nullptr || loop == nullptr) return -1;
    for (QuadBlock *block : *func->quadblocklist) {
        int pred_label = labelNum(block);
        if (pred_label < 0 || loop->bodyBlocks.count(pred_label) > 0 || block->exit_labels == nullptr) continue;
        for (Label *exit : *block->exit_labels) {
            if (exit != nullptr && exit->num == loop->headerLabel) return pred_label;
        }
    }
    return -1;
}

void removeHoistedStatements(QuadFuncDecl *func, const set<QuadStm*> &hoisted) {
    if (func == nullptr || func->quadblocklist == nullptr) return;
    for (QuadBlock *block : *func->quadblocklist) {
        if (block == nullptr || block->quadlist == nullptr) continue;
        vector<QuadStm*> *new_list = new vector<QuadStm*>();
        for (QuadStm *stm : *block->quadlist) {
            if (hoisted.count(stm) == 0) new_list->push_back(stm);
        }
        block->quadlist = new_list;
    }
}

void insertBeforeTerminator(QuadBlock *preheader, const vector<QuadStm*> &stms) {
    if (preheader == nullptr || preheader->quadlist == nullptr || stms.empty()) return;
    auto insert_pos = preheader->quadlist->end();
    for (auto it = preheader->quadlist->begin(); it != preheader->quadlist->end(); ++it) {
        if (isTerminator(*it)) {
            insert_pos = it;
            break;
        }
    }
    preheader->quadlist->insert(insert_pos, stms.begin(), stms.end());
}

vector<LoopHeader*> sortedLoops(QuadFuncDecl *func, LoopHeaderMap *loopHeaderMap) {
    vector<LoopHeader*> loops;
    if (func == nullptr || loopHeaderMap == nullptr || !loopHeaderMap->funcLoopHeaders.count(func)) return loops;
    for (LoopHeader *loop : loopHeaderMap->funcLoopHeaders[func]) {
        if (loop != nullptr) loops.push_back(loop);
    }
    sort(loops.begin(), loops.end(), [](LoopHeader *a, LoopHeader *b) {
        if (a->bodyBlocks.size() != b->bodyBlocks.size()) return a->bodyBlocks.size() > b->bodyBlocks.size();
        return a->headerLabel < b->headerLabel;
    });
    return loops;
}

} // namespace

QuadFuncDecl* loopHoistFunc(QuadFuncDecl* func, LoopHeaderMap *loopHeaderMap) {
    if (func == nullptr || func->quadblocklist == nullptr || loopHeaderMap == nullptr) {
        return func;
    }

    for (LoopHeader *loop : sortedLoops(func, loopHeaderMap)) {
        int preheader_label = findPreheader(func, loop);
        QuadBlock *preheader = findBlock(func, preheader_label);
        if (preheader == nullptr) continue;

        map<int, int> def_block;
        collectDefInFunction(func, def_block);

        set<int> invariant_defs;
        set<QuadStm*> hoisted_set;
        vector<QuadStm*> hoisted_order;

        bool changed = true;
        while (changed) {
            changed = false;
            for (QuadBlock *block : *func->quadblocklist) {
                if (block == nullptr || block->quadlist == nullptr) continue;
                if (loop->bodyBlocks.count(labelNum(block)) == 0) continue;

                for (QuadStm *stm : *block->quadlist) {
                    if (hoisted_set.count(stm) > 0) continue;
                    if (!hoistable(stm, loop->bodyBlocks, def_block, invariant_defs)) continue;

                    int def = defTemp(stm);
                    if (def < 0) continue;
                    hoisted_set.insert(stm);
                    hoisted_order.push_back(stm);
                    invariant_defs.insert(def);
                    changed = true;
                }
            }
        }

        if (hoisted_order.empty()) continue;
        removeHoistedStatements(func, hoisted_set);
        insertBeforeTerminator(preheader, hoisted_order);
    }

    return func;
}
