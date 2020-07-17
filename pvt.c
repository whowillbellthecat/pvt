#include <sys/stat.h>

#include <curses.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
// #include <stdio.h> /* included by curses.h */
#include <term.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define EVENT_MAX 2048

struct event {
	int	i; /* interstimulus interval */
	int	s; /* duration stimulus shown */
	int	e; /* count of unhandled keypresses */
};

struct stats {
	int	errors;		/* Count of exteraneous key presses */
	int	lapses;		/* Count of response times greater than lapse_threshold */
	int	false_starts;	/* Count of responses faster than fs_threshold */
	int	stimuli_count;	/* Total count of stimuli shown */
};

struct configuration {
	int	n;	/* lower interval */
	int	m;	/* upper interval */
	int	t;	/* stimulus timeout */
	int	d;	/* test duration */
	int	l;	/* lapse threshold */
	int	f;	/* false start threshold */
};

static const struct configuration pvtb	= { 1, 4, 30*1000, 3*60, 355, 100 };
static const struct configuration pvt	= { 2, 10, 30*1000, 10*60, 500, 100 };

static long	parse_number(const char *, const long, const long, const char *);
static long	show_timer();
static int	handle_commission_errors();
static int	signum(const int);
static int	is_empty(const int);
static int	write_hdr(const int);
static int	get_interval(int, int);
static void	check_react(struct event *);
static int	stats_record(char *, const int, const struct stats *,
		    const struct configuration *, const time_t *);

static struct stats	populate_stats(struct event *, int, const struct configuration *);

static long
show_timer()
{
	int ready = 0, delta = 0, errors = 0;
	struct pollfd pfd[1];
	pfd[0].fd = STDIN_FILENO;
	pfd[0].events = POLLIN;
	struct timespec t = {0, 1000000};

	while(!ready || (ready >= 0 && pfd[0].revents & POLLIN && getch() != '\n')) {
		if (ready)
			errors++;
		erase(); mvprintw(LINES/2, COLS/2, "%d", delta); refresh();
		ready = poll(pfd, 1, 0);
		nanosleep(&t, NULL);
		delta+=10;
	}

	if(ready < 0)
		exit(EXIT_FAILURE);
	return errors;
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
			

static void
check_react(struct event *e)
{
	struct timespec t, ts1, ts2;
	int d;
	t.tv_sec = e->i;
	t.tv_nsec = 0;
	nanosleep(&t, NULL);
	e->e += handle_commission_errors();

	(void)bkgd(COLOR_PAIR(2));
	(void)clock_gettime(CLOCK_MONOTONIC, &ts1);
	e->e += show_timer();
	(void)clock_gettime(CLOCK_MONOTONIC, &ts2);
	(void)bkgd(COLOR_PAIR(1));

	clear();
	(void)mvprintw(LINES/2, COLS/2 - 3,"Wait...");
	(void)refresh();

	d = difftime(ts2.tv_sec, ts1.tv_sec);
	e->s = (d * 1000) + ((ts2.tv_nsec - ts1.tv_nsec)/1000000);
}

static int
get_interval(int n, int m)
{
	if (n == m)
		return n;
	return n + (rand()/(RAND_MAX/(m - n)));
}

static struct stats
populate_stats(struct event *events, int size, const struct configuration *config)
{
	int i;
	struct stats s = {};
	for (i=0; i < EVENT_MAX && events[i].s != 0; i++) {
		if (events[i].s >= config->l)
			s.lapses++;
		if (events[i].s <= config->f)
			s.false_starts++;
		s.errors += events[i].e;
	}
	s.stimuli_count = i;
	return s;
}

static int
stats_record(char *buf, const int len, const struct stats *s,
    const struct configuration *config, const time_t *start_time)
{
	struct tm *t;
	const char date_fmt[] = "%F %R %Z";
	char date[25];

	if (NULL == (t = localtime(start_time)))
		return -1;

	if (!strftime(date, sizeof(date), date_fmt, t))
		return -1;

	return snprintf(buf, len, "%s|(%d,%d,%d,%d,%d,%d)|%d|%d|%d|%d\n", date,
	    config->n, config->m, config->t, config->d, config->l, config->f,
	    s->errors, s->lapses, s->false_starts, s->stimuli_count);
}

static int
write_hdr(const int fd)
{
	const char hdr[] = "date|config|errors|lapses|false_starts|events\n";
	return write(fd, hdr, sizeof(hdr)-1);
}

static int
signum(const int x)
{
	return (x > 0) - (x < 0);

}

static int
is_empty(const int fd)
{
	struct stat sb;

	if(fstat(fd, &sb) == -1)
		err(1, "fstat");

	return !signum(sb.st_size);

}

static long
parse_number(const char *s, const long n, const long m, const char *type)
{
	errno = 0;
	char *e;
	long p = strtol(s, &e, 10);
	if(s[0] == '\0' || *e != '\0')
		errx(1, "constraint (%s : numeric) failed", type);
	if(p<n || p>m || (errno == ERANGE && (p == LONG_MAX || p == LONG_MIN)))
		errx(1,"constraint %ld <= %s <= %ld failed", n, type, m);
	return p;
}

int
main(int argc, char **argv)
{
	int	i, ch, ret; 	/* temporary variables */
	int	fd = 0;
	struct configuration config = pvtb;

	struct event	events[EVENT_MAX] = {};
	struct stats	stats;
	struct timespec	start, cur;
	struct timeval	time_of_day;

	char	record[128];
	char	*output_file = NULL;
	char	*usage = "pvt [-p] [-l lapse_threshold] [-d duration] [-n interval_min]"
		    " [-m interval_max] [-f falsestart_threshold] [file]";

	srand(time(NULL));

	output_file = getenv("PVT_FILE");

	while ((ch = getopt(argc, argv, "hpt:n:m:f:d:l:")) != -1) {
		switch (ch) {
		case 'l':
			config.l = parse_number(optarg, 0, 2048, "l");
			break;
		case 'd':
			config.d = parse_number(optarg, 0, 2048, "d");
			break;
		case 'p':
			config = pvt;
			break;
		case 'f':
			config.f = parse_number(optarg, 0, 2048, "f");
			break;
		case 'n':
			config.n = parse_number(optarg, 0, 2048, "n");
			break;
		case 'm':
			config.m = parse_number(optarg, 0, 2048, "m");
			break;
		case 't':
			config.t = parse_number(optarg, 0, INT_MAX, "t");
			break;
		default:
			errx(1, "%s", usage);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1)
		errx(1, "%s", usage);
	
	if (argc)
		output_file = *argv;

	if (config.n > config.m)
		errx(1, "constraint n <= m failed");
	if (config.m >= config.d)
		errx(1, "constraint m < d failed");
	if (config.l >= config.t)
		errx(1, "constraint l < t failed");
	if (config.f >= config.l - 1)
		errx(1, "constraint f < l - 1 failed");

	if (output_file) {
		fd = open(output_file, O_RDWR|O_APPEND|O_CREAT, S_IRUSR|S_IWUSR);
		if (fd < 0)
			err(1, "%s", output_file);
		if (gettimeofday(&time_of_day, NULL) != 0)
			err(1, NULL);
	}

	if (NULL == initscr())
		err(1, "could not initialize ncurses");
	ret = start_color();
	ret |= cbreak();
	ret |= noecho();
	ret |= init_pair(1, COLOR_WHITE, COLOR_BLACK);
	ret |= init_pair(2, COLOR_GREEN, COLOR_BLACK);
	if (ERR & ret)
		err(1, "ncurses initialization error");

#ifdef __OpenBSD__
	if (unveil(NULL, NULL) != 0)
		err(1, NULL);
	if (pledge("stdio tty", NULL) != 0)
		err(1, NULL);
#endif

	(void)bkgd(1);

	(void)clear();
	(void)mvprintw(LINES/2, COLS/2 - 3, "Wait...");
	(void)refresh();

	for (i = 0; i < EVENT_MAX; i++)
		events[i].i = get_interval(config.n, config.m);

	(void)clock_gettime(CLOCK_MONOTONIC, &start);

	cur = start;

	for(i = 0; cur.tv_sec - start.tv_sec < config.d; i++) {
		check_react(&events[i]);
		clock_gettime(CLOCK_MONOTONIC, &cur);
	}

	(void)clear();
	stats = populate_stats(events, EVENT_MAX, &config);

	(void)printw("Test Configuration: (%d, %d, %d, %d, %d, %d)\n\n",
	    config.n, config.m, config.t, config.d, config.l, config.f);

	(void)printw("Event count: %d\n", stats.stimuli_count);
	(void)printw("Lapses: %d\n", stats.lapses);
	(void)printw("False starts: %d\n", stats.false_starts);
	(void)printw("Extraneous keypresses: %d\n\n", stats.errors);

	if (fd) {
		if (is_empty(fd) && write_hdr(fd) < ret)
				err(1, "write_hdr");

		ret = stats_record(record, sizeof(record), &stats, &config, &time_of_day.tv_sec);
		if (ret <= 0 || ret >= sizeof(record))
			err(1, "error generating stats record");

		if (write(fd, record, ret) < ret)
			err(1, "write");
	}

	(void)printw("Press any key to exit.\n");
	(void)refresh();
	(void)getch();
	(void)endwin();
}
