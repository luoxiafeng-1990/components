# LOG_REVIEW_REPORT — QA_ENCODE

- 生成时间: 2026-07-18 07:24:18
- 结果目录: `/home/ubuntu/test/qa_cases/component_bitable_test_res_20260717_234050`
- 复查总数: **381**（= 本轮执行 case 总数）
- 一致数: **381** / 不一致数: **0**（result.txt FINAL_VERDICT 与日志 ENC_COMPARE/Status 一致；P5 已补三段式判定）
- 最终 PASS: **288** / FAIL: **93** / Flaky(retested_pass): **66**

## 按归因分类的 FAIL 统计

| FAIL_REASON | 数量 | 主要签名 |
|-------------|------|----------|
| case_fail | 71 | quality_fail=42, mmap_dmabuf=18, unexpected_pass=8, compared_zero=3 |
| hardware_limit | 17 | huge_res_8192=16, high_parallel=1 |
| procedure_error | 5 | procedure_bad_input=5 |

## FAIL 逐条（P5.2 已读 qa_cases_full.log）

| TC-ID | 归因 | 签名 | 问题概要 | Log |
|-------|------|------|----------|-----|
| TC-1599 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1599/qa_cases_full.log) |
| TC-1626 | hardware_limit | high_parallel | 高并行通道编码资源耗尽 | [log](./encode/TC-1626/qa_cases_full.log) |
| TC-1633 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1633/qa_cases_full.log) |
| TC-1634 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1634/qa_cases_full.log) |
| TC-1636 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1636/qa_cases_full.log) |
| TC-1639 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1639/qa_cases_full.log) |
| TC-1643 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1643/qa_cases_full.log) |
| TC-1644 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1644/qa_cases_full.log) |
| TC-1779 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-1779/qa_cases_full.log) |
| TC-2204 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2204/qa_cases_full.log) |
| TC-2207 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2207/qa_cases_full.log) |
| TC-2230 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2230/qa_cases_full.log) |
| TC-2231 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2231/qa_cases_full.log) |
| TC-2232 | case_fail | compared_zero | compared=0 管线失败（非 mmap） | [log](./encode/TC-2232/qa_cases_full.log) |
| TC-2233 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2233/qa_cases_full.log) |
| TC-2247 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2247/qa_cases_full.log) |
| TC-2248 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2248/qa_cases_full.log) |
| TC-2256 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2256/qa_cases_full.log) |
| TC-2257 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2257/qa_cases_full.log) |
| TC-2258 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2258/qa_cases_full.log) |
| TC-2259 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2259/qa_cases_full.log) |
| TC-2260 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2260/qa_cases_full.log) |
| TC-2261 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2261/qa_cases_full.log) |
| TC-2262 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2262/qa_cases_full.log) |
| TC-2263 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2263/qa_cases_full.log) |
| TC-2264 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2264/qa_cases_full.log) |
| TC-2265 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2265/qa_cases_full.log) |
| TC-2266 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2266/qa_cases_full.log) |
| TC-2267 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2267/qa_cases_full.log) |
| TC-2268 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2268/qa_cases_full.log) |
| TC-2270 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2270/qa_cases_full.log) |
| TC-2271 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2271/qa_cases_full.log) |
| TC-2272 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2272/qa_cases_full.log) |
| TC-2273 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2273/qa_cases_full.log) |
| TC-2277 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2277/qa_cases_full.log) |
| TC-2279 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2279/qa_cases_full.log) |
| TC-2281 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2281/qa_cases_full.log) |
| TC-2283 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2283/qa_cases_full.log) |
| TC-2285 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2285/qa_cases_full.log) |
| TC-2286 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2286/qa_cases_full.log) |
| TC-2287 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2287/qa_cases_full.log) |
| TC-2288 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2288/qa_cases_full.log) |
| TC-2289 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2289/qa_cases_full.log) |
| TC-2291 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2291/qa_cases_full.log) |
| TC-2293 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2293/qa_cases_full.log) |
| TC-2295 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2295/qa_cases_full.log) |
| TC-2297 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2297/qa_cases_full.log) |
| TC-2298 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2298/qa_cases_full.log) |
| TC-2299 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2299/qa_cases_full.log) |
| TC-2300 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2300/qa_cases_full.log) |
| TC-2301 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2301/qa_cases_full.log) |
| TC-2302 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2302/qa_cases_full.log) |
| TC-2303 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2303/qa_cases_full.log) |
| TC-2304 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2304/qa_cases_full.log) |
| TC-2305 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2305/qa_cases_full.log) |
| TC-2306 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2306/qa_cases_full.log) |
| TC-2307 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2307/qa_cases_full.log) |
| TC-2308 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2308/qa_cases_full.log) |
| TC-2309 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2309/qa_cases_full.log) |
| TC-2311 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2311/qa_cases_full.log) |
| TC-2313 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2313/qa_cases_full.log) |
| TC-2314 | hardware_limit | huge_res_8192 | 8192 超大分辨率 JPEG/编码失败（硬件/内存边界） | [log](./encode/TC-2314/qa_cases_full.log) |
| TC-2316 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2316/qa_cases_full.log) |
| TC-2431 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2431/qa_cases_full.log) |
| TC-2432 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2432/qa_cases_full.log) |
| TC-2437 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2437/qa_cases_full.log) |
| TC-2438 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2438/qa_cases_full.log) |
| TC-2845 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2845/qa_cases_full.log) |
| TC-2846 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2846/qa_cases_full.log) |
| TC-2847 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2847/qa_cases_full.log) |
| TC-2848 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2848/qa_cases_full.log) |
| TC-2849 | case_fail | mmap_dmabuf | RGB/BGR 输入硬编 mmap DMA-BUF 失败（libenc24 bpp/size 不匹配） | [log](./encode/TC-2849/qa_cases_full.log) |
| TC-2850 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2850/qa_cases_full.log) |
| TC-2851 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2851/qa_cases_full.log) |
| TC-2852 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2852/qa_cases_full.log) |
| TC-2853 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2853/qa_cases_full.log) |
| TC-2854 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2854/qa_cases_full.log) |
| TC-2856 | case_fail | quality_fail | PSNR/SSIM 不达标（管线通，质量 FAIL） | [log](./encode/TC-2856/qa_cases_full.log) |
| TC-2873 | procedure_error | procedure_bad_input | Procedure 把用例名当成输入路径 | [log](./encode/TC-2873/qa_cases_full.log) |
| TC-2874 | procedure_error | procedure_bad_input | Procedure 把用例名当成输入路径 | [log](./encode/TC-2874/qa_cases_full.log) |
| TC-2875 | procedure_error | procedure_bad_input | Procedure 把用例名当成输入路径 | [log](./encode/TC-2875/qa_cases_full.log) |
| TC-2928 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2928/qa_cases_full.log) |
| TC-2929 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2929/qa_cases_full.log) |
| TC-2936 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2936/qa_cases_full.log) |
| TC-2946 | case_fail | compared_zero | compared=0 管线失败（非 mmap） | [log](./encode/TC-2946/qa_cases_full.log) |
| TC-2947 | case_fail | compared_zero | compared=0 管线失败（非 mmap） | [log](./encode/TC-2947/qa_cases_full.log) |
| TC-2948 | procedure_error | procedure_bad_input | Procedure 把用例名当成输入路径 | [log](./encode/TC-2948/qa_cases_full.log) |
| TC-2949 | procedure_error | procedure_bad_input | Procedure 把用例名当成输入路径 | [log](./encode/TC-2949/qa_cases_full.log) |
| TC-2951 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2951/qa_cases_full.log) |
| TC-2955 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2955/qa_cases_full.log) |
| TC-2958 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2958/qa_cases_full.log) |
| TC-2962 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2962/qa_cases_full.log) |
| TC-2964 | case_fail | unexpected_pass | Expectation=FAIL 却实际 PASS（边界参数未拦截） | [log](./encode/TC-2964/qa_cases_full.log) |

## 不一致项

无。P4.5 Expectation 对照由调度器写入；P5 仅追加源码/日志/综合结论，未改 FINAL_VERDICT。

## Flaky（retested_pass）双计说明

共 **66** 条首轮 FAIL、后续轮次 PASS。常见反转原因：轮间 reboot 清 VPU/编码器残留、资源竞争缓解。清单见 MULTI_ROUND_SUMMARY.json `retested_pass`。

