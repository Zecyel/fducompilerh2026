#define DEBUG
#undef DEBUG

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "namemaps.hh"
#include "semant.hh"

using namespace std;
using namespace fdmj;

namespace {

const string kMainClass = "__main__";
const string kMainMethod = "main";
const string kReturnFormal = "__$ret$__";

[[noreturn]] void semant_error(AST *node, const string &msg) {
    if (node != nullptr && node->getPos() != nullptr) {
        cerr << "Semantic error at " << node->getPos()->print() << ": " << msg << endl;
    } else {
        cerr << "Semantic error: " << msg << endl;
    }
    exit(1);
}

variant<monostate, string, int> type_par_from_type(Type *type) {
    if (type == nullptr) {
        return monostate{};
    }
    switch (type->typeKind) {
        case TypeKind::INT:
            return monostate{};
        case TypeKind::CLASS:
            return type->cid == nullptr ? variant<monostate, string, int>(string(""))
                                         : variant<monostate, string, int>(type->cid->id);
        case TypeKind::ARRAY:
            return type->arity == nullptr ? variant<monostate, string, int>(0)
                                          : variant<monostate, string, int>(type->arity->val);
        default:
            return monostate{};
    }
}

AST_Semant *make_value_semant(TypeKind tk, variant<monostate, string, int> tp, bool lvalue) {
    return new AST_Semant(AST_Semant::Kind::Value, tk, tp, lvalue);
}

AST_Semant *make_value_semant(Type *type, bool lvalue) {
    if (type == nullptr) {
        return make_value_semant(TypeKind::INT, monostate{}, lvalue);
    }
    return make_value_semant(type->typeKind, type_par_from_type(type), lvalue);
}

bool is_subclass(Name_Maps *nm, const string &child, const string &parent) {
    if (child == parent) {
        return true;
    }
    string cur = child;
    set<string> visited;
    while (!cur.empty()) {
        if (visited.find(cur) != visited.end()) {
            return false;
        }
        visited.insert(cur);
        cur = nm->get_parent(cur);
        if (cur == parent) {
            return true;
        }
    }
    return false;
}

bool same_type(AST_Semant *a, AST_Semant *b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a->get_type() != b->get_type()) {
        return false;
    }
    switch (a->get_type()) {
        case TypeKind::INT:
            return true;
        case TypeKind::CLASS:
            return get<string>(a->get_type_par()) == get<string>(b->get_type_par());
        case TypeKind::ARRAY:
            return get<int>(a->get_type_par()) == get<int>(b->get_type_par());
        default:
            return false;
    }
}

bool is_type_assignable(Name_Maps *nm, AST_Semant *to, AST_Semant *from) {
    if (to == nullptr || from == nullptr) {
        return false;
    }
    if (to->get_type() != from->get_type()) {
        if (to->get_type() == TypeKind::CLASS && from->get_type() == TypeKind::CLASS) {
            const string &lhs = get<string>(to->get_type_par());
            const string &rhs = get<string>(from->get_type_par());
            return is_subclass(nm, rhs, lhs);
        }
        return false;
    }

    switch (to->get_type()) {
        case TypeKind::INT:
            return true;
        case TypeKind::ARRAY:
            return get<int>(to->get_type_par()) == get<int>(from->get_type_par());
        case TypeKind::CLASS:
            return is_subclass(nm, get<string>(from->get_type_par()), get<string>(to->get_type_par()));
        default:
            return false;
    }
}

bool is_type_decl_valid(Name_Maps *nm, Type *type) {
    if (type == nullptr) {
        return false;
    }
    if (type->typeKind != TypeKind::CLASS) {
        return true;
    }
    return type->cid != nullptr && nm->is_class(type->cid->id);
}

string find_method_owner(Name_Maps *nm, const string &class_name, const string &method_name) {
    string cur = class_name;
    set<string> visited;
    while (!cur.empty()) {
        if (visited.find(cur) != visited.end()) {
            return "";
        }
        visited.insert(cur);
        if (nm->is_method(cur, method_name)) {
            return cur;
        }
        cur = nm->get_parent(cur);
    }
    return "";
}

VarDecl *find_class_var(Name_Maps *nm, const string &class_name, const string &field_name) {
    string cur = class_name;
    set<string> visited;
    while (!cur.empty()) {
        if (visited.find(cur) != visited.end()) {
            return nullptr;
        }
        visited.insert(cur);
        VarDecl *vd = nm->get_class_var(cur, field_name);
        if (vd != nullptr) {
            return vd;
        }
        cur = nm->get_parent(cur);
    }
    return nullptr;
}

bool same_decl_type(Type *a, Type *b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a->typeKind != b->typeKind) {
        return false;
    }
    switch (a->typeKind) {
        case TypeKind::INT:
            return true;
        case TypeKind::ARRAY:
            return (a->arity == nullptr ? 0 : a->arity->val) == (b->arity == nullptr ? 0 : b->arity->val);
        case TypeKind::CLASS:
            if (a->cid == nullptr || b->cid == nullptr) {
                return false;
            }
            return a->cid->id == b->cid->id;
        default:
            return false;
    }
}

bool covariant_return_ok(Name_Maps *nm, Type *child_ret, Type *parent_ret) {
    if (child_ret == nullptr || parent_ret == nullptr) {
        return false;
    }
    if (same_decl_type(child_ret, parent_ret)) {
        return true;
    }
    if (child_ret->typeKind != TypeKind::CLASS || parent_ret->typeKind != TypeKind::CLASS) {
        return false;
    }
    if (child_ret->cid == nullptr || parent_ret->cid == nullptr) {
        return false;
    }
    return is_subclass(nm, child_ret->cid->id, parent_ret->cid->id);
}

bool is_binary_op_int_to_int(const string &op) {
    static const set<string> ops = {
        "+", "-", "*", "/", "%", "||", "&&", "<", ">", "<=", ">=", "==", "!="};
    return ops.find(op) != ops.end();
}

} // namespace

AST_Semant_Map *semant_analyze(Program *node) {
    if (node == nullptr) {
        return nullptr;
    }

    Name_Maps *name_maps = makeNameMaps(node);
    AST_Semant_Visitor semant_visitor(name_maps);
    node->accept(semant_visitor);
    return semant_visitor.getSemantMap();
}

void AST_Semant_Visitor::visit(Program *node) {
#ifdef DEBUG
    std::cout << "Visiting Program" << std::endl;
#endif
    if (node == nullptr) {
        return;
    }

    if (node->main != nullptr) {
        node->main->accept(*this);
    }

    if (node->cdl != nullptr) {
        for (auto *cl : *(node->cdl)) {
            if (cl != nullptr) {
                cl->accept(*this);
            }
        }
    }
}

void AST_Semant_Visitor::visit(MainMethod *node) {
    if (node == nullptr) {
        return;
    }

    string old_class = current_visiting_class;
    string old_method = current_visiting_method;
    current_visiting_class = kMainClass;
    current_visiting_method = kMainMethod;

    if (node->vdl != nullptr) {
        for (auto *vd : *(node->vdl)) {
            if (vd != nullptr) {
                vd->accept(*this);
            }
        }
    }

    if (node->sl != nullptr) {
        for (auto *s : *(node->sl)) {
            if (s != nullptr) {
                s->accept(*this);
            }
        }
    }

    current_visiting_class = old_class;
    current_visiting_method = old_method;
}

void AST_Semant_Visitor::visit(ClassDecl *node) {
    if (node == nullptr || node->id == nullptr) {
        semant_error(node, "invalid class declaration");
    }

    const string class_name = node->id->id;
    const string parent = node->eid == nullptr ? "" : node->eid->id;

    if (!parent.empty()) {
        if (!name_maps->is_class(parent)) {
            semant_error(node, "undefined parent class: " + parent);
        }

        if (!name_maps->get_parent(parent).empty()) {
            semant_error(node, "single-level inheritance violated: class " + parent + " already extends another class");
        }

        string cur = parent;
        set<string> visited;
        while (!cur.empty()) {
            if (cur == class_name) {
                semant_error(node, "circular inheritance involving class " + class_name);
            }
            if (visited.find(cur) != visited.end()) {
                semant_error(node, "circular inheritance detected");
            }
            visited.insert(cur);
            cur = name_maps->get_parent(cur);
        }
    }

    string old_class = current_visiting_class;
    current_visiting_class = class_name;

    if (node->vdl != nullptr) {
        for (auto *vd : *(node->vdl)) {
            if (vd != nullptr) {
                vd->accept(*this);
            }
        }
    }

    if (node->mdl != nullptr) {
        for (auto *md : *(node->mdl)) {
            if (md != nullptr) {
                md->accept(*this);
            }
        }
    }

    current_visiting_class = old_class;
}

void AST_Semant_Visitor::visit(Type *node) {
    if (node == nullptr) {
        semant_error(node, "null type node");
    }
    if (!is_type_decl_valid(name_maps, node)) {
        semant_error(node, "undefined class type");
    }
}

void AST_Semant_Visitor::visit(VarDecl *node) {
    if (node == nullptr || node->type == nullptr) {
        semant_error(node, "invalid variable declaration");
    }
    node->type->accept(*this);

    if (holds_alternative<monostate>(node->init)) {
        return;
    }

    if (holds_alternative<IntExp *>(node->init)) {
        if (node->type->typeKind != TypeKind::INT) {
            semant_error(node, "integer initializer can only be used with int variable");
        }
        return;
    }

    if (holds_alternative<vector<IntExp *> *>(node->init)) {
        if (node->type->typeKind != TypeKind::ARRAY) {
            semant_error(node, "array initializer can only be used with array variable");
        }
    }
}

void AST_Semant_Visitor::visit(MethodDecl *node) {
    if (node == nullptr || node->id == nullptr || node->type == nullptr) {
        semant_error(node, "invalid method declaration");
    }

    node->type->accept(*this);

    const string method_name = node->id->id;
    const string parent = name_maps->get_parent(current_visiting_class);
    if (!parent.empty() && name_maps->is_method(parent, method_name)) {
        vector<Formal *> *child_fl = name_maps->get_method_formal_list(current_visiting_class, method_name);
        vector<Formal *> *parent_fl = name_maps->get_method_formal_list(parent, method_name);

        if (child_fl == nullptr || parent_fl == nullptr || child_fl->empty() || parent_fl->empty()) {
            semant_error(node, "invalid method signature metadata for override check");
        }

        if (child_fl->size() != parent_fl->size()) {
            semant_error(node, "override parameter count mismatch in method " + method_name);
        }

        for (size_t i = 0; i + 1 < child_fl->size(); ++i) {
            if (!same_decl_type((*child_fl)[i]->type, (*parent_fl)[i]->type)) {
                semant_error(node, "override parameter type mismatch in method " + method_name);
            }
        }

        Type *child_ret = child_fl->back()->type;
        Type *parent_ret = parent_fl->back()->type;
        if (!covariant_return_ok(name_maps, child_ret, parent_ret)) {
            semant_error(node, "override return type mismatch in method " + method_name);
        }
    }

    string old_method = current_visiting_method;
    current_visiting_method = method_name;

    if (node->fl != nullptr) {
        for (auto *f : *(node->fl)) {
            if (f != nullptr) {
                f->accept(*this);
            }
        }
    }

    if (node->vdl != nullptr) {
        for (auto *vd : *(node->vdl)) {
            if (vd != nullptr) {
                vd->accept(*this);
            }
        }
    }

    if (node->sl != nullptr) {
        for (auto *s : *(node->sl)) {
            if (s != nullptr) {
                s->accept(*this);
            }
        }
    }

    current_visiting_method = old_method;
}

void AST_Semant_Visitor::visit(Formal *node) {
    if (node == nullptr || node->type == nullptr) {
        semant_error(node, "invalid formal parameter");
    }
    node->type->accept(*this);
}

void AST_Semant_Visitor::visit(Nested *node) {
    if (node == nullptr || node->sl == nullptr) {
        return;
    }
    for (auto *s : *(node->sl)) {
        if (s != nullptr) {
            s->accept(*this);
        }
    }
}

void AST_Semant_Visitor::visit(If *node) {
    if (node == nullptr || node->exp == nullptr || node->stm1 == nullptr) {
        semant_error(node, "invalid if statement");
    }

    node->exp->accept(*this);
    AST_Semant *cond = semant_map->getSemant(node->exp);
    if (cond == nullptr || cond->get_kind() != AST_Semant::Kind::Value || cond->get_type() != TypeKind::INT) {
        semant_error(node->exp, "if condition must be int");
    }

    node->stm1->accept(*this);
    if (node->stm2 != nullptr) {
        node->stm2->accept(*this);
    }
}

void AST_Semant_Visitor::visit(While *node) {
    if (node == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid while statement");
    }

    node->exp->accept(*this);
    AST_Semant *cond = semant_map->getSemant(node->exp);
    if (cond == nullptr || cond->get_kind() != AST_Semant::Kind::Value || cond->get_type() != TypeKind::INT) {
        semant_error(node->exp, "while condition must be int");
    }

    ++in_a_while_loop;
    if (node->stm != nullptr) {
        node->stm->accept(*this);
    }
    --in_a_while_loop;
}

void AST_Semant_Visitor::visit(Assign *node) {
    if (node == nullptr || node->left == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid assignment");
    }

    node->left->accept(*this);
    node->exp->accept(*this);

    AST_Semant *lhs = semant_map->getSemant(node->left);
    AST_Semant *rhs = semant_map->getSemant(node->exp);

    if (lhs == nullptr || rhs == nullptr) {
        semant_error(node, "assignment has unresolved expression type");
    }
    if (lhs->get_kind() != AST_Semant::Kind::Value || rhs->get_kind() != AST_Semant::Kind::Value) {
        semant_error(node, "assignment operands must be values");
    }
    if (!lhs->is_lvalue()) {
        semant_error(node->left, "left-hand side of assignment is not an lvalue");
    }

    if (!is_type_assignable(name_maps, lhs, rhs)) {
        semant_error(node, "assignment type mismatch");
    }
}

void AST_Semant_Visitor::visit(CallStm *node) {
    if (node == nullptr || node->obj == nullptr || node->name == nullptr) {
        semant_error(node, "invalid call statement");
    }

    node->obj->accept(*this);
    AST_Semant *obj_sem = semant_map->getSemant(node->obj);
    if (obj_sem == nullptr || obj_sem->get_kind() != AST_Semant::Kind::Value || obj_sem->get_type() != TypeKind::CLASS) {
        semant_error(node->obj, "method call target must be an object");
    }

    string owner = find_method_owner(name_maps, get<string>(obj_sem->get_type_par()), node->name->id);
    if (owner.empty()) {
        semant_error(node, "undefined method: " + node->name->id);
    }
    semant_map->setSemant(node->name,
                          new AST_Semant(AST_Semant::Kind::MethodName, TypeKind::CLASS, owner, false));

    vector<Formal *> *formals = name_maps->get_method_formal_list(owner, node->name->id);
    if (formals == nullptr || formals->empty()) {
        semant_error(node, "invalid method metadata for call: " + node->name->id);
    }

    size_t expected_args = formals->size() - 1;
    size_t actual_args = node->par == nullptr ? 0 : node->par->size();
    if (expected_args != actual_args) {
        semant_error(node, "method argument count mismatch for " + node->name->id);
    }

    for (size_t i = 0; i < actual_args; ++i) {
        Exp *arg = (*(node->par))[i];
        arg->accept(*this);
        AST_Semant *arg_sem = semant_map->getSemant(arg);
        AST_Semant *formal_sem = make_value_semant((*formals)[i]->type, true);
        if (!is_type_assignable(name_maps, formal_sem, arg_sem)) {
            semant_error(arg, "method argument type mismatch at index " + to_string(i));
        }
    }
}

void AST_Semant_Visitor::visit(Continue *node) {
    if (in_a_while_loop <= 0) {
        semant_error(node, "continue must appear inside while");
    }
}

void AST_Semant_Visitor::visit(Break *node) {
    if (in_a_while_loop <= 0) {
        semant_error(node, "break must appear inside while");
    }
}

void AST_Semant_Visitor::visit(Return *node) {
    if (node == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid return statement");
    }

    node->exp->accept(*this);
    AST_Semant *ret_sem = semant_map->getSemant(node->exp);
    if (ret_sem == nullptr || ret_sem->get_kind() != AST_Semant::Kind::Value) {
        semant_error(node, "invalid return expression");
    }

    Formal *expected = name_maps->get_method_formal(current_visiting_class, current_visiting_method, kReturnFormal);
    if (expected == nullptr || expected->type == nullptr) {
        semant_error(node, "internal: missing return type for method " + current_visiting_method);
    }

    AST_Semant *expected_sem = make_value_semant(expected->type, false);
    if (!is_type_assignable(name_maps, expected_sem, ret_sem)) {
        semant_error(node, "return type mismatch in method " + current_visiting_method);
    }
}

void AST_Semant_Visitor::visit(PutInt *node) {
    if (node == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid putint statement");
    }
    node->exp->accept(*this);
    AST_Semant *s = semant_map->getSemant(node->exp);
    if (s == nullptr || s->get_kind() != AST_Semant::Kind::Value || s->get_type() != TypeKind::INT) {
        semant_error(node, "putint argument must be int");
    }
}

void AST_Semant_Visitor::visit(PutCh *node) {
    if (node == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid putch statement");
    }
    node->exp->accept(*this);
    AST_Semant *s = semant_map->getSemant(node->exp);
    if (s == nullptr || s->get_kind() != AST_Semant::Kind::Value || s->get_type() != TypeKind::INT) {
        semant_error(node, "putch argument must be int");
    }
}

void AST_Semant_Visitor::visit(PutArray *node) {
    if (node == nullptr || node->n == nullptr || node->arr == nullptr) {
        semant_error(node, "invalid putarray statement");
    }
    node->n->accept(*this);
    node->arr->accept(*this);

    AST_Semant *n_sem = semant_map->getSemant(node->n);
    AST_Semant *arr_sem = semant_map->getSemant(node->arr);
    if (n_sem == nullptr || n_sem->get_kind() != AST_Semant::Kind::Value || n_sem->get_type() != TypeKind::INT) {
        semant_error(node->n, "putarray first argument must be int");
    }
    if (arr_sem == nullptr || arr_sem->get_kind() != AST_Semant::Kind::Value || arr_sem->get_type() != TypeKind::ARRAY) {
        semant_error(node->arr, "putarray second argument must be array");
    }
}

void AST_Semant_Visitor::visit(Starttime *node) {
    (void)node;
}

void AST_Semant_Visitor::visit(Stoptime *node) {
    (void)node;
}

void AST_Semant_Visitor::visit(BinaryOp *node) {
    if (node == nullptr || node->left == nullptr || node->right == nullptr || node->op == nullptr) {
        semant_error(node, "invalid binary expression");
    }

    node->left->accept(*this);
    node->right->accept(*this);

    AST_Semant *lhs = semant_map->getSemant(node->left);
    AST_Semant *rhs = semant_map->getSemant(node->right);
    if (lhs == nullptr || rhs == nullptr) {
        semant_error(node, "binary operator with unresolved operand type");
    }

    if (!is_binary_op_int_to_int(node->op->op)) {
        semant_error(node, "unsupported binary operator: " + node->op->op);
    }

    if (lhs->get_kind() != AST_Semant::Kind::Value || rhs->get_kind() != AST_Semant::Kind::Value ||
        lhs->get_type() != TypeKind::INT || rhs->get_type() != TypeKind::INT) {
        semant_error(node, "binary operator requires int operands");
    }

    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(UnaryOp *node) {
    if (node == nullptr || node->exp == nullptr || node->op == nullptr) {
        semant_error(node, "invalid unary expression");
    }

    node->exp->accept(*this);
    AST_Semant *s = semant_map->getSemant(node->exp);
    if (s == nullptr || s->get_kind() != AST_Semant::Kind::Value || s->get_type() != TypeKind::INT) {
        semant_error(node, "unary operator requires int operand");
    }

    if (node->op->op != "-" && node->op->op != "!") {
        semant_error(node, "unsupported unary operator: " + node->op->op);
    }

    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(ArrayExp *node) {
    if (node == nullptr || node->arr == nullptr || node->index == nullptr) {
        semant_error(node, "invalid array access expression");
    }

    node->arr->accept(*this);
    node->index->accept(*this);

    AST_Semant *arr = semant_map->getSemant(node->arr);
    AST_Semant *idx = semant_map->getSemant(node->index);
    if (arr == nullptr || idx == nullptr) {
        semant_error(node, "array access has unresolved operand type");
    }

    if (arr->get_kind() != AST_Semant::Kind::Value || arr->get_type() != TypeKind::ARRAY) {
        semant_error(node->arr, "array base must be array type");
    }
    if (idx->get_kind() != AST_Semant::Kind::Value || idx->get_type() != TypeKind::INT) {
        semant_error(node->index, "array index must be int");
    }

    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, true));
}

void AST_Semant_Visitor::visit(CallExp *node) {
    if (node == nullptr || node->obj == nullptr || node->name == nullptr) {
        semant_error(node, "invalid call expression");
    }

    node->obj->accept(*this);
    AST_Semant *obj_sem = semant_map->getSemant(node->obj);
    if (obj_sem == nullptr || obj_sem->get_kind() != AST_Semant::Kind::Value || obj_sem->get_type() != TypeKind::CLASS) {
        semant_error(node->obj, "method call target must be an object");
    }

    string owner = find_method_owner(name_maps, get<string>(obj_sem->get_type_par()), node->name->id);
    if (owner.empty()) {
        semant_error(node, "undefined method: " + node->name->id);
    }
    semant_map->setSemant(node->name,
                          new AST_Semant(AST_Semant::Kind::MethodName, TypeKind::CLASS, owner, false));

    vector<Formal *> *formals = name_maps->get_method_formal_list(owner, node->name->id);
    if (formals == nullptr || formals->empty()) {
        semant_error(node, "invalid method metadata for call: " + node->name->id);
    }

    size_t expected_args = formals->size() - 1;
    size_t actual_args = node->par == nullptr ? 0 : node->par->size();
    if (expected_args != actual_args) {
        semant_error(node, "method argument count mismatch for " + node->name->id);
    }

    for (size_t i = 0; i < actual_args; ++i) {
        Exp *arg = (*(node->par))[i];
        arg->accept(*this);
        AST_Semant *arg_sem = semant_map->getSemant(arg);
        AST_Semant *formal_sem = make_value_semant((*formals)[i]->type, true);
        if (!is_type_assignable(name_maps, formal_sem, arg_sem)) {
            semant_error(arg, "method argument type mismatch at index " + to_string(i));
        }
    }

    Formal *ret = formals->back();
    semant_map->setSemant(node, make_value_semant(ret->type, false));
}

void AST_Semant_Visitor::visit(ClassVar *node) {
    if (node == nullptr || node->obj == nullptr || node->id == nullptr) {
        semant_error(node, "invalid class field access");
    }

    node->obj->accept(*this);
    AST_Semant *obj_sem = semant_map->getSemant(node->obj);
    if (obj_sem == nullptr || obj_sem->get_kind() != AST_Semant::Kind::Value || obj_sem->get_type() != TypeKind::CLASS) {
        semant_error(node->obj, "field access target must be an object");
    }

    string class_name = get<string>(obj_sem->get_type_par());
    VarDecl *field = find_class_var(name_maps, class_name, node->id->id);
    if (field == nullptr || field->type == nullptr) {
        semant_error(node, "undefined class field: " + node->id->id);
    }

    semant_map->setSemant(node->id, make_value_semant(field->type, true));
    semant_map->setSemant(node, make_value_semant(field->type, true));
}

void AST_Semant_Visitor::visit(This *node) {
    if (current_visiting_class.empty() || current_visiting_class == kMainClass) {
        semant_error(node, "'this' cannot be used in main context");
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::CLASS, current_visiting_class, false));
}

void AST_Semant_Visitor::visit(Length *node) {
    if (node == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid length expression");
    }

    node->exp->accept(*this);
    AST_Semant *exp_sem = semant_map->getSemant(node->exp);
    if (exp_sem == nullptr || exp_sem->get_kind() != AST_Semant::Kind::Value || exp_sem->get_type() != TypeKind::ARRAY) {
        semant_error(node->exp, "length() requires array expression");
    }

    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(NewArray *node) {
    if (node == nullptr || node->size == nullptr) {
        semant_error(node, "invalid new array expression");
    }

    node->size->accept(*this);
    AST_Semant *size_sem = semant_map->getSemant(node->size);
    if (size_sem == nullptr || size_sem->get_kind() != AST_Semant::Kind::Value || size_sem->get_type() != TypeKind::INT) {
        semant_error(node->size, "new array size must be int");
    }

    semant_map->setSemant(node, make_value_semant(TypeKind::ARRAY, 0, false));
}

void AST_Semant_Visitor::visit(NewObject *node) {
    if (node == nullptr || node->id == nullptr) {
        semant_error(node, "invalid new object expression");
    }

    if (!name_maps->is_class(node->id->id)) {
        semant_error(node, "undefined class in object creation: " + node->id->id);
    }

    semant_map->setSemant(node->id,
                          new AST_Semant(AST_Semant::Kind::ClassName, TypeKind::CLASS, node->id->id, false));
    semant_map->setSemant(node, make_value_semant(TypeKind::CLASS, node->id->id, false));
}

void AST_Semant_Visitor::visit(GetInt *node) {
    if (node == nullptr) {
        semant_error(node, "invalid getint expression");
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(GetCh *node) {
    if (node == nullptr) {
        semant_error(node, "invalid getch expression");
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(GetArray *node) {
    if (node == nullptr || node->exp == nullptr) {
        semant_error(node, "invalid getarray expression");
    }

    node->exp->accept(*this);
    AST_Semant *arr_sem = semant_map->getSemant(node->exp);
    if (arr_sem == nullptr || arr_sem->get_kind() != AST_Semant::Kind::Value || arr_sem->get_type() != TypeKind::ARRAY) {
        semant_error(node->exp, "getarray argument must be array");
    }

    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(IdExp *node) {
    if (node == nullptr) {
        semant_error(node, "invalid identifier expression");
    }

    VarDecl *local = name_maps->get_method_var(current_visiting_class, current_visiting_method, node->id);
    if (local != nullptr && local->type != nullptr) {
        semant_map->setSemant(node, make_value_semant(local->type, true));
        return;
    }

    Formal *formal = name_maps->get_method_formal(current_visiting_class, current_visiting_method, node->id);
    if (formal != nullptr && formal->type != nullptr) {
        semant_map->setSemant(node, make_value_semant(formal->type, true));
        return;
    }

    VarDecl *field = find_class_var(name_maps, current_visiting_class, node->id);
    if (field != nullptr && field->type != nullptr) {
        semant_map->setSemant(node, make_value_semant(field->type, true));
        return;
    }

    semant_error(node, "undefined identifier: " + node->id);
}

void AST_Semant_Visitor::visit(OpExp *node) {
    (void)node;
}

void AST_Semant_Visitor::visit(IntExp *node) {
    if (node == nullptr) {
        semant_error(node, "invalid integer expression");
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}
