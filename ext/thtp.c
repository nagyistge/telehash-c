#include "ext.h"

typedef struct thtp_struct 
{
  xht_t index;
  packet_t glob;
} *thtp_t;

thtp_t thtp_new(switch_t s, xht_t index)
{
  thtp_t t;
  t = malloc(sizeof (struct thtp_struct));
  memset(t,0,sizeof (struct thtp_struct));
  if(!index) index = xht_new(13);
  t->index = index;
  xht_set(s->index,"thtp",t);
  return t;
  
}

// typeless wrapper
void thtp_init(switch_t s, xht_t index)
{
  thtp_new(s,index);
}

thtp_t thtp_get(switch_t s)
{
  thtp_t t;
  t = xht_get(s->index,"thtp");
  return t ? t : thtp_new(s,NULL);
}

void thtp_free(switch_t s)
{
  thtp_t t = thtp_get(s);
  //xht_walk() TODO free all notes
  // free all globs
  xht_free(t->index);
  free(t);
}

void thtp_glob(switch_t s, char *glob, packet_t note)
{
  thtp_t t = thtp_get(s);
  packet_set_str(note,"glob",glob);
  note->next = t->glob;
  t->glob = note;
}

void thtp_path(switch_t s, char *path, packet_t note)
{
  thtp_t t = thtp_get(s);
  packet_set_str(note,"path",path);
  xht_set(t->index,packet_get_str(note,"path"),(void*)note);
}

packet_t _thtp_glob(thtp_t t, char *p1)
{
  char *p2;
  packet_t cur;
  if(!t || !p1) return NULL;
  cur = t->glob;
  while(cur)
  {
    p2 = packet_get_str(cur,"glob");
    if(strncasecmp(p1,p2,strlen(p2)) == 0) return cur;
    cur = cur->next;
  }
  return NULL;
}

// chunk the packet out
void thtp_send(chan_t c, packet_t p)
{
  packet_t chunk;
  unsigned char *raw;
  unsigned short len, space;
  if(!c || !p) return;
  raw = packet_raw(p);
  len = packet_len(p);
  while(len)
  {
    chunk = chan_packet(c);
    if(!chunk) return; // TODO backpressure
    space = packet_space(chunk);
    if(space > len) space = len;
    packet_body(chunk,raw,space);
    if(len==space) packet_set(chunk,"end","true",4);
    chan_send(c,chunk);
    raw+=space;
    len-=space;
  }
}

void ext_thtp(chan_t c)
{
  packet_t p, req, match, note;
  char *path;
  thtp_t t = thtp_get(c->s);

  // incoming note as an answer
  if(c->state == ENDING && (note = chan_notes(c)))
  {
    DEBUG_PRINTF("got note resp %.*s",note->json_len,note->json);
    thtp_send(c,packet_linked(note));
    packet_free(note);
    return;
  }

  while((p = chan_pop(c)))
  {
    if(!c->arg)
    {
      c->arg = req = p;
    }else{
      req = c->arg;
      packet_append(req,p->body,p->body_len);
      packet_free(p);
    }
    // for now we're processing whole-requests-at-once, to do streaming we can try parsing note->body for the headers anytime
    if(!c->state == ENDING) continue;

    // parse the request
    p = packet_parse(req->body,req->body_len);
    packet_free(req);
    if(!p) return (void)chan_fail(c,"422");
    req = p;

    DEBUG_PRINTF("thtp packet %.*s", req->json_len, req->json);
    path = packet_get_str(req,"path");
    match = xht_get(t->index,path);
    if(!match) match = _thtp_glob(t,path);
    if(!match)
    {
      chan_fail(c,"404");
      packet_free(req);
      return;
    }

    // built in response
    if(packet_linked(match))
    {
      thtp_send(c,packet_linked(match));
      packet_free(req);
      return;
    }
    
    // attach and route request to a new note
    note = packet_copy(match);
    packet_link(note,req);
    if(chan_reply(c,note) == 0) return;

    chan_fail(c,"500");
    packet_free(req);
  }
  
  // optionally sends ack if needed
  chan_ack(c);
}