#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "udp.h"
#include "mfs.h"

int lfs_init(int port, char* image_path);
int lfs_lookup(int pinum, char* name);
int lfs_stat(int inum, MFS_Stat_t* m);
int lfs_write(int inum, char* buffer, int block);
int lfs_read(int inum, char* buffer, int block);
int lfs_creat(int pinum, int type, char* name);
int lfs_unlink(int pinum, char* name);
int lfs_shutdown();

int fs_image; // global variable to store file system image
MFS_CR_t* CR; // global variable to store checkpoint region


// method to initialize and run a log-structured file server
int lfs_init(int port, char* image_path) {

  // try to open the given file system image
  fs_image = open(image_path, O_RDWR);
  
  // check if given file is empty
  if (fs_image == -1){
    // given file is empty, do the initialization
    // creation and initialization of the checkpoint region
    fs_image = open(image_path, O_RDWR|O_CREAT);

    CR = (MFS_CR_t *)malloc(sizeof(MFS_CR_t)); 
    CR->end_of_log = 0;
    for(int i = 0; i < MFS_IMAP_PIECE_NUM; i++)
      CR->imap[i] = -1;
    // add CR to file image
    lseek(fs_image, CR->end_of_log, SEEK_SET);
    write(fs_image, CR, sizeof(MFS_CR_t));
    CR->end_of_log += sizeof(MFS_CR_t); // update CR->end_of_log for future lseek

    // creation and initialization of the root directory
    MFS_DirBlock_t root_dir;
    strcpy(root_dir.DirEntry[0].name, ".\0");
    root_dir.DirEntry[0].inum = 0;
    strcpy(root_dir.DirEntry[1].name, "..\0");
    root_dir.DirEntry[1].inum = 0;
    for(int i = 2; i < MFS_MAX_ENTRIES_PER_DIR; i++)
      root_dir.DirEntry[i].inum = -1;
    // add root directory to file image
    lseek(fs_image, CR->end_of_log, SEEK_SET);
    write(fs_image, &root_dir, sizeof(MFS_DirBlock_t));
    CR->end_of_log += sizeof(MFS_DirBlock_t); // update CR->end_of_log for future lseek

    // set up the inode for the root directory
    MFS_Inode_t root_inode;
    root_inode.size = MFS_BLOCK_SIZE; 
    root_inode.type = MFS_DIRECTORY;
    root_inode.data[0] = CR->end_of_log - sizeof(MFS_DirBlock_t); // address of root directory in the file image
    for (int i = 1; i < MFS_INODE_BLOCK_NUM; i++) 
      root_inode.data[i] = -1; 
    // add root directory's inode to file image
    lseek(fs_image, CR->end_of_log, SEEK_SET);
    write(fs_image, &root_inode, sizeof(MFS_Inode_t));
    CR->end_of_log += sizeof(MFS_Inode_t); // update CR->end_of_log for future lseek

    // set up imap_piece for root directory's inode
    MFS_ImapPiece_t imap_piece;
    imap_piece.inodes[0] = CR->end_of_log - sizeof(MFS_Inode_t); // address of root directory's inode
    for(int i = 1; i < MFS_IMAP_PIECE_INODE_NUM; i++) 
      imap_piece.inodes[i] = -1;

    lseek(fs_image, CR->end_of_log, SEEK_SET);
    write(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));
    CR->end_of_log += sizeof(MFS_ImapPiece_t);  

    // update checkpoint region after change in imap and end_of_log
    CR->imap[0] = CR->end_of_log - sizeof(MFS_ImapPiece_t);
    lseek(fs_image, 0, SEEK_SET);
    write(fs_image, CR, sizeof(MFS_CR_t));
    fsync(fs_image); // commit changes to disk after write
  }
  else {
    // given file exists, retrieve its checkpoint region
    lseek(fs_image, 0, SEEK_SET);
    read(fs_image, CR, sizeof(MFS_CR_t));
  } // end of file system image initialization

  // start running the server
  // open port with given port num and deal with requests
  int fd = UDP_Open(port);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  Packet send_packet, return_packet;

  while (1) {
    if (UDP_Read(fd, &addr, (char *)&send_packet, sizeof(Packet)) < 1) continue;

    if(send_packet.request == LOOKUP){
      return_packet.return_val = lfs_lookup(send_packet.inum, send_packet.name);
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
    }
    else if(send_packet.request == STAT){
      return_packet.return_val = lfs_stat(send_packet.inum, &(return_packet.stat));
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
    }
    else if(send_packet.request == WRITE){
      return_packet.return_val = lfs_write(send_packet.inum, send_packet.buffer, send_packet.block);
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
    }
    else if(send_packet.request == READ){
      return_packet.return_val = lfs_read(send_packet.inum, return_packet.buffer, send_packet.block);
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
    }
    else if(send_packet.request == CREAT){
      return_packet.return_val = lfs_creat(send_packet.inum, send_packet.type, send_packet.name);
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
    }
    else if(send_packet.request == UNLINK){
      return_packet.return_val = lfs_unlink(send_packet.inum, send_packet.name);
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
    }
    else if(send_packet.request == SHUTDOWN) {
      UDP_Write(fd, &addr, (char*)&return_packet, sizeof(Packet));
      lfs_shutdown();
    }
    else {
      return -1; // invalid request
    }
  }
  return 0;
}


// method used to response to lookup requests
int lfs_lookup(int pinum, char* name) {

  if (pinum < 0 || pinum >= MFS_INODE_NUM) return -1; // check if pinum is valid
  if (CR->imap[pinum / MFS_IMAP_PIECE_INODE_NUM] == -1) return -1; // check if parent imap piece exists

  // find imap piece for parent inode
  int pimap_piece_addr =  CR->imap[pinum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  if (pimap_piece_addr == -1) return -1; // check if imap piece for parent inode exists
  MFS_ImapPiece_t pimap_piece;
  lseek(fs_image, pimap_piece_addr, SEEK_SET);
  read(fs_image, &pimap_piece, sizeof(MFS_ImapPiece_t));

  // find parent inode
  int pinode_addr = pimap_piece.inodes[pinum % MFS_IMAP_PIECE_INODE_NUM]; // get address of parent inode with given inum
  if(pinode_addr == -1) return -1; // check if parent inode exists
  MFS_Inode_t pinode;
  lseek(fs_image, pinode_addr, SEEK_SET);
  read(fs_image, &pinode, sizeof(MFS_Inode_t));

  // make sure given parent inode is a directory
  if (pinode.type != MFS_DIRECTORY) return -1;
  
  for(int i = 0; i < MFS_INODE_BLOCK_NUM; i++) {
    int dir_addr = pinode.data[i]; // get address of parent directory
    if(dir_addr == -1) return -1; // check if given parent directory is empty
    // read data from given parent directory
    MFS_DirBlock_t pdir_block;
    lseek(fs_image, dir_addr, SEEK_SET);
    read(fs_image, &pdir_block, MFS_BLOCK_SIZE);
    // search for given name
    for(int j = 0; j < MFS_MAX_ENTRIES_PER_DIR; j++) {
      MFS_DirEnt_t cur_entry = pdir_block.DirEntry[j];
      if (cur_entry.inum == -1) continue; // skip empty DirEntry
      if (strcmp(cur_entry.name, name) == 0)
        return cur_entry.inum;
    }
  }
  return -1; // return -1 if not found after looping over all DirEntry in the directory
}


// method used to response to stat requests
int lfs_stat(int inum, MFS_Stat_t* m) {

  if (inum < 0 || inum >= MFS_INODE_NUM) return -1; // check if inum is valid
  if (CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM] == -1) return -1; // check if imap piece exists

  // find imap piece
  int imap_piece_addr = CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  if (imap_piece_addr == -1) return -1; // check if imap piece exists
  MFS_ImapPiece_t imap_piece;
  lseek(fs_image, imap_piece_addr, SEEK_SET);
  read(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));

  // find inode
  int inode_addr = imap_piece.inodes[inum % MFS_IMAP_PIECE_INODE_NUM]; // get address of inode with given inum
  if (inode_addr == -1) return -1; // check if inode exists
  MFS_Inode_t inode;
  lseek(fs_image, inode_addr, SEEK_SET);
  read(fs_image, &inode, sizeof(MFS_Inode_t));

  // get inode stat
  m->size = inode.size;
  m->type = inode.type;

  return 0;
}


// method used to response to write requests
int lfs_write(int inum, char* buffer, int block) {

  if (inum < 0 || inum >= MFS_INODE_NUM) return -1; // check if inum is valid
  if (block < 0 || block > MFS_INODE_BLOCK_NUM-1)  return -1; // check if block is valid
  if (CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM] == -1) return -1; // check if imap piece exists

  // find imap piece
  int imap_piece_addr =  CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  if (imap_piece_addr == -1) return -1; // check if imap piece exists
  MFS_ImapPiece_t imap_piece;
  lseek(fs_image, imap_piece_addr, SEEK_SET);
  read(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));

  // find inode
  int inode_addr = imap_piece.inodes[inum % MFS_IMAP_PIECE_INODE_NUM]; // get address of inode with given inum
  if (inode_addr == -1) return -1; // check if inode exists
  MFS_Inode_t inode;
  lseek(fs_image, inode_addr, SEEK_SET);
  read(fs_image, &inode, sizeof(MFS_Inode_t));
  if (inode.type != MFS_REGULAR_FILE) return -1; // make sure given inode points to regular file
  
  // write data to end of log and update given inode block pointer
  int block_addr = CR->end_of_log;
  inode.data[block] = block_addr;
  lseek(fs_image, block_addr, SEEK_SET);
  write(fs_image, buffer, MFS_BLOCK_SIZE);
  CR->end_of_log += MFS_BLOCK_SIZE;
  inode.size = (block + 1) * MFS_BLOCK_SIZE;

  // update inode in the file system image after write
  lseek(fs_image, inode_addr, SEEK_SET);
  write(fs_image, &inode, sizeof(MFS_Inode_t));
  fsync(fs_image); // commit changes to disk after write

  return 0;
}


// method used to response to read requests
int lfs_read(int inum, char* buffer, int block) {

  if (inum < 0 || inum >= MFS_INODE_NUM) return -1; // check if inum is valid
  if (block < 0 || block > MFS_INODE_BLOCK_NUM-1)  return -1; // check if block is valid
  if (CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM] == -1) return -1; // check if imap piece exists

  // find imap piece
  int imap_piece_addr = CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  if (imap_piece_addr == -1) return -1; // check if imap piece exists
  MFS_ImapPiece_t imap_piece;
  lseek(fs_image, imap_piece_addr, SEEK_SET);
  read(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));

  // find inode
  int inode_addr = imap_piece.inodes[inum % MFS_IMAP_PIECE_INODE_NUM]; // get address of inode with given inum
  if (inode_addr == -1) return -1; // check if inode exists
  MFS_Inode_t inode;
  lseek(fs_image, inode_addr, SEEK_SET);
  read(fs_image, &inode, sizeof(MFS_Inode_t));
 
  // read the block
  int block_addr = inode.data[block];
  lseek(fs_image, block_addr, SEEK_SET);
  read(fs_image, buffer, MFS_BLOCK_SIZE);

  return 0;
}


// method used to response to creat requests
int lfs_creat(int pinum, int type, char* name) {

  if (pinum < 0 || pinum >= MFS_INODE_NUM) return -1; // check if pinum is valid

  // check if given name is too long
  int len_name = 0;
  while (name[len_name] != '\0') len_name++;
  if (len_name > 27) return -1; // too long, creat failed

  // check if name already exists, return success if found
  if (lfs_lookup(pinum, name) != -1) return 0;

  if (CR->imap[pinum / MFS_IMAP_PIECE_INODE_NUM] == -1) return -1; // check if parent imap piece exists

  // find parent imap piece
  int pimap_piece_addr = CR->imap[pinum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  if (pimap_piece_addr == -1) return -1; // check if imap piece exists
  MFS_ImapPiece_t pimap_piece;
  lseek(fs_image, pimap_piece_addr, SEEK_SET);
  read(fs_image, &pimap_piece, sizeof(MFS_ImapPiece_t));

  // find parent inode
  int pinode_addr = pimap_piece.inodes[pinum % MFS_IMAP_PIECE_INODE_NUM]; // get address of inode with given inum
  if(pinode_addr == -1) return -1; // check if inode exists
  MFS_Inode_t pinode;
  lseek(fs_image, pinode_addr, SEEK_SET);
  read(fs_image, &pinode, sizeof(MFS_Inode_t));

  // make sure given parent inode is a directory
  if (pinode.type != MFS_DIRECTORY) return -1;

  // check if parent directory is full
  int full_parent_directory = 1;
  for (int i = 0; i < MFS_INODE_BLOCK_NUM; i++) {
    if (full_parent_directory == 0) break;
    int dir_addr = pinode.data[i]; // get address of parent directory block
    if(dir_addr == -1) {
      full_parent_directory = 0;
      break;
    }
    MFS_DirBlock_t pdir_block;
    lseek(fs_image, dir_addr, SEEK_SET);
    read(fs_image, &pdir_block, MFS_BLOCK_SIZE);
    // search for empty entry
    for(int j = 0; j < MFS_MAX_ENTRIES_PER_DIR; j++) {
      if (pdir_block.DirEntry[j].inum == -1) {
        full_parent_directory = 0;
        break;
      }
    }
  }
  if (full_parent_directory == 1) return -1; // given parent directory is full, fail

 
  // create an inode for name at end_of_log
  MFS_Inode_t new_inode;
  new_inode.type = type;
  new_inode.size = 0;
  for (int i = 0; i < MFS_INODE_BLOCK_NUM; i++) 
    new_inode.data[i] = -1; 
  // add new inode to file system image
  lseek(fs_image, CR->end_of_log, SEEK_SET);
  write(fs_image, &new_inode, sizeof(MFS_Inode_t));
  int new_inode_addr = CR->end_of_log;
  CR->end_of_log += sizeof(MFS_Inode_t); // update CR->end_of_log for future lseek

  // update imap corresponding to the creation
  int new_inode_num = -1;
  int added = 0; // variable to check whether new_inode has been added to imap
  for(int i = 0; i < MFS_IMAP_PIECE_NUM; i++){
    if (added == 1) break; // check if already added
    int imap_piece_addr = CR->imap[i];
    if (imap_piece_addr == -1) { // case when new piece of imap should be initialized
      new_inode_num = i * MFS_IMAP_PIECE_INODE_NUM;
      MFS_ImapPiece_t new_imap_piece;
      new_imap_piece.inodes[0] = new_inode_addr;
      for (int k = 1; k < MFS_IMAP_PIECE_INODE_NUM; k++)
        new_imap_piece.inodes[k] = -1;
      lseek(fs_image, CR->end_of_log, SEEK_SET);
      write(fs_image, &new_imap_piece, sizeof(MFS_ImapPiece_t));
      CR->imap[i] = CR->end_of_log; // update imap
      CR->end_of_log += sizeof(MFS_ImapPiece_t);  
      break;
    }
    MFS_ImapPiece_t imap_piece;
    lseek(fs_image, imap_piece_addr, SEEK_SET);
    read(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));
    for(int j = 0; j < MFS_IMAP_PIECE_INODE_NUM; j++) {
      if (imap_piece.inodes[j] != -1) continue; // find empty entry
      imap_piece.inodes[j] = new_inode_addr;
      new_inode_num = i * MFS_IMAP_PIECE_INODE_NUM + j;
      lseek(fs_image, imap_piece_addr, SEEK_SET);
      write(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));
      added = 1; // added
      break;
    }
  }

  // create a new directory block at end_of_log if type is MFS_DIRECTORY
  if (type == MFS_DIRECTORY){
    // initialize directory
    MFS_DirBlock_t new_dir;
    strcpy(new_dir.DirEntry[0].name, ".\0");
    new_dir.DirEntry[0].inum = new_inode_num;
    strcpy(new_dir.DirEntry[1].name, "..\0");
    new_dir.DirEntry[1].inum = pinum;
    for(int i = 2; i < MFS_MAX_ENTRIES_PER_DIR; i++)
      new_dir.DirEntry[i].inum = -1;
    // add new directory block to file system image
    lseek(fs_image, CR->end_of_log, SEEK_SET);
    write(fs_image, &new_dir, sizeof(MFS_DirBlock_t));
    CR->end_of_log += sizeof(MFS_DirBlock_t); // update CR->end_of_log for future lseek
    // update new_inode in file system image
    new_inode.data[0] = CR->end_of_log - sizeof(MFS_DirBlock_t);
    new_inode.size = MFS_BLOCK_SIZE;
    lseek(fs_image, new_inode_addr, SEEK_SET);
    write(fs_image, &new_inode, sizeof(MFS_Inode_t));
  } 

  // add name to parent directory
  added = 0; // variable to check whether name has been added to parent directory
  for (int i = 0; i < MFS_INODE_BLOCK_NUM; i++) {
    if (added == 1) break; // check if already added
    int dir_addr = pinode.data[i]; // get address of parent directory block
    if(dir_addr == -1) { // case all previous blocks are filled
      // create a new block of parent direcotry at end_of_log
      MFS_DirBlock_t new_dirBlock;
      new_dirBlock.DirEntry[0].inum = new_inode_num;
      strcpy(new_dirBlock.DirEntry[0].name, name);
      for(int k = 1; k < MFS_MAX_ENTRIES_PER_DIR; k++)
        new_dirBlock.DirEntry[k].inum = -1;
      lseek(fs_image, CR->end_of_log, SEEK_SET);
      write(fs_image, &new_dirBlock, sizeof(MFS_DirBlock_t));
      CR->end_of_log += sizeof(MFS_DirBlock_t);
      // update pinode into file system image
      pinode.data[i] = CR->end_of_log - sizeof(MFS_DirBlock_t); 
      pinode.size += MFS_BLOCK_SIZE;
      lseek(fs_image, pinode_addr, SEEK_SET);
      write(fs_image, &pinode, sizeof(MFS_Inode_t));
      break;
    }
    // read data from given parent directory
    MFS_DirBlock_t pdir_block;
    lseek(fs_image, dir_addr, SEEK_SET);
    read(fs_image, &pdir_block, MFS_BLOCK_SIZE);
    // creat name-inum pair in given parent directory's empty entry
    for(int j = 0; j < MFS_MAX_ENTRIES_PER_DIR; j++) {
      if (pdir_block.DirEntry[j].inum == -1){
        pdir_block.DirEntry[j].inum = new_inode_num;
        strcpy(pdir_block.DirEntry[j].name, name);
        // update parent directory to file system image
        lseek(fs_image, dir_addr, SEEK_SET);
        write(fs_image, &pdir_block, sizeof(MFS_DirBlock_t));
        added = 1; // added
        break;
      }
    }
  }

  // update checkpoint region after change in imap and end_of_log
  lseek(fs_image, 0, SEEK_SET);
  write(fs_image, CR, sizeof(MFS_CR_t));

  fsync(fs_image); // commit changes to disk after write

  return 0;
}


// method used to response to unlink requests
int lfs_unlink(int pinum, char* name){

  if (pinum < 0 || pinum >= MFS_INODE_NUM) return -1; // check if pinum is valid
  
  int inum = lfs_lookup(pinum, name); // get inum with lookup
  if (inum == -1) return 0; // if inum does not exist, return 0 and do nothing

  if (CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM] == -1) return -1; // check if imap piece exists

  // find imap piece
  int imap_piece_addr =  CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  if (imap_piece_addr == -1) return -1; // check if imap piece exists
  MFS_ImapPiece_t imap_piece;
  lseek(fs_image, imap_piece_addr, SEEK_SET);
  read(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));

  // find inode
  int inode_addr = imap_piece.inodes[inum % MFS_IMAP_PIECE_INODE_NUM]; // get address of inode with given inum
  if (inode_addr == -1) return -1; // check if inode exists
  MFS_Inode_t inode;
  lseek(fs_image, inode_addr, SEEK_SET);
  read(fs_image, &inode, sizeof(MFS_Inode_t));

  // if inode to unlink points to a directory, check if directory is empty
  if (inode.type == MFS_DIRECTORY){ 
    for (int i = 0; i < MFS_INODE_BLOCK_NUM; i++){
      int dir_addr = inode.data[i];
      if (dir_addr == -1) continue;
      MFS_DirBlock_t dir_block;
      lseek(fs_image, dir_addr, SEEK_SET);
      read(fs_image, &dir_block, MFS_BLOCK_SIZE);
      int j = 0;
      if (i == 0) j = 2; // skip . and ..
      for(int k = j; k < MFS_MAX_ENTRIES_PER_DIR; k++) {
        if (dir_block.DirEntry[k].inum != -1) return -1; // not empty, unlink fail
      }
    }
  }

  // valid to unlink, set inum to -1 in parent directory and imap
  // find imap piece for parent inode
  int pimap_piece_addr = CR->imap[pinum / MFS_IMAP_PIECE_INODE_NUM]; // get address of corresponding imap piece
  MFS_ImapPiece_t pimap_piece;
  lseek(fs_image, pimap_piece_addr, SEEK_SET);
  read(fs_image, &pimap_piece, sizeof(MFS_ImapPiece_t));

  // find parent inode
  int pinode_addr = pimap_piece.inodes[pinum % MFS_IMAP_PIECE_INODE_NUM]; // get address of parent inode with given inum
  MFS_Inode_t pinode;
  lseek(fs_image, pinode_addr, SEEK_SET);
  read(fs_image, &pinode, sizeof(MFS_Inode_t));

  for (int i = 0; i < MFS_INODE_BLOCK_NUM; i++){
    int dir_addr = pinode.data[i]; // get address of parent directory
    // read data from given parent directory
    MFS_DirBlock_t pdir_block;
    lseek(fs_image, dir_addr, SEEK_SET);
    read(fs_image, &pdir_block, MFS_BLOCK_SIZE);
    // search for given name
    for(int j = 0; j < MFS_MAX_ENTRIES_PER_DIR; j++) {
      if (pdir_block.DirEntry[j].inum == -1) continue; // skip empty DirEntry
      if (strcmp(pdir_block.DirEntry[j].name, name) == 0){
	pdir_block.DirEntry[j].inum = -1; // unlink
        strcpy(pdir_block.DirEntry[j].name, "\0");
        // update parent directory in file system image
        lseek(fs_image, dir_addr, SEEK_SET);
        write(fs_image, &pdir_block, sizeof(MFS_DirBlock_t));
        break;
      }
    }
  }
  
  // change imap and imap piece corresponding to the unlink
  imap_piece.inodes[inum % MFS_IMAP_PIECE_INODE_NUM] = -1;
  // update imap piece in file system image
  lseek(fs_image, imap_piece_addr, SEEK_SET);
  write(fs_image, &imap_piece, sizeof(MFS_ImapPiece_t));

  // check if imap piece is empty after unlink
  int empty_piece = 1; // 1 stand for empty imap piece
  for(int i = 0; i < MFS_IMAP_PIECE_INODE_NUM; i++){
    if (imap_piece.inodes[i] != -1) {
      empty_piece = 0; // 0 stand for not empty imap piece
      break;
    }
  }
  if (empty_piece == 1) {
    // imap piece becomes empty after unlink, update
    CR->imap[inum / MFS_IMAP_PIECE_INODE_NUM] = -1;
    lseek(fs_image, 0, SEEK_SET);
    write(fs_image, CR, sizeof(MFS_CR_t));
  }

  fsync(fs_image); // commit changes to disk after write
  
  return 0;
}


// method to shutdown the server
int lfs_shutdown() {
  fsync(fs_image); // force file image to disk
  exit(0);
}


// main method call init to run the server
int main(int argc, char *argv[]) {
  // check if the command line argument is correct
  if(argc != 3) {
    printf("Usage: server [portnum] [file-system-image]\n");
    return -1;
  }

  // run the server
  lfs_init(atoi(argv[1]),argv[2]);

  return 0;
}
