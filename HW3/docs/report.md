# FDU Compiler HW3 实验报告

## 基本信息

- 学号：`23300240014`
- 姓名：`Zecyel(朱程炀)`
- 作业：Homework Assignment 3（AST 到 IR 的翻译，不含 class 和 array）

## 1. 环境配置

本次作业在 WSL2 Linux 环境完成，核心工具链如下：

- 系统：`Linux 5.15.167.4-microsoft-standard-WSL2`
- 编译器：`gcc/g++ 13.3.0`
- 构建工具：`cmake 3.28.3`、`ninja 1.11.1`

项目使用 `Makefile + CMake + Ninja` 进行构建与测试，主要命令：

```bash
make build        # 构建项目
make run          # 对所有测试文件执行 AST→IR 翻译
make run-one FILE=irtest1  # 单独测试某个文件
```

## 2. 整体架构

HW3 的任务是将语义分析后的 AST（`.2-semant.ast`）翻译为 Tiger IR+（`.3.irp`）。翻译范围限定为：

1. 只有 `main` 方法（无 class）
2. 只有 `int` 类型（无数组、无对象类型）
3. 函数调用均为外部函数（`putint`、`putch`、`getint` 等）

翻译流程在 `main.cc` 中：

```cpp
fdmj::Program *root = xml2ast(file_ast, &semant_map);  // 读取语义 AST
tree::Program *ir = ast2tree(root, semant_map);          // AST → IR 翻译
XMLDocument *x = tree2xml(ir);                           // IR → XML 输出
```

核心实现文件：

- `lib/ir/ast2tree.cc`：`ASTToTreeVisitor` 的所有 `visit` 方法实现
- `lib/ir/tr_exp.cc`：`Tr_Exp` 三种形式（`Tr_ex`、`Tr_nx`、`Tr_cx`）之间的转换

## 3. Q1.1：`treep.hh` 中各 class 的作用

### 顶层结构

- `tree::Program`：IR 程序的顶层节点，包含一个函数声明列表（`vector<FuncDecl*>`）。
- `tree::FuncDecl`：函数声明，包含函数名、参数列表（temp 列表）、函数体（一条 `Stm`）、返回类型，以及该函数使用的最大 temp 编号和 label 编号。

### 语句（Stm）

- `tree::Seq`：语句序列，将多条语句组合为一条。
- `tree::Move`：赋值语句，将 `src` 表达式的值赋给 `dst`（可以是 `TempExp` 或 `Mem`）。
- `tree::Jump`：无条件跳转到指定 label。
- `tree::Cjump`：条件跳转，根据 `relop` 比较 `left` 和 `right`，为真跳转到 `t`，为假跳转到 `f`。
- `tree::LabelStm`：定义一个 label，作为跳转目标。
- `tree::ExpStm`：将表达式当作语句执行，忽略返回值，只保留副作用（如函数调用）。
- `tree::Return`：返回语句，携带一个返回值表达式。

### 表达式（Exp）

- `tree::Binop`：二元运算，支持 `+`、`-`、`*`、`/`、`&&`、`||`、`xor` 等操作。
- `tree::TempExp`：将一个临时变量（`Temp`）转换为表达式，用于表示变量的值。
- `tree::Const`：整数常量表达式。
- `tree::Mem`：内存访问，通过地址表达式读写内存（HW3 中未使用，HW4 用于数组和对象）。
- `tree::Eseq`：表达式序列，先执行一条语句，再求值一个表达式并返回其结果。用于在表达式中嵌入副作用。
- `tree::Name`：将 label 转换为指针（地址），用于函数指针等场景。
- `tree::Call`：方法调用，包含方法名、对象指针和参数列表（HW3 中未使用）。
- `tree::ExtCall`：外部函数调用，包含函数名和参数列表，用于 `putint`、`getint` 等运行时函数。

## 4. Q1.2：Tiger IR+ 相对于 Tiger IR 的扩展

相对于虎书中的 Tiger IR，我们的 Tiger IR+ 增加了以下内容：

1. **`tree::Return`**：Tiger IR 没有显式的 return 语句，函数返回值通过特殊寄存器传递。Tiger IR+ 增加了 `Return` 节点，使函数返回语义更加明确，适配 FDMJ 中 `main` 方法必须有返回值的要求。

2. **`tree::ExtCall`**：Tiger IR 中所有函数调用统一使用 `CALL`。Tiger IR+ 区分了内部方法调用（`Call`）和外部函数调用（`ExtCall`），因为 FDMJ 的 `putint`、`getint` 等是运行时库函数，不需要对象指针，调用约定不同。

3. **`tree::Program` 和 `tree::FuncDecl`**：Tiger IR 以单个表达式为程序入口。Tiger IR+ 增加了 `Program`（函数列表）和 `FuncDecl`（函数声明，含参数列表和 temp/label 计数），以适配 FDMJ 的多函数结构。

4. **`tree::Seq` 使用 `vector`**：Tiger IR 的 `SEQ` 只能包含两条语句（二叉结构）。Tiger IR+ 的 `Seq` 使用 `vector<Stm*>` 支持任意多条语句的序列，简化了语句列表的表示。

5. **类型系统**：Tiger IR+ 为每个表达式增加了 `Type`（`INT` 或 `PTR`），用于区分整数值和指针值，为后续代码生成提供类型信息。

这些扩展的根本原因是 FDMJ 以语句（stm）为主体，而 Tiger 以表达式（exp）为主体。FDMJ 支持 class、多函数、外部调用等特性，需要更丰富的 IR 节点来表达。

## 5. Q2：不带 class 时各成分的翻译方法

翻译采用虎书中的三种表示形式（`Tr_ex`、`Tr_nx`、`Tr_cx`）和 Patch_list 回填机制：

- `Tr_ex`：有返回值的表达式（如常量、变量、算术运算）
- `Tr_nx`：无返回值的语句（如赋值、putint）
- `Tr_cx`：条件表达式，携带 true/false 的 Patch_list，延迟确定跳转目标

### 5.1 变量与常量

- **整数常量**（`IntExp`）：直接翻译为 `Tr_ex(new tree::Const(val))`。
- **变量引用**（`IdExp`）：通过 `Method_var_table` 查找变量对应的 `Temp`，翻译为 `Tr_ex(new tree::TempExp(type, temp))`。
- **变量声明**（`VarDecl`）：若有初始化值（如 `int i = 1`），生成 `Move(TempExp(temp), Const(val))`；无初始化则不生成代码。

### 5.2 算术运算

对于 `+`、`-`、`*`、`/` 等算术运算符，分别访问左右子表达式并调用 `unEx()` 获取 `tree::Exp*`，然后生成 `Tr_ex(new tree::Binop(INT, op, left, right))`。

一元运算符的处理：
- 取负 `-exp`：翻译为 `Binop(INT, "-", Const(0), exp)`
- 逻辑非 `!exp`：将表达式转换为 `Tr_cx`，然后交换 true_list 和 false_list

### 5.3 比较运算

对于 `<`、`>`、`<=`、`>=`、`==`、`!=` 等比较运算符，生成 `Tr_cx`，其中包含一个 `Cjump` 节点和两个 Patch_list（true_list 和 false_list）。Cjump 的 true/false label 使用占位符（num=-1），等待后续回填。

当比较结果需要作为整数值使用时（如 `(i < j) + 1`），通过 `Tr_cx::unEx()` 转换：分配一个临时变量，先赋值 0，执行条件跳转，若为真则赋值 1，最终返回该临时变量。

### 5.4 逻辑运算（短路求值）

- **`&&`（逻辑与）**：
  1. 将左操作数转换为 `Tr_cx`
  2. 分配 mid_label，将左侧 true_list 回填到 mid_label（左侧为真才继续判断右侧）
  3. 将右操作数转换为 `Tr_cx`
  4. 合并：true_list = 右侧 true_list，false_list = 左侧 false_list ∪ 右侧 false_list

- **`||`（逻辑或）**：
  1. 将左操作数转换为 `Tr_cx`
  2. 分配 mid_label，将左侧 false_list 回填到 mid_label（左侧为假才继续判断右侧）
  3. 将右操作数转换为 `Tr_cx`
  4. 合并：true_list = 左侧 true_list ∪ 右侧 true_list，false_list = 右侧 false_list

### 5.5 赋值

访问左侧表达式获取目标 `Temp`，访问右侧表达式获取源值，生成 `Move(dst, src)`。若右侧是条件表达式，`unEx()` 会自动将其转换为 0/1 整数值。

### 5.6 条件语句（If）

1. 访问条件表达式，转换为 `Tr_cx`
2. 先访问 then 分支和 else 分支（若有），收集生成的语句
3. 分配 then_label、else_label（若有 else）、end_label
4. 回填 true_list → then_label，false_list → else_label（或 end_label）
5. 生成结构：`[cx_stm, Label(then), then_stms, Jump(end), Label(else), else_stms, Label(end)]`

### 5.7 循环语句（While）

1. 先访问条件表达式，转换为 `Tr_cx`
2. 分配 test_label、body_label、done_label
3. 回填 true_list → body_label，false_list → done_label
4. 设置 continue_label = test_label，break_label = done_label
5. 访问循环体
6. 生成结构：`[Label(test), cx_stm, Label(body), body_stms, Jump(test), Label(done)]`

### 5.8 Break 和 Continue

- `break`：生成 `Jump(break_label)`，跳转到当前 while 的 done_label
- `continue`：生成 `Jump(continue_label)`，跳转到当前 while 的 test_label

### 5.9 外部函数调用

- `putint(exp)`：访问参数表达式，生成 `ExpStm(ExtCall(INT, "putint", {exp}))`
- `putch(exp)`：同上，函数名为 `"putch"`
- `getint()`：生成 `Tr_ex(ExtCall(INT, "getint", {}))`
- `getch()`：生成 `Tr_ex(ExtCall(INT, "getch", {}))`

### 5.10 返回语句

访问返回表达式，调用 `unEx()` 获取值，生成 `tree::Return(exp)`。

### 5.11 主方法翻译

`MainMethod` 翻译为一个 `FuncDecl`：
- 函数名：`"__$main__^main"`
- 参数列表：空（main 方法无参数）
- 函数体：所有变量初始化和语句组成的 `Seq`
- 返回类型：`INT`

变量通过 `generate_method_var_table` 分配 `Temp`，局部变量按字母序分配，然后为 `_^return^_main` 形参分配 Temp。Label 编号从 Temp 编号之后开始，避免编号冲突。

## 6. 测试与结果

### 6.1 测试方法

共 8 个测试用例（irtest1 ~ irtest8），覆盖以下场景：

| 测试 | 场景 |
|------|------|
| irtest1 | 简单赋值、putint、putch、return |
| irtest2 | while 循环（条件为变量） |
| irtest3 | while 循环（条件为比较 `>` ） |
| irtest4 | while 循环（条件含 `\|\|` 短路求值） |
| irtest5 | while 循环（条件含 `&&` 短路求值） |
| irtest6 | if-else、比较运算作为整数值 |
| irtest7 | getint()、`\|\|` 短路求值作为整数值 |
| irtest8 | while 内 if-else 含 break/continue |

### 6.2 测试结果

所有 8 个测试用例均成功编译运行，生成的 IR 结构与参考输出语义等价。IR 的控制流结构（条件跳转、循环、短路求值）完全正确，仅 label 编号因分配顺序不同而存在数值偏移，不影响语义正确性。

```bash
make build && make run   # 全部通过，无崩溃、无错误
```

## 7. 我是如何完成这次作业的

1. 阅读 `docs/lab3&4.md` 和 `README.md`，理解 HW3 的范围限定（只有 main、只有 int）。
2. 阅读 `treep.hh`、`tr_exp.hh`、`ast2tree.hh`、`temp.hh` 等头文件，理解 IR 树结构、三种翻译形式（Tr_ex/Tr_nx/Tr_cx）和 Patch_list 回填机制。
3. 仔细研究所有 8 个测试用例的输入（`.fmj`）和期望输出（`.3.irp`），理解每种语法结构对应的 IR 模式。
4. 实现 `ast2tree.cc` 中的 `ASTToTreeVisitor`，从简单节点（IntExp、IdExp）到复杂节点（BinaryOp、While、If）逐步实现。
5. 修改 `tr_exp.cc` 中 `Tr_cx::unEx()` 的分配顺序（先分配 Temp 再分配 Label），并移除未使用的多余 label 分配。
6. 对比较运算和简单表达式条件使用占位符 label（num=-1）配合 Patch_list 回填，避免分配多余的 label。
7. 反复运行测试、对比输出，调试 label/temp 分配顺序和回填逻辑。

