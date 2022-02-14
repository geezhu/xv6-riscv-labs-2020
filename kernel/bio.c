// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#define NBUCKET 13

struct {
  struct spinlock lock[NBUCKET];

  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf unused_buf[NBUCKET];
  struct buf head[NBUCKET];
  struct spinlock bucket_lock[NBUCKET];
} bcache;
uint hash(uint dev,uint blockno){
    return blockno%NBUCKET;
}
uint buf_hash(struct buf* b){
    return (b-bcache.buf)%NBUCKET;
}
void bucket_insert(uint bucket,struct buf* b){
    if(bucket>=13){
        panic("bucket_insert");
    }
    b->next=bcache.head[bucket].next;
    b->prev=&bcache.head[bucket];
    bcache.head[bucket].next->prev=b;
    bcache.head[bucket].next=b;
}
void unused_insert(struct buf* b){
    uint bucket=(b-bcache.buf)%NBUCKET;
    acquire(&bcache.lock[bucket]);
    b->next=bcache.unused_buf[bucket].next;
    b->prev=&bcache.unused_buf[bucket];
    bcache.unused_buf[bucket].next->prev=b;
    bcache.unused_buf[bucket].next=b;
    release(&bcache.lock[bucket]);
}
struct buf* unused_fetch(uint bucket){
    acquire(&bcache.lock[bucket]);
    struct buf* b=0;
    for(struct buf* r = bcache.unused_buf[bucket].prev; r != &bcache.unused_buf[bucket]; r = r->prev){
        r->prev->next=r->next;
        r->next->prev=r->prev;
        r->prev=0;
        r->next=0;
        b=r;
        break;
    }
    release(&bcache.lock[bucket]);
    return b;
}
struct buf* unused_steal(uint bucket){
    struct buf* b=0;
    for (int i = (bucket+1)%NBUCKET; i != bucket; i=(i+1)%NBUCKET) {
        b= unused_fetch(i);
        if(b){
            break;
        }
    }
    return b;
}
struct buf* unused_get(uint bucket){
    struct buf* b;
    b= unused_fetch(bucket);
    if(!b){
        b= unused_steal(bucket);
    }
    return b;
}

void bucket_lock(uint bucket){
    acquire(&bcache.bucket_lock[bucket]);
}
void bucket_unlock(uint bucket){
    release(&bcache.bucket_lock[bucket]);
}
void
binit(void)
{

  for (int i = 0; i < NBUCKET; ++i) {
      initlock(&bcache.lock[i], "bcache.lock");
      initlock(&bcache.bucket_lock[i], "bcache.bucket");
  }


  // Create linked list of buffers
  for (int i = 0; i < NBUCKET; ++i) {
      bcache.head[i].prev = &bcache.head[i];
      bcache.head[i].next = &bcache.head[i];
  }
  for (int i = 0; i < NBUCKET; ++i) {
      bcache.unused_buf[i].prev=&bcache.unused_buf[i];
      bcache.unused_buf[i].next=&bcache.unused_buf[i];
  }

  for (int i = 0; i < NBUF; ++i) {
      unused_insert(&bcache.buf[i]);
      initsleeplock(&bcache.buf[i].lock, "buffer");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint hash_bucket= hash(dev,blockno);
  bucket_lock(hash_bucket);

  // Is the block already cached?

  for(b = bcache.head[hash_bucket].next; b != &bcache.head[hash_bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      bucket_unlock(hash_bucket);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.

  b=unused_get(hash_bucket);
  if(b){
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      bucket_insert(hash_bucket,b);
      bucket_unlock(hash_bucket);
      acquiresleep(&b->lock);
      return b;
  }
  bucket_unlock(hash_bucket);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  uint  hash_bucket=hash(b->dev,b->blockno);
  bucket_lock(hash_bucket);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    unused_insert(b);
  }
  bucket_unlock(hash_bucket);
}

void
bpin(struct buf *b) {
  uint  hash_bucket=hash(b->dev,b->blockno);
  bucket_lock(hash_bucket);
  b->refcnt++;
  bucket_unlock(hash_bucket);
}

void
bunpin(struct buf *b) {
  uint  hash_bucket=hash(b->dev,b->blockno);
  bucket_lock(hash_bucket);
  b->refcnt--;
  bucket_unlock(hash_bucket);
}


