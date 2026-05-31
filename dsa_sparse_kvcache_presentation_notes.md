# DSA 稀疏推理 SSD KVCache 访问问题汇报稿

## 1. NPU 侧开销：控制面进入 Decode 热路径

![NPU侧开销图](/Users/warren/Documents/KVoF-motivation/images/01_npu_control_overhead.png)

### 核心观点

DSA 稀疏推理在 decode 阶段按 token 粒度访问 SSD KVCache，会把存储访问控制面放进 NPU 的 decode 临界路径。请求越碎，NPU 侧地址生成、MTE 发起和 completion 监控开销越高。

### 图示逻辑

```text
Token -> 稀疏选择 -> 地址生成 -> MTE控制 -> SSD KVCache -> Attention
```

### 主要开销来源

| 开销来源 | 说明 |
| --- | --- |
| 地址生成 | 稀疏索引需要映射到 SSD 上的文件 offset、对象 key 或块地址 |
| 元数据查表 | KVCache 分片、页表、块表、跨盘路由都可能引入查表 |
| MTE 发起 | 每个小 I/O 都需要描述符、队列、DMA 或搬运控制 |
| Completion 轮询 | 大量小请求会带来高频完成检查和状态监控 |

### 汇报表述

> 问题不只是 SSD 慢，而是 token 粒度小随机 I/O 把存储控制面搬进了 NPU decode 热路径。小随机 I/O 越多，NPU 控制开销越高。

---

## 2. 100M IOPS：逐请求协议栈不可持续

![100M IOPS协议栈瓶颈图](/Users/warren/Documents/KVoF-motivation/images/02_100m_iops_protocol_bottleneck.png)

### 核心观点

100M IOPS 对应平均 `10ns / I/O` 的发起间隔。这个量级下，不能让每个 I/O 都逐请求经过完整软件协议栈，否则发起路径、链路路径和 completion 路径都会成为瓶颈。

### 图示逻辑

```text
推理调度
  -> 文件/对象/块语义
  -> NVMe/网络协议
  -> UB/RoCE/TCP传输
  -> SSD控制器
  -> 介质
```

### 关键瓶颈

| 瓶颈 | 说明 |
| --- | --- |
| 发起瓶颈 | 请求生成、队列提交、doorbell、描述符维护无法逐请求承受 100M IOPS |
| 链路瓶颈 | 小包效率低；512B 下 100M IOPS 也约等于 51.2GB/s |
| Completion 瓶颈 | 100M IOPS 同时意味着 100M completions/s |
| 控制器瓶颈 | SSD 控制器需要处理队列、FTL、ECC、通道调度和内部元数据访问 |

### 汇报表述

> 100M IOPS 的核心挑战不是单点带宽，而是逐请求协议栈不可持续。系统必须避免每个 token 小块读都走重软件路径。

---

## 3. 现有问题：多协议生态压到 NPU 侧

![第三页预览](/Users/warren/Documents/KVoF-motivation/slide3_problem_preview.svg)

### 核心观点

当前 SSD KVCache 访问可能采用文件、对象或块语义；传输层又可能使用 UB、RoCE、TCP、NVMe 等协议。若这些访问能力都要求进入 NPU decode 热路径，NPU 侧就需要承接多套语义转换和协议适配，导致生态兼容和工程适配复杂。

### 图示逻辑

```text
文件 / 对象 / 块
      ->
NPU适配层：UB / RoCE / TCP / NVMe
      ->
接口割裂 / 驱动绑定 / 调试困难
```

### 现有问题

| 问题 | 说明 |
| --- | --- |
| 接口割裂 | 文件、对象、块的访问语义不同，运行时接口难以统一 |
| 驱动绑定 | NPU、SSD、NIC、协议栈版本容易相互绑定 |
| 调试困难 | I/O 问题跨 NPU runtime、驱动、网络、SSD 控制器，定位链路长 |
| 兼容成本高 | NPU 需要适配多套存储生态和传输生态 |

### 汇报表述

> 现有问题是多语义、多协议进入 NPU 热路径后，NPU 不再只是计算和搬运数据，还要承接存储生态适配。协议越多，生态边界越复杂，兼容成本越高。

---

## 一页总结

| 方向 | 核心问题 | 汇报关键词 |
| --- | --- | --- |
| NPU 侧开销 | 控制面进入 decode 热路径 | 地址生成、MTE、Completion |
| 100M IOPS | 逐请求协议栈不可持续 | 10ns/I/O、发起瓶颈、链路瓶颈 |
| 生态兼容 | 多协议复杂度上移到 NPU | 文件/对象/块、UB/RoCE/TCP/NVMe、驱动适配 |

## 交付文件

| 文件 | 用途 |
| --- | --- |
| [dsa_sparse_kvcache_editable.pptx](/Users/warren/Documents/KVoF-motivation/dsa_sparse_kvcache_editable.pptx) | 三页可编辑 PPT |
| [slide3_problem_preview.svg](/Users/warren/Documents/KVoF-motivation/slide3_problem_preview.svg) | 第三页预览图 |
| [dsa_sparse_inference_kvcache_analysis.md](/Users/warren/Documents/KVoF-motivation/dsa_sparse_inference_kvcache_analysis.md) | 完整分析草稿 |
