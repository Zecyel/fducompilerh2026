#define DEBUG
#undef DEBUG

#include "constantPropagation.hh"
#include "MinusIntConverter.hh"
#include <vector>

using namespace std;
using namespace fdmj;

template <typename T>
static vector<T *> *visitList(ConstantPropagationVisitor &v,
                              vector<T *> *nodes) {
  if (nodes == nullptr || nodes->empty())
    return nullptr;
  vector<T *> *res = new vector<T *>();
  for (T *item : *nodes) {
    if (item == nullptr)
      continue;
    item->accept(v);
    if (v.newNode == nullptr)
      continue;
    res->push_back(static_cast<T *>(v.newNode));
  }
  if (res->empty()) {
    delete res;
    res = nullptr;
  }
  return res;
}

void ConstantPropagationVisitor::visit(Program *node) {
  if (node == nullptr) {
    newNode = nullptr;
    return;
  }
  MainMethod *m = nullptr;
  if (node->main != nullptr) {
    node->main->accept(*this);
    m = static_cast<MainMethod *>(newNode);
  }
  newNode = new Program(node->getPos()->clone(), m);
}

void ConstantPropagationVisitor::visit(MainMethod *node) {
  if (node == nullptr) {
    newNode = nullptr;
    return;
  }
  vector<Stm *> *sl = visitList(*this, node->sl);
  newNode = new MainMethod(node->getPos()->clone(), sl);
}

void ConstantPropagationVisitor::visit(Assign *node) {
  if (node == nullptr) {
    newNode = nullptr;
    return;
  }
  Exp *l = nullptr;
  if (node->left != nullptr) {
    node->left->accept(*this);
    l = static_cast<Exp *>(newNode);
  }
  Exp *r = nullptr;
  if (node->exp != nullptr) {
    node->exp->accept(*this);
    r = static_cast<Exp *>(newNode);
  }
  newNode = new Assign(node->getPos()->clone(), l, r);
}

void ConstantPropagationVisitor::visit(Return *node) {
  if (node == nullptr) {
    newNode = nullptr;
    return;
  }
  Exp *e = nullptr;
  if (node->exp != nullptr) {
    node->exp->accept(*this);
    e = static_cast<Exp *>(newNode);
  }
  newNode = new Return(node->getPos()->clone(), e);
}

void ConstantPropagationVisitor::visit(BinaryOp *node) {
  if (node == nullptr) {
    newNode = nullptr;
    return;
  }
  Exp *l = nullptr;
  if (node->left != nullptr) {
    node->left->accept(*this);
    l = static_cast<Exp *>(newNode);
  }
  Exp *r = nullptr;
  if (node->right != nullptr) {
    node->right->accept(*this);
    r = static_cast<Exp *>(newNode);
  }

  bool canFold = l != nullptr && r != nullptr && node->op != nullptr &&
                 l->getASTKind() == ASTKind::IntExp &&
                 r->getASTKind() == ASTKind::IntExp;

  if (canFold) {
    int leftVal = static_cast<IntExp *>(l)->val;
    int rightVal = static_cast<IntExp *>(r)->val;
    const string &op = node->op->op;
    bool valid = true;
    int result = 0;
    if (op == "+")
      result = leftVal + rightVal;
    else if (op == "-")
      result = leftVal - rightVal;
    else if (op == "*")
      result = leftVal * rightVal;
    else if (op == "/") {
      if (rightVal == 0)
        valid = false;
      else
        result = leftVal / rightVal;
    } else {
      valid = false;
    }

    if (valid) {
      newNode = new IntExp(node->getPos()->clone(), result);
      return;
    }
  }

  OpExp *opClone = node->op ? static_cast<OpExp *>(node->op->clone()) : nullptr;
  newNode = new BinaryOp(node->getPos()->clone(), l, opClone, r);
}

void ConstantPropagationVisitor::visit(UnaryOp *node) {
  if (node == nullptr) {
    newNode = nullptr;
    return;
  }
  Exp *e = nullptr;
  if (node->exp != nullptr) {
    node->exp->accept(*this);
    e = static_cast<Exp *>(newNode);
  }
  OpExp *opClone = node->op ? static_cast<OpExp *>(node->op->clone()) : nullptr;
  newNode = new UnaryOp(node->getPos()->clone(), opClone, e);
}

void ConstantPropagationVisitor::visit(IdExp *node) {
  newNode =
      (node == nullptr) ? nullptr : static_cast<IdExp *>(node->clone());
}

void ConstantPropagationVisitor::visit(OpExp *node) {
  newNode =
      (node == nullptr) ? nullptr : static_cast<OpExp *>(node->clone());
}

void ConstantPropagationVisitor::visit(IntExp *node) {
  newNode =
      (node == nullptr) ? nullptr : static_cast<IntExp *>(node->clone());
}

Program *constantPropagate(Program *root) {
  if (root == nullptr)
    return nullptr;
  Program *converted = minusIntRewrite(root);
  if (converted == nullptr)
    return nullptr;
  ConstantPropagationVisitor visitor;
  converted->accept(visitor);
  return static_cast<Program *>(visitor.newNode);
}
