#define DEBUG
//#undef DEBUG

#include <algorithm> // IWYU pragma: keep
#include <iostream> // IWYU pragma: keep
#include <map> // IWYU pragma: keep
#include <set>
#include <string>
#include <variant>
#include <vector>
#include "ASTheader.hh" // IWYU pragma: keep
#include "FDMJAST.hh" // IWYU pragma: keep
#include "ast2tree.hh" // IWYU pragma: keep
#include "config.hh" // IWYU pragma: keep
#include "temp.hh" // IWYU pragma: keep
#include "treep.hh" // IWYU pragma: keep

using namespace std;

namespace {

constexpr const char* kMainClass = "__$main__";
constexpr const char* kMainMethod = "main";
constexpr const char* kThisName = "_^this^_";

tree::Type ast_type_to_tree_type(fdmj::Type* type) {
    if (type == nullptr) {
        return tree::Type::INT;
    }
    return type->typeKind == TypeKind::INT ? tree::Type::INT : tree::Type::PTR;
}

tree::Type semant_to_tree_type(AST_Semant* semant) {
    if (semant == nullptr) {
        return tree::Type::INT;
    }
    return semant->get_type() == TypeKind::INT ? tree::Type::INT : tree::Type::PTR;
}

string semant_class_name(AST_Semant* semant) {
    if (semant == nullptr || semant->get_type() != TypeKind::CLASS) {
        return "";
    }
    variant<monostate, string, int> par = semant->get_type_par();
    return holds_alternative<string>(par) ? get<string>(par) : "";
}

bool is_simple_exp(tree::Exp* exp) {
    return dynamic_cast<tree::Const*>(exp) != nullptr ||
           dynamic_cast<tree::TempExp*>(exp) != nullptr;
}

tree::Stm* build_stm(vector<tree::Stm*>* stms, bool force_seq = false) {
    if (stms == nullptr || stms->empty()) {
        return nullptr;
    }
    if (!force_seq && stms->size() == 1) {
        return stms->front();
    }
    return new tree::Seq(stms);
}

tree::Exp* wrap_exp_with_setup(tree::Type type, vector<tree::Stm*>* setup,
                               tree::Exp* exp, bool force_seq = false) {
    if (setup == nullptr || setup->empty()) {
        return exp;
    }
    return new tree::Eseq(type, build_stm(setup, force_seq), exp);
}

tree::Binop* record_addr(tree::Exp* base, int offset) {
    return new tree::Binop(tree::Type::PTR, "+", base, new tree::Const(offset));
}

tree::Mem* record_mem(tree::Type type, tree::Exp* base, int offset) {
    return new tree::Mem(type, record_addr(base, offset));
}

tree::ExpStm* exit_on_bounds_error() {
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(new tree::Const(-1));
    return new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "exit", args));
}

tree::Exp* ensure_simple_exp(tree::Type type, tree::Exp* exp, Temp_map* tm,
                             vector<tree::Stm*>* setup) {
    if (is_simple_exp(exp)) {
        return exp;
    }
    tree::Temp* temp = tm->newtemp();
    setup->push_back(new tree::Move(new tree::TempExp(type, temp), exp));
    return new tree::TempExp(type, temp);
}

tree::Exp* build_array_checked_index(tree::Exp* array_base, tree::Exp* index_value,
                                     Temp_map* tm) {
    vector<tree::Stm*>* checks = new vector<tree::Stm*>();
    tree::Temp* length_temp = tm->newtemp();
    tree::TempExp* length_exp = new tree::TempExp(tree::Type::INT, length_temp);

    tree::Label* fail = tm->newlabel();
    tree::Label* non_negative = tm->newlabel();
    tree::Label* done = tm->newlabel();

    checks->push_back(new tree::Move(length_exp, new tree::Mem(tree::Type::INT, array_base)));
    checks->push_back(new tree::Cjump(">=", index_value, new tree::Const(0), non_negative, fail));
    checks->push_back(new tree::LabelStm(non_negative));
    checks->push_back(new tree::Cjump(">=", index_value, length_exp, fail, done));
    checks->push_back(new tree::LabelStm(fail));
    checks->push_back(exit_on_bounds_error());
    checks->push_back(new tree::LabelStm(done));

    return new tree::Eseq(tree::Type::INT, new tree::Seq(checks), index_value);
}

tree::Exp* build_array_access(tree::Exp* array_exp, tree::Exp* index_exp, Temp_map* tm) {
    vector<tree::Stm*>* setup = new vector<tree::Stm*>();
    tree::Exp* array_base = ensure_simple_exp(tree::Type::PTR, array_exp, tm, setup);
    tree::Exp* index_value = ensure_simple_exp(tree::Type::INT, index_exp, tm, setup);
    tree::Exp* checked_index = build_array_checked_index(array_base, index_value, tm);

    tree::Exp* elem_offset = new tree::Binop(
        tree::Type::INT, "*",
        new tree::Binop(tree::Type::INT, "+", checked_index, new tree::Const(1)),
        new tree::Const(4));
    tree::Exp* address = new tree::Binop(tree::Type::PTR, "+", array_base, elem_offset);
    tree::Exp* mem = new tree::Mem(tree::Type::INT, address);
    return wrap_exp_with_setup(tree::Type::INT, setup, mem, true);
}

tree::Exp* build_length_exp(tree::Exp* array_exp, Temp_map* tm) {
    vector<tree::Stm*>* setup = new vector<tree::Stm*>();
    tree::Exp* array_base = ensure_simple_exp(tree::Type::PTR, array_exp, tm, setup);
    tree::Temp* length_temp = tm->newtemp();
    tree::TempExp* length_exp = new tree::TempExp(tree::Type::INT, length_temp);
    setup->push_back(new tree::Move(length_exp, new tree::Mem(tree::Type::INT, array_base)));
    return wrap_exp_with_setup(tree::Type::INT, setup, length_exp);
}

Tr_cx* make_cx(Tr_Exp* tr, Temp_map* tm) {
    Tr_cx* cx = dynamic_cast<Tr_cx*>(tr);
    if (cx != nullptr) {
        return cx;
    }

    Tr_ex* ex = dynamic_cast<Tr_ex*>(tr);
    if (ex == nullptr) {
        return tr->unCx(tm);
    }

    Patch_list* true_list = new Patch_list();
    Patch_list* false_list = new Patch_list();
    tree::Label* true_label = new tree::Label(-1);
    tree::Label* false_label = new tree::Label(-1);
    true_list->add_patch(true_label);
    false_list->add_patch(false_label);
    return new Tr_cx(true_list, false_list,
                     new tree::Cjump("!=", ex->exp, new tree::Const(0),
                                     true_label, false_label));
}

string resolve_field_owner(const string& class_name, const string& field_name, Name_Maps* nm,
                           Class_table* class_table) {
    string current = class_name;
    while (!current.empty()) {
        if (class_table->has_var(current, field_name)) {
            return current;
        }
        current = nm->get_parent(current);
    }
    return "";
}

string resolve_method_owner(const string& class_name, const string& method_name, Name_Maps* nm) {
    string current = class_name;
    while (!current.empty()) {
        if (nm->is_method(current, method_name)) {
            return current;
        }
        current = nm->get_parent(current);
    }
    return "";
}

int object_size_bytes(Class_table* class_table) {
    return static_cast<int>(4 * (class_table->var_pos_map.size() + class_table->method_pos_map.size()));
}

} // namespace

// ============================================================
// Helper: generate_method_var_table
// ============================================================
Method_var_table* generate_method_var_table(string class_name, string method_name, Name_Maps* nm,
                                            Temp_map* tm) {
    Method_var_table* mvt = new Method_var_table();

    set<string>* local_names = nm->get_method_var_list(class_name, method_name);
    if (local_names != nullptr) {
        for (const string& name : *local_names) {
            tree::Temp* temp = tm->newtemp();
            (*mvt->var_temp_map)[name] = temp;
            VarDecl* decl = nm->get_method_var(class_name, method_name, name);
            (*mvt->var_type_map)[name] = decl == nullptr ? tree::Type::INT : ast_type_to_tree_type(decl->type);
        }
    }

    if (class_name != kMainClass) {
        tree::Temp* this_temp = tm->newtemp();
        (*mvt->var_temp_map)[kThisName] = this_temp;
        (*mvt->var_type_map)[kThisName] = tree::Type::PTR;
    }

    vector<string>* formal_names = nm->get_method_formal_list_string(class_name, method_name);
    if (formal_names != nullptr) {
        for (const string& name : *formal_names) {
            if (mvt->var_temp_map->find(name) != mvt->var_temp_map->end()) {
                continue;
            }
            tree::Temp* temp = tm->newtemp();
            (*mvt->var_temp_map)[name] = temp;
            Formal* formal = nm->get_method_formal(class_name, method_name, name);
            (*mvt->var_type_map)[name] = formal == nullptr ? tree::Type::INT : ast_type_to_tree_type(formal->type);
        }
    }

    return mvt;
}

// ============================================================
// generate_class_table
// ============================================================
Class_table* generate_class_table(AST_Semant_Map* semant_map) {
    Class_table* class_table = new Class_table();
    Name_Maps* nm = semant_map->getNameMaps();
    if (nm == nullptr) {
        return class_table;
    }

    set<string> var_keys;
    set<string> method_names;
    set<string>* classes = nm->get_class_list();
    if (classes != nullptr) {
        for (const string& class_name : *classes) {
            if (class_name == kMainClass) {
                continue;
            }
            set<string>* vars = nm->get_class_var_list(class_name);
            if (vars != nullptr) {
                for (const string& var_name : *vars) {
                    var_keys.insert(class_name + "^" + var_name);
                }
            }
            set<string>* methods = nm->get_method_list(class_name);
            if (methods != nullptr) {
                for (const string& method_name : *methods) {
                    method_names.insert(method_name);
                }
            }
        }
    }

    int offset = 0;
    for (const string& key : var_keys) {
        class_table->var_pos_map[key] = offset;
        offset += 4;
    }
    for (const string& method_name : method_names) {
        class_table->method_pos_map[method_name] = offset;
        offset += 4;
    }

    return class_table;
}

// ============================================================
// ast2tree — entry point
// ============================================================
tree::Program* ast2tree(fdmj::Program* prog, AST_Semant_Map* semant_map) {
    ASTToTreeVisitor visitor;
    visitor.semant_map = semant_map;
    visitor.class_table = generate_class_table(semant_map);
    prog->accept(visitor);
    return static_cast<tree::Program*>(visitor.getTree());
}

// ============================================================
// visit(Program*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Program* node) {
    vector<tree::FuncDecl*>* funcs = new vector<tree::FuncDecl*>();

    if (node->main != nullptr) {
        node->main->accept(*this);
        if (visit_tree_result != nullptr) {
            funcs->push_back(static_cast<tree::FuncDecl*>(visit_tree_result));
        }
    }

    if (node->cdl != nullptr) {
        string saved_class = current_class;
        for (fdmj::ClassDecl* class_decl : *node->cdl) {
            current_class = class_decl->id->id;
            if (class_decl->mdl != nullptr) {
                for (fdmj::MethodDecl* method_decl : *class_decl->mdl) {
                    method_decl->accept(*this);
                    if (visit_tree_result != nullptr) {
                        funcs->push_back(static_cast<tree::FuncDecl*>(visit_tree_result));
                    }
                }
            }
        }
        current_class = saved_class;
    }

    visit_tree_result = new tree::Program(funcs);
}

// ============================================================
// visit(MainMethod*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::MainMethod* node) {
    current_class = kMainClass;
    current_method = kMainMethod;

    delete method_temp_map;
    delete method_var_table;
    method_temp_map = new Temp_map();
    method_var_table = generate_method_var_table(current_class, current_method,
                                                 semant_map->getNameMaps(), method_temp_map);
    continue_label = nullptr;
    break_label = nullptr;

    vector<tree::Stm*>* stms = new vector<tree::Stm*>();

    if (node->vdl != nullptr) {
        for (fdmj::VarDecl* decl : *node->vdl) {
            decl->accept(*this);
            if (visit_tree_result != nullptr) {
                tree::Seq* seq = dynamic_cast<tree::Seq*>(visit_tree_result);
                if (seq != nullptr && seq->sl != nullptr) {
                    stms->insert(stms->end(), seq->sl->begin(), seq->sl->end());
                } else {
                    stms->push_back(static_cast<tree::Stm*>(visit_tree_result));
                }
            }
        }
    }

    if (node->sl != nullptr) {
        for (fdmj::Stm* stm : *node->sl) {
            stm->accept(*this);
            if (visit_tree_result != nullptr) {
                stms->push_back(static_cast<tree::Stm*>(visit_tree_result));
            }
        }
    }

    tree::Stm* body = build_stm(stms, true);
    int last_temp = method_temp_map->next_temp - 1;
    int last_label = method_temp_map->next_label - 1;
    visit_tree_result = new tree::FuncDecl(string(kMainClass) + "^" + kMainMethod, nullptr,
                                           body, tree::Type::INT, last_temp, last_label);
}

// ============================================================
// visit(MethodDecl*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::MethodDecl* node) {
    current_method = node->id->id;

    delete method_temp_map;
    delete method_var_table;
    method_temp_map = new Temp_map();
    method_var_table = generate_method_var_table(current_class, current_method,
                                                 semant_map->getNameMaps(), method_temp_map);
    continue_label = nullptr;
    break_label = nullptr;

    vector<tree::Temp*>* args = new vector<tree::Temp*>();
    args->push_back(method_var_table->get_var_temp(kThisName));

    vector<string>* formals = semant_map->getNameMaps()->get_method_formal_list_string(current_class, current_method);
    if (formals != nullptr) {
        for (const string& name : *formals) {
            if (name.rfind("_^return^_", 0) == 0) {
                continue;
            }
            args->push_back(method_var_table->get_var_temp(name));
        }
    }
    if (args->empty()) {
        delete args;
        args = nullptr;
    }

    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    if (node->vdl != nullptr) {
        for (fdmj::VarDecl* decl : *node->vdl) {
            decl->accept(*this);
            if (visit_tree_result != nullptr) {
                tree::Seq* seq = dynamic_cast<tree::Seq*>(visit_tree_result);
                if (seq != nullptr && seq->sl != nullptr) {
                    stms->insert(stms->end(), seq->sl->begin(), seq->sl->end());
                } else {
                    stms->push_back(static_cast<tree::Stm*>(visit_tree_result));
                }
            }
        }
    }
    if (node->sl != nullptr) {
        for (fdmj::Stm* stm : *node->sl) {
            stm->accept(*this);
            if (visit_tree_result != nullptr) {
                stms->push_back(static_cast<tree::Stm*>(visit_tree_result));
            }
        }
    }

    tree::Stm* body = build_stm(stms, true);
    int last_temp = method_temp_map->next_temp - 1;
    int last_label = method_temp_map->next_label - 1;
    visit_tree_result = new tree::FuncDecl(current_class + "^" + current_method, args, body,
                                           ast_type_to_tree_type(node->type), last_temp, last_label);
}

// ============================================================
// visit(VarDecl*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::VarDecl* node) {
    visit_tree_result = nullptr;
    tree::Temp* temp = method_var_table->get_var_temp(node->id->id);
    if (temp == nullptr || node->type == nullptr) {
        return;
    }

    tree::Type var_type = ast_type_to_tree_type(node->type);

    if (node->type->typeKind == TypeKind::CLASS) {
        if (holds_alternative<monostate>(node->init)) {
            visit_tree_result = new tree::Move(new tree::TempExp(tree::Type::PTR, temp), new tree::Const(0));
        }
        return;
    }

    if (node->type->typeKind == TypeKind::INT) {
        if (holds_alternative<fdmj::IntExp*>(node->init)) {
            fdmj::IntExp* init = get<fdmj::IntExp*>(node->init);
            if (init != nullptr) {
                visit_tree_result = new tree::Move(new tree::TempExp(var_type, temp),
                                                   new tree::Const(init->val));
            }
        }
        return;
    }

    if (!holds_alternative<vector<fdmj::IntExp*>*>(node->init)) {
        return;
    }

    vector<fdmj::IntExp*>* init_list = get<vector<fdmj::IntExp*>*>(node->init);
    if (init_list == nullptr) {
        return;
    }

    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    int length = static_cast<int>(init_list->size());
    vector<tree::Exp*>* malloc_args = new vector<tree::Exp*>();
    malloc_args->push_back(new tree::Const((length + 1) * 4));
    tree::TempExp* array_temp = new tree::TempExp(tree::Type::PTR, temp);

    stms->push_back(new tree::Move(array_temp,
                                   new tree::ExtCall(tree::Type::PTR, "malloc", malloc_args)));
    stms->push_back(new tree::Move(new tree::Mem(tree::Type::INT, array_temp), new tree::Const(length)));
    for (int i = 0; i < length; ++i) {
        stms->push_back(new tree::Move(
            new tree::Mem(tree::Type::INT,
                          new tree::Binop(tree::Type::PTR, "+", array_temp, new tree::Const((i + 1) * 4))),
            new tree::Const((*init_list)[i]->val)));
    }
    visit_tree_result = build_stm(stms, true);
}

// ============================================================
// visit(Nested*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Nested* node) {
    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    if (node->sl != nullptr) {
        for (fdmj::Stm* stm : *node->sl) {
            stm->accept(*this);
            if (visit_tree_result != nullptr) {
                stms->push_back(static_cast<tree::Stm*>(visit_tree_result));
            }
        }
    }
    visit_tree_result = build_stm(stms, true);
}

// ============================================================
// visit(If*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::If* node) {
    node->exp->accept(*this);
    Tr_cx* cond = make_cx(visit_exp_result, method_temp_map);
    vector<tree::Stm*>* stms = new vector<tree::Stm*>();

    node->stm1->accept(*this);
    tree::Stm* then_stm = static_cast<tree::Stm*>(visit_tree_result);
    tree::Stm* else_stm = nullptr;

    if (node->stm2 != nullptr) {
        node->stm2->accept(*this);
        else_stm = static_cast<tree::Stm*>(visit_tree_result);
    }

    tree::Label* then_label = method_temp_map->newlabel();
    tree::Label* else_label = method_temp_map->newlabel();
    tree::Label* end_label = method_temp_map->newlabel();

    cond->true_list->patch(then_label);
    cond->false_list->patch(else_label);

    stms->push_back(cond->stm);
    stms->push_back(new tree::LabelStm(then_label));
    if (then_stm != nullptr) {
        stms->push_back(then_stm);
    }

    stms->push_back(new tree::Jump(end_label));
    stms->push_back(new tree::LabelStm(else_label));
    if (else_stm != nullptr) {
        stms->push_back(else_stm);
    }

    stms->push_back(new tree::LabelStm(end_label));
    visit_tree_result = new tree::Seq(stms);
}

// ============================================================
// visit(While*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::While* node) {
    node->exp->accept(*this);
    Tr_cx* cond = make_cx(visit_exp_result, method_temp_map);

    tree::Label* test_label = method_temp_map->newlabel();
    tree::Label* body_label = method_temp_map->newlabel();
    tree::Label* done_label = method_temp_map->newlabel();
    cond->true_list->patch(body_label);
    cond->false_list->patch(done_label);

    tree::Label* old_continue = continue_label;
    tree::Label* old_break = break_label;
    continue_label = test_label;
    break_label = done_label;

    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    stms->push_back(new tree::LabelStm(test_label));
    stms->push_back(cond->stm);
    stms->push_back(new tree::LabelStm(body_label));

    if (node->stm != nullptr) {
        node->stm->accept(*this);
        if (visit_tree_result != nullptr) {
            stms->push_back(static_cast<tree::Stm*>(visit_tree_result));
        }
    }

    stms->push_back(new tree::Jump(test_label));
    stms->push_back(new tree::LabelStm(done_label));

    continue_label = old_continue;
    break_label = old_break;
    visit_tree_result = new tree::Seq(stms);
}

// ============================================================
// visit(Assign*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Assign* node) {
    node->left->accept(*this);
    tree::Exp* dst = visit_exp_result->unEx(method_temp_map)->exp;
    node->exp->accept(*this);
    tree::Exp* src = visit_exp_result->unEx(method_temp_map)->exp;
    visit_tree_result = new tree::Move(dst, src);
}

// ============================================================
// visit(CallStm*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::CallStm* node) {
    node->obj->accept(*this);
    tree::Exp* obj_exp = visit_exp_result->unEx(method_temp_map)->exp;
    vector<tree::Stm*>* setup = new vector<tree::Stm*>();
    tree::Exp* obj_base = ensure_simple_exp(tree::Type::PTR, obj_exp, method_temp_map, setup);

    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(obj_base);
    if (node->par != nullptr) {
        for (fdmj::Exp* exp : *node->par) {
            exp->accept(*this);
            args->push_back(visit_exp_result->unEx(method_temp_map)->exp);
        }
    }

    tree::Call* call = new tree::Call(
        tree::Type::INT, node->name->id,
        record_mem(tree::Type::PTR, obj_base, class_table->get_method_pos(node->name->id)), args);
    tree::Exp* call_exp = wrap_exp_with_setup(tree::Type::INT, setup, call);
    visit_tree_result = new tree::ExpStm(call_exp);
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
// visit(Return*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Return* node) {
    if (node->exp == nullptr) {
        visit_tree_result = new tree::Return(new tree::Const(0));
        return;
    }
    node->exp->accept(*this);
    visit_tree_result = new tree::Return(visit_exp_result->unEx(method_temp_map)->exp);
}

// ============================================================
// visit(PutInt*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::PutInt* node) {
    node->exp->accept(*this);
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(visit_exp_result->unEx(method_temp_map)->exp);
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "putint", args));
}

// ============================================================
// visit(PutCh*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::PutCh* node) {
    node->exp->accept(*this);
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(visit_exp_result->unEx(method_temp_map)->exp);
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "putch", args));
}

// ============================================================
// visit(PutArray*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::PutArray* node) {
    node->n->accept(*this);
    tree::Exp* n_exp = visit_exp_result->unEx(method_temp_map)->exp;
    node->arr->accept(*this);
    tree::Exp* arr_exp = visit_exp_result->unEx(method_temp_map)->exp;

    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(n_exp);
    args->push_back(arr_exp);
    visit_tree_result = new tree::ExpStm(new tree::ExtCall(tree::Type::INT, "putarray", args));
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
    tree::Temp* temp = method_var_table->get_var_temp(node->id);
    tree::Type type = method_var_table->get_var_type(node->id);
    visit_exp_result = new Tr_ex(new tree::TempExp(type, temp));
}

// ============================================================
// visit(OpExp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::OpExp* node) {
    visit_exp_result = nullptr;
}

// ============================================================
// visit(BinaryOp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::BinaryOp* node) {
    string op = node->op->op;

    if (op == "&&") {
        node->left->accept(*this);
        Tr_cx* left = make_cx(visit_exp_result, method_temp_map);
        tree::Label* mid = method_temp_map->newlabel();
        left->true_list->patch(mid);

        node->right->accept(*this);
        Tr_cx* right = make_cx(visit_exp_result, method_temp_map);

        vector<tree::Stm*>* stms = new vector<tree::Stm*>();
        stms->push_back(left->stm);
        stms->push_back(new tree::LabelStm(mid));
        stms->push_back(right->stm);

        Patch_list* false_list = left->false_list;
        false_list->add(right->false_list);
        visit_exp_result = new Tr_cx(right->true_list, false_list, new tree::Seq(stms));
        return;
    }

    if (op == "||") {
        node->left->accept(*this);
        Tr_cx* left = make_cx(visit_exp_result, method_temp_map);
        tree::Label* mid = method_temp_map->newlabel();
        left->false_list->patch(mid);

        node->right->accept(*this);
        Tr_cx* right = make_cx(visit_exp_result, method_temp_map);

        vector<tree::Stm*>* stms = new vector<tree::Stm*>();
        stms->push_back(left->stm);
        stms->push_back(new tree::LabelStm(mid));
        stms->push_back(right->stm);

        Patch_list* true_list = left->true_list;
        true_list->add(right->true_list);
        visit_exp_result = new Tr_cx(true_list, right->false_list, new tree::Seq(stms));
        return;
    }

    node->left->accept(*this);
    tree::Exp* left = visit_exp_result->unEx(method_temp_map)->exp;
    node->right->accept(*this);
    tree::Exp* right = visit_exp_result->unEx(method_temp_map)->exp;

    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") {
        Patch_list* true_list = new Patch_list();
        Patch_list* false_list = new Patch_list();
        tree::Label* true_label = new tree::Label(-1);
        tree::Label* false_label = new tree::Label(-1);
        true_list->add_patch(true_label);
        false_list->add_patch(false_label);
        visit_exp_result = new Tr_cx(true_list, false_list,
                                     new tree::Cjump(op, left, right, true_label, false_label));
        return;
    }

    visit_exp_result = new Tr_ex(new tree::Binop(tree::Type::INT, op, left, right));
}

// ============================================================
// visit(UnaryOp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::UnaryOp* node) {
    string op = node->op->op;
    node->exp->accept(*this);

    if (op == "-") {
        visit_exp_result = new Tr_ex(new tree::Binop(
            tree::Type::INT, "-", new tree::Const(0), visit_exp_result->unEx(method_temp_map)->exp));
        return;
    }

    if (op == "!") {
        Tr_cx* cond = make_cx(visit_exp_result, method_temp_map);
        visit_exp_result = new Tr_cx(cond->false_list, cond->true_list, cond->stm);
    }
}

// ============================================================
// visit(ArrayExp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::ArrayExp* node) {
    node->arr->accept(*this);
    tree::Exp* array_exp = visit_exp_result->unEx(method_temp_map)->exp;
    node->index->accept(*this);
    tree::Exp* index_exp = visit_exp_result->unEx(method_temp_map)->exp;
    visit_exp_result = new Tr_ex(build_array_access(array_exp, index_exp, method_temp_map));
}

// ============================================================
// visit(CallExp*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::CallExp* node) {
    node->obj->accept(*this);
    tree::Exp* obj_exp = visit_exp_result->unEx(method_temp_map)->exp;
    vector<tree::Stm*>* setup = new vector<tree::Stm*>();
    tree::Exp* obj_base = ensure_simple_exp(tree::Type::PTR, obj_exp, method_temp_map, setup);

    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(obj_base);
    if (node->par != nullptr) {
        for (fdmj::Exp* exp : *node->par) {
            exp->accept(*this);
            args->push_back(visit_exp_result->unEx(method_temp_map)->exp);
        }
    }

    AST_Semant* semant = semant_map->getSemant(node);
    tree::Call* call = new tree::Call(
        semant_to_tree_type(semant), node->name->id,
        record_mem(tree::Type::PTR, obj_base, class_table->get_method_pos(node->name->id)), args);
    visit_exp_result = new Tr_ex(wrap_exp_with_setup(semant_to_tree_type(semant), setup, call));
}

// ============================================================
// visit(ClassVar*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::ClassVar* node) {
    node->obj->accept(*this);
    tree::Exp* obj_exp = visit_exp_result->unEx(method_temp_map)->exp;
    AST_Semant* obj_semant = semant_map->getSemant(node->obj);
    string owner = resolve_field_owner(semant_class_name(obj_semant), node->id->id,
                                       semant_map->getNameMaps(), class_table);
    int offset = class_table->get_var_pos(owner, node->id->id);
    AST_Semant* semant = semant_map->getSemant(node);
    visit_exp_result = new Tr_ex(record_mem(semant_to_tree_type(semant), obj_exp, offset));
}

// ============================================================
// visit(This*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::This* node) {
    visit_exp_result = new Tr_ex(
        new tree::TempExp(tree::Type::PTR, method_var_table->get_var_temp(kThisName)));
}

// ============================================================
// visit(Length*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::Length* node) {
    node->exp->accept(*this);
    visit_exp_result = new Tr_ex(build_length_exp(visit_exp_result->unEx(method_temp_map)->exp,
                                                  method_temp_map));
}

// ============================================================
// visit(NewArray*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::NewArray* node) {
    node->size->accept(*this);
    tree::Exp* size_exp = visit_exp_result->unEx(method_temp_map)->exp;

    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    tree::Temp* array_temp = method_temp_map->newtemp();
    tree::TempExp* array_exp = new tree::TempExp(tree::Type::PTR, array_temp);
    vector<tree::Exp*>* malloc_args = new vector<tree::Exp*>();
    malloc_args->push_back(new tree::Binop(
        tree::Type::INT, "*",
        new tree::Binop(tree::Type::INT, "+", size_exp, new tree::Const(1)),
        new tree::Const(4)));

    stms->push_back(new tree::Move(array_exp,
                                   new tree::ExtCall(tree::Type::PTR, "malloc", malloc_args)));
    stms->push_back(new tree::Move(new tree::Mem(tree::Type::INT, array_exp), size_exp));

    visit_exp_result = new Tr_ex(
        new tree::Eseq(tree::Type::PTR, new tree::Seq(stms), array_exp));
}

// ============================================================
// visit(NewObject*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::NewObject* node) {
    string class_name = node->id->id;
    vector<tree::Stm*>* stms = new vector<tree::Stm*>();
    tree::Temp* object_temp = method_temp_map->newtemp();
    tree::TempExp* object_exp = new tree::TempExp(tree::Type::PTR, object_temp);

    vector<tree::Exp*>* malloc_args = new vector<tree::Exp*>();
    malloc_args->push_back(new tree::Const(object_size_bytes(class_table)));
    stms->push_back(new tree::Move(object_exp,
                                   new tree::ExtCall(tree::Type::PTR, "malloc", malloc_args)));

    for (const auto& entry : class_table->method_pos_map) {
        string method_name = entry.first;
        string impl_class = resolve_method_owner(class_name, method_name, semant_map->getNameMaps());
        if (impl_class.empty()) {
            continue;
        }
        stms->push_back(new tree::Move(
            record_mem(tree::Type::PTR, object_exp, entry.second),
            new tree::Name(method_temp_map->newstringlabel(impl_class + "^" + method_name))));
    }

    visit_exp_result = new Tr_ex(
        new tree::Eseq(tree::Type::PTR, new tree::Seq(stms), object_exp));
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
// visit(GetArray*)
// ============================================================
void ASTToTreeVisitor::visit(fdmj::GetArray* node) {
    node->exp->accept(*this);
    vector<tree::Exp*>* args = new vector<tree::Exp*>();
    args->push_back(visit_exp_result->unEx(method_temp_map)->exp);
    visit_exp_result = new Tr_ex(new tree::ExtCall(tree::Type::INT, "getarray", args));
}

// ============================================================
// Stub methods that are not used directly during translation
// ============================================================
void ASTToTreeVisitor::visit(fdmj::ClassDecl* node) {}
void ASTToTreeVisitor::visit(fdmj::Formal* node) {}
void ASTToTreeVisitor::visit(fdmj::Type* node) {}
