# KV 缓存 TTL 与推理系统性价比分析报告

2026-11-13 更新

## 核心结论

DeepInfer 的 agent 3 天、chat 7 天 TTL 是成本最优解，不是慷慨。每 token 的 KV 被压到约 0.9 KB 后，SSD 留存一天只需约 $0.00024/百万 token。

- **别家 TTL 短的原因**：KV 放在显存或单机本地存储，介质贵且依赖路由亲和；同时采用“付费写入加承诺时长”的产品契约。
- **长 TTL 的边际收益很小**：chat 从 7 天缩到 3 天，命中率只掉 0.5–1 个点。
- **7 天大致落在盈亏平衡点**：按 82.9% 毛利率反推成本后，chat 命中率在 80% 左右时，多留 4 天基本打平到略赚。硬件角度的独立重算见下方。
- **分析性价比应看命中率–TTL 曲线**，TTL 本身是结果变量。分层后 7 天留存在任何 GPU 成本假设下都成立。

硬件成本角度的结论（不依赖公开价格和毛利率）：

- **判断指标 T***：存一个 token 多少天等于重算一次。全 SSD 按 2025 年盘价为 90–400 天，按 2026 年盘价为 19–85 天，HDD 约 280–1,650 天。
- **多留 4 天的盈亏平衡**：每写入一个 token 需被命中 1%–4.4%（2025 盘价）、4.7%–21%（2026 盘价）或 0.2%–1.4%（HDD），实际约 2.5%–10%。涨价前成立，现价下全 SSD 只是边缘，HDD 下成立。
- **最敏感的输入是盘价**：30 TB TLC 盘一年涨约 6.5 倍，每卡配套存储占 GPU 投资从约 5% 升到约 25%；线上利用率的影响在 30% 以内。
- **3–7 天有两个与经济性无关的解释**：装机容量除以写入速率正好是 3–7 天；写入寿命要求 TTL 不短于约 1 天（TLC）或 3–10 天（QLC）。按能耗算，留 7 天只相当于重算一次的 2%–3%。
- **合理架构是 SSD + HDD 分层**：热层 SSD 约 1 天，冷层 HDD 数周，HDD 需聚合到单盘单次读 1 MiB 以上。按 2026 年盘价新建，每卡存储成本从约 $13,900 降到约 $3,900（留 7 天）。

除标注来源的数字外，本报告的换算和估算均为推算，SSD 单价等为假设值。

## 各家 TTL 配置与模型核对

主流厂商的缓存保留时间集中在 5 分钟到 24 小时，只有 DeepSeek 和豆包显式缓存达到天级。

| 厂商 | 当前 TTL 配置 | 写入 / 读取计费 | 是否对应最新模型 |
| --- | --- | --- | --- |
| [DeepSeek](https://github.com/deepseek-ai/DeepInfer/blob/main/doc/inference_deployment_qa.md) | agent 3 天、chat 7 天；[API 文档](https://api-docs.deepseek.com/guides/kv_cache)对外只承诺尽力而为、几小时到几天 | 写入免费，读取为输入价的 2% | 披露只针对 V4.1 Flash，其他型号未证实 |
| [OpenAI（新）](https://developers.openai.com/api/docs/guides/prompt-caching) | 默认 30 分钟，可选 12 小时 | 写入 1.25× / 2.5×，读取 0.1×（GPT-6.1 Sol/Terra 为 0.05×） | 是，GPT-5.6 及以后 |
| OpenAI（旧） | 内存档闲置 5–10 分钟、最长 1 小时；24h 档通常约 30 分钟、最长 24 小时 | 写入不加价 | 否，GPT-5.5 及更早 |
| [Anthropic](https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching) | 5 分钟或 1 小时，命中即续期 | 写入 1.25× / 2×；读取 0.1×，新模型 0.05×–0.025× | 核对到 Fable 5.1、Opus/Sonnet/Haiku 5.5，未查到 5.2 的单独条目 |
| [Gemini](https://ai.google.dev/gemini-api/docs/generate-content/caching) | 隐式未公布；显式默认 1 小时、无上下限 | 显式按小时收存储费 | 部分：显式缓存不支持 Gemini 4 Argon |
| [豆包（火山方舟）](https://docs.volcengine.com/docs/ark/context-api?lang=zh) | 显式 1 小时到 7 天可配；隐式未公布 | 显式收存储费加命中折扣价；隐式不收存储费 | 支持模型清单未逐一核对 |
| [Qwen（百炼）](https://help.aliyun.com/model-studio/context-cache) | 显式 5 分钟，命中后重置 | 显式写入 1.25× | 是，清单含 qwen3.8-max |
| [Kimi](https://github.com/MoonshotAI/kimi-code/issues/3942) | 默认 5 分钟，可选 1 小时 | 未核实 | K3 已核对，来源为 GitHub issue |
| xAI / Mistral | 未公布 | 写入免费 | 未核对 |

校验说明：

- **OpenAI 口径有冲突。** 10 月中旬的[第三方横评](https://www.developersdigest.tech/blog/prompt-caching-across-ai-providers-2026)写 GPT-5.6+ 只有 30 分钟一档，官方文档现为 30 分钟和 12 小时两档，以官方为准。
- **“OpenAI 5–10 分钟”是旧模型口径。** 新模型已转向付费写入加承诺最短时长。
- **内部策略与对外承诺不同。** DeepSeek 的 3/7 天是存储侧淘汰策略，Anthropic 和 OpenAI 新模型的 TTL 是付费买到的档位。
- **实测 TTL 是一条曲线。** [第三方实测](https://gist.github.com/monperrus/9784f52d5144975fd11be7f642c4a8fc) gpt-4.1-mini 在 45 分钟内全中，90 分钟全不中。

## 从 TTL 反推请求画像

每个 TTL 档位对应一类请求间隔，以下均为推断。

| TTL 档位 | 代表厂商 | 推断的请求画像 |
| --- | --- | --- |
| 5 分钟，命中续期 | Anthropic、Qwen、Kimi 默认 | agent 工具循环，请求间隔为秒到分钟级，靠续期让缓存在任务期间保持热 |
| 30 分钟到 1 小时 | OpenAI 新默认、Anthropic/Kimi 加价档 | 人在回路的会话，用户阅读或思考后再回复；每十几分钟一次的定时任务 |
| 12–24 小时 | OpenAI | 跨工作日的固定前缀，如大段系统提示和工具定义 |
| 按小时收存储费 | Gemini、豆包显式 | 一份大文档或知识库被反复查询，缓存是被命名的资源 |
| 3–7 天，尽力而为 | DeepSeek | 全量流量自动缓存，覆盖隔夜、隔周末和隔周的回访 |

OpenAI 旧架构的缓存状态在单机上，每个 key 超过约 15 次/分钟就会溢出到别的机器。这说明它依赖路由亲和，天然不适合长留存。

## DeepInfer TTL 的成因

DeepSeek 把“留存一天的成本”压到了极低，靠的是四项设计。事实来自 [DeepInfer 官方 Q&A](https://github.com/deepseek-ai/DeepInfer/blob/main/doc/inference_deployment_qa.md) 和 [InfoQ 报道](https://infoq.cn/article/NBXP966Bz3eqr6Qidvaf?amp=)。

1. **每 token 的 KV 极小。** KV 以 128 token 为一页，3FS 针对约 112 KiB 的块做了优化。若一块对应一页，折合每 token 约 0.9 KB（推算）。主 KV 采用 FP4 级别的低精度格式。
2. **只留值得留的那一半。** SWA 层的 KV 只在活跃会话的几分钟内有用，落盘会占掉近一半持久容量。V4.1 改为不落盘，未命中时只重算最近 128 个 token。
3. **集中式 SSD 存储池。** 约 100 PiB SSD、10+2 纠删码、每卡约 14.4 TB 逻辑容量。任何 prefill 机器都能命中任何前缀，不需要路由亲和，计算节点故障不丢缓存。
4. **读取带宽跟得上。** 存储节点单机读吞吐至少 40 GB/s，集群峰值聚合读取约 10 TiB/s，读回比重算快。

### chat 7 天与 agent 3 天的差异

官方解释是 chat 存在明显长尾命中，少量缓存被反复读取。以下为推断：

- **Agent**：单会话上下文大、写入量大，任务结束或上下文被压缩改写后几乎不再回访。3 天已覆盖周五到周一的间隔。
- **Chat**：单会话体积小，但用户会隔几天回到旧对话。7 天覆盖一个周循环，体积小、回访概率高，留得久更划算。

### 容量自洽性检验（推算）

- 每卡 14.4 TB 除以 0.9 KB，约可存 160 亿 token。
- 单集群每天 155 万亿 token，按约 6,500–7,800 张卡估算，每卡每天处理约 200–240 亿 token。
- 存储只够放不到一天的总流量，却要撑 3–7 天。每天新写入的 token 只能占总流量的约 10%–25%，即整体命中率大概在 75%–90%。官方未公布命中率。

## 命中节省的两种口径

用户每命中百万 token 少付 $0.147（谷时），DeepSeek 自己只省约 $0.02–0.05 的算力。两个口径要分开看。

### 用户侧：按价格差

| 时段 | 未命中输入价（$/百万 token） | 命中输入价 | 每百万 token 节省 |
| --- | --- | --- | --- |
| 谷时 | 0.15 | 0.003 | 0.147 |
| 峰时 | 0.30 | 0.006 | 0.294 |

价格取自 [Developers Digest 横评](https://www.developersdigest.tech/blog/prompt-caching-across-ai-providers-2026)中 deepseek-flash 的报价，两档折扣均为 98%。写入不收费，命中一次即净省。

单次请求输入费用 = 未命中 token × 未命中价 + 命中 token × 命中价。例：10 万 token 输入、谷时、95% 命中，费用从 $0.015 降到约 $0.00104，节省约 93%。

### DeepSeek 侧：按算力成本

净节省 = 省掉的 prefill 算力成本 −（存储留存成本 + 读取搬运成本 + 尾部重算成本）。

- **读取搬运**占用网络和 IO，不占 GPU 算力。
- **尾部重算**是每次命中固定重算最近 128 个 token，与上下文长度无关，可忽略。
- **prefill 算力成本**官方未披露，下一节用毛利率反推。

## 基于 82.9% 毛利率的成本推算

按媒体报道的 API 毛利率反推，prefill 成本约 $0.034–0.050/百万 token，chat 留存 7 天大致落在盈亏平衡点。

82.9% 是 [The Information 和彭博报道](https://deepthink.ltd/blog/deepseek-revenue-surge-82-9-api-gross-margin-ipo-2026/)的 2026 年 1–7 月 API 毛利率，不是官方财报，时间早于 V4.1 Flash 和峰谷定价。同期公司整体毛利率为 44.6%。

| 步骤 | 计算 | 结果 |
| --- | --- | --- |
| 每卡每小时成本 | [$38 收入](https://www.yicai.com/news/103401110.html) × (1 − 0.829) | 约 $6.5 |
| prefill 吞吐 | [每卡每秒 6 万–8.9 万未命中 token](https://dev.to/tokely/deepseek-open-sourced-the-engine-behind-its-api-is-self-hosting-v41-flash-cheaper-than-paying-per-3ki7)（131K 上下文） | 每卡每小时 2.2–3.2 亿 token |
| prefill 成本，满载 | $6.5 ÷ 2.2–3.2 亿 | $0.020–0.030/百万 token |
| prefill 成本，60% 利用率 | 满载成本 ÷ 0.6 | $0.034–0.050/百万 token |
| 存储成本 | [每 504 卡缓存层年成本约 70 万美元](https://vkambati.substack.com/p/the-38-gpu-hour) | 每卡每小时约 $0.16；约 $0.00024/百万 token/天 |

交叉验证：第三方测算自建全成本约 $5.18/卡·小时（区间 $3.10–$8.32），云上租用 B200 约 $8.6/卡·小时，$6.5 落在区间内。未命中输入卖 $0.15、成本 $0.02–0.05，单项毛利率约 67%–87%，与 82.9% 吻合。存储只占单卡成本的约 2.4%。

### chat 多留 4 天的盈亏平衡

划算条件：命中率提升 × prefill 成本 > 未命中占比 × 4 天 × 每天存储成本。

- 收益：0.5%–1% × $0.035 ≈ $0.00018–0.00035/百万 token。
- 成本：未命中占比 × $0.00096/百万 token。

| chat 命中率 | 存储成本（$/百万 token） | 结论 |
| --- | --- | --- |
| 90% | 0.00010 | 划算，收益是成本的 2–3.6 倍 |
| 80% | 0.00019 | 基本打平到略赚 |
| 70% | 0.00029 | 接近打平或略亏 |

agent 流量只给 3 天，说明它在第 3–7 天的命中率增量明显低于 0.5 个点。

## 对性价比分析的含义

TTL 是结果变量，建模时应以四个输入为核心：每 token KV 字节数、存储介质单价、prefill 单位成本、复用间隔分布。

- **短 TTL 不等于命中率低很多。** 5 分钟加命中续期已覆盖 agent 循环的绝大部分复用，长 TTL 多拿到的是隔夜、隔周回访这一小段尾部。
- **可比口径是命中率和每次未命中的代价。** DeepSeek 写入免费，过期只是多算一次 prefill；Anthropic 和 OpenAI 新模型过期后要再付一次 1.25× 以上的写入费。
- **缓存折扣让出的远多于省下的。** 未命中 token 的单位毛利约 $0.10–0.13，命中 token 只有约 $0.001。谷时 GPU 有空闲时，命中对 DeepSeek 是减少毛利的（推论）。
- **缓存对 DeepSeek 的价值在峰时容量。** 峰时 GPU 是瓶颈，省下的 prefill 算力可直接接新流量，一次命中的价值接近价格差 $0.294。建议用价格差作上限、算力成本作下限来框定区间。
- **存储定价差距反映介质差距。** Gemini 3.8 Flash 显式存储约 $12/百万 token/天，豆包约 0.4 元/天，DeepSeek 估算成本约 $0.00024/天。

## 硬件成本角度的独立分析

不依赖公开价格和毛利率，只用硬件成本重算：按涨价前的盘价 7 天明显合理，按 2026 年盘价全 SSD 方案只在 GPU 成本偏高时勉强打平。

判断指标是 T*：存一个 token 多少天，花的钱等于重算它一次。

### 存储侧

[30 TB TLC 企业盘](https://growthmarketreports.com/report/data-center-solid-state-drive-market-global-industry-analysis)从 2025 年三季度的 3,460 美元涨到 2026 年三季度的 22,600 美元。[30 TB HDD](https://www.storagereview.com/news/enterprise-ssd-prices-run-at-6-5x-last-year-vdura-pegs-a-30tb-tlc-drive-at-22600) 现价约 1,216 美元。假设：机箱等每 GB 摊 0.05 美元（HDD 为 0.01 美元），纠删码开销 1.2 倍，5 年折旧。

| 介质与采购时点 | 每百万 token 每天（$） | 留 7 天（$） |
| --- | --- | --- |
| SSD，2025 年盘价 | 0.00010 | 0.0007 |
| SSD，2026 年盘价 | 0.00047 | 0.0033 |
| HDD，2026 年盘价 | 0.00003 | 0.0002 |

### 计算侧

吞吐取 [DeepInfer 开源仓库](https://github.com/deepseek-ai/DeepInfer)的离线基准（4 张 NVIDIA SM100，13 万上下文，每卡每秒 6 万–8.9 万未命中 token），乘负载均衡效率 0.8 和峰谷负载率 0.6–0.8，线上有效吞吐约每卡每小时 1.0–2.1 亿 token。GPU 每小时成本取两个端点，型号与基准不一致，仅作区间参考。

| GPU 成本口径 | 重算每百万 token（$） |
| --- | --- |
| [$1.73/卡·小时](https://inferencex.semianalysis.com/chips/b200)（B200 超大规模厂商全成本） | 0.008–0.017 |
| [$5.18/卡·小时](https://vkambati.substack.com/p/the-38-gpu-hour)（自建 B300 机柜测算） | 0.025–0.050 |

### T* 与盈亏平衡

每写入一个 token，在第 3–7 天实际被命中约 2.5%–10% 次（0.5%–1% 的命中率提升除以 10%–20% 的未命中占比）。多留 4 天需要的回报等于 4 除以 T*。

| 组合 | T*（天） | 多留 4 天需要的回报 | 是否成立 |
| --- | --- | --- | --- |
| SSD 2025 盘价 + 高 GPU 成本 | 270–400 | 1.0%–1.5% | 成立 |
| SSD 2025 盘价 + 低 GPU 成本 | 90–130 | 3%–4.4% | 基本成立 |
| SSD 2026 盘价 + 高 GPU 成本 | 57–85 | 4.7%–7% | 边缘 |
| SSD 2026 盘价 + 低 GPU 成本 | 19–28 | 14%–21% | 不成立 |
| HDD 2026 盘价 | 约 280–1,650 | 0.2%–1.4% | 成立 |

### 只有硬件角度能看到的三点

- **TTL 约等于容量除以写入速率。** 每卡 14.4 TB、每卡每天新写入约 2–5 TB 逻辑数据，能撑 3–7 天。存量盘是沉没成本，留到写满即最优。
- **写入寿命给 TTL 设下限。** 全量写入下每天整盘写入次数约为 1/TTL。[TLC 额定 1–3 次，QLC 为 0.1–0.3 次](https://electronics.alibaba.com/buyingguides/30tb-ssd-guide-when-to-buy,-what-to-skip)，对应 TTL 下限约 1 天和 3–10 天。官方未披露盘型。
- **按能耗算留存几乎免费。** 重算一百万 token 约耗 0.009–0.013 度电，存一天约 0.00004 度（每 TB 1.6 瓦为假设），留 7 天只相当于重算一次能耗的 2%–3%。

每卡配套约 17.3 TB 物理容量：按 2025 年盘价约 2,900 美元，占每卡 GPU 投资约 5%；按 2026 年盘价约 13,900 美元，约 25%。

由 prefill 产能反向校验：prefill 约占一半 GPU，以离线上限满负荷一天最多处理约 30 万亿未命中 token，命中率不会低于约 81%。

## SSD + HDD 冷热分层与留存时间更新

分层后留存时间由两个 TTL 决定：热层约 1 天，冷层在经济上几乎不设限。DeepSeek 目前是全 SSD，3 天和 7 天是该约束下的结果，不是理论最优。

### HDD 聚合块大小

冷层应聚合到单盘单次读 1 MiB 以上，目标 4–16 MiB。按定位约 10 毫秒、[顺序吞吐 200–250 MB/s](https://datacenterdisk.com/reports/ssd-hdd-price-divergence-2026) 取 220 MB/s 计算：

| 单盘单次读大小 | 耗时（ms） | 有效吞吐（MB/s） | 占顺序吞吐 |
| --- | --- | --- | --- |
| 112 KiB（现有块） | 10.5 | 11 | 5% |
| 1 MiB | 14.8 | 71 | 32% |
| 2 MiB | 19.5 | 107 | 49% |
| 4 MiB | 29 | 144 | 65% |
| 8 MiB | 48 | 174 | 79% |
| 16 MiB | 86 | 195 | 89% |

- **平衡点约 2 MiB。** 读得更小，时间主要花在寻道上；1 MiB 是值得做的门槛。
- **尺寸按单盘读计算。** 10+2 纠删码会把 1 MiB 逻辑对象切成约 105 KiB 的分片，逻辑条带应设计为 10–64 MiB，或冷层改用副本、局部修复码。
- **按会话前缀链聚合。** 冷命中需要整条前缀；1 万 token 的会话约 9 MB，3 万 token 约 26 MB，10 盘并行读取约 22 毫秒。
- **队列门槛。** 冷层承接 1% 流量时，112 KiB 读需要约 1,500 块盘满负荷，占冷层盘数 50%–75%；聚合到 1 MiB 降到约 10%，4 MiB 约 5%。
- **降级时重排。** 数据从 SSD 降级到 HDD 时按会话写成连续大段，按过期时间整段删除，不需要垃圾回收。

### 两个 TTL

- **热层（SSD）约 1 天。** 下限由 TLC 写入寿命决定；多在 SSD 放一天比 HDD 贵约 15 倍；1 天覆盖 agent 任务和当天内的 chat 回访。
- **冷层（HDD）几周到几个月。** 盈亏平衡门槛是每写入 token 每天被命中 0.06%–0.4% 次，第 3–7 天的实际水平约为每天 0.6%–2.5%。上限来自模型更新使 KV 失效、数据留存合规和硬盘供应。

| 问题 | 全 SSD | SSD + HDD 分层 |
| --- | --- | --- |
| 7 天是否合理 | 取决于盘是否在涨价前采购 | 在任何 GPU 成本假设下成立 |
| chat 7 天、agent 3 天的差异 | 由尾部命中率差异解释 | 必要性下降，agent 也可留 7 天以上 |
| 留存时间的决定因素 | 存储成本与命中率的平衡 | 热层看寿命和延迟，冷层看模型发布周期 |
| 新建成本（每卡，2026 盘价） | 约 $13,900 | 留 7 天约 $3,900；留 30 天约 $7,800 |

新建成本按每卡每天新写入约 3.5 TB 物理容量粗算。待定问题：冷命中后是否回迁 SSD、共享前缀是否单独留在热层、7 天以后的命中率。

## 相关论文与官方说明

没有论文专门论证 KVCache 是否需要跨天保存，带数字的官方说明只有 DeepSeek 一家；公开 trace 基本只覆盖一天以内。

| 论文 | 来源 | 与本报告相关的结论 |
| --- | --- | --- |
| [KVCache in the Wild](https://arxiv.org/html/2506.02634v3) | 上交 + 阿里，通义线上 trace | to-C 流量 80% 的复用间隔在 10 分钟内，90% 的块在 612 秒后不再复用；无限缓存下命中率上限约 62% 和 54%；分析窗口为一天 |
| [CacheWise](https://arxiv.org/pdf/2606.16824) | 华盛顿大学，Claude Code trace | 会话时长中位数约 36 分钟，长尾超过 2.6 小时；按预测的下次复用时间淘汰 |
| [DualPath](https://arxiv.org/html/2602.21548v2) | 北大 + 清华 + DeepSeek | agent 命中率 95% 以上；每 PFLOP 对应的 KV 量 DeepSeek-V3 为 4.8–5.8 GB，Qwen2.5-32B 为 117–267 GB |
| [CacheTTL](https://arxiv.org/html/2511.02230v4) | 伯克利 | 显存层的 TTL 取使复用概率乘收益减占用成本最大的值，与本报告的盈亏平衡式同构 |
| [Tutti](https://arxiv.org/html/2605.03375) | 厦大等 | 云上 SSD 比 DRAM 便宜约 100 倍；约 2 TB DRAM 只能留约 5 分钟，100 TB 以上 NVMe 可留一小时以上 |
| [LMCache](https://arxiv.org/pdf/2510.09665) | Tensormesh + 芝加哥大学 | 主张 KV 持久化到查询生命周期之外，未给出 TTL |
| [KV Cache 管理综述](https://arxiv.org/html/2607.02574v1) | arXiv 2607.02574 | 没有系统形式化按会话的留存契约 |

仅读过摘要或摘录的相关工作：[CachedAttention](https://arxiv.org/abs/2403.19708)、[Mooncake](https://www.usenix.org/conference/fast25/presentation/qin)、[CacheGen](https://arxiv.org/pdf/2310.07240)、[Compute or Load](https://arxiv.org/html/2410.03065)、[To Restore or To Recompute](https://doi.org/10.1145/3837053.3837336)、[NVIDIA 上下文内存存储平台技术博客](https://developer.nvidia.com/blog/introducing-nvidia-bluefield-4-powered-inference-context-memory-storage-platform-for-the-next-frontier-of-ai)。后者把推理上下文定性为可重算数据，同时主张 agent 的上下文要跨更长会话保留，未给出天数。

## 主要不确定性

结论的方向可靠，具体数值依赖以下未经官方确认的输入。硬件成本分析另外依赖未公开的 GPU 型号、盘型和采购价，以及机箱成本、每 TB 功耗和利用率系数等假设；基准 GPU 与成本口径的 GPU 型号不一致。

- **毛利率口径。** 82.9% 对应 1–7 月的旧模型和旧定价，来自媒体报道。DeepInfer 发布后的第三方估算从 77% 到 96% 不等。
- **每卡收入。** $38 是按谷价和吞吐推出的理论收入，不是审计后的实收。
- **每 token KV 字节数。** 0.9 KB 由 112 KiB 块大小换算，前提是一块对应一个 128 token 页，官方未给出压缩比。
- **prefill 吞吐。** 是 131K 上下文下的基准值，短上下文和线上实际利用率会不同。
- **命中率。** 官方未公布，75%–90% 是由容量反推的区间，前提是存储接近写满。
- **命中率–TTL 曲线。** 公开数据只有 chat 3 天对 7 天这一个点，5 分钟、1 小时、24 小时各档的增量需要自有流量数据或假设。
- **其他厂商的实现。** Anthropic 未公开缓存介质，合规因素对短 TTL 的影响为推断。

## 来源

- [DeepInfer 官方部署 Q&A（GitHub）](https://github.com/deepseek-ai/DeepInfer/blob/main/doc/inference_deployment_qa.md)
- [InfoQ：DeepSeek 开源最新推理框架](https://infoq.cn/article/NBXP966Bz3eqr6Qidvaf?amp=)
- [OpenAI Prompt Caching 文档](https://developers.openai.com/api/docs/guides/prompt-caching)
- [Developers Digest：Prompt Caching Across AI Providers](https://www.developersdigest.tech/blog/prompt-caching-across-ai-providers-2026)
- [The $38 GPU-Hour（Substack）](https://vkambati.substack.com/p/the-38-gpu-hour)
- [DEV：Is Self-Hosting V4.1 Flash Cheaper Than Paying per Token?](https://dev.to/tokely/deepseek-open-sourced-the-engine-behind-its-api-is-self-hosting-v41-flash-cheaper-than-paying-per-3ki7)

以下来源仅取自搜索结果摘要，未打开全文核对：

- [DeepSeek API 文档：Context Caching](https://api-docs.deepseek.com/guides/kv_cache)
- [Anthropic Prompt Caching 文档](https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching)
- [Gemini API Context caching 文档](https://ai.google.dev/gemini-api/docs/generate-content/caching)
- [火山方舟：上下文缓存](https://docs.volcengine.com/docs/ark/context-api?lang=zh)
- [阿里云百炼：上下文缓存](https://help.aliyun.com/model-studio/context-cache)
- [Kimi Code issue：prompt_cache_options.ttl](https://github.com/MoonshotAI/kimi-code/issues/3942)
- [实测 11 个端点的缓存 TTL（gist）](https://gist.github.com/monperrus/9784f52d5144975fd11be7f642c4a8fc)
- [DeepSeek Revenue Surges 10x as API Gross Margin Hits 82.9%](https://deepthink.ltd/blog/deepseek-revenue-surge-82-9-api-gross-margin-ipo-2026/)
- [第一财经：一张卡每小时收入 38 美元](https://www.yicai.com/news/103401110.html)

硬件成本与分层分析的来源：

- [DeepInfer 仓库首页（离线基准）](https://github.com/deepseek-ai/DeepInfer)
- [StorageReview：企业 SSD 与 HDD 价格（VDURA 指数）](https://www.storagereview.com/news/enterprise-ssd-prices-run-at-6-5x-last-year-vdura-pegs-a-30tb-tlc-drive-at-22600)
- [Growth Market Reports：数据中心 SSD 市场与价格](https://growthmarketreports.com/report/data-center-solid-state-drive-market-global-industry-analysis)
- [30TB SSD 选购指南（TLC/QLC 寿命指标）](https://electronics.alibaba.com/buyingguides/30tb-ssd-guide-when-to-buy,-what-to-skip)
- [DatacenterDisk：SSD/HDD 价格分化报告](https://datacenterdisk.com/reports/ssd-hdd-price-divergence-2026)
- [SemiAnalysis InferenceX：B200](https://inferencex.semianalysis.com/chips/b200)

价格类来源均取自搜索结果摘要，未打开全文核对。
