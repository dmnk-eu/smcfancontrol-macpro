/*
 *  smcfancontrol.c : fan control daemon for Apple Intel Mac Pro 
 *  based on cmp-daemon 0.21 from http://aur.archlinux.org/packages.php?ID=21391
 *
 *  Copyright (C) 2009 Alexey Anikeenko
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, 
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <systemd/sd-daemon.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>

#define SYSFS_PATH_MAX 256
#define SPEED_STEP_MAX 20

static const char *PIDFILE = "/var/run/smcfancontrol.pid";
static const char *SMCDIR = "/sys/devices/platform/applesmc.768";
// static const char *CORETEMP_PREFIX = "/sys/devices/platform/coretemp";
static const char *CORETEMP_PREFIX = "/sys/devices/platform/applesmc.768";

static const struct timespec smc_write_delay = { 0, 5000000 }; /* 5 ms delay between writes to smc controller */ 

/* Reference data for dual-Xeon_X5482 Mac Pro:
const int FANS_EFI_MIN[] = { 500, 800, 600, 600 };
const int FANS_EFI_MAX[] = { 2900, 2900, 2900, 2800 };
const char *FANS_LABEL[] = { "CPU_MEM", "IO", "EXHAUST", "PS" };
*/ 

const int FANS[] = { 1, 2, 3, 4 };  /* suffixes of fans under control */
const int FANS_MIN[] = { 1000, 1000, 1000, 1000 };
const int FANS_MAX[] = { 2200, 2200, 2200, 2200 };
const int FANS_NUM = sizeof(FANS)/sizeof(FANS[0]);

// const int SENSORS[] = { 0, 1, 2, 3, 4, 5, 6, 7, 22 };  /* suffixes of used coretemp sensors */
const int SENSORS[] = { 22 };
const int SENSORS_NUM = sizeof(SENSORS)/sizeof(SENSORS[0]);

static int get_sensors_temp(void);
static void set_fans_manual(int);
static void set_fans_min(int);


/*
 * Fan speed depends linearly on the speed step and changes 
 * between FAN_MIN and FAN_MAX for steps in range [0, SPEED_STEP_MAX]. 
 * This function converts current temperature to speed step 
 * and should be calibrated for your box! 
 */
int temperature_to_speed_step(int t)
{
    int step = ((t-70)*SPEED_STEP_MAX)/(80-70);  

    if (step < 0 ) step = 0;
    if (step > SPEED_STEP_MAX ) step = SPEED_STEP_MAX;

    return step;
}


/* clean up and exit */
static void clean_exit_with_status(int status)
{
    set_fans_manual(0); /* set fans to automatic control */
    syslog(LOG_NOTICE, "exiting\n");
    unlink(PIDFILE);    /* remove pidfile */
    exit(status);
}

/* signal handler for clean exit */
static void clean_exit(int sig __attribute__((unused)))
{
    clean_exit_with_status(EXIT_SUCCESS);
}


/* set fanX_min (min rotation speed) according to current speed step */
static void set_fans_min(int speed_step)
{
    int i;
    FILE *file;
    char fan_path[SYSFS_PATH_MAX];

    /* for fans under control write rpm to fanX_min */
    for(i=0; i < FANS_NUM; i++ )
    {
        /* fan speed depends linearly on the speed step and changes 
         * between FAN_MIN and FAN_MAX for steps in range [0, SPEED_STEP_MAX] */
        int speed = FANS_MIN[i] + speed_step*(FANS_MAX[i]-FANS_MIN[i])/SPEED_STEP_MAX;

        sprintf(fan_path, "%s/fan%d_min", SMCDIR, FANS[i]);
        nanosleep(&smc_write_delay, NULL);
        if ((file=fopen(fan_path, "w")) != NULL) {
            fprintf(file, "%d", speed);
            fclose(file);
        }
        else {
            syslog(LOG_WARNING, "Error writing to %s, check if applesmc module loaded", fan_path);
        }
    }
}


/* write value to fanX_manual */
static void set_fans_manual(int value)
{
    int i;
    FILE *file;
    char fan_path[SYSFS_PATH_MAX];

    /* for fans under control write value to fanX_manual */
    for(i=0; i < FANS_NUM; i++ )
    {
        sprintf(fan_path, "%s/fan%d_manual", SMCDIR, FANS[i]);
        nanosleep(&smc_write_delay, NULL);
        if ((file=fopen(fan_path, "w")) != NULL) {
            fprintf(file, "%d", value);
            fclose(file);
        }
        else {
            syslog(LOG_WARNING, "Error writing to %s, check if applesmc module loaded", fan_path);
        }
    }
}


/* sort function for qsort: integers descending order */
static int compare_int_desc(const void *a, const void *b) {
    int *da, *db;
    da = (int *)a;
    db = (int *)b;

    return *db - *da;
}


/* return average of two largest temperatures, degrees  */
static int get_sensors_temp(void)
{
    int i;
    int t[SENSORS_NUM];
    char sensor_path[SYSFS_PATH_MAX];
    FILE *file;

    /* for all sensors read temp1_input */
    for(i=0; i < SENSORS_NUM; i++ )
    {
        // sprintf(sensor_path, "%s.%d/temp1_input", CORETEMP_PREFIX, SENSORS[i]);
	sprintf(sensor_path, "%s/temp%d_input", CORETEMP_PREFIX, SENSORS[i]);
        if ((file=fopen(sensor_path, "r")) != NULL) {
            fscanf(file, "%d", &(t[i])); /* read temperature in millidegrees */ 
            fclose(file);
        }
        else {
            syslog(LOG_ERR, "Error reading %s, check if coretemp module loaded and number of sensors", sensor_path);
            clean_exit_with_status(EXIT_FAILURE);
        }
    }

    /* sort temperature values, descending order */
    qsort(t, SENSORS_NUM, sizeof(t[0]), compare_int_desc);

    /* calculate average of two largest temps (if have more than one sensor) */
    int avg = 0, cnt = 0;
    for(i=0; (i < 2) && (i < SENSORS_NUM); i++) {
        avg += t[i];
        cnt ++;
    }
    avg = avg/cnt;

    /* return temperature in degrees */
    return avg/1000;
}


/* lock entire file */
static int lock_fd(int fd)
{
    struct flock lock;

    lock.l_type = F_WRLCK;
    lock.l_start = 0;
    lock.l_whence = SEEK_SET;
    lock.l_len = 0;

    return fcntl(fd, F_SETLK, &lock);
}


/* creates and locks pidfile */
static void create_pidfile(void)
{
    int fd;
    FILE *f;

    /* open the pidfile */
    fd = open(PIDFILE, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    if (fd < 0) {
        syslog(LOG_ERR, "can't open %s: %s\n", PIDFILE, strerror(errno));
        exit(EXIT_FAILURE);
    }
    /* lock pid file */
    if (lock_fd(fd) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            syslog(LOG_ERR, "daemon already running");
        } else {
            syslog(LOG_ERR, "can't lock %s: %s", PIDFILE, strerror(errno));
        }
        exit(EXIT_FAILURE);
    }

    /* write pid */
    ftruncate(fd, 0);
    f = fdopen(fd, "w");
    fprintf(f, "%d\n", getpid());
    fflush(f);
    /* keep file open and locked */
}


/* become a daemon and open log */
void daemonize(void)
{
    int i, maxfd;
    int fd0, fd1, fd2;
    pid_t pid;

    /* clear file creation mask */
    umask(0);

    /* fork and become a session leader */
    if ((pid=fork()) < 0) { 
        fprintf(stderr, "fork failed\n");
        exit(EXIT_FAILURE);
    }
    else if (pid != 0) { /* parent */
        exit(EXIT_SUCCESS);
    }
    setsid();

    /* change current working directory to the root */
    if (chdir("/") < 0) {
        fprintf(stderr, "can't change directory to /");
        exit(EXIT_FAILURE);
    }

    /* close all open file descriptors */
    maxfd = sysconf(_SC_OPEN_MAX);
    for (i=0; i < maxfd; i++)
        close(i); 

    /* set up stdin, stdout, stderr to /dev/null */
    fd0 = open("/dev/null", O_RDWR);
    fd1 = dup(fd0);
    fd2 = dup(fd0);

    /* open log */
    openlog("smcfancontrol", LOG_PID, LOG_DAEMON);
    if (fd0 != STDIN_FILENO || fd1 != STDOUT_FILENO || fd2 != STDERR_FILENO) {
        syslog(LOG_ERR, "unexpected file descriptors %d %d %d", fd0, fd1, fd2);
        exit(EXIT_FAILURE);
    }
}


int main(int argc, char *argv[])
{
    struct timespec sleep_period;   /* delay between sensor polls */
    int t, t_old;                   /* current and previous temperature */
    int speed_step, old_speed_step; /* current and old speed step */
    int cold, hot;                  /* number of consecutive temperature (de/in)creases */

    /* become a daemon */
    daemonize();

    syslog(LOG_INFO, "starting up\n");

    /* trap key signals */
    signal(SIGTERM, clean_exit);
    signal(SIGQUIT, clean_exit);
    signal(SIGINT,  clean_exit);
    signal(SIGHUP,  SIG_IGN);

    /* create pidfile and lock it */
    create_pidfile();

    /* set fans to automatic control */
    set_fans_manual(0);

    /* initial temperatures */
    t = t_old = get_sensors_temp();
    cold = hot = 1;

    /* init speed step and set fans */
    speed_step = temperature_to_speed_step(t);
    set_fans_min(speed_step);
    old_speed_step = speed_step;

    /* set sleep timer to 0.5 sec */
    sleep_period.tv_sec = 0;
    sleep_period.tv_nsec = 500000000;
    
    /* main loop */
    while(1)
    {
	
	// send signal so that systemd knows the daemon is still alive
	sd_notify(0, "WATCHDOG=1");


        t = get_sensors_temp();

        if ( t < t_old ) { /* it's getting colder */
            cold++;
            hot = 0;
        }
        if ( t > t_old ) { /* it's getting hotter */
            hot++;
            cold = 0;
        }

        if ( (cold==2) || (hot==2) ) 
        {
            speed_step = temperature_to_speed_step(t);

            if ( speed_step != old_speed_step) {
                set_fans_min(speed_step);
                syslog(LOG_INFO, "changed to speed step %d (of %d) at temperature %d", speed_step, SPEED_STEP_MAX, t);
                old_speed_step = speed_step;
            }

            cold = 0;
            hot = 0;
        }

        if( nanosleep(&sleep_period, NULL) < 0 && errno != EINTR )
        {
            clean_exit_with_status(EXIT_FAILURE);
        }

        t_old = t;
    }

    clean_exit_with_status(EXIT_SUCCESS);

    return 0;
}

