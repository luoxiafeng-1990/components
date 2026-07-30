# SlashSubs 3.7.8

Cursor 侧栏 MCP 持续对话插件（试用 + USDT 续费）。

## 本版重点（3.7.8）
1. **冻结 mcp.json**：激活/切工作区默认不再改写配置，避免 Cursor `config_changed` 主动掐断 MCP 连接  
2. 仅在「加/减通道」或你主动点「刷新配置（慎用）」时才改写 mcp.json  
3. stamp/路径不一致不再误判离线，减少被迫「刷新配置」  
4. 保留同会话「停止当前」与队列路径对齐能力（不新开场、不耗新额度）

## 安装
卸载旧版 → 安装 `slashsubs-3.7.8.vsix` → Reload Window。  
正常使用请勿点「刷新配置」。

## 仓库
https://github.com/luoxiafeng-1990/slashsubs-plugin
