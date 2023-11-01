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

struct cache_bucket {
  struct spinlock lock;
  struct buf *head;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct cache_bucket buckets[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.buckets[i].lock, "bcache.bucket");
    bcache.buckets[i].head = 0;
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  struct cache_bucket *bucket = &bcache.buckets[blockno % NBUCKET];

  acquire(&bucket->lock);

  // Is the block already cached?
  for(b = bucket->head; b != 0; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle an unused buffer.
  acquire(&bcache.lock);
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    if(b->refcnt == 0) {
      b->refcnt = 1;
      release(&bcache.lock);
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;

      if (bucket->head == 0) {
        b->prev = b->next = 0;
        bucket->head = b;
      } else {
        b->prev = 0;
        b->next = bucket->head;
        bucket->head = b;
        b->next->prev = b;
      }

      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
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
  struct cache_bucket *bucket = &bcache.buckets[b->blockno % NBUCKET];

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bucket->lock);
  if (b->refcnt != 1) {
    b->refcnt--;
  } else {
    if (b->prev != 0) {
      b->prev->next = b->next;
    }

    if (b->next != 0) {
      b->next->prev = b->prev;
    }

    if (bucket->head == b) {
      bucket->head = b->next;
    }

    b->refcnt = 0;
  }
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.buckets[b->blockno % NBUCKET].lock);
  b->refcnt++;
  release(&bcache.buckets[b->blockno % NBUCKET].lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.buckets[b->blockno % NBUCKET].lock);
  b->refcnt--;
  release(&bcache.buckets[b->blockno % NBUCKET].lock);
}


