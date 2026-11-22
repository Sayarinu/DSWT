#ifndef WONDER_TRADE_BW_SDK_SOCL_H
#define WONDER_TRADE_BW_SDK_SOCL_H

/* NitroWiFi's sockets in overlay 9. The SOC_* calls take SOCL's own socket
 * handles, so the few SOCL helpers the core uses map onto them. */

#include <nitro.h>

typedef u32 CPSInAddr;

#define SOC_AF_INET 2
#define SOC_SOCK_STREAM 1
#define SOC_F_SETFL 4
#define SOC_O_NONBLOCK 0x04
#define SOC_EWOULDBLOCK (-6)
#define SOCL_EWOULDBLOCK SOC_EWOULDBLOCK
#define SOCL_EALREADY (-7)
#define SOCL_EINPROGRESS (-26)

int SOC_Socket(int domain, int type, int protocol);
int SOC_Fcntl(int socket, int command, int mode);
int SOC_Recv(int socket, void *buffer, int length, int flags);
int SOC_Send(int socket, const void *buffer, int length, int flags);
int SOCL_Connect(int socket, u16 port, CPSInAddr address);
int SOCL_Close(int socket);
BOOL SOCL_IsClosed(int socket);
void SOCLi_TrashSocket(void);

int WtSocl_TcpSocket(void);
int WtSocl_SetNoBlock(int socket);

#define SOCL_TcpSocket() WtSocl_TcpSocket()
#define SOCL_SetNoBlock(socket) WtSocl_SetNoBlock(socket)
#define SOCL_ReadNoBlock(socket, buffer, size, from, port) SOC_Recv((socket), (buffer), (int)(size), 0)
#define SOCL_WriteNoBlock(socket, buffer, size, port, address) SOC_Send((socket), (buffer), (int)(size), 0)

#endif
