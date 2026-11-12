import sys, glob, os
from PIL import Image, ImageDraw
# usage: montage.py out.png img1 img2 ... (labels = parent dir/file)
out=sys.argv[1]; files=sys.argv[2:]
ims=[Image.open(f).convert('RGB') for f in files]
W=max(i.width for i in ims); H=max(i.height for i in ims)
cols=min(len(ims),4); rows=(len(ims)+cols-1)//cols
sheet=Image.new('RGB',(cols*(W*2+8),rows*(H*2+22)),(40,40,40))
d=ImageDraw.Draw(sheet)
for k,(im,f) in enumerate(zip(ims,files)):
    x=(k%cols)*(W*2+8); y=(k//cols)*(H*2+22)
    sheet.paste(im.resize((im.width*2,im.height*2),Image.NEAREST),(x,y+18))
    d.text((x+2,y+2),os.path.basename(os.path.dirname(f))+'/'+os.path.basename(f),fill=(255,255,0))
sheet.save(out)
