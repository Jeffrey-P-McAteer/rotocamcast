
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

#define FRAMERATE_SKIP 2
#define WIDTH (320*2)
#define HEIGHT (240*2)
// #define WIDTH (1280)
// #define HEIGHT (720)
#define X1 1100
#define Y1 600
#define NULLSPACE_SAMPLE_FRAMES 60
#define NULLSPACE_RADIUS 0

char get_y_(int x, int y, char* buffer, int max);
char get_cr_(int x, int y, char* buffer, int max);
char get_cb_(int x, int y, char* buffer, int max);

int main(int argc, char** argv) {
   // First ensure a compositor is running
   system("pgrep compton || i3-msg exec compton");
   
   // Also ensure facetimehd driver doesn't try any auto-anything
   system("v4l2-ctl "
          "--set-ctrl=brightness=150 "
          "--set-ctrl=contrast=100 "
          "--set-ctrl=saturation=100 "
          // "--set-ctrl=white_balance_temperature_auto=0 "
          // "--set-ctrl=gain=90 "
          // "--set-ctrl=power_line_frequency=1 "
          // "--set-ctrl=white_balance_temperature=1140 "
          // "--set-ctrl=sharpness=24 "
          // "--set-ctrl=backlight_compensation=1 "
          // "--set-ctrl=exposure_auto=1 "
          // "--set-ctrl=exposure_absolute=870 "
          // "--set-ctrl=exposure_auto_priority=1"
    );
   
   // Now make a transparent GUI window
   Display    * display = XOpenDisplay( 0 ) ;
   const char * xserver = getenv( "DISPLAY" ) ;

   if (display == 0) {
      printf("Could not establish a connection to X-server '%s'\n", xserver);
      exit(1);
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
   attr.border_pixel     = 0;
   win = XCreateWindow(    display, DefaultRootWindow(display),
                           X1, Y1, WIDTH, HEIGHT, // x,y,width,height : are possibly opverwriteen by window manager
                           0,
                           visualinfo.depth,
                           InputOutput,
                           visualinfo.visual,
                           CWColormap|CWEventMask|CWBackPixmap|CWBorderPixel,
                           &attr
                           ) ;
   gc = XCreateGC( display, win, 0, 0) ;
  
   // set title bar name of window
   XStoreName( display, win, "rotocamcast" ) ;

   // say window manager which position we would prefer
   XSizeHints sizehints ;
   sizehints.flags = PPosition | PSize ;
   sizehints.x     = X1 ;  sizehints.y = Y1 ;
   sizehints.width = WIDTH ; sizehints.height = HEIGHT ;
   XSetWMNormalHints( display, win, &sizehints ) ;
   // Switch On >> If user pressed close key let window manager only send notification >>
   Atom wm_delete_window = XInternAtom( display, "WM_DELETE_WINDOW", 0) ;
   XSetWMProtocols( display, win, &wm_delete_window, 1) ;

   XColor transparent_color;
   transparent_color.red = 0 * 255; // x11 uses 16 bit colors, so 128 becomes  (128 * 256).
   transparent_color.green = 0 * 256;
   transparent_color.blue = 0 * 256;
   transparent_color.flags = (!DoRed) | (!DoGreen) | (!DoBlue);
   XAllocColor(display, attr.colormap, &transparent_color);
   
   XColor white;
   white.red = 256 * 255; // x11 uses 16 bit colors, so 128 becomes  (128 * 256).
   white.green = 256 * 255;
   white.blue = 256 * 255;
   white.flags = DoRed | DoGreen | DoBlue;
   XAllocColor(display, attr.colormap, &white);


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

   // just a memcpy() of frame number 10
   char* upper_null_frame = malloc(bufferinfo.length * 1);
   char* lower_null_frame = malloc(bufferinfo.length * 1);
   

   // Begin main event loop; read frames and paint pixels
   int frame = 0;
   int nullframe_begin = -1;
   int nullframe_end = -1;
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
         
         if (frame > nullframe_begin && frame < nullframe_end) {
            // Iterate buffer copying lowest values to lower_null_frame and highest to upper_null_frame
            for (int i=0; i<bufferinfo.length; i++) {
              char dis_val = *((char*) buffer_start + i);
              if (dis_val < lower_null_frame[i]) {
                char low_val = dis_val;
                if (low_val > NULLSPACE_RADIUS) {
                  low_val -= NULLSPACE_RADIUS;
                }
                lower_null_frame[i] = low_val;
              }
              if (dis_val > upper_null_frame[i]) {
                char high_val = dis_val;
                if (high_val < 256-NULLSPACE_RADIUS) {
                  high_val += NULLSPACE_RADIUS;
                }
                upper_null_frame[i] = high_val;
              }
            }
            if (frame < nullframe_end) {
              printf("nullframed at %d\n", frame);
            }
         }
         
      }
      
      XSetBackground(display, gc, 0UL);
      XSetForeground(display, gc, ~0UL);
       
      
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
                  if (XLookupKeysym(&event.xkey, 0) == XK_Escape || XLookupKeysym(&event.xkey, 0) == 'q') {
                     isUserWantsWindowToClose = 1 ;
                  }
                  else if (XLookupKeysym(&event.xkey, 0) == 'r') {
                     printf("Forcing a nullframe reload over approx. 2 seconds...\n");
                     memset(lower_null_frame, 255, bufferinfo.length);
                     memset(upper_null_frame, 0, bufferinfo.length);
                     nullframe_begin = frame + 5;
                     nullframe_end = frame + 5 + NULLSPACE_SAMPLE_FRAMES;
                     
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
         
         int nonsense = 0;
         Window more_nonsense;
         
         int win_x = 0;
         int win_y = 0;
         int win_width = 0;
         int win_height = 0;
         XGetGeometry(display, win, &more_nonsense,
            &win_x, &win_y, // x,y
            &win_width, &win_height, // width, height
            &nonsense, &nonsense // border_w, color depth
         );
         
         //XClearArea(display, win, 0, 0, win_width, win_height, False);
         //XClearWindow(display, win);
         XRectangle rectangles[1] = {
          {0, 0, win_width, win_height}
         };
         
         XSetFunction(display, gc, GXandInverted);
         XFillRectangles(display, win, gc, rectangles, 1);
         XSetFunction(display, gc, GXor);
         
         //XSetForeground(display, gc, white.pixel);
         
         XImage* frame_img = XGetImage(display, win, 0, 0, win_width, win_height, AllPlanes, ZPixmap);
         
         for (int y=0; y<HEIGHT; y++) {
          for (int x=0; x<WIDTH; x++) {
            
            // // Testing
            // if (x < y-75) {
            //   //XDrawPoint(display, win, gc, x,y);
            //   // xputpixel seems to use AABBGGRR
            //   XPutPixel(frame_img, x, y, 0x00ff00ff);
            // }
            
            char y1 = get_y_(x, y, (char*) buffer_start, bufferinfo.length);
            char cr = get_cr_(x, y, (char*) buffer_start, bufferinfo.length);
            char cb = get_cb_(x, y, (char*) buffer_start, bufferinfo.length);
            
            char l_n_y1 = get_y_(x, y, lower_null_frame, bufferinfo.length);
            char l_n_cr = get_cr_(x, y, lower_null_frame, bufferinfo.length);
            char l_n_cb = get_cb_(x, y, lower_null_frame, bufferinfo.length);
            
            char u_n_y1 = get_y_(x, y, upper_null_frame, bufferinfo.length);
            char u_n_cr = get_cr_(x, y, upper_null_frame, bufferinfo.length);
            char u_n_cb = get_cb_(x, y, upper_null_frame, bufferinfo.length);
            
            // If croma is within lower and higher values, this pixel is transparent
            if ( (cr > l_n_cr && cr < u_n_cr) ||
                 (cb > l_n_cb && cb < u_n_cb)
            ) {
              continue;
            }
            
            // xputpixel seems to use AABBGGRR
            // char r = (char) ( (1.164 * (double) (y1-16) ) + (2.018 * (double) (cr-128)) );
            // char g = (char) ( (1.164 * (double) (y1-16) ) - (0.813 * (double) (cb-128)) + (0.391 * (double) (cr-128)) );
            // char b = (char) ( (1.164 * (double) (y1-16) ) + (1.596 * (double)  (cb-128)));
            //unsigned long pixel_val = (b << 16) + (g << 8) + (r << 0);
            unsigned long pixel_val = (0xff << 24) + (y1 << 16) + (y1 << 8) + (y1 << 0);
            XPutPixel(frame_img, x, y, pixel_val);
            
          }
         }
         
         XPutImage(display, win, gc, frame_img, 0, 0, 0, 0, win_width, win_height);
         
         if (win_width != WIDTH || win_height != HEIGHT) {
          XMoveResizeWindow(display, win, win_x, win_y, WIDTH, HEIGHT);
         }
         
         XSync(display, False);
      }

      
      if (frame % FRAMERATE_SKIP == 0) {
         // Queue the next one.
         if(ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
            perror("VIDIOC_QBUF");
            exit(1);
         }
      }
      
      // Sleep 10ms at and of loop
      usleep(5 * 1000);

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

char get_y_(int x, int y, char* buffer, int max) {
  // LAME
  x = x / 2;
  y = y / 2;
  int o = (x*2) + (y * WIDTH);
  //int o = (x) + (y * WIDTH);
  if (o < max) {
    return *(buffer + o);
  }
  return 0;
}

char get_cr_(int x, int y, char* buffer, int max) {
  // LAME
  x = x / 2;
  y = y / 2;
  int o = (x*2) + (y * WIDTH) + 1;
  //int o = (x) + (y * WIDTH);
  if (o < max) {
    return *(buffer + o);
  }
  return 0;
}

char get_cb_(int x, int y, char* buffer, int max) {
  // LAME
  x = x / 2;
  y = y / 2;
  int o = (x*2) + (y * WIDTH) + 3;
  //int o = (x) + (y * WIDTH);
  if (o < max) {
    return *(buffer + o);
  }
  return 0;
}

