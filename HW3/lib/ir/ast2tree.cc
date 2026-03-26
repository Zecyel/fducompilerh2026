#define DEBUG
//#undef DEBUG

#include <iostream> // IWYU pragma: keep
#include <string>
#include <map> // IWYU pragma: keep
#include <vector>
#include <algorithm> // IWYU pragma: keep
#include "config.hh" // IWYU pragma: keep
#include "ASTheader.hh" // IWYU pragma: keep
#include "FDMJAST.hh" // IWYU pragma: keep
#include "treep.hh" // IWYU pragma: keep
#include "temp.hh" // IWYU pragma: keep
#include "ast2tree.hh" // IWYU pragma: keep

using namespace std;

// Helper: convert Tr_Exp to Tr_cx using placeholder labels (num=-1)
// This avoids allocating real label numbers from the temp_map for Tr_ex cases
static Tr_cx* make_cx(Tr_Exp* tr, Temp_map* tm) {
    Tr_cx* cx = dynamic_cast<Tr_cx*>(tr);
    if (cx) return cx;
    // It's a Tr_ex — create CJump(!=, exp, 0) with placeholder labels
    Tr_ex* ex = dynamic_cast<Tr_ex*>(tr);
    if (ex) {
        Patch_list* t = new Patch_list();
        Patch_list* f = new Patch_list();
        tree::Label* tl = new tree::Label(-1);
        tree::Label* fl = new tree::Label(-1);
        t->add_patch(tl);
        f->add_patch(fl);
        return new Tr_cx(t, f, new tree::Cjump("!=", ex->exp, new tree::Const(0), tl, fl));
    }
    // Tr_nx — shouldn't happen for conditions
    return tr->unCx(tm);
}

// ============================================================
// Helper: generate_method_var_table
// ============================================================
Method_var_table* generate_method_var_table(string class_name, string method_name, Name_Maps* nm, Temp_map* tm) {
    Method_var_table* mvt = new Method_var_table();
    // local vars first (alphabetical order from set)
    set<string>* var_names = nm->get_method_var_list(class_name, method_name);
    if (var_names) {
        for (auto &vname : *var_names) {
            tree::Temp* t = tm->newtemp();
            (*mvt->var_temp_map)[vname] = t;
            VarDecl* vd = nm->get_method_var(class_name, method_name, vname);
            if (vd && vd->type) {
                if (vd->type->typeKind == TypeKind::INT)
                    (*mvt->var_type_map)[vname] = tree::Type::INT;
                else
                    (*mvt->var_type_map)[vname] = tree::Type::PTR;
            } else {
                (*mvt->var_type_map)[vname] = tree::Type::INT;
            }
        }
    }
    // formals after (including _^return^_method_name)
    vector<string>* formal_names = nm->get_method_formal_list_string(class_name, method_name);
    if (formal_names) {
        for (auto &fname : *formal_names) {
            if (mvt->var_temp_map->find(fname) != mvt->var_temp_map->end())
                continue; // already allocated as local var
            tree::Temp* t = tm->newtemp();
            (*mvt->var_temp_map)[fname] = t;
            Formal* f = nm->get_method_formal(class_name, method_name, fname);
            if (f && f->type) {
                if (f->type->typeKind == TypeKind::INT)
                    (*mvt->var_type_map)[fname] = tree::Type::INT;
                else
                    (*mvt->var_type_map)[fname] = tree::Type::PTR;
            } else {
                (*mvt->var_type_map)[fname] = tree::Type::INT;
            }
        }
    }
    return mvt;
}

// ============================================================
// generate_class_table (stub for HW3)
// ============================================================
Class_table* generate_class_table(AST_Semant_Map* semant_map) {
    return new Class_table();
}

// ============================================================
// ast2tree — entry point
// ============================================================
tree::Program* ast2tree(fdmj::Program* prog, AST_Semant_Map* semant_map) {
    ASTToTreeVisitor visitor;
    visitor.semant_map = semant_map;
    visitor.class_table = generate_class_table(semant_map);
    prog->accept(visitor);
    return (tree::Program*)visitor.getTree();
}

// ============================================================
// visit(Program*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Program* node) {
    vector<tree::FuncDecl*>* fdl = new vector<tree::FuncDecl*>();
    node->main->accept(*this);
    fdl->push_back((tree::FuncDecl*)visit_tree_result);
    visit_tree_result = new tree::Program(fdl);
}

// ============================================================
// visit(MainMethod*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::MainMethod* node) {
    current_class = "__$main__";
    current_method = "main";
    method_temp_map = new Temp_map();
    Name_Maps* nm = semant_map->getNameMaps();
    method_var_table = generate_method_var_table(current_class, current_method, nm, method_temp_map);

    // Sync label counter to start after all temps
    method_temp_map->next_label = method_temp_map->next_temp;

    vector<tree::Stm*>* stms = new vector<tree::Stm*>();

    // visit VarDecls for initializations
    if (node->vdl) {
        for (auto vd : *(node->vdl)) {
            vd->accept(*this);
            if (visit_tree_result) {
                stms->push_back((tree::Stm*)visit_tree_result);
                visit_tree_result = nullptr;
            }
        }
    }

    // visit statements
    if (node->sl) {
        for (auto s : *(node->sl)) {
            s->accept(*this);
            if (visit_tree_result) {
                stms->push_back((tree::Stm*)visit_tree_result);
                visit_tree_result = nullptr;
            }
        }
    }

    tree::Seq* body = new tree::Seq(stms);
    string fname = "__$main__^main";
    int lt = method_temp_map->next_temp - 1;
    int ll = method_temp_map->next_label - 1;
    visit_tree_result = new tree::FuncDecl(fname, nullptr, body, tree::Type::INT, lt, ll);
}

// ============================================================
// visit(VarDecl*) — emit Move for initialized int vars
// ============================================================
void ASTToTreeVisitor::visit(fdmj::VarDecl* node) {
    visit_tree_result = nullptr;
    if (holds_alternative<fdmj::IntExp*>(node->init)) {
        fdmj::IntExp* init_val = get<fdmj::IntExp*>(node->init);
        if (init_val) {
            string vname = node->id->id;
            tree::Temp* t = method_var_table->get_var_temp(vname);
            if (t) {
                visit_tree_result = new tree::Move(
                    new tree::TempExp(tree::Type::INT, t),
                    new tree::Const(init_val->val));
            }
        }
    }
}

// ============================================================
// visit(Assign*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Assign* node) {
    node->left->accept(*this);
    Tr_Exp* left_tr = visit_exp_result;
    tree::Exp* dst = left_tr->unEx(method_temp_map)->exp;

    node->exp->accept(*this);
    Tr_Exp* right_tr = visit_exp_result;
    tree::Exp* src = right_tr->unEx(method_temp_map)->exp;

    visit_tree_result = new tree::Move(dst, src);
}

// ============================================================
// visit(If*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::If* node) {
    node->exp->accept(*this);
    Tr_Exp* cond_tr = visit_exp_result;
    Tr_cx* cx = make_cx(cond_tr, method_temp_map);

    vector<tree::Stm*>* sl = new vector<tree::Stm*>();

    if (node->stm2) {
        // if-else: visit branches first to get their stms, then allocate labels
        node->stm1->accept(*this);
        tree::Stm* then_stm = (tree::Stm*)visit_tree_result;
        visit_tree_result = nullptr;

        node->stm2->accept(*this);
        tree::Stm* else_stm = (tree::Stm*)visit_tree_result;
        visit_tree_result = nullptr;

        tree::Label* then_label = method_temp_map->newlabel();
        tree::Label* else_label = method_temp_map->newlabel();
        tree::Label* end_label = method_temp_map->newlabel();
        cx->true_list->patch(then_label);
        cx->false_list->patch(else_label);

        sl->push_back(cx->stm);
        sl->push_back(new tree::LabelStm(then_label));
        if (then_stm) sl->push_back(then_stm);
        sl->push_back(new tree::Jump(end_label));
        sl->push_back(new tree::LabelStm(else_label));
        if (else_stm) sl->push_back(else_stm);
        sl->push_back(new tree::LabelStm(end_label));
    } else {
        // if without else: visit branch first, then allocate labels
        node->stm1->accept(*this);
        tree::Stm* then_stm = (tree::Stm*)visit_tree_result;
        visit_tree_result = nullptr;

        tree::Label* then_label = method_temp_map->newlabel();
        tree::Label* end_label = method_temp_map->newlabel();
        cx->true_list->patch(then_label);
        cx->false_list->patch(end_label);

        sl->push_back(cx->stm);
        sl->push_back(new tree::LabelStm(then_label));
        if (then_stm) sl->push_back(then_stm);
        sl->push_back(new tree::LabelStm(end_label));
    }

    visit_tree_result = new tree::Seq(sl);
}

// ============================================================
// visit(While*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::While* node) {
    // visit condition first (may allocate labels for &&/||)
    node->exp->accept(*this);
    Tr_Exp* cond_tr = visit_exp_result;
    Tr_cx* cx = make_cx(cond_tr, method_temp_map);

    // allocate test, body, done labels after condition processing
    tree::Label* test_label = method_temp_map->newlabel();
    tree::Label* body_label = method_temp_map->newlabel();
    tree::Label* done_label = method_temp_map->newlabel();
    cx->true_list->patch(body_label);
    cx->false_list->patch(done_label);

    // save old loop labels
    Label* old_continue = continue_label;
    Label* old_break = break_label;
    continue_label = test_label;
    break_label = done_label;

    vector<tree::Stm*>* sl = new vector<tree::Stm*>();
    sl->push_back(new tree::LabelStm(test_label));
    sl->push_back(cx->stm);
    sl->push_back(new tree::LabelStm(body_label));

    // body
    if (node->stm) {
        node->stm->accept(*this);
        if (visit_tree_result) {
            sl->push_back((tree::Stm*)visit_tree_result);
            visit_tree_result = nullptr;
        }
    }

    sl->push_back(new tree::Jump(test_label));
    sl->push_back(new tree::LabelStm(done_label));

    // restore loop labels
    continue_label = old_continue;
    break_label = old_break;

    visit_tree_result = new tree::Seq(sl);
}

// ============================================================
// visit(Nested*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Nested* node) {
    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    if (node->sl) {
        for (auto s : *(node->sl)) {
            s->accept(*this);
            if (visit_tree_result) {
                stms->push_back((tree::Stm*)visit_tree_result);
                visit_tree_result = nullptr;
            }
        }
    }
    visit_tree_result = new tree::Seq(stms);
}

// ============================================================
// visit(Return*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Return* node) {
    if (node->exp) {
        node->exp->accept(*this);
        tree::Exp* e = visit_exp_result->unEx(method_temp_map)->exp;
        visit_tree_result = new tree::Return(e);
    } else {
        visit_tree_result = new tree::Return(new tree::Const(0));
    }
}

// ============================================================
// visit(PutInt*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::PutInt* node) {
    node->exp->accept(*this);
    tree::Exp* e = visit_exp_result->unEx(method_temp_map)->exp;
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(e);
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "putint", args));
}

// ============================================================
// visit(PutCh*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::PutCh* node) {
    node->exp->accept(*this);
    tree::Exp* e = visit_exp_result->unEx(method_temp_map)->exp;
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(e);
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "putch", args));
}

// ============================================================
// visit(Continue*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Continue* node) {
    visit_tree_result = new tree::Jump(continue_label);
}

// ============================================================
// visit(Break*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Break* node) {
    visit_tree_result = new tree::Jump(break_label);
}

// ============================================================
// visit(Starttime*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Starttime* node) {
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "starttime", args));
}

// ============================================================
// visit(Stoptime*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Stoptime* node) {
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "stoptime", args));
}

// ============================================================
// visit(IntExp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::IntExp* node) {
    visit_exp_result = new Tr_ex(new tree::Const(node->val));
}

// ============================================================
// visit(IdExp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::IdExp* node) {
    tree::Temp* t = method_var_table->get_var_temp(node->id);
    tree::Type tp = method_var_table->get_var_type(node->id);
    visit_exp_result = new Tr_ex(new tree::TempExp(tp, t));
}

// ============================================================
// visit(OpExp*) — not directly used, handled in BinaryOp
// ============================================================
void ASTToTreeVisitor::visit(fdmj::OpExp* node) {
    // OpExp is accessed via BinaryOp/UnaryOp, not visited directly
}

// ============================================================
// visit(BinaryOp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::BinaryOp* node) {
    string op = node->op->op;

    if (op == "&&") {
        // short-circuit AND
        node->left->accept(*this);
        Tr_cx* left_cx = make_cx(visit_exp_result, method_temp_map);
        tree::Label* mid_label = method_temp_map->newlabel();
        left_cx->true_list->patch(mid_label);

        node->right->accept(*this);
        Tr_cx* right_cx = make_cx(visit_exp_result, method_temp_map);

        vector<tree::Stm*>* sl = new vector<tree::Stm*>();
        sl->push_back(left_cx->stm);
        sl->push_back(new tree::LabelStm(mid_label));
        sl->push_back(right_cx->stm);

        Patch_list* true_list = right_cx->true_list;
        Patch_list* false_list = left_cx->false_list;
        false_list->add(right_cx->false_list);

        visit_exp_result = new Tr_cx(true_list, false_list, new tree::Seq(sl));
        return;
    }

    if (op == "||") {
        // short-circuit OR
        node->left->accept(*this);
        Tr_cx* left_cx = make_cx(visit_exp_result, method_temp_map);
        tree::Label* mid_label = method_temp_map->newlabel();
        left_cx->false_list->patch(mid_label);

        node->right->accept(*this);
        Tr_cx* right_cx = make_cx(visit_exp_result, method_temp_map);

        vector<tree::Stm*>* sl = new vector<tree::Stm*>();
        sl->push_back(left_cx->stm);
        sl->push_back(new tree::LabelStm(mid_label));
        sl->push_back(right_cx->stm);

        Patch_list* true_list = left_cx->true_list;
        true_list->add(right_cx->true_list);
        Patch_list* false_list = right_cx->false_list;

        visit_exp_result = new Tr_cx(true_list, false_list, new tree::Seq(sl));
        return;
    }

    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") {
        // comparison — produce Tr_cx with placeholder labels
        node->left->accept(*this);
        tree::Exp* left_e = visit_exp_result->unEx(method_temp_map)->exp;
        node->right->accept(*this);
        tree::Exp* right_e = visit_exp_result->unEx(method_temp_map)->exp;

        Patch_list* t = new Patch_list();
        Patch_list* f = new Patch_list();
        tree::Label* tl = new tree::Label(-1);
        tree::Label* fl = new tree::Label(-1);
        t->add_patch(tl);
        f->add_patch(fl);
        visit_exp_result = new Tr_cx(t, f, new tree::Cjump(op, left_e, right_e, tl, fl));
        return;
    }

    // arithmetic: +, -, *, /
    node->left->accept(*this);
    tree::Exp* left_e = visit_exp_result->unEx(method_temp_map)->exp;
    node->right->accept(*this);
    tree::Exp* right_e = visit_exp_result->unEx(method_temp_map)->exp;
    visit_exp_result = new Tr_ex(new tree::Binop(tree::Type::INT, op, left_e, right_e));
}

// ============================================================
// visit(UnaryOp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::UnaryOp* node) {
    string op = node->op->op;
    node->exp->accept(*this);
    if (op == "-") {
        tree::Exp* e = visit_exp_result->unEx(method_temp_map)->exp;
        visit_exp_result = new Tr_ex(new tree::Binop(tree::Type::INT, "-", new tree::Const(0), e));
    } else if (op == "!") {
        // NOT: swap true/false lists
        Tr_cx* cx = make_cx(visit_exp_result, method_temp_map);
        visit_exp_result = new Tr_cx(cx->false_list, cx->true_list, cx->stm);
    }
}

// ============================================================
// visit(GetInt*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::GetInt* node) {
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    visit_exp_result = new Tr_ex(new tree::ExtCall(tree::Type::INT, "getint", args));
}

// ============================================================
// visit(GetCh*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::GetCh* node) {
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    visit_exp_result = new Tr_ex(new tree::ExtCall(tree::Type::INT, "getch", args));
}

// ============================================================
// Stub methods — not needed for HW3
// ============================================================
void ASTToTreeVisitor::visit(fdmj::ClassDecl* node) {}
void ASTToTreeVisitor::visit(fdmj::MethodDecl* node) {}
void ASTToTreeVisitor::visit(fdmj::Formal* node) {}
void ASTToTreeVisitor::visit(fdmj::Type* node) {}
void ASTToTreeVisitor::visit(fdmj::CallStm* node) {}
void ASTToTreeVisitor::visit(fdmj::CallExp* node) {}
void ASTToTreeVisitor::visit(fdmj::ClassVar* node) {}
void ASTToTreeVisitor::visit(fdmj::This* node) {}
void ASTToTreeVisitor::visit(fdmj::Length* node) {}
void ASTToTreeVisitor::visit(fdmj::NewArray* node) {}
void ASTToTreeVisitor::visit(fdmj::NewObject* node) {}
void ASTToTreeVisitor::visit(fdmj::ArrayExp* node) {}
void ASTToTreeVisitor::visit(fdmj::PutArray* node) {}
void ASTToTreeVisitor::visit(fdmj::GetArray* node) {}

