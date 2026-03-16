# HW2 语义分析实现文档

## 1. 整体架构

语义分析分为两个阶段，各由一个 Visitor 完成：

```
源码 → Parser → AST (XML)
                   ↓
            AST_Name_Map_Visitor    ← 第一遍：构建符号表 (Name_Maps)
                   ↓
            AST_Semant_Visitor      ← 第二遍：类型检查 + 语义标注 (AST_Semant_Map)
                   ↓
            带语义信息的 AST (XML)
```

入口函数在 `semantanlyzer.cc` 的 `semant_analyze()`：

```cpp
Name_Maps *name_maps = makeNameMaps(node);   // 第一遍
AST_Semant_Visitor semant_visitor(name_maps); // 第二遍
node->accept(semant_visitor);
return semant_visitor.getSemantMap();
```

---

## 2. Visitor Pattern 的使用

### 2.1 模式说明

项目使用经典的 Visitor 设计模式遍历 AST。核心机制：

- 每个 AST 节点类（`Program`、`ClassDecl`、`BinaryOp` 等）都继承自 `AST`，并实现 `accept` 方法：
  ```cpp
  void accept(AST_Visitor &v) override { v.visit(this); }
  ```
- `AST_Visitor` 是抽象基类，声明了所有节点类型的 `visit()` 虚函数。
- 具体的 Visitor（`AST_Name_Map_Visitor`、`AST_Semant_Visitor`）继承 `AST_Visitor`，为每种节点实现 `visit()` 方法。

调用流程（以 `If` 语句为例）：

```
semant_visitor.visit(Program*)
  → node->main->accept(semant_visitor)     // MainMethod::accept 调用 visit(MainMethod*)
    → stm->accept(semant_visitor)           // If::accept 调用 visit(If*)
      → node->exp->accept(semant_visitor)   // 递归访问条件表达式
      → node->stm1->accept(semant_visitor)  // 递归访问 then 分支
      → node->stm2->accept(semant_visitor)  // 递归访问 else 分支
```

### 2.2 两个 Visitor 的职责

| Visitor | 文件 | 职责 |
|---------|------|------|
| `AST_Name_Map_Visitor` | `setnamemaps.cc` | 收集所有声明信息，构建符号表 |
| `AST_Semant_Visitor` | `semantanlyzer.cc` | 利用符号表做类型检查，给表达式标注语义信息 |

---

## 3. 第一遍：Name Map 构建 (`setnamemaps.cc`)

### 3.1 符号表结构 (`Name_Maps`)

```
classes              : set<string>                          — 所有类名
classHierarchy       : map<string, string>                  — 子类 → 父类
methods              : set<pair<string, string>>            — (类名, 方法名)
classVar             : map<(类名, 变量名), VarDecl*>        — 类成员变量
methodVar            : map<(类名, 方法名, 变量名), VarDecl*> — 方法局部变量
methodFormal         : map<(类名, 方法名, 形参名), Formal*>  — 方法形参
methodFormalList     : map<(类名, 方法名), vector<string>>   — 形参名列表（末尾含返回类型）
```

### 3.2 关键处理逻辑

**类注册（两遍扫描）**：先遍历所有 `ClassDecl` 注册类名，再逐个深入访问。这样 `extends` 可以引用后面声明的类。

```cpp
// 第一遍：注册所有类名
for (auto *cl : *(node->cdl))
    name_maps->add_class(cl->id->id);
// 第二遍：处理继承、成员、方法
for (auto *cl : *(node->cdl))
    cl->accept(*this);
```

**MainMethod 处理**：合成一个 `__main__` 类和 `main` 方法，统一处理。

**变量声明**：根据 `current_visiting_method` 是否为空，决定注册为类变量还是方法局部变量。局部变量允许遮蔽（shadow）同名形参。

**方法返回类型**：通过合成一个名为 `__$ret$__` 的 Formal 存入 `methodFormalList` 末尾。

---

## 4. 第二遍：语义分析 (`semantanlyzer.cc`)

### 4.1 语义信息结构 (`AST_Semant`)

每个表达式节点被标注以下信息：

| 字段 | 含义 | 取值 |
|------|------|------|
| `s_kind` | 语义种类 | `Value`（值）、`MethodName`（方法名）、`ClassName`（类名） |
| `typeKind` | 类型 | `INT`、`ARRAY`、`CLASS` |
| `type_par` | 类型参数 | `monostate`（INT）、`string`（CLASS 的类名）、`int`（ARRAY 的 arity） |
| `lvalue` | 是否左值 | `true`/`false` |

### 4.2 类型检查规则

**表达式类型推导**：

| 表达式 | 类型 | 左值 |
|--------|------|------|
| `IntExp` | INT | 否 |
| `IdExp` | 查符号表（局部变量 > 形参 > 类变量） | 是 |
| `BinaryOp` / `UnaryOp` | INT（要求操作数为 INT） | 否 |
| `ArrayExp` (a[i]) | INT（要求 a 为 ARRAY，i 为 INT） | 是 |
| `CallExp` (obj.m(args)) | 方法返回类型 | 否 |
| `ClassVar` (obj.field) | 字段类型（沿继承链查找） | 是 |
| `This` | 当前类的 CLASS 类型 | 否 |
| `NewObject` | CLASS 类型 | 否 |
| `NewArray` | ARRAY 类型 | 否 |
| `Length` | INT（要求参数为 ARRAY） | 否 |
| `GetInt` / `GetCh` | INT | 否 |
| `GetArray` | INT（要求参数为 ARRAY） | 否 |

**赋值兼容性** (`is_type_assignable`)：
- INT → INT：兼容
- ARRAY → ARRAY：arity 必须相同
- CLASS → CLASS：右侧必须是左侧的子类（或同类）

**方法重写检查**：
- 参数数量和类型必须完全一致
- 返回类型支持协变（子类方法可返回父类方法返回类型的子类）

**继承**：
- 仅允许单层继承（父类不能再有父类）
- 检测循环继承
- 类变量和方法沿继承链向上查找（`find_class_var`、`find_method_owner`）

**变量遮蔽规则**：
- 方法局部变量可以遮蔽同名形参
- 方法局部变量/形参可以遮蔽同名类变量
- `IdExp` 解析优先级：局部变量 → 形参 → 类变量（含继承链）

### 4.3 辅助函数

| 函数 | 作用 |
|------|------|
| `is_subclass(nm, child, parent)` | 沿继承链判断子类关系 |
| `find_method_owner(nm, class, method)` | 沿继承链查找方法定义所在的类 |
| `find_class_var(nm, class, field)` | 沿继承链查找类变量 |
| `same_decl_type(a, b)` | 比较两个 Type 节点是否相同 |
| `covariant_return_ok(nm, child_ret, parent_ret)` | 检查协变返回类型 |

---

## 5. 测试方法

### 5.1 测试流程

```bash
make parse    # 用 parser 将 test/*.fmj 编译为 .2.ast (XML 格式的 AST)
make run      # 对每个 .fmj：先 parse，再运行 main 做语义分析，输出 .2-semant.ast
make run-one FILE=newtest3   # 只测试单个文件
```

### 5.2 测试用例分类

**应通过的测试**（有 `.2-semant.ast` 参考文件）：

| 测试 | 覆盖点 |
|------|--------|
| `newtest3` | 局部变量遮蔽形参（同名不同类型） |
| `newtest6` | 多个方法中局部变量遮蔽形参 |
| `newtest10` | 基本类方法调用、Return 语义标注 |
| `newtest11` | 类变量访问（this.field）、冒泡排序 |
| `newtest12` | 递归方法调用、fibonacci |
| `newtest13` | 继承 + 方法重写 + 协变返回类型 |
| `newtest16` | 变量隐藏（子类同名类变量） |
| `newtest17` | 子类赋值给父类变量 |

**应报错的测试**：

| 测试 | 预期错误 |
|------|----------|
| `newtest1` | 类型不匹配（class 赋值 array） |
| `newtest2` | 重复声明局部变量 |
| `newtest4` | while 条件不是 int（是 class） |
| `newtest5` | if 条件不是 int（是 class） |
| `newtest7` | 返回类型不匹配 |
| `newtest8` | 违反单层继承 |
| `newtest9` | continue 不在循环内 |
| `newtest14` | 对非数组类型做下标访问 |
| `newtest15` | 赋值类型不匹配（int 赋给 array） |
| `newtest18` | 赋值类型不匹配 |

### 5.3 验证方式

对于应通过的测试，将生成的 `.2-semant.ast` 与参考文件 diff：

```bash
diff /tmp/hw2_ref/newtest10.2-semant.ast test/newtest10.2-semant.ast
```

无输出表示完全一致。对于应报错的测试，检查程序是否输出了正确的错误信息并以非零状态退出。
