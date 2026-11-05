# LuaTeX 常驻渲染与 checkpoint 可行性研究

## 结论

**可以沿着 StemTeX 的方向做，但不能直接移植 XeTeX 的快照代码。**

- **一次预加载、同进程连续排版：可行，已做原版引擎实验。**
- **运行中 DVI 截取、补尾并转 PDF/SVG：简单传统字体和 OpenType 数学场景已通过；第 8 节的实验引擎已实现逐页立即 flush，并通过真实请求测试。**
- **出错后在原进程恢复：需要引擎级改造；当前尚未实现或证明。**
- **任意 LuaLaTeX 宏包 / Lua 代码的完整事务回滚：不能据此承诺。**
  TeX 内存、Lua VM、字体/节点引用和输出后端必须保持一致。
- 推荐面向本机实际的 **LuaHBTeX / LuaLaTeX** 做原型，而不是只做不含
  OpenType 字体加载器的 plain LuaTeX 演示。

前期研究使用原版引擎探针；后续逐页 DVI flush 实现见第 8 节，GUI 实验入口见第 9 节。
正式 renderer API 和 GUI 默认仍使用现有 XeTeX 后端；GUI 可显式选择 LuaHBTeX 实验后端。

## 1. 先准确界定现有 XeTeX 能力

`cpp-daemon/stemtex_renderer.cpp` 的 worker：

1. 加载固定 preamble，执行首次 warmup。
2. `stemtex:checkpoint` 保存一次基线，输出 `WORKER_BASELINE_READY`。
3. 正常请求通过 `\begingroup` 和 `\stemtexresetfragmentstate` 隔离常见状态。
4. TeX 错误触发引擎内恢复；宿主等到 `STEMTEX_RESTORED` 后的
   `WORKER_WAIT` 才确认可以接收下一请求。

**成功请求后并没有无条件恢复完整 checkpoint。** 因此“与现在一样的错误恢复”
和“每次请求都是严格无状态事务”是两种不同目标。全局赋值等任意副作用也不能仅凭
正常路径的分组就认为已隔离。

现有 snapshot 位于 `texlive-xetex/src/web2c/xetex0.c`，包括 TeX 用户状态和
worker continuation；格式文件清单用于审计，但不是直接调用 `\dump`。
取消、进程崩溃及某些依赖层退出仍由进程重建处理。

## 2. 本机实验：哪些已经证实

环境：Windows，TeX Live 2026。

- `luatex --version`：LuaTeX 1.24.0，development id 7724。
- `luahbtex --version`：LuaHBTeX 1.24.0，development id 7724。
- `lualatex --version`：实际运行 LuaHBTeX 1.24.0。

### 2.1 分组、Lua 错误与序列化边界

文件：

- `experiments/luatex-daemon/state-boundary.tex`
- `experiments/luatex-daemon/state-boundary.lua`

原版 LuaTeX 和 LuaHBTeX 上各通过 9 项断言：

| 检查 | 观察 |
| --- | --- |
| TeX 局部 count 赋值 | 离开分组后恢复 |
| Lua 全局变量 | 离开分组后仍被修改 |
| Lua table 内容 | 修改保留 |
| Lua closure upvalue | 修改保留 |
| Lua 持有的 node 内容 | 修改保留 |
| node 的 Lua property | 修改保留 |
| callback 注册 | 修改保留 |
| `pcall` 内修改后抛错 | 错误被捕获，修改不撤销 |
| `string.dump` / `load` | 不保留原 closure 的 upvalue 值 |

这些是**原版引擎的状态边界实验，不是 checkpoint 成功实验**。
节点实验仅操作仍然有效的节点，最后正常释放，不故意解引用悬空节点。

### 2.2 一次 preamble、重复数学排版

文件：`experiments/luatex-daemon/warm-typesetting.tex`。

使用 LuaLaTeX、`unicode-math`、Latin Modern Math 和五个 `sym` 选项：

- preamble 执行一次；
- warmup 后同进程连续排版相同数学公式 20 次；
- 每次盒子的宽、高、深与基线一致；
- 本机基线：`185.12297pt, 14.65701pt, 11.098pt`。

没有 shipout，所以不产生页面。这证明字体和宏包初始化可以复用，**没有证明**
请求通信、实时文件输出、错误恢复、长期内存稳定性或性能优于 XeTeX。

第一次在文件沙箱内运行时，luaotfload 因缓存不可写而失败；授权同一命令后通过。
这是运行环境限制，不是 warm worker 的否定证据，也说明字体缓存有独立的资源生命周期。

### 2.3 补充验证：stdin 请求循环与未结束的 PDF

新增 `experiments/luatex-daemon/live-output-probe.py`，使用本机原版
LuaLaTeX / LuaHBTeX 1.24.0，加载 `unicode-math`，先排一个 warmup 盒子，
再通过 stdin 逐次发送三个请求文件名。每次请求都有 `WAIT / BOX / DONE`
标记，并实际执行一次 `\shipout`。第三次请求后阻塞等待宿主发出停止指令。

结果：

- 同一 PID 完成三个请求；第一次与第三次相同公式的宽、高、深完全一致。
- 三次 shipout 后宿主读取的 PDF 分别为 246、461、706 字节，均没有
  `startxref` 和 `%%EOF`。停止前保存的 `before-stop.pdf` 经 `pdfinfo`
  检查，报缺少 trailer dictionary / xref table，不能作为完整 PDF 读取。
- 发出停止指令、执行 `\end{document}` 后，PDF 为 5620 字节，具有
  `startxref` 和 `%%EOF`；`pdfinfo` 正常识别为 3 页。
- 这证明**常驻接收请求和实际 shipout 不需要先修改引擎**；也证明在这个
  原版 PDF 路径中，逐页产生字节不等于逐请求发布可用文件。

脚本将字体缓存放在 `build/luatex-live-output-probe/cache`，无需修改全局缓存。
首次创建这份缓存时就绪约 15.5 秒，复用后约 1.48 秒；复用缓存那次的三个请求
分别约 56.4、1.76、1.51 毫秒。这只是两次功能探针的观察，不是性能基准：
warmup 没有 shipout，第一次请求仍包含输出后端和字体的惰性初始化；计时也不包含
完整 PDF 发布或 SVG 转换，不能直接与现有 renderer 延迟比较。

本次还复跑了两个引擎的状态边界探针（各 9 项通过）和 20 次重复数学布局探针。
以上实验仍**没有实现 checkpoint、坏请求恢复、每请求独立 PDF 或长期稳定性验证**。

复现：在仓库根目录用可用的 Python 3 执行：

```powershell
python experiments/luatex-daemon/live-output-probe.py
pdfinfo build/luatex-live-output-probe/before-stop.pdf
pdfinfo build/luatex-live-output-probe/worker.pdf
```

第一个 `pdfinfo` 预期失败。完整控制台记录和结构化结果分别保存在该输出目录的
`console.log` 与 `result.json`。Python 必须是真实可执行的安装，不能是不可用的
Windows Store 命令别名。

### 2.4 补充验证：DVI 能在进程结束前输出并转换，但缺逐页 flush

探针：`experiments/luatex-daemon/live-dvi-probe.py`。原版 LuaTeX / LuaHBTeX
1.24.0，通过 `--output-format=dvi` 启动；每个进程逐次接收三个请求，每次实际
shipout 后停下来等 stdin。宿主此时读取 DVI 快照，按 DVI 指令解析完整的
`bop ... eop` 页面，不以搜索某个字节代替解析。

四组实测如下；括号内是宿主能解析到的完整页面数：

| 场景 | 第一次 shipout 后 | 第二次 shipout 后 | 第三次 shipout 后 | 正常结束后 |
| --- | ---: | ---: | ---: | ---: |
| plain LuaTeX / cmr10 / 小片段 | 0 B (0) | 0 B (0) | 0 B (0) | 620 B (3) |
| plain LuaTeX / cmr10 / 大页面 | 20480 B (0) | 53248 B (1) | 86016 B (2) | 106436 B (3) |
| LuaLaTeX / unicode-math / 小片段 | 0 B (0) | 0 B (0) | 0 B (0) | 1280 B (3) |
| LuaLaTeX / unicode-math / 大页面 | 28672 B (0) | 77824 B (1) | 126976 B (2) | 141188 B (3) |

大页面是 300 行重复内容，用于触发缓冲区写出，不是正常尺寸的 snippet。
Unicode 组使用 Latin Modern Roman / Latin Modern Math，包括分数、求和与上下标。
上表记录最初原版探针的字节数；当前脚本已增加请求编号和结构校验，重跑时具体
字节数会变化，缓冲/完整页数及转换结果才是该实验的判断依据。

**已经实现并验证的运行中转换**：对大页面组的第二、第三次快照，只保留截至最后
完整 `eop` 的字节，收集其中出现的字体定义，按当前页面数和 BOP 位置补 DVI
postamble / post_post；然后在原引擎仍然阻塞等请求时调用本机 TeX Live 的
`dvipdfmx` 和 `dvisvgm`，两组均成功得到 PDF / SVG。补尾没有使用引擎结束后的
字体表或其他未来数据，也没有关闭或改写引擎正在写的文件。

校验包括：运行中完整页面的字节前缀与最终文件一致；生成的第一页 SVG 在展开
字形引用、忽略字体定义编号及排列次序后，路径和位置与正常结束后的第一页一致。
Unicode 组的 PDF 嵌入了预期的 LM Roman / Latin Modern Math，抽取文本包含目标
公式；渲染核对未见公式字形错误。大页面默认 PDF 纸张没有随压力内容变高，因此
PDF 可视区域不是全部 300 行的验证，整页内容比较以 SVG 几何为准。

这个结果收窄了之前的输出风险判断：**简单 Latin Modern + unicode-math 场景下，
DVI → PDF/SVG 路线已验证可用，不仅限于传统 TFM 字体。** 本机
`luaotfload-dvi.lua` 在 shipout 前将字形转换为后端字体/GID，并通过 DVI 字体名
携带字体文件信息；因此无需把它先转换成 XDV。这里只测试了当前 TeX Live 转换器，
没有验证 StemTeX 打包的热转换器 ABI、CJK、图片或任意宏包的 specials。

**仍缺的一步是当前页及时落盘。** 小片段连出三页仍为 0 字节；大页面也会留下
当前页尾部在缓冲区内，不能保证当前请求结束就能取出其完整页面。上游
[`dvi/dvigen.c`](https://github.com/TeX-Live/texlive-source/blob/trunk/texk/web2c/luatexdir/dvi/dvigen.c)
的 `dvi_end_page()` 只追加 `eop`；`dvi_swap()` 按半缓冲区写出，`write_dvi()`
通过 `fputc` 写入 C 文件流。因此需要兼顾 DVI 内部缓冲和 C stdio 缓冲。

下一步可具体评估在 `dvi_end_page()` 增加显式逐页 flush：保持 DVI 偏移、
双缓冲指针和后续写入一致，再 `fflush` 文件流。**这与 XeTeX 的逐页 XDV flush
改法非常接近。这个原版探针阶段没有修改或编译 LuaTeX 引擎；后续补丁见第 8 节。**
输出路线与 TeX/Lua checkpoint 是两件独立工作，DVI 可流式读取不代表错误恢复完成。

复现：

```powershell
python experiments/luatex-daemon/live-dvi-probe.py
```

脚本需要 PATH 上可用的 `luatex`、`lualatex`；转换测试使用 `dvipdfmx` 和
`dvisvgm`。输出位于 `build/luatex-live-dvi-probe`，包含每次读取的原始快照、
补尾副本、转换结果、日志和 `results.json`。没有修改正式 renderer。

## 3. 源码与官方手册支持的判断

阅读的上游源码来自 TeX Live `trunk`，属于架构参考，不代表已锁定与本机二进制
完全相同的提交；开始引擎补丁前必须固定版本、依赖和构建方式。

### 3.1 TeX 状态层可沿用“按格式清单审计”的方法

[`tex/dumpdata.c`](https://github.com/TeX-Live/texlive-source/blob/trunk/texk/web2c/luatexdir/tex/dumpdata.c)
的 `store_fmt_file()` / `load_fmt_file()` 涵盖字符串、节点和 token 内存、eqtb、hash、
math/text codes、字体、math data、language data 等。

但 LuaTeX 的数据结构与 XeTeX 不同：例如节点/token 区分、属性与属性列表、字体对象、
语言对象和额外代码表。需要重新建立“快照 / 保留 / 重建 / 释放”的所有权清单，
不能按 XeTeX 的全局变量名称逐个复制。

### 3.2 格式文件不是 Lua VM 快照

[`lua/llualib.c`](https://github.com/TeX-Live/texlive-source/blob/trunk/texk/web2c/luatexdir/lua/llualib.c)
的 `dump_luac_registers()` 保存 bytecode registers 和 chunk names；这不是序列化
全部 Lua heap、registry、closures、coroutines 和 C userdata。

本机官方手册 `luatex-tex.tex` 的 “Bytecode registers” 明确说明：保存的是 Lua
bytecode，函数不得带 upvalues；加载后执行代码才能重新创建定义。
因此 `.fmt`、`string.dump` 或重新执行少量初始化代码，都不自动等价于 post-warmup
LuaLaTeX 状态恢复。

### 3.3 Lua 与 TeX 不是可独立回滚的两个黑盒

本机手册 `luatex-nodes.tex` 的 “Direct access” 说明，node 的内部标识是节点内存表
索引，userdata 和 direct 引用可以互相转换；properties 又存储于 Lua 表。

如果只恢复 TeX 节点池，Lua 表或闭包可能仍持有请求中新分配的节点编号；该编号
随后可能失效或被复用。反过来只清理 Lua 全局表，也不会恢复 TeX 节点、字体和
registry 中的关系。

同样需要审计 luaotfload 字体表、callback 链和资源缓存。**禁止用户写 `\directlua`
也不意味着没有 Lua 状态**：LuaLaTeX 自身的字体处理就使用 Lua。

### 3.4 错误回调不是恢复 API

[`tex/errors.c`](https://github.com/TeX-Live/texlive-source/blob/trunk/texk/web2c/luatexdir/tex/errors.c)
中的 `error()` 仍会进行错误计数、交互或退出处理；`normal_error()` 有直接退出路径。
本机官方手册 `luatex-callbacks.tex` 的 `show_error_hook`、`show_error_message`
说明它们用于错误报告，不会把错误处理变成事务回滚。

[`tex/maincontrol.c`](https://github.com/TeX-Live/texlive-source/blob/trunk/texk/web2c/luatexdir/tex/maincontrol.c)
既有主 `main_control()`，也有嵌套的 `local_control()`。
`llualib.c` 的 Lua 调用路径使用 `lua_pcall`、Lua 栈深度和 `lua_active` 计数。

因此不能从任意 TeX/Lua 错误位置直接 `longjmp` 到顶层再假定 Lua VM 已清理。
需要设计安全的退出边界、Lua protected-call 展开和 C 资源清理；恢复点应没有
存活的请求级 Lua/C 调用帧。OOM、原生崩溃、无限循环/取消等仍应允许杀 worker。

### 3.5 输出链也必须单独设计

本机手册 `luatex-enhancements.tex` 的 `outputmode`：0 是 DVI，1 是 PDF。
不能把 DVI 直接当作现有 XeTeX XDV，也不能假定 OpenType/CJK 字体和 specials
会无修改地通过当前转换器。

原生 PDF 路线需要解决：每请求文件的结束/发布、页面与对象编号、字体子集、图片
与资源引用，以及失败请求已产生的输出。单次 `shipout` 或 flush 不等于已经得到
完整且可独立读取的文档。

因此有两条需分别验证的候选路径：

- 保留 native PDF，增加独立的请求输出生命周期，再评估 SVG 转换；
- 研究 LuaTeX DVI 路线，但先验证目标字体、宏包、图片和转换器支持，不能预设兼容。

第 2.4 节现已验证简单 OpenType 数学配置的 DVI 运行中截取和转换；因此后续
可采用第 8 节已验证的 DVI 逐页 flush 原型。原生 PDF 每请求结束/重置仍未实现。

## 4. 建议的工程路线

### 第一阶段：证明真实热 worker（不承诺恢复）

第 2.3 节已经验证最小 stdin/shipout 循环；以下仍是产品原型所需工作，
不能把三次成功请求视为整个阶段验收完成。

- 固定 LuaHBTeX 版本和 Windows 构建。
- 固定最小 LuaLaTeX profile：Latin Modern + unicode-math，无 CJK、图片和用户 Lua。
- 移植 `READY / WAIT / DONE / ERROR` 请求协议；先返回盒子尺寸作为排版确认。
- 同一 worker 连续处理不同请求；测量冷启动、warmup、稳定期延迟和内存。
- 任意错误先销毁 worker，明确标记为 fallback；不要把它称作原地恢复。

### 第二阶段：提前验证最难的完整状态恢复

建立以下共同基线，而非只复制 eqtb：

| 状态域 | 候选处理 | 必须验证 |
| --- | --- | --- |
| TeX 主表、节点/token 内存、栈、代码表、字体和语言对象 | 引擎内快照与所有权清单 | 动态扩容、指针和引用计数 |
| Lua heap、registry、closures/upvalues、GC/弱表、协程 | 专用分配域/VM 级快照原型，或严格审计的可重建状态 | 地址稳定、GC 元数据、终结器与 C 引用 |
| LuaTeX API、节点 properties、字体加载器、callback 链 | 与前两域同步恢复 | ID/指针交叉引用与惰性加载 |
| HarfBuzz/fontloader/image 等原生资源 | 不可变基线资源保留，请求资源按 epoch 清理或重建 | 内部可变缓存、析构与外部指针 |
| 输出后端、文件与宿主协议 | 独立的请求提交/丢弃机制 | 不恢复宿主序号；不暴露失败输出 |

专用 Lua allocator 是**研究方案，不是完整解法**：扩展库可能绕过该 allocator，
userdata 指向的原生对象、OS handles、文件位置及外部副作用也不在 Lua heap 内。
仅复制 `lua_State` 结构体或 `_G` 不能解决问题。

若这一步不能稳定恢复所有必要状态，先交付受限热 worker + 重建 fallback，
不要对外宣称达到现有 XeTeX 的原地错误恢复保证。

### 第三阶段：输出与真实宏包验证

- 先证明成功请求能发布独立可读的输出，再接现有通用输出 API。
- 验证“错误发生在已输出部分页面/资源之后”仍能丢弃该请求。
- 纳入 `mathtools`、列表、动态字号和字体切换，随后再加图片及 CJK。
- `xeCJK` 明确要求 XeTeX；LuaLaTeX CJK profile 必须另做适配，不能复用其 preamble。

### 第四阶段：验收矩阵

至少覆盖：

1. 普通 TeX 错误、未闭合数学/分组/列表、文件缺失；每次错误后立刻请求正确片段。
2. Lua 错误前修改 table/upvalue、callback、字体、node 和 property。
3. GC 与节点重用，懒加载字体/字号，基线字体参数修改。
4. 多次成功请求中的全局 TeX/Lua 污染；明确是允许持久化还是必须回滚。
5. 已开始输出后失败、取消、超时、worker 崩溃。
6. 多 profile 隔离；长时间好坏请求交替，检查 RSS、handles 和输出稳定性。
7. 验证 recovery 前后 worker 身份不变，不能偷偷重启后宣称原地恢复。

**成功门槛**：可恢复类别的错误返回后，同一 worker 明确到达 `WAIT`，下一条请求
与干净 worker 对照一致；TeX/Lua 交叉引用有效，输出和资源没有持续增长或污染。
数值性能目标应在第一阶段量到基线后设定，本研究不编造延迟或工期。

## 5. 其他路线与边界

- POSIX 的预热父进程 + `fork` 子进程可利用 COW 隔离内存，但不是同一 worker
  原地回滚；文件描述符、线程和外部副作用仍需处理。Windows 主目标不能直接照搬。
- 多个预热 worker 可以维持服务可用性，但消耗更多内存，后台重建仍有预热成本。
- 任意 Lua/native 扩展、文件写入等外部副作用不能仅通过内存快照撤销；本方案不是
  运行不可信 TeX/Lua 的安全沙箱。

## 6. 重现实验

在仓库根目录运行；仅 `build/luatex-research` 是输出目录：

```powershell
New-Item -ItemType Directory -Force build/luatex-research | Out-Null
& C:/texlive/2026/bin/windows/luatex.exe --interaction=nonstopmode --halt-on-error --no-shell-escape --output-directory=build/luatex-research --jobname=state-luatex experiments/luatex-daemon/state-boundary.tex
& C:/texlive/2026/bin/windows/luahbtex.exe --interaction=nonstopmode --halt-on-error --no-shell-escape --output-directory=build/luatex-research --jobname=state-luahbtex experiments/luatex-daemon/state-boundary.tex
& C:/texlive/2026/bin/windows/lualatex.exe --interaction=nonstopmode --halt-on-error --no-shell-escape --output-directory=build/luatex-research --jobname=warm-lualatex experiments/luatex-daemon/warm-typesetting.tex
```

预期两个 `STEMTEX_PROBE_COMPLETE: 9 checks`，以及一个
`STEMTEX_WARM_PASS: 20 repeated layouts`。没有输出页面的警告是预期行为。
LuaLaTeX 需要可写的正常 luaotfload 缓存；`--no-shell-escape` 不是完整 Lua 安全隔离。

手册原文位置：`C:/texlive/2026/texmf-dist/doc/luatex/base/` 下的
`luatex-tex.tex`、`luatex-nodes.tex`、`luatex-callbacks.tex`、`luatex-enhancements.tex`。
上述观察使用这些 TeX 源文件，无需依赖手册页面编号。

## 7. 从当前 XeTeX 实现到 LuaHBTeX 的具体对应

| 当前代码 / 机制 | LuaHBTeX 上的处理 |
| --- | --- |
| `cpp-daemon/stemtex_renderer.cpp` 的 `kDefaultWorkerTemplate` | 保留固定 preamble、warmup、stdin 和明确完成标记的架构；替换引擎专用命令与 profile，最小循环已实测。 |
| `xetex0.c` 的 `stemtex_save_checkpoint()` / `stemtex_restore_checkpoint()` | 沿用“格式状态清单 + 运行时 continuation”的审计方法；重新实现 LuaTeX 数据结构，加入 Lua VM 和跨域引用，不能复制现有结构体。 |
| `xetexini.c` 在 `maincontrol()` 外的 `setjmp` | LuaTeX 有嵌套 `local_control()` 以及 Lua protected calls；必须先设计安全退栈边界，不能直接照搬跳转位置。 |
| `--flush-output-on-shipout` 与 `finalize_xdv_body()` | XeTeX 输出 XDV 后由宿主补 postamble；Lua 原生 PDF 的字体资源、页面树、对象表和 xref 需要自己的完成机制。 |
| 热 `xdvipdfmx` / `dvisvgm` | 进程或库常驻的思路可保留；输入格式和资源契约要重新验证，不能把 LuaTeX DVI 当成 XDV。 |
| `--no-font-cache-refresh` | 这是 XeTeX/Fontconfig 的优化；Lua 路线应分别管理 luaotfload 字体数据库、磁盘缓存和进程内字体对象。 |
| profile creator 的 `xeCJK` 配方 | 需要 LuaLaTeX 专用配方，例如经过验证的 LuaTeX-ja/CTEX 组合；字体选择 UI 可复用，生成的 TeX 配置不能原样照用。 |
| renderer C ABI、worker supervisor、超时/取消机制 | 可作为外层架构复用；输出实现和恢复能力必须如实区分，不能把进程重建报告为 checkpoint 恢复。 |

上游 `pdf/pdfgen.c` 的 `pdf_end_page()` 在页面流结束时触发
`finish_pdfpage`，后续还要写页面对象；文件结束路径另行完成字体输出、页面树、
Catalog 和交叉引用。这解释了第 2.3 节的观察。`finish_pdffile` 是文件结束时的
扩展钩子，不是“立即结束当前 PDF 并开始下一份”的公开 API。

**建议的决策顺序**：先保留现有 XeTeX 后端，基于第 2.4 节已经通过的 DVI
运行中截取实验，使用第 8 节已验证的逐页 flush，使小片段也能立即发布当前请求结果；
同时限定并验证错误恢复的状态域。
完整输出与坏请求恢复均通过后，再接现有 API/GUI。仅把进程改为常驻的技术风险较低，
达到现有“PDF/SVG 即时输出 + 普通错误原进程恢复”的服务能力则是新的引擎后端工程。

## 8. 逐页 DVI flush 实现

补丁：`patches/luatex-dvi-flush-on-shipout.patch`。目标源码为本地 TeX Live
source 中的 LuaTeX 1.26.2，development revision 7748；与前面原版探针的
1.24.0 区分。补丁只涉及 `dvi/dvigen.c`、`dvi/dvigen.h` 和 `lua/luainit.c`。

新增开关：

```text
--output-format=dvi --flush-output-on-shipout
```

开关默认关闭。开启后，在 `dvi_end_page()` 写入 `eop` 后：

1. 按时间顺序写出双缓冲中尚未落盘的字节，包括环绕时较早的第二半缓冲。
2. 保留累计绝对偏移，重置当前写入指针和缓冲区边界。
3. 对输出文件执行 `fflush` 并检查写入错误，使宿主可以立即读到完整当前页。
4. 最终 postamble 的填充按绝对文件位置计算，保持四字节对齐。

文件继续保持打开；补丁不会为原文件逐页追加 postamble。宿主读取到完成标记后，
复制当前完整 DVI，给副本补尾，再选取最新页转换。`fflush` 保证读者可见性，
不提供断电持久化保证。原生 PDF 后端不执行此 DVI 刷新逻辑。

### Windows 构建和验证入口

构建脚本在 `build/luahbtex-mingw/source` 中复制并打补丁，不修改传入的源码树。
工具链为 Git for Windows Bash、Qt MinGW GCC 13.1 和本仓库已有的 GNU make；
依赖库从 TeX Live 源码构建，不向系统 TeX Live 安装实验引擎。

```powershell
.\scripts\build-luahbtex-mingw.ps1 -SourceRoot ..\texlive-source
.\scripts\test-luatex-dvi-flush.ps1 -TexLiveRoot C:\texlive\2026
```

已构建产物位于 `build/luahbtex-mingw/bin/luahbtex.exe`，同目录的
`build-manifest.json` 记录版本、补丁/源码/二进制 SHA256 和所需运行库。
测试脚本为这个引擎单独生成 `luatex.fmt` / `lualatex.fmt`，显式传入格式路径，
将字体缓存和格式写到测试输出目录。不能直接依赖系统 1.24.0 的格式文件。

`live-dvi-probe.py --flush-output-on-shipout` 要求每次请求之后：完整页面数等于
已发送请求数、文件恰好结束于最后一个 `eop`、宿主能在同一进程仍然等待时转换
最新页。结束后再验证 BOP 后向链、post/post_post 指针、页数、填充和 SVG 几何。
默认 Windows 测试包含 plain/Unicode 的小页、混合大小页和四种对齐余数，共六组。

同一入口最后运行 `test-luatex-output-modes.py`，比较开关开启/关闭的行为：
关闭时小页仍缓冲；开启时立即可读；最终 DVI 字节一致；原生 PDF 的最终字节也一致。

底层缓冲测试可单独运行：

```powershell
python experiments/luatex-daemon/test-dvi-flush-buffer.py --mutation-checks
```

它提取实际补丁中的 C 函数编译测试，覆盖五种缓冲大小、520 次逐页刷新、
环绕/边界/四种余数、默认关闭以及注入的写入/刷新/偏移错误；故意删掉第二半
缓冲写入、偏移更新、`fflush` 或使用旧填充计算时，测试都能识别失败。

### 已完成的真实引擎验证

LuaHBTeX 1.26.2 / revision 7748 的 Windows 原生构建运行成功。可执行文件约
11.2 MB，旁边配套 `libwinpthread-1.dll`。使用本机 TeX Live 2026 的包树生成
专用格式后，六组、36 个请求全部通过：

| 测试 | 请求数 | 结果 |
| --- | ---: | --- |
| plain-small | 3 | 每次立即得到完整当前页，首请求 247 B |
| unicode-small | 3 | 每次立即得到完整当前页，首请求 1144 B |
| plain-boundary / unicode-boundary | 7 + 7 | 大小页交替、跨缓冲续写、最新页转换通过 |
| plain-alignment / unicode-alignment | 8 + 8 | 四种页尾偏移余数、完整 DVI 对齐与指针检查通过 |

每组请求全过程使用同一 PID。每次实时转换的最新页 SVG 与正常结束后的对应页
几何一致，完整页面的 DVI 字节前缀也一致。抽查实时 Unicode PDF 的文字、
分数、求和和上下标显示正常。

`test-luatex-output-modes.py` 的四种模式也通过：DVI 关闭开关时小页仍为 0 B，
开启时立即出现完整页，最终两个 DVI 字节相同；原生 PDF 开关前后都是有效单页，
固定元数据后的最终 PDF 字节相同。说明开关是显式启用，并未改变原生 PDF 输出。

本轮记录：`build/luatex-flush-tests/results.json`、
`build/luatex-flush-tests/output-modes/results.json`；每次运行会保留独立的
`run-*` 目录，记录命令、PID、每页字节数、转换结果和最终结构检查。

### 范围

这是实验引擎和可复现宿主探针；GUI 实验入口见下节，尚未接入 StemTeX C ABI。没有实现
TeX/Lua checkpoint；本次成功不改变前面关于错误恢复的结论。长时间服务仍应
轮换 worker，避免累计 DVI 接近引擎有符号 32 位偏移限制；补丁的页尾检查不是
对任意超大流的全面溢出保护。CJK、图片和复杂 specials 仍需另验；GUI 的热转换器接入见下节。

## 9. GUI 实验入口

GUI 工具栏增加 `LuaHBTeX (experimental)`，也可用
`scripts/start-luatex-gui.ps1` 直接启动。构建、staging 和 smoke 命令统一放在
[构建文档](BUILDING_AND_PACKAGING.md#experimental-luahbtex-gui)。

这条路径使用原生 Qt/C++ 宿主，无 Python 运行时依赖。每次成功排版保留同一
LuaHBTeX PID，从累计 DVI 解析完整页并为副本补尾，再用 StemTeX runtime 中的
`dvipdfmxdaemon.dll` / `dvisvgmdaemon.dll` 转换最新页。两个转换器与 XeTeX 路径
使用相同的热启动 ABI，在后端初始化时加载，连续请求复用，不再逐次启动转换进程。
完整 TeX Live 提供 LuaLaTeX 包和字体；格式与字体缓存单独保存，不修改系统 TeX Live。
GUI 的编辑、宽度、字号、PDF/SVG 预览和图像复制/保存复用已有界面。

实验固定使用内置 Latin Modern / `unicode-math` profile 和 UTF-8；自定义 Profile、
`xeCJK` 配方及公共 C ABI 暂未扩展。TeX 错误通过重建 worker 恢复，
因此错误后的 PID 会变化，不属于 checkpoint 恢复。

GUI 的耗时摘要与 XeTeX 使用同一格式，分别显示排版、转换和输出完成总时间。
Lua 指标单独记录 DVI 读取/补尾/写入的时间，排版计时不包含这段工作；原始 JSON
沿用 `requestToOutputMs`、`convertMs`、`outputBytes` 等字段，并保留 worker 信息。
