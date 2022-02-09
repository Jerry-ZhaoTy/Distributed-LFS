#ifndef __MFS_h__
#define __MFS_h__

#define MFS_DIRECTORY    (0)
#define MFS_REGULAR_FILE (1)

#define MFS_BLOCK_SIZE   (4096)
#define MFS_INODE_NUM   (4096)
#define MFS_INODE_BLOCK_NUM (14)
#define MFS_IMAP_PIECE_INODE_NUM (16)
#define MFS_IMAP_PIECE_NUM (256) // 256 come from 4096(maximum num of nodes)/16(num of inodes in imap pieces)
#define MFS_MAX_ENTRIES_PER_DIR (128) // 128 come from 4096(size of block)/32(size of directory entry)

typedef struct __MFS_Stat_t {
    int type;   // MFS_DIRECTORY or MFS_REGULAR
    int size;   // bytes
    // note: no permissions, access times, etc.
} MFS_Stat_t;

typedef struct __MFS_DirEnt_t {
    char name[28];  // up to 28 bytes of name in directory (including \0)
    int  inum;      // inode number of entry (-1 means entry not used)
} MFS_DirEnt_t;

typedef struct __MFS_Inode_t{
    int size;
    int type;
    int data[MFS_INODE_BLOCK_NUM];
} MFS_Inode_t;

typedef struct __MFS_ImapPiece_t{
    int inodes[MFS_IMAP_PIECE_INODE_NUM];
} MFS_ImapPiece_t;

typedef struct __MFS_DirBlock_t {
  MFS_DirEnt_t DirEntry[MFS_MAX_ENTRIES_PER_DIR]; 
} MFS_DirBlock_t;

typedef struct __MFS_CR_t{
    int imap[MFS_IMAP_PIECE_NUM]; 
    int end_of_log;
} MFS_CR_t; // CR = checkpoint region


enum REQUEST {
    INIT,
    LOOKUP,
    STAT,
    WRITE,
    READ,
    CREAT,
    UNLINK,
    SHUTDOWN
};

typedef struct __Packet {
    enum REQUEST request;
    int inum;
    char name[28];
    MFS_Stat_t stat;
    char buffer[MFS_BLOCK_SIZE];
    int block;
    int type;
    int return_val;
} Packet;


int MFS_Init(char *hostname, int port);
int MFS_Lookup(int pinum, char *name);
int MFS_Stat(int inum, MFS_Stat_t *m);
int MFS_Write(int inum, char *buffer, int block);
int MFS_Read(int inum, char *buffer, int block);
int MFS_Creat(int pinum, int type, char *name);
int MFS_Unlink(int pinum, char *name);
int MFS_Shutdown();

#endif // __MFS_h__
