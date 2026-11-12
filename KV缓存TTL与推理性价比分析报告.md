# KV 缓存 TTL 与推理系统性价比分析报告

2026-11-12

## 核心结论

DeepInfer 的 agent 3 天、chat 7 天 TTL 是成本最优解，不是慷慨。每 token 的 KV 被压到约 0.9 KB 后，SSD 留存一天只需约 $0.00024/百万 token。

- **别家 TTL 短的原因**：KV 放在显存或单机本地存储，介质贵且依赖路由亲和；同时采用“付费写入加承诺时长”的产品契约。
- **长 TTL 的边际收益很小**：chat 从 7 天缩到 3 天，命中率只掉 0.5–1 个点。
- **7 天大致落在盈亏平衡点**：按 82.9% 毛利率反推成本后，chat 命中率在 80% 左右时，多留 4 天基本打平到略赚。
- **分析性价比应看命中率–TTL 曲线**，TTL 本身是结果变量。

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

## 主要不确定性

结论的方向可靠，具体数值依赖以下未经官方确认的输入。

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
