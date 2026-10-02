#!/usr/bin/env python3
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path
import base64, textwrap, sys

out_path = Path(sys.argv[1] if len(sys.argv) > 1 else "az/v07/zh_glyphs.h")
font_candidates = [
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0),
    ("/usr/share/fonts/truetype/arphic-gbsn00lp/gbsn00lp.ttf", 0),
]
font = None
font_path = None
for p, idx in font_candidates:
    if Path(p).exists():
        font = ImageFont.truetype(p, 20, index=idx)
        font_path = p
        break
if font is None:
    raise SystemExit("No CJK font found on build runner")

chars = set()
# GB2312 level-1: 3755 common Simplified Chinese characters.
for lead in range(0xB0, 0xD8):
    for trail in range(0xA1, 0xFF):
        try:
            ch = bytes((lead, trail)).decode("gb2312")
        except Exception:
            continue
        if len(ch) == 1:
            chars.add(ch)

chars.update("、。【】（）《》〈〉，。：；！？“”‘’…—·￥％＃＆＠～＋－＝／\\｜「」『』")
chars.update(
    "中文脚本输出信息界面语言字体大小实际字号画布粘贴并运行保存载入上次复制全部清空"
    "自动滚动状态连接本地脚本主题添加运行结果内容刷新自定义目录内置目录选择直接运行"
    "加载到编辑器主题设置强调色背景色文字色窗口透明度恢复默认深红蓝色绿色紫色未找到"
    "可读文件读取失败文件为空已找到个脚本已选择"
)
chars = sorted(chars, key=ord)

W = H = 22
packed = bytearray()
meta = []
for ch in chars:
    img = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(img)
    bbox = draw.textbbox((0, 0), ch, font=font)
    gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (W - gw) // 2 - bbox[0]
    y = (H - gh) // 2 - bbox[1] - 1
    draw.text((x, y), ch, font=font, fill=255)
    pix = list(img.getdata())
    off = len(packed)
    for i in range(0, len(pix), 4):
        b = 0
        for j in range(4):
            v = pix[i + j] if i + j < len(pix) else 0
            q = min(3, (v + 42) // 85)
            b |= q << (6 - 2 * j)
        packed.append(b)
    meta.append((ord(ch), off, len(packed) - off))

enc = base64.b64encode(packed).decode("ascii")
lines = [
    "#pragma once",
    "#include <cstdint>",
    "struct AZZhGlyphMeta { uint16_t code; uint32_t offset; uint16_t bytes; };",
    f"static constexpr int AZ_ZH_W = {W};",
    f"static constexpr int AZ_ZH_H = {H};",
    f"static constexpr int AZ_ZH_PACKED_BYTES = {len(packed)};",
    "static const char AZ_ZH_DATA_B64[] =",
]
for chunk in textwrap.wrap(enc, 120):
    lines.append(f'"{chunk}"')
lines[-1] += ";"
lines.append("static const AZZhGlyphMeta AZ_ZH_GLYPHS[] = {")
for code, off, size in meta:
    lines.append(f"  {{0x{code:04X}, {off}, {size}}},")
lines.append("};")
lines.append("static constexpr int AZ_ZH_GLYPH_COUNT = sizeof(AZ_ZH_GLYPHS)/sizeof(AZ_ZH_GLYPHS[0]);")
out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"Generated {len(chars)} CJK glyphs ({len(packed)} packed bytes) from {font_path}")
