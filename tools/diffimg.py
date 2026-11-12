import sys
from PIL import Image, ImageChops
a=Image.open(sys.argv[1]).convert('RGB'); b=Image.open(sys.argv[2]).convert('RGB')
d=ImageChops.difference(a,b).convert('L').point(lambda v:255 if v else 0)
w,h=a.size; o=Image.new('RGB',(w*3+20,h),(40,40,40))
o.paste(a,(0,0)); o.paste(b,(w+10,0)); o.paste(Image.merge('RGB',(d,d,d)),(2*w+20,0))
o=o.resize((o.width*2,o.height*2),Image.NEAREST); o.save(sys.argv[3])
