# FDU Compiler HW9 实验报告

## 基本信息

- 学号：`23300240014`
- 姓名：`Zecyel(朱程炀)`
- 作业：Homework Assignment 9（Loop Header 识别与 Loop Invariant Hoisting）

## 1. 环境配置

本次作业在 Linux 环境完成，核心工具链如下：

- 编译器：`g++`，支持 C++17
- 构建系统：`Makefile + CMake + Ninja`
- 测试方式：运行 `HW9/test` 中的 `opttest1` 到 `opttest10`

项目提供的主要命令如下：

```bash
make build
make run
make run-one FILE=opttest1
```

## 2. 整体架构

HW9 的目标是在已经带有控制流、支配关系和活跃变量信息的 SSA Quad 上完成简单循环优化，主要包括两部分：

1. 识别函数中的自然循环及其 loop header。
2. 将 loop invariant statement 提取到 loop preheader 中。

输入文件为 `.4-ssa-withflow-xml.quad`，输出为 `.4-ssa-loopopt.quad`。主程序流程如下：

```cpp
set<FuncFlowInfo*>* sffi = xml2flow(file_quad_ssa_withflow_xml.c_str());
LoopHeaderMap *loopHeaderMap = findLoopHeaders(func, ffi);
QuadFuncDecl* optimizedFunc = loopHoistFunc(func, loopHeaderMap);
```

本次实现集中在两个文件中：

- `lib/opt/findloopheader.cc`：根据 CFG 和 dominator 信息寻找自然循环。
- `lib/opt/loophoistfunc.cc`：识别并移动 loop invariant statement。

## 3. Loop Header 识别

### 3.1 Back Edge 判定

在控制流图中，一条边 `B -> H` 是 back edge，当且仅当目标块 `H` 支配源块 `B`。也就是说，只要程序执行到 `B`，此前一定已经经过了 `H`，此时从 `B` 跳回 `H` 就形成了一个循环。

已有的 `ControlFlowInfo` 中包含：

- `successors`：每个基本块的后继集合。
- `predecessors`：每个基本块的前驱集合。
- `dominators`：每个基本块的支配者集合。

因此判断一条边是否为 back edge 非常直接：

```cpp
bool dominates(ControlFlowInfo *cfi, int dom, int block) {
    auto it = cfi->dominators.find(block);
    return it != cfi->dominators.end() && it->second.count(dom) > 0;
}
```

遍历所有 `successors[from]`，如果 `to` 支配 `from`，则 `to` 是一个 loop header。

### 3.2 Natural Loop Body 构造

发现 back edge `latch -> header` 后，需要构造这个自然循环的 body。做法是从 latch 反向沿 predecessor 边搜索，直到 header 为止：

1. 初始集合包含 `header` 和 `latch`。
2. 将 `latch` 入栈。
3. 不断弹出一个 block，把它的所有 predecessor 加入 body。
4. 对新加入且不是 header 的 predecessor 继续入栈。

这样得到的集合就是该 back edge 对应的自然循环体。

### 3.3 多条 Back Edge 的合并

同一个 header 可能有多条 back edge。为了避免同一个 header 被重复记录，我用：

```cpp
map<int, set<int>> header_to_body;
```

将相同 header 的 loop body 做并集。最后再统一生成 `LoopHeader` 对象加入 `LoopHeaderMap`。

## 4. Loop Hoisting 的基本原则

README 中给出的关键假设是：

- 每个 loop header 已经有 preheader。
- 每个 method call 或 ext call 都可能有副作用。
- 每个 memory load 都可能读到不同的值，即使地址相同。

因此我只 hoist 没有副作用且计算结果只依赖 loop invariant 值的语句：

- `MOVE`
- `MOVE_BINOP`
- `PTR_CALC`

以下语句不会被 hoist：

- `LOAD`：内存值可能变化。
- `STORE`：有副作用。
- `CALL`、`MOVE_CALL`、`EXTCALL`、`MOVE_EXTCALL`：可能有副作用或依赖外部状态。
- `PHI`：表示控制流合流，不应移动。
- `LABEL`、`JUMP`、`CJUMP`、`RETURN`：控制流语句，不移动。

## 5. Loop Invariant 判定

### 5.1 Def 所在块

在 hoist 前，我先扫描整个函数，建立 temp 到定义所在 block 的映射：

```cpp
map<int, int> def_block;
```

这样在判断某个 operand 是否 invariant 时，可以区分它是在 loop 内定义还是 loop 外定义。

### 5.2 Operand Invariant 规则

一个 `QuadTerm` 是 loop invariant，当满足以下任意条件：

1. 它不是 temp，例如 `Const` 或 `Name`。
2. 它是 temp，但没有在当前函数中定义，视为来自参数或外部输入。
3. 它是 temp，定义所在 block 不属于当前 loop body。
4. 它是 temp，虽然定义在 loop body 内，但该定义语句已经被判定为 invariant 并准备 hoist。

第 4 条用于处理链式不变式。例如：

```text
t10001 <- Const:10
t10100 <- t10001 + 1
t10200 <- t10001 + t10100
```

第一条先被识别为 invariant，随后第二条、第三条也能依次变成 invariant。

### 5.3 Statement Invariant 规则

对于可 hoist 的语句，只要其所有输入 operand 都是 invariant，该语句本身就是 invariant。例如：

- `MOVE t <- Const:3` 总是 invariant。
- `MOVE t <- other_temp` 只有当 `other_temp` invariant 时才 invariant。
- `MOVE_BINOP t <- op(a, b)` 需要 `a` 和 `b` 都 invariant。
- `PTR_CALC t <- base + offset` 需要 `base` 和 `offset` 都 invariant。

实现上使用一个不动点循环，不断扫描 loop body，直到再也找不到新的 invariant statement。

## 6. Preheader 选择与语句移动

### 6.1 Preheader 查找

作业假设每个 loop header 已经有 preheader。因此对一个 loop，我查找所有不在 loop body 中、但 exit label 指向 header 的基本块，找到的块就是 preheader。

例如：

```text
L110 -> L102
L109 -> L102
```

如果 `L102` 是 loop header，且 `L109` 在 loop body 内，那么 `L110` 就是 preheader。

### 6.2 插入位置

hoist 出来的语句需要插入到 preheader 的终结语句之前。通常 preheader 末尾是：

```text
JUMP Lheader
```

因此最终结构变成：

```text
LABEL Lpreheader
...
hoisted statement 1
hoisted statement 2
JUMP Lheader
```

这样可以保证 hoisted statement 在进入 loop 前执行，并且不会破坏原本的控制流。

### 6.3 嵌套循环处理

嵌套循环中，同一条语句可能对内层循环 invariant，也可能对外层循环 invariant。为了尽量把不变式提到最外层可合法的位置，我按 loop body 大小从大到小处理循环：

```cpp
sort(loops.begin(), loops.end(), [](LoopHeader *a, LoopHeader *b) {
    if (a->bodyBlocks.size() != b->bodyBlocks.size()) return a->bodyBlocks.size() > b->bodyBlocks.size();
    return a->headerLabel < b->headerLabel;
});
```

也就是说，外层循环先处理，内层循环后处理。

这可以解释测试中的两个典型情况：

- 如果语句对外层循环也 invariant，就直接移动到外层 preheader。
- 如果语句只对内层循环 invariant，就保留在外层循环内部，移动到内层 preheader。

## 7. 测试与结果

### 7.1 测试范围

本次测试包括 `opttest1` 到 `opttest10`，覆盖了以下场景：

- 单层 while 循环。
- 嵌套 while 循环。
- loop preheader 不同位置的情况。
- 条件分支中的 invariant statement。
- 多条 invariant statement 的链式依赖。
- method call、ext call、load、store 不可 hoist 的情况。
- 多函数输入。

### 7.2 典型测试说明

`opttest1` 中：

```text
MOVE t10101:int <- t10000:int
```

该语句在 loop body 中，但 `t10000` 来自 loop 外的 `getint()` 结果，在 loop 内不会重新定义，因此可以移动到 preheader。

`opttest4` 中：

```text
MOVE_BINOP t10202:int <- (*, Const:3, t10002:int)
```

它依赖的 `t10002` 定义在内层循环之外，但仍在外层循环之内。因此它不能被提到外层 preheader，只能提到内层 preheader。

`opttest6` 中包含对象创建、store、load 和 method call。由于这些语句可能有副作用或依赖内存状态，所以都不会被 hoist。只有纯计算且操作数 invariant 的语句会被移动。

`opttest9` 中存在链式不变式：

```text
t10001 <- 10
t10100 <- t10001 + 1
t10200 <- t10001 + t10100
t10403 <- 3 * t10200
```

这些语句可以依次被识别为 invariant，并整体移动到外层 preheader 中。

### 7.3 测试结果

我逐一运行了 `opttest1` 到 `opttest10`。生成的 `.4-ssa-loopopt.quad` 与参考输出在优化内容上保持一致：

- loop headers 均能正确识别。
- 可 hoist 的纯计算语句被移动到正确 preheader。
- `LOAD`、`STORE`、`CALL`、`EXTCALL` 均按要求保留在原位置。
- 嵌套循环中，不变式会被移动到最外层合法 preheader。

## 8. 我是如何完成这次作业的

1. 阅读 `README.md`、`loopopt.hh`、`flowinfo.hh` 和 `quad.hh`，明确输入中已有的 CFG、支配关系和 SSA Quad 结构。
2. 对照 `opttest1` 到 `opttest10` 的输入和参考输出，总结哪些语句应该被 hoist、哪些语句必须保留。
3. 在 `findloopheader.cc` 中根据 back edge 规则识别 loop header，并反向遍历 predecessor 构造 natural loop body。
4. 在 `loophoistfunc.cc` 中建立 temp 到定义 block 的映射，用不动点方法识别 loop invariant statement。
5. 按 loop body 大小从大到小处理嵌套循环，让不变式尽量提到最外层合法 preheader。
6. 删除原位置中的 hoisted statement，并将它们插入 preheader 的终结跳转前。
7. 运行全部测试并对照输出，修正嵌套循环和链式不变式的处理细节。

## 9. 小结

HW9 的核心是利用支配关系识别自然循环，再利用 SSA 的单定义性质判断某条语句是否真正依赖循环内变化的值。SSA 形式让 loop invariant 判断变得比较直接：只要沿 def-use 关系检查 operand 的定义位置，就能判断一个表达式是否可安全提前。

本次实现的 hoisting 是保守的：凡是可能有副作用或依赖内存状态的语句都不移动，只处理纯计算语句。这样虽然不会覆盖所有可能的优化机会，但符合本次作业给出的假设，也能保证优化后的 Quad 保持原程序语义。
