#define DEBUG
#undef DEBUG

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <stack>
#include <utility>
#include <variant>
#include <vector>
#include <map>
#include "quad.hh"
#include "opt.hh"

using namespace std;
using namespace quad;
using namespace tree;

namespace {

using Edge = pair<int, int>;

map<Opt*, set<Edge>> executable_edges;
map<Opt*, set<int>> defined_temps;

bool sameValue(RtValue a, RtValue b) {
    if (a.getType() != b.getType()) return false;
    if (a.getType() == ValueType::ONE_VALUE) return a.getIntValue() == b.getIntValue();
    return true;
}

RtValue meetValue(RtValue a, RtValue b) {
    if (a.getType() == ValueType::NO_VALUE) return b;
    if (b.getType() == ValueType::NO_VALUE) return a;
    if (a.getType() == ValueType::MANY_VALUES || b.getType() == ValueType::MANY_VALUES)
        return RtValue(ValueType::MANY_VALUES);
    if (a.getIntValue() == b.getIntValue()) return a;
    return RtValue(ValueType::MANY_VALUES);
}

bool setTempValue(Opt* opt, int temp_num, RtValue value) {
    RtValue old_value = opt->getRtValue(temp_num);
    RtValue new_value = meetValue(old_value, value);
    if (sameValue(old_value, new_value)) return false;
    opt->temp_value[temp_num] = new_value;
    return true;
}

int tempNum(QuadTemp* temp) {
    if (temp == nullptr || temp->temp == nullptr) return -1;
    return temp->temp->num;
}

int tempNum(QuadTerm* term) {
    if (term == nullptr || term->kind != QuadTermKind::TEMP) return -1;
    return tempNum(term->get_temp());
}

bool isDefinedTemp(Opt* opt, int temp_num) {
    auto it = defined_temps.find(opt);
    return it != defined_temps.end() && it->second.count(temp_num) > 0;
}

void collectDef(Opt* opt, QuadStm* stm) {
    if (stm == nullptr) return;
    switch (stm->kind) {
        case QuadKind::MOVE:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadMove*>(stm)->dst));
            break;
        case QuadKind::LOAD:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadLoad*>(stm)->dst));
            break;
        case QuadKind::MOVE_BINOP:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadMoveBinop*>(stm)->dst));
            break;
        case QuadKind::MOVE_CALL:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadMoveCall*>(stm)->dst));
            break;
        case QuadKind::MOVE_EXTCALL:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadMoveExtCall*>(stm)->dst));
            break;
        case QuadKind::PHI:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadPhi*>(stm)->temp_exp));
            break;
        case QuadKind::PTR_CALC:
            defined_temps[opt].insert(tempNum(dynamic_cast<QuadPtrCalc*>(stm)->dst));
            break;
        default:
            break;
    }
}

bool markBlockExecutable(Opt* opt, int label_num) {
    if (opt->label2block.find(label_num) == opt->label2block.end()) return false;
    if (opt->block_executable[label_num]) return false;
    opt->block_executable[label_num] = true;
    return true;
}

bool markEdgeExecutable(Opt* opt, int from, int to) {
    bool changed = executable_edges[opt].insert({from, to}).second;
    changed |= markBlockExecutable(opt, to);
    return changed;
}

bool isEdgeExecutable(Opt* opt, int from, int to) {
    return executable_edges[opt].count({from, to}) > 0;
}

RtValue evalTerm(Opt* opt, QuadTerm* term, bool strict_use, bool& changed) {
    if (term == nullptr) return RtValue(ValueType::MANY_VALUES);
    if (term->kind == QuadTermKind::CONST) return RtValue(term->get_const());
    if (term->kind == QuadTermKind::NAME) return RtValue(ValueType::MANY_VALUES);

    int num = tempNum(term);
    RtValue value = opt->getRtValue(num);
    if (value.getType() == ValueType::NO_VALUE && strict_use && !isDefinedTemp(opt, num)) {
        changed |= setTempValue(opt, num, RtValue(ValueType::MANY_VALUES));
        return RtValue(ValueType::MANY_VALUES);
    }
    return value;
}

int divLikeResult(const string& op, int lhs, int rhs, bool& ok) {
    ok = true;
    if (op == "+") return lhs + rhs;
    if (op == "-") return lhs - rhs;
    if (op == "*") return lhs * rhs;
    if (op == "/") {
        if (rhs == 0) {
            ok = false;
            return 0;
        }
        return lhs / rhs;
    }
    if (op == "%") {
        if (rhs == 0) {
            ok = false;
            return 0;
        }
        return lhs % rhs;
    }
    if (op == "&&") return (lhs != 0 && rhs != 0) ? 1 : 0;
    if (op == "||") return (lhs != 0 || rhs != 0) ? 1 : 0;
    if (op == "<") return lhs < rhs ? 1 : 0;
    if (op == ">") return lhs > rhs ? 1 : 0;
    if (op == "<=") return lhs <= rhs ? 1 : 0;
    if (op == ">=") return lhs >= rhs ? 1 : 0;
    if (op == "==") return lhs == rhs ? 1 : 0;
    if (op == "!=") return lhs != rhs ? 1 : 0;
    ok = false;
    return 0;
}

RtValue evalBinopValue(const string& op, RtValue lhs, RtValue rhs) {
    if (lhs.getType() == ValueType::MANY_VALUES || rhs.getType() == ValueType::MANY_VALUES)
        return RtValue(ValueType::MANY_VALUES);
    if (lhs.getType() == ValueType::NO_VALUE || rhs.getType() == ValueType::NO_VALUE)
        return RtValue();

    bool ok = false;
    int value = divLikeResult(op, lhs.getIntValue(), rhs.getIntValue(), ok);
    if (!ok) return RtValue(ValueType::MANY_VALUES);
    return RtValue(value);
}

void addUse(set<Temp*>* uses, QuadTerm* term) {
    int num = tempNum(term);
    if (num >= 0) uses->insert(new Temp(num));
}

set<Temp*>* makeDef(int temp_num) {
    set<Temp*>* defs = new set<Temp*>();
    if (temp_num >= 0) defs->insert(new Temp(temp_num));
    return defs;
}

set<Temp*>* makeUse(initializer_list<QuadTerm*> terms) {
    set<Temp*>* uses = new set<Temp*>();
    for (QuadTerm* term : terms) addUse(uses, term);
    return uses;
}

set<Temp*>* makeUse(const vector<QuadTerm*>* terms) {
    set<Temp*>* uses = new set<Temp*>();
    if (terms != nullptr) {
        for (QuadTerm* term : *terms) addUse(uses, term);
    }
    return uses;
}

QuadTerm* tempTerm(int temp_num, QuadType type) {
    return new QuadTerm(new QuadTemp(new Temp(temp_num), type));
}

QuadTerm* replaceTerm(Opt* opt, QuadTerm* term) {
    if (term == nullptr) return nullptr;
    if (term->kind != QuadTermKind::TEMP) return term->clone();

    int num = tempNum(term);
    RtValue value = opt->getRtValue(num);
    if (value.getType() == ValueType::ONE_VALUE) return new QuadTerm(value.getIntValue());
    return term->clone();
}

vector<QuadTerm*>* replaceTerms(Opt* opt, vector<QuadTerm*>* terms) {
    vector<QuadTerm*>* result = new vector<QuadTerm*>();
    if (terms != nullptr) {
        for (QuadTerm* term : *terms) result->push_back(replaceTerm(opt, term));
    }
    return result;
}

QuadCall* replaceCall(Opt* opt, QuadCall* call) {
    if (call == nullptr) return nullptr;
    QuadTerm* obj = replaceTerm(opt, call->obj_term);
    vector<QuadTerm*>* args = replaceTerms(opt, call->args);
    set<Temp*>* uses = makeUse(args);
    addUse(uses, obj);
    return new QuadCall(call->name, obj, args, new set<Temp*>(), uses);
}

QuadExtCall* replaceExtCall(Opt* opt, QuadExtCall* call) {
    if (call == nullptr) return nullptr;
    vector<QuadTerm*>* args = replaceTerms(opt, call->args);
    return new QuadExtCall(call->extfun, args, new set<Temp*>(), makeUse(args));
}

bool evalCond(const string& op, QuadTerm* lhs, QuadTerm* rhs, bool& result) {
    if (lhs == nullptr || rhs == nullptr) return false;
    if (lhs->kind != QuadTermKind::CONST || rhs->kind != QuadTermKind::CONST) return false;
    bool ok = false;
    int value = divLikeResult(op, lhs->get_const(), rhs->get_const(), ok);
    if (!ok) return false;
    result = value != 0;
    return true;
}

QuadStm* rewriteNonPhi(Opt* opt, QuadStm* stm) {
    if (stm == nullptr) return nullptr;

    switch (stm->kind) {
        case QuadKind::MOVE: {
            auto* move = dynamic_cast<QuadMove*>(stm);
            int dst_num = tempNum(move->dst);
            if (opt->getRtValue(dst_num).getType() == ValueType::ONE_VALUE) return nullptr;
            QuadTerm* src = replaceTerm(opt, move->src);
            return new QuadMove(move->dst->clone(), src, makeDef(dst_num), makeUse({src}));
        }
        case QuadKind::LOAD: {
            auto* load = dynamic_cast<QuadLoad*>(stm);
            QuadTerm* src = replaceTerm(opt, load->src);
            return new QuadLoad(load->dst->clone(), src, makeDef(tempNum(load->dst)), makeUse({src}));
        }
        case QuadKind::STORE: {
            auto* store = dynamic_cast<QuadStore*>(stm);
            QuadTerm* src = replaceTerm(opt, store->src);
            QuadTerm* dst = replaceTerm(opt, store->dst);
            return new QuadStore(src, dst, new set<Temp*>(), makeUse({src, dst}));
        }
        case QuadKind::MOVE_BINOP: {
            auto* binop = dynamic_cast<QuadMoveBinop*>(stm);
            int dst_num = tempNum(binop->dst);
            if (opt->getRtValue(dst_num).getType() == ValueType::ONE_VALUE) return nullptr;
            QuadTerm* lhs = replaceTerm(opt, binop->left);
            QuadTerm* rhs = replaceTerm(opt, binop->right);
            return new QuadMoveBinop(binop->dst->clone(), lhs, binop->binop, rhs,
                                     makeDef(dst_num), makeUse({lhs, rhs}));
        }
        case QuadKind::CALL: {
            return replaceCall(opt, dynamic_cast<QuadCall*>(stm));
        }
        case QuadKind::MOVE_CALL: {
            auto* move_call = dynamic_cast<QuadMoveCall*>(stm);
            QuadCall* call = replaceCall(opt, move_call->call);
            set<Temp*>* uses = call == nullptr ? new set<Temp*>() : call->use;
            return new QuadMoveCall(move_call->dst->clone(), call,
                                    makeDef(tempNum(move_call->dst)), uses);
        }
        case QuadKind::EXTCALL: {
            return replaceExtCall(opt, dynamic_cast<QuadExtCall*>(stm));
        }
        case QuadKind::MOVE_EXTCALL: {
            auto* move_ext = dynamic_cast<QuadMoveExtCall*>(stm);
            QuadExtCall* call = replaceExtCall(opt, move_ext->extcall);
            set<Temp*>* uses = call == nullptr ? new set<Temp*>() : call->use;
            return new QuadMoveExtCall(move_ext->dst->clone(), call,
                                       makeDef(tempNum(move_ext->dst)), uses);
        }
        case QuadKind::LABEL: {
            return static_cast<QuadStm*>(stm->clone());
        }
        case QuadKind::JUMP: {
            return static_cast<QuadStm*>(stm->clone());
        }
        case QuadKind::CJUMP: {
            auto* cjump = dynamic_cast<QuadCJump*>(stm);
            QuadTerm* lhs = replaceTerm(opt, cjump->left);
            QuadTerm* rhs = replaceTerm(opt, cjump->right);
            bool cond = false;
            if (evalCond(cjump->relop, lhs, rhs, cond)) {
                return new QuadJump(cond ? cjump->t : cjump->f, new set<Temp*>(), new set<Temp*>());
            }
            return new QuadCJump(cjump->relop, lhs, rhs, cjump->t, cjump->f,
                                 new set<Temp*>(), makeUse({lhs, rhs}));
        }
        case QuadKind::RETURN: {
            auto* ret = dynamic_cast<QuadReturn*>(stm);
            QuadTerm* exp = replaceTerm(opt, ret->exp);
            return new QuadReturn(exp, new set<Temp*>(), makeUse({exp}));
        }
        case QuadKind::PTR_CALC: {
            auto* ptr = dynamic_cast<QuadPtrCalc*>(stm);
            int dst_num = tempNum(ptr->dst);
            if (opt->getRtValue(dst_num).getType() == ValueType::ONE_VALUE) return nullptr;
            QuadTerm* dst = ptr->dst == nullptr ? nullptr : ptr->dst->clone();
            QuadTerm* base = replaceTerm(opt, ptr->ptr);
            QuadTerm* offset = replaceTerm(opt, ptr->offset);
            return new QuadPtrCalc(dst, base, offset, makeDef(dst_num), makeUse({base, offset}));
        }
        default:
            return nullptr;
    }
}

bool isTerminator(QuadStm* stm) {
    if (stm == nullptr) return false;
    if (stm->kind == QuadKind::JUMP || stm->kind == QuadKind::CJUMP || stm->kind == QuadKind::RETURN)
        return true;
    if (stm->kind == QuadKind::EXTCALL) {
        auto* call = dynamic_cast<QuadExtCall*>(stm);
        return call != nullptr && call->extfun == "exit";
    }
    if (stm->kind == QuadKind::MOVE_EXTCALL) {
        auto* call = dynamic_cast<QuadMoveExtCall*>(stm);
        return call != nullptr && call->extcall != nullptr && call->extcall->extfun == "exit";
    }
    return false;
}

void appendPendingMoves(vector<QuadStm*>* quads, vector<QuadStm*>* pending) {
    if (pending == nullptr) return;
    quads->insert(quads->end(), pending->begin(), pending->end());
    pending->clear();
}

vector<Label*>* executableExitLabels(Opt* opt, QuadBlock* block) {
    vector<Label*>* exits = new vector<Label*>();
    if (block == nullptr || block->entry_label == nullptr || block->exit_labels == nullptr) return exits;
    int from = block->entry_label->num;
    set<int> seen;
    for (Label* label : *block->exit_labels) {
        if (label == nullptr) continue;
        if (isEdgeExecutable(opt, from, label->num) && seen.insert(label->num).second) {
            exits->push_back(label);
        }
    }
    return exits;
}

} // namespace

void Opt::calculateBT() {
    label2block.clear();
    block_executable.clear();
    temp_value.clear();
    executable_edges[this].clear();
    defined_temps[this].clear();

    if (func == nullptr || func->quadblocklist == nullptr || func->quadblocklist->empty()) return;

    for (QuadBlock* block : *func->quadblocklist) {
        if (block == nullptr || block->entry_label == nullptr) continue;
        label2block[block->entry_label->num] = block;
        block_executable[block->entry_label->num] = false;
        if (block->quadlist != nullptr) {
            for (QuadStm* stm : *block->quadlist) collectDef(this, stm);
        }
    }

    if (func->params != nullptr) {
        for (Temp* param : *func->params) {
            if (param != nullptr) temp_value[param->num] = RtValue(ValueType::MANY_VALUES);
        }
    }

    int entry = func->quadblocklist->front()->entry_label->num;
    block_executable[entry] = true;

    bool changed = true;
    while (changed) {
        changed = false;
        for (QuadBlock* block : *func->quadblocklist) {
            if (block == nullptr || block->entry_label == nullptr || block->quadlist == nullptr) continue;
            int block_label = block->entry_label->num;
            if (!block_executable[block_label]) continue;

            bool saw_terminator = false;
            for (QuadStm* stm : *block->quadlist) {
                if (stm == nullptr) continue;
                switch (stm->kind) {
                    case QuadKind::MOVE: {
                        auto* move = dynamic_cast<QuadMove*>(stm);
                        RtValue value = evalTerm(this, move->src, true, changed);
                        changed |= setTempValue(this, tempNum(move->dst), value);
                        break;
                    }
                    case QuadKind::LOAD: {
                        auto* load = dynamic_cast<QuadLoad*>(stm);
                        evalTerm(this, load->src, true, changed);
                        changed |= setTempValue(this, tempNum(load->dst), RtValue(ValueType::MANY_VALUES));
                        break;
                    }
                    case QuadKind::STORE: {
                        auto* store = dynamic_cast<QuadStore*>(stm);
                        evalTerm(this, store->src, true, changed);
                        evalTerm(this, store->dst, true, changed);
                        break;
                    }
                    case QuadKind::MOVE_BINOP: {
                        auto* binop = dynamic_cast<QuadMoveBinop*>(stm);
                        RtValue lhs = evalTerm(this, binop->left, true, changed);
                        RtValue rhs = evalTerm(this, binop->right, true, changed);
                        changed |= setTempValue(this, tempNum(binop->dst), evalBinopValue(binop->binop, lhs, rhs));
                        break;
                    }
                    case QuadKind::CALL: {
                        auto* call = dynamic_cast<QuadCall*>(stm);
                        evalTerm(this, call->obj_term, true, changed);
                        if (call->args != nullptr) {
                            for (QuadTerm* arg : *call->args) evalTerm(this, arg, true, changed);
                        }
                        break;
                    }
                    case QuadKind::MOVE_CALL: {
                        auto* move_call = dynamic_cast<QuadMoveCall*>(stm);
                        if (move_call->call != nullptr) {
                            evalTerm(this, move_call->call->obj_term, true, changed);
                            if (move_call->call->args != nullptr) {
                                for (QuadTerm* arg : *move_call->call->args) evalTerm(this, arg, true, changed);
                            }
                        }
                        changed |= setTempValue(this, tempNum(move_call->dst), RtValue(ValueType::MANY_VALUES));
                        break;
                    }
                    case QuadKind::EXTCALL: {
                        auto* call = dynamic_cast<QuadExtCall*>(stm);
                        if (call->args != nullptr) {
                            for (QuadTerm* arg : *call->args) evalTerm(this, arg, true, changed);
                        }
                        if (call->extfun == "exit") saw_terminator = true;
                        break;
                    }
                    case QuadKind::MOVE_EXTCALL: {
                        auto* move_call = dynamic_cast<QuadMoveExtCall*>(stm);
                        if (move_call->extcall != nullptr && move_call->extcall->args != nullptr) {
                            for (QuadTerm* arg : *move_call->extcall->args) evalTerm(this, arg, true, changed);
                        }
                        changed |= setTempValue(this, tempNum(move_call->dst), RtValue(ValueType::MANY_VALUES));
                        if (move_call->extcall != nullptr && move_call->extcall->extfun == "exit") saw_terminator = true;
                        break;
                    }
                    case QuadKind::PHI: {
                        auto* phi = dynamic_cast<QuadPhi*>(stm);
                        RtValue value;
                        if (phi->args != nullptr) {
                            for (auto& arg : *phi->args) {
                                if (arg.second == nullptr || !isEdgeExecutable(this, arg.second->num, block_label)) continue;
                                QuadTerm term(new QuadTemp(arg.first, phi->temp_exp->type));
                                bool local_changed = false;
                                RtValue arg_value = evalTerm(this, &term, false, local_changed);
                                if (arg_value.getType() == ValueType::NO_VALUE && !isDefinedTemp(this, arg.first->num)) {
                                    arg_value = RtValue(ValueType::MANY_VALUES);
                                }
                                value = meetValue(value, arg_value);
                            }
                        }
                        changed |= setTempValue(this, tempNum(phi->temp_exp), value);
                        break;
                    }
                    case QuadKind::JUMP: {
                        auto* jump = dynamic_cast<QuadJump*>(stm);
                        if (jump->label != nullptr) changed |= markEdgeExecutable(this, block_label, jump->label->num);
                        saw_terminator = true;
                        break;
                    }
                    case QuadKind::CJUMP: {
                        auto* cjump = dynamic_cast<QuadCJump*>(stm);
                        RtValue lhs = evalTerm(this, cjump->left, true, changed);
                        RtValue rhs = evalTerm(this, cjump->right, true, changed);
                        RtValue cond = evalBinopValue(cjump->relop, lhs, rhs);
                        if (cond.getType() == ValueType::ONE_VALUE) {
                            Label* target = cond.getIntValue() != 0 ? cjump->t : cjump->f;
                            if (target != nullptr) changed |= markEdgeExecutable(this, block_label, target->num);
                        } else if (cond.getType() == ValueType::MANY_VALUES) {
                            if (cjump->t != nullptr) changed |= markEdgeExecutable(this, block_label, cjump->t->num);
                            if (cjump->f != nullptr) changed |= markEdgeExecutable(this, block_label, cjump->f->num);
                        }
                        saw_terminator = true;
                        break;
                    }
                    case QuadKind::RETURN: {
                        auto* ret = dynamic_cast<QuadReturn*>(stm);
                        evalTerm(this, ret->exp, true, changed);
                        saw_terminator = true;
                        break;
                    }
                    case QuadKind::PTR_CALC: {
                        auto* ptr = dynamic_cast<QuadPtrCalc*>(stm);
                        evalTerm(this, ptr->ptr, true, changed);
                        evalTerm(this, ptr->offset, true, changed);
                        changed |= setTempValue(this, tempNum(ptr->dst), RtValue(ValueType::MANY_VALUES));
                        break;
                    }
                    default:
                        break;
                }
            }

            if (!saw_terminator && block->exit_labels != nullptr) {
                for (Label* label : *block->exit_labels) {
                    if (label != nullptr) changed |= markEdgeExecutable(this, block_label, label->num);
                }
            }
        }
    }
}

void Opt::modifyFunc() {
    if (func == nullptr || func->quadblocklist == nullptr) return;

    map<int, vector<QuadStm*>*> pending_pred_moves;
    map<QuadStm*, vector<QuadStm*>*> phi_replacements;
    int next_temp = func->last_temp_num + 1;
    int inserted_temp_count = 0;

    for (QuadBlock* block : *func->quadblocklist) {
        if (block == nullptr || block->entry_label == nullptr || block->quadlist == nullptr) continue;
        int block_label = block->entry_label->num;
        if (!block_executable[block_label]) continue;

        for (QuadStm* stm : *block->quadlist) {
            if (stm == nullptr || stm->kind != QuadKind::PHI) continue;
            auto* phi = dynamic_cast<QuadPhi*>(stm);
            vector<pair<Temp*, Label*>> effective_args;
            if (phi->args != nullptr) {
                for (auto& arg : *phi->args) {
                    if (arg.first == nullptr || arg.second == nullptr) continue;
                    if (isEdgeExecutable(this, arg.second->num, block_label)) effective_args.push_back(arg);
                }
            }

            vector<QuadStm*>* replacement = new vector<QuadStm*>();
            if (effective_args.empty()) {
                phi_replacements[stm] = replacement;
                continue;
            }

            bool all_const = true;
            bool same_const = true;
            int const_value = 0;
            bool first_const = true;
            for (auto& arg : effective_args) {
                RtValue value = getRtValue(arg.first->num);
                if (value.getType() != ValueType::ONE_VALUE) {
                    all_const = false;
                    same_const = false;
                    break;
                }
                if (first_const) {
                    const_value = value.getIntValue();
                    first_const = false;
                } else if (const_value != value.getIntValue()) {
                    same_const = false;
                }
            }

            if (all_const && same_const) {
                phi_replacements[stm] = replacement;
                continue;
            }

            if (effective_args.size() == 1) {
                Temp* src_temp = effective_args.front().first;
                RtValue value = getRtValue(src_temp->num);
                if (value.getType() != ValueType::ONE_VALUE) {
                    QuadTerm* src = tempTerm(src_temp->num, phi->temp_exp->type);
                    replacement->push_back(new QuadMove(phi->temp_exp->clone(), src,
                                                        makeDef(tempNum(phi->temp_exp)), makeUse({src})));
                }
                phi_replacements[stm] = replacement;
                continue;
            }

            vector<pair<Temp*, Label*>>* new_args = new vector<pair<Temp*, Label*>>();
            for (auto& arg : effective_args) {
                RtValue value = getRtValue(arg.first->num);
                if (value.getType() == ValueType::ONE_VALUE) {
                    int new_num = next_temp++;
                    inserted_temp_count++;
                    QuadTemp* dst = new QuadTemp(new Temp(new_num), phi->temp_exp->type);
                    QuadTerm* src = new QuadTerm(value.getIntValue());
                    QuadMove* move = new QuadMove(dst, src, makeDef(new_num), new set<Temp*>());
                    if (pending_pred_moves[arg.second->num] == nullptr) {
                        pending_pred_moves[arg.second->num] = new vector<QuadStm*>();
                    }
                    pending_pred_moves[arg.second->num]->push_back(move);
                    new_args->push_back({new Temp(new_num), arg.second});
                } else {
                    new_args->push_back({new Temp(arg.first->num), arg.second});
                }
            }

            set<Temp*>* uses = new set<Temp*>();
            for (auto& arg : *new_args) uses->insert(new Temp(arg.first->num));
            replacement->push_back(new QuadPhi(phi->temp_exp->clone(), new_args,
                                               makeDef(tempNum(phi->temp_exp)), uses));
            phi_replacements[stm] = replacement;
        }
    }

    vector<QuadBlock*>* new_blocks = new vector<QuadBlock*>();
    for (QuadBlock* block : *func->quadblocklist) {
        if (block == nullptr || block->entry_label == nullptr || block->quadlist == nullptr) continue;
        int block_label = block->entry_label->num;
        if (!block_executable[block_label]) continue;

        vector<QuadStm*>* new_quads = new vector<QuadStm*>();
        vector<QuadStm*>* pending = pending_pred_moves[block_label];
        bool pending_flushed = false;

        for (QuadStm* stm : *block->quadlist) {
            if (stm == nullptr) continue;
            if (stm->kind == QuadKind::PHI) {
                appendPendingMoves(new_quads, phi_replacements[stm]);
                continue;
            }

            QuadStm* rewritten = rewriteNonPhi(this, stm);
            if (rewritten == nullptr) continue;
            if (isTerminator(rewritten) && !pending_flushed) {
                appendPendingMoves(new_quads, pending);
                pending_flushed = true;
            }
            new_quads->push_back(rewritten);
        }

        if (!pending_flushed) appendPendingMoves(new_quads, pending);
        new_blocks->push_back(new QuadBlock(new_quads, block->entry_label, executableExitLabels(this, block)));
    }

    int new_last_temp = func->last_temp_num + 2 + inserted_temp_count;
    func = new QuadFuncDecl(func->funcname, func->params, new_blocks, func->last_label_num, new_last_temp);
}

QuadFuncDecl* Opt::optFunc() {
    calculateBT();
    modifyFunc();
    return func;
}

QuadProgram* optProg(QuadProgram* prog) {
    QuadProgram* newProg = new QuadProgram(new vector<QuadFuncDecl*>(), prog->last_label_num, prog->last_temp_num);
    for (int i=0; i < prog->quadFuncDeclList->size(); i++) {
        Opt optthis(prog->quadFuncDeclList->at(i));
        newProg->quadFuncDeclList->push_back(optthis.optFunc());
    }
    return newProg;
}
