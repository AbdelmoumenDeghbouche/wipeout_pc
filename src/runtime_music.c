/* Custom soundtrack. Audio files in <user>/music (or GOW3_MUSIC_DIR), subfolders included, play
 * instead of the game's music, as on a PS4 playing the user's own music: the game's BGM port
 * (sceAudioOut type 1) carries only its music. FFmpeg decodes the files to 48 kHz stereo float on
 * their own thread. Each song is leveled (mean square over ~8 s, without the bass below ~150 Hz)
 * to a fixed level times "music_volume"; following the game's own level made them fade with it
 * (title screen, loading). "music" and "music_shuffle" in gow3.ini (launcher's Music page, in-game menu's Music
 * tab) turn them off or play the list in order; the Music tab picks a song or skips
 * (runtime_music_play, runtime_music_step). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#define RING (48000*4) /* frames: 4 s */
static float ring[RING*2];
static size_t head, tail; /* frames written and read so far */
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t space=PTHREAD_COND_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static char dir[1024];
static char **files, **names; /* sorted by path; names: file name without extension, '_' as space */
static size_t file_count, file_cap;
static volatile int scanned;  /* files and names are complete */
static volatile int playable=1; /* 0: none of the files could be played */
static size_t *order, pos;    /* play order (file indexes): order[pos] is being decoded. Under lock: */
static int order_shuffled;
static long jump=-1;          /* the song the menu asked for */
static volatile int skipping; /* the decoder drops what it decodes until it takes the jump */
static char notice[600];      /* "Now playing" once the ring reaches frame notice_at */
static size_t notice_at;
static long notice_file=-1, playing=-1;
static int notice_pending;
static double ours_ms;
/* Target RMS per channel at 100%, measured above ~150 Hz: the bass, most of a modern song's energy
 * (67% below 150 Hz in a trap song) and inaudible on laptop speakers, made songs land far under the
 * race's effects (about 0.02 there). */
#define LEVEL 0.045

static int audio_file(const char *name) {
    static const char *const types[]={"mp3","ogg","oga","opus","flac","wav","m4a","aac","wma"};
    const char *dot=strrchr(name,'.');
    if (!dot) return 0;
    char ext[8]; size_t n=0;
    for (++dot; *dot && n<sizeof(ext)-1; ++dot) ext[n++]=(char)tolower((unsigned char)*dot);
    if (*dot) return 0;
    ext[n]=0;
    for (size_t i=0;i<sizeof(types)/sizeof(*types);++i) if (!strcmp(ext,types[i])) return 1;
    return 0;
}
static void scan(const char *folder, int depth) {
    DIR *d=opendir(folder);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        char path[1024]; struct stat s;
        if (e->d_name[0]=='.' || snprintf(path,sizeof(path),"%s/%s",folder,e->d_name)>=(int)sizeof(path) || stat(path,&s)) continue;
        if (S_ISDIR(s.st_mode)) { if (depth<8) scan(path,depth+1); continue; }
        if (!audio_file(e->d_name)) continue;
        if (file_count==file_cap) {
            char **grown=realloc(files,(file_cap ? file_cap*2 : 64)*sizeof(*files));
            if (!grown) break;
            files=grown; file_cap=file_cap ? file_cap*2 : 64;
        }
        if ((files[file_count]=strdup(path))) ++file_count;
    }
    closedir(d);
}
static int compare(const void *a, const void *b) {
    const unsigned char *x=*(const unsigned char *const *)a, *y=*(const unsigned char *const *)b;
    for (; *x && tolower(*x)==tolower(*y); ++x, ++y) {}
    return tolower(*x)-tolower(*y);
}
static char *display_name(const char *path) {
    const char *slash=strrchr(path,'/');
    char *name=strdup(slash ? slash+1 : path);
    if (!name) return NULL;
    char *dot=strrchr(name,'.');
    if (dot) *dot=0;
    for (char *c=name; *c; ++c) if (*c=='_') *c=' ';
    return name;
}
/* The play order from `first` on (under lock): shuffled, or the list's order. */
static void arrange(int shuffle, size_t first) {
    for (size_t i=0;i<file_count;++i) order[i]=i;
    pos=first;
    if (shuffle) {
        for (size_t i=file_count;i>1;--i) { size_t j=(size_t)rand()%i, t=order[i-1]; order[i-1]=order[j]; order[j]=t; }
        for (size_t i=0;i<file_count;++i) if (order[i]==first) { order[i]=order[0]; order[0]=first; break; }
        pos=0;
    }
    order_shuffled=shuffle;
}

static void push(const float *pcm, int frames) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<(size_t)frames && !skipping;) {
        while (head-tail>=RING && !skipping) pthread_cond_wait(&space,&lock);
        if (skipping) break;
        size_t at=head%RING, n=(size_t)frames-i;
        if (n>RING-(head-tail)) n=RING-(head-tail);
        if (n>RING-at) n=RING-at;
        memcpy(ring+at*2,pcm+i*2,n*2*sizeof(float));
        head+=n; i+=n;
    }
    pthread_mutex_unlock(&lock);
}
/* One decoded frame (NULL: the resampler's last samples) to the ring. */
static void convert(SwrContext *swr, const AVFrame *frame) {
    static float *out;
    static int cap;
    int need=swr_get_out_samples(swr,frame ? frame->nb_samples : 0);
    if (need<=0) return;
    if (need>cap) {
        float *grown=realloc(out,(size_t)need*2*sizeof(float));
        if (!grown) return;
        out=grown; cap=need;
    }
    uint8_t *planes[1]={(uint8_t *)out};
    int n=swr_convert(swr,planes,cap,frame ? (const uint8_t *const *)frame->extended_data : NULL,frame ? frame->nb_samples : 0);
    if (n>0) push(out,n);
}
static void announce(const AVFormatContext *format, const AVStream *stream, size_t file) {
    const AVDictionaryEntry *title=av_dict_get(format->metadata,"title",NULL,0), *artist=av_dict_get(format->metadata,"artist",NULL,0);
    if (!title) title=av_dict_get(stream->metadata,"title",NULL,0);
    if (!artist) artist=av_dict_get(stream->metadata,"artist",NULL,0);
    char text[sizeof(notice)];
    if (title && artist) snprintf(text,sizeof(text),"Now playing: %s - %s",artist->value,title->value);
    else snprintf(text,sizeof(text),"Now playing: %s",title ? title->value : names[file]);
    pthread_mutex_lock(&lock); /* shown once the ring plays this file's first frame */
    if (!skipping) { memcpy(notice,text,sizeof(notice)); notice_at=head; notice_file=(long)file; notice_pending=1; }
    pthread_mutex_unlock(&lock);
}
/* Decodes one file into the ring; 0 if it cannot be played. */
static int play(size_t file) {
    const char *path=files[file];
    AVFormatContext *format=NULL; AVCodecContext *codec=NULL; SwrContext *swr=NULL;
    AVPacket *packet=av_packet_alloc(); AVFrame *frame=av_frame_alloc();
    const AVCodec *decoder=NULL;
    int stream=-1, ok=0;
    if (packet && frame && avformat_open_input(&format,path,NULL,NULL)>=0 && avformat_find_stream_info(format,NULL)>=0
        && (stream=av_find_best_stream(format,AVMEDIA_TYPE_AUDIO,-1,-1,&decoder,0))>=0 && (codec=avcodec_alloc_context3(decoder))
        && avcodec_parameters_to_context(codec,format->streams[stream]->codecpar)>=0 && avcodec_open2(codec,decoder,NULL)>=0) {
        if (codec->ch_layout.order==AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&codec->ch_layout,codec->ch_layout.nb_channels);
        AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        ok=swr_alloc_set_opts2(&swr,&stereo,AV_SAMPLE_FMT_FLT,48000,&codec->ch_layout,codec->sample_fmt,codec->sample_rate,0,NULL)>=0
           && swr_init(swr)>=0;
    }
    if (ok) {
        printf("Music: %s\n",path);
        announce(format,format->streams[stream],file);
        while (!skipping && av_read_frame(format,packet)>=0) {
            if (packet->stream_index==stream && avcodec_send_packet(codec,packet)>=0)
                while (avcodec_receive_frame(codec,frame)>=0) convert(swr,frame);
            av_packet_unref(packet);
        }
        if (!skipping) {
            avcodec_send_packet(codec,NULL);
            while (avcodec_receive_frame(codec,frame)>=0) convert(swr,frame);
            convert(swr,NULL);
        }
    } else {
        printf("Music: cannot play %s\n",path);
    }
    swr_free(&swr); avcodec_free_context(&codec); avformat_close_input(&format);
    av_frame_free(&frame); av_packet_free(&packet);
    return ok;
}
static void *decode(void *unused) {
    (void)unused;
    scan(dir,0);
    if (file_count) {
        qsort(files,file_count,sizeof(*files),compare);
        names=malloc(file_count*sizeof(*names));
        order=malloc(file_count*sizeof(*order));
        if (!names || !order) file_count=0;
        for (size_t i=0;i<file_count;++i) if (!(names[i]=display_name(files[i]))) names[i]=files[i];
    }
    scanned=1;
    if (!file_count) return NULL;
    printf("Music: %zu files in %s replace the game's music\n",file_count,dir);
    srand((unsigned)time(NULL));
    pthread_mutex_lock(&lock);
    arrange(gow3gpu_music_shuffle(),(size_t)rand()%file_count);
    pthread_mutex_unlock(&lock);
    for (size_t failures=0; failures<file_count;) {
        pthread_mutex_lock(&lock);
        const size_t file=order[pos];
        pthread_mutex_unlock(&lock);
        failures=play(file) ? 0 : failures+1;
        pthread_mutex_lock(&lock);
        const int shuffle=gow3gpu_music_shuffle();
        if (jump>=0) {
            const size_t song=(size_t)jump;
            jump=-1; skipping=0; failures=0;
            if (shuffle!=order_shuffled) arrange(shuffle,song);
            else for (size_t i=0;i<file_count;++i) if (order[i]==song) pos=i;
        } else if (shuffle!=order_shuffled) {
            arrange(shuffle,shuffle ? (size_t)rand()%file_count : (file+1)%file_count);
        } else if (++pos==file_count) {
            arrange(shuffle,shuffle ? (size_t)rand()%file_count : 0); /* the next round */
        }
        pthread_mutex_unlock(&lock);
    }
    printf("Music: none of the files plays; the game's music is back\n");
    playable=0;
    return NULL;
}
static void start(void) {
    const char *custom=getenv("GOW3_MUSIC_DIR");
    if (custom && *custom) snprintf(dir,sizeof(dir),"%s",custom);
    else snprintf(dir,sizeof(dir),"%s/music",runtime_file_user_dir());
    pthread_t thread;
    if (pthread_create(&thread,NULL,decode,NULL)) scanned=1;
    else pthread_detach(thread);
}

int runtime_music_songs(void) { pthread_once(&once,start); return scanned ? (int)file_count : -1; }
const char *runtime_music_song(int song) { return scanned && song>=0 && (size_t)song<file_count ? names[song] : ""; }
const char *runtime_music_folder(void) { pthread_once(&once,start); return dir; }
int runtime_music_playing(void) {
    pthread_mutex_lock(&lock);
    const long song=playing;
    pthread_mutex_unlock(&lock);
    return (int)song;
}
void runtime_music_play(int song) {
    if (!scanned || song<0 || (size_t)song>=file_count) return;
    pthread_mutex_lock(&lock);
    jump=song; skipping=1; tail=head; notice_pending=0; /* drop what was decoded ahead */
    pthread_cond_broadcast(&space);
    pthread_mutex_unlock(&lock);
}
void runtime_music_step(int direction) {
    if (!scanned || !file_count) return;
    pthread_mutex_lock(&lock);
    const size_t now=playing>=0 ? (size_t)playing : order[pos];
    size_t k=pos;
    for (size_t i=0;i<file_count;++i) if (order[i]==now) k=i;
    const size_t song=order[(k+(direction>0 ? 1 : file_count-1))%file_count];
    pthread_mutex_unlock(&lock);
    runtime_music_play((int)song);
}

int runtime_music_bgm(float *pcm, int frames, int channels) {
    static float music[2048*2];
    char text[sizeof(notice)];
    int show=0;
    pthread_once(&once,start);
    if (!scanned || !file_count || !playable || channels<2 || frames<=0 || frames>2048) return 0;
    if (!gow3gpu_music_on()) return 0; /* the game's music; the ring waits where it stopped */
    pthread_mutex_lock(&lock);
    size_t n=head-tail;
    if (n>(size_t)frames) n=(size_t)frames;
    for (size_t i=0;i<n;++i,++tail) { music[i*2]=ring[(tail%RING)*2]; music[i*2+1]=ring[(tail%RING)*2+1]; }
    if (notice_pending && tail>=notice_at) { memcpy(text,notice,sizeof(text)); playing=notice_file; notice_pending=0; show=1; }
    pthread_cond_signal(&space);
    pthread_mutex_unlock(&lock);
    if (show) { printf("Music: %s\n",text); gow3gpu_notice(text); }
    static float hp_in[2][2], hp_out[2][2]; /* the measurement's two one-pole high-pass stages per channel */
    const float a=1.0f/(1.0f+2.0f*3.14159265f*150.0f/48000.0f);
    double ours=0;
    for (size_t i=0;i<n;++i) for (int c=0;c<2;++c) {
        float v=music[i*2+c];
        for (int s=0;s<2;++s) { const float y=a*(hp_out[c][s]+v-hp_in[c][s]); hp_in[c][s]=v; hp_out[c][s]=y; v=y; }
        ours+=(double)v*v;
    }
    if (n==(size_t)frames) { /* not the gaps between files; the first sound sets the level at once */
        const double now=ours/frames;
        /* Louder: within ~0.5 s (a loud song after a quiet one); quieter: over ~8 s (quiet parts stay quiet). */
        ours_ms=ours_ms>0 ? ours_ms+frames/(48000.0*(now>ours_ms ? 0.5 : 8.0))*(now-ours_ms) : now;
    }
    const double target=LEVEL*gow3gpu_music_volume()/100.0;
    double gain=ours_ms>1e-12 ? target*sqrt(2.0/ours_ms) : 0;
    if (gain>1) gain=1;
    memset(pcm,0,(size_t)frames*(size_t)channels*sizeof(float));
    for (size_t i=0;i<n;++i) {
        pcm[i*(size_t)channels]=(float)(music[i*2]*gain);
        pcm[i*(size_t)channels+1]=(float)(music[i*2+1]*gain);
    }
    return 1;
}
