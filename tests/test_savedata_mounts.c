/* Save mounts use the real shared file table, including capacity failures. */
#undef NDEBUG
#include "../src/runtime_savedata.c"
#include <assert.h>

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static int32_t mount_save(int index,uint32_t mode,MountResult *result) {
    DirName name;
    snprintf(name.data,sizeof(name.data),"SAVE%02d",index);
    Mount2 request={.user=1,.dir=&name,.blocks=32768,.mode=mode};
    return save_mount2(&request,result);
}
static void verify_save(const MountResult *result,int index,int existing) {
    char actual[1024],expected[1024],guest[64];
    snprintf(expected,sizeof(expected),"%s/savedata/1/CUSA07671/SAVE%02d",runtime_file_user_dir(),index);
    assert(!runtime_file_translate(result->point.data,actual,sizeof(actual)));
    assert(!strcmp(actual,expected));
    snprintf(guest,sizeof(guest),"%s/data.bin",result->point.data);
    unsigned char wanted=(unsigned char)(index+1),got=0;
    int fd;
    if (!existing) {
        fd=(int)runtime_file_open(guest,0x202,0644); assert(fd>=3);
        assert(runtime_file_write(fd,&wanted,1)==1 && !runtime_file_close(fd));
    }
    fd=(int)runtime_file_open(guest,0,0); assert(fd>=3);
    assert(runtime_file_read(fd,&got,1)==1 && !runtime_file_close(fd));
    assert(got==wanted);
}
int main(int argc,char **argv) {
    assert(argc==3);
    const char *mode=argv[1];
    runtime_file_configure(argv[2],argv[2]);
    runtime_savedata_configure("CUSA07671");
    assert(!save_initialize(NULL));
    MountResult saves[16],result;
    char actual[1024];
    if (!strcmp(mode,"default") || !strcmp(mode,"custom")) {
        if (!strcmp(mode,"custom")) {
            for (int i=0;i<8;++i) {
                char guest[64]; snprintf(guest,sizeof(guest),"/app0/asset-%d",i);
                assert(!runtime_file_mount(guest,argv[2]));
            }
            assert(!runtime_file_mount("/app0/EXEC/WAD/PS4_LE/R_UI.WAD",argv[2]));
        }
        for (int i=0;i<16;++i) {
            assert(!mount_save(i,MODE_CREATE,&saves[i]));
            verify_save(&saves[i],i,0);
        }
        assert(mount_save(16,MODE_CREATE,&result)==ERR_MOUNT_FULL);
        assert(!save_umount(&saves[7].point));
        assert(runtime_file_translate(saves[7].point.data,actual,sizeof(actual))==ENOENT);
        assert(!mount_save(16,MODE_CREATE,&result));
        assert(!strcmp(result.point.data,saves[7].point.data));
        verify_save(&result,16,0);
        for (int i=0;i<16;++i) if (i!=7) verify_save(&saves[i],i,1);
        if (!strcmp(mode,"custom")) {
            assert(!runtime_file_translate("/app0/asset-0",actual,sizeof(actual)));
            assert(!strcmp(actual,argv[2]));
        }
    } else if (!strcmp(mode,"init-failure")) {
        char blocker[1024]; snprintf(blocker,sizeof(blocker),"%s/savedata",argv[2]);
        FILE *f=fopen(blocker,"wb"); assert(f);
        assert(fputs("keep",f)>=0 && !fclose(f));
        assert(mount_save(0,MODE_CREATE,&result)==ERR_INTERNAL);
        assert(runtime_file_translate("/savedata0",actual,sizeof(actual))==ENOENT);
        f=fopen(blocker,"rb"); assert(f);
        char bytes[4]; assert(fread(bytes,1,sizeof(bytes),f)==sizeof(bytes));
        assert(!memcmp(bytes,"keep",4) && !fclose(f) && !unlink(blocker));
        assert(!mount_save(0,MODE_CREATE,&result));
        verify_save(&result,0,0);
    } else {
        int existing=!strcmp(mode,"full-existing");
        if (existing) {
            assert(!mount_save(0,MODE_CREATE,&result));
            verify_save(&result,0,0);
            assert(!save_umount(&result.point));
        }
        int fillers=0;
        for (;fillers<128;++fillers) {
            char guest[64]; snprintf(guest,sizeof(guest),"/test-fill-%d",fillers);
            if (runtime_file_mount(guest,argv[2])) break;
        }
        assert(fillers>0 && fillers<128);
        memset(&result,0x5a,sizeof(result));
        MountResult before=result;
        assert(mount_save(0,existing ? 0 : MODE_CREATE,&result)==ERR_INTERNAL);
        assert(!memcmp(&result,&before,sizeof(result)));
        assert(runtime_file_translate("/savedata0",actual,sizeof(actual))==ENOENT);
        if (!existing) {
            char host[1024]; struct stat st;
            snprintf(host,sizeof(host),"%s/savedata/1/CUSA07671/SAVE00",argv[2]);
            assert(stat(host,&st)!=0 && errno==ENOENT);
        }
        char released[64]; snprintf(released,sizeof(released),"/test-fill-%d",fillers-1);
        runtime_file_unmount(released);
        assert(!mount_save(0,existing ? 0 : MODE_CREATE,&result));
        assert(result.status==(existing ? 0u : 1u));
        verify_save(&result,0,existing);
        assert(!save_umount(&result.point));
        assert(runtime_file_translate(result.point.data,actual,sizeof(actual))==ENOENT);
        assert(!runtime_file_translate("/test-fill-0",actual,sizeof(actual)));
        assert(!strcmp(actual,argv[2]));
    }
    puts("PASS: save mount capacity, error handling and retry");
    return 0;
}
