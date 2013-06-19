#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "ppcomm.h"
#include "ppfile.h"
#include "datapack.h"

void sendpacket(int sockfd,ppacket* p){
  int i;
  int len = 0;

  while(p->bytesleft){
    i = write(sockfd,p->startptr,p->bytesleft);
    if(i < 0){
      if(i != -EAGAIN){
        fprintf(stderr,"write error:%d\n",i);
        exit(1);
      }
      continue;
    }
    if(i==0){
      fprintf(stderr,"Connection closed\n");
      exit(1);
    }

    len += i;
    p->startptr += i;
    p->bytesleft -= i;
  }

  printf("%d bytes sent\n",len);
  printf("size=%d,cmd=%X,id=%d\n",p->size,p->cmd,p->id);
}

ppacket* receivepacket(int sockfd){
  char headbuf[20];
  uint8_t* startptr;
  const uint8_t* ptr;
  int i,hleft;
  ppacket* p;

  hleft = HEADER_LEN;
  startptr = headbuf;
  while(hleft){
    i = read(sockfd,startptr,hleft);
    if(i < 0){
      if(i != -EAGAIN){
        fprintf(stderr,"read error:%d\n",i);
        exit(1);
      }
      continue;
    }
    if(i==0){
      fprintf(stderr,"Connection closed\n");
      exit(1);
    }

    hleft -= i;
    startptr += i;
  }

  int size,cmd,id;
  ptr = headbuf;
  size = get32bit(&ptr);
  cmd = get32bit(&ptr);
  id = get32bit(&ptr);

  printf("got packet:size=%d,cmd=%X,id=%d\n",size,cmd,id);
  p = createpacket_r(size,cmd,id);
  
  while(p->bytesleft){
    i = read(sockfd,p->startptr,p->bytesleft);
    if(i < 0){
      if(i != -EAGAIN){
        fprintf(stderr,"read error:%d\n",i);
        exit(1);
      }
      continue;
    }
    if(i==0){
      fprintf(stderr,"Connection closed\n");
      exit(1);
    }

    p->bytesleft -= i;
    p->startptr += i;
  }

  p->startptr = p->buf;

  return p;
}

void print_attr(const attr* a){
  if(a->mode & S_IFDIR){
    printf("\tdirectory\n");
  } else if(a->mode & S_IFREG){
    printf("\tregular file\n");
  }


  printf("\tperm:%d%d%d\n",a->mode / 0100 & 7,
                           a->mode / 0010 & 7,
                           a->mode & 7);

  printf("\tuid=%d,gid=%d",a->uid,a->gid);
  printf("\tatime=%d,ctime=%d,mtime=%d\n",a->atime,a->ctime,a->mtime);
  printf("\tlink=%d\n",a->link);
  printf("\tsize=%d\n",a->size);
}

static void send_and_receive(int sockfd,int ip,char* path,int cmd){
  ppacket* p = createpacket_s(4+strlen(path),cmd,ip);
  uint8_t* ptr = p->startptr + HEADER_LEN;
  put32bit(&ptr,strlen(path));
  memcpy(ptr,path,strlen(path));
  sendpacket(sockfd,p);
  free(p);

  p = receivepacket(sockfd);

  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);
  printf("status:%d\n",status);

  free(p);
}

static void chxxx(int sockfd,int ip,char* path,int cmd,int opt){
  int len = strlen(path);
  ppacket* p = createpacket_s(8+strlen(path),cmd,ip);
  uint8_t* ptr = p->startptr + HEADER_LEN;
  put32bit(&ptr,len);
  memcpy(ptr,path,len);
  ptr += len;
  put32bit(&ptr,opt);

  sendpacket(sockfd,p);
  free(p);

  p = receivepacket(sockfd);

  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);
  printf("status:%d\n",status);

  free(p);
}

static char* hexdigit = "0123456789ABCDEF";

int main(void){
  int sockfd = socket(AF_INET,SOCK_STREAM,0);
  int i;
  struct sockaddr_in servaddr;
  char path[100],cmd[100],buf[200];
  uint32_t csip = 0;
  uint64_t cid = -1;

  if (tcpnodelay(sockfd)<0) {
    fprintf(stderr,"can't set TCP_NODELAY\n");
  }

  memset(&servaddr,0,sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET,"127.0.0.1",&servaddr.sin_addr);
  servaddr.sin_port = htons(MDS_PORT);
  if(connect(sockfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) != 0){
    perror("cannot connect to mds");
    exit(1);
  }
  printf("connected to 127.0.0.1:%d\n",MDS_PORT);

  printf(">>>");
  while(scanf("%s%s",cmd,path)!=EOF){
    if(!strcmp(cmd,"getattr")){
      ppacket* p = createpacket_s(4+strlen(path),CLTOMD_GETATTR,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,strlen(path));
      memcpy(ptr,path,strlen(path));
      sendpacket(sockfd,p);
      free(p);

      p = receivepacket(sockfd);

      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        print_attr((const attr*)ptr2);
      } else {
        if(status == -ENOENT){
          printf("\tENOENT\n");
        }
        if(status == -ENOTDIR){
          printf("\tENOTDIR\n");
        }
      }

      free(p);
    }
    if(!strcmp(cmd,"readdir")){
      ppacket* p = createpacket_s(4+strlen(path),CLTOMD_READDIR,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,strlen(path));
      memcpy(ptr,path,strlen(path));
      sendpacket(sockfd,p);
      free(p);

      p = receivepacket(sockfd);

      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        int n = get32bit(&ptr2);
        printf("%d files:\n",n);
        for(i=0;i<n;i++){
          int len = get32bit(&ptr2);
          memcpy(buf,ptr2,len);
          ptr2 += len;

          buf[len] = 0;
          printf("\t%s\n",buf);
        }
      } else {
        if(status == -ENOENT){
          printf("\tENOENT\n");
        }
        if(status == -ENOTDIR){
          printf("\tENOTDIR\n");
        }
      }

      free(p);
    }
    if(!strcmp(cmd,"chmod")){
      int perm;
      char perms[10];
      scanf("%s",perms);
      perm = (perms[0]-'0')*64 + (perms[1]-'0')*8 + (perms[2]-'0');

      printf("chmod %s %o\n",path,perm);
      chxxx(sockfd,-1,path,CLTOMD_CHMOD,perm);
    }
    if(!strcmp(cmd,"chgrp")){
      int gid;
      scanf("%d",&gid);
      chxxx(sockfd,-1,path,CLTOMD_CHGRP,gid);
    }
    if(!strcmp(cmd,"chown")){
      int uid;
      scanf("%d",&uid);
      chxxx(sockfd,-1,path,CLTOMD_CHOWN,uid);
    }
    if(!strcmp(cmd,"create")){
      send_and_receive(sockfd,-1,path,CLTOMD_CREATE);
    }
    if(!strcmp(cmd,"open")){
      send_and_receive(sockfd,-1,path,CLTOMD_OPEN);
    }
    if(!strcmp(cmd,"read_chunk_info")){
      ppacket* p = createpacket_s(4+strlen(path),CLTOMD_READ_CHUNK_INFO,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,strlen(path));
      memcpy(ptr,path,strlen(path));
      sendpacket(sockfd,p);
      free(p);

      p = receivepacket(sockfd);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        uint32_t ip = get32bit(&ptr2);
        if(ip == -1){
          printf("local mds\n");
        } else {
          printf("remote mds:%X\n",ip);
        }

        int chunks = get32bit(&ptr2);
        printf("chunks=%d\n",chunks);
        int i;
        for(i=0;i<chunks;i++){
          uint64_t chunkid = get64bit(&ptr2);
          printf("(%d):id=%lld\n",i,chunkid);
        }
      }
    }
    if(!strcmp(cmd,"append_chunk")){
      ppacket* p = createpacket_s(4+strlen(path),CLTOMD_APPEND_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,strlen(path));
      memcpy(ptr,path,strlen(path));
      sendpacket(sockfd,p);
      free(p);

      p = receivepacket(sockfd);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        uint64_t chunkid = get64bit(&ptr2);
        printf("chunkid=%lld\n",chunkid);
      }
    }
    if(!strcmp(cmd,"lookup_chunk")){
      uint64_t chunkid = atoi(path);

      ppacket* p = createpacket_s(8,CLTOMD_LOOKUP_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,chunkid);
      sendpacket(sockfd,p);
      free(p);

      p = receivepacket(sockfd);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        csip = get32bit(&ptr2);
        cid = chunkid;
        printf("csip:%X\n",csip);
      }
    }
    if(!strcmp(cmd,"pop_chunk")){
      ppacket* p = createpacket_s(4+strlen(path),CLTOMD_POP_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,strlen(path));
      memcpy(ptr,path,strlen(path));
      sendpacket(sockfd,p);
      free(p);

      p = receivepacket(sockfd);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        uint64_t chunkid = get64bit(&ptr2);
        printf("chunkid=%lld\n",chunkid);
      }
    }
    if(!strcmp(cmd,"read_chunk")){
      int offset,len;

      offset = atoi(path);
      scanf("%d",&len);

      if(csip == 0 || cid == -1)
        goto repeat;

      int cssock = tcpsocket();
      if (tcpnodelay(cssock)<0) {
        fprintf(stderr,"can't set TCP_NODELAY\n");
      }

      if(tcpnumconnect(cssock,csip,CS_PORT) < 0){
        fprintf(stderr,"can't connect to csip:%X\n",csip);
        tcpclose(cssock);
        goto repeat;
      }

      ppacket* p = createpacket_s(8+4+4,CLTOCS_READ_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,cid);
      put32bit(&ptr,offset);
      put32bit(&ptr,len);

      sendpacket(cssock,p);
      free(p);

      p = receivepacket(cssock);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status=%d\n",status);
      if(status == 0){
        int rlen =  get32bit(&ptr2);
        printf("rlen=%d\n",rlen);

        if(rlen > 0){
          int i;
          for(i=0;i<rlen;i++){
            uint8_t c = get8bit(&ptr2);
            printf("%c%c ",hexdigit[c/16],hexdigit[c % 16]);
          }
          printf("\n");
        }
      }

      tcpclose(cssock);
    }
    if(!strcmp(cmd,"write_chunk")){
      int offset;
      uint8_t* wbuf;
      offset = atoi(path);
      scanf("%s",path);
      wbuf = path;
      int len = strlen(wbuf);

      if(csip == 0 || cid == -1)
        goto repeat;

      int cssock = tcpsocket();
      if (tcpnodelay(cssock)<0) {
        fprintf(stderr,"can't set TCP_NODELAY\n");
      }

      if(tcpnumconnect(cssock,csip,CS_PORT) < 0){
        fprintf(stderr,"can't connect to csip:%X\n",csip);
        tcpclose(cssock);
        goto repeat;
      }

      ppacket* p = createpacket_s(8+4+4+len,CLTOCS_WRITE_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,cid);
      put32bit(&ptr,offset);
      put32bit(&ptr,len);
      memcpy(ptr,wbuf,len);

      sendpacket(cssock,p);
      free(p);

      p = receivepacket(cssock);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status=%d\n",status);
      if(status == 0){
        int wlen =  get32bit(&ptr2);
        printf("wlen=%d\n",wlen);
      }

      tcpclose(cssock);
    }

repeat:
    printf(">>>");
  }

  close(sockfd);
  return 0;
}
