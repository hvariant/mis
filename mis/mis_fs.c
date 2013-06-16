#include "mis_fs.h"

static hashnode* tab[HASHSIZE];

int init_fs(){
  memset(tab,0,sizeof(tab));

  return 0;
}

void term_fs(){
  //clean up?
}

static hashnode* node_new(ppfile* f){
  hashnode* ret = (hashnode*)malloc(sizeof(hashnode));
  ret->data = (void*)f;
  ret->next = NULL;

  return ret;
}

static void node_free(hashnode* n){
  free(n);
}

void add_file(ppfile* f){
  unsigned int k = strhash(f->path) % HASHSIZE;

  if(tab[k] != NULL)
    if(!strcmp(f->path,tab[k]->key))
      return;

  hashnode* n = node_new(f);
  n->next = tab[k];
  tab[k] = n;
}

void remove_file(ppfile* f){
  unsigned int k = strhash(f->path) % HASHSIZE;
  hashnode* n = tab[k];
  hashnode* np = NULL;

  while(n){
    if(!strcmp(n->key,f->path)){
      if(np == NULL){ //head of list
        tab[k] = n->next;
        free(n);
      } else {
        np->next = n->next;
        free(n);
      }
    }

    np = n;
    n = n->next;
  }
}

ppfile* lookup_file(char* p){
  unsigned int k = strhash(p) % HASHSIZE;
  hashnode* n = tab[k];
  while(n){
    ppfile* f = (ppfile*)n->data;
    if(!strcmp(f->path,p)){
      return f;
    }

    n = n->next;
  }

  return NULL;
}
