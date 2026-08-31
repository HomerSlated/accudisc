/* mmcquant — map the drive's read-speed QUANTISER without any timed read.
 * SET CD SPEED with a requested rate, then read back mode page 2A's "current
 * read speed" field, which reports the request AS ACCEPTED (i.e. quantised).
 * Read-only apart from SET CD SPEED. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
static int fd, st;
static unsigned char sn[64];
static int cmd(const unsigned char*c,int l,unsigned char*b,int n){
    sg_io_hdr_t io; memset(&io,0,sizeof io); memset(sn,0,sizeof sn);
    io.interface_id='S'; io.cmd_len=l; io.cmdp=(unsigned char*)c;
    io.sbp=sn; io.mx_sb_len=sizeof sn;
    if(n>0){io.dxfer_direction=SG_DXFER_FROM_DEV;io.dxferp=b;io.dxfer_len=n;}
    else io.dxfer_direction=SG_DXFER_NONE;
    io.timeout=20000;
    if(ioctl(fd,SG_IO,&io)<0) return -1;
    st=io.status; return n-io.resid;
}
int main(int argc,char**argv){
    fd=open(argc>1?argv[1]:"/dev/sg3",O_RDWR|O_NONBLOCK);
    if(fd<0){perror("open");return 2;}
    printf("%10s %8s   %10s %8s\n","req kB/s","req x","accepted","accept x");
    int prev=-1;
    for(int k=100;k<=8600;k+=100){
        unsigned char sc[12]={0xbb,0,(k>>8)&0xff,k&0xff,0xff,0xff,0,0,0,0,0,0};
        cmd(sc,12,NULL,0);
        unsigned char b[256];
        unsigned char ms[10]={0x5a,0,0x2a,0,0,0,0,0,254,0};
        int n=cmd(ms,10,b,254);
        if(n<24||st){printf("%10d   (mode sense failed)\n",k);continue;}
        int cur=(b[22]<<8)|b[23];
        if(cur!=prev){
            printf("%10d %7.1fx   %10d %7.1fx   <== step\n",k,k/176.4,cur,cur/176.4);
            prev=cur;
        }
    }
    unsigned char sc[12]={0xbb,0,0xff,0xff,0xff,0xff,0,0,0,0,0,0};
    cmd(sc,12,NULL,0);
    close(fd); return 0;
}
