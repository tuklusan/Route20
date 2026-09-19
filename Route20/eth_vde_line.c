/* eth_vde_line.c: Ethernet VDE line
------------------------------------------------------------------------------

Copyright (c) 2014, Robert M. A. Jarratt

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

------------------------------------------------------------------------------*/

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if !defined(WIN32)
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#include "platform.h"
#include "route20.h"
#include "eth_decnet.h"
#include "eth_line.h"
#include "eth_vde_line.h"

#define VDE_MAX_PACKET 1518
#define VDE_MIN_PACKET 60

#if !defined(WIN32)
typedef struct
{
    int port;
    char *group;
    unsigned int mode;
} vde_open_args_t;

typedef void *(*vde_open_fn)(char *, char *, vde_open_args_t *);
typedef void *(*vde_open_real_fn)(char *, char *, int, vde_open_args_t *);
typedef int (*vde_datafd_fn)(void *);
typedef int (*vde_close_fn)(void *);
typedef ssize_t (*vde_recv_fn)(void *, void *, size_t, int);
typedef ssize_t (*vde_send_fn)(void *, const void *, size_t, int);

typedef struct
{
    void *library;
    void *connection;
    vde_open_fn open;
    vde_open_real_fn open_real;
    vde_datafd_fn datafd;
    vde_close_fn close;
    vde_recv_fn recv;
    vde_send_fn send;
} eth_vde_t;

static int EthVdeLoad(eth_vde_t *context)
{
    context->library = dlopen("libvdeplug.so.2", RTLD_NOW | RTLD_LOCAL);
    if (context->library == NULL)
        context->library = dlopen("libvdeplug.so", RTLD_NOW | RTLD_LOCAL);
    if (context->library == NULL)
        context->library = dlopen("libvdeplug.so.3", RTLD_NOW | RTLD_LOCAL);
    if (context->library == NULL)
        return 0;

    /*
     * Modern libvdeplug exposes vde_open as a source-level macro whose ABI
     * entry point is vde_open_real.  Older VDE releases exported vde_open.
     */
    context->open_real = (vde_open_real_fn)dlsym(context->library, "vde_open_real");
    if (context->open_real == NULL)
        context->open = (vde_open_fn)dlsym(context->library, "vde_open");
    context->datafd = (vde_datafd_fn)dlsym(context->library, "vde_datafd");
    context->close = (vde_close_fn)dlsym(context->library, "vde_close");
    context->recv = (vde_recv_fn)dlsym(context->library, "vde_recv");
    context->send = (vde_send_fn)dlsym(context->library, "vde_send");
    if ((context->open_real == NULL && context->open == NULL) ||
        context->datafd == NULL || context->close == NULL ||
        context->recv == NULL || context->send == NULL)
    {
        dlclose(context->library);
        memset(context, 0, sizeof(*context));
        return 0;
    }
    return 1;
}
#endif

int EthVdeLineStart(line_t *line)
{
#if defined(WIN32)
    Log(LogEthVdeLine, LogError, "VDE Ethernet is not available on Windows\n");
    return 0;
#else
    eth_vde_t *context;
    Log(LogEthVdeLine, LogInfo, "Starting VDE line %s\n", line->name);
    context = (eth_vde_t *)calloc(1, sizeof(*context));
    if (context == NULL)
        return 0;
    line->lineContext = context;

    if (!EthVdeLoad(context))
    {
        Log(LogEthVdeLine, LogError, "Could not load libvdeplug\n");
        EthVdeLineStop(line);
        return 0;
    }

    if (context->open_real != NULL)
        context->connection = context->open_real(line->name, "Route20", 1, NULL);
    else
        context->connection = context->open(line->name, "Route20", NULL);
    if (context->connection == NULL)
    {
        Log(LogEthVdeLine, LogError, "Could not open VDE network %s\n", line->name);
        EthVdeLineStop(line);
        return 0;
    }

    line->waitHandle = context->datafd(context->connection);
    if (line->waitHandle < 0)
    {
        Log(LogEthVdeLine, LogError, "Could not get VDE data descriptor for %s\n", line->name);
        EthVdeLineStop(line);
        return 0;
    }

    RegisterEventHandler(line->waitHandle, "EthVde Line", line,
                         line->LineWaitEventHandler);
    QueueImmediate(line, (void (*)(void *))(line->LineUp));
    return 1;
#endif
}

void EthVdeLineStop(line_t *line)
{
#if !defined(WIN32)
    eth_vde_t *context = (eth_vde_t *)line->lineContext;
    if (context == NULL)
        return;
    if (context->connection != NULL && context->close != NULL)
        context->close(context->connection);
    if (context->library != NULL)
        dlclose(context->library);
    free(context);
    line->lineContext = NULL;
#else
    (void)line;
#endif
}

packet_t *EthVdeLineReadPacket(line_t *line)
{
#if defined(WIN32)
    (void)line;
    return NULL;
#else
    eth_vde_t *context = (eth_vde_t *)line->lineContext;
    static byte data[VDE_MAX_PACKET];
    static packet_t packet;
    ssize_t len;

    if (context == NULL || context->connection == NULL)
        return NULL;

    len = context->recv(context->connection, data, sizeof(data), MSG_DONTWAIT);
    if (len < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            Log(LogEthVdeLine, LogError, "Error reading VDE network %s: %s\n",
                line->name, strerror(errno));
        return NULL;
    }
    if (len == 0)
        return NULL;

    memset(&packet, 0, sizeof(packet));
    packet.rawData = data;
    packet.rawLen = (int)len;
    packet.IsDecnet = EthPcapIsDecnet;
    if (!EthValidPacket(&packet))
    {
        line->stats.invalidPacketsReceived++;
        return NULL;
    }
    if (!packet.IsDecnet(&packet))
        return NULL;

    GetDecnetAddress((decnet_eth_address_t *)&packet.rawData[0], &packet.to);
    GetDecnetAddress((decnet_eth_address_t *)&packet.rawData[6], &packet.from);
    EthSetPayload(&packet);
    line->stats.validPacketsReceived++;
    return &packet;
#endif
}

int EthVdeLineWritePacket(line_t *line, packet_t *packet)
{
#if defined(WIN32)
    (void)line;
    (void)packet;
    return 0;
#else
    eth_vde_t *context = (eth_vde_t *)line->lineContext;
    byte smallBuf[VDE_MIN_PACKET];
    byte *data;
    int len;
    ssize_t sent;

    if (context == NULL || context->connection == NULL || packet == NULL ||
        packet->rawData == NULL || packet->rawLen <= 0)
        return 0;

    data = packet->rawData;
    len = packet->rawLen;
    if (len < VDE_MIN_PACKET)
    {
        memset(smallBuf, 0, sizeof(smallBuf));
        memcpy(smallBuf, data, len);
        data = smallBuf;
        len = VDE_MIN_PACKET;
    }

    sent = context->send(context->connection, data, (size_t)len, 0);
    if (sent != len)
    {
        Log(LogEthVdeLine, LogError, "Error writing VDE network %s\n", line->name);
        return 0;
    }
    return 1;
#endif
}
