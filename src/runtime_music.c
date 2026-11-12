/* Custom soundtrack. Audio files in <user>/music (or GOW3_MUSIC_DIR), subfolders included, play
 * instead of the game's music, as on a PS4 playing the user's own music: the game's BGM port
 * (sceAudioOut type 1) carries only its music. FFmpeg decodes the files to 48 kHz stereo float on
 * their own thread. Each song is leveled (mean square over ~8 s, without the bass below ~150 Hz)
 * to a fixed level times "music_volume", not to the game's level; only the game's silences and fades
 * are followed (runtime_music_bgm). "music" and "music_shuffle" in gow3.ini (launcher's Music page, in-game menu's Music
 * tab) turn them off or play the list in order; the Music tab picks a song or skips
 * (runtime_music_play, runtime_music_step). Songs in a "menu" subfolder play in the menus only,
 * the others in races (#66); the game's sound banks tell which (runtime_music_scene). */
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
static char current_song[256]; /* the song announced last, for Discord when the music comes back on */
static char notice[600];      /* "Now playing" once the ring reaches frame notice_at */
static size_t notice_at;
static long notice_file=-1, playing=-1;
/* Song groups by subfolder (#130): menu/ plays in the menus, zone/, eliminator/ and detonator/ in
 * those modes, the rest in the other races (and in a mode whose folder is empty). */
enum { GROUP_RACE, GROUP_MENU, GROUP_ZONE, GROUP_ELIMINATOR, GROUP_DETONATOR, GROUPS };
static const char *const group_folders[GROUPS]={"","menu","zone","eliminator","detonator"};
static unsigned char *menu_song; /* per file: its group */
static unsigned char *played;    /* per file: played in this round (music_played.txt, #124) */
static char played_path[1100];
static size_t menu_songs;        /* songs in any group's subfolder */
static size_t group_songs[GROUPS];
static volatile int mode;        /* GROUP_ZONE..GROUP_DETONATOR from the mode's sound banks, else 0 */
static volatile int in_race;     /* the game loaded a race's sound banks, not the menus' */
static volatile unsigned loads;  /* sound bank loads of a race or the menus: a loading screen */
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
/* Whether a song plays now: the group of the scene (menus, the race's mode, other races); every
 * song when that group has none. */
static int fits(size_t file) {
    const int group=!in_race ? GROUP_MENU : group_songs[mode] ? mode : GROUP_RACE;
    return !group_songs[group] || menu_song[file]==group;
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

/* #124: the songs of this round already played are kept in the user folder, so the next start
 * plays the others first: every song plays once before any plays again, however short the
 * sessions. A round ends when all the songs of the menus, or of the races, have played. */
static void load_played(void) {
    snprintf(played_path,sizeof(played_path),"%s/music_played.txt",runtime_file_user_dir());
    FILE *f=fopen(played_path,"r");
    if (!f) return;
    char line[1100];
    const size_t root=strlen(dir);
    while (fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        for (size_t i=0;i<file_count;++i) if (!strcmp(files[i]+root,line)) played[i]=1;
    }
    fclose(f);
}
static void save_played(void) { /* under lock */
    FILE *f=fopen(played_path,"w");
    if (!f) return;
    for (size_t i=0;i<file_count;++i) if (played[i]) fprintf(f,"%s\n",files[i]+strlen(dir));
    fclose(f);
}
/* Under lock: the first place in `order` from `from` on (wrapping) with a song that fits the
 * menus or the race and, in shuffle, has not played in this round; -1 if there is none. */
static long find_song(size_t from, int shuffle) {
    for (size_t step=0;step<file_count;++step) {
        const size_t i=(from+step)%file_count;
        if (fits(order[i]) && !(shuffle && played[order[i]])) return (long)i;
    }
    return -1;
}
/* Under lock: pos moves to the next song to play. In shuffle, once every song that fits now (the
 * menu songs, or the race songs) has played, those start a new round in a new order; the song
 * just played does not open it, so it never plays twice in a row (#124: the search used to wrap
 * past the end without a new order, so the same chain came round). */
static void next_song(size_t from, int shuffle, long last) {
    long i=find_song(from,shuffle);
    if (i<0 && shuffle) {
        for (size_t f=0;f<file_count;++f) if (fits(f)) played[f]=0;
        arrange(1,(size_t)rand()%file_count);
        i=find_song(0,1);
        if (i>=0 && (long)order[i]==last) { /* it plays last in this round instead */
            const long other=find_song((size_t)i+1,1);
            if (other>=0 && other!=i) i=other;
        }
    }
    if (i>=0) pos=(size_t)i;
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
    int n=swr_convert(swr,planes,cap,frame ? (const uint8_t **)frame->extended_data : NULL,frame ? frame->nb_samples : 0);
    if (n>0) push(out,n);
}
static void announce(const AVFormatContext *format, const AVStream *stream, size_t file) {
    const AVDictionaryEntry *title=av_dict_get(format->metadata,"title",NULL,0), *artist=av_dict_get(format->metadata,"artist",NULL,0);
    if (!title) title=av_dict_get(stream->metadata,"title",NULL,0);
    if (!artist) artist=av_dict_get(stream->metadata,"artist",NULL,0);
    char text[sizeof(notice)], song[sizeof(notice)];
    if (title && artist) snprintf(song,sizeof(song),"%s - %s",artist->value,title->value);
    else snprintf(song,sizeof(song),"%s",title ? title->value : names[file]);
    snprintf(text,sizeof(text),"Now playing: %.*s",(int)sizeof(text)-14,song); /* GCC -Wformat-truncation */
    pthread_mutex_lock(&lock); /* shown once the ring plays this file's first frame */
    const int announced=!skipping;
    if (announced) { memcpy(notice,text,sizeof(notice)); notice_at=head; notice_file=(long)file; notice_pending=1; snprintf(current_song,sizeof(current_song),"%.*s",(int)sizeof(current_song)-1,song); }
    pthread_mutex_unlock(&lock);
    if (announced) runtime_presence_song(song); /* Discord shows it too */
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
        menu_song=calloc(file_count,1);
        played=calloc(file_count,1);
        if (!names || !order || !menu_song || !played) file_count=0;
        const size_t root=strlen(dir);
        for (size_t i=0;i<file_count;++i) {
            if (!(names[i]=display_name(files[i]))) names[i]=files[i];
            const char *sub=files[i]+root; /* scan() joins with '/' */
            for (int g=1;g<GROUPS;++g) {
                const size_t n=strlen(group_folders[g]);
                if (sub[0]=='/' && !strncasecmp(sub+1,group_folders[g],n) && sub[1+n]=='/') menu_song[i]=(unsigned char)g;
            }
            ++group_songs[menu_song[i]];
            menu_songs+=menu_song[i]!=GROUP_RACE;
        }
    }
    scanned=1;
    if (!file_count) return NULL;
    printf("Music: %zu files in %s replace the game's music\n",file_count,dir);
    for (int g=1;g<GROUPS;++g)
        if (group_songs[g]) printf("Music: %zu of them in %s/ play in the %s\n",group_songs[g],group_folders[g],
                                   g==GROUP_MENU ? "menus" : group_folders[g]);
    srand((unsigned)time(NULL));
    pthread_mutex_lock(&lock);
    load_played();
    arrange(gow3gpu_music_shuffle(),(size_t)rand()%file_count);
    next_song(pos,order_shuffled,-1);
    pthread_mutex_unlock(&lock);
    for (size_t failures=0; failures<file_count;) {
        pthread_mutex_lock(&lock);
        const size_t file=order[pos];
        played[file]=1;
        save_played();
        pthread_mutex_unlock(&lock);
        failures=play(file) ? 0 : failures+1;
        pthread_mutex_lock(&lock);
        const int shuffle=gow3gpu_music_shuffle();
        if (jump>=0) { /* the song the menu asked for plays next */
            const size_t song=(size_t)jump;
            jump=-1; skipping=0; failures=0;
            if (shuffle!=order_shuffled) arrange(shuffle,song);
            else for (size_t i=0;i<file_count;++i) if (order[i]==song) pos=i;
        } else if (shuffle!=order_shuffled) {
            arrange(shuffle,shuffle ? (size_t)rand()%file_count : (file+1)%file_count);
            next_song(pos,shuffle,(long)file);
        } else {
            next_song(pos+1,shuffle,(long)file);
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
    size_t song=order[(k+(direction>0 ? 1 : file_count-1))%file_count];
    for (size_t tries=1; tries<file_count && !fits(song); ++tries)
        song=order[(k+(direction>0 ? 1+tries : file_count-1-tries))%file_count];
    pthread_mutex_unlock(&lock);
    runtime_music_play((int)song);
}

/* The game loaded a race's sound banks last, not the menus' (frame limit in the menus, #118). */
int runtime_music_in_race(void) { return in_race; }

void runtime_music_scene(const char *guest_path) {
    const char *env=strstr(guest_path,"/audio/sound/") ? strstr(guest_path,"/env") : NULL;
    const int was=in_race, was_mode=mode;
    if (strstr(guest_path,"/frontend_fliers.bnk")) { in_race=0; mode=0; ++loads; }
    else if (env && (env[4]=='_' || isdigit((unsigned char)env[4]))) { in_race=1; ++loads; } /* env_tower, env1_vinetak */
    /* The mode's sound banks (#130): env0_zone, speech_zone(_ngp), weapons_zone, ship_ngp_zone,
     * speech_zbattle; speech_elim; env0_det, speech_det, weapons_det. */
    const char *base=strrchr(guest_path,'/');
    char name[40]; size_t n=0;
    for (base=base ? base+1 : guest_path; base[n] && n<sizeof(name)-1; ++n) name[n]=(char)tolower((unsigned char)base[n]);
    name[n]=0;
    if ((strstr(name,"zone") || strstr(name,"zbattle")) && (!strncmp(name,"speech",6) || !strncmp(name,"weapons",7) || !strncmp(name,"ship",4) || !strncmp(name,"env",3)))
        mode=GROUP_ZONE;
    else if (!strcmp(name,"speech_elim.bnk")) mode=GROUP_ELIMINATOR;
    else if (!strcmp(name,"speech_det.bnk") || !strcmp(name,"weapons_det.bnk") || !strcmp(name,"env0_det.bnk")) mode=GROUP_DETONATOR;
    if ((in_race!=was || mode!=was_mode) && menu_songs) {
        const int group=!in_race ? GROUP_MENU : group_songs[mode] ? mode : GROUP_RACE;
        printf("Music: %s songs from now on\n",group==GROUP_RACE ? "race" : group_folders[group]);
    }
}

int runtime_music_bgm(float *pcm, int frames, int channels) {
    static float music[2048*2];
    char text[sizeof(notice)];
    int show=0;
    pthread_once(&once,start);
    if (!scanned || !file_count || !playable || channels<2 || frames<=0 || frames>2048) return 0;
    static int music_off; /* Discord shows no song while the game's music plays */
    if (!gow3gpu_music_on()) { /* the game's music; the ring waits where it stopped */
        if (!music_off) { music_off=1; runtime_presence_song(""); }
        return 0;
    }
    if (music_off) { music_off=0; runtime_presence_song(current_song); }
    /* Follow the game's own music (#66), which pcm still holds: it is silent while loading and at
     * the race intro, fades out between menus and races, and starts again for each race. Our songs
     * play at full level while the game's music is within 10 dB of its usual level, follow its
     * fades below that, wait (keeping their place) while it is silent, and after a loading screen
     * the next song starts with it ("music_continue": the same song goes on, #93). A loading screen
     * is a sound bank load (runtime_music_scene) and then silence; a pause or a quiet moment in a
     * race is not one, and the song goes on (#104). The level is smoothed (up in ~50 ms, down
     * over ~1 s) and only a silence of 0.25 s or more counts: taken block by block (10 ms), the
     * dips between beats and short drop-outs of the game's stream made the songs choppy (#66). */
    static double game_ref, game_env, follow;
    static size_t silent_frames;
    static int heard; /* some of the current song has played */
    static int switch_song; /* the scene changed: its own song starts after the loading screen */
    static unsigned loads_seen;
    static size_t since_silence, since_load; /* frames of the game's music after a silence / after a load */
    double game_ms=0;
    for (size_t i=0;i<(size_t)frames;++i) for (int c=0;c<2;++c) { const double v=pcm[i*(size_t)channels+c]; game_ms+=v*v; }
    const double game_rms=sqrt(game_ms/(2.0*frames));
    game_env+=fmin(1.0,frames/(48000.0*(game_rms>game_env ? 0.05 : 1.0)))*(game_rms-game_env);
    /* A song of the other scene (menu/ in a race, or the other way round) fades out with the
     * game's music when the scene changes, and one of the new scene starts when the game's music
     * comes back after the loading screen's silence, music_continue or not (#104). Changed at once,
     * the new song began while the game's music still faded out, and the silence after it then
     * moved on to a second song. */
    if (heard && menu_songs && !switch_song) {
        const long now=runtime_music_playing();
        if (now>=0 && !fits((size_t)now)) switch_song=1;
    }
    if (game_rms>1e-5) {
        if (silent_frames>=12000) since_silence=0;
        const unsigned now_loads=loads;
        /* After a loading screen: the game's music comes back after its silence, or it came back
         * just before the sound banks loaded (Quit in the pause menu, whose silence came first),
         * or it never stopped: 10 s, or 2 s when the song is of the other scene (#130: after a
         * finished race the game's music goes on, and the race song played on for 10 s). */
        if (now_loads!=loads_seen && (silent_frames>=12000 || since_silence<3*48000 || since_load>=(switch_song ? 2 : 10)*48000)) {
            loads_seen=now_loads; since_load=0;
            if (heard && (switch_song || !gow3gpu_music_continue())) { runtime_music_step(1); heard=0; }
            switch_song=0;
        } else if (now_loads!=loads_seen) {
            since_load+=(size_t)frames;
        }
        since_silence+=(size_t)frames;
        silent_frames=0;
        /* Its usual level: up within ~1 s, down over ~10 s (a fade lasts ~2 s). */
        game_ref=game_ref>0 ? game_ref+frames/(48000.0*(game_env>game_ref ? 1.0 : 10.0))*(game_env-game_ref) : game_env;
    } else {
        silent_frames+=(size_t)frames;
    }
    const double follow_to=silent_frames<12000 && game_ref>0 ? fmin(1.0,game_env/(game_ref*0.3)) : 0, follow_from=follow;
    follow=follow_to;
    if (follow_from==0 && follow_to==0) {
        memset(pcm,0,(size_t)frames*(size_t)channels*sizeof(float));
        return 1;
    }
    pthread_mutex_lock(&lock);
    size_t n=head-tail;
    if (n>(size_t)frames) n=(size_t)frames;
    if (n) heard=1;
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
        const double g=gain*(follow_from+(follow_to-follow_from)*(double)(i+1)/frames); /* ramp: no clicks */
        pcm[i*(size_t)channels]=(float)(music[i*2]*g);
        pcm[i*(size_t)channels+1]=(float)(music[i*2+1]*g);
    }
    return 1;
}
