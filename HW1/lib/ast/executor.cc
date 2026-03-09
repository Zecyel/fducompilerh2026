#define DEBUG
#undef DEBUG

#include "executor.hh"
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace fdmj;

namespace {

class ExecutorVisitor : public ASTVisitor {
public:
  map<string, int> table;
  int returnValue = 0;

  void visit(Program *node) override {
    if (node == nullptr) {
      returnValue = 0;
      return;
    }
    if (node->main != nullptr)
      node->main->accept(*this);
  }

  void visit(MainMethod *node) override {
    if (node == nullptr || node->sl == nullptr)
      return;
    for (Stm *stm : *node->sl) {
      if (stm == nullptr)
        continue;
      stm->accept(*this);
    }
  }

  void visit(Assign *node) override {
    if (node == nullptr || node->left == nullptr || node->exp == nullptr)
      return;
    int rightVal = evalExpression(node->exp);
    if (node->left->getASTKind() == ASTKind::IdExp) {
      table[static_cast<IdExp *>(node->left)->id] = rightVal;
    }
  }

  void visit(Return *node) override {
    if (node == nullptr || node->exp == nullptr)
      return;
    returnValue = evalExpression(node->exp);
  }

  void visit(BinaryOp *node) override {}
  void visit(UnaryOp *node) override {}
  void visit(IdExp *node) override {}
  void visit(OpExp *node) override {}
  void visit(IntExp *node) override {}

  int evalExpression(Exp *node) {
    if (node == nullptr)
      return 0;
    switch (node->getASTKind()) {
    case ASTKind::IntExp:
      return static_cast<IntExp *>(node)->val;
    case ASTKind::IdExp:
      return getVariableValue(static_cast<IdExp *>(node));
    case ASTKind::UnaryOp:
      return evalUnary(static_cast<UnaryOp *>(node));
    case ASTKind::BinaryOp:
      return evalBinary(static_cast<BinaryOp *>(node));
    default:
      return 0;
    }
  }

  int getVariableValue(IdExp *id) {
    if (id == nullptr)
      return 0;
    auto it = table.find(id->id);
    if (it == table.end()) {
      reportUndefined(id);
      table[id->id] = 0;
      return 0;
    }
    return it->second;
  }

  int evalUnary(UnaryOp *node) {
    if (node == nullptr || node->op == nullptr)
      return evalExpression(node ? node->exp : nullptr);
    int val = evalExpression(node->exp);
    if (node->op->op == "-")
      return -val;
    return val;
  }

  int evalBinary(BinaryOp *node) {
    if (node == nullptr || node->op == nullptr)
      return 0;
    int l = evalExpression(node->left);
    int r = evalExpression(node->right);
    const string &op = node->op->op;
    if (op == "+")
      return l + r;
    if (op == "-")
      return l - r;
    if (op == "*")
      return l * r;
    if (op == "/")
      return (r == 0) ? 0 : l / r;
    return 0;
  }

  void reportUndefined(IdExp *id) {
    if (id == nullptr || id->getPos() == nullptr)
      return;
    Pos *pos = id->getPos();
    cerr << "Warning: variable '" << id->id << "' used before assignment at "
         << "line " << pos->sline << ", column " << pos->scolumn << endl;
  }
};

} // namespace

int execute(Program *root) {
  ExecutorVisitor visitor;
  if (root != nullptr)
    root->accept(visitor);
  return visitor.returnValue;
}
