#define _BSD_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <X11/Xlib.h>

char *tzindia = "Asia/Kolkata";
char *tznewyork = "America/New_York";

static Display *dpy;
int charging_count = 0;

  char *
smprintf(char *fmt, ...)
{
  va_list fmtargs;
  char *ret;
  int len;

  va_start(fmtargs, fmt);
  len = vsnprintf(NULL, 0, fmt, fmtargs);
  va_end(fmtargs);

  ret = malloc(++len);
  if (ret == NULL) {
    perror("malloc");
    exit(1);
  }

  va_start(fmtargs, fmt);
  vsnprintf(ret, len, fmt, fmtargs);
  va_end(fmtargs);

  return ret;
}

  void
settz(char *tzname)
{
  setenv("TZ", tzname, 1);
}

  char *
mktimes(char *fmt, char *tzname)
{
  char buf[129];
  time_t tim;
  struct tm *timtm;

  memset(buf, 0, sizeof(buf));
  settz(tzname);
  tim = time(NULL);
  timtm = localtime(&tim);
  if (timtm == NULL) {
    perror("localtime");
    exit(1);
  }

  if (!strftime(buf, sizeof(buf)-1, fmt, timtm)) {
    fprintf(stderr, "strftime == 0\n");
    exit(1);
  }

  return smprintf("%s", buf);
}

  void
setstatus(char *str)
{
  XStoreName(dpy, DefaultRootWindow(dpy), str);
  XSync(dpy, False);
}

  char *
loadavg(void)
{
  double avgs[3];

  if (getloadavg(avgs, 3) < 0) {
    perror("getloadavg");
    exit(1);
  }

  return smprintf("%.2f %.2f %.2f", avgs[0], avgs[1], avgs[2]);
}

  int
parse_netdev(unsigned long long int *receivedabs, unsigned long long int *sentabs)
{
  char buf[255];
  char *datastart;
  static int bufsize;
  int rval;
  FILE *devfd;
  unsigned long long int receivedacc, sentacc;

  bufsize = 255;
  devfd = fopen("/proc/net/dev", "r");
  rval = 1;

  // Ignore the first two lines of the file
  fgets(buf, bufsize, devfd);
  fgets(buf, bufsize, devfd);

  while (fgets(buf, bufsize, devfd)) {
    if ((datastart = strstr(buf, "lo:")) == NULL) {
      datastart = strstr(buf, ":");

      // With thanks to the conky project at http://conky.sourceforge.net/
      sscanf(datastart + 1, "%llu  %*d     %*d  %*d  %*d  %*d   %*d        %*d       %llu",\
	  &receivedacc, &sentacc);
      *receivedabs += receivedacc;
      *sentabs += sentacc;
      rval = 0;
    }
  }

  fclose(devfd);
  return rval;
}

  void
calculate_speed(char *speedstr, unsigned long long int newval, unsigned long long int oldval)
{
  double speed;
  speed = (newval - oldval) / 1024.0;
  if (speed > 1024.0) {
    speed /= 1024.0;
    sprintf(speedstr, "%.3f MB/s", speed);
  } else {
    sprintf(speedstr, "%.2f KB/s", speed);
  }
}

  char *
get_netusage(unsigned long long int *rec, unsigned long long int *sent)
{
  unsigned long long int newrec, newsent;
  newrec = newsent = 0;
  char downspeedstr[15], upspeedstr[15];
  static char retstr[42];
  int retval;

  retval = parse_netdev(&newrec, &newsent);
  if (retval) {
    fprintf(stdout, "Error when parsing /proc/net/dev file.\n");
    exit(1);
  }

  calculate_speed(downspeedstr, newrec, *rec);
  calculate_speed(upspeedstr, newsent, *sent);

  sprintf(retstr, "down: %s up: %s", downspeedstr, upspeedstr);

  *rec = newrec;
  *sent = newsent;
  return retstr;
}

  char *
get_brightness()
{
  char buf[255];
  static int bufsize;
  FILE *brightnessfd;

  bufsize = 255;
  brightnessfd = fopen("/sys/class/backlight/intel_backlight/brightness", "r");
  fgets(buf, bufsize, brightnessfd);
  int brightness = atoi(buf);
  fclose(brightnessfd);

  brightnessfd = fopen("/sys/class/backlight/intel_backlight/max_brightness", "r");
  fgets(buf, bufsize, brightnessfd);
  int max_brightness = atoi(buf);
  fclose(brightnessfd);

  int current_brightness = (brightness * 100) / max_brightness;

  static char retstr[4];
  sprintf(retstr, "%d%%", current_brightness);
  return retstr;
}

  char *
readfile(char *base, char *file)
{
  char *path, line[513];
  FILE *fd;

  memset(line, 0, sizeof(line));

  path = smprintf("%s/%s", base, file);
  fd = fopen(path, "r");
  if (fd == NULL)
    return NULL;
  free(path);

  if (fgets(line, sizeof(line)-1, fd) == NULL)
    return NULL;
  fclose(fd);

  return smprintf("%s", line);
}

/*
 * Linux seems to change the filenames after suspend/hibernate
 * according to a random scheme. So just check for both possibilities.
 */
  char *
getbattery(char *base)
{
  char *co;
  int descap, remcap;

  descap = -1;
  remcap = -1;

  co = readfile(base, "present");
  if (co == NULL || co[0] != '1') {
    if (co != NULL) free(co);
    return smprintf("not present");
  }
  free(co);

  co = readfile(base, "charge_full");
  if (co == NULL) {
    co = readfile(base, "energy_full");
    if (co == NULL)
      return smprintf("");
  }
  sscanf(co, "%d", &descap);
  free(co);

  co = readfile(base, "charge_now");
  if (co == NULL) {
    co = readfile(base, "energy_now");
    if (co == NULL)
      return smprintf("");
  }
  sscanf(co, "%d", &remcap);
  free(co);

  co = readfile(base, "status");
  int charging = 0;
  if (strcmp(co, "Charging\n") == 0 || strcmp(co, "Full\n") == 0) {
    charging = 1;
  }
  free(co);

  if (remcap < 0 || descap < 0)
    return smprintf("invalid");

  int current = (remcap * 100) / descap;
  static char retstr[8];
  if (charging == 1) {
    if (charging_count == 4) {
      charging_count = 0;
    } else {
      charging_count++;
    }
    switch(charging_count) {
      case 0:
        sprintf(retstr, "%s %d%%", "", current);
        break;
      case 1:
        sprintf(retstr, "%s %d%%", "", current);
        break;
      case 2:
        sprintf(retstr, "%s %d%%", "", current);
        break;
      case 3:
        sprintf(retstr, "%s %d%%", "", current);
        break;
      case 4:
        sprintf(retstr, "%s %d%%", "", current);
        break;
    }
  } else {
    charging_count = 0;
    if (current < 10) {
      sprintf(retstr, "%s %d%%", "", current);
    } else if (current < 25) {
      sprintf(retstr, "%s %d%%", "", current);
    } else if (current < 65) {
      sprintf(retstr, "%s %d%%", "", current);
    } else if (current < 85) {
      sprintf(retstr, "%s %d%%", "", current);
    } else {
      sprintf(retstr, "%s %d%%", "", current);
    }
  }
  /*sprintf(retstr, "%d%%", current);*/

  return retstr;
}

  int
main(void)
{
  char *status;
  char *avgs;
  char *tmind;
  char *netstats;
  char *brightness;
  char *battery;
  static unsigned long long int rec, sent;

  if (!(dpy = XOpenDisplay(NULL))) {
    fprintf(stderr, "dwmstatus: cannot open display.\n");
    return 1;
  }

  parse_netdev(&rec, &sent);
  for (;;sleep(1)) {
    avgs = loadavg();
    tmind = mktimes(" %a %d %b |  %H:%M", tznewyork);
    netstats = get_netusage(&rec, &sent);
    brightness = get_brightness();
    battery = getbattery("/sys/class/power_supply/BAT1");

    status = smprintf(" %s |  %s |  %s | %s",
	battery, brightness, netstats, tmind);
    setstatus(status);
    free(avgs);
    free(tmind);
    /*free(netstats);*/
    /*free(brightness);*/
    /*free(battery);*/
    free(status);
  }

  XCloseDisplay(dpy);

  return 0;
}

