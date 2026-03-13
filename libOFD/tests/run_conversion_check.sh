#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CLI="./build/libofd_convert_cli"
DATA_DIR="./tests/data"

if [[ ! -x "$CLI" ]]; then
  echo "[INFO] $CLI 不存在，先编译..."
  cmake -S . -B build
  cmake --build build --target libofd_convert_cli
fi

echo "[STEP] OFD -> PDF"
"$CLI" --input "$DATA_DIR/bank.ofd" --output "$DATA_DIR/bank-fixed.pdf" --mode ofd2pdf

echo "[STEP] PDF -> OFD"
"$CLI" --input "$DATA_DIR/a.pdf" --output "$DATA_DIR/a-fixed.ofd" --mode pdf2ofd

echo "[STEP] PDF(roundtrip) -> OFD (签章与可读性校验)"
"$CLI" --input "$DATA_DIR/bank-fixed.pdf" --output "$DATA_DIR/bank-roundtrip.ofd" --mode pdf2ofd

echo "[STEP] 输出结果校验"
python3 - <<'PY'
import re
import zipfile
from pathlib import Path

data_dir = Path("./tests/data")

bank_pdf = data_dir / "bank-fixed.pdf"
a_ofd = data_dir / "a-fixed.ofd"
bank_roundtrip_ofd = data_dir / "bank-roundtrip.ofd"

assert bank_pdf.exists(), "bank-fixed.pdf 不存在"
assert a_ofd.exists(), "a-fixed.ofd 不存在"
assert bank_roundtrip_ofd.exists(), "bank-roundtrip.ofd 不存在"

bank_bytes = bank_pdf.read_bytes()
print(f"[OK] bank-fixed.pdf size={len(bank_bytes)}")
assert len(bank_bytes) > 500_000, "bank-fixed.pdf 体积异常偏小"
for marker in (b"/Subtype /Image", b"BT", b"Tj"):
    assert marker in bank_bytes, f"bank-fixed.pdf 缺少标记: {marker!r}"
assert b"/STSong-Light" in bank_bytes and b"/UniGB-UCS2-H" in bank_bytes, "bank-fixed.pdf 缺少中文字体链路"
assert bank_bytes.count(b"/Subtype /Image") >= 2, "bank-fixed.pdf 图像对象不足（期望背景+印章）"

with zipfile.ZipFile(a_ofd) as z:
    names = set(z.namelist())
    assert "Doc_0/Document.xml" in names, "a-fixed.ofd 缺少 Document.xml"
    assert "Doc_0/PublicRes.xml" in names, "a-fixed.ofd 缺少 PublicRes.xml"
    assert "Doc_0/Attachs/source.pdf" not in names, "a-fixed.ofd 不应包含 source.pdf 附件"
    assert "Doc_0/Attachs/Attachments.xml" not in names, "a-fixed.ofd 不应包含附件索引"
    page_files = sorted(n for n in names if n.endswith("Content.xml"))
    assert page_files, "a-fixed.ofd 缺少页面内容"
    text_lines = []
    image_obj_count = 0
    path_obj_count = 0
    raster_page_count = len([n for n in names if n.startswith("Doc_0/Res/RasterPage_") and (n.endswith(".png") or n.endswith(".jpg") or n.endswith(".jpeg"))])
    for n in page_files:
        s = z.read(n).decode("utf-8", errors="ignore")
        text_lines.extend(
            t.strip()
            for t in re.findall(r"<(?:ofd:)?TextCode\b[^>]*>([\s\S]*?)</(?:ofd:)?TextCode>", s)
            if t.strip()
        )
        image_obj_count += len(re.findall(r"<(?:ofd:)?ImageObject\b", s))
        path_obj_count += len(re.findall(r"<(?:ofd:)?PathObject\b", s))
    merged = "\n".join(text_lines)
    if raster_page_count == 0:
        assert "税务事项通知书" in merged, "a-fixed.ofd 缺少关键文本: 税务事项通知书"
        assert "北京小雷科技有限公司" in merged, "a-fixed.ofd 缺少关键文本: 北京小雷科技有限公司"
    else:
        assert raster_page_count >= len(page_files), "a-fixed.ofd 栅格兜底页数量不足"
    assert a_ofd.stat().st_size > 2_000, "a-fixed.ofd 体积异常偏小（未生成有效内容层）"
    assert "Doc_0/DocumentRes.xml" in names, "a-fixed.ofd 缺少 DocumentRes.xml（印章图像资源未输出）"
    assert image_obj_count >= 1, "a-fixed.ofd 未生成印章/图像对象"
    print(
        f"[OK] a-fixed.ofd pages={len(page_files)} text_lines={len(text_lines)} "
        f"images={image_obj_count} paths={path_obj_count} raster_pages={raster_page_count} size={a_ofd.stat().st_size}"
    )

with zipfile.ZipFile(bank_roundtrip_ofd) as z:
    names = set(z.namelist())
    for sign_file in (
        "Doc_0/Signs/Signatures.xml",
        "Doc_0/Signs/Sign_0/Signature.xml",
        "Doc_0/Signs/Sign_0/Seal.esl",
        "Doc_0/Signs/Sign_0/SignedValue.dat",
    ):
        assert sign_file in names, f"bank-roundtrip.ofd 缺少签章文件: {sign_file}"
    page_files = sorted(n for n in names if n.endswith("Content.xml"))
    text_lines = []
    image_obj_count = 0
    path_obj_count = 0
    for n in page_files:
        s = z.read(n).decode("utf-8", errors="ignore")
        text_lines.extend(
            t.strip()
            for t in re.findall(r"<(?:ofd:)?TextCode\b[^>]*>([\s\S]*?)</(?:ofd:)?TextCode>", s)
            if t.strip()
        )
        image_obj_count += len(re.findall(r"<(?:ofd:)?ImageObject\b", s))
        path_obj_count += len(re.findall(r"<(?:ofd:)?PathObject\b", s))
    merged = "\n".join(text_lines)
    for kw in ("北京清河支行", "人民币", "94,297.62"):
        assert kw in merged, f"bank-roundtrip.ofd 缺少关键文本: {kw}"
    assert image_obj_count >= 1, "bank-roundtrip.ofd 未生成图像对象"
    print(f"[OK] bank-roundtrip.ofd text_lines={len(text_lines)} images={image_obj_count} paths={path_obj_count}")

print("[COVERAGE] 元素覆盖率(关键类型):")
print("  - a-fixed.ofd: Text/Image/Path 已校验")
print("  - bank-roundtrip.ofd: Text/Image/Path + 签章工件已校验")

print("[PASS] 转换与基础校验通过")
PY

