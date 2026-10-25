

/* 
 * API and defines for Many Faces of Go Engine 
 */
#include "ui2g2.h"
#include "learn.h"

#include <memory.h>
#include <string.h>
#include <stdio.h>

/* prevent visual studio 2005 warnings */
#pragma warning(disable : 4996)

static int boardsize = 19;
static int handicap = 0;
static int rules = JAPANESE;
static int color_to_move = BLACKCOLOR;
static int computer_color = WHITECOLOR;
static int computer_both = FALSE;
static int playlevel = 6;
static int komi = 325;		/* 50 per point, so 325 is 6.5 points */

static unsigned char *fusekidirectory = ".";

static struct learn_game thegame;

/******************************************************************
 *
 *   Simple text interface to the go engine.  First is all of the
 *   user interface code.
 *
 ******************************************************************/

/* output the current position.  use getboard() to get the state of the board,
 * print an ugly ascii board, use getprisoners() to get the prisoner count.
 */

static void printboard(void)
{
	char pointcolors[NUMPOINTS];
	int i, j;
	int blackpris, whitepris;
	
	getboard(pointcolors);

	printf("\n");
	for( i = 0; i < boardsize; ++i) {
		printf("%2d",boardsize-i);
		for (j = 0; j < boardsize; ++j) {
			if (pointcolors[i*boardsize+j] == BLACKCOLOR)
				printf(" O");
			else if (pointcolors[i*boardsize+j] == WHITECOLOR)
				printf(" @");
			else
				printf(" +");
		}
		printf("\n");
	}
	printf("   a b c d e f g h j k l m n o p q r s t  ");
	getprisoners(&blackpris, &whitepris);
	printf("prisoners of black: %d, white: %d\n",blackpris, whitepris);
}

int translate_move(char *buf)
{
	int move;
	if (buf[0] == 'p' && buf[1] == 'a')
	   return PASS;
	if (buf[0] >= 'j')buf[0]--;
	sscanf(&(buf[1]), "%d", &move);
	move = boardsize-move;
	move *= boardsize;
	move += buf[0]-'a';
	return move;
}

int movenumber = 0;			/* number of current move */

main()
{
		point_t move;				/* move to make */
		char buf[100];
		int score;
		int color[NUMPOINTS+1];
		int dead[NUMPOINTS+1];
		int status;                 /* status returned from engine */
		int atari;
#ifdef G2DEBUGOUTPUT
		char debug_command;
#endif
		
		/* 
		 * initialize the engine 
		 * call once when the program starts
		 * specify the installation folder wher it can find the fuseki database
		 */
		if (initmfgo("./", 30) != G2OK) {
			printf("initmfgo failed\n");
			return 1;
		}

		/* 
		 * initialize all variables for a new game (6.5 pts komi) 
		 * call to start a new game
		 */
		initvars(boardsize, komi, 4, 10000000);

		/*
		 * initialiize the game structure
		 */
		memset(&thegame, 0, sizeof(struct learn_game));
		strcpy(thegame.event, "Test game");
		thegame.strength[0] = thegame.strength[1] = -5;	/* 5 kyu */
		
		/* get commands */
		for (;;) {
			/* Computer move? */
			if (computer_color == color_to_move || computer_both) {
				if ((status = compmove(FALSE, color_to_move, handicap, JAPANESE, &move, PASS, 3000, 20, UCTLEVEL, FALSE, 0, NULL, FALSE, 0, TRUE)) != G2OK) {
					printf("go engine internal error.\n");
					if(status == G2NOROOM)
						printf("Ran out of space for moves.\n");
					goto end;
				}
				if ((status = makeamove(move, color_to_move, rules, &atari, FALSE)) != G2OK) {
					if (atari)
						printf("Atari!\n");
					if(status == G2NOROOM)
						printf("Ran out of memory.\n");
					else
						printf("Computer made illegal move\n");
					goto end;
				}
				if (move == PASS) {
					printf("Computer passes\n");
					computer_both = FALSE;
				}

				/* record the move in the game */
				if (thegame.move_count < LEARN_MAX_MOVES) {
					thegame.move[thegame.move_count] = move;
					thegame.color[thegame.move_count] = color_to_move;
					thegame.move_count++;
				}

				color_to_move = 1 - color_to_move;
				movenumber++;
			}
			
			printboard();

			if (color_to_move == BLACKCOLOR)
				printf("%d: Black to move.\n", movenumber);
			else
				printf("%d: White to move.\n", movenumber);

			if (computer_both)	/* play both colors for computer */
				continue;

			printf("? (m-move, t-take back, s-score, n-new, l-level, b-both, d-debug p-pass q-quit) > ");
			gets(buf);
			switch (buf[0]) {
				case 'p':
					printf("game over\n");
					/* learn positions and results from this game */
					learn_add_game(&thegame, TRUE);
					learn_save();
				break;

				case 'd':
#ifdef G2DEBUGOUTPUT
					printf("command letter ( ^a to turn on debug, ? for help)? ");
					gets(buf);
					debug_command = buf[0];
					printf("location? ");
					gets(buf);
					move = translate_move(buf);
					debugcommand(debug_command, move, color_to_move, 0);
					printf("hit return to continue.");
					gets(buf);
#else
					printf("recompile with G2DEBUGOUTPUT defined.\n");
#endif
				break;

				case 'b':
					computer_both = TRUE;
				break;

				case 's':
					score = getthescore(rules == GOE || rules == CHINESE, handicap, 
										-1, color, dead);
					/* remember to add back 1/2 point for white */
					if(score > 0)
						printf("White is ahead by %d 1/2", score);
					else if(score == 0)
						printf("White is ahead by 1/2 point");
					else
						printf("Black is ahead by %d 1/2", -score-1);
				break;

				case 't':
					takebackamove(rules, &move);  /* computer's move */
					takebackamove(rules, &move);  /* my last move */
				break;

				case 'l':
					printf("New level? ");
					gets(buf);
					sscanf(buf, "%d", &playlevel);
				break;

				case 'm':
					printf("Enter move: ");
					gets(buf);
					move = translate_move(buf);
					if ((status = makeamove(move, color_to_move, rules, &atari, FALSE)) == G2OK) {
						/* record the move in the game */
						if (thegame.move_count < LEARN_MAX_MOVES) {
							thegame.move[thegame.move_count] = move;
							thegame.color[thegame.move_count] = color_to_move;
							thegame.move_count++;
						}
						color_to_move = 1-color_to_move;
						movenumber++;
					}
					else if (status == G2NOROOM) {
						printf("Out of memory, can't make this move.\n");
					}
					else if(status == G2SUICIDE) {
						printf("Illegal suicide move\n");
					}
					else if(status == G2KO) {
						printf("Illegal Ko capture\n");
					}
					else {
						printf("Illegal move\n");
					}
				break;

				case 'h':	/* hint */
				{
					point_t points[3];
					int values[3];
					int i;
					char pbuf[10];
					int num_hints = 3;
					if (compmove_multi(FALSE, color_to_move, handicap, JAPANESE, &num_hints, points, values, 20, MAXLEVEL) != G2OK) {
						printf("\nHint error\n");
						break;
					}
					printf("\nGood moves are:");
					for (i = 0; i < num_hints; ++i) {
						printf(" %s", ssqr(points[i], pbuf));
					}
					printf("\n");
					break;
				}

				case 'q':
					goto end;
				break;

				case 'n':	/* new game */
					do {
						printf("boardsize? ");
						gets(buf);
						sscanf(buf,"%d",&boardsize);
					} while(boardsize < 9 || boardsize > 19);
					initvars(boardsize, komi, 4, 10000000);

					/* reset the game structure */
					thegame.boardsize = boardsize;
					thegame.move_count = 0;
				break;

			}
		}
	end:
	return 0;
}



/***************************************************************************
 *
 *  Support code that must be supplied - called from the go engine
 *
 ***************************************************************************/


/* show dots to indicate thinking.  return FALSE so computer will continue to think */

int thinking(void)
{
	printf(".");
	return FALSE;
} 

static char str[200];

/* go engine asks for some text (see file wingo.txt for text strings
 * lines starting with '.' have the page numbers, line is the line within the page
 */

char *getstring(int page, int line)
{
	sprintf(str,"Go engine asked for page %d, string %d\n",page,line);
	return str;
}

/* show progress toward solving the problem */

void problemprogress(int size, int result, int confidence){
	printf("%d positions\n%s\n%d%% confidence of success\n",
		size, result, confidence);
	}


/* output a string */

void outerr(const char *s)
{
	puts(s);
}


void outstone(point_t s, char *c)
{
}

void histone(point_t s)
{
}

void lookmove(point_t s, int c)
{
}

void looktakeback(point_t s)
{
}

void lookbackup(point_t s)
{
}

#ifndef SMALLCODE

void addcomment(char *s)
{
	printf("comment: %s\n", s);
}

#endif


/* calls from the go engine for debug commands */

#ifdef G2DEBUGOUTPUT

void turnoffcplay(void)
{
	computer_color = NOCOLOR;
}

void outerror(char *s)
{
	puts("error: ");
	puts(s);
}

void clearerror(void)
{
}

void waitaction(void)
{
	getc(stdin);
}

point_t waitclick(void)
{
	int move;
	char buf[100];
	printf("Point? ");
	gets(buf);
	move = translate_move(buf);
	return move;
}

void hispecstone(point_t stone)
{
}

void fixsd(int mvnumber, int color)  /* called after a move is taken back the board */
{
}

void fixlm(int mvnumber, sqr_t s, int color)      /* called after a move is made on the board */
{
}

void unhighlight(void)
{
}

void _Assert(char *file, int line)
{
	printf("Assertion failure in file %s at line %d\n", file, line);
}

#endif
