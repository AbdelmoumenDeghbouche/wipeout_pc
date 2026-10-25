/* Custom soundtrack. Audio files in <user>/music (or GOW3_MUSIC_DIR), subfolders included, play
 * shuffled and looped instead of the game's music, as on a PS4 playing the user's own music: the
 * game's BGM port (sceAudioOut type 1) carries only its music. FFmpeg decodes the files to 48 kHz
 * stereo float on their own thread. Their level follows the game's music (mean square over ~8 s),
 * so the in-game music volume still applies. */
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
static char **files;
static size_t file_count, file_cap;
static volatile int enabled; /* the files replace the game's music */
static double game_ms, ours_ms;
static char notice[600]; /* "Now playing" once the ring reaches frame notice_at */
static size_t notice_at;
static int notice_pending;

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
static void scan(const char *dir, int depth) {
    DIR *d=opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        char path[1024]; struct stat s;
        if (e->d_name[0]=='.' || snprintf(path,sizeof(path),"%s/%s",dir,e->d_name)>=(int)sizeof(path) || stat(path,&s)) continue;
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

static void push(const float *pcm, int frames) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<(size_t)frames;) {
        while (head-tail>=RING) pthread_cond_wait(&space,&lock);
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
static void announce(const AVFormatContext *format, const AVStream *stream, const char *path) {
    const AVDictionaryEntry *title=av_dict_get(format->metadata,"title",NULL,0), *artist=av_dict_get(format->metadata,"artist",NULL,0);
    if (!title) title=av_dict_get(stream->metadata,"title",NULL,0);
    if (!artist) artist=av_dict_get(stream->metadata,"artist",NULL,0);
    char name[256], text[600];
    const char *slash=strrchr(path,'/');
    snprintf(name,sizeof(name),"%s",slash ? slash+1 : path);
    char *dot=strrchr(name,'.');
    if (dot) *dot=0;
    for (char *c=name; *c; ++c) if (*c=='_') *c=' ';
    if (title && artist) snprintf(text,sizeof(text),"Now playing: %s - %s",artist->value,title->value);
    else snprintf(text,sizeof(text),"Now playing: %s",title ? title->value : name);
    pthread_mutex_lock(&lock); /* shown once the ring plays this file's first frame */
    memcpy(notice,text,sizeof(notice)); notice_at=head; notice_pending=1;
    pthread_mutex_unlock(&lock);
}
/* Decodes one file into the ring; 0 if it cannot be played. */
static int play(const char *path) {
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
        announce(format,format->streams[stream],path);
        while (av_read_frame(format,packet)>=0) {
            if (packet->stream_index==stream && avcodec_send_packet(codec,packet)>=0)
                while (avcodec_receive_frame(codec,frame)>=0) convert(swr,frame);
            av_packet_unref(packet);
        }
        avcodec_send_packet(codec,NULL);
        while (avcodec_receive_frame(codec,frame)>=0) convert(swr,frame);
        convert(swr,NULL);
    } else {
        printf("Music: cannot play %s\n",path);
    }
    swr_free(&swr); avcodec_free_context(&codec); avformat_close_input(&format);
    av_frame_free(&frame); av_packet_free(&packet);
    return ok;
}
static void *decode(void *dir) {
    scan(dir,0);
    if (!file_count) return NULL;
    printf("Music: %zu files in %s replace the game's music\n",file_count,(const char *)dir);
    enabled=1;
    srand((unsigned)time(NULL));
    for (int played=1; played;) {
        played=0;
        for (size_t i=file_count;i>1;--i) { size_t j=(size_t)rand()%i; char *t=files[i-1]; files[i-1]=files[j]; files[j]=t; }
        for (size_t i=0;i<file_count;++i) played|=play(files[i]);
    }
    printf("Music: none of the files plays; the game's music is back\n");
    enabled=0;
    return NULL;
}
static void start(void) {
    static char dir[1024];
    const char *custom=getenv("GOW3_MUSIC_DIR");
    if (custom && *custom) snprintf(dir,sizeof(dir),"%s",custom);
    else snprintf(dir,sizeof(dir),"%s/music",runtime_file_user_dir());
    pthread_t thread;
    if (!pthread_create(&thread,NULL,decode,dir)) pthread_detach(thread);
}

int runtime_music_bgm(float *pcm, int frames, int channels) {
    static float music[2048*2];
    char text[sizeof(notice)];
    int show=0;
    pthread_once(&once,start);
    if (!enabled || channels<2 || frames<=0 || frames>2048) return 0;
    double game=0, ours=0;
    for (int i=0;i<frames*channels;++i) game+=(double)pcm[i]*pcm[i];
    pthread_mutex_lock(&lock);
    size_t n=head-tail;
    if (n>(size_t)frames) n=(size_t)frames;
    for (size_t i=0;i<n;++i,++tail) { music[i*2]=ring[(tail%RING)*2]; music[i*2+1]=ring[(tail%RING)*2+1]; }
    if (notice_pending && tail>=notice_at) { memcpy(text,notice,sizeof(text)); notice_pending=0; show=1; }
    pthread_cond_signal(&space);
    pthread_mutex_unlock(&lock);
    if (show) { printf("Music: %s\n",text); gow3gpu_notice(text); }
    for (size_t i=0;i<n*2;++i) ours+=(double)music[i]*music[i];
    const double a=frames/(48000.0*8);
    game_ms+=a*(game/frames-game_ms);
    if (n==(size_t)frames) ours_ms+=a*(ours/frames-ours_ms); /* not the gaps between files */
    double gain=ours_ms>1e-12 ? sqrt(game_ms/ours_ms) : 0;
    if (gain>1) gain=1;
    memset(pcm,0,(size_t)frames*(size_t)channels*sizeof(float));
    for (size_t i=0;i<n;++i) {
        pcm[i*(size_t)channels]=(float)(music[i*2]*gain);
        pcm[i*(size_t)channels+1]=(float)(music[i*2+1]*gain);
    }
    return 1;
}
