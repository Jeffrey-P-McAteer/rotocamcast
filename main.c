
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdbe.h>

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
#include <dirent.h>

// Config tuneables
#define WIDTH (320*2)
#define HEIGHT (240*2)
#define WIN_X1 1950
#define WIN_Y1 1140
#define FRAMECAP_SLEEP_MS 25
#define XPROCESSING_SLEEP_MS 25
#define NULLFRAME_CAP_MS 4200
#define NO_DRAW_MAX_MS 90
#define WIN_DRAW_INITIAL_DELAY_MS 750

// Global variables
int shutdown_flag = 0;
Display* display;
Window win;
GC gc;
XdbeBackBuffer d_backbuf;

pthread_t camera_t_id;

struct v4l2_buffer bufferinfo;
char* camera_frame_buffer;
char* upper_null_frame;
char* lower_null_frame;

unsigned long nullframe_end_ms = 0;
unsigned long last_draw_ms = 0;
int camera_done_captured = 0;
int recording_desktop_w_ffmpeg = 1;

void env_setup() {
  system("pgrep compton || i3-msg exec compton");
  system("v4l2-ctl "
          "--set-ctrl=brightness=150 "
          "--set-ctrl=contrast=150 "
          "--set-ctrl=saturation=150 "
          "--set-ctrl=white_balance_automatic=0 "
  );
  // enable sticky windows in 2 seconds
  system("( sleep 2 ; i3-msg sticky enable ) & ");
  // Begin screen recording in 3 seconds
  if (recording_desktop_w_ffmpeg == 1) {
    system("[ -e /run/media/jeffrey/VIDEOS/rotograb.avi ] && rm /run/media/jeffrey/VIDEOS/rotograb.avi");
    system("( ffmpeg   -f alsa -ac 2 -i default -itsoffset 00:00:00.5    -f x11grab -s 2560x1600 -framerate 16 -i $DISPLAY -qscale 1 -vcodec libx264 -crf 0 -preset ultrafast /run/media/jeffrey/VIDEOS/rotograb.avi ) & ");
  }
}

void env_teardown() {
  system("pkill compton");
  system("pkill -INT ffmpeg");
  system("pkill -INT ffmpeg");
  system("sync");
  system("ls -alh /run/media/jeffrey/VIDEOS/rotograb.avi");
  
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
    (get_y_(x, y, frame_buffer, max) < get_y_(x, y, upper_buffer, max) && get_y_(x, y, frame_buffer, max) > get_y_(x, y, lower_buffer, max)) && 
    ( (get_cr_(x, y, frame_buffer, max) < get_cr_(x, y, upper_buffer, max) && get_cr_(x, y, frame_buffer, max) > get_cr_(x, y, lower_buffer, max)) ||
      (get_cb_(x, y, frame_buffer, max) < get_cb_(x, y, upper_buffer, max) && get_cb_(x, y, frame_buffer, max) > get_cb_(x, y, lower_buffer, max)) )
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

  { // Attempt to restore previous nullframe
    FILE* fp;
    if (access(".rotocamcast-lastnullframe-lower", F_OK ) != -1 ) {
      fp = fopen(".rotocamcast-lastnullframe-lower", "rb");
      if (fp != NULL) {
        fread(lower_null_frame, 1, bufferinfo.length, fp);
        fclose(fp);
      }
    }
    if (access(".rotocamcast-lastnullframe-upper", F_OK ) != -1 ) {
      fp = fopen(".rotocamcast-lastnullframe-upper", "rb");
      if (fp != NULL) {
        fread(upper_null_frame, 1, bufferinfo.length, fp);
        fclose(fp);
      }
    }
  }

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
    camera_done_captured = 0;
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
        
        // Iterate buffer copying lowest values to lower_null_frame and highest to upper_null_frame
        for (int i=0; i<bufferinfo.length; i++) {
          char dis_val = *(camera_frame_buffer + i);
          if (dis_val < lower_null_frame[i]) {
            char low_val = dis_val - 1;
            lower_null_frame[i] = low_val;
          }
          if (dis_val > upper_null_frame[i]) {
            char high_val = dis_val + 1;
            upper_null_frame[i] = high_val;
          }
        }
        
      }
      else { // we have gone past the time, set to 0 to avoid time lookup cost from now_ms()
        printf("Nullframe DONE\n");
        nullframe_end_ms = 0;
        // Save lower_null_frame to .rotocamcast-lastnullframe-lower
        FILE* fp = fopen(".rotocamcast-lastnullframe-lower", "wb");
        if (fp != NULL) {
          fwrite(lower_null_frame, 1, bufferinfo.length, fp);
          fclose(fp);
        }
        
        // Save upper_null_frame to .rotocamcast-lastnullframe-upper
        fp = fopen(".rotocamcast-lastnullframe-upper", "wb");
        if (fp != NULL) {
          fwrite(upper_null_frame, 1, bufferinfo.length, fp);
          fclose(fp);
        }
      }
    }
    camera_done_captured = 1;
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
  
  DIR* VIDEO_DIR = opendir("/run/media/jeffrey/VIDEOS");
  if (!VIDEO_DIR) {
    printf("Please insert VIDEOS drive to record to\n");
    printf("Continuing WITHOUT desktop recording...\n");
    //exit(1);
    recording_desktop_w_ffmpeg = 0;
  }
  closedir(VIDEO_DIR);
  
  env_setup();
  
  pthread_create(&camera_t_id, NULL, capture_camera_thread, NULL);
  
  
  display = XOpenDisplay(0);
  
  int major, minor;
  if (!XdbeQueryExtension(display, &major, &minor)) {
    printf("Error, cannot perform double buffering using Xdbe.\n");
    exit(5);
  }
  
  int numScreens = 1;
  Drawable screens[] = { DefaultRootWindow(display) };
  XdbeScreenVisualInfo *info = XdbeGetVisualInfo(display, screens, &numScreens);
  if (!info || numScreens < 1 || info->count < 1) {
    fprintf(stderr, "No visuals support Xdbe\n");
    exit(5);
  }
  
  XVisualInfo xvisinfo_templ;
  xvisinfo_templ.visualid = info->visinfo[0].visual; // We know there's at least one
  // As far as I know, screens are densely packed, so we can assume that if at least 1 exists, it's screen 0.
  xvisinfo_templ.screen = 0;
  xvisinfo_templ.depth = info->visinfo[0].depth;

  int matches;
  XVisualInfo *visualinfo = XGetVisualInfo(display, VisualIDMask|VisualScreenMask|VisualDepthMask, &xvisinfo_templ, &matches);

  if (!visualinfo || matches < 1) {
    fprintf(stderr, "Couldn't match a Visual with double buffering\n");
    exit(5);
  }
  
  XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, visualinfo);
  
  XSetWindowAttributes attr;
  attr.colormap   = XCreateColormap( display, DefaultRootWindow(display), visualinfo->visual, AllocNone) ;
  attr.event_mask = ExposureMask | KeyPressMask ;
  attr.background_pixmap = None ;
  attr.border_pixel     = 0;
  attr.background_pixel = 0;
  win = XCreateWindow(display, DefaultRootWindow(display),
    WIN_X1, WIN_Y1, WIDTH, HEIGHT,
    0,
    visualinfo->depth,
    InputOutput,
    visualinfo->visual,
    CWColormap|CWEventMask|CWBackPixmap|CWBorderPixel,
    &attr
  );
  // Create the back buffer, setting swap action hint to background (automatic clearing)
  d_backbuf = XdbeAllocateBackBufferName(display, win, XdbeBackground);
  gc = XCreateGC(display, d_backbuf, 0, 0);
  //gc = XCreateGC(display, win, 0, 0);
  
  XStoreName(display, win, "rotocamcast");
  XMapWindow(display, win);
  
  unsigned long app_start_ms = now_ms();
  
  XSetBackground(display, gc, 0UL);
  XSetForeground(display, gc, ~0UL);
  XSetFunction(display, gc, GXcopy);
  
  // Counts how many frames in a row the pixel has been transparent.
  char trans_count[WIDTH*HEIGHT] = {0};
  
  while (!shutdown_flag) {
    unsigned long ms = now_ms();
    int want_redraw = 0;
    int pressed_a_key = 0;
    
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
          else if (XLookupKeysym(&event.xkey, 0) == 'a') {
            pressed_a_key = 1;
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
    else { // we _do_ want a redraw, but again if it happens closer than X ms apart, ignore the draw
      if (ms - last_draw_ms < NO_DRAW_MAX_MS) {
        want_redraw = 0;
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
      
      if (ms < nullframe_end_ms) {
        XSetBackground(display, gc, 0UL);
        XSetForeground(display, gc, ~0UL);
        
        XSetFunction(display, gc, GXandInverted);
        XFillRectangle(display, win, gc, 0, 0, win_width, win_height);
        //XSetFunction(display, gc, GXor); // useful: https://www.oreilly.com/library/view/xlib-reference-manual/9780937175262/15_appendix-g.html
        XSetFunction(display, gc, GXcopy);
        //XSetFunction(display, gc, GXnoop);
      }
      
      XImage* frame_img = XGetImage(display, win, 0, 0, win_width, win_height, AllPlanes, ZPixmap);
      
      while (camera_done_captured == 0) {
        //printf("Camera not done capping!\n");
        usleep(1 * 1000);
      }
      
      // values of 1 mean the pixel is transparent
      char trans_map[WIDTH*HEIGHT] = {1};
      
      for (int x=3; x<WIDTH-3; x++) {
        for (int y=3; y<HEIGHT-3; y++) {
          if (
            // extended extended family of pixels
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x-3, y-3, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x+3, y+3, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y-3, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y+3, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x-3, y, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x+3, y, bufferinfo.length) ||
            
            // extended family of pixels
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x-2, y-2, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x+2, y+2, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y-2, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y+2, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x-2, y, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x+2, y, bufferinfo.length) ||
            
            // the ones around us
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x-1, y-1, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x-1, y,   bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y-1,   bufferinfo.length) ||
            
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y,     bufferinfo.length) ||
            
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x+1, y+1, bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x+1, y,   bufferinfo.length) ||
            should_be_trans(camera_frame_buffer, lower_null_frame, upper_null_frame, x, y+1,   bufferinfo.length)
          ) {
            trans_map[(y*WIDTH) + x] = 1;
            if (trans_count[(y*WIDTH) + x] < 125) {
              trans_count[(y*WIDTH) + x] += 1;
            }
          }
          else {
            // we _think_ this should be solid, but let's check and make sure this was solid a lot before
            if (trans_count[(y*WIDTH) + x] < 5) {
              // We were mostly transparent, so this will now be transparent as well (but counters are unaffected)
              trans_map[WIDTH*HEIGHT] = 1;
            }
            else {
              trans_map[WIDTH*HEIGHT] = 0;
              if (trans_count[(y*WIDTH) + x] > -125) {
                trans_count[(y*WIDTH) + x] -= 1;
              }
            }
          }
        }
      }
      
      // Make edges transparent
      for (int x=0; x<WIDTH; x++) {
        for (int y=0; y<4; y++) {
          trans_map[(y*WIDTH) + x] = 1;
        }
      }
      for (int x=0; x<4; x++) {
        for (int y=0; y<HEIGHT; y++) {
          trans_map[(y*WIDTH) + x] = 1;
        }
      }
      for (int x=WIDTH-1; x>=0; x--) {
        for (int y=HEIGHT-1; y>HEIGHT-4; y--) {
          trans_map[(y*WIDTH) + x] = 1;
        }
      }
      for (int x=WIDTH-1; x>WIDTH-4; x--) {
        for (int y=HEIGHT-1; y>=0; y--) {
          trans_map[(y*WIDTH) + x] = 1;
        }
      }
      
      // Draw transparent pixels first
      XSetFunction(display, gc, GXandInverted);
      for (int x=0; x<WIDTH; x++) {
        for (int y=0; y<HEIGHT; y++) {
          if (trans_map[(y*WIDTH) + x] == 1) {
            XPutPixel(frame_img, WIDTH-x, y, 0);
          }
        }
      }
      
      XSetFunction(display, gc, GXcopy);
      for (int x=0; x<WIDTH; x++) {
        for (int y=0; y<HEIGHT; y++) {
          if (trans_map[(y*WIDTH) + x] == 0) {
            char y1 = get_y_(x, y, camera_frame_buffer, bufferinfo.length);
            unsigned long pixel_val = (0xff << 24) + (y1 << 16) + (y1 << 8) + (y1 << 0);
            XPutPixel(frame_img, WIDTH-x, y, pixel_val);
          }
        }
      }
      
      XPutImage(display, win, gc, frame_img, 0, 0, 0, 0, win_width, win_height);
      
      if (win_width != WIDTH || win_height != HEIGHT) {
        XMoveResizeWindow(display, win, win_x, win_y, WIDTH, HEIGHT);
      }
      
      // Finally swap the buffers
      XdbeSwapInfo swapInfo;
      swapInfo.swap_window = win;
      swapInfo.swap_action = XdbeBackground;

      if (!XdbeSwapBuffers(display, &swapInfo, 1)) {
        printf("Error swapping buffers!\n");
        //exit(5);
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
