# Qualcomm KGSL 图形驱动：内核信任的 GPU 同步状态内存可被普通应用读取与写入

# Qualcomm KGSL driver: kernel-trusted GPU synchronization state can be read and written by any unprivileged app

> **English abstract — full report below is in Chinese.**
>
> Any ordinary third-party app — no permissions, no `adb`, no user interaction — can
> read and write the GPU's `memstore`, the kernel-trusted block holding per-context
> retire timestamps, using only the standard KGSL submission API. Because the kernel
> reads that block back without cross-checking anything, a value the app chose is
> accepted as the authoritative "this work has retired", and a `TIMESTAMP_EVENT`
> fence is signalled for a timestamp that was never queued.
>
> **This report does not claim root.** The write primitive is confined to the 32 KiB
> memstore, 4 bytes per write, and cross-process fence forging is verified only
> against the caller's own context.
>
> Scope is a platform property, not a single code site. The condition holds across 18
> Adreno cores, including `a630v2`, `a640` and `a680` — it is not limited to older
> silicon. Four independent conditions compose the bug; the report lists them, and
> [`patches/`](patches/README.md) contains proposed fixes ordered by how much each can
> be trusted. One originally-suggested mitigation is explicitly retracted there,
> because making the memstore unconditionally privileged may stop the GPU writing its
> own timestamps on parts without `ADRENO_APRIV`.
>
> Verification program: [`poc/probe_fence.c`](poc/probe_fence.c).

---

> 公开研究披露（公开版，不含"修复请求/时间线"等私有披露要素）。
> 本文是完整报告；可运行的验证程序见 [`poc/probe_fence.c`](poc/probe_fence.c)，编译与运行说明见 [`poc/README.md`](poc/README.md)。
>
> 一句话结论：**任意普通应用、无需任何权限、无需用户交互**，即可通过标准 GPU 接口读取并写入内核完全信任的 GPU 同步状态内存（memstore），并让内核把伪造值当作"工作已完成"的权威依据。**本报告不主张可达成 root。**

| 项目 | 内容 |
|---|---|
| 组件 | Linux 内核 Qualcomm KGSL 驱动（`drivers/gpu/kgsl`）及平台设备树配置 |
| 严重级别 | **高危（High）** |
| 影响 | 跨进程信息泄露 / 内核内存写入 / 同步原语可被伪造 |
| 所需权限 | **无**（普通第三方应用，uid ≥ 10000，无需任何声明权限） |
| 用户交互 | 无 |
| 提权 | 本报告不主张可达成 root |
| 验证状态 | 关键路径已实机验证 |
| 设备 | Xiaomi sunstone（Redmi Note 12 系列）/ 高通 Blair SoC / Adreno 619v2 |
| 内核 | `5.4.289-qgki-g3dd36cfe40b3` |
| 报告日期 | 2026-09-27 |

---

## 1. 概要

在本设备上，**任意普通应用无需任何权限，即可通过正常的 GPU 提交接口读取并写入内核完全信任的 GPU 同步状态内存（memstore）**。

memstore 是内核与 GPU 之间的共享状态区，保存各 GPU 上下文的完成时间戳与状态标志。内核读取它时**不做任何交叉校验**——它的值就是判定"GPU 工作是否已完成"的唯一依据。

由此获得三项能力：

1. **跨进程信息泄露**：读走全表，实时观察整机所有 GPU 上下文的提交与完成情况；
2. **内核内存写入**：4 字节、偏移与内容均可控，写入位置为内核信任的数据结构；
3. **同步原语伪造**：使其他进程的 GPU 同步对象在实际完成前提前完成。

第 1、2 项已实机验证（写入后回读确认）。第 3 项针对**调用方自身上下文**的同步对象已实测确认：伪造的字段值被内核当作权威的"已退休"状态，并据此完成同步对象的 signal，且该结果通过对照臂区分（未伪造时内核正确拒绝），连续多次复现一致（见 §5.3）。跨进程扩展属同一机制，未单独复验。

---

## 2. 影响与严重程度

**定级为高危。**

理由不是"已达成提权"，而是**零前置条件的内核写入原语**：攻击者不需要 root、不需要提权能力、不需要用户交互，只需调用标准、合法的图形驱动接口。此外该问题取决于平台配置而非某处编码失误，因此影响面按 GPU 世代展开（见 §4）。

| 能力 | 状态 | 影响 |
|---|---|---|
| 读取内核 memstore 全表，含所有 GPU 上下文时间戳与全局环缓冲进度 | ✅ 已验证 | 任何普通应用可实时观察整机 GPU 活动，无需目标进程配合 |
| 向 memstore 任意偏移写入任意 32 位值 | ✅ 已验证（回读确认） | 内核写入原语，位置在内核信任数据结构内 |
| 使调用方自身的同步对象在目标时间戳**从未被执行**的情况下被判定为已完成 | ✅ 已验证（含对照臂） | 内核"已退休"判定完全由该字段驱动，伪造即被采纳；连续多次复现 |
| 使其他进程的同步对象提前完成 | 🔍 静态确认（同一机制，未跨进程复验） | 跨进程生效 |
| 图形内存释放时机的判定可被调用方控制 | 🔍 静态确认 | 内核"是否已退休"的判定完全基于该字段，释放决策可被劫持，且**无需触发任何硬件故障** |
| 至本地 root 的完整利用链 | ❌ 未构造 | —— |

**明确排除的主张**：不主张可达成 root；不主张可读写其他应用的用户空间图形内存（进程间页表隔离仍然有效，只有内核全局内存是共享的）；不主张存在系统崩溃风险（验证全程设备无异常）。

---

## 3. 漏洞原因

本报告对象的问题由四个条件叠加构成：**前三个是必要条件**（缺一不可），**第四个把"可写"升级为"可被内核消费"**：

**① 全局内存被映射进每个应用的页表。** GPU 的内核态内存本应只在内核侧可寻址，隔离依赖 SMMU 的 split tables，其开关是设备树属性——本设备**缺失**：

```dts
arm,smmu-kgsl@5940000 {
    compatible = "qcom,smmu-v2";
    qcom,use-3-lvl-tables;
    /* qcom,split-tables;   ← 缺失 */
};
```

未启用时，驱动将全局内存映射进**每一个**进程页表：

```c
/* kgsl_iommu.c :: kgsl_iommu_map_globals() */
if (!kgsl_iommu_is_global_pagetable(pt) && kgsl_iommu_split_tables_enabled(mmu))
    return;                                    /* 未启用 ⇒ 不返回 */
list_for_each_entry(md, &device->globals, node)
    kgsl_mmu_map(pagetable, &md->memdesc);     /* memstore 进入每个应用页表 */
```

**② memstore 的特权标记绑定在硬件特性上。** 页表项的特权位由 `PRIVILEGED` 标记决定，而该标记取决于 GPU 特性 `ADRENO_APRIV`：

```c
/* adreno.c */
if (ADRENO_FEATURE(adreno_dev, ADRENO_APRIV))
    priv |= KGSL_MEMDESC_PRIVILEGED;           /* 仅带 APRIV 的 GPU 才有保护 */
device->memstore = kgsl_allocate_global(device,
        KGSL_MEMSTORE_SIZE, 0, 0, priv, "memstore");
```

**Adreno 619 不具备 `ADRENO_APRIV`**，memstore 的页表项为普通 `READ|WRITE`，GPU 非特权访问即可写入。

**③ 内核不校验提交内容，也不校验时间戳来源。**

```c
/* adreno_dispatch.c :: _verify_ib() */
if (ib->size == 0 || (ib->size >> 2) > 0xFFFFF) return false;
if (!kgsl_mmu_gpuaddr_in_range(pagetable, ib->gpuaddr, ib->size)) return false;
return true;      /* 只校验指令缓冲自身的地址与大小，指令内容不做检查 */

/* adreno.c :: __adreno_readtimestamp() */
case KGSL_TIMESTAMP_RETIRED:
    kgsl_sharedmem_readl(device->memstore, timestamp,
        KGSL_MEMSTORE_OFFSET(index, eoptimestamp));   /* 直接信任，无交叉校验 */
```

指令内部的目标地址不经过范围校验；时间戳读取直接信任 memstore 中的值。

**④ 事件机制的时间戳上界校验是可选的，开关在调用方。**

事件注册时会校验"目标时间戳不得超过已排队的时间戳"，但该校验被一个由调用方自行声明的上下文标志关闭：

```c
/* kgsl_events.c :: kgsl_add_event() */
/* If the caller is creating their own timestamps, let them schedule
 * events in the future. Otherwise only allow timestamps that have been
 * queued. */
if (!context || !(context->flags & KGSL_CONTEXT_USER_GENERATED_TS)) {
    group->readtimestamp(device, group->priv, KGSL_TIMESTAMP_QUEUED, &queued);
    if (timestamp_cmp(timestamp, queued) > 0)
        return -EINVAL;
}
```

该标志 `KGSL_CONTEXT_USER_GENERATED_TS (0x80)` 在上下文创建接口的标志白名单中，普通应用可直接传入；配套语义是每批提交自行指定时间戳（`adreno_dispatch.c :: get_timestamp()`）。

**这使得第 ③ 项从"可写"升级为"可被消费"**：事件注册函数在注册当次调用内就会读取该字段并据此判定事件已完成：

```c
group->readtimestamp(device, group->priv, KGSL_TIMESTAMP_RETIRED, &retired);  /* 读取上表字段 */
if (timestamp_cmp(retired, timestamp) >= 0) {
    event->result = KGSL_EVENT_RETIRED;
    queue_work(device->events_wq, &event->work);
    return 0;                     /* 注册当次即判定"已完成" */
}
```

⇒ 写入后的消费**不需要中断、不需要调度器、不需要目标进程配合**，在调用方自己发起的一次接口返回点内即被采纳。

**④ 项不是编码错误，而是接口契约本身**：注释明确把"允许调度到未来时间戳"授权给"自己产生时间戳的调用方"，而后者只是一个无服务端验证的自声明位。

---

## 4. 受影响范围

缺陷成立需要上述条件同时满足，两者独立，因此影响面按两个维度展开。

### 4.1 GPU 世代

以下核心**不具备** `ADRENO_APRIV`，memstore 无特权保护（以下按上游 `gfx-kernel.lnx.1.0.r50-rel` 的代码代号列，共 18 个，含同名变体）：

```
a505   a506   a508   a512   a530v2 a530v3 a540v2            ← a5xx 全系列
a610   a611   a612   a615   a616   a618   a619   a619_variant
a630v2 a640   a680   gen6_3_26_0                             ← 本设备 = a619
```

具备该特性的为 a620 / a621 / a635 / a650 / a660 / a662 / a663 / a702 及 gen7、gen8 全系列。
该判断在 `msm-5.4.289`（本设备）与 `gfx-kernel.lnx.1.0.r50-rel` 两树中**无任何差异**。
**注意并非"越新越安全"**：a630 / a640 / a680 同样处于无保护一侧。

### 4.2 平台配置

`qcom,split-tables` 是每机型独立的设备树属性。本设备实测 3 个 board 设备树、29 个 overlay、`boot.img`、`dtbo.img`，全部缺失。且该属性依赖 SMMU 硬件支持，**部分 SoC 不满足**，此时厂商无法通过该属性启用隔离。

⇒ 该问题无法通过一次 OTA 覆盖修复，需按 **SoC × 机型** 逐一复核。

### 4.3 结构性问题：同一类安全不变量被降级为"配置项"或"特性位"

本报告涉及的问题不是单点编码失误，而是同一类设计取向的重复出现。四个安全不变量都被实现为开关，且开关缺失时**不报错、不降级、不记日志**，系统照常工作：

| 不变量 | 被降级成的开关 | 位置 | 缺失时的行为 |
|---|---|---|---|
| 内核全局内存不应进入用户页表 | 设备树布尔位 `qcom,split-tables` | `kgsl_iommu_map_globals()` | 静默映射进所有进程页表 |
| GPU 非特权访问不应写可信状态 | 硬件特性位 `ADRENO_APRIV` | memstore 分配处 | 静默取消特权位（本报告对象即此类） |
| IOMMU 绑定失败应拒绝启动 | `mmu.type = NOMMU` | `kgsl_mmu.c` 绑定路径 | 静默回落为非 IOMMU 模式，无日志；分配路径随之切换到物理地址直通 |
| 事件目标不应超前于已排队时间戳 | 上下文标志 `KGSL_CONTEXT_USER_GENERATED_TS` | `kgsl_events.c` 事件注册 | 静默取消上界校验（调用方可自声明） |

**前三个是厂商配置或硬件能力问题，第四个是接口契约问题。** 后者的影响面不依赖设备树，凡是运行该驱动版本的产品都具备该能力面。

其中第 3 项（IOMMU 绑定失败静默回落）在本次核查中确认**与交付源码完全一致、未作修改**，且在当前设备上**不可达**（IOMMU 已正常绑定）。它不是本报告的攻击路径，但建议一并处理：一个"失败即降级为最小权限"的初始化被实现成了"失败即降级为无隔离"，属于配置项语义与安全性语义不一致的典型形态。

第 4 项与前三项不同——它不需要特定 SoC 或特定设备树组合，任何使用该驱动版本的产品都受影响，因此**建议按驱动版本而非按机型评估**。

### 4.4 第五类问题：布局边界常量在版本演进中被改动而引入新的越界

与上面四例的"开关缺失"不同，这一类是**改动了本该受保护的边界常量**。

memstore 是一块 32 KiB 的共享内存，内部按 40 字节分槽：槽 0 为全局槽，中间一段分配给各上下文，末尾若干槽留给 GPU 的环形缓冲（ringbuffer）。槽位上限由
`KGSL_MEMSTORE_MAX = KGSL_MEMSTORE_SIZE / sizeof(struct kgsl_devmemstore) − 1 − KGSL_PRIORITY_MAX_RB_LEVELS`
决定，而上下文的 id 上界与该常量直接绑定。

| | 本设备所用驱动版本 | 较新的驱动版本 |
|---|---|---|
| 减项 | `−1` | `−2`（为低权限环形缓冲预留） |
| 上下文 id 上界 | `KGSL_MEMSTORE_MAX`（含端点） | `KGSL_MEMSTORE_MAX − 1` |
| 上下文槽区间 | 1..814 | 1..812 |
| 环形缓冲槽区间 | **814..817** | **813..817** |
| 是否重叠 | **是：槽 814 被同一上下文 id 与环形缓冲 level 0 同时占用** | 否 |

较新的版本里有两个注释写明了这个减项的用途（`Subtract one for LPAC` 与 `Last context id is reserved for global context`），即"预留一格间隙"是**有意为之**的。本设备版本在移除低权限环形缓冲相关定义的同时，把减项改回 `−1`，但**没有同步收紧上下文 id 上界**，两处的净效果是从"留一格"变成"差一格"。

**后果**：当某个上下文的 id 恰好等于槽上限时，该上下文的内存槽与环形缓冲 level 0 的槽是同一块 40 字节内存。两个消费者读的是同一批字段：

- 查询该上下文时间戳的内核路径，取到的是环形缓冲的值（反之亦然）；
- 该上下文的事件组以环形缓冲的时间戳判定"已退休"，事件可能被提前触发或永不触发，且驱动不会自愈、不报错。

**可达性边界（如实声明）**：单进程上下文数上限为 200，无法单独到达槽 814；触发需要约 814 个并发上下文，即至少 5 个进程协同。**这不满足单一应用独立触发的条件**，危害定性为跨对象状态串扰与事件闸门可被间接控制，**不构成任意内核内存写入，也不构成提权路径**。

**建议**：此类问题的修复成本极低（收紧一处上界），但**发现成本很高** —— 它不表现为报错、不表现为功能异常，只在特定槽位组合下才显现。建议对驱动中的布局边界常量做逐版本比对，把"减项数字"列为需要复核的项，而不是只比对函数逻辑。

---

## 5. 验证过程

| 项目 | 值 |
|---|---|
| 验证身份 | 普通第三方应用，**uid 10240**，无任何声明权限 |
| 使用接口 | 仅标准 kgsl ioctl（分配、创建上下文、提交命令、读时间戳）与 `mmap` |
| 是否提权 | 否 |
| 是否使用非常规接口 | 否 |

```
[1] open("/dev/kgsl-3d0", O_RDWR)                    → fd = 104
[2] mmap(0xfff00000, 32768, PROT_READ, MAP_SHARED)   → 0（成功）
[4] 扫描全表，活跃上下文槽位                          → 命中 22 个
[5] 1.5 秒后重采样
    Δ 槽位 815  时间戳 2755043 → 2755054
    Δ 槽位 816  时间戳 20215112 → 20215118
[6] 对照 mmap 加 PROT_WRITE                          → -1（EPERM）
[7] 对照 mmap 长度减半                               → -22（EINVAL）
[8] 对照 mmap offset=0                               → -22（EINVAL）
```

以上为**信息泄露**能力。槽位 815/816 为全局共享环缓冲槽位，其时间戳随其他进程活动实时推进。

```
[B1] 向自身 buffer 提交写指令（值 0x600df00d）        → 成功，回读一致
[B3] 向 memstore 偏移 28008（初始 0x0）提交同一值     → 成功
[B3] 回读 memstore[28008]                            → 0x600df00d
[B3] 全表搜索该特征值                                 → 命中偏移 28008
```

以上为**内核写入**能力。**写入来源的唯一性**：由 `[6]` 可知 CPU 侧写被驱动拒绝（EPERM），故上述变更只能由 GPU 执行。

**稳定性**：设备连续运行 2 天 22 小时未重启；系统日志测试前后完全一致；GPU 相关关键字 0 命中；无 GPU 故障或系统崩溃。写入位置刻意选择无消费者的闲置槽位。

### 5.3 同步原语伪造的实测确认（含对照臂）

写入原语的能力边界已通过一次**带对照臂**的实验进一步确认：被改写的字段值会被内核采纳为权威的退休状态，并驱动内核的同步判定。

**实验设计**（三个上下文分工，隔离"改写者"与"被改写者"两个角色）：

- 上下文 A：仅提交一个 NOP 指令流，退休到时间戳 T，此后不再提交任何指令流
- 上下文 B：仅提交一条 `CP_MEM_WRITE`，把上下文 A 所在槽位的结束时间戳改写为 T+4096
- 上下文 C：仅提交一个 NOP 指令流，退休到 T'，**不**进行任何改写

随后对 A 和 C 分别请求"以 T+4096 / T'+4096 为退休条件"的同步栅栏，并等待其完成信号：

| 组 | 结束时间戳是否被改写 | 内核响应 |
|---|---|---|
| 实验组（上下文 A） | 是 | 栅栏被 signal，等待立即返回 |
| 对照组（上下文 C） | 否 | 等待超时 |

**判定依据**：目标时间戳 T+4096 从未被任何指令流排队执行，GPU 的退休计数从未推进到该值。内核仍采纳该值并完成 signal —— 说明退休判定路径**只信任被改写的共享字段，不校验该值是否真实执行过**。

**可复现性**：完整实验连续 4 次结果一致（栅栏均被 signal）；对照组多次重复均为超时。设备运行时间在整个实验期间连续增长，无重启、无 GPU 故障。

**需要说明的设计约束**：改写指令本身必须经由某个上下文提交，而该上下文的提交动作会使 GPU 更新该上下文自己的结束时间戳，从而覆盖刚写入的值。因此实验必须把改写者与被改写者分置于两个不同上下文，且被改写者在整个实验期间不得再提交任何指令流。这解释了为何此类现象在更早的验证尝试中未能观察到。

---

## 6. 修复建议

本报告附四个候选修复于 [`patches/`](patches/README.md)，**均未编译、未运行**。按可验证程度排序，与 §3 的条件一一对应：

| 顺序 | 层级 | 建议 | 状态 |
|---|---|---|---|
| **1** | 平台配置 | 启用 `qcom,split-tables`，使内核全局内存不进入任何用户上下文页表 | 与树内其他平台一致，是驱动本就假设的设计 |
| **2** | 驱动 | 事件注册的时间戳上界校验**不应由调用方自声明的标志关闭** | 属 API 决策，本报告不给补丁（见 `patches/README.md`） |
| **3** | 驱动 | memstore 的槽区间加**互不重叠**的编译期断言，并回退被改动过的 `- 1` 常量 | 两段算术改动，不改变现有可用配置的运行时行为 |
| **4** | 驱动 | 全局内存映射函数显式跳过 memstore | 仅在 split-tables 已开启时安全（见下） |
| **5** | 驱动 | memstore **无条件**设置 `KGSL_MEMDESC_PRIVILEGED` | **未经硬件验证，不建议直接采用** |

**排序理由（重要）**：

- **1 是最该先做的。** 它是树内其他平台的既有设计，`kgsl_iommu_map_globals()` 在 split-tables 开启时本就对非全局页表提前返回。一个平台级开关的改动，风险低于任何驱动改动。
- **4 单独使用会断功能。** `map_globals()` 有两个调用点：`iommu_pt_create()` 在进程页表上调用（并置 `iommu->ppt_active`），`iommu_probe()` 在 `defaultpagetable` 与 `lpac_pagetable` 上调用。split-tables **关闭**时进程页表就是 GPU 看到的地址空间（PPT 模式），跳过 memstore 会把 GPU 自己对时间戳存储的写权限一起切掉。所以 4 只在 1 已实施时成立。
- **5 是本报告里唯一未经硬件验证的建议。** `KGSL_MEMDESC_PRIVILEGED` 决定 memstore 页表项是否带特权访问位；在不含 `ADRENO_APRIV` 的芯片上，GPU 的用户上下文访问并非特权访问，把 memstore 置为特权可能直接中断 GPU 自身的退休时间戳写入。这一点无法在静态分析下判定，因此列为候选而非推荐。
- **3 是最值得无条件采用的**：两段宏算术加两个 `BUILD_BUG_ON`，把 4.4 那类 off-by-one 变成编译期错误。
- **2 没有补丁**：`kgsl_events.c:247-254` 的注释表明该行为是显式设计意图（允许为尚未排队的将来时间戳注册事件），这是 EGL/Vulkan 依赖的接口契约。无条件关闭会使现有客户端全部失败；改为要求 signature 权限则会静默打断它们。这个取舍属于驱动维护者。

---

## 7. 说明与边界

1. **不主张可达成提权**。已确立的能力为内核写入原语、跨进程信息泄露、同步原语伪造机制。若需延伸至本地提权，还需额外的堆布局控制、地址随机化绕过、策略绕过等独立工程，本报告未涉及。
2. **写入原语作用域受限**：仅能写入 memstore 所在 32 KB 区域，每次 4 字节。不主张由此可达成任意内核读写。
3. **已排除的方向**（供参考）：时间戳读取接口的索引参数经上下文有效性校验，有上界，不构成越界读；只读映射通道由驱动强制 `PROT_READ`，泄露范围即 memstore 内容本身。
4. **同步原语伪造的动态演示已执行**（见 §5.3），范围限于**调用方自身的上下文**：伪造的结束时间戳被内核采纳为退休状态，连续多次复现，并通过对照臂区分（未伪造时内核正确拒绝）。**跨进程扩展未复验** —— 该方向属同一机制，但涉及其他进程的状态，本报告不主张已确认。
5. 第 4 节中"无 `ADRENO_APRIV` 的 GPU 清单"为代码核对结论，核对范围是两份独立驱动树：真机所用的 `msm-5.4.289` 与上游 `gfx-kernel.lnx.1.0.r50-rel`。**两份树在该判断上无任何差异**：a619 的特性位集合在两树中都是 `PREEMPTION | CONTENT_PROTECTION | IFPC | IOCOHERENT`，均不含 `ADRENO_APRIV`。"其他机型实际受影响"需按机型逐一验证，本报告仅持有本设备固件。
6. **可达性边界（供评估参考，避免重复摸底）**。以普通应用身份实测系统接口面：
   - `/dev` 节点共 44 个，第三方应用可成功打开的仅 4 个：`/dev/kgsl-3d0`、`/dev/ion`、`/dev/adsprpc-smd`、`/dev/binder`（含 `/dev/hwbinder`）。其余全部 `EACCES`，包括 `dri/*`、`ashmem`、`qmi` 类节点。
   - GPU 相关的 `sysfs` 是**细粒度白名单**而非整体关闭：`gpu_model`、`max_gpuclk` 可读，但 `gpuclk`、`gpu_busy_percentage`、`devfreq/cur_freq`、以及 `snapshot/dump`、`snapshot/faultcount` 等全部拒绝。
   - 调试通道不可用：内核日志、命令行动参、设备树 `soc/` 子树、`iomem`、`kallsyms` 均不可读。其中设备树根目录可读、子树被拒绝，属策略层面的细粒度拒绝，不是权限缺失，普通高权限 shell 亦无法绕过。

   ⇒ 上述边界说明**本报告的攻击面不需要任何越权访问**：所用接口全部是标准图形接口，且在应用沙箱内可用。评估方无需在提权前提下复核本报告结论。
   ⇒ 反过来，**本报告无法通过公开接口确认设备树属性 `qcom,split-tables` 的实际取值**，第 3 节第 ① 项的"缺失"结论来自固件镜像的文件级核对，而非运行时读取。

---

## 8. 披露信息

| 项目 | 内容 |
|---|---|
| 报告性质 | 公开披露，含可运行的验证程序（`poc/`） |
| 主张范围 | 内核写入原语（作用域 32 KB、每次 4 字节）、跨进程信息泄露、同步原语伪造机制。**不主张提权** |
| 报告日期 | 2026-09-27（首次形成结论） |
| 公开日期 | 2026-10-01 |
| 后续动作 | 本版为公开披露，不占用私有披露窗口；若需协商执行窗口或厂商沟通，请通过 SECURITY.md 中的渠道 |

---

## 附录 · 复现要点

```
前置：任何普通应用，无任何权限
1. open("/dev/kgsl-3d0", O_RDWR)
2. mmap(NULL, 32768, PROT_READ, MAP_SHARED, fd, 0xfff00000)   → 成功
   按 40 字节步长扫描 32 KB，可枚举全部活跃上下文的时间戳
3. 分配图形内存（SVM）+ 创建上下文（flags = 0x12）
4. 提交一条标准命令，指令内容为
     写 0xfc006d68 ← 任意值
   ioctl 返回成功
5. 回读第 2 步映射视图的偏移 28008 → 等于该任意值
```

**固件指纹（用于核对复现对象）**

| 文件 | 大小 | sha256 |
|---|---|---|
| `boot.img` | 134,217,728 | `a81fbd715c660e7ea57e053eca9f4a284f38c699c8aa718b7b6d5ec7f192496a` |
| `dtbo.img` | 8,388,608 | `778fc061f643be455be244e69e53edb7244ef98c6898acc2991570f7312f1589` |
| `vendor_boot.img` | 100,663,296 | `d32e6f7745727b2b8edc763e21bea289c886c5694f42211464ac2929bb5ba546` |

**关键数值**

```
内核全局内存区基址      0xfc000000
memstore 大小          32 KB
只读映射 offset        0xfff00000
写入目标               0xfc006d68（第 700 号上下文的完成时间戳字段）
```

**安全提示**：第 4 步目标地址在页表内有效，不产生 GPU 故障，设备无可见影响。若改写到活跃上下文的时间戳槽位，将产生跨进程的同步时序影响，不建议在共享设备上执行。
