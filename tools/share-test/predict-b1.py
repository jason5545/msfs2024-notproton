"""predict-b1.py <grab.ppm>: checks the arm64ec yuv2rgb_neon.S register clash in a BGRA frame.
For each 16-pixel group, the B byte of pixels 0-7 should equal sqrshrun(v10.8h, #1) where v10 holds the R bytes of
pixels 8-15 packed into the low 64 bits (upper 64 bits zero): lane i<4 -> int16(R[8+2i] | R[8+2i+1] << 8), lane i>=4 -> 0."""
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); w, h = im.size; px = im.tobytes()
def sat(v):
    v = (v + 1) >> 1           # rounding shift right by 1 (sqrshrun #1)
    return 0 if v < 0 else 255 if v > 255 else v
ok = bad = 0; ok8 = bad8 = 0; ex = []
for y in range(0, h, 3):
    row = px[y * w * 3:(y + 1) * w * 3]
    for g in range(0, w - 15, 16):
        R = [row[(g + 8 + k) * 3] for k in range(8)]
        for i in range(8):
            if i < 4:
                hw = R[2 * i] | (R[2 * i + 1] << 8)
                if hw >= 32768: hw -= 65536
                pred = sat(hw)
            else:
                pred = 0
            b = row[(g + i) * 3 + 2]
            if b == pred: ok += 1
            else:
                bad += 1
                if len(ex) < 5: ex.append((g + i, y, b, pred, R))
print(f"{sys.argv[1]}: pixels 0-7 of each group: B matches the clash prediction {ok}/{ok + bad} ({100.0 * ok / (ok + bad):.4f}%)")
for e in ex: print("  mismatch x=%d y=%d B=%d predicted=%d R2=%s" % e)
