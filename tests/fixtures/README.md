# 自动更新（M6）验收 fixture

`release_latest_20261114T143647Z.json` 是 **GitHub Releases API 的真实响应体**，原样保存，未做任何改写。

| 项 | 值 |
|---|---|
| 请求 URL | `https://api.github.com/repos/LinYuwei82/dev_card_cpp/releases/latest` |
| 请求头 | `Accept: application/vnd.github+json` |
| 抓取时间 | 2026-11-14T14:36:47Z（UTC） |
| HTTP 状态 | 200 |
| 响应体字节数 | 8,198 |
| 响应体 SHA256 | `c05aedbe8acebdc2a0e76965cfe55ff947de4f18e7d0c0d04df25f3c57ff2a93` |
| 关键字段 | `tag_name=v1.1.3`、`html_url=…/releases/tag/v1.1.3` |
| 响应头 | 同目录 `release_latest_20261114T143647Z.headers.txt` |

复现抓取（同一 URL、同一请求头，内容是动态的，重抓会得到不同 SHA256）：

```bash
curl -sS -D headers.txt -o body.json \
  -H "Accept: application/vnd.github+json" \
  https://api.github.com/repos/LinYuwei82/dev_card_cpp/releases/latest
```

## 回放怎么用

```bash
cmake --build build-new
cd build-new
HOME=/tmp/updcheck \
DEVCARD_SELFTEST_UPDATE=replay \
DEVCARD_UPDATE_FIXTURE=$PWD/../tests/fixtures/release_latest_20261114T143647Z.json \
./shell/DevCard.app/Contents/MacOS/DevCard
```

输出：

- `/tmp/m6_update_replay.json`：解析结果（当前版本、最新版本、下载页、提示文案）
- `/tmp/m6_update_ui.png`：WKWebView 自带的 `takeSnapshotWithConfiguration` 快照

## 这条证据到底是什么，不是什么

- **是**：真实响应体 → 页面上同一条 `parseRelease` / `compareVersion` / `setUpdatePill` 链路 → 界面提示。
  传输路径换成本地回放，响应体一个字节没改。
- **不是**：真机联网验证。它证明的是「解析与提示链路对真实数据成立」，不单独证明网络可达。
  网络可达由另一条证据负责：`DEVCARD_SELFTEST_UPDATE=live` 真打 GitHub 接口，结果与快照
  曾落在 `docs/assets/m6_update_live.json`、`docs/assets/m6_update_live.png`（这两个文件已删除，
  仅存于 Git 历史，删除提交 `35cf20f`）。

## 联网 `Load failed` 的根因（2026-11-14）

不是 DNS、TLS、代理或沙箱，是**本程序自己发的 CSP**：`/` 的响应头
`default-src 'self'; img-src 'self' data:; style-src 'self'` 没有 `connect-src`，
跨域 `fetch` 被 `default-src` 兜住，WebKit 只报 `TypeError: Load failed`。

同机同 URL、只换页面来源的对照（用 `WKWebView` 加载页面后 `fetch` 同一接口）：

| 页面来源 | 结果 |
|---|---|
| `https://example.com`（无 CSP） | `HTTP 200 len=15` |
| 本地 `python3 -m http.server`（无 CSP） | `HTTP 200 len=15` |
| 本程序 `http://127.0.0.1:<port>`（有 CSP） | `TypeError: Load failed` |

修法：CSP 增加 `connect-src 'self' https: http:`（详见 `docs/改造技术方案.md` §10.4.2）。
