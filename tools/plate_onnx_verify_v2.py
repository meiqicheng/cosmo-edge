#!/usr/bin/env python3
import argparse,json
from pathlib import Path
import cv2,numpy as np,onnxruntime as ort

CHARS = "#\u4eac\u6caa\u6d25\u6e1d\u5180\u664b\u8499\u8fbd\u5409\u9ed1\u82cf\u6d59\u7696\u95fd\u8d63\u9c81\u8c6b\u9102\u6e58\u7ca4\u6842\u743c\u5ddd\u8d35\u4e91\u85cf\u9655\u7518\u9752\u5b81\u65b0\u5b66\u8b66\u6e2f\u6fb3\u6302\u4f7f\u9886\u6c11\u822a\u5371\u0030\u0031\u0032\u0033\u0034\u0035\u0036\u0037\u0038\u0039ABCDEFGHJKLMNPQRSTUVWXYZ\u9669\u54c1"
COLORS=["black","blue","green","white","yellow"]

def ctc(ids):
    out=[]; prev=0
    for value in ids:
        value=int(value)
        if value and value != prev: out.append(CHARS[value])
        prev=value
    return ''.join(out)

def main():
    p=argparse.ArgumentParser(); p.add_argument('image',type=Path); p.add_argument('--detector',required=True,type=Path); p.add_argument('--recognizer',required=True,type=Path); p.add_argument('--threshold',type=float,default=.35); a=p.parse_args()
    image=cv2.imread(str(a.image))
    detector=ort.InferenceSession(str(a.detector),providers=['CPUExecutionProvider']); recognizer=ort.InferenceSession(str(a.recognizer),providers=['CPUExecutionProvider'])
    h,w=image.shape[:2]; scale=min(640/w,640/h); nw,nh=round(w*scale),round(h*scale); left,top=(640-nw)//2,(640-nh)//2; padded=np.full((640,640,3),114,np.uint8); padded[top:top+nh,left:left+nw]=cv2.resize(image,(nw,nh)); tensor=padded.transpose(2,0,1).astype(np.float32)[None]/255.
    rows=np.asarray(detector.run(None,{detector.get_inputs()[0].name:tensor})[0])[0]; result=[]
    for row in rows:
        if float(row[4])<a.threshold: continue
        points=row[6:14].reshape(4,2).astype(np.float32); points[:,0]=(points[:,0]-left)/scale; points[:,1]=(points[:,1]-top)/scale; pw=max(32,int(np.linalg.norm(points[1]-points[0]))); ph=max(16,int(np.linalg.norm(points[2]-points[1]))); destination=np.array([[0,0],[pw-1,0],[pw-1,ph-1],[0,ph-1]],np.float32); roi=cv2.warpPerspective(image,cv2.getPerspectiveTransform(points,destination),(pw,ph));
        if int(row[5])==1: roi=np.concatenate([cv2.resize(roi[:int(ph*.6)],(pw//2,ph)),cv2.resize(roi[int(ph*.4):],(pw//2,ph))],1)
        x=cv2.resize(roi,(168,48)).astype(np.float32); x=(x/255.-.588)/.193; o,c=recognizer.run(None,{recognizer.get_inputs()[0].name:x.transpose(2,0,1)[None]}); ids=np.argmax(o,axis=-1)[0].tolist(); probs=np.asarray(c)[0]; probs=np.exp(probs-probs.max()); probs/=probs.sum(); ci=int(probs.argmax()); result.append({'score':float(row[4]),'class':int(row[5]),'plate':ctc(ids),'plateColor':COLORS[ci],'colorScore':float(probs[ci]),'ocrIndices':ids,'points':points.tolist()})
    print(json.dumps({'image':str(a.image),'detections':result},ensure_ascii=True,indent=2))

if __name__=='__main__': main()
