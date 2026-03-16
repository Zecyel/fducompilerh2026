#define DEBUG
#undef DEBUG

#include <iostream>
#include <map>
#include <variant>
#include <vector>

#include "ASTheader.hh"
#include "FDMJAST.hh"
#include "namemaps.hh"

using namespace std;
using namespace fdmj;

namespace {

const string kMainClass = "__$main__";
const string kMainMethod = "main";
const string kReturnPrefix = "_^return^_";

[[noreturn]] void name_error(AST *node, const string &msg) {
    if (node != nullptr && node->getPos() != nullptr) {
        cerr << "Error: at position " << node->getPos()->print() << endl;
    }
    cerr << "Error: " << msg << endl;
    cerr << "Name mapping failed due to errors. Compilation aborted." << endl;
    exit(1);
}

Formal *make_return_formal(Type *ret_type, const string &method_name) {
    Pos *p = new Pos(0, 0, 0, 0);
    return new Formal(p, ret_type->clone(), new IdExp(new Pos(0, 0, 0, 0), kReturnPrefix + method_name));
}

} // namespace

void AST_Name_Map_Visitor::visit(Program *node) {
#ifdef DEBUG
    std::cout << "Visiting Program" << std::endl;
#endif
    if (node == nullptr) {
        return;
    }

    if (node->main != nullptr) {
        node->main->accept(*this);
    }

    // Register all classes first so `extends` can reference later declarations.
    if (node->cdl != nullptr) {
        for (auto *cl : *(node->cdl)) {
            if (cl == nullptr || cl->id == nullptr) {
                name_error(cl, "invalid class declaration");
            }
            if (!name_maps->add_class(cl->id->id)) {
                name_error(cl, "duplicated class name: " + cl->id->id);
            }
        }

        for (auto *cl : *(node->cdl)) {
            cl->accept(*this);
        }
    }
}

void AST_Name_Map_Visitor::visit(MainMethod *node) {
    if (node == nullptr) {
        return;
    }

    if (!name_maps->add_class(kMainClass)) {
        name_error(node, "internal: duplicated synthetic main class");
    }
    if (!name_maps->add_method(kMainClass, kMainMethod)) {
        name_error(node, "internal: duplicated synthetic main method");
    }

    Formal *ret = make_return_formal(new Type(new Pos(0, 0, 0, 0), TypeKind::INT, nullptr, nullptr), kMainMethod);
    string ret_name = kReturnPrefix + kMainMethod;
    if (!name_maps->add_method_formal(kMainClass, kMainMethod, ret_name, ret)) {
        name_error(node, "internal: duplicated synthetic main return type");
    }
    if (!name_maps->add_method_formal_list(kMainClass, kMainMethod, {ret_name})) {
        name_error(node, "internal: failed to create main formal list");
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

void AST_Name_Map_Visitor::visit(ClassDecl *node) {
    if (node == nullptr) {
        return;
    }
    if (node->id == nullptr) {
        name_error(node, "class has no identifier");
    }

    current_visiting_class = node->id->id;

    if (node->eid != nullptr) {
        if (!name_maps->add_class_hiearchy(node->id->id, node->eid->id)) {
            name_error(node, "invalid extends relation: " + node->id->id + " extends " + node->eid->id);
        }
    }

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

    current_visiting_class = "";
}

void AST_Name_Map_Visitor::visit(Type *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(VarDecl *node) {
    if (node == nullptr || node->id == nullptr) {
        name_error(node, "invalid variable declaration");
    }

    if (current_visiting_class.empty()) {
        name_error(node, "variable declaration outside class/main scope");
    }

    if (current_visiting_method.empty()) {
        if (!name_maps->add_class_var(current_visiting_class, node->id->id, node)) {
            name_error(node, "duplicated class variable: " + current_visiting_class + "." + node->id->id);
        }
        return;
    }

    if (!name_maps->add_method_var(current_visiting_class, current_visiting_method, node->id->id, node)) {
        name_error(node, "Variable " + node->id->id + " is already declared in method " +
                           current_visiting_method + " of class " + current_visiting_class);
    }
}

void AST_Name_Map_Visitor::visit(MethodDecl *node) {
    if (node == nullptr || node->id == nullptr || node->type == nullptr) {
        name_error(node, "invalid method declaration");
    }

    if (!name_maps->add_method(current_visiting_class, node->id->id)) {
        name_error(node, "duplicated method: " + current_visiting_class + "." + node->id->id);
    }

    string old_method = current_visiting_method;
    current_visiting_method = node->id->id;

    vector<string> formal_names;
    if (node->fl != nullptr) {
        for (auto *f : *(node->fl)) {
            if (f != nullptr) {
                f->accept(*this);
                formal_names.push_back(f->id->id);
            }
        }
    }

    Formal *ret = make_return_formal(node->type, current_visiting_method);
    string ret_name = kReturnPrefix + current_visiting_method;
    if (!name_maps->add_method_formal(current_visiting_class, current_visiting_method, ret_name, ret)) {
        name_error(node, "internal: duplicated synthetic method return type");
    }
    formal_names.push_back(ret_name);

    if (!name_maps->add_method_formal_list(current_visiting_class, current_visiting_method, formal_names)) {
        name_error(node, "failed to register formal list for method: " + current_visiting_method);
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

void AST_Name_Map_Visitor::visit(Formal *node) {
    if (node == nullptr || node->id == nullptr) {
        name_error(node, "invalid formal parameter");
    }
    if (!name_maps->add_method_formal(current_visiting_class, current_visiting_method, node->id->id, node)) {
        name_error(node, "duplicated formal parameter: " + current_visiting_class + "." +
                           current_visiting_method + "." + node->id->id);
    }
}

void AST_Name_Map_Visitor::visit(Nested *node) {
    if (node == nullptr || node->sl == nullptr) {
        return;
    }
    for (auto *s : *(node->sl)) {
        if (s != nullptr) {
            s->accept(*this);
        }
    }
}

void AST_Name_Map_Visitor::visit(If *node) {
    if (node == nullptr) {
        return;
    }
    if (node->exp != nullptr) {
        node->exp->accept(*this);
    }
    if (node->stm1 != nullptr) {
        node->stm1->accept(*this);
    }
    if (node->stm2 != nullptr) {
        node->stm2->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(While *node) {
    if (node == nullptr) {
        return;
    }
    if (node->exp != nullptr) {
        node->exp->accept(*this);
    }
    if (node->stm != nullptr) {
        node->stm->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(Assign *node) {
    if (node == nullptr) {
        return;
    }
    if (node->left != nullptr) {
        node->left->accept(*this);
    }
    if (node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(CallStm *node) {
    if (node == nullptr) {
        return;
    }
    if (node->obj != nullptr) {
        node->obj->accept(*this);
    }
    if (node->name != nullptr) {
        node->name->accept(*this);
    }
    if (node->par != nullptr) {
        for (auto *e : *(node->par)) {
            if (e != nullptr) {
                e->accept(*this);
            }
        }
    }
}

void AST_Name_Map_Visitor::visit(Continue *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(Break *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(Return *node) {
    if (node != nullptr && node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(PutInt *node) {
    if (node != nullptr && node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(PutCh *node) {
    if (node != nullptr && node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(PutArray *node) {
    if (node == nullptr) {
        return;
    }
    if (node->n != nullptr) {
        node->n->accept(*this);
    }
    if (node->arr != nullptr) {
        node->arr->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(Starttime *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(Stoptime *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(BinaryOp *node) {
    if (node == nullptr) {
        return;
    }
    if (node->left != nullptr) {
        node->left->accept(*this);
    }
    if (node->right != nullptr) {
        node->right->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(UnaryOp *node) {
    if (node != nullptr && node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(ArrayExp *node) {
    if (node == nullptr) {
        return;
    }
    if (node->arr != nullptr) {
        node->arr->accept(*this);
    }
    if (node->index != nullptr) {
        node->index->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(CallExp *node) {
    if (node == nullptr) {
        return;
    }
    if (node->obj != nullptr) {
        node->obj->accept(*this);
    }
    if (node->name != nullptr) {
        node->name->accept(*this);
    }
    if (node->par != nullptr) {
        for (auto *e : *(node->par)) {
            if (e != nullptr) {
                e->accept(*this);
            }
        }
    }
}

void AST_Name_Map_Visitor::visit(ClassVar *node) {
    if (node == nullptr) {
        return;
    }
    if (node->obj != nullptr) {
        node->obj->accept(*this);
    }
    if (node->id != nullptr) {
        node->id->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(This *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(Length *node) {
    if (node != nullptr && node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(NewArray *node) {
    if (node != nullptr && node->size != nullptr) {
        node->size->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(NewObject *node) {
    if (node != nullptr && node->id != nullptr) {
        node->id->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(GetInt *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(GetCh *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(GetArray *node) {
    if (node != nullptr && node->exp != nullptr) {
        node->exp->accept(*this);
    }
}

void AST_Name_Map_Visitor::visit(IdExp *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(OpExp *node) {
    (void)node;
}

void AST_Name_Map_Visitor::visit(IntExp *node) {
    (void)node;
}
