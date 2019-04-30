#!/usr/bin/env python3

import sys, os

# sudo pip3 install opencv-python
import cv2

from PIL import Image
import numpy

# When we "import enhance" below it depends on the following:
# sudo pip3 install scipy theano lasagne
# sudo pip3 install --upgrade https://github.com/Lasagne/Lasagne/archive/master.zip
  

if __name__ == '__main__':
  
  cam = cv2.VideoCapture(0)
  # Array holds numpy.asarray(Image.fromarray(cv2.cvtColor(img,cv2.COLOR_BGR2RGB)))
  want_null_img = True
  null_img = None
  
  while True:
    ret_val, img = cam.read()
    # Simple image scale up
    #img = cv2.resize(img, (0,0), fx=2.0, fy=2.0)
    
    # np_img = numpy.asarray(
    #   Image.fromarray(
    #     cv2.cvtColor(img,cv2.COLOR_BGR2RGB)
    #   )
    # )
    # # Increase possible resolution
    # np_img = np_img.astype('uint32')
    
    new_pil_img = Image.fromarray(img.astype('uint8'))
    # Go from RGB to BGR
    new_pil_img = new_pil_img.convert('RGB')
    open_cv_image = numpy.array(new_pil_img)
    
    if want_null_img:
      null_img = open_cv_image
      want_null_img = False
    
    print("open_cv_image.shape = {}".format(open_cv_image.shape))
    
    # Now rotoscope
    for x in range(0, open_cv_image.shape[1]-1):
      for y in range(0, open_cv_image.shape[0]-1):
        # if red ([0]) similar zero open_cv_image[x,y,0]
        if abs(open_cv_image[y,x,0] - null_img[y,x,0]) < 10:
          open_cv_image[y,x,0] = 0
      
    cv2.imshow('rotocamcast', open_cv_image)
    
    the_key = cv2.waitKey(1)
    if the_key == 27: 
      break  # esc to quit
    elif the_key == 32:
      # space to cap nullframe
      want_null_img = False
  
  cv2.destroyAllWindows()
  
  

