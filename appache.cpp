// ------------------------------------------------------------
// appache.cpp : Definiert den Einstiegspunkt für die Konsolenanwendung.
//
// Copyright(C) 2015  Albert Zedlitz
//
// This program is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
// ------------------------------------------------------------
// #define AP_HAVE_DESIGNATED_INITIALIZER

// #include <iostream>
#include <Winsock2.h>
#include <Ws2ipdef.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <locale>
#include <codecvt>
#include <direct.h>
#include <exception>
#include <hash_map>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <strstream>
#include <fstream> 
#include <iostream>
#include <map>

#include "apr_hash.h"
#include "apr_strings.h"
#include "apr_sha1.h"
#include "apr_base64.h"
#include "apr_tables.h"
#include "apr_pools.h"
#include "apr_file_io.h"

#include "ap_config.h"
#include "ap_provider.h"

#include "httpd.h"
#include "http_core.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"

#undef _DEBUG

using namespace std;

static   mutex  aMutex;
static   condition_variable mCv;
static   condition_variable mReady;
static   mutex   aPythonMtx;
static   mutex   aReadyMtx;
static   bool    aPythonBusy = true;
static   thread *mThread;

class TWebSocketException : public exception {
public:
    TWebSocketException() {};
};

// ------------------------------------------------------------
// ------------------------------------------------------------
typedef struct CEezzConfig {
    int            mWebsocket;
    string         mHostname;

    CEezzConfig() {
        mHostname  = "localhost";
        mWebsocket = 8401;
    }
} TEezzConfig;

typedef struct {
    int           mState;
    apr_pool_t   *mPool;
    apr_pollfd_t  mClientPfd;
    apr_pollfd_t  mServerPfd;
    apr_socket_t *mServer;
    apr_socket_t *mClient;
    bool          mFinal;
    bool          mMasked;
    unsigned char mOpcode;
    char          mVector[4];
    char          mBuffer[65536 * 2];
    UINT64        mPayload;
    UINT64        mRest;

} TConnection;

static TEezzConfig gConfig;

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setWebsocket(cmd_parms *cmd, void *cfg, const char *arg) {
    istrstream iss(arg);
    iss >> gConfig.mWebsocket;
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
static const command_rec directives[] = {
    AP_INIT_TAKE1("Websocket",  reinterpret_cast<cmd_func>(setWebsocket),  NULL, ACCESS_CONF, "set websocket port"),
    { NULL }
};

// ------------------------------------------------------------
// ------------------------------------------------------------
extern "C" static void register_hooks(apr_pool_t *pool);
extern "C" module AP_MODULE_DECLARE_DATA eezz_websocket_module = {
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    register_hooks
};

// ------------------------------------------------------------
// ------------------------------------------------------------
void genHandshake(request_rec *r) {
    string         x64Key;
    apr_size_t     xLen;
    char           x64Hash[APR_SHA1_DIGESTSIZE * 3];
    unsigned char  xDigest[APR_SHA1_DIGESTSIZE];
    apr_sha1_ctx_t aSha1;

    x64Key = apr_table_get(r->headers_in, "Sec-WebSocket-Key");
    x64Key.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    apr_sha1_init(&aSha1);
    apr_sha1_update(&aSha1, x64Key.c_str(), x64Key.length());
    apr_sha1_final(xDigest, &aSha1);

    xLen = apr_base64_encode(x64Hash, (const char*)xDigest, APR_SHA1_DIGESTSIZE);

    apr_table_clear(r->headers_out);
    apr_table_set(r->headers_out, "Connection", "Upgrade");
    apr_table_set(r->headers_out, "Upgrade", "websocket");
    apr_table_set(r->headers_out, "Sec-WebSocket-Accept", x64Hash);

    r->status = HTTP_SWITCHING_PROTOCOLS;
    r->status_line = ap_get_status_line(r->status);
    r->connection->keepalive = AP_CONN_KEEPALIVE;
    ap_send_interim_response(r, 1);
}

// ------------------------------------------------------------
// ------------------------------------------------------------
void writeHandshake(request_rec *r, TConnection *aConnection) {
    apr_status_t xState;
    char         xBuffer[4096];

    apr_table_set(r->headers_out, "Upgrade", "peezz");
    const apr_array_header_t *tarr = apr_table_elts(r->headers_out);
    const apr_table_entry_t *telts = (const apr_table_entry_t*)tarr->elts;
    int i;

    ostrstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n";
    for (i = 0; i < tarr->nelts; i++) {
        oss << telts[i].key << ":" << telts[i].val << "\r\n";
    }
    oss << "\r\n";
    apr_size_t aLen = oss.pcount();
    xState = apr_socket_send(aConnection->mClient, oss.str(), &aLen);
    if (xState != APR_SUCCESS) {
        throw(TWebSocketException());
    }
    aLen   = sizeof(xBuffer);
    xState = apr_socket_recv(aConnection->mClient, xBuffer, &aLen);
    if (xState != APR_SUCCESS) {
        throw(TWebSocketException());
    }
}

// ------------------------------------------------------------
// ------------------------------------------------------------
typedef union {
    char          mBuffer[8];
    apr_int16_t   mShort;
    apr_int64_t   mLong;
} TBytes;

// ------------------------------------------------------------
// ------------------------------------------------------------
void readHeader(TConnection *xConnection) {
    TBytes       xBytes;
    apr_status_t xState;
    apr_size_t   xLen;

    xLen = 2;
    xState = apr_socket_recv(xConnection->mServer, xBytes.mBuffer, &xLen);
    if (xLen <= 0) {
        throw(TWebSocketException());
    }

    xConnection->mFinal = ((1 << 7) & xBytes.mBuffer[0]) != 0;
    xConnection->mOpcode = xBytes.mBuffer[0] & 0xf;
    xConnection->mMasked = ((1 << 7) & xBytes.mBuffer[1]) != 0;
    xConnection->mPayload = (int)(xBytes.mBuffer[1] & 0x7f);

    if (xConnection->mPayload == 126) {
        xBytes.mLong = 0;
        xState = apr_socket_recv(xConnection->mServer, xBytes.mBuffer, &xLen);
        xConnection->mPayload = ntohs(xBytes.mLong);
    }
    else if (xConnection->mPayload == 127) {
        xBytes.mLong = 0;
        xState = apr_socket_recv(xConnection->mServer, xBytes.mBuffer, &xLen);
        xConnection->mPayload = ntohll(xBytes.mLong);
    }

    if (xLen <= 0) {
        throw(TWebSocketException());
    }

    if (xConnection->mMasked) {
        xLen = 4;
        xState = apr_socket_recv(xConnection->mServer, xConnection->mVector, &xLen);
        if (xLen != 4) {
            throw(TWebSocketException());
        }
    }
}

// ------------------------------------------------------------
// ------------------------------------------------------------
bool readFrame(TConnection *xConnection) {
    apr_size_t   xLen;
    apr_size_t   xRest;
    apr_status_t xState;
    ostrstream   oss;
    bool         mFinal = false;

    if (xConnection->mRest == 0) {
        return true;
    }

    xLen = sizeof(xConnection->mBuffer);
    xState = apr_socket_recv(xConnection->mServer, xConnection->mBuffer, &xLen);
    if (xLen <= 0) {
        throw(TWebSocketException());
    }

    xConnection->mRest -= xLen;

    if (xConnection->mMasked) {
        for (int i = 0; i < xLen; i++) {
            xConnection->mBuffer[i] ^= xConnection->mVector[i % 4];
        }
    }
    return (xConnection->mRest == 0);
}

// ------------------------------------------------------------
// ------------------------------------------------------------
void writeFrame(TConnection *xConnection, bool aMasked = false) {
    TBytes       xBytes;
    char         xBuffer[16];
    int          xPos = 0;
    int          xMasked = 0;
    apr_size_t   xLen;
    apr_status_t xState;

    if (xConnection->mFinal) {
        xBuffer[xPos++] = (1 << 7) | xConnection->mOpcode;
    }
    else {
        xBuffer[xPos++] = xConnection->mOpcode;
    }

    if (aMasked) {
        xMasked = 1 << 7;
    }

    if (xConnection->mPayload >= 126) {
        if (xConnection->mPayload < 0xffff) {
            xBuffer[xPos++] = 0x7e | xMasked;
            xBytes.mShort = htons((unsigned short)xConnection->mPayload);
            for (int i = 0; i < 2; i++) {
                xBuffer[xPos++] = xBytes.mBuffer[i];
            }
        }
        else {
            xBuffer[xPos++] = 0x7f | xMasked;
            xBytes.mLong = htonll(xConnection->mPayload);
            for (int i = 0; i < 8; i++) {
                xBuffer[xPos++] = xBytes.mBuffer[i];
            }
        }
    }
    else {
        xBuffer[xPos++] = xConnection->mPayload | xMasked;
    }

    if (aMasked) {
        int i;
        for (i = 0; i < 4; i++) {  
            xBuffer[xPos++] = xConnection->mVector[i];
        }

        for (i = 0; i < xConnection->mPayload; i++) {
            xConnection->mBuffer[i] ^= xConnection->mVector[i % 4];
        }
    }

    xLen   = xPos;
    xState = apr_socket_send(xConnection->mServer, xBuffer, &xLen);
    if (xState != APR_SUCCESS) {
        throw(TWebSocketException());
    }
    xLen   = xConnection->mPayload;
    xState = apr_socket_send(xConnection->mServer, xConnection->mBuffer, &xLen);
    if (xState != APR_SUCCESS) {
        throw(TWebSocketException());
    }
}

// ------------------------------------------------------------
// ------------------------------------------------------------
void wsproxy(request_rec *r, TConnection *xConnection) {
    apr_size_t      xLen;
    apr_pollset_t  *xPollset;
    apr_pollfd_t    xPfd;
    apr_pollfd_t   *xRetPfd;
    apr_int32_t     xNum;
    apr_status_t    xState;
    apr_sockaddr_t *xAddress;

    apr_pollset_create(&xPollset, 32, r->pool, 0);

    xConnection->mServer = ap_get_conn_socket(r->connection);
    xPfd = { r->pool, APR_POLL_SOCKET, APR_POLLIN, 0, { NULL }, xConnection };
    xPfd.desc.s = xConnection->mServer;
    apr_pollset_add(xPollset, &xPfd);

    xState = apr_sockaddr_info_get(&xAddress, "localhost", APR_INET, gConfig.mWebsocket, 0, r->pool);
    if (xState != APR_SUCCESS) {
        throw(TWebSocketException());
    }
    xState = apr_socket_create(&(xConnection->mClient), APR_INET, SOCK_STREAM, APR_PROTO_TCP, r->pool);
    xState = apr_socket_connect(xConnection->mClient, xAddress);
    if (xState != APR_SUCCESS) {
        xConnection->mClient = NULL;
        throw(TWebSocketException());
    }
    
    xPfd = { r->pool, APR_POLL_SOCKET, APR_POLLIN, 0, { NULL }, xConnection };
    xPfd.desc.s = xConnection->mClient;
    apr_pollset_add(xPollset, &xPfd);

    xConnection->mServerPfd = xPfd;
    xConnection->mClientPfd = xPfd;
    writeHandshake(r, xConnection);

    for (;;) {
        xState = apr_pollset_poll(xPollset, (APR_USEC_PER_SEC * 30), &xNum, (const apr_pollfd_t**)&xRetPfd);
        if (xState != APR_SUCCESS) {
            continue;
        }

        for (int i = 0; i < xNum; i++) {
            TConnection *aConnection = (TConnection*)xRetPfd[i].client_data;

            if (xRetPfd[i].desc.s == aConnection->mServer) {
                readHeader(aConnection);
                int xOpcode = aConnection->mOpcode;

                if (xOpcode == 0x9) {
                    aConnection->mOpcode = 0xA;
                    readFrame(aConnection);
                    writeFrame(aConnection);
                }

                if (xOpcode == 0xA) {
                    aConnection->mOpcode = 0x9;
                    readFrame(aConnection);
                    writeFrame(aConnection);
                }

                if (xOpcode == 0x1 || xOpcode == 0x2) {
                    readFrame(aConnection);
                    xLen = aConnection->mPayload;
                    xState = apr_socket_send(aConnection->mClient, aConnection->mBuffer, &xLen);
                    if (xState != APR_SUCCESS) {
                        throw(TWebSocketException());
                    }
                }
            }

            if (xRetPfd[i].desc.s == aConnection->mClient) {
                xLen = sizeof(aConnection->mBuffer);
                xState = apr_socket_recv(aConnection->mClient, aConnection->mBuffer, &xLen);

                if (xState != APR_SUCCESS) {
                    throw(TWebSocketException());
                }
                aConnection->mFinal = true;
                aConnection->mOpcode = 1;
                aConnection->mPayload = xLen;
                writeFrame(aConnection);
            }
        }
    }
}

// ------------------------------------------------------------
// ------------------------------------------------------------
extern "C" static int eezz_handler(request_rec *r) {
    TConnection     xConnection;
    static bool     aInitialized = false;
    int             aReturn;
    apr_status_t    xState;
    apr_sockaddr_t *xAddress;
    char            xBuffer[1024];
    char            aFileBuf[4096];
    apr_size_t      xLen;
    apr_file_t     *xFile;
    apr_file_t     *xWsDef;
    size_t xPos;


    if (!r->handler) {
        return DECLINED;
    }
    const char *xUpgrade = apr_table_get(r->headers_in, "Upgrade");

    // Generate the base document injecting the websocket scripts
    if (strcmp(r->handler, "eezz_websocket") == 0) {
        if (xUpgrade == NULL) {
            string xPath = apr_pstrdup(r->pool, r->filename);
            xPos = xPath.find_last_of('/');
            xPath = xPath.substr(0, xPos) + "/../resources/websocket.js";

            ifstream xifs = ifstream(r->filename);
            ifstream xiws = ifstream(xPath);
            ostrstream xbuf;

            while (xifs.good()) {
                xifs.getline(aFileBuf, 4096);
                string xBuff = aFileBuf;

                if ((xPos = xBuff.find("{websocket}")) != string::npos) {
                    xbuf << xBuff.substr(0, xPos);
                    xbuf << "var gSocketAddr   = \"ws://localhost:80\";" << endl;
                    xbuf << "var eezzArguments = \"\";" << endl;

                    while (xiws.good()) {
                        xiws.getline(aFileBuf, 4096);
                        xbuf << aFileBuf << endl;
                    }
                    xbuf << xBuff.substr(xPos + 11) << endl;
                }
                else {
                    xbuf << xBuff << endl;
                }
            }
            xbuf << ends;
            ap_rputs(xbuf.str(), r);
            return OK;
        }
    }

    if (xUpgrade == NULL || strcmp(xUpgrade, "websocket") != 0) {
        return DECLINED;
    }

    try {
        genHandshake(r);
        // ap_switch_protocol(r->connection, r, r->server, "websocket");
        wsproxy(r, &xConnection);
    }
    catch (TWebSocketException &e) {
        if (xConnection.mClient != NULL) {
            apr_socket_close(xConnection.mClient);
        }
    }

    return OK;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
extern "C" static void register_hooks(apr_pool_t *pool) {
    ap_hook_handler(eezz_handler, NULL, NULL, APR_HOOK_LAST);
}


// ------------------------------------------------------------
// ------------------------------------------------------------
int main(int argc, char* argv[]) {
    return 0;
}
