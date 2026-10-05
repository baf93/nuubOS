#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define S "/run/nuubos/regionald.sock"
static int wa(int f,const char*s){size_t n=strlen(s);while(n){ssize_t w=write(f,s,n);if(w<0){if(errno==EINTR)continue;return-1;}s+=w;n-=(size_t)w;}return 0;}
int main(int c,char**v){char q[512],b[2048];int f;ssize_t n;struct sockaddr_un a;if(c<2)return 2;if(!strcmp(v[1],"status"))snprintf(q,sizeof(q),"STATUS\n");else if(!strcmp(v[1],"timezones"))snprintf(q,sizeof(q),"LIST_TIMEZONES\n");else if(!strcmp(v[1],"keyboards"))snprintf(q,sizeof(q),"LIST_KEYBOARDS\n");else if(!strcmp(v[1],"sync"))snprintf(q,sizeof(q),"SYNC_NOW\n");else if(!strcmp(v[1],"set-timezone")&&c==3)snprintf(q,sizeof(q),"SET_TIMEZONE\t%s\n",v[2]);else if(!strcmp(v[1],"set-auto-time")&&c==3)snprintf(q,sizeof(q),"SET_AUTOMATIC_TIME\t%s\n",v[2]);else if(!strcmp(v[1],"set-keyboard")&&c==3)snprintf(q,sizeof(q),"SET_KEYBOARD\t%s\n",v[2]);else return 2;f=socket(AF_UNIX,SOCK_STREAM,0);if(f<0)return 1;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;snprintf(a.sun_path,sizeof(a.sun_path),"%s",S);if(connect(f,(struct sockaddr*)&a,sizeof(a))){close(f);return 1;}wa(f,q);shutdown(f,SHUT_WR);while((n=read(f,b,sizeof(b)))>0)fwrite(b,1,(size_t)n,stdout);close(f);return 0;}
