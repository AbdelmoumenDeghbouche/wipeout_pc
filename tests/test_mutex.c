/* Dump libc writes its FreeBSD mutex flags through the returned handle. */
#undef NDEBUG
#include "../src/runtime_mutex.c"
#include <assert.h>

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }

static void mark_private(GuestMutex *mutex,int type) {
    /* The x86-64 guest pthread_mutex occupies 0x50 bytes, before host state. */
    assert(sizeof(*mutex)>=0x50+sizeof(pthread_mutex_t));
    unsigned char guest[0x50]={0};
    uint32_t expected_type=(uint32_t)type;
    memcpy(guest+0x20,&expected_type,sizeof(expected_type));
    assert(!memcmp(mutex,guest,sizeof(guest)));
    unsigned char before[sizeof(pthread_mutex_t)];
    memcpy(before,&mutex->native,sizeof(before));
    uint32_t flags;
    memcpy(&flags,(unsigned char *)mutex+0x20,sizeof(flags));
    assert((flags&0xff)==(uint32_t)type);
    flags|=0x100; /* libc's PMUTEX_FLAG_PRIVATE, at offset 0x20 */
    memcpy((unsigned char *)mutex+0x20,&flags,sizeof(flags));
    assert(!memcmp(before,&mutex->native,sizeof(before)));
}

static void recursive(void) {
    GuestAttr *attr=NULL;
    GuestMutex *mutex=NULL;
    assert(!attr_init(&attr) && !attr_type(&attr,2));
    assert(!mutex_init(&mutex,&attr,"guest-private") && !attr_destroy(&attr));
    mark_private(mutex,2);
    assert(!mutex_lock(&mutex) && !mutex_trylock(&mutex));
    assert((uint32_t)mutex_destroy(&mutex)==0x80020010);
    assert(!mutex_unlock(&mutex) && !mutex_unlock(&mutex) && !mutex_destroy(&mutex));
    assert((uint32_t)mutex_lock(&mutex)==0x80020016);
}

static GuestMutex *shared;
static GuestCond *condition;
static int total;
static int ready;
static void *worker(void *unused) {
    (void)unused;
    for (int i=0;i<10000;++i) {
        assert(!mutex_lock(&shared)); ++total; assert(!mutex_unlock(&shared));
    }
    return NULL;
}
static void *signal_ready(void *unused) {
    (void)unused;
    assert(!mutex_lock(&shared));
    ready=1;
    assert(!cond_signal(&condition) && !mutex_unlock(&shared));
    return NULL;
}

int main(void) {
    recursive();
    for (int i=0;i<256;++i) {
        GuestMutex *a=NULL,*b=NULL;
        assert(!mutex_init(&a,NULL,NULL) && !mutex_init(&b,NULL,NULL));
        mark_private(a,1); mark_private(b,1);
        assert(!mutex_timedlock(&a,1000000) && !mutex_lock(&b));
        assert(!mutex_unlock(&a) && !mutex_unlock(&b));
        assert(!mutex_destroy(&a) && !mutex_destroy(&b));
    }
    shared=(GuestMutex *)(uintptr_t)1;
    assert(!mutex_lock(&shared) && !mutex_unlock(&shared));
    mark_private(shared,1);
    pthread_t threads[8];
    for (int i=0;i<8;++i) assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for (int i=0;i<8;++i) assert(!pthread_join(threads[i],NULL));
    assert(total==80000 && !mutex_destroy(&shared));
    shared=NULL;
    assert(!mutex_init(&shared,NULL,NULL) && !cond_init(&condition,NULL,NULL));
    mark_private(shared,1);
    assert(!mutex_lock(&shared));
    pthread_t notifier;
    assert(!pthread_create(&notifier,NULL,signal_ready,NULL));
    while (!ready) assert(!cond_timedwait(&condition,&shared,5000000));
    assert(!mutex_unlock(&shared) && !pthread_join(notifier,NULL));
    assert(!cond_destroy(&condition) && !mutex_destroy(&shared));
    puts("PASS: guest mutex header, libc private flags, recursion, contention and condition wait");
    return 0;
}
