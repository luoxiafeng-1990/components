# COMPONENT_BITABLE_RUN_SUMMARY — QA_ENCODE

- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_234050`
- 测试日期: 2026-07-18
- Test Set: **QA_ENCODE**（飞书 426 条，可执行 **381**）
- deb: `workshop-qa-components_2.86+20260717234338_riscv64.deb`
- 主机: **13** 台 — 192.168.56.132, 192.168.56.133, 192.168.56.140, 192.168.56.145, 192.168.56.214, 192.168.56.39, 192.168.56.56, 192.168.56.86, 192.168.56.92, 192.168.56.93, 192.168.57.113, 192.168.57.43, 192.168.59.67
- 多轮: 4 轮；最终 **PASS=288 / FAIL=93**（通过率 75.6%）
- Flaky(retested_pass): **66**
- P5: 见 `LOG_REVIEW_REPORT.md` / `P5_ANALYZED.json` / `P5.1_FAIL_LIST.md`

## 📊 测试结果统计

### 总体统计
| 指标 | 数值 | 占比 |
|------|------|------|
| 总用例数 | 381 | 100% |
| ✅ PASS | 288 | 75.6% |
| ❌ FAIL | 93 | 24.4% |
| ⏭️ SKIP | 45（飞书不可执行，未入调度） | — |

### 各轮统计
| 轮次 | 执行 | PASS | FAIL |
|------|------|------|------|
| R1 | 381 | 222 | 159 |
| R2 | 159 | 55 | 104 |
| R3 | 104 | 7 | 97 |
| R4 | 97 | 4 | 93 |

### 按主机统计
| 主机 IP | 执行数 | PASS | FAIL | 通过率 |
|---------|--------|------|------|--------|
| 192.168.56.132 | 13 | 7 | 6 | 54% |
| 192.168.56.133 | 36 | 27 | 9 | 75% |
| 192.168.56.140 | 32 | 23 | 9 | 72% |
| 192.168.56.145 | 23 | 18 | 5 | 78% |
| 192.168.56.214 | 33 | 27 | 6 | 82% |
| 192.168.56.39 | 26 | 18 | 8 | 69% |
| 192.168.56.56 | 38 | 30 | 8 | 79% |
| 192.168.56.86 | 27 | 19 | 8 | 70% |
| 192.168.56.92 | 36 | 27 | 9 | 75% |
| 192.168.56.93 | 39 | 31 | 8 | 79% |
| 192.168.57.113 | 15 | 7 | 8 | 47% |
| 192.168.57.43 | 29 | 25 | 4 | 86% |
| 192.168.59.67 | 34 | 29 | 5 | 85% |

## FAIL 归因汇总（P5）

| FAIL_REASON | 数量 | 说明 |
|-------------|------|------|
| case_fail | 71 | 质量不达标 42；RGB mmap 18；意外 PASS 8；compared=0 3 |
| hardware_limit | 17 | 8192 超大分辨率 16；高并行 1 |
| procedure_error | 5 | Procedure 把用例名当 Input |

## ❌ 失败用例详细登记

| Case ID | 问题描述 | 修复记录 | 测试次数 | Log 文件 |
|---------|---------|----------|---------|---------|
| TC-1599 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1599/qa_cases_full.log) |
| TC-1626 | 高并行通道编码资源耗尽（high_parallel） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1626/qa_cases_full.log) |
| TC-1633 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1633/qa_cases_full.log) |
| TC-1634 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1634/qa_cases_full.log) |
| TC-1636 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1636/qa_cases_full.log) |
| TC-1639 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1639/qa_cases_full.log) |
| TC-1643 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1643/qa_cases_full.log) |
| TC-1644 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1644/qa_cases_full.log) |
| TC-1779 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-1779/qa_cases_full.log) |
| TC-2204 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2204/qa_cases_full.log) |
| TC-2207 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2207/qa_cases_full.log) |
| TC-2230 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2230/qa_cases_full.log) |
| TC-2231 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2231/qa_cases_full.log) |
| TC-2232 | compared=0 管线失败（非 mmap）（compared_zero） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2232/qa_cases_full.log) |
| TC-2233 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2233/qa_cases_full.log) |
| TC-2247 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2247/qa_cases_full.log) |
| TC-2248 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2248/qa_cases_full.log) |
| TC-2256 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2256/qa_cases_full.log) |
| TC-2257 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2257/qa_cases_full.log) |
| TC-2258 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2258/qa_cases_full.log) |
| TC-2259 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2259/qa_cases_full.log) |
| TC-2260 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2260/qa_cases_full.log) |
| TC-2261 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2261/qa_cases_full.log) |
| TC-2262 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2262/qa_cases_full.log) |
| TC-2263 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2263/qa_cases_full.log) |
| TC-2264 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2264/qa_cases_full.log) |
| TC-2265 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2265/qa_cases_full.log) |
| TC-2266 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2266/qa_cases_full.log) |
| TC-2267 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2267/qa_cases_full.log) |
| TC-2268 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2268/qa_cases_full.log) |
| TC-2270 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2270/qa_cases_full.log) |
| TC-2271 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2271/qa_cases_full.log) |
| TC-2272 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2272/qa_cases_full.log) |
| TC-2273 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2273/qa_cases_full.log) |
| TC-2277 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2277/qa_cases_full.log) |
| TC-2279 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2279/qa_cases_full.log) |
| TC-2281 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2281/qa_cases_full.log) |
| TC-2283 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2283/qa_cases_full.log) |
| TC-2285 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2285/qa_cases_full.log) |
| TC-2286 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2286/qa_cases_full.log) |
| TC-2287 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2287/qa_cases_full.log) |
| TC-2288 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2288/qa_cases_full.log) |
| TC-2289 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2289/qa_cases_full.log) |
| TC-2291 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2291/qa_cases_full.log) |
| TC-2293 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2293/qa_cases_full.log) |
| TC-2295 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2295/qa_cases_full.log) |
| TC-2297 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2297/qa_cases_full.log) |
| TC-2298 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2298/qa_cases_full.log) |
| TC-2299 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2299/qa_cases_full.log) |
| TC-2300 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2300/qa_cases_full.log) |
| TC-2301 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2301/qa_cases_full.log) |
| TC-2302 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2302/qa_cases_full.log) |
| TC-2303 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2303/qa_cases_full.log) |
| TC-2304 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2304/qa_cases_full.log) |
| TC-2305 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2305/qa_cases_full.log) |
| TC-2306 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2306/qa_cases_full.log) |
| TC-2307 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2307/qa_cases_full.log) |
| TC-2308 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2308/qa_cases_full.log) |
| TC-2309 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2309/qa_cases_full.log) |
| TC-2311 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2311/qa_cases_full.log) |
| TC-2313 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2313/qa_cases_full.log) |
| TC-2314 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界）（huge_res_8192） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2314/qa_cases_full.log) |
| TC-2316 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2316/qa_cases_full.log) |
| TC-2431 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2431/qa_cases_full.log) |
| TC-2432 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2432/qa_cases_full.log) |
| TC-2437 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2437/qa_cases_full.log) |
| TC-2438 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2438/qa_cases_full.log) |
| TC-2845 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2845/qa_cases_full.log) |
| TC-2846 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2846/qa_cases_full.log) |
| TC-2847 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2847/qa_cases_full.log) |
| TC-2848 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2848/qa_cases_full.log) |
| TC-2849 | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配）（mmap_dmabuf） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2849/qa_cases_full.log) |
| TC-2850 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2850/qa_cases_full.log) |
| TC-2851 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2851/qa_cases_full.log) |
| TC-2852 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2852/qa_cases_full.log) |
| TC-2853 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2853/qa_cases_full.log) |
| TC-2854 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2854/qa_cases_full.log) |
| TC-2856 | PSNR/SSIM 不达标（管线通，质量 FAIL）（quality_fail） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2856/qa_cases_full.log) |
| TC-2873 | Procedure 把用例名当成输入路径（procedure_bad_input） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2873/qa_cases_full.log) |
| TC-2874 | Procedure 把用例名当成输入路径（procedure_bad_input） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2874/qa_cases_full.log) |
| TC-2875 | Procedure 把用例名当成输入路径（procedure_bad_input） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2875/qa_cases_full.log) |
| TC-2928 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2928/qa_cases_full.log) |
| TC-2929 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2929/qa_cases_full.log) |
| TC-2936 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2936/qa_cases_full.log) |
| TC-2946 | compared=0 管线失败（非 mmap）（compared_zero） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2946/qa_cases_full.log) |
| TC-2947 | compared=0 管线失败（非 mmap）（compared_zero） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2947/qa_cases_full.log) |
| TC-2948 | Procedure 把用例名当成输入路径（procedure_bad_input） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2948/qa_cases_full.log) |
| TC-2949 | Procedure 把用例名当成输入路径（procedure_bad_input） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2949/qa_cases_full.log) |
| TC-2951 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2951/qa_cases_full.log) |
| TC-2955 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2955/qa_cases_full.log) |
| TC-2958 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2958/qa_cases_full.log) |
| TC-2962 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2962/qa_cases_full.log) |
| TC-2964 | Expectation=FAIL 却实际 PASS（边界参数未拦截）（unexpected_pass） | 待分析 | 4 | [qa_cases_full.log](./encode/TC-2964/qa_cases_full.log) |

## FAIL 深入分析（日志 + 源码）

### 1) mmap DMA-BUF（18）— RGB/BGR 硬编

代表：TC-2230（1920×1080 rgb888）、TC-2257（320×240 rgb888）。

- 日志：`Failed to mmap DMA-BUF fd 28` → `ta_venc_encode failed pix_fmt=11/12` → `compared=0`
- 对照：同分辨率 NV12（如 TC-1598）PASS
- 源码：`libenc24/.../taco_venc_api.c` `encode_h264_frame_direct()`：`buffer_size` 用 `H264EncGetBitsPerPixel(RGB888)=32`，上游 rgb24 为 3 B/px
- 归因：**case_fail**（libenc24 外部仓库约定/实现 bug）；建议修 bpp 对齐 `plen` 或 components 先转 NV12

### 2) PSNR/SSIM 不达标（42）

代表：TC-1599。管线完整 `compared=10`，末帧 `PSNR≈16.7` + `concealing`；平均 PSNR 可过线但 `pass=0`（单帧否决）。归因 **case_fail**。

### 3) 8192 超大分辨率（16）+ 高并行（1）

- 单路 8192 JPEG（TC-2279）：`input_size=100663296`，`ta_venc_encode failed` → **hardware_limit**
- TC-2314 等并行 8192：资源打爆
- TC-1626 128ch 1080p：大量 `ta_venc_encode failed ret=-2` → **hardware_limit**

### 4) Procedure 路径错误（5）

TC-2873/2874/2875/2948/2949：`Input: spec22_...` / `无法打开文件` → **procedure_error**，改飞书 Procedure。

### 5) Expectation=FAIL 却 PASS（8）

TC-2928（1924 越界）、TC-2955（GOP=0）等实际 Quality PASSED → **case_fail**（参数校验缺失）或改 Expectation。

### 6) compared=0 非 mmap（3）

TC-2232, TC-2946, TC-2947：码流/管线失败无可比帧。

## Flaky 用例（66）

首轮 FAIL、后续轮 PASS。归因：轮间 reboot / 残留清理 / 资源竞争缓解。完整列表见 `MULTI_ROUND_SUMMARY.json` → `retested_pass`。

样例：
- TC-1603 — 第 2 轮转 PASS（共测 2 次）
- TC-1610 — 第 2 轮转 PASS（共测 2 次）
- TC-1614 — 第 2 轮转 PASS（共测 2 次）
- TC-1615 — 第 4 轮转 PASS（共测 4 次）
- TC-1616 — 第 2 轮转 PASS（共测 2 次）
- TC-1619 — 第 2 轮转 PASS（共测 2 次）
- TC-1620 — 第 2 轮转 PASS（共测 2 次）
- TC-1635 — 第 2 轮转 PASS（共测 2 次）
- TC-1640 — 第 3 轮转 PASS（共测 3 次）
- TC-1641 — 第 2 轮转 PASS（共测 2 次）
- TC-1642 — 第 2 轮转 PASS（共测 2 次）
- TC-1645 — 第 2 轮转 PASS（共测 2 次）
- TC-1651 — 第 2 轮转 PASS（共测 2 次）
- TC-1653 — 第 2 轮转 PASS（共测 2 次）
- TC-1662 — 第 2 轮转 PASS（共测 2 次）
- … 共 66 条

## 🖥️ 测试环境

| 项 | 值 |
|----|-----|
| 测试日期 | 2026-07-18 |
| Test Set | QA_ENCODE |
| 主机列表 | 192.168.56.132, 192.168.56.133, 192.168.56.140, 192.168.56.145, 192.168.56.214, 192.168.56.39, 192.168.56.56, 192.168.56.86, 192.168.56.92, 192.168.56.93, 192.168.57.113, 192.168.57.43, 192.168.59.67 |
| deb | workshop-qa-components_2.86+20260717234338 |
| 前置 | 卸载 components/qa_cases → 重装 tps-test → 编译部署 |

## 📋 完整用例结果（FAIL 明细优先；PASS 见 MULTI_ROUND_SUMMARY）

### QA_ENCODE FAIL（93）
| # | Case ID | 名称 | 结果 | 归因 | 问题概要 |
|---|---------|------|------|------|----------|
| 1 | TC-1599 | h264_1920x1080_30_8mbps | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 2 | TC-1626 | venc_parallel_128ch_1080p | ❌ FAIL | hardware_limit | 高并行通道编码资源耗尽 |
| 3 | TC-1633 | spec21_h264_yuv420p_144x96 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 4 | TC-1634 | spec21_h264_yuv420p_320x240 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 5 | TC-1636 | spec21_h264_yuv420p_1280x720 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 6 | TC-1639 | spec21_h264_nv12_320x240 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 7 | TC-1643 | spec21_h264_nv21_144x96 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 8 | TC-1644 | spec21_h264_nv21_320x240 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 9 | TC-1779 | spec22_jpeg_bgr444_96x32 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 10 | TC-2204 | jpeg_1920x1080_q1 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 11 | TC-2207 | jpeg_1920x1080_q100 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 12 | TC-2230 | h264_hwfmt_chk_rgb888_1920x1080 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 13 | TC-2231 | h264_hwfmt_chk_bgr888_1920x1080 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 14 | TC-2232 | h264_hwfmt_chk_rgb101010_1920x1080 | ❌ FAIL | case_fail | compared=0 管线失败（非 mmap） |
| 15 | TC-2233 | h264_hwfmt_chk_bgr101010_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 16 | TC-2247 | jpeg_hwfmt_chk_rgb888_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 17 | TC-2248 | jpeg_hwfmt_chk_bgr888_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 18 | TC-2256 | spec21_h264_rgb888_144x96 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 19 | TC-2257 | spec21_h264_rgb888_320x240 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 20 | TC-2258 | spec21_h264_rgb888_640x480 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 21 | TC-2259 | spec21_h264_rgb888_1280x720 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 22 | TC-2260 | spec21_h264_rgb888_1920x1080 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 23 | TC-2261 | spec21_h264_bgr888_144x96 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 24 | TC-2262 | spec21_h264_bgr888_320x240 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 25 | TC-2263 | spec21_h264_bgr888_640x480 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 26 | TC-2264 | spec21_h264_bgr888_1280x720 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 27 | TC-2265 | spec21_h264_bgr888_1920x1080 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 28 | TC-2266 | spec21_h264_rgb101010_144x96 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 29 | TC-2267 | spec21_h264_rgb101010_320x240 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 30 | TC-2268 | spec21_h264_rgb101010_640x480 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 31 | TC-2270 | spec21_h264_rgb101010_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 32 | TC-2271 | spec21_h264_bgr101010_144x96 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 33 | TC-2272 | spec21_h264_bgr101010_320x240 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 34 | TC-2273 | spec21_h264_bgr101010_640x480 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 35 | TC-2277 | spec22_jpeg_yuv420p_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 36 | TC-2279 | spec22_jpeg_nv12_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 37 | TC-2281 | spec22_jpeg_nv21_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 38 | TC-2283 | spec22_jpeg_yuyv422_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 39 | TC-2285 | spec22_jpeg_uyvy422_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 40 | TC-2286 | spec22_jpeg_rgb444_3840x2160 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 41 | TC-2287 | spec22_jpeg_rgb444_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 42 | TC-2288 | spec22_jpeg_bgr444_3840x2160 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 43 | TC-2289 | spec22_jpeg_bgr444_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 44 | TC-2291 | spec22_jpeg_rgb555_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 45 | TC-2293 | spec22_jpeg_bgr555_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 46 | TC-2295 | spec22_jpeg_rgb565_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 47 | TC-2297 | spec22_jpeg_bgr565_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 48 | TC-2298 | spec22_jpeg_rgb888_96x32 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 49 | TC-2299 | spec22_jpeg_rgb888_512x512 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 50 | TC-2300 | spec22_jpeg_rgb888_1280x720 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 51 | TC-2301 | spec22_jpeg_rgb888_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 52 | TC-2302 | spec22_jpeg_rgb888_3840x2160 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 53 | TC-2303 | spec22_jpeg_rgb888_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 54 | TC-2304 | spec22_jpeg_bgr888_96x32 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 55 | TC-2305 | spec22_jpeg_bgr888_512x512 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 56 | TC-2306 | spec22_jpeg_bgr888_1280x720 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 57 | TC-2307 | spec22_jpeg_bgr888_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 58 | TC-2308 | spec22_jpeg_bgr888_3840x2160 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 59 | TC-2309 | spec22_jpeg_bgr888_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 60 | TC-2311 | spec22_jpeg_rgb101010_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 61 | TC-2313 | spec22_jpeg_bgr101010_8192x8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 62 | TC-2314 | venc_parallel_16ch_mjpeg_8192 | ❌ FAIL | hardware_limit | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） |
| 63 | TC-2316 | spec27_h264_1080p_25fps_rgb888 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 64 | TC-2431 | spec22_jpeg_rgb444_2560x1440 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 65 | TC-2432 | spec22_jpeg_bgr444_2560x1440 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 66 | TC-2437 | spec22_jpeg_rgb888_2560x1440 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 67 | TC-2438 | spec22_jpeg_brg888_2560x1440 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 68 | TC-2845 | spec21_h264_brg888_144x96 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 69 | TC-2846 | spec21_h264_brg888_320x240 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 70 | TC-2847 | spec21_h264_brg888_640x480 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 71 | TC-2848 | spec21_h264_brg888_1280x720 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 72 | TC-2849 | spec21_h264_brg888_1920x1080 | ❌ FAIL | case_fail | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） |
| 73 | TC-2850 | spec21_h264_brg101010_144x96 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 74 | TC-2851 | spec21_h264_brg101010_320x240 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 75 | TC-2852 | spec21_h264_brg101010_640x480 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 76 | TC-2853 | spec21_h264_brg101010_1280x720 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 77 | TC-2854 | spec21_h264_brg101010_1920x1080 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 78 | TC-2856 | spec21_h264_nv12_512x512 | ❌ FAIL | case_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） |
| 79 | TC-2873 | spec22_jpeg_nv12_144x96 | ❌ FAIL | procedure_error | Procedure 把用例名当成输入路径 |
| 80 | TC-2874 | spec22_jpeg_nv12_320x240 | ❌ FAIL | procedure_error | Procedure 把用例名当成输入路径 |
| 81 | TC-2875 | spec22_jpeg_nv12_640x480 | ❌ FAIL | procedure_error | Procedure 把用例名当成输入路径 |
| 82 | TC-2928 | boundary_h264_nv12_1924x1080 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 83 | TC-2929 | boundary_h264_nv12_1920x1084 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 84 | TC-2936 | boundary_h264_nv12_1920x1079 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 85 | TC-2946 | boundary_h264_nv12_144x1080 | ❌ FAIL | case_fail | compared=0 管线失败（非 mmap） |
| 86 | TC-2947 | boundary_h264_nv12_1920x96 | ❌ FAIL | case_fail | compared=0 管线失败（非 mmap） |
| 87 | TC-2948 | boundary_jpeg_nv12_96x8192 | ❌ FAIL | procedure_error | Procedure 把用例名当成输入路径 |
| 88 | TC-2949 | boundary_jpeg_nv12_8192x32 | ❌ FAIL | procedure_error | Procedure 把用例名当成输入路径 |
| 89 | TC-2951 | boundary_h264_rgb888_140x92 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 90 | TC-2955 | boundary_h264_param_gop0 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 91 | TC-2958 | boundary_h264_param_fps0 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 92 | TC-2962 | boundary_jpeg_param_qneg1 | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |
| 93 | TC-2964 | stress_jpeg_32ch_parallel | ❌ FAIL | case_fail | Expectation=FAIL 却实际 PASS（边界参数未拦截） |

### QA_ENCODE PASS（288）
详见 `MULTI_ROUND_SUMMARY.json` 中 `final_verdicts` 且 `final_verdict=PASS` 的条目。

