/*
 * gtp.c - a minimal GTP (Go Text Protocol, version 2) front-end for the
 * Many Faces of Go engine.
 */

#include "ui2g2.h"
#include "learn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int gsize = 19;
static int ghandicap = 0;
static int gkomi50 = 325;
static int glast_atari = FALSE;	/* last move made gave atari */
static int grules = JAPANESE;
static int glevel = UCTLEVEL;
static int gtime_ms = 3000;
static unsigned int gtime_left = 0;
static unsigned int gstones_left = 0;

static FILE *gtpout;

/* room for "b Q16 " per move */
#define NUMMOVES_OUT ((MAXMOVE + 100) * 8)

/* engine move stack (g2uihd.h), used to report the last move */
extern unsigned int msptr;
extern sqr_t mvs[];
extern char mvcolor[];

/* ------------------------------------------------------------------ */
/* calls the engine makes back into the "user interface"               */
/* ------------------------------------------------------------------ */

int thinking(void) { return FALSE; }

static char stringbuf[256];
char *getstring(int page, int line)
{
	snprintf(stringbuf, sizeof(stringbuf),
		 "engine string page %d line %d", page, line);
	return stringbuf;
}

void problemprogress(int size, int result, int confidence) { (void)size;(void)result;(void)confidence; }
void outerr(const char *s) { fputs(s, stderr); fputc('\n', stderr); }
void outstone(point_t s, char *c) { (void)s;(void)c; }
void histone(point_t s) { (void)s; }
void lookmove(point_t s, int c) { (void)s;(void)c; }
void looktakeback(point_t s) { (void)s; }
void lookbackup(point_t s) { (void)s; }
void addcomment(char *s) { (void)s; }

/* ------------------------------------------------------------------ */
/* coordinate helpers                                                  */
/* ------------------------------------------------------------------ */

static int parse_color(const char *s)
{
	if (s == NULL)
		return NOCOLOR;
	if (s[0] == 'b' || s[0] == 'B')
		return BLACKCOLOR;
	if (s[0] == 'w' || s[0] == 'W')
		return WHITECOLOR;
	return NOCOLOR;
}

static int parse_vertex(const char *s, point_t *out)
{
	int col, row;
	char c;

	if (s == NULL)
		return 0;
	if (strcasecmp(s, "pass") == 0 || strcasecmp(s, "resign") == 0) {
		*out = PASS;
		return 1;
	}
	c = (char)tolower((unsigned char)s[0]);
	if (c < 'a' || c > 'z')
		return 0;
	if (c >= 'j')
		c--;
	col = c - 'a';
	if (col < 0 || col >= gsize)
		return 0;
	if (!isdigit((unsigned char)s[1]))
		return 0;
	row = atoi(s + 1);
	if (row < 1 || row > gsize)
		return 0;
	*out = (point_t)((gsize - row) * gsize + col);
	return 1;
}

static void format_vertex(point_t s, char *buf)
{
	int col, row;
	char c;

	if (s == PASS) {
		strcpy(buf, "pass");
		return;
	}
	col = s % gsize;
	row = gsize - s / gsize;
	c = (char)('A' + col);
	if (c >= 'I')
		c++;
	sprintf(buf, "%c%d", c, row);
}

/* ------------------------------------------------------------------ */
/* engine control                                                      */
/* ------------------------------------------------------------------ */

static void newgame(void)
{
	initvars(gsize, gkomi50, 1, 10000000);
	glast_atari = FALSE;
}

static g2status do_play(int color, point_t p)
{
	int atari = FALSE;
	g2status st = makeamove(p, color, grules, &atari, FALSE);
	glast_atari = (st == G2OK && atari);
	if (st != G2OK)
		fprintf(stderr, "makeamove rejected color=%d point=%d status=%d\n",
			color, p, st);
	return st;
}

/*
 * fixed handicap placement as in the GTP spec, on any odd board size:
 * 2-4 corners, 5 adds the center, 6 corners + left/right sides,
 * 7 adds the center, 8 corners + all sides, 9 everything.
 * Points are {col, row} with 0 = low (3-3 or 4-4 point), 1 = mid, 2 = high.
 */
static const int hcorner[4][2] = { {0, 0}, {2, 2}, {2, 0}, {0, 2} };
static const int hside_lr[2][2] = { {0, 1}, {2, 1} };
static const int hside_tb[2][2] = { {1, 0}, {1, 2} };
static const int hcenter[2] = { 1, 1 };

static int handicap_points(int n, point_t *pts)
{
	int low = (gsize >= 13) ? 3 : 2;
	int coord[3];
	int sel[9][2];
	int i, k = 0;

	if (n < 2 || n > 9 || gsize < 7 || gsize % 2 == 0)
		return 0;
	coord[0] = low;
	coord[1] = (gsize - 1) / 2;
	coord[2] = gsize - 1 - low;

	for (i = 0; i < 4 && i < n; ++i, ++k) {
		sel[k][0] = hcorner[i][0];
		sel[k][1] = hcorner[i][1];
	}
	if (n >= 6)
		for (i = 0; i < 2; ++i, ++k) {
			sel[k][0] = hside_lr[i][0];
			sel[k][1] = hside_lr[i][1];
		}
	if (n >= 8)
		for (i = 0; i < 2; ++i, ++k) {
			sel[k][0] = hside_tb[i][0];
			sel[k][1] = hside_tb[i][1];
		}
	if (n % 2 == 1 && n >= 5) {
		sel[k][0] = hcenter[0];
		sel[k][1] = hcenter[1];
		++k;
	}
	for (i = 0; i < k; ++i) {
		int col = coord[sel[i][0]];
		int row = coord[sel[i][1]];	/* counted from the bottom */
		pts[i] = (point_t)((gsize - 1 - row) * gsize + col);
	}
	return k;
}

/* ------------------------------------------------------------------ */
/* GTP command handling                                                */
/* ------------------------------------------------------------------ */

static void reply(const char *id, const char *msg)
{
	if (msg == NULL || msg[0] == '\0')
		fprintf(gtpout, "=%s\n\n", id[0] ? id : "");
	else
		fprintf(gtpout, "=%s %s\n\n", id[0] ? id : "", msg);
	fflush(gtpout);
}

static void fail(const char *id, const char *msg)
{
	fprintf(gtpout, "?%s %s\n\n", id[0] ? id : "", msg ? msg : "error");
	fflush(gtpout);
}

static const char *known[] = {
	"protocol_version", "name", "version", "known_command",
	"list_commands", "boardsize", "clear_board", "komi", "play",
	"genmove", "undo", "showboard", "final_score", "fixed_handicap",
	"place_free_handicap", "time_settings", "time_left", "level",
	"igowin_state", "igowin_score", "igowin_hint", "igowin_moves",
	"igowin_learn", "quit", NULL
};

static void cmd_showboard(void)
{
	char pointcolors[NUMPOINTS];
	char buf[256];
	int i, j;
	int bp = 0, wp = 0;

	getboard(pointcolors);
	getprisoners(&bp, &wp);
	snprintf(buf, sizeof(buf),
		 "Prisoners B:%d W:%d  Komi:%d/50  Handicap:%d",
		 bp, wp, gkomi50, ghandicap);
	reply("", buf);
	for (i = 0; i < gsize; ++i) {
		char row[128];
		int n = 0;
		n += sprintf(row + n, "%2d ", gsize - i);
		for (j = 0; j < gsize; ++j) {
			char pc = pointcolors[i * gsize + j];
			if (pc == BLACKCOLOR)
				n += sprintf(row + n, "O ");
			else if (pc == WHITECOLOR)
				n += sprintf(row + n, "@ ");
			else
				n += sprintf(row + n, "+ ");
		}
		fprintf(stderr, "%s\n", row);
	}
}

/*
 * igowin_state: one-line JSON snapshot for the web UI, e.g.
 * {"size":9,"moves":3,"last":"E5","lastcolor":"w","pris":[0,1],"atari":false,"board":"..X..O..."}
 * board is row-major from the top row, X = black, O = white, . = empty.
 */
static void cmd_state(const char *id)
{
	char pointcolors[NUMPOINTS];
	char out[NUMPOINTS + 256];
	char last[8] = "";
	const char *lastcolor = "";
	int bp = 0, wp = 0;
	int n, i;

	getboard(pointcolors);
	getprisoners(&bp, &wp);
	if (msptr > 0) {
		format_vertex(mvs[msptr - 1], last);
		lastcolor = mvcolor[msptr - 1] == BLACKCOLOR ? "b" : "w";
	}
	n = sprintf(out, "{\"size\":%d,\"moves\":%u,\"last\":\"%s\",\"lastcolor\":\"%s\","
		    "\"pris\":[%d,%d],\"atari\":%s,\"board\":\"",
		    gsize, msptr, last, lastcolor, bp, wp, glast_atari ? "true" : "false");
	for (i = 0; i < gsize * gsize; ++i)
		out[n++] = pointcolors[i] == BLACKCOLOR ? 'X' :
			   pointcolors[i] == WHITECOLOR ? 'O' : '.';
	strcpy(out + n, "\"}");
	reply(id, out);
}

static void format_score(int score, char *sb)
{
	if (score > 0)
		sprintf(sb, "W+%d.5", score);
	else if (score == 0)
		sprintf(sb, "W+0.5");
	else
		sprintf(sb, "B+%d.5", -score - 1);
}

/*
 * igowin_score [vertex|keep]: one-line JSON for the scoring screen, e.g.
 * {"result":"B+3.5","komi":6.5,"black":[terr,pris],"white":[terr,pris],"map":"..."}
 * map is row-major from the top row: b/w = territory, . = neutral,
 * X/O = live stones, x/o = dead stones.  Dead stones count as territory
 * and prisoners for the other side (Japanese counting).
 * With a vertex, the group there is toggled dead/alive; "keep" keeps
 * earlier toggles; no argument starts from the engine's own judgement.
 */
static void cmd_score(const char *id, const char *arg)
{
	int color[NUMPOINTS + 1], dead[NUMPOINTS + 1];
	char pointcolors[NUMPOINTS];
	char out[NUMPOINTS + 256];
	char sb[32];
	int swap = -1;
	int terr[2] = {0, 0}, pris[2] = {0, 0};
	int score, n, i;
	point_t v;

	if (arg != NULL && strcmp(arg, "keep") == 0)
		swap = -2;
	else if (arg != NULL && parse_vertex(arg, &v) && v != PASS)
		swap = v;

	score = getthescore(grules == GOE || grules == CHINESE, ghandicap,
			    swap, color, dead);
	getboard(pointcolors);
	getprisoners(&pris[BLACKCOLOR], &pris[WHITECOLOR]);
	format_score(score, sb);

	n = sprintf(out, "{\"result\":\"%s\",\"komi\":%.1f,\"map\":\"", sb, gkomi50 / 50.0);
	for (i = 0; i < gsize * gsize; ++i) {
		char c;
		if (pointcolors[i] == BLACKCOLOR || pointcolors[i] == WHITECOLOR) {
			int stone = pointcolors[i];
			if (dead[i]) {
				terr[1 - stone]++;
				pris[1 - stone]++;
				c = stone == BLACKCOLOR ? 'x' : 'o';
			} else {
				c = stone == BLACKCOLOR ? 'X' : 'O';
			}
		} else if (color[i] == BLACKCOLOR || color[i] == WHITECOLOR) {
			terr[color[i]]++;
			c = color[i] == BLACKCOLOR ? 'b' : 'w';
		} else {
			c = '.';
		}
		out[n++] = c;
	}
	sprintf(out + n, "\",\"black\":[%d,%d],\"white\":[%d,%d]}",
		terr[BLACKCOLOR], pris[BLACKCOLOR], terr[WHITECOLOR], pris[WHITECOLOR]);
	reply(id, out);
}

/* igowin_moves: all moves so far, e.g. "b Q16 w D4 b pass" */
static void cmd_moves(const char *id)
{
	static char out[NUMMOVES_OUT];
	unsigned int i;
	int n = 0;

	out[0] = 0;
	for (i = 0; i < msptr; ++i) {
		char vb[8];
		format_vertex(mvs[i], vb);
		n += sprintf(out + n, "%s%c %s", i ? " " : "",
			     mvcolor[i] == BLACKCOLOR ? 'b' : 'w', vb);
	}
	reply(id, out);
}

/*
 * igowin_learn <result> <black strength> <white strength>
 * result: 0 black wins, 1 white wins, 2 tie.  Strength as in learn_game:
 * -10 is 10 kyu, 3 is 3 dan.  Adds the finished game (the position in
 * the engine) to the learned patterns and saves learn.dat/gamedb.dat,
 * so the engine avoids repeating lost lines.
 */
static void cmd_learn(const char *id, int ntok, char **tok)
{
	static struct learn_game game;
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (ntok < 4) {
		fail(id, "syntax: igowin_learn <result> <black strength> <white strength>");
		return;
	}
	memset(&game, 0, sizeof(game));
	strcpy(game.names[0], "Black");
	strcpy(game.names[1], "White");
	snprintf(game.filename, sizeof(game.filename), "igowin-web-%ld", (long)now);
	strcpy(game.event, "igowin web");
	game.boardsize = (char)gsize;
	game.handicap = (char)ghandicap;
	game.result = (char)atoi(tok[1]);
	game.strength[0] = (char)atoi(tok[2]);
	game.strength[1] = (char)atoi(tok[3]);
	game.year = (short)(tm->tm_year + 1900);
	game.month = (char)(tm->tm_mon + 1);
	game.day = (char)tm->tm_mday;
	game.move_count = 0;		/* learn from the position in the engine */
	if (!learn_add_current_game(&game, TRUE))
		fail(id, "game not learned");
	else if (!learn_save())
		fail(id, "could not save learn.dat");
	else
		reply(id, "");
}

static int handle_line(char *line)
{
	char *tok[64];
	int ntok = 0;
	char *p = line;
	char id[32] = "";

	line[strcspn(line, "\r\n")] = 0;
	while (*p && ntok < 64) {
		while (*p == ' ' || *p == '\t')
			++p;
		if (*p == 0)
			break;
		tok[ntok++] = p;
		while (*p && *p != ' ' && *p != '\t')
			++p;
		if (*p)
			*p++ = 0;
	}
	if (ntok == 0)
		return 0;

	{
		const char *t = tok[0];
		int numeric = 1;
		while (*t) {
			if (!isdigit((unsigned char)*t)) { numeric = 0; break; }
			++t;
		}
		if (numeric && t != tok[0]) {
			strncpy(id, tok[0], sizeof(id) - 1);
			id[sizeof(id) - 1] = 0;
			memmove(tok, tok + 1, (ntok - 1) * sizeof(tok[0]));
			--ntok;
		}
		if (ntok == 0)
			return 0;
	}

	fprintf(stderr, "gtp> %s\n", tok[0]);

	if (strcmp(tok[0], "protocol_version") == 0) {
		reply(id, "2");
	} else if (strcmp(tok[0], "name") == 0) {
		reply(id, "MFGO (Many Faces of Go engine, 2007)");
	} else if (strcmp(tok[0], "version") == 0) {
		reply(id, "1.0");
	} else if (strcmp(tok[0], "known_command") == 0) {
		int i, found = 0;
		if (ntok >= 2)
			for (i = 0; known[i]; ++i)
				if (strcmp(known[i], tok[1]) == 0)
					found = 1;
		reply(id, found ? "true" : "false");
	} else if (strcmp(tok[0], "list_commands") == 0) {
		int i;
		for (i = 0; known[i]; ++i)
			fprintf(gtpout, "%s\n", known[i]);
		reply(id, "");
	} else if (strcmp(tok[0], "boardsize") == 0) {
		int n = (ntok >= 2) ? atoi(tok[1]) : gsize;
		if (n < 9 || n > 19) {
			fail(id, "unacceptable size");
		} else {
			gsize = n;
			ghandicap = 0;
			newgame();
			reply(id, "");
		}
	} else if (strcmp(tok[0], "clear_board") == 0) {
		ghandicap = 0;
		newgame();
		reply(id, "");
	} else if (strcmp(tok[0], "komi") == 0) {
		if (ntok < 2) {
			fail(id, "syntax: komi <value>");
		} else {
			gkomi50 = (int)(atof(tok[1]) * 50.0 + 0.5);
			initkomi(gkomi50);
			reply(id, "");
		}
	} else if (strcmp(tok[0], "play") == 0) {
		int color;
		point_t v;
		if (ntok < 3) {
			fail(id, "syntax: play <color> <vertex>");
		} else if ((color = parse_color(tok[1])) == NOCOLOR) {
			fail(id, "invalid color");
		} else if (!parse_vertex(tok[2], &v)) {
			fail(id, "invalid vertex");
		} else {
			g2status st = do_play(color, v);
			if (st == G2OK)
				reply(id, "");
			else if (st == G2KO)
				fail(id, "illegal move: ko");
			else if (st == G2SUICIDE)
				fail(id, "illegal move: suicide");
			else
				fail(id, "illegal move");
		}
	} else if (strcmp(tok[0], "genmove") == 0) {
		int color;
		point_t mv = PASS;
		g2status st;
		char vbuf[8];
		if (ntok < 2) {
			fail(id, "syntax: genmove <color>");
		} else if ((color = parse_color(tok[1])) == NOCOLOR) {
			fail(id, "invalid color");
		} else {
			unsigned int tl = gtime_left ? gtime_left : (unsigned)gtime_ms;
			unsigned int sl = gtime_left ? gstones_left : 0;
			/*
			 * The engine may only pass after half the board is filled or
			 * after the opponent passed (allowpass in compmove).  The weakest
			 * levels sometimes find no move with a good reason, e.g. against
			 * 5 handicap stones on 9x9, and return a pass anyway.  Then ask
			 * the next stronger level for this one move.
			 */
			int may_pass = (int)msptr > gsize * gsize / 2 ||
				       (msptr > 0 && mvs[msptr - 1] == PASS);
			int lvl = glevel;
			for (;;) {
				st = compmove(FALSE, color, ghandicap, grules, &mv,
					      PASS, tl, sl, lvl, FALSE, 0, NULL,
					      FALSE, 0, TRUE);
				if (st != G2OK || mv != PASS || may_pass || lvl >= UCTLEVEL)
					break;
				++lvl;
				fprintf(stderr, "level %d found no move, trying level %d\n", lvl - 1, lvl);
			}
			if (st != G2OK && st != G2RESIGN) {
				fail(id, "engine error generating move");
			} else if (st == G2RESIGN) {
				reply(id, "resign");
			} else {
				int atari = FALSE;
				makeamove(mv, color, grules, &atari, FALSE);
				glast_atari = atari;
				format_vertex(mv, vbuf);
				reply(id, vbuf);
			}
		}
	} else if (strcmp(tok[0], "undo") == 0) {
		point_t s;
		glast_atari = FALSE;
		if (takebackamove(grules, &s) == G2OK)
			reply(id, "");
		else
			fail(id, "cannot undo");
	} else if (strcmp(tok[0], "igowin_state") == 0) {
		cmd_state(id);
	} else if (strcmp(tok[0], "showboard") == 0) {
		cmd_showboard();
		reply(id, "");
	} else if (strcmp(tok[0], "final_score") == 0) {
		int color[NUMPOINTS + 1], dead[NUMPOINTS + 1];
		int score = getthescore(grules == GOE || grules == CHINESE,
					ghandicap, -1, color, dead);
		char sb[32];
		format_score(score, sb);
		reply(id, sb);
	} else if (strcmp(tok[0], "igowin_hint") == 0) {
		/* igowin_hint <color> [n]: the n best moves, best first */
		int color = (ntok >= 2) ? parse_color(tok[1]) : NOCOLOR;
		int num = (ntok >= 3) ? atoi(tok[2]) : 3;
		point_t pts[5];
		int vals[5];
		if (color == NOCOLOR) {
			fail(id, "syntax: igowin_hint <color> [n]");
		} else {
			if (num < 1) num = 1;
			if (num > 5) num = 5;
			if (compmove_multi(FALSE, color, ghandicap, grules, &num, pts, vals,
					   (unsigned)gtime_ms, MAXLEVEL) != G2OK) {
				fail(id, "engine error generating hints");
			} else {
				char out[64] = "";
				int i;
				for (i = 0; i < num; ++i) {
					char vb[8];
					format_vertex(pts[i], vb);
					if (i) strcat(out, " ");
					strcat(out, vb);
				}
				reply(id, out);
			}
		}
	} else if (strcmp(tok[0], "igowin_moves") == 0) {
		cmd_moves(id);
	} else if (strcmp(tok[0], "igowin_learn") == 0) {
		cmd_learn(id, ntok, tok);
	} else if (strcmp(tok[0], "igowin_score") == 0) {
		cmd_score(id, ntok >= 2 ? tok[1] : NULL);
	} else if (strcmp(tok[0], "fixed_handicap") == 0 ||
		   strcmp(tok[0], "place_free_handicap") == 0) {
		int n = (ntok >= 2) ? atoi(tok[1]) : 0;
		point_t pts[10];
		int i, got;
		if (strcmp(tok[0], "place_free_handicap") == 0)
			n = (ntok >= 2) ? atoi(tok[1]) : 2;
		got = handicap_points(n, pts);
		if (got <= 0) {
			fail(id, "unsupported handicap/size");
		} else {
			char out[128] = "";
			ghandicap = n;
			for (i = 0; i < got; ++i) {
				char vb[8];
				int atari = FALSE;
				makeamove(pts[i], BLACKCOLOR, grules, &atari, FALSE);
				format_vertex(pts[i], vb);
				if (i) strncat(out, " ", sizeof(out) - strlen(out) - 1);
				strncat(out, vb, sizeof(out) - strlen(out) - 1);
			}
			reply(id, out);
		}
	} else if (strcmp(tok[0], "time_settings") == 0) {
		double main_t = (ntok >= 2) ? atof(tok[1]) : 0;
		int stones = (ntok >= 4) ? atoi(tok[3]) : 0;
		if (main_t > 0) {
			double per = main_t * 1000.0 / (stones > 0 ? stones : 200);
			if (per < 500) per = 500;
			if (per > 30000) per = 30000;
			gtime_ms = (int)per;
		}
		gtime_left = 0;
		reply(id, "");
	} else if (strcmp(tok[0], "time_left") == 0) {
		if (ntok >= 3) {
			gtime_left = (unsigned int)atoi(tok[2]);
			gstones_left = (ntok >= 4) ? (unsigned int)atoi(tok[3]) : 0;
		}
		reply(id, "");
	} else if (strcmp(tok[0], "level") == 0) {
		if (ntok >= 2)
			glevel = atoi(tok[1]);
		reply(id, "");
	} else if (strcmp(tok[0], "quit") == 0) {
		reply(id, "");
		return 1;
	} else {
		fail(id, "unknown command");
	}
	return 0;
}

int main(int argc, char **argv)
{
	char line[4096];
	char *data_dir = ".";
	char dircwd[1024];
	int saved_stdout;
	int listen_port = -1;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
			listen_port = atoi(argv[++i]);
			continue;
		}
		if (argv[i][0] != '-')
			data_dir = argv[i];
	}

	{
		size_t n = strlen(data_dir);
		if (n > 0 && data_dir[n - 1] == '/')
			snprintf(dircwd, sizeof(dircwd), "%s", data_dir);
		else
			snprintf(dircwd, sizeof(dircwd), "%s/", data_dir);
	}

	saved_stdout = dup(1);
	if (saved_stdout < 0) {
		perror("dup");
		return 1;
	}
	dup2(2, 1);
	gtpout = fdopen(saved_stdout, "w");
	if (gtpout == NULL) {
		perror("fdopen");
		return 1;
	}
	setvbuf(gtpout, NULL, _IOLBF, 0);

	if (initmfgo(dircwd, 30) != G2OK) {
		fail("", "initmfgo failed (missing gamedb.dat/learn.dat/pattern.dat?)");
		return 1;
	}
	newgame();

	if (listen_port >= 1024 && listen_port <= 65535) {
		int srv = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in addr;
		int opt = 1;
		if (srv < 0) {
			perror("socket");
			return 1;
		}
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons((unsigned short)listen_port);
		if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			perror("bind");
			return 1;
		}
		if (listen(srv, 1) < 0) {
			perror("listen");
			return 1;
		}
		fprintf(stderr, "GTP listening on 127.0.0.1:%d\n", listen_port);
		for (;;) {
			struct sockaddr_in cli;
			socklen_t clilen = sizeof(cli);
			int fd = accept(srv, (struct sockaddr *)&cli, &clilen);
			if (fd < 0)
				break;
			fprintf(stderr, "GTP client connected\n");
			FILE *client_out = fdopen(dup(fd), "w");
			if (client_out == NULL) {
				close(fd);
				continue;
			}
			setvbuf(client_out, NULL, _IOLBF, 0);
			FILE *in = fdopen(fd, "r");
			if (in == NULL) {
				fclose(client_out);
				continue;
			}
			FILE *oldout = gtpout;
			gtpout = client_out;
			ghandicap = 0;
			newgame();
			while (fgets(line, sizeof(line), in) != NULL) {
				if (handle_line(line))
					break;
			}
			gtpout = oldout;
			fprintf(stderr, "GTP client disconnected\n");
			fclose(in);
			fclose(client_out);
		}
		return 0;
	}

	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (handle_line(line))
			break;
	}
	return 0;
}
