#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "telehash.h"
#include "tmesh.h"

struct sigblk_struct {
  uint8_t medium[4];
  uint8_t id[5];
  uint8_t neighbor:1;
  uint8_t val:7;
};

static tempo_t tempo_free(tempo_t tempo)
{
  if(!tempo) return NULL;
  util_frames_free(tempo->frames);
  free(tempo);
  return NULL;
}

static tempo_t tempo_new(cmnty_t com, hashname_t to, uint32_t medium, tempo_t signal)
{
  if(!com || !to || !medium) return LOG("bad args");

  tempo_t tempo;
  if(!(tempo = malloc(sizeof(struct tempo_struct)))) return LOG("OOM");
  memset(tempo,0,sizeof (struct tempo_struct));
  tempo->medium = medium;
  tempo->com = com;

  // generate tempo-specific mesh unique secret
  uint8_t roll[64];
  if(signal)
  {
    // signal tempo spawns streams
    tempo->frames = util_frames_new(64);

    // inherit seq and secret base
    tempo->seq = signal->seq;
    memcpy(roll,signal->secret,32);

    // add in the other party
    memcpy(roll+32,hashname_bin(to),32);

  }else{
    // new signal tempo, defaults to lost
    tempo->signal = 1;
    tempo->lost = 1;
    
    // base secret name+hn
    e3x_hash((uint8_t*)(com->name),strlen(com->name),roll);
    memcpy(roll+32,hashname_bin(to),32);
  }
  e3x_hash(roll,64,tempo->secret);

  // driver init for medium customizations
  if(com->tm->init && !com->tm->init(com->tm, tempo, NULL)) return tempo_free(tempo);

  return tempo;
}

static mote_t mote_free(mote_t mote);

static cmnty_t cmnty_free(cmnty_t com)
{
  if(!com) return NULL;
  while(com->motes)
  {
    mote_t m = com->motes;
    com->motes = com->motes->next;
    mote_free(m);
  }
  tempo_free(com->signal);
  if(com->tm->free) com->tm->free(com->tm, NULL, com);
  free(com->name);
  
  free(com);
  return NULL;
}

// create a new blank community
static cmnty_t cmnty_new(tmesh_t tm, char *name, uint32_t mediums[3])
{
  cmnty_t com;
  if(!tm || !name || !mediums) return LOG("bad args");

  if(!(com = malloc(sizeof (struct cmnty_struct)))) return LOG("OOM");
  memset(com,0,sizeof (struct cmnty_struct));
  com->tm = tm;
  com->name = strdup(name);
  com->m_lost = mediums[0];
  com->m_signal = mediums[1];
  com->m_stream = mediums[2];
  
  // driver init
  if(tm->init && !tm->init(tm, NULL, com)) return cmnty_free(com);

  // our default signal outgoing
  com->signal = tempo_new(com, tm->mesh->id, com->m_lost, NULL);
  com->signal->tx = 1; // our signal is always tx

  // make official
  com->next = tm->coms;
  tm->coms = com;

  return com;
}

/*
// initiates handshake over lost stream tempo
static tempo_t tempo_handshake(tempo_t m)
{
  if(!m) return LOG("bad args");
  tmesh_t tm = m->medium->com->tm;

  // TODO, set up first sync timeout to reset!
  util_frames_free(m->frames);
  if(!(m->frames = util_frames_new(64))) return LOG("OOM");
  
  // get relevant link, if any
  link_t link = m->link;
  if(!link) link = mesh_linked(tm->mesh, hashname_char(m->beacon), 0);

  // if public and no keys, send discovery
  if(m->medium->com->tm->pubim && (!link || !e3x_exchange_out(link->x,0)))
  {
    LOG("sending bare discovery %s",lob_json(tm->pubim));
    util_frames_send(m->frames, lob_copy(tm->pubim));
  }else{
    LOG("sending new handshake");
    util_frames_send(m->frames, link_handshakes(link));
  }

  return m;
}

// attempt to establish link from a lost stream tempo
static tempo_t tempo_link(tempo_t tempo)
{
  if(!tempo) return LOG("bad args");
  tmesh_t tm = tempo->medium->com->tm;

  // can't proceed until we've flushed
  if(util_frames_outbox(tempo->frames,NULL)) return LOG("not flushed yet: %d",util_frames_outlen(tempo->frames));

  // and also have received
  lob_t packet;
  if(!(packet = util_frames_receive(tempo->frames))) return LOG("none received yet");

  // for beacon tempos we process only handshakes here and create/sync link if good
  link_t link = NULL;
  do {
    LOG("beacon packet %s",lob_json(packet));
    
    // only receive raw handshakes
    if(packet->head_len == 1)
    {
      link = mesh_receive(tm->mesh, packet, tempo->medium->com->pipe);
      continue;
    }

    if(tm->pubim && lob_get(packet,"1a"))
    {
      hashname_t id = hashname_vkey(packet,0x1a);
      if(hashname_cmp(id,tempo->beacon) != 0)
      {
        printf("dropping mismatch key %s != %s\n",hashname_short(id),hashname_short(tempo->beacon));
        tempo_reset(tempo);
        return LOG("mismatch");
      }
      // if public, try new link
      lob_t keys = lob_new();
      lob_set_base32(keys,"1a",packet->body,packet->body_len);
      link = link_keys(tm->mesh, keys);
      lob_free(keys);
      lob_free(packet);
      continue;
    }
    
    LOG("unknown packet received on beacon tempo: %s",lob_json(packet));
    lob_free(packet);
  } while((packet = util_frames_receive(tempo->frames)));
  
  // booo, start over
  if(!link)
  {
    LOG("TODO: if a tempo reset its handshake may be old and rejected above, reset link?");
    tempo_reset(tempo);
    printf("no link\n");
    return LOG("no link found");
  }
  
  if(hashname_cmp(link->id,tempo->beacon) != 0)
  {
    printf("link beacon mismatch %s != %s\n",hashname_short(link->id),hashname_short(tempo->beacon));
    tempo_reset(tempo);
    return LOG("mismatch");
  }

  LOG("established link");
  tempo_t linked = tmesh_link(tm, tempo->medium->com, link);
  tempo_reset(linked);
  linked->at = tempo->at;
  memcpy(linked->nonce,tempo->nonce,8);
  linked->priority = 2;
  link_pipe(link, tempo->medium->com->pipe);
  lob_free(link_sync(link));
  
  // stop private beacon, make sure link fail resets it
  tempo->z = 0;

  return linked;
}
*/

// process new stream data on a tempo
static tempo_t tempo_process(tempo_t tempo)
{
  if(!tempo) return LOG("bad args");
  tmesh_t tm = tempo->mote->com->tm;
  
  // process any packets on this tempo
  lob_t packet;
  while((packet = util_frames_receive(tempo->frames)))
  {
    LOG("pkt %s",lob_json(packet));
    // TODO associate tempo for neighborhood
    mesh_receive(tm->mesh, packet, tempo->mote->pipe);
  }
  
  return tempo;
}

// find a stream to send it to for this mote
static void mote_send(pipe_t pipe, lob_t packet, link_t link)
{
  if(!pipe || !pipe->arg || !packet || !link)
  {
    LOG("bad args");
    lob_free(packet);
    return;
  }

  mote_t mote = (mote_t)pipe->arg;
  if(!mote->streams)
  {
    if(mote->cached)
    {
      LOG("dropping queued packet(%lu) waiting for stream",lob_len(mote->cached));
      lob_free(mote->cached);
    }
    mote->cached = packet;
    LOG("queued packet(%lu) waiting for stream",lob_len(packet));
    return;
  }

  util_frames_send(mote->streams->frames, packet);
  LOG("delivering %d to mote %s",lob_len(packet),hashname_short(link->id));
}

static mote_t mote_new(cmnty_t com, link_t link)
{
  if(!com || !link) return LOG("bad args");

  mote_t mote;
  if(!(mote = malloc(sizeof(struct mote_struct)))) return LOG("OOM");
  memset(mote,0,sizeof (struct mote_struct));
  mote->link = link;
  mote->com = com;
  
  // set up pipe
  if(!(mote->pipe = pipe_new("tmesh"))) return mote_free(mote);
  mote->pipe->arg = mote;
  mote->pipe->send = mote_send;
  
  // create lost signal
  mote->signal = tempo_new(com, link->id, com->m_lost, NULL);
  mote->signal->mote = mote;

  return mote;
}

static mote_t mote_free(mote_t mote)
{
  if(!mote) return NULL;
  // free signals and streams
  tempo_free(mote->signal);
  while(mote->streams)
  {
    tempo_t t = mote->streams;
    mote->streams = mote->streams->next;
    tempo_free(t);
  }
  pipe_free(mote->pipe);
  free(mote);
  return LOG("TODO");
}

// join a new community, starts lost signal on given medium
cmnty_t tmesh_join(tmesh_t tm, char *name, uint32_t mediums[3])
{
  cmnty_t com = cmnty_new(tm,name,mediums);
  if(!com) return LOG("bad args");

  LOG("joining community %s on mediums %lu, %lu, %lu",name,mediums[0],mediums[1],mediums[2]);
  lob_t path = lob_new();
  lob_set(path,"type","tmesh");
  lob_set_uint(path,"medium",mediums[0]);
  lob_set(path,"name",name);
  tm->mesh->paths = lob_push(tm->mesh->paths, path);

  return com;
}

// leave any community
tmesh_t tmesh_leave(tmesh_t tm, cmnty_t com)
{
  cmnty_t i, cn, c2 = NULL;
  if(!tm || !com) return LOG("bad args");
  
  // snip c out
  for(i=tm->coms;i;i = cn)
  {
    cn = i->next;
    if(i==com) continue;
    i->next = c2;
    c2 = i;
  }
  tm->coms = c2;
  
  cmnty_free(com);
  return tm;
}

// add a link known to be in this community
mote_t tmesh_find(tmesh_t tm, cmnty_t com, link_t link, uint32_t mediums[3])
{
  mote_t m;
  if(!tm || !com || !link) return LOG("bad args");

  // check list of motes, add if not there
  for(m=com->motes;m;m = m->next) if(m->link == link) return m;

  if(!(m = mote_new(com, link))) return LOG("OOM");
  m->link = link;
  m->next = com->motes;
  com->motes = m;

  // TODO add lost signal and lost stream
  
  // TODO set up link down event handler to remove this mote
  
  return m;
}

// if there's a mote for this link, return it
mote_t tmesh_mote(tmesh_t tm, link_t link)
{
  if(!tm || !link) return LOG("bad args");
  cmnty_t c;
  for(c=tm->coms;c;c=c->next)
  {
    mote_t m;
    for(m=c->motes;m;m = m->next) if(m->link == link) return m;
  }
  return LOG("no mote found for link %s",hashname_short(link->id));
}

pipe_t tmesh_on_path(link_t link, lob_t path)
{
  tmesh_t tm;

  // just sanity check the path first
  if(!link || !path) return NULL;
  if(!(tm = xht_get(link->mesh->index, "tmesh"))) return NULL;
  if(util_cmp("tmesh",lob_get(path,"type"))) return NULL;
  // TODO, check for community match and add
  // or create direct?
  return NULL;
}

tmesh_t tmesh_new(mesh_t mesh, lob_t options)
{
  tmesh_t tm;
  if(!mesh) return NULL;

  if(!(tm = malloc(sizeof (struct tmesh_struct)))) return LOG("OOM");
  memset(tm,0,sizeof (struct tmesh_struct));

  // connect us to this mesh
  tm->mesh = mesh;
  xht_set(mesh->index, "tmesh", tm);
  mesh_on_path(mesh, "tmesh", tmesh_on_path);
  
  tm->pubim = hashname_im(tm->mesh->keys, hashname_id(tm->mesh->keys,tm->mesh->keys));
  
  return tm;
}

void tmesh_free(tmesh_t tm)
{
  cmnty_t com, next;
  if(!tm) return;
  for(com=tm->coms;com;com=next)
  {
    next = com->next;
    cmnty_free(com);
  }
  lob_free(tm->pubim);
  // TODO path cleanup
  free(tm);
  return;
}

// fills in next tx knock
static knock_t tempo_knock(tempo_t tempo, knock_t k)
{
  if(!tempo || !k) return LOG("bad args");
  mote_t mote = tempo->mote;
  cmnty_t com = tempo->com;
  // ;)

  // send data frames if any
  if(tempo->frames)
  {
    if(!util_frames_outbox(tempo->frames,k->frame))
    {
      // nothing to send, noop
      tempo->skip++;
      LOG("tx noop %u",tempo->skip);
      return NULL;
    }

    LOG("TX frame %s\n",util_hex(k->frame,64,NULL));

    // ciphertext full frame
    chacha20(tempo->secret,k->nonce,k->frame,64);
    return k;
  }

  // construct signal tx

  struct sigblk_struct blk;

  // lost signal is special
  if(tempo->lost)
  {
    // nonce is prepended to lost signals
    memcpy(k->frame,k->nonce,8);
    
    uint8_t at = 8, syncs = 0;
    mote_t mote;
    for(mote=com->motes;mote;mote = mote->next) if(mote->streams && mote->streams->lost)
    {
      memcpy(blk.medium, &(mote->streams->medium), 4);
      memcpy(blk.id, hashname_bin(mote->link->id), 5);
      blk.neighbor = 0;
      blk.val = 2; // lost broadcasts an accept
      k->syncs[syncs++] = mote->streams; // needs to be sync'd after tx
      memcpy(k->frame+at,&blk,10);
      at += 10;
      if(at > 50) break; // full!
    }
    
    // check hash at end
    murmur(k->frame+8,64-(8+4),k->frame+60);

    LOG("TX lost signal frame: %s",util_hex(k->frame,64,NULL));

    // ciphertext frame after nonce
    chacha20(tempo->secret,k->nonce,k->frame+8,64-8);

    return k;
  }

  // TODO normal signal
  for(mote = com->motes;mote;mote = mote->next)
  {
    // check for any w/ cached packets to request stream
    // accept any requested streams (mote->m_req)
    //    tempo->syncs[syncs++] = mote->streams; // needs to be sync'd after tx
  }

  for(mote = com->motes;mote;mote = mote->next)
  {
    // fill in neighbors in remaining slots
  }

  LOG("TX signal frame: %s",util_hex(k->frame,64,NULL));

  // check hash at end
  murmur(k->frame,60,k->frame+60);

  // ciphertext full frame
  chacha20(tempo->secret,k->nonce,k->frame,64);

  return k;
}

// handle a knock that has been sent/received
tmesh_t tmesh_knocked(tmesh_t tm, knock_t k)
{
  if(!tm || !k) return LOG("bad args");
  if(!k->ready) return LOG("knock wasn't ready");
  
  tempo_t tempo = k->tempo;

  // clear some flags straight away
  k->ready = 0;
  
  if(k->err)
  {
    // missed rx windows
    if(!tempo->tx)
    {
      tempo->miss++; // count missed rx knocks
      // if expecting data, trigger a flush
      if(util_frames_await(tempo->frames)) util_frames_send(tempo->frames,NULL);
    }
    LOG("knock error");
    if(tempo->tx) printf("tx error\n");
    return tm;
  }
  
  // tx just updates state things here
  if(tempo->tx)
  {
    tempo->skip = 0; // clear skipped tx's
    tempo->itx++;
    
    // did we send a data frame?
    if(tempo->frames)
    {
      LOG("tx frame done %lu",util_frames_outlen(tempo->frames));

      return tm;
    }

    // lost signals always sync next at time to when actually done
    if(tempo->lost) tempo->at = k->stopped;
    
    // sync any bundled/accepted stream tempos too
    uint8_t syncs;
    for(syncs=0;syncs<5;syncs++) if(k->syncs[syncs]) k->syncs[syncs]->at = k->stopped;

    return tm;
  }
  
  // process streams first
  if(!tempo->signal)
  {
    chacha20(tempo->secret,k->nonce,k->frame,64);
    LOG("RX data RSSI %d frame %s\n",k->rssi,util_hex(k->frame,64,NULL));

    if(!util_frames_inbox(tempo->frames, k->frame))
    {
      k->tempo->bad++;
      return LOG("bad frame: %s",util_hex(k->frame,64,NULL));
    }

    // received stats only after validation
    tempo->miss = 0;
    tempo->irx++;
    if(k->rssi < tempo->best || !tempo->best) tempo->best = k->rssi;
    if(k->rssi > tempo->worst) tempo->worst = k->rssi;
    tempo->last = k->rssi;

    LOG("RX data received, total %lu rssi %d/%d/%d\n",util_frames_inlen(k->tempo->frames),k->tempo->last,k->tempo->best,k->tempo->worst);

    // process any new packets
    tempo_process(tempo);
    return tm;
  }

  // decode/validate signal safely
  uint8_t at = 0;
  uint8_t frame[64];
  memcpy(frame,k->frame,64);
  chacha20(tempo->secret,k->nonce,frame,64);
  uint32_t check = murmur4(frame,60);
  
  // did it validate
  if(memcmp(&check,frame+60,4) != 0)
  {
    // also check if lost encoded
    memcpy(frame,k->frame,64);
    chacha20(tempo->secret,frame,frame+8,64-8);
    uint32_t check = murmur4(frame,60);
    
    // lost encoded signal fail
    if(memcmp(&check,frame+60,4) != 0)
    {
      tempo->bad++;
      return LOG("signal frame validation failed");
    }
    
    LOG("valid lost signal: %s",util_hex(frame,64,NULL));

    // always sync lost time
    tempo->at = k->stopped;
    tempo->lost = 0;

    // sync lost nonce info
    memcpy(&(tempo->medium),frame,4); // sync medium
    memcpy(&(tempo->mote->seq),frame+4,2); // sync mote nonce
    memcpy(&(tempo->seq),frame+6,2); // sync tempo nonce
    
    // fall through w/ offset past prefixed nonce
    at = 8;
  }else{
    LOG("valid regular signal: %s",util_hex(frame,64,NULL));
  }

  struct sigblk_struct blk;
  while(at <= 50)
  {
    memcpy(&blk,frame+at,10);
    at += 10;

    uint32_t medium;
    memcpy(&medium,blk.medium,4);
    hashname_t id = hashname_sbin(blk.id);

    if(blk.neighbor)
    {
      LOG("neighbor %s on %lu q %u",hashname_short(id),medium,blk.val);
      // TODO, do we want to talk to them? add id as a peer path now and do a peer/connect
      continue;
    }

    // TODO also match against our exchange token so neighbors don't recognize stream requests
    if(hashname_scmp(id,tm->mesh->id) != 0) continue;
    
    // streams r us!
    if(blk.val == 2)
    {
      LOG("TODO start new stream from here");
    }
    if(blk.val == 1)
    {
      LOG("stream requested from %s on medium %lu",hashname_short(id),medium);
      tempo->mote->m_req = medium;
    }
  }

  return tm;
}

// inner logic
static tempo_t tempo_schedule(tempo_t tempo, uint32_t at, uint32_t rebase)
{
  mote_t mote = tempo->mote;
  cmnty_t com = tempo->com;
  tmesh_t tm = com->tm;

  // initialize to *now* if not set
  if(!tempo->at) tempo->at = at + rebase;

  // first rebase cycle count if requested
  if(rebase) tempo->at -= rebase;

  // already have one active, noop
  if(com->knock->ready) return tempo;

  // move ahead window(s)
  while(tempo->at < at)
  {
    // handle seq overflow cascading, notify driver if the big one
    tempo->seq++;
    if(tempo->seq == 0)
    {
      tempo->seq++;
      if(tempo->seq == 0)
      {
        if(mote)
        {
          mote->seq++;
        }else{
          com->seq++;
          tm->notify(tm, NULL);
        }
      }
    }

    // use encrypted seed (follows frame)
    uint8_t seed[64+8] = {0};
    uint8_t nonce[8] = {0};
    memcpy(nonce,&(tempo->medium),4);
    memcpy(nonce+4,&(mote->seq),2);
    memcpy(nonce+6,&(tempo->seq),2);
    chacha20(tempo->secret,nonce,seed,64+8);
    
    // call driver to apply seed to tempo
    if(!tm->advance(tm, tempo, seed+64)) return LOG("driver advance failed");
  }

  return tempo;
}

// process everything based on current cycle count, returns success
tmesh_t tmesh_schedule(tmesh_t tm, uint32_t at, uint32_t rebase)
{
  if(!tm || !at) return LOG("bad args");
  if(!tm->sort || !tm->advance || !tm->schedule) return LOG("driver missing");
  
  LOG("processing for %s at %lu",hashname_short(tm->mesh->id),at);

  // we are looking for the next knock anywhere
  cmnty_t com;
  for(com=tm->coms;com;com=com->next)
  {
    tempo_t lost = NULL;

    // upcheck our signal first
    tempo_t best = tempo_schedule(com->signal, at, rebase);

    // walk all the tempos for next best knock
    mote_t mote;
    for(mote=com->motes;mote;mote=mote->next)
    {
      // advance
      tempo_schedule(mote->signal, at, rebase);
      
      // lost signals are elected for seek, others for best
      if(mote->signal->lost) lost = tm->sort(tm, lost, mote->signal);
      else best = tm->sort(tm, best, mote->signal);
      
      // any/every stream for best pool
      tempo_t tempo;
      for(tempo=mote->streams;tempo;tempo = tempo->next) best = tm->sort(tm, best, tempo_schedule(tempo, at, rebase));
    }
    
    // already an active knock
    if(com->knock->ready) continue;

    // any lost tempo, always set seek
    memset(com->seek,0,sizeof(struct knock_struct));
    if(lost)
    {
      // copy nonce parts in, nothing to prep since is just RX
      memcpy(com->seek->nonce,&(lost->medium),4);
      memcpy(com->seek->nonce+4,&(lost->mote->seq),2);
      memcpy(com->seek->nonce+6,&(lost->seq),2);
      com->seek->tempo = lost;
      com->seek->ready = 1;
    }

    // init new knock for this tempo
    memset(com->knock,0,sizeof(struct knock_struct));

    // copy nonce parts in
    memcpy(com->knock->nonce,&(best->medium),4);
    if(best->mote) memcpy(com->knock->nonce+4,&(best->mote->seq),2);
    else memcpy(com->knock->nonce+4,&(best->com->seq),2);
    memcpy(com->knock->nonce+6,&(best->seq),2);

    // do the work to fill in the tx frame only once here
    if(best->tx && !tempo_knock(best, com->knock))
    {
      LOG("tx prep failed, skipping knock");
      continue;
    }
    com->knock->tempo = best;
    com->knock->ready = 1;

    // signal driver
    if(!tm->schedule(tm, com->knock))
    {
      LOG("radio ready driver failed, canceling knock");
      memset(com->knock,0,sizeof(struct knock_struct));
      continue;
    }
  }

  return tm;
}
