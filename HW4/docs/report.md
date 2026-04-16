# FDU Compiler HW4 实验报告

## 基本信息

- 学号：`23300240014`
- 姓名：`Zecyel(朱程炀)`
- 作业：Homework Assignment 4（AST 到 IR 的翻译，补全 array 和 class）

## 1. 环境配置

本次作业仍在 WSL2 Linux 环境完成，核心工具链如下：

- 系统：`Linux 5.15.167.4-microsoft-standard-WSL2`
- 编译器：`gcc/g++ 13.3.0`
- 构建工具：`cmake 3.28.3`、`ninja 1.11.1`

项目使用 `Makefile + CMake + Ninja` 进行构建与测试，主要命令：

```bash
make build
make run
make run-one FILE=irtest21
```

## 2. 整体架构

HW4 在 HW3 的基础上继续完成 AST 到 Tiger IR+（IRP）的翻译。与 HW3 相比，新增支持的语言成分主要有：

1. `int[]` 数组类型
2. `class` / `extends`
3. 对象字段访问
4. 成员方法调用
5. `this`
6. 动态分派（多态）

翻译流程与 HW3 一致：

```cpp
fdmj::Program *root = xml2ast(file_ast, &semant_map);
tree::Program *ir = ast2tree(root, semant_map);
XMLDocument *x = tree2xml(ir);
```

核心实现文件：

- `lib/ir/ast2tree.cc`：HW4 的主要翻译逻辑
- `lib/ir/tr_exp.cc`：`Tr_ex / Tr_nx / Tr_cx` 的相互转换

HW4 的总体思路是：

1. 保留 HW3 中针对整型表达式、控制流、短路求值的翻译框架
2. 用 `Mem` + 偏移的方式统一表达数组和对象的内存访问
3. 用全局 `Class_table` 记录 Unified Object Record 中各字段和方法槽位的偏移
4. 将 class method 消解为普通 function，函数名重命名为 `类名^方法名`
5. 在方法调用时通过对象记录中的方法槽读取真正的函数入口地址，从而实现动态分派

## 3. Q1.1：`treep.hh` 中各 class 的作用

### 3.1 顶层结构

- `tree::Program`：IR 程序的根节点，内部保存函数声明列表。
- `tree::FuncDecl`：一个函数定义，包含函数名、形参 temp 列表、函数体、返回类型以及该函数使用到的 temp / label 上界。

### 3.2 语句节点

- `tree::Seq`：语句序列，用 `vector<Stm*>` 保存多条语句。
- `tree::Move`：赋值语句，把右侧表达式结果写入左侧目的位置。
- `tree::Jump`：无条件跳转到某个 label。
- `tree::Cjump`：条件跳转，根据关系运算结果跳向 true 或 false 分支。
- `tree::LabelStm`：定义一个 label，供跳转语句落点使用。
- `tree::ExpStm`：把表达式当作语句执行，只保留副作用，忽略返回值。
- `tree::Return`：函数返回语句，携带返回值表达式。

### 3.3 表达式节点

- `tree::Binop`：二元运算表达式，例如加减乘除、逻辑运算、地址加偏移等。
- `tree::TempExp`：把一个 temp 作为表达式使用，用来表示局部变量、形参或临时结果。
- `tree::Const`：整数常量。
- `tree::Mem`：内存访问表达式，表示“取地址表达式所指向内存单元的值”。数组访问、字段访问、方法槽访问都依赖它。
- `tree::Eseq`：先执行一段语句，再返回一个表达式结果。它用于把“有副作用的求值过程”嵌入到表达式中。
- `tree::Name`：把 label 当作地址常量使用，主要用于方法入口地址。
- `tree::Call`：内部函数/方法调用。它记录被调用方法名、函数入口表达式以及参数列表。
- `tree::ExtCall`：外部运行时函数调用，例如 `putint`、`getint`、`malloc`、`exit`、`putarray`、`getarray`。

## 4. Q1.2：Tiger IR+ 相对于 Tiger IR 的扩展

相对于虎书中的 Tiger IR，本实验使用的 Tiger IR+ 主要有以下扩展：

1. `Program` 和 `FuncDecl`
   因为 FDMJ 程序由多个函数组成，而不是单个顶层表达式，所以需要显式表示“程序”和“函数列表”。

2. `Return`
   FDMJ 源语言里有明确的 `return` 语义，因此 IR 中直接保留 `Return` 节点更自然。

3. `ExtCall`
   内部方法调用和运行时库调用的角色不同。把 `ExtCall` 独立出来后，`putint/getint/malloc/exit` 等外部接口更容易区分。

4. `Seq` 采用 `vector`
   Tiger IR 原本常用二叉 `SEQ`，但 FDMJ 的语句块天然更适合直接保存为一个语句列表。

5. 表达式类型 `Type`
   每个表达式都带有 `INT` 或 `PTR` 类型信息，便于区分整型值和指针值。HW4 中数组、对象、方法地址都依赖这种区分。

这些扩展的根本原因是：FDMJ 是以语句和函数为核心的语言，还包含数组、对象、外部运行时等特性，直接照搬 Tiger IR 会不够自然，因此需要做适配。

## 5. Q2：不带 class 时的翻译方法

HW4 仍然保留 HW3 中的基本翻译框架：使用 `Tr_ex`、`Tr_nx`、`Tr_cx` 三种中间表示，并通过 `Patch_list` 完成条件跳转的回填。

### 5.1 整数、变量、算术与控制流

这部分与 HW3 基本一致：

- 常量 `IntExp` 翻译为 `Const`
- 普通变量 `IdExp` 翻译为 `TempExp`
- 算术运算翻译为 `Binop`
- 比较运算翻译为 `Tr_cx + Cjump`
- `&&` / `||` 通过中间 label 实现短路求值
- `if` / `while` 用 `Cjump + Label + Jump` 拼接控制流
- `break` / `continue` 通过记录当前循环的 `break_label` 和 `continue_label` 实现

### 5.2 数组初始化

数组在内存中的布局是：

- `arr[0]` 保存数组长度
- `arr[1] ... arr[n]` 保存真实元素

因此数组总大小是 `(长度 + 1) * 4` 字节。

对于 `int[] a = {1, 2, 3}` 这种初始化，我的翻译步骤是：

1. 调用 `malloc` 分配 `16` 字节
2. 在偏移 `0` 处写入数组长度 `3`
3. 在偏移 `4 / 8 / 12` 处依次写入元素值
4. 将返回的数组首地址保存在变量 temp 中

对于 `new int[n]`，做法类似，只是长度表达式是一般表达式而不是常量：

1. 先翻译出 `n`
2. 调用 `malloc((n + 1) * 4)`
3. 在偏移 `0` 写入 `n`
4. 返回数组首地址

这里我使用 `Eseq` 把“分配 + 写长度”的副作用和最终返回的数组指针绑定在一起。

### 5.3 数组访问

访问 `a[i]` 时，真实元素地址不是 `a + i * 4`，而是：

```text
a + (i + 1) * 4
```

因为第 0 个槽位存的是长度。

为了避免数组表达式和下标表达式被重复求值，我的实现会先检查：

- 如果数组基址或下标本身已经是简单表达式（如 `TempExp` 或 `Const`），直接使用
- 否则先分配新 temp，把该表达式的值保存下来，再在后续地址计算里复用这个 temp

### 5.4 数组越界检查

在 `a[i]` 的翻译中，我额外插入了边界检查逻辑：

1. 先从 `Mem(a)` 中读取数组长度到临时变量
2. 检查 `i >= 0`
3. 再检查 `i < len`
4. 若不满足边界条件，则调用 `ExtCall("exit", {-1})`
5. 若满足，则继续计算元素地址

因此数组访问最终不是一个简单 `Mem`，而是一个 `Eseq`：

- `Eseq.stm` 负责长度读取和越界检查
- `Eseq.exp` 返回被检查后的下标，再参与地址计算

这种写法可以同时支持数组读取和数组写入，因为赋值左值本身也可以翻译成 `Mem(...)`。

### 5.5 数组长度

`length(a)` 直接翻译为读取数组第 0 个槽位，即：

```text
Mem(a)
```

为了保证 `a` 只求值一次，我同样把必要的临时赋值包在 `Eseq` 中。

### 5.6 运行时数组函数

- `putarray(n, a)` 翻译为 `ExtCall("putarray", {n, a})`
- `getarray(a)` 翻译为 `ExtCall("getarray", {a})`

运行时库实现默认也采用“第 0 个位置存长度”的约定，因此可以与前面的数组布局直接配合。

## 6. Q3：带 class 时的翻译方法

### 6.1 方法重命名

在 IR 中我不再保留 class 语法层面的结构，而是把每个方法翻译成普通函数：

- `A.f` 重命名为 `A^f`
- `B.g` 重命名为 `B^g`
- `main` 约定为 `__$main__^main`

这样做以后，IR 的顶层就是“函数列表”，与 `tree::Program` / `tree::FuncDecl` 的结构一致。

### 6.2 main method 与 class method 的参数列表

两者的主要区别是 `this`：

- `main` 没有接收者对象，因此参数列表为空
- 普通类方法会额外拥有一个隐式的 `this`

在具体实现里：

1. 为成员方法单独分配一个名字为 `_^this^_` 的 temp
2. 构造 `FuncDecl.args` 时，把这个 temp 作为第一个参数
3. 再把显式形参依次放到参数列表后面
4. `_ ^return^_method` 这个伪 formal 只用于语义信息和 temp 分配，不进入真正的函数参数表

因此像 `int bubbleSort(int[] array, int size)` 会被翻译成参数顺序：

```text
this, array, size
```

### 6.3 `this` 的处理

`this` 在翻译时非常直接：它就是当前方法的 `_^this^_` 对应的 `TempExp(PTR, temp)`。

例如：

- `this.i` 会先把 `this` 取出来，再根据字段偏移构造 `Mem(this + offset)`
- `this.f(x)` 会先从 `this + 方法槽偏移` 处取出函数入口，再把 `this` 自己作为第一个实参传给 `Call`

### 6.4 Unified Object Record 的记录方式

我实现了一个全局 `Class_table` 来记录统一对象记录布局：

- `var_pos_map`
  key 为 `class_name^var_name`
  value 为该字段在统一对象记录中的字节偏移

- `method_pos_map`
  key 为 `method_name`
  value 为该方法槽在统一对象记录中的字节偏移

这样设计有两个目的：

1. 字段使用 `class^field` 作为 key，可以支持字段隐藏（如子类重新定义同名字段）
2. 方法只按方法名建槽，这样父类和子类的同名方法天然共用同一个方法槽，便于实现覆盖和动态分派

对象大小统一按：

```text
(全部字段槽数量 + 全部方法槽数量) * 4
```

来分配。也就是说，不同类的对象都遵循同一个总布局，只是某些槽位会被当前类实际使用。

### 6.5 字段访问

翻译 `obj.field` 时，我会先根据 `obj` 的静态类型，从当前类一路沿继承链向上查找：

1. 当前类是否定义了该字段
2. 若没有，则找父类
3. 直到找到真正拥有该字段声明的类

找到拥有者类后，再用 `class_name^field_name` 去 `Class_table` 查询偏移，最后翻译成：

```text
Mem(obj + offset)
```

这样就能正确处理字段隐藏。例如：

- `A.i` 和 `B.i` 可以落在不同槽位
- 当表达式静态类型是 `A` 时访问 `a.i`，会命中 `A^i`
- 当表达式静态类型是 `B` 时访问 `b.i`，会命中 `B^i`

### 6.6 方法调用与多态

方法调用分两步：

1. 由方法名在 `method_pos_map` 中找到统一的方法槽偏移
2. 从对象记录的该槽位读出真正的函数入口地址，再生成 `Call`

即：

```text
Call(return_type, method_name, Mem(obj + method_offset), {obj, args...})
```

其中：

- `Mem(obj + method_offset)` 是动态读取出来的函数指针
- 参数列表的第一个参数永远是对象自身 `obj`

多态正是通过这一层“先读槽再调用”实现的。

例如 `A a; B b; a = b; a.f();`：

1. `f` 的槽位在 `A` 和 `B` 中相同
2. `a = b` 后，`a` 实际上指向一个 `B` 对象
3. 读取 `a` 的 `f` 槽时，槽里保存的是 `B^f` 的入口地址
4. 因此最终调用到的是 `B^f`，而不是 `A^f`

### 6.7 对象创建

`new A()` 的翻译过程是：

1. 按统一对象记录大小调用 `malloc`
2. 对当前类可见的每个方法槽写入实际实现地址
3. 返回对象首地址

方法地址通过 `Name(String_Label("类名^方法名"))` 写入对象记录。

例如 `new B()` 时：

- 如果 `B` 覆盖了 `f`，则 `f` 槽写入 `B^f`
- 如果 `B` 继承但没有覆盖某个方法，则写入继承链上解析到的实现，例如 `A^f`

这样对象一创建出来，就具备了动态分派所需的完整方法表。

与参考 IR 保持一致，本实现中的对象分配主要负责写入方法槽；字段是否初始化由后续赋值语句决定。

## 7. 测试与结果

### 7.1 测试范围

HW4 测试覆盖了 `irtest9 ~ irtest22`，主要场景包括：

- `new int[n]`
- 数组字面量初始化
- 数组读写
- 数组越界检查
- `length`
- 对象创建
- 字段访问与字段隐藏
- 继承与覆盖
- 动态分派
- `this`
- 递归方法调用
- 数组与对象混合使用

### 7.2 测试结果

我在 `HW4` 下执行了构建和批量生成：

```bash
make build
make run
```

所有测试用例都能够成功生成 IR，不出现崩溃或构建错误。

将生成结果与给定参考 `.3.irp` 对比后：

- `irtest9 ~ irtest20` 的结构与参考输出一致
- `irtest21`、`irtest22` 仅存在 label 编号和 `last_label` 数值差异
- 这些差异只来自 label 分配顺序，不影响 IR 的控制流语义

因此可以认为 HW4 的数组和类翻译已经正确实现。

## 8. 我是如何完成这次作业的

1. 先复用并整理 HW3 中已经工作的 AST→IR 工程骨架，保留 `Tr_ex / Tr_nx / Tr_cx` 与回填机制。
2. 阅读 `HW4/test` 中的输入样例和参考 `.3.irp`，反推出数组布局、对象布局和方法调用约定。
3. 在 `ast2tree.cc` 中补上数组相关节点：`VarDecl` 中的数组初始化、`NewArray`、`ArrayExp`、`Length`、`PutArray`、`GetArray`。
4. 实现数组访问中的边界检查逻辑，并用 `Eseq` 保证复杂表达式只求值一次。
5. 实现 `Class_table`，把所有类字段和方法整理成统一对象记录的槽位映射。
6. 实现成员方法翻译，把方法重命名为 `类名^方法名`，并在参数列表中显式加入 `this`。
7. 实现 `ClassVar`、`This`、`CallExp`、`CallStm`、`NewObject` 等节点，完成字段访问、对象创建和动态分派。
8. 反复构建、逐个对照测试输出，修正数组访问地址计算、字段拥有者查找、方法槽偏移和控制流拼接细节。

## 9. 小结

HW4 的核心工作是把“数组和类”也统一到 Tiger IR+ 的 `Temp / Mem / Call / Eseq / Cjump` 体系中。完成之后可以发现：

- 数组本质上是“带长度头部的线性内存块”
- 对象本质上是“带字段槽和方法槽的统一记录”
- 字段访问和数组访问本质上都是“基址 + 偏移”的 `Mem`
- 多态本质上是“对象记录中的方法槽 + 间接调用”

因此，虽然源语言表面上增加了很多高级结构，但在 IR 层最终都可以还原为少量统一的底层操作。
