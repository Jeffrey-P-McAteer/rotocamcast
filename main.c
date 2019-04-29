/*
      ____      _____
    /\__  \   /\  ___\
    \/__/\ \  \ \ \__/_
        \ \ \  \ \____ \
        _\_\ \  \/__/_\ \
      /\ _____\  /\ _____\
      \/______/  \/______/

   Copyright (C) 2011 Joerg Seebohn

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program demonstrates how an X11 window with OpenGL support
   can be drawn transparent.

   The title bar and window border drawn by the window manager are
   drawn opaque.
   Only the background of the window which is drawn with OpenGL
         glClearColor( 0.7, 0.7, 0.7, 0.7) ;
         glClear(GL_COLOR_BUFFER_BIT) ;
   is 30% transparent.

   Compile it with: 
     gcc -std=gnu99 -o test testprogram.c -lX11 -lGL
*/
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>

#define FRAMERATE_SKIP 3

int main(int argc, char** argv) {
   // First ensure a compositor is running
   system("pgrep compton || i3-msg exec compton");
   
   // Now make a transparent GUI window
   Display    * display = XOpenDisplay( 0 ) ;
   const char * xserver = getenv( "DISPLAY" ) ;

   if (display == 0)
   {
      printf("Could not establish a connection to X-server '%s'\n", xserver ) ;
      exit(1) ;
   }

   // query Visual for "TrueColor" and 32 bits depth (RGBA)
   XVisualInfo visualinfo ;
   XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &visualinfo);
   
   // create window
   Window   win ;
   GC       gc ;
   XSetWindowAttributes attr ;
   attr.colormap   = XCreateColormap( display, DefaultRootWindow(display), visualinfo.visual, AllocNone) ;
   attr.event_mask = ExposureMask | KeyPressMask ;
   attr.background_pixmap = None ;
   attr.border_pixel      = 0 ;
   win = XCreateWindow(    display, DefaultRootWindow(display),
                           50, 300, 400, 100, // x,y,width,height : are possibly opverwriteen by window manager
                           0,
                           visualinfo.depth,
                           InputOutput,
                           visualinfo.visual,
                           CWColormap|CWEventMask|CWBackPixmap|CWBorderPixel,
                           &attr
                           ) ;
   gc = XCreateGC( display, win, 0, 0) ;

   // set title bar name of window
   XStoreName( display, win, "Transparent Window with OpenGL Support" ) ;

   // say window manager which position we would prefer
   XSizeHints sizehints ;
   sizehints.flags = PPosition | PSize ;
   sizehints.x     = 50 ;  sizehints.y = 300 ;
   sizehints.width = 400 ; sizehints.height = 100 ;
   XSetWMNormalHints( display, win, &sizehints ) ;
   // Switch On >> If user pressed close key let window manager only send notification >>
   Atom wm_delete_window = XInternAtom( display, "WM_DELETE_WINDOW", 0) ;
   XSetWMProtocols( display, win, &wm_delete_window, 1) ;
   
   // create OpenGL context
   GLXContext glcontext = glXCreateContext( display, &visualinfo, 0, True ) ;
   if (!glcontext)
   {
      printf("X11 server '%s' does not support OpenGL\n", xserver ) ;
      exit(1) ;
   }
   glXMakeCurrent( display, win, glcontext ) ;

   // now let the window appear to the user
   XMapWindow( display, win) ;

   int isUserWantsWindowToClose = 0 ;
   
   // Now get a camera
   int fd;
   if ((fd = open("/dev/video0", O_RDWR)) < 0){
      perror("open");
      exit(1);
   }
   
   struct v4l2_format format;
   format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
   // v4l2-ctl --list-formats-ext
   format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
   format.fmt.pix.width = 320;
   format.fmt.pix.height = 240;

   if (ioctl(fd, VIDIOC_S_FMT, &format) < 0){
       perror("VIDIOC_S_FMT");
       exit(1);
   }
   
   struct v4l2_requestbuffers bufrequest;
   bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
   bufrequest.memory = V4L2_MEMORY_MMAP;
   bufrequest.count = 1;

   if (ioctl(fd, VIDIOC_REQBUFS, &bufrequest) < 0){
       perror("VIDIOC_REQBUFS");
       exit(1);
   }

   struct v4l2_buffer bufferinfo;
   memset(&bufferinfo, 0, sizeof(bufferinfo));
    
   bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
   bufferinfo.memory = V4L2_MEMORY_MMAP;
   bufferinfo.index = 0;
    
   if (ioctl(fd, VIDIOC_QUERYBUF, &bufferinfo) < 0){
       perror("VIDIOC_QUERYBUF");
       exit(1);
   }

   void* buffer_start = mmap(
       NULL,
       bufferinfo.length,
       PROT_READ | PROT_WRITE,
       MAP_SHARED,
       fd,
       bufferinfo.m.offset
   );
   
   if (buffer_start == MAP_FAILED){
       perror("mmap");
       exit(1);
   }
   
   memset(buffer_start, 0, bufferinfo.length);

   // Put the buffer in the incoming queue.
   if(ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0){
       perror("VIDIOC_QBUF");
       exit(1);
   }


   // Activate streaming
   int type = bufferinfo.type;
   if (ioctl(fd, VIDIOC_STREAMON, &type) < 0){
       perror("VIDIOC_STREAMON");
       exit(1);
   }


   // Begin main event loop; read frames and paint pixels
   int frame = 0;
   while( !isUserWantsWindowToClose ) {
      int got_new_frame = 0;
      frame++;
      if (frame % FRAMERATE_SKIP == 0) {
         got_new_frame = 1;
         // Dequeue the buffer.
         if(ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0){
            perror("VIDIOC_QBUF");
            exit(1);
         }
         bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
         bufferinfo.memory = V4L2_MEMORY_MMAP;
      }
      
      int  isRedraw = 0 ;

      /* XPending returns number of already queued events.
       * If no events are queued XPending sends all queued requests to the X-server
       * and tries to read new incoming events. */

      while( XPending(display) > 0 )
      {
         // process event
         XEvent   event ;
         XNextEvent( display, &event) ;

         switch(event.type)
         {  // see 'man XAnyEvent' for a list of available events
         case ClientMessage:
                  // check if the client message was send by window manager to indicate user wants to close the window
                  if (  event.xclient.message_type  == XInternAtom( display, "WM_PROTOCOLS", 1)
                        && event.xclient.data.l[0]  == XInternAtom( display, "WM_DELETE_WINDOW", 1)
                        )
                  {
                     isUserWantsWindowToClose = 1 ;
                  }
         case KeyPress:
                  if (XLookupKeysym(&event.xkey, 0) == XK_Escape || XLookupKeysym(&event.xkey, 0) == 'q')
                  {
                     isUserWantsWindowToClose = 1 ;
                  }
                  break ;
         case Expose:
                  isRedraw = 1 ;
                  break ;
         default:
               // do no thing
               break ;
         }

      }

      // ... all events processed, now do other stuff ...

      if (isRedraw || got_new_frame == 1) {
         // use opengl to clear background in (transparent) light grey
         glClearColor( 0.0, 0.0, 0.0, 0.0);
         glClear(GL_COLOR_BUFFER_BIT);
         
         glMatrixMode(GL_PROJECTION);
         glLoadIdentity();
         gluOrtho2D(0.0, 500.0, 500.0,0.0);
         
         glBegin(GL_POINTS);
         // draw pixels from buffer
         for (int x=0; x < 100; x++) {
            for (int y=0; y<100; y++) {
               glColor3f(1,1,1);
               glVertex2i(x,y);
            }
         }
         glEnd();
         
         glXSwapBuffers(display, win);
         glXWaitGL();
      }

      
      if (frame % FRAMERATE_SKIP == 0) {
         // Queue the next one.
         if(ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0){
            perror("VIDIOC_QBUF");
            exit(1);
         }
      }

   }

   XDestroyWindow( display, win ) ; 
   win = 0 ;
   XCloseDisplay( display ) ; 
   display = 0 ;

   // Deactivate streaming
   if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0){
       perror("VIDIOC_STREAMOFF");
       exit(1);
   }
   // Close camera
   close(fd);

   return 0 ;
}
