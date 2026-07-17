# COMPONENT_BITABLE_RUN_SUMMARY — QA_ENCODE

- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717`
- 执行用例: 381（飞书 QA_ENCODE 可执行集；另 45 条标不可执行未入队）
- 最终: PASS=290 / FAIL=91
- Flaky(retested_pass): 63 条（见下表）
- 主机: 空闲探测 SSH 可达 12 台并行（排除 192.168.56.214 SSH reset）
- Procedure 特征: `qa_cases venc` / ENC_COMPARE（确认为编码测试集）
- P5: 91 条 FAIL 已逐条读 log；见 `LOG_REVIEW_REPORT.md`

## FAIL 归因汇总

| FAIL_REASON | 数量 | 说明 |
|-------------|------|------|
| case_fail | 50 | 编码质量/管线失败（PSNR·SSIM 不达标、compared=0、或 Expectation=FAIL 却意外 PASS）: TC-1599, TC-1633, TC-1634, TC-1639, TC-1643, TC-1644, TC-1779, TC-2200, TC-2204, TC-2207, TC-2221, TC-2232 … 共50条 |
| hardware_limit | 36 | 硬件限制（mmap DMA-BUF / 8192 超大分辨率 / 高并行通道）: TC-1626, TC-1841, TC-2230, TC-2231, TC-2256, TC-2257, TC-2258, TC-2259, TC-2260, TC-2261, TC-2262, TC-2263 … 共36条 |
| procedure_error | 5 | 飞书 Procedure 输入路径错误（把用例名当文件路径）: TC-2873, TC-2874, TC-2875, TC-2948, TC-2949 |

### case_fail 细分

| 子类 | 数量 | TC-ID |
|------|------|-------|
| psnr_ssim_below_threshold | 39 | 管线跑通但 PSNR/SSIM 低于阈值 — TC-1599, TC-1633, TC-1634, TC-1639, TC-1643, TC-1644, TC-1779, TC-2200, TC-2204, TC-2207, TC-2221, TC-2247, TC-2248, TC-2266, TC-2267, TC-2268, TC-2269, TC-2272, TC-2275, TC-2286, TC-2288, TC-2298, TC-2299, TC-2300, TC-2301, TC-2302, TC-2304, TC-2305, TC-2306, TC-2307, TC-2308, TC-2431, TC-2432, TC-2437, TC-2438, TC-2850, TC-2851, TC-2852, TC-2853 |
| compared_zero_pipeline | 6 | compared=0 未产出可比较帧 — TC-2232, TC-2271, TC-2854, TC-2856, TC-2946, TC-2947 |
| unexpected_pass | 5 | Expectation=FAIL 但实际 PASS（意外通过） — TC-2928, TC-2950, TC-2952, TC-2955, TC-2956 |

## Flaky 双套日志对比（P5.2bis）

共 63 条：第 1 轮 FAIL，后续轮次转 PASS。共性归因：轮间 reboot + 板端残留清理后 VPU/Buffer 状态恢复，或瞬时质量抖动。

| TC-ID | 尝试次数 | 转 PASS 轮次 | 状态反转物理归因 |
|-------|----------|-------------|------------------|
| TC-1597 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1598 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1605 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1615 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1616 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1620 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1630 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1635 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1636 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1637 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1640 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1641 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1646 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1649 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1650 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1659 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1660 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1661 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1663 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1669 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1673 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1674 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1745 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1763 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1781 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1804 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-1840 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2201 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2209 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2211 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2212 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2213 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2214 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2220 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2223 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2233 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2243 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2255 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2270 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2273 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2274 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2278 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2427 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2861 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2862 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2866 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2867 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2869 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2883 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2887 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2890 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2894 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2895 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2896 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2906 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2907 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2910 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2913 | 4 | R4 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2936 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2953 | 2 | R2 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2958 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2961 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |
| TC-2964 | 3 | R3 | 轮间 reboot / clean_board_residuals 后重跑通过 |

## 逐条结果（FINAL_VERDICT）

| TC-ID | FINAL | EXIT | HOST | EXPECTATION_MATCH | FAIL_REASON |
|-------|-------|------|------|-------------------|-------------|
| TC-1597 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1598 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1599 | FAIL | 1 | 192.168.56.92 | NO | case_fail |
| TC-1600 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1603 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1604 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1605 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1607 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1608 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1609 | PASS | 0 | 192.168.56.86 | YES | - |
| TC-1610 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1611 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1614 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1615 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1616 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1618 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1619 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1620 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1624 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1625 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1626 | FAIL | 1 | 192.168.56.56 | NO | hardware_limit |
| TC-1627 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1629 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1630 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1631 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1633 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-1634 | FAIL | 1 | 192.168.56.93 | NO | case_fail |
| TC-1635 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1636 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1637 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1638 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1639 | FAIL | 1 | 192.168.56.140 | NO | case_fail |
| TC-1640 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1641 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1642 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1643 | FAIL | 1 | 192.168.56.132 | NO | case_fail |
| TC-1644 | FAIL | 1 | 192.168.57.43 | NO | case_fail |
| TC-1645 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1646 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1647 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1648 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1649 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1650 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1651 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1652 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1653 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1654 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1655 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1656 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1657 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1658 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1659 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1660 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1661 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1662 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1663 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1664 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1665 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1666 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1667 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1668 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1669 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1670 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1671 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1672 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1673 | PASS | 0 | 192.168.56.86 | YES | - |
| TC-1674 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1675 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1676 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1677 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1678 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1679 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1680 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1681 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1682 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1743 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1744 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1745 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1746 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1749 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1750 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1751 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1752 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1755 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1756 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1757 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1758 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1761 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1762 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1763 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1764 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1767 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1768 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1769 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1770 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1773 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1774 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1775 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1776 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1779 | FAIL | 1 | 192.168.56.145 | NO | case_fail |
| TC-1780 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1781 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1782 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1785 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1786 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1787 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1788 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1791 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1792 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1793 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1794 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1797 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1798 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1799 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-1800 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1803 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1804 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1805 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-1806 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1821 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-1822 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-1823 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-1824 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-1827 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-1828 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-1829 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-1830 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-1839 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-1840 | PASS | 0 | 192.168.56.86 | YES | - |
| TC-1841 | FAIL | 1 | 192.168.57.113 | NO | hardware_limit |
| TC-2200 | FAIL | 1 | 192.168.56.39 | NO | case_fail |
| TC-2201 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2202 | PASS | 1 | 192.168.56.140 | YES | - |
| TC-2203 | PASS | 1 | 192.168.56.132 | YES | - |
| TC-2204 | FAIL | 1 | 192.168.56.92 | NO | case_fail |
| TC-2205 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2206 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2207 | FAIL | 1 | 192.168.56.145 | NO | case_fail |
| TC-2208 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2209 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2211 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2212 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2213 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2214 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2215 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2216 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2219 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2220 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2221 | FAIL | 1 | 192.168.56.140 | NO | case_fail |
| TC-2222 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2223 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2224 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2225 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2226 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2227 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2228 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2229 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2230 | FAIL | 1 | 192.168.56.39 | NO | hardware_limit |
| TC-2231 | FAIL | 1 | 192.168.57.43 | NO | hardware_limit |
| TC-2232 | FAIL | 1 | 192.168.56.132 | NO | case_fail |
| TC-2233 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2234 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2235 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2236 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2237 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2238 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2239 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2240 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2241 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2242 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2243 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2244 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2245 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2246 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2247 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-2248 | FAIL | 1 | 192.168.56.92 | NO | case_fail |
| TC-2249 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2250 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2251 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2252 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2253 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2254 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2255 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2256 | FAIL | 1 | 192.168.56.56 | NO | hardware_limit |
| TC-2257 | FAIL | 1 | 192.168.56.145 | NO | hardware_limit |
| TC-2258 | FAIL | 1 | 192.168.56.93 | NO | hardware_limit |
| TC-2259 | FAIL | 1 | 192.168.56.140 | NO | hardware_limit |
| TC-2260 | FAIL | 1 | 192.168.56.39 | NO | hardware_limit |
| TC-2261 | FAIL | 1 | 192.168.57.43 | NO | hardware_limit |
| TC-2262 | FAIL | 1 | 192.168.56.133 | NO | hardware_limit |
| TC-2263 | FAIL | 1 | 192.168.56.56 | NO | hardware_limit |
| TC-2264 | FAIL | 1 | 192.168.56.92 | NO | hardware_limit |
| TC-2265 | FAIL | 1 | 192.168.56.145 | NO | hardware_limit |
| TC-2266 | FAIL | 1 | 192.168.56.93 | NO | case_fail |
| TC-2267 | FAIL | 1 | 192.168.56.132 | NO | case_fail |
| TC-2268 | FAIL | 1 | 192.168.56.140 | NO | case_fail |
| TC-2269 | FAIL | 1 | 192.168.56.39 | NO | case_fail |
| TC-2270 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2271 | FAIL | 1 | 192.168.57.43 | NO | case_fail |
| TC-2272 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-2273 | PASS | 0 | 192.168.56.86 | YES | - |
| TC-2274 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2275 | FAIL | 1 | 192.168.56.92 | NO | case_fail |
| TC-2276 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2277 | FAIL | 1 | 192.168.56.145 | NO | hardware_limit |
| TC-2278 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2279 | FAIL | 1 | 192.168.56.93 | NO | hardware_limit |
| TC-2280 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2281 | FAIL | 1 | 192.168.56.132 | NO | hardware_limit |
| TC-2282 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2283 | FAIL | 1 | 192.168.56.140 | NO | hardware_limit |
| TC-2284 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2285 | FAIL | 1 | 192.168.57.43 | NO | hardware_limit |
| TC-2286 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-2287 | FAIL | 1 | 192.168.56.86 | NO | hardware_limit |
| TC-2288 | FAIL | 1 | 192.168.56.39 | NO | case_fail |
| TC-2289 | FAIL | 1 | 192.168.56.145 | NO | hardware_limit |
| TC-2290 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2291 | FAIL | 1 | 192.168.56.56 | NO | hardware_limit |
| TC-2292 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2293 | FAIL | 1 | 192.168.56.132 | NO | hardware_limit |
| TC-2294 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2295 | FAIL | 1 | 192.168.56.140 | NO | hardware_limit |
| TC-2296 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2297 | FAIL | 1 | 192.168.56.92 | NO | hardware_limit |
| TC-2298 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-2299 | FAIL | 1 | 192.168.56.132 | NO | case_fail |
| TC-2300 | FAIL | 1 | 192.168.56.56 | NO | case_fail |
| TC-2301 | FAIL | 1 | 192.168.56.39 | NO | case_fail |
| TC-2302 | FAIL | 1 | 192.168.56.86 | NO | case_fail |
| TC-2303 | FAIL | 1 | 192.168.57.43 | NO | hardware_limit |
| TC-2304 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-2305 | FAIL | 1 | 192.168.56.145 | NO | case_fail |
| TC-2306 | FAIL | 1 | 192.168.56.92 | NO | case_fail |
| TC-2307 | FAIL | 1 | 192.168.57.113 | NO | case_fail |
| TC-2308 | FAIL | 1 | 192.168.56.140 | NO | case_fail |
| TC-2309 | FAIL | 1 | 192.168.56.132 | NO | hardware_limit |
| TC-2310 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2311 | FAIL | 1 | 192.168.56.39 | NO | hardware_limit |
| TC-2312 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2313 | FAIL | 1 | 192.168.56.56 | NO | hardware_limit |
| TC-2314 | FAIL | 1 | 192.168.56.86 | NO | hardware_limit |
| TC-2315 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2316 | FAIL | 1 | 192.168.56.133 | NO | hardware_limit |
| TC-2426 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2427 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2428 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2429 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2430 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2431 | FAIL | 1 | 192.168.56.145 | NO | case_fail |
| TC-2432 | FAIL | 1 | 192.168.56.92 | NO | case_fail |
| TC-2433 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2434 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2435 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2436 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2437 | FAIL | 1 | 192.168.56.140 | NO | case_fail |
| TC-2438 | FAIL | 1 | 192.168.57.113 | NO | case_fail |
| TC-2439 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2440 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2441 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2442 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2845 | FAIL | 1 | 192.168.56.86 | NO | hardware_limit |
| TC-2846 | FAIL | 1 | 192.168.56.133 | NO | hardware_limit |
| TC-2847 | FAIL | 1 | 192.168.56.145 | NO | hardware_limit |
| TC-2848 | FAIL | 1 | 192.168.56.93 | NO | hardware_limit |
| TC-2849 | FAIL | 1 | 192.168.56.92 | NO | hardware_limit |
| TC-2850 | FAIL | 1 | 192.168.56.140 | NO | case_fail |
| TC-2851 | FAIL | 1 | 192.168.57.113 | NO | case_fail |
| TC-2852 | FAIL | 1 | 192.168.57.43 | NO | case_fail |
| TC-2853 | FAIL | 1 | 192.168.56.86 | NO | case_fail |
| TC-2854 | FAIL | 1 | 192.168.56.133 | NO | case_fail |
| TC-2855 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2856 | FAIL | 1 | 192.168.56.145 | NO | case_fail |
| TC-2857 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2858 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2859 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2860 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2861 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2862 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2863 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2864 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2865 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2866 | PASS | 0 | 192.168.56.86 | YES | - |
| TC-2867 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2868 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2869 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2870 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2871 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2872 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2873 | FAIL | 1 | 192.168.56.93 | NO | procedure_error |
| TC-2874 | FAIL | 1 | 192.168.56.132 | NO | procedure_error |
| TC-2875 | FAIL | 1 | 192.168.56.92 | NO | procedure_error |
| TC-2876 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2877 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2878 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2879 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2880 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2881 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2882 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2883 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2884 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2885 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2886 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2887 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2888 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2889 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2890 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2891 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2892 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2893 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2894 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2895 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2896 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2897 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2898 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2899 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2900 | PASS | 0 | 192.168.56.39 | YES | - |
| TC-2901 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2902 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2903 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2904 | PASS | 0 | 192.168.56.140 | YES | - |
| TC-2905 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2906 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2907 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2908 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2909 | PASS | 0 | 192.168.57.43 | YES | - |
| TC-2910 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2911 | PASS | 0 | 192.168.56.93 | YES | - |
| TC-2912 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2913 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2914 | PASS | 0 | 192.168.56.92 | YES | - |
| TC-2915 | PASS | 1 | 192.168.56.56 | YES | - |
| TC-2916 | PASS | 1 | 192.168.57.113 | YES | - |
| TC-2917 | PASS | 1 | 192.168.56.86 | YES | - |
| TC-2918 | PASS | 1 | 192.168.56.140 | YES | - |
| TC-2919 | PASS | 1 | 192.168.56.145 | YES | - |
| TC-2920 | PASS | 1 | 192.168.56.133 | YES | - |
| TC-2921 | PASS | 1 | 192.168.56.93 | YES | - |
| TC-2922 | PASS | 1 | 192.168.57.43 | YES | - |
| TC-2923 | PASS | 1 | 192.168.56.132 | YES | - |
| TC-2924 | PASS | 1 | 192.168.56.135 | YES | - |
| TC-2925 | PASS | 1 | 192.168.56.92 | YES | - |
| TC-2926 | PASS | 1 | 192.168.56.39 | YES | - |
| TC-2927 | PASS | 1 | 192.168.56.56 | YES | - |
| TC-2928 | FAIL | 0 | 192.168.57.43 | NO | case_fail |
| TC-2929 | PASS | 1 | 192.168.56.145 | YES | - |
| TC-2930 | PASS | 1 | 192.168.56.86 | YES | - |
| TC-2931 | PASS | 1 | 192.168.57.113 | YES | - |
| TC-2932 | PASS | 1 | 192.168.56.133 | YES | - |
| TC-2933 | PASS | 1 | 192.168.56.132 | YES | - |
| TC-2934 | PASS | 1 | 192.168.57.43 | YES | - |
| TC-2935 | PASS | 1 | 192.168.56.93 | YES | - |
| TC-2936 | PASS | 1 | 192.168.57.113 | YES | - |
| TC-2937 | PASS | 1 | 192.168.56.39 | YES | - |
| TC-2938 | PASS | 1 | 192.168.56.135 | YES | - |
| TC-2939 | PASS | 1 | 192.168.56.56 | YES | - |
| TC-2940 | PASS | 1 | 192.168.56.86 | YES | - |
| TC-2941 | PASS | 1 | 192.168.56.132 | YES | - |
| TC-2942 | PASS | 1 | 192.168.56.145 | YES | - |
| TC-2943 | PASS | 1 | 192.168.56.140 | YES | - |
| TC-2944 | PASS | 1 | 192.168.56.133 | YES | - |
| TC-2945 | PASS | 1 | 192.168.56.93 | YES | - |
| TC-2946 | FAIL | 1 | 192.168.56.39 | NO | case_fail |
| TC-2947 | FAIL | 1 | 192.168.56.56 | NO | case_fail |
| TC-2948 | FAIL | 1 | 192.168.56.93 | NO | procedure_error |
| TC-2949 | FAIL | 1 | 192.168.56.145 | NO | procedure_error |
| TC-2950 | FAIL | 0 | 192.168.56.132 | NO | case_fail |
| TC-2951 | PASS | 1 | 192.168.56.86 | YES | - |
| TC-2952 | FAIL | 0 | 192.168.56.92 | NO | case_fail |
| TC-2953 | PASS | 1 | 192.168.56.132 | YES | - |
| TC-2954 | PASS | 0 | 192.168.56.133 | YES | - |
| TC-2955 | FAIL | 0 | 192.168.56.86 | NO | case_fail |
| TC-2956 | FAIL | 0 | 192.168.56.140 | NO | case_fail |
| TC-2957 | PASS | 0 | 192.168.56.135 | YES | - |
| TC-2958 | PASS | 1 | 192.168.57.43 | YES | - |
| TC-2959 | PASS | 0 | 192.168.57.113 | YES | - |
| TC-2960 | PASS | 0 | 192.168.56.56 | YES | - |
| TC-2961 | PASS | 0 | 192.168.56.145 | YES | - |
| TC-2962 | PASS | 1 | 192.168.56.86 | YES | - |
| TC-2963 | PASS | 0 | 192.168.56.132 | YES | - |
| TC-2964 | PASS | 1 | 192.168.56.135 | YES | - |


## FAIL 深入分析（日志 + 源码）

### 1) mmap DMA-BUF（18 条）— 是什么、怎么触发的

**它不是一句空话。** 报错字符串来自硬编库 `libenc24`：

路径: `packages/libenc24/source/h264/taco_venc_api.c` → `encode_h264_frame_direct()`

关键逻辑（摘要）：

1. 把 `pFrame->inputPoolBlkId` 当成 **DMA-BUF 文件描述符** `dmabuf_fd`
2. 用 `DmabufHeapGetPhysAddr()` 取物理地址（这一步对本批失败用例是成功的，否则走不到 mmap）
3. 按像素格式算 `buffer_size = align16(width) * height * H264EncGetBitsPerPixel(pixFormat) / 8`
4. `mmap(NULL, buffer_size, PROT_READ|PROT_WRITE, MAP_SHARED, dmabuf_fd, 0)`
5. 若返回 `MAP_FAILED` → 打印 `ERROR: Failed to mmap DMA-BUF fd %d` → `ta_venc_encode failed` → components 侧 `avcodec_send_frame` 报 `Generic error in an external library`

**本批 18 条共同特征（对照同分辨率 NV12 PASS）：**

| 对比 | NV12 用例（如 TC-1598） | RGB/BGR 用例（如 TC-2230） |
|------|-------------------------|----------------------------|
| 输入 | `1920x1080_nv12.yuv` | `1920x1080_rgb888.rgb` / `bgr888` |
| EncodeWorker | `pix_fmt=23(nv12)` | `pix_fmt=2(rgb24)` / `3(bgr24)` |
| taco 侧 | 正常编码 | `ta_venc_encode failed: pix_fmt=11(RGB888) 或 12(BGR888)` |
| mmap | 无报错 | `Failed to mmap DMA-BUF fd 28`（反复） |
| ENC_COMPARE | compared≥10 Quality=PASSED | compared=0 |

覆盖用例：TC-2230/2231、TC-2256~2265、TC-2316、TC-2845~2849。  
全是 **RGB888 / BGR888 / BRG888** 输入的 H264 硬格式检查/spec21，分辨率从 144x96 到 1920x1080 **全部失败**——与分辨率无关，与 **RGB 打包格式**强相关。

**根因判断（有代码证据）：**

1. **直接原因**：硬编路径对输入 DMA-BUF 做 CPU `mmap` 时长度/可映射性不匹配，`mmap` 失败，编码一帧都送不进去 → compared=0。
2. **高嫌疑机制**：`H264ENC_RGB888` 在 API 注释为 *24-bit RGB **32bpp***（按 4 字节/像素算 size），而 FFmpeg/components 侧输入是 `rgb24`（3 字节/像素，frame_size=6220800）。若上游按 3bpp 分配 dmabuf，下游按 4bpp 去 `mmap` 更大长度 → 典型 `MAP_FAILED`。NV12（1.5bpp）两侧一致所以 PASS。
3. **次要嫌疑**：即便 size 对齐，RGB 路径仍用 YUV420 式 `busLuma/busChromaU/V` 切分（同文件 399–405 行），stride 日志也出现 `[1920 960 960]`（YUV 形态）。说明 RGB 硬编通路本身就不完整；但本批日志里 **先挂在 mmap**，还没走到真正 ASIC 编码成功路径。
4. **分层责任**：
   - **libenc24（外部仓库）**：mmap 长度计算 / RGB dmabuf 约定 / 失败时无 errno 打印
   - **components**：把 rgb24 帧交给 `h264_taco` 且 `scale_needed=0, fmt_conv=0`，未在用户态先转 NV12
   - **测试期望**：若产品不承诺 RGB 直通硬编，飞书 Expectation 应标 FAIL；若承诺，则属实 bug

**结论**：这不是“板子偶发坏了”，而是 **RGB/BGR 输入走硬编 DMA-BUF 映射路径的确定性失败**；同机同分辨率 NV12 对照 PASS 可证。

---

### 2) PSNR/SSIM 不达标（39 条）— 管线通了，质量判失败

典型日志（TC-1599）：

- Worker 生命周期完整：encode success=10，decode success=10
- 阈值：`min_psnr=28 min_ssim=0.90`
- 末几帧：`PSNR≈16.7 SSIM≈0.67 → FAIL`，并伴随软解 `error while decoding MB ... concealing ...`
- 汇总：`ENC_COMPARE done: compared=10 PSNR=38.15 SSIM=0.9200 pass=0`  
  （**平均指标看起来过线，但 `pass=0`**：比较器按“是否存在失败帧”一票否决，不是只看平均值）

统计：39 条中约 16 条日志含 decode conceal；约 32 条平均值本身也低于阈值；约 7 条属于“平均过线但有失败帧 → pass=0”。

**根因**：硬编码流经软解回环比较时，部分帧质量崩坏（常伴随码流错误隐藏），导致 Quality=FAILED。属 **编码质量/码流正确性** 问题（`case_fail`），不是资源缺失。

---

### 3) 8192 超大分辨率 JPEG（约 17 条）+ 高并行

- **单路 8192**（如 TC-2279）：文件能打开（单帧 ~96–200MB），随后 `jpeg_taco`/`ta_venc_encode failed`，`avcodec_send_frame` 外部库错误。单帧体量远超常见 DMA/编码器上限（历史经验 codingWidth 常 ≤8176），属 **硬件/内存能力边界**。
- **并行 16ch×8192（TC-2314）**：多路同时 `open('/usr/data/qa/8192x8192_nv12.yuv')` 报 `Invalid argument`，通道全 `FAILED (0 frames)`——文件句柄/映射/内存被打爆，不是“单纯 JPEG 算法错”。
- **128 路 1080p（TC-1626）**：大量 `打开编码器 h264_taco 失败: Generic error in an external library`，通道数超过硬编实例能力。

---

### 4) Procedure 路径错误（5 条）— 不是编码器坏了

飞书 Procedure 形如：

`qa_cases venc -p -S -M 28 -N 0.85 spec22_jpeg_nv12_144x96 /usr/data/qa/144x96_nv12.yuv`

CLI 把 **用例名** 当成了 Input，真实 yuv 路径被挤到后面未按 `-i` 使用。日志：

- `Input: spec22_jpeg_nv12_144x96`
- `无法打开文件: spec22_jpeg_nv12_144x96`

正确写法应类似成功用例：`qa_cases venc -i /usr/data/qa/144x96_nv12.yuv <profile> -p -S ...`

TC-2873/2874/2875/2948/2949 全是此类 → `procedure_error`，应改飞书 Procedure，不应改编码器。

---

### 5) Expectation=FAIL 却实际 PASS（5 条）— 边界用例“意外通过”

| TC | 飞书描述意图 | 实际 |
|----|--------------|------|
| TC-2928 | W=1924 越界应失败 | Quality PASSED |
| TC-2950 | 140×92 越界应失败 | PASSED |
| TC-2952 | JPEG 92×28 应失败 | PASSED |
| TC-2955 | GOP=0 应失败 | PASSED |
| TC-2956 | bitrate=0 应失败 | PASSED |

说明当前实现对这些“非法参数/越界分辨率”做了夹紧或默许，**没有按用例设计优雅失败**。FINAL 判 FAIL 是对的（期望失败却通过）。需要产品确认：改实现做参数校验，或改飞书 Expectation。

---

### 6) compared=0 非 mmap 类（少数）

如 TC-2232（rgb101010）：编码侧有产出迹象，但解码 `Invalid data` / conceal，比较帧数为 0。与 RGB888 mmap 批次不同，更偏 **码流无效导致无法回环比较**。
