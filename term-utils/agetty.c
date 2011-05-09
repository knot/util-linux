/*
 * Alternate Getty (agetty) 'agetty' is a versatile, portable, easy to use
 * replacement for getty on SunOS 4.1.x or the SAC ttymon/ttyadm/sacadm/pmadm
 * suite on Solaris and other SVR4 systems. 'agetty' was written by Wietse
 * Venema, enhanced by John DiMarco, and further enhanced by Dennis Cronin.
 *
 * Ported to Linux by Peter Orbaek <poe@daimi.aau.dk>
 *
 * This program is freely distributable.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <utmp.h>
#include <getopt.h>
#include <time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netdb.h>

#include "strutils.h"
#include "nls.h"
#include "pathnames.h"
#include "c.h"
#include "xalloc.h"

#ifdef __linux__
#  include <sys/param.h>
#  define USE_SYSLOG
#endif

/* If USE_SYSLOG is undefined all diagnostics go to /dev/console. */
#ifdef	USE_SYSLOG
#  include <syslog.h>
#endif

/*
 * Some heuristics to find out what environment we are in: if it is not
 * System V, assume it is SunOS 4. The LOGIN_PROCESS is defined in System V
 * utmp.h, which will select System V style getty.
 */
#ifdef LOGIN_PROCESS
#  define SYSV_STYLE
#endif

/*
 * Things you may want to modify.
 *
 * If ISSUE is not defined, agetty will never display the contents of the
 * /etc/issue file. You will not want to spit out large "issue" files at the
 * wrong baud rate. Relevant for System V only.
 *
 * You may disagree with the default line-editing etc. characters defined
 * below. Note, however, that DEL cannot be used for interrupt generation
 * and for line editing at the same time.
 */

/* Displayed before the login prompt. */
#ifdef	SYSV_STYLE
#  define ISSUE _PATH_ISSUE
#  include <sys/utsname.h>
#endif

/* Login prompt. */
#define LOGIN " login: "

/* Some shorthands for control characters. */
#define CTL(x)		(x ^ 0100)	/* Assumes ASCII dialect */
#define	CR		CTL('M')	/* carriage return */
#define	NL		CTL('J')	/* line feed */
#define	BS		CTL('H')	/* back space */
#define	DEL		CTL('?')	/* delete */

/* Defaults for line-editing etc. characters; you may want to change these. */
#define DEF_ERASE	DEL		/* default erase character */
#define DEF_INTR	CTL('C')	/* default interrupt character */
#define DEF_QUIT	CTL('\\')	/* default quit char */
#define DEF_KILL	CTL('U')	/* default kill char */
#define DEF_EOF		CTL('D')	/* default EOF char */
#define DEF_EOL		0
#define DEF_SWITCH	0		/* default switch char */

#ifndef MAXHOSTNAMELEN
#  ifdef HOST_NAME_MAX
#    define MAXHOSTNAMELEN HOST_NAME_MAX
#  else
#    define MAXHOSTNAMELEN 64
#  endif			/* HOST_NAME_MAX */
#endif				/* MAXHOSTNAMELEN */

/*
 * When multiple baud rates are specified on the command line, the first one
 * we will try is the first one specified.
 */
#define	FIRST_SPEED	0

/* Storage for command-line options. */
#define	MAX_SPEED	10	/* max. nr. of baud rates */

struct options {
	int flags;			/* toggle switches, see below */
	int timeout;			/* time-out period */
	char *login;			/* login program */
	char *tty;			/* name of tty */
	char *initstring;		/* modem init string */
	char *issue;			/* alternative issue file */
	int numspeed;			/* number of baud rates to try */
	speed_t speeds[MAX_SPEED];	/* baud rates to be tried */
};

#define	F_PARSE		(1<<0)	/* process modem status messages */
#define	F_ISSUE		(1<<1)	/* display /etc/issue */
#define	F_RTSCTS	(1<<2)	/* enable RTS/CTS flow control */
#define F_LOCAL		(1<<3)	/* force local */
#define F_INITSTRING    (1<<4)	/* initstring is set */
#define F_WAITCRLF	(1<<5)	/* wait for CR or LF */
#define F_CUSTISSUE	(1<<6)	/* give alternative issue file */
#define F_NOPROMPT	(1<<7)	/* do not ask for login name! */
#define F_LCUC		(1<<8)	/* support for *LCUC stty modes */
#define F_KEEPSPEED	(1<<9)	/* follow baud rate from kernel */
#define F_KEEPCFLAGS	(1<<10)	/* reuse c_cflags setup from kernel */
#define F_EIGHTBITS	(1<<11)	/* Assume 8bit-clean tty */

/* Storage for things detected while the login name was read. */
struct chardata {
	int erase;		/* erase character */
	int kill;		/* kill character */
	int eol;		/* end-of-line character */
	int parity;		/* what parity did we see */
	int capslock;		/* upper case without lower case */
};

/* Initial values for the above. */
struct chardata init_chardata = {
	DEF_ERASE,		/* default erase character */
	DEF_KILL,		/* default kill character */
	13,			/* default eol char */
	0,			/* space parity */
	0,			/* no capslock */
};

struct Speedtab {
	long speed;
	speed_t code;
};

static struct Speedtab speedtab[] = {
	{50, B50},
	{75, B75},
	{110, B110},
	{134, B134},
	{150, B150},
	{200, B200},
	{300, B300},
	{600, B600},
	{1200, B1200},
	{1800, B1800},
	{2400, B2400},
	{4800, B4800},
	{9600, B9600},
#ifdef	B19200
	{19200, B19200},
#endif
#ifdef	B38400
	{38400, B38400},
#endif
#ifdef	EXTA
	{19200, EXTA},
#endif
#ifdef	EXTB
	{38400, EXTB},
#endif
#ifdef B57600
	{57600, B57600},
#endif
#ifdef B115200
	{115200, B115200},
#endif
#ifdef B230400
	{230400, B230400},
#endif
	{0, 0},
};

static void parse_args(int argc, char **argv, struct options *op);
static void parse_speeds(struct options *op, char *arg);
static void update_utmp(char *line);
static void open_tty(char *tty, struct termios *tp, int local);
static void termio_init(struct options *op, struct termios *tp);
static void auto_baud(struct termios *tp);
static void do_prompt(struct options *op, struct termios *tp);
static void next_speed(struct options *op, struct termios *tp);
static char *get_logname(struct options *op,
			 struct termios *tp, struct chardata *cp);
static void termio_final(struct options *op,
			 struct termios *tp, struct chardata *cp);
static int caps_lock(char *s);
static speed_t bcode(char *s);
static void usage(FILE * out) __attribute__((__noreturn__));
static void log_err(const char *, ...) __attribute__((__noreturn__)) __attribute__((__format__(printf, 1, 2)));
static void log_warn (const char *, ...) __attribute__((__format__(printf, 1, 2)));

/* Fake hostname for ut_host specified on command line. */
static char *fakehost;

#ifdef DEBUGGING
#define debug(s) fprintf(dbf,s); fflush(dbf)
FILE *dbf;
#else
#define debug(s)
#endif				/* DEBUGGING */

int main(int argc, char **argv)
{
	char *logname = NULL;		/* login name, given to /bin/login */
	struct chardata chardata;	/* will be set by get_logname() */
	struct termios termios;		/* terminal mode bits */
	static struct options options = {
		F_ISSUE,	/* show /etc/issue (SYSV_STYLE) */
		0,		/* no timeout */
		_PATH_LOGIN,	/* default login program */
		"tty1",		/* default tty line */
		"",		/* modem init string */
		ISSUE,		/* default issue file */
		0, 		/* no baud rates known yet */
		{ 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

#ifdef DEBUGGING
	dbf = fopen("/dev/ttyp0", "w");
	for (int i = 1; i < argc; i++)
		debug(argv[i]);
#endif				/* DEBUGGING */

	/* Parse command-line arguments. */
	parse_args(argc, argv, &options);

#ifdef __linux__
	setsid();
#endif

	/* Update the utmp file. */
#ifdef	SYSV_STYLE
	update_utmp(options.tty);
#endif

	debug("calling open_tty\n");

	/* Open the tty as standard { input, output, error }. */
	open_tty(options.tty, &termios, options.flags & F_LOCAL);

	tcsetpgrp(STDIN_FILENO, getpid());
	/* Initialize the termios settings (raw mode, eight-bit, blocking i/o). */
	debug("calling termio_init\n");
	termio_init(&options, &termios);

	/* Write the modem init string and DO NOT flush the buffers. */
	if (options.flags & F_INITSTRING) {
		debug("writing init string\n");
		ignore_result(write
			      (STDIN_FILENO, options.initstring,
			       strlen(options.initstring)));
	}

	if (!(options.flags & F_LOCAL))
		/* Go to blocking write mode unless -L is specified. */
		fcntl(STDOUT_FILENO, F_SETFL,
		      fcntl(STDOUT_FILENO, F_GETFL, 0) & ~O_NONBLOCK);

	/* Optionally detect the baud rate from the modem status message. */
	debug("before autobaud\n");
	if (options.flags & F_PARSE)
		auto_baud(&termios);

	/* Set the optional timer. */
	if (options.timeout)
		alarm((unsigned)options.timeout);

	/* Optionally wait for CR or LF before writing /etc/issue */
	if (options.flags & F_WAITCRLF) {
		char ch;

		debug("waiting for cr-lf\n");
		while (read(STDIN_FILENO, &ch, 1) == 1) {
			/* Strip "parity bit". */
			ch &= 0x7f;
#ifdef DEBUGGING
			fprintf(dbf, "read %c\n", ch);
#endif
			if (ch == '\n' || ch == '\r')
				break;
		}
	}

	chardata = init_chardata;
	if (!(options.flags & F_NOPROMPT)) {
		/* Read the login name. */
		debug("reading login name\n");
		while ((logname =
			get_logname(&options, &termios, &chardata)) == 0)
			next_speed(&options, &termios);
	}

	/* Disable timer. */
	if (options.timeout)
		alarm(0);

	/* Finalize the termios settings. */
	termio_final(&options, &termios, &chardata);

	/* Now the newline character should be properly written. */
	ignore_result(write(STDOUT_FILENO, "\n", 1));

	/* Let the login program take care of password validation. */
	execl(options.login, options.login, "--", logname, NULL);
	log_err(_("%s: can't exec %s: %m"), options.tty, options.login);
}

/* Parse command-line arguments. */
static void parse_args(int argc, char **argv, struct options *op)
{
	extern char *optarg;
	extern int optind;
	int c;

	enum {
		VERSION_OPTION = CHAR_MAX + 1,
		HELP_OPTION
	};
	static const struct option longopts[] = {
		{  "8bits",	     no_argument,	 0,  '8'  },
		{  "noreset",	     no_argument,	 0,  'c'  },
		{  "issue-file",     required_argument,  0,  'f'  },
		{  "flow-control",   no_argument,	 0,  'h'  },
		{  "host",	     required_argument,  0,  'H'  },
		{  "noissue",	     no_argument,	 0,  'i'  },
		{  "init-string",    required_argument,  0,  'I'  },
		{  "login-program",  required_argument,  0,  'l'  },
		{  "local-line",     no_argument,	 0,  'L'  },
		{  "extract-baud",   no_argument,	 0,  'm'  },
		{  "skip-login",     no_argument,	 0,  'n'  },
		{  "keep-baud",      no_argument,	 0,  's'  },
		{  "timeout",	     required_argument,  0,  't'  },
		{  "detect-case",    no_argument,	 0,  'U'  },
		{  "wait-cr",	     no_argument,	 0,  'w'  },
		{  "version",	     no_argument,	 0,  VERSION_OPTION  },
		{  "help",	     no_argument,	 0,  HELP_OPTION     },
		{ NULL, 0, 0, 0 }
	};

	while ((c = getopt_long(argc, argv, "8cf:hH:iI:l:Lmnst:Uw", longopts,
			    NULL)) != -1) {
		switch (c) {
		case '8':
			op->flags |= F_EIGHTBITS;
			break;
		case 'c':
			op->flags |= F_KEEPCFLAGS;
			break;
		case 'f':
			op->flags |= F_CUSTISSUE;
			op->issue = optarg;
			break;
		case 'h':
			op->flags |= F_RTSCTS;
			break;
		case 'H':
			fakehost = optarg;
			break;
		case 'i':
			op->flags &= ~F_ISSUE;
			break;
		case 'I':
			/*
			 * FIXME: It would be better to use a separate
			 * function for this task.
			 */
			{
				char ch, *p, *q;
				int i;

				op->initstring = xmalloc(strlen(optarg) + 1);

				/*
				 * Copy optarg into op->initstring decoding \ddd octal
				 * codes into chars.
				 */
				q = op->initstring;
				p = optarg;
				while (*p) {
					/* The \\ is converted to \ */
					if (*p == '\\') {
						p++;
						if (*p == '\\') {
							ch = '\\';
							p++;
						} else {
							/* Handle \000 - \177. */
							ch = 0;
							for (i = 1; i <= 3; i++) {
								if (*p >= '0' && *p <= '7') {
									ch <<= 3;
									ch += *p - '0';
									p++;
								} else {
									break;
								}
							}
						}
						*q++ = ch;
					} else
						*q++ = *p++;
				}
				*q = '\0';
				op->flags |= F_INITSTRING;
				break;
			}
		case 'l':
			op->login = optarg;
			break;
		case 'L':
			op->flags |= F_LOCAL;
			break;
		case 'm':
			op->flags |= F_PARSE;
			break;
		case 'n':
			op->flags |= F_NOPROMPT;
			break;
		case 's':
			op->flags |= F_KEEPSPEED;
			break;
		case 't':
			if ((op->timeout = atoi(optarg)) <= 0)
				log_err(_("bad timeout value: %s"), optarg);
			break;
		case 'U':
			op->flags |= F_LCUC;
			break;
		case 'w':
			op->flags |= F_WAITCRLF;
			break;
		case VERSION_OPTION:
			printf(_("%s from %s\n"), program_invocation_short_name,
			       PACKAGE_STRING);
			exit(EXIT_SUCCESS);
		case HELP_OPTION:
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	debug("after getopt loop\n");

	if (argc < optind + 2) {
		log_warn(_("not enough arguments"));
		usage(stderr);
	}

	/* Accept both "baudrate tty" and "tty baudrate". */
	if ('0' <= argv[optind][0] && argv[optind][0] <= '9') {
		/* Assume BSD style speed. */
		parse_speeds(op, argv[optind++]);
		op->tty = argv[optind];
	} else {
		op->tty = argv[optind++];
		parse_speeds(op, argv[optind]);
	}

	optind++;
	if (argc > optind && argv[optind])
		setenv("TERM", argv[optind], 1);

#ifdef DO_DEVFS_FIDDLING
	/*
	 * Some devfs junk, following Goswin Brederlow:
	 *   turn ttyS<n> into tts/<n>
	 *   turn tty<n> into vc/<n>
	 * http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=72241
	 */
	if (op->tty && strlen(op->tty) < 90) {
		char dev_name[100];
		struct stat st;

		if (strncmp(op->tty, "ttyS", 4) == 0) {
			strcpy(dev_name, "/dev/");
			strcat(dev_name, op->tty);
			if (stat(dev_name, &st) < 0) {
				strcpy(dev_name, "/dev/tts/");
				strcat(dev_name, op->tty + 4);
				if (stat(dev_name, &st) == 0)
					op->tty = xstrdup(dev_name + 5);
			}
		} else if (strncmp(op->tty, "tty", 3) == 0) {
			strcpy(dev_name, "/dev/");
			strncat(dev_name, op->tty, 90);
			if (stat(dev_name, &st) < 0) {
				strcpy(dev_name, "/dev/vc/");
				strcat(dev_name, op->tty + 3);
				if (stat(dev_name, &st) == 0)
					op->tty = xstrdup(dev_name + 5);
			}
		}
	}
#endif				/* DO_DEVFS_FIDDLING */

	debug("exiting parseargs\n");
}

/* Parse alternate baud rates. */
static void parse_speeds(struct options *op, char *arg)
{
	char *cp;

	debug("entered parse_speeds\n");
	for (cp = strtok(arg, ","); cp != 0; cp = strtok((char *)0, ",")) {
		if ((op->speeds[op->numspeed++] = bcode(cp)) <= 0)
			log_err(_("bad speed: %s"), cp);
		if (op->numspeed >= MAX_SPEED)
			log_err(_("too many alternate speeds"));
	}
	debug("exiting parsespeeds\n");
}

#ifdef	SYSV_STYLE

/* Update our utmp entry. */
static void update_utmp(char *line)
{
	struct utmp ut;
	time_t t;
	int mypid = getpid();
	struct utmp *utp;

	/*
	 * The utmp file holds miscellaneous information about things started by
	 * /sbin/init and other system-related events. Our purpose is to update
	 * the utmp entry for the current process, in particular the process type
	 * and the tty line we are listening to. Return successfully only if the
	 * utmp file can be opened for update, and if we are able to find our
	 * entry in the utmp file.
	 */
	utmpname(_PATH_UTMP);
	setutent();

	/*
	 * Find mypid in utmp.
	 *
	 * FIXME: Earlier (when was that?) code here tested only utp->ut_type !=
	 * INIT_PROCESS, so maybe the >= here should be >.
	 *
	 * FIXME: The present code is taken from login.c, so if this is changed,
	 * maybe login has to be changed as well (is this true?).
	 */
	while ((utp = getutent()))
		if (utp->ut_pid == mypid
				&& utp->ut_type >= INIT_PROCESS
				&& utp->ut_type <= DEAD_PROCESS)
			break;

	if (utp) {
		memcpy(&ut, utp, sizeof(ut));
	} else {
		/* Some inits do not initialize utmp. */
		memset(&ut, 0, sizeof(ut));
		strncpy(ut.ut_id, line + 3, sizeof(ut.ut_id));
	}

	strncpy(ut.ut_user, "LOGIN", sizeof(ut.ut_user));
	strncpy(ut.ut_line, line, sizeof(ut.ut_line));
	if (fakehost)
		strncpy(ut.ut_host, fakehost, sizeof(ut.ut_host));
	time(&t);
	ut.ut_time = t;
	ut.ut_type = LOGIN_PROCESS;
	ut.ut_pid = mypid;

	pututline(&ut);
	endutent();

	{
#ifdef HAVE_UPDWTMP
		updwtmp(_PATH_WTMP, &ut);
#else
		int ut_fd;
		int lf;

		if ((lf = open(_PATH_WTMPLOCK, O_CREAT | O_WRONLY, 0660)) >= 0) {
			flock(lf, LOCK_EX);
			if ((ut_fd =
			     open(_PATH_WTMP, O_APPEND | O_WRONLY)) >= 0) {
				ignore_result(write(ut_fd, &ut, sizeof(ut)));
				close(ut_fd);
			}
			flock(lf, LOCK_UN);
			close(lf);
		}
#endif				/* HAVE_UPDWTMP */
	}
}

#endif				/* SYSV_STYLE */

/* Set up tty as stdin, stdout & stderr. */
static void open_tty(char *tty, struct termios *tp, int __attribute__((__unused__)) local)
{
	/* Get rid of the present outputs. */
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	errno = 0;

	/*
	 * Set up new standard input, unless we are given an already opened
	 * port.
	 */
	if (strcmp(tty, "-")) {
		struct stat st;

		/* Sanity checks. */
		if (chdir("/dev"))
			log_err(_("/dev: chdir() failed: %m"));
		if (stat(tty, &st) < 0)
			log_err("/dev/%s: %m", tty);
		if ((st.st_mode & S_IFMT) != S_IFCHR)
			log_err(_("/dev/%s: not a character device"), tty);

		/* Open the tty as standard input. */
		close(STDIN_FILENO);
		errno = 0;

		debug("open(2)\n");
		if (open(tty, O_RDWR | O_NONBLOCK, 0) != 0)
			log_err(_("/dev/%s: cannot open as standard input: %m"),
			      tty);
	} else {
		/*
		 * Standard input should already be connected to an open port. Make
		 * sure it is open for read/write.
		 */
		if ((fcntl(STDIN_FILENO, F_GETFL, 0) & O_RDWR) != O_RDWR)
			log_err(_("%s: not open for read/write"), tty);
	}

	/* Set up standard output and standard error file descriptors. */
	debug("duping\n");

	/* set up stdout and stderr */
	if (dup(STDIN_FILENO) != 1 || dup(STDIN_FILENO) != 2)
		log_err(_("%s: dup problem: %m"), tty);

	/*
	 * The following ioctl will fail if stdin is not a tty, but also when
	 * there is noise on the modem control lines. In the latter case, the
	 * common course of action is (1) fix your cables (2) give the modem
	 * more time to properly reset after hanging up.
	 *
	 * SunOS users can achieve (2) by patching the SunOS kernel variable
	 * "zsadtrlow" to a larger value; 5 seconds seems to be a good value.
	 * http://www.sunmanagers.org/archives/1993/0574.html
	 */
	memset(tp, 0, sizeof(struct termios));
	if (tcgetattr(STDIN_FILENO, tp) < 0)
		log_err("%s: tcgetattr: %m", tty);

	/*
	 * Linux login(1) will change tty permissions. Use root owner and group
	 * with permission -rw------- for the period between getty and login.
	 */
	ignore_result(chown(tty, 0, 0));
	ignore_result(chmod(tty, 0600));
	errno = 0;
}

/* Initialize termios settings. */
static void termio_init(struct options *op, struct termios *tp)
{
	speed_t ispeed, ospeed;

	if (op->flags & F_KEEPSPEED) {
		/* Save the original setting. */
		ispeed = cfgetispeed(tp);
		ospeed = cfgetospeed(tp);
	} else {
		ospeed = ispeed = op->speeds[FIRST_SPEED];
	}

	/*
	 * Initial termios settings: 8-bit characters, raw-mode, blocking i/o.
	 * Special characters are set after we have read the login name; all
	 * reads will be done in raw mode anyway. Errors will be dealt with
	 * later on.
	 */

	 /* Flush input and output queues, important for modems! */
	tcflush(STDIN_FILENO, TCIOFLUSH);

	tp->c_iflag = tp->c_lflag = tp->c_oflag = 0;

	if (!(op->flags & F_KEEPCFLAGS))
		tp->c_cflag = CS8 | HUPCL | CREAD | (tp->c_cflag & CLOCAL);

	/*
	 * Note that the speed is stored in the c_cflag termios field, so we have
	 * set the speed always when the cflag se reseted.
	 */
	cfsetispeed(tp, ispeed);
	cfsetospeed(tp, ospeed);

	if (op->flags & F_LOCAL)
		tp->c_cflag |= CLOCAL;
#ifdef HAVE_STRUCT_TERMIOS_C_LINE
	tp->c_line = 0;
#endif
	tp->c_cc[VMIN] = 1;
	tp->c_cc[VTIME] = 0;

	/* Optionally enable hardware flow control. */
#ifdef	CRTSCTS
	if (op->flags & F_RTSCTS)
		tp->c_cflag |= CRTSCTS;
#endif

	tcsetattr(STDIN_FILENO, TCSANOW, tp);

	/* Go to blocking input even in local mode. */
	fcntl(STDIN_FILENO, F_SETFL,
	      fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);

	debug("term_io 2\n");
}

/* Extract baud rate from modem status message. */
static void auto_baud(struct termios *tp)
{
	speed_t speed;
	int vmin;
	unsigned iflag;
	char buf[BUFSIZ];
	char *bp;
	int nread;

	/*
	 * This works only if the modem produces its status code AFTER raising
	 * the DCD line, and if the computer is fast enough to set the proper
	 * baud rate before the message has gone by. We expect a message of the
	 * following format:
	 *
	 * <junk><number><junk>
	 *
	 * The number is interpreted as the baud rate of the incoming call. If the
	 * modem does not tell us the baud rate within one second, we will keep
	 * using the current baud rate. It is advisable to enable BREAK
	 * processing (comma-separated list of baud rates) if the processing of
	 * modem status messages is enabled.
	 */

	/*
	 * Use 7-bit characters, don't block if input queue is empty. Errors will
	 * be dealt with later on.
	 */
	iflag = tp->c_iflag;
	/* Enable 8th-bit stripping. */
	tp->c_iflag |= ISTRIP;
	vmin = tp->c_cc[VMIN];
	/* Do not block when queue is empty. */
	tp->c_cc[VMIN] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, tp);

	/*
	 * Wait for a while, then read everything the modem has said so far and
	 * try to extract the speed of the dial-in call.
	 */
	sleep(1);
	if ((nread = read(STDIN_FILENO, buf, sizeof(buf) - 1)) > 0) {
		buf[nread] = '\0';
		for (bp = buf; bp < buf + nread; bp++)
			if (isascii(*bp) && isdigit(*bp)) {
				if ((speed = bcode(bp))) {
					cfsetispeed(tp, speed);
					cfsetospeed(tp, speed);
				}
				break;
			}
	}

	/* Restore terminal settings. Errors will be dealt with later on. */
	tp->c_iflag = iflag;
	tp->c_cc[VMIN] = vmin;
	tcsetattr(STDIN_FILENO, TCSANOW, tp);
}

/* Show login prompt, optionally preceded by /etc/issue contents. */
static void do_prompt(struct options *op, struct termios *tp)
{
#ifdef	ISSUE
	FILE *fd;
	int oflag;
	int c, i;
	struct utsname uts;

	uname(&uts);
#endif				/* ISSUE */

	/* Issue not in use, start with a new line. */
	ignore_result(write(STDOUT_FILENO, "\r\n", 2));
#ifdef	ISSUE
	if ((op->flags & F_ISSUE) && (fd = fopen(op->issue, "r"))) {
		/* Save current setting. */
		oflag = tp->c_oflag;
		/* Map new line in output to carriage return & new line. */
		tp->c_oflag |= (ONLCR | OPOST);
		tcsetattr(STDIN_FILENO, TCSADRAIN, tp);

		while ((c = getc(fd)) != EOF) {
			if (c == '\\') {
				c = getc(fd);

				switch (c) {
				case 's':
					printf("%s", uts.sysname);
					break;
				case 'n':
					printf("%s", uts.nodename);
					break;
				case 'r':
					printf("%s", uts.release);
					break;
				case 'v':
					printf("%s", uts.version);
					break;
				case 'm':
					printf("%s", uts.machine);
					break;
				case 'o':
					/*
					 * FIXME: It would be better to use a
					 * separate function for this task.
					 */
					{
						char domainname[MAXHOSTNAMELEN + 1];
#ifdef HAVE_GETDOMAINNAME
						if (getdomainname(domainname, sizeof(domainname)))
#endif				/* HAVE_GETDOMAINNAME */
							strcpy(domainname, "unknown_domain");
						domainname[sizeof(domainname) - 1] = '\0';
						printf("%s", domainname);
						break;
					}
				case 'O':
					/*
					 * FIXME: It would be better to use a
					 * separate function for this task.
					 */
					{
						char *dom = "unknown_domain";
						char host[MAXHOSTNAMELEN + 1];
						struct addrinfo hints, *info = NULL;

						memset(&hints, 0, sizeof(hints));
						hints.ai_flags = AI_CANONNAME;

						if (gethostname(host, sizeof(host))
						    || getaddrinfo(host, NULL, &hints, &info)
						    || info == NULL) {
							fputs(dom, stdout);
						} else {
							char *canon;

							if (info->ai_canonname
							    && (canon =
								strchr(info->ai_canonname, '.'))) {
								dom = canon + 1;
							}
							fputs(dom, stdout);
							freeaddrinfo(info);
						}
						break;
					}
				case 'd':
				case 't':
					/*
					 * FIXME: It would be better to use a
					 * separate function for this task.
					 */
					{
						time_t now;
						struct tm *tm;

						time(&now);
						tm = localtime(&now);

						if (c == 'd')
							printf
							    ("%s %s %d  %d",
							     nl_langinfo(ABDAY_1 + tm->tm_wday),
							     nl_langinfo(ABMON_1 + tm->tm_mon),
							     tm->tm_mday,
							     /* FIXME: y2070 bug */
							     tm->tm_year < 70 ?
								 tm->tm_year + 2000 :
								 tm->tm_year + 1900);
						else
							printf("%02d:%02d:%02d",
							       tm->tm_hour, tm->tm_min, tm->tm_sec);
						break;
					}
				case 'l':
					printf("%s", op->tty);
					break;
				case 'b':
					for (i = 0; speedtab[i].speed; i++)
						if (speedtab[i].code == cfgetispeed(tp))
							printf("%ld", speedtab[i].speed);
					break;
					break;
				case 'u':
				case 'U':
					/*
					 * FIXME: It would be better to use a
					 * separate function for this task.
					 */
					{
						int users = 0;
						struct utmp *ut;
						setutent();
						while ((ut = getutent()))
							if (ut->ut_type == USER_PROCESS)
								users++;
						endutent();
						printf("%d ", users);
						if (c == 'U')
							printf((users == 1) ?
								_("user") : _("users"));
						break;
					}
				default:
					putchar(c);
				}
			} else
				putchar(c);
		}
		fflush(stdout);

		/* Restore settings. */
		tp->c_oflag = oflag;
		/* Wait till output is gone. */
		tcsetattr(STDIN_FILENO, TCSADRAIN, tp);
		fclose(fd);
	}
#endif				/* ISSUE */
	{
		char hn[MAXHOSTNAMELEN + 1];
		if (gethostname(hn, sizeof(hn)) == 0)
			ignore_result(write(STDIN_FILENO, hn, strlen(hn)));
	}
	/* Always show login prompt. */
	ignore_result(write(STDOUT_FILENO, LOGIN, sizeof(LOGIN) - 1));
}

/* Select next baud rate. */
static void next_speed(struct options *op, struct termios *tp)
{
	static int baud_index = -1;

	if (baud_index == -1)
		/*
		 * If the F_KEEPSPEED flags is set then the FIRST_SPEED is not
		 * tested yet (see termio_init()).
		 */
		baud_index =
		    (op->flags & F_KEEPSPEED) ? FIRST_SPEED : 1 % op->numspeed;
	else
		baud_index = (baud_index + 1) % op->numspeed;

	cfsetispeed(tp, op->speeds[baud_index]);
	cfsetospeed(tp, op->speeds[baud_index]);
	tcsetattr(STDIN_FILENO, TCSANOW, tp);
}

/* Get user name, establish parity, speed, erase, kill & eol. */
static char *get_logname(struct options *op, struct termios *tp, struct chardata *cp)
{
	static char logname[BUFSIZ];
	char *bp;
	char c;			/* input character, full eight bits */
	char ascval;		/* low 7 bits of input character */
	int bits;		/* # of "1" bits per character */
	int mask;		/* mask with 1 bit up */
	static char *erase[] = {	/* backspace-space-backspace */
		"\010\040\010",		/* space parity */
		"\010\040\010",		/* odd parity */
		"\210\240\210",		/* even parity */
		"\210\240\210",		/* no parity */
	};

	/* Initialize kill, erase, parity etc. (also after switching speeds). */
	*cp = init_chardata;

	/*
	 * Flush pending input (especially important after parsing or switching
	 * the baud rate).
	 */
	sleep(1);
	tcflush(STDIN_FILENO, TCIFLUSH);

	/* Prompt for and read a login name. */
	for (*logname = 0; *logname == 0; /* void */ ) {
		/* Write issue file and prompt, with "parity" bit == 0. */
		do_prompt(op, tp);

		/*
		 * Read name, watch for break, parity, erase, kill,
		 * end-of-line.
		 */
		for (bp = logname, cp->eol = 0; cp->eol == 0; /* void */ ) {
			/* Do not report trivial EINTR/EIO errors. */
			if (read(STDIN_FILENO, &c, 1) < 1) {
				if (errno == EINTR || errno == EIO)
					exit(EXIT_SUCCESS);
				log_err(_("%s: read: %m"), op->tty);
			}
			/* Do BREAK handling elsewhere. */
			if ((c == 0) && op->numspeed > 1)
				return EXIT_SUCCESS;
			/* Do parity bit handling. */
			if (op->flags & F_EIGHTBITS) {
				ascval = c;
			} else if (c != (ascval = (c & 0177))) {
				/* Set "parity" bit on. */
				for (bits = 1, mask = 1; mask & 0177;
				     mask <<= 1)
					if (mask & ascval)
						/* Count "1" bits. */
						bits++;
				cp->parity |= ((bits & 1) ? 1 : 2);
			}
			/* Do erase, kill and end-of-line processing. */
			switch (ascval) {
			case CR:
			case NL:
				/* Terminate logname. */
				*bp = 0;
				/* Set end-of-line char. */
				cp->eol = ascval;
				break;
			case BS:
			case DEL:
			case '#':
				/* Set erase character. */
				cp->erase = ascval;
				if (bp > logname) {
					ignore_result(write
						      (STDIN_FILENO,
						       erase[cp->parity], 3));
					bp--;
				}
				break;
			case CTL('U'):
			case '@':
				/* Set kill character. */
				cp->kill = ascval;
				while (bp > logname) {
					ignore_result(write
						      (STDIN_FILENO,
						       erase[cp->parity], 3));
					bp--;
				}
				break;
			case CTL('D'):
				exit(EXIT_SUCCESS);
			default:
				if (!isascii(ascval) || !isprint(ascval)) {
					/* Ignore garbage characters. */ ;
				} else if ((size_t)(bp - logname) >= sizeof(logname) - 1) {
					log_err(_("%s: input overrun"), op->tty);
				} else {
					/* Echo the character... */
					ignore_result(write
						      (STDIN_FILENO, &c, 1));
					/* ...and store it. */
					*bp++ = ascval;
				}
				break;
			}
		}
	}
	/* Handle names with upper case and no lower case. */
	if ((op->flags & F_LCUC) && (cp->capslock = caps_lock(logname)))
		for (bp = logname; *bp; bp++)
			if (isupper(*bp))
				*bp = tolower(*bp);
	return logname;
}

/* Set the final tty mode bits. */
static void termio_final(struct options *op, struct termios *tp, struct chardata *cp)
{
	/* General terminal-independent stuff. */

	/* 2-way flow control */
	tp->c_iflag |= IXON | IXOFF;
	tp->c_lflag |= ICANON | ISIG | ECHO | ECHOE | ECHOK | ECHOKE;
	/* no longer| ECHOCTL | ECHOPRT */
	tp->c_oflag |= OPOST;
	/* tp->c_cflag = 0; */
	tp->c_cc[VINTR] = DEF_INTR;
	tp->c_cc[VQUIT] = DEF_QUIT;
	tp->c_cc[VEOF] = DEF_EOF;
	tp->c_cc[VEOL] = DEF_EOL;
#ifdef __linux__
	tp->c_cc[VSWTC] = DEF_SWITCH;
#elif defined(VSWTCH)
	tp->c_cc[VSWTCH] = DEF_SWITCH;
#endif				/* __linux__ */

	/* Account for special characters seen in input. */
	if (cp->eol == CR) {
		tp->c_iflag |= ICRNL;
		tp->c_oflag |= ONLCR;
	}
	tp->c_cc[VERASE] = cp->erase;
	tp->c_cc[VKILL] = cp->kill;

	/* Account for the presence or absence of parity bits in input. */
	switch (cp->parity) {
	case 0:
		/* space (always 0) parity */
		break;
	case 1:
		/* odd parity */
		tp->c_cflag |= PARODD;
		/* do not break */
	case 2:
		/* even parity */
		tp->c_cflag |= PARENB;
		tp->c_iflag |= INPCK | ISTRIP;
		/* do not break */
	case (1 | 2):
		/* no parity bit */
		tp->c_cflag &= ~CSIZE;
		tp->c_cflag |= CS7;
		break;
	}
	/* Account for upper case without lower case. */
	if (cp->capslock) {
#ifdef IUCLC
		tp->c_iflag |= IUCLC;
#endif
#ifdef XCASE
		tp->c_lflag |= XCASE;
#endif
#ifdef OLCUC
		tp->c_oflag |= OLCUC;
#endif
	}
	/* Optionally enable hardware flow control. */
#ifdef	CRTSCTS
	if (op->flags & F_RTSCTS)
		tp->c_cflag |= CRTSCTS;
#endif

	/* Finally, make the new settings effective. */
	if (tcsetattr(STDIN_FILENO, TCSANOW, tp) < 0)
		log_err("%s: tcsetattr: TCSANOW: %m", op->tty);
}

/*
 * String contains upper case without lower case.
 * http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=52940
 * http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=156242
 */
static int caps_lock(char *s)
{
	int capslock;

	for (capslock = 0; *s; s++) {
		if (islower(*s))
			return EXIT_SUCCESS;
		if (capslock == 0)
			capslock = isupper(*s);
	}
	return capslock;
}

/* Convert speed string to speed code; return 0 on failure. */
static speed_t bcode(char *s)
{
	struct Speedtab *sp;
	long speed = atol(s);

	for (sp = speedtab; sp->speed; sp++)
		if (sp->speed == speed)
			return sp->code;
	return 0;
}

static void __attribute__ ((__noreturn__)) usage(FILE * out)
{
	fprintf(out, _("\nUsage:\n"
		       "    %1$s [options] line baud_rate,... [termtype]\n"
		       "    %1$s [options] baud_rate,... line [termtype]\n"),
		program_invocation_short_name);

	fprintf(out, _("\nOptions:\n"
		       " -8, --8bits                assume 8-bit tty\n"
		       " -c, --noreset              do not reset control mode\n"
		       " -f, --issue-file FILE      display issue file\n"
		       " -h, --flow-control         enable hardware flow control\n"
		       " -H, --host HOSTNAME        specify login host\n"
		       " -i, --noissue              do not display issue file\n"
		       " -I, --init-string STRING   set init string\n"
		       " -l, --login-program FILE   specify login program\n"
		       " -L, --local-line           force local line\n"
		       " -m, --extract-baud         extract baud rate during connect\n"
		       " -n, --skip-login           do not prompt for login\n"
		       " -s, --keep-baud            try to keep baud rate after break\n"
		       " -t, --timeout NUMBER       login process timeout\n"
		       " -U, --detect-case          detect uppercase terminal\n"
		       " -w, --wait-cr              wait carriage-return\n"
		       "     --version              output version information and exit\n"
		       "     --help                 display this help and exit\n\n"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

/*
 * Helper function reports errors to console or syslog.
 * Will be used by log_err() and log_warn() therefore
 * it takes a format as well as va_list.
 */
#define	str2cpy(b,s1,s2)	strcat(strcpy(b,s1),s2)

static void dolog(int priority, const char *fmt, va_list ap)
{
#ifndef	USE_SYSLOG
	int fd;
#endif
	char buf[BUFSIZ];
	char *bp;

	/*
	 * If the diagnostic is reported via syslog(3), the process name is
	 * automatically prepended to the message. If we write directly to
	 * /dev/console, we must prepend the process name ourselves.
	 */
#ifdef USE_SYSLOG
	buf[0] = '\0';
	bp = buf;
#else
	str2cpy(buf, program_invocation_short_name, ": ");
	bp = buf + strlen(buf);
#endif				/* USE_SYSLOG */
	vsnprintf (bp, sizeof(buf)-strlen(buf), fmt, ap);

	/*
	 * Write the diagnostic directly to /dev/console if we do not use the
	 * syslog(3) facility.
	 */
#ifdef	USE_SYSLOG
	openlog(program_invocation_short_name, LOG_PID, LOG_AUTHPRIV);
	syslog(priority, "%s", buf);
	closelog();
#else
	/* Terminate with CR-LF since the console mode is unknown. */
	strcat(bp, "\r\n");
	if ((fd = open("/dev/console", 1)) >= 0) {
		ignore_result(write(fd, buf, strlen(buf)));
		close(fd);
	}
#endif				/* USE_SYSLOG */
}

static void log_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	dolog(LOG_ERR, fmt, ap);
	va_end(ap);

	/* Be kind to init(8). */
	sleep(10);
	exit(EXIT_FAILURE);
}

static void log_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	dolog(LOG_WARNING, fmt, ap);
	va_end(ap);
}