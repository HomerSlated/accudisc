/* eedump — dump the PX-716A EEPROM via the documented read-only 0xF1 command.
 * CDB form is pxfw's PX-716 variant: F1 01 .. cmd[7]=block, cmd[8:9]=size. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
static int fd,st; static unsigned char sn[64];
static int rd(int idx,unsigned char*b,int n){
    unsigned char c[12]={0xf1,0x01,0,0,0,0,0,(unsigned char)idx,
                         (unsigned char)((n>>8)&0xff),(unsigned char)(n&0xff),0,0};
    sg_io_hdr_t io; memset(&io,0,sizeof io); memset(sn,0,sizeof sn);
    io.interface_id='S'; io.cmd_len=12; io.cmdp=c;
    io.sbp=sn; io.mx_sb_len=sizeof sn;
    io.dxfer_direction=SG_DXFER_FROM_DEV; io.dxferp=b; io.dxfer_len=n;
    io.timeout=20000;
    if(ioctl(fd,SG_IO,&io)<0) return -1;
    st=io.status; return n-io.resid;
}
int main(int argc,char**argv){
    const char*dev=argc>1?argv[1]:"/dev/sg3";
    const char*out=argc>2?argv[2]:"/var/tmp/px716a_eeprom.bin";
    fd=open(dev,O_RDWR|O_NONBLOCK); if(fd<0){perror("open");return 2;}
    FILE*f=fopen(out,"wb"); if(!f){perror("fopen");return 2;}
    unsigned char b[256]; int blocks=0;
    for(int i=0;i<64;i++){
        int n=rd(i,b,256);
        if(n!=256||st){ printf("block %d: stop (n=%d status=%02x)\n",i,n,st); break; }
        fwrite(b,1,256,f); blocks++;
    }
    fclose(f); close(fd);
    printf("wrote %d blocks (%d bytes) to %s\n",blocks,blocks*256,out);
    return 0;
}
