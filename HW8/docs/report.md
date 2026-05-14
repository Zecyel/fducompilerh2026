# FDU Compiler HW8 实验报告

## 基本信息

- 学号：`23300240014`
- 姓名：`Zecyel(朱程炀)`
- 作业：Homework Assignment 8（稀疏条件常量传播优化）

## 1. 环境配置

本次作业在 Linux 环境完成，核心工具链如下：

- 编译器：`g++`，支持 C++17
- 构建系统：`Makefile + CMake + Ninja`
- 测试方式：运行 `HW8/test` 中的 `opttest1` 到 `opttest9`

项目提供的主要命令如下：

```bash
make build
make run
make run-one FILE=opttest1
```

## 2. 整体架构

HW8 的任务是在 SSA Quad 上实现稀疏条件常量传播（Sparse Conditional Constant Propagation，SCCP）。输入文件为 `.4-ssa-xml.quad`，优化后的结果输出为 `.4-ssa-opt.quad`。

本次实现集中在一个文件中：

- `lib/opt/opt.cc`：实现 SCCP 分析、函数体重写和程序级优化入口

整体流程如下：

```cpp
quad::QuadProgram *x3 = xml2quad(file_quad_ssa_xml.c_str());
QuadProgram *x4 = optProg(x3);
x4->print(output_str, 0, false);
```

我将优化拆成两个阶段：

1. `calculateBT()`：计算基本块可达性和 SSA temp 的运行时值格。
2. `modifyFunc()`：根据分析结果重写 Quad 函数，删除不可达块、替换常量、清理 Phi、简化条件跳转。

## 3. 核心数据结构

### 3.1 运行时值格

`RtValue` 表示一个 SSA 临时变量在运行时可能具有的值性质：

- `NO_VALUE`：当前还没有有效信息，相当于格中的 bottom。
- `ONE_VALUE`：该 temp 在所有可达路径上都是同一个整数常量。
- `MANY_VALUES`：该 temp 可能有多个值，不能当作常量使用。

SCCP 的关键就是在这个格上做单调传播。我的合并规则是：

- `NO_VALUE meet x = x`
- `ONE_VALUE(a) meet ONE_VALUE(a) = ONE_VALUE(a)`
- `ONE_VALUE(a) meet ONE_VALUE(b) = MANY_VALUES`，其中 `a != b`
- 只要一边是 `MANY_VALUES`，结果就是 `MANY_VALUES`

### 3.2 可执行边和可执行块

框架中已有：

- `block_executable`：记录每个基本块是否可达。
- `label2block`：从 label 编号找到对应基本块。

我额外在 `opt.cc` 中维护了当前 `Opt` 对象对应的可执行控制流边集合：

```cpp
map<Opt*, set<pair<int, int>>> executable_edges;
```

原因是 Phi 节点不能只看某个前驱块是否可达，还必须看“前驱到当前块的边”是否真的可执行。比如一个前驱块中的 `CJUMP` 被常量条件化简后，另一个分支边不再存在，即使目标块原本在 CFG 中有这条边，也不应该再影响 Phi 的结果。

### 3.3 定义过的 temp 集合

我还记录了函数中真正由语句定义过的 temp：

```cpp
map<Opt*, set<int>> defined_temps;
```

这样做是为了区分两类 `NO_VALUE`：

- SSA 定义还在不可达路径中，所以暂时没有值。
- 某个 temp 在可达路径上被使用，但函数内没有定义它，例如测试中的 `t100` 或 `t101`。

后一种情况不能继续当成常量，需要提升为 `MANY_VALUES`，否则会错误删除相关 Phi 或 Return。

## 4. `calculateBT()` 的实现

`calculateBT()` 负责同时求解控制流可达性和 SSA temp 的常量性质。实现步骤如下。

### 4.1 初始化

初始化时遍历函数中的所有基本块：

1. 建立 `label2block`。
2. 将所有 `block_executable[label]` 设为 `false`。
3. 收集所有会定义 temp 的语句，包括 `MOVE`、`MOVE_BINOP`、`LOAD`、`MOVE_CALL`、`MOVE_EXTCALL`、`PHI`、`PTR_CALC`。
4. 将函数入口块标记为可执行。
5. 将函数参数初始化为 `MANY_VALUES`，因为参数值来自函数外部，优化阶段无法假定它是常量。

### 4.2 不动点迭代

核心循环是一个普通的不动点迭代：只要可执行边、可执行块或 temp 值发生变化，就重新扫描所有可执行块。

对每类语句的处理如下：

- `MOVE`：将右侧 term 的值传播给目标 temp。
- `MOVE_BINOP`：若左右操作数都是常量，则尝试计算结果。
- `LOAD`、`PTR_CALC`、`MOVE_CALL`、`MOVE_EXTCALL`：结果通常依赖内存或外部调用，因此定义为 `MANY_VALUES`。
- `STORE`、`CALL`、`EXTCALL`、`RETURN`：不定义可传播常量，但需要访问其操作数，发现未定义值时提升为 `MANY_VALUES`。
- `JUMP`：标记唯一后继边可执行。
- `CJUMP`：若条件可算成常量，只标记对应一条边；否则标记 true 和 false 两条边。
- `PHI`：只考虑可执行前驱边传来的参数，忽略不可达前驱。

### 4.3 除零处理

作业特别指出除以零的表达式不应折叠。我在二元运算求值中对 `/` 和 `%` 做了特殊处理：

```cpp
if (rhs == 0) {
    ok = false;
    return 0;
}
```

当 `ok == false` 时，不把表达式折叠成常量，而是将结果提升为 `MANY_VALUES`。这样后续重写时仍会保留原来的除法或取模语句，只会把已知操作数替换为常量。

例如测试 `opttest3` 中的：

```text
MOVE_BINOP t10101:int <- (/, Const:1, t10300:int)
```

其中 `t10300` 可传播为 `0`，最终会被重写为：

```text
MOVE_BINOP t10101:int <- (/, Const:1, Const:0);
```

但不会继续把它折叠掉。

## 5. `modifyFunc()` 的实现

`modifyFunc()` 根据 `calculateBT()` 的结果创建新的 `QuadFuncDecl`。它不会直接在原 block 中原地删除，而是构造新的 block 列表。

### 5.1 删除不可达块

遍历原函数的所有基本块时，如果 `block_executable[label] == false`，直接跳过该块。

保留下来的块会重新计算 `exit_labels`。新的出口只包含已经被 `calculateBT()` 标记为可执行的边。这样当 `CJUMP` 被常量化简后，删除的分支不会再出现在 block 的出口列表中。

### 5.2 常量替换和无用赋值删除

对普通语句，我统一先调用 `replaceTerm()` 替换操作数：

- 如果 term 是常量或 name，直接复制。
- 如果 term 是 temp 且 `temp_value[temp] == ONE_VALUE`，替换成 `Const:value`。
- 否则保留原 temp。

对于定义出常量的赋值语句，我会删除该定义。例如：

```text
MOVE t10000:int <- Const:1
```

如果 `t10000` 已知是常量 `1`，后续使用点都会被替换为 `Const:1`，因此这条赋值本身不再需要保留。

同理，如果 `MOVE_BINOP` 的结果已经被分析为常量，也会删除该语句。

### 5.3 条件跳转简化

`CJUMP` 在操作数替换后如果两侧都是常量，就直接计算条件：

```text
CJUMP != Const:1 Const:0? L102 : L103
```

会被重写为：

```text
JUMP L102
```

如果条件仍不是常量，就保留 `CJUMP`，但其中能替换为常量的操作数仍然会被替换。

### 5.4 Phi 节点处理

Phi 节点是本次实现中最容易出错的部分。我按以下顺序处理：

1. 先删除不可执行前驱边对应的 Phi 参数。
2. 如果所有有效输入都是同一个常量，删除 Phi，后续使用点直接替换为常量。
3. 如果只剩一个有效输入，并且该输入不是常量，将 Phi 改成普通 `MOVE`。
4. 如果还需要保留 Phi，但某些参数是常量，则在对应前驱块末尾插入一条新 `MOVE`，再让 Phi 引用新 temp。

第 4 点是为了保持 Quad Phi 的结构，因为 `QuadPhi` 的参数类型是 `Temp*` 而不是 `QuadTerm*`，不能直接把常量写进 Phi 参数列表。例如：

```text
PHI t10102:int <- Phi([Const:1, L102], [Const:0, L103])
```

在当前 IR 中不能这样表达，所以要改成：

```text
L102:
  MOVE t106:int <- Const:1
  JUMP L104
L103:
  MOVE t107:int <- Const:0
  JUMP L104
L104:
  PHI t10102:int <- Phi([t106, L102], [t107, L103])
```

这也是 `opttest5` 中需要额外生成 temp 的原因。

### 5.5 插入 Phi 常量参数对应的 MOVE

当 Phi 的某条可达入边需要把常量变成 temp 时，我将新 `MOVE` 记录到 `pending_pred_moves[pred_label]` 中。重写前驱块时，在终结语句之前插入这些 MOVE。

这样可以保证新 temp 的定义仍然支配对应的 Phi 入边，同时不会被插到 `JUMP` 之后变成不可执行代码。

## 6. `optProg()` 的实现

`optProg()` 按函数粒度应用优化：

```cpp
QuadProgram* newProg = new QuadProgram(new vector<QuadFuncDecl*>(), prog->last_label_num, prog->last_temp_num);
for (int i = 0; i < prog->quadFuncDeclList->size(); i++) {
    Opt optthis(prog->quadFuncDeclList->at(i));
    newProg->quadFuncDeclList->push_back(optthis.optFunc());
}
```

每个函数都独立构造一个 `Opt` 对象，先执行 `calculateBT()`，再执行 `modifyFunc()`，最后加入新的 `QuadProgram`。

## 7. 测试与结果

### 7.1 测试范围

本次作业提供了 `opttest1` 到 `opttest9`。这些测试覆盖了以下场景：

- 常量条件分支删除
- 常量赋值删除
- Phi 节点在不可达前驱删除后的清理
- 除以零表达式保留
- 循环体保持可达
- 外部输入 `getint()` 导致的 `MANY_VALUES`
- Phi 参数为常量时补临时变量
- 两条可达路径汇合后得到相同常量
- 未定义但可达使用的 temp 保留为非常量

### 7.2 测试结果

我逐一运行了 `opttest1` 到 `opttest9`，生成的 `.4-ssa-opt.quad` 均与给定参考输出一致。

典型结果包括：

- `opttest1`：常量条件 `1 != 0` 被化简为只跳转到 true 分支，false 块被删除。
- `opttest3`：`1 / 0` 没有被折叠，仍然保留运行时除法语句。
- `opttest4`：常量为真的循环条件保留可达循环体，没有错误删除循环。
- `opttest5`：Phi 的常量参数被转换为前驱块中的 MOVE，再由 Phi 接收 temp。
- `opttest8`：两条分支都返回常量 `9`，Phi 被删除，Return 直接改为 `Const:9`。

## 8. 我是如何完成这次作业的

1. 先阅读 `README.md`、`opt.hh`、`quad.hh` 和 `quad.cc`，明确 `RtValue`、`QuadPhi`、`QuadTerm`、`def/use` 和打印格式。
2. 对照 `opttest1` 到 `opttest9` 的 SSA 输入和参考输出，整理出 SCCP 必须处理的优化情形。
3. 实现值格合并函数、term 求值函数和二元运算折叠函数。
4. 在 `calculateBT()` 中同时传播可执行边和 temp 值，保证 Phi 只使用可执行前驱边。
5. 在 `modifyFunc()` 中重建函数体，删除不可达块并替换常量使用点。
6. 单独处理 Phi 节点，特别是不可达入边、单入边 Phi、全常量 Phi 和常量参数转 temp 的情况。
7. 反复运行测试并对照参考输出，修正 last_temp、Phi 清理和前驱 MOVE 插入位置等细节。

## 9. 小结

HW8 的核心是把常量传播和控制流可达性放在同一个不动点中联合求解。相比普通常量传播，SCCP 可以利用条件跳转的常量结果删除不可达分支，再反过来让 Phi 节点忽略不可达前驱，从而得到更多常量。

完成本次作业后可以看到，SSA 形式非常适合做这类优化：每个 temp 只有一个定义，Phi 明确表达路径合流，只要正确维护可执行边，就能比较直接地实现稀疏传播和分支删除。
