// Unofficial, personal-use Old 3DS homebrew recreation of The Impossible Game.
// Visual/audio assets and the 501-object level come from the user's own game files.
// No original PC executable is used. Engine written for libctru / devkitARM.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>

#define TOP_W 400
#define BOT_W 320
#define LCD_H 240
#define MAX_OBJECTS 5000
#define MAX_COLORS 160
#define MAX_GRAVITY_EVENTS 128
#define MAX_EFFECT_SECTIONS 128
#define TILE 40.0f
#define UNIT_TO_PIX 0.40f
#define RUN_SPEED 450.0f
// Scale jump physics to the authored four-tile jump span, not song length.
// The former fixed 680/2600 arc travelled ~6 tiles at 450 units/s and
// made chains of short blocks (e.g., the Classic second/third blocks) fail.
#define JUMP_SPAN_TILES 4.0f
// Original-game reference video: standing cube ~50 px across and ~84 px
// from standing center to apex. That is ~1.68 cube heights, rounded 1.70.
// Never increase jump height for a particular stage: the original has one
// shared fixed arc across all levels. Verify on real hardware for precision.
#define JUMP_PEAK_TILES 1.70f
#define JUMP_BUFFER_TIME 0.10f
#define AUDIO_SAMPLES 2048
#define AUDIO_BUFS 4
#define MAX_FLAGS 10
#define AUDIO_RATE 22050

#define RGB(r,g,b) ((uint32_t)(((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b)))
#define C_BLACK RGB(0,0,0)
#define C_WHITE RGB(255,255,255)
#define C_CYAN RGB(40,192,255)
#define C_YELLOW RGB(255,209,20)
#define C_DARK RGB(7,17,28)
#define C_MUTED RGB(164,184,204)

// Input/image/audio all remain software-side so this runs on an original Old 3DS.
typedef struct { u8 *p; int w, h; } Canvas;
static void rectangle(Canvas *c,int x,int y,int w,int h,u32 color);
typedef struct { u16 w,h; u8 *pixels; } Image;
typedef struct { u8 kind; s32 x,y; } LevelObject;
typedef struct { s32 x; u8 color; } BackgroundEvent;
typedef struct { s32 a,b; } LevelSection;

typedef struct { float x,y,vy,angle; bool ground; float jumpBuffer, coyote; } Player;
typedef struct { Player saved; } Checkpoint;
typedef struct { char name; u8 rows[7]; } Letter;

enum { IM_TITLE, IM_MENU, IM_FOOTER, IM_LEFTBAR, IM_RIGHTBAR, IM_BG,
       IM_PLAYER, IM_SPIKE, IM_BLOCK, IM_ATTEMPT, IM_PRESSSTART,
       IM_LEFTARROW, IM_RIGHTARROW, IM_BG2, IM_BG3, IM_BG4, IM_BG5,
       IM_LEVELS, IM_PRACTICE_HEADER, IM_FLAG, IM_RESUMEMENU, IM_END_NORMAL, IM_END_PRACTICE, NUM_IMAGES };
static const char *imageNames[NUM_IMAGES] = {
    "title", "menu", "footer", "leftbar", "rightbar", "background", "player",
    "spike", "block", "attempt", "pressstart", "leftarrow", "rightarrow",
    "background2", "background3", "background4", "background5", "levels",
    "practiceheader", "flag", "resumemenu", "endnormal", "endpractice"
};
static Image images[NUM_IMAGES];
static u8 *cachedGameBackground[5]={0};
static u8 *cachedBottom = NULL;
static int cachedBottomScene=-1;
static bool cachedBottomPressed=false;

static LevelObject objects[MAX_OBJECTS];
static BackgroundEvent backgroundEvents[MAX_COLORS];
static s32 gravityEvents[MAX_GRAVITY_EVENTS];
static LevelSection risingSections[MAX_EFFECT_SECTIONS], fallingSections[MAX_EFFECT_SECTIONS];
static int numColors=0, numGravity=0, numRising=0, numFalling=0;
static int colorCursor=0, gravityCursor=0;
static int currentPalette=0, previousPalette=0;
static float paletteTransition=1.0f;  // 0=previous, 1=target
static bool gravityFlipped=false;
static u8 *knownBottomBuffers[2]={NULL,NULL};
static int bottomBufferRevision[2]={-1,-1};
static int bottomCacheRevision=0;

static bool practiceMode = false;
static int flagCount = 0;
static Checkpoint flags[MAX_FLAGS];
static int numObjects = 0;
static Player player;
static float cameraGround = 172.0f;
// Attempt count is session-local: it increments on every death/restart,
// and is cleared when leaving a stage. The game writes NO log/save files.
static int attempts = 0, bestPercent = 0, menuItem = 0, infoPage = 0;
static int stageChoice = 0, activeStage = 4;
static bool stagesAvailable[5] = {false};

static const char *practiceAudio = "romfs:/audio/practice.pcm";
static const char* stageAudio[5] = {
    "romfs:/audio/level.pcm", "romfs:/audio/level.pcm", "romfs:/audio/level2.pcm",
    "romfs:/audio/level3.pcm", "romfs:/audio/level4.pcm"
};
static float splashTime = 0.0f;
static float animationTime = 0.0f;
static float winAge = 0.0f;
static float flagAnimation = 0.0f;
static float fadeRemaining=0.0f;
static bool launchingStage=false;
static float fadeOutRemaining=0.0f;
static const float fadeSeconds=0.23f;
static void beginFadeIn(void){fadeRemaining=fadeSeconds;}
static void drawFade(Canvas *c){
    if(fadeRemaining<=0 && !launchingStage)return;
    float f=launchingStage?(1.0f-fmaxf(0.0f,fadeOutRemaining)/fadeSeconds):
        fminf(1.0f,fadeRemaining/fadeSeconds);
    int dark=(int)(255.0f * fminf(1.0f,f));
    if(dark>=255){rectangle(c,0,0,c->w,c->h,C_BLACK);return;}
    int factor=255-dark;
    for(int x=0;x<c->w;++x){
        u8 *col=c->p+(size_t)x*c->h*3;
        for(int y=0;y<c->h*3;++y)col[y]=(u8)((col[y]*factor)>>8);
    }
}
static bool soundEnabled = true, isQuit = false;
static float deathTimer = 0;
static float levelEnd = 39400.0f;
static float runSpeed = RUN_SPEED;
static float jumpVelocity = 900.0f, fallGravity = 5062.5f;
static void refreshJumpPhysics(void) {
    // Same distance/height on every stage despite soundtrack-duration speed
    // corrections. 2D projectile equations: range=vx*2*vy/g, peak=vy^2/(2g).
    float flightSeconds = (JUMP_SPAN_TILES*TILE) / runSpeed;
    float peak = JUMP_PEAK_TILES*TILE;
    jumpVelocity = 4.0f * peak / flightSeconds;
    fallGravity = 8.0f * peak / (flightSeconds*flightSeconds);
}

typedef enum { S_SPLASH, S_MENU, S_LEVELS, S_PLAY, S_DEAD, S_PAUSED, S_INFO, S_STATS, S_OPTIONS, S_WIN } Scene;
static Scene scene = S_SPLASH;

// Mono 16-bit PCM streamed from romfs. Audio remains optional if DSP init fails.
static bool dspReady = false;
static FILE *musicFile = NULL;
static char musicPath[80] = "";
static s16 *musicSamples[AUDIO_BUFS] = {0};
static ndspWaveBuf musicWave[AUDIO_BUFS];
typedef struct { s16 *data; size_t count; ndspWaveBuf wave; } Effect;
static Effect effects[3] = {0}; // 0=death, 1=menu select, 2=original fireworks

static const Letter font[] = {
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{31,4,4,4,4,4,31}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {'%',{25,25,2,4,8,19,19}}, {'?',{14,17,1,2,4,0,4}},
    {'!',{4,4,4,4,4,0,4}}, {'-',{0,0,0,31,0,0,0}},
    {':',{0,4,0,0,4,0,0}}, {'.',{0,0,0,0,0,0,4}},
    {'/',{1,1,2,4,8,16,16}}, {'>',{16,8,4,2,4,8,16}},
    {'<',{1,2,4,8,4,2,1}}, {'+',{0,4,4,31,4,4,0}},
    {'(',{2,4,8,8,8,4,2}}, {')',{8,4,2,2,2,4,8}},
    {'=',{0,31,0,31,0,0,0}},
};

static void __attribute__((unused)) dot(Canvas *c, int x, int y, u32 rgb) {
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h) return;
    // The 3DS framebuffer is rotated: columns in memory, rows bottom-to-top.
    size_t ofs = ((size_t)x * (size_t)c->h + (size_t)(c->h-1-y))*3;
    c->p[ofs + 0] = (u8)(rgb & 255);
    c->p[ofs + 1] = (u8)((rgb >> 8) & 255);
    c->p[ofs + 2] = (u8)((rgb >> 16) & 255);
}
static void rectangle(Canvas *c, int x, int y, int w, int h, u32 color) {
    int l = x > 0 ? x : 0, r = x+w < c->w ? x+w : c->w;
    int t = y > 0 ? y : 0, b = y+h < c->h ? y+h : c->h;
    if (r <= l || b <= t) return;
    u8 bl = (u8)color, gr=(u8)(color>>8), rd=(u8)(color>>16);
    for (int ix=l; ix<r; ++ix) {
        u8 *p = c->p + ((size_t)ix*c->h+(c->h-b))*3;
        for (int iy=b-1; iy>=t; --iy, p+=3) {
            p[0]=bl; p[1]=gr; p[2]=rd;
        }
    }
}
static void frame(Canvas *c, int x, int y, int w, int h, u32 color) {
    rectangle(c,x,y,w,2,color); rectangle(c,x,y+h-2,w,2,color);
    rectangle(c,x,y,2,h,color); rectangle(c,x+w-2,y,2,h,color);
}
static void alphaPixel(Canvas *c,int x,int y,const u8 *px) {
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h) return;
    int a=px[3]; if (!a) return;
    u8 *p=c->p+((size_t)x*c->h+(c->h-1-y))*3;
    if (a==255) {p[0]=px[2];p[1]=px[1];p[2]=px[0];}
    else {
        p[0]=(u8)((px[2]*a+p[0]*(255-a))/255);
        p[1]=(u8)((px[1]*a+p[1]*(255-a))/255);
        p[2]=(u8)((px[0]*a+p[2]*(255-a))/255);
    }
}
static void sprite(Canvas *c, int idx, int x, int y, int w, int h) {
    Image *im=&images[idx];
    if (!im->pixels || w<1 || h<1) return;
    int left=x<0?0:x, right=x+w>c->w?c->w:x+w;
    int top=y<0?0:y, bot=y+h>c->h?c->h:y+h;
    if (right<=left || bot<=top) return;
    for(int dx=left;dx<right;++dx) {
        int sx=((dx-x)*im->w)/w;
        for(int dy=top;dy<bot;++dy) {
            int sy=((dy-y)*im->h)/h;
            alphaPixel(c,dx,dy,im->pixels+((size_t)sy*im->w+sx)*4);
        }
    }
}
// Rotate around the cube's center using a diagonal-sized destination box.
// The old 24x24 destination clipped the cube corners at 45 degrees.
static void spriteRotated(Canvas *c,int idx,int x,int y,int size,float angle) {
    Image *im=&images[idx]; if(!im->pixels)return;
    float cs=cosf(angle), sn=sinf(angle), half=size*0.5f;
    int margin=(int)ceilf((1.414214f*size-size)*0.5f)+1;
    float cx=x+half, cy=y+half;
    for(int dx=x-margin;dx<x+size+margin;++dx) {
        if((unsigned)dx >= (unsigned)c->w)continue;
        for(int dy=y-margin;dy<y+size+margin;++dy) {
            if((unsigned)dy >= (unsigned)c->h)continue;
            float u=(dx+0.5f-cx), v=(dy+0.5f-cy);
            float sx=cs*u+sn*v+half, sy=-sn*u+cs*v+half;
            if(sx<0 || sy<0 || sx>=size || sy>=size)continue;
            int ix=(int)(sx*im->w/size), iy=(int)(sy*im->h/size);
            alphaPixel(c,dx,dy,im->pixels+((size_t)iy*im->w+ix)*4);
        }
    }
}
static void drawText(Canvas *c,int x,int y,const char *s,int scale,u32 color) {
    for (;*s;s++,x+=scale*6) {
        char ch=(char)toupper((unsigned char)*s);
        const Letter *glyph=NULL;
        for(size_t k=0;k<sizeof(font)/sizeof(font[0]);++k) {
            if(font[k].name==ch) {glyph=&font[k];break;}
        }
        if(!glyph) continue;
        for(int row=0;row<7;++row)for(int col=0;col<5;++col){
            if(glyph->rows[row] & (1u << (4-col)))
                rectangle(c,x+col*scale,y+row*scale,scale,scale,color);
        }
    }
}
static int textWidth(const char *txt,int scale) {return (int)strlen(txt)*6*scale;}
static void centered(Canvas *c,int cy,const char *txt,int scale,u32 color) {
    drawText(c,(c->w-textWidth(txt,scale))/2,cy,txt,scale,color);
}
static bool loadImage(int which) {
    char path[128];snprintf(path,sizeof(path),"romfs:/sprites/%s.rgba",imageNames[which]);
    FILE *f=fopen(path,"rb");if (!f)return false;
    u8 header[4];if(fread(header,1,4,f)!=4){fclose(f);return false;}
    Image *im=&images[which];im->w=header[0]|(header[1]<<8);im->h=header[2]|(header[3]<<8);
    size_t bytes=(size_t)im->w*im->h*4;
    if (im->w<1 || im->w>1500 || im->h<1 || im->h>1200 || bytes>2*1024*1024) {fclose(f);return false;}
    im->pixels=(u8*)malloc(bytes);
    if(!im->pixels){fclose(f);return false;}
    bool result=fread(im->pixels,1,bytes,f)==bytes;
    fclose(f);if(!result){free(im->pixels);im->pixels=NULL;}return result;
}
static u32 getBE32(const u8 *p){return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|(u32)p[3];}
static bool readBE16(FILE *f, int *out){
    u8 v[2];if(fread(v,1,2,f)!=2)return false;
    *out=((int)v[0]<<8)|v[1];return true;
}
static bool readBE32(FILE *f, s32 *out){
    u8 v[4];if(fread(v,1,4,f)!=4)return false;
    *out=(s32)getBE32(v);return true;
}
static int sortObjects(const void *a,const void *b){
    const LevelObject *x=a,*y=b;
    return x->x<y->x?-1:x->x>y->x?1:0;
}
static int firstObjectAt(float x){
    int lo=0,hi=numObjects;
    while(lo<hi){int mid=(lo+hi)/2;if(objects[mid].x<x)lo=mid+1;else hi=mid;}
    return lo;
}
static bool loadLevel(int stage) {
    if(stage<0 || stage>=5)return false;
    char path[64];snprintf(path,sizeof(path),"romfs:/levels/stage%d.bin",stage);
    FILE *f=fopen(path,"rb");if(!f)return false;
    int total=0;
    if(!readBE16(f,&total) || total<1 || total>MAX_OBJECTS){fclose(f);return false;}
    float maxX=0;
    for(int i=0;i<total;++i){
        u8 entry[9];if(fread(entry,1,9,f)!=9){fclose(f);return false;}
        objects[i]=(LevelObject){entry[0],(s32)getBE32(entry+1),(s32)getBE32(entry+5)};
        if(entry[0]>2){fclose(f);return false;}
        float right=(float)(entry[0]==2?objects[i].y:objects[i].x+40);
        if(right>maxX)maxX=right;
    }
    s32 endX=0;
    if(!readBE32(f,&endX)){fclose(f);return false;}
    levelEnd=(float)endX;
    if(levelEnd<1000)levelEnd=maxX+600;
    numColors=numGravity=numRising=numFalling=0;
    u8 mark[4];
    if(fread(mark,1,4,f)==4 && memcmp(mark,"EVT1",4)==0){
        int n=0;
        if(!readBE16(f,&n)||n>MAX_COLORS){fclose(f);return false;}
        numColors=n;
        for(int i=0;i<n;++i){
            s32 x=0;u8 color=0;
            if(!readBE32(f,&x)||fread(&color,1,1,f)!=1||color>5){fclose(f);return false;}
            backgroundEvents[i]=(BackgroundEvent){x,color};
        }
        if(!readBE16(f,&n)||n>MAX_GRAVITY_EVENTS){fclose(f);return false;}
        numGravity=n;
        for(int i=0;i<n;++i)if(!readBE32(f,&gravityEvents[i])){fclose(f);return false;}
        if(!readBE16(f,&n)||n>MAX_EFFECT_SECTIONS){fclose(f);return false;}
        numRising=n;
        for(int i=0;i<n;++i)if(!readBE32(f,&risingSections[i].a)||!readBE32(f,&risingSections[i].b)){fclose(f);return false;}
        if(!readBE16(f,&n)||n>MAX_EFFECT_SECTIONS){fclose(f);return false;}
        numFalling=n;
        for(int i=0;i<n;++i)if(!readBE32(f,&fallingSections[i].a)||!readBE32(f,&fallingSections[i].b)){fclose(f);return false;}
    }
    fclose(f);
    numObjects=total;
    qsort(objects,(size_t)numObjects,sizeof(objects[0]),sortObjects);
    activeStage=stage;
    // Duration-based speed is an initial approximation: stage layouts and
    // soundtrack timing are original, precise physics must be calibrated.
    runSpeed=RUN_SPEED;
    FILE *audio=fopen(stageAudio[stage],"rb");
    if(audio){
        if(fseek(audio,0,SEEK_END)==0){
            long bytes=ftell(audio);
            float seconds=(float)bytes/(2.0f*(float)AUDIO_RATE);
            if(seconds>30 && seconds<200){
                float speed=levelEnd/seconds;
                if(speed>=370 && speed<=620)runSpeed=speed;
            }
        }
        fclose(audio);
    }
    refreshJumpPhysics();
    return true;
}
static bool hasStage(int stage) {
    char path[64];snprintf(path,sizeof(path),"romfs:/levels/stage%d.bin",stage);
    FILE *f=fopen(path,"rb");if(!f)return false;
    fclose(f);return true;
}
static Effect loadEffect(const char *path) {
    Effect fx={0};FILE *f=fopen(path,"rb");if(!f)return fx;
    fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);
    if(len<=0 || len>400000){fclose(f);return fx;}
    fx.data=(s16*)linearAlloc((size_t)len);if(!fx.data){fclose(f);return fx;}
    fx.count=fread(fx.data,1,(size_t)len,f)/2;
    fclose(f);DSP_FlushDataCache(fx.data,fx.count*2);return fx;
}
static void playEffect(int fx) {
    if(!dspReady || !soundEnabled || fx<0 || fx>2 || !effects[fx].data)return;
    ndspChnWaveBufClear(1);
    ndspChnReset(1);
    ndspChnSetRate(1,AUDIO_RATE);
    ndspChnSetInterp(1,NDSP_INTERP_LINEAR);
    ndspChnSetFormat(1,NDSP_FORMAT_MONO_PCM16);
    memset(&effects[fx].wave,0,sizeof(ndspWaveBuf));
    effects[fx].wave.data_pcm16=effects[fx].data;
    effects[fx].wave.nsamples=(u32)effects[fx].count;
    ndspChnWaveBufAdd(1,&effects[fx].wave);
}
static void stopMusic(void) {
    if(!dspReady)return;
    ndspChnWaveBufClear(0);
    if(musicFile){fclose(musicFile);musicFile=NULL;}
    musicPath[0]='\0';
}
static void updateMusic(void) {
    if(!dspReady || !musicFile || !soundEnabled)return;
    for(int i=0;i<AUDIO_BUFS;++i){
        ndspWaveBuf *w=&musicWave[i];
        if (w->status!=NDSP_WBUF_FREE && w->status!=NDSP_WBUF_DONE)continue;
        size_t count=fread(musicSamples[i],sizeof(s16),AUDIO_SAMPLES,musicFile);
        if(count<AUDIO_SAMPLES){
            rewind(musicFile);
            count+=fread(musicSamples[i]+count,sizeof(s16),AUDIO_SAMPLES-count,musicFile);
        }
        if(count==0)break;
        DSP_FlushDataCache(musicSamples[i],count*sizeof(s16));
        memset(w,0,sizeof(*w));w->data_pcm16=musicSamples[i];w->nsamples=(u32)count;
        ndspChnWaveBufAdd(0,w);
    }
}
static void startMusic(const char *path) {
    if(!dspReady || !soundEnabled)return;
    if(musicFile && strcmp(path,musicPath)==0)return;
    stopMusic();musicFile=fopen(path,"rb");if(!musicFile)return;
    snprintf(musicPath,sizeof(musicPath),"%s",path);
    ndspChnReset(0);
    ndspChnSetInterp(0,NDSP_INTERP_LINEAR);
    ndspChnSetRate(0,AUDIO_RATE);
    ndspChnSetFormat(0,NDSP_FORMAT_MONO_PCM16);
    for(int i=0;i<AUDIO_BUFS;++i){memset(&musicWave[i],0,sizeof(ndspWaveBuf));}
    updateMusic();
}
// Seek music on checkpoint respawn to the same stage timeline as the cube.
// PCM is mono 16-bit at AUDIO_RATE; align to complete samples.
static void startStageMusicAt(float stageSeconds) {
    const char *path=stageAudio[activeStage];
    stopMusic();
    if (!dspReady || !soundEnabled) return;
    musicFile=fopen(path,"rb");
    if(!musicFile)return;
    snprintf(musicPath,sizeof(musicPath),"%s",path);
    ndspChnReset(0);
    ndspChnSetInterp(0,NDSP_INTERP_LINEAR);
    ndspChnSetRate(0,AUDIO_RATE);
    ndspChnSetFormat(0,NDSP_FORMAT_MONO_PCM16);
    if(stageSeconds<0)stageSeconds=0;
    if(fseek(musicFile,0,SEEK_END)==0){
        long n=ftell(musicFile);
        long ofs=(long)(stageSeconds*AUDIO_RATE)*2L;
        if(ofs>=n)ofs=0;
        if(ofs<0)ofs=0;
        fseek(musicFile,ofs,SEEK_SET);
    }else rewind(musicFile);
    for(int i=0;i<AUDIO_BUFS;++i)memset(&musicWave[i],0,sizeof(ndspWaveBuf));
    updateMusic();
}
static void initAudio(void) {
    if(R_FAILED(ndspInit()))return;
    dspReady=true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    float mix[12]={0};mix[0]=0.8f;mix[1]=0.8f;
    ndspChnSetMix(0,mix);ndspChnSetMix(1,mix);
    for(int i=0;i<AUDIO_BUFS;++i){
        musicSamples[i]=(s16*)linearAlloc(AUDIO_SAMPLES*sizeof(s16));
        if(!musicSamples[i]){dspReady=false;break;}
    }
    if(!dspReady){for(int i=0;i<AUDIO_BUFS;++i)if(musicSamples[i])linearFree(musicSamples[i]);ndspExit();return;}
    effects[0]=loadEffect("romfs:/audio/explosion.pcm");
    effects[1]=loadEffect("romfs:/audio/select.pcm");
    effects[2]=loadEffect("romfs:/audio/fireworks.pcm");
    startMusic("romfs:/audio/menu.pcm");
}
static void endAudio(void) {
    if(!dspReady)return;
    stopMusic();ndspChnWaveBufClear(1);
    for(int i=0;i<AUDIO_BUFS;++i)linearFree(musicSamples[i]);
    for(int i=0;i<3;++i)if(effects[i].data)linearFree(effects[i].data);
    ndspExit();
}
static float progress(void){float p=player.x/levelEnd*100.0f;return fminf(100.0f,fmaxf(0,p));}
static void recordBest(void) {
    int score=(int)progress();if (score>bestPercent) bestPercent=score;
}
static void resetPalette(void){
    currentPalette=previousPalette=0;
    paletteTransition=1.0f;
    colorCursor=0;gravityCursor=0;gravityFlipped=false;
    for(;colorCursor<numColors&&backgroundEvents[colorCursor].x<=0;++colorCursor)
        currentPalette=previousPalette=backgroundEvents[colorCursor].color;
}
static void advanceLevelEffects(float dt){
    while(colorCursor<numColors && player.x>=backgroundEvents[colorCursor].x){
        previousPalette=currentPalette;
        currentPalette=backgroundEvents[colorCursor].color;
        paletteTransition=(previousPalette==currentPalette)?1.0f:0.0f;
        ++colorCursor;
    }
    // The event is only fired at source-game authored x positions.
    // Inversion mirrors the level viewport while local jump geometry stays
    // unchanged. The original effect timing still needs PC comparison.
    while(gravityCursor<numGravity && player.x>=gravityEvents[gravityCursor]){
        gravityFlipped=!gravityFlipped;
        ++gravityCursor;
    }
    if(paletteTransition<1.0f){
        paletteTransition+=dt/0.50f;
        if(paletteTransition>1.0f)paletteTransition=1.0f;
    }
}
static void replayLevelEventsAt(float x) {
    // Set the original authored palette and gravity state at this checkpoint.
    resetPalette();
    while(colorCursor<numColors && backgroundEvents[colorCursor].x<=x){
        currentPalette=previousPalette=backgroundEvents[colorCursor].color;
        ++colorCursor;
    }
    while(gravityCursor<numGravity && gravityEvents[gravityCursor]<=x){
        gravityFlipped=!gravityFlipped;
        ++gravityCursor;
    }
    paletteTransition=1.0f;
}
static void resetPlayer(void) {
    player=(Player){0};
    player.x=0;player.y=0;player.ground=true;
    cameraGround=172.0f;
    deathTimer=0;
    replayLevelEventsAt(0.0f);
    ++attempts;
    if(practiceMode){
        if(!musicFile || strcmp(musicPath,practiceAudio)!=0)startMusic(practiceAudio);
    }else startStageMusicAt(0.0f);
    if(dspReady)ndspChnSetPaused(0,false);
    scene=S_PLAY;
    beginFadeIn();
}
static void respawn(void) {
    if(practiceMode && flagCount>0){
        player=flags[flagCount-1].saved;
        player.jumpBuffer=0.0f;
        deathTimer=0.0f;
        replayLevelEventsAt(player.x);
        cameraGround=172.0f;
        ++attempts;
        // Practice Mode uses its own original music and does not seek the
        // normal stage soundtrack to the checkpoint position.
        if(!musicFile || strcmp(musicPath,practiceAudio)!=0)startMusic(practiceAudio);
        if(dspReady)ndspChnSetPaused(0,false);
        scene=S_PLAY;
        beginFadeIn();
    }else resetPlayer();
}
static void placeFlag(void) {
    if(scene!=S_PLAY || flagCount>=MAX_FLAGS)return;
    if(!practiceMode){
        practiceMode=true;
        startMusic(practiceAudio);
    }
    flags[flagCount++].saved=player;
    flagAnimation=0.25f;
    ++bottomCacheRevision;
    playEffect(1);
}
static void removeFlag(void) {
    if(!practiceMode || flagCount<1)return;
    --flagCount;
    flagAnimation=0.0f;
    ++bottomCacheRevision;
    playEffect(1);
}
static void exitPracticeMode(void){
    if(!practiceMode)return;
    practiceMode=false;
    flagCount=0;
    ++bottomCacheRevision;
    // As on the original, exiting Practice restarts from the beginning.
    resetPlayer();
}
static void goMenu(void) {
    recordBest();
    attempts=0;  // Leaving ANY level ends that level's attempt session.
    practiceMode=false;
    flagCount=0;
    scene=S_MENU;menuItem=0;beginFadeIn();
    stopMusic();startMusic("romfs:/audio/menu.pcm"); if(dspReady)ndspChnSetPaused(0,false);
}
static void die(void) {
    if(scene!=S_PLAY)return;
    recordBest();scene=S_DEAD;deathTimer=0;
    stopMusic();playEffect(0);
}
static bool playerOverPit(float x) {
    for(int i=0;i<numObjects;++i){
        if(objects[i].kind != 2)continue;
        float start=(float)objects[i].x, end=(float)objects[i].y;
        // Ground support uses the cube's central third to avoid accidental edge falls.
        if(x+TILE*0.68f>start && x+TILE*0.32f<end)return true;
    }
    return false;
}
static void launchJump(void) {
    player.vy=jumpVelocity;
    player.ground=false;
    player.jumpBuffer=0;
}
static void updateGame(float dt, bool jumpHeld, bool jumpPressed) {
    // Keep taps pressed just before landing for 0.10 s, including short touch
    // taps at low display FPS. Holding the button continues to chain jumps
    // when landing, as in the one-button original game.
    if(jumpPressed)player.jumpBuffer=JUMP_BUFFER_TIME;
    else if(player.jumpBuffer>0.0f)
        player.jumpBuffer=fmaxf(0.0f,player.jumpBuffer-dt);

    if((jumpHeld || player.jumpBuffer>0) && player.ground)launchJump();

    float oldY=player.y;
    player.x+=runSpeed*dt;
    advanceLevelEffects(dt);
    player.vy-=fallGravity*dt;    player.y+=player.vy*dt;

    // Find surface crossings BEFORE side-collision checks. Adjacent raised
    // blocks otherwise occasionally killed the cube on its landing frame.
    bool landed=false;
    float blockTop=-100000.0f;
    if(!playerOverPit(player.x) && oldY>=-0.01f && player.y<=0 && player.vy<=0){
        landed=true;
        blockTop=0;
    }
    const int left=firstObjectAt(player.x-TILE*2.0f);
    for(int i=left;i<numObjects;++i) {
        const LevelObject *obj=&objects[i];
        if(obj->x>player.x+TILE*2.0f)break;
        if(obj->kind!=0)continue;
        const float bx=(float)obj->x, by=(float)obj->y;
        if(player.x+TILE-5<=bx || player.x+5>=bx+TILE)continue;
        const float top=by+TILE;
        if(player.vy<=0 && oldY>=top-0.05f && player.y<=top+0.05f){
            if(top>blockTop){landed=true;blockTop=top;}
        }
    }
    if(landed){
        bool firstLanding=!player.ground;
        player.y=blockTop;
        player.vy=0;
        player.ground=true;
        if(firstLanding)player.angle=roundf(player.angle/(3.14159265f/2))*(3.14159265f/2);
    }else{
        player.ground=false;
        player.angle+=dt*7.8f;
    }

    // Side/underside collisions are tested against the resolved landing y.
    for(int i=left;i<numObjects;++i){
        const LevelObject *obj=&objects[i];
        if(obj->x>player.x+TILE*2.0f)break;
        if(obj->kind>1)continue;
        const float bx=(float)obj->x, by=(float)obj->y;
        if(player.x+TILE-5<=bx || player.x+5>=bx+TILE)continue;
        if(obj->kind==0){
            const float top=by+TILE;
            if(player.y<top-0.5f && player.y+TILE>by+0.5f){die();return;}
        }else{
            float cx=player.x+TILE*0.5f, cy=player.y+TILE*0.5f;
            float ox=fabsf(cx-(bx+TILE*0.5f));
            if(ox<TILE*0.68f && cy>by+3 && cy<by+TILE-1){
                float safeBase=by+TILE*(ox/(TILE*0.68f));
                if(player.y<by+TILE-3 && player.y+TILE>safeBase+4){die();return;}
            }
        }
    }

    // A tap made immediately before landing triggers as soon as the feet
    // reach the black block instead of being lost before the next frame.
    if(player.ground && (jumpHeld || player.jumpBuffer>0))launchJump();
    if(player.y<-TILE*2.5f){die();return;}
    if(player.x>=levelEnd){
        scene=S_WIN;bestPercent=100;stopMusic();
        winAge=0.0f;
        playEffect(2); // Original 'fireworks.wav' completion sound.
    }
    float target=gravityFlipped?(68.0f-player.y*UNIT_TO_PIX+30.0f):
        (172.0f+player.y*UNIT_TO_PIX-30.0f);
    if(gravityFlipped){if(target>68.0f)target=68.0f;}
    else if(target<172.0f)target=172.0f;
    cameraGround+=(target-cameraGround)*fminf(1.0f,dt*7.0f);
}

static void drawMenu(Canvas *c) {
    rectangle(c,0,0,c->w,c->h,C_BLACK);
    sprite(c,IM_LEFTBAR,26,40,13,180);
    sprite(c,IM_RIGHTBAR,361,40,13,180);
    sprite(c,IM_TITLE,16,6,368,45);
    sprite(c,IM_MENU,132,51,136,161);
    // The menu sprite is rendered on the TOP screen: touch buttons stay below.
    int ay=51+menuItem*23;
    int nudge=(int)(1.5f+1.5f*sinf(animationTime*7.5f));
    sprite(c,IM_RIGHTARROW,87+nudge,ay+5,22,16);
    sprite(c,IM_LEFTARROW,290-nudge,ay+5,22,16);
    sprite(c,IM_FOOTER,76,214,248,24);
}
static bool isInSection(const LevelSection *sections,int count,float xpos){
    for(int i=0;i<count;++i)if(xpos>=sections[i].a && xpos<=sections[i].b)return true;
    return false;
}
static void fillBackground(Canvas *c){
    const size_t frameBytes=400u*240u*3u;
    const u8 *a=currentPalette<5?cachedGameBackground[currentPalette]:NULL;
    if(paletteTransition>=1.0f || currentPalette==previousPalette){
        if(a)memcpy(c->p,a,frameBytes);
        else memset(c->p,0,frameBytes);
        return;
    }
    const u8 *b=previousPalette<5?cachedGameBackground[previousPalette]:NULL;
    if(!a && !b){memset(c->p,0,frameBytes);return;}
    unsigned mix=(unsigned)(paletteTransition*256.0f);
    unsigned inv=256u-mix;
    // Palette morph is tied to actual source event x coordinates, not music beats.
    // Color channels are the bytes in the original pre-rendered textures.
    for(size_t i=0;i<frameBytes;++i){
        unsigned av=a?a[i]:0u,bv=b?b[i]:0u;
        c->p[i]=(u8)((bv*inv+av*mix)>>8);
    }
}
static void drawGameplay(Canvas *c) {
    fillBackground(c);
    float ground=cameraGround;
    // The original displays only the fine white ground line, not painted
    // platforms, a grid or a filled floor. Pits interrupt the line.
    if(ground>=0 && ground<240) {
        // Precompute visible holes once per frame, not once per pixel.
        u8 solid[400];memset(solid,1,sizeof(solid));
        for(int i=0;i<numObjects;++i){
            if(objects[i].kind!=2)continue;
            int l=121+(int)((objects[i].x-player.x)*UNIT_TO_PIX);
            int r=121+(int)((objects[i].y-player.x)*UNIT_TO_PIX);
            if(r<=0 || l>=400)continue;
            if(l<0)l=0;
            if(r>400)r=400;
            if(l<r)memset(solid+l,0,(size_t)(r-l));
        }
        int runStart=-1;
        for(int xx=0;xx<=400;++xx){
            bool keep=xx<400 && solid[xx]!=0;
            if(keep && runStart<0)runStart=xx;
            if(!keep && runStart>=0){
                rectangle(c,runStart,(int)ground,xx-runStart,1,RGB(241,251,250));
                runStart=-1;
            }
        }
    }
    // Sorted geometry allows culling offscreen objects before rasterization.
    float leftX=player.x-(121.0f+22.0f)/UNIT_TO_PIX;
    for(int i=firstObjectAt(leftX);i<numObjects;++i) {
        LevelObject *o=&objects[i];
        if(o->x>player.x+(400.0f-121.0f+22.0f)/UNIT_TO_PIX)break;
        if(o->kind>1)continue;
        int sx=121+(int)((o->x-player.x)*UNIT_TO_PIX);
        if(sx<-20 || sx>420)continue;
        int sy=gravityFlipped?
            (int)(ground+o->y*UNIT_TO_PIX):
            (int)(ground-(o->y+TILE)*UNIT_TO_PIX);
        // Original authored rise/fall sections: visual-only movement of blocks.
        // Smooth-in window is an approximation until PC frame-by-frame measured.
        if(o->kind==0 && isInSection(risingSections,numRising,(float)o->x)){
            float entry=fminf(1.0f,fmaxf(0.0f,(400.0f-sx)/110.0f));
            sy+=(int)((1.0f-entry)*20.0f);
        }
        if(o->kind==0 && isInSection(fallingSections,numFalling,(float)o->x)){
            float leave=fminf(1.0f,fmaxf(0.0f,(sx+40.0f)/110.0f));
            sy-=(int)((1.0f-leave)*20.0f);
        }
        if(sy<-20 || sy>250)continue;
        if(gravityFlipped && o->kind==1)
            spriteRotated(c,IM_SPIKE,sx,sy,16,3.14159265f);
        else sprite(c,o->kind==0?IM_BLOCK:IM_SPIKE,sx,sy,16,16);
    }
    int py=gravityFlipped?(int)(ground+player.y*UNIT_TO_PIX):
        (int)(ground-(player.y+TILE)*UNIT_TO_PIX);
    if(scene!=S_DEAD) spriteRotated(c,IM_PLAYER,121,py,16,
        gravityFlipped?-player.angle:player.angle);
    // The original game shows ATTEMPT <n> centered at the top and no progress
    // percentage or progress bar in ordinary gameplay.
    // Original sprite for the word "Attempt" and original orange cube texture.
    char count[32];
    snprintf(count,sizeof(count),"%d",attempts);
    int countW=textWidth(count,2);
    int attemptW=90+8+countW;
    int startX=(c->w-attemptW)/2;
    if(practiceMode){
        sprite(c,IM_PRACTICE_HEADER,103,3,194,27);
        sprite(c,IM_ATTEMPT,startX,33,90,24);
        drawText(c,startX+98,38,count,2,C_WHITE);
        char msg[48];snprintf(msg,sizeof(msg),"FLAGS LEFT %d",MAX_FLAGS-flagCount);
        centered(c,62,msg,1,C_WHITE);
        // Source-game flag artwork is drawn at the authored checkpoint positions.
        for(int f=0;f<flagCount;++f){
            const Player *p=&flags[f].saved;
            int fx=121+(int)((p->x-player.x)*UNIT_TO_PIX);
            int fy=(int)(ground-(p->y+TILE)*UNIT_TO_PIX)-17;
            if(fx>=-22 && fx<400 && fy>=-45 && fy<240){
                int flagW=13,flagH=31;
                if(f==flagCount-1 && flagAnimation>0.0f){
                    float t=1.0f-flagAnimation/0.25f;
                    if(t<0)t=0;if(t>1)t=1;
                    flagH=(int)(31.0f*t);
                }
                if(flagH>0)sprite(c,IM_FLAG,fx,fy+31-flagH,flagW,flagH);
            }
        }
    }else{
        sprite(c,IM_ATTEMPT,startX,10,90,24);
        drawText(c,startX+98,15,count,2,C_WHITE);
    }
}
static void drawTop(Canvas *c) {
    if(scene==S_MENU){drawMenu(c);return;}
    if(scene==S_SPLASH){
        rectangle(c,0,0,400,240,C_BLACK);
        sprite(c,IM_LEFTBAR,26,40,13,180);
        sprite(c,IM_RIGHTBAR,361,40,13,180);
        sprite(c,IM_TITLE,16,9,368,45);
        // Original press-start graphic, gently pulsed like the menu hint.
        if(fmodf(animationTime,1.5f)<1.23f)
            sprite(c,IM_PRESSSTART,118,124,164,30);
        sprite(c,IM_FOOTER,76,214,248,24);
        return;
    }
    if(scene==S_LEVELS){
        rectangle(c,0,0,400,240,C_BLACK);
        sprite(c,IM_LEFTBAR,26,40,13,180);
        sprite(c,IM_RIGHTBAR,361,40,13,180);
        sprite(c,IM_TITLE,16,6,368,45);
        // Original menu art has the same level names, colors and typography
        // seen in the user's video: Fire Aura / Original / Chaoz / Heaven / Phazd.
        sprite(c,IM_LEVELS,107,67,198,128);
        int selectionY[]={67,82,99,115,133};
        int yy=selectionY[stageChoice];
        int nudge=(int)(1.5f+1.5f*sinf(animationTime*7.5f));
        sprite(c,IM_RIGHTARROW,79+nudge,yy,21,16);
        sprite(c,IM_LEFTARROW,301-nudge,yy,21,16);
        return;
    }
    if(scene==S_PLAY || scene==S_DEAD || scene==S_PAUSED || scene==S_WIN){
        drawGameplay(c);
        if(scene==S_DEAD){
            // Original game returns automatically to another attempt.
        }else if(scene==S_PAUSED){
            rectangle(c,42,74,316,120,C_BLACK);
            sprite(c,IM_RESUMEMENU,91,92,218,88);
            if(practiceMode) centered(c,189,"Y EXIT PRACTICE",1,C_WHITE);
        }else if(scene==S_WIN){
            // These are the original PC game's 'endnoflags' and
            // 'endpracticemode' images, not a new result screen.
            // A brief black fade follows completion, then the graphic settles.
            float fade=fminf(1.0f,winAge/0.35f);
            rectangle(c,0,0,400,240,C_BLACK);
            if(fade>0.03f){
                int art=practiceMode?IM_END_PRACTICE:IM_END_NORMAL;
                int ww=practiceMode?352:350,hh=practiceMode?169:169;
                sprite(c,art,(400-ww)/2,33,ww,hh);
                if(fade<1.0f){
                    // Fade in by darkening the already-composited original art.
                    int dark=(int)((1.0f-fade)*235.0f);
                    for(int x=0;x<400;x++){
                        u8 *col=c->p+(size_t)x*240*3;
                        for(int k=0;k<240*3;k++)col[k]=(u8)((col[k]*(255-dark))>>8);
                    }
                }
            }
        }
        return;
    }
    rectangle(c,0,0,400,240,C_BLACK);
    sprite(c,IM_TITLE,16,5,368,45);
    if(scene==S_INFO){
        centered(c,64,infoPage==0?"HOW TO PLAY":"ABOUT",2,C_CYAN);
        centered(c,103,infoPage==0?"JUMP OVER SPIKES":"PERSONAL 3DS PORT",2,C_WHITE);
        centered(c,130,infoPage==0?"AND BLOCKS!":"USING YOUR GAME FILES",2,C_WHITE);
        centered(c,170,infoPage==0?"A OR TOUCH TO JUMP":"ORIGINAL GAME BY FLUKEGAMES",1,C_YELLOW);
        centered(c,193,"B TO RETURN",1,C_MUTED);
    }else if(scene==S_STATS){
        centered(c,65,"STATS",2,C_CYAN);
        char temp[50];snprintf(temp,sizeof(temp),"TOTAL ATTEMPTS %d",attempts);centered(c,103,temp,2,C_WHITE);
        snprintf(temp,sizeof(temp),"BEST %d%%",bestPercent);centered(c,138,temp,2,C_YELLOW);
        centered(c,193,"B TO RETURN",1,C_MUTED);
    }else if(scene==S_OPTIONS){
        centered(c,65,"OPTIONS",2,C_CYAN);
        centered(c,111,soundEnabled?"MUSIC ON":"MUSIC OFF",2,C_WHITE);
        centered(c,153,"A TO TOGGLE",2,C_YELLOW);
        centered(c,193,"B TO RETURN",1,C_MUTED);
    }
}
static void button(Canvas *c,int x,int y,int w,int h,const char *label,bool active) {
    rectangle(c,x,y,w,h,active?RGB(22,113,145):RGB(13,48,70));
    frame(c,x,y,w,h,active?RGB(113,245,251):RGB(30,145,185));
    int scale=(textWidth(label,2)<w-8)?2:1;
    drawText(c,x+(w-textWidth(label,scale))/2,y+(h-7*scale)/2,label,scale,C_WHITE);
}
static void drawBottom(Canvas *c,bool pressed) {
    rectangle(c,0,0,320,240,C_DARK);
    rectangle(c,0,0,320,4,C_CYAN);
    if(scene==S_SPLASH){
        centered(c,34,"THE IMPOSSIBLE GAME",2,C_CYAN);
        button(c,35,91,250,103,"START",false);
        centered(c,217,"A OR START TO CONTINUE",1,C_MUTED);
    }else if(scene==S_MENU || scene==S_LEVELS){
        centered(c,14,"THE IMPOSSIBLE GAME",2,C_CYAN);
        centered(c,38,"3DS TOUCH CONTROLS",1,C_MUTED);
        button(c,10,74,71,130,"UP",false);
        button(c,87,74,146,130,"SELECT",false);
        button(c,239,74,71,130,"DOWN",false);
        centered(c,216,"DPAD + A ALSO WORK",1,C_MUTED);
    }else if(scene==S_PLAY || scene==S_DEAD || scene==S_WIN){
        centered(c,11,practiceMode?"PRACTICE MODE":"IMPOSSIBLE GAME 3DS",2,C_CYAN);
        centered(c,40,practiceMode?"R FLAG / L UNDO":"A / TOUCH  JUMP",1,C_MUTED);
        button(c,9,74,214,145,scene==S_PLAY?"JUMP":"RETRY",pressed);
        button(c,230,74,81,67,"FLAG",false);
        button(c,230,148,81,71,practiceMode?"UNDO":"PAUSE",false);
        centered(c,223,practiceMode?"START PAUSE  Y EXIT PRACTICE":"START PAUSE",1,C_MUTED);
    }else if(scene==S_PAUSED){
        centered(c,13,practiceMode?"PRACTICE PAUSED":"PAUSED",2,C_CYAN);
        button(c,25,57,270,50,"RESUME",false);
        button(c,25,116,270,50,practiceMode?"EXIT PRACTICE":"RESTART",false);
        button(c,25,174,270,51,"QUIT LEVEL",false);
    }else{
        centered(c,25,"THE IMPOSSIBLE GAME",2,C_CYAN);
        button(c,35,91,250,102,scene==S_OPTIONS?"TOGGLE":"BACK",false);
        centered(c,213,"PRESS B TO RETURN",1,C_MUTED);
    }
}
static void openSelected(void) {
    playEffect(1);
    if(menuItem==0){stageChoice=0;scene=S_LEVELS;beginFadeIn();return;}
    if(menuItem==1 || menuItem==5){scene=S_INFO;infoPage=(menuItem==1)?0:1;}
    else if(menuItem==2 || menuItem==3) scene=S_STATS;
    else if(menuItem==4)scene=S_OPTIONS;
    else if(menuItem==6)isQuit=true;
}
int main(int argc,char **argv) {
    (void)argc;(void)argv;
    gfxInitDefault();
    gfxSet3D(false);
    romfsInit();
    for(int k=0;k<NUM_IMAGES;++k)loadImage(k);
    for(int bg=0;bg<5;++bg){
        cachedGameBackground[bg]=(u8*)malloc(400*240*3);
        if(cachedGameBackground[bg]) {
            Canvas cache={cachedGameBackground[bg],400,240};
            rectangle(&cache,0,0,400,240,RGB(12,36,37));
            sprite(&cache,bg==0?IM_BG:IM_BG2+bg-1,0,0,400,240);
        }
    }
    cachedBottom=(u8*)malloc(320*240*3);
    bool hasLevel=false;
    for(int i=0;i<5;++i){stagesAvailable[i]=hasStage(i);hasLevel |= stagesAvailable[i];}
    initAudio();
    u64 prev=osGetTime();
    float accumulator=0.0f;
    bool jumpLatched=false;
    // All of these act as the same single jump control during gameplay.
    const u32 jumpKeys=KEY_A|KEY_B|KEY_X|KEY_Y|
                       KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT;
    while(aptMainLoop() && !isQuit) {
        u64 now=osGetTime();
        float dt=(float)(now-prev)/1000.0f;prev=now;
        if(dt<0)dt=0;
        if(dt>0.15f)dt=0.15f;
        animationTime+=dt;
        if(animationTime>1000.0f)animationTime-=1000.0f;
        if(scene==S_WIN)winAge+=dt;
        flagAnimation=fmaxf(0.0f,flagAnimation-dt);
        hidScanInput();u32 down=hidKeysDown(),held=hidKeysHeld();
        touchPosition touch;hidTouchRead(&touch);
        bool justTouch=(down&KEY_TOUCH)!=0;
        bool holdingTouch=(held&KEY_TOUCH)!=0;
        bool touchJump=holdingTouch && touch.px<230;
        bool touchJumpDown=justTouch && touch.px<230;
        if(down&KEY_SELECT)break;
        bool justSpawned=false;
        if(launchingStage){
            fadeOutRemaining-=dt;
            if(fadeOutRemaining<=0){
                launchingStage=false;
                resetPlayer();
                accumulator=0;
                jumpLatched=false;
                justSpawned=true;
            }
        }
        if(launchingStage){
            // Menu fades to black before the game and its soundtrack begin.
        }else if(scene==S_SPLASH){
            splashTime+=dt;
            if((down&(KEY_A|KEY_START)) || justTouch){scene=S_MENU;beginFadeIn();}
        }else if(scene==S_LEVELS){
            if((down&KEY_UP)||(justTouch&&touch.px<82&&touch.py>=74))stageChoice=(stageChoice+4)%5;
            else if((down&KEY_DOWN)||(justTouch&&touch.px>=239&&touch.py>=74))stageChoice=(stageChoice+1)%5;
            else if((down&KEY_B)){scene=S_MENU;beginFadeIn();}
            else if((down&KEY_A)||(justTouch&&touch.px>=87&&touch.px<234&&touch.py>=74)){
                if(loadLevel(stageChoice)){
                    attempts=0;
                    bestPercent=0;
                    practiceMode=false;
                    flagCount=0;
                    launchingStage=true;
                    fadeOutRemaining=fadeSeconds;
                }
            }
        }else if(scene==S_MENU){
            if((down&KEY_UP) || (justTouch && touch.py>=74 && touch.py<205 && touch.px<82))menuItem=(menuItem+6)%7;
            else if((down&KEY_DOWN)||(justTouch&&touch.py>=74&&touch.py<205&&touch.px>=239))menuItem=(menuItem+1)%7;
            else if((down&KEY_A)||(justTouch && touch.py>=74 && touch.py<205 && touch.px>=87 && touch.px<234))openSelected();
            if((down&KEY_B)||(down&KEY_START))isQuit=true;
        }else if(scene==S_PLAY){
            if((down&KEY_START)||(justTouch&&touch.px>=230&&touch.py>=148&&!practiceMode)){
                scene=S_PAUSED; jumpLatched=false;
                if(dspReady)ndspChnSetPaused(0,true);
            }else if((down&KEY_R)||(justTouch&&touch.px>=230&&touch.py>=74&&touch.py<143)){
                placeFlag();
            }else if((down&KEY_L)||(justTouch&&touch.px>=230&&touch.py>=148&&practiceMode)){
                removeFlag();
            }else if(!justSpawned) {
                if((down&jumpKeys)||touchJumpDown)jumpLatched=true;
                bool wantsJump=((held&jumpKeys)!=0)||touchJump;
                // Advance physics in equal 1/120 s steps, independent of display FPS.
                accumulator+=dt;
                int steps=0;
                while(accumulator>=1.0f/120.0f && steps<18 && scene==S_PLAY){
                    // Deliver new presses exactly once. A pre-landing press is
                    // buffered by updateGame for the next few physics ticks.
                    updateGame(1.0f/120.0f,wantsJump,jumpLatched);
                    accumulator-=1.0f/120.0f;
                    jumpLatched=false;
                    steps++;
                }
                // Never accumulate an ever-growing backlog after a hardware stall.
                if(accumulator>1.0f/120.0f*18.0f)accumulator=1.0f/120.0f*18.0f;
                if(scene!=S_PLAY){accumulator=0; jumpLatched=false;}
            }
        }else if(scene==S_DEAD || scene==S_WIN){
            deathTimer+=dt;
            if((down&KEY_START)||(justTouch&&touch.px>=230&&touch.py>=148))goMenu();
            else if(scene==S_DEAD && (deathTimer>=0.55f || (down&jumpKeys))) {
                respawn();accumulator=0;jumpLatched=false;
            }
            else if(scene==S_WIN && ((down&jumpKeys) || justTouch))goMenu();
        }else if(scene==S_PAUSED){
            if((down&KEY_START)||(down&KEY_A)||(justTouch&&touch.py>=57&&touch.py<108)) {
                scene=S_PLAY; if(dspReady)ndspChnSetPaused(0,false);
                accumulator=0; jumpLatched=false;
            }else if((down&KEY_B)||(justTouch&&touch.py>=174)){
                goMenu();
            }else if((down&KEY_Y)||(justTouch&&touch.py>=116&&touch.py<167)){
                if(practiceMode)exitPracticeMode();else resetPlayer();
                accumulator=0;jumpLatched=false;
            }
        }else if(scene==S_INFO || scene==S_STATS || scene==S_OPTIONS){
            if((down&KEY_B)||(down&KEY_START)||(justTouch&&scene!=S_OPTIONS))scene=S_MENU;
            else if(scene==S_OPTIONS && ((down&KEY_A)||justTouch)){
                soundEnabled=!soundEnabled;
                if(soundEnabled)startMusic("romfs:/audio/menu.pcm");else stopMusic();
            }
        }
        updateMusic();
        if(fadeRemaining>0)fadeRemaining=fmaxf(0,fadeRemaining-dt);
        u16 w=0,h=0;
        Canvas top={gfxGetFramebuffer(GFX_TOP,GFX_LEFT,&w,&h),TOP_W,LCD_H};
        Canvas bottom={gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h),BOT_W,LCD_H};
        if(!hasLevel){
            rectangle(&top,0,0,400,240,C_DARK);
            centered(&top,73,"NO LEVELS INSTALLED",2,C_YELLOW);
            centered(&top,110,"RUN PREPARE_LEVELS.PY",2,C_WHITE);
            rectangle(&bottom,0,0,320,240,C_DARK);
            centered(&bottom,105,"CHECK THE README",2,C_WHITE);
        }else{
            drawTop(&top);
            drawFade(&top);
            // Top screen updates every frame. The bottom controls are only
            // rasterized when their state changes; copying their cached frame
            // is much cheaper on Old 3DS than rendering all buttons at 60Hz.
            if(cachedBottom) {
                bool pressed=(scene==S_PLAY && touchJump);
                if(cachedBottomScene!=(int)scene || cachedBottomPressed!=pressed) {
                    Canvas offscreen={cachedBottom,320,240};
                    drawBottom(&offscreen,pressed);
                    cachedBottomScene=(int)scene;
                    cachedBottomPressed=pressed;
                    ++bottomCacheRevision;
                }
                int slot=-1;
                for(int i=0;i<2;++i)if(knownBottomBuffers[i]==bottom.p)slot=i;
                if(slot<0){
                    slot=knownBottomBuffers[0]?1:0;
                    knownBottomBuffers[slot]=bottom.p;
                    bottomBufferRevision[slot]=-1;
                }
                if(bottomBufferRevision[slot]!=bottomCacheRevision){
                    memcpy(bottom.p,cachedBottom,320*240*3);
                    bottomBufferRevision[slot]=bottomCacheRevision;
                }
            }else drawBottom(&bottom,touchJump);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    endAudio();
    for(int k=0;k<NUM_IMAGES;++k)free(images[k].pixels);
    for(int bg=0;bg<5;++bg)free(cachedGameBackground[bg]);
    free(cachedBottom);
    romfsExit();gfxExit();
    return 0;
}