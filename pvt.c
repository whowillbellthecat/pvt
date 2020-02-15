#include <sys/stat.h>

#include <curses.h>
#include <err.h>
#include <fcntl.h>
#include <poll.h>
// #include <stdio.h> /* included by curses.h */
#include <term.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define EVENT_MAX 2048

struct event {
	struct	timespec stimulus;
	struct	timespec rt;
	int	commission_err_count;
};

struct stats {
	int	testlen;		/* Length of test in seconds */
	int	commission_err_count;	/* Count of exteraneous key presses */
	int	lapses;			/* Count of response times greater than lapse_threshold */
	int	lapse_threshold;	/* Time in milliseconds before responses considered lapses */
	int	false_starts;		/* Count of responses faster than 100ms */
	int	stimuli_count;		/* Total count of stimuli shown */
};

const int	pvtb_lapse_threshold = 355;	/* PVT-B lapse threshold (in milliseconds) */
const int	pvtb_testlen = 3*60;		/* PVT-B test length (in seconds) */
const int	pvtb_interval_lower = 1;	/* PVT-B stimulus interval lower bound (in seconds) */
const int	pvtb_interval_upper = 4;	/* PVT-B stimulus interval upper bound (in seconds) */

static long	show_timer();
static int	handle_commission_errors();
static void	print_stats(struct stats);
static void	write_stats_to_file(int, const struct stats, const time_t *);
static struct stats	populate_stats(struct event *, int, int, int);
static struct event	check_react(const struct timespec *);
static struct timespec	get_interval(int, int);

static long
show_timer()
{
	int ready = 0, delta = 0;
	struct pollfd pfd[1];
	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;
	struct timespec t = {0, 1000000};

	while(!ready) {
		clear(); mvprintw(LINES/2, COLS/2, "%d", delta); refresh();
		ready = poll(pfd, 1, 0);
		nanosleep(&t, NULL);
		delta+=10;
	}

	if(ready < 0)
		exit(EXIT_FAILURE);
	if(pfd[0].revents & POLLIN && getch() == '\n')
		return 1;
	return 0;
}

static int
handle_commission_errors()
{
	int ready = 0, count = 0;
	struct pollfd pfd[1];
	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;
	
	do {
		ready = poll(pfd, 1, 0);
		if (ready > 0 && pfd[0].revents & POLLIN) {
			(void)getch();
			count++;
		}
	} while (ready);
	return count;
}
			

static struct event
check_react(const struct timespec *t)
{
	struct timespec ts1, ts2;
	struct event e;
	nanosleep(t, NULL);
	e.commission_err_count += handle_commission_errors();

	bkgd(COLOR_PAIR(1));
	clock_gettime(CLOCK_MONOTONIC, &ts1);
	show_timer();
	clock_gettime(CLOCK_MONOTONIC, &ts2);
	bkgd(COLOR_PAIR(0));

	clear();
	mvprintw(LINES/2, COLS/2 - 3,"Wait...");
	refresh();
	e.stimulus = ts1;
	e.rt = ts2;
	return e;
}

static struct timespec
get_interval(int min, int range)
{
	struct timespec t;
	t.tv_sec = min+(rand()/(RAND_MAX/range));
	t.tv_nsec = 0;
	return t;
}

static void
print_stats(struct stats s)
{
	printw("Test length:                %d seconds\n", s.testlen);
	printw("Errors of commission:       %d\n", s.commission_err_count);
	printw("Lapses       (rt > %d ms): %d\n", s.lapse_threshold, s.lapses);
	printw("False starts (rt < 100 ms): %d\n", s.false_starts);
	printw("Total stimulus count:       %d\n", s.stimuli_count);
}

static struct stats
populate_stats(struct event *events, int size, int lapse_threshold, int testlen)
{
	int i, d;
	struct stats s;
	for (i=0; i < EVENT_MAX && events[i].stimulus.tv_nsec != 0; i++) {
		d = difftime(events[i].rt.tv_sec, events[i].stimulus.tv_sec);
		if (events[i].rt.tv_nsec/1000000 >= lapse_threshold || d)
			s.lapses++;
		if (events[i].rt.tv_nsec/1000000 <= 100 && 0 == d)
			s.false_starts++;
		s.commission_err_count += events[i].commission_err_count;
	}
	s.stimuli_count = i;
	s.lapse_threshold = lapse_threshold;
	s.testlen = testlen;
	return s;
}

static void
write_stats_to_file(int fd, const struct stats s, const time_t *start_time)
{ 
	struct tm *t;
	size_t ret;
	if (NULL == (t = localtime(start_time)))
		err(1, "could not determine localtime");

	char date_fmt[] = "%F %R %Z";
	char date[25];
	if (0 == (ret = strftime(date, sizeof(date), "%F %R %Z", t)))
		err(1, "could not format date (probably a bug)");
	
	char record[128]; // bound settings and calculate this using those bounds
	
	ret = snprintf(record, 128, "%s,%d,%d,%d,%d,%d,%d\n", date, s.testlen,
	    s.commission_err_count, s.lapses, s.lapse_threshold, s.false_starts,
	    s.stimuli_count);
	if (0 == ret || 128 <= ret)
		err(1, "stats record too large (probably a bug)");
	write(fd, record, ret);
}

int
main(int argc, char **argv)
{
	int false_starts = 0, lapses = 0, verbose = 0, i, d, ch;
	int fd = 0;
	int	testlen 	= pvtb_testlen;
	int	lapse_threshold = pvtb_lapse_threshold;
	int	min		= pvtb_interval_lower;
	int	interval_range	= pvtb_interval_upper - pvtb_interval_lower;
	const char *emsg;
	struct event events[EVENT_MAX] = {0};
	struct stats stats;
	struct timespec start, cur, interval;
	struct timeval time_of_day;
	char *output_file = NULL;
	char *usage = "pvt [-vd] [-l lapse-threshold] [-d duration] [[file] ...]";
	srand(time(NULL));

	while ((ch = getopt(argc, argv, "hvpd:l:")) != -1) {
		switch (ch) {
		case 'v':
			verbose++;
			break;
		case 'l':
			lapse_threshold = strtonum(optarg, 1, 2048, &emsg);
			if (emsg)
				errx(1, "lapse threshold %s", emsg);
			break;
		case 'd':
			testlen = strtonum(optarg, 1, 2048, &emsg);
			if (emsg)
				errx(1, "test duration %s", emsg);
			break;
		case 'p': /* Take PVT instead of PVT-B */
			testlen = 10*60;
			lapse_threshold = 500;
			min = 2;
			interval_range = 8;
			break;
		default:
			errx(1, "%s", usage);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc) {
		fd = open(*argv, O_RDWR|O_APPEND|O_CREAT, S_IRUSR|S_IWUSR);
		if (fd < 0)
			err(1, "%s", *argv);
		if (gettimeofday(&time_of_day, NULL) != 0)
			err(1, NULL);
	}

	initscr();
	start_color();
	cbreak();
	noecho();
	init_pair(1, COLOR_RED, COLOR_BLACK);

#ifdef __OpenBSD__
	if (unveil(NULL, NULL) != 0)
		err(1, NULL);
	if (pledge("stdio tty", NULL) != 0)
		err(1, NULL);
#endif

	clear();
	mvprintw(LINES/2, COLS/2 - 3, "Wait...");
	refresh();

	interval = get_interval(min, interval_range);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for(i=0, cur = start; cur.tv_sec - start.tv_sec < testlen; i++, clock_gettime(CLOCK_MONOTONIC, &cur))
		events[i]=check_react(&interval);

	
	clear();
	stats = populate_stats(events, EVENT_MAX, lapse_threshold, testlen);
	print_stats(stats);

	if (fd)
		write_stats_to_file(fd, stats, &time_of_day.tv_sec);

	if (verbose)
		for(i=0; d = difftime(events[i].rt.tv_sec, events[i].stimulus.tv_sec),
			i < EVENT_MAX && (events[i].stimulus.tv_nsec || d); i++)
			printw("rt: %d ms\n", (d*1000) + (events[i].rt.tv_nsec/1000000));

	printw("Press any key to exit.\n");
	refresh();
	(void)getch();
	endwin();
}
