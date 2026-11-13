#ifndef WONDER_TRADE_DP_SDK_SOCL_H
#define WONDER_TRADE_DP_SDK_SOCL_H

/* The socket calls the shared network core makes, declared for Diamond and
 * Pearl's static SOC library (arm9/asm/SOC_*.s). A socket handle is the
 * address of its socket record, as SOCL_CreateSocket returns it. */

#include <nitro.h>

typedef u32 SOCLInAddr;

/* Error codes the assembly returns: would block (SOCLi_ReadFrom), connection
 * in progress (SOCLiResultCodeInConnecting) and already connecting. */
#define SOCL_EWOULDBLOCK (-6)
#define SOCL_EALREADY (-7)
#define SOCL_EINPROGRESS (-26)

/* SOCLi_ReadFrom and SOCL_WriteTo return at once when flags has this bit. */
#define SOCL_MSG_DONTWAIT 0x04

/* The TCP creation parameters SOC_Socket also uses (SOC_soc.s). */
extern u8 SOCLSocketParamTCP[];

int SOCL_CreateSocket(const void *param);
int SOCL_Connect(int socket, u16 port, SOCLInAddr address);
int SOCLi_ReadFrom(int socket, void *buffer, int size, u16 *port, SOCLInAddr *address, int flags);
int SOCL_WriteTo(int socket, const void *buffer, int size, u16 port, SOCLInAddr address, int flags);
int SOCL_Close(int socket);
int SOCL_IsClosed(int socket);
void SOCLi_TrashSocket(void);

static inline int SOCL_TcpSocket(void)
{
    return SOCL_CreateSocket(SOCLSocketParamTCP);
}

/* SOCLi_InitSocket copies the creation parameters' blocking mode to byte
 * 0x72 of the record, which SOCL_Connect and the read and write paths test;
 * zero is nonblocking. */
static inline void SOCL_SetNoBlock(int socket)
{
    ((volatile s8 *)socket)[0x72] = 0;
}

static inline int SOCL_ReadNoBlock(int socket, void *buffer, int size, u16 *port, SOCLInAddr *address)
{
    return SOCLi_ReadFrom(socket, buffer, size, port, address, SOCL_MSG_DONTWAIT);
}

static inline int SOCL_WriteNoBlock(int socket, const void *buffer, int size, u16 port, SOCLInAddr address)
{
    return SOCL_WriteTo(socket, buffer, size, port, address, SOCL_MSG_DONTWAIT);
}

#endif
