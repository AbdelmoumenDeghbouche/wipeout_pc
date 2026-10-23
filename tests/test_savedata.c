/* Real save-data calls with failures injected only at metadata I/O boundaries. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static char fault;
static FILE *metadata_stream;
static FILE *metadata_open(const char *path,const char *mode) {
    int metadata=strstr(path,"param.bin")!=NULL;
    if (metadata && ((fault=='o' && mode[0]=='w') || (fault=='i' && mode[0]=='r'))) {
        errno=EACCES; return NULL;
    }
    FILE *f=fopen(path,mode);
    if (metadata && mode[0]=='w') metadata_stream=f;
    return f;
}
static size_t metadata_write(const void *data,size_t size,size_t count,FILE *f) {
    if (f==metadata_stream && fault=='w') {
        fwrite(data,1,size*count/2,f);
        errno=ENOSPC; return 0;
    }
    return fwrite(data,size,count,f);
}
static int metadata_close(FILE *f) {
    int fail=f==metadata_stream && fault=='c';
    if (f==metadata_stream) metadata_stream=NULL;
    int r=fclose(f);
    if (fail) { errno=ENOSPC; return EOF; }
    return r;
}
#ifdef _WIN32
BOOL WINAPI metadata_replace(LPCSTR from,LPCSTR to,DWORD flags) {
    if (fault=='r') { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return MoveFileExA(from,to,flags);
}
#define MoveFileExA metadata_replace
#else
int metadata_replace(const char *from,const char *to) {
    if (fault=='r') { errno=EACCES; return -1; }
    return rename(from,to);
}
#define rename metadata_replace
#endif
#define fopen metadata_open
#define fwrite metadata_write
#define fclose metadata_close
#include "../src/runtime_savedata.c"
#undef fopen
#undef fwrite
#undef fclose
#ifdef _WIN32
#undef MoveFileExA
#else
#undef rename
#endif

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static Param stored(const char *path) {
    Param p;
    FILE *f=fopen(path,"rb"); assert(f);
    assert(fread(&p,sizeof(p),1,f)==1);
    assert(!fclose(f));
    return p;
}

int main(int argc,char **argv) {
    assert(argc==4);
    const char *mode=argv[1], selected=argv[2][0];
    runtime_file_configure(argv[3],argv[3]);
    runtime_savedata_configure("CUSA07671");
    assert(!save_initialize(NULL));
    Param old={0}, next={0};
    strcpy(old.title,"Original metadata"); old.user_param=17;
    strcpy(next.title,"Updated metadata"); next.user_param=42;
    char path[1024], pending[1030];
    DirName name={"SAVE"};
    Mount2 request={.user=1,.dir=&name,.blocks=32768,.mode=MODE_CREATE};
    MountResult result;
    int32_t status;
    if (!strcmp(mode,"set") || !strcmp(mode,"partial")) {
        assert(!save_mount2(&request,&result));
        assert(!save_set_param(&result.point,0,&old,sizeof(old)));
        snprintf(path,sizeof(path),"%s/savedata/1/CUSA07671/SAVE.sce_sys/param.bin",argv[3]);
        Param before=stored(path);
#ifndef _WIN32
        if (selected=='f') {
            snprintf(pending,sizeof(pending),"%s.part",path);
            assert(!symlink("/dev/full",pending));
        }
#endif
        fault=selected;
        status=!strcmp(mode,"partial")
            ? save_set_param(&result.point,1,next.title,strlen(next.title))
            : save_set_param(&result.point,0,&next,sizeof(next));
        fault=0;
        assert(status==(selected=='n' ? 0 : ERR_INTERNAL));
        Param after=stored(path);
        if (selected!='n') assert(!memcmp(&before,&after,sizeof(before)));
        else if (!strcmp(mode,"partial"))
            assert(!strcmp(after.title,next.title) && after.user_param==17);
        assert(!save_set_param(&result.point,0,&next,sizeof(next)));
        after=stored(path);
        assert(!strcmp(after.title,next.title) && after.user_param==42);
    } else if (!strcmp(mode,"mount")) {
        fault=selected;
        status=save_mount2(&request,&result);
        fault=0;
        assert(status==(selected=='n' ? 0 : ERR_INTERNAL));
        if (selected!='n') {
            char translated[1024];
            assert(runtime_file_translate("/savedata0",translated,sizeof(translated))!=0);
            assert(!save_mount2(&request,&result));
        }
        snprintf(path,sizeof(path),"%s/savedata/1/CUSA07671/SAVE.sce_sys/param.bin",argv[3]);
        Param after=stored(path);
        assert(!after.title[0] && after.user_param==0);
    } else {
        int existing=!strcmp(mode,"memory-existing");
        unsigned char bytes[16]={0}, wanted[16]; memset(wanted,0x51,sizeof(wanted));
        if (existing) {
            assert(!memory_setup(1,sizeof(bytes),&old));
            assert(!memory_set(1,wanted,sizeof(wanted),0));
        }
        int user=existing ? 2 : 1;
        fault=selected;
        status=memory_setup(user,sizeof(bytes),&next);
        fault=0;
        assert(status==(selected=='n' ? 0 : ERR_INTERNAL));
        if (selected!='n') {
            if (existing) {
                assert(!memory_get(1,bytes,sizeof(bytes),0));
                assert(!memcmp(bytes,wanted,sizeof(bytes)));
            } else assert(memory_get(1,bytes,sizeof(bytes),0)==ERR_MEMORY_NOT_READY);
        }
        assert(!memory_setup(user,sizeof(bytes),&next));
        snprintf(path,sizeof(path),"%s/savedata/%d/CUSA07671.memory/param.bin",argv[3],user);
        Param after=stored(path);
        assert(!strcmp(after.title,next.title) && after.user_param==42);
    }
    snprintf(pending,sizeof(pending),"%s.part",path);
    struct stat st; assert(stat(pending,&st)!=0 && errno==ENOENT);
    printf("PASS: save metadata %s / %c\n",mode,selected);
    return 0;
}
