/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

// 变量end是由编译时的连接程序ld生成，用于表明内核代码的末端，即指明内核模块某段位置。
// 也可以从编译内核时生成的System.map文件中查出。这里用它来表明高速缓冲区开始于内核
// 代码某段位置。
// buffer_wait等变量是等待空闲缓冲块而睡眠的任务队列头指针。它与缓冲块头部结构中b_wait
// 指针的租用不同。当任务申请一个缓冲块而正好遇到系统缺乏可用空闲缓冲块时，当前任务
// 就会被添加到buffer_wait睡眠等待队列中。而b_wait则是专门供等待指定缓冲块(即b_wait
// 对应的缓冲块)的任务使用的等待队列头指针。
extern int end;
extern void put_super(int);
extern void invalidate_inodes(int);

struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH];           // NR_HASH ＝ 307项
static struct buffer_head * free_list;              // 空闲缓冲块链表头指针
static struct task_struct * buffer_wait = NULL;     // 等待空闲缓冲块而睡眠的任务队列
// 下面定义系统缓冲区中含有的缓冲块个数。这里，NR_BUFFERS是一个定义在linux/fs.h中的
// 宏，其值即使变量名nr_buffers，并且在fs.h文件中声明为全局变量。大写名称通常都是一个
// 宏名称，Linus这样编写代码是为了利用这个大写名称来隐含地表示nr_buffers是一个在内核
// 初始化之后不再改变的“变量”。它将在后面的缓冲区初始化函数buffer_init中被设置。
int NR_BUFFERS = 0;                                 // 系统含有缓冲区块的个数

//// 等待指定缓冲块解锁
// 如果指定的缓冲块bh已经上锁就让进程不可中断地睡眠在该缓冲块的等待队列b_wait中。
// 在缓冲块解锁时，其等待队列上的所有进程将被唤醒。虽然是在关闭中断(cli)之后
// 去睡眠的，但这样做并不会影响在其他进程上下文中影响中断。因为每个进程都在自己的
// TSS段中保存了标志寄存器EFLAGS的值，所以在进程切换时CPU中当前EFLAGS的值也随之
// 改变。使用sleep_on进入睡眠状态的进程需要用wake_up明确地唤醒。
static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();                          // 关中断
	while (bh->b_lock)              // 如果已被上锁则进程进入睡眠，等待其解锁
		sleep_on(&bh->b_wait);
	sti();                          // 开中断
}

//// 设备数据同步。
// 同步设备和内存高速缓冲中数据，其中sync_inode()定义在inode.c中。
int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

    // 首先调用i节点同步函数，把内存i节点表中所有修改过的i节点写入高速缓冲中。
    // 然后扫描所有高速缓冲区，对已被修改的缓冲块产生写盘请求，将缓冲中数据写入
    // 盘中，做到高速缓冲中的数据与设备中的同步。
	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);                 // 等待缓冲区解锁(如果已经上锁的话)
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);          // 产生写设备块请求
	}
	return 0;
}

//// 对指定设备进行高速缓冲数据与设备上数据的同步操作
// 该函数首先搜索高速缓冲区所有缓冲块。对于指定设备dev的缓冲块，若其数据已经
// 被修改过就写入盘中(同步操作)。然后把内存中i节点表数据写入 高速缓冲中。之后
// 再对指定设备dev执行一次与上述相同的写盘操作。
int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

    // 首先对参数指定的设备执行数据同步操作，让设备上的数据与高速缓冲区中的数据
    // 同步。方法是扫描高速缓冲区中所有缓冲块，对指定设备dev的缓冲块，先检测其
    // 是否已被上锁，若已被上锁就睡眠等待其解锁。然后再判断一次该缓冲块是否还是
    // 指定设备的缓冲块并且已修改过(b_dirt标志置位)，若是就对其执行写盘操作。
    // 因为在我们睡眠期间该缓冲块有可能已被释放或者被挪作他用，所以在继续执行前
    // 需要再次判断一下该缓冲块是否还是指定设备的缓冲块。
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)               // 不是设备dev的缓冲块则继续
			continue;
		wait_on_buffer(bh);                     // 等待缓冲区解锁
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
    // 再将i节点数据吸入高速缓冲。让i姐电表inode_table中的inode与缓冲中的信息同步。
	sync_inodes();
    // 然后在高速缓冲中的数据更新之后，再把他们与设备中的数据同步。这里采用两遍同步
    // 操作是为了提高内核执行效率。第一遍缓冲区同步操作可以让内核中许多"脏快"变干净，
    // 使得i节点的同步操作能够高效执行。本次缓冲区同步操作则把那些由于i节点同步操作
    // 而又变脏的缓冲块与设备中数据同步。
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

//// 使指定设备在高速缓冲区中的数据无效
// 扫描高速缓冲区中所有缓冲块，对指定设备的缓冲块复位其有效(更新)标志和已修改标志
void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
        // 如果不是指定设备的缓冲块，则继续扫描下一块
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
        // 由于进程执行过程睡眠等待，所以需要再判断一下缓冲区是否是指定设备的。
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
// 检查磁盘是否更换，如果已更换就使对应高速缓冲区无效
void check_disk_change(int dev)
{
	int i;

    // 首先检测一下是不是软盘设备。因为现在仅支持软盘可移动介质。如果不是则
    // 退出。然后测试软盘是否已更换，如果没有则退出。
	if (MAJOR(dev) != 2)
		return;
	if (!floppy_change(dev & 0x03))
		return;
    // 软盘已经更换，所以解释对应设备的i节点位图和逻辑块位图所占的高速缓冲区；
    // 并使该设备的i节点和数据库信息所占据的高速缓冲块无效。
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
		free_list = bh->b_next_free;
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	if ((bh = get_hash_table(dev,block)))
		return bh;
	tmp = free_list;
	do {
		if (tmp->b_count)
			continue;
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	wait_on_buffer(bh);
	if (bh->b_count)
		goto repeat;
	while (bh->b_dirt) {
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	remove_from_queues(bh);
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh);
	return bh;
}

void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ,bh);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	)

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			if ((bh[i] = getblk(dev,b[i])))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

// 缓冲区初始化函数
// 参数buffer_end是缓冲区内存末端。对于具有16MB内存的系统，缓冲区末端被设置为4MB.
// 对于有8MB内存的系统，缓冲区末端被设置为2MB。该函数从缓冲区开始位置start_buffer
// 处和缓冲区末端buffer_end处分别同时设置(初始化)缓冲块头结构和对应的数据块。直到
// 缓冲区中所有内存被分配完毕。
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;

    // 首先根据参数提供的缓冲区高端位置确定实际缓冲区高端位置b。如果缓冲区高端等于1Mb，
    // 则因为从640KB - 1MB被显示内存和BIOS占用，所以实际可用缓冲区内存高端位置应该是
    // 640KB。否则缓冲区内存高端一定大于1MB。
	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
    // 这段代码用于初始化缓冲区，建立空闲缓冲区块循环链表，并获取系统中缓冲块数目。
    // 操作的过程是从缓冲区高端开始划分1KB大小的缓冲块，与此同时在缓冲区低端建立
    // 描述该缓冲区块的结构buffer_head,并将这些buffer_head组成双向链表。
    // h是指向缓冲头结构的指针，而h+1是指向内存地址连续的下一个缓冲头地址，也可以说
    // 是指向h缓冲头的末端外。为了保证有足够长度的内存来存储一个缓冲头结构，需要b所
    // 指向的内存块地址 >= h 缓冲头的末端，即要求 >= h+1.
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;                       // 使用该缓冲块的设备号
		h->b_dirt = 0;                      // 脏标志，即缓冲块修改标志
		h->b_count = 0;                     // 缓冲块引用计数
		h->b_lock = 0;                      // 缓冲块锁定标志
		h->b_uptodate = 0;                  // 缓冲块更新标志(或称数据有效标志)
		h->b_wait = NULL;                   // 指向等待该缓冲块解锁的进程
		h->b_next = NULL;                   // 指向具有相同hash值的下一个缓冲头
		h->b_prev = NULL;                   // 指向具有相同hash值的前一个缓冲头
		h->b_data = (char *) b;             // 指向对应缓冲块数据块（1024字节）
		h->b_prev_free = h-1;               // 指向链表中前一项
		h->b_next_free = h+1;               // 指向连表中后一项
		h++;                                // h指向下一新缓冲头位置
		NR_BUFFERS++;                       // 缓冲区块数累加
		if (b == (void *) 0x100000)         // 若b递减到等于1MB，则跳过384KB
			b = (void *) 0xA0000;           // 让b指向地址0xA0000(640KB)处
	}
	h--;                                    // 让h指向最后一个有效缓冲块头
	free_list = start_buffer;               // 让空闲链表头指向头一个缓冲快
	free_list->b_prev_free = h;             // 链表头的b_prev_free指向前一项(即最后一项)。
	h->b_next_free = free_list;             // h的下一项指针指向第一项，形成一个环链
    // 最后初始化hash表，置表中所有指针为NULL。
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
