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
// along with this program.If not, see <http://www.gnu.org/licenses/>.
// ------------------------------------------------------------
// #define AP_HAVE_DESIGNATED_INITIALIZER

// #include <iostream>
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
#include <fstream> 

#include "apr_hash.h"
#include "ap_config.h"
#include "ap_provider.h"
#include "httpd.h"
#include "http_core.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"

#undef _DEBUG
#include <Python.h>

using namespace std;
static   mutex  aMutex;
static   condition_variable mCv;
static   condition_variable mReady;
static   mutex   aPythonMtx;
static   mutex   aReadyMtx;
static   bool    aPythonBusy = true;
static   thread *mThread;
typedef  hash_map<string, PyObject*> TPyHash;

// ------------------------------------------------------------
// ------------------------------------------------------------
typedef struct {
    vector<string> mPathList;
    int            mWebsocket;
    string         mHostname;
    string         mDocumentRoot;
    request_rec   *mRequest;
} TEezzConfig;

// ------------------------------------------------------------
// Takes a mutex and tries to lock it. I would also accept a
// mutex which is already locked. 
// Unlocks the given mutex at the end of the scope.
// ------------------------------------------------------------
class TGuard {
private:
    mutex *mMutex;
public:
    TGuard(mutex *aMutex) {
        mMutex = aMutex;
        mMutex->try_lock();
    }
    ~TGuard() {
        mMutex->unlock();
    }
};

// ------------------------------------------------------------
// ------------------------------------------------------------
class TGuardObjects {
private:
    TPyHash *aPythonObjects;
public:
    TGuardObjects(TPyHash *aObjects) {
        aPythonObjects = aObjects;
    }
    ~TGuardObjects() {
        for (auto xEntry : *aPythonObjects) {
            Py_XDECREF(xEntry.second);
        }
    }
};

static TEezzConfig mConfig;

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setPythonPath(cmd_parms *cmd, void *cfg, const char *arg) {
    istringstream aPath(arg);
    string        aSegment;

    while (std::getline(aPath, aSegment, ':')) {
        mConfig.mPathList.push_back(aSegment);
    }
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setWebsocket(cmd_parms *cmd, void *cfg, const char *arg) {
    mConfig.mWebsocket = atoi(arg);
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
const char *setHostname(cmd_parms *cmd, void *cfg, const char *arg) {
    mConfig.mHostname = arg;
    return NULL;
}

// ------------------------------------------------------------
// ------------------------------------------------------------
static const command_rec directives[] = {
    AP_INIT_TAKE1("PythonPath", reinterpret_cast<cmd_func>(setPythonPath), NULL, ACCESS_CONF, "set the python path"),
    AP_INIT_TAKE1("Websocket",  reinterpret_cast<cmd_func>(setWebsocket),  NULL, ACCESS_CONF, "set websocket port"),
    AP_INIT_TAKE1("WsHostname", reinterpret_cast<cmd_func>(setHostname),   NULL, ACCESS_CONF, "set websocket host"),
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
    directives,
    register_hooks
};


// ------------------------------------------------------------
// Define an exception to interrupt Python processing
// and decrement reference to allocated variables
// ------------------------------------------------------------
class TPyExcept : public exception {
    string mMessage;
public:
    TPyExcept(string aMessage) {
        mMessage = aMessage;
    }
    virtual const char* what() const throw() {
        return mMessage.c_str();
    }
};

// ------------------------------------------------------------
// The Python thread
// Python would not allow to change threads, so all calls 
// have to be gathered in this method
// This makes sychonization necessary using conditions, 
// mutex and a wakeup socket
// ------------------------------------------------------------
void runPython() {
    TPyHash aPyHashMap;
    Py_SetProgramName(L"eezz_websocket_handler");
    Py_Initialize();

    aPyHashMap["sys.Import"]    = Py_BuildValue("s", "sys");
    aPyHashMap["sys.Module"]    = PyImport_Import(aPyHashMap["sys.Import"]);
    aPyHashMap["sys.path"]      = PyObject_GetAttrString(aPyHashMap["sys.Module"], "path");

    for (string aSegment : mConfig.mPathList) {
        PyObject_CallMethod(aPyHashMap["sys.path"], "append", "s", aSegment.c_str());
    }

    // start websocket
    aPyHashMap["websocket.Import"] = Py_BuildValue("s", "eezz.websocket");
    aPyHashMap["websocket.Module"] = PyImport_Import(aPyHashMap["websocket.Import"]);

    if (aPyHashMap["websocket.Module"] == NULL) {
        throw TPyExcept("");
    }
    //ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "load eezz.websocket");
    aPyHashMap["TWebSocket"]    = PyObject_GetAttrString(aPyHashMap["websocket.Module"], "TWebSocket");
    aPyHashMap["TWakeup"]       = PyObject_GetAttrString(aPyHashMap["websocket.Module"], "TWakeup");
    aPyHashMap["aCvExtern"]     = PyObject_GetAttrString(aPyHashMap["TWakeup"], "mCvExtern");

    aPyHashMap["arg.Address"]   = Py_BuildValue("(si)", mConfig.mHostname.c_str(), mConfig.mWebsocket);
    aPyHashMap["arg.Arguments"] = Py_BuildValue("(S)", aPyHashMap["arg.Address"]);
    aPyHashMap["aWebSocket"]    = PyObject_CallObject(aPyHashMap["TWebSocket"], aPyHashMap["arg.Arguments"]);
    aPyHashMap["aWakeup"]       = PyObject_CallObject(aPyHashMap["TWakeup"], NULL);

    if (aPyHashMap["aWebSocket"] == NULL) {
        throw TPyExcept("");
    }
    PyObject_CallMethod(aPyHashMap["aWebSocket"], "start", NULL);
    PyObject_CallMethod(aPyHashMap["aWakeup"],    "start", NULL);

    aPyHashMap["Import"] = Py_BuildValue("s", "eezz.agent");
    aPyHashMap["Module"] = PyImport_Import(aPyHashMap["Import"]);

    if (aPyHashMap["Module"] == NULL) {
        throw TPyExcept("");
    }

    aPyHashMap["DocRoot"]      = Py_BuildValue("s",    mConfig.mDocumentRoot.c_str());
    aPyHashMap["Address"]      = Py_BuildValue("(si)", mConfig.mHostname.c_str(), mConfig.mWebsocket);
    aPyHashMap["Arguments"]    = Py_BuildValue("(SS)", aPyHashMap["DocRoot"], aPyHashMap["Address"]);
    aPyHashMap["TEezzAgent"]   = PyObject_GetAttrString(aPyHashMap["Module"], "TEezzAgent");

    if (aPyHashMap["TEezzAgent"] == NULL) {
        throw TPyExcept("");
    }
    else {
        std::unique_lock<std::mutex> lck(aReadyMtx);
        mReady.notify_all();
    }

    for (;;) {
        TPyHash aPyLocalMap;

        // Suspend the python thread, waiting to handle requests
        PyObject_CallMethod(aPyHashMap["aCvExtern"], "acquire", NULL);
        PyObject_CallMethod(aPyHashMap["aCvExtern"], "wait",    NULL);
        PyObject_CallMethod(aPyHashMap["aCvExtern"], "release", NULL);
        //PyObject_CallMethod(aPyHashMap["aWakeup"], "join", NULL);

        aPyLocalMap["Agent"] = PyObject_CallObject(aPyHashMap["TEezzAgent"], aPyHashMap["Arguments"]);
        aPyLocalMap["ResultHdle"] = PyObject_CallMethod(aPyLocalMap["Agent"], "handle_request", "s", mConfig.mRequest->filename);

        // Send the result
        Py_ssize_t aSize;
        string     aAsciiResult;
        wchar_t   *aStringRes = PyUnicode_AsWideCharString(aPyLocalMap["ResultHdle"], &aSize);
        if (aStringRes == NULL) {
            PyErr_Print();
        }
        else {
            wstring_convert<codecvt_utf8<wchar_t>> xConvert;
            aAsciiResult = xConvert.to_bytes(aStringRes);

            ap_set_content_type(mConfig.mRequest, "text/html");
            ap_rprintf(mConfig.mRequest, "%s", aAsciiResult.c_str());
            PyMem_Free(aStringRes);
        }
        aPyLocalMap["ResultShut"] = PyObject_CallMethod(aPyLocalMap["Agent"], "shutdown", NULL);

        // Yield the calling thread, which waits on mReady mutex 
        std::unique_lock<std::mutex> lck(aReadyMtx);
        aPythonBusy = false;
        mReady.notify_all();
    }
}

#include <winsock.h>
// ------------------------------------------------------------
// Next steps:
// configure sys path to the distribution
// configure python path
// ------------------------------------------------------------
extern "C" static int eezz_handler(request_rec *r) {
    // Initialize Python runtime
    TPyHash xPyHashMap;
    int     aReturn = OK;
    WSADATA aWsa;
    static bool aInitial = true;
    struct sockaddr_in aServerAddr;

    if (!r->handler || strcmp(r->handler, "eezz_websocket")) {
        return (DECLINED);
    }

    mConfig.mRequest = r;
    aPythonBusy      = true;
    mReady.notify_all();

    if (aInitial) {
        aInitial = false;
        WSAStartup(MAKEWORD(2, 2), &aWsa);

        string aFile = r->filename;
        size_t aPos  = aFile.find(r->uri);
        mConfig.mDocumentRoot = aFile.substr(0, aPos);
        aPythonBusy  = true;
        mThread      = new thread(runPython); 

        // wait for the python to initialize
        std::unique_lock<std::mutex> lck(aReadyMtx);
        mReady.wait(lck);
    }
    
    // Wakeup the python process
    SOCKET  aSocket = socket(AF_INET, SOCK_STREAM, 0);
    aServerAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    aServerAddr.sin_family = AF_INET;
    aServerAddr.sin_port   = htons(63000);
    connect(aSocket, (struct sockaddr *)&aServerAddr, sizeof(aServerAddr));
    closesocket(aSocket);

    // Wait for the python thread to finish
    std::unique_lock<std::mutex> lck(aReadyMtx);
    if (aPythonBusy) {
        mReady.wait(lck);
    }
    return aReturn;
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

