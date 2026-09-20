"""Visual motion alarm, not a metric rig pose or an odometry substitute.

Background feature motion can invalidate a fixed registration. A quiet image
cannot prove the rig is stationary, and never automatically restores guidance.
"""
class RigMotionMonitor:
    def __init__(self):
        self.previous=None
        self.previous_boxes=[]
        self.reference=None
        self.streak=0
        self.latched=False

    def update(self,gray,boxes,reference):
        import cv2
        import numpy as np
        if reference!=self.reference:
            self.previous=None;self.streak=0;self.latched=False;self.reference=reference
        result=dict(status='unobserved',motion_alarm=self.latched,metric_pose_available=False)
        if self.previous is not None and self.previous.shape==gray.shape:
            mask=np.full(gray.shape,255,np.uint8)
            for box in self.previous_boxes:
                x1,y1,x2,y2=(int(x) for x in box)
                cv2.rectangle(mask,(max(0,x1-20),max(0,y1-20)),(x2+20,y2+20),0,-1)
            features=cv2.goodFeaturesToTrack(self.previous,100,.015,12,mask=mask)
            if features is not None and len(features)>=15:
                moved,status,_=cv2.calcOpticalFlowPyrLK(self.previous,gray,features,None)
                if moved is not None:
                    valid=status.ravel()==1
                    old,new=features[valid],moved[valid]
                    for box in boxes:
                        x1,y1,x2,y2=box
                        keep=~((new[:,0,0]>=x1-20)&(new[:,0,0]<=x2+20)&(new[:,0,1]>=y1-20)&(new[:,0,1]<=y2+20))
                        old,new=old[keep],new[keep]
                    if len(old)>=15:
                        matrix,inliers=cv2.estimateAffinePartial2D(old,new,method=cv2.RANSAC,ransacReprojThreshold=2)
                        if matrix is not None and inliers.sum()>=12 and inliers.mean()>=.65:
                            distance=float(np.median(np.linalg.norm(new[inliers.ravel()==1]-old[inliers.ravel()==1],axis=2)))
                            changed=distance>max(2.5,gray.shape[1]*.004)
                            self.streak=self.streak+1 if changed else 0
                            self.latched|=self.streak>=2
                            result=dict(status='background_motion' if changed else 'no_motion_alarm',
                                motion_alarm=self.latched,median_flow_px=distance,
                                background_features=int(inliers.sum()),metric_pose_available=False)
        if result['status']=='unobserved':
            self.streak=0
        self.previous=gray.copy();self.previous_boxes=list(boxes)
        return result
