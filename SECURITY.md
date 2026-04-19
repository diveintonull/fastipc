# 安全策略

FastIPC 是工程化衍生项目，目前没有生产支持的 release line 或 security-response SLA。

发现疑似 vulnerability 时，如仓库支持，请通过 GitHub Security Advisory / private vulnerability-reporting interface 私下报告。若该接口不可用，请先通过私密渠道联系当前仓库 owner，再公开 exploit detail。

除非问题能在未修改上游代码中独立复现，否则不要把 FastIPC 报告发送给上游 `libsharedmemory` maintainer。FastIPC 重写后的 layout、transport、recovery logic 与 API 由本项目维护。

报告请包含：

- 受影响 revision 与 Linux/kernel detail；
- transport 与 configuration；
- minimal reproduction；
- expected/observed behavior；
- sanitizer 或 crash output；
- untrusted local user 是否能访问 channel namespace。
