# KVoF 层次化缓存系统 — 详细设计说明书 (LDD)

> **文档版本：** V2.0  
> **密级：** 内部公开  
> **拟制日期：** 2026-04-20

| 项目 | 内容 |
|------|------|
| **产品/项目名称** | KVoF 层次化缓存系统 |
| **文档类型** | 详细设计说明书 (LDD) |
| **文档版本** | V2.0 |
| **拟制日期** | 2026-04-20 |

---

## 1  摘要

KVoF（KV over Fabric）是一套面向大语言模型推理场景的层次化 KVCache 管理系统。系统将缓存划分为 L1（本地 HBM）、L2（超节点内 DRAM/SSD）、L3（超节点间）、L4（块/文件/对象存储）四个层次，通过 11 个功能模块实现元数据管理、数据传输与连接控制。

**关键设计决策：**(1) 对接 vLLM 框架，提供 block 和 token 双粒度读写接口；(2) Client 在 XPU 侧部署轻量化客户端，批量通知 CPU 侧完整客户端；(3) Server 独立部署，数据传输采用 pull 模式，冷数据加载采用 push 模式直推 HBM；(4) 支持硬件加速（请求解析硬化、元数据热缓存、ZNS 映射简化）；(5) 通过语义转换层兼容块/文件/对象存储。

---

## 2  引言

### 2.1  编写目的

本文档为 KVoF 层次化缓存系统的详细设计说明书，面向开发人员、测试人员和架构评审人员。

### 2.2  适用范围

适用于基于 Prefill-Decode 分离架构的大语言模型推理场景，对接 vLLM 框架，覆盖 KVCache 的生成、存储、查询和传输全流程。不适用于训练场景的梯度缓存管理。

### 2.3  术语定义

| 术语 | 说明 |
|------|------|
| KVCache | 大语言模型推理过程中生成的 Key-Value 缓存，用于加速 Attention 计算 |
| Prefill | 推理的首次全序列计算阶段，生成完整 KVCache |
| Decode | 自回归生成阶段，逐 token 生成并复用 KVCache |
| Supernode | 由多个计算节点通过 Scale Up 网络互联组成的超节点 |
| PD Pair | Prefill 节点与 Decode 节点的绑定对 |
| XPU | 泛指 GPU/NPU 等加速计算设备 |
| HBM | 高带宽内存（High Bandwidth Memory） |
| CSD | Computational Storage Device，计算型存储设备 |
| Block | KVCache 的最小传输粒度单元，包含多个 token 的 KV 数据 |
| Token Key | 由 (reqID, layerID, token indices) 或全局 hash 生成的元数据查询键 |
| ZNS | Zoned Namespace SSD，支持顺序写入的分区命名空间 SSD |
| DPU | Data Processing Unit，数据处理单元 |
| SSU | Solid State Unit，固态存储单元 |
| H2D | Host to Device，主机内存到设备内存的数据传输 |
| SGL | Scatter-Gather List，离散-聚合列表 |
| vLLM | 开源 LLM 推理框架，本系统对接的上层框架 |

### 2.4  设计原则

| 原则 | 说明 |
|------|------|
| 分层解耦 | Client 与 Server 分开部署，XPU 轻量化客户端与 CPU 客户端分离 |
| 层次化缓存 | 通过 L1-L4 四级缓存层次实现性能与容量的平衡 |
| 双粒度接口 | 提供 block 粒度（批量传输）和 token 粒度（低延迟读取）双接口 |
| 软硬协同 | 关键路径硬件加速（请求解析、元数据查询、数据读写），其余路径软件实现 |
| 存储兼容 | 通过语义转换层对接块语义、文件语义和对象语义 |

---

## 3  系统概述

### 3.1  缓存层次架构

| 层级 | 存储介质 | 共享范围 | 互联方式 | 职责 |
|------|----------|----------|----------|------|
| L1 | HBM | GPU 间共享 | 本地总线 | 缓存热点 KVCache，本地高速管理 |
| L2 | DRAM/SSD | 超节点内共享 | Scale Up 网络 | 管理活跃及非活跃数据（SSD 为 L2.5） |
| L3 | 远端内存 | 超节点间共享 | Scale Out / 参数面网络 | 通过 PD Pair 绑定实现数据共享 |
| L4 | 块/文件/对象 | 集群间共享 | 存储网络 | 扩展存储系统，提供基础 KV 接口 |

### 3.2  模块清单

| 编号 | 模块名称 | 部署位置 | 职责 | 主要依赖 |
|------|----------|----------|------|----------|
| 1 | KVoFClient | XPU (轻量) + CPU (完整) | 对接 vLLM 框架，提供 block/token 双粒度接口 | BatchOp, Polling, SearchMeta |
| 2 | KVoFServer | CPU (计算/存储节点) | Pull 模式接收请求，读写本地数据并应答 | Append, UpdateMeta, Polling, SearchMeta |
| 3 | KVoFInit | XPU + CPU | 队列注册、传输缓存注册、Polling 服务创建 | ConnCtrl, MemAlloc |
| 4 | KVoFConnectionCtrl | CPU | 跨节点连接建立及 XPU-网卡连接，可调用第三方接口 | 无 |
| 5 | KVoFMemoryAlloc | XPU + CPU | XPU/CPU 传输缓存申请与分配 | 无 |
| 6 | KVoFSearchMeta | CPU | 查询元数据，Key=(reqID,layerID,token indices) | 无 |
| 7 | KVoFBatchOp | CPU | 批量 KVCache 读写转换为网络报文，支持稀疏聚合 | Transfer |
| 8 | KVoFTransfer | CPU + NIC | 调用内存语义/UB/RoCE 完成数据传输 | 无 |
| 9 | KVoFAppend | CPU | Server 侧追加写 SSD 存储数据 | 无 |
| 10 | KVoFPolling | CPU + NPU | 检测 KVoF 请求是否完成 | 无 |
| 11 | KVoFUpdateMeta | CPU | 按 token 粒度更新元数据 | 无 |

### 3.3  核心数据流

**Prefill 阶段数据流：** KVCache 在 Prefill 阶段产生后，对 token 和 block 进行 hash 算 key。通过调度模块将数据传输到某个 KVoFServer 进行持久化，或直接传输到 Decode 节点的 HBM 进行推理。数据流方向：vLLM → KVoFClient (XPU) → KVoFClient (CPU) → 网络 → KVoFServer (Decode 节点)

**Decode 冷数据加载流：** Decode 逐层加载冷数据时对低延迟有要求。vLLM 通过 KVoFGet(reqID, layerID, token indices, sgl) 读取 KVCache。首先查询本地元数据，如果命中，通过 H2D 传输到 HBM；如果本地未命中，获取全局 token Key 和存储服务器 IP。通过 KVoFBatchOperation 组成合适的 batch（稀疏 block 或稀疏 token 聚合），调用 KVoFTransfer 将请求发送到 KVoFServer。Server 解析后查询本地元数据，将 SSD 或 DRAM 中的数据通过 push 方式直接传输到发起请求的 XPU 所在 HBM，然后回复完成。

---

## 4  架构设计

### 4.1  逻辑视图 — 模块调用关系

模块间的调用关系如下（箭头表示调用方向）：

**第一层级：** vLLM Framework → KVoFClient (XPU 轻量化客户端)

**第二层级：** KVoFClient (XPU) 批量通知 → KVoFClient (CPU 完整客户端)

**第三层级：** KVoFClient (CPU) 与 KVoFServer 通过网络交互，Server 独立部署

**Client 侧调用链：** KVoFClient → KVoFSearchMeta；KVoFClient → KVoFBatchOperation → KVoFTransfer；KVoFClient → KVoFPolling

**Server 侧调用链：** KVoFServer → KVoFSearchMeta；KVoFServer → KVoFAppend；KVoFServer → KVoFUpdateMeta；KVoFServer → KVoFPolling

**初始化调用链：** KVoFClient/KVoFServer → KVoFInit → KVoFConnectionControl + KVoFMemoryAllocation

### 4.2  部署视图

#### 4.2.1  硬件节点拓扑

| 硬件节点 | 硬件类型 | 角色定位 | 典型配置 |
|----------|----------|----------|----------|
| Prefill 节点 - XPU | GPU / NPU (HBM) | 推理计算 + KVCache 生成 | 多块 XPU，本地 HBM 作为 L1 缓存 |
| Prefill 节点 - CPU | 服务器 CPU + DRAM | Client 完整逻辑执行 | 与 XPU 同机，通过 PCIe/NVLink 连接 XPU |
| Prefill 节点 - NIC | RDMA 网卡 | 网络传输执行 | 支持 RoCE / 内存语义 |
| Decode 节点 - XPU | GPU / NPU (HBM) | Decode 推理计算 | 多块 XPU，本地 HBM 作为 L1 缓存 |
| Decode 节点 - CPU | 服务器 CPU + DRAM | Client 完整逻辑 + Server 实例 | DRAM 作为 L2 缓存 |
| 存储节点 - CPU | 存储服务器 / CSD | Server 实例（L3/L4 存储） | DRAM + SSD (ZNS)，含 DPU/SSU SRAM 热缓存 |

#### 4.2.2  模块到硬件的映射

| 模块 | 硬件位置 | 实例说明 |
|------|----------|----------|
| KVoFClient (轻量化) | Prefill/Decode 节点 XPU | 与 vLLM 同进程，汇聚请求后批量通知 CPU 端 |
| KVoFClient (完整) | Prefill/Decode 节点 CPU | 独立进程，执行元数据查询、内存分配、传输控制 |
| KVoFServer | Decode CPU / 存储 CPU / CSD | 独立部署，通过网络接收 Client 请求 |
| KVoFInit | Prefill/Decode 节点 CPU | 分别在 Client 和 Server 启动时调用 |
| KVoFConnectionCtrl | CPU | 配置 NIC，建立跨节点连接和 XPU-NIC 连接，可调用第三方接口 |
| KVoFMemoryAlloc | XPU (HBM) + CPU (DRAM) | XPU 侧分配 HBM，CPU 侧分配 DRAM |
| KVoFSearchMeta | CPU / DPU SRAM (硬化路径) | 软件路径运行在 CPU，硬化路径元数据缓存在 DPU/SSU SRAM |
| KVoFBatchOp | CPU | 封装批量请求，支持稀疏 block/token 聚合 |
| KVoFTransfer | NIC (硬化路径可用) | 调用 NIC 完成 RDMA/RoCE/UB 传输，硬化时由硬件执行 |
| KVoFAppend | CPU / CSD | 追加写 SSD 存储数据 |
| KVoFPolling | CPU + NPU | Client 和 Server 各自创建 Polling 线程，同时部署在 CPU 和 NPU |
| KVoFUpdateMeta | CPU | 按 token 粒度更新元数据 |

#### 4.2.3  调用环节与硬件边界

| 调用环节 | 发起侧 | 目标侧 | 跨越边界 | 通信机制 | 延迟 |
|----------|--------|--------|----------|----------|------|
| vLLM → Client (XPU) | XPU | XPU | 进程内 | 函数调用 | 纳秒 |
| Client (XPU) 批量通知 Client (CPU) | XPU | CPU | XPU→CPU | PCIe / 共享内存 | 微秒 |
| Client (CPU) → SearchMeta | CPU | CPU / DPU SRAM | 进程内 / 硬化快路径 | 函数调用 / SRAM 查询 | 纳秒~微秒 |
| Client → BatchOp → Transfer | CPU | NIC | CPU→NIC | RDMA Verbs / UB | 微秒 |
| Transfer → 远端 Server (pull) | NIC | NIC | 网络 | RoCE / 内存语义 | 微秒~毫秒 |
| Server → XPU HBM (push 冷数据) | NIC (远端) | XPU HBM | 网络 → XPU | RDMA Write 直推 | 微秒~毫秒 |
| Server → Append (SSD) | CPU | SSD | CPU→存储 | 追加写 (ZNS) | 微秒 |
| 少量数据应答合并 | Server | Client | 网络 | 数据+应答合并一个网络包 | 微秒 |

### 4.3  运行视图 — 进程模型

| 进程/线程 | 角色 | 部署位置 | 说明 |
|-----------|------|----------|------|
| KVoFClient (XPU) | 轻量化客户端 | XPU | 与 vLLM 同进程，汇聚请求 |
| KVoFClient (CPU) | 完整客户端 | CPU | 独立进程，处理实际传输逻辑 |
| KVoFServer | 服务端 | CPU | 独立进程，Pull 模式数据存取与应答 |
| Polling 线程 | 完成检测 | CPU + NPU | Init 阶段创建，在 CPU 和 NPU 上均部署 |

### 4.4  时序视图 — 阶段流程

#### 4.4.1  Init 阶段

初始化阶段的交互流程：

1. KVoFClient / KVoFServer 调用 KVoFInit，触发初始化流程
2. KVoFInit 调用 KVoFConnectionControl，完成跨节点连接建立和 XPU-网卡连接（可调用第三方接口）
3. KVoFInit 调用 KVoFMemoryAllocation，完成队列注册和传输缓存注册
4. KVoFInit 创建 KVoFPolling 服务（在 CPU 和 NPU 上均部署），用于后续请求完成检测

#### 4.4.2  Prefill 阶段

Prefill 阶段负责将生成的 KVCache 以 block 粒度传输并持久化：

1. vLLM 框架将 Prefill 产生的 KVCache 交给 KVoFClient (XPU 轻量端)，调用 KVoFPut(blockID)
2. Prefill 产生的 KVCache 对 token 和 block 进行 hash 算 key，生成元数据索引
3. KVoFClient (XPU) 汇聚多个 block 后批量通知 KVoFClient (CPU)
4. KVoFClient (CPU) 调用 KVoFBatchOperation 将数据封装为固定格式网络传输报文
5. KVoFBatchOperation 调用 KVoFTransfer，将数据发送至远端 KVoFServer 持久化，或直接传输到 Decode 节点 HBM
6. KVoFServer 调用 KVoFAppend 以追加写方式存储数据到 SSD
7. KVoFServer 调用 KVoFUpdateMeta 按 token 粒度更新元数据

#### 4.4.3  Decode 阶段

Decode 阶段分为本地命中、远端访问和新数据持久化三条路径：

1. 推理框架选择 topk 通知 KVoFClient (XPU)，调用 KVoFGet(reqID, layerID, token indices, sgl)，XPU 批量通知 CPU 客户端
2. KVoFClient (CPU) 调用 KVoFSearchMeta，以 Key=(reqID, layerID, token indices) 查询本地元数据
3. 本地命中路径：通过 H2D 传输到 HBM，或通过 KVoFBatchOperation 访问本地数据
4. 远端访问路径：获取全局 token Key 和服务器 IP，KVoFClient 调用 KVoFMemoryAllocation 分配传输缓存，KVoFBatchOperation 组成稀疏 block/token 聚合 batch，调用 KVoFTransfer 发送请求
5. KVoFServer 调用 KVoFSearchMeta 查找本地数据，将 SSD/DRAM 中的数据通过 push 方式直接传输到请求方 XPU HBM，然后回复完成。少量数据可与应答合并在一个网络包中
6. 如有新生成的 KVCache，采用 Prefill 阶段的流程进行数据持久化

---

## 5  模块详细设计

### 5.1  KVoFClient 设计

#### 5.1.1  模块职责

KVoFClient 部署在 GPU/NPU/CPU 上，负责对接 vLLM 框架。架构分为两层：

- **XPU 轻量化客户端：** 部署在 GPU/NPU 上，接收 vLLM 的 KVCache 读写请求，汇聚多个请求后批量通知 CPU 侧客户端。轻量化设计确保对 XPU 计算资源的占用最小化。
- **CPU 完整客户端：** 部署在计算节点 CPU 上，接收 XPU 的批量通知后执行实际的元数据查询、内存分配和数据传输操作。

#### 5.1.2  接口定义

**说明：** Prefill 阶段、Decode 存储阶段和数据搬移阶段使用 block 粒度传输；Decode 读取数据阶段可采用 token 粒度读取 KVCache。

| 方法/API | 参数 | 粒度 | 说明 |
|----------|------|------|------|
| KVoFPut(block) | blockID | Block | Prefill/Decode 存储阶段写入 KVCache block |
| KVoFGet(block) | blockID | Block | Decode 数据搬移阶段读取 KVCache block |
| KVoFPut(token) | reqID, layerID, token indices, sgl | Token | Decode 阶段按 token 粒度写入 KVCache |
| KVoFGet(token) | reqID, layerID, token indices, sgl | Token | Decode 阶段按 token 粒度读取 KVCache，低延迟 |
| kvof_batch_notify() | request_list[] | - | XPU 轻量端汇聚后批量通知 CPU 客户端 |

### 5.2  KVoFServer 设计

#### 5.2.1  模块职责

KVoFServer 部署在 CPU 上（计算节点或存储节点，含 CSD）。负责接收 KVoFClient 发起的请求，读写本地数据并应答。Server 与 vLLM 无直接交互。

**传输模式：** 数据传输采用 pull 模式（Client 发起请求，Server 响应）。少量数据传输时，数据可以和应答信息合并在一个网络包中传输。Decode 冷数据加载场景下，Server 通过 push 方式将数据直接传输到发起请求的 XPU HBM。

#### 5.2.2  接口定义

| 方法/API | 说明 |
|----------|------|
| handle_request() | 接收并分发 Client 请求（固定 64B 请求报文） |
| send_response() | 发送 16B 应答报文，少量数据可合并传输 |
| local_read() | 读取本地 DRAM/SSD 存储的 KVCache 数据 |
| local_write() | 通过 KVoFAppend 追加写入 SSD |
| push_to_hbm() | 冷数据加载时 push 数据直接到请求方 XPU HBM |

### 5.3  KVoFInit 设计

用于队列注册、传输缓存注册和 Polling 服务创建。Init 阶段完成线程绑定、队列创建、内存分配和连接建立。被 KVoFClient 和 KVoFServer 共同调用。向下调用 KVoFConnectionControl、KVoFMemoryAllocation，并创建 KVoFPolling 服务。

### 5.4  KVoFConnectionControl 设计

用于实现 KVoF 跨节点连接建立，也包括 XPU 和网卡之间的连接建立。可以调用第三方接口实现。被 KVoFInit 调用。

### 5.5  KVoFMemoryAllocation 设计

实现 XPU 或 CPU 的传输缓存申请与分配。在 Init 阶段被 KVoFInit 调用完成初始分配，在 Decode 阶段远端访问时被 KVoFClient (CPU) 直接调用进行动态分配。

### 5.6  KVoFSearchMeta 设计

#### 5.6.1  模块职责

用于查询元数据，定位 KVCache 数据的存储层级和位置。

#### 5.6.2  查询流程

KVoFClient 收到请求后，先根据 Key = (reqID, layerID, token indices) 查询本地元数据。如果本地命中，直接返回本地存储位置；如果本地未命中，获取 Key = 全局 token Key 和存储数据的服务器 IP，以便 Client 发起远端访问。硬化场景下，元数据热缓存在 DPU/SSU 的 SRAM 上，查询走硬化的快路径。

#### 5.6.3  元数据 Key 结构

| 字段 | 说明 |
|------|------|
| reqID | 请求标识，唯一标识一次推理请求 |
| layerID | 模型层编号，标识 KVCache 属于哪一层 |
| token indices | Token 索引列表，标识具体的 token 位置 |
| 全局 token Key | Prefill 阶段对 token 和 block 进行 hash 生成的全局唯一标识 |

### 5.7  KVoFBatchOperation 设计

用于将 vLLM 的批量 KVCache 读写转换为适合网络传输的固定格式报文。支持稀疏 block 或稀疏 token 的聚合，组成合适的 batch。被 KVoFClient (CPU) 调用，向下调用 KVoFTransfer 完成实际传输。

### 5.8  KVoFTransfer 设计

调用传输层协议完成数据传输，支持内存语义、UB、RoCE 等多种传输方式。硬化场景下由硬件进行请求组装和执行。被 KVoFBatchOperation 调用。

### 5.9  KVoFAppend 设计

在 Server 侧存储接收到的 KVCache 数据。采用追加写方式写入 SSD，配合 ZNS 机制实现顺序写入优化。Prefill 阶段 KVoFServer 调用 Append 完成数据落盘。

### 5.10  KVoFPolling 设计

用于检测 KVoF 请求是否完成。需要同时部署在 CPU 和 NPU 上，在 Init 阶段作为 Polling 服务创建，被 KVoFClient 和 KVoFServer 共同使用。

### 5.11  KVoFUpdateMeta 设计

用于写数据后更新元数据，按照 token 粒度更新。Prefill 阶段数据存储完成后，KVoFServer 调用 UpdateMeta 记录数据的存储位置、层级等信息。

---

## 6  硬化设计

KVoF 系统在关键路径上支持硬件加速，以降低延迟并提升吞吐。下文总结五项核心硬化设计点。

### 6.1  请求解析硬化

固定请求报文大小为 64B，应答报文为 16B，硬件可直接解析而无需软件参与。

**请求报文格式（64B）：**

| 字段 | 大小 | 说明 |
|------|------|------|
| opcode | 1B | 操作码（读/写/查询等） |
| command ID | 2B | 命令标识，用于匹配应答 |
| NSID | 4B | 命名空间 ID |
| data pointer | 16B | 数据指针（SGL 地址） |
| token Key | 16B | Token 元数据查询键 |
| token bit map | 8B | Token 位图，指示请求的 token 位置 |
| block bit map | 8B | Block 位图，指示请求的 block 位置 |
| rsv | 剩余 | 保留字段 |

**应答报文格式（16B）：**

| 字段 | 大小 | 说明 |
|------|------|------|
| command ID | 2B | 命令标识，与请求匹配 |
| status | 4B | 完成状态码 |
| dfx 信息 | 4B | 诊断与调试信息 |
| rsv | 剩余 | 保留字段 |

### 6.2  元数据热缓存

元数据热缓存在 DPU 或 SSU（Solid State Unit）的 SRAM 上。元数据查询走硬化的快路径，无需经过 CPU，大幅降低查询延迟。软硬协同时，CPU 上的 KVoFSearchMeta 作为回退路径。

### 6.3  KVCache 数据读写硬化

KVCache 数据读写走硬化路径，由硬件进行请求组装和执行。硬件直接解析固定 64B 请求报文，组装 SSD 读写命令，完成后生成 16B 应答。全路径无 CPU 参与。

### 6.4  ZNS 地址映射简化

利用 ZNS（Zoned Namespace）机制，将 token Key 到盘物理块地址（PBA）的映射简化为一层。相比传统 FTL 的多层地址转换，显著降低地址转换延时和内存容量开销。配合 KVoFAppend 的追加写模式，充分利用 ZNS 的顺序写入特性。

### 6.5  SSD Page 多 Token 命中优化

同一个 SSD page 中的数据读到内存后，筛选是否命中多个 token。这种设计减少了 token 粒度读盘的 IOPS 需求，将多次独立的 token 读取合并为一次 page 读取，显著提升冷数据加载效率。

---

## 7  兼容性设计

### 7.1  语义转换层

KVoF 系统对外提供 KVCache block 和 token 粒度的读写接口。内部通过语义转换层对接不同的底层存储引擎：

| 存储语义 | 对接引擎 | 适用场景 | 说明 |
|----------|----------|----------|------|
| 块语义 | SSD (含 ZNS) | L2/L2.5 层本地存储 | 直接对接 SSD 块设备，配合 ZNS 追加写优化 |
| 文件语义 | 3FS（分布式文件系统） | L3/L4 层跨节点存储 | 通过文件接口实现跨节点数据共享 |
| 对象语义 | 云存储（S3 等） | L4 层集群间存储 | 对接云存储服务，实现大规模冷数据归档 |

语义转换层位于 KVoFAppend 和 KVoFTransfer 的下方，对上层模块透明。上层模块始终使用统一的 KVoFPut/KVoFGet 接口，语义转换层根据配置自动选择底层存储引擎。

---

## 8  测试要点

### 8.1  单元测试范围

| 模块 | 测试内容 |
|------|----------|
| KVoFClient (XPU) | 请求汇聚逻辑、批量通知触发条件、block/token 双粒度接口参数校验 |
| KVoFClient (CPU) | 元数据查询路由、本地/远端分支逻辑、稀疏聚合正确性 |
| KVoFServer | Pull/Push 双模式应答、少量数据合并传输、Append 与 UpdateMeta 序列 |
| KVoFSearchMeta | 本地/全局 Key 查询准确性、硬化路径 SRAM 查询一致性 |
| KVoFBatchOp | 固定 64B 报文封装、稀疏 block/token 聚合正确性 |
| KVoFAppend | SSD 追加写正确性、ZNS 顺序写入验证 |
| 硬化路径 | 请求解析 64B/16B 报文、元数据 SRAM 热缓存、ZNS PBA 映射、SSD page 多 token 命中 |

### 8.2  集成测试场景

| 场景 | 验证内容 |
|------|----------|
| Prefill 端到端 | KVCache hash key 生成 → XPU 批量通知 CPU → BatchOp → Transfer → Server Append → UpdateMeta |
| Decode 本地命中 | SearchMeta(reqID,layerID,indices) 命中 → H2D 传输到 HBM |
| Decode 远端冷数据加载 | 全局 Key + IP → MemAlloc → BatchOp 稀疏聚合 → Transfer → Server push 到 XPU HBM |
| 少量数据合并 | 远端少量数据与应答合并在一个网络包中传输 |
| 硬化路径端到端 | 硬件解析请求 → SRAM 元数据查询 → 硬件数据读写 → 硬件生成应答 |
| 存储兼容性 | 块语义(SSD) / 文件语义(3FS) / 对象语义(云存储) 分别验证 |

---

## 9  实现优先级

### 9.1  Phase 1: 核心功能

| 任务 | 描述 |
|------|------|
| KVoFInit 完整实现 | ConnectionControl、MemoryAllocation、Polling 服务创建（CPU+NPU） |
| KVoFClient 双层架构 + 双粒度接口 | XPU 轻量端 + CPU 完整端，实现 KVoFPut/KVoFGet 的 block 和 token 双粒度 |
| Prefill 数据通路 | Hash key 生成 → BatchOp → Transfer → Server Append (SSD 追加写) → UpdateMeta (按 token) |
| SearchMeta Key 架构 | 本地 Key=(reqID,layerID,indices) + 全局 token Key + 服务器 IP 路由 |

### 9.2  Phase 2: Decode 与 Server 优化

| 任务 | 描述 |
|------|------|
| Decode 本地/远端分支 | SearchMeta 查询 + 本地 H2D / 远端 MemAlloc + 稀疏聚合 |
| Server Pull/Push 双模式 | Pull 模式普通应答 + Push 模式冷数据直推 HBM + 少量数据合并 |
| 多传输协议支持 | Transfer 模块支持内存语义、UB、RoCE 等协议热插拔 |

### 9.3  Phase 3: 硬化与兼容性

| 任务 | 描述 |
|------|------|
| 请求解析硬化 | 固定 64B/16B 报文硬件解析与生成 |
| 元数据 SRAM 热缓存 | DPU/SSU SRAM 元数据查询硬化快路径 |
| ZNS + SSD page 优化 | Key→PBA 一层映射 + SSD page 多 token 命中 |
| 语义转换层 | 对接块语义(SSD)、文件语义(3FS)、对象语义(云存储) |
