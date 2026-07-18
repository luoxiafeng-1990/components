# 远程 VPN 不通根因与修复（2026-07-18）

## 现象

家宽（`192.168.1.81`）下板端全 ping 不通；此前全功能 skill 可连板。

## 根因（Agent 侧）

1. **误删 VPN 路由**：`start_stress30h_soak.sh` 无条件 `ip route del 192.168.56.0/23`。远程场景该路由是唯一通路，删了即全挂。
2. **`ensure_lan_route()` 被改成空 `return`**，且未在不通时拉起固化 VPN。
3. **重复启动 openvpn** 留下多 tun / 错误 `ip rule`，干扰转发。
4. **soak launch 用 `pkill -f qa_cases`**：远程 SSH 命令行自身含该子串，会话被自杀 → 全部 `launch fail`。

## 正确做法（已写入 skill）

| 场景 | 动作 |
|---|---|
| 远程（无 56/57 本机地址） | `nohup sudo bash /home/ubuntu/openvpn-2.7.2/start_vpn.sh >/tmp/start_vpn.log 2>&1 &`，**保留** `192.168.56.0/23 via tun*` |
| 办公室 LAN | 才删除 tun 劫持路由 |

## 本轮重跑

- 结果目录：`/home/ubuntu/test/qa_cases/component_bitable_stress30h_rerun_20260718_160615`
- duration：108000s，sync：300s

## 正确启 VPN（本机）

```bash
sudo systemctl restart openvpn-client.service
# ExecStart=/home/ubuntu/openvpn-2.7.2/start_vpn.sh
```

禁止再 `nohup start_vpn.sh`（与 systemd 叠成双 tun）。

## 本轮成功重跑

- 目录：`/home/ubuntu/test/qa_cases/component_bitable_stress30h_rerun_20260718_161302`
- 14/14 launch OK，duration=108000s，sync=300s
