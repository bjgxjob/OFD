# libOFD

`libOFD` 是一个使用 C/C++ 构建的 OFD 基础库工程，目标是分阶段实现 `GB/T 33190-2016`。

## 当前能力（v0.2）

- C API + C++ API 双接口。
- 创建空白 OFD 文档（元数据 + 页面文本）。
- 编辑文档：
  - 修改 `Creator`
  - 添加页面文本
  - 更新指定页面文本
- 读取“解包后的 OFD 目录”：
  - 读取 `OFD.xml`
  - 解析 `DocRoot`
  - 读取 `Document.xml`
  - 解析页面列表（`Page ID`、`BaseLoc`）和页面文本（`TextCode`）
- 存储：保存为“解包后的 OFD 目录结构”。
- 转换：`OFD -> TXT` 导出、`TXT -> OFD` 导入。
- 转换接口：内置独立模块 `src/pdf_engine/` 实现 `OFD <-> PDF` 转换；同时支持外部 provider 覆盖。
- 签章与验签：基于可切换后端（OpenSSL / TaSSL）完成签名和验证。
- 提供 `libofd_load_path` 统一入口（目录可读，`.ofd` 文件读取待实现）。
- 提供端到端生命周期测试（创建、编辑、保存、签章、验签、转换、导入）。

> 说明：当前版本为了快速建立可演进代码基线，先支持“目录包”输入；`.ofd(zip)` 直接读取将在下一阶段补齐。

## 构建

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

> 签章相关测试优先使用 `/home/yb/tassl/bin/openssl`，若不存在则回退系统 `openssl`。

## 目录结构

- `include/libofd/`：公共头文件与 C API
- `src/`：核心实现
- `tests/`：基础测试与样例数据

## 主要 C API

- `libofd_create_empty`
- `libofd_load_path` / `libofd_load_exploded_package`
- `libofd_save_exploded_package`
- `libofd_set_creator`
- `libofd_add_page_text` / `libofd_set_page_text` / `libofd_get_page_text`
- `libofd_export_to_text` / `libofd_import_from_text`
- `libofd_set_sign_backend` / `libofd_set_tassl_root`
- `libofd_set_external_sign_provider` / `libofd_clear_external_sign_provider`
- `libofd_set_external_convert_provider` / `libofd_clear_external_convert_provider`
- `libofd_set_external_image_decode_provider` / `libofd_clear_external_image_decode_provider`
- `libofd_sign_with_private_key` / `libofd_verify_signatures`
- `libofd_convert_ofd_to_pdf` / `libofd_convert_pdf_to_ofd`

## PDF 转换引擎（独立模块）

- 模块路径：`src/pdf_engine/`
- 调用优先级：
  1. 若已注册外部转换 provider，优先调用外部实现
  2. 否则回退到内置 `pdf_engine` 实现
- 内置实现当前能力：
  - OFD -> PDF：解析 OFD 页面文本对象（`TextObject` 的 `Boundary/Size/TextCode`）并映射到 PDF 坐标排版，支持多 `TextCode` 合并排版
  - OFD -> PDF：解析资源中的字体与常见图片（`ImageObject + MultiMedia`），当前支持 `JPEG / PNG / BMP` 并输出 `/XObject` 图像资源
  - OFD -> PDF：解析 `PathObject`（`AbbreviatedData`）并输出 PDF 路径绘制命令（`m/l/c/S/f/B`）
  - PDF -> OFD：解析 PDF 文本流并重建 OFD 目录包
  - 可通过外部图片解码 provider 扩展 `GIF / WebP / TIFF` 等额外格式（输出 RAW 像素后由引擎嵌入 PDF）

## 签章接口可重构设计

- 业务层仅调用统一 C API，不直接依赖具体命令行工具。
- 内部通过 `CryptoProvider` 抽象隔离签名实现。
- 当前提供：
  - `OpenSSL` provider
  - `TaSSL` provider（根路径可配置，默认 `/home/yb/tassl/`）
- 支持注册外部 C 回调 provider（签名/验签），用于未来替换为国密 SDK 或 HSM 实现。
- 后续可无缝替换为国密 SDK/HSM provider，无需改动上层调用接口。

## 下一步建议（对齐 GB/T 33190-2016）

1. 增加 `.ofd` 容器读取（zip 解包层）。
2. 完善标准对象模型（Page、Layer、Resource、Annot、Signature 结构体与解析器）。
3. 引入真实图元渲染模型，构建打印一致性基线测试。
4. 接入证书链、时间戳与国密算法，实现规范化签章验证流程。
5. 建立标准条款映射矩阵与自动化回归。

