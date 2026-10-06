"""Global frame statistics (640x360 downsample): mean luma, 10th/90th luma
percentiles, mean chroma (max-min channel), share of pixels with luma < 20.
Usage: frame_stats.py img...
"""
import sys
import numpy as np
from PIL import Image
def st(p):
    a = np.asarray(Image.open(p).convert('RGB').resize((640,360))).astype(float)
    lum = (0.299*a[...,0]+0.587*a[...,1]+0.114*a[...,2])
    ch = a.max(-1)-a.min(-1)
    return lum.mean(), np.percentile(lum,10), np.percentile(lum,90), ch.mean(), (lum<20).mean()*100
for p in sys.argv[1:]:
    m,p10,p90,c,dk = st(p)
    print("%-55s lum=%5.1f p10=%5.1f p90=%5.1f chroma=%5.1f dark%%=%5.1f" % (p[-55:], m,p10,p90,c,dk))
