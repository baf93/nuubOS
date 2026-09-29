/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define SOCK_PATH "/run/nuubos/inputd.sock"
static int conn(void){int f=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a;if(f<0)return -1;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;snprintf(a.sun_path,sizeof(a.sun_path),"%s",SOCK_PATH);if(connect(f,(struct sockaddr*)&a,sizeof(a))){close(f);return -1;}return f;}
int main(int ac,char**av){char c[128],r[512];ssize_t n;int f;if(ac==2&&!strcmp(av[1],"status"))snprintf(c,sizeof(c),"STATUS\n");else if(ac==4&&!strcmp(av[1],"bind")&&!strcmp(av[2],"get"))snprintf(c,sizeof(c),"BIND GET %s\n",av[3]);else if(ac==5&&!strcmp(av[1],"bind")&&!strcmp(av[2],"set"))snprintf(c,sizeof(c),"BIND SET %s %s\n",av[3],av[4]);else{fprintf(stderr,"Usage: %s status | bind get ACTION | bind set ACTION EV_KEY_CODE\n",av[0]);return 2;}f=conn();if(f<0){fprintf(stderr,"cannot connect to %s: %s\n",SOCK_PATH,strerror(errno));return 1;}if(write(f,c,strlen(c))<0){close(f);return 1;}n=read(f,r,sizeof(r)-1);if(n<0){close(f);return 1;}r[n]='\0';fputs(r,stdout);close(f);return !strncmp(r,"ERR",3);}
