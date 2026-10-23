/* Test-only subset of InkView; never used in the device target. */
#ifndef EC_TEST_INKVIEW_H
#define EC_TEST_INKVIEW_H
typedef struct { int size; } ifont;
typedef int (*iv_handler)(int,int,int);
#define BLACK 0
#define DGRAY 0x555555
#define LGRAY 0xaaaaaa
#define WHITE 0xffffff
#define ALIGN_LEFT 1
#define DEFAULTFONT "test"
#define KBD_NUMERIC 4
#define KBD_PASSWORD 0x100
#define KEY_BACK 27
#define EVT_INIT 1
#define EVT_SHOW 2
#define EVT_POINTERUP 3
#define EVT_KEYPRESS 4
#define EVT_HIDE 5
#define EVT_BACKGROUND 6
#define EVT_EXIT 7
int ScreenWidth(void);
int ScreenHeight(void);
ifont *OpenFont(const char *,int,int);
void CloseFont(ifont *);
void SetFont(const ifont *,int);
void ClearScreen(void);
char *DrawTextRect(int,int,int,int,const char *,int);
void DrawRect(int,int,int,int,int);
void FillArea(int,int,int,int,int);
void FullUpdate(void);
void OpenKeyboard(const char *,char *,int,int,void (*)(char *));
void SetHardTimer(const char *,void (*)(void),int);
void ClearTimer(void (*)(void));
void CloseApp(void);
void InkViewMain(iv_handler);
#endif
