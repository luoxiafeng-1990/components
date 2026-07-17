# QingTian 本地补丁说明（3.6.8）

## 已修复

### 1. Cursor 重启黑屏
根因：`seamlessSwitch.injectWorkbench()` 会优先使用过期的  
`workbench.desktop.main.js.qingtian.backup` 覆盖新版 Cursor 的 workbench。

修复：
- 永远以当前安装目录的 workbench 为底座
- 自动隔离体积/时间戳不兼容的旧备份
- Auth hook 兼容新版压缩参数名（`e`/`n`/…）
- `restoreWorkbench` 拒绝回灌跨版本旧备份

### 2. 授权模式改为「首月免费 + 加密货币续费」
- 首次启动自动开通本地试用（默认 30 天），无需激活码
- 到期后侧栏展示收款二维码/地址/金额
- 用户转账后输入「续费口令」完成续期

## 需要你配置的设置

在 Cursor Settings 搜索 `qingtian`：

| 配置项 | 含义 | 示例 |
|--------|------|------|
| `qingtian.paymentAddress` | 收款地址 | `TXxxxx...` |
| `qingtian.paymentNetwork` | 网络 | `USDT-TRC20` |
| `qingtian.paymentAmount` | 展示金额 | `99` |
| `qingtian.paymentQrUrl` | 自定义二维码图（可选） | `https://.../qr.png` |
| `qingtian.renewalPassword` | 续费口令 | 运营私下发给已付款用户 |
| `qingtian.trialDays` | 试用天数 | `30` |
| `qingtian.paidDays` | 每次续费天数 | `30` |

## 安装补丁到本机已安装扩展

```bash
EXT=~/.cursor/extensions/qingtian.qingtian-v2-3.6.8
cp -a tools/qingtian-patch/seamlessSwitch.js "$EXT/out/"
cp -a tools/qingtian-patch/activation.js "$EXT/out/"
cp -a tools/qingtian-patch/webviewProvider.js "$EXT/out/"
# package.json 可选：用于设置面板出现新配置项
cp -a tools/qingtian-patch/package.json "$EXT/"

# 隔离危险旧备份（防黑屏）
WB=/usr/share/cursor/resources/app/out/vs/workbench/workbench.desktop.main.js
if [ -f "$WB.qingtian.backup" ]; then
  sudo mv "$WB.qingtian.backup" "$WB.qingtian.backup.stale.$(date +%s)"
fi
```

然后 **Reload Window** 或重启 Cursor。

## 注意

- 无感切号（Workbench 注入）在新版 Cursor 上仍有兼容风险；日常 MCP 对话可不开启注入。
- 续费目前是「口令确认」模型，不是链上自动验单；口令请勿写进公开文档。
