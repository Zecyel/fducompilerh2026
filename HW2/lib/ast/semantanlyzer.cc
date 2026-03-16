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

const string kMainClass = "__$main__";
const string kMainMethod = "main";
const string kReturnPrefix = "_^return^_";

int error_count = 0;

void semant_error(AST *node, const string &msg) {
    error_count++;
    if (node != nullptr && node->getPos() != nullptr) {
        cerr << "Error: at position " << node->getPos()->print() << endl;
    }
    cerr << "Error: " << msg << endl;
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
    if (child == parent) return true;
    string cur = child;
    set<string> visited;
    while (!cur.empty()) {
        if (visited.count(cur)) return false;
        visited.insert(cur);
        cur = nm->get_parent(cur);
        if (cur == parent) return true;
    }
    return false;
}

bool is_type_assignable(Name_Maps *nm, AST_Semant *to, AST_Semant *from) {
    if (to == nullptr || from == nullptr) return false;
    if (to->get_type() != from->get_type()) return false;
    switch (to->get_type()) {
        case TypeKind::INT: return true;
        case TypeKind::ARRAY:
            return get<int>(to->get_type_par()) == get<int>(from->get_type_par());
        case TypeKind::CLASS:
            return is_subclass(nm, get<string>(from->get_type_par()), get<string>(to->get_type_par()));
        default: return false;
    }
}

bool is_type_decl_valid(Name_Maps *nm, Type *type) {
    if (type == nullptr) return false;
    if (type->typeKind != TypeKind::CLASS) return true;
    return type->cid != nullptr && nm->is_class(type->cid->id);
}

string find_method_owner(Name_Maps *nm, const string &cls, const string &method) {
    string cur = cls;
    set<string> visited;
    while (!cur.empty()) {
        if (visited.count(cur)) return "";
        visited.insert(cur);
        if (nm->is_method(cur, method)) return cur;
        cur = nm->get_parent(cur);
    }
    return "";
}

VarDecl *find_class_var(Name_Maps *nm, const string &cls, const string &field) {
    string cur = cls;
    set<string> visited;
    while (!cur.empty()) {
        if (visited.count(cur)) return nullptr;
        visited.insert(cur);
        VarDecl *vd = nm->get_class_var(cur, field);
        if (vd != nullptr) return vd;
        cur = nm->get_parent(cur);
    }
    return nullptr;
}

bool same_decl_type(Type *a, Type *b) {
    if (a == nullptr || b == nullptr) return false;
    if (a->typeKind != b->typeKind) return false;
    switch (a->typeKind) {
        case TypeKind::INT: return true;
        case TypeKind::ARRAY:
            return (a->arity == nullptr ? 0 : a->arity->val) == (b->arity == nullptr ? 0 : b->arity->val);
        case TypeKind::CLASS:
            if (a->cid == nullptr || b->cid == nullptr) return false;
            return a->cid->id == b->cid->id;
        default: return false;
    }
}

bool covariant_return_ok(Name_Maps *nm, Type *child_ret, Type *parent_ret) {
    if (child_ret == nullptr || parent_ret == nullptr) return false;
    if (same_decl_type(child_ret, parent_ret)) return true;
    if (child_ret->typeKind != TypeKind::CLASS || parent_ret->typeKind != TypeKind::CLASS) return false;
    if (child_ret->cid == nullptr || parent_ret->cid == nullptr) return false;
    return is_subclass(nm, child_ret->cid->id, parent_ret->cid->id);
}

bool is_binary_op_int_to_int(const string &op) {
    static const set<string> ops = {
        "+", "-", "*", "/", "%", "||", "&&", "<", ">", "<=", ">=", "==", "!="};
    return ops.count(op);
}

} // namespace

AST_Semant_Map *semant_analyze(Program *node, Name_Maps *nm) {
    if (node == nullptr) return nullptr;
    Name_Maps *name_maps = (nm != nullptr) ? nm : makeNameMaps(node);
    AST_Semant_Visitor semant_visitor(name_maps);
    cout << "Start Semantic Analysis" << endl;
    error_count = 0;
    node->accept(semant_visitor);
    if (error_count > 0) {
        cerr << "Semantic Analysis failed due to errors" << endl;
        exit(1);
    }
    cout << "Semantic Analysis Done" << endl;
    return semant_visitor.getSemantMap();
}

void AST_Semant_Visitor::visit(Program *node) {
    if (node == nullptr) return;
    if (node->main != nullptr) node->main->accept(*this);
    if (node->cdl != nullptr) {
        for (auto *cl : *(node->cdl))
            if (cl != nullptr) cl->accept(*this);
    }
}

void AST_Semant_Visitor::visit(MainMethod *node) {
    if (node == nullptr) return;
    string old_class = current_visiting_class;
    string old_method = current_visiting_method;
    current_visiting_class = kMainClass;
    current_visiting_method = kMainMethod;
    if (node->vdl != nullptr)
        for (auto *vd : *(node->vdl))
            if (vd != nullptr) vd->accept(*this);
    if (node->sl != nullptr)
        for (auto *s : *(node->sl))
            if (s != nullptr) s->accept(*this);
    current_visiting_class = old_class;
    current_visiting_method = old_method;
}

void AST_Semant_Visitor::visit(ClassDecl *node) {
    if (node == nullptr || node->id == nullptr) return;
    const string class_name = node->id->id;
    const string parent = node->eid == nullptr ? "" : node->eid->id;

    if (!parent.empty()) {
        if (!name_maps->is_class(parent)) {
            semant_error(node, "undefined parent class: " + parent);
        } else if (!name_maps->get_parent(parent).empty()) {
            semant_error(node, "Class " + class_name + " extends " + parent +
                " which already extends " + name_maps->get_parent(parent) +
                ". FDMJ2026 only allows single-level inheritance.");
        } else {
            string cur = parent;
            set<string> visited;
            while (!cur.empty()) {
                if (cur == class_name) {
                    semant_error(node, "circular inheritance involving class " + class_name);
                    break;
                }
                if (visited.count(cur)) break;
                visited.insert(cur);
                cur = name_maps->get_parent(cur);
            }
        }
    }
    string old_class = current_visiting_class;
    current_visiting_class = class_name;
    if (node->vdl != nullptr)
        for (auto *vd : *(node->vdl))
            if (vd != nullptr) vd->accept(*this);
    if (node->mdl != nullptr)
        for (auto *md : *(node->mdl))
            if (md != nullptr) md->accept(*this);
    current_visiting_class = old_class;
}

void AST_Semant_Visitor::visit(Type *node) {
    // Type validation is done in VarDecl/Formal context where we have the variable name
    (void)node;
}

void AST_Semant_Visitor::visit(VarDecl *node) {
    if (node == nullptr || node->type == nullptr) return;
    if (!is_type_decl_valid(name_maps, node->type)) {
        string cname = (node->type->cid != nullptr) ? node->type->cid->id : "?";
        string vname = (node->id != nullptr) ? node->id->id : "?";
        semant_error(node, "Variable " + vname + " has undefined class type " + cname);
    }
    if (holds_alternative<IntExp *>(node->init)) {
        if (node->type->typeKind != TypeKind::INT)
            semant_error(node, "integer initializer can only be used with int variable");
    } else if (holds_alternative<vector<IntExp *> *>(node->init)) {
        if (node->type->typeKind != TypeKind::ARRAY)
            semant_error(node, "array initializer can only be used with array variable");
    }
}
void AST_Semant_Visitor::visit(MethodDecl *node) {
    if (node == nullptr || node->id == nullptr || node->type == nullptr) return;
    node->type->accept(*this);
    const string method_name = node->id->id;
    const string parent = name_maps->get_parent(current_visiting_class);
    if (!parent.empty() && name_maps->is_method(parent, method_name)) {
        vector<Formal *> *child_fl = name_maps->get_method_formal_list(current_visiting_class, method_name);
        vector<Formal *> *parent_fl = name_maps->get_method_formal_list(parent, method_name);
        if (child_fl != nullptr && parent_fl != nullptr && !child_fl->empty() && !parent_fl->empty()) {
            if (child_fl->size() != parent_fl->size()) {
                semant_error(node, "Method " + method_name + " has a different number of parameters with the same method in class " + parent);
            } else {
                for (size_t i = 0; i + 1 < child_fl->size(); ++i) {
                    if (!same_decl_type((*child_fl)[i]->type, (*parent_fl)[i]->type))
                        semant_error(node, "Method " + method_name + " has a different type for parameter with the same method in class " + parent);
                }
                Type *child_ret = child_fl->back()->type;
                Type *parent_ret = parent_fl->back()->type;
                if (!covariant_return_ok(name_maps, child_ret, parent_ret))
                    semant_error(node, "Method " + method_name + " has incompatible class for a return type with the same method in class " + parent);
            }
        }
    }
    string old_method = current_visiting_method;
    current_visiting_method = method_name;
    if (node->fl != nullptr)
        for (auto *f : *(node->fl))
            if (f != nullptr) f->accept(*this);
    if (node->vdl != nullptr)
        for (auto *vd : *(node->vdl))
            if (vd != nullptr) vd->accept(*this);
    if (node->sl != nullptr)
        for (auto *s : *(node->sl))
            if (s != nullptr) s->accept(*this);
    current_visiting_method = old_method;
}

void AST_Semant_Visitor::visit(Formal *node) {
    if (node == nullptr || node->type == nullptr) return;
    node->type->accept(*this);
}

void AST_Semant_Visitor::visit(Nested *node) {
    if (node == nullptr || node->sl == nullptr) return;
    for (auto *s : *(node->sl))
        if (s != nullptr) s->accept(*this);
}
void AST_Semant_Visitor::visit(If *node) {
    if (node == nullptr) return;
    if (node->exp != nullptr) {
        node->exp->accept(*this);
        AST_Semant *cond = semant_map->getSemant(node->exp);
        if (cond == nullptr || cond->get_type() != TypeKind::INT)
            semant_error(node, "If condition must be of integer type");
    }
    if (node->stm1 != nullptr) node->stm1->accept(*this);
    if (node->stm2 != nullptr) node->stm2->accept(*this);
}

void AST_Semant_Visitor::visit(While *node) {
    if (node == nullptr) return;
    if (node->exp != nullptr) {
        node->exp->accept(*this);
        AST_Semant *cond = semant_map->getSemant(node->exp);
        if (cond == nullptr || cond->get_type() != TypeKind::INT)
            semant_error(node, "While condition must be of integer type");
    }
    ++in_a_while_loop;
    if (node->stm != nullptr) node->stm->accept(*this);
    --in_a_while_loop;
}

void AST_Semant_Visitor::visit(Assign *node) {
    if (node == nullptr || node->left == nullptr || node->exp == nullptr) return;
    node->left->accept(*this);
    node->exp->accept(*this);
    AST_Semant *lhs = semant_map->getSemant(node->left);
    AST_Semant *rhs = semant_map->getSemant(node->exp);
    if (lhs == nullptr) {
        semant_error(node, "Assign node has no semantic information for its left expression");
        return;
    }
    if (rhs == nullptr) {
        semant_error(node, "Assign node has no semantic information for its right expression");
        return;
    }
    if (!lhs->is_lvalue())
        semant_error(node->left, "left-hand side of assignment is not an lvalue");
    if (!is_type_assignable(name_maps, lhs, rhs))
        semant_error(node, "Assign node has a different type between left and right");
}
void AST_Semant_Visitor::visit(CallStm *node) {
    if (node == nullptr || node->obj == nullptr || node->name == nullptr) return;
    node->obj->accept(*this);
    AST_Semant *obj_sem = semant_map->getSemant(node->obj);
    if (obj_sem == nullptr || obj_sem->get_type() != TypeKind::CLASS) {
        semant_error(node->obj, "method call target must be an object");
        return;
    }
    string owner = find_method_owner(name_maps, get<string>(obj_sem->get_type_par()), node->name->id);
    if (owner.empty()) {
        semant_error(node, "undefined method: " + node->name->id);
        return;
    }
    semant_map->setSemant(node->name,
        new AST_Semant(AST_Semant::Kind::MethodName, TypeKind::CLASS, owner, false));
    vector<Formal *> *formals = name_maps->get_method_formal_list(owner, node->name->id);
    if (formals == nullptr || formals->empty()) return;
    size_t expected_args = formals->size() - 1;
    size_t actual_args = node->par == nullptr ? 0 : node->par->size();
    if (expected_args != actual_args) {
        semant_error(node, "method argument count mismatch for " + node->name->id);
        return;
    }
    for (size_t i = 0; i < actual_args; ++i) {
        Exp *arg = (*(node->par))[i];
        arg->accept(*this);
        AST_Semant *arg_sem = semant_map->getSemant(arg);
        AST_Semant *formal_sem = make_value_semant((*formals)[i]->type, true);
        if (!is_type_assignable(name_maps, formal_sem, arg_sem))
            semant_error(arg, "method argument type mismatch at index " + to_string(i));
    }
}

void AST_Semant_Visitor::visit(Continue *node) {
    if (in_a_while_loop <= 0)
        semant_error(nullptr, "Continue node is not in a loop");
}

void AST_Semant_Visitor::visit(Break *node) {
    if (in_a_while_loop <= 0)
        semant_error(nullptr, "Break node is not in a loop");
}

void AST_Semant_Visitor::visit(Return *node) {
    if (node == nullptr) return;
    if (node->exp != nullptr) node->exp->accept(*this);
    AST_Semant *ret_sem = (node->exp != nullptr) ? semant_map->getSemant(node->exp) : nullptr;
    Formal *expected = name_maps->get_method_formal(current_visiting_class, current_visiting_method, kReturnPrefix + current_visiting_method);
    if (expected == nullptr || expected->type == nullptr) return;
    AST_Semant *expected_sem = make_value_semant(expected->type, false);
    if (ret_sem == nullptr) {
        semant_error(node, "Return node has no semantic information for its expression");
        return;
    }
    if (!is_type_assignable(name_maps, expected_sem, ret_sem)) {
        if (expected_sem->get_type() == TypeKind::CLASS && ret_sem->get_type() == TypeKind::CLASS)
            semant_error(node, "Return node has incompatible classes between return and method");
        else
            semant_error(node, "Return node has a different type between return and method");
    }
    semant_map->setSemant(node, expected_sem);
}
void AST_Semant_Visitor::visit(PutInt *node) {
    if (node == nullptr || node->exp == nullptr) return;
    node->exp->accept(*this);
    AST_Semant *s = semant_map->getSemant(node->exp);
    if (s == nullptr || s->get_type() != TypeKind::INT)
        semant_error(node, "putint argument must be int");
}

void AST_Semant_Visitor::visit(PutCh *node) {
    if (node == nullptr || node->exp == nullptr) return;
    node->exp->accept(*this);
    AST_Semant *s = semant_map->getSemant(node->exp);
    if (s == nullptr || s->get_type() != TypeKind::INT)
        semant_error(node, "putch argument must be int");
}

void AST_Semant_Visitor::visit(PutArray *node) {
    if (node == nullptr || node->n == nullptr || node->arr == nullptr) return;
    node->n->accept(*this);
    node->arr->accept(*this);
    AST_Semant *n_sem = semant_map->getSemant(node->n);
    AST_Semant *arr_sem = semant_map->getSemant(node->arr);
    if (n_sem == nullptr || n_sem->get_type() != TypeKind::INT)
        semant_error(node->n, "putarray first argument must be int");
    if (arr_sem == nullptr || arr_sem->get_type() != TypeKind::ARRAY)
        semant_error(node->arr, "putarray second argument must be array");
}

void AST_Semant_Visitor::visit(Starttime *node) { (void)node; }
void AST_Semant_Visitor::visit(Stoptime *node) { (void)node; }

void AST_Semant_Visitor::visit(BinaryOp *node) {
    if (node == nullptr || node->left == nullptr || node->right == nullptr || node->op == nullptr) return;
    node->left->accept(*this);
    node->right->accept(*this);
    AST_Semant *lhs = semant_map->getSemant(node->left);
    AST_Semant *rhs = semant_map->getSemant(node->right);
    if (lhs == nullptr || rhs == nullptr) return;
    if (!is_binary_op_int_to_int(node->op->op)) {
        semant_error(node, "unsupported binary operator: " + node->op->op);
        return;
    }
    if (lhs->get_type() != TypeKind::INT || rhs->get_type() != TypeKind::INT) {
        semant_error(node, "binary operator requires int operands");
        return;
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(UnaryOp *node) {
    if (node == nullptr || node->exp == nullptr || node->op == nullptr) return;
    node->exp->accept(*this);
    AST_Semant *s = semant_map->getSemant(node->exp);
    if (s == nullptr || s->get_type() != TypeKind::INT) {
        semant_error(node, "unary operator requires int operand");
        return;
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}
void AST_Semant_Visitor::visit(ArrayExp *node) {
    if (node == nullptr || node->arr == nullptr || node->index == nullptr) return;
    node->arr->accept(*this);
    node->index->accept(*this);
    AST_Semant *arr = semant_map->getSemant(node->arr);
    AST_Semant *idx = semant_map->getSemant(node->index);
    if (arr == nullptr) {
        semant_error(node, "ArrayExp node has no semantic information for its array expression");
        return;
    }
    if (arr->get_type() != TypeKind::ARRAY) {
        semant_error(node, "ArrayExp node has a non-array value expression");
        return;
    }
    if (idx == nullptr || idx->get_type() != TypeKind::INT)
        semant_error(node->index, "array index must be int");
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, true));
}

void AST_Semant_Visitor::visit(CallExp *node) {
    if (node == nullptr || node->obj == nullptr || node->name == nullptr) return;
    node->obj->accept(*this);
    AST_Semant *obj_sem = semant_map->getSemant(node->obj);
    if (obj_sem == nullptr || obj_sem->get_type() != TypeKind::CLASS) {
        semant_error(node->obj, "method call target must be an object");
        return;
    }
    string owner = find_method_owner(name_maps, get<string>(obj_sem->get_type_par()), node->name->id);
    if (owner.empty()) {
        semant_error(node, "undefined method: " + node->name->id);
        return;
    }
    semant_map->setSemant(node->name,
        new AST_Semant(AST_Semant::Kind::MethodName, TypeKind::CLASS, owner, false));
    vector<Formal *> *formals = name_maps->get_method_formal_list(owner, node->name->id);
    if (formals == nullptr || formals->empty()) return;
    size_t expected_args = formals->size() - 1;
    size_t actual_args = node->par == nullptr ? 0 : node->par->size();
    if (expected_args != actual_args) {
        semant_error(node, "method argument count mismatch for " + node->name->id);
        return;
    }
    for (size_t i = 0; i < actual_args; ++i) {
        Exp *arg = (*(node->par))[i];
        arg->accept(*this);
        AST_Semant *arg_sem = semant_map->getSemant(arg);
        AST_Semant *formal_sem = make_value_semant((*formals)[i]->type, true);
        if (!is_type_assignable(name_maps, formal_sem, arg_sem))
            semant_error(arg, "method argument type mismatch at index " + to_string(i));
    }
    Formal *ret = formals->back();
    semant_map->setSemant(node, make_value_semant(ret->type, false));
}

void AST_Semant_Visitor::visit(ClassVar *node) {
    if (node == nullptr || node->obj == nullptr || node->id == nullptr) return;
    node->obj->accept(*this);
    AST_Semant *obj_sem = semant_map->getSemant(node->obj);
    if (obj_sem == nullptr || obj_sem->get_type() != TypeKind::CLASS) {
        semant_error(node, "ClassVar node has no semantic information for its object");
        return;
    }
    string class_name = get<string>(obj_sem->get_type_par());
    VarDecl *field = find_class_var(name_maps, class_name, node->id->id);
    if (field == nullptr || field->type == nullptr) {
        semant_error(node, "undefined class field: " + node->id->id);
        return;
    }
    semant_map->setSemant(node->id, make_value_semant(field->type, true));
    semant_map->setSemant(node, make_value_semant(field->type, true));
}
void AST_Semant_Visitor::visit(This *node) {
    if (current_visiting_class.empty() || current_visiting_class == kMainClass) {
        semant_error(node, "'this' is not allowed in main method");
        return;
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::CLASS, current_visiting_class, false));
}

void AST_Semant_Visitor::visit(Length *node) {
    if (node == nullptr || node->exp == nullptr) return;
    node->exp->accept(*this);
    AST_Semant *exp_sem = semant_map->getSemant(node->exp);
    if (exp_sem == nullptr || exp_sem->get_type() != TypeKind::ARRAY) {
        semant_error(node->exp, "length() requires array expression");
        return;
    }
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(NewArray *node) {
    if (node == nullptr || node->size == nullptr) return;
    node->size->accept(*this);
    AST_Semant *size_sem = semant_map->getSemant(node->size);
    if (size_sem == nullptr || size_sem->get_type() != TypeKind::INT)
        semant_error(node->size, "new array size must be int");
    semant_map->setSemant(node, make_value_semant(TypeKind::ARRAY, 0, false));
}

void AST_Semant_Visitor::visit(NewObject *node) {
    if (node == nullptr || node->id == nullptr) return;
    if (!name_maps->is_class(node->id->id)) {
        semant_error(node, "undefined class in object creation: " + node->id->id);
        return;
    }
    semant_map->setSemant(node->id,
        new AST_Semant(AST_Semant::Kind::ClassName, TypeKind::CLASS, node->id->id, false));
    semant_map->setSemant(node, make_value_semant(TypeKind::CLASS, node->id->id, false));
}

void AST_Semant_Visitor::visit(GetInt *node) {
    if (node == nullptr) return;
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(GetCh *node) {
    if (node == nullptr) return;
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(GetArray *node) {
    if (node == nullptr || node->exp == nullptr) return;
    node->exp->accept(*this);
    AST_Semant *arr_sem = semant_map->getSemant(node->exp);
    if (arr_sem == nullptr || arr_sem->get_type() != TypeKind::ARRAY)
        semant_error(node->exp, "getarray argument must be array");
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

void AST_Semant_Visitor::visit(IdExp *node) {
    if (node == nullptr) return;
    // Check local variable first
    VarDecl *local = name_maps->get_method_var(current_visiting_class, current_visiting_method, node->id);
    if (local != nullptr && local->type != nullptr) {
        semant_map->setSemant(node, make_value_semant(local->type, true));
        return;
    }
    // Check formal parameter
    Formal *formal = name_maps->get_method_formal(current_visiting_class, current_visiting_method, node->id);
    if (formal != nullptr && formal->type != nullptr) {
        semant_map->setSemant(node, make_value_semant(formal->type, true));
        return;
    }
    // Class variables must be accessed via this.x or obj.x, not bare name
    VarDecl *field = find_class_var(name_maps, current_visiting_class, node->id);
    if (field != nullptr) {
        semant_error(node, "Class variable " + node->id + " must be accessed via object (this." + node->id + " or obj." + node->id + ")");
        return;
    }
    semant_error(node, "undefined identifier: " + node->id);
}

void AST_Semant_Visitor::visit(OpExp *node) { (void)node; }

void AST_Semant_Visitor::visit(IntExp *node) {
    if (node == nullptr) return;
    semant_map->setSemant(node, make_value_semant(TypeKind::INT, monostate{}, false));
}

