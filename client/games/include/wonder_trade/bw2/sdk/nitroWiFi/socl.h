#ifndef WONDER_TRADE_BW2_SDK_SOCL_H
#define WONDER_TRADE_BW2_SDK_SOCL_H

/* NitroWiFi 3.3's sockets in overlay 11. The SOC_* calls of soc.c take SOCL's
 * own socket handles, so the few SOCL helpers the core uses map onto them. */

#include "nitrowifi/soc.h"

#define SOCL_EWOULDBLOCK SOC_EWOULDBLOCK

int SOCL_Connect(int socket, u16 port, CPSInAddr address);
int SOCL_Close(int socket);
BOOL SOCL_IsClosed(int socket);

int WtSocl_TcpSocket(void);
int WtSocl_SetNoBlock(int socket);

#define SOCL_TcpSocket() WtSocl_TcpSocket()
#define SOCL_SetNoBlock(socket) WtSocl_SetNoBlock(socket)
#define SOCL_ReadNoBlock(socket, buffer, size, from, port) SOC_Recv((socket), (buffer), (int)(size), 0)
#define SOCL_WriteNoBlock(socket, buffer, size, port, address) SOC_Send((socket), (buffer), (int)(size), 0)

#endif
