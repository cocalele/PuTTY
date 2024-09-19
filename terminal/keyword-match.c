#include "putty.h"
#include "terminal.h"

struct keyword_matcher{
	int begin;
	int current;
};

struct keyword {
	const char* word;
	optionalrgb fg_color;
	int word_len;
};

#define UNMATCHING 0
#define MATCHING 1
#define MATCHED 2

struct keyword_state {
	int state; //state machine:
	int next; //next char in keyword to compare
	int start_pos; //position in line matched with this word
};
static struct keyword_matcher m;
struct keyword keywords[] = {
	{"ERROR", { true, 255,0,0}},
	{"FAIL", { true, 255,0,0}},
	{"FATA", { true, 255,0,0}},
	{"EXCEPTION", { true, 227,227,0}},
	{"WARN", { true, 227,227,0}},
};
struct keyword_state keystates[sizeof(keywords)/sizeof(keywords[0])]={0};
static void reset_all_state()
{
	//printf("Reset\n");
	for (int i=0;i< sizeof(keystates) / sizeof(keystates[0]); i++){
		keystates[i].next = 0;
	}
}
static void hilight_word(struct termchar* linechars, int start, int length, optionalrgb fg){
	printf("hilight:%d, len:%d\n", start, length);
	for(int i=0;i<length;i++){
		linechars[start + i].truecolour.fg = fg;
	}
}
static void feed_char(struct termchar* linechars, int key_index, const char  letter, int position)
{
	struct keyword* k = &keywords[key_index];
	struct keyword_state* s = &keystates[key_index];
	//printf("%d [%c:%c]\n", s->next, k->word[s->next], letter);
	switch(s->state){
	case UNMATCHING:
		if(letter == k->word[s->next]){
			s->state = MATCHING;
			s->next++;
			s->start_pos = position;
		}
		break;
	case MATCHING:
		if (letter == k->word[s->next]) {
			s->state = MATCHING;
			s->next++;
			if(s->next == k->word_len){
				hilight_word(linechars, s->start_pos, position-s->start_pos+1, k->fg_color);
				s->state = UNMATCHING;
				s->next = 0;
				s->start_pos = 0;
			}
		} else {
			s->state = UNMATCHING;
			s->next = 0;
			s->start_pos = 0;
		}
	}

}
void hilhghtLine(struct termchar* linechars, int len)
{
	reset_all_state();
	for(int i=0;i<len;i++){
		termchar* lchar = linechars + i;
		unsigned long tchar;
		tchar = lchar->chr;
		//printf("%i:[0x%x]%lc\n", i, tchar, tchar);
		if ((tchar & CSET_MASK) != CSET_ACP || (tchar & 0x0080) != 0) {
			continue;
		}
		int upper = toupper(tchar & 0x00FF);
		//printf("%c", upper);
		for(int j=0;j< sizeof(keywords) / sizeof(keywords[0]);j++){
			feed_char(linechars, j, upper, i);
		}
	}
	//printf("\n");

}

#pragma section(".CRT$XCU",read)
#define INITIALIZER2_(f,p) \
        static void f(void); \
        __declspec(allocate(".CRT$XCU")) void (*f##_)(void) = f; \
        __pragma(comment(linker,"/include:" p #f "_")) \
        static void f(void)
#ifdef _WIN64
#define INITIALIZER(f) INITIALIZER2_(f,"")
#else
#define INITIALIZER(f) INITIALIZER2_(f,"_")
#endif
	
INITIALIZER(__init)
{
	for (int j = 0; j < sizeof(keywords) / sizeof(keywords[0]); j++) {
		keywords[j].word_len = strlen(keywords[j].word);
	}
	//return 1;
}

