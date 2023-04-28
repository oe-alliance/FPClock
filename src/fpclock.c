/*
 * FPClock (c) 2023 jbleyel
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

/*
The sceleton of the daemon is based on https://github.com/jirihnidek/daemon
by Jiri Hnidek <jiri.hnidek@tul.cz>
*/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static int verbose = 0;
static int forcedate = -1;
static int running = 0;
static int delay = 1800;
static char *conf_file_name = NULL;
static char *pid_file_name = NULL;
static char *log_file_name = NULL;
static int pid_fd = -1;
static FILE *log_stream;
static int drift_data[10];
static int drift_index = 0;

const char *APP = "FPClock";
const char *app_name = "fpclock";
const char *app_ver = "1.7";
const char *proc_file = "/proc/stb/fp/rtc";
const char *dev_file = "/dev/dbox/fp0";
const char *drift_file = "/etc/fpclock.drift";

#define FP_IOCTL_SET_RTC 0x101
#define FP_IOCTL_GET_RTC 0x102

/**
 * \brief Log helper function
 * \param    print  0 = print to file if possible / 1 = print to console
 * \param    format  printf format
 */
void LOG(int print, const char *format, ...)
{
	char buf[2048];
	char timebuf[50];
	va_list other_args;
	time_t t;
	struct tm *tm;
	va_start(other_args, format);
	vsnprintf(buf, sizeof(buf), format, other_args);
	va_end(other_args);

	if (print)
	{
		printf("[%s] %s\n", APP, buf);
		return;
	}

	time(&t);
	tm = gmtime(&t);

	if (tm)
	{
		strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", tm);
		fprintf(log_stream, "[%s] %s\n", timebuf, buf);
	}
	else
	{
		fprintf(log_stream, "[%s] %s\n", APP, buf);
	}
	fflush(log_stream);
}

// drift functions

/**
 * \brief add value to drift array
 * \param    drift  new drift value
 */
void add_drift(int drift)
{
	if (drift != 0)
	{
		drift_data[drift_index] = drift;
		drift_index++;
		if (drift_index > 9)
			drift_index = 0;
	}
}
/**
 * \brief qsort compare function
 * \param   a value a
 * \param   b value b
 */
int cmpfunc(const void *a, const void *b) { return (*(int *)a - *(int *)b); }

/**
 * \brief Get calculated drift value
 */
double calc_drift(void)
{
	qsort(drift_data, 10, sizeof(int), cmpfunc);
	double median = (double)(drift_data[(9) / 2] + drift_data[5]) / 2.0;
	return median / (double)delay; // calculate drift value per second
}

/**
 * \brief Get drift delta in seconds from file
 */
int get_drift_seconds(int rtctime)
{
	FILE *f = fopen(drift_file, "r");
	if (f)
	{
		double drift;
		int lastsave;
		if (fscanf(f, "%d:%lf", &lastsave, &drift) != 2)
		{
			drift = 0;
			lastsave = 0;
			LOG(0, "Read %s failed: %m", drift_file);
		}
		fclose(f);

		if (drift != 0 && lastsave != 0)
		{
			int driftseconds = (int)((double)(rtctime - lastsave) * drift);
			if (verbose)
			{
				LOG(0, "FP RC drift:%f lastsave:%d offline seconds:%d drift seconds:%d", drift,
					lastsave, rtctime - lastsave, driftseconds);
			}
			return driftseconds;
		}
	}
	else
		LOG(0, "File %s not exists", drift_file);

	return 0;
}

/**
 * \brief Get epoch from RTC
 */
time_t getRTC(void)
{
	time_t rtc_time = 0;
	FILE *f = fopen(proc_file, "r");
	if (f)
	{
		unsigned int tmp;
		if (fscanf(f, "%u", &tmp) != 1)
			LOG(0, "Read %s failed: %m", proc_file);
		else
#ifdef HAVE_NO_RTC
			rtc_time = 0; // Sorry no RTC
#else
			rtc_time = tmp;
#endif
		fclose(f);
	}
	else
	{
		if (verbose)
		{
			LOG(0, "%s not exists", proc_file);
		}

		int fd = open(dev_file, O_RDWR);
		if (fd >= 0)
		{
			if (ioctl(fd, FP_IOCTL_GET_RTC, (void *)&rtc_time) < 0)
				LOG(0, "FP_IOCTL_GET_RTC failed: %m");
			close(fd);
		}
		else
		{
			if (verbose)
			{
				LOG(0, "%s not exists", dev_file);
			}
		}
	}
	return rtc_time;
}

/**
 * \brief Set epoch to RTC
 * \param    time   New epoch time value.
 */
void setRTC(time_t time, int saveDrift, int logMode)
{
	char *dt = ctime(&time);

	if (verbose)
		LOG(logMode, "Set FP RTC time to %s", dt);

	// Todo read FP and save the drift

	if (saveDrift)
	{
		time_t old = getRTC();
		int drift = (int)old - (int)time;
		if (drift != 0)
		{
			add_drift(drift);
			if (verbose)
				LOG(logMode, "FP RTC time drift value:%d / data:%d %d %d %d %d %d %d %d %d %d",
					drift, drift_data[0], drift_data[1], drift_data[2], drift_data[3],
					drift_data[4], drift_data[5], drift_data[6], drift_data[7], drift_data[8],
					drift_data[9]);
		}
	}

	FILE *fd = fopen(proc_file, "w");
	if (fd)
	{
		if (!fprintf(fd, "%u", (unsigned int)time))
			LOG(logMode, "Write %s failed: %m", proc_file);
		fclose(fd);
	}
	else
	{
		int fd = open(dev_file, O_RDWR);
		if (fd >= 0)
		{
			if (ioctl(fd, FP_IOCTL_SET_RTC, (void *)&time) < 0)
				LOG(logMode, "FP_IOCTL_SET_RTC failed: %m");
			close(fd);
		}
	}
}

/**
 * \brief Read configuration from config file
 */
int read_conf_file(int reload)
{
	FILE *conf_file = NULL;
	int ret = -1;

	if (conf_file_name == NULL)
		return 0;

	conf_file = fopen(conf_file_name, "r");

	if (conf_file == NULL)
	{
		syslog(LOG_ERR, "Can not open config file: %s, error: %s", conf_file_name, strerror(errno));
		return -1;
	}

	char *line = NULL;
	size_t len = 0;
	ssize_t read;

	int val = 0;
	while ((read = getline(&line, &len, conf_file)) != -1)
	{
		if (line[0] == '#')
			continue;
		if (sscanf(line, "verbose=%d", &val) == 1)
		{
			ret = 1;
			verbose = val;
		}
		if (sscanf(line, "timeout=%d", &val) == 1)
		{
			ret = 1;
			delay = val;
		}
	}

	if (line)
		free(line);

	if (ret > 0)
	{
		if (reload == 1)
		{
			syslog(LOG_INFO, "Reloaded configuration file %s of %s", conf_file_name, app_name);
		}
		else
		{
			syslog(LOG_INFO, "Configuration of %s read from file %s", app_name, conf_file_name);
		}
	}

	fclose(conf_file);

	return ret;
}

/**
 * \brief Callback function for handling signals.
 * \param    sig    identifier of signal
 */
void handle_signal(int sig)
{
	if (sig == SIGINT)
	{
		LOG(0, "Debug: stopping daemon ...");

		if (pid_fd != -1)
		{ // Unlock and close lockfile.
			lockf(pid_fd, F_ULOCK, 0);
			close(pid_fd);
		}
		if (pid_file_name != NULL)
		{ // Try to delete lockfile.
			unlink(pid_file_name);
		}
		// save drift info
		FILE *fd = fopen(drift_file, "w");
		if (fd)
		{
			double drift = calc_drift();
			LOG(0, "Write drift %ld:%lf", time(0), drift);
			if (!fprintf(fd, "%ld:%lf", time(0), drift))
				LOG(0, "Write %s failed: %m", drift_file);
			fclose(fd);
		}
		running = 0;
		signal(SIGINT, SIG_DFL); // Reset signal handling to default behavior.
	}
	else if (sig == SIGHUP)
	{
		LOG(0, "Debug: reloading daemon config file ...");
		read_conf_file(1);
	}
	else if (sig == SIGCHLD)
	{
		LOG(0, "Debug: received SIGCHLD signal");
	}
}

/**
 * \brief clean variables
 */
void clean(void)
{
	// Free allocated memory
	if (conf_file_name != NULL)
		free(conf_file_name);
	if (log_file_name != NULL)
		free(log_file_name);
	if (pid_file_name != NULL)
		free(pid_file_name);
}

/**
 * \brief clean exit
 */
static void clean_exit(int code)
{
	clean();
	exit(code);
}

/**
 * \brief This function will daemonize this app
 */
static void daemonize()
{
	pid_t pid = 0;
	int fd;

	pid = fork(); // Fork off the parent process.

	if (pid < 0)
	{ // An error occurred.
		LOG(1, "fork failed!");
		clean_exit(EXIT_FAILURE);
	}

	if (pid > 0)
	{ // Success: Let the parent terminate.
		clean_exit(EXIT_SUCCESS);
	}

	if (setsid() < 0)
	{ // On success the child process becomes session leader.
		LOG(1, "setsid failed!");
		clean_exit(EXIT_FAILURE);
	}

	signal(SIGCHLD, SIG_IGN); // Ignore signal sent from child to parent process.

	pid = fork(); // Fork off for the second time.

	if (pid < 0)
	{ // An error occurred.
		LOG(1, "fork failed!");
		clean_exit(EXIT_FAILURE);
	}

	if (pid > 0)
	{ // Daemon started successfully let the parent terminate.
		clean_exit(EXIT_SUCCESS);
	}

	umask(0); // Set new file permissions.

	int chdir_ret = chdir("/"); // Change the working directory to the root directory or another
								// appropriated directory.

	for (fd = (int)sysconf(_SC_OPEN_MAX); fd > 0;
		 fd--)
	{ // Close all open inherited file descriptors.
		close(fd);
	}

	/* Reopen stdin (fd = 0), stdout (fd = 1), stderr (fd = 2) */
	stdin = fopen("/dev/null", "r");
	stdout = fopen("/dev/null", "w+");
	stderr = fopen("/dev/null", "w+");

	if (pid_file_name != NULL)
	{ // Try to write PID of daemon to lockfile.
		char str[256];
		pid_fd = open(pid_file_name, O_RDWR | O_CREAT, 0640);
		if (pid_fd < 0)
		{
			LOG(1, "Can't open lockfile.!");
			clean_exit(EXIT_FAILURE); // Can't open lockfile.
		}
		if (lockf(pid_fd, F_TLOCK, 0) < 0)
		{
			LOG(1, "Can't lock lockfile.!");
			clean_exit(EXIT_FAILURE); // Can't lock file.
		}
		snprintf(str, 256, "%d\n", getpid());		   // Get current PID.
		ssize_t ret = write(pid_fd, str, strlen(str)); // Write PID to lockfile.
	}
}

/**
 * \brief Print command line options help.
 */
void print_help(void)
{
	printf("%s: Version %s\n\n", APP, app_ver);
	printf("Usage: %s [OPTIONS]\n\n", app_name);
	printf("  Options:\n");
	printf("\t-h --help                 Print this help text.\n");
	printf("\t-c --conf_file filename   Read configuration from the file.\n");
	printf("\t-t --timeout timeout      Set the loop timeout in seconds. (Default 1800)\n");
	printf("\t-l --log_file  filename   Write logs to the file. (Only for daemon mode)\n");
	printf("\t-d --daemon               Daemonize this application.\n");
	printf("\t-p --print                Print FP clock time.\n");
	printf("\t-u --update               Update FP clock with the current system time.\n");
	printf("\t-f --force epoch          Force FP clock to given epoch time.\n");
	printf("\t-r --restore              Restore current system time from FP  clock.\n");
	printf("\t-v --verbose              Enable debugging output.\n");
	printf("\n");
}

/**
 * \brief Prints time from RTC
 */
int print_fp(void)
{
	time_t time = getRTC();
	if (time)
	{
		char *dt = ctime(&time);
		LOG(1, "Read result:%s", dt);
	}
	else
	{
		LOG(1, "Read RTC failed");
	}
	return 0;
}

/**
 * \brief Set epoch to RTC
 * \param    c   New epoch
 */
int write_fp(int c)
{
	if (c != -1)
	{
		if (verbose)
			LOG(1, "Write %d", c);

		if (c < 1672527600)
		{ // 1.1.2023
			LOG(1, "Write Error epoch:%d to low.", c);
			return 1;
		}
		setRTC(c, 0, 1);
	}
	else
	{
		setRTC(time(0), 1, 0);
	}
	return 0;
}

/**
 * \brief write epoch from RTC to system
 */
int sync_fp(int cmdline)
{
	time_t rtc_time = getRTC();
	time_t system_time = time(0);

	if (rtc_time)
	{
		if (!cmdline)
		{
			int drift = get_drift_seconds(rtc_time);
			rtc_time += drift;
		}

		int time_difference = (int)rtc_time - (int)system_time;
		int atime_difference = abs(time_difference);
		if (atime_difference > 30)
		{ // diff higher than 30 seconds
			struct timeval tdelta, tolddelta;
			tdelta.tv_sec = time_difference;
			int rc = adjtime(&tdelta, &tolddelta);
			if (rc == -1)
			{
				if (errno == EINVAL)
				{
					struct timeval tnow;
					gettimeofday(&tnow, 0);
					tnow.tv_sec = time_difference;
					settimeofday(&tnow, 0);
					LOG(cmdline, "Slewing Linux time by %d seconds.", time_difference);
				}
				else
				{
					LOG(cmdline, "Slewing Linux time by %d seconds FAILED! (%d) %m",
						time_difference, errno);
				}
			}
			else
				LOG(cmdline, "Slewing Linux time by %d seconds.", time_difference);
		}
	}
	else
	{
		LOG(cmdline, "Sync failed Update because FP RTC time is 0");
	}

	return 0;
}

/**
 * \brief main
 */
int main(int argc, char *argv[])
{
	static struct option long_options[] = {{"timeout", required_argument, 0, 't'},
										   {"force", required_argument, 0, 'f'},
										   //        {"conf_file", required_argument, 0, 'c'},
										   {"test_conf", required_argument, 0, 't'},
										   {"log_file", required_argument, 0, 'l'},
										   {"help", no_argument, 0, 'h'},
										   {"daemon", no_argument, 0, 'd'},
										   {"verbose", no_argument, 0, 'v'},
										   {"restore", no_argument, 0, 'r'},
										   {"print", no_argument, 0, 'p'},
										   {"update", no_argument, 0, 'u'},
										   {NULL, 0, 0, 0}};
	int value, option_index = 0;
	int start_daemonized = 0;

	if (argc == 1)
	{
		print_help();
		return EXIT_SUCCESS;
	}

	char str[256];
	snprintf(str, 256, "/var/run/%s.pid", app_name);
	pid_file_name = strdup(str);

	log_stream = stdout;

	int action = 0;

	while ((value = getopt_long(argc, argv, "l:t:f:pdhrudpv", long_options, &option_index)) != -1)
	{
		switch (value)
		{
		case 't':
			sscanf(optarg, "%d", &delay);
			break;
		case 'c':
			conf_file_name = strdup(optarg);
			break;
		case 'l':
			log_file_name = strdup(optarg);
			break;
		case 'd':
			start_daemonized = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'f':
			sscanf(optarg, "%d", &forcedate);
			action = 2;
			break;
		case 'r':
			action = 3;
			break;
		case 'u':
			action = 2;
			break;
		case 'h':
			print_help();
			clean();
			return EXIT_SUCCESS;
		case 'p':
			action = 1;
			break;
		case '?':
			print_help();
			clean();
			return EXIT_SUCCESS;
		default:
			break;
		}
	}

	if (verbose)
	{
		LOG(1, "Version %s\n\n", app_ver);
		LOG(1, "Verbose logging");
		LOG(1, "Delay : %d", delay);
		if (forcedate != -1)
			LOG(1, "Force epoch : %d", forcedate);
	}

	if (action)
	{
		if (action == 1)
		{
			print_fp();
		}
		else if (action == 2)
		{
			write_fp(forcedate);
		}
		else if (action == 3)
		{
			sync_fp(1);
		}
		clean();
		return EXIT_SUCCESS;
	}

	if (start_daemonized)
	{ // When daemonizing is requested at command line.
		daemonize();
	}
	else
	{
		clean();
		return EXIT_SUCCESS;
	}

	/* Open system log and write message to it */
	openlog(argv[0], LOG_PID | LOG_CONS, LOG_DAEMON);
	syslog(LOG_INFO, "Started %s V:%s", app_name, app_ver);

	/* Daemon will handle two signals */
	signal(SIGINT, handle_signal);
	signal(SIGHUP, handle_signal);

	/* Try to open log file to this daemon */
	if (log_file_name != NULL)
	{
		log_stream = fopen(log_file_name, "a+");
		if (log_stream == NULL)
		{
			syslog(LOG_ERR, "Can not open log file: %s, error: %s", log_file_name, strerror(errno));
			log_stream = stdout;
		}
	}

	// Read configuration from config file.
	read_conf_file(0);

	// This global variable can be changed in function handling signal.
	running = 1;

	for (int x = 0; x < 10; x++)
		drift_data[x] = -1;

	LOG(0, "Start loop");

	sync_fp(0); // initial sync from FP

	while (running == 1)
	{ // Never ending loop of the daemon.
		write_fp(-1);
		sleep(delay);
	}

	// Close log file, when it is used.
	if (log_stream != stdout)
	{
		fclose(log_stream);
	}

	// Write system log and close it.
	syslog(LOG_INFO, "Stopped %s", app_name);
	closelog();

	clean();

	return EXIT_SUCCESS;
}
