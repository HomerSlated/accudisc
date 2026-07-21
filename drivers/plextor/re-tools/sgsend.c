/* sgsend — scratch RE instrument (vendor-driver zone; not built or shipped).
 * Sends an arbitrary 12-byte CDB via SG_IO to a /dev/sg* node for vendor-opcode
 * probing. READ (data-in) by default; --out / --pl <hex...> for data-out SET.
 * Open O_RDWR on the sg char node grants full command access (vendor opcodes).
 *   build:  gcc -O2 -Wall -o sgsend sgsend.c
 *   e.g. GET SpeedRead:  ./sgsend --dev /dev/sg3 e9 00 bb 00 00 00 00 00 00 00 08 00 --in 8
 *        SET SpeedRead ON: ./sgsend --dev /dev/sg3 e9 10 bb 01 00 00 00 00 00 00 08 00 --in 8
 * Used to live-verify the Plextor feature map in FEATURES.md / PROTOCOL.md. */
/* scratch RE tool: send an arbitrary 12-byte CDB via SG_IO.
 * READ (data-in) by default; --out makes it DATA_OUT with hex payload.
 * NOT part of accudisc; RE instrumentation only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

static int hexbyte(const char*s){return (int)strtol(s,0,16);}

int main(int argc,char**argv){
    const char*dev="/dev/sr0";
    unsigned char cdb[16]={0}; int cdblen=0;
    unsigned char dxfer[512]; int dxlen=0; int dir_out=0;
    unsigned char outbuf[512]={0}; int outlen=0;
    int ai=1;
    for(;ai<argc;ai++){
        if(!strcmp(argv[ai],"--dev")){dev=argv[++ai];}
        else if(!strcmp(argv[ai],"--in")){dxlen=atoi(argv[++ai]);}
        else if(!strcmp(argv[ai],"--out")){dir_out=1;
            /* remaining args after --out ... but we take payload via --pl */}
        else if(!strcmp(argv[ai],"--pl")){ /* hex payload bytes follow until next -- */
            while(ai+1<argc && strncmp(argv[ai+1],"--",2)) outbuf[outlen++]=hexbyte(argv[++ai]);
            dir_out=1;}
        else if(!strncmp(argv[ai],"--",2)){fprintf(stderr,"unknown %s\n",argv[ai]);return 2;}
        else cdb[cdblen++]=hexbyte(argv[ai]);
    }
    int fd=open(dev,O_RDWR|O_NONBLOCK);
    if(fd<0) fd=open(dev,O_RDONLY|O_NONBLOCK);
    if(fd<0){perror("open");return 2;}
    sg_io_hdr_t io; memset(&io,0,sizeof io);
    io.interface_id='S';
    io.cmd_len=cdblen; io.cmdp=cdb;
    unsigned char sense[64]; io.sbp=sense; io.mx_sb_len=sizeof sense;
    if(dir_out){ io.dxfer_direction=SG_DXFER_TO_DEV; io.dxferp=outbuf; io.dxfer_len=outlen; }
    else if(dxlen>0){ io.dxfer_direction=SG_DXFER_FROM_DEV; io.dxferp=dxfer; io.dxfer_len=dxlen; }
    else io.dxfer_direction=SG_DXFER_NONE;
    io.timeout=20000;
    int rc=ioctl(fd,SG_IO,&io);
    printf("CDB:"); for(int i=0;i<cdblen;i++)printf(" %02x",cdb[i]); printf("\n");
    if(rc<0){perror("SG_IO");close(fd);return 2;}
    printf("status=%02x host=%02x driver=%02x resid=%d sense_len=%d\n",
        io.status,io.host_status,io.driver_status,io.resid,io.sb_len_wr);
    if(io.sb_len_wr){printf("SENSE:"); for(int i=0;i<io.sb_len_wr;i++)printf(" %02x",sense[i]); printf("\n");}
    if(!dir_out && dxlen>0){
        int got=dxlen-io.resid;
        printf("DATA(%d):",got);
        for(int i=0;i<got;i++){ if(i%16==0)printf("\n  %04x:",i); printf(" %02x",dxfer[i]); }
        printf("\n");
    }
    close(fd);
    return io.status?1:0;
}
