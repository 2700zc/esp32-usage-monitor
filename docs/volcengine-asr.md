# 火山引擎语音识别接入指南

这份文档说明如何给 `tools/agent-bridge/bridge.py` 配置火山引擎（豆包语音）的录音文件识别，以及设备端 BOOT 语音输入从录音到出字的完整链路。

> 不需要把任何密钥写进固件或 `data/config.json`。API Key 只保存在 PC 上，设备只负责录音和上传 PCM。

设备端只有 **BOOT** 参与语音：短按一下开始，再短按一次结束；空闲时长按 0.9 秒
会让桥接把刚才粘贴的文字逐字回删（`GET /clear`，只对同一前台窗口、2 分钟内的
最近一次粘贴生效）。PWR / IO18 的完整功能见
[主 README 的「按键功能」](../README.md#按键功能)。

---

## 1. 先确认你要用哪个服务

火山引擎的录音文件识别有两套常用接口，资源 ID 不同，桥接会自动选择：

| 服务 | 资源 ID | 接口 | 特点 |
|---|---|---|---|
| 录音文件识别标准版 2.0 | `volc.seedasr.auc` | `submit` + `query`（异步） | **默认**；短音频通常 1~3 秒，语言自动识别 |
| 录音文件识别标准版 1.0 | `volc.bigasr.auc` | `submit` + `query`（异步） | 老账号；同样异步 |
| 录音文件识别极速版 | `volc.bigasr.auc_turbo` | `recognize/flash`（同步） | 延迟最低；需要单独开通 |

桥接的判断规则：

- 资源 ID 是 `volc.seedasr.auc` 或 `volc.bigasr.auc` → 走标准版提交/查询；
- 其他资源 ID（例如 `volc.bigasr.auc_turbo`）→ 走极速版 flash。

如果你不确定账号开通了哪个，先看控制台的「服务管理 / 已开通服务」，或者直接在桥接里逐个试（见第 6 节）。

---

## 2. 开通服务

1. 打开火山引擎语音技术控制台：

   <https://console.volcengine.com/speech/>

2. 完成实名认证（未认证通常不能开通）。
3. 在「服务管理 / 语音识别」里找到 **录音文件识别**：
   - 新账号优先开通 **标准版 2.0**（资源 ID `volc.seedasr.auc`）；
   - 如果控制台只提供极速版，就开通 **极速版**（`volc.bigasr.auc_turbo`）。
4. 确认账号有免费额度、资源包或余额，否则调用会返回 403/欠费类错误。

> 控制台改版后菜单名可能略有不同。认准两点：产品是「语音技术 / 语音识别」，服务是「录音文件识别」，不要开成「流式语音识别」（那是 `volc.*.sauc.*` 资源，接口不一样）。

---

## 3. 获取 API Key

### 新版控制台（推荐）

在语音技术控制台里找 **API Key 管理**，创建一个 API Key，复制整串值。

- 只需要这一串，不需要 AppID/Access Token。
- 不要带 `Bearer ` 前缀，直接一行。
- 如果创建 Key 时可以选权限范围，确保它能访问语音识别服务。

### 旧版控制台

在「应用管理」里创建应用，记下 **APP ID** 和 **Access Token**。这两个不要放进 `volc.key`，而是用桥接的启动参数：

```text
--volc-app-key 你的APPID --volc-access-key 你的AccessToken
```

旧版方式改完需要重启桥接。

---

## 4. 写到本地

在 `tools/agent-bridge/` 下创建 `volc.key`，一行放 API Key：

```bash
# 从示例复制，再把第一行替换成真实 Key
cp tools/agent-bridge/volc.key.example tools/agent-bridge/volc.key

# 或者直接写
printf '你的火山APIKey\n' > tools/agent-bridge/volc.key
```

`volc.key` 已在 `.gitignore` 里，不会被提交。

桥接每次识别都会重新读一次这个文件，所以：

- **只换 Key**：不用重启桥接；
- **改资源 ID、语言或旧版鉴权参数**：需要重启桥接。

可选的环境变量：

```text
VOLC_API_KEY        API Key（等价于 volc.key）
VOLC_RESOURCE_ID    资源 ID，默认 volc.seedasr.auc
VOLC_LANGUAGE       极速版语言，默认 zh-CN（标准版自动识别）
VOLC_APP_KEY        旧版 APP ID
VOLC_ACCESS_KEY     旧版 Access Token
```

---

## 5. 请求细节

### 5.1 标准版 2.0（默认）

提交任务：

```text
POST https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit
Content-Type: application/json
X-Api-Key: <API Key>
X-Api-Resource-Id: volc.seedasr.auc
X-Api-Request-Id: <随机 UUID，提交和查询共用>
X-Api-Sequence: -1
```

请求体：

```json
{
  "audio": {
    "data": "<WAV 文件的 base64>",
    "format": "wav",
    "codec": "raw",
    "rate": 16000,
    "bits": 16,
    "channel": 1
  },
  "request": {
    "model_name": "bigmodel",
    "enable_itn": true,
    "enable_punc": true,
    "show_utterances": false
  }
}
```

提交成功后，查询结果：

```text
POST https://openspeech.bytedance.com/api/v3/auc/bigmodel/query
Content-Type: application/json
X-Api-Key: <API Key>
X-Api-Resource-Id: volc.seedasr.auc
X-Api-Request-Id: <提交时用的同一个 UUID>
X-Api-Sequence: -1
```

查询体为空 JSON：`{}`。

结果在 `result.text`：

```json
{
  "audio_info": {"duration": 6920},
  "result": {
    "additions": {"duration": "6920"},
    "text": "你好，这是一次标准版语音识别测试。今天天气不错。"
  }
}
```

桥接会每 0.5 秒查询一次，最多等 25 秒。

> 标准版文档示例只写了 `audio.url`，但实测 `audio.data` base64 也能用（本项目就是用它）。如果以后接口收紧，再改成先上传文件、传 URL 的方式。

### 5.2 极速版 flash

```text
POST https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash
X-Api-Key: <API Key>
X-Api-Resource-Id: volc.bigasr.auc_turbo
X-Api-Request-Id: <随机 UUID>
X-Api-Sequence: -1
```

请求体与标准版类似，但 `audio.language` 可以指定语言；一次请求直接返回 `result.text`。

---

## 6. 自测

### 只看识别，不粘贴

```powershell
# 先停掉常驻桥接
$p=(Get-NetTCPConnection -LocalPort 8787 -State Listen).OwningProcess
Stop-Process -Id $p -Force

cd D:\esp32\esp32-usage-monitor\tools\agent-bridge
python bridge.py --asr-dry-run
```

然后按设备 BOOT 说一句话，再按一次 BOOT。桥接窗口会打印：

```text
asr: ... pcm 16000Hz/1ch -> volcengine
asr: submitted task ......
asr dry-run: 23 chars: '你好……'
```

测试完恢复常驻：

```powershell
wscript D:\esp32\esp32-usage-monitor\tools\agent-bridge\bridge-hidden.vbs
```

### 不接火山，只测设备链路

```powershell
python bridge.py --asr-mock-text "语音测试成功" --asr-dry-run
```

### 保存最近一次录音

```powershell
python bridge.py --asr-dry-run --asr-save-wav D:\esp32\asr-test.wav
```

录完可以用播放器听一下，确认麦克风收声正常。

### 直接调用 `/asr` 接口

```bash
curl -X POST "http://127.0.0.1:8787/asr?tok=<act_token>" \
  -H "Content-Type: application/octet-stream" \
  -H "X-Sample-Rate: 16000" \
  -H "X-Channels: 1" \
  -H "X-Bits: 16" \
  --data-binary @pcm.raw
```

返回：

```json
{"ok": true, "text": "识别结果"}
```

---

## 7. 常见错误

| 现象 | 含义 / 处理 |
|---|---|
| `401` | API Key 无效、过期，或不是语音控制台的 Key |
| `403` / `45000030 requested resource not granted` | Key 有效，但账号没有开通对应资源；确认开的是「录音文件识别」并且资源 ID 匹配 |
| `api=45000001` | 参数/资源不匹配，或服务未开通 |
| `api=45000002` | 空音频；录得太短或麦克风没收到声音 |
| `api=45000151` | 音频格式不符合要求；本项目固定 16 kHz / 16 bit / 单声道 |
| `api=55000031` | 服务繁忙；稍后重试或购买并发 |
| 设备显示「火山未授权 45000030…」 | 去语音技术控制台开通标准版 2.0（`volc.seedasr.auc`）或极速版（`volc.bigasr.auc_turbo`） |
| 设备显示「没有识别到文字」 | 录到的是静音，或火山返回 `20000003`（任务完成但无有效语音） |
| 文字没进输入框 | 按 BOOT 前光标不在输入框；桥接日志会打印实际前台程序名 |

标准版查询状态码：

- `20000000`：成功，读取 `result.text`；
- `20000001` / `20000002`：处理中 / 排队中，继续轮询；
- `20000003`：任务完成，但没有检测到有效语音，结果为空。

`X-Tt-Logid` / `X-Api-Request-Id` 是找火山技术支持时最有用的两个值，桥接日志里会保留原始错误体。

---

## 8. 实测参考

- 标准版 2.0，6.9 秒中文 TTS：提交后约 **1.7 秒**返回正确文字。
- 设备录音 9.2 秒但环境静音：火山返回 `20000003` + 空文本，桥接按「没有识别到文字」处理。
- 60 秒长录音标准版可能更慢；桥接最多等 25 秒，超时会返回「火山识别超时」。

---

## 9. 安全注意

- API Key 只放在 PC 的 `tools/agent-bridge/volc.key`，不要写进固件、`config.json` 或 Git。
- `volc.key` 已在 `.gitignore`；如果你把项目复制到别处，记得不要把该文件一起提交。
- 设备到桥接的 `/asr` 是局域网 HTTP，只有 `act_token` 校验，没有加密。不要在不可信网络里长期开放。
- 如果 Key 曾经出现在聊天记录、截图或公开仓库里，建议在火山控制台重置/轮换。
