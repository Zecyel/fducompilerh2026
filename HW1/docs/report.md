# FDU Compiler HW1 实验报告

## 基本信息

- 学号：`23300240014`
- 姓名：`Zecyel(朱程炀)`
- 作业：Homework Assignment 1（常量传播 + 程序执行）

## 1. 环境配置

本次作业在 WSL2 Linux 环境完成，核心工具链如下：

- 系统：`Linux 6.6.87.1-microsoft-standard-WSL2`
- 编译器：`gcc/g++ 13.3.0`
- 构建工具：`cmake 3.28.3`、`ninja 1.11.1`
- 词法/语法工具：`flex 2.6.4`、`bison 3.8.2`

项目使用 `Makefile + CMake + Ninja` 进行构建与测试，主要命令：

```bash
make build
make test
```

## 2. 完成内容与代码实现

### 2.1 常量传播（`constantPropagation`）

实现文件：

- `include/ast/constantPropagation.hh`
- `lib/ast/constantPropagation.cc`

实现思路：

1. 在 `constantPropagate` 中先调用 `minusIntRewrite(root)`，将一元负号先规范化，满足作业要求。
2. 使用 `ConstantPropagationVisitor` 自底向上遍历 AST。
3. 在访问 `BinaryOp` 时，如果左右子树都已经化简为 `IntExp`，则直接计算并替换为新的 `IntExp`。
4. 对除法做保护：当除数为 0 时不折叠，保留原表达式结构，避免引入错误语义。

这样可以把所有可静态计算的二元常量表达式提前折叠，减少后续执行阶段负担。

关键代码片段（`lib/ast/constantPropagation.cc`）：

```cpp
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
```

### 2.2 程序执行（`executor`）

实现文件：

- `include/ast/executor.hh`
- `lib/ast/executor.cc`

核心做法是基于 Visitor 模式实现一个解释执行器 `ExecutorVisitor`。

- 使用成员 `map<string, int> table` 维护变量表（符号表）：
  - 赋值语句 `Assign`：计算右值后写入 `table[id]`。
  - 变量读取 `IdExp`：从 `table` 查询当前值。
- 使用成员 `int returnValue` 保存最终返回值：
  - 访问 `Return` 时计算表达式并写入 `returnValue`。

#### 变量保存与访问（Visitor 模式关键点）

1. **保存变量值**：在 `visit(Assign*)` 中识别左侧 `IdExp`，把右值写入表中。  
2. **访问变量值**：在表达式求值过程中，遇到 `IdExp` 时调用 `getVariableValue` 查询表。  
3. **未定义变量处理**：若变量首次使用前未赋值：
   - 默认其值为 `0`；
   - 向 `stderr` 输出告警，并带上位置信息（line/column）。

该实现与 README 要求一致：既保证程序可继续运行，也提供了可定位问题的报错信息。

关键代码片段 1：变量赋值（保存）

```cpp
void ExecutorVisitor::visit(Assign *node) {
  if (node == nullptr || node->left == nullptr || node->exp == nullptr)
    return;
  int rightVal = evalExpression(node->exp);
  if (node->left->getASTKind() == ASTKind::IdExp) {
    table[static_cast<IdExp *>(node->left)->id] = rightVal;
  }
}
```

关键代码片段 2：变量读取（访问）+ 未定义变量处理

```cpp
int ExecutorVisitor::getVariableValue(IdExp *id) {
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

void ExecutorVisitor::reportUndefined(IdExp *id) {
  if (id == nullptr || id->getPos() == nullptr)
    return;
  Pos *pos = id->getPos();
  cerr << "Warning: variable '" << id->id << "' used before assignment at "
       << "line " << pos->sline << ", column " << pos->scolumn << endl;
}
```

关键代码片段 3：执行入口与返回值

```cpp
int execute(Program *root) {
  ExecutorVisitor visitor;
  if (root != nullptr)
    root->accept(visitor);
  return visitor.returnValue;
}
```

## 3. 我是如何完成这次作业的

我的完成流程如下：

1. 先阅读 `README.md` 明确两个接口目标：`constantPropagate(Program*)` 与 `execute(Program*)`。
2. 优先实现 AST 重写相关的常量传播逻辑，确保表达式树可稳定地递归化简。
3. 再实现执行器 Visitor，把表达式求值、变量表维护、返回值处理串起来。
4. 最后补齐边界行为（空节点保护、除零、未定义变量告警）并反复跑测试验证输出。

这种顺序把“树结构正确性”和“运行语义正确性”分开处理，调试效率更高。

## 4. 测试与结果

测试命令：

```bash
make build
make test
```

测试程序会生成：

- `*.1.out`：原始 AST XML
- `*.2.out`：常量传播后的 AST XML
- `*.3.out`：执行结果（原 AST 与优化后 AST 的返回值）

从结果可验证：

- 常量表达式被正确折叠；
- 执行器能正确维护变量值并得到最终返回值；
- 未定义变量会按要求输出警告并按 0 处理。

### 4.1 示例：`test1.fmj` 的执行结果

输入程序（`test/test1.fmj`）：

```java
public int main() {
	x = ((-1)+((-2)*3));
	return x;
}
```

执行 `make test` 后，`test/test1.3.out` 内容为：

```text
-7
-7
```

第一行是原始 AST 的执行结果，第二行是常量传播后 AST 的执行结果。两者一致，说明优化没有改变程序语义。
