# 生成 tests/fixtures/JpegFixtures.h：原创合成图像的顺序式 / 渐进式 JPEG 字节。
# 用法：python tools/gen-jpeg-fixtures.py（需要 Pillow）
import io, math
from PIL import Image

def rgb_image(w, h):
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if x < w // 2 and y < h // 2:   # 左上：纯色块（抽样点）
                px[x, y] = (200, 40, 40)
            elif x >= w // 2 and y < h // 2:
                px[x, y] = (30, 160, 220)
            else:                            # 下半：渐变 + 棋盘，产生 AC 系数
                c = 255 if ((x // 3 + y // 3) % 2) else 0
                px[x, y] = ((x * 7) % 256, c, (y * 11) % 256)
    return im

def encode(im, **kw):
    b = io.BytesIO(); im.save(b, 'JPEG', **kw); return b.getvalue()

def markers(data):
    out = []
    i = 2
    while i < len(data) - 1:
        if data[i] == 0xFF and data[i+1] not in (0x00, 0xFF) and not (0xD0 <= data[i+1] <= 0xD7):
            out.append(hex(data[i+1]))
            if data[i+1] == 0xD9: break
            i += 2 + int.from_bytes(data[i+2:i+4], 'big'); continue
        i += 1
    return out

cases = {}
rgb = rgb_image(37, 21)
cases['kRgbBaseline'] = encode(rgb, quality=85, subsampling=2)
cases['kRgbProgressive'] = encode(rgb, quality=85, subsampling=2, progressive=True)
cmyk = Image.merge('CMYK', [rgb.getchannel(0), rgb.getchannel(1), rgb.getchannel(2), rgb.convert('L')]).crop((0, 0, 29, 19))
cases['kCmykBaseline'] = encode(cmyk, quality=90)
cases['kCmykProgressiveRestart'] = encode(cmyk, quality=90, progressive=True, restart_marker_blocks=1)
gray = rgb.convert('L').crop((3, 2, 26, 19))
cases['kGrayBaseline'] = encode(gray, quality=80)
cases['kGrayProgressive'] = encode(gray, quality=80, progressive=True)
for k, v in cases.items():
    print(k, len(v), markers(v).count('0xda'), 'scans', 'DRI' if '0xdd' in markers(v) else '', 'SOF2' if '0xc2' in markers(v) else '')

# 抽样点期望（PIL 解码，纯色区域内部）
dec = Image.open(io.BytesIO(cases['kRgbBaseline'])).convert('RGB')
samples = [(4, 4), (12, 6), (24, 4), (33, 7)]
print('samples', [(x, y, dec.getpixel((x, y))) for x, y in samples])

import pathlib
out = pathlib.Path(__file__).resolve().parent.parent / 'tests' / 'fixtures' / 'JpegFixtures.h'
with open(out, 'w', encoding='utf-8', newline='\r\n') as f:
    f.write('#pragma once\n\n')
    f.write('// 由 tools/gen-jpeg-fixtures.py 生成（Pillow）：原创合成图像的顺序式与渐进式编码。\n')
    f.write('// 两种编码的量化系数相同，解码结果应逐字节一致。\n\n')
    for k, v in cases.items():
        f.write(f'constexpr unsigned char {k}[] = {{\n')
        for i in range(0, len(v), 20):
            f.write('    ' + ', '.join(f'0x{b:02X}' for b in v[i:i+20]) + ',\n')
        f.write('};\n\n')
