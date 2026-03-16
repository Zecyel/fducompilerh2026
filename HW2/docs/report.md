# FDU Compiler HW2 实验报告

## 基本信息

- 学号：`23300240014`
- 姓名：`Zecyel(朱程炀)`
- 作业：Homework Assignment 2（语义分析：名称映射 + 类型检查）

## 1. 环境配置

本次作业在 WSL2 Linux 环境完成，核心工具链如下：

- 系统：`Linux 6.6.87.1-microsoft-standard-WSL2`
- 编译器：`gcc/g++ 13.3.0`
- 构建工具：`cmake 3.28.3`、`ninja 1.11.1`
- 解析器：使用 `vendor/parser/parser` 生成 `.2.ast` 文件

项目使用 `Makefile + CMake + Ninja` 进行构建与测试，主要命令：

```bash
make build        # 构建项目
make parse        # 解析所有 .fmj 文件生成 AST
make run          # 解析 + 语义分析所有测试文件
make run-one FILE=newtest1  # 单独测试某个文件
```

## 2. 整体架构

语义分析分为两个阶段，各由一个 Visitor 实现：

1. **名称映射阶段**（`setnamemaps.cc`）：遍历 AST 构建符号表 `Name_Maps`，注册类、方法、变量、形参等信息。
2. **语义检查阶段**（`semantanlyzer.cc`）：基于符号表对 AST 进行类型检查，为每个表达式节点附加语义信息 `AST_Semant`。

入口函数在 `main.cc` 中：

```cpp
Name_Maps *name_maps = makeNameMaps(root);  // 第一阶段：构建符号表
name_maps->print();                          // 打印符号表
AST_Semant_Map *semant_map = semant_analyze(root, name_maps);  // 第二阶段：语义检查
```

## 3. 完成内容与代码实现

### 3.1 名称映射（`setnamemaps.cc`）

实现文件：

- `include/ast/namemaps.hh`（符号表数据结构，已提供）
- `lib/ast/setnamemaps.cc`（名称映射 Visitor 实现）

#### 核心设计

`AST_Name_Map_Visitor` 遍历 AST，将所有声明信息注册到 `Name_Maps` 中：

- **类注册**：先遍历所有 `ClassDecl` 注册类名，再逐个处理类体。这样 `extends` 可以引用后声明的类。
- **主方法合成**：将 `main()` 合成为 `__$main__` 类的 `main` 方法，返回类型存为形参 `_^return^_main`。
- **继承关系**：通过 `add_class_hiearchy` 注册父子关系。
- **变量作用域**：根据 `current_visiting_method` 是否为空区分类变量和方法局部变量。

#### 关键代码：主方法合成

```cpp
const string kMainClass = "__$main__";
const string kMainMethod = "main";
const string kReturnPrefix = "_^return^_";

void AST_Name_Map_Visitor::visit(MainMethod *node) {
    name_maps->add_class(kMainClass);
    name_maps->add_method(kMainClass, kMainMethod);
    Formal *ret = make_return_formal(
        new Type(new Pos(0,0,0,0), TypeKind::INT, nullptr, nullptr), kMainMethod);
    name_maps->add_method_formal(kMainClass, kMainMethod, kReturnPrefix + kMainMethod, ret);
    name_maps->add_method_formal_list(kMainClass, kMainMethod, {kReturnPrefix + kMainMethod});
    // ... 遍历变量声明和语句
}
```

#### 关键代码：变量注册与重名检测

```cpp
void AST_Name_Map_Visitor::visit(VarDecl *node) {
    if (current_visiting_method.empty()) {
        // 类变量
        if (!name_maps->add_class_var(current_visiting_class, node->id->id, node))
            name_error(node, "duplicated class variable: " + current_visiting_class + "." + node->id->id);
    } else {
        // 方法局部变量（允许与形参同名，即局部变量遮蔽形参）
        if (!name_maps->add_method_var(current_visiting_class, current_visiting_method, node->id->id, node))
            name_error(node, "Variable " + node->id->id + " is already declared in method "
                + current_visiting_method + " of class " + current_visiting_class);
    }
}
```

#### 错误处理

名称映射阶段的错误是致命的，遇到第一个错误即终止编译：

```cpp
[[noreturn]] void name_error(AST *node, const string &msg) {
    if (node != nullptr && node->getPos() != nullptr)
        cerr << "Error: at position " << node->getPos()->print() << endl;
    cerr << "Error: " << msg << endl;
    cerr << "Name mapping failed due to errors. Compilation aborted." << endl;
    exit(1);
}
```

### 3.2 语义检查（`semantanlyzer.cc`）

实现文件：

- `include/ast/semant.hh`（语义信息数据结构，已提供）
- `lib/ast/semantanlyzer.cc`（语义检查 Visitor 实现）

#### 核心设计

`AST_Semant_Visitor` 遍历 AST，对每个表达式节点进行类型推导和检查，通过 `semant_map->setSemant(node, sem)` 附加语义信息。语义信息包含：

- `Kind`：`Value`（值）、`MethodName`（方法名）、`ClassName`（类名）
- `TypeKind`：`INT`、`ARRAY`、`CLASS`
- `type_par`：类型参数（类名或数组维度）
- `lvalue`：是否为左值

#### 错误处理

与名称映射不同，语义检查阶段采用非致命错误收集策略，遇到错误后继续检查以尽可能多地报告问题：

```cpp
int error_count = 0;

void semant_error(AST *node, const string &msg) {
    error_count++;
    if (node != nullptr && node->getPos() != nullptr)
        cerr << "Error: at position " << node->getPos()->print() << endl;
    cerr << "Error: " << msg << endl;
}
```

分析结束后统一判断是否有错误：

```cpp
AST_Semant_Map *semant_analyze(Program *node, Name_Maps *nm) {
    // ...
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
```

## 4. 类型检查规则

以下是本程序实现的所有类型检查规则。

### 4.1 赋值类型匹配

在 `visit(Assign*)` 中检查左右两侧类型是否兼容：

```cpp
if (!lhs->is_lvalue())
    semant_error(node->left, "left-hand side of assignment is not an lvalue");
if (!is_type_assignable(name_maps, lhs, rhs))
    semant_error(node, "Assign node has a different type between left and right");
```

类型兼容规则（`is_type_assignable`）：
- `INT` ← `INT`：始终兼容
- `ARRAY` ← `ARRAY`：维度（arity）必须相同
- `CLASS` ← `CLASS`：右侧类必须是左侧类的子类（含自身）

### 4.2 返回类型检查

在 `visit(Return*)` 中检查返回表达式类型是否与方法声明的返回类型匹配：

```cpp
Formal *expected = name_maps->get_method_formal(
    current_visiting_class, current_visiting_method, kReturnPrefix + current_visiting_method);
AST_Semant *expected_sem = make_value_semant(expected->type, false);
if (!is_type_assignable(name_maps, expected_sem, ret_sem)) {
    if (expected_sem->get_type() == TypeKind::CLASS && ret_sem->get_type() == TypeKind::CLASS)
        semant_error(node, "Return node has incompatible classes between return and method");
    else
        semant_error(node, "Return node has a different type between return and method");
}
semant_map->setSemant(node, expected_sem);  // 为 Return 节点附加语义信息
```

### 4.3 方法调用参数检查

在 `visit(CallExp*)` 和 `visit(CallStm*)` 中检查：

1. 调用目标必须是对象类型（`TypeKind::CLASS`）
2. 方法必须存在（沿继承链查找）
3. 参数数量必须匹配
4. 每个参数类型必须与形参类型兼容

```cpp
string owner = find_method_owner(name_maps, class_name, node->name->id);
if (owner.empty()) {
    semant_error(node, "undefined method: " + node->name->id);
    return;
}
// 检查参数数量和类型...
for (size_t i = 0; i < actual_args; ++i) {
    AST_Semant *arg_sem = semant_map->getSemant(arg);
    AST_Semant *formal_sem = make_value_semant((*formals)[i]->type, true);
    if (!is_type_assignable(name_maps, formal_sem, arg_sem))
        semant_error(arg, "method argument type mismatch at index " + to_string(i));
}
```

### 4.4 数组下标检查

在 `visit(ArrayExp*)` 中检查：

- 被索引的表达式必须是数组类型
- 下标必须是整数类型

```cpp
if (arr->get_type() != TypeKind::ARRAY)
    semant_error(node, "ArrayExp node has a non-array value expression");
if (idx == nullptr || idx->get_type() != TypeKind::INT)
    semant_error(node->index, "array index must be int");
```

### 4.5 条件表达式检查

`If` 和 `While` 的条件必须是整数类型：

```cpp
// visit(If*) / visit(While*)
if (cond == nullptr || cond->get_type() != TypeKind::INT)
    semant_error(node, "If/While condition must be of integer type");
```

### 4.6 二元/一元运算符检查

- 二元运算符（`+`, `-`, `*`, `/`, `%`, `||`, `&&`, `<`, `>`, `<=`, `>=`, `==`, `!=`）要求两侧均为 `INT`
- 一元运算符（`!`, `-`）要求操作数为 `INT`

### 4.7 变量解析与类变量访问限制

在 `visit(IdExp*)` 中按优先级查找标识符：

1. 方法局部变量（最高优先级）
2. 方法形参
3. 类变量 → 报错，必须通过 `this.x` 或 `obj.x` 访问
4. 以上都找不到 → 报错，未定义标识符

```cpp
void AST_Semant_Visitor::visit(IdExp *node) {
    VarDecl *local = name_maps->get_method_var(cls, method, node->id);
    if (local != nullptr) { semant_map->setSemant(node, ...); return; }
    Formal *formal = name_maps->get_method_formal(cls, method, node->id);
    if (formal != nullptr) { semant_map->setSemant(node, ...); return; }
    VarDecl *field = find_class_var(name_maps, cls, node->id);
    if (field != nullptr) {
        semant_error(node, "Class variable " + node->id +
            " must be accessed via object (this." + node->id + " or obj." + node->id + ")");
        return;
    }
    semant_error(node, "undefined identifier: " + node->id);
}
```

### 4.8 继承相关检查

#### 单层继承限制

FDMJ2026 只允许单层继承，即父类不能再有父类：

```cpp
if (!name_maps->get_parent(parent).empty()) {
    semant_error(node, "Class " + class_name + " extends " + parent +
        " which already extends " + name_maps->get_parent(parent) +
        ". FDMJ2026 only allows single-level inheritance.");
}
```

#### 方法重写检查

子类重写父类方法时：
- 参数数量必须相同
- 参数类型必须完全一致
- 返回类型允许协变（子类方法可返回父类方法返回类型的子类）

```cpp
if (!covariant_return_ok(name_maps, child_ret, parent_ret))
    semant_error(node, "Method " + method_name +
        " has incompatible class for a return type with the same method in class " + parent);
```

#### 子类赋值兼容

`is_subclass` 沿继承链向上查找，支持子类对象赋值给父类变量。

### 4.9 其他检查

| 检查项 | 位置 | 说明 |
|--------|------|------|
| `this` 限制 | `visit(This*)` | `this` 不允许在 main 方法中使用 |
| `length()` 参数 | `visit(Length*)` | 参数必须是数组类型 |
| `new int[n]` 大小 | `visit(NewArray*)` | 大小表达式必须是整数类型 |
| `new Class()` | `visit(NewObject*)` | 类名必须已声明 |
| `putint` / `putch` | `visit(PutInt/PutCh*)` | 参数必须是整数类型 |
| `putarray` | `visit(PutArray*)` | 第一个参数为整数，第二个为数组 |
| `continue` / `break` | `visit(Continue/Break*)` | 必须在 while 循环内 |
| 变量类型声明 | `visit(VarDecl*)` | CLASS 类型的类名必须已声明 |
| 初始化器类型 | `visit(VarDecl*)` | int 初始化器只能用于 int 变量，数组初始化器只能用于数组变量 |

## 5. 我是如何完成这次作业的

我的完成流程如下：

1. 阅读 `README.md` 和 `docs/` 下的文档，理解 `Name_Maps`、`AST_Semant`、`AST_Semant_Map` 的数据结构和接口。
2. 先实现 `setnamemaps.cc`，构建符号表。重点处理主方法合成（`__$main__`）、返回类型形参（`_^return^_method`）、继承关系注册。
3. 再实现 `semantanlyzer.cc`，逐个实现各 AST 节点的 Visitor 方法。从简单的（`IntExp`、`IdExp`）到复杂的（`CallExp`、`MethodDecl`）。
4. 采用非致命错误收集策略，使语义分析能报告多个错误。每个 Visitor 方法都对 null 语义信息做了防御性处理，避免因前序错误导致后续崩溃。
5. 反复对照测试用例的期望输出调试，确保错误消息格式、位置信息、符号表打印格式完全匹配。

## 6. 测试与结果

### 6.1 测试方法

测试用例分为两类：

- **通过测试**（10 个）：有 `.2-semant.ast` 参考文件，程序输出必须与参考文件完全一致。
- **错误测试**（12 个）：`.fmj` 文件末尾注释中包含期望的错误输出，程序 stderr 必须完全匹配。

```bash
make build && make run
```

### 6.2 测试结果

所有 22 个测试用例全部通过：

| 类别 | 测试文件 | 结果 |
|------|----------|------|
| 通过 | newtest3, newtest6, newtest10, newtest11, newtest12, newtest13, newtest16, newtest17, test2, fibonacci | 全部 MATCH |
| 错误 | newtest1, newtest2, newtest4, newtest5, newtest7, newtest8, newtest9, newtest14, newtest15, newtest18, newtest19, newtest20 | 全部 MATCH |

### 6.3 示例：`newtest1.fmj` 的错误检测

输入程序（`test/newtest1.fmj`）：

```java
public int main() {
    class TestClass a;
    a = new int[10];      // 类型不匹配：Class ← IntArray
    return a[1];          // a 不是数组类型
}
public class TestClass {
    int a;
    int[] b;
    class A c;            // 类 A 未定义
    public class TestClass test1(class TestClass a) {
        int[] a;
    }
}
```

程序输出 4 个错误后终止：

```
Error: at position Position(sline: 5, scolumn: 5, eline: 5, ecolumn: 20)
Error: Assign node has a different type between left and right
Error: at position Position(sline: 7, scolumn: 12, eline: 7, ecolumn: 15)
Error: ArrayExp node has a non-array value expression
Error: at position Position(sline: 7, scolumn: 5, eline: 7, ecolumn: 16)
Error: Return node has no semantic information for its expression
Error: at position Position(sline: 13, scolumn: 5, eline: 13, ecolumn: 14)
Error: Variable c has undefined class type A
Semantic Analysis failed due to errors
```

### 6.4 示例：`newtest10.fmj` 的正确分析

输入程序（`test/newtest10.fmj`）：

```java
public int main() {
  int a;
  int[] b;
  a = 0;
  b = new int[100];
  while (a<length(b)) {
     b[a] = a*a;
  }
  if (length(b)>99) putint(b[99]);
  putch(10);
  return 1;
}
```

程序正确完成语义分析，输出符号表和语义 AST：

```
--Making Name Maps...
Classes: __$main__ ;
Class Hiearchy:
Methods: __$main__->main ;
Class Variables:
Method Variables: __$main__->main->a with type=Int ; __$main__->main->b with type=IntArray ;
Method Formals: __$main__->main->_^return^_main with type=Int ;
--Analyzing Semantics...
Start Semantic Analysis
Semantic Analysis Done
```
