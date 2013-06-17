#include "mis.h"
#include "datapack.h"
#include "mis_fs.h"

misserventry* misservhead = NULL;

int lsock;
int lsockpdescpos;

ppfile* root;

void mis_fs_demo_init(void){
  attr a;
  int i;

  a.uid = a.gid = 0;
  a.atime = a.ctime = a.mtime = time(NULL);
  a.link = 1;
  a.size = 1;

  a.mode = 0777 | S_IFDIR;
  root = new_file("/",a);
  add_file(root);
}

int mis_init(void){
  lsock = tcpsocket();
  if (lsock<0) {
    mfs_errlog(LOG_ERR,"main master server module: can't create socket");
    return -1;
  }
  tcpnonblock(lsock);
  tcpnodelay(lsock);
  tcpreuseaddr(lsock);

  lsockpdescpos = -1;

	if (tcpsetacceptfilter(lsock)<0 && errno!=ENOTSUP) {
		mfs_errlog_silent(LOG_NOTICE,"main master server module: can't set accept filter");
	}
	if (tcpstrlisten(lsock,"*",MIS_PORT_STR,100)<0) {
		mfs_errlog(LOG_ERR,"main master server module: can't listen on socket");
		return -1;
	}
  
  fprintf(stderr,"listening on port %s\n",MIS_PORT_STR);

	main_destructregister(mis_term);
	main_pollregister(mis_desc,mis_serve);

  mis_fs_demo_init();

  return 0;
}

void mis_serve(struct pollfd *pdesc) {
	misserventry *eptr;

	if (lsockpdescpos >=0 && (pdesc[lsockpdescpos].revents & POLLIN)) {
		int ns=tcpaccept(lsock);
		if (ns<0) {
			mfs_errlog_silent(LOG_NOTICE,"main master server module: accept error");
		} else {
			tcpnonblock(ns);
			tcpnodelay(ns);
			eptr = malloc(sizeof(misserventry));
			passert(eptr);

			eptr->next = misservhead;
			misservhead = eptr;

			eptr->sock = ns;
			eptr->pdescpos = -1;

			tcpgetpeer(ns,&(eptr->peerip),NULL);
			eptr->mode = HEADER;

      eptr->inpacket = NULL;
      eptr->outpacket = NULL;
      eptr->bytesleft = HEADER_LEN;
      eptr->startptr = eptr->headbuf;

      fprintf(stderr,"mds(ip:%u.%u.%u.%u) connected\n",(eptr->peerip>>24)&0xFF,(eptr->peerip>>16)&0xFF,(eptr->peerip>>8)&0xFF,eptr->peerip&0xFF);
		}
	}

// read
	for (eptr=misservhead ; eptr ; eptr=eptr->next) {
		if (eptr->pdescpos>=0) {
			if (pdesc[eptr->pdescpos].revents & (POLLERR|POLLHUP)) {
				eptr->mode = KILL;
			}
			if ((pdesc[eptr->pdescpos].revents & POLLIN) && eptr->mode!=KILL) {
				mis_read(eptr);
			}
		}
	}

// write
	for (eptr=misservhead ; eptr ; eptr=eptr->next) {
		if (eptr->pdescpos>=0) {
			if ((((pdesc[eptr->pdescpos].events & POLLOUT)==0 && (eptr->outpacket!=NULL)) || (pdesc[eptr->pdescpos].revents & POLLOUT)) && eptr->mode!=KILL) {
				mis_write(eptr);
			}
		}
	}

	misserventry** kptr = &misservhead;
	while ((eptr=*kptr)) {
		if (eptr->mode == KILL) {
			tcpclose(eptr->sock);
			*kptr = eptr->next;
			free(eptr);
		} else {
			kptr = &(eptr->next);
		}
	}
}

void mis_desc(struct pollfd *pdesc,uint32_t *ndesc) {
	uint32_t pos = *ndesc;
	misserventry *eptr;

  pdesc[pos].fd = lsock;
  pdesc[pos].events = POLLIN;
  lsockpdescpos = pos;
  pos++;

	for(eptr=misservhead ; eptr ; eptr=eptr->next){
		pdesc[pos].fd = eptr->sock;
		pdesc[pos].events = 0;
		eptr->pdescpos = pos;

		pdesc[pos].events |= POLLIN;
		if (eptr->outpacket != NULL) {
			pdesc[pos].events |= POLLOUT;
		}
		pos++;
	}
	*ndesc = pos;
}

void mis_term(void) {
	misserventry *eptr,*eptrn;

	fprintf(stderr,"main master server module: closing %s:%s\n","*",MIS_PORT_STR);
	tcpclose(lsock);

	for (eptr = misservhead ; eptr ; eptr = eptrn) {
    ppacket *pp,*ppn;

		eptrn = eptr->next;

    for( pp = eptr->inpacket; pp; pp = ppn){
      ppn = pp->next;
      free(pp);
    }

    for( pp = eptr->outpacket; pp; pp = ppn){
      ppn = pp->next;
      free(pp);
    }

		free(eptr);
	}
}

void mis_read(misserventry *eptr) {
	int i;
  int size,cmd,id;

	while(1){
    if(eptr->mode == HEADER){
		  i=read(eptr->sock,eptr->startptr,eptr->bytesleft);
    } else {
      i=read(eptr->sock,eptr->inpacket->startptr,eptr->inpacket->bytesleft);
    }

		if (i==0) {
      fprintf(stderr,"connection with client(ip:%u.%u.%u.%u) has been closed by peer\n",(eptr->peerip>>24)&0xFF,(eptr->peerip>>16)&0xFF,(eptr->peerip>>8)&0xFF,eptr->peerip&0xFF);
			eptr->mode = KILL;
			return;
		}

		if (i<0) {
			if (errno!=EAGAIN) {
				eptr->mode = KILL;
			}
			return;
		}

    //debug
    fprintf(stderr,"read %d from (ip:%u.%u.%u.%u)\n",i,(eptr->peerip>>24)&0xFF,(eptr->peerip>>16)&0xFF,(eptr->peerip>>8)&0xFF,eptr->peerip&0xFF);

    if(eptr->mode == HEADER){
      eptr->bytesleft -= i;
      eptr->startptr += i;
      if(eptr->bytesleft > 0) return;

      const uint8_t *pptr = eptr->headbuf;
      size = get32bit(&pptr);
      cmd = get32bit(&pptr);
      id = get32bit(&pptr);
      fprintf(stderr,"got packet header,size=%d,cmd=%X,id=%d\n",size,cmd,id);

      ppacket* inp = createpacket_r(size,cmd,id);
      inp->next = eptr->inpacket;
      eptr->inpacket = inp;

      eptr->mode = DATA;

      continue;
    } else {
      eptr->inpacket->bytesleft -= i;
      eptr->inpacket->startptr += i;

      if(eptr->inpacket->bytesleft > 0) return;

      eptr->inpacket->startptr = eptr->inpacket->buf;

      fprintf(stderr,"packet received,size=%d,cmd=%X\n",eptr->inpacket->size,eptr->inpacket->cmd);

      mis_gotpacket(eptr,eptr->inpacket);
      ppacket* p = eptr->inpacket;
      eptr->inpacket = eptr->inpacket->next;
      free(p);

      if(eptr->inpacket == NULL){
        eptr->mode = HEADER;
        eptr->startptr = eptr->headbuf;
        eptr->bytesleft = HEADER_LEN;
      }

      return;
    }

	}
}

void mis_write(misserventry *eptr) {
	int32_t i;

  while(eptr->outpacket){
		i=write(eptr->sock,eptr->outpacket->startptr,eptr->outpacket->bytesleft);

		if (i<0) {
			if (errno!=EAGAIN) {
				mfs_arg_errlog_silent(LOG_NOTICE,"main master server module: (ip:%u.%u.%u.%u) write error",(eptr->peerip>>24)&0xFF,(eptr->peerip>>16)&0xFF,(eptr->peerip>>8)&0xFF,eptr->peerip&0xFF);
				eptr->mode = KILL;
			}
			return;
		}

    //debug
    fprintf(stderr,"wrote %d from (ip:%u.%u.%u.%u)\n",i,(eptr->peerip>>24)&0xFF,(eptr->peerip>>16)&0xFF,(eptr->peerip>>8)&0xFF,eptr->peerip&0xFF);

		eptr->outpacket->startptr += i;
		eptr->outpacket->bytesleft -=i;

    if(eptr->outpacket->bytesleft > 0) return;

    ppacket* p = eptr->outpacket;
    eptr->outpacket = eptr->outpacket->next;
    free(p);
	}
}

void mis_gotpacket(misserventry* eptr,ppacket* p){
  fprintf(stderr,"dispatching packet ,size:%d,cmd:%X\n",p->size,p->cmd);
  switch(p->cmd){
    case MDTOMI_GETATTR:
      mis_getattr(eptr,p);
      break;
    case MDTOMI_READDIR:
      mis_readdir(eptr,p);
      break;
    case MDTOMI_CREATE:
      mis_create(eptr,p);
      break;
    case MDTOMI_ACCESS:
      mis_access(eptr,p);
      break;
    case MDTOMI_OPEN:
      mis_open(eptr,p);
      break;
    case MDTOMI_UPDATE_ATTR:
      mis_update_attr(eptr,p);
      break;
    case MDTOMI_CHMOD:
      mis_chmod(eptr,p);
      break;
    case MDTOMI_CHGRP:
      mis_chgrp(eptr,p);
      break;
    case MDTOMI_CHOWN:
      mis_chown(eptr,p);
      break;
  }

  fprintf(stderr,"\n\n");
}

void mis_getattr(misserventry* eptr,ppacket* inp){
  fprintf(stderr,"+mis_getattr\n");

  char* path;
  int len;
  int i;
  ppacket* p;
  const uint8_t* inptr;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%X\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(f == NULL){
    p = createpacket_s(4,MITOMD_GETATTR,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    p = createpacket_s(4+sizeof(attr),MITOMD_GETATTR,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);
    memcpy(ptr,&f->a,sizeof(attr));
  }

  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

void mis_readdir(misserventry* eptr,ppacket* inp){
  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(f == NULL){
    p = createpacket_s(4,MITOMD_READDIR,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    if(S_ISDIR(f->a.mode)){
      ppfile* cf;
      int totsize = 8;
      int nfiles = 0;

      for(cf = f->child;cf;cf = cf->next){
        totsize += 4 + strlen(cf->name);
        nfiles++;
      }

      p = createpacket_s(totsize,MITOMD_READDIR,inp->id);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,0);
      put32bit(&ptr,nfiles);

      for(cf = f->child;cf;cf = cf->next){
        int len = strlen(cf->name);

        put32bit(&ptr,len);
        memcpy(ptr,cf->name,len);
        ptr += len;
      }
    } else {
      p = createpacket_s(4,MITOMD_READDIR,inp->id);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,-ENOTDIR);
    }
  }

  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

void mis_create(misserventry* eptr,ppacket* inp){
  fprintf(stderr,"+mis_create\n");

  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(f){
    fprintf(stderr,"file exists\n");

    p = createpacket_s(4,MITOMD_CREATE,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-EEXIST);

    goto end;
  } else {
    char* dir;
    if(len > 1){
      dir = &path[len-1];
      while(dir >= path && *dir != '/') dir--;

      int dirlen = dir - path+1;
      dir = strdup(path);
      dir[dirlen] = 0;
    } else {
      dir = strdup("/");
    }


    printf("dir=%s\n",dir);
    f = lookup_file(dir);
    if(!f){
      p = createpacket_s(4,MITOMD_CREATE,inp->id);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put32bit(&ptr,-ENOENT);
      
      free(dir);
      goto end;
    } else {
      if(!S_ISDIR(f->a.mode)){
        p = createpacket_s(4,MITOMD_CREATE,inp->id);
        uint8_t* ptr = p->startptr + HEADER_LEN;
        put32bit(&ptr,-ENOTDIR);

        free(dir);
        goto end;
      }
    }

    attr a;

    a.uid = a.gid = 0;
    a.atime = a.ctime = a.mtime = time(NULL);
    a.link = 1;
    a.size = 0;

    a.mode = 0777 | S_IFREG;

    ppfile* nf = new_file(path,a);
    add_file(nf);
    nf->next = f->child;
    f->child = nf;

    nf->srcip = eptr->peerip;

    p = createpacket_s(4,MITOMD_CREATE,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);

    free(dir);
  }

end:
  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

//need to add access control
void mis_access(misserventry* eptr,ppacket* inp){
  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(!f){
    p = createpacket_s(4,MITOMD_ACCESS,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    p = createpacket_s(4,MITOMD_ACCESS,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);
  }

end:
  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

//need to add access control
void mis_open(misserventry* eptr,ppacket* inp){
  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(!f){
    p = createpacket_s(4,MITOMD_OPEN,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    p = createpacket_s(4,MITOMD_OPEN,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);
  }

end:
  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

void mis_update_attr(misserventry* eptr,ppacket* inp){ //no need to send back
  fprintf(stderr,"+mis_update_attr\n");

  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  inptr += len;
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(f){
    attr a;
    memcpy(&a,inptr,sizeof(attr));
    f->a = a;
  }

  free(path);
}

misserventry* mis_entry_from_ip(int ip){ //maybe add a hash?
  misserventry* eptr = misservhead;
  while(eptr){
    if(eptr->peerip == htonl(ip))
      return eptr;

    eptr = eptr->next;
  }

  return eptr;
}

//need to add access control
void mis_chmod(misserventry* eptr,ppacket* inp){
  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  inptr += len;
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(!f){
    p = createpacket_s(4,MITOMD_CHMOD,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    int perm = get32bit(&inptr);
    f->a.mode ^= (perm & 0777);

    fprintf(stderr,"perm=%o\n",perm);

    p = createpacket_s(4,MITOMD_CHMOD,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);

    if(f->srcip != eptr->peerip){//update mds info
      misserventry* ceptr = mis_entry_from_ip(f->srcip);

      if(eptr){
        ppacket* outp = createpacket_s(inp->size,CLTOMD_CHMOD,inp->id);
        memcpy(outp->startptr+HEADER_LEN,p->startptr,p->size);

        outp->next = ceptr->outpacket;
        ceptr->outpacket = outp;
      }
    }
  }

end:
  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

//need to add access control
void mis_chgrp(misserventry* eptr,ppacket* inp){
  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  inptr += len;
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(!f){
    p = createpacket_s(4,MITOMD_CHMOD,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    int gid = get32bit(&inptr);
    f->a.gid = gid;

    p = createpacket_s(4,MITOMD_CHMOD,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);

    if(f->srcip != eptr->peerip){//update mds info
      misserventry* ceptr = mis_entry_from_ip(f->srcip);

      if(eptr){
        ppacket* outp = createpacket_s(inp->size,CLTOMD_CHMOD,inp->id);
        memcpy(outp->startptr+HEADER_LEN,p->startptr,p->size);

        outp->next = ceptr->outpacket;
        ceptr->outpacket = outp;
      }
    }
  }

end:
  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

//need to add access control
void mis_chown(misserventry* eptr,ppacket* inp){
  ppacket* p;
  char* path;
  int len;
  const uint8_t* inptr;
  int i;

  inptr = inp->startptr;
  len = get32bit(&inptr);
  printf("plen=%d\n",len);

  path = (char*)malloc((len+10)*sizeof(char));
  memcpy(path,inptr,len*sizeof(char));
  inptr += len;
  path[len] = 0;

  printf("path=%s\n",path);

  ppfile* f = lookup_file(path);
  if(!f){
    p = createpacket_s(4,MITOMD_CHMOD,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,-ENOENT);
  } else {
    int uid = get32bit(&inptr);
    f->a.uid = uid;

    p = createpacket_s(4,MITOMD_CHMOD,inp->id);
    uint8_t* ptr = p->startptr + HEADER_LEN;
    put32bit(&ptr,0);

    if(f->srcip != eptr->peerip){//update mds info
      misserventry* ceptr = mis_entry_from_ip(f->srcip);

      if(eptr){
        ppacket* outp = createpacket_s(inp->size,CLTOMD_CHMOD,inp->id);
        memcpy(outp->startptr+HEADER_LEN,p->startptr,p->size);

        outp->next = ceptr->outpacket;
        ceptr->outpacket = outp;
      }
    }
  }

end:
  free(path);
  p->next = eptr->outpacket;
  eptr->outpacket = p;
}

