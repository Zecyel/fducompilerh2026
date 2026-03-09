#ifndef _EXECUTOR_H
#define _EXECUTOR_H

#include "ASTheader.hh"
#include "FDMJAST.hh"
#include <map>
#include <string>

using namespace std;
using namespace fdmj;

int execute(Program *root);

class ExecutorVisitor : public ASTVisitor {
public:
  map<string, int> table;
  int returnValue = 0;

  void visit(Program *node) override;
  void visit(MainMethod *node) override;
  void visit(Assign *node) override;
  void visit(Return *node) override;
  void visit(BinaryOp *node) override;
  void visit(UnaryOp *node) override;
  void visit(IdExp *node) override;
  void visit(OpExp *node) override;
  void visit(IntExp *node) override;

  int evalExpression(Exp *node);
  int getVariableValue(IdExp *id);
  int evalUnary(UnaryOp *node);
  int evalBinary(BinaryOp *node);
  void reportUndefined(IdExp *id);
};

#endif
