#include "xs8_tx_replay_core.h"
#include <assert.h>
#include <stdio.h>

struct fake {
    unsigned int checks, opens, sends, releases, closes, waits;
    uint8_t bytes[4];
    int64_t time, deadlines[3];
    int deny, open_fail, fail_at, cancel_at;
    bool cancel, release_fail;
    int wait_error;
};
static int services(void *ctx) { struct fake *f=ctx; ++f->checks; if(f->deny){errno=EACCES;return -1;} return 0; }
static int open_tx(void *ctx) { struct fake *f=ctx; ++f->opens; if(f->open_fail){errno=EBUSY;return -1;} return 0; }
static int send_tx(void *ctx,uint8_t b,int64_t *start)
{
    struct fake *f=ctx; f->bytes[f->sends++]=b; *start=f->time;
    f->time+=100000000;
    if ((int)f->sends==f->cancel_at) f->cancel=true;
    if ((int)f->sends==f->fail_at) {errno=EIO;return -1;} return 0;
}
static int release_tx(void *ctx) { struct fake *f=ctx; ++f->releases; return f->release_fail?-1:0; }
static void close_tx(void *ctx) { ++((struct fake *)ctx)->closes; }
static int64_t now(void *ctx) { return ((struct fake *)ctx)->time; }
static int wait_tx(void *ctx,int64_t deadline) { struct fake *f=ctx; f->deadlines[f->waits++]=deadline; if(f->wait_error)return f->wait_error; f->time=deadline; return 0; }
static bool cancelled(void *ctx) { return ((struct fake *)ctx)->cancel; }
static int run(struct fake *f,struct replay_options o,struct replay_report *r)
{
    struct replay_ops ops={f,services,open_tx,send_tx,release_tx,close_tx,now,wait_tx,cancelled};
    return replay_run(&o,&ops,r);
}
int main(void)
{
    struct replay_report r;
    struct replay_options o={.disconnected=true};
    for (unsigned int up=0;up<2;++up) {
        struct fake f={.time=1000000000}; o.up=up;
        assert(run(&f,o,&r)==0 && f.sends==4 && f.waits==0 && f.checks==1);
        assert(f.bytes[0]==0x17 && f.bytes[1]==(up?0xF9:0xE9) && f.bytes[2]==0x09 && f.bytes[3]==0x07);
        assert(f.releases==1 && f.closes==1 && r.cleanup_ok);
        for(unsigned int i=0;i<4;++i) assert(r.frames[i].start_ns==r.frames[i].invoked_ns && r.frames[i].completed_ns==r.frames[i].start_ns+100000000);
    }
    o.gap_ms[0]=2; o.gap_ms[1]=150; o.gap_ms[2]=3;
    struct fake f={.time=1000000000};
    assert(run(&f,o,&r)==0 && f.waits==3);
    for(unsigned int i=0;i<3;++i) assert(f.deadlines[i]==r.frames[i].completed_ns+(int64_t)o.gap_ms[i]*1000000 && r.frames[i+1].start_ns==f.deadlines[i]);
    const int wait_errors[]={EINTR,ECANCELED};
    for (unsigned int i=0;i<2;++i) {
        f=(struct fake){.wait_error=wait_errors[i]};
        assert(run(&f,o,&r)==wait_errors[i] && f.sends==1 && f.releases==1 && f.closes==1);
    }
    o.gap_ms[0]=5001; f=(struct fake){0};
    assert(run(&f,o,&r)==EINVAL && f.opens==0); o.gap_ms[0]=0;
    o.disconnected=false; f=(struct fake){0};
    assert(run(&f,o,&r)==EACCES && f.checks==0 && f.opens==0);
    o.dry_run=true;
    assert(run(&f,o,&r)==0 && f.checks==0 && f.opens==0 && f.sends==0);
    o.dry_run=false; o.disconnected=true;
    f=(struct fake){.deny=1}; assert(run(&f,o,&r)==EACCES && f.opens==0);
    f=(struct fake){.open_fail=1}; assert(run(&f,o,&r)==EBUSY && f.sends==0);
    for (int at=1;at<=4;++at) {
        f=(struct fake){.fail_at=at}; assert(run(&f,o,&r)==EIO && f.sends==(unsigned int)at && f.releases==1 && f.closes==1);
        f=(struct fake){.cancel_at=at}; assert(run(&f,o,&r)==ECANCELED && f.sends==(unsigned int)at && f.releases==1 && f.closes==1);
    }
    f=(struct fake){.cancel=true}; assert(run(&f,o,&r)==ECANCELED && f.checks==0);
    f=(struct fake){.release_fail=true}; assert(run(&f,o,&r)==EIO && !r.cleanup_ok && f.closes==1);
    assert(replay_service_state("LoadState=loaded\nActiveState=inactive\n"));
    assert(replay_service_state("ActiveState=inactive\nLoadState=loaded\n"));
    const char *bad[]={"LoadState=loaded\nActiveState=active\n","LoadState=not-found\nActiveState=inactive\n","","ActiveState=inactive\n","LoadState=loaded\nActiveState=failed\n","LoadState=loaded\nActiveState=inactive\nX=1\n"};
    for(unsigned int i=0;i<sizeof(bad)/sizeof(bad[0]);++i) assert(!replay_service_state(bad[i]));
    puts("TX replay sequence, bounded timing, service guards, dry-run and cleanup tests passed");
    return 0;
}
