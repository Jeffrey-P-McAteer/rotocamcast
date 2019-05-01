
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
#include <pthread.h>
#include <sys/time.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>

// Config tuneables
#define WIDTH (320*2)
#define HEIGHT (240*2)
#define WIN_X1 1100
#define WIN_Y1 600
#define FRAMECAP_SLEEP_MS 50
#define XPROCESSING_SLEEP_MS 50
#define NULLFRAME_CAP_MS 1500
#define NO_DRAW_MAX_MS 150
#define WIN_DRAW_INITIAL_DELAY_MS 750

// Global variables
int shutdown_flag = 0;
Display* display;
Window win;
GC gc;

pthread_t camera_t_id;

struct v4l2_buffer bufferinfo;
char* camera_frame_buffer;
char* upper_null_frame;
char* lower_null_frame;

unsigned long nullframe_end_ms = 0;
unsigned long last_draw_ms = 0;

void env_setup() {
  system("pgrep compton || i3-msg exec compton");
  system("v4l2-ctl "
          "--set-ctrl=brightness=150 "
          "--set-ctrl=contrast=150 "
          "--set-ctrl=saturation=150 "
          "--set-ctrl=white_balance_automatic=0 "
  );
}

void env_teardown() {
  system("pkill compton");
}

unsigned long now_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

char get_y_(int x, int y, char* buffer, int max) {
  int o = (x*2) + (y * WIDTH * 2);
  //int o = (x) + (y * WIDTH);
  if (o < max) {
    return *(buffer + o);
  }
  return 0;
}

char get_cr_(int x, int y, char* buffer, int max) {
  int o = (x*2) + (y * WIDTH * 2) + 1;
  //int o = (x) + (y * WIDTH);
  if (o < max) {
    return *(buffer + o);
  }
  return 0;
}

char get_cb_(int x, int y, char* buffer, int max) {
  int o = (x*2) + (y * WIDTH * 2) + 3;
  //int o = (x) + (y * WIDTH);
  if (o < max) {
    return *(buffer + o);
  }
  return 0;
}

Bool should_be_trans(char* frame_buffer, char* lower_buffer, char* upper_buffer, int x, int y, int max) {
  return (
    //(get_y_(x, y, frame_buffer, max) < get_y_(x, y, upper_buffer, max) && get_y_(x, y, frame_buffer, max) > get_y_(x, y, lower_buffer, max)) || 
    (get_cr_(x, y, frame_buffer, max) < get_cr_(x, y, upper_buffer, max) && get_cr_(x, y, frame_buffer, max) > get_cr_(x, y, lower_buffer, max)) || 
    (get_cb_(x, y, frame_buffer, max) < get_cb_(x, y, upper_buffer, max) && get_cb_(x, y, frame_buffer, max) > get_cb_(x, y, lower_buffer, max))
  );
}

void* capture_camera_thread(void* vargp) {
  int fd;
  if ((fd = open("/dev/video0", O_RDWR)) < 0) {
    perror("open");
    exit(1);
  }

  struct v4l2_format format;
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  // v4l2-ctl --list-formats-ext
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  format.fmt.pix.width = WIDTH;
  format.fmt.pix.height = HEIGHT;

  if (ioctl(fd, VIDIOC_S_FMT, &format) < 0) {
    perror("VIDIOC_S_FMT");
    exit(1);
  }

  struct v4l2_requestbuffers bufrequest;
  bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  bufrequest.memory = V4L2_MEMORY_MMAP;
  bufrequest.count = 1;

  if (ioctl(fd, VIDIOC_REQBUFS, &bufrequest) < 0) {
    perror("VIDIOC_REQBUFS");
    exit(1);
  }

  memset(&bufferinfo, 0, sizeof(bufferinfo));

  bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  bufferinfo.memory = V4L2_MEMORY_MMAP;
  bufferinfo.index = 0;

  if (ioctl(fd, VIDIOC_QUERYBUF, &bufferinfo) < 0) {
    perror("VIDIOC_QUERYBUF");
    exit(1);
  }

  camera_frame_buffer = (char*) mmap(
    NULL,
    bufferinfo.length,
    PROT_READ | PROT_WRITE,
    MAP_SHARED,
    fd,
    bufferinfo.m.offset
  );
  
  upper_null_frame = malloc(bufferinfo.length * 1);
  lower_null_frame = malloc(bufferinfo.length * 1);

  if ((void*) camera_frame_buffer == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  memset((void*) camera_frame_buffer, 0, bufferinfo.length);

  // Put the buffer in the incoming queue.
  if(ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
    perror("VIDIOC_QBUF");
    exit(1);
  }

  // Activate streaming
  int type = bufferinfo.type;
  if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    perror("VIDIOC_STREAMON");
    exit(1);
  }
  
  while (!shutdown_flag) {
    // Dequeue the buffer.
    if (ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0) {
      perror("VIDIOC_QBUF");
      exit(1);
    }
    bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufferinfo.memory = V4L2_MEMORY_MMAP;
    // Performance optimization: we set nullframe_end_ms=0 when we are done, thereby avoiding
    if (nullframe_end_ms > 100) {
      unsigned long now = now_ms();
      if (now < nullframe_end_ms) {
        // we are using this to setup our null frame
        printf("Capturing nullframe...\n");
        
        
        
      }
      else { // we have gone past the time, set to 0 to avoid time lookup cost from now_ms()
        nullframe_end_ms = 0;
      }
    }
    // Queue the next buffer fetch
    if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0) {
      perror("VIDIOC_QBUF");
      exit(1);
    }
    usleep(FRAMECAP_SLEEP_MS * 1000);
  }
  
  // Deactivate streaming
  if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
    perror("VIDIOC_STREAMOFF");
    exit(1);
  }
  // Close camera
  close(fd);
  
  return NULL;
}

// Sets upper_null_frame to 0 and lower_null_frame to 255
void reset_nullframe() {
  memset(lower_null_frame, 255, bufferinfo.length);
  memset(upper_null_frame, 0, bufferinfo.length);
}

int main(int argc, char** argv) {
  env_setup();
  
  pthread_create(&camera_t_id, NULL, capture_camera_thread, NULL);
  
  
  display = XOpenDisplay(0);
  
  XVisualInfo visualinfo;
  XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &visualinfo);
  
  XSetWindowAttributes attr;
  attr.colormap   = XCreateColormap( display, DefaultRootWindow(display), visualinfo.visual, AllocNone) ;
  attr.event_mask = ExposureMask | KeyPressMask ;
  attr.background_pixmap = None ;
  attr.border_pixel     = 0;
  win = XCreateWindow(display, DefaultRootWindow(display),
    WIN_X1, WIN_Y1, WIDTH, HEIGHT,
    0,
    visualinfo.depth,
    InputOutput,
    visualinfo.visual,
    CWColormap|CWEventMask|CWBackPixmap|CWBorderPixel,
    &attr
  );
  gc = XCreateGC(display, win, 0, 0);
  
  XStoreName(display, win, "rotocamcast");
  XMapWindow(display, win);
  
  XSetBackground(display, gc, 0UL);
  XSetForeground(display, gc, ~0UL);
  
  unsigned long app_start_ms = now_ms();
  
  while (!shutdown_flag) {
    unsigned long ms = now_ms();
    int want_redraw = 0;
    
    while (XPending(display) > 0) {
      XEvent event;
      XNextEvent(display, &event);
      
      switch(event.type) {  // see 'man XAnyEvent' for a list of available events
        case ClientMessage:
          // check if the client message was send by window manager to indicate user wants to close the window
          if ( event.xclient.message_type  == XInternAtom( display, "WM_PROTOCOLS", 1)
            && event.xclient.data.l[0]  == XInternAtom( display, "WM_DELETE_WINDOW", 1) )
          {
            shutdown_flag = 1;
          }
        case KeyPress:
          if (XLookupKeysym(&event.xkey, 0) == XK_Escape || XLookupKeysym(&event.xkey, 0) == 'q') {
            shutdown_flag = 1;
          }
          else if (XLookupKeysym(&event.xkey, 0) == 'r') {
            reset_nullframe();
            nullframe_end_ms = ms + NULLFRAME_CAP_MS;
          }
          break;
        case Expose:
          want_redraw = 1;
          break;
        default:
          // do no thing
          break;
      }
    }
    // Finished processing X events
    
    if (!want_redraw) {
      // Check if we should redraw based on time
      if (ms - last_draw_ms > NO_DRAW_MAX_MS) {
        want_redraw = 1;
      }
    }
    
    // If not yet mapped...
    if (ms - app_start_ms < WIN_DRAW_INITIAL_DELAY_MS) {
      continue;
    }
    
    if (want_redraw) {
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
      
      XSetFunction(display, gc, GXandInverted);
      XFillRectangle(display, win, gc, 0, 0, win_width, win_height);
      XSetFunction(display, gc, GXor);
      
      XImage* frame_img = XGetImage(display, win, 0, 0, win_width, win_height, AllPlanes, ZPixmap);
      
      for (int y=1; y<HEIGHT-1; y++) {
        for (int x=1; x<WIDTH-1; x++) {
          char y1 = get_y_(x, y, camera_frame_buffer, bufferinfo.length);
          unsigned long pixel_val = (0xff << 24) + (y1 << 16) + (y1 << 8) + (y1 << 0);
          XPutPixel(frame_img, x, y, pixel_val);
          
        }
      }
      
      XPutImage(display, win, gc, frame_img, 0, 0, 0, 0, win_width, win_height);
      
      if (win_width != WIDTH || win_height != HEIGHT) {
        XMoveResizeWindow(display, win, win_x, win_y, WIDTH, HEIGHT);
      }
      
      XSync(display, False);
      
      last_draw_ms = ms;
      
      usleep(XPROCESSING_SLEEP_MS * 1000);
      
    }
    
  }
  
  XDestroyWindow(display, win);
  win = 0;
  XCloseDisplay(display);
  display = 0;
  
  pthread_join(camera_t_id, NULL);
  
  env_teardown();
}
