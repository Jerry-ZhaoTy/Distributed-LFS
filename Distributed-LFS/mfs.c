#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "udp.h"
#include "mfs.h"

struct sockaddr_in addr, return_addr;

int MFS_Transmit_Helper(Packet *send_packet, Packet *return_packet) {

    int fd = UDP_Open(0);
    if (fd < -1) return -1;

    fd_set rfds;
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        UDP_Write(fd, &addr, (char*)send_packet, sizeof(Packet));
        if (select(fd+1, &rfds, NULL, NULL, &tv)){
            if (UDP_Read(fd, &return_addr, (char*)return_packet, sizeof(Packet)) > 0){
                UDP_Close(fd);
                return 0;
            }
        }
    }
}

int MFS_Init(char *hostname, int port) {
    if (UDP_FillSockAddr(&addr, hostname, port) == -1) return -1;
    return 0;
}

int MFS_Lookup(int pinum, char *name){

    Packet send_packet;
    Packet return_packet;
    send_packet.inum = pinum;
    strcpy(send_packet.name, name);
    send_packet.request = LOOKUP;

    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;

    return return_packet.return_val;
}

int MFS_Stat(int inum, MFS_Stat_t *m) {

    Packet send_packet;
    Packet return_packet;
    send_packet.inum = inum;
    send_packet.request = STAT;
	
    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;

    if (return_packet.return_val == -1) return -1;
    else {
	m->type = return_packet.stat.type;
        m->size = return_packet.stat.size;
	return 0;
    }
}

int MFS_Write(int inum, char *buffer, int block){
	
    Packet send_packet;
    Packet return_packet;
    send_packet.inum = inum;
    for(int i = 0; i < MFS_BLOCK_SIZE; i++)
        send_packet.buffer[i] = buffer[i];
    send_packet.block = block;
    send_packet.request = WRITE;
	
    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;
	
    return return_packet.return_val;
}

int MFS_Read(int inum, char *buffer, int block){

    Packet send_packet;
    Packet return_packet;
    send_packet.inum = inum;
    send_packet.block = block;
    send_packet.request = READ;

    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;

    if (return_packet.return_val == -1) return -1;
    else{
        for(int i = 0; i < MFS_BLOCK_SIZE; i++)
      	    buffer[i] = return_packet.buffer[i];
  	return 0;
    }
}

int MFS_Creat(int pinum, int type, char *name){
	
    Packet send_packet;
    Packet return_packet;
    send_packet.inum = pinum;
    send_packet.type = type;
    strcpy(send_packet.name, name);
    send_packet.request = CREAT;

    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;

    return return_packet.return_val;
}

int MFS_Unlink(int pinum, char *name){
	
    Packet send_packet;
    Packet return_packet;
    send_packet.inum = pinum;
    strcpy(send_packet.name, name);
    send_packet.request = UNLINK;

    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;

    return return_packet.return_val;
}

int MFS_Shutdown(){

    Packet send_packet;
    Packet return_packet;
    send_packet.request = SHUTDOWN;

    if (MFS_Transmit_Helper(&send_packet, &return_packet) < 0) return -1;
	
    return 0;
}
